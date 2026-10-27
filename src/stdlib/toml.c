/* SPDX-License-Identifier: MIT */
/*
 * toml.c — Implementation of the std.toml standard library module.
 *
 * FFI wrapper around tomlc99 (Tom Pawlak, MIT licence).
 * tomlc99 is vendored at stdlib/vendor/tomlc99/.
 *
 * Story: 55.6.2  Branch: feature/stdlib-55.6-toml
 */

#include "toml.h"
#include "tk_runtime.h" /* 127.272: TK_TLS for g_toml_err */
#include "file.h"   /* 121.74: file_read -- AMB-07, $isdir, FIFO-safe */
#include "../../stdlib/vendor/tomlc99/toml.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Internal helper — create a TomlErr result.
 */
static TomlResult toml_err(const char *msg)
{
    TomlResult r = {NULL, 1, NULL};
    r.err_msg = msg ? msg : "toml error";
    return r;
}

/*
 * 113.B.4 — toml_miss: the reason an accessor failed on `key`.
 *
 * Every accessor used to say "key not found or not a string" (or integer,
 * boolean; "section not found" for a table), so a typo in a key could not be
 * told from a value of the wrong type -- and the caller's fix differs.  Like
 * toml_get_f64 (121.68), the message now says which: `key not found` when
 * the table has no such key, else the accessor's own `wrongtype` reason.
 */
static const char *toml_miss(const toml_table_t *tab, const char *key,
                             const char *wrongtype)
{
    return toml_key_exists(tab, key) ? wrongtype : "key not found";
}

/*
 * toml_load — parse a TOML string.
 *
 * Returns a TomlResult wrapping a heap-allocated toml_table_t *.
 * On success, caller owns the table and must call toml_free() via toml_free_table().
 * On error, err_msg describes the parse failure.
 *
 * 127.272: err_msg points into this thread's g_toml_err and is valid until
 * the next toml_load on the same thread -- the contract csv.c (g_csv_err)
 * and file.c (g_file_err_msg) already keep.  It used to be a malloc'd copy
 * of the parser's stack buffer that nothing ever freed: the glue stored the
 * pointer in the error box, which the runtime reuses and never frees
 * (127.109), so every failed toml.load leaked 256 bytes.  A message that must
 * outlive the next call is the CALLER's copy to make: toml_glue.c's
 * toml_raise copies it through tk_err_msg(), the stdlib's message ring.
 */
static TK_TLS char g_toml_err[256];

/*
 * 113.B.4 — the registry of live top-level tables, so toml.free is safe to
 * call on any handle a program holds.
 *
 * A `$tomlval` is either a document (from toml.load / toml.loadfile), which
 * owns its whole tree, or a view into one (toml.section, toml.sectionarr).
 * toke cannot tell the two apart by type, and tomlc99's toml_free on a view
 * frees part of its parent's tree -- the parent's later toml_free then frees
 * it again.  So toml_load records every document it returns here, and
 * toml_free_root frees only a recorded document, once: on a view, an
 * already-freed document, or any other value it does nothing.
 *
 * Guarded by a spinlock (the tables may be shared between tasks); the
 * registry is a plain array, append on load, swap-remove on free.
 */
static void       **g_toml_roots;
static int64_t      g_toml_nroots, g_toml_caproots;
static int          g_toml_roots_lock;

static void toml_roots_lock(void)
{
    while (__atomic_exchange_n(&g_toml_roots_lock, 1, __ATOMIC_ACQUIRE)) { /* spin */ }
}
static void toml_roots_unlock(void)
{
    __atomic_store_n(&g_toml_roots_lock, 0, __ATOMIC_RELEASE);
}

static int toml_root_add(void *tab)
{
    int rc = 0;
    toml_roots_lock();
    if (g_toml_nroots == g_toml_caproots) {
        int64_t nc = g_toml_caproots ? g_toml_caproots * 2 : 16;
        void **n = (void **)realloc(g_toml_roots, (size_t)nc * sizeof *n);
        if (!n) rc = -1;
        else { g_toml_roots = n; g_toml_caproots = nc; }
    }
    if (rc == 0) g_toml_roots[g_toml_nroots++] = tab;
    toml_roots_unlock();
    return rc;
}

int toml_free_root(void *tab)
{
    int found = 0;
    if (!tab) return 0;
    toml_roots_lock();
    for (int64_t i = 0; i < g_toml_nroots; i++) {
        if (g_toml_roots[i] == tab) {
            g_toml_roots[i] = g_toml_roots[--g_toml_nroots];
            found = 1;
            break;
        }
    }
    toml_roots_unlock();
    if (found) toml_free((toml_table_t *)tab);
    return found;
}

TomlResult toml_load(const char *src)
{
    TomlResult r = {NULL, 0, NULL};
    if (!src) return toml_err("null input");

    g_toml_err[0] = '\0';
    toml_table_t *tab = toml_parse((char *)src, g_toml_err, sizeof(g_toml_err));
    if (!tab) {
        r.is_err = 1;
        r.err_msg = g_toml_err[0] ? g_toml_err : "parse error";
        return r;
    }
    if (toml_root_add(tab) != 0) {
        /* Not tracked means toml.free could never release it; refuse rather
         * than hand out a table that leaks by construction. */
        toml_free(tab);
        r.is_err = 1;
        r.err_msg = "out of memory";
        return r;
    }
    r.ok = tab;
    return r;
}

/*
 * toml_load_file — read a TOML file and parse it.
 *
 * 121.74: read through std.file's file_read(), not a plain fopen().  fopen
 * followed a symlinked final path component -- the AMB-07 rule every
 * std.file read enforces with O_NOFOLLOW -- and on Darwin it opened a
 * directory, whose fseek/ftell/fread do not fail, so a directory parsed as
 * an empty TOML document.  file_read refuses both ("final path component is
 * a symlink ..." and "is a directory"), and does not hang on a FIFO with no
 * writer (121.73).
 */
TomlResult toml_load_file(const char *path)
{
    if (!path) return toml_err("null path");
    StrFileResult fr = file_read(path);
    if (fr.is_err) return toml_err(fr.err.msg ? fr.err.msg : "file read failed");
    TomlResult r = toml_load(fr.ok);
    free((void *)fr.ok);
    return r;
}

void toml_free_table(void *tab)
{
    (void)toml_free_root(tab);
}

/*
 * toml_get_str — get a string value from a table by key.
 */
TomlStrResult toml_get_str(void *tab, const char *key)
{
    TomlStrResult r = {NULL, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_string_in((toml_table_t *)tab, key);
    if (!d.ok) { r.is_err = 1; r.err_msg = toml_miss((toml_table_t *)tab, key, "key is not a string"); return r; }
    r.ok = d.u.s;  /* heap-allocated by tomlc99 */
    return r;
}

/*
 * toml_get_i64 — get an integer value from a table by key.
 */
TomlI64Result toml_get_i64(void *tab, const char *key)
{
    TomlI64Result r = {0, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_int_in((toml_table_t *)tab, key);
    if (!d.ok) { r.is_err = 1; r.err_msg = toml_miss((toml_table_t *)tab, key, "key is not an integer"); return r; }
    r.ok = (int64_t)d.u.i;
    return r;
}

/*
 * toml_get_bool — get a boolean value from a table by key.
 */
TomlBoolResult toml_get_bool(void *tab, const char *key)
{
    TomlBoolResult r = {0, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_bool_in((toml_table_t *)tab, key);
    if (!d.ok) { r.is_err = 1; r.err_msg = toml_miss((toml_table_t *)tab, key, "key is not a boolean"); return r; }
    r.ok = d.u.b ? 1 : 0;
    return r;
}

/*
 * toml_get_section — get a sub-table by key.
 * Returns a non-owning pointer into the parent table; do not free separately.
 */
TomlResult toml_get_section(void *tab, const char *key)
{
    TomlResult r = {NULL, 0, NULL};
    if (!tab || !key) return toml_err("null argument");
    toml_table_t *sub = toml_table_in((toml_table_t *)tab, key);
    if (!sub) { r.is_err = 1; r.err_msg = toml_miss((toml_table_t *)tab, key, "key is not a table"); return r; }
    r.ok = sub;
    return r;
}

/*
 * 121.68 — toml_get_f64: a float value from a table by key.
 *
 * TOML types its numbers: `ratio = 1.5` is a float, `ratio = 1` an integer.
 * This reads floats only, as toml_get_i64 reads integers only, so a value is
 * never silently converted; a caller that accepts either tries both.  The
 * two failures say which they are -- absent, or present with another type --
 * because "key not found or not a float" cannot tell a typo from a
 * mistyped value, and the caller's fix differs.
 */
TomlF64Result toml_get_f64(void *tab, const char *key)
{
    TomlF64Result r = {0.0, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_double_in((toml_table_t *)tab, key);
    if (!d.ok) {
        r.is_err = 1;
        r.err_msg = toml_key_exists((toml_table_t *)tab, key)
                        ? "key is not a float" : "key not found";
        return r;
    }
    r.ok = d.u.d;
    return r;
}

/*
 * 121.68 — toml_has: is `key` present in this table, whatever its value --
 * a string, number, bool, date, array or sub-table.  `key` is one key of
 * this table, not a dotted path: `server.port` is found by
 * toml.section(cfg; "server") then toml.has(server; "port").
 */
int toml_has(void *tab, const char *key)
{
    if (!tab || !key) return 0;
    return toml_key_exists((toml_table_t *)tab, key) ? 1 : 0;
}

/*
 * 113.B.4 — toml_get_arr: the array at `key`, element by element.
 *
 * `kind` names the element type the caller wants: 's' string, 'i' integer,
 * 'd' float (a decimal integer reads as the nearest double, as toml_get_f64
 * does), 'b' boolean, 't' table.  Each element is read with the same tomlc99
 * accessor its scalar counterpart uses, so an array's element is accepted
 * exactly when the same value under a key would be.  An empty array is a
 * success with len 0.  Slot encoding: a string pointer (malloc'd, the
 * caller's), the integer, the double's bit pattern, 0/1, or a non-owning
 * table view.  On failure `err_msg` is `key not found`, `key is not an
 * array`, or `array element N is not a <type>` (N from 0), in a per-thread
 * buffer valid until the next call on the thread.
 */
static TK_TLS char g_toml_arr_err[64];

TomlArrResult toml_get_arr(void *tab, const char *key, char kind)
{
    TomlArrResult r = {NULL, 0, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_array_t *a = toml_array_in((toml_table_t *)tab, key);
    if (!a) {
        r.is_err = 1;
        r.err_msg = toml_miss((toml_table_t *)tab, key, "key is not an array");
        return r;
    }
    int n = toml_array_nelem(a);
    if (n <= 0) return r;
    r.data = (int64_t *)malloc((size_t)n * sizeof(int64_t));
    if (!r.data) { r.is_err = 1; r.err_msg = "out of memory"; return r; }
    const char *what = kind == 's' ? "a string"  : kind == 'i' ? "an integer" :
                       kind == 'd' ? "a float"   : kind == 'b' ? "a boolean"  :
                                     "a table";
    for (int i = 0; i < n; i++) {
        int ok = 0;
        int64_t slot = 0;
        if (kind == 't') {
            toml_table_t *t = toml_table_at(a, i);
            ok = t != NULL;
            slot = (int64_t)(intptr_t)t;
        } else {
            toml_datum_t d;
            switch (kind) {
            case 's': d = toml_string_at(a, i); break;
            case 'i': d = toml_int_at(a, i);    break;
            case 'd': d = toml_double_at(a, i); break;
            default:  d = toml_bool_at(a, i);   break;
            }
            ok = d.ok;
            if (ok) {
                if (kind == 's')      slot = (int64_t)(intptr_t)d.u.s;
                else if (kind == 'i') slot = (int64_t)d.u.i;
                else if (kind == 'd') memcpy(&slot, &d.u.d, sizeof slot);
                else                  slot = d.u.b ? 1 : 0;
            }
        }
        if (!ok) {
            if (kind == 's') for (int j = 0; j < i; j++) free((void *)(intptr_t)r.data[j]);
            free(r.data);
            r.data = NULL;
            r.is_err = 1;
            snprintf(g_toml_arr_err, sizeof g_toml_arr_err,
                     "array element %d is not %s", i, what);
            r.err_msg = g_toml_arr_err;
            return r;
        }
        r.data[i] = slot;
    }
    r.len = n;
    return r;
}

/*
 * 113.B.4 — toml_count / toml_key_at: the keys of one table, whatever their
 * values.  tomlc99 keeps a table's keys in three lists -- plain values, then
 * arrays, then sub-tables, each in document order -- and toml_key_in walks
 * them in that order, so that is the order here.  toml_key_at's string
 * belongs to the table; a caller that outlives it copies it.
 */
int64_t toml_count(void *tab)
{
    if (!tab) return 0;
    const toml_table_t *t = (const toml_table_t *)tab;
    return (int64_t)toml_table_nkval(t) + toml_table_narr(t) + toml_table_ntab(t);
}

const char *toml_key_at(void *tab, int64_t idx)
{
    if (!tab || idx < 0 || idx > 0x7fffffff) return NULL;
    return toml_key_in((const toml_table_t *)tab, (int)idx);
}

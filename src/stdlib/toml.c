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
    if (tab) toml_free((toml_table_t *)tab);
}

/*
 * toml_get_str — get a string value from a table by key.
 */
TomlStrResult toml_get_str(void *tab, const char *key)
{
    TomlStrResult r = {NULL, 0, NULL};
    if (!tab || !key) { r.is_err = 1; r.err_msg = "null argument"; return r; }
    toml_datum_t d = toml_string_in((toml_table_t *)tab, key);
    if (!d.ok) { r.is_err = 1; r.err_msg = "key not found or not a string"; return r; }
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
    if (!d.ok) { r.is_err = 1; r.err_msg = "key not found or not an integer"; return r; }
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
    if (!d.ok) { r.is_err = 1; r.err_msg = "key not found or not a boolean"; return r; }
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
    if (!sub) { r.is_err = 1; r.err_msg = "section not found"; return r; }
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

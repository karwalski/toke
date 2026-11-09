/* SPDX-License-Identifier: MIT */
/*
 * toml_glue.c — i64-ABI wrappers for std.toml module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.toml.
 *
 * ── Error signalling (story 127.67) ──────────────────────────────────────
 * Every wrapper here returns a bare i64.  The default codegen for
 * `mt toml.x(...) {$ok:…;$err:…}` takes the $ok arm when that i64 is
 * non-zero, so a zero return means "error".  That sentinel is sound for a
 * table handle or a string pointer, which are never 0 on success, and it is
 * WRONG for a value type whose valid domain includes 0:
 *
 *   toml.bool(cfg; "minify")   with  minify = false     -> 0 -> read as error
 *   toml.i64 (cfg; "retries")  with  retries = 0        -> 0 -> read as error
 *
 * Callers treat "error" as "key absent" and substitute their default, so a
 * key that says false is obeyed as true.  Configuration read as the opposite
 * of what it says is worse than configuration that fails loudly.
 *
 * The fix is the same one str.toint/str.tofloat use (114.53/114.54): set the
 * @tk_current_error global and always return the real value.  These wrappers
 * now maintain that global truthfully.  Consuming it at a match site is the
 * codegen's half: llvm.c's is_num_parse_wrapper() (bool, i64) and
 * is_stdlib_slot_wrapper() (load, loadfile, str, section — 127.182) list the
 * wrappers whose $ok/$err arm is chosen from @tk_current_error instead of the
 * 0 sentinel, and whose $err arm binds the {msg} payload.
 */

#include "toml.h"
#include "tk_runtime.h"   /* 127.182: tk_err_box (127.109) */
#include "capabilities.h" /* 121.22: toml.loadfile is an fs.read sink */
#include <stdint.h>
#include <string.h>   /* 121.68: memcpy for the f64 bit pattern */

/* 127.182 — the error carries the parser's message.  stdlib/toml.tki
 * declares `tomlerr` as a record {msg:str}; a failure stores a one-slot box
 * of exactly that layout (runtime-abi.md §7.3), so `e.msg` in the $err arm
 * reads it.  Until 127.182 every failure stored the payload-less flag 1, and
 * `e.msg` was E4034 at compile time (the type was not registered) or RT005
 * at run time (the payload was nil).  An ok return stores 0 (127.123). */
/*
 * 127.272: the message is copied through tk_err_msg() (tk_runtime.c), as
 * csv_glue.c and file_glue.c do.  toml_load's parse message lives in toml.c's
 * per-thread buffer, which the next toml.load rewrites, and toml.loadfile's
 * read failure in file.c's static buffer; a `let`-kept error keeps a copy of
 * its BOX, not of the string (127.103), so without this copy a later call
 * would rewrite a message the program still holds.  The ring keeps a message
 * exact across the next TK_ERR_MSG_RING - 1 raises on the thread, with no
 * allocation per error (127.109) -- before this, toml_load malloc'd the
 * message and nothing freed it. */
static int64_t toml_raise(const char *msg) {
    const char *m = tk_err_msg("%s", msg && msg[0] ? msg : "toml error");
    int64_t *box = (int64_t *)tk_err_box((int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)m;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

int64_t tk_toml_load_w(int64_t src) {
    if (!src) { return toml_raise("toml: null argument"); }
    TomlResult r = toml_load((const char *)(intptr_t)src);
    if (r.is_err) return toml_raise(r.err_msg);
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok;
}

/*
 * 127.67: a missing or non-table key used to return the PARENT table, so
 * `toml.section(cfg; "typo")` silently handed back cfg itself and every
 * subsequent lookup read top-level keys through the $ok arm.  Return the
 * error sentinel instead so the $err arm actually fires.
 */
int64_t tk_toml_section_w(int64_t tab, int64_t key) {
    if (!tab || !key) { return toml_raise("toml: null argument"); }
    TomlResult r = toml_get_section((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    if (r.is_err) return toml_raise(r.err_msg);
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_str_w(int64_t tab, int64_t key) {
    if (!tab || !key) { return toml_raise("toml: null argument"); }
    TomlStrResult r = toml_get_str((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    if (r.is_err) return toml_raise(r.err_msg);
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok;
}

/* 127.67: `retries = 0` is a value, not an error — report it through
 * tk_current_error and return the real 0. */
int64_t tk_toml_i64_w(int64_t tab, int64_t key) {
    if (!tab || !key) { return toml_raise("toml: null argument"); }
    TomlI64Result r = toml_get_i64((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    if (r.is_err) return toml_raise(r.err_msg);
    tk_current_error = 0;
    return r.ok;
}

/* 127.67: `minify = false` is a value, not an error. */
int64_t tk_toml_bool_w(int64_t tab, int64_t key) {
    if (!tab || !key) { return toml_raise("toml: null argument"); }
    TomlBoolResult r = toml_get_bool((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    if (r.is_err) return toml_raise(r.err_msg);
    tk_current_error = 0;
    return (int64_t)r.ok;
}

/*
 * Story 136.32 — toml.loadfile(path) : $tomlval!$tomlerr.
 *
 * toml_load_file() sat beside toml_load() in toml.h from the start and
 * stdlib/toml.tki exports both; only toml.load got a wrapper, so the two
 * examples on docs/stdlib/toml.md that read a config file off disk — the
 * ordinary way to use the module — failed at link. Same error protocol as
 * tk_toml_load_w above.
 */
int64_t tk_toml_loadfile_w(int64_t path) {
    /* 121.22 (ADR-0010): reading a file is fs.read.  This wrapper opened the
     * path with no gate, so a --cap-enforce binary with no grants could read
     * any TOML file (measured on main: exit 0).  file.read has been gated
     * since 124.4c; this is the same check at the same place. */
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) { return toml_raise("toml: null argument"); }
    TomlResult r = toml_load_file((const char *)(intptr_t)path);
    if (r.is_err) return toml_raise(r.err_msg);
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok;
}

/*
 * Story 121.68 — toml.f64(v; key) : f64!$tomlerr and toml.has(v; key) : bool.
 *
 * ooke could not read a float-valued config key, nor tell a key that is
 * absent from one whose value is an array or table (every typed accessor
 * fails on both).  toml.f64 returns the double's bit pattern in the i64 ABI
 * (the .tki's f64 return makes the compiler bitcast it back) and keeps the
 * error slot truthful like toml.i64 (127.67): 0.0's bit pattern IS the 0
 * sentinel, so only the slot can say whether it failed.  toml.has cannot
 * fail: a null table or key is simply not present.
 */
int64_t tk_toml_f64_w(int64_t tab, int64_t key) {
    if (!tab || !key) { return toml_raise("toml: null argument"); }
    TomlF64Result r = toml_get_f64((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    if (r.is_err) return toml_raise(r.err_msg);
    tk_current_error = 0;
    int64_t bits;
    memcpy(&bits, &r.ok, sizeof bits);
    return bits;
}

int64_t tk_toml_has_w(int64_t tab, int64_t key) {
    return (int64_t)toml_has((void *)(intptr_t)tab, (const char *)(intptr_t)key);
}

/*
 * Story 113.B.4 — arrays, keys, len, free.
 *
 * toml.strarr / i64arr / f64arr / boolarr / sectionarr (v; key) : @T!$tomlerr
 *   read the array at `key` into a real toke array (tk_array.h layout: one
 *   i64 slot per element -- a string pointer, the integer, the double's bit
 *   pattern (as @f64 is laid out), 0/1, or a section view).  f64arr waited
 *   on 127.297 (the checker now types a std `@f64!E` $ok binding as @f64, so
 *   `.get(i)` bitcasts the slot back to a double); it reads a decimal
 *   integer element as the nearest double, as toml.f64 does.
 *   Same error protocol as the scalar accessors: the slot is the
 *   discriminant, never the returned handle.
 * toml.keys(v) : @$str   the table's own keys (copies: they outlive v).
 * toml.len(v)  : u64     how many keys the table has.
 * toml.free(v) : bool    release a document; true if v was a live document,
 *   false (and nothing happens) for a section view, a document already
 *   freed, or any other value -- see toml_free_root (toml.c).
 */
#include "tk_array.h"
#include <stdlib.h>

static int64_t toml_arr_w(int64_t tab, int64_t key, char kind) {
    if (!tab || !key) { return toml_raise("toml: null argument"); }
    TomlArrResult r = toml_get_arr((void *)(intptr_t)tab, (const char *)(intptr_t)key, kind);
    if (r.is_err) return toml_raise(r.err_msg);
    int64_t h = tk_arr_alloc(r.len, r.len);
    if (!h) {
        if (kind == 's') for (int64_t i = 0; i < r.len; i++) free((void *)(intptr_t)r.data[i]);
        free(r.data);
        return toml_raise("toml: out of memory");
    }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < r.len; i++) slots[i] = r.data[i];
    free(r.data);
    tk_current_error = 0;
    return h;
}

int64_t tk_toml_strarr_w(int64_t tab, int64_t key)     { return toml_arr_w(tab, key, 's'); }
int64_t tk_toml_i64arr_w(int64_t tab, int64_t key)     { return toml_arr_w(tab, key, 'i'); }
int64_t tk_toml_f64arr_w(int64_t tab, int64_t key)     { return toml_arr_w(tab, key, 'd'); }
int64_t tk_toml_boolarr_w(int64_t tab, int64_t key)    { return toml_arr_w(tab, key, 'b'); }
int64_t tk_toml_sectionarr_w(int64_t tab, int64_t key) { return toml_arr_w(tab, key, 't'); }

int64_t tk_toml_keys_w(int64_t tab) {
    int64_t n = toml_count((void *)(intptr_t)tab);
    int64_t h = tk_arr_alloc(n, n);
    if (!h) return tk_arr_alloc(0, 0);
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) {
        const char *k = toml_key_at((void *)(intptr_t)tab, i);
        size_t kl = k ? strlen(k) : 0;
        char *c = k ? (char *)malloc(kl + 1) : NULL;
        if (c) memcpy(c, k, kl + 1);
        slots[i] = (int64_t)(intptr_t)(c ? c : "");
    }
    return h;
}

int64_t tk_toml_len_w(int64_t tab) {
    return toml_count((void *)(intptr_t)tab);
}

int64_t tk_toml_free_w(int64_t tab) {
    return (int64_t)toml_free_root((void *)(intptr_t)tab);
}

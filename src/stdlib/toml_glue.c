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
static int64_t toml_raise(const char *msg) {
    int64_t *box = (int64_t *)tk_err_box((int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)(msg && msg[0] ? msg : "toml error");
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

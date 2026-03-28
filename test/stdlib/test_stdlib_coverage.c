/*
 * test_stdlib_coverage.c — Gap-filler tests for stdlib conformance coverage.
 *
 * Story: 3.8.3
 * Branch: feature/stdlib-3.8-bench
 *
 * Covers cases not exercised by the per-module test files:
 *   - std.json  : Type error variant (key exists but wrong type)
 *   - std.json  : Missing error via u64/i64/f64/bool/arr on absent key
 *   - std.file  : file.list NotFound on nonexistent directory
 *   - std.file  : file.write + file.append + file.list happy paths
 *
 * Build: cc -Isrc/stdlib -o test/stdlib/test_stdlib_coverage
 *            test/stdlib/test_stdlib_coverage.c
 *            src/stdlib/json.c src/stdlib/file.c src/stdlib/str.c
 *            src/stdlib/db.c -lsqlite3
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "json.h"

/* file.h / str.h StrArray collision: include file.h only */
#include "file.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

/* ── std.json Type errors ─────────────────────────────────────────────────
 * json.str on a numeric key must return JsonErr with kind == JSON_ERR_TYPE.
 * Same for u64/i64/f64/bool/arr when the key has the wrong JSON type.      */
static void test_json_type_errors(void)
{
    JsonResult jr = json_dec("{\"num\":42,\"str\":\"hello\",\"arr\":[1,2]}");
    ASSERT(!jr.is_err, "json_dec for type-error tests ok");
    Json j = jr.ok;

    /* json_str on a numeric value → Type error */
    StrJsonResult sr = json_str(j, "num");
    ASSERT(sr.is_err, "json_str(numeric key) is error");
    ASSERT(sr.err.kind == JSON_ERR_TYPE, "json_str(numeric key) err == Type");

    /* json_u64 on a string value → Type error */
    U64JsonResult ur = json_u64(j, "str");
    ASSERT(ur.is_err, "json_u64(string key) is error");
    ASSERT(ur.err.kind == JSON_ERR_TYPE, "json_u64(string key) err == Type");

    /* json_i64 on a string value → Type error */
    I64JsonResult ir = json_i64(j, "str");
    ASSERT(ir.is_err, "json_i64(string key) is error");
    ASSERT(ir.err.kind == JSON_ERR_TYPE, "json_i64(string key) err == Type");

    /* json_f64 on a string value → Type error */
    F64JsonResult fr = json_f64(j, "str");
    ASSERT(fr.is_err, "json_f64(string key) is error");
    ASSERT(fr.err.kind == JSON_ERR_TYPE, "json_f64(string key) err == Type");

    /* json_bool on a numeric value → Type error */
    BoolJsonResult br = json_bool(j, "num");
    ASSERT(br.is_err, "json_bool(numeric key) is error");
    ASSERT(br.err.kind == JSON_ERR_TYPE, "json_bool(numeric key) err == Type");

    /* json_arr on a non-array value → Type error */
    JsonArrayResult ar = json_arr(j, "str");
    ASSERT(ar.is_err, "json_arr(string key) is error");
    ASSERT(ar.err.kind == JSON_ERR_TYPE, "json_arr(string key) err == Type");
}

/* ── std.json Missing errors ──────────────────────────────────────────────
 * All extraction functions must return Missing when the key is absent.     */
static void test_json_missing_errors(void)
{
    JsonResult jr = json_dec("{\"x\":1}");
    ASSERT(!jr.is_err, "json_dec for missing-error tests ok");
    Json j = jr.ok;

    U64JsonResult ur = json_u64(j, "absent");
    ASSERT(ur.is_err, "json_u64(absent) is error");
    ASSERT(ur.err.kind == JSON_ERR_MISSING, "json_u64(absent) err == Missing");

    I64JsonResult ir = json_i64(j, "absent");
    ASSERT(ir.is_err, "json_i64(absent) is error");
    ASSERT(ir.err.kind == JSON_ERR_MISSING, "json_i64(absent) err == Missing");

    F64JsonResult fr = json_f64(j, "absent");
    ASSERT(fr.is_err, "json_f64(absent) is error");
    ASSERT(fr.err.kind == JSON_ERR_MISSING, "json_f64(absent) err == Missing");

    BoolJsonResult br = json_bool(j, "absent");
    ASSERT(br.is_err, "json_bool(absent) is error");
    ASSERT(br.err.kind == JSON_ERR_MISSING, "json_bool(absent) err == Missing");

    JsonArrayResult ar = json_arr(j, "absent");
    ASSERT(ar.is_err, "json_arr(absent) is error");
    ASSERT(ar.err.kind == JSON_ERR_MISSING, "json_arr(absent) err == Missing");
}

/* ── std.file list errors ─────────────────────────────────────────────────
 * file.list on a nonexistent directory must return FileErr.NotFound.       */
static void test_file_list_errors(void)
{
    StrArrayFileResult r = file_list("/nonexistent_tk_bench_dir_xyz");
    ASSERT(r.is_err, "file.list(nonexistent) is error");
    ASSERT(r.err.kind == FILE_ERR_NOT_FOUND,
           "file.list(nonexistent) err == NotFound");
}

/* ── std.file list happy path ─────────────────────────────────────────────
 * file.list on /tmp must succeed and return at least one entry.            */
static void test_file_list_happy(void)
{
    StrArrayFileResult r = file_list("/tmp");
    ASSERT(!r.is_err, "file.list(/tmp) ok");
    ASSERT(r.ok.len >= 1, "file.list(/tmp) returns >= 1 entry");
}

/* ── std.file append ──────────────────────────────────────────────────────
 * Verify file.append creates content that accumulates.                     */
static void test_file_append(void)
{
    const char *path = "/tmp/tk_coverage_append_test.txt";
    file_delete(path);

    BoolFileResult w = file_write(path, "first");
    ASSERT(!w.is_err && w.ok, "file.write creates file");

    BoolFileResult a = file_append(path, " second");
    ASSERT(!a.is_err && a.ok, "file.append succeeds");

    StrFileResult r = file_read(path);
    ASSERT(!r.is_err, "file.read after append ok");
    ASSERT(r.ok && strstr(r.ok, "second") != NULL,
           "file.read after append contains appended data");

    file_delete(path);
}

int main(void)
{
    test_json_type_errors();
    test_json_missing_errors();
    test_file_list_errors();
    test_file_list_happy();
    test_file_append();

    if (failures == 0) { printf("All coverage tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

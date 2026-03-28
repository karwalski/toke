/*
 * test_json.c — Unit tests for std.json (json.h / json.c).
 *
 * Story: 1.3.4  Branch: feature/stdlib-json
 *
 * Build: cc -I../../src/stdlib test_json.c ../../src/stdlib/json.c -o test_json
 */

#include "json.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, label) \
    do { if (cond) { printf("PASS  %s\n", label); g_pass++; } \
         else      { printf("FAIL  %s\n", label); g_fail++; } } while(0)

int main(void) {
    /* --- json_dec success --- */
    JsonResult dr = json_dec("{\"name\":\"alice\",\"age\":30}");
    CHECK(!dr.is_err, "json_dec valid object");
    Json j = dr.ok;

    /* --- json_str success --- */
    StrJsonResult sr = json_str(j, "name");
    CHECK(!sr.is_err && strcmp(sr.ok, "alice") == 0, "json_str key=name");

    /* --- json_u64 success --- */
    U64JsonResult ur = json_u64(j, "age");
    CHECK(!ur.is_err && ur.ok == 30, "json_u64 key=age");

    /* --- json_str missing key --- */
    StrJsonResult mr = json_str(j, "missing");
    CHECK(mr.is_err && mr.err.kind == JSON_ERR_MISSING, "json_str missing key");

    /* --- json_dec parse error --- */
    JsonResult bad = json_dec("not json");
    CHECK(bad.is_err && bad.err.kind == JSON_ERR_PARSE, "json_dec invalid input");

    /* --- json_bool --- */
    JsonResult br = json_dec("{\"flag\":true}");
    CHECK(!br.is_err, "json_dec bool object");
    BoolJsonResult bv = json_bool(br.ok, "flag");
    CHECK(!bv.is_err && bv.ok == 1, "json_bool key=flag true");

    /* --- json_arr --- */
    JsonResult ar = json_dec("{\"items\":[1,2,3]}");
    CHECK(!ar.is_err, "json_dec array object");
    JsonArrayResult av = json_arr(ar.ok, "items");
    CHECK(!av.is_err && av.ok.len == 3, "json_arr key=items len=3");

    /* --- json_enc stub --- */
    const char *enc = json_enc("hello");
    CHECK(strcmp(enc, "\"hello\"") == 0, "json_enc basic string");

    /* --- json_i64 --- */
    JsonResult ir = json_dec("{\"temp\":-42}");
    CHECK(!ir.is_err, "json_dec i64 object");
    I64JsonResult iv = json_i64(ir.ok, "temp");
    CHECK(!iv.is_err && iv.ok == -42, "json_i64 key=temp");

    /* --- json_f64 --- */
    JsonResult fr = json_dec("{\"pi\":3.14}");
    CHECK(!fr.is_err, "json_dec f64 object");
    F64JsonResult fv = json_f64(fr.ok, "pi");
    CHECK(!fv.is_err && fv.ok > 3.13 && fv.ok < 3.15, "json_f64 key=pi");

    /* --- summary --- */
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

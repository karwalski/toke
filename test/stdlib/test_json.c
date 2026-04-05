/*
 * test_json.c — Unit tests for std.json (json.h / json.c).
 *
 * Story: 1.3.4  Branch: feature/stdlib-json
 *
 * Build: cc -I../../src/stdlib test_json.c ../../src/stdlib/json.c -o test_json
 */

#include "json.h"
#include <stdio.h>
#include <stdlib.h>
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

    /* ================================================================ */
    /* Streaming API — Story 35.1.3                                     */
    /* ================================================================ */

    /* --- json_streamparser + json_streamnext: simple object --- */
    {
        const char *input = "{\"name\":\"alice\",\"age\":30}";
        JsonStream st = json_streamparser(
            (const uint8_t *)input, (uint64_t)strlen(input));

        JsonTokenResult tr;

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_START,
              "stream: ObjectStart");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_KEY &&
              strcmp(tr.ok.val.str, "name") == 0,
              "stream: Key=name");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_STR &&
              strcmp(tr.ok.val.str, "alice") == 0,
              "stream: Str=alice");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_KEY &&
              strcmp(tr.ok.val.str, "age") == 0,
              "stream: Key=age");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_U64 &&
              tr.ok.val.u64 == 30,
              "stream: U64=30");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_END,
              "stream: ObjectEnd");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_END,
              "stream: End");
    }

    /* --- streaming parser: array with mixed types --- */
    {
        const char *input = "[1, -42, 3.14, true, false, null, \"hi\"]";
        JsonStream st = json_streamparser(
            (const uint8_t *)input, (uint64_t)strlen(input));

        JsonTokenResult tr;

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_ARRAY_START,
              "stream arr: ArrayStart");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_U64 && tr.ok.val.u64 == 1,
              "stream arr: U64=1");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_I64 && tr.ok.val.i64 == -42,
              "stream arr: I64=-42");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_F64 &&
              tr.ok.val.f64 > 3.13 && tr.ok.val.f64 < 3.15,
              "stream arr: F64=3.14");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_BOOL && tr.ok.val.b == 1,
              "stream arr: Bool=true");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_BOOL && tr.ok.val.b == 0,
              "stream arr: Bool=false");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_NULL,
              "stream arr: Null");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_STR &&
              strcmp(tr.ok.val.str, "hi") == 0,
              "stream arr: Str=hi");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_ARRAY_END,
              "stream arr: ArrayEnd");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_END,
              "stream arr: End");
    }

    /* --- streaming parser: nested object --- */
    {
        const char *input = "{\"a\":{\"b\":1}}";
        JsonStream st = json_streamparser(
            (const uint8_t *)input, (uint64_t)strlen(input));

        JsonTokenResult tr;

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_START,
              "stream nested: outer ObjectStart");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_KEY &&
              strcmp(tr.ok.val.str, "a") == 0,
              "stream nested: Key=a");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_START,
              "stream nested: inner ObjectStart");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_KEY &&
              strcmp(tr.ok.val.str, "b") == 0,
              "stream nested: Key=b");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_U64 && tr.ok.val.u64 == 1,
              "stream nested: U64=1");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_END,
              "stream nested: inner ObjectEnd");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_END,
              "stream nested: outer ObjectEnd");

        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_END,
              "stream nested: End");
    }

    /* --- streaming parser: error on truncated input --- */
    {
        const char *input = "{\"key\":";
        JsonStream st = json_streamparser(
            (const uint8_t *)input, (uint64_t)strlen(input));

        json_streamnext(&st); /* ObjectStart */
        json_streamnext(&st); /* Key */
        JsonTokenResult tr = json_streamnext(&st);
        CHECK(tr.is_err && tr.err.kind == JSON_STREAM_ERR_TRUNCATED,
              "stream err: truncated");
    }

    /* --- streaming parser: empty object --- */
    {
        const char *input = "{}";
        JsonStream st = json_streamparser(
            (const uint8_t *)input, (uint64_t)strlen(input));

        JsonTokenResult tr;
        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_START,
              "stream empty obj: ObjectStart");
        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_OBJECT_END,
              "stream empty obj: ObjectEnd");
        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_END,
              "stream empty obj: End");
    }

    /* --- streaming parser: empty array --- */
    {
        const char *input = "[]";
        JsonStream st = json_streamparser(
            (const uint8_t *)input, (uint64_t)strlen(input));

        JsonTokenResult tr;
        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_ARRAY_START,
              "stream empty arr: ArrayStart");
        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_ARRAY_END,
              "stream empty arr: ArrayEnd");
        tr = json_streamnext(&st);
        CHECK(!tr.is_err && tr.ok.kind == JSON_TOK_END,
              "stream empty arr: End");
    }

    /* --- json_newwriter + json_streamemit + json_writerbytes --- */
    {
        JsonWriter w = json_newwriter(64);
        Json j1; j1.raw = "{\"x\":1}";
        JsonStreamVoidResult er = json_streamemit(&w, j1);
        CHECK(!er.is_err, "writer: emit ok");

        JsonBytes bytes = json_writerbytes(&w);
        CHECK(bytes.len == 7 && memcmp(bytes.data, "{\"x\":1}", 7) == 0,
              "writer: bytes match");

        /* emit a second value */
        Json j2; j2.raw = "[2,3]";
        er = json_streamemit(&w, j2);
        CHECK(!er.is_err, "writer: second emit ok");

        bytes = json_writerbytes(&w);
        CHECK(bytes.len == 12 &&
              memcmp(bytes.data, "{\"x\":1}[2,3]", 12) == 0,
              "writer: accumulated bytes");

        free(w.buf);
    }

    /* --- json_streamemit: null json error --- */
    {
        JsonWriter w = json_newwriter(64);
        Json jn; jn.raw = NULL;
        JsonStreamVoidResult er = json_streamemit(&w, jn);
        CHECK(er.is_err && er.err.kind == JSON_STREAM_ERR_INVALID,
              "writer: null json error");
        free(w.buf);
    }

    /* --- summary --- */
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

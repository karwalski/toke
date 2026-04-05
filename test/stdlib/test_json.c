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

    /* ================================================================ */
    /* Story 29.1.1 — object inspection and manipulation               */
    /* ================================================================ */

    /* --- json_keys --- */
    {
        Json jk; jk.raw = "{\"a\":1,\"b\":2,\"c\":3}";
        StrArrayJsonResult kr = json_keys(jk);
        CHECK(!kr.is_err && kr.ok.len == 3, "json_keys: len=3");
        if (!kr.is_err && kr.ok.len == 3) {
            CHECK(strcmp(kr.ok.data[0], "a") == 0, "json_keys: key[0]=a");
            CHECK(strcmp(kr.ok.data[1], "b") == 0, "json_keys: key[1]=b");
            CHECK(strcmp(kr.ok.data[2], "c") == 0, "json_keys: key[2]=c");
            for (uint64_t ki = 0; ki < kr.ok.len; ki++) free((void *)kr.ok.data[ki]);
            free(kr.ok.data);
        }
    }

    /* --- json_keys on non-object --- */
    {
        Json jk2; jk2.raw = "[1,2,3]";
        StrArrayJsonResult kr2 = json_keys(jk2);
        CHECK(kr2.is_err && kr2.err.kind == JSON_ERR_TYPE,
              "json_keys: non-object returns err");
    }

    /* --- json_has --- */
    {
        Json jh; jh.raw = "{\"a\":1,\"b\":2,\"c\":3}";
        CHECK(json_has(jh, "b") == 1, "json_has: key b exists");
        CHECK(json_has(jh, "z") == 0, "json_has: key z missing");
    }

    /* --- json_len array --- */
    {
        Json jla; jla.raw = "[1,2,3,4,5]";
        U64JsonResult lr = json_len(jla);
        CHECK(!lr.is_err && lr.ok == 5, "json_len: array len=5");
    }

    /* --- json_len object --- */
    {
        Json jlo; jlo.raw = "{\"a\":1,\"b\":2}";
        U64JsonResult lr2 = json_len(jlo);
        CHECK(!lr2.is_err && lr2.ok == 2, "json_len: object len=2");
    }

    /* --- json_len on non-array/non-object --- */
    {
        Json jlx; jlx.raw = "\"hello\"";
        U64JsonResult lr3 = json_len(jlx);
        CHECK(lr3.is_err && lr3.err.kind == JSON_ERR_TYPE,
              "json_len: non-array/object returns err");
    }

    /* --- json_type --- */
    {
        Json jt1; jt1.raw = "{\"a\":1}";
        StrJsonResult tr1 = json_type(jt1);
        CHECK(!tr1.is_err && strcmp(tr1.ok, "object") == 0, "json_type: object");

        Json jt2; jt2.raw = "[1,2]";
        StrJsonResult tr2 = json_type(jt2);
        CHECK(!tr2.is_err && strcmp(tr2.ok, "array") == 0, "json_type: array");

        Json jt3; jt3.raw = "\"hello\"";
        StrJsonResult tr3 = json_type(jt3);
        CHECK(!tr3.is_err && strcmp(tr3.ok, "string") == 0, "json_type: string");

        Json jt4; jt4.raw = "42";
        StrJsonResult tr4 = json_type(jt4);
        CHECK(!tr4.is_err && strcmp(tr4.ok, "number") == 0, "json_type: number");

        Json jt5; jt5.raw = "true";
        StrJsonResult tr5 = json_type(jt5);
        CHECK(!tr5.is_err && strcmp(tr5.ok, "bool") == 0, "json_type: bool (true)");

        Json jt6; jt6.raw = "false";
        StrJsonResult tr6 = json_type(jt6);
        CHECK(!tr6.is_err && strcmp(tr6.ok, "bool") == 0, "json_type: bool (false)");

        Json jt7; jt7.raw = "null";
        StrJsonResult tr7 = json_type(jt7);
        CHECK(!tr7.is_err && strcmp(tr7.ok, "null") == 0, "json_type: null");
    }

    /* --- json_pretty --- */
    {
        Json jp; jp.raw = "{\"a\":1,\"b\":[2,3]}";
        StrJsonResult pr = json_pretty(jp);
        CHECK(!pr.is_err, "json_pretty: no error");
        if (!pr.is_err) {
            /* Must contain newlines and spaces */
            CHECK(strchr(pr.ok, '\n') != NULL, "json_pretty: has newlines");
            CHECK(strstr(pr.ok, "  ") != NULL, "json_pretty: has indentation");
            free((void *)pr.ok);
        }
    }

    /* --- json_is_null --- */
    {
        Json jn1; jn1.raw = "{\"x\":null}";
        CHECK(json_is_null(jn1, "x") == 1, "json_is_null: x=null -> 1");

        Json jn2; jn2.raw = "{\"x\":1}";
        CHECK(json_is_null(jn2, "x") == 0, "json_is_null: x=1 -> 0");

        Json jn3; jn3.raw = "{\"x\":1}";
        CHECK(json_is_null(jn3, "missing") == 1, "json_is_null: missing key -> 1");
    }

    /* --- summary --- */
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/*
 * test_json.c — Unit tests for std.json (json.h / json.c).
 *
 * Story: 1.3.4  Branch: feature/stdlib-json
 *
 * Build: cc -I../../src/stdlib test_json.c ../../src/stdlib/json.c -o test_json
 */

#include "json.h"
#include "tk_array.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* json_glue.c — typed encoders under test from 127.43 onward. */
int64_t tk_json_encnum_w(int64_t v);
int64_t tk_json_encbool_w(int64_t b);
int64_t tk_json_encf64_w(int64_t bits);
int64_t tk_json_encarr_w(int64_t h, int64_t depth, int64_t kind);

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

    /* ================================================================ */
    /* Story 29.1.2 — path access and construction                     */
    /* ================================================================ */

    /* --- json_at: dotted path hit --- */
    {
        Json jpath; jpath.raw = "{\"user\":{\"name\":\"Alice\"}}";
        JsonResult pr = json_at(jpath, "user.name");
        CHECK(!pr.is_err && strcmp(pr.ok.raw, "\"Alice\"") == 0,
              "json_at: user.name == \"Alice\"");
        if (!pr.is_err) free((void *)pr.ok.raw);
    }

    /* --- json_at: missing key --- */
    {
        Json jpath2; jpath2.raw = "{\"user\":{\"name\":\"Alice\"}}";
        JsonResult pr2 = json_at(jpath2, "user.missing");
        CHECK(pr2.is_err, "json_at: missing key returns err");
    }

    /* --- json_index: valid index --- */
    {
        Json jarr; jarr.raw = "[10,20,30]";
        JsonResult ir = json_index(jarr, 1);
        CHECK(!ir.is_err && strcmp(ir.ok.raw, "20") == 0,
              "json_index: [10,20,30][1] == 20");
        if (!ir.is_err) free((void *)ir.ok.raw);
    }

    /* --- json_index: out of bounds --- */
    {
        Json jarr2; jarr2.raw = "[10,20,30]";
        JsonResult ir2 = json_index(jarr2, 5);
        CHECK(ir2.is_err, "json_index: out of bounds returns err");
    }

    /* --- json_merge: disjoint keys --- */
    {
        Json jm1; jm1.raw = "{\"a\":1}";
        Json jm2; jm2.raw = "{\"b\":2}";
        JsonResult mr = json_merge(jm1, jm2);
        CHECK(!mr.is_err, "json_merge: disjoint no error");
        if (!mr.is_err) {
            CHECK(strstr(mr.ok.raw, "\"a\"") != NULL, "json_merge: disjoint has a");
            CHECK(strstr(mr.ok.raw, "\"b\"") != NULL, "json_merge: disjoint has b");
            free((void *)mr.ok.raw);
        }
    }

    /* --- json_merge: j2 overrides j1 --- */
    {
        Json jm3; jm3.raw = "{\"a\":1}";
        Json jm4; jm4.raw = "{\"a\":2}";
        JsonResult mr2 = json_merge(jm3, jm4);
        CHECK(!mr2.is_err, "json_merge: override no error");
        if (!mr2.is_err) {
            /* "a":2 must appear, "a":1 must not be the value */
            CHECK(strstr(mr2.ok.raw, "\"a\":2") != NULL, "json_merge: j2 wins (a=2)");
            free((void *)mr2.ok.raw);
        }
    }

    /* --- json_from_pairs --- */
    {
        const char *keys[]   = { "x", "y" };
        const char *values[] = { "1", "hello" };
        Json fp = json_from_pairs(keys, values, 2);
        CHECK(fp.raw != NULL, "json_from_pairs: raw not null");
        if (fp.raw) {
            CHECK(strstr(fp.raw, "\"x\"") != NULL, "json_from_pairs: has key x");
            CHECK(strstr(fp.raw, "\"y\"") != NULL, "json_from_pairs: has key y");
            CHECK(strstr(fp.raw, "\"1\"") != NULL, "json_from_pairs: has value 1");
            CHECK(strstr(fp.raw, "\"hello\"") != NULL, "json_from_pairs: has value hello");
            free((void *)fp.raw);
        }
    }

    /* --- 127.43: typed number / bool encoding (json_glue.c) --- */
    {
        struct { int64_t v; const char *want; } nums[] = {
            { 0,                     "0" },
            { 2147483647LL,          "2147483647" },          /* 2^31-1 */
            { 2147483648LL,          "2147483648" },          /* 2^31   */
            { 4294967296LL,          "4294967296" },          /* 2^32   */
            { 73000000000000LL,      "73000000000000" },      /* 7.3e13 */
            { 9223372036854775807LL, "9223372036854775807" }, /* 2^63-1 */
            { -2147483648LL,         "-2147483648" },
            { -4294967296LL,         "-4294967296" },
            { -9223372036854775807LL - 1, "-9223372036854775808" },
        };
        int ok = 1;
        for (size_t i = 0; i < sizeof nums / sizeof nums[0]; i++) {
            const char *got = (const char *)(intptr_t)tk_json_encnum_w(nums[i].v);
            if (!got || strcmp(got, nums[i].want) != 0) {
                printf("      encnum(%lld) = %s, want %s\n",
                       (long long)nums[i].v, got ? got : "(null)", nums[i].want);
                ok = 0;
            }
        }
        CHECK(ok, "127.43 tk_json_encnum_w: full i64 range, no truncation");

        const char *bt = (const char *)(intptr_t)tk_json_encbool_w(1);
        const char *bf = (const char *)(intptr_t)tk_json_encbool_w(0);
        CHECK(bt && strcmp(bt, "true") == 0 && bf && strcmp(bf, "false") == 0,
              "127.43 tk_json_encbool_w: true/false literals");

        double d1 = 1.5, d2 = 0.1; int64_t b1, b2;
        memcpy(&b1, &d1, sizeof b1); memcpy(&b2, &d2, sizeof b2);
        const char *f1 = (const char *)(intptr_t)tk_json_encf64_w(b1);
        const char *f2 = (const char *)(intptr_t)tk_json_encf64_w(b2);
        CHECK(f1 && strcmp(f1, "1.5") == 0, "127.43 tk_json_encf64_w: 1.5 stays short");
        CHECK(f2 && strtod(f2, NULL) == 0.1, "127.43 tk_json_encf64_w: 0.1 round-trips");
    }

    /* --- 127.44: typed array encoding (flat) --- */
    {
        /* the exact regression: [122,5] printed as "z" by the byte heuristic */
        int64_t h = tk_arr_alloc(2, 2);
        int64_t *d = (int64_t *)(intptr_t)h;
        d[0] = 122; d[1] = 5;
        const char *got = (const char *)(intptr_t)tk_json_encarr_w(h, 1, 0);
        CHECK(got && strcmp(got, "[122,5]") == 0,
              "127.44 encarr: @i64 [122,5] is an array, not \"z\"");

        int64_t big = tk_arr_alloc(3, 3);
        int64_t *bd = (int64_t *)(intptr_t)big;
        bd[0] = 4294967296LL; bd[1] = -4294967296LL; bd[2] = 9223372036854775807LL;
        const char *gb = (const char *)(intptr_t)tk_json_encarr_w(big, 1, 0);
        CHECK(gb && strcmp(gb, "[4294967296,-4294967296,9223372036854775807]") == 0,
              "127.44 encarr: @i64 elements keep the full i64 range");

        int64_t sa = tk_arr_alloc(2, 2);
        int64_t *sd = (int64_t *)(intptr_t)sa;
        sd[0] = (int64_t)(intptr_t)"a"; sd[1] = (int64_t)(intptr_t)"b\"c";
        const char *gs = (const char *)(intptr_t)tk_json_encarr_w(sa, 1, 1);
        CHECK(gs && strcmp(gs, "[\"a\",\"b\\\"c\"]") == 0,
              "127.44 encarr: @str elements are quoted and escaped");

        int64_t ba = tk_arr_alloc(2, 2);
        int64_t *bb = (int64_t *)(intptr_t)ba;
        bb[0] = 1; bb[1] = 0;
        const char *gbo = (const char *)(intptr_t)tk_json_encarr_w(ba, 1, 3);
        CHECK(gbo && strcmp(gbo, "[true,false]") == 0, "127.44 encarr: @bool elements");

        int64_t fa = tk_arr_alloc(2, 2);
        int64_t *fd = (int64_t *)(intptr_t)fa;
        double f0 = 1.5, f1v = -0.25;
        memcpy(&fd[0], &f0, sizeof f0); memcpy(&fd[1], &f1v, sizeof f1v);
        const char *gf = (const char *)(intptr_t)tk_json_encarr_w(fa, 1, 2);
        CHECK(gf && strcmp(gf, "[1.5,-0.25]") == 0, "127.44 encarr: @f64 elements");

        const char *ge = (const char *)(intptr_t)tk_json_encarr_w(tk_arr_alloc(0, 0), 1, 0);
        CHECK(ge && strcmp(ge, "[]") == 0, "127.44 encarr: empty array");
        const char *gn = (const char *)(intptr_t)tk_json_encarr_w(0, 1, 0);
        CHECK(gn && strcmp(gn, "null") == 0, "127.44 encarr: nil handle is null");
    }

    /* --- 127.45: nested array encoding (@@i64 printed pointers) --- */
    {
        int64_t a = tk_arr_alloc(2, 2), b2 = tk_arr_alloc(1, 1);
        ((int64_t *)(intptr_t)a)[0] = 1; ((int64_t *)(intptr_t)a)[1] = 2;
        ((int64_t *)(intptr_t)b2)[0] = 3;
        int64_t outer = tk_arr_alloc(2, 2);
        ((int64_t *)(intptr_t)outer)[0] = a; ((int64_t *)(intptr_t)outer)[1] = b2;
        const char *got = (const char *)(intptr_t)tk_json_encarr_w(outer, 2, 0);
        CHECK(got && strcmp(got, "[[1,2],[3]]") == 0,
              "127.45 encarr: @@i64 nests, no element addresses");

        /* depth 3, and a nil inner handle */
        int64_t o3 = tk_arr_alloc(2, 2);
        ((int64_t *)(intptr_t)o3)[0] = outer; ((int64_t *)(intptr_t)o3)[1] = 0;
        const char *g3 = (const char *)(intptr_t)tk_json_encarr_w(o3, 3, 0);
        CHECK(g3 && strcmp(g3, "[[[1,2],[3]],null]") == 0,
              "127.45 encarr: depth 3 and a nil inner handle");

        int64_t so = tk_arr_alloc(1, 1), si = tk_arr_alloc(2, 2);
        ((int64_t *)(intptr_t)si)[0] = (int64_t)(intptr_t)"x";
        ((int64_t *)(intptr_t)si)[1] = (int64_t)(intptr_t)"y";
        ((int64_t *)(intptr_t)so)[0] = si;
        const char *gs = (const char *)(intptr_t)tk_json_encarr_w(so, 2, 1);
        CHECK(gs && strcmp(gs, "[[\"x\",\"y\"]]") == 0,
              "127.45 encarr: @@str nests with quoting");
    }

    /* --- summary --- */
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

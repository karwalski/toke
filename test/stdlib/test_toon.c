/*
 * test_toon.c — Unit tests for std.toon (toon.h / toon.c).
 *
 * Story: 6.3.2
 *
 * Build: cc -I../../src/stdlib test_toon.c ../../src/stdlib/toon.c -o test_toon
 */

#include "toon.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, label) \
    do { if (cond) { printf("PASS  %s\n", label); g_pass++; } \
         else      { printf("FAIL  %s\n", label); g_fail++; } } while(0)

int main(void) {
    /* --- toon_dec success --- */
    ToonResult dr = toon_dec("users[2]{id,name,active}:\n1|Alice|true\n2|Bob|false\n");
    CHECK(!dr.is_err, "toon_dec valid TOON");
    Toon t = dr.ok;

    /* --- toon_str success --- */
    StrToonResult sr = toon_str(t, "name");
    CHECK(!sr.is_err && strcmp(sr.ok, "Alice") == 0, "toon_str key=name");

    /* --- toon_i64 success --- */
    I64ToonResult ir = toon_i64(t, "id");
    CHECK(!ir.is_err && ir.ok == 1, "toon_i64 key=id");

    /* --- toon_bool success --- */
    BoolToonResult br = toon_bool(t, "active");
    CHECK(!br.is_err && br.ok == 1, "toon_bool key=active (true)");

    /* --- toon_str missing key --- */
    StrToonResult mr = toon_str(t, "missing");
    CHECK(mr.is_err && mr.err.kind == TOON_ERR_MISSING, "toon_str missing key");

    /* --- toon_dec parse error --- */
    ToonResult bad = toon_dec("not toon");
    CHECK(bad.is_err && bad.err.kind == TOON_ERR_PARSE, "toon_dec bad input");

    /* --- toon_dec empty --- */
    ToonResult empty = toon_dec("");
    CHECK(empty.is_err, "toon_dec empty input");

    /* --- toon_from_json single object --- */
    const char *json_obj = "{\"id\":1,\"name\":\"Alice\"}";
    const char *toon_out = toon_from_json(json_obj);
    CHECK(toon_out && strstr(toon_out, "id") && strstr(toon_out, "name"),
          "toon_from_json single object has keys");
    CHECK(toon_out && strstr(toon_out, "1") && strstr(toon_out, "Alice"),
          "toon_from_json single object has values");

    /* --- toon_from_json array --- */
    const char *json_arr = "[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]";
    const char *toon_arr_out = toon_from_json(json_arr);
    CHECK(toon_arr_out && strstr(toon_arr_out, "data[2]"),
          "toon_from_json array has count");
    CHECK(toon_arr_out && strstr(toon_arr_out, "Alice") && strstr(toon_arr_out, "Bob"),
          "toon_from_json array has values");

    /* --- toon_to_json round-trip --- */
    const char *toon_input = "data[2]{id,name}:\n1|Alice\n2|Bob\n";
    const char *json_output = toon_to_json(toon_input);
    CHECK(json_output && json_output[0] == '[', "toon_to_json starts with [");
    CHECK(json_output && strstr(json_output, "\"id\"") && strstr(json_output, "\"name\""),
          "toon_to_json has keys");
    CHECK(json_output && strstr(json_output, "Alice") && strstr(json_output, "Bob"),
          "toon_to_json has values");

    /* --- toon_f64 --- */
    ToonResult fd = toon_dec("{x,y}:\n3.14|2.71\n");
    CHECK(!fd.is_err, "toon_dec float data");
    F64ToonResult fr = toon_f64(fd.ok, "x");
    CHECK(!fr.is_err && fr.ok > 3.13 && fr.ok < 3.15, "toon_f64 key=x");

    /* --- toon_enc --- */
    const char *enc = toon_enc("hello");
    CHECK(enc && strcmp(enc, "hello") == 0, "toon_enc passthrough");

    printf("\n---\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

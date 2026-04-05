/*
 * test_http_form.c — Unit tests for application/x-www-form-urlencoded parsing
 * (Story 27.1.14).
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *      -o test/stdlib/test_http_form \
 *      test/stdlib/test_http_form.c src/stdlib/http.c src/stdlib/encoding.c \
 *      src/stdlib/str.c
 * Or via make:
 *   make test-stdlib-http-form
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/http.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%d]: %s\n", __LINE__, (msg)); \
            failures++; \
        } else { \
            printf("pass: %s\n", (msg)); \
        } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a), (b)) == 0, (msg))

#define ASSERT_NULL(a, msg) \
    ASSERT((a) == NULL, (msg))

#define ASSERT_SIZE(a, b, msg) \
    ASSERT((a) == (size_t)(b), (msg))

/* ── Test 1: Simple single key=value ─────────────────────────────────── */

static void test_simple_key_value(void)
{
    TkFormResult r = http_form_parse("name=alice");
    ASSERT_SIZE(r.nfields, 1, "simple kv: 1 field");
    ASSERT_STREQ(r.fields[0].key,   "name",  "simple kv: key");
    ASSERT_STREQ(r.fields[0].value, "alice", "simple kv: value");
    http_form_free(r);
}

/* ── Test 2: Multiple fields separated by & ──────────────────────────── */

static void test_multiple_fields(void)
{
    TkFormResult r = http_form_parse("first=one&second=two&third=three");
    ASSERT_SIZE(r.nfields, 3, "multiple fields: 3 fields");
    ASSERT_STREQ(r.fields[0].key,   "first",  "multiple fields: key[0]");
    ASSERT_STREQ(r.fields[0].value, "one",    "multiple fields: value[0]");
    ASSERT_STREQ(r.fields[1].key,   "second", "multiple fields: key[1]");
    ASSERT_STREQ(r.fields[1].value, "two",    "multiple fields: value[1]");
    ASSERT_STREQ(r.fields[2].key,   "third",  "multiple fields: key[2]");
    ASSERT_STREQ(r.fields[2].value, "three",  "multiple fields: value[2]");
    http_form_free(r);
}

/* ── Test 3: URL-encoded value with percent-encoded space ────────────── */

static void test_percent_encoded_space_in_value(void)
{
    TkFormResult r = http_form_parse("city=New%20York");
    ASSERT_SIZE(r.nfields, 1, "percent space: 1 field");
    ASSERT_STREQ(r.fields[0].key,   "city",     "percent space: key");
    ASSERT_STREQ(r.fields[0].value, "New York",  "percent space: value decoded");
    http_form_free(r);
}

/* ── Test 4: + decoded as space ──────────────────────────────────────── */

static void test_plus_decoded_as_space(void)
{
    TkFormResult r = http_form_parse("greeting=Hello+World");
    ASSERT_SIZE(r.nfields, 1, "plus-space: 1 field");
    ASSERT_STREQ(r.fields[0].key,   "greeting",    "plus-space: key");
    ASSERT_STREQ(r.fields[0].value, "Hello World", "plus-space: value decoded");
    http_form_free(r);
}

/* ── Test 5: Special characters percent-encoded in value ─────────────── */

static void test_special_chars_in_value(void)
{
    /* email=user%40example.com  =>  email=user@example.com */
    TkFormResult r = http_form_parse("email=user%40example.com");
    ASSERT_SIZE(r.nfields, 1, "special chars: 1 field");
    ASSERT_STREQ(r.fields[0].key,   "email",            "special chars: key");
    ASSERT_STREQ(r.fields[0].value, "user@example.com", "special chars: @ decoded");
    http_form_free(r);
}

/* ── Test 6: Key with no value (key=) ────────────────────────────────── */

static void test_key_with_empty_value(void)
{
    TkFormResult r = http_form_parse("token=");
    ASSERT_SIZE(r.nfields, 1, "empty value: 1 field");
    ASSERT_STREQ(r.fields[0].key,   "token", "empty value: key");
    ASSERT_STREQ(r.fields[0].value, "",      "empty value: value is empty string");
    http_form_free(r);
}

/* ── Test 7: Key with no equals sign (bare key) ──────────────────────── */

static void test_bare_key_no_equals(void)
{
    TkFormResult r = http_form_parse("subscribe");
    ASSERT_SIZE(r.nfields, 1, "bare key: 1 field");
    ASSERT_STREQ(r.fields[0].key,   "subscribe", "bare key: key");
    ASSERT_STREQ(r.fields[0].value, "",           "bare key: value is empty string");
    http_form_free(r);
}

/* ── Test 8: Empty body ───────────────────────────────────────────────── */

static void test_empty_body(void)
{
    TkFormResult r = http_form_parse("");
    ASSERT_SIZE(r.nfields, 0, "empty body: 0 fields");
    http_form_free(r);
}

/* ── Test 9: NULL body ───────────────────────────────────────────────── */

static void test_null_body(void)
{
    TkFormResult r = http_form_parse(NULL);
    ASSERT_SIZE(r.nfields, 0, "null body: 0 fields");
    http_form_free(r);
}

/* ── Test 10: Multiple values for the same key ───────────────────────── */

static void test_multiple_values_same_key(void)
{
    TkFormResult r = http_form_parse("color=red&color=blue&color=green");
    ASSERT_SIZE(r.nfields, 3, "multi-value key: 3 fields");
    ASSERT_STREQ(r.fields[0].key,   "color", "multi-value: key[0]");
    ASSERT_STREQ(r.fields[0].value, "red",   "multi-value: value[0]");
    ASSERT_STREQ(r.fields[1].key,   "color", "multi-value: key[1]");
    ASSERT_STREQ(r.fields[1].value, "blue",  "multi-value: value[1]");
    ASSERT_STREQ(r.fields[2].key,   "color", "multi-value: key[2]");
    ASSERT_STREQ(r.fields[2].value, "green", "multi-value: value[2]");
    http_form_free(r);
}

/* ── Test 11: http_form_get — found ──────────────────────────────────── */

static void test_form_get_found(void)
{
    TkFormResult r = http_form_parse("a=1&b=2&c=3");
    const char *v = http_form_get(r, "b");
    ASSERT_STREQ(v, "2", "form_get found: returns correct value");
    http_form_free(r);
}

/* ── Test 12: http_form_get — not found ──────────────────────────────── */

static void test_form_get_not_found(void)
{
    TkFormResult r = http_form_parse("a=1&b=2");
    const char *v = http_form_get(r, "z");
    ASSERT_NULL(v, "form_get not found: returns NULL");
    http_form_free(r);
}

/* ── Test 13: http_form_get — first occurrence returned for dup key ──── */

static void test_form_get_first_occurrence(void)
{
    TkFormResult r = http_form_parse("x=first&x=second");
    const char *v = http_form_get(r, "x");
    ASSERT_STREQ(v, "first", "form_get dup key: returns first occurrence");
    http_form_free(r);
}

/* ── Test 14: Encoded key name ───────────────────────────────────────── */

static void test_encoded_key_name(void)
{
    /* field%5Bname%5D=alice  =>  field[name]=alice */
    TkFormResult r = http_form_parse("field%5Bname%5D=alice");
    ASSERT_SIZE(r.nfields, 1, "encoded key: 1 field");
    ASSERT_STREQ(r.fields[0].key,   "field[name]", "encoded key: key decoded");
    ASSERT_STREQ(r.fields[0].value, "alice",        "encoded key: value");
    http_form_free(r);
}

/* ── Test 15: Semicolon separator (legacy RFC 1866 / HTML4) ──────────── */

static void test_semicolon_separator(void)
{
    TkFormResult r = http_form_parse("a=1;b=2;c=3");
    ASSERT_SIZE(r.nfields, 3, "semicolon sep: 3 fields");
    ASSERT_STREQ(r.fields[0].key,   "a", "semicolon sep: key[0]");
    ASSERT_STREQ(r.fields[0].value, "1", "semicolon sep: value[0]");
    ASSERT_STREQ(r.fields[1].key,   "b", "semicolon sep: key[1]");
    ASSERT_STREQ(r.fields[1].value, "2", "semicolon sep: value[1]");
    ASSERT_STREQ(r.fields[2].key,   "c", "semicolon sep: key[2]");
    ASSERT_STREQ(r.fields[2].value, "3", "semicolon sep: value[2]");
    http_form_free(r);
}

/* ── Test 16: Mixed percent and plus encoding in same field ──────────── */

static void test_mixed_encoding(void)
{
    /* q=hello+world%21  =>  q=hello world! */
    TkFormResult r = http_form_parse("q=hello+world%21");
    ASSERT_SIZE(r.nfields, 1, "mixed encoding: 1 field");
    ASSERT_STREQ(r.fields[0].value, "hello world!", "mixed encoding: decoded correctly");
    http_form_free(r);
}

/* ── Test 17: http_form_get on empty result ──────────────────────────── */

static void test_form_get_on_empty_result(void)
{
    TkFormResult r = http_form_parse("");
    const char *v = http_form_get(r, "key");
    ASSERT_NULL(v, "form_get empty result: returns NULL");
    http_form_free(r);
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
    test_simple_key_value();
    test_multiple_fields();
    test_percent_encoded_space_in_value();
    test_plus_decoded_as_space();
    test_special_chars_in_value();
    test_key_with_empty_value();
    test_bare_key_no_equals();
    test_empty_body();
    test_null_body();
    test_multiple_values_same_key();
    test_form_get_found();
    test_form_get_not_found();
    test_form_get_first_occurrence();
    test_encoded_key_name();
    test_semicolon_separator();
    test_mixed_encoding();
    test_form_get_on_empty_result();

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

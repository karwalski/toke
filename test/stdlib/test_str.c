/*
 * test_str.c — Unit tests for the std.str C library (Story 1.3.1).
 *
 * Build and run: make test-stdlib
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/str.h"

static int failures = 0;
static const uint8_t bad_utf8[] = {0x80};  /* lone continuation byte */

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* str_len */
    ASSERT(str_len("hello") == 5, "str_len(hello)==5");
    ASSERT(str_len("") == 0,      "str_len(empty)==0");

    /* str_concat */
    ASSERT_STREQ(str_concat("foo","bar"), "foobar", "concat(foo,bar)");
    ASSERT_STREQ(str_concat("","x"),      "x",      "concat(empty,x)");

    /* str_slice */
    StrSliceResult sl = str_slice("hello", 1, 3);
    ASSERT(!sl.is_err, "slice(1,3) ok");
    ASSERT_STREQ(sl.ok, "el", "slice(1,3)==el");
    ASSERT(str_slice("hello",3,10).is_err, "slice oob is err");

    /* str_from_int / str_from_float */
    ASSERT_STREQ(str_from_int(42),  "42",   "from_int(42)");
    ASSERT_STREQ(str_from_int(-7),  "-7",   "from_int(-7)");
    ASSERT_STREQ(str_from_float(3.14), "3.14", "from_float(3.14)");

    /* str_to_int */
    IntParseResult ip = str_to_int("123");
    ASSERT(!ip.is_err && ip.ok == 123, "to_int(123)==123");
    ASSERT(str_to_int("abc").is_err,   "to_int(abc) is err");
    ASSERT(str_to_int("").is_err,      "to_int(empty) is err");

    /* str_to_float */
    FloatParseResult fp = str_to_float("2.5");
    ASSERT(!fp.is_err && fp.ok == 2.5, "to_float(2.5)==2.5");
    ASSERT(str_to_float("xyz").is_err, "to_float(xyz) is err");

    /* str_contains */
    ASSERT(str_contains("foobar","oba")==1, "contains(oba)==true");
    ASSERT(str_contains("foobar","xyz")==0, "contains(xyz)==false");

    /* str_split */
    StrArray parts = str_split("a,b,c", ",");
    ASSERT(parts.len == 3, "split len==3");
    ASSERT_STREQ(parts.data[0], "a", "split[0]==a");
    ASSERT_STREQ(parts.data[2], "c", "split[2]==c");

    /* str_trim / str_upper / str_lower */
    ASSERT_STREQ(str_trim("  hi  "), "hi",    "trim(hi)");
    ASSERT_STREQ(str_upper("hello"), "HELLO", "upper(hello)");
    ASSERT_STREQ(str_lower("WORLD"), "world", "lower(WORLD)");

    /* str_bytes */
    ByteArray ba = str_bytes("abc");
    ASSERT(ba.len == 3 && ba.data[0] == 'a', "bytes(abc)");

    /* str_from_bytes */
    StrEncResult enc = str_from_bytes(ba);
    ASSERT(!enc.is_err, "from_bytes(valid) ok");
    ASSERT_STREQ(enc.ok, "abc", "from_bytes(abc)==abc");
    ByteArray bad_ba = {bad_utf8, 1};
    ASSERT(str_from_bytes(bad_ba).is_err, "from_bytes(invalid) is err");

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

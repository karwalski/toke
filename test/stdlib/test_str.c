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

    /* --- Story 28.1.1: search and transform -------------------------------- */

    /* str_index */
    ASSERT(str_index("foobar", "bar") == 3,  "index(bar)==3");
    ASSERT(str_index("foobar", "foo") == 0,  "index(foo)==0");
    ASSERT(str_index("foobar", "xyz") == -1, "index(xyz)==-1");
    ASSERT(str_index("foobar", "")    == 0,  "index(empty)==0");

    /* str_rindex */
    ASSERT(str_rindex("abcabc", "bc") == 4,  "rindex(bc)==4");
    ASSERT(str_rindex("abcabc", "ab") == 3,  "rindex(ab)==3");
    ASSERT(str_rindex("foobar", "xyz") == -1, "rindex(xyz)==-1");

    /* str_replace */
    ASSERT_STREQ(str_replace("aabbaa", "aa", "X"),  "XbbX",    "replace(aa->X)");
    ASSERT_STREQ(str_replace("hello",  "x",  "y"),  "hello",   "replace(no match)");
    ASSERT_STREQ(str_replace("abcabc", "abc","Z"),   "ZZ",      "replace(all)");
    ASSERT_STREQ(str_replace("hello",  "hello",""),  "",        "replace(full->empty)");

    /* str_replace_first */
    ASSERT_STREQ(str_replace_first("aabbaa", "aa", "X"), "Xbbaa", "replace_first(aa->X)");
    ASSERT_STREQ(str_replace_first("hello",  "x",  "y"), "hello", "replace_first(no match)");
    ASSERT_STREQ(str_replace_first("aaa",    "a",  "b"), "baa",   "replace_first(a->b)");

    /* str_join */
    {
        const char *data[] = {"a", "b", "c"};
        StrArray arr; arr.data = data; arr.len = 3;
        ASSERT_STREQ(str_join(",", arr), "a,b,c", "join(a,b,c)==a,b,c");

        StrArray empty_arr; empty_arr.data = NULL; empty_arr.len = 0;
        const char *j = str_join("-", empty_arr);
        ASSERT(j && strcmp(j, "") == 0, "join(empty)==empty");

        const char *one[] = {"only"};
        StrArray one_arr; one_arr.data = one; one_arr.len = 1;
        ASSERT_STREQ(str_join("||", one_arr), "only", "join(one)==only");
    }

    /* str_repeat */
    ASSERT_STREQ(str_repeat("ab", 3),  "ababab", "repeat(ab,3)");
    ASSERT_STREQ(str_repeat("x",  1),  "x",      "repeat(x,1)");
    {
        const char *r = str_repeat("z", 0);
        ASSERT(r && strcmp(r, "") == 0, "repeat(z,0)==empty");
    }

    /* --- Story 28.1.2: prefix/suffix and line operations ------------------ */

    /* str_starts_with */
    ASSERT(str_starts_with("foobar", "foo") == 1, "starts_with(foo)==true");
    ASSERT(str_starts_with("foobar", "bar") == 0, "starts_with(bar)==false");
    ASSERT(str_starts_with("foobar", "")    == 1, "starts_with(empty)==true");
    ASSERT(str_starts_with("", "foo")       == 0, "starts_with(empty str)==false");

    /* str_ends_with */
    ASSERT(str_ends_with("foobar", "bar") == 1, "ends_with(bar)==true");
    ASSERT(str_ends_with("foobar", "foo") == 0, "ends_with(foo)==false");
    ASSERT(str_ends_with("foobar", "")    == 1, "ends_with(empty)==true");
    ASSERT(str_ends_with("", "bar")       == 0, "ends_with(empty str)==false");

    /* str_split_lines */
    {
        StrArray lines = str_split_lines("one\ntwo\nthree");
        ASSERT(lines.len == 3,                       "split_lines len==3");
        ASSERT_STREQ(lines.data[0], "one",   "split_lines[0]==one");
        ASSERT_STREQ(lines.data[2], "three", "split_lines[2]==three");

        StrArray crlf = str_split_lines("a\r\nb\r\nc");
        ASSERT(crlf.len == 3,                        "split_lines(CRLF) len==3");
        ASSERT_STREQ(crlf.data[1], "b",     "split_lines(CRLF)[1]==b");

        StrArray single = str_split_lines("no newline");
        ASSERT(single.len == 1,                      "split_lines(no nl) len==1");
        ASSERT_STREQ(single.data[0], "no newline", "split_lines(no nl)[0]");
    }

    /* str_count */
    ASSERT(str_count("banana", "an") == 2,  "count(an)==2");
    ASSERT(str_count("aaa",    "aa") == 1,  "count(aa non-overlap)==1");
    ASSERT(str_count("hello",  "xyz") == 0, "count(xyz)==0");
    ASSERT(str_count("abcabc", "abc") == 2, "count(abc)==2");

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

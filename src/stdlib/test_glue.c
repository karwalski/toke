/*
 * test_glue.c — i64-ABI wrappers for std.test module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.test.
 */

#include "tk_test.h"
#include <stdint.h>
#include <stdio.h>

int64_t tk_test_assert_w(int64_t cond, int64_t msg) {
    if (!cond) {
        const char *m = msg ? (const char *)(intptr_t)msg : "assertion failed";
        fprintf(stderr, "ASSERT FAILED: %s\n", m);
    }
    return cond ? 1 : 0;
}
/*
 * Story 136.21 — asserteq / assertne take the message and compare the strings.
 *
 * stdlib/test.tki, docs/stdlib/test.md and tk_test.h all declare
 * (a:str; b:str; msg:str), and tk_test_assert_eq/tk_test_assert_ne in
 * tk_test.c implement exactly that: they strcmp the two strings, print the
 * caller's message in a structured `DIAGNOSTIC: kind=assertion_failed`
 * line, and call mark_current_test_failed().
 *
 * The wrappers took two arguments and called none of it. They compared the
 * two i64s directly -- which for str arguments is a comparison of POINTERS,
 * so `test.asserteq(str.upper("hello"); "HELLO")` compared two different
 * addresses and reported failure for two identical strings -- printed the
 * addresses as decimal ("ASSERT_EQ FAILED: 4302859104 != 4302859120"), threw
 * the caller's explanation away, and never marked the test failed.
 *
 * That last part compounds 127.68 and 134.17, where assertions could not fail
 * at all: here they could fail but could say neither what was being checked
 * nor what the values were.
 */
int64_t tk_test_asserteq_w(int64_t a, int64_t b, int64_t msg) {
    return tk_test_assert_eq((const char *)(intptr_t)a,
                             (const char *)(intptr_t)b,
                             msg ? (const char *)(intptr_t)msg : NULL) ? 1 : 0;
}
int64_t tk_test_assertne_w(int64_t a, int64_t b, int64_t msg) {
    return tk_test_assert_ne((const char *)(intptr_t)a,
                             (const char *)(intptr_t)b,
                             msg ? (const char *)(intptr_t)msg : NULL) ? 1 : 0;
}
int64_t tk_test_assertequal_w(int64_t a, int64_t b, int64_t msg) {
    return tk_test_asserteq_w(a, b, msg);
}
int64_t tk_test_ok_w(int64_t val) { return val ? 1 : 0; }
int64_t tk_test_eq_w(int64_t a, int64_t b) { return (a == b) ? 1 : 0; }
int64_t tk_test_run_w(int64_t suite) {
    /* suite is a function pointer to the test body; run it with a default name */
    TkTestFn fn = (TkTestFn)(intptr_t)suite;
    if (!fn) return 0;
    tk_test_run("(unnamed)", fn);
    return 1;
}
int64_t tk_test_report_w(int64_t suite) {
    (void)suite;
    tk_test_print_summary();
    return 1;
}

int64_t tk_test_fail_w(int64_t msg) {
    const char *m = msg ? (const char *)(intptr_t)msg : "test failed";
    fprintf(stderr, "FAIL: %s\n", m);
    return 0;
}

/*
 * test_glue.c — i64-ABI wrappers for std.test module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.test.
 */

#include <stdint.h>
#include <stdio.h>

int64_t tk_test_assert_w(int64_t cond, int64_t msg) {
    if (!cond) {
        const char *m = msg ? (const char *)(intptr_t)msg : "assertion failed";
        fprintf(stderr, "ASSERT FAILED: %s\n", m);
    }
    return cond ? 1 : 0;
}
int64_t tk_test_asserteq_w(int64_t a, int64_t b) {
    if (a != b) fprintf(stderr, "ASSERT_EQ FAILED: %lld != %lld\n", (long long)a, (long long)b);
    return (a == b) ? 1 : 0;
}
int64_t tk_test_assertne_w(int64_t a, int64_t b) {
    if (a == b) fprintf(stderr, "ASSERT_NE FAILED: %lld == %lld\n", (long long)a, (long long)b);
    return (a != b) ? 1 : 0;
}
int64_t tk_test_assertequal_w(int64_t a, int64_t b) {
    return tk_test_asserteq_w(a, b);
}
int64_t tk_test_ok_w(int64_t val) { return val ? 1 : 0; }
int64_t tk_test_eq_w(int64_t a, int64_t b) { return (a == b) ? 1 : 0; }
int64_t tk_test_run_w(int64_t suite) { (void)suite; return 0; }
int64_t tk_test_report_w(int64_t suite) { (void)suite; return 0; }

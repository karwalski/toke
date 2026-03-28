/*
 * test_tktest.c — Unit tests for the std.test C library (Story 2.7.5).
 *
 * Build and run: make test-stdlib-test
 *
 * Note: several sub-tests intentionally trigger failing assertions to verify
 * that diagnostics are emitted to stderr and 0 is returned. Those failures
 * are expected and do NOT count toward this test's exit code.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/tk_test.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

int main(void)
{
    /* --- assert: passes for true condition --- */
    int r1 = tk_test_assert(1, "this should pass");
    ASSERT(r1 == 1, "assert(true) returns 1");

    /* --- assert: fails for false condition, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int r2 = tk_test_assert(0, "intentional failure");
    ASSERT(r2 == 0, "assert(false) returns 0");

    /* --- assert with non-zero integer (still true) --- */
    int r3 = tk_test_assert(42, "non-zero is truthy");
    ASSERT(r3 == 1, "assert(42) returns 1");

    /* --- assert_eq: passes when strings are equal --- */
    int r4 = tk_test_assert_eq("hello", "hello", "equal strings");
    ASSERT(r4 == 1, "assert_eq equal strings returns 1");

    /* --- assert_eq: fails when strings differ --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int r5 = tk_test_assert_eq("foo", "bar", "intentional eq failure");
    ASSERT(r5 == 0, "assert_eq unequal strings returns 0");

    /* --- assert_ne: passes when strings differ --- */
    int r6 = tk_test_assert_ne("foo", "bar", "different strings");
    ASSERT(r6 == 1, "assert_ne different strings returns 1");

    /* --- assert_ne: fails when strings are equal --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int r7 = tk_test_assert_ne("same", "same", "intentional ne failure");
    ASSERT(r7 == 0, "assert_ne equal strings returns 0");

    /* --- assert_eq with empty strings --- */
    int r8 = tk_test_assert_eq("", "", "empty strings are equal");
    ASSERT(r8 == 1, "assert_eq empty strings returns 1");

    /* --- assert_ne: empty vs non-empty --- */
    int r9 = tk_test_assert_ne("", "x", "empty vs non-empty");
    ASSERT(r9 == 1, "assert_ne empty vs non-empty returns 1");

    if (failures == 0) { printf("All test-module tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

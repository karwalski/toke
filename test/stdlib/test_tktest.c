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

    /* ------------------------------------------------------------------ */
    /* Story 28.4.1: comparison and containment assertions                 */
    /* ------------------------------------------------------------------ */

    /* --- assert_true: passes for non-zero --- */
    int rt1 = tk_test_assert_true(1, "assert_true with 1");
    ASSERT(rt1 == 1, "assert_true(1) returns 1");

    int rt2 = tk_test_assert_true(42, "assert_true with 42");
    ASSERT(rt2 == 1, "assert_true(42) returns 1");

    /* --- assert_true: fails for zero, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rt3 = tk_test_assert_true(0, "intentional assert_true failure");
    ASSERT(rt3 == 0, "assert_true(0) returns 0");

    /* --- assert_false: passes for zero --- */
    int rf1 = tk_test_assert_false(0, "assert_false with 0");
    ASSERT(rf1 == 1, "assert_false(0) returns 1");

    /* --- assert_false: fails for non-zero, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rf2 = tk_test_assert_false(1, "intentional assert_false failure");
    ASSERT(rf2 == 0, "assert_false(1) returns 0");

    /* --- assert_gt: passes when a > b --- */
    int rgt1 = tk_test_assert_gt(5.0, 3.0, "5 > 3");
    ASSERT(rgt1 == 1, "assert_gt(5,3) returns 1");

    /* --- assert_gt: fails when a <= b, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rgt2 = tk_test_assert_gt(3.0, 5.0, "intentional assert_gt failure");
    ASSERT(rgt2 == 0, "assert_gt(3,5) returns 0");

    /* --- assert_gt: fails on equal values, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rgt3 = tk_test_assert_gt(4.0, 4.0, "intentional assert_gt equal failure");
    ASSERT(rgt3 == 0, "assert_gt(4,4) returns 0");

    /* --- assert_lt: passes when a < b --- */
    int rlt1 = tk_test_assert_lt(3.0, 5.0, "3 < 5");
    ASSERT(rlt1 == 1, "assert_lt(3,5) returns 1");

    /* --- assert_lt: fails when a >= b, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rlt2 = tk_test_assert_lt(5.0, 3.0, "intentional assert_lt failure");
    ASSERT(rlt2 == 0, "assert_lt(5,3) returns 0");

    /* --- assert_gte: passes when a > b --- */
    int rge1 = tk_test_assert_gte(5.0, 3.0, "5 >= 3");
    ASSERT(rge1 == 1, "assert_gte(5,3) returns 1");

    /* --- assert_gte: passes when a == b --- */
    int rge2 = tk_test_assert_gte(4.0, 4.0, "4 >= 4");
    ASSERT(rge2 == 1, "assert_gte(4,4) returns 1");

    /* --- assert_gte: fails when a < b, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rge3 = tk_test_assert_gte(2.0, 5.0, "intentional assert_gte failure");
    ASSERT(rge3 == 0, "assert_gte(2,5) returns 0");

    /* --- assert_lte: passes when a < b --- */
    int rle1 = tk_test_assert_lte(3.0, 5.0, "3 <= 5");
    ASSERT(rle1 == 1, "assert_lte(3,5) returns 1");

    /* --- assert_lte: passes when a == b --- */
    int rle2 = tk_test_assert_lte(4.0, 4.0, "4 <= 4");
    ASSERT(rle2 == 1, "assert_lte(4,4) returns 1");

    /* --- assert_lte: fails when a > b, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rle3 = tk_test_assert_lte(5.0, 2.0, "intentional assert_lte failure");
    ASSERT(rle3 == 0, "assert_lte(5,2) returns 0");

    /* --- assert_contains: passes when needle found --- */
    int rc1 = tk_test_assert_contains("hello world", "world", "contains world");
    ASSERT(rc1 == 1, "assert_contains found returns 1");

    int rc2 = tk_test_assert_contains("hello", "hello", "contains exact match");
    ASSERT(rc2 == 1, "assert_contains exact match returns 1");

    /* --- assert_contains: fails when needle not found, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rc3 = tk_test_assert_contains("hello", "xyz", "intentional contains failure");
    ASSERT(rc3 == 0, "assert_contains not found returns 0");

    /* --- assert_not_contains: passes when needle absent --- */
    int rnc1 = tk_test_assert_not_contains("hello world", "xyz", "not contains xyz");
    ASSERT(rnc1 == 1, "assert_not_contains absent returns 1");

    int rnc2 = tk_test_assert_not_contains("", "abc", "not contains in empty string");
    ASSERT(rnc2 == 1, "assert_not_contains empty haystack returns 1");

    /* --- assert_not_contains: fails when needle present, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rnc3 = tk_test_assert_not_contains("hello world", "world", "intentional not_contains failure");
    ASSERT(rnc3 == 0, "assert_not_contains present returns 0");

    /* --- assert_nil: passes for NULL pointer --- */
    int rn1 = tk_test_assert_nil(NULL, "nil check on NULL");
    ASSERT(rn1 == 1, "assert_nil(NULL) returns 1");

    /* --- assert_nil: fails for non-NULL pointer, returns 0 --- */
    int dummy = 0;
    fprintf(stderr, "[expected diagnostic below]\n");
    int rn2 = tk_test_assert_nil(&dummy, "intentional nil failure");
    ASSERT(rn2 == 0, "assert_nil(non-NULL) returns 0");

    /* --- assert_not_nil: passes for non-NULL pointer --- */
    int rnn1 = tk_test_assert_not_nil(&dummy, "not nil check on non-NULL");
    ASSERT(rnn1 == 1, "assert_not_nil(non-NULL) returns 1");

    /* --- assert_not_nil: also passes for string literals --- */
    int rnn2 = tk_test_assert_not_nil("some string", "not nil on string literal");
    ASSERT(rnn2 == 1, "assert_not_nil(string) returns 1");

    /* --- assert_not_nil: fails for NULL pointer, returns 0 --- */
    fprintf(stderr, "[expected diagnostic below]\n");
    int rnn3 = tk_test_assert_not_nil(NULL, "intentional not_nil failure");
    ASSERT(rnn3 == 0, "assert_not_nil(NULL) returns 0");

    if (failures == 0) { printf("All test-module tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

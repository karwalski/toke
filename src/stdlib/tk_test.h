#ifndef TK_STDLIB_TK_TEST_H
#define TK_STDLIB_TK_TEST_H

/*
 * tk_test.h — C interface for the std.test standard library module.
 *
 * Named tk_test.h to avoid potential future naming conflicts.
 *
 * Type mappings:
 *   bool  = int  (0 = false, 1 = true)
 *   Str   = const char *  (null-terminated UTF-8)
 *
 * Failed assertions emit a structured diagnostic to stderr.
 * All functions return 1 on pass, 0 on fail.
 *
 * Story: 2.7.5  Branch: feature/stdlib-2.7-crypto-time-test
 */

/* test.assert(cond:bool;msg:Str):bool
 * Passes if cond is non-zero; emits diagnostic and returns 0 otherwise. */
int tk_test_assert(int cond, const char *msg);

/* test.assert_eq(a:Str;b:Str;msg:Str):bool
 * Passes if strcmp(a, b) == 0; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_eq(const char *a, const char *b, const char *msg);

/* test.assert_ne(a:Str;b:Str;msg:Str):bool
 * Passes if strcmp(a, b) != 0; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_ne(const char *a, const char *b, const char *msg);

/* test.assert_true(cond:bool;msg:Str):bool
 * Passes if cond is non-zero; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_true(int cond, const char *msg);

/* test.assert_false(cond:bool;msg:Str):bool
 * Passes if cond is zero; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_false(int cond, const char *msg);

/* test.assert_gt(a:double;b:double;msg:Str):bool
 * Passes if a > b; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_gt(double a, double b, const char *msg);

/* test.assert_lt(a:double;b:double;msg:Str):bool
 * Passes if a < b; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_lt(double a, double b, const char *msg);

/* test.assert_gte(a:double;b:double;msg:Str):bool
 * Passes if a >= b; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_gte(double a, double b, const char *msg);

/* test.assert_lte(a:double;b:double;msg:Str):bool
 * Passes if a <= b; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_lte(double a, double b, const char *msg);

/* test.assert_contains(haystack:Str;needle:Str;msg:Str):bool
 * Passes if strstr(haystack, needle) != NULL; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_contains(const char *haystack, const char *needle, const char *msg);

/* test.assert_not_contains(haystack:Str;needle:Str;msg:Str):bool
 * Passes if strstr(haystack, needle) == NULL; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_not_contains(const char *haystack, const char *needle, const char *msg);

/* test.assert_nil(ptr:void*;msg:Str):bool
 * Passes if ptr == NULL; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_nil(const void *ptr, const char *msg);

/* test.assert_not_nil(ptr:void*;msg:Str):bool
 * Passes if ptr != NULL; emits diagnostic and returns 0 otherwise. */
int tk_test_assert_not_nil(const void *ptr, const char *msg);

/* -----------------------------------------------------------------------
 * Story 28.4.2: test runner and lifecycle hooks
 * ----------------------------------------------------------------------- */

#include <stdint.h>

/* Function pointer type for test functions and lifecycle hooks. */
typedef void (*TkTestFn)(void);

/* Register a function called BEFORE each test run by tk_test_run(). */
void tk_test_setup(TkTestFn fn);

/* Register a function called AFTER each test run by tk_test_run()
 * (called even when the test fails). */
void tk_test_teardown(TkTestFn fn);

/* Run fn() as a named test.  Calls setup/teardown if registered.
 * Prints "PASS [name]" or "FAIL [name]" to stderr.
 * Increments the global passed/failed counters. */
void tk_test_run(const char *name, TkTestFn fn);

/* Accumulated test statistics. */
typedef struct {
    uint64_t passed;
    uint64_t failed;
    uint64_t skipped;
} TkTestSummary;

/* Return the accumulated pass/fail/skip counts. */
TkTestSummary tk_test_summary(void);

/* Print "Results: X passed, Y failed" to stderr.
 * Calls exit(1) if failed > 0. */
void tk_test_print_summary(void);

#endif /* TK_STDLIB_TK_TEST_H */

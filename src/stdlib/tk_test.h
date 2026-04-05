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

#endif /* TK_STDLIB_TK_TEST_H */

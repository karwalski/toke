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

#endif /* TK_STDLIB_TK_TEST_H */

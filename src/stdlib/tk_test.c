/*
 * tk_test.c — Implementation of the std.test standard library module.
 *
 * Named tk_test.c to avoid potential future naming conflicts.
 *
 * Failed assertions emit a structured diagnostic line to stderr in the format:
 *   DIAGNOSTIC: kind=assertion_failed msg="<msg>" [detail="<extra>"]
 *
 * This format mirrors the toke compiler diagnostic style so test output
 * can be parsed by tooling.
 *
 * Story: 2.7.5  Branch: feature/stdlib-2.7-crypto-time-test
 * Story: 28.4.1 comparison and containment assertions
 */

#include "tk_test.h"
#include <stdio.h>
#include <string.h>

int tk_test_assert(int cond, const char *msg)
{
    if (cond) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\"\n", msg);
    return 0;
}

int tk_test_assert_eq(const char *a, const char *b, const char *msg)
{
    if (!msg) msg = "(no message)";
    const char *sa = a ? a : "(null)";
    const char *sb = b ? b : "(null)";
    if (a && b && strcmp(a, b) == 0) return 1;
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_eq: \\\"%s\\\" != \\\"%s\\\"\"\n",
            msg, sa, sb);
    return 0;
}

int tk_test_assert_ne(const char *a, const char *b, const char *msg)
{
    if (!msg) msg = "(no message)";
    if (!(a && b && strcmp(a, b) == 0)) return 1;
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_ne: both equal \\\"%s\\\"\"\n",
            msg, a ? a : "(null)");
    return 0;
}

int tk_test_assert_true(int cond, const char *msg)
{
    if (cond) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_true: condition is false\"\n",
            msg);
    return 0;
}

int tk_test_assert_false(int cond, const char *msg)
{
    if (!cond) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_false: condition is true\"\n",
            msg);
    return 0;
}

int tk_test_assert_gt(double a, double b, const char *msg)
{
    if (a > b) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_gt: expected a > b but %.2f <= %.2f\"\n",
            msg, a, b);
    return 0;
}

int tk_test_assert_lt(double a, double b, const char *msg)
{
    if (a < b) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_lt: expected a < b but %.2f >= %.2f\"\n",
            msg, a, b);
    return 0;
}

int tk_test_assert_gte(double a, double b, const char *msg)
{
    if (a >= b) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_gte: expected a >= b but %.2f < %.2f\"\n",
            msg, a, b);
    return 0;
}

int tk_test_assert_lte(double a, double b, const char *msg)
{
    if (a <= b) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_lte: expected a <= b but %.2f > %.2f\"\n",
            msg, a, b);
    return 0;
}

int tk_test_assert_contains(const char *haystack, const char *needle, const char *msg)
{
    if (!msg) msg = "(no message)";
    if (haystack && needle && strstr(haystack, needle) != NULL) return 1;
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_contains: \\\"%s\\\" not found in \\\"%s\\\"\"\n",
            msg,
            needle    ? needle    : "(null)",
            haystack  ? haystack  : "(null)");
    return 0;
}

int tk_test_assert_not_contains(const char *haystack, const char *needle, const char *msg)
{
    if (!msg) msg = "(no message)";
    if (haystack && needle && strstr(haystack, needle) == NULL) return 1;
    /* also pass if either pointer is NULL — nothing to find */
    if (!haystack || !needle) return 1;
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_not_contains: \\\"%s\\\" found in \\\"%s\\\"\"\n",
            msg, needle, haystack);
    return 0;
}

int tk_test_assert_nil(const void *ptr, const char *msg)
{
    if (ptr == NULL) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_nil: pointer is not NULL\"\n",
            msg);
    return 0;
}

int tk_test_assert_not_nil(const void *ptr, const char *msg)
{
    if (ptr != NULL) return 1;
    if (!msg) msg = "(no message)";
    fprintf(stderr,
            "DIAGNOSTIC: kind=assertion_failed msg=\"%s\" detail=\"assert_not_nil: pointer is NULL\"\n",
            msg);
    return 0;
}

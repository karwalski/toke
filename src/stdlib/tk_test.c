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

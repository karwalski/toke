/* test_args.c — Unit tests for the std.args C library (Story 55.2). */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/stdlib/args.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

/* Simulate argv for tests */
static const char *test_argv[] = {"myprogram", "arg1", "arg2", NULL};
static int test_argc = 3;

int main(void)
{
    args_init(test_argc, (char **)test_argv);

    /* args_count */
    ASSERT(args_count() == 3, "args_count() == 3");

    /* args_get — valid indices */
    StrArgsResult r0 = args_get(0);
    ASSERT(!r0.is_err, "args_get(0) not is_err");
    ASSERT_STREQ(r0.ok, "myprogram", "args_get(0) == 'myprogram'");

    StrArgsResult r1 = args_get(1);
    ASSERT(!r1.is_err, "args_get(1) not is_err");
    ASSERT_STREQ(r1.ok, "arg1", "args_get(1) == 'arg1'");

    StrArgsResult r2 = args_get(2);
    ASSERT(!r2.is_err, "args_get(2) not is_err");
    ASSERT_STREQ(r2.ok, "arg2", "args_get(2) == 'arg2'");

    /* args_get — out of bounds */
    StrArgsResult r3 = args_get(3);
    ASSERT(r3.is_err, "args_get(3) is_err (out of bounds)");

    StrArgsResult r999 = args_get(999);
    ASSERT(r999.is_err, "args_get(999) is_err (out of bounds)");

    /* args_all */
    StrArray all = args_all();
    ASSERT(all.len == 3, "args_all() len == 3");
    ASSERT(all.data != NULL, "args_all() data != NULL");
    ASSERT_STREQ(all.data[0], "myprogram", "args_all() data[0] == 'myprogram'");

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return failures > 0 ? 1 : 0;
}

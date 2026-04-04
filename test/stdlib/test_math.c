/*
 * test_math.c — Unit tests for the std.math C library (Story 16.1.2).
 *
 * Compile and run:
 *   $(CC) $(CFLAGS) -o test_math test_math.c \
 *       ../../src/stdlib/math.c -lm -I../../src/stdlib && ./test_math
 *
 * Or from the repo root:
 *   cc -std=c99 -Wall -o /tmp/test_math \
 *       test/stdlib/test_math.c src/stdlib/math.c -lm \
 *       -Isrc/stdlib && /tmp/test_math
 */

#include <stdio.h>
#include <math.h>
#include "../../src/stdlib/math.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_DBL_EQ(a, b, eps, msg) \
    ASSERT(fabs((a) - (b)) <= (eps), msg)

/* Build an F64Array from a stack array literal. */
#define FA(arr) ((F64Array){ (arr), sizeof(arr)/sizeof((arr)[0]) })

int main(void)
{
    /* --- math_sum --- */
    {
        double xs[] = {1.0, 2.0, 3.0, 4.0, 5.0};
        ASSERT_DBL_EQ(math_sum(FA(xs)), 15.0, 1e-12, "sum([1,2,3,4,5]) == 15.0");
    }

    /* --- math_mean --- */
    {
        double xs[] = {1.0, 2.0, 3.0};
        ASSERT_DBL_EQ(math_mean(FA(xs)), 2.0, 1e-12, "mean([1,2,3]) == 2.0");
    }

    /* --- math_median: odd count --- */
    {
        double xs[] = {3.0, 1.0, 2.0};
        ASSERT_DBL_EQ(math_median(FA(xs)), 2.0, 1e-12, "median([3,1,2]) == 2.0");
    }

    /* --- math_median: even count --- */
    {
        double xs[] = {1.0, 2.0, 3.0, 4.0};
        ASSERT_DBL_EQ(math_median(FA(xs)), 2.5, 1e-12, "median([1,2,3,4]) == 2.5");
    }

    /* --- math_stddev: classic textbook example --- */
    /* Population stddev of {2,4,4,4,5,5,7,9} = 2.0 */
    {
        double xs[] = {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
        ASSERT_DBL_EQ(math_stddev(FA(xs)), 2.0, 1e-9, "stddev([2,4,4,4,5,5,7,9]) ≈ 2.0");
    }

    /* --- math_percentile: [0..100] --- */
    {
        double xs[101];
        for (int i = 0; i <= 100; i++) xs[i] = (double)i;
        F64Array fa = { xs, 101 };
        ASSERT_DBL_EQ(math_percentile(fa, 50.0), 50.0, 1e-9,
                      "percentile([0..100], 50) ≈ 50.0");
        ASSERT_DBL_EQ(math_percentile(fa, 0.0),   0.0, 1e-12,
                      "percentile([0..100], 0) == 0.0");
        ASSERT_DBL_EQ(math_percentile(fa, 100.0), 100.0, 1e-12,
                      "percentile([0..100], 100) == 100.0");
    }

    /* --- math_linreg: perfect line y=2x --- */
    {
        double xs[] = {1.0, 2.0, 3.0};
        double ys[] = {2.0, 4.0, 6.0};
        LinRegResult lr = math_linreg(FA(xs), FA(ys));
        ASSERT_DBL_EQ(lr.slope,     2.0, 1e-12, "linreg([1,2,3],[2,4,6]) slope == 2.0");
        ASSERT_DBL_EQ(lr.intercept, 0.0, 1e-12, "linreg([1,2,3],[2,4,6]) intercept == 0.0");
        ASSERT_DBL_EQ(lr.r_squared, 1.0, 1e-12, "linreg([1,2,3],[2,4,6]) r_squared == 1.0");
    }

    /* --- math_min / math_max --- */
    {
        double xs[] = {5.0, 3.0, 8.0, 1.0};
        ASSERT_DBL_EQ(math_min(FA(xs)), 1.0, 1e-12, "min([5,3,8,1]) == 1.0");
        ASSERT_DBL_EQ(math_max(FA(xs)), 8.0, 1e-12, "max([5,3,8,1]) == 8.0");
    }

    /* --- scalar functions --- */
    ASSERT_DBL_EQ(math_abs(-5.0),       5.0,    1e-12, "abs(-5.0) == 5.0");
    ASSERT_DBL_EQ(math_sqrt(9.0),       3.0,    1e-12, "sqrt(9.0) == 3.0");
    ASSERT_DBL_EQ(math_floor(3.7),      3.0,    1e-12, "floor(3.7) == 3.0");
    ASSERT_DBL_EQ(math_ceil(3.2),       4.0,    1e-12, "ceil(3.2) == 4.0");
    ASSERT_DBL_EQ(math_pow(2.0, 10.0),  1024.0, 1e-9,  "pow(2.0, 10.0) == 1024.0");

    /* --- empty array returns NaN --- */
    {
        F64Array empty = { NULL, 0 };
        double m = math_mean(empty);
        ASSERT(isnan(m), "mean(empty) returns NaN");
    }

    /* --- summary --- */
    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}

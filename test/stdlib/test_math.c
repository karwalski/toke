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

    /* ===================================================================
     * Edge-case and hardening tests (Story 20.1.5)
     * =================================================================== */

    /* --- Empty arrays return NaN for all aggregate functions --- */
    {
        F64Array empty = { NULL, 0 };
        ASSERT(isnan(math_mean(empty)),       "mean(empty) returns NaN");
        ASSERT(isnan(math_median(empty)),     "median(empty) returns NaN");
        ASSERT(isnan(math_stddev(empty)),     "stddev(empty) returns NaN");
        ASSERT(isnan(math_variance(empty)),   "variance(empty) returns NaN");
        ASSERT(isnan(math_min(empty)),        "min(empty) returns NaN");
        ASSERT(isnan(math_max(empty)),        "max(empty) returns NaN");
        ASSERT(isnan(math_percentile(empty, 50.0)),
               "percentile(empty, 50) returns NaN");
        ASSERT_DBL_EQ(math_sum(empty), 0.0, 1e-12, "sum(empty) == 0.0");
    }

    /* --- NaN propagation in input arrays --- */
    {
        double xs[] = {1.0, NAN, 3.0};
        ASSERT(isnan(math_sum(FA(xs))),    "sum([1,NaN,3]) is NaN");
        ASSERT(isnan(math_mean(FA(xs))),   "mean([1,NaN,3]) is NaN");
        ASSERT(isnan(math_stddev(FA(xs))), "stddev([1,NaN,3]) is NaN");
    }

    /* --- +/-Inf inputs --- */
    {
        double xs[] = {1.0, INFINITY, 3.0};
        ASSERT(math_sum(FA(xs)) == INFINITY, "sum([1,Inf,3]) == Inf");
        ASSERT(math_mean(FA(xs)) == INFINITY, "mean([1,Inf,3]) == Inf");
        ASSERT(math_max(FA(xs)) == INFINITY, "max([1,Inf,3]) == Inf");
    }
    {
        double xs[] = {1.0, -INFINITY, 3.0};
        ASSERT(math_sum(FA(xs)) == -INFINITY, "sum([1,-Inf,3]) == -Inf");
        ASSERT(math_min(FA(xs)) == -INFINITY, "min([1,-Inf,3]) == -Inf");
    }
    {
        double xs[] = {INFINITY, -INFINITY};
        ASSERT(isnan(math_sum(FA(xs))), "sum([Inf,-Inf]) is NaN");
    }

    /* --- Single-element arrays --- */
    {
        double xs[] = {42.0};
        ASSERT_DBL_EQ(math_median(FA(xs)), 42.0, 1e-12,
                      "median([42]) == 42.0");
        ASSERT_DBL_EQ(math_stddev(FA(xs)), 0.0, 1e-12,
                      "stddev([42]) == 0.0 (single element)");
        ASSERT_DBL_EQ(math_variance(FA(xs)), 0.0, 1e-12,
                      "variance([42]) == 0.0 (single element)");
        ASSERT_DBL_EQ(math_mean(FA(xs)), 42.0, 1e-12,
                      "mean([42]) == 42.0");
        ASSERT_DBL_EQ(math_min(FA(xs)), 42.0, 1e-12,
                      "min([42]) == 42.0");
        ASSERT_DBL_EQ(math_max(FA(xs)), 42.0, 1e-12,
                      "max([42]) == 42.0");
    }

    /* --- Identical values: stddev == 0 --- */
    {
        double xs[] = {7.0, 7.0, 7.0, 7.0, 7.0};
        ASSERT_DBL_EQ(math_stddev(FA(xs)), 0.0, 1e-12,
                      "stddev([7,7,7,7,7]) == 0.0");
        ASSERT_DBL_EQ(math_variance(FA(xs)), 0.0, 1e-12,
                      "variance([7,7,7,7,7]) == 0.0");
        ASSERT_DBL_EQ(math_median(FA(xs)), 7.0, 1e-12,
                      "median([7,7,7,7,7]) == 7.0");
    }

    /* --- Percentile at extreme boundaries --- */
    {
        double xs[] = {10.0, 20.0, 30.0, 40.0, 50.0};
        ASSERT_DBL_EQ(math_percentile(FA(xs), 0.0), 10.0, 1e-12,
                      "percentile(5-elem, 0) == min");
        ASSERT_DBL_EQ(math_percentile(FA(xs), 100.0), 50.0, 1e-12,
                      "percentile(5-elem, 100) == max");
        /* Near-boundary: 0.001 and 99.999 */
        double p_low = math_percentile(FA(xs), 0.001);
        ASSERT(p_low >= 10.0 && p_low <= 10.001,
               "percentile(5-elem, 0.001) near min");
        double p_high = math_percentile(FA(xs), 99.999);
        ASSERT(p_high >= 49.999 && p_high <= 50.0,
               "percentile(5-elem, 99.999) near max");
        /* Negative and >100 clamp */
        ASSERT_DBL_EQ(math_percentile(FA(xs), -5.0), 10.0, 1e-12,
                      "percentile(5-elem, -5) clamps to min");
        ASSERT_DBL_EQ(math_percentile(FA(xs), 105.0), 50.0, 1e-12,
                      "percentile(5-elem, 105) clamps to max");
    }

    /* --- Single-element percentile --- */
    {
        double xs[] = {99.0};
        ASSERT_DBL_EQ(math_percentile(FA(xs), 0.0), 99.0, 1e-12,
                      "percentile([99], 0) == 99");
        ASSERT_DBL_EQ(math_percentile(FA(xs), 50.0), 99.0, 1e-12,
                      "percentile([99], 50) == 99");
        ASSERT_DBL_EQ(math_percentile(FA(xs), 100.0), 99.0, 1e-12,
                      "percentile([99], 100) == 99");
    }

    /* --- Linear regression: vertical line (all same x) --- */
    {
        double xs[] = {5.0, 5.0, 5.0};
        double ys[] = {1.0, 2.0, 3.0};
        LinRegResult lr = math_linreg(FA(xs), FA(ys));
        ASSERT(isnan(lr.slope),     "linreg vertical: slope is NaN");
        ASSERT(isnan(lr.intercept), "linreg vertical: intercept is NaN");
        ASSERT(isnan(lr.r_squared), "linreg vertical: r_squared is NaN");
    }

    /* --- Linear regression: all same y (horizontal line) --- */
    {
        double xs[] = {1.0, 2.0, 3.0};
        double ys[] = {5.0, 5.0, 5.0};
        LinRegResult lr = math_linreg(FA(xs), FA(ys));
        ASSERT_DBL_EQ(lr.slope, 0.0, 1e-12,
                      "linreg horizontal: slope == 0");
        ASSERT_DBL_EQ(lr.intercept, 5.0, 1e-12,
                      "linreg horizontal: intercept == 5");
        ASSERT_DBL_EQ(lr.r_squared, 1.0, 1e-12,
                      "linreg horizontal: r_squared == 1 (perfect flat)");
    }

    /* --- Linear regression: single point --- */
    {
        double xs[] = {3.0};
        double ys[] = {7.0};
        LinRegResult lr = math_linreg(FA(xs), FA(ys));
        /* Single point: sxx==0, returns NaN */
        ASSERT(isnan(lr.slope), "linreg single point: slope is NaN");
    }

    /* --- Linear regression: empty arrays --- */
    {
        F64Array empty = { NULL, 0 };
        LinRegResult lr = math_linreg(empty, empty);
        ASSERT(isnan(lr.slope),     "linreg empty: slope is NaN");
        ASSERT(isnan(lr.intercept), "linreg empty: intercept is NaN");
        ASSERT(isnan(lr.r_squared), "linreg empty: r_squared is NaN");
    }

    /* --- Very large array (1000+ elements) --- */
    {
        double big[2000];
        for (int i = 0; i < 2000; i++) big[i] = (double)(i + 1);
        F64Array fa = { big, 2000 };
        /* sum = n*(n+1)/2 = 2000*2001/2 = 2001000 */
        ASSERT_DBL_EQ(math_sum(fa), 2001000.0, 1e-6,
                      "sum(1..2000) == 2001000");
        ASSERT_DBL_EQ(math_mean(fa), 1000.5, 1e-9,
                      "mean(1..2000) == 1000.5");
        /* median of 1..2000 (even count) = (1000+1001)/2 = 1000.5 */
        ASSERT_DBL_EQ(math_median(fa), 1000.5, 1e-9,
                      "median(1..2000) == 1000.5");
        ASSERT_DBL_EQ(math_min(fa), 1.0, 1e-12,
                      "min(1..2000) == 1.0");
        ASSERT_DBL_EQ(math_max(fa), 2000.0, 1e-12,
                      "max(1..2000) == 2000.0");
        ASSERT_DBL_EQ(math_percentile(fa, 50.0), 1000.5, 1e-6,
                      "percentile(1..2000, 50) == 1000.5");
    }

    /* --- Floating point precision: catastrophic cancellation test --- */
    {
        /* Values near a large offset: stddev should reflect small spread. */
        double xs[] = {1e15 + 1.0, 1e15 + 2.0, 1e15 + 3.0};
        double var = math_variance(FA(xs));
        /* Population variance of {1,2,3} offset = 2/3 ≈ 0.6667 */
        /* With naive algorithm on doubles, this may lose precision. */
        /* We allow a generous tolerance to verify it doesn't blow up. */
        ASSERT(var >= 0.0 && var < 2.0,
               "variance near 1e15 offset is non-negative and reasonable");
    }

    /* --- Scalar edge cases --- */
    ASSERT_DBL_EQ(math_abs(0.0), 0.0, 1e-12, "abs(0.0) == 0.0");
    ASSERT_DBL_EQ(math_abs(-0.0), 0.0, 1e-12, "abs(-0.0) == 0.0");
    ASSERT(isnan(math_sqrt(-1.0)), "sqrt(-1.0) is NaN");
    ASSERT_DBL_EQ(math_sqrt(0.0), 0.0, 1e-12, "sqrt(0.0) == 0.0");
    ASSERT(math_pow(2.0, -1.0) == 0.5, "pow(2, -1) == 0.5");
    ASSERT_DBL_EQ(math_floor(-3.7), -4.0, 1e-12, "floor(-3.7) == -4.0");
    ASSERT_DBL_EQ(math_ceil(-3.2), -3.0, 1e-12, "ceil(-3.2) == -3.0");

    /* ===================================================================
     * Trig / transcendental functions (Story 29.3.1)
     * =================================================================== */

#define TK_PI 3.14159265358979323846

    /* --- math_sin --- */
    ASSERT_DBL_EQ(math_sin(0.0),       0.0, 1e-9, "sin(0.0) == 0.0");
    ASSERT_DBL_EQ(math_sin(TK_PI/2.0), 1.0, 1e-9, "sin(pi/2) == 1.0");

    /* --- math_cos --- */
    ASSERT_DBL_EQ(math_cos(0.0), 1.0, 1e-9, "cos(0.0) == 1.0");

    /* --- math_tan --- */
    ASSERT_DBL_EQ(math_tan(0.0), 0.0, 1e-9, "tan(0.0) == 0.0");

    /* --- math_asin --- */
    ASSERT_DBL_EQ(math_asin(0.0), 0.0, 1e-9, "asin(0.0) == 0.0");

    /* --- math_acos --- */
    ASSERT_DBL_EQ(math_acos(1.0), 0.0, 1e-9, "acos(1.0) == 0.0");

    /* --- math_atan --- */
    ASSERT_DBL_EQ(math_atan(0.0), 0.0, 1e-9, "atan(0.0) == 0.0");

    /* --- math_atan2 --- */
    ASSERT_DBL_EQ(math_atan2(1.0, 0.0), TK_PI/2.0, 1e-9,
                  "atan2(1.0, 0.0) == pi/2");

    /* --- math_log --- */
    ASSERT_DBL_EQ(math_log(1.0), 0.0, 1e-9, "log(1.0) == 0.0");

    /* --- math_log10 --- */
    ASSERT_DBL_EQ(math_log10(1.0),   0.0, 1e-9, "log10(1.0) == 0.0");
    ASSERT_DBL_EQ(math_log10(100.0), 2.0, 1e-9, "log10(100.0) == 2.0");

    /* --- math_exp --- */
    ASSERT_DBL_EQ(math_exp(0.0), 1.0, 1e-9, "exp(0.0) == 1.0");

    /* --- math_hypot --- */
    ASSERT_DBL_EQ(math_hypot(3.0, 4.0), 5.0, 1e-9, "hypot(3.0, 4.0) == 5.0");

    /* ===================================================================
     * Rounding, NaN/Inf classification, and combinatorics (Story 29.3.2)
     * =================================================================== */

    /* --- math_round --- */
    ASSERT_DBL_EQ(math_round(3.14159, 2), 3.14, 1e-9,
                  "round(3.14159, 2) ≈ 3.14");

    /* --- math_trunc --- */
    ASSERT_DBL_EQ(math_trunc(3.9),  3.0, 1e-12, "trunc(3.9) == 3.0");
    ASSERT_DBL_EQ(math_trunc(-3.9), -3.0, 1e-12, "trunc(-3.9) == -3.0");

    /* --- math_fmod --- */
    ASSERT_DBL_EQ(math_fmod(10.0, 3.0), 1.0, 1e-9, "fmod(10.0, 3.0) ≈ 1.0");

    /* --- math_isnan --- */
    {
        volatile double nan_val = 0.0;
        nan_val /= nan_val;
        ASSERT(math_isnan(nan_val) == 1, "isnan(0.0/0.0) == 1");
        ASSERT(math_isnan(1.0)     == 0, "isnan(1.0) == 0");
    }

    /* --- math_isinf --- */
    {
        volatile double inf_val = 1.0;
        volatile double zero    = 0.0;
        inf_val /= zero;
        ASSERT(math_isinf(inf_val) == 1, "isinf(1.0/0.0) == 1");
        ASSERT(math_isinf(1.0)     == 0, "isinf(1.0) == 0");
    }

    /* --- math_copysign --- */
    ASSERT_DBL_EQ(math_copysign(3.0, -1.0), -3.0, 1e-12,
                  "copysign(3.0, -1.0) == -3.0");
    ASSERT_DBL_EQ(math_copysign(-3.0, 1.0),  3.0, 1e-12,
                  "copysign(-3.0, 1.0) == 3.0");

    /* --- math_gcd --- */
    ASSERT(math_gcd(12, 8) == 4, "gcd(12, 8) == 4");
    ASSERT(math_gcd(0, 5)  == 5, "gcd(0, 5) == 5");
    ASSERT(math_gcd(5, 0)  == 5, "gcd(5, 0) == 5");
    ASSERT(math_gcd(-12, 8) == 4, "gcd(-12, 8) == 4 (always non-negative)");

    /* --- math_lcm --- */
    ASSERT(math_lcm(4, 6) == 12, "lcm(4, 6) == 12");
    ASSERT(math_lcm(0, 5) == 0,  "lcm(0, 5) == 0");

    /* --- math_factorial --- */
    ASSERT(math_factorial(0)  == 1,   "factorial(0) == 1");
    ASSERT(math_factorial(1)  == 1,   "factorial(1) == 1");
    ASSERT(math_factorial(5)  == 120, "factorial(5) == 120");
    ASSERT(math_factorial(20) == (int64_t)2432902008176640000LL,
           "factorial(20) == 2432902008176640000");
    ASSERT(math_factorial(21) == -1,  "factorial(21) == -1 (overflow)");

    /* --- math_mode --- */
    {
        double xs_mode[] = {1.0, 2.0, 2.0, 3.0};
        ASSERT_DBL_EQ(math_mode(FA(xs_mode)), 2.0, 1e-12,
                      "mode([1,2,2,3]) == 2.0");
    }
    {
        /* Tie: no unique mode → NaN */
        double xs_tie[] = {1.0, 1.0, 2.0, 2.0};
        ASSERT(isnan(math_mode(FA(xs_tie))),
               "mode([1,1,2,2]) is NaN (tie)");
    }
    {
        /* All unique → NaN */
        double xs_uniq[] = {1.0, 2.0, 3.0};
        ASSERT(isnan(math_mode(FA(xs_uniq))),
               "mode([1,2,3]) is NaN (all unique)");
    }
    {
        /* Empty → NaN */
        F64Array empty = { NULL, 0 };
        ASSERT(isnan(math_mode(empty)), "mode(empty) is NaN");
    }

    /* --- MATH_TAU ≈ 2 * pi --- */
    ASSERT_DBL_EQ(MATH_TAU, 2.0 * TK_PI, 1e-9, "MATH_TAU ≈ 2 * pi");

    /* --- summary --- */
    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}

/*
 * math_glue.c — i64-ABI wrappers for std.math module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.math.
 */

#include "math.h"
#include <stdint.h>
#include <string.h>

/* f64<->i64 bitcast helpers */
static double i64_to_f64(int64_t i) {
    double d;
    memcpy(&d, &i, sizeof(d));
    return d;
}
static int64_t f64_to_i64(double d) {
    int64_t i;
    memcpy(&i, &d, sizeof(i));
    return i;
}

int64_t tk_math_abs_w(int64_t x) { return f64_to_i64(math_abs(i64_to_f64(x))); }
int64_t tk_math_max_w(int64_t a, int64_t b) { return a > b ? a : b; }
int64_t tk_math_min_w(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t tk_math_pow_w(int64_t base, int64_t exp) {
    return f64_to_i64(math_pow(i64_to_f64(base), i64_to_f64(exp)));
}
int64_t tk_math_floor_w(int64_t x) { return f64_to_i64(math_floor(i64_to_f64(x))); }
int64_t tk_math_mean_w(int64_t arr) { (void)arr; return 0; }
int64_t tk_math_median_w(int64_t arr) { (void)arr; return 0; }
int64_t tk_math_percentile_w(int64_t arr, int64_t p) { (void)arr; (void)p; return 0; }
int64_t tk_math_linreg_w(int64_t xs, int64_t ys) { (void)xs; (void)ys; return 0; }
int64_t tk_math_sqrt_w(int64_t x) { return f64_to_i64(math_sqrt(i64_to_f64(x))); }
int64_t tk_math_sum_w(int64_t arr) { (void)arr; return 0; }
int64_t tk_math_stddev_w(int64_t arr) { (void)arr; return 0; }
int64_t tk_math_ceil_w(int64_t v) { return f64_to_i64(math_ceil(i64_to_f64(v))); }
int64_t tk_math_round_w(int64_t v) { return f64_to_i64(math_round(i64_to_f64(v), 0)); }

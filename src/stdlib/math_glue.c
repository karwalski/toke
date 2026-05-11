/*
 * math_glue.c — i64-ABI wrappers for std.math module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.math.
 */

#include "math.h"
#include <stdint.h>
#include <stdlib.h>
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
/*
 * Toke array layout for F64Array:
 *   ptr[-1] = element count (i64)
 *   ptr[0..count-1] = f64 values stored as i64 (bitcast)
 *
 * We decode the packed toke array into an F64Array struct.
 */
static F64Array decode_f64_array(int64_t arr) {
    F64Array fa = {NULL, 0};
    if (!arr) return fa;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t count = ptr[-1];
    if (count <= 0) return fa;
    /* The i64 values are bitcast f64s stored contiguously */
    fa.data = (const double *)ptr;
    fa.len  = (uint64_t)count;
    return fa;
}

int64_t tk_math_mean_w(int64_t arr) {
    F64Array fa = decode_f64_array(arr);
    return f64_to_i64(math_mean(fa));
}

int64_t tk_math_median_w(int64_t arr) {
    F64Array fa = decode_f64_array(arr);
    return f64_to_i64(math_median(fa));
}

int64_t tk_math_percentile_w(int64_t arr, int64_t p) {
    F64Array fa = decode_f64_array(arr);
    return f64_to_i64(math_percentile(fa, i64_to_f64(p)));
}

int64_t tk_math_linreg_w(int64_t xs, int64_t ys) {
    F64Array fxs = decode_f64_array(xs);
    F64Array fys = decode_f64_array(ys);
    LinRegResult lr = math_linreg(fxs, fys);
    /* Pack slope, intercept, r_squared into a heap block of 3 doubles */
    double *result = (double *)malloc(3 * sizeof(double));
    if (!result) return 0;
    result[0] = lr.slope;
    result[1] = lr.intercept;
    result[2] = lr.r_squared;
    return (int64_t)(intptr_t)result;
}
int64_t tk_math_sqrt_w(int64_t x) { return f64_to_i64(math_sqrt(i64_to_f64(x))); }
int64_t tk_math_sum_w(int64_t arr) {
    F64Array fa = decode_f64_array(arr);
    return f64_to_i64(math_sum(fa));
}

int64_t tk_math_stddev_w(int64_t arr) {
    F64Array fa = decode_f64_array(arr);
    return f64_to_i64(math_stddev(fa));
}
int64_t tk_math_ceil_w(int64_t v) { return f64_to_i64(math_ceil(i64_to_f64(v))); }
int64_t tk_math_round_w(int64_t v) { return f64_to_i64(math_round(i64_to_f64(v), 0)); }

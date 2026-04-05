#ifndef TK_STDLIB_MATH_H
#define TK_STDLIB_MATH_H

/*
 * math.h — C interface for the std.math standard library module.
 *
 * Type mappings:
 *   [f64]        = F64Array  (defined below)
 *   f64          = double
 *   linregresult = LinRegResult
 *
 * Link with -lm.
 *
 * Story: 16.1.2
 */

#include <stdint.h>

#ifndef TK_F64ARRAY_DEFINED
#define TK_F64ARRAY_DEFINED
typedef struct { const double *data; uint64_t len; } F64Array;
#endif

typedef struct {
    double slope;
    double intercept;
    double r_squared;
} LinRegResult;

/* math.sum(xs:[f64]):f64 */
double       math_sum(F64Array xs);

/* math.mean(xs:[f64]):f64 — NaN if empty */
double       math_mean(F64Array xs);

/* math.median(xs:[f64]):f64 — NaN if empty; does NOT sort in-place */
double       math_median(F64Array xs);

/* math.stddev(xs:[f64]):f64 — population stddev; NaN if empty */
double       math_stddev(F64Array xs);

/* math.variance(xs:[f64]):f64 — population variance; NaN if empty */
double       math_variance(F64Array xs);

/* math.percentile(xs:[f64];p:f64):f64 — p in [0,100]; NaN if empty */
double       math_percentile(F64Array xs, double p);

/* math.linreg(xs:[f64];ys:[f64]):linregresult — least squares; sets r_squared */
LinRegResult math_linreg(F64Array xs, F64Array ys);

/* math.min(xs:[f64]):f64 */
double       math_min(F64Array xs);

/* math.max(xs:[f64]):f64 */
double       math_max(F64Array xs);

/* math.abs(x:f64):f64 */
double       math_abs(double x);

/* math.sqrt(x:f64):f64 */
double       math_sqrt(double x);

/* math.floor(x:f64):f64 */
double       math_floor(double x);

/* math.ceil(x:f64):f64 */
double       math_ceil(double x);

/* math.pow(base:f64;exp:f64):f64 */
double       math_pow(double base, double exp);

/* Trig / transcendental wrappers (Story 29.3.1) */

/* math.sin(x:f64):f64 */
double       math_sin(double x);

/* math.cos(x:f64):f64 */
double       math_cos(double x);

/* math.tan(x:f64):f64 */
double       math_tan(double x);

/* math.asin(x:f64):f64 */
double       math_asin(double x);

/* math.acos(x:f64):f64 */
double       math_acos(double x);

/* math.atan(x:f64):f64 */
double       math_atan(double x);

/* math.atan2(y:f64;x:f64):f64 */
double       math_atan2(double y, double x);

/* math.log(x:f64):f64 — natural log ln(x) */
double       math_log(double x);

/* math.log10(x:f64):f64 — log base 10 */
double       math_log10(double x);

/* math.exp(x:f64):f64 — e^x */
double       math_exp(double x);

/* math.hypot(x:f64;y:f64):f64 — sqrt(x*x + y*y) */
double       math_hypot(double x, double y);

/* -----------------------------------------------------------------------
 * Rounding, NaN/Inf classification, and combinatorics (Story 29.3.2)
 * ----------------------------------------------------------------------- */

/* math.round(x:f64;digits:int):f64 — round to N decimal places */
double       math_round(double x, int digits);

/* math.trunc(x:f64):f64 — truncate toward zero */
double       math_trunc(double x);

/* math.fmod(x:f64;y:f64):f64 — floating-point remainder */
double       math_fmod(double x, double y);

/* math.isnan(x:f64):int — 1 if x is NaN, 0 otherwise */
int          math_isnan(double x);

/* math.isinf(x:f64):int — 1 if x is +/-Inf, 0 otherwise */
int          math_isinf(double x);

/* math.copysign(x:f64;y:f64):f64 — magnitude of x with sign of y */
double       math_copysign(double x, double y);

/* math.gcd(a:int;b:int):int — greatest common divisor (always >= 0) */
int64_t      math_gcd(int64_t a, int64_t b);

/* math.lcm(a:int;b:int):int — least common multiple (always >= 0);
 * returns -1 on overflow */
int64_t      math_lcm(int64_t a, int64_t b);

/* math.factorial(n:uint):int — n!; returns -1 for n > 20 (overflow) */
int64_t      math_factorial(uint64_t n);

/* math.mode(xs:[f64]):f64 — most frequent value;
 * returns NaN if empty or no unique mode */
double       math_mode(F64Array xs);

#define MATH_E   2.718281828459045235360
#define MATH_TAU 6.283185307179586476925

#endif /* TK_STDLIB_MATH_H */

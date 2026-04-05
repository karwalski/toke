/*
 * math.c — Implementation of the std.math standard library module.
 *
 * Implements statistical and scalar math functions over F64Array.
 *
 * Link flag required: -lm
 *
 * malloc is permitted here for temporary sort copies in math_median and
 * math_percentile. Callers do not own any returned pointers (all results
 * are returned by value).
 *
 * Story: 16.1.2
 */

#include <math.h>   /* system math — must be angle brackets, not quotes */
#include "math.h"
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

/* Returns a heap-allocated sorted copy of xs.data, or NULL on empty/alloc
 * failure. Caller must free(). */
static double *sorted_copy(F64Array xs)
{
    if (xs.len == 0) return NULL;
    double *buf = malloc(xs.len * sizeof(double));
    if (!buf) return NULL;
    memcpy(buf, xs.data, xs.len * sizeof(double));
    qsort(buf, (size_t)xs.len, sizeof(double), cmp_double);
    return buf;
}

/* -----------------------------------------------------------------------
 * Aggregate / statistical functions
 * ----------------------------------------------------------------------- */

double math_sum(F64Array xs)
{
    double s = 0.0;
    for (uint64_t i = 0; i < xs.len; i++) {
        s += xs.data[i];
    }
    return s;
}

double math_mean(F64Array xs)
{
    if (xs.len == 0) return 0.0 / 0.0; /* NaN */
    return math_sum(xs) / (double)xs.len;
}

double math_median(F64Array xs)
{
    if (xs.len == 0) return 0.0 / 0.0; /* NaN */

    double *buf = sorted_copy(xs);
    if (!buf) return 0.0 / 0.0;

    double result;
    uint64_t n = xs.len;
    if (n % 2 == 1) {
        result = buf[n / 2];
    } else {
        result = (buf[n / 2 - 1] + buf[n / 2]) / 2.0;
    }
    free(buf);
    return result;
}

double math_variance(F64Array xs)
{
    if (xs.len == 0) return 0.0 / 0.0; /* NaN */
    double m = math_mean(xs);
    double acc = 0.0;
    for (uint64_t i = 0; i < xs.len; i++) {
        double d = xs.data[i] - m;
        acc += d * d;
    }
    return acc / (double)xs.len;
}

double math_stddev(F64Array xs)
{
    return sqrt(math_variance(xs));
}

double math_percentile(F64Array xs, double p)
{
    if (xs.len == 0) return 0.0 / 0.0; /* NaN */

    double *buf = sorted_copy(xs);
    if (!buf) return 0.0 / 0.0;

    double result;
    uint64_t n = xs.len;

    if (p <= 0.0) {
        result = buf[0];
    } else if (p >= 100.0) {
        result = buf[n - 1];
    } else {
        /* Linear interpolation between nearest ranks. */
        double rank = (p / 100.0) * (double)(n - 1);
        uint64_t lo = (uint64_t)rank;
        uint64_t hi = lo + 1;
        if (hi >= n) hi = n - 1;
        double frac = rank - (double)lo;
        result = buf[lo] + frac * (buf[hi] - buf[lo]);
    }

    free(buf);
    return result;
}

LinRegResult math_linreg(F64Array xs, F64Array ys)
{
    LinRegResult r;
    r.slope     = 0.0 / 0.0; /* NaN defaults */
    r.intercept = 0.0 / 0.0;
    r.r_squared = 0.0 / 0.0;

    uint64_t n = xs.len < ys.len ? xs.len : ys.len;
    if (n == 0) return r;

    /* Compute means. */
    double sum_x = 0.0, sum_y = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        sum_x += xs.data[i];
        sum_y += ys.data[i];
    }
    double mx = sum_x / (double)n;
    double my = sum_y / (double)n;

    /* Least squares: slope = Sxy / Sxx */
    double sxx = 0.0, sxy = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        double dx = xs.data[i] - mx;
        sxx += dx * dx;
        sxy += dx * (ys.data[i] - my);
    }

    if (sxx == 0.0) {
        /* Vertical line — undefined slope. */
        return r;
    }

    r.slope     = sxy / sxx;
    r.intercept = my - r.slope * mx;

    /* r_squared = 1 - SS_res / SS_tot */
    double ss_res = 0.0, ss_tot = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        double y_pred = r.slope * xs.data[i] + r.intercept;
        double res    = ys.data[i] - y_pred;
        double dev    = ys.data[i] - my;
        ss_res += res * res;
        ss_tot += dev * dev;
    }
    r.r_squared = (ss_tot == 0.0) ? 1.0 : 1.0 - ss_res / ss_tot;

    return r;
}

double math_min(F64Array xs)
{
    if (xs.len == 0) return 0.0 / 0.0; /* NaN */
    double m = xs.data[0];
    for (uint64_t i = 1; i < xs.len; i++) {
        if (xs.data[i] < m) m = xs.data[i];
    }
    return m;
}

double math_max(F64Array xs)
{
    if (xs.len == 0) return 0.0 / 0.0; /* NaN */
    double m = xs.data[0];
    for (uint64_t i = 1; i < xs.len; i++) {
        if (xs.data[i] > m) m = xs.data[i];
    }
    return m;
}

/* -----------------------------------------------------------------------
 * Scalar wrappers around <math.h>
 * ----------------------------------------------------------------------- */

double math_abs(double x)   { return fabs(x);      }
double math_sqrt(double x)  { return sqrt(x);       }
double math_floor(double x) { return floor(x);      }
double math_ceil(double x)  { return ceil(x);       }
double math_pow(double base, double exp) { return pow(base, exp); }

/* -----------------------------------------------------------------------
 * Trig / transcendental wrappers (Story 29.3.1)
 * ----------------------------------------------------------------------- */

double math_sin(double x)              { return sin(x);        }
double math_cos(double x)              { return cos(x);        }
double math_tan(double x)              { return tan(x);        }
double math_asin(double x)             { return asin(x);       }
double math_acos(double x)             { return acos(x);       }
double math_atan(double x)             { return atan(x);       }
double math_atan2(double y, double x)  { return atan2(y, x);   }
double math_log(double x)              { return log(x);        }
double math_log10(double x)            { return log10(x);      }
double math_exp(double x)              { return exp(x);        }
double math_hypot(double x, double y)  { return hypot(x, y);   }

#ifndef TK_STDLIB_ANALYTICS_H
#define TK_STDLIB_ANALYTICS_H

/*
 * analytics.h — C interface for the std.analytics standard library module.
 *
 * Type mappings:
 *   Str      = const char *  (null-terminated UTF-8)
 *   f64      = double
 *   u64      = uint64_t
 *   i64      = int64_t
 *
 * malloc is permitted; callers own returned pointers.
 *
 * No external dependencies beyond libc, dataframe.h, and math.h.
 *
 * Story: 16.1.4
 */

#include <stdint.h>
#include "dataframe.h"
#include "math.h"

/* -------------------------------------------------------------------------
 * analytics.describe — per-column summary statistics
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *col;
    double      count;
    double      mean;
    double      stddev;
    double      min;
    double      p25;
    double      p50;
    double      p75;
    double      max;
} DescribeRow;

typedef struct {
    DescribeRow *rows;
    uint64_t     ncols;  /* one row per numeric column */
} DescribeResult;

/* -------------------------------------------------------------------------
 * analytics.timeseries — bucketed time-series aggregation
 * ------------------------------------------------------------------------- */

typedef struct {
    double   *values;       /* bucketed counts or averages */
    int64_t  *timestamps;   /* bucket start times (Unix ms) */
    uint64_t  nbuckets;
} TkTimeseries;

/* -------------------------------------------------------------------------
 * analytics.anomalies — z-score outlier detection
 * ------------------------------------------------------------------------- */

typedef struct {
    uint64_t *outlier_indices;  /* row indices where z-score > threshold */
    uint64_t  noutliers;
} AnomalyResult;

/* -------------------------------------------------------------------------
 * analytics.corr — Pearson correlation matrix
 * ------------------------------------------------------------------------- */

typedef struct {
    const char **col_names;
    double      *matrix;      /* ncols × ncols, row-major */
    uint64_t     ncols;
} CorrMatrix;

/* -------------------------------------------------------------------------
 * analytics.groupstats — per-group descriptive statistics
 * ------------------------------------------------------------------------- */

typedef struct {
    const char *group;      /* group value (borrowed from dataframe) */
    uint64_t    count;
    double      sum;
    double      mean;
} GroupStatRow;

typedef struct {
    GroupStatRow *rows;
    uint64_t      ngroups;
} GroupStatResult;

/* -------------------------------------------------------------------------
 * analytics.pivot — pivot table producing a new dataframe
 * ------------------------------------------------------------------------- */

/* No extra types needed; analytics_pivot returns a TkDataframe*. */

/* -------------------------------------------------------------------------
 * Function declarations
 * ------------------------------------------------------------------------- */

/* analytics_describe: compute count/mean/stddev/min/p25/p50/p75/max for
 * every DF_COL_F64 column in df.  One DescribeRow per numeric column.
 * Caller owns the returned DescribeResult (and its rows array). */
DescribeResult analytics_describe(TkDataframe *df);

/* analytics_timeseries: aggregate values from val_col into fixed-width
 * time buckets of bucket_ms milliseconds, using ts_col (DF_COL_F64,
 * Unix ms) for ordering.  If val_col is NULL, each bucket holds a count;
 * otherwise each bucket holds the mean of val_col values that fall within
 * it.  Caller owns the returned TkTimeseries (values and timestamps arrays). */
TkTimeseries   analytics_timeseries(TkDataframe *df, const char *ts_col,
                                     const char *val_col, int64_t bucket_ms);

/* analytics_anomalies: detect rows in col whose z-score exceeds
 * z_threshold in absolute value.  Returns the row indices of outliers.
 * Caller owns the returned AnomalyResult (and its outlier_indices array). */
AnomalyResult  analytics_anomalies(TkDataframe *df, const char *col,
                                    double z_threshold);

/* analytics_corr: compute the Pearson correlation matrix for all
 * DF_COL_F64 columns in df.  matrix is stored row-major (ncols×ncols).
 * Caller owns the returned CorrMatrix (col_names and matrix arrays). */
CorrMatrix     analytics_corr(TkDataframe *df);

/* analytics_groupstats: group df by the string column group_col and compute
 * count, sum, and mean of the f64 column val_col for each unique group.
 * Caller owns the returned GroupStatResult (and its rows array). */
GroupStatResult analytics_groupstats(TkDataframe *df, const char *group_col,
                                      const char *val_col);

/* analytics_pivot: build a pivot table from df.  row_col (DF_COL_STR) supplies
 * unique row keys, col_col (DF_COL_STR) supplies unique column keys, and
 * val_col (DF_COL_F64) supplies values.  Cells are filled with the sum of
 * val_col for matching (row_key, col_key) pairs.  Returns a new dataframe
 * whose first column is the row key and remaining columns are the unique
 * column keys.  Returns NULL on error.  Caller owns the result via df_free(). */
TkDataframe   *analytics_pivot(TkDataframe *df, const char *row_col,
                                const char *col_col, const char *val_col);

/* -------------------------------------------------------------------------
 * analytics.ttest — Welch's two-sample t-test (Story 31.2.1)
 * ------------------------------------------------------------------------- */

typedef struct {
    double t_stat;
    double p_value;
} TtestResult;

/* analytics_ttest: perform a Welch's two-sample t-test on g1 and g2.
 * Returns t_stat=0.0 and p_value=1.0 if either group has fewer than 2
 * elements.  p_value uses a logistic approximation of the two-tailed
 * t-distribution CDF.  Caller owns nothing (no heap allocation). */
TtestResult analytics_ttest(F64Array g1, F64Array g2);

/* -------------------------------------------------------------------------
 * analytics.histogram — equal-width histogram (Story 31.2.1)
 * ------------------------------------------------------------------------- */

typedef struct {
    const double   *bins;    /* nbins+1 bin edges: [min, min+w, ..., max] */
    const uint64_t *counts;  /* nbins bucket counts */
    uint64_t        n;       /* number of bins (== nbins) */
} HistResult;

/* analytics_histogram: bin xs into nbins equal-width buckets.
 * bins has nbins+1 edges; counts has nbins values summing to xs.len.
 * The last bin is closed on the right (inclusive of max).
 * Caller owns bins and counts (both heap-allocated). */
HistResult analytics_histogram(F64Array xs, uint64_t nbins);

/* -------------------------------------------------------------------------
 * analytics.moving_average / analytics.exponential_smoothing (31.2.1)
 * ------------------------------------------------------------------------- */

/* analytics_moving_average: trailing window moving average.
 * For index i, averages xs[max(0,i-window+1)..i].
 * Returns a heap-allocated F64Array of the same length as xs.
 * data pointer is NULL on allocation failure. */
F64Array analytics_moving_average(F64Array xs, uint64_t window);

/* analytics_exponential_smoothing: S[0]=xs[0], S[i]=alpha*xs[i]+(1-alpha)*S[i-1].
 * Returns a heap-allocated F64Array of the same length as xs.
 * data pointer is NULL on allocation failure or empty input. */
F64Array analytics_exponential_smoothing(F64Array xs, double alpha);

/* -------------------------------------------------------------------------
 * analytics.trend — linear regression vs. index (Story 31.2.1)
 * ------------------------------------------------------------------------- */

typedef struct {
    double slope;
    double intercept;
    double r2;
} TrendResult;

/* analytics_trend: fit y = slope*i + intercept where i is the array index
 * [0..n-1].  r2 = 1 - SS_res/SS_tot.  Returns zero-valued struct on
 * degenerate input (fewer than 2 points or zero variance in indices). */
TrendResult analytics_trend(F64Array ts);

/* -------------------------------------------------------------------------
 * analytics.covariance (Story 31.2.1)
 * ------------------------------------------------------------------------- */

/* analytics_covariance: population covariance of xs and ys.
 * Returns 0.0 if either array is empty or lengths differ. */
double analytics_covariance(F64Array xs, F64Array ys);

#endif /* TK_STDLIB_ANALYTICS_H */

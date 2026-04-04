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

#endif /* TK_STDLIB_ANALYTICS_H */

/*
 * analytics.c — Implementation of the std.analytics standard library module.
 *
 * Implements describe, timeseries, anomalies, and corr using the dataframe
 * and math stdlib modules.  All heavy statistical lifting is delegated to
 * math.h functions (math_mean, math_stddev, math_min, math_max,
 * math_percentile) so this file stays thin and testable.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own the returned structs and their heap members.
 *
 * No external dependencies beyond libc, dataframe.h, and math.h.
 *
 * Story: 16.1.4
 */

#include "analytics.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Count the number of DF_COL_F64 columns in df. */
static uint64_t count_f64_cols(TkDataframe *df)
{
    uint64_t n = 0;
    for (uint64_t i = 0; i < df->ncols; i++) {
        if (df->cols[i].type == DF_COL_F64) {
            n++;
        }
    }
    return n;
}

/* -------------------------------------------------------------------------
 * analytics_describe
 * ------------------------------------------------------------------------- */

DescribeResult analytics_describe(TkDataframe *df)
{
    DescribeResult result;
    result.ncols = count_f64_cols(df);
    result.rows  = NULL;

    if (result.ncols == 0) {
        return result;
    }

    result.rows = (DescribeRow *)malloc(result.ncols * sizeof(DescribeRow));
    if (!result.rows) {
        result.ncols = 0;
        return result;
    }

    uint64_t out = 0;
    for (uint64_t i = 0; i < df->ncols; i++) {
        DfCol *col = &df->cols[i];
        if (col->type != DF_COL_F64) {
            continue;
        }

        F64Array arr = { col->f64_data, col->nrows };
        DescribeRow *row = &result.rows[out++];

        row->col    = col->name;
        row->count  = (double)col->nrows;
        row->mean   = math_mean(arr);
        row->stddev = math_stddev(arr);
        row->min    = math_min(arr);
        row->p25    = math_percentile(arr, 25.0);
        row->p50    = math_percentile(arr, 50.0);
        row->p75    = math_percentile(arr, 75.0);
        row->max    = math_max(arr);
    }

    return result;
}

/* -------------------------------------------------------------------------
 * analytics_timeseries
 * ------------------------------------------------------------------------- */

/* Comparison context for qsort on indices by timestamp value. */
typedef struct {
    const double *ts_data;
} SortCtx;

/* We cannot pass context through POSIX qsort; use a file-scope pointer for
 * the sort comparator.  This is not re-entrant but is safe for single-threaded
 * stdlib use. */
static const double *g_sort_ts_data = NULL;

static int cmp_ts_idx(const void *a, const void *b)
{
    uint64_t ia = *(const uint64_t *)a;
    uint64_t ib = *(const uint64_t *)b;
    double ta = g_sort_ts_data[ia];
    double tb = g_sort_ts_data[ib];
    if (ta < tb) return -1;
    if (ta > tb) return  1;
    return 0;
}

TkTimeseries analytics_timeseries(TkDataframe *df, const char *ts_col,
                                   const char *val_col, int64_t bucket_ms)
{
    TkTimeseries result;
    result.values     = NULL;
    result.timestamps = NULL;
    result.nbuckets   = 0;

    if (!df || !ts_col || bucket_ms <= 0) {
        return result;
    }

    DfCol *tsc = df_column(df, ts_col);
    if (!tsc || tsc->type != DF_COL_F64) {
        return result;
    }

    DfCol *valc = NULL;
    if (val_col) {
        valc = df_column(df, val_col);
        if (!valc || valc->type != DF_COL_F64) {
            return result;
        }
    }

    uint64_t nrows = tsc->nrows;
    if (nrows == 0) {
        return result;
    }

    /* Build a sorted index array. */
    uint64_t *idx = (uint64_t *)malloc(nrows * sizeof(uint64_t));
    if (!idx) {
        return result;
    }
    for (uint64_t i = 0; i < nrows; i++) {
        idx[i] = i;
    }
    g_sort_ts_data = tsc->f64_data;
    qsort(idx, nrows, sizeof(uint64_t), cmp_ts_idx);
    g_sort_ts_data = NULL;

    double min_ts = tsc->f64_data[idx[0]];
    double max_ts = tsc->f64_data[idx[nrows - 1]];

    uint64_t nbuckets = (uint64_t)((max_ts - min_ts) / (double)bucket_ms) + 1;

    double  *sums   = (double  *)calloc(nbuckets, sizeof(double));
    uint64_t *cnts  = (uint64_t *)calloc(nbuckets, sizeof(uint64_t));
    if (!sums || !cnts) {
        free(idx);
        free(sums);
        free(cnts);
        return result;
    }

    for (uint64_t i = 0; i < nrows; i++) {
        uint64_t row   = idx[i];
        double   ts    = tsc->f64_data[row];
        uint64_t bkt   = (uint64_t)((ts - min_ts) / (double)bucket_ms);
        if (bkt >= nbuckets) bkt = nbuckets - 1;
        cnts[bkt]++;
        if (valc) {
            sums[bkt] += valc->f64_data[row];
        } else {
            sums[bkt] += 1.0;
        }
    }

    result.values     = (double  *)malloc(nbuckets * sizeof(double));
    result.timestamps = (int64_t *)malloc(nbuckets * sizeof(int64_t));
    if (!result.values || !result.timestamps) {
        free(idx);
        free(sums);
        free(cnts);
        free(result.values);
        free(result.timestamps);
        result.values     = NULL;
        result.timestamps = NULL;
        result.nbuckets   = 0;
        return result;
    }

    for (uint64_t b = 0; b < nbuckets; b++) {
        result.timestamps[b] = (int64_t)(min_ts + (double)b * (double)bucket_ms);
        if (valc && cnts[b] > 0) {
            result.values[b] = sums[b] / (double)cnts[b];
        } else {
            result.values[b] = sums[b];   /* count mode: sums holds counts */
        }
    }

    result.nbuckets = nbuckets;

    free(idx);
    free(sums);
    free(cnts);
    return result;
}

/* -------------------------------------------------------------------------
 * analytics_anomalies
 * ------------------------------------------------------------------------- */

AnomalyResult analytics_anomalies(TkDataframe *df, const char *col,
                                   double z_threshold)
{
    AnomalyResult result;
    result.outlier_indices = NULL;
    result.noutliers       = 0;

    if (!df || !col) {
        return result;
    }

    DfCol *c = df_column(df, col);
    if (!c || c->type != DF_COL_F64 || c->nrows == 0) {
        return result;
    }

    F64Array arr = { c->f64_data, c->nrows };
    double mean   = math_mean(arr);
    double stddev = math_stddev(arr);

    /* If stddev is zero every point has z-score 0; no outliers. */
    if (stddev == 0.0) {
        return result;
    }

    /* First pass: count outliers. */
    uint64_t count = 0;
    for (uint64_t i = 0; i < c->nrows; i++) {
        double z = fabs((c->f64_data[i] - mean) / stddev);
        if (z > z_threshold) {
            count++;
        }
    }

    if (count == 0) {
        return result;
    }

    result.outlier_indices = (uint64_t *)malloc(count * sizeof(uint64_t));
    if (!result.outlier_indices) {
        return result;
    }

    /* Second pass: fill index array. */
    uint64_t out = 0;
    for (uint64_t i = 0; i < c->nrows; i++) {
        double z = fabs((c->f64_data[i] - mean) / stddev);
        if (z > z_threshold) {
            result.outlier_indices[out++] = i;
        }
    }
    result.noutliers = count;

    return result;
}

/* -------------------------------------------------------------------------
 * analytics_corr
 * ------------------------------------------------------------------------- */

CorrMatrix analytics_corr(TkDataframe *df)
{
    CorrMatrix result;
    result.col_names = NULL;
    result.matrix    = NULL;
    result.ncols     = 0;

    if (!df) {
        return result;
    }

    uint64_t ncols = count_f64_cols(df);
    if (ncols == 0) {
        return result;
    }

    /* Gather pointers to numeric columns. */
    DfCol **f64_cols = (DfCol **)malloc(ncols * sizeof(DfCol *));
    if (!f64_cols) {
        return result;
    }
    uint64_t ci = 0;
    for (uint64_t i = 0; i < df->ncols; i++) {
        if (df->cols[i].type == DF_COL_F64) {
            f64_cols[ci++] = &df->cols[i];
        }
    }

    result.col_names = (const char **)malloc(ncols * sizeof(const char *));
    result.matrix    = (double *)malloc(ncols * ncols * sizeof(double));
    if (!result.col_names || !result.matrix) {
        free(f64_cols);
        free(result.col_names);
        free(result.matrix);
        result.col_names = NULL;
        result.matrix    = NULL;
        return result;
    }

    for (uint64_t i = 0; i < ncols; i++) {
        result.col_names[i] = f64_cols[i]->name;
    }

    uint64_t nrows = df->nrows;

    for (uint64_t i = 0; i < ncols; i++) {
        F64Array xi = { f64_cols[i]->f64_data, nrows };
        double mean_i  = math_mean(xi);
        double stddev_i = math_stddev(xi);

        for (uint64_t j = 0; j < ncols; j++) {
            if (i == j) {
                result.matrix[i * ncols + j] = 1.0;
                continue;
            }

            F64Array xj = { f64_cols[j]->f64_data, nrows };
            double mean_j   = math_mean(xj);
            double stddev_j = math_stddev(xj);

            if (stddev_i == 0.0 || stddev_j == 0.0) {
                result.matrix[i * ncols + j] = 0.0;
                continue;
            }

            /* Pearson r = cov(xi, xj) / (stddev_i * stddev_j)
             * cov = mean((xi - mean_i) * (xj - mean_j))       */
            double cov = 0.0;
            for (uint64_t k = 0; k < nrows; k++) {
                cov += (f64_cols[i]->f64_data[k] - mean_i)
                     * (f64_cols[j]->f64_data[k] - mean_j);
            }
            if (nrows > 0) {
                cov /= (double)nrows;
            }

            result.matrix[i * ncols + j] = cov / (stddev_i * stddev_j);
        }
    }

    result.ncols = ncols;
    free(f64_cols);
    return result;
}

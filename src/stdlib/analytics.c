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
#include <stdio.h>
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
 * analytics_groupstats
 * ------------------------------------------------------------------------- */

GroupStatResult analytics_groupstats(TkDataframe *df, const char *group_col,
                                      const char *val_col)
{
    GroupStatResult result;
    result.rows    = NULL;
    result.ngroups = 0;

    if (!df || !group_col || !val_col) {
        return result;
    }

    DfCol *gcol = df_column(df, group_col);
    if (!gcol || gcol->type != DF_COL_STR) {
        return result;
    }

    DfCol *vcol = df_column(df, val_col);
    if (!vcol || vcol->type != DF_COL_F64) {
        return result;
    }

    uint64_t nrows = df->nrows;
    if (nrows == 0) {
        return result;
    }

    /* Collect unique group values.  Simple O(n*g) approach — fine for stdlib
     * use where group counts are modest. */
    uint64_t cap = 16;
    uint64_t ngroups = 0;

    /* Parallel arrays: group key, running sum, running count. */
    const char **keys  = (const char **)malloc(cap * sizeof(const char *));
    double      *sums  = (double *)calloc(cap, sizeof(double));
    uint64_t    *cnts  = (uint64_t *)calloc(cap, sizeof(uint64_t));
    if (!keys || !sums || !cnts) {
        free(keys);
        free(sums);
        free(cnts);
        return result;
    }

    for (uint64_t r = 0; r < nrows; r++) {
        const char *gval = gcol->str_data[r];
        double      val  = vcol->f64_data[r];

        /* Find existing group. */
        uint64_t gi = 0;
        for (; gi < ngroups; gi++) {
            if (strcmp(keys[gi], gval) == 0) {
                break;
            }
        }

        if (gi == ngroups) {
            /* New group. */
            if (ngroups == cap) {
                cap *= 2;
                const char **nk = (const char **)realloc(keys, cap * sizeof(const char *));
                double      *ns = (double *)realloc(sums, cap * sizeof(double));
                uint64_t    *nc = (uint64_t *)realloc(cnts, cap * sizeof(uint64_t));
                if (!nk || !ns || !nc) {
                    free(nk ? nk : keys);
                    free(ns ? ns : sums);
                    free(nc ? nc : cnts);
                    return result;
                }
                keys = nk;
                sums = ns;
                cnts = nc;
                /* Zero the new portion of sums and cnts. */
                memset(sums + ngroups, 0, (cap - ngroups) * sizeof(double));
                memset(cnts + ngroups, 0, (cap - ngroups) * sizeof(uint64_t));
            }
            keys[ngroups] = gval;
            sums[ngroups] = 0.0;
            cnts[ngroups] = 0;
            ngroups++;
        }

        sums[gi] += val;
        cnts[gi] += 1;
    }

    result.rows = (GroupStatRow *)malloc(ngroups * sizeof(GroupStatRow));
    if (!result.rows) {
        free(keys);
        free(sums);
        free(cnts);
        return result;
    }

    for (uint64_t i = 0; i < ngroups; i++) {
        result.rows[i].group = keys[i];
        result.rows[i].count = cnts[i];
        result.rows[i].sum   = sums[i];
        result.rows[i].mean  = (cnts[i] > 0) ? sums[i] / (double)cnts[i] : 0.0;
    }
    result.ngroups = ngroups;

    free(keys);
    free(sums);
    free(cnts);
    return result;
}

/* -------------------------------------------------------------------------
 * analytics_pivot
 * ------------------------------------------------------------------------- */

TkDataframe *analytics_pivot(TkDataframe *df, const char *row_col,
                              const char *col_col, const char *val_col)
{
    if (!df || !row_col || !col_col || !val_col) {
        return NULL;
    }

    DfCol *rcol = df_column(df, row_col);
    if (!rcol || rcol->type != DF_COL_STR) {
        return NULL;
    }

    DfCol *ccol = df_column(df, col_col);
    if (!ccol || ccol->type != DF_COL_STR) {
        return NULL;
    }

    DfCol *vcol = df_column(df, val_col);
    if (!vcol || vcol->type != DF_COL_F64) {
        return NULL;
    }

    uint64_t nrows = df->nrows;
    if (nrows == 0) {
        return df_new();
    }

    /* Collect unique row keys and column keys. */
    uint64_t rk_cap = 16, rk_n = 0;
    uint64_t ck_cap = 16, ck_n = 0;
    const char **rk = (const char **)malloc(rk_cap * sizeof(const char *));
    const char **ck = (const char **)malloc(ck_cap * sizeof(const char *));
    if (!rk || !ck) {
        free(rk);
        free(ck);
        return NULL;
    }

    for (uint64_t i = 0; i < nrows; i++) {
        /* Add row key if new. */
        const char *rv = rcol->str_data[i];
        uint64_t found = 0;
        for (uint64_t j = 0; j < rk_n; j++) {
            if (strcmp(rk[j], rv) == 0) { found = 1; break; }
        }
        if (!found) {
            if (rk_n == rk_cap) {
                rk_cap *= 2;
                const char **tmp = (const char **)realloc(rk, rk_cap * sizeof(const char *));
                if (!tmp) { free(rk); free(ck); return NULL; }
                rk = tmp;
            }
            rk[rk_n++] = rv;
        }

        /* Add col key if new. */
        const char *cv = ccol->str_data[i];
        found = 0;
        for (uint64_t j = 0; j < ck_n; j++) {
            if (strcmp(ck[j], cv) == 0) { found = 1; break; }
        }
        if (!found) {
            if (ck_n == ck_cap) {
                ck_cap *= 2;
                const char **tmp = (const char **)realloc(ck, ck_cap * sizeof(const char *));
                if (!tmp) { free(rk); free(ck); return NULL; }
                ck = tmp;
            }
            ck[ck_n++] = cv;
        }
    }

    /* Build a rk_n x ck_n aggregation matrix (sum). */
    double *agg = (double *)calloc(rk_n * ck_n, sizeof(double));
    if (!agg) {
        free(rk);
        free(ck);
        return NULL;
    }

    for (uint64_t i = 0; i < nrows; i++) {
        const char *rv = rcol->str_data[i];
        const char *cv = ccol->str_data[i];
        double      vv = vcol->f64_data[i];

        uint64_t ri = 0, ci = 0;
        for (ri = 0; ri < rk_n; ri++) {
            if (strcmp(rk[ri], rv) == 0) break;
        }
        for (ci = 0; ci < ck_n; ci++) {
            if (strcmp(ck[ci], cv) == 0) break;
        }
        agg[ri * ck_n + ci] += vv;
    }

    /* Build the result dataframe using df_fromrows.
     * Output has (1 + ck_n) columns: the row key column + one column per
     * unique column key.  Values are stringified doubles. */
    uint64_t out_ncols = 1 + ck_n;

    /* Headers: first is row_col name, then each unique col key. */
    const char **headers = (const char **)malloc(out_ncols * sizeof(const char *));
    if (!headers) {
        free(rk); free(ck); free(agg);
        return NULL;
    }
    headers[0] = row_col;
    for (uint64_t c = 0; c < ck_n; c++) {
        headers[1 + c] = ck[c];
    }

    /* Build row data as string arrays.  Each cell is a char buffer. */
    /* We need rk_n rows, each with out_ncols strings. */
    char ***row_ptrs = (char ***)malloc(rk_n * sizeof(char **));
    if (!row_ptrs) {
        free(rk); free(ck); free(agg); free(headers);
        return NULL;
    }
    for (uint64_t r = 0; r < rk_n; r++) {
        row_ptrs[r] = (char **)malloc(out_ncols * sizeof(char *));
        if (!row_ptrs[r]) {
            for (uint64_t rr = 0; rr < r; rr++) {
                for (uint64_t cc = 0; cc < out_ncols; cc++) free(row_ptrs[rr][cc]);
                free(row_ptrs[rr]);
            }
            free(row_ptrs); free(rk); free(ck); free(agg); free(headers);
            return NULL;
        }

        /* First column: the row key string. */
        row_ptrs[r][0] = (char *)malloc(strlen(rk[r]) + 1);
        if (row_ptrs[r][0]) {
            strcpy(row_ptrs[r][0], rk[r]);
        }

        /* Remaining columns: stringified aggregate values. */
        for (uint64_t c = 0; c < ck_n; c++) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.17g", agg[r * ck_n + c]);
            row_ptrs[r][1 + c] = (char *)malloc(strlen(buf) + 1);
            if (row_ptrs[r][1 + c]) {
                strcpy(row_ptrs[r][1 + c], buf);
            }
        }
    }

    TkDataframe *pivot = df_fromrows(headers, out_ncols,
                                      (const char ***)row_ptrs, rk_n);

    /* Clean up temp strings. */
    for (uint64_t r = 0; r < rk_n; r++) {
        for (uint64_t c = 0; c < out_ncols; c++) {
            free(row_ptrs[r][c]);
        }
        free(row_ptrs[r]);
    }
    free(row_ptrs);
    free(headers);
    free(agg);
    free(rk);
    free(ck);

    return pivot;
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

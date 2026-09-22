/*
 * analytics_glue.c — i64-ABI wrappers for std.analytics (Story 136.32).
 *
 * std.analytics is one of the six modules src/stdlib_deps.c registered with no
 * glue file at all. analytics.c has implemented all six documented functions
 * since story 16.1.4, stdlib/analytics.tki exports them, and
 * docs/stdlib/analytics.md documents them as implemented with a worked example
 * each — and not one had a wrapper, so all eight examples on the page failed
 * at link.
 *
 * ABI shapes
 * ----------
 * A `$dataframe` crosses as the TkDataframe* itself (the convention
 * df_shape_impl in tk_web_glue.c uses). The three record types come back as
 * toke arrays of struct handles, a struct handle being a pointer to a block of
 * i64 slots in the field order stdlib/analytics.tki declares, with f64 fields
 * stored as raw bit patterns:
 *
 *   $statsrow  { col; count; mean; stddev; min; p25; p50; p75; max }   9 slots
 *   $groupstat { group; count; sum; mean }                             4 slots
 *   $tspoint   { ts; value; rollingmean }                              3 slots
 *
 * Errors follow 114.53/127.67: set tk_current_error and return an empty array
 * (or 0.0), because an empty result is itself meaningful and the 0 sentinel
 * cannot carry the difference on its own.
 */
#include "analytics.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_array.h"

/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

static int64_t f64_to_i64(double d) { int64_t i; memcpy(&i, &d, sizeof(i)); return i; }

static int64_t empty_arr(void) { tk_current_error = 1; return tk_arr_alloc(0, 0); }

/* analytics.describe(d) : @($statsrow)!$dferr */
int64_t tk_analytics_describe_w(int64_t df) {
    if (!df) return empty_arr();
    DescribeResult r = analytics_describe((TkDataframe *)(intptr_t)df);
    if (!r.rows) return empty_arr();
    tk_current_error = 0;

    int64_t n = (int64_t)r.ncols;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) { free(r.rows); return empty_arr(); }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) {
        int64_t *row = (int64_t *)malloc(9 * sizeof(int64_t));
        if (!row) { tk_arr_setlen(h, i); break; }
        row[0] = (int64_t)(intptr_t)r.rows[i].col;
        row[1] = (int64_t)r.rows[i].count;      /* u64 in toke, double in C */
        row[2] = f64_to_i64(r.rows[i].mean);
        row[3] = f64_to_i64(r.rows[i].stddev);
        row[4] = f64_to_i64(r.rows[i].min);
        row[5] = f64_to_i64(r.rows[i].p25);
        row[6] = f64_to_i64(r.rows[i].p50);
        row[7] = f64_to_i64(r.rows[i].p75);
        row[8] = f64_to_i64(r.rows[i].max);
        slots[i] = (int64_t)(intptr_t)row;
    }
    free(r.rows);
    return h;
}

/*
 * analytics.corr(d; cola; colb) : f64!$dferr
 *
 * analytics_corr() computes the whole Pearson matrix over every numeric
 * column; the documented toke call asks for one pair, so the cell is selected
 * here by column name. An absent or non-numeric column is $dferr, which is
 * what the page promises.
 */
int64_t tk_analytics_corr_w(int64_t df, int64_t cola, int64_t colb) {
    if (!df || !cola || !colb) { tk_current_error = 1; return f64_to_i64(0.0); }
    CorrMatrix m = analytics_corr((TkDataframe *)(intptr_t)df);
    if (!m.matrix || !m.col_names || m.ncols == 0) {
        free(m.matrix); free((void *)m.col_names);
        tk_current_error = 1;
        return f64_to_i64(0.0);
    }
    const char *a = (const char *)(intptr_t)cola;
    const char *b = (const char *)(intptr_t)colb;
    uint64_t ia = m.ncols, ib = m.ncols;
    for (uint64_t i = 0; i < m.ncols; i++) {
        if (m.col_names[i] && !strcmp(m.col_names[i], a)) ia = i;
        if (m.col_names[i] && !strcmp(m.col_names[i], b)) ib = i;
    }
    double out = 0.0;
    int missing = (ia == m.ncols || ib == m.ncols);
    if (!missing) out = m.matrix[ia * m.ncols + ib];
    free(m.matrix); free((void *)m.col_names);
    tk_current_error = missing ? 1 : 0;
    return f64_to_i64(out);
}

/* analytics.anomalies(d; col; threshold) : @(u64)!$dferr */
int64_t tk_analytics_anomalies_w(int64_t df, int64_t col, int64_t threshold) {
    if (!df || !col) return empty_arr();
    double z;
    memcpy(&z, &threshold, sizeof z);
    AnomalyResult r = analytics_anomalies((TkDataframe *)(intptr_t)df,
                                          (const char *)(intptr_t)col, z);
    if (!r.outlier_indices) return empty_arr();
    tk_current_error = 0;

    int64_t n = (int64_t)r.noutliers;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) { free(r.outlier_indices); return empty_arr(); }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) slots[i] = (int64_t)r.outlier_indices[i];
    free(r.outlier_indices);
    return h;
}

/* analytics.groupstats(d; groupcol; valuecol) : @($groupstat)!$dferr */
int64_t tk_analytics_groupstats_w(int64_t df, int64_t groupcol, int64_t valuecol) {
    if (!df || !groupcol || !valuecol) return empty_arr();
    GroupStatResult r = analytics_groupstats((TkDataframe *)(intptr_t)df,
                                             (const char *)(intptr_t)groupcol,
                                             (const char *)(intptr_t)valuecol);
    if (!r.rows) return empty_arr();
    tk_current_error = 0;

    int64_t n = (int64_t)r.ngroups;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) { free(r.rows); return empty_arr(); }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) {
        int64_t *row = (int64_t *)malloc(4 * sizeof(int64_t));
        if (!row) { tk_arr_setlen(h, i); break; }
        row[0] = (int64_t)(intptr_t)r.rows[i].group;
        row[1] = (int64_t)r.rows[i].count;
        row[2] = f64_to_i64(r.rows[i].sum);
        row[3] = f64_to_i64(r.rows[i].mean);
        slots[i] = (int64_t)(intptr_t)row;
    }
    free(r.rows);
    return h;
}

/* analytics.pivot(d; rowcol; colcol; valuecol) : $dataframe!$dferr */
int64_t tk_analytics_pivot_w(int64_t df, int64_t rowcol, int64_t colcol,
                             int64_t valuecol) {
    if (!df || !rowcol || !colcol || !valuecol) { tk_current_error = 1; return 0; }
    TkDataframe *out = analytics_pivot((TkDataframe *)(intptr_t)df,
                                       (const char *)(intptr_t)rowcol,
                                       (const char *)(intptr_t)colcol,
                                       (const char *)(intptr_t)valuecol);
    tk_current_error = out ? 0 : 1;
    return (int64_t)(intptr_t)out;
}

/*
 * analytics.timeseries(d; tscol; valuecol; window) : @($tspoint)!$dferr
 *
 * analytics_timeseries() returns the bucket timestamps and bucket means;
 * $tspoint also carries `rollingmean`, so that column is computed here with
 * analytics_moving_average() over a trailing window of TS_ROLLING_BUCKETS
 * buckets. docs/stdlib/analytics.md names that window rather than leaving
 * "rolling average" to the reader's imagination.
 */
#define TS_ROLLING_BUCKETS 3

int64_t tk_analytics_timeseries_w(int64_t df, int64_t tscol, int64_t valuecol,
                                  int64_t window) {
    if (!df || !tscol) return empty_arr();
    TkTimeseries ts = analytics_timeseries((TkDataframe *)(intptr_t)df,
                                           (const char *)(intptr_t)tscol,
                                           valuecol ? (const char *)(intptr_t)valuecol : NULL,
                                           window);
    if (!ts.values || !ts.timestamps) {
        free(ts.values); free(ts.timestamps);
        return empty_arr();
    }
    tk_current_error = 0;

    int64_t n = (int64_t)ts.nbuckets;
    F64Array src = { ts.values, ts.nbuckets };
    F64Array roll = analytics_moving_average(src, TS_ROLLING_BUCKETS);

    int64_t h = tk_arr_alloc(n, n);
    if (!h) {
        free(ts.values); free(ts.timestamps); free((void *)roll.data);
        return empty_arr();
    }
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++) {
        int64_t *pt = (int64_t *)malloc(3 * sizeof(int64_t));
        if (!pt) { tk_arr_setlen(h, i); break; }
        pt[0] = ts.timestamps[i];
        pt[1] = f64_to_i64(ts.values[i]);
        pt[2] = f64_to_i64((roll.data && (uint64_t)i < roll.len)
                               ? roll.data[i] : ts.values[i]);
        slots[i] = (int64_t)(intptr_t)pt;
    }
    free(ts.values); free(ts.timestamps); free((void *)roll.data);
    return h;
}

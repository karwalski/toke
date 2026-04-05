/*
 * test_analytics.c — Unit tests for the std.analytics C library.
 *
 * Build and run: make test-stdlib-analytics
 *
 * All tests use synthetic dataframes built via df_fromcsv so that the full
 * parsing path is exercised along with the analytics logic.
 *
 * Test inventory (16 tests):
 *   1.  describe: mean==3.0 on [1,2,3,4,5]
 *   2.  describe: min==1.0, max==5.0 on [1,2,3,4,5]
 *   3.  describe: ncols counts only f64 columns (mixed df)
 *   4.  describe: stddev≈2.0 on [2,4,4,4,5,5,7,9]
 *   5.  anomalies: detects 100 as outlier in [1,1,1,1,100] with z>2
 *   6.  anomalies: noutliers==0 when no outliers present
 *   7.  corr: coefficient≈1.0 for perfectly correlated columns
 *   8.  corr: coefficient≈-1.0 for anti-correlated columns
 *   9.  corr: coefficient near 0.0 for uncorrelated columns
 *  10.  timeseries: 4 events in 2 equal buckets → 2 buckets, each count==2
 *  11.  groupstats: 2 groups with correct count/sum/mean
 *  12.  groupstats: single group covers all rows
 *  13.  groupstats: returns empty on bad column name
 *  14.  pivot: 2×2 pivot with correct sums
 *  15.  pivot: duplicate (row,col) pairs are summed
 *  16.  pivot: returns NULL on bad column name
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "../../src/stdlib/analytics.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_NEAR(a, b, eps, msg) \
    ASSERT(fabs((double)(a) - (double)(b)) < (eps), msg)

/* -------------------------------------------------------------------------
 * Test 1 & 2: describe mean/min/max on [1,2,3,4,5]
 * ------------------------------------------------------------------------- */

static void test_describe_basic(void)
{
    const char *csv = "val\n1\n2\n3\n4\n5\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "describe_basic: df_fromcsv succeeds");
    if (dr.is_err) return;

    DescribeResult res = analytics_describe(dr.ok);

    ASSERT(res.ncols == 1, "describe_basic: ncols==1");
    ASSERT_NEAR(res.rows[0].mean,  3.0, 1e-9, "describe_basic: mean==3.0");
    ASSERT_NEAR(res.rows[0].min,   1.0, 1e-9, "describe_basic: min==1.0");
    ASSERT_NEAR(res.rows[0].max,   5.0, 1e-9, "describe_basic: max==5.0");
    ASSERT_NEAR(res.rows[0].count, 5.0, 1e-9, "describe_basic: count==5");

    free(res.rows);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 3: describe ncols counts only f64 columns in a mixed dataframe
 * ------------------------------------------------------------------------- */

static void test_describe_ncols_mixed(void)
{
    /* Two numeric columns, one string column. */
    const char *csv = "name,a,b\nalice,1,10\nbob,2,20\ncarol,3,30\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "describe_ncols: df_fromcsv succeeds");
    if (dr.is_err) return;

    DescribeResult res = analytics_describe(dr.ok);
    ASSERT(res.ncols == 2, "describe_ncols: only 2 f64 cols counted");

    free(res.rows);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 4: describe stddev≈2.0 on [2,4,4,4,5,5,7,9]
 * Population stddev of that dataset is exactly 2.0.
 * ------------------------------------------------------------------------- */

static void test_describe_stddev(void)
{
    const char *csv = "x\n2\n4\n4\n4\n5\n5\n7\n9\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "describe_stddev: df_fromcsv succeeds");
    if (dr.is_err) return;

    DescribeResult res = analytics_describe(dr.ok);
    ASSERT(res.ncols == 1, "describe_stddev: ncols==1");
    ASSERT_NEAR(res.rows[0].stddev, 2.0, 1e-9, "describe_stddev: stddev≈2.0");

    free(res.rows);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 5: anomalies detects 100 as outlier in [1,1,1,1,100]
 * ------------------------------------------------------------------------- */

static void test_anomalies_detects_outlier(void)
{
    const char *csv = "v\n1\n1\n1\n1\n100\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "anomalies_detect: df_fromcsv succeeds");
    if (dr.is_err) return;

    AnomalyResult res = analytics_anomalies(dr.ok, "v", 2.0);
    ASSERT(res.noutliers == 1, "anomalies_detect: exactly 1 outlier found");
    ASSERT(res.outlier_indices != NULL && res.outlier_indices[0] == 4,
           "anomalies_detect: outlier is row index 4 (value=100)");

    free(res.outlier_indices);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 6: anomalies returns noutliers==0 when all values are uniform
 * ------------------------------------------------------------------------- */

static void test_anomalies_no_outliers(void)
{
    const char *csv = "v\n5\n5\n5\n5\n5\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "anomalies_none: df_fromcsv succeeds");
    if (dr.is_err) return;

    AnomalyResult res = analytics_anomalies(dr.ok, "v", 2.0);
    ASSERT(res.noutliers == 0, "anomalies_none: noutliers==0");

    free(res.outlier_indices);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 7: corr coefficient≈1.0 for perfectly correlated columns
 * x = [1,2,3,4,5], y = [2,4,6,8,10]
 * ------------------------------------------------------------------------- */

static void test_corr_perfect_positive(void)
{
    const char *csv = "x,y\n1,2\n2,4\n3,6\n4,8\n5,10\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "corr_pos: df_fromcsv succeeds");
    if (dr.is_err) return;

    CorrMatrix cm = analytics_corr(dr.ok);
    ASSERT(cm.ncols == 2, "corr_pos: ncols==2");
    /* Off-diagonal element [0,1] should be ≈ 1.0 */
    ASSERT_NEAR(cm.matrix[0 * 2 + 1], 1.0, 1e-9, "corr_pos: r≈1.0");

    free(cm.col_names);
    free(cm.matrix);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 8: corr coefficient≈-1.0 for anti-correlated columns
 * x = [1,2,3,4,5], y = [10,8,6,4,2]
 * ------------------------------------------------------------------------- */

static void test_corr_perfect_negative(void)
{
    const char *csv = "x,y\n1,10\n2,8\n3,6\n4,4\n5,2\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "corr_neg: df_fromcsv succeeds");
    if (dr.is_err) return;

    CorrMatrix cm = analytics_corr(dr.ok);
    ASSERT(cm.ncols == 2, "corr_neg: ncols==2");
    ASSERT_NEAR(cm.matrix[0 * 2 + 1], -1.0, 1e-9, "corr_neg: r≈-1.0");

    free(cm.col_names);
    free(cm.matrix);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 9: corr near 0 for uncorrelated columns
 * Chosen pair: x=[1,2,3,4], y=[2,2,2,2] (constant y → r=0 by definition)
 * ------------------------------------------------------------------------- */

static void test_corr_uncorrelated(void)
{
    const char *csv = "x,y\n1,2\n2,2\n3,2\n4,2\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "corr_zero: df_fromcsv succeeds");
    if (dr.is_err) return;

    CorrMatrix cm = analytics_corr(dr.ok);
    ASSERT(cm.ncols == 2, "corr_zero: ncols==2");
    /* stddev of y is 0, so corr returns 0 by convention */
    ASSERT_NEAR(cm.matrix[0 * 2 + 1], 0.0, 1e-9, "corr_zero: r≈0.0");

    free(cm.col_names);
    free(cm.matrix);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 10: timeseries — 4 events in 2 equal buckets → nbuckets==2, each 2
 *
 * Timestamps: 0, 100, 1000, 1100 ms, bucket_ms=1000
 *   bucket 0: ts 0 and 100   → count 2
 *   bucket 1: ts 1000, 1100  → count 2
 * ------------------------------------------------------------------------- */

static void test_timeseries_two_buckets(void)
{
    const char *csv = "ts\n0\n100\n1000\n1100\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "timeseries: df_fromcsv succeeds");
    if (dr.is_err) return;

    TkTimeseries ts = analytics_timeseries(dr.ok, "ts", NULL, 1000);
    ASSERT(ts.nbuckets == 2, "timeseries: nbuckets==2");
    if (ts.nbuckets == 2) {
        ASSERT_NEAR(ts.values[0], 2.0, 1e-9, "timeseries: bucket0 count==2");
        ASSERT_NEAR(ts.values[1], 2.0, 1e-9, "timeseries: bucket1 count==2");
        ASSERT(ts.timestamps[0] == 0,    "timeseries: bucket0 ts==0");
        ASSERT(ts.timestamps[1] == 1000, "timeseries: bucket1 ts==1000");
    }

    free(ts.values);
    free(ts.timestamps);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 11: groupstats — 2 groups with correct count/sum/mean
 *
 * group,val
 * a,10
 * b,20
 * a,30
 * b,40
 *
 * group "a": count=2, sum=40, mean=20
 * group "b": count=2, sum=60, mean=30
 * ------------------------------------------------------------------------- */

static void test_groupstats_two_groups(void)
{
    const char *csv = "group,val\na,10\nb,20\na,30\nb,40\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "groupstats_two: df_fromcsv succeeds");
    if (dr.is_err) return;

    GroupStatResult gs = analytics_groupstats(dr.ok, "group", "val");
    ASSERT(gs.ngroups == 2, "groupstats_two: ngroups==2");

    if (gs.ngroups == 2) {
        /* Find group "a" and "b" (order may vary). */
        GroupStatRow *ga = NULL;
        GroupStatRow *gb = NULL;
        for (uint64_t i = 0; i < gs.ngroups; i++) {
            if (strcmp(gs.rows[i].group, "a") == 0) ga = &gs.rows[i];
            if (strcmp(gs.rows[i].group, "b") == 0) gb = &gs.rows[i];
        }
        ASSERT(ga != NULL && gb != NULL, "groupstats_two: found groups a and b");
        if (ga && gb) {
            ASSERT(ga->count == 2, "groupstats_two: a.count==2");
            ASSERT_NEAR(ga->sum,  40.0, 1e-9, "groupstats_two: a.sum==40");
            ASSERT_NEAR(ga->mean, 20.0, 1e-9, "groupstats_two: a.mean==20");
            ASSERT(gb->count == 2, "groupstats_two: b.count==2");
            ASSERT_NEAR(gb->sum,  60.0, 1e-9, "groupstats_two: b.sum==60");
            ASSERT_NEAR(gb->mean, 30.0, 1e-9, "groupstats_two: b.mean==30");
        }
    }

    free(gs.rows);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 12: groupstats — single group covers all rows
 * ------------------------------------------------------------------------- */

static void test_groupstats_single_group(void)
{
    const char *csv = "g,v\nx,1\nx,2\nx,3\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "groupstats_single: df_fromcsv succeeds");
    if (dr.is_err) return;

    GroupStatResult gs = analytics_groupstats(dr.ok, "g", "v");
    ASSERT(gs.ngroups == 1, "groupstats_single: ngroups==1");
    if (gs.ngroups == 1) {
        ASSERT(gs.rows[0].count == 3, "groupstats_single: count==3");
        ASSERT_NEAR(gs.rows[0].sum,  6.0, 1e-9, "groupstats_single: sum==6");
        ASSERT_NEAR(gs.rows[0].mean, 2.0, 1e-9, "groupstats_single: mean==2");
    }

    free(gs.rows);
    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 13: groupstats — returns empty on bad column name
 * ------------------------------------------------------------------------- */

static void test_groupstats_bad_col(void)
{
    const char *csv = "g,v\nx,1\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "groupstats_bad: df_fromcsv succeeds");
    if (dr.is_err) return;

    GroupStatResult gs = analytics_groupstats(dr.ok, "nonexistent", "v");
    ASSERT(gs.ngroups == 0, "groupstats_bad: ngroups==0 for bad group_col");

    gs = analytics_groupstats(dr.ok, "g", "nonexistent");
    ASSERT(gs.ngroups == 0, "groupstats_bad: ngroups==0 for bad val_col");

    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 14: pivot — 2×2 pivot with correct sums
 *
 * row,col,val
 * r1,c1,10
 * r1,c2,20
 * r2,c1,30
 * r2,c2,40
 *
 * Expected pivot:
 *   row | c1 | c2
 *   r1  | 10 | 20
 *   r2  | 30 | 40
 * ------------------------------------------------------------------------- */

static void test_pivot_basic(void)
{
    const char *csv = "row,col,val\nr1,c1,10\nr1,c2,20\nr2,c1,30\nr2,c2,40\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "pivot_basic: df_fromcsv succeeds");
    if (dr.is_err) return;

    TkDataframe *piv = analytics_pivot(dr.ok, "row", "col", "val");
    ASSERT(piv != NULL, "pivot_basic: result not NULL");

    if (piv) {
        ASSERT(piv->nrows == 2, "pivot_basic: nrows==2");
        /* 3 columns: row key + c1 + c2 */
        ASSERT(piv->ncols == 3, "pivot_basic: ncols==3");

        /* Find the c1 and c2 columns. */
        DfCol *c1 = df_column(piv, "c1");
        DfCol *c2 = df_column(piv, "c2");
        ASSERT(c1 != NULL && c2 != NULL, "pivot_basic: found c1 and c2 cols");

        if (c1 && c2 && c1->type == DF_COL_F64 && c2->type == DF_COL_F64) {
            /* Find which row is r1 and which is r2. */
            DfCol *rkey = df_column(piv, "row");
            ASSERT(rkey != NULL && rkey->type == DF_COL_STR,
                   "pivot_basic: row key col is str");
            if (rkey && rkey->type == DF_COL_STR) {
                for (uint64_t i = 0; i < piv->nrows; i++) {
                    if (strcmp(rkey->str_data[i], "r1") == 0) {
                        ASSERT_NEAR(c1->f64_data[i], 10.0, 1e-9,
                                    "pivot_basic: r1,c1==10");
                        ASSERT_NEAR(c2->f64_data[i], 20.0, 1e-9,
                                    "pivot_basic: r1,c2==20");
                    } else if (strcmp(rkey->str_data[i], "r2") == 0) {
                        ASSERT_NEAR(c1->f64_data[i], 30.0, 1e-9,
                                    "pivot_basic: r2,c1==30");
                        ASSERT_NEAR(c2->f64_data[i], 40.0, 1e-9,
                                    "pivot_basic: r2,c2==40");
                    }
                }
            }
        }

        df_free(piv);
    }

    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 15: pivot — duplicate (row,col) pairs are summed
 *
 * row,col,val
 * r1,c1,5
 * r1,c1,15
 *
 * Expected: r1,c1 = 20
 * ------------------------------------------------------------------------- */

static void test_pivot_sum_duplicates(void)
{
    const char *csv = "row,col,val\nr1,c1,5\nr1,c1,15\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "pivot_dup: df_fromcsv succeeds");
    if (dr.is_err) return;

    TkDataframe *piv = analytics_pivot(dr.ok, "row", "col", "val");
    ASSERT(piv != NULL, "pivot_dup: result not NULL");

    if (piv) {
        DfCol *c1 = df_column(piv, "c1");
        ASSERT(c1 != NULL && c1->type == DF_COL_F64, "pivot_dup: c1 is f64");
        if (c1 && c1->type == DF_COL_F64) {
            ASSERT_NEAR(c1->f64_data[0], 20.0, 1e-9, "pivot_dup: sum==20");
        }
        df_free(piv);
    }

    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * Test 16: pivot — returns NULL on bad column name
 * ------------------------------------------------------------------------- */

static void test_pivot_bad_col(void)
{
    const char *csv = "row,col,val\nr1,c1,10\n";
    DfResult dr = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!dr.is_err, "pivot_bad: df_fromcsv succeeds");
    if (dr.is_err) return;

    TkDataframe *piv = analytics_pivot(dr.ok, "nope", "col", "val");
    ASSERT(piv == NULL, "pivot_bad: NULL for bad row_col");

    piv = analytics_pivot(dr.ok, "row", "nope", "val");
    ASSERT(piv == NULL, "pivot_bad: NULL for bad col_col");

    piv = analytics_pivot(dr.ok, "row", "col", "nope");
    ASSERT(piv == NULL, "pivot_bad: NULL for bad val_col");

    df_free(dr.ok);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    test_describe_basic();
    test_describe_ncols_mixed();
    test_describe_stddev();
    test_anomalies_detects_outlier();
    test_anomalies_no_outliers();
    test_corr_perfect_positive();
    test_corr_perfect_negative();
    test_corr_uncorrelated();
    test_timeseries_two_buckets();
    test_groupstats_two_groups();
    test_groupstats_single_group();
    test_groupstats_bad_col();
    test_pivot_basic();
    test_pivot_sum_duplicates();
    test_pivot_bad_col();

    if (failures == 0) {
        printf("All analytics tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}

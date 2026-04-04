/*
 * test_analytics.c — Unit tests for the std.analytics C library (Story 16.1.4).
 *
 * Build and run: make test-stdlib-analytics
 *
 * All tests use synthetic dataframes built via df_fromcsv so that the full
 * parsing path is exercised along with the analytics logic.
 *
 * Test inventory (10 tests):
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
 */

#include <stdio.h>
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

    if (failures == 0) {
        printf("All analytics tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}

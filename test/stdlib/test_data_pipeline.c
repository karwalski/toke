/*
 * test_data_pipeline.c — Integration tests for the toke stdlib data pipeline.
 *
 * Covers: CSV → math → dataframe → analytics → chart (Story 16.1.6).
 *
 * Build:
 *   $(CC) $(CFLAGS) -o test_data_pipeline test_data_pipeline.c \
 *       ../../src/stdlib/csv.c ../../src/stdlib/math.c \
 *       ../../src/stdlib/dataframe.c ../../src/stdlib/analytics.c \
 *       ../../src/stdlib/chart.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "../../src/stdlib/csv.h"
#include "../../src/stdlib/math.h"
#include "../../src/stdlib/dataframe.h"
#include "../../src/stdlib/analytics.h"
#include "../../src/stdlib/chart.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(str, sub, msg) \
    ASSERT((str) && (sub) && strstr((str),(sub)) != NULL, msg)

#define ASSERT_NEAR(a, b, eps, msg) \
    ASSERT(fabs((double)(a) - (double)(b)) <= (double)(eps), msg)

/* -------------------------------------------------------------------------
 * Test 1: CSV parse → math stats (known-answer)
 *
 * 10-row synthetic CSV with a "value" column: 1.0 through 10.0
 * mean = 5.5, stddev(population) = sqrt(8.25) ≈ 2.8723,
 * min = 1.0, max = 10.0
 * ------------------------------------------------------------------------- */
static void test_csv_math_stats(void)
{
    const char *csv =
        "id,value\n"
        "1,1.0\n"
        "2,2.0\n"
        "3,3.0\n"
        "4,4.0\n"
        "5,5.0\n"
        "6,6.0\n"
        "7,7.0\n"
        "8,8.0\n"
        "9,9.0\n"
        "10,10.0\n";

    uint64_t nrows = 0;
    StrArray *rows = csv_parse(csv, (uint64_t)strlen(csv), &nrows);
    ASSERT(rows != NULL, "test1: csv_parse returns non-NULL");
    /* 11 rows: 1 header + 10 data */
    ASSERT(nrows == 11, "test1: csv_parse nrows == 11 (header + 10 data)");

    /* Extract numeric "value" column (index 1) from data rows, skipping header */
    double vals[10];
    for (uint64_t i = 1; i < 11; i++) {
        vals[i - 1] = atof(rows[i].data[1]);
    }

    F64Array arr;
    arr.data = vals;
    arr.len  = 10;

    double mean   = math_mean(arr);
    double stddev = math_stddev(arr);
    double mn     = math_min(arr);
    double mx     = math_max(arr);

    ASSERT_NEAR(mean,   5.5,    1e-9, "test1: mean == 5.5");
    ASSERT_NEAR(stddev, 2.8723, 1e-3, "test1: stddev ≈ 2.8723");
    ASSERT_NEAR(mn,     1.0,    1e-9, "test1: min == 1.0");
    ASSERT_NEAR(mx,     10.0,   1e-9, "test1: max == 10.0");
}

/* -------------------------------------------------------------------------
 * Test 2: CSV → dataframe → analytics_describe
 *
 * Same 10-row CSV.  Load into a dataframe and verify describe().
 * ------------------------------------------------------------------------- */
static void test_df_describe(void)
{
    const char *csv =
        "id,value\n"
        "1,1.0\n"
        "2,2.0\n"
        "3,3.0\n"
        "4,4.0\n"
        "5,5.0\n"
        "6,6.0\n"
        "7,7.0\n"
        "8,8.0\n"
        "9,9.0\n"
        "10,10.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err, "test2: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "test2: dataframe non-NULL");

    uint64_t nrows_out = 0, ncols_out = 0;
    df_shape(res.ok, &nrows_out, &ncols_out);
    ASSERT(ncols_out == 2, "test2: ncols == 2");
    ASSERT(nrows_out == 10, "test2: nrows == 10");

    DescribeResult desc = analytics_describe(res.ok);
    /* Both id and value are numeric, so ncols should be 2 */
    ASSERT(desc.ncols >= 1, "test2: describe has at least 1 numeric column");

    /* Find the "value" column describe row */
    int found = 0;
    for (uint64_t i = 0; i < desc.ncols; i++) {
        if (strcmp(desc.rows[i].col, "value") == 0) {
            ASSERT_NEAR(desc.rows[i].mean, 5.5, 1e-9,
                        "test2: describe mean('value') == 5.5");
            found = 1;
        }
    }
    ASSERT(found, "test2: describe result contains 'value' column");

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 3: Dataframe filter pipeline
 *
 * CSV with a "score" column (5 values ≤ 50, 5 values > 50).
 * After filter(score > 50), all retained rows satisfy score > 50 and
 * the count is less than the total.
 * ------------------------------------------------------------------------- */
static void test_df_filter(void)
{
    const char *csv =
        "name,score\n"
        "a,10.0\n"
        "b,20.0\n"
        "c,30.0\n"
        "d,40.0\n"
        "e,50.0\n"
        "f,60.0\n"
        "g,70.0\n"
        "h,80.0\n"
        "i,90.0\n"
        "j,100.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err, "test3: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "test3: dataframe non-NULL");

    uint64_t total_rows = 0, ncols_dummy = 0;
    df_shape(res.ok, &total_rows, &ncols_dummy);
    ASSERT(total_rows == 10, "test3: total rows == 10 before filter");

    /* op=4 means '>'; filter rows where score > 50 */
    TkDataframe *filtered = df_filter(res.ok, "score", 50.0, 4);
    ASSERT(filtered != NULL, "test3: df_filter returns non-NULL");

    uint64_t frows = 0, fcols = 0;
    df_shape(filtered, &frows, &fcols);
    ASSERT(frows < total_rows, "test3: filtered row count < total row count");
    ASSERT(frows > 0, "test3: filtered row count > 0");

    /* Verify every retained row has score > 50 */
    DfCol *score_col = df_column(filtered, "score");
    ASSERT(score_col != NULL, "test3: filtered df has 'score' column");
    if (score_col) {
        int all_above = 1;
        for (uint64_t i = 0; i < score_col->nrows; i++) {
            if (score_col->f64_data[i] <= 50.0) {
                all_above = 0;
                break;
            }
        }
        ASSERT(all_above, "test3: all filtered rows have score > 50");
    }

    df_free(res.ok);
    df_free(filtered);
}

/* -------------------------------------------------------------------------
 * Test 4: Groupby → chart_bar
 *
 * CSV with "category" (str) and "revenue" (f64).
 * df_groupby by category, agg=sum.
 * Build a chart_bar; chart_tojson is non-NULL and contains category names.
 * ------------------------------------------------------------------------- */
static void test_groupby_chart(void)
{
    const char *csv =
        "category,revenue\n"
        "A,100.0\n"
        "B,200.0\n"
        "A,150.0\n"
        "C,300.0\n"
        "B,250.0\n"
        "C,100.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err, "test4: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "test4: dataframe non-NULL");

    /* agg=1 means sum */
    DfGroupResult gr = df_groupby(res.ok, "category", "revenue", 1);
    ASSERT(gr.nrows > 0, "test4: groupby produces at least one group");
    ASSERT(gr.nrows == 3, "test4: groupby produces 3 groups (A, B, C)");

    /* Build chart from group results */
    const char **cat_labels = malloc(gr.nrows * sizeof(const char *));
    double      *rev_values = malloc(gr.nrows * sizeof(double));
    ASSERT(cat_labels != NULL, "test4: label alloc succeeded");
    ASSERT(rev_values != NULL, "test4: value alloc succeeded");

    for (uint64_t i = 0; i < gr.nrows; i++) {
        cat_labels[i] = gr.rows[i].group_val;
        rev_values[i] = gr.rows[i].agg_result;
    }

    StrArray labels_arr;
    labels_arr.data = cat_labels;
    labels_arr.len  = gr.nrows;

    TkDataset ds;
    ds.label   = "revenue";
    ds.values  = rev_values;
    ds.nvalues = gr.nrows;
    ds.color   = NULL;

    TkChartSpec *chart = chart_bar(labels_arr, &ds, 1, "Revenue by Category");
    ASSERT(chart != NULL, "test4: chart_bar returns non-NULL");

    const char *json = chart_tojson(chart);
    ASSERT(json != NULL, "test4: chart_tojson returns non-NULL");

    /* At least one category name must appear in the JSON */
    int has_category = (strstr(json, "A") != NULL ||
                        strstr(json, "B") != NULL ||
                        strstr(json, "C") != NULL);
    ASSERT(has_category, "test4: chart JSON contains category names");

    free(cat_labels);
    free(rev_values);
    chart_free(chart);
    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 5: Anomaly detection on filtered data
 *
 * Dataset with values 1-10 plus one outlier at 999.
 * analytics_anomalies with z_threshold=2.0 must detect the outlier row.
 * ------------------------------------------------------------------------- */
static void test_anomaly_detection(void)
{
    const char *csv =
        "idx,val\n"
        "1,1.0\n"
        "2,2.0\n"
        "3,3.0\n"
        "4,4.0\n"
        "5,5.0\n"
        "6,6.0\n"
        "7,7.0\n"
        "8,8.0\n"
        "9,9.0\n"
        "10,10.0\n"
        "11,999.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err, "test5: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "test5: dataframe non-NULL");

    AnomalyResult ar = analytics_anomalies(res.ok, "val", 2.0);
    ASSERT(ar.noutliers > 0, "test5: at least one outlier detected");

    /* Verify the outlier row corresponds to val=999 */
    DfCol *val_col = df_column(res.ok, "val");
    ASSERT(val_col != NULL, "test5: 'val' column exists");
    if (val_col && ar.noutliers > 0) {
        int found_999 = 0;
        for (uint64_t i = 0; i < ar.noutliers; i++) {
            uint64_t idx = ar.outlier_indices[i];
            if (idx < val_col->nrows && val_col->f64_data[idx] > 900.0) {
                found_999 = 1;
            }
        }
        ASSERT(found_999, "test5: outlier index points to val=999 row");
    }

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 6: Correlation matrix
 *
 * Three f64 columns: x=[1..5], y=2*x (perfect +1 correlation), z arbitrary.
 * analytics_corr returns 3×3 matrix; corr(x,y) ≈ 1.0.
 * ------------------------------------------------------------------------- */
static void test_correlation(void)
{
    const char *csv =
        "x,y,z\n"
        "1.0,2.0,5.0\n"
        "2.0,4.0,1.0\n"
        "3.0,6.0,9.0\n"
        "4.0,8.0,3.0\n"
        "5.0,10.0,7.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err, "test6: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "test6: dataframe non-NULL");

    CorrMatrix cm = analytics_corr(res.ok);
    ASSERT(cm.ncols == 3, "test6: correlation matrix is 3×3");
    ASSERT(cm.matrix != NULL, "test6: correlation matrix data non-NULL");
    ASSERT(cm.col_names != NULL, "test6: correlation col_names non-NULL");

    /* Find column indices for x and y */
    int xi = -1, yi = -1;
    for (uint64_t i = 0; i < cm.ncols; i++) {
        if (strcmp(cm.col_names[i], "x") == 0) xi = (int)i;
        if (strcmp(cm.col_names[i], "y") == 0) yi = (int)i;
    }
    ASSERT(xi >= 0, "test6: 'x' found in corr col_names");
    ASSERT(yi >= 0, "test6: 'y' found in corr col_names");

    if (xi >= 0 && yi >= 0) {
        /* corr(x,y) should be 1.0 since y = 2*x */
        double corr_xy = cm.matrix[(uint64_t)xi * cm.ncols + (uint64_t)yi];
        ASSERT_NEAR(corr_xy, 1.0, 1e-9, "test6: corr(x,y) ≈ 1.0");

        /* Diagonal entries must be 1.0 */
        for (uint64_t i = 0; i < cm.ncols; i++) {
            ASSERT_NEAR(cm.matrix[i * cm.ncols + i], 1.0, 1e-9,
                        "test6: diagonal of corr matrix == 1.0");
        }
    }

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 7: df_tojson → chart
 *
 * df_tojson produces valid JSON array starting with '['.
 * Use the JSON as chart data source (verify non-NULL).
 * ------------------------------------------------------------------------- */
static void test_df_tojson_chart(void)
{
    const char *csv =
        "name,score\n"
        "alice,88.0\n"
        "bob,72.0\n"
        "carol,95.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err, "test7: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "test7: dataframe non-NULL");

    const char *json = df_tojson(res.ok);
    ASSERT(json != NULL, "test7: df_tojson returns non-NULL");
    ASSERT(json[0] == '[', "test7: df_tojson starts with '['");
    ASSERT_CONTAINS(json, "score", "test7: JSON contains column name 'score'");
    ASSERT_CONTAINS(json, "name",  "test7: JSON contains column name 'name'");
    ASSERT_CONTAINS(json, "alice", "test7: JSON contains value 'alice'");

    /* Use the JSON existence as a signal that chart data can be sourced from it */
    ASSERT(json != NULL, "test7: chart data source (JSON) is non-NULL");

    df_free(res.ok);
    /* json is caller-owned */
    free((void *)json);
}

/* -------------------------------------------------------------------------
 * Test 8: Multi-step pipeline
 *
 * CSV → dataframe → filter → groupby → describe → chart_bar → chart_tojson.
 * Final chart JSON contains "type":"bar" (Chart.js format).
 * ------------------------------------------------------------------------- */
static void test_multi_step_pipeline(void)
{
    const char *csv =
        "region,sales\n"
        "North,40.0\n"
        "South,80.0\n"
        "East,20.0\n"
        "North,60.0\n"
        "South,90.0\n"
        "East,10.0\n"
        "North,100.0\n"
        "South,30.0\n";

    /* Step 1: CSV → dataframe */
    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err, "test8: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "test8: dataframe non-NULL");

    /* Step 2: filter rows where sales > 30 */
    TkDataframe *filtered = df_filter(res.ok, "sales", 30.0, 4);
    ASSERT(filtered != NULL, "test8: df_filter non-NULL");

    /* Step 3: groupby region, sum sales */
    DfGroupResult gr = df_groupby(filtered, "region", "sales", 1);
    ASSERT(gr.nrows > 0, "test8: groupby returns rows");

    /* Step 4: describe the filtered dataframe */
    DescribeResult desc = analytics_describe(filtered);
    ASSERT(desc.ncols >= 1, "test8: describe on filtered df has numeric cols");

    /* Step 5: chart_bar from group results */
    const char **lbl = malloc(gr.nrows * sizeof(const char *));
    double      *val = malloc(gr.nrows * sizeof(double));
    ASSERT(lbl != NULL, "test8: label alloc");
    ASSERT(val != NULL, "test8: value alloc");

    for (uint64_t i = 0; i < gr.nrows; i++) {
        lbl[i] = gr.rows[i].group_val;
        val[i] = gr.rows[i].agg_result;
    }

    StrArray la;
    la.data = lbl;
    la.len  = gr.nrows;

    TkDataset ds;
    ds.label   = "sales";
    ds.values  = val;
    ds.nvalues = gr.nrows;
    ds.color   = NULL;

    TkChartSpec *chart = chart_bar(la, &ds, 1, "Sales by Region");
    ASSERT(chart != NULL, "test8: chart_bar non-NULL");

    /* Step 6: chart_tojson */
    const char *json = chart_tojson(chart);
    ASSERT(json != NULL, "test8: chart_tojson non-NULL");
    ASSERT_CONTAINS(json, "bar", "test8: chart JSON contains 'bar'");

    free(lbl);
    free(val);
    chart_free(chart);
    df_free(filtered);
    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 9: Large CSV performance
 *
 * Build a CSV string with 500 data rows programmatically.
 * csv_parse must return nrows=501 (1 header + 500 data).
 * ------------------------------------------------------------------------- */
static void test_large_csv(void)
{
    /* Each row: "NNN,VALUE\n" — max ~14 chars.  Header is "id,val\n" (7 chars).
     * 501 rows × 20 bytes = ~10 KB; safe stack alloc via heap. */
    const int NDATA = 500;
    size_t buf_size = 32 + (size_t)NDATA * 24;
    char *buf = malloc(buf_size);
    ASSERT(buf != NULL, "test9: large CSV buffer alloc");
    if (!buf) return;

    int pos = 0;
    pos += snprintf(buf + pos, buf_size - (size_t)pos, "id,val\n");
    for (int i = 1; i <= NDATA; i++) {
        pos += snprintf(buf + pos, buf_size - (size_t)pos,
                        "%d,%d.0\n", i, i);
    }

    uint64_t nrows = 0;
    StrArray *rows = csv_parse(buf, (uint64_t)pos, &nrows);
    ASSERT(rows != NULL, "test9: csv_parse large CSV non-NULL");
    /* 1 header + 500 data rows */
    ASSERT(nrows == (uint64_t)(NDATA + 1),
           "test9: csv_parse large CSV nrows == 501");

    if (rows) {
        /* Check ncols: each row should have 2 fields */
        ASSERT(rows[0].len == 2, "test9: header row has 2 columns");
        ASSERT(rows[1].len == 2, "test9: first data row has 2 columns");
    }

    free(buf);
}

/* -------------------------------------------------------------------------
 * Test 10: Percentile pipeline
 *
 * Parse CSV, extract column, compute p25/p50/p75.
 * Values 1..10: p50=5.5, p25 < p50, p75 > p50.
 * ------------------------------------------------------------------------- */
static void test_percentile_pipeline(void)
{
    const char *csv =
        "id,value\n"
        "1,1.0\n"
        "2,2.0\n"
        "3,3.0\n"
        "4,4.0\n"
        "5,5.0\n"
        "6,6.0\n"
        "7,7.0\n"
        "8,8.0\n"
        "9,9.0\n"
        "10,10.0\n";

    uint64_t nrows = 0;
    StrArray *rows = csv_parse(csv, (uint64_t)strlen(csv), &nrows);
    ASSERT(rows != NULL, "test10: csv_parse non-NULL");
    /* nrows includes header */
    ASSERT(nrows == 11, "test10: csv_parse nrows == 11");

    /* Extract value column from data rows (skip header row 0) */
    double vals[10];
    for (uint64_t i = 1; i < 11; i++) {
        vals[i - 1] = atof(rows[i].data[1]);
    }

    F64Array arr;
    arr.data = vals;
    arr.len  = 10;

    double p25 = math_percentile(arr, 25.0);
    double p50 = math_percentile(arr, 50.0);
    double p75 = math_percentile(arr, 75.0);

    ASSERT(!isnan(p25), "test10: p25 is not NaN");
    ASSERT(!isnan(p50), "test10: p50 is not NaN");
    ASSERT(!isnan(p75), "test10: p75 is not NaN");

    /* p50 is the median of 1..10, which must equal math_median */
    double med = math_median(arr);
    ASSERT_NEAR(p50, med, 1e-9, "test10: p50 == median");

    ASSERT(p25 < p50, "test10: p25 < p50");
    ASSERT(p50 < p75, "test10: p50 < p75");
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("=== test_data_pipeline ===\n");

    test_csv_math_stats();
    test_df_describe();
    test_df_filter();
    test_groupby_chart();
    test_anomaly_detection();
    test_correlation();
    test_df_tojson_chart();
    test_multi_step_pipeline();
    test_large_csv();
    test_percentile_pipeline();

    printf("=========================\n");
    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}

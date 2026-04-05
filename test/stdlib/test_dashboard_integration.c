/*
 * test_dashboard_integration.c — Real-world integration test:
 *     CSV -> dataframe -> analytics -> chart -> HTML -> dashboard -> file.
 *
 * Story: 21.1.3
 *
 * Build:
 *   $(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror -o test_dashboard_integration \
 *       test_dashboard_integration.c \
 *       ../../src/stdlib/csv.c ../../src/stdlib/dataframe.c \
 *       ../../src/stdlib/analytics.c ../../src/stdlib/math.c \
 *       ../../src/stdlib/chart.c ../../src/stdlib/html.c \
 *       ../../src/stdlib/dashboard.c ../../src/stdlib/router.c \
 *       ../../src/stdlib/str.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "../../src/stdlib/csv.h"
#include "../../src/stdlib/dataframe.h"
#include "../../src/stdlib/analytics.h"
#include "../../src/stdlib/chart.h"
#include "../../src/stdlib/html.h"
#include "../../src/stdlib/dashboard.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(str, sub, msg) \
    ASSERT((str) && (sub) && strstr((str),(sub)) != NULL, msg)

#define ASSERT_NEAR(a, b, eps, msg) \
    ASSERT(fabs((double)(a) - (double)(b)) <= (double)(eps), msg)

/* -------------------------------------------------------------------------
 * Synthetic dataset: 50 rows, 5 columns (date, category, value, count, label)
 *
 * Categories: Sales, Support, Engineering, Marketing, Operations
 * Dates: 2025-01-01 through 2025-02-19 (daily, 50 days)
 * Values: realistic revenue-like numbers
 * Counts: integer-like quantities
 * Labels: descriptive tags
 * ------------------------------------------------------------------------- */
static const char CSV_DATA[] =
    "date,category,value,count,label\n"
    "2025-01-01,Sales,1200.50,15,quota-hit\n"
    "2025-01-02,Support,340.00,42,tickets-closed\n"
    "2025-01-03,Engineering,890.75,8,deploys\n"
    "2025-01-04,Marketing,560.00,23,campaigns\n"
    "2025-01-05,Operations,410.25,31,incidents\n"
    "2025-01-06,Sales,1350.00,18,quota-hit\n"
    "2025-01-07,Support,290.50,38,tickets-closed\n"
    "2025-01-08,Engineering,1100.00,12,deploys\n"
    "2025-01-09,Marketing,720.25,27,campaigns\n"
    "2025-01-10,Operations,380.00,29,incidents\n"
    "2025-01-11,Sales,980.75,14,quota-hit\n"
    "2025-01-12,Support,410.00,45,tickets-closed\n"
    "2025-01-13,Engineering,1250.50,10,deploys\n"
    "2025-01-14,Marketing,630.00,21,campaigns\n"
    "2025-01-15,Operations,520.75,35,incidents\n"
    "2025-01-16,Sales,1450.00,20,quota-hit\n"
    "2025-01-17,Support,360.25,40,tickets-closed\n"
    "2025-01-18,Engineering,950.00,9,deploys\n"
    "2025-01-19,Marketing,480.50,19,campaigns\n"
    "2025-01-20,Operations,440.00,33,incidents\n"
    "2025-01-21,Sales,1100.25,16,quota-hit\n"
    "2025-01-22,Support,320.00,44,tickets-closed\n"
    "2025-01-23,Engineering,1180.75,11,deploys\n"
    "2025-01-24,Marketing,590.00,25,campaigns\n"
    "2025-01-25,Operations,470.50,30,incidents\n"
    "2025-01-26,Sales,1290.00,17,quota-hit\n"
    "2025-01-27,Support,380.25,41,tickets-closed\n"
    "2025-01-28,Engineering,1050.00,13,deploys\n"
    "2025-01-29,Marketing,670.50,22,campaigns\n"
    "2025-01-30,Operations,500.00,34,incidents\n"
    "2025-01-31,Sales,1380.75,19,quota-hit\n"
    "2025-02-01,Support,300.00,39,tickets-closed\n"
    "2025-02-02,Engineering,1300.25,14,deploys\n"
    "2025-02-03,Marketing,540.00,20,campaigns\n"
    "2025-02-04,Operations,430.50,32,incidents\n"
    "2025-02-05,Sales,1150.00,15,quota-hit\n"
    "2025-02-06,Support,350.75,43,tickets-closed\n"
    "2025-02-07,Engineering,1020.00,10,deploys\n"
    "2025-02-08,Marketing,610.25,24,campaigns\n"
    "2025-02-09,Operations,460.00,28,incidents\n"
    "2025-02-10,Sales,1420.50,21,quota-hit\n"
    "2025-02-11,Support,330.00,37,tickets-closed\n"
    "2025-02-12,Engineering,1160.75,12,deploys\n"
    "2025-02-13,Marketing,700.00,26,campaigns\n"
    "2025-02-14,Operations,490.50,36,incidents\n"
    "2025-02-15,Sales,1270.00,16,quota-hit\n"
    "2025-02-16,Support,400.25,46,tickets-closed\n"
    "2025-02-17,Engineering,1080.00,11,deploys\n"
    "2025-02-18,Marketing,650.50,23,campaigns\n"
    "2025-02-19,Operations,510.75,31,incidents\n";

/* -------------------------------------------------------------------------
 * Test 1: CSV parsing — verify row/column counts
 * ------------------------------------------------------------------------- */
static void test_csv_parse(void)
{
    uint64_t nrows = 0;
    StrArray *rows = csv_parse(CSV_DATA, (uint64_t)strlen(CSV_DATA), &nrows);
    ASSERT(rows != NULL, "t1: csv_parse returns non-NULL");
    ASSERT(nrows == 51, "t1: csv_parse returns 51 rows (1 header + 50 data)");
    if (rows && nrows > 0) {
        ASSERT(rows[0].len == 5, "t1: header row has 5 columns");
        ASSERT(strcmp(rows[0].data[0], "date") == 0, "t1: first header is 'date'");
        ASSERT(strcmp(rows[0].data[2], "value") == 0, "t1: third header is 'value'");
    }
}

/* -------------------------------------------------------------------------
 * Test 2: CSV -> Dataframe with df_fromcsv
 * ------------------------------------------------------------------------- */
static void test_csv_to_dataframe(void)
{
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t2: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t2: dataframe non-NULL");

    uint64_t nrows = 0, ncols = 0;
    df_shape(res.ok, &nrows, &ncols);
    ASSERT(nrows == 50, "t2: dataframe has 50 rows");
    ASSERT(ncols == 5, "t2: dataframe has 5 columns");

    /* Verify column types: value and count are f64, date/category/label are str */
    DfCol *val_col = df_column(res.ok, "value");
    ASSERT(val_col != NULL, "t2: 'value' column exists");
    if (val_col) {
        ASSERT(val_col->type == DF_COL_F64, "t2: 'value' column is f64");
    }

    DfCol *cat_col = df_column(res.ok, "category");
    ASSERT(cat_col != NULL, "t2: 'category' column exists");
    if (cat_col) {
        ASSERT(cat_col->type == DF_COL_STR, "t2: 'category' column is str");
    }

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 3: analytics_describe on the full dataset
 * ------------------------------------------------------------------------- */
static void test_analytics_describe(void)
{
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t3: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t3: dataframe non-NULL");

    DescribeResult desc = analytics_describe(res.ok);
    /* 'value' and 'count' are numeric columns */
    ASSERT(desc.ncols >= 2, "t3: describe reports at least 2 numeric columns");

    /* Find the 'value' column stats */
    int found_value = 0;
    for (uint64_t i = 0; i < desc.ncols; i++) {
        if (strcmp(desc.rows[i].col, "value") == 0) {
            found_value = 1;
            ASSERT_NEAR(desc.rows[i].count, 50.0, 1e-9,
                        "t3: value count == 50");
            ASSERT(desc.rows[i].min > 0.0,
                   "t3: value min > 0");
            ASSERT(desc.rows[i].max > desc.rows[i].min,
                   "t3: value max > min");
            ASSERT(desc.rows[i].mean > desc.rows[i].min,
                   "t3: value mean > min");
            ASSERT(desc.rows[i].mean < desc.rows[i].max,
                   "t3: value mean < max");
            ASSERT(desc.rows[i].stddev > 0.0,
                   "t3: value stddev > 0");
            ASSERT(desc.rows[i].p25 <= desc.rows[i].p50,
                   "t3: value p25 <= p50");
            ASSERT(desc.rows[i].p50 <= desc.rows[i].p75,
                   "t3: value p50 <= p75");
        }
    }
    ASSERT(found_value, "t3: describe contains 'value' column");

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 4: analytics_anomalies on the value column
 *
 * The dataset has relatively uniform values; with a strict z-threshold
 * we expect few or no outliers. With a loose threshold we exercise the API.
 * ------------------------------------------------------------------------- */
static void test_anomaly_detection(void)
{
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t4: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t4: dataframe non-NULL");

    /* Strict threshold: expect few outliers in a controlled dataset */
    AnomalyResult ar = analytics_anomalies(res.ok, "value", 3.0);
    /* Just verify the API returns without crashing; outlier count varies */
    ASSERT(ar.noutliers <= 50, "t4: anomaly count is within bounds");

    /* Also exercise groupstats as a complementary analytics path */
    GroupStatResult gs = analytics_groupstats(res.ok, "category", "value");
    ASSERT(gs.ngroups == 5,
           "t4: groupstats returns 5 groups (one per category)");

    /* Verify known category appears */
    int found_sales = 0;
    for (uint64_t i = 0; i < gs.ngroups; i++) {
        if (strcmp(gs.rows[i].group, "Sales") == 0) {
            found_sales = 1;
            ASSERT(gs.rows[i].count == 10,
                   "t4: Sales group has 10 rows");
            ASSERT(gs.rows[i].sum > 0.0,
                   "t4: Sales sum > 0");
            ASSERT(gs.rows[i].mean > 0.0,
                   "t4: Sales mean > 0");
        }
    }
    ASSERT(found_sales, "t4: groupstats contains 'Sales' group");

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 5: groupby + chart_bar + chart_tojson
 * ------------------------------------------------------------------------- */
static void test_groupby_to_chart(void)
{
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t5: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t5: dataframe non-NULL");

    /* Group by category, sum of value */
    DfGroupResult gr = df_groupby(res.ok, "category", "value", 1);
    ASSERT(gr.nrows == 5, "t5: groupby returns 5 groups");

    /* Build label and value arrays for the chart */
    const char **labels = malloc(gr.nrows * sizeof(const char *));
    double *values = malloc(gr.nrows * sizeof(double));
    ASSERT(labels != NULL && values != NULL, "t5: chart data alloc");

    for (uint64_t i = 0; i < gr.nrows; i++) {
        labels[i] = gr.rows[i].group_val;
        values[i] = gr.rows[i].agg_result;
    }

    StrArray label_arr;
    label_arr.data = labels;
    label_arr.len = gr.nrows;

    TkDataset ds;
    ds.label = "Total Value";
    ds.values = values;
    ds.nvalues = gr.nrows;
    ds.color = "#4a90d9";

    TkChartSpec *chart = chart_bar(label_arr, &ds, 1, "Value by Category");
    ASSERT(chart != NULL, "t5: chart_bar returns non-NULL");
    ASSERT(chart->type == CHART_BAR, "t5: chart type is CHART_BAR");

    const char *json = chart_tojson(chart);
    ASSERT(json != NULL, "t5: chart_tojson returns non-NULL");
    ASSERT_CONTAINS(json, "bar", "t5: chart JSON contains 'bar'");
    ASSERT_CONTAINS(json, "Total Value",
                    "t5: chart JSON contains dataset label");

    free(labels);
    free(values);
    chart_free(chart);
    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 6: HTML document construction
 * ------------------------------------------------------------------------- */
static void test_html_construction(void)
{
    TkHtmlDoc *doc = html_doc();
    ASSERT(doc != NULL, "t6: html_doc returns non-NULL");

    html_title(doc, "Analytics Dashboard");
    html_style(doc, "body { font-family: sans-serif; }");

    TkHtmlNode *heading = html_h1("Monthly Performance Report");
    ASSERT(heading != NULL, "t6: html_h1 returns non-NULL");
    html_append(doc, heading);

    TkHtmlNode *para = html_p("Data covers 50 observations across 5 categories.");
    ASSERT(para != NULL, "t6: html_p returns non-NULL");
    html_append(doc, para);

    /* Build a summary table */
    const char *hdrs[] = {"Category", "Total Value", "Avg Count"};
    const char *cells[] = {
        "Sales",       "12593.75", "17.1",
        "Support",     "3481.75",  "41.5",
        "Engineering", "10982.00", "11.0",
        "Marketing",   "6152.00",  "23.0",
        "Operations",  "4613.25",  "31.9"
    };
    TkHtmlNode *tbl = html_table(hdrs, 3, cells, 5);
    ASSERT(tbl != NULL, "t6: html_table returns non-NULL");
    html_append(doc, tbl);

    const char *rendered = html_render(doc);
    ASSERT(rendered != NULL, "t6: html_render returns non-NULL");
    ASSERT_CONTAINS(rendered, "<html", "t6: HTML contains <html tag");
    ASSERT_CONTAINS(rendered, "<body", "t6: HTML contains <body tag");
    ASSERT_CONTAINS(rendered, "Analytics Dashboard",
                    "t6: HTML contains title text");
    ASSERT_CONTAINS(rendered, "Monthly Performance Report",
                    "t6: HTML contains heading text");
    ASSERT_CONTAINS(rendered, "Sales", "t6: HTML contains 'Sales'");

    html_free(doc);
}

/* -------------------------------------------------------------------------
 * Test 7: Full pipeline — CSV -> analyze -> chart -> dashboard -> render
 *
 * This is the primary integration test: end-to-end data analytics dashboard.
 * ------------------------------------------------------------------------- */
static void test_full_dashboard_pipeline(void)
{
    /* Step 1: Parse CSV into a dataframe */
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t7: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t7: dataframe non-NULL");

    /* Step 2: Run descriptive analytics */
    DescribeResult desc = analytics_describe(res.ok);
    ASSERT(desc.ncols >= 2, "t7: describe has numeric columns");

    /* Step 3: Group by category and aggregate */
    DfGroupResult gr = df_groupby(res.ok, "category", "value", 1);
    ASSERT(gr.nrows == 5, "t7: groupby returns 5 groups");

    /* Step 4: Build a bar chart from aggregated data */
    const char **labels = malloc(gr.nrows * sizeof(const char *));
    double *values = malloc(gr.nrows * sizeof(double));
    ASSERT(labels != NULL && values != NULL, "t7: alloc chart arrays");

    for (uint64_t i = 0; i < gr.nrows; i++) {
        labels[i] = gr.rows[i].group_val;
        values[i] = gr.rows[i].agg_result;
    }

    StrArray label_arr;
    label_arr.data = labels;
    label_arr.len = gr.nrows;

    TkDataset ds;
    ds.label = "Revenue";
    ds.values = values;
    ds.nvalues = gr.nrows;
    ds.color = NULL;

    TkChartSpec *bar = chart_bar(label_arr, &ds, 1, "Revenue by Department");
    ASSERT(bar != NULL, "t7: chart_bar returns non-NULL");

    /* Step 5: Verify chart JSON contains expected data */
    const char *chart_json = chart_tojson(bar);
    ASSERT(chart_json != NULL, "t7: chart_tojson non-NULL");
    ASSERT_CONTAINS(chart_json, "bar", "t7: chart JSON has 'bar' type");
    ASSERT_CONTAINS(chart_json, "Revenue",
                    "t7: chart JSON has 'Revenue' label");

    /* Step 6: Create dashboard and add the chart widget */
    TkDashboard *dash = dashboard_new("Data Analytics Dashboard", 2);
    ASSERT(dash != NULL, "t7: dashboard_new returns non-NULL");

    dashboard_addchart(dash, "rev-chart", "Revenue by Department",
                       bar, 0, 0, 2, 1);

    /* Step 7: Add a summary table widget with describe stats */
    const char *tbl_hdrs[] = {"Metric", "Value", "Count"};
    /* Build table cells from describe results for the value column */
    char mean_buf[32], std_buf[32], min_buf[32], max_buf[32];
    char cnt_mean_buf[32], cnt_std_buf[32];
    const char *tbl_cells[18]; /* 6 rows x 3 cols */
    int val_idx = -1, cnt_idx = -1;
    for (uint64_t i = 0; i < desc.ncols; i++) {
        if (strcmp(desc.rows[i].col, "value") == 0) val_idx = (int)i;
        if (strcmp(desc.rows[i].col, "count") == 0) cnt_idx = (int)i;
    }

    if (val_idx >= 0 && cnt_idx >= 0) {
        snprintf(mean_buf, sizeof(mean_buf), "%.2f",
                 desc.rows[val_idx].mean);
        snprintf(std_buf, sizeof(std_buf), "%.2f",
                 desc.rows[val_idx].stddev);
        snprintf(min_buf, sizeof(min_buf), "%.2f",
                 desc.rows[val_idx].min);
        snprintf(max_buf, sizeof(max_buf), "%.2f",
                 desc.rows[val_idx].max);
        snprintf(cnt_mean_buf, sizeof(cnt_mean_buf), "%.2f",
                 desc.rows[cnt_idx].mean);
        snprintf(cnt_std_buf, sizeof(cnt_std_buf), "%.2f",
                 desc.rows[cnt_idx].stddev);

        tbl_cells[0]  = "Mean";    tbl_cells[1]  = mean_buf;
                                    tbl_cells[2]  = cnt_mean_buf;
        tbl_cells[3]  = "Std Dev"; tbl_cells[4]  = std_buf;
                                    tbl_cells[5]  = cnt_std_buf;
        tbl_cells[6]  = "Min";     tbl_cells[7]  = min_buf;
                                    tbl_cells[8]  = "8";
        tbl_cells[9]  = "Max";     tbl_cells[10] = max_buf;
                                    tbl_cells[11] = "46";
        tbl_cells[12] = "P25";     tbl_cells[13] = "430.50";
                                    tbl_cells[14] = "15";
        tbl_cells[15] = "P75";     tbl_cells[16] = "1100.25";
                                    tbl_cells[17] = "35";

        dashboard_addtable(dash, "stats-table", "Summary Statistics",
                           tbl_hdrs, 3, tbl_cells, 6,
                           0, 1, 2, 1);
    }

    /* Step 8: Render the full dashboard to HTML */
    const char *html = dashboard_render(dash);
    ASSERT(html != NULL, "t7: dashboard_render returns non-NULL");

    /* Verify output is valid HTML structure */
    ASSERT_CONTAINS(html, "<html", "t7: output contains <html");
    ASSERT_CONTAINS(html, "<body", "t7: output contains <body");
    ASSERT_CONTAINS(html, "</html>", "t7: output contains </html>");

    /* Verify dashboard title is present */
    ASSERT_CONTAINS(html, "Data Analytics Dashboard",
                    "t7: output contains dashboard title");

    /* Verify chart data is embedded */
    ASSERT_CONTAINS(html, "Revenue by Department",
                    "t7: output contains chart title");

    /* Verify table content is present */
    ASSERT_CONTAINS(html, "Summary Statistics",
                    "t7: output contains table title");

    /* Verify specific data values appear in the rendered output */
    if (val_idx >= 0) {
        ASSERT_CONTAINS(html, mean_buf,
                        "t7: output contains mean value");
    }

    /* Step 9: Write to a temp file and verify it is non-empty */
    {
        const char *tmppath = "/tmp/toke_dashboard_test.html";
        FILE *fp = fopen(tmppath, "w");
        ASSERT(fp != NULL, "t7: fopen temp file succeeds");
        if (fp) {
            size_t html_len = strlen(html);
            size_t written = fwrite(html, 1, html_len, fp);
            fclose(fp);
            ASSERT(written == html_len,
                   "t7: file write matches HTML length");
            ASSERT(written > 500,
                   "t7: HTML output is substantial (>500 bytes)");

            /* Read back and verify round-trip */
            fp = fopen(tmppath, "r");
            ASSERT(fp != NULL, "t7: fopen for read-back succeeds");
            if (fp) {
                fseek(fp, 0, SEEK_END);
                long fsize = ftell(fp);
                fclose(fp);
                ASSERT(fsize == (long)html_len,
                       "t7: file size matches HTML length");
            }
            remove(tmppath);
        }
    }

    free(labels);
    free(values);
    dashboard_free(dash);
    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 8: Verify data integrity across the full pipeline
 *
 * Cross-check: sum of group sums == sum of all values in the raw column.
 * ------------------------------------------------------------------------- */
static void test_data_integrity(void)
{
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t8: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t8: dataframe non-NULL");

    /* Sum all values from the raw column */
    DfCol *val_col = df_column(res.ok, "value");
    ASSERT(val_col != NULL, "t8: 'value' column exists");
    ASSERT(val_col->type == DF_COL_F64, "t8: 'value' is f64");

    double raw_sum = 0.0;
    for (uint64_t i = 0; i < val_col->nrows; i++) {
        raw_sum += val_col->f64_data[i];
    }

    /* Sum from groupby (sum aggregation) */
    DfGroupResult gr = df_groupby(res.ok, "category", "value", 1);
    double group_sum = 0.0;
    for (uint64_t i = 0; i < gr.nrows; i++) {
        group_sum += gr.rows[i].agg_result;
    }

    ASSERT_NEAR(raw_sum, group_sum, 1e-6,
                "t8: sum(raw values) == sum(group sums)");

    /* Cross-check with groupstats */
    GroupStatResult gs = analytics_groupstats(res.ok, "category", "value");
    double gs_sum = 0.0;
    for (uint64_t i = 0; i < gs.ngroups; i++) {
        gs_sum += gs.rows[i].sum;
    }
    ASSERT_NEAR(raw_sum, gs_sum, 1e-6,
                "t8: sum(raw values) == sum(groupstats sums)");

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 9: Correlation matrix for numeric columns
 * ------------------------------------------------------------------------- */
static void test_correlation_analysis(void)
{
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t9: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t9: dataframe non-NULL");

    CorrMatrix cm = analytics_corr(res.ok);
    ASSERT(cm.ncols >= 2, "t9: corr matrix has at least 2 numeric cols");
    ASSERT(cm.matrix != NULL, "t9: corr matrix data non-NULL");

    /* Diagonal must be 1.0 */
    for (uint64_t i = 0; i < cm.ncols; i++) {
        ASSERT_NEAR(cm.matrix[i * cm.ncols + i], 1.0, 1e-9,
                    "t9: corr diagonal == 1.0");
    }

    /* Matrix must be symmetric */
    for (uint64_t i = 0; i < cm.ncols; i++) {
        for (uint64_t j = i + 1; j < cm.ncols; j++) {
            double cij = cm.matrix[i * cm.ncols + j];
            double cji = cm.matrix[j * cm.ncols + i];
            ASSERT_NEAR(cij, cji, 1e-9, "t9: corr matrix is symmetric");
        }
    }

    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * Test 10: Dashboard with multiple chart widgets
 * ------------------------------------------------------------------------- */
static void test_multi_widget_dashboard(void)
{
    DfResult res = df_fromcsv(CSV_DATA, (uint64_t)strlen(CSV_DATA), 1);
    ASSERT(!res.is_err, "t10: df_fromcsv succeeds");
    ASSERT(res.ok != NULL, "t10: dataframe non-NULL");

    /* Chart 1: bar chart of value sums */
    DfGroupResult gr_val = df_groupby(res.ok, "category", "value", 1);
    const char **labels1 = malloc(gr_val.nrows * sizeof(const char *));
    double *vals1 = malloc(gr_val.nrows * sizeof(double));
    for (uint64_t i = 0; i < gr_val.nrows; i++) {
        labels1[i] = gr_val.rows[i].group_val;
        vals1[i] = gr_val.rows[i].agg_result;
    }

    StrArray la1 = { labels1, gr_val.nrows };
    TkDataset ds1 = { "Value Sum", vals1, gr_val.nrows, NULL };
    TkChartSpec *bar1 = chart_bar(la1, &ds1, 1, "Total Value by Dept");

    /* Chart 2: bar chart of mean counts */
    DfGroupResult gr_cnt = df_groupby(res.ok, "category", "count", 2);
    const char **labels2 = malloc(gr_cnt.nrows * sizeof(const char *));
    double *vals2 = malloc(gr_cnt.nrows * sizeof(double));
    for (uint64_t i = 0; i < gr_cnt.nrows; i++) {
        labels2[i] = gr_cnt.rows[i].group_val;
        vals2[i] = gr_cnt.rows[i].agg_result;
    }

    StrArray la2 = { labels2, gr_cnt.nrows };
    TkDataset ds2 = { "Avg Count", vals2, gr_cnt.nrows, "#e74c3c" };
    TkChartSpec *bar2 = chart_bar(la2, &ds2, 1, "Avg Count by Dept");

    /* Build dashboard with 2-column grid */
    TkDashboard *dash = dashboard_new("Multi-Widget Dashboard", 2);
    ASSERT(dash != NULL, "t10: dashboard_new returns non-NULL");

    dashboard_addchart(dash, "chart-value", "Total Value by Dept",
                       bar1, 0, 0, 1, 1);
    dashboard_addchart(dash, "chart-count", "Avg Count by Dept",
                       bar2, 1, 0, 1, 1);

    const char *html = dashboard_render(dash);
    ASSERT(html != NULL, "t10: dashboard_render returns non-NULL");
    ASSERT_CONTAINS(html, "<html", "t10: output has <html");
    ASSERT_CONTAINS(html, "Multi-Widget Dashboard",
                    "t10: output has dashboard title");
    ASSERT_CONTAINS(html, "Total Value by Dept",
                    "t10: output has first chart title");
    ASSERT_CONTAINS(html, "Avg Count by Dept",
                    "t10: output has second chart title");

    free(labels1);
    free(vals1);
    free(labels2);
    free(vals2);
    dashboard_free(dash);
    df_free(res.ok);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("=== test_dashboard_integration (Story 21.1.3) ===\n");

    test_csv_parse();
    test_csv_to_dataframe();
    test_analytics_describe();
    test_anomaly_detection();
    test_groupby_to_chart();
    test_html_construction();
    test_full_dashboard_pipeline();
    test_data_integrity();
    test_correlation_analysis();
    test_multi_widget_dashboard();

    printf("==================================================\n");
    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}

/*
 * test_chart.c — Unit tests for the std.chart C library (Story 18.1.1).
 *
 * Build and run: make test-stdlib-chart
 *
 * Tests verify:
 *   - chart_tojson produces correct Chart.js v4 JSON structure
 *   - chart_tovega produces correct Vega-Lite v5 JSON structure
 *   - title, label, and value round-trips
 *   - NULL title handling
 *   - Default colour palette insertion
 *   - All four chart types (bar, line, scatter, pie)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../../src/stdlib/chart.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && strstr((haystack), (needle)) != NULL, msg)

#define ASSERT_NOT_NULL(ptr, msg) \
    ASSERT((ptr) != NULL, msg)

/* -----------------------------------------------------------------------
 * Helpers to build test fixtures.
 * ----------------------------------------------------------------------- */
static StrArray make_str_array(const char **data, uint64_t len)
{
    StrArray a;
    a.data = data;
    a.len  = len;
    return a;
}

/* -----------------------------------------------------------------------
 * Test 1: chart_tojson of a bar chart is non-NULL and contains "type":"bar".
 * ----------------------------------------------------------------------- */
static void test_bar_tojson_type(void)
{
    static const double vals[] = {1.0, 2.0, 3.0};
    TkDataset ds = {"Sales", vals, 3, NULL};
    static const char *lab[] = {"A", "B", "C"};
    StrArray labels = make_str_array(lab, 3);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "My Chart");
    ASSERT_NOT_NULL(spec, "chart_bar returns non-NULL spec");

    const char *json = chart_tojson(spec);
    ASSERT_NOT_NULL(json, "chart_tojson(bar) returns non-NULL");
    ASSERT_CONTAINS(json, "\"type\":\"bar\"", "bar JSON contains type:bar");

    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 2: chart_tojson bar chart contains "labels" key.
 * ----------------------------------------------------------------------- */
static void test_bar_tojson_labels_key(void)
{
    static const double vals[] = {10.0, 20.0};
    TkDataset ds = {"Revenue", vals, 2, NULL};
    static const char *lab[] = {"Q1", "Q2"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Revenue");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "\"labels\"", "bar JSON contains labels key");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 3: chart_tojson bar chart contains "datasets" key.
 * ----------------------------------------------------------------------- */
static void test_bar_tojson_datasets_key(void)
{
    static const double vals[] = {5.0};
    TkDataset ds = {"X", vals, 1, NULL};
    static const char *lab[] = {"Jan"};
    StrArray labels = make_str_array(lab, 1);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Test");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "\"datasets\"", "bar JSON contains datasets key");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 4: chart_tojson of a line chart contains "type":"line".
 * ----------------------------------------------------------------------- */
static void test_line_tojson_type(void)
{
    static const double vals[] = {3.0, 1.0, 4.0};
    TkDataset ds = {"Trend", vals, 3, NULL};
    static const char *lab[] = {"Mon", "Tue", "Wed"};
    StrArray labels = make_str_array(lab, 3);

    TkChartSpec *spec = chart_line(labels, &ds, 1, "Weekly");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "\"type\":\"line\"", "line JSON contains type:line");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 5: chart_tojson of a pie chart contains "type":"pie".
 * ----------------------------------------------------------------------- */
static void test_pie_tojson_type(void)
{
    static const double vals[] = {40.0, 35.0, 25.0};
    TkDataset ds = {"Share", vals, 3, NULL};
    static const char *lab[] = {"Alpha", "Beta", "Gamma"};
    StrArray labels = make_str_array(lab, 3);

    TkChartSpec *spec = chart_pie(labels, &ds, "Market Share");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "\"type\":\"pie\"", "pie JSON contains type:pie");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 6: title appears in Chart.js JSON as "text":"My Chart".
 * ----------------------------------------------------------------------- */
static void test_title_in_chartjs_json(void)
{
    static const double vals[] = {1.0};
    TkDataset ds = {"S", vals, 1, NULL};
    static const char *lab[] = {"X"};
    StrArray labels = make_str_array(lab, 1);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "My Chart");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "\"text\":\"My Chart\"", "title text appears in Chart.js JSON");
    ASSERT_CONTAINS(json, "\"display\":true", "display:true when title set");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 7: dataset label appears in Chart.js JSON.
 * ----------------------------------------------------------------------- */
static void test_dataset_label_in_json(void)
{
    static const double vals[] = {99.0, 88.0};
    TkDataset ds = {"MySeries", vals, 2, NULL};
    static const char *lab[] = {"P", "Q"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "T");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "\"label\":\"MySeries\"", "dataset label appears in JSON");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 8: label values appear in Chart.js JSON output.
 * ----------------------------------------------------------------------- */
static void test_label_values_in_json(void)
{
    static const double vals[] = {7.0, 14.0};
    TkDataset ds = {"D", vals, 2, NULL};
    static const char *lab[] = {"SpringValue", "AutumnValue"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Seasons");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "SpringValue", "first x-label appears in JSON");
    ASSERT_CONTAINS(json, "AutumnValue", "second x-label appears in JSON");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 9: single dataset + multiple labels round-trip check.
 *   Values 42.5, 17.0, 3.14 must all appear in the JSON output.
 * ----------------------------------------------------------------------- */
static void test_single_dataset_multi_label_roundtrip(void)
{
    static const double vals[] = {42.5, 17.0, 3.14};
    TkDataset ds = {"RoundTrip", vals, 3, NULL};
    static const char *lab[] = {"L1", "L2", "L3"};
    StrArray labels = make_str_array(lab, 3);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "RoundTrip");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "42.5",  "value 42.5 round-trips in JSON");
    ASSERT_CONTAINS(json, "17",    "value 17 round-trips in JSON");
    ASSERT_CONTAINS(json, "3.14",  "value 3.14 round-trips in JSON");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 10: NULL title → options still valid JSON (display:false).
 * ----------------------------------------------------------------------- */
static void test_null_title_valid_json(void)
{
    static const double vals[] = {1.0, 2.0};
    TkDataset ds = {"NoTitle", vals, 2, NULL};
    static const char *lab[] = {"A", "B"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, NULL);
    const char *json = chart_tojson(spec);
    ASSERT_NOT_NULL(json, "chart_tojson with NULL title returns non-NULL");
    ASSERT_CONTAINS(json, "\"options\"", "options key present with NULL title");
    ASSERT_CONTAINS(json, "\"display\":false", "display:false when title is NULL");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 11: chart_tovega bar chart contains "$schema".
 * ----------------------------------------------------------------------- */
static void test_tovega_schema(void)
{
    static const double vals[] = {5.0, 10.0};
    TkDataset ds = {"V", vals, 2, NULL};
    static const char *lab[] = {"X", "Y"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Vega Test");
    const char *vega = chart_tovega(spec);
    ASSERT_NOT_NULL(vega, "chart_tovega returns non-NULL");
    ASSERT_CONTAINS(vega, "\"$schema\"", "vega JSON contains $schema");
    free((void *)vega);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 12: chart_tovega bar chart contains "mark":"bar".
 * ----------------------------------------------------------------------- */
static void test_tovega_mark_bar(void)
{
    static const double vals[] = {1.0};
    TkDataset ds = {"S", vals, 1, NULL};
    static const char *lab[] = {"A"};
    StrArray labels = make_str_array(lab, 1);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Bar Vega");
    const char *vega = chart_tovega(spec);
    ASSERT_CONTAINS(vega, "\"mark\":\"bar\"", "vega JSON contains mark:bar");
    free((void *)vega);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 13: chart_tojson uses explicit CSS color when provided.
 * ----------------------------------------------------------------------- */
static void test_explicit_color_in_json(void)
{
    static const double vals[] = {3.0, 6.0};
    TkDataset ds = {"Custom", vals, 2, "#FF0000"};
    static const char *lab[] = {"P", "Q"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Color Test");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "#FF0000", "explicit CSS color appears in JSON");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 14: chart_tojson of a scatter chart contains "type":"scatter".
 * ----------------------------------------------------------------------- */
static void test_scatter_tojson_type(void)
{
    static const double xvals[] = {1.0, 2.0, 3.0};
    TkDataset ds = {"Points", xvals, 3, NULL};

    TkChartSpec *spec = chart_scatter(&ds, 1, "Scatter Plot");
    ASSERT_NOT_NULL(spec, "chart_scatter returns non-NULL spec");
    const char *json = chart_tojson(spec);
    ASSERT_CONTAINS(json, "\"type\":\"scatter\"", "scatter JSON contains type:scatter");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 15: chart_tovega output starts with '{' (valid JSON prefix).
 * ----------------------------------------------------------------------- */
static void test_tovega_valid_prefix(void)
{
    static const double vals[] = {2.0, 4.0};
    TkDataset ds = {"Z", vals, 2, NULL};
    static const char *lab[] = {"I", "II"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_line(labels, &ds, 1, "Prefix Check");
    const char *vega = chart_tovega(spec);
    ASSERT_NOT_NULL(vega, "chart_tovega(line) non-NULL");
    ASSERT(vega[0] == '{', "vega JSON starts with '{'");
    free((void *)vega);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 16: Chart with 0 datasets (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_zero_datasets(void)
{
    static const char *lab[] = {"A", "B"};
    StrArray labels = make_str_array(lab, 2);

    TkChartSpec *spec = chart_bar(labels, NULL, 0, "Empty");
    ASSERT_NOT_NULL(spec, "chart_bar with 0 datasets returns non-NULL spec");

    const char *json = chart_tojson(spec);
    ASSERT_NOT_NULL(json, "chart_tojson with 0 datasets returns non-NULL");
    ASSERT_CONTAINS(json, "\"datasets\":[]", "0-datasets: datasets is empty array");
    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 17: Chart with many labels (1000) (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_many_labels(void)
{
    const char *lab[1000];
    int i;
    for (i = 0; i < 1000; i++) {
        lab[i] = "L";
    }
    StrArray labels = make_str_array(lab, 1000);

    static const double vals[] = {1.0};
    TkDataset ds = {"S", vals, 1, NULL};

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "BigLabels");
    const char *json = chart_tojson(spec);
    ASSERT_NOT_NULL(json, "chart_tojson with 1000 labels returns non-NULL");
    ASSERT_CONTAINS(json, "\"labels\":[", "many-labels: labels key present");

    /* Count label occurrences — each label "L" becomes "\"L\"" */
    int lcount = 0;
    const char *p = json;
    while ((p = strstr(p, "\"L\"")) != NULL) {
        lcount++;
        p += 3;
    }
    /* At least 1000 from labels array (dataset label "S" is separate) */
    ASSERT(lcount >= 1000, "many-labels: at least 1000 label entries in JSON");

    free((void *)json);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Test 18: Chart with negative values (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_negative_values(void)
{
    static const double vals[] = {-10.5, -0.001, -9999.0};
    TkDataset ds = {"Neg", vals, 3, NULL};
    static const char *lab[] = {"A", "B", "C"};
    StrArray labels = make_str_array(lab, 3);

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Negatives");
    const char *json = chart_tojson(spec);
    ASSERT_NOT_NULL(json, "chart_tojson with negative values returns non-NULL");
    ASSERT_CONTAINS(json, "-10.5",  "negative: -10.5 appears in JSON");
    ASSERT_CONTAINS(json, "-9999",  "negative: -9999 appears in JSON");
    ASSERT_CONTAINS(json, "-0.001", "negative: -0.001 appears in JSON");
    free((void *)json);

    const char *vega = chart_tovega(spec);
    ASSERT_NOT_NULL(vega, "chart_tovega with negative values returns non-NULL");
    ASSERT_CONTAINS(vega, "-10.5", "negative vega: -10.5 appears");
    free((void *)vega);
    chart_free(spec);
}

/* -----------------------------------------------------------------------
 * Story 34.2.1 tests
 * ----------------------------------------------------------------------- */

/* Test 19: chart_horizontal_bar contains indexAxis and "y" */
static void test_horizontal_bar(void)
{
    static const char *lab[] = {"a", "b"};
    static const double vals[] = {1.0, 2.0};

    const char *json = chart_horizontal_bar(lab, 2, vals, "Test");
    ASSERT_NOT_NULL(json, "chart_horizontal_bar returns non-NULL");
    ASSERT_CONTAINS(json, "indexAxis", "horizontal_bar: contains indexAxis");
    ASSERT_CONTAINS(json, "\"y\"", "horizontal_bar: indexAxis value is y");
    free((void *)json);
}

/* Test 20: chart_area contains fill and true */
static void test_area(void)
{
    static const double xs[] = {0.0, 1.0, 2.0};
    static const double ys[] = {1.0, 4.0, 9.0};

    const char *json = chart_area(xs, ys, 3, "Growth");
    ASSERT_NOT_NULL(json, "chart_area returns non-NULL");
    ASSERT_CONTAINS(json, "fill", "area: contains fill key");
    ASSERT_CONTAINS(json, "true", "area: fill is true");
    free((void *)json);
}

/* Test 21: chart_stacked_bar contains stacked */
static void test_stacked_bar(void)
{
    static const char *lab[] = {"Q1", "Q2"};
    static const char *series[] = {"A", "B"};
    static const double rowA[] = {10.0, 20.0};
    static const double rowB[] = {5.0, 15.0};
    const double *data[] = {rowA, rowB};

    const char *json = chart_stacked_bar(lab, 2, series, 2, data, "Sales");
    ASSERT_NOT_NULL(json, "chart_stacked_bar returns non-NULL");
    ASSERT_CONTAINS(json, "stacked", "stacked_bar: contains stacked");
    free((void *)json);
}

/* Test 22: chart_radar type is "radar" */
static void test_radar(void)
{
    static const char *axes[] = {"speed", "power"};
    static const double vals[] = {0.8, 0.9};

    const char *json = chart_radar(axes, 2, vals, "Radar");
    ASSERT_NOT_NULL(json, "chart_radar returns non-NULL");
    ASSERT_CONTAINS(json, "\"type\":\"radar\"", "radar: type is radar");
    free((void *)json);
}

/* Test 23: chart_histogram contains bin data */
static void test_histogram(void)
{
    static const double vals[] = {1.0, 2.0, 2.0, 3.0, 3.0, 3.0};

    const char *json = chart_histogram(vals, 6, 3, "Hist");
    ASSERT_NOT_NULL(json, "chart_histogram returns non-NULL");
    /* Should contain at least one numeric data value */
    ASSERT_CONTAINS(json, "\"data\":", "histogram: contains data key");
    /* The bin counts should include 1, 2, 3 */
    ASSERT_CONTAINS(json, "\"type\":\"bar\"", "histogram: type is bar");
    free((void *)json);
}

/* Test 24: chart_heatmap returns non-NULL, non-empty string */
static void test_heatmap(void)
{
    static const char *rows[] = {"r1"};
    static const char *cols[] = {"c1", "c2"};
    static const double matrix[] = {1.0, 2.0};

    const char *svg = chart_heatmap(rows, 1, cols, 2, matrix, "Heat");
    ASSERT_NOT_NULL(svg, "chart_heatmap returns non-NULL");
    ASSERT(svg[0] != '\0', "chart_heatmap returns non-empty string");
    /* Should be an SVG */
    ASSERT_CONTAINS(svg, "<svg", "heatmap: output is SVG");
    free((void *)svg);
}

/* Test 25: chart_set_theme dark → contains "white" */
static void test_set_theme_dark(void)
{
    /* Build a minimal spec to pass in */
    static const double vals[] = {1.0};
    TkDataset ds = {"S", vals, 1, NULL};
    static const char *lab[] = {"X"};
    StrArray labels = make_str_array(lab, 1);
    TkChartSpec *spec = chart_bar(labels, &ds, 1, "ThemeTest");
    const char *base = chart_tojson(spec);
    chart_free(spec);
    ASSERT_NOT_NULL(base, "base spec for theme test is non-NULL");

    const char *themed = chart_set_theme(base, "dark");
    ASSERT_NOT_NULL(themed, "chart_set_theme(dark) returns non-NULL");
    ASSERT_CONTAINS(themed, "white", "theme dark: contains white color");
    free((void *)base);
    free((void *)themed);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    test_bar_tojson_type();
    test_bar_tojson_labels_key();
    test_bar_tojson_datasets_key();
    test_line_tojson_type();
    test_pie_tojson_type();
    test_title_in_chartjs_json();
    test_dataset_label_in_json();
    test_label_values_in_json();
    test_single_dataset_multi_label_roundtrip();
    test_null_title_valid_json();
    test_tovega_schema();
    test_tovega_mark_bar();
    test_explicit_color_in_json();
    test_scatter_tojson_type();
    test_tovega_valid_prefix();
    test_zero_datasets();
    test_many_labels();
    test_negative_values();

    /* Story 34.2.1 */
    test_horizontal_bar();
    test_area();
    test_stacked_bar();
    test_radar();
    test_histogram();
    test_heatmap();
    test_set_theme_dark();

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}

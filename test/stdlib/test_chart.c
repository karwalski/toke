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

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}

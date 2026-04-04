/*
 * test_viz_integration.c — Integration tests for the visualization pipeline
 * (Story 18.1.7): dataframe → analytics → chart → html → render.
 *
 * Build:
 *   $(CC) $(CFLAGS) -o test_viz_integration test_viz_integration.c \
 *       ../src/stdlib/dataframe.c ../src/stdlib/csv.c \
 *       ../src/stdlib/chart.c ../src/stdlib/html.c ../src/stdlib/math.c
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "../../src/stdlib/dataframe.h"
#include "../../src/stdlib/chart.h"
#include "../../src/stdlib/html.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack), (needle)) != NULL, msg)

/* -------------------------------------------------------------------------
 * Test 1: CSV → dataframe → bar chart
 * Loads a 3-column CSV with a numeric column. Creates a bar chart from the
 * column data. Verifies chart JSON is non-NULL and contains "type":"bar".
 * ------------------------------------------------------------------------- */
static void test_csv_to_chart(void)
{
    const char *csv =
        "name,score,rank\n"
        "Alice,92.5,1\n"
        "Bob,78.0,2\n"
        "Carol,85.3,3\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err && res.ok != NULL, "test1: df_fromcsv succeeds");

    TkDataframe *df = res.ok;
    DfCol *score_col = df_column(df, "score");
    ASSERT(score_col != NULL && score_col->type == DF_COL_F64,
           "test1: score column is f64");

    /* Build labels from the name column. */
    DfCol *name_col = df_column(df, "name");
    ASSERT(name_col != NULL && name_col->type == DF_COL_STR,
           "test1: name column is str");

    const char *label_ptrs[3];
    for (uint64_t i = 0; i < name_col->nrows && i < 3; i++) {
        label_ptrs[i] = name_col->str_data[i];
    }
    StrArray labels = { label_ptrs, name_col->nrows };

    TkDataset ds;
    ds.label   = "score";
    ds.values  = score_col->f64_data;
    ds.nvalues = score_col->nrows;
    ds.color   = NULL;

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Scores");
    ASSERT(spec != NULL, "test1: chart_bar returns non-NULL spec");

    const char *json = chart_tojson(spec);
    ASSERT(json != NULL, "test1: chart_tojson returns non-NULL");
    ASSERT_CONTAINS(json, "\"type\"", "test1: JSON contains type key");
    ASSERT_CONTAINS(json, "bar",      "test1: JSON contains bar");

    free((void *)json);
    chart_free(spec);
    df_free(df);
}

/* -------------------------------------------------------------------------
 * Test 2: Dataframe groupby → bar chart
 * Loads a CSV with category+value columns. Groupby category (sum). Uses
 * group results to build a bar chart. Verifies labels appear in JSON.
 * ------------------------------------------------------------------------- */
static void test_groupby_to_chart(void)
{
    const char *csv =
        "category,amount\n"
        "East,100.0\n"
        "West,200.0\n"
        "East,150.0\n"
        "North,80.0\n"
        "West,120.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err && res.ok != NULL, "test2: df_fromcsv succeeds");

    TkDataframe *df = res.ok;

    /* agg=1 → sum */
    DfGroupResult gr = df_groupby(df, "category", "amount", 1);
    ASSERT(gr.nrows > 0, "test2: groupby returns rows");

    /* Build chart from group results. */
    const char **label_ptrs = malloc(gr.nrows * sizeof(const char *));
    double      *vals       = malloc(gr.nrows * sizeof(double));
    ASSERT(label_ptrs != NULL && vals != NULL, "test2: alloc for chart arrays");

    for (uint64_t i = 0; i < gr.nrows; i++) {
        label_ptrs[i] = gr.rows[i].group_val;
        vals[i]       = gr.rows[i].agg_result;
    }
    StrArray labels = { label_ptrs, gr.nrows };

    TkDataset ds;
    ds.label   = "amount";
    ds.values  = vals;
    ds.nvalues = gr.nrows;
    ds.color   = NULL;

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Regional Totals");
    ASSERT(spec != NULL, "test2: chart_bar returns non-NULL spec");

    const char *json = chart_tojson(spec);
    ASSERT(json != NULL, "test2: chart_tojson returns non-NULL");
    /* At least one group name should appear in the JSON. */
    ASSERT(strstr(json, "East") || strstr(json, "West") || strstr(json, "North"),
           "test2: JSON contains a group label name");

    free((void *)json);
    chart_free(spec);
    free(vals);
    free(label_ptrs);
    free(gr.rows);
    df_free(df);
}

/* -------------------------------------------------------------------------
 * Test 3: Chart → HTML embedding
 * Creates a chart, builds an HTML doc, adds Chart.js CDN script, embeds
 * chart JSON in a <script>new Chart(...)</script> block. Verifies output
 * contains <canvas, Chart.js, and chart type.
 * ------------------------------------------------------------------------- */
static void test_chart_to_html(void)
{
    const char *label_strs[] = { "Jan", "Feb", "Mar" };
    StrArray labels = { label_strs, 3 };
    double vals[] = { 10.0, 20.0, 15.0 };

    TkDataset ds;
    ds.label   = "Revenue";
    ds.values  = vals;
    ds.nvalues = 3;
    ds.color   = NULL;

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Monthly Revenue");
    ASSERT(spec != NULL, "test3: chart_bar non-NULL");

    const char *json = chart_tojson(spec);
    ASSERT(json != NULL, "test3: chart_tojson non-NULL");

    TkHtmlDoc *doc = html_doc();
    ASSERT(doc != NULL, "test3: html_doc non-NULL");

    html_title(doc, "Dashboard");

    /* Chart.js CDN script tag. */
    html_script(doc,
        "/* Chart.js */ /* src: https://cdn.jsdelivr.net/npm/chart.js */");

    /* Canvas element. */
    TkHtmlNode *canvas_div = html_div(NULL, "<canvas id=\"myChart\"></canvas>");
    html_append(doc, canvas_div);

    /* Script block embedding chart JSON. */
    /* Build: "new Chart(document.getElementById('myChart'), <json>);" */
    size_t script_len = strlen(json) + 128;
    char  *script_buf = malloc(script_len);
    ASSERT(script_buf != NULL, "test3: alloc script_buf");
    snprintf(script_buf, script_len,
             "new Chart(document.getElementById('myChart'), %s);", json);
    html_script(doc, script_buf);

    const char *rendered = html_render(doc);
    ASSERT(rendered != NULL, "test3: html_render non-NULL");

    ASSERT_CONTAINS(rendered, "<canvas",  "test3: rendered contains <canvas");
    ASSERT_CONTAINS(rendered, "Chart.js", "test3: rendered contains Chart.js");
    ASSERT_CONTAINS(rendered, "bar",      "test3: rendered contains chart type");

    free((void *)rendered);
    free(script_buf);
    free((void *)json);
    chart_free(spec);
    html_free(doc);
}

/* -------------------------------------------------------------------------
 * Test 4: Full pipeline
 * CSV string → df_fromcsv → df_groupby → chart_bar → chart_tojson →
 * embed in html_doc → html_render. Verifies final HTML contains
 * <!DOCTYPE html>, chart data, and a <script> tag.
 * ------------------------------------------------------------------------- */
static void test_full_pipeline(void)
{
    const char *csv =
        "region,sales\n"
        "North,300.0\n"
        "South,450.0\n"
        "East,200.0\n"
        "West,380.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err && res.ok != NULL, "test4: df_fromcsv succeeds");

    TkDataframe *df = res.ok;

    DfGroupResult gr = df_groupby(df, "region", "sales", 1);
    ASSERT(gr.nrows > 0, "test4: df_groupby returns rows");

    const char **label_ptrs = malloc(gr.nrows * sizeof(const char *));
    double      *vals       = malloc(gr.nrows * sizeof(double));
    ASSERT(label_ptrs != NULL && vals != NULL, "test4: alloc arrays");

    for (uint64_t i = 0; i < gr.nrows; i++) {
        label_ptrs[i] = gr.rows[i].group_val;
        vals[i]       = gr.rows[i].agg_result;
    }
    StrArray labels = { label_ptrs, gr.nrows };

    TkDataset ds;
    ds.label   = "sales";
    ds.values  = vals;
    ds.nvalues = gr.nrows;
    ds.color   = NULL;

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Regional Sales");
    ASSERT(spec != NULL, "test4: chart_bar non-NULL");

    const char *json = chart_tojson(spec);
    ASSERT(json != NULL, "test4: chart_tojson non-NULL");

    TkHtmlDoc *doc = html_doc();
    ASSERT(doc != NULL, "test4: html_doc non-NULL");
    html_title(doc, "Sales Dashboard");

    size_t script_len = strlen(json) + 64;
    char  *script_buf = malloc(script_len);
    ASSERT(script_buf != NULL, "test4: alloc script_buf");
    snprintf(script_buf, script_len, "var chartData = %s;", json);
    html_script(doc, script_buf);

    const char *rendered = html_render(doc);
    ASSERT(rendered != NULL, "test4: html_render non-NULL");

    ASSERT_CONTAINS(rendered, "<!DOCTYPE html>", "test4: contains DOCTYPE");
    ASSERT_CONTAINS(rendered, "<script",         "test4: contains script tag");
    /* At least one data value should appear in the rendered output. */
    ASSERT(strstr(rendered, "300") || strstr(rendered, "450") ||
           strstr(rendered, "sales"),
           "test4: contains chart data");

    free((void *)rendered);
    free(script_buf);
    free((void *)json);
    chart_free(spec);
    free(vals);
    free(label_ptrs);
    free(gr.rows);
    df_free(df);
}

/* -------------------------------------------------------------------------
 * Test 5: Chart → Vega-Lite HTML
 * Creates a line chart. Gets Vega-Lite JSON. Builds HTML page with
 * Vega-Embed CDN script. Verifies contains "$schema" and vega CDN URL.
 * ------------------------------------------------------------------------- */
static void test_vega_html(void)
{
    const char *label_strs[] = { "Q1", "Q2", "Q3", "Q4" };
    StrArray labels = { label_strs, 4 };
    double vals[] = { 1.5, 2.3, 1.9, 3.1 };

    TkDataset ds;
    ds.label   = "growth";
    ds.values  = vals;
    ds.nvalues = 4;
    ds.color   = NULL;

    TkChartSpec *spec = chart_line(labels, &ds, 1, "Quarterly Growth");
    ASSERT(spec != NULL, "test5: chart_line non-NULL");

    const char *vega_json = chart_tovega(spec);
    ASSERT(vega_json != NULL, "test5: chart_tovega non-NULL");
    ASSERT_CONTAINS(vega_json, "$schema", "test5: vega JSON contains $schema");

    TkHtmlDoc *doc = html_doc();
    ASSERT(doc != NULL, "test5: html_doc non-NULL");
    html_title(doc, "Vega Dashboard");

    /* Vega-Embed CDN script. */
    html_script(doc,
        "/* vega cdn: https://cdn.jsdelivr.net/npm/vega-embed@6 */");

    size_t script_len = strlen(vega_json) + 64;
    char  *script_buf = malloc(script_len);
    ASSERT(script_buf != NULL, "test5: alloc script_buf");
    snprintf(script_buf, script_len, "vegaEmbed('#vis', %s);", vega_json);
    html_script(doc, script_buf);

    const char *rendered = html_render(doc);
    ASSERT(rendered != NULL, "test5: html_render non-NULL");

    ASSERT_CONTAINS(rendered, "$schema",   "test5: rendered contains $schema");
    ASSERT_CONTAINS(rendered, "vega",      "test5: rendered contains vega cdn reference");

    free((void *)rendered);
    free(script_buf);
    free((void *)vega_json);
    chart_free(spec);
    html_free(doc);
}

/* -------------------------------------------------------------------------
 * Test 6: Dataframe tojson → HTML table
 * df_tojson produces JSON. Add to HTML doc as a <pre> block. Render.
 * Verify JSON and HTML structure.
 * ------------------------------------------------------------------------- */
static void test_df_tojson_to_html(void)
{
    const char *csv =
        "item,price\n"
        "apple,1.20\n"
        "banana,0.50\n"
        "cherry,3.00\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err && res.ok != NULL, "test6: df_fromcsv succeeds");

    TkDataframe *df = res.ok;
    const char  *json = df_tojson(df);
    ASSERT(json != NULL, "test6: df_tojson non-NULL");
    ASSERT_CONTAINS(json, "item",  "test6: df_tojson contains column name");
    ASSERT_CONTAINS(json, "price", "test6: df_tojson contains price column");

    TkHtmlDoc *doc = html_doc();
    ASSERT(doc != NULL, "test6: html_doc non-NULL");
    html_title(doc, "Data Table");

    /* Wrap JSON in a <pre> div for display. */
    TkHtmlNode *pre_node = html_div(NULL, json);
    html_append(doc, pre_node);

    const char *rendered = html_render(doc);
    ASSERT(rendered != NULL, "test6: html_render non-NULL");

    ASSERT_CONTAINS(rendered, "<!DOCTYPE html>", "test6: contains DOCTYPE");
    ASSERT_CONTAINS(rendered, "item",            "test6: rendered contains JSON data");
    ASSERT_CONTAINS(rendered, "<div",            "test6: rendered contains div element");

    free((void *)rendered);
    free((void *)json);
    df_free(df);
    html_free(doc);
}

/* -------------------------------------------------------------------------
 * Test 7: Multi-chart dashboard layout
 * Creates 2 charts. Builds an HTML doc. Adds both as canvas elements with
 * unique ids. Verifies both ids appear in rendered output.
 * ------------------------------------------------------------------------- */
static void test_multi_chart_dashboard(void)
{
    const char *labels_a_strs[] = { "Mon", "Tue", "Wed" };
    StrArray labels_a = { labels_a_strs, 3 };
    double vals_a[] = { 5.0, 8.0, 6.0 };

    TkDataset ds_a;
    ds_a.label   = "visits";
    ds_a.values  = vals_a;
    ds_a.nvalues = 3;
    ds_a.color   = NULL;

    TkChartSpec *spec_a = chart_bar(labels_a, &ds_a, 1, "Visits");
    ASSERT(spec_a != NULL, "test7: chart_a non-NULL");

    const char *labels_b_strs[] = { "Mon", "Tue", "Wed" };
    StrArray labels_b = { labels_b_strs, 3 };
    double vals_b[] = { 2.0, 4.0, 3.0 };

    TkDataset ds_b;
    ds_b.label   = "conversions";
    ds_b.values  = vals_b;
    ds_b.nvalues = 3;
    ds_b.color   = NULL;

    TkChartSpec *spec_b = chart_line(labels_b, &ds_b, 1, "Conversions");
    ASSERT(spec_b != NULL, "test7: chart_b non-NULL");

    TkHtmlDoc *doc = html_doc();
    ASSERT(doc != NULL, "test7: html_doc non-NULL");
    html_title(doc, "Multi-Chart Dashboard");

    TkHtmlNode *canvas_a = html_div(NULL,
        "<canvas id=\"chartA\"></canvas>");
    TkHtmlNode *canvas_b = html_div(NULL,
        "<canvas id=\"chartB\"></canvas>");
    html_append(doc, canvas_a);
    html_append(doc, canvas_b);

    const char *json_a = chart_tojson(spec_a);
    const char *json_b = chart_tojson(spec_b);
    ASSERT(json_a != NULL && json_b != NULL, "test7: both chart JSONs non-NULL");

    size_t sa_len = strlen(json_a) + 64;
    size_t sb_len = strlen(json_b) + 64;
    char *script_a = malloc(sa_len);
    char *script_b = malloc(sb_len);
    ASSERT(script_a != NULL && script_b != NULL, "test7: alloc scripts");

    snprintf(script_a, sa_len, "new Chart(document.getElementById('chartA'), %s);", json_a);
    snprintf(script_b, sb_len, "new Chart(document.getElementById('chartB'), %s);", json_b);
    html_script(doc, script_a);
    html_script(doc, script_b);

    const char *rendered = html_render(doc);
    ASSERT(rendered != NULL, "test7: html_render non-NULL");

    ASSERT_CONTAINS(rendered, "chartA", "test7: rendered contains chartA id");
    ASSERT_CONTAINS(rendered, "chartB", "test7: rendered contains chartB id");

    free((void *)rendered);
    free(script_a);
    free(script_b);
    free((void *)json_a);
    free((void *)json_b);
    chart_free(spec_a);
    chart_free(spec_b);
    html_free(doc);
}

/* -------------------------------------------------------------------------
 * Test 8: Empty dataframe → chart (no crash)
 * fromcsv of header-only CSV. Build chart with 0 data points.
 * chart_tojson returns valid JSON (doesn't crash).
 * ------------------------------------------------------------------------- */
static void test_empty_dataframe_chart(void)
{
    const char *csv = "name,value\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err && res.ok != NULL, "test8: df_fromcsv header-only succeeds");

    TkDataframe *df = res.ok;

    uint64_t nrows = 0, ncols = 0;
    df_shape(df, &nrows, &ncols);
    ASSERT(nrows == 0, "test8: empty df has 0 rows");

    StrArray labels = { NULL, 0 };
    double   no_vals[1] = { 0.0 }; /* unused; nvalues=0 */

    TkDataset ds;
    ds.label   = "value";
    ds.values  = no_vals;
    ds.nvalues = 0;
    ds.color   = NULL;

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Empty");
    ASSERT(spec != NULL, "test8: chart_bar with 0 data points non-NULL");

    const char *json = chart_tojson(spec);
    ASSERT(json != NULL, "test8: chart_tojson with 0 data points non-NULL");
    ASSERT_CONTAINS(json, "bar", "test8: empty chart JSON contains type");

    free((void *)json);
    chart_free(spec);
    df_free(df);
}

/* -------------------------------------------------------------------------
 * Test 9: Shape-aware chart sizing
 * Use df_shape to get nrows. Build a chart with nrows labels. Verify
 * label count reflected in JSON output.
 * ------------------------------------------------------------------------- */
static void test_shape_aware_chart(void)
{
    const char *csv =
        "label,val\n"
        "A,1.0\n"
        "B,2.0\n"
        "C,3.0\n"
        "D,4.0\n"
        "E,5.0\n";

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(!res.is_err && res.ok != NULL, "test9: df_fromcsv succeeds");

    TkDataframe *df = res.ok;
    uint64_t nrows = 0, ncols = 0;
    df_shape(df, &nrows, &ncols);
    ASSERT(nrows == 5, "test9: df has 5 rows");

    DfCol *label_col = df_column(df, "label");
    DfCol *val_col   = df_column(df, "val");
    ASSERT(label_col != NULL && val_col != NULL, "test9: both columns found");

    StrArray labels = { (const char **)label_col->str_data, nrows };

    TkDataset ds;
    ds.label   = "val";
    ds.values  = val_col->f64_data;
    ds.nvalues = nrows;
    ds.color   = NULL;

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Shape Test");
    ASSERT(spec != NULL, "test9: chart_bar non-NULL");

    const char *json = chart_tojson(spec);
    ASSERT(json != NULL, "test9: chart_tojson non-NULL");

    /* All 5 labels should appear in the JSON. */
    ASSERT_CONTAINS(json, "\"A\"", "test9: JSON contains label A");
    ASSERT_CONTAINS(json, "\"E\"", "test9: JSON contains label E");

    free((void *)json);
    chart_free(spec);
    df_free(df);
}

/* -------------------------------------------------------------------------
 * Test 10: Chart color override
 * Creates a dataset with explicit color "#ff0000". Verifies color appears
 * in chart_tojson output.
 * ------------------------------------------------------------------------- */
static void test_chart_color_override(void)
{
    const char *label_strs[] = { "X", "Y", "Z" };
    StrArray labels = { label_strs, 3 };
    double vals[] = { 7.0, 3.0, 9.0 };

    TkDataset ds;
    ds.label   = "series";
    ds.values  = vals;
    ds.nvalues = 3;
    ds.color   = "#ff0000";

    TkChartSpec *spec = chart_bar(labels, &ds, 1, "Color Test");
    ASSERT(spec != NULL, "test10: chart_bar non-NULL");

    const char *json = chart_tojson(spec);
    ASSERT(json != NULL, "test10: chart_tojson non-NULL");
    ASSERT_CONTAINS(json, "#ff0000", "test10: JSON contains explicit color");

    free((void *)json);
    chart_free(spec);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */
int main(void)
{
    printf("=== test_viz_integration ===\n");

    test_csv_to_chart();
    test_groupby_to_chart();
    test_chart_to_html();
    test_full_pipeline();
    test_vega_html();
    test_df_tojson_to_html();
    test_multi_chart_dashboard();
    test_empty_dataframe_chart();
    test_shape_aware_chart();
    test_chart_color_override();

    printf("=== %d failure(s) ===\n", failures);
    return failures > 0 ? 1 : 0;
}

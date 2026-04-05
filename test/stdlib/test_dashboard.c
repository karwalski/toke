/*
 * test_dashboard.c — Unit tests for the std.dashboard C library (Story 18.1.3).
 *
 * Build and run: make test-stdlib-dashboard
 *
 * Tests cover:
 *   - dashboard_new returns non-NULL
 *   - dashboard_render of empty dashboard contains boilerplate HTML
 *   - chart widget: canvas element and Chart.js CDN URL present
 *   - chart widget id appears in canvas id attribute
 *   - chart widget title appears in rendered h2
 *   - table widget: table, th, td elements present
 *   - table widget title appears in rendered h2
 *   - two widgets: both ids appear in rendered output
 *   - dashboard_update changes chart spec, reflected in next render
 *   - dashboard_render always returns non-NULL
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/dashboard.h"
#include "../../src/stdlib/chart.h"
#include "../../src/stdlib/router.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack), (needle)) != NULL, msg)

#define ASSERT_NOT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack), (needle)) == NULL, msg)

/* -----------------------------------------------------------------------
 * Helpers to build minimal chart specs for testing
 * ----------------------------------------------------------------------- */

static TkChartSpec *make_bar_chart(const char *title)
{
    static const char  *labels_data[] = {"A", "B", "C"};
    static const double vals[]        = {1.0, 2.0, 3.0};
    static TkDataset    ds;

    StrArray labels;
    labels.data = labels_data;
    labels.len  = 3;

    ds.label   = "series1";
    ds.values  = vals;
    ds.nvalues = 3;
    ds.color   = NULL;

    return chart_bar(labels, &ds, 1, title);
}

static TkChartSpec *make_line_chart(const char *title)
{
    static const char  *labels_data[] = {"X", "Y"};
    static const double vals[]        = {10.0, 20.0};
    static TkDataset    ds;

    StrArray labels;
    labels.data = labels_data;
    labels.len  = 2;

    ds.label   = "line1";
    ds.values  = vals;
    ds.nvalues = 2;
    ds.color   = NULL;

    return chart_line(labels, &ds, 1, title);
}

/* -----------------------------------------------------------------------
 * Tests
 * ----------------------------------------------------------------------- */

static void test_new_returns_nonnull(void)
{
    TkDashboard *d = dashboard_new("Test", 2);
    ASSERT(d != NULL, "dashboard_new returns non-NULL");
    dashboard_free(d);
}

static void test_empty_render_doctype(void)
{
    TkDashboard *d = dashboard_new("MyDash", 2);
    const char  *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "<!DOCTYPE html>",
                    "empty dashboard render contains <!DOCTYPE html>");
    dashboard_free(d);
}

static void test_empty_render_title(void)
{
    TkDashboard *d = dashboard_new("MyDash", 2);
    const char  *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "MyDash", "empty dashboard render contains title");
    dashboard_free(d);
}

static void test_empty_render_css_grid(void)
{
    TkDashboard *d = dashboard_new("Test", 3);
    const char  *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "display:grid",
                    "empty dashboard render contains CSS grid style");
    dashboard_free(d);
}

static void test_chart_render_canvas(void)
{
    TkDashboard *d     = dashboard_new("ChartDash", 2);
    TkChartSpec *spec  = make_bar_chart("Sales");
    dashboard_addchart(d, "chart1", "Sales Chart", spec, 0, 0, 1, 1);
    const char *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "<canvas", "chart widget render contains <canvas");
    chart_free(spec);
    dashboard_free(d);
}

static void test_chart_render_cdn_url(void)
{
    TkDashboard *d     = dashboard_new("ChartDash", 2);
    TkChartSpec *spec  = make_bar_chart("Revenue");
    dashboard_addchart(d, "chart2", "Revenue Chart", spec, 0, 0, 1, 1);
    const char *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "https://cdn.jsdelivr.net/npm/chart.js",
                    "chart widget render contains Chart.js CDN URL");
    chart_free(spec);
    dashboard_free(d);
}

static void test_chart_id_in_canvas(void)
{
    TkDashboard *d    = dashboard_new("Test", 2);
    TkChartSpec *spec = make_bar_chart("T");
    dashboard_addchart(d, "mywidget99", "Title", spec, 0, 0, 1, 1);
    const char *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "id=\"mywidget99\"",
                    "chart widget id appears in canvas id attribute");
    chart_free(spec);
    dashboard_free(d);
}

static void test_chart_title_in_h2(void)
{
    TkDashboard *d    = dashboard_new("Test", 2);
    TkChartSpec *spec = make_bar_chart("T");
    dashboard_addchart(d, "cw1", "My Chart Title", spec, 0, 0, 1, 1);
    const char *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "My Chart Title",
                    "chart widget title appears in rendered h2");
    chart_free(spec);
    dashboard_free(d);
}

static void test_table_render_elements(void)
{
    TkDashboard *d = dashboard_new("TableDash", 2);

    const char *headers[] = {"Name", "Score"};
    const char *cells[]   = {"Alice", "95", "Bob", "87"};

    dashboard_addtable(d, "tbl1", "Results",
                        headers, 2,
                        cells, 2,
                        0, 0, 1, 1);
    const char *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "<table",  "table widget render contains <table");
    ASSERT_CONTAINS(html, "<th",     "table widget render contains <th");
    ASSERT_CONTAINS(html, "<td",     "table widget render contains <td");
    dashboard_free(d);
}

static void test_table_title_in_h2(void)
{
    TkDashboard *d = dashboard_new("Test", 2);

    const char *headers[] = {"Col"};
    const char *cells[]   = {"Val"};

    dashboard_addtable(d, "t2", "My Table Title",
                        headers, 1,
                        cells, 1,
                        0, 0, 1, 1);
    const char *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "My Table Title",
                    "table widget title appears in rendered h2");
    dashboard_free(d);
}

static void test_two_widgets_both_ids(void)
{
    TkDashboard *d     = dashboard_new("Multi", 2);
    TkChartSpec *spec  = make_bar_chart("S");

    dashboard_addchart(d, "widget_alpha", "Chart A", spec, 0, 0, 1, 1);

    const char *headers[] = {"H"};
    const char *cells[]   = {"V"};
    dashboard_addtable(d, "widget_beta", "Table B",
                        headers, 1, cells, 1,
                        1, 0, 1, 1);

    const char *html = dashboard_render(d);
    ASSERT_CONTAINS(html, "widget_alpha",
                    "two-widget render contains first widget id");
    ASSERT_CONTAINS(html, "widget_beta",
                    "two-widget render contains second widget id");
    chart_free(spec);
    dashboard_free(d);
}

static void test_update_changes_chart(void)
{
    TkDashboard *d      = dashboard_new("UpdateTest", 1);
    TkChartSpec *spec1  = make_bar_chart("OriginalTitle");
    TkChartSpec *spec2  = make_line_chart("UpdatedTitle");

    dashboard_addchart(d, "upd1", "Widget", spec1, 0, 0, 1, 1);

    /* First render — spec1 JSON should appear */
    const char *html1 = dashboard_render(d);
    const char *json1 = chart_tojson(spec1);
    ASSERT_CONTAINS(html1, json1 ? json1 : "bar",
                    "pre-update render contains original spec JSON");

    /* Update to spec2 */
    dashboard_update(d, "upd1", spec2);

    const char *html2 = dashboard_render(d);
    const char *json2 = chart_tojson(spec2);
    ASSERT_CONTAINS(html2, json2 ? json2 : "line",
                    "post-update render contains new spec JSON");

    chart_free(spec1);
    chart_free(spec2);
    dashboard_free(d);
}

static void test_serve_function_exists(void)
{
    /* Verify dashboard_serve is a valid, non-NULL function pointer.
     * We do not actually call it (it would block), just confirm linkage. */
    typedef TkRouterErr (*ServeFn)(TkDashboard *, uint64_t);
    ServeFn fn = dashboard_serve;
    ASSERT(fn != NULL, "dashboard_serve function pointer is non-NULL");
}

static void test_serve_null_dashboard(void)
{
    /* Calling with NULL should return an error, not crash or block. */
    TkRouterErr err = dashboard_serve(NULL, 9999);
    ASSERT(err.failed != 0,
           "dashboard_serve(NULL, ...) returns error");
    ASSERT(err.msg != NULL,
           "dashboard_serve(NULL, ...) error has message");
}

static void test_render_always_nonnull(void)
{
    /* Empty dashboard */
    TkDashboard *d1   = dashboard_new("", 1);
    const char  *html = dashboard_render(d1);
    ASSERT(html != NULL, "dashboard_render always returns non-NULL");
    dashboard_free(d1);

    /* NULL title fallback */
    TkDashboard *d2    = dashboard_new(NULL, 2);
    const char  *html2 = dashboard_render(d2);
    ASSERT(html2 != NULL,
           "dashboard_render non-NULL when created with NULL title");
    dashboard_free(d2);
}

/* -----------------------------------------------------------------------
 * Test: Dashboard with 0 widgets (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_zero_widgets_render(void)
{
    TkDashboard *d   = dashboard_new("EmptyDash", 3);
    const char  *html = dashboard_render(d);

    ASSERT(html != NULL, "0-widgets: render returns non-NULL");
    ASSERT_CONTAINS(html, "<!DOCTYPE html>",
                    "0-widgets: render produces valid DOCTYPE");
    ASSERT_CONTAINS(html, "<html>",
                    "0-widgets: render contains <html>");
    ASSERT_CONTAINS(html, "</html>",
                    "0-widgets: render contains </html>");
    ASSERT_CONTAINS(html, "display:grid",
                    "0-widgets: CSS grid still present with no widgets");

    dashboard_free(d);
}

/* -----------------------------------------------------------------------
 * Test: Dashboard render produces valid HTML structure (Story 20.1.8)
 * ----------------------------------------------------------------------- */
static void test_render_valid_html_structure(void)
{
    TkDashboard *d    = dashboard_new("StructTest", 2);
    TkChartSpec *spec = make_bar_chart("Test");
    dashboard_addchart(d, "s1", "Widget", spec, 0, 0, 1, 1);

    const char *html = dashboard_render(d);
    ASSERT(html != NULL, "html-structure: render non-NULL");
    ASSERT_CONTAINS(html, "<head>",
                    "html-structure: contains <head>");
    ASSERT_CONTAINS(html, "</head>",
                    "html-structure: contains </head>");
    ASSERT_CONTAINS(html, "<body>",
                    "html-structure: contains <body>");
    ASSERT_CONTAINS(html, "</body>",
                    "html-structure: contains </body>");
    ASSERT_CONTAINS(html, "<title>StructTest</title>",
                    "html-structure: title tag correct");
    ASSERT_CONTAINS(html, "<style>",
                    "html-structure: style tag present");

    chart_free(spec);
    dashboard_free(d);
}

/* -----------------------------------------------------------------------
 * Story 34.4.1: stat/gauge/markdown widgets, theming, JSON export
 * ----------------------------------------------------------------------- */

static void test_stat_widget_render_nonnull(void)
{
    TkDashboard *d = dashboard_new("StatDash", 2);
    dashboard_add_stat(d, "stat1", "Users", "42", "");
    const char *html = dashboard_render(d);
    ASSERT(html != NULL, "stat widget: render returns non-NULL");
    dashboard_free(d);
}

static void test_gauge_widget_export_json_contains_gauge(void)
{
    TkDashboard *d = dashboard_new("GaugeDash", 2);
    dashboard_add_gauge(d, "g1", "CPU", 75.0, 0.0, 100.0);
    const char *json = dashboard_export_json(d);
    ASSERT_CONTAINS(json, "gauge",
                    "gauge widget: export_json contains \"gauge\"");
    dashboard_free(d);
}

static void test_markdown_widget_export_json_contains_content(void)
{
    TkDashboard *d = dashboard_new("MdDash", 2);
    dashboard_add_markdown(d, "md1", "## Hello world");
    const char *json = dashboard_export_json(d);
    ASSERT_CONTAINS(json, "Hello world",
                    "markdown widget: export_json contains markdown content");
    dashboard_free(d);
}

static void test_set_theme_dark_in_export_json(void)
{
    TkDashboard *d = dashboard_new("ThemeDash", 2);
    dashboard_set_theme(d, "dark");
    const char *json = dashboard_export_json(d);
    ASSERT_CONTAINS(json, "dark",
                    "set_theme(dark): export_json contains \"dark\"");
    dashboard_free(d);
}

static void test_set_refresh_interval_in_export_json(void)
{
    TkDashboard *d = dashboard_new("RefreshDash", 2);
    dashboard_set_refresh_interval(d, 5000);
    const char *json = dashboard_export_json(d);
    ASSERT_CONTAINS(json, "5000",
                    "set_refresh_interval(5000): export_json contains 5000");
    dashboard_free(d);
}

static void test_export_json_widgets_array(void)
{
    TkDashboard *d = dashboard_new("JsonDash", 2);
    dashboard_add_stat(d, "s1", "Requests", "1234", "req/s");
    dashboard_add_gauge(d, "g1", "Disk", 60.0, 0.0, 100.0);
    const char *json = dashboard_export_json(d);
    ASSERT(json != NULL, "export_json: returns non-NULL");
    ASSERT_CONTAINS(json, "\"widgets\"",
                    "export_json: output contains \"widgets\" array key");
    ASSERT_CONTAINS(json, "\"theme\"",
                    "export_json: output contains \"theme\" key");
    dashboard_free(d);
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

int main(void)
{
#define RUN(fn) do { fn(); fflush(stdout); } while (0)
    RUN(test_new_returns_nonnull);
    RUN(test_empty_render_doctype);
    RUN(test_empty_render_title);
    RUN(test_empty_render_css_grid);
    RUN(test_chart_render_canvas);
    RUN(test_chart_render_cdn_url);
    RUN(test_chart_id_in_canvas);
    RUN(test_chart_title_in_h2);
    RUN(test_table_render_elements);
    RUN(test_table_title_in_h2);
    RUN(test_two_widgets_both_ids);
    RUN(test_update_changes_chart);
    RUN(test_render_always_nonnull);
    RUN(test_serve_function_exists);
    RUN(test_serve_null_dashboard);
    RUN(test_zero_widgets_render);
    RUN(test_render_valid_html_structure);
    /* Story 34.4.1 */
    RUN(test_stat_widget_render_nonnull);
    RUN(test_gauge_widget_export_json_contains_gauge);
    RUN(test_markdown_widget_export_json_contains_content);
    RUN(test_set_theme_dark_in_export_json);
    RUN(test_set_refresh_interval_in_export_json);
    RUN(test_export_json_widgets_array);
#undef RUN
    fflush(stdout);
    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}

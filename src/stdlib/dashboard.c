/*
 * dashboard.c — Implementation of the std.dashboard standard library module.
 *
 * Produces a self-contained HTML dashboard page using html.h and chart.h.
 * Widgets are arranged on a CSS grid; chart widgets use Chart.js via CDN.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 18.1.3
 */

#include "dashboard.h"
#include "router.h"
#include "strbuf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* -----------------------------------------------------------------------
 * Internal widget list
 * ----------------------------------------------------------------------- */

#define DASH_INITIAL_CAP 8

struct TkDashboard {
    char         *title;
    uint32_t      grid_cols;
    TkDashWidget *widgets;
    uint64_t      nwidgets;
    uint64_t      cap;
    /* Story 34.4.1 */
    char         *theme;            /* "light" or "dark", default "light" */
    uint64_t      refresh_interval_ms;
    char         *export_json_buf;  /* owned buffer for dashboard_export_json */
};

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

TkDashboard *dashboard_new(const char *title, uint32_t grid_cols)
{
    TkDashboard *d = malloc(sizeof(TkDashboard));
    if (!d) return NULL;

    d->title               = title ? strdup(title) : strdup("Dashboard");
    d->grid_cols           = grid_cols > 0 ? grid_cols : 1;
    d->widgets             = malloc(sizeof(TkDashWidget) * DASH_INITIAL_CAP);
    d->nwidgets            = 0;
    d->cap                 = DASH_INITIAL_CAP;
    d->theme               = strdup("light");
    d->refresh_interval_ms = 0;
    d->export_json_buf     = NULL;

    if (!d->title || !d->widgets || !d->theme) {
        free(d->title);
        free(d->widgets);
        free(d->theme);
        free(d);
        return NULL;
    }
    return d;
}

void dashboard_free(TkDashboard *d)
{
    if (!d) return;
    free(d->title);
    free(d->widgets);
    free(d->theme);
    free(d->export_json_buf);
    free(d);
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* Grow widget array if needed, return 0 on success, -1 on failure. */
static int ensure_cap(TkDashboard *d)
{
    if (d->nwidgets < d->cap) return 0;
    uint64_t new_cap = d->cap * 2;
    TkDashWidget *buf = realloc(d->widgets, sizeof(TkDashWidget) * new_cap);
    if (!buf) return -1;
    d->widgets = buf;
    d->cap     = new_cap;
    return 0;
}

/* Concatenate two strings; caller must free result. */
static char *strconcat2(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char *out = malloc(la + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

/* -----------------------------------------------------------------------
 * Widget registration
 * ----------------------------------------------------------------------- */

void dashboard_addchart(TkDashboard *d, const char *id, const char *title,
                         TkChartSpec *spec, uint32_t col, uint32_t row,
                         uint32_t colspan, uint32_t rowspan)
{
    if (!d || !id || !spec) return;
    if (ensure_cap(d) != 0) return;

    TkDashWidget *w  = &d->widgets[d->nwidgets++];
    w->type          = DASH_WIDGET_CHART;
    w->id            = id;
    w->title         = title ? title : "";
    w->col           = col;
    w->row           = row;
    w->colspan       = colspan > 0 ? colspan : 1;
    w->rowspan       = rowspan > 0 ? rowspan : 1;
    w->chart         = spec;
    w->table_headers = NULL;
    w->table_cells   = NULL;
    w->table_ncols   = 0;
    w->table_nrows   = 0;
}

void dashboard_addtable(TkDashboard *d, const char *id, const char *title,
                         const char **headers, uint64_t ncols,
                         const char **cells, uint64_t nrows,
                         uint32_t col, uint32_t row,
                         uint32_t colspan, uint32_t rowspan)
{
    if (!d || !id) return;
    if (ensure_cap(d) != 0) return;

    TkDashWidget *w  = &d->widgets[d->nwidgets++];
    w->type          = DASH_WIDGET_TABLE;
    w->id            = id;
    w->title         = title ? title : "";
    w->col           = col;
    w->row           = row;
    w->colspan       = colspan > 0 ? colspan : 1;
    w->rowspan       = rowspan > 0 ? rowspan : 1;
    w->chart         = NULL;
    w->table_headers = headers;
    w->table_cells   = cells;
    w->table_ncols   = ncols;
    w->table_nrows   = nrows;
}

/* -----------------------------------------------------------------------
 * Update
 * ----------------------------------------------------------------------- */

void dashboard_update(TkDashboard *d, const char *widget_id, TkChartSpec *new_spec)
{
    if (!d || !widget_id || !new_spec) return;
    for (uint64_t i = 0; i < d->nwidgets; i++) {
        if (d->widgets[i].type == DASH_WIDGET_CHART &&
            strcmp(d->widgets[i].id, widget_id) == 0) {
            d->widgets[i].chart = new_spec;
            return;
        }
    }
}

/* -----------------------------------------------------------------------
 * Render
 * ----------------------------------------------------------------------- */

/*
 * Build the class attribute value for a widget div, embedding the style
 * inline.  html_div(class_val, content) produces:
 *   <div class="CLASS_VAL">content</div>
 * We inject the style attribute by including a closing quote and extra
 * attribute in the class value string:
 *   class_val = "widget" style="grid-column:span N"
 * This gives: <div class="widget" style="grid-column:span N">
 *
 * Caller must free the returned string.
 */
static char *widget_class_attr(uint32_t colspan, uint32_t rowspan)
{
    char buf[256];
    if (rowspan > 1) {
        snprintf(buf, sizeof(buf),
                 "widget\" style=\"grid-column:span %u;grid-row:span %u",
                 colspan, rowspan);
    } else {
        snprintf(buf, sizeof(buf),
                 "widget\" style=\"grid-column:span %u",
                 colspan);
    }
    return strdup(buf);
}

const char *dashboard_render(TkDashboard *d)
{
    if (!d) return NULL;

    TkHtmlDoc *doc = html_doc();
    if (!doc) return NULL;

    /* 1. Title */
    html_title(doc, d->title);

    /* 2. CSS grid style */
    char css_buf[512];
    snprintf(css_buf, sizeof(css_buf),
             ".grid{"
             "display:grid;"
             "grid-template-columns:repeat(%u,1fr);"
             "gap:16px;"
             "padding:16px;"
             "}"
             ".widget{"
             "background:#fff;"
             "border-radius:8px;"
             "padding:16px;"
             "box-shadow:0 2px 4px rgba(0,0,0,0.1);"
             "}"
             ".widget h2{"
             "margin:0 0 8px;"
             "font-size:1rem;"
             "}",
             d->grid_cols);
    html_style(doc, css_buf);

    /* 3. Build inline script accumulator for Chart.js init calls.
     * The Chart.js CDN <script src> tag is appended to the body after
     * the widgets (step 6) using html_div with raw text content.
     *
     * 122.13: the widget markup was snprintf'd into char[512]/char[1024]
     * buffers (a long title or markdown body was cut mid-element) and the
     * script accumulator was re-concatenated per chart; all of it is
     * TkStrBuf now.  html_div() writes its content as markup, so every
     * caller string is escaped here for where it lands: titles, stat
     * values and markdown as HTML text, ids as HTML attributes, and the
     * chart id inside a JavaScript string. */
    TkStrBuf scripts = TK_STRBUF_INIT;   /* grows per chart widget */

    /* 4. Open grid wrapper div */
    TkHtmlNode *grid_div = html_div("grid", NULL);
    html_append(doc, grid_div);

    /* 5. Emit each widget appended directly to body.
     * The CSS grid layout positions them via the grid-column/row style
     * attributes encoded in the class value via widget_class_attr(). */
    for (uint64_t i = 0; i < d->nwidgets; i++) {
        TkDashWidget *w = &d->widgets[i];
        char *class_attr = widget_class_attr(w->colspan, w->rowspan);
        TkStrBuf in = TK_STRBUF_INIT;

        if (w->type == DASH_WIDGET_CHART) {
            /* Build: <h2>title</h2><canvas id="id"></canvas> */
            strbuf_append(&in, "<h2>");
            strbuf_append_xml(&in, w->title);
            strbuf_append(&in, "</h2><canvas id=\"");
            strbuf_append_xml(&in, w->id);
            strbuf_append(&in, "\"></canvas>");

            /* Accumulate Chart.js init: new Chart(getElementById('id'), JSON) */
            const char *chart_json = chart_tojson(w->chart);
            if (chart_json) {
                strbuf_append(&scripts, "new Chart(document.getElementById('");
                strbuf_append_js(&scripts, w->id);
                strbuf_append(&scripts, "'),");
                strbuf_append(&scripts, chart_json);
                strbuf_append(&scripts, ");\n");
            }

        } else if (w->type == DASH_WIDGET_STAT) {
            /* Stat widget: big number display */
            strbuf_append(&in, "<h2 id=\"");
            strbuf_append_xml(&in, w->id);
            strbuf_append(&in, "\">");
            strbuf_append_xml(&in, w->title);
            strbuf_append(&in, "</h2><p class=\"stat-value\">");
            strbuf_append_xml(&in, w->stat_value ? w->stat_value : "");
            strbuf_putc(&in, ' ');
            strbuf_append_xml(&in, w->stat_unit ? w->stat_unit : "");
            strbuf_append(&in, "</p>");

        } else if (w->type == DASH_WIDGET_GAUGE) {
            /* Gauge widget: arc gauge placeholder */
            strbuf_append(&in, "<h2 id=\"");
            strbuf_append_xml(&in, w->id);
            strbuf_append(&in, "\">");
            strbuf_append_xml(&in, w->title);
            strbuf_appendf(&in, "</h2>"
                     "<p class=\"gauge-value\">%.4g / %.4g (max: %.4g)</p>",
                     w->gauge_value, w->gauge_max, w->gauge_max);

        } else if (w->type == DASH_WIDGET_MARKDOWN) {
            /* Markdown widget: render content as preformatted text */
            strbuf_append(&in, "<span id=\"");
            strbuf_append_xml(&in, w->id);
            strbuf_append(&in, "\"></span><pre>");
            strbuf_append_xml(&in, w->markdown_content ? w->markdown_content : "");
            strbuf_append(&in, "</pre>");

        } else {
            /* Table widget */
            strbuf_append(&in, "<h2 id=\"");
            strbuf_append_xml(&in, w->id);
            strbuf_append(&in, "\">");
            strbuf_append_xml(&in, w->title);
            strbuf_append(&in, "</h2>");

            TkHtmlNode *tbl = html_table(w->table_headers, w->table_ncols,
                                          w->table_cells, w->table_nrows);
            char *tbl_str = tbl ? (char *)html_node_render(tbl) : NULL;
            strbuf_append(&in, tbl_str);
            free(tbl_str);
            if (tbl) html_node_free(tbl);
        }

        TkHtmlNode *wdiv = html_div(class_attr, strbuf_cstr(&in));
        html_append(doc, wdiv);
        strbuf_free(&in);
        free(class_attr);
    }

    /* 6. Append Chart.js CDN script tag as a raw body node.
     * html_div(NULL, raw) emits <div>raw</div>; the browser parses the
     * inner <script src="..."> correctly. */
    TkHtmlNode *cdn_node = html_div(
        NULL,
        "<script src=\"https://cdn.jsdelivr.net/npm/chart.js\"></script>");
    html_append(doc, cdn_node);

    /* 7. Append accumulated inline Chart.js init scripts */
    if (scripts.len > 0) {
        html_script(doc, strbuf_cstr(&scripts));
    }
    strbuf_free(&scripts);

    const char *rendered = html_render(doc);
    char *result = rendered ? strdup(rendered) : NULL;
    html_free(doc);
    return result;
}

/* -----------------------------------------------------------------------
 * Serve
 * ----------------------------------------------------------------------- */

/* File-scope pointer used to pass the rendered HTML to the route handler.
 * This is set by dashboard_serve before calling router_serve and remains
 * valid for the lifetime of the blocking serve call. */
static const char *s_serve_html;

static TkRouteResp dashboard_index_handler(TkRouteCtx ctx)
{
    (void)ctx;
    return router_resp_ok(s_serve_html, "text/html; charset=utf-8");
}

TkRouterErr dashboard_serve(TkDashboard *d, uint64_t port)
{
    TkRouterErr err = {0, NULL};

    if (!d) {
        err.failed = 1;
        err.msg    = "dashboard_serve: NULL dashboard";
        return err;
    }

    const char *html = dashboard_render(d);
    if (!html) {
        err.failed = 1;
        err.msg    = "dashboard_serve: render failed";
        return err;
    }

    s_serve_html = html;

    TkRouter *r = router_new();
    if (!r) {
        err.failed = 1;
        err.msg    = "dashboard_serve: router_new failed";
        return err;
    }

    router_get(r, "/", dashboard_index_handler);

    err = router_serve(r, "0.0.0.0", port);

    router_free(r);
    free((char *)s_serve_html);
    s_serve_html = NULL;
    return err;
}

/* -----------------------------------------------------------------------
 * Story 34.4.1: stat / gauge / markdown widgets, theming, JSON export
 * ----------------------------------------------------------------------- */

void dashboard_add_stat(TkDashboard *d, const char *id, const char *title,
                         const char *value, const char *unit)
{
    if (!d || !id) return;
    if (ensure_cap(d) != 0) return;

    TkDashWidget *w       = &d->widgets[d->nwidgets++];
    w->type               = DASH_WIDGET_STAT;
    w->id                 = id;
    w->title              = title ? title : "";
    w->col                = 0;
    w->row                = 0;
    w->colspan            = 1;
    w->rowspan            = 1;
    w->chart              = NULL;
    w->table_headers      = NULL;
    w->table_cells        = NULL;
    w->table_ncols        = 0;
    w->table_nrows        = 0;
    w->stat_value         = value ? value : "";
    w->stat_unit          = unit  ? unit  : "";
    w->gauge_value        = 0.0;
    w->gauge_min          = 0.0;
    w->gauge_max          = 0.0;
    w->markdown_content   = NULL;
}

void dashboard_add_gauge(TkDashboard *d, const char *id, const char *title,
                          double value, double min, double max)
{
    if (!d || !id) return;
    if (ensure_cap(d) != 0) return;

    TkDashWidget *w       = &d->widgets[d->nwidgets++];
    w->type               = DASH_WIDGET_GAUGE;
    w->id                 = id;
    w->title              = title ? title : "";
    w->col                = 0;
    w->row                = 0;
    w->colspan            = 1;
    w->rowspan            = 1;
    w->chart              = NULL;
    w->table_headers      = NULL;
    w->table_cells        = NULL;
    w->table_ncols        = 0;
    w->table_nrows        = 0;
    w->stat_value         = NULL;
    w->stat_unit          = NULL;
    w->gauge_value        = value;
    w->gauge_min          = min;
    w->gauge_max          = max;
    w->markdown_content   = NULL;
}

void dashboard_add_markdown(TkDashboard *d, const char *id, const char *content)
{
    if (!d || !id) return;
    if (ensure_cap(d) != 0) return;

    TkDashWidget *w       = &d->widgets[d->nwidgets++];
    w->type               = DASH_WIDGET_MARKDOWN;
    w->id                 = id;
    w->title              = "";
    w->col                = 0;
    w->row                = 0;
    w->colspan            = 1;
    w->rowspan            = 1;
    w->chart              = NULL;
    w->table_headers      = NULL;
    w->table_cells        = NULL;
    w->table_ncols        = 0;
    w->table_nrows        = 0;
    w->stat_value         = NULL;
    w->stat_unit          = NULL;
    w->gauge_value        = 0.0;
    w->gauge_min          = 0.0;
    w->gauge_max          = 0.0;
    w->markdown_content   = content ? content : "";
}

void dashboard_set_theme(TkDashboard *d, const char *theme)
{
    if (!d || !theme) return;
    free(d->theme);
    d->theme = strdup(theme);
}

void dashboard_set_refresh_interval(TkDashboard *d, uint64_t interval_ms)
{
    if (!d) return;
    d->refresh_interval_ms = interval_ms;
}

/* 122.13: export_json's private buffer (buf_append), its json_escape (which
 * passed control bytes other than \n \r \t through: invalid JSON) and the
 * per-field escaped copies are replaced by TkStrBuf + strbuf_append_json. */
static void dj_str(TkStrBuf *b, const char *key, const char *val)
{
    strbuf_putc(b, '"');
    strbuf_append(b, key);
    strbuf_append(b, "\":\"");
    strbuf_append_json(b, val ? val : "");
    strbuf_putc(b, '"');
}

/* A JSON number; NaN/Inf are not JSON, written as null. */
static void dj_num(TkStrBuf *b, const char *key, double v)
{
    strbuf_appendf(b, "\"%s\":", key);
    if (v == v && v - v == 0.0) strbuf_appendf(b, "%g", v);
    else strbuf_append(b, "null");
}

const char *dashboard_export_json(TkDashboard *d)
{
    if (!d) return NULL;

    TkStrBuf b = TK_STRBUF_INIT;
    strbuf_putc(&b, '{');
    dj_str(&b, "theme", d->theme ? d->theme : "light");
    strbuf_appendf(&b, ",\"refresh_interval_ms\":%llu,",
                   (unsigned long long)d->refresh_interval_ms);
    strbuf_append(&b, "\"widgets\":[");

    for (uint64_t i = 0; i < d->nwidgets; i++) {
        TkDashWidget *w = &d->widgets[i];
        if (i > 0) strbuf_putc(&b, ',');

        if (w->type == DASH_WIDGET_STAT) {
            strbuf_append(&b, "{\"type\":\"stat\",");
            dj_str(&b, "id", w->id);            strbuf_putc(&b, ',');
            dj_str(&b, "title", w->title);      strbuf_putc(&b, ',');
            dj_str(&b, "value", w->stat_value); strbuf_putc(&b, ',');
            dj_str(&b, "unit", w->stat_unit);
            strbuf_putc(&b, '}');
        } else if (w->type == DASH_WIDGET_GAUGE) {
            strbuf_append(&b, "{\"type\":\"gauge\",");
            dj_str(&b, "id", w->id);            strbuf_putc(&b, ',');
            dj_str(&b, "title", w->title);      strbuf_putc(&b, ',');
            dj_num(&b, "value", w->gauge_value); strbuf_putc(&b, ',');
            dj_num(&b, "min", w->gauge_min);     strbuf_putc(&b, ',');
            dj_num(&b, "max", w->gauge_max);
            strbuf_putc(&b, '}');
        } else if (w->type == DASH_WIDGET_MARKDOWN) {
            strbuf_append(&b, "{\"type\":\"markdown\",");
            dj_str(&b, "id", w->id);            strbuf_putc(&b, ',');
            dj_str(&b, "content", w->markdown_content);
            strbuf_putc(&b, '}');
        } else {
            strbuf_append(&b, w->type == DASH_WIDGET_CHART
                              ? "{\"type\":\"chart\"," : "{\"type\":\"table\",");
            dj_str(&b, "id", w->id);            strbuf_putc(&b, ',');
            dj_str(&b, "title", w->title);
            strbuf_putc(&b, '}');
        }
    }
    strbuf_append(&b, "]}");

    char *buf = strbuf_take(&b);
    if (!buf) return NULL;

    /* Store in dashboard so it lives long enough for caller to use */
    free(d->export_json_buf);
    d->export_json_buf = buf;
    return d->export_json_buf;
}

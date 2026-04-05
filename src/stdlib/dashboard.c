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
};

/* -----------------------------------------------------------------------
 * Lifecycle
 * ----------------------------------------------------------------------- */

TkDashboard *dashboard_new(const char *title, uint32_t grid_cols)
{
    TkDashboard *d = malloc(sizeof(TkDashboard));
    if (!d) return NULL;

    d->title      = title ? strdup(title) : strdup("Dashboard");
    d->grid_cols  = grid_cols > 0 ? grid_cols : 1;
    d->widgets    = malloc(sizeof(TkDashWidget) * DASH_INITIAL_CAP);
    d->nwidgets   = 0;
    d->cap        = DASH_INITIAL_CAP;

    if (!d->title || !d->widgets) {
        free(d->title);
        free(d->widgets);
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
     * the widgets (step 6) using html_div with raw text content. */
    char *inline_scripts = strdup("");   /* grows per chart widget */
    if (!inline_scripts) {
        html_free(doc);
        return NULL;
    }

    /* 4. Open grid wrapper div */
    TkHtmlNode *grid_div = html_div("grid", NULL);
    html_append(doc, grid_div);

    /* 5. Emit each widget appended directly to body.
     * The CSS grid layout positions them via the grid-column/row style
     * attributes encoded in the class value via widget_class_attr(). */
    for (uint64_t i = 0; i < d->nwidgets; i++) {
        TkDashWidget *w = &d->widgets[i];
        char *class_attr = widget_class_attr(w->colspan, w->rowspan);

        if (w->type == DASH_WIDGET_CHART) {
            /* Build: <h2>title</h2><canvas id="id"></canvas> */
            char inner[1024];
            snprintf(inner, sizeof(inner),
                     "<h2>%s</h2><canvas id=\"%s\"></canvas>",
                     w->title, w->id);

            TkHtmlNode *wdiv = html_div(class_attr, inner);
            html_append(doc, wdiv);

            /* Accumulate Chart.js init: new Chart(getElementById('id'), JSON) */
            const char *chart_json = chart_tojson(w->chart);
            if (chart_json) {
                /* new Chart(document.getElementById('id'),JSON);\n */
                size_t slen = strlen(w->id) + strlen(chart_json) + 64;
                char *stmt = malloc(slen);
                if (stmt) {
                    snprintf(stmt, slen,
                             "new Chart(document.getElementById('%s'),%s);\n",
                             w->id, chart_json);
                    char *combined = strconcat2(inline_scripts, stmt);
                    free(stmt);
                    if (combined) {
                        free(inline_scripts);
                        inline_scripts = combined;
                    }
                }
            }

        } else {
            /* Table widget */
            char h2_buf[512];
            snprintf(h2_buf, sizeof(h2_buf), "<h2>%s</h2>", w->title);

            TkHtmlNode *tbl = html_table(w->table_headers, w->table_ncols,
                                          w->table_cells, w->table_nrows);
            const char *tbl_str = tbl ? html_node_render(tbl) : "";
            char *inner = strconcat2(h2_buf, tbl_str ? tbl_str : "");

            TkHtmlNode *wdiv = html_div(class_attr, inner ? inner : h2_buf);
            html_append(doc, wdiv);

            free(inner);
            if (tbl) html_node_free(tbl);
        }

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
    if (inline_scripts && inline_scripts[0] != '\0') {
        html_script(doc, inline_scripts);
    }
    free(inline_scripts);

    const char *result = html_render(doc);
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
    return err;
}

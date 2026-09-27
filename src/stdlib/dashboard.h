#ifndef TK_STDLIB_DASHBOARD_H
#define TK_STDLIB_DASHBOARD_H

/*
 * dashboard.h — C interface for the std.dashboard standard library module.
 *
 * Type mappings:
 *   Str     = const char *  (null-terminated UTF-8)
 *   u32     = uint32_t
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * malloc is permitted: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 18.1.3, 34.4.1
 */

#include <stdint.h>
#include "chart.h"
#include "html.h"
#include "router.h"

typedef enum {
    DASH_WIDGET_CHART,
    DASH_WIDGET_TABLE,
    DASH_WIDGET_STAT,
    DASH_WIDGET_GAUGE,
    DASH_WIDGET_MARKDOWN
} DashWidgetType;

typedef struct {
    DashWidgetType  type;
    const char     *id;
    const char     *title;
    uint32_t        col;       /* grid column (0-indexed) */
    uint32_t        row;       /* grid row (0-indexed) */
    uint32_t        colspan;
    uint32_t        rowspan;
    /* widget data — only one is valid based on type */
    TkChartSpec    *chart;
    const char    **table_headers;
    const char    **table_cells;   /* flat row-major */
    uint64_t        table_ncols;
    uint64_t        table_nrows;
    /* stat widget */
    const char     *stat_value;
    const char     *stat_unit;
    /* gauge widget */
    double          gauge_value;
    double          gauge_min;
    double          gauge_max;
    /* markdown widget */
    const char     *markdown_content;
} TkDashWidget;

/*
 * The records toke sees (135.30): tkc reads `x.f` as 8-byte slot <index of
 * f in stdlib/dashboard.tki>.  A TkDashboard begins with a TkDashboardHead,
 * so a $dashboard's `.id` is a process-unique number (it read the title
 * pointer); dashboard.addchart returns a TkDashWidgetRec (it returned the
 * dashboard itself, so `w.id` read the dashboard's title and `w.spec` its
 * grid width as an array).  `make check-tki-layout` keeps them in step.
 */
/* tki-record: dashboard.dashboard */
typedef struct {
    int64_t     id;         /* u64, 1-based, unique per process */
} TkDashboardHead;

/* tki-record: dashboard.dashwidget */
typedef struct {
    const char *id;
    const char *type;       /* "chart", "table", ... */
    int64_t     spec;       /* [byte]: the widget's chart JSON */
} TkDashWidgetRec;

typedef struct TkDashboard TkDashboard;

TkDashboard *dashboard_new(const char *title, uint32_t grid_cols);
void         dashboard_free(TkDashboard *d);
void         dashboard_addchart(TkDashboard *d, const char *id, const char *title,
                                 TkChartSpec *spec, uint32_t col, uint32_t row,
                                 uint32_t colspan, uint32_t rowspan);
void         dashboard_addtable(TkDashboard *d, const char *id, const char *title,
                                 const char **headers, uint64_t ncols,
                                 const char **cells, uint64_t nrows,
                                 uint32_t col, uint32_t row,
                                 uint32_t colspan, uint32_t rowspan);
void         dashboard_update(TkDashboard *d, const char *widget_id, TkChartSpec *new_spec);
const char  *dashboard_render(TkDashboard *d);   /* full HTML string */

/* Render the dashboard to HTML and serve it on a single GET "/" route.
 * Binds to 0.0.0.0:port and blocks.  Returns TkRouterErr on failure. */
TkRouterErr  dashboard_serve(TkDashboard *d, uint64_t port);

/* Story 34.4.1: stat/gauge/markdown widgets, theming, JSON export */
void         dashboard_add_stat(TkDashboard *d, const char *id, const char *title,
                                 const char *value, const char *unit);
void         dashboard_add_gauge(TkDashboard *d, const char *id, const char *title,
                                  double value, double min, double max);
void         dashboard_add_markdown(TkDashboard *d, const char *id, const char *content);
void         dashboard_set_theme(TkDashboard *d, const char *theme); /* "dark" or "light" */
void         dashboard_set_refresh_interval(TkDashboard *d, uint64_t interval_ms);
const char  *dashboard_export_json(TkDashboard *d);

#endif /* TK_STDLIB_DASHBOARD_H */

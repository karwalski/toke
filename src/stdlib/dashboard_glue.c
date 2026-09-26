/*
 * dashboard_glue.c — i64-ABI wrappers for std.dashboard (Story 136.40).
 *
 * Moved verbatim out of tk_web_glue.c, which src/stdlib_deps.c registers
 * against the HTTP module. dashboard.c and dashboard.h have carried
 * dashboard_new/addchart/serve all along, but with the wrappers living in
 * HTTP's glue file a program importing only std.dashboard type-checked and
 * then failed at link on tk_dashboard_new_w, tk_dashboard_addchart_w and
 * tk_dashboard_serve_w -- the single docs/stdlib/dashboard.md example.
 * Story 136.33 diagnosed the misregistration; this is the extraction for the
 * dashboard half of it, in the shape 136.32 used for yaml and 136.25 for ml.
 *
 * No behaviour is changed: the three bodies are byte-for-byte what
 * tk_web_glue.c held.
 */
#include "dashboard.h"
#include "chart.h"
#include "router.h"
#include <stdint.h>

/* ── dashboard wrappers (dashboard.h) ─────────────────────────────── */
/* 136.70: dashboard.new(title:str; cols:u64) -- the documented signature and
 * dashboard_new()'s own.  The wrapper took the title alone and hard-coded
 * 12 grid columns, so the documented `dashboard.new("Metrics"; 2)` was an
 * arity error.  dashboard_new() clamps cols 0 to 1. */
int64_t tk_dashboard_new_w(int64_t title, int64_t cols) {
    const char *t = title ? (const char *)(intptr_t)title : "Dashboard";
    uint32_t n = (cols < 0 || cols > 0xFFFF) ? 0u : (uint32_t)cols;
    TkDashboard *d = dashboard_new(t, n);
    return (int64_t)(intptr_t)d;
}

/* 136.70: addchart(d; id; spec), as documented and as dashboard_addchart()
 * takes it; the wrapper dropped the widget id. */
int64_t tk_dashboard_addchart_w(int64_t dash, int64_t id, int64_t chart) {
    if (!dash || !chart) return 0;
    dashboard_addchart((TkDashboard *)(intptr_t)dash,
                        id ? (const char *)(intptr_t)id : NULL, NULL,
                        (TkChartSpec *)(intptr_t)chart, 0, 0, 1, 1);
    return dash;
}

int64_t tk_dashboard_serve_w(int64_t dash, int64_t port) {
    if (!dash) return -1;
    TkRouterErr err = dashboard_serve((TkDashboard *)(intptr_t)dash,
                                       (uint64_t)port);
    return err.failed ? -1 : 0;
}

/* ── chart.addchart / chart.serve (136.33) ────────────────────────────
 * chart.* names with dashboard_* bodies. 136.22 left them in tk_web_glue.c
 * (registered under std.http) so a plain chart import would not drag in the
 * dashboard link set -- which meant `i=chart:std.chart` calling either one
 * failed at link unless the program also imported std.http. They live with
 * the dashboard code they call, and src/stdlib_deps.c lists dashboard as a
 * dependency of chart. Bodies unchanged. */
int64_t tk_chart_addchart_w(int64_t dash, int64_t chart) {
    if (!dash || !chart) return 0;
    dashboard_addchart((TkDashboard *)(intptr_t)dash, NULL, NULL,
                        (TkChartSpec *)(intptr_t)chart, 0, 0, 1, 1);
    return dash;
}

int64_t tk_chart_serve_w(int64_t dash, int64_t port) {
    if (!dash) return -1;
    TkRouterErr err = dashboard_serve((TkDashboard *)(intptr_t)dash,
                                       (uint64_t)port);
    return err.failed ? -1 : 0;
}

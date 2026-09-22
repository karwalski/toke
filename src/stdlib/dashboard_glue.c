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
int64_t tk_dashboard_new_w(int64_t title) {
    const char *t = title ? (const char *)(intptr_t)title : "Dashboard";
    TkDashboard *d = dashboard_new(t, 12);
    return (int64_t)(intptr_t)d;
}

int64_t tk_dashboard_addchart_w(int64_t dash, int64_t chart) {
    if (!dash || !chart) return 0;
    dashboard_addchart((TkDashboard *)(intptr_t)dash, NULL, NULL,
                        (TkChartSpec *)(intptr_t)chart, 0, 0, 1, 1);
    return dash;
}

int64_t tk_dashboard_serve_w(int64_t dash, int64_t port) {
    if (!dash) return -1;
    TkRouterErr err = dashboard_serve((TkDashboard *)(intptr_t)dash,
                                       (uint64_t)port);
    return err.failed ? -1 : 0;
}

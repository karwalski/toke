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
 * Story: 18.1.3
 */

#include <stdint.h>
#include "chart.h"
#include "html.h"

typedef enum { DASH_WIDGET_CHART, DASH_WIDGET_TABLE } DashWidgetType;

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
} TkDashWidget;

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

#endif /* TK_STDLIB_DASHBOARD_H */

#ifndef TK_STDLIB_CHART_H
#define TK_STDLIB_CHART_H

/*
 * chart.h — C interface for the std.chart standard library module.
 *
 * Type mappings:
 *   [Str]      = StrArray   (defined in str.h)
 *   [f64]      = F64Array
 *   Str        = const char *  (null-terminated UTF-8)
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * Story: 18.1.1
 */

#include <stdint.h>

#ifndef TK_STRARRAY_DEFINED
#define TK_STRARRAY_DEFINED
typedef struct { const char **data; uint64_t len; } StrArray;
#endif

#ifndef TK_F64ARRAY_DEFINED
#define TK_F64ARRAY_DEFINED
typedef struct { const double *data; uint64_t len; } F64Array;
#endif

typedef enum { CHART_BAR, CHART_LINE, CHART_SCATTER, CHART_PIE } ChartType;

typedef struct {
    const char *label;
    const double *values;
    uint64_t nvalues;
    const char *color;   /* CSS color string, may be NULL */
} TkDataset;

typedef struct {
    ChartType    type;
    const char  *title;
    const char **labels;     /* x-axis labels */
    uint64_t     nlabels;
    TkDataset   *datasets;
    uint64_t     ndatasets;
} TkChartSpec;

/* chart.bar(labels:[Str]; datasets:[TkDataset]; title:Str) : TkChartSpec*
 * Returns a heap-allocated TkChartSpec for a bar chart.
 * Caller owns the returned pointer; use chart_free() to release. */
TkChartSpec *chart_bar(StrArray labels, TkDataset *datasets, uint64_t nds, const char *title);

/* chart.line(labels:[Str]; datasets:[TkDataset]; title:Str) : TkChartSpec*
 * Returns a heap-allocated TkChartSpec for a line chart. */
TkChartSpec *chart_line(StrArray labels, TkDataset *datasets, uint64_t nds, const char *title);

/* chart.scatter(datasets:[TkDataset]; title:Str) : TkChartSpec*
 * Returns a heap-allocated TkChartSpec for a scatter chart (no x labels). */
TkChartSpec *chart_scatter(TkDataset *datasets, uint64_t nds, const char *title);

/* chart.pie(labels:[Str]; ds:TkDataset*; title:Str) : TkChartSpec*
 * Returns a heap-allocated TkChartSpec for a pie chart (single dataset). */
TkChartSpec *chart_pie(StrArray labels, TkDataset *ds, const char *title);

/* chart.tojson(spec:TkChartSpec*) : Str
 * Serialises the chart spec to Chart.js v4-compatible JSON.
 * Returns a heap-allocated null-terminated string; caller owns the pointer. */
const char  *chart_tojson(TkChartSpec *spec);

/* chart.tovega(spec:TkChartSpec*) : Str
 * Serialises the chart spec to Vega-Lite v5-compatible JSON.
 * Returns a heap-allocated null-terminated string; caller owns the pointer. */
const char  *chart_tovega(TkChartSpec *spec);

/* chart_free(spec) — release a TkChartSpec and its owned sub-allocations. */
void         chart_free(TkChartSpec *spec);

#endif /* TK_STDLIB_CHART_H */

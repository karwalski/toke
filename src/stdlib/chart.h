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

/* -----------------------------------------------------------------------
 * Story 34.2.1: Additional chart types and configuration helpers.
 * All functions return heap-allocated Chart.js JSON strings (or SVG/HTML
 * for heatmap).  Callers own the returned pointer and must free() it.
 * ----------------------------------------------------------------------- */

/* chart_stacked_bar — type="bar" with stacked scales.
 *   labels[nlabels]          x-axis category labels
 *   series_names[nseries]    one name per dataset row
 *   data[nseries][nlabels]   values; row-major via pointer-to-pointer
 *   title                    chart title (may be NULL)
 */
const char *chart_stacked_bar(const char *const *labels, uint64_t nlabels,
                               const char *const *series_names, uint64_t nseries,
                               const double *const *data,
                               const char *title);

/* chart_horizontal_bar — type="bar" with indexAxis="y". */
const char *chart_horizontal_bar(const char *const *labels, uint64_t n,
                                  const double *values, const char *title);

/* chart_area — type="line" with fill=true. */
const char *chart_area(const double *xs, const double *ys, uint64_t n,
                        const char *title);

/* chart_radar — type="radar". */
const char *chart_radar(const char *const *axes, uint64_t naxes,
                         const double *values, const char *title);

/* chart_histogram — compute nbins equal-width bins from values[n], render
 *                   as a bar chart with bin-midpoint labels. */
const char *chart_histogram(const double *values, uint64_t n,
                              uint64_t nbins, const char *title);

/* chart_heatmap — returns an SVG string (no native Chart.js heatmap).
 *   matrix is row-major: element [r][c] = matrix[r * ncols + c]. */
const char *chart_heatmap(const char *const *rows, uint64_t nrows,
                            const char *const *cols, uint64_t ncols,
                            const double *matrix,
                            const char *title);

/* chart_set_theme — parse existing Chart.js JSON spec and inject dark/light
 *                   theme colours into options.  Returns a new heap string. */
const char *chart_set_theme(const char *spec, const char *theme);

/* chart_set_legend — add/modify options.plugins.legend in the spec JSON. */
const char *chart_set_legend(const char *spec, const char *position, int display);

/* chart_set_tooltip — add/modify options.plugins.tooltip.callbacks with the
 *                     given field names.  Returns a new heap string. */
const char *chart_set_tooltip(const char *spec, const char *const *fields,
                               uint64_t n);

#endif /* TK_STDLIB_CHART_H */

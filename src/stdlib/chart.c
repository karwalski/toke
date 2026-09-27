/*
 * chart.c — Implementation of the std.chart standard library module.
 *
 * Produces Chart.js v4 JSON (chart_tojson) and Vega-Lite v5 JSON
 * (chart_tovega) from a TkChartSpec.  Uses a simple growable string
 * builder backed by malloc/realloc; no external JSON library.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own the returned strings.
 *
 * Story: 18.1.1
 * Story: 34.2.1 — additional chart types and configuration helpers
 */

#include "chart.h"
#include "strbuf.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <float.h>
#include <limits.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Default Chart.js colour palette (6 colours, cycled when color==NULL).
 * ----------------------------------------------------------------------- */
static const char *DEFAULT_COLORS[] = {
    "rgba(54,162,235,0.5)",
    "rgba(255,99,132,0.5)",
    "rgba(75,192,192,0.5)",
    "rgba(255,206,86,0.5)",
    "rgba(153,102,255,0.5)",
    "rgba(255,159,64,0.5)"
};
#define NDEFAULT_COLORS 6

/* -----------------------------------------------------------------------
 * Output buffer: the shared TkStrBuf (122.13; chart.c's private SB was the
 * last copy of the class 122.2 collapsed).  Appends are unchecked because
 * an allocation failure is sticky and strbuf_take() then returns NULL, so a
 * partial chart is never returned as if whole.
 * ----------------------------------------------------------------------- */

/* A JSON string, quoted.  122.13: control bytes other than \n \r \t were
 * copied raw, which is invalid JSON; NULL (a missing label) crashed. */
static void sb_json_str(TkStrBuf *sb, const char *s)
{
    strbuf_putc(sb, '"');
    strbuf_append_json(sb, s);
    strbuf_putc(sb, '"');
}

/* isfinite() without <math.h> (a local math.h can shadow it under
 * -Isrc/stdlib, see svg.c): NaN != NaN, and Inf - Inf is NaN. */
static int chart_finite(double v) { return v == v && v - v == 0.0; }

/* A JSON number.  122.13: NaN and +-Inf printed as "nan"/"inf", which is
 * not JSON; they are written as null (Chart.js and Vega-Lite both read a
 * null datum as a gap). */
static void sb_double(TkStrBuf *sb, double v)
{
    if (!chart_finite(v)) { strbuf_append(sb, "null"); return; }
    strbuf_appendf(sb, "%.10g", v);
}

#define A(s)  strbuf_append(&sb, (s))
#define AJ(s) sb_json_str(&sb, (s))
#define AD(v) sb_double(&sb, (v))

/* -----------------------------------------------------------------------
 * ChartType → string helpers.
 * ----------------------------------------------------------------------- */
static const char *chart_type_str(ChartType t)
{
    switch (t) {
        case CHART_BAR:     return "bar";
        case CHART_LINE:    return "line";
        case CHART_SCATTER: return "scatter";
        case CHART_PIE:     return "pie";
    }
    return "bar";
}

/* Vega-Lite mark name. */
static const char *vega_mark_str(ChartType t)
{
    switch (t) {
        case CHART_BAR:     return "bar";
        case CHART_LINE:    return "line";
        case CHART_SCATTER: return "point";
        case CHART_PIE:     return "arc";
    }
    return "bar";
}

/* -----------------------------------------------------------------------
 * Internal spec builder (shared by all chart_* constructors).
 * ----------------------------------------------------------------------- */
static TkChartSpec *make_spec(ChartType type,
                              const char **labels, uint64_t nlabels,
                              TkDataset *datasets, uint64_t ndatasets,
                              const char *title)
{
    TkChartSpec *spec = (TkChartSpec *)malloc(sizeof(TkChartSpec));
    if (!spec) return NULL;
    spec->type      = type;
    spec->title     = title;   /* not owned; caller controls lifetime */
    spec->labels    = labels;
    spec->nlabels   = nlabels;
    spec->datasets  = datasets;
    spec->ndatasets = ndatasets;
    return spec;
}

/* -----------------------------------------------------------------------
 * Public constructors.
 * ----------------------------------------------------------------------- */
TkChartSpec *chart_bar(StrArray labels, TkDataset *datasets, uint64_t nds,
                       const char *title)
{
    return make_spec(CHART_BAR, labels.data, labels.len, datasets, nds, title);
}

TkChartSpec *chart_line(StrArray labels, TkDataset *datasets, uint64_t nds,
                        const char *title)
{
    return make_spec(CHART_LINE, labels.data, labels.len, datasets, nds, title);
}

TkChartSpec *chart_scatter(TkDataset *datasets, uint64_t nds, const char *title)
{
    return make_spec(CHART_SCATTER, NULL, 0, datasets, nds, title);
}

TkChartSpec *chart_pie(StrArray labels, TkDataset *ds, const char *title)
{
    /* pie takes a single dataset passed by pointer; wrap in array for spec */
    return make_spec(CHART_PIE, labels.data, labels.len, ds, ds ? 1 : 0, title);
}

/* -----------------------------------------------------------------------
 * chart_free — release spec allocation (datasets/labels are caller-owned).
 * ----------------------------------------------------------------------- */
void chart_free(TkChartSpec *spec)
{
    free(spec);
}

/* -----------------------------------------------------------------------
 * chart_tojson — Chart.js v4 JSON serialisation.
 *
 * Output structure:
 * {
 *   "type": "bar",
 *   "data": {
 *     "labels": ["A","B"],
 *     "datasets": [{"label":"Sales","data":[1,2],"backgroundColor":"..."}]
 *   },
 *   "options": {"plugins":{"title":{"display":true,"text":"My Chart"}}}
 * }
 * ----------------------------------------------------------------------- */
const char *chart_tojson(TkChartSpec *spec)
{
    if (!spec) return NULL;

    TkStrBuf sb = TK_STRBUF_INIT;


    A("{");
    A("\"type\":");
    AJ(chart_type_str(spec->type));

    /* data */
    A(",\"data\":{");

    /* labels array */
    A("\"labels\":[");
    for (uint64_t i = 0; i < spec->nlabels; i++) {
        if (i > 0) A(",");
        AJ(spec->labels[i]);
    }
    A("]");

    /* datasets array */
    A(",\"datasets\":[");
    for (uint64_t d = 0; d < spec->ndatasets; d++) {
        if (d > 0) A(",");
        TkDataset *ds = &spec->datasets[d];
        A("{");
        A("\"label\":");
        AJ(ds->label ? ds->label : "");

        A(",\"data\":[");
        for (uint64_t v = 0; v < ds->nvalues; v++) {
            if (v > 0) A(",");
            AD(ds->values[v]);
        }
        A("]");

        /* colour */
        const char *color = ds->color
            ? ds->color
            : DEFAULT_COLORS[d % NDEFAULT_COLORS];
        A(",\"backgroundColor\":");
        AJ(color);

        A("}");
    }
    A("]");   /* end datasets */
    A("}");   /* end data */

    /* options / title */
    A(",\"options\":{\"plugins\":{\"title\":{");
    if (spec->title) {
        A("\"display\":true,\"text\":");
        AJ(spec->title);
    } else {
        A("\"display\":false");
    }
    A("}}}");   /* end title / plugins / options */

    A("}");   /* end root */


    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_tovega — Vega-Lite v5 JSON serialisation.
 *
 * For multi-dataset charts the datasets are flattened into a single
 * values array of {"label":…,"value":…,"series":…} objects, which is the
 * conventional Vega-Lite tidy-data form.
 *
 * Output structure:
 * {
 *   "$schema": "https://vega.github.io/schema/vega-lite/v5.json",
 *   "mark": "bar",
 *   "data": {"values": [{"label":"A","value":1,"series":"Sales"}, ...]},
 *   "encoding": {
 *     "x": {"field":"label","type":"nominal"},
 *     "y": {"field":"value","type":"quantitative"},
 *     "color": {"field":"series","type":"nominal"}
 *   },
 *   "title": "My Chart"
 * }
 * ----------------------------------------------------------------------- */
const char *chart_tovega(TkChartSpec *spec)
{
    if (!spec) return NULL;

    TkStrBuf sb = TK_STRBUF_INIT;


    A("{");
    A("\"$schema\":\"https://vega.github.io/schema/vega-lite/v5.json\"");
    A(",\"mark\":");
    AJ(vega_mark_str(spec->type));

    /* data — tidy values array */
    A(",\"data\":{\"values\":[");
    int first_row = 1;
    for (uint64_t d = 0; d < spec->ndatasets; d++) {
        TkDataset *ds = &spec->datasets[d];
        for (uint64_t v = 0; v < ds->nvalues; v++) {
            if (!first_row) A(",");
            first_row = 0;
            A("{");
            A("\"label\":");
            /* use x-axis label if available, else index */
            if (spec->labels && v < spec->nlabels) {
                AJ(spec->labels[v]);
            } else {
                char idx[24];
                snprintf(idx, sizeof(idx), "%llu", (unsigned long long)v);
                AJ(idx);
            }
            A(",\"value\":");
            AD(ds->values[v]);
            A(",\"series\":");
            AJ(ds->label ? ds->label : "");
            A("}");
        }
    }
    A("]}");   /* end data */

    /* encoding */
    A(",\"encoding\":{");
    A("\"x\":{\"field\":\"label\",\"type\":\"nominal\"}");
    A(",\"y\":{\"field\":\"value\",\"type\":\"quantitative\"}");
    A(",\"color\":{\"field\":\"series\",\"type\":\"nominal\"}");
    A("}");   /* end encoding */

    /* title (optional) */
    if (spec->title) {
        A(",\"title\":");
        AJ(spec->title);
    }

    A("}");   /* end root */


    return strbuf_take(&sb);
}

/* =======================================================================
 * Story 34.2.1 — Additional chart types and configuration helpers.
 * ======================================================================= */

/* Convenience macros used throughout this section; undefined at section end. */

/* -----------------------------------------------------------------------
 * chart_stacked_bar
 * ----------------------------------------------------------------------- */
const char *chart_stacked_bar(const char *const *labels, uint64_t nlabels,
                               const char *const *series_names, uint64_t nseries,
                               const double *const *data,
                               const char *title)
{
    TkStrBuf sb = TK_STRBUF_INIT;

    A("{\"type\":\"bar\"");

    /* data */
    A(",\"data\":{\"labels\":[");
    for (uint64_t i = 0; i < nlabels; i++) {
        if (i > 0) A(",");
        AJ(labels[i]);
    }
    A("],\"datasets\":[");
    for (uint64_t s = 0; s < nseries; s++) {
        if (s > 0) A(",");
        A("{\"label\":");
        AJ(series_names[s]);
        A(",\"data\":[");
        for (uint64_t j = 0; j < nlabels; j++) {
            if (j > 0) A(",");
            AD(data[s][j]);
        }
        A("],\"backgroundColor\":");
        AJ(DEFAULT_COLORS[s % NDEFAULT_COLORS]);
        A("}");
    }
    A("]}");  /* end datasets / data */

    /* options with stacked scales */
    A(",\"options\":{");
    A("\"scales\":{");
    A("\"x\":{\"stacked\":true}");
    A(",\"y\":{\"stacked\":true}");
    A("}");
    A(",\"plugins\":{\"title\":{");
    if (title) {
        A("\"display\":true,\"text\":");
        AJ(title);
    } else {
        A("\"display\":false");
    }
    A("}}}");  /* end title / plugins / options */

    A("}");
    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_horizontal_bar
 * ----------------------------------------------------------------------- */
const char *chart_horizontal_bar(const char *const *labels, uint64_t n,
                                  const double *values, const char *title)
{
    TkStrBuf sb = TK_STRBUF_INIT;

    A("{\"type\":\"bar\"");
    A(",\"data\":{\"labels\":[");
    for (uint64_t i = 0; i < n; i++) {
        if (i > 0) A(",");
        AJ(labels[i]);
    }
    A("],\"datasets\":[{\"label\":");
    AJ(title ? title : "");
    A(",\"data\":[");
    for (uint64_t i = 0; i < n; i++) {
        if (i > 0) A(",");
        AD(values[i]);
    }
    A("],\"backgroundColor\":");
    AJ(DEFAULT_COLORS[0]);
    A("}]}");  /* end dataset / datasets / data */

    A(",\"options\":{\"indexAxis\":\"y\"");
    A(",\"plugins\":{\"title\":{");
    if (title) {
        A("\"display\":true,\"text\":");
        AJ(title);
    } else {
        A("\"display\":false");
    }
    A("}}}");  /* end title / plugins / options */

    A("}");
    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_area
 * ----------------------------------------------------------------------- */
const char *chart_area(const double *xs, const double *ys, uint64_t n,
                        const char *title)
{
    TkStrBuf sb = TK_STRBUF_INIT;

    A("{\"type\":\"line\"");

    /* labels = x values */
    A(",\"data\":{\"labels\":[");
    for (uint64_t i = 0; i < n; i++) {
        if (i > 0) A(",");
        AD(xs[i]);
    }
    A("],\"datasets\":[{\"label\":");
    AJ(title ? title : "");
    A(",\"data\":[");
    for (uint64_t i = 0; i < n; i++) {
        if (i > 0) A(",");
        AD(ys[i]);
    }
    A("],\"fill\":true");
    A(",\"backgroundColor\":");
    AJ(DEFAULT_COLORS[0]);
    A("}]}");  /* end dataset / datasets / data */

    A(",\"options\":{\"plugins\":{\"title\":{");
    if (title) {
        A("\"display\":true,\"text\":");
        AJ(title);
    } else {
        A("\"display\":false");
    }
    A("}}}");

    A("}");
    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_radar
 * ----------------------------------------------------------------------- */
const char *chart_radar(const char *const *axes, uint64_t naxes,
                         const double *values, const char *title)
{
    TkStrBuf sb = TK_STRBUF_INIT;

    A("{\"type\":\"radar\"");

    A(",\"data\":{\"labels\":[");
    for (uint64_t i = 0; i < naxes; i++) {
        if (i > 0) A(",");
        AJ(axes[i]);
    }
    A("],\"datasets\":[{\"label\":");
    AJ(title ? title : "");
    A(",\"data\":[");
    for (uint64_t i = 0; i < naxes; i++) {
        if (i > 0) A(",");
        AD(values[i]);
    }
    A("],\"backgroundColor\":");
    AJ(DEFAULT_COLORS[0]);
    A("}]}");

    A(",\"options\":{\"plugins\":{\"title\":{");
    if (title) {
        A("\"display\":true,\"text\":");
        AJ(title);
    } else {
        A("\"display\":false");
    }
    A("}}}");

    A("}");
    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_histogram — compute equal-width bins, then emit as bar chart.
 * ----------------------------------------------------------------------- */
const char *chart_histogram(const double *values, uint64_t n,
                              uint64_t nbins, const char *title)
{
    if (nbins == 0 || n == 0) nbins = 1;

    /* Find min / max over the FINITE values.  122.13 / 135.3j: this read
     * values[0] even when n == 0 (past the input), and NaN or +-Inf reached
     * the (uint64_t) conversion below, which is undefined behaviour.
     * Non-finite values are not counted in any bin; with no finite value
     * at all the chart is one empty bin at 0, the same as n == 0. */
    int have = 0;
    double vmin = 0.0, vmax = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        double v = values[i];
        if (!chart_finite(v)) continue;
        if (!have) { vmin = vmax = v; have = 1; continue; }
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    /* Guard against zero range (and a range that overflows to Inf) */
    double range = vmax - vmin;
    if (range == 0.0 || !chart_finite(range)) range = range == 0.0 ? 1.0 : DBL_MAX;

    double bin_width = range / (double)nbins;

    /* Count per bin (heap-allocated) */
    uint64_t *counts = (uint64_t *)calloc(nbins, sizeof(uint64_t));
    if (!counts) return NULL;

    for (uint64_t i = 0; i < n; i++) {
        if (!chart_finite(values[i])) continue;
        double q = (values[i] - vmin) / bin_width;   /* >= 0, finite */
        uint64_t idx = (q >= (double)nbins) ? nbins - 1 : (uint64_t)q;
        counts[idx]++;
    }

    TkStrBuf sb = TK_STRBUF_INIT;

    A("{\"type\":\"bar\"");

    /* labels = bin midpoints */
    A(",\"data\":{\"labels\":[");
    for (uint64_t b = 0; b < nbins; b++) {
        if (b > 0) A(",");
        double mid = vmin + bin_width * (double)b + bin_width * 0.5;
        AD(mid);
    }
    A("],\"datasets\":[{\"label\":");
    AJ(title ? title : "");
    A(",\"data\":[");
    for (uint64_t b = 0; b < nbins; b++) {
        char tmp[32];
        if (b > 0) A(",");
        snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)counts[b]);
        A(tmp);
    }
    free(counts);
    counts = NULL;
    A("],\"backgroundColor\":");
    AJ(DEFAULT_COLORS[0]);
    A("}]}");

    A(",\"options\":{\"plugins\":{\"title\":{");
    if (title) {
        A("\"display\":true,\"text\":");
        AJ(title);
    } else {
        A("\"display\":false");
    }
    A("}}}");

    A("}");
    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_heatmap — SVG-based matrix visualisation (no native CJS heatmap).
 *
 * Layout:
 *   - Each cell is CELL_W x CELL_H pixels.
 *   - Colour interpolates from white (min) to steelblue (max).
 *   - Row/column labels sit outside the grid.
 * ----------------------------------------------------------------------- */
const char *chart_heatmap(const char *const *rows, uint64_t nrows,
                            const char *const *cols, uint64_t ncols,
                            const double *matrix,
                            const char *title)
{
    if (nrows == 0 || ncols == 0 || !matrix) return NULL;

    const int CELL_W  = 60;
    const int CELL_H  = 40;
    const int LPAD    = 80;   /* left padding for row labels */
    const int TPAD    = 60;   /* top padding for col labels + title */

    /* 122.13: svg_w/svg_h and every cell coordinate are int, computed as
     * (int)ncols * CELL_W -- signed overflow (undefined behaviour) once
     * ncols passes ~35.8M, and nrows * ncols could wrap size_t.  Refuse
     * any size whose pixel extent does not fit an int. */
    if (ncols > (uint64_t)(INT_MAX - LPAD - 10) / (uint64_t)CELL_W ||
        nrows > (uint64_t)(INT_MAX - TPAD - 10) / (uint64_t)CELL_H ||
        nrows > SIZE_MAX / sizeof(double) / ncols)
        return NULL;

    /* Find min / max for colour normalisation, over the finite cells.
     * 122.13: a NaN cell made t NaN and (int)NaN is undefined behaviour;
     * non-finite cells are drawn in grey, labelled "n/a". */
    int have = 0;
    double vmin = 0.0, vmax = 0.0;
    for (uint64_t i = 0; i < nrows * ncols; i++) {
        double v = matrix[i];
        if (!chart_finite(v)) continue;
        if (!have) { vmin = vmax = v; have = 1; continue; }
        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }
    double range = vmax - vmin;
    if (range == 0.0 || !chart_finite(range)) range = range == 0.0 ? 1.0 : DBL_MAX;

    int svg_w = LPAD + (int)ncols * CELL_W + 10;
    int svg_h = TPAD + (int)nrows * CELL_H + 10;

    TkStrBuf sb = TK_STRBUF_INIT;

    /* SVG open */
    strbuf_appendf(&sb,
             "<svg xmlns=\"http://www.w3.org/2000/svg\" "
             "width=\"%d\" height=\"%d\">", svg_w, svg_h);

    /* Title and labels are XML text.  122.13: they were written raw (a
     * title "</text><script>..." injected markup) and through fixed
     * char[256]/char[512] buffers, so a long label was cut mid-element. */
    if (title) {
        strbuf_appendf(&sb,
                 "<text x=\"%d\" y=\"20\" text-anchor=\"middle\" "
                 "font-size=\"14\" font-weight=\"bold\">", svg_w / 2);
        strbuf_append_xml(&sb, title);
        A("</text>");
    }

    /* Column labels */
    for (uint64_t c = 0; c < ncols; c++) {
        int cx = LPAD + (int)c * CELL_W + CELL_W / 2;
        strbuf_appendf(&sb,
                 "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" "
                 "font-size=\"11\">", cx, TPAD - 5);
        strbuf_append_xml(&sb, cols ? cols[c] : NULL);
        A("</text>");
    }

    /* Row labels + cells */
    for (uint64_t r = 0; r < nrows; r++) {
        int cy = TPAD + (int)r * CELL_H;

        /* Row label */
        strbuf_appendf(&sb,
                 "<text x=\"%d\" y=\"%d\" text-anchor=\"end\" "
                 "font-size=\"11\">", LPAD - 5, cy + CELL_H / 2 + 4);
        strbuf_append_xml(&sb, rows ? rows[r] : NULL);
        A("</text>");

        for (uint64_t c = 0; c < ncols; c++) {
            double v   = matrix[r * ncols + c];
            int cx = LPAD + (int)c * CELL_W;
            if (!chart_finite(v)) {
                strbuf_appendf(&sb,
                         "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                         "fill=\"rgb(204,204,204)\" stroke=\"#ccc\"/>"
                         "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" "
                         "font-size=\"10\">n/a</text>",
                         cx, cy, CELL_W, CELL_H,
                         cx + CELL_W / 2, cy + CELL_H / 2 + 4);
                continue;
            }
            double t   = (v - vmin) / range;   /* 0..1 */
            if (!(t >= 0.0)) t = 0.0;
            if (t > 1.0) t = 1.0;
            /* Interpolate white->steelblue (70,130,180) */
            int red   = (int)(255.0 * (1.0 - t) + 70.0  * t);
            int green = (int)(255.0 * (1.0 - t) + 130.0 * t);
            int blue  = (int)(255.0 * (1.0 - t) + 180.0 * t);

            strbuf_appendf(&sb,
                     "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                     "fill=\"rgb(%d,%d,%d)\" stroke=\"#ccc\"/>"
                     "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" "
                     "font-size=\"10\">%.2f</text>",
                     cx, cy, CELL_W, CELL_H,
                     red, green, blue,
                     cx + CELL_W / 2, cy + CELL_H / 2 + 4,
                     v);
        }
    }

    A("</svg>");
    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_set_theme — inject dark/light colour overrides into an existing
 *                   Chart.js JSON spec string.
 *
 * Appends a "theme_meta" sibling key before the final closing brace,
 * recording the chosen colour values so the Chart.js runtime and test
 * code can read them back.
 * ----------------------------------------------------------------------- */
const char *chart_set_theme(const char *spec, const char *theme)
{
    if (!spec || !theme) return NULL;

    int is_dark = (strcmp(theme, "dark") == 0);
    const char *color    = is_dark ? "white" : "black";
    const char *bg_color = is_dark ? "#1a1a1a" : "#ffffff";

    /* Find the last '}' in the spec */
    size_t slen = strlen(spec);
    size_t last = slen;
    while (last > 0 && spec[last - 1] != '}') last--;
    if (last == 0) return NULL;

    TkStrBuf sb = TK_STRBUF_INIT;

    /* Copy everything up to (but not including) the last '}' */
    strbuf_appendn(&sb, spec, last - 1);

    /* Append theme meta block and close.  122.13: the theme name was
     * written into the JSON raw, so theme 'x","injected":"1' added keys. */
    A(",\"theme_meta\":{\"theme\":");
    AJ(theme);
    strbuf_appendf(&sb,
             ",\"label_color\":\"%s\","
             "\"backgroundColor\":\"%s\","
             "\"scales_x_ticks_color\":\"%s\","
             "\"scales_y_ticks_color\":\"%s\""
             "}}",
             color, bg_color, color, color);

    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_set_legend — append legend configuration before the final '}'.
 * ----------------------------------------------------------------------- */
const char *chart_set_legend(const char *spec, const char *position, int display)
{
    if (!spec) return NULL;

    size_t slen = strlen(spec);
    size_t last = slen;
    while (last > 0 && spec[last - 1] != '}') last--;
    if (last == 0) return NULL;

    TkStrBuf sb = TK_STRBUF_INIT;
    strbuf_appendn(&sb, spec, last - 1);

    A(",\"legend_meta\":{\"position\":");
    if (position) { AJ(position); } else { A("\"top\""); }
    A(",\"display\":");
    A(display ? "true" : "false");
    A("}}");

    return strbuf_take(&sb);
}

/* -----------------------------------------------------------------------
 * chart_set_tooltip — append tooltip field list before the final '}'.
 * ----------------------------------------------------------------------- */
const char *chart_set_tooltip(const char *spec, const char *const *fields,
                               uint64_t n)
{
    if (!spec) return NULL;

    size_t slen = strlen(spec);
    size_t last = slen;
    while (last > 0 && spec[last - 1] != '}') last--;
    if (last == 0) return NULL;

    TkStrBuf sb = TK_STRBUF_INIT;
    strbuf_appendn(&sb, spec, last - 1);

    A(",\"tooltip_meta\":{\"fields\":[");
    for (uint64_t i = 0; i < n; i++) {
        if (i > 0) A(",");
        if (fields && fields[i]) { AJ(fields[i]); } else { A("null"); }
    }
    A("]}}");

    return strbuf_take(&sb);
}


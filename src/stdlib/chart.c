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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
 * Simple growable string builder.
 * ----------------------------------------------------------------------- */
typedef struct {
    char    *buf;
    size_t   len;   /* bytes written, excluding NUL */
    size_t   cap;   /* bytes allocated */
} SB;

static int sb_init(SB *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->buf = (char *)malloc(sb->cap);
    if (!sb->buf) return 0;
    sb->buf[0] = '\0';
    return 1;
}

static int sb_grow(SB *sb, size_t need)
{
    if (sb->len + need + 1 <= sb->cap) return 1;
    size_t newcap = sb->cap * 2;
    while (newcap < sb->len + need + 1) newcap *= 2;
    char *tmp = (char *)realloc(sb->buf, newcap);
    if (!tmp) return 0;
    sb->buf = tmp;
    sb->cap = newcap;
    return 1;
}

static int sb_append(SB *sb, const char *s)
{
    size_t slen = strlen(s);
    if (!sb_grow(sb, slen)) return 0;
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
    return 1;
}

/* Append a JSON-escaped string (with surrounding quotes). */
static int sb_append_json_str(SB *sb, const char *s)
{
    if (!sb_append(sb, "\"")) return 0;
    for (; *s; s++) {
        char esc[7];
        switch (*s) {
            case '"':  if (!sb_append(sb, "\\\"")) return 0; break;
            case '\\': if (!sb_append(sb, "\\\\")) return 0; break;
            case '\n': if (!sb_append(sb, "\\n"))  return 0; break;
            case '\r': if (!sb_append(sb, "\\r"))  return 0; break;
            case '\t': if (!sb_append(sb, "\\t"))  return 0; break;
            default:
                esc[0] = *s; esc[1] = '\0';
                if (!sb_append(sb, esc)) return 0;
                break;
        }
    }
    return sb_append(sb, "\"");
}

/* Append a double value as a JSON number. */
static int sb_append_double(SB *sb, double v)
{
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%.10g", v);
    return sb_append(sb, tmp);
}

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

    SB sb;
    if (!sb_init(&sb)) return NULL;

#define A(s)  do { if (!sb_append(&sb, (s))) { free(sb.buf); return NULL; } } while(0)
#define AJ(s) do { if (!sb_append_json_str(&sb, (s))) { free(sb.buf); return NULL; } } while(0)
#define AD(v) do { if (!sb_append_double(&sb, (v))) { free(sb.buf); return NULL; } } while(0)

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

#undef A
#undef AJ
#undef AD

    return sb.buf;
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

    SB sb;
    if (!sb_init(&sb)) return NULL;

#define A(s)  do { if (!sb_append(&sb, (s))) { free(sb.buf); return NULL; } } while(0)
#define AJ(s) do { if (!sb_append_json_str(&sb, (s))) { free(sb.buf); return NULL; } } while(0)
#define AD(v) do { if (!sb_append_double(&sb, (v))) { free(sb.buf); return NULL; } } while(0)

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

#undef A
#undef AJ
#undef AD

    return sb.buf;
}

/* =======================================================================
 * Story 34.2.1 — Additional chart types and configuration helpers.
 * ======================================================================= */

/* Convenience macros used throughout this section; undefined at section end. */
#define A(s)   do { if (!sb_append(&sb, (s)))             { free(sb.buf); return NULL; } } while(0)
#define AJ(s)  do { if (!sb_append_json_str(&sb, (s)))    { free(sb.buf); return NULL; } } while(0)
#define AD(v)  do { if (!sb_append_double(&sb, (v)))      { free(sb.buf); return NULL; } } while(0)

/* -----------------------------------------------------------------------
 * chart_stacked_bar
 * ----------------------------------------------------------------------- */
const char *chart_stacked_bar(const char *const *labels, uint64_t nlabels,
                               const char *const *series_names, uint64_t nseries,
                               const double *const *data,
                               const char *title)
{
    SB sb;
    if (!sb_init(&sb)) return NULL;

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
    return sb.buf;
}

/* -----------------------------------------------------------------------
 * chart_horizontal_bar
 * ----------------------------------------------------------------------- */
const char *chart_horizontal_bar(const char *const *labels, uint64_t n,
                                  const double *values, const char *title)
{
    SB sb;
    if (!sb_init(&sb)) return NULL;

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
    return sb.buf;
}

/* -----------------------------------------------------------------------
 * chart_area
 * ----------------------------------------------------------------------- */
const char *chart_area(const double *xs, const double *ys, uint64_t n,
                        const char *title)
{
    SB sb;
    if (!sb_init(&sb)) return NULL;

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
    return sb.buf;
}

/* -----------------------------------------------------------------------
 * chart_radar
 * ----------------------------------------------------------------------- */
const char *chart_radar(const char *const *axes, uint64_t naxes,
                         const double *values, const char *title)
{
    SB sb;
    if (!sb_init(&sb)) return NULL;

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
    return sb.buf;
}

/* -----------------------------------------------------------------------
 * chart_histogram — compute equal-width bins, then emit as bar chart.
 * ----------------------------------------------------------------------- */
const char *chart_histogram(const double *values, uint64_t n,
                              uint64_t nbins, const char *title)
{
    if (nbins == 0 || n == 0) nbins = 1;

    /* Find min / max */
    double vmin = values[0], vmax = values[0];
    for (uint64_t i = 1; i < n; i++) {
        if (values[i] < vmin) vmin = values[i];
        if (values[i] > vmax) vmax = values[i];
    }
    /* Guard against zero range */
    double range = vmax - vmin;
    if (range == 0.0) range = 1.0;

    double bin_width = range / (double)nbins;

    /* Count per bin (heap-allocated) */
    uint64_t *counts = (uint64_t *)calloc(nbins, sizeof(uint64_t));
    if (!counts) return NULL;

    for (uint64_t i = 0; i < n; i++) {
        uint64_t idx = (uint64_t)((values[i] - vmin) / bin_width);
        if (idx >= nbins) idx = nbins - 1;
        counts[idx]++;
    }

    SB sb;
    if (!sb_init(&sb)) { free(counts); return NULL; }

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
    return sb.buf;
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
    if (nrows == 0 || ncols == 0) return NULL;

    /* Find min / max for colour normalisation */
    double vmin = matrix[0], vmax = matrix[0];
    for (uint64_t i = 1; i < nrows * ncols; i++) {
        if (matrix[i] < vmin) vmin = matrix[i];
        if (matrix[i] > vmax) vmax = matrix[i];
    }
    double range = vmax - vmin;
    if (range == 0.0) range = 1.0;

    const int CELL_W  = 60;
    const int CELL_H  = 40;
    const int LPAD    = 80;   /* left padding for row labels */
    const int TPAD    = 60;   /* top padding for col labels + title */

    int svg_w = LPAD + (int)ncols * CELL_W + 10;
    int svg_h = TPAD + (int)nrows * CELL_H + 10;

    SB sb;
    if (!sb_init(&sb)) return NULL;

    /* SVG open */
    {
        char hdr[256];
        snprintf(hdr, sizeof(hdr),
                 "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                 "width=\"%d\" height=\"%d\">", svg_w, svg_h);
        A(hdr);
    }

    /* Title text */
    if (title) {
        char ttl[512];
        snprintf(ttl, sizeof(ttl),
                 "<text x=\"%d\" y=\"20\" text-anchor=\"middle\" "
                 "font-size=\"14\" font-weight=\"bold\">%s</text>",
                 svg_w / 2, title);
        A(ttl);
    }

    /* Column labels */
    for (uint64_t c = 0; c < ncols; c++) {
        int cx = LPAD + (int)c * CELL_W + CELL_W / 2;
        char lbl[256];
        snprintf(lbl, sizeof(lbl),
                 "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" "
                 "font-size=\"11\">%s</text>",
                 cx, TPAD - 5, cols[c]);
        A(lbl);
    }

    /* Row labels + cells */
    for (uint64_t r = 0; r < nrows; r++) {
        int cy = TPAD + (int)r * CELL_H;

        /* Row label */
        {
            char lbl[256];
            snprintf(lbl, sizeof(lbl),
                     "<text x=\"%d\" y=\"%d\" text-anchor=\"end\" "
                     "font-size=\"11\">%s</text>",
                     LPAD - 5, cy + CELL_H / 2 + 4, rows[r]);
            A(lbl);
        }

        for (uint64_t c = 0; c < ncols; c++) {
            double v   = matrix[r * ncols + c];
            double t   = (v - vmin) / range;   /* 0..1 */
            /* Interpolate white->steelblue (70,130,180) */
            int red   = (int)(255.0 * (1.0 - t) + 70.0  * t);
            int green = (int)(255.0 * (1.0 - t) + 130.0 * t);
            int blue  = (int)(255.0 * (1.0 - t) + 180.0 * t);

            int cx = LPAD + (int)c * CELL_W;

            char cell[512];
            snprintf(cell, sizeof(cell),
                     "<rect x=\"%d\" y=\"%d\" width=\"%d\" height=\"%d\" "
                     "fill=\"rgb(%d,%d,%d)\" stroke=\"#ccc\"/>"
                     "<text x=\"%d\" y=\"%d\" text-anchor=\"middle\" "
                     "font-size=\"10\">%.2f</text>",
                     cx, cy, CELL_W, CELL_H,
                     red, green, blue,
                     cx + CELL_W / 2, cy + CELL_H / 2 + 4,
                     v);
            A(cell);
        }
    }

    A("</svg>");
    return sb.buf;
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

    SB sb;
    if (!sb_init(&sb)) return NULL;

    /* Copy everything up to (but not including) the last '}' */
    if (!sb_grow(&sb, slen + 512)) { free(sb.buf); return NULL; }
    memcpy(sb.buf, spec, last - 1);
    sb.len = last - 1;
    sb.buf[sb.len] = '\0';

    /* Append theme meta block and close */
    char block[512];
    snprintf(block, sizeof(block),
             ",\"theme_meta\":{"
             "\"theme\":\"%s\","
             "\"label_color\":\"%s\","
             "\"backgroundColor\":\"%s\","
             "\"scales_x_ticks_color\":\"%s\","
             "\"scales_y_ticks_color\":\"%s\""
             "}}",
             theme, color, bg_color, color, color);
    A(block);

    return sb.buf;
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

    SB sb;
    if (!sb_init(&sb)) return NULL;

    if (!sb_grow(&sb, slen + 256)) { free(sb.buf); return NULL; }
    memcpy(sb.buf, spec, last - 1);
    sb.len = last - 1;
    sb.buf[sb.len] = '\0';

    A(",\"legend_meta\":{\"position\":");
    if (position) { AJ(position); } else { A("\"top\""); }
    A(",\"display\":");
    A(display ? "true" : "false");
    A("}}");

    return sb.buf;
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

    SB sb;
    if (!sb_init(&sb)) return NULL;

    if (!sb_grow(&sb, slen + 256 + (size_t)n * 64)) { free(sb.buf); return NULL; }
    memcpy(sb.buf, spec, last - 1);
    sb.len = last - 1;
    sb.buf[sb.len] = '\0';

    A(",\"tooltip_meta\":{\"fields\":[");
    for (uint64_t i = 0; i < n; i++) {
        if (i > 0) A(",");
        if (fields && fields[i]) { AJ(fields[i]); } else { A("null"); }
    }
    A("]}}");

    return sb.buf;
}

#undef A
#undef AJ
#undef AD

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

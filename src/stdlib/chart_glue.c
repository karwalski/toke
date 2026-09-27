/*
 * chart_glue.c — i64-ABI wrappers for std.chart.
 *
 * Extracted from tk_web_glue.c (story 136.22), the same move toon_glue.c
 * (114.35), net_glue.c (114.31) and llm_glue.c (136.18) made: these wrappers
 * only linked when std.http dragged tk_web_glue.c in, so `i=chart:std.chart`
 * on its own died at link with E9003 naming tk_chart_bar_w.
 * src/stdlib_deps.c lists this file under the chart module.
 *
 * chart.addchart and chart.serve stay in tk_web_glue.c: their bodies are
 * dashboard_* calls, and pulling them here would put the dashboard, router
 * and http link set behind a plain chart import.
 */
#include "chart.h"
#include "bytes_rt.h"  /* 135.19: tk_bytes_pack */
#include "tk_array.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_chart_new_w(void) {
    /* Allocate an empty TkChartSpec (bar type, no labels/datasets).
     * Caller populates it via tk_chart_bar_w or similar. */
    TkChartSpec *spec = (TkChartSpec *)calloc(1, sizeof(TkChartSpec));
    if (!spec) return 0;
    spec->type = CHART_BAR;
    return (int64_t)(intptr_t)spec;
}

/*
 * Story 136.22 — chart.bar(labels; data; title).
 *
 * stdlib/chart.tki, docs/stdlib/chart.md and chart_bar() in chart.h all take
 * a title; the wrapper once passed a hard NULL in its place.
 *
 * 135.30: the result is the $chartspec record toke reads (TkChartSpecRec),
 * not the C TkChartSpec -- `spec.type` read the ChartType enum as a string
 * and crashed.  Its arrays are fresh toke arrays, so a later in-place change
 * to the caller's labels or data does not reach the chart.
 */
static int64_t copy_arr(const int64_t *src) {
    int64_t n = src ? src[-1] : 0;
    int64_t h = tk_arr_alloc(n, n);
    if (h && n > 0)
        memcpy((void *)(intptr_t)h, src, (size_t)n * sizeof(int64_t));
    return h;
}

int64_t tk_chart_bar_w(int64_t labels_i64, int64_t data_i64, int64_t title_i64) {
    TkDatasetRec   *ds   = (TkDatasetRec *)malloc(sizeof *ds);
    TkChartSpecRec *spec = (TkChartSpecRec *)malloc(sizeof *spec);
    int64_t dss = tk_arr_alloc(1, 1);
    if (!ds || !spec || !dss) { free(ds); free(spec); return 0; }
    ds->label = "data";
    ds->data  = copy_arr((int64_t *)(intptr_t)data_i64);
    ds->color = "";
    ((int64_t *)(intptr_t)dss)[0] = (int64_t)(intptr_t)ds;
    spec->type     = "bar";
    spec->labels   = copy_arr((int64_t *)(intptr_t)labels_i64);
    spec->datasets = dss;
    spec->title    = title_i64 ? (const char *)(intptr_t)title_i64 : "";
    return (int64_t)(intptr_t)spec;
}

static ChartType chart_type_of(const char *t) {
    if (t && strcmp(t, "line") == 0)    return CHART_LINE;
    if (t && strcmp(t, "scatter") == 0) return CHART_SCATTER;
    if (t && strcmp(t, "pie") == 0)     return CHART_PIE;
    return CHART_BAR;
}

TkChartSpec *chart_spec_from_rec(int64_t rec) {
    if (!rec) return NULL;
    const TkChartSpecRec *r = (const TkChartSpecRec *)(intptr_t)rec;
    TkChartSpec *spec = (TkChartSpec *)calloc(1, sizeof *spec);
    if (!spec) return NULL;
    spec->type  = chart_type_of(r->type);
    spec->title = (r->title && *r->title) ? r->title : NULL;
    int64_t nl = tk_arr_len(r->labels);
    if (nl > 0) {
        spec->labels = (const char **)malloc((size_t)nl * sizeof(const char *));
        if (!spec->labels) { chart_spec_free_conv(spec); return NULL; }
        for (int64_t i = 0; i < nl; i++)
            spec->labels[i] = (const char *)(intptr_t)((int64_t *)(intptr_t)r->labels)[i];
        spec->nlabels = (uint64_t)nl;
    }
    int64_t nd = tk_arr_len(r->datasets);
    if (nd > 0) {
        spec->datasets = (TkDataset *)calloc((size_t)nd, sizeof(TkDataset));
        if (!spec->datasets) { chart_spec_free_conv(spec); return NULL; }
        spec->ndatasets = (uint64_t)nd;
        for (int64_t i = 0; i < nd; i++) {
            const TkDatasetRec *d =
                (const TkDatasetRec *)(intptr_t)((int64_t *)(intptr_t)r->datasets)[i];
            TkDataset *o = &spec->datasets[i];
            if (!d) continue;
            o->label = d->label;
            o->color = (d->color && *d->color) ? d->color : NULL;
            int64_t nv = tk_arr_len(d->data);
            if (nv > 0) {
                double *v = (double *)malloc((size_t)nv * sizeof(double));
                if (!v) { chart_spec_free_conv(spec); return NULL; }
                memcpy(v, (const void *)(intptr_t)d->data, (size_t)nv * sizeof(double));
                o->values  = v;
                o->nvalues = (uint64_t)nv;
            }
        }
    }
    return spec;
}

void chart_spec_free_conv(TkChartSpec *spec) {
    if (!spec) return;
    for (uint64_t i = 0; spec->datasets && i < spec->ndatasets; i++)
        free((void *)spec->datasets[i].values);
    free(spec->datasets);
    free((void *)spec->labels);
    free(spec);
}

/* 135.19 (3): chart.tojson is declared `[byte]` (stdlib/chart.tki,
 * docs/stdlib/chart.md) but returned the C string itself, so `.len` read the
 * eight bytes before the malloc'd text and `str.frombytes` of it was empty.
 * It now returns a real [byte] block of the JSON text. */
int64_t tk_chart_tojson_w(int64_t chart) {
    /* 135.30: `chart` is a $chartspec record (TkChartSpecRec) */
    TkChartSpec *spec = chart_spec_from_rec(chart);
    if (!spec) return tk_bytes_pack(NULL, 0);
    const char *s = chart_tojson(spec);
    chart_spec_free_conv(spec);
    if (!s) return tk_bytes_pack(NULL, 0);
    int64_t h = tk_bytes_pack((const uint8_t *)s, (uint64_t)strlen(s));
    free((void *)s);
    return h;
}

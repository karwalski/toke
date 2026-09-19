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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_chart_new_w(int64_t dummy) {
    /* Allocate an empty TkChartSpec (bar type, no labels/datasets).
     * Caller populates it via tk_chart_bar_w or similar. */
    (void)dummy;
    TkChartSpec *spec = (TkChartSpec *)calloc(1, sizeof(TkChartSpec));
    if (!spec) return 0;
    spec->type = CHART_BAR;
    return (int64_t)(intptr_t)spec;
}

/*
 * Story 136.22 — chart.bar(labels; data; title).
 *
 * stdlib/chart.tki, docs/stdlib/chart.md and chart_bar() in chart.h all take
 * a title, and make_spec() stores it and chart_tojson() emits it. The wrapper
 * declared two parameters and passed a hard NULL in the title's place, so the
 * argument a caller wrote was not merely ignored -- it could not be written
 * at all, and every chart came out untitled.
 */
int64_t tk_chart_bar_w(int64_t labels_i64, int64_t data_i64, int64_t title_i64) {
    /* labels_i64: toke array of strings (ptr[-1]=count, ptr[0..n-1]=str ptrs)
     * data_i64:   toke array of f64 values (ptr[-1]=count, ptr[0..n-1]=f64 bits)
     * title_i64:  chart title, or 0 for none
     * Returns a TkChartSpec* for a bar chart.
     *
     * Everything the spec points at is heap-allocated and NOT freed here.
     * make_spec() in chart.c stores the pointers it is given and copies
     * nothing (its own comment says "not owned"), so the wrapper's
     * old shape handed back a spec pointing at a STACK TkDataset and at two
     * blocks it had just free()d, and chart.tojson read all three. That is
     * the SIGSEGV, not the title; see the commit for 136.22. toke's runtime
     * model is leak-forever (tk_array.h), so owning them here is consistent. */
    StrArray labels = { NULL, 0 };
    if (labels_i64) {
        int64_t *lp = (int64_t *)(intptr_t)labels_i64;
        int64_t n = lp[-1];
        if (n > 0) {
            labels.data = (const char **)malloc((size_t)n * sizeof(const char *));
            if (labels.data) {
                labels.len = (uint64_t)n;
                for (int64_t i = 0; i < n; i++)
                    labels.data[i] = (const char *)(intptr_t)lp[i];
            }
        }
    }
    TkDataset *ds = (TkDataset *)calloc(1, sizeof(TkDataset));
    if (!ds) { free((void *)labels.data); return 0; }
    ds->label = "data";
    ds->color = NULL;
    if (data_i64) {
        int64_t *dp = (int64_t *)(intptr_t)data_i64;
        int64_t n = dp[-1];
        if (n > 0) {
            double *vals = (double *)malloc((size_t)n * sizeof(double));
            if (vals) {
                for (int64_t i = 0; i < n; i++)
                    memcpy(&vals[i], &dp[i], sizeof(double));
                ds->values = vals;
                ds->nvalues = (uint64_t)n;
            }
        }
    }
    return (int64_t)(intptr_t)chart_bar(labels, ds, 1,
               title_i64 ? (const char *)(intptr_t)title_i64 : NULL);
}

int64_t tk_chart_tojson_w(int64_t chart) {
    if (!chart) return 0;
    const char *s = chart_tojson((TkChartSpec *)(intptr_t)chart);
    return (int64_t)(intptr_t)s;
}

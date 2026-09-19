/*
 * ml_glue.c — i64-ABI wrappers for std.ml.
 *
 * Extracted from tk_web_glue.c (story 136.25), as toon_glue.c (114.35),
 * net_glue.c (114.31), llm_glue.c (136.18) and chart_glue.c (136.22) were:
 * these wrappers only linked when std.http dragged tk_web_glue.c in, so
 * `i=ml:std.ml` alone died at link. src/stdlib_deps.c lists this file under
 * the ml module.
 */
#include "ml.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_ml_linregfit_w(int64_t data, int64_t targets) {
    if (!data || !targets) return 0;
    /* Decode toke F64Array layout: ptr[-1] = count, ptr[0..] = doubles */
    int64_t *dptr = (int64_t *)(intptr_t)data;
    int64_t *tptr = (int64_t *)(intptr_t)targets;
    F64Array xs = { (const double *)dptr, (uint64_t)dptr[-1] };
    F64Array ys = { (const double *)tptr, (uint64_t)tptr[-1] };
    LinearModel m = ml_linregfit(xs, ys);
    /* Pack slope + intercept into heap block */
    double *block = (double *)malloc(2 * sizeof(double));
    if (!block) return 0;
    block[0] = m.slope;
    block[1] = m.intercept;
    return (int64_t)(intptr_t)block;
}

int64_t tk_ml_linregpredict_w(int64_t model, int64_t input) {
    if (!model) return 0;
    double *block = (double *)(intptr_t)model;
    LinearModel m;
    m.slope = block[0];
    m.intercept = block[1];
    double x;
    memcpy(&x, &input, sizeof(double));
    double result = ml_linregpredict(m, x);
    int64_t r;
    memcpy(&r, &result, sizeof(r));
    return r;
}

/*
 * Story 136.25 — ml.accuracy(ytrue; ypred).
 *
 * ml_accuracy() has been in ml.c and declared in ml.h since the module
 * shipped, and docs/stdlib/ml.md documents ml.accuracy(ytrue: @(u64);
 * ypred: @(u64)): f64. It had no wrapper and no .tki export, so nothing could
 * reach it from toke: the call raised E4027 at compile time and, had it got
 * past that, would have looked for a tk_ml_accuracy_w that did not exist.
 * The implementation was complete and unreachable.
 *
 * Both arrays are toke u64 arrays (handle[-1] = length). When the two lengths
 * differ, the shorter one governs -- reading past the end of either would be
 * a buffer overrun, and ml_accuracy takes a single n for both.
 */
int64_t tk_ml_accuracy_w(int64_t ytrue, int64_t ypred) {
    if (!ytrue || !ypred) return 0;   /* 0.0 bit pattern */
    int64_t *tp = (int64_t *)(intptr_t)ytrue;
    int64_t *pp = (int64_t *)(intptr_t)ypred;
    int64_t nt = tp[-1], np = pp[-1];
    int64_t n = nt < np ? nt : np;
    if (n <= 0) return 0;
    double acc = ml_accuracy((const uint64_t *)tp, (const uint64_t *)pp, (uint64_t)n);
    int64_t bits;
    memcpy(&bits, &acc, sizeof bits);
    return bits;
}

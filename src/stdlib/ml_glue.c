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

/* ── Story 136.32: k-means, decision tree and KNN ─────────────────────────
 *
 * ml_kmeanstrain, ml_kmeansassign, ml_dtreefit, ml_dtreepredict and
 * ml_knnpredict have been in ml.c and declared in ml.h since story 16.1.5,
 * and stdlib/ml.tki exports all five, so docs/stdlib/ml.md documents them as
 * implemented. None had a wrapper: five of the page's examples type-checked
 * and then failed at link. Same shape as 136.25's ml.accuracy — a complete
 * implementation with no way to reach it.
 *
 * Marshalling
 * -----------
 * `$row{vals:@(f64)}` crosses as a pointer to a one-slot block whose slot is
 * an f64 array handle, and toke stores f64 array elements as raw bit patterns
 * in i64 slots, so a slot pointer IS a double* (the convention
 * tk_ml_linregfit_w above already relies on). ml.c wants one F64Array per
 * COLUMN, so rows are transposed here.
 *
 * Labels are `[str]` in toke and double* in C. They are coded to their index
 * in a per-model vocabulary on the way in and decoded on the way out, so
 * ml.dtreepredict and ml.knnpredict return the caller's own label strings
 * rather than a number the caller would have to map back by hand.
 */

#include "tk_array.h"

/* Build one F64Array per column from an array of $row handles.
 * Returns a malloc'd F64Array[ncols] (caller frees, including each .data),
 * or NULL when the input is empty or ragged. */
static F64Array *ml_cols_from_rows(int64_t rows, uint64_t *ncols_out,
                                   uint64_t *nrows_out) {
    if (!rows) return NULL;
    int64_t nrows = tk_arr_len(rows);
    if (nrows <= 0) return NULL;
    int64_t *rh = (int64_t *)(intptr_t)rows;

    int64_t first = ((int64_t *)(intptr_t)rh[0])[0];   /* $row.vals */
    if (!first) return NULL;
    int64_t ncols = tk_arr_len(first);
    if (ncols <= 0) return NULL;

    F64Array *cols = (F64Array *)malloc((size_t)ncols * sizeof(F64Array));
    if (!cols) return NULL;
    for (int64_t c = 0; c < ncols; c++) {
        double *buf = (double *)malloc((size_t)nrows * sizeof(double));
        if (!buf) {
            for (int64_t j = 0; j < c; j++) free((void *)cols[j].data);
            free(cols);
            return NULL;
        }
        for (int64_t r = 0; r < nrows; r++) {
            int64_t vals = rh[r] ? ((int64_t *)(intptr_t)rh[r])[0] : 0;
            double v = 0.0;
            if (vals && tk_arr_len(vals) > c)
                memcpy(&v, &((int64_t *)(intptr_t)vals)[c], sizeof v);
            buf[r] = v;
        }
        cols[c].data = buf;
        cols[c].len  = (uint64_t)nrows;
    }
    *ncols_out = (uint64_t)ncols;
    *nrows_out = (uint64_t)nrows;
    return cols;
}

static void ml_cols_free(F64Array *cols, uint64_t ncols) {
    if (!cols) return;
    for (uint64_t i = 0; i < ncols; i++) free((void *)cols[i].data);
    free(cols);
}

/* Copy an f64 array handle into a plain double buffer (caller frees). */
static double *ml_point(int64_t arr, uint64_t *n_out) {
    int64_t n = tk_arr_len(arr);
    if (n <= 0) { *n_out = 0; return NULL; }
    double *p = (double *)malloc((size_t)n * sizeof(double));
    if (!p) { *n_out = 0; return NULL; }
    int64_t *slots = (int64_t *)(intptr_t)arr;
    for (int64_t i = 0; i < n; i++) memcpy(&p[i], &slots[i], sizeof(double));
    *n_out = (uint64_t)n;
    return p;
}

/* A label vocabulary: distinct strings in first-seen order. Codes are the
 * index, so decoding is a bounds-checked lookup. */
typedef struct { const char **names; uint64_t n; } MlVocab;

static double *ml_encode_labels(int64_t labels, MlVocab *vocab) {
    int64_t n = tk_arr_len(labels);
    if (n <= 0) return NULL;
    int64_t *slots = (int64_t *)(intptr_t)labels;
    double *codes = (double *)malloc((size_t)n * sizeof(double));
    const char **names = (const char **)malloc((size_t)n * sizeof(char *));
    if (!codes || !names) { free(codes); free(names); return NULL; }
    uint64_t nv = 0;
    for (int64_t i = 0; i < n; i++) {
        const char *s = (const char *)(intptr_t)slots[i];
        if (!s) s = "";
        uint64_t k = 0;
        for (; k < nv; k++) if (!strcmp(names[k], s)) break;
        if (k == nv) names[nv++] = s;
        codes[i] = (double)k;
    }
    vocab->names = names;
    vocab->n     = nv;
    return codes;
}

static int64_t ml_decode_label(const MlVocab *v, double code) {
    if (!v || !v->names || v->n == 0) return (int64_t)(intptr_t)"";
    long idx = (long)(code + 0.5);
    if (idx < 0) idx = 0;
    if ((uint64_t)idx >= v->n) idx = (long)v->n - 1;
    return (int64_t)(intptr_t)v->names[idx];
}

/*
 * ml.kmeanstrain(rows; k; maxiter) : $kmeansmodel!$mlerr
 *
 * Returns the DOCUMENTED toke struct — `$kmeansmodel{centroids:@($centroid);
 * k:u64}` with `$centroid{id:u64; center:@(f64)}` — rather than hiding a C
 * pointer behind it, so the fields the page describes really are readable and
 * ml.kmeansassign can be handed a model the caller built itself.
 */
int64_t tk_ml_kmeanstrain_w(int64_t rows, int64_t k, int64_t maxiter) {
    uint64_t ncols = 0, nrows = 0;
    F64Array *cols = ml_cols_from_rows(rows, &ncols, &nrows);
    if (!cols) return 0;
    if (k <= 0) k = 1;
    if (maxiter <= 0) maxiter = 100;

    KMeansModel m = ml_kmeanstrain(cols, ncols, nrows,
                                   (uint64_t)k, (uint64_t)maxiter);
    ml_cols_free(cols, ncols);
    if (!m.centroids || m.k == 0) return 0;

    int64_t arr = tk_arr_alloc((int64_t)m.k, (int64_t)m.k);
    if (!arr) { ml_kmeans_free(&m); return 0; }
    int64_t *slots = (int64_t *)(intptr_t)arr;
    for (uint64_t c = 0; c < m.k; c++) {
        int64_t center = tk_arr_alloc((int64_t)m.ndim, (int64_t)m.ndim);
        if (!center) { tk_arr_setlen(arr, (int64_t)c); break; }
        int64_t *cs = (int64_t *)(intptr_t)center;
        for (uint64_t d = 0; d < m.ndim; d++)
            memcpy(&cs[d], &m.centroids[c * m.ndim + d], sizeof(int64_t));
        int64_t *cent = (int64_t *)malloc(2 * sizeof(int64_t));
        if (!cent) { tk_arr_setlen(arr, (int64_t)c); break; }
        cent[0] = (int64_t)c;      /* $centroid.id     */
        cent[1] = center;          /* $centroid.center */
        slots[c] = (int64_t)(intptr_t)cent;
    }
    ml_kmeans_free(&m);

    int64_t *model = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!model) return 0;
    model[0] = arr;                        /* $kmeansmodel.centroids */
    model[1] = tk_arr_len(arr);            /* $kmeansmodel.k         */
    return (int64_t)(intptr_t)model;
}

/* ml.kmeansassign(model; point) : u64 — index of the nearest centroid (L2).
 * Reads the documented struct back, so a hand-built model works. */
int64_t tk_ml_kmeansassign_w(int64_t model, int64_t point) {
    if (!model || !point) return 0;
    int64_t *m = (int64_t *)(intptr_t)model;
    int64_t arr = m[0];
    int64_t k = tk_arr_len(arr);
    if (k <= 0) return 0;
    int64_t *cents = (int64_t *)(intptr_t)arr;

    int64_t c0 = cents[0] ? ((int64_t *)(intptr_t)cents[0])[1] : 0;
    int64_t ndim = tk_arr_len(c0);
    if (ndim <= 0) return 0;

    double *flat = (double *)malloc((size_t)(k * ndim) * sizeof(double));
    if (!flat) return 0;
    for (int64_t c = 0; c < k; c++) {
        int64_t center = cents[c] ? ((int64_t *)(intptr_t)cents[c])[1] : 0;
        int64_t *cs = (int64_t *)(intptr_t)center;
        for (int64_t d = 0; d < ndim; d++) {
            double v = 0.0;
            if (center && tk_arr_len(center) > d) memcpy(&v, &cs[d], sizeof v);
            flat[c * ndim + d] = v;
        }
    }
    uint64_t pn = 0;
    double *p = ml_point(point, &pn);
    if (!p) { free(flat); return 0; }
    /* ml_kmeansassign reads ndim coordinates; pad a short query with zeros. */
    if (pn < (uint64_t)ndim) {
        double *q = (double *)calloc((size_t)ndim, sizeof(double));
        if (q) { memcpy(q, p, (size_t)pn * sizeof(double)); free(p); p = q; }
    }
    KMeansModel km; km.centroids = flat; km.k = (uint64_t)k; km.ndim = (uint64_t)ndim;
    uint64_t idx = ml_kmeansassign(km, p);
    free(flat); free(p);
    return (int64_t)idx;
}

/* The C model plus the label vocabulary it was fitted against; this is what
 * `$dtreemodel.arena` points at. */
typedef struct { DTreeModel tree; MlVocab vocab; } MlDTree;

/* ml.dtreefit(rows; labels; maxdepth) : $dtreemodel!$mlerr */
int64_t tk_ml_dtreefit_w(int64_t rows, int64_t labels, int64_t maxdepth) {
    uint64_t ncols = 0, nrows = 0;
    F64Array *cols = ml_cols_from_rows(rows, &ncols, &nrows);
    if (!cols) return 0;
    MlVocab vocab = { NULL, 0 };
    double *codes = ml_encode_labels(labels, &vocab);
    if (!codes || tk_arr_len(labels) < (int64_t)nrows) {
        ml_cols_free(cols, ncols); free(codes); free((void *)vocab.names);
        return 0;
    }
    if (maxdepth <= 0) maxdepth = 1;

    MlDTree *h = (MlDTree *)malloc(sizeof(MlDTree));
    if (!h) { ml_cols_free(cols, ncols); free(codes); free((void *)vocab.names); return 0; }
    h->tree  = ml_dtreefit(cols, ncols, codes, nrows, (uint64_t)maxdepth);
    h->vocab = vocab;
    ml_cols_free(cols, ncols);
    free(codes);

    int64_t *model = (int64_t *)malloc(3 * sizeof(int64_t));
    if (!model) { free(h); return 0; }
    model[0] = (int64_t)h->tree.nnodes;   /* $dtreemodel.nodes    */
    model[1] = maxdepth;                  /* $dtreemodel.maxdepth */
    model[2] = (int64_t)(intptr_t)h;      /* $dtreemodel.arena    */
    return (int64_t)(intptr_t)model;
}

/* ml.dtreepredict(model; point) : $str — the caller's own label string.
 * Returns "" for the empty `$dtreemodel{}` the docs use as an error arm. */
int64_t tk_ml_dtreepredict_w(int64_t model, int64_t point) {
    if (!model || !point) return (int64_t)(intptr_t)"";
    int64_t *m = (int64_t *)(intptr_t)model;
    MlDTree *h = (MlDTree *)(intptr_t)m[2];
    if (!h || h->tree.nnodes == 0) return (int64_t)(intptr_t)"";
    uint64_t pn = 0;
    double *p = ml_point(point, &pn);
    if (!p) return (int64_t)(intptr_t)"";
    double code = ml_dtreepredict(&h->tree, p);
    free(p);
    return ml_decode_label(&h->vocab, code);
}

/* ml.knnpredict(rows; labels; query; k) : $str */
int64_t tk_ml_knnpredict_w(int64_t rows, int64_t labels, int64_t query, int64_t k) {
    uint64_t ncols = 0, nrows = 0;
    F64Array *cols = ml_cols_from_rows(rows, &ncols, &nrows);
    if (!cols) return (int64_t)(intptr_t)"";
    MlVocab vocab = { NULL, 0 };
    double *codes = ml_encode_labels(labels, &vocab);
    uint64_t qn = 0;
    double *q = ml_point(query, &qn);
    int64_t out = (int64_t)(intptr_t)"";
    if (codes && q && tk_arr_len(labels) >= (int64_t)nrows) {
        if (k <= 0) k = 1;
        if ((uint64_t)k > nrows) k = (int64_t)nrows;
        if (qn < ncols) {
            double *qq = (double *)calloc((size_t)ncols, sizeof(double));
            if (qq) { memcpy(qq, q, (size_t)qn * sizeof(double)); free(q); q = qq; }
        }
        double code = ml_knnpredict(cols, ncols, codes, nrows, q, (uint64_t)k);
        out = ml_decode_label(&vocab, code);
    }
    ml_cols_free(cols, ncols);
    free(codes); free(q); free((void *)vocab.names);
    return out;
}

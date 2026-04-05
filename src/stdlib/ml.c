/*
 * ml.c — Implementation of the std.ml standard library module.
 *
 * Algorithms implemented:
 *   - Simple (1-feature) linear regression via ordinary least squares.
 *   - K-means clustering (Lloyd's algorithm, deterministic initialisation).
 *   - CART decision tree (greedy Gini-impurity minimisation, classification).
 *   - K-nearest neighbours (brute-force L2, majority vote).
 *
 * No external dependencies beyond libc and -lm.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Callers own returned model structs and must call the
 * corresponding *_free() function when done.
 *
 * Story: 16.1.5
 */

#include "ml.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Squared Euclidean distance between two ndim-dimensional points. */
static double l2sq(const double *a, const double *b, uint64_t ndim)
{
    double sum = 0.0;
    for (uint64_t d = 0; d < ndim; d++) {
        double diff = a[d] - b[d];
        sum += diff * diff;
    }
    return sum;
}

/* =========================================================================
 * Linear regression
 * ========================================================================= */

/*
 * ml_linregfit — ordinary least squares for y = slope*x + intercept.
 *
 *   slope     = (n*sum(xy) - sum(x)*sum(y)) / (n*sum(x^2) - sum(x)^2)
 *   intercept = (sum(y) - slope*sum(x)) / n
 *
 * Returns zero slope/intercept when xs.len < 2 or the denominator is zero.
 */
LinearModel ml_linregfit(F64Array xs, F64Array ys)
{
    LinearModel m = {0.0, 0.0};
    uint64_t n = xs.len < ys.len ? xs.len : ys.len;
    if (n < 2) return m;

    double sum_x = 0.0, sum_y = 0.0, sum_xy = 0.0, sum_xx = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        sum_x  += xs.data[i];
        sum_y  += ys.data[i];
        sum_xy += xs.data[i] * ys.data[i];
        sum_xx += xs.data[i] * xs.data[i];
    }
    double denom = (double)n * sum_xx - sum_x * sum_x;
    if (fabs(denom) < 1e-15) return m;

    m.slope     = ((double)n * sum_xy - sum_x * sum_y) / denom;
    m.intercept = (sum_y - m.slope * sum_x) / (double)n;
    return m;
}

/* ml_linregpredict — evaluate the fitted line at x. */
double ml_linregpredict(LinearModel m, double x)
{
    return m.slope * x + m.intercept;
}

/* =========================================================================
 * K-means clustering (Lloyd's algorithm)
 * ========================================================================= */

/*
 * ml_kmeanstrain — cluster nrows points in ndim=ncols dimensions into k
 * clusters using Lloyd's algorithm with at most max_iter iterations.
 *
 * Centroid initialisation: take the first k data points (column-major input
 * is converted to row-major internally).
 *
 * Convergence: stop early when no centroid moves more than 1e-9 in L2.
 */
KMeansModel ml_kmeanstrain(F64Array *cols, uint64_t ncols, uint64_t nrows,
                            uint64_t k, uint64_t max_iter)
{
    KMeansModel model;
    model.k    = k;
    model.ndim = ncols;
    model.centroids = (double *)calloc(k * ncols, sizeof(double));

    /* Build a temporary row-major point array for convenience. */
    double *points = (double *)malloc(nrows * ncols * sizeof(double));
    if (!points || !model.centroids) {
        free(points);
        return model; /* out of memory; caller checks centroids != NULL */
    }
    for (uint64_t row = 0; row < nrows; row++) {
        for (uint64_t d = 0; d < ncols; d++) {
            points[row * ncols + d] = cols[d].data[row];
        }
    }

    /* Initialise centroids to first k points. */
    uint64_t init_k = k < nrows ? k : nrows;
    for (uint64_t ci = 0; ci < init_k; ci++) {
        memcpy(&model.centroids[ci * ncols],
               &points[ci * ncols],
               ncols * sizeof(double));
    }

    uint64_t *assign  = (uint64_t *)malloc(nrows * sizeof(uint64_t));
    double   *new_c   = (double   *)malloc(k * ncols * sizeof(double));
    uint64_t *counts  = (uint64_t *)malloc(k * sizeof(uint64_t));
    if (!assign || !new_c || !counts) {
        free(assign); free(new_c); free(counts); free(points);
        return model;
    }

    for (uint64_t iter = 0; iter < max_iter; iter++) {
        /* Assignment step. */
        for (uint64_t row = 0; row < nrows; row++) {
            double best_d = DBL_MAX;
            uint64_t best_c = 0;
            for (uint64_t ci = 0; ci < k; ci++) {
                double d = l2sq(&points[row * ncols],
                                &model.centroids[ci * ncols], ncols);
                if (d < best_d) { best_d = d; best_c = ci; }
            }
            assign[row] = best_c;
        }

        /* Update step: compute mean of each cluster. */
        memset(new_c,  0, k * ncols * sizeof(double));
        memset(counts, 0, k * sizeof(uint64_t));
        for (uint64_t row = 0; row < nrows; row++) {
            uint64_t ci = assign[row];
            counts[ci]++;
            for (uint64_t d = 0; d < ncols; d++) {
                new_c[ci * ncols + d] += points[row * ncols + d];
            }
        }
        for (uint64_t ci = 0; ci < k; ci++) {
            if (counts[ci] > 0) {
                for (uint64_t d = 0; d < ncols; d++) {
                    new_c[ci * ncols + d] /= (double)counts[ci];
                }
            }
            /* If a cluster is empty keep the old centroid. */
        }

        /* Convergence check. */
        double max_shift = 0.0;
        for (uint64_t ci = 0; ci < k; ci++) {
            double shift = l2sq(&new_c[ci * ncols],
                                &model.centroids[ci * ncols], ncols);
            if (shift > max_shift) max_shift = shift;
        }
        memcpy(model.centroids, new_c, k * ncols * sizeof(double));
        if (max_shift < 1e-18) break; /* sqrt(1e-18) = 1e-9 */
    }

    free(assign); free(new_c); free(counts); free(points);
    return model;
}

/* ml_kmeansassign — return the index of the nearest centroid. */
uint64_t ml_kmeansassign(KMeansModel m, double *point)
{
    double best_d = DBL_MAX;
    uint64_t best_c = 0;
    for (uint64_t ci = 0; ci < m.k; ci++) {
        double d = l2sq(point, &m.centroids[ci * m.ndim], m.ndim);
        if (d < best_d) { best_d = d; best_c = ci; }
    }
    return best_c;
}

/* ml_kmeans_free — release heap memory inside the model. */
void ml_kmeans_free(KMeansModel *m)
{
    if (m) { free(m->centroids); m->centroids = NULL; m->k = 0; m->ndim = 0; }
}

/* =========================================================================
 * Decision tree (CART, greedy Gini, classification)
 * ========================================================================= */

/*
 * Gini impurity of a subset described by an index array.
 *
 *   G = 1 - sum_c( (n_c / n)^2 )
 *
 * We work with integer-valued labels.  Labels are compared as doubles but
 * class counts are tracked by rounding to the nearest integer.
 */
static double gini(double *labels, uint64_t *idx, uint64_t n)
{
    if (n == 0) return 0.0;

    /* Count distinct classes.  We use a small inline counter: collect
     * unique values in a dynamic array.  For the test sizes (< 1000 rows)
     * O(n^2) is fine. */
    double  cls_buf[256];
    uint64_t cnt_buf[256];
    uint64_t ncls = 0;

    for (uint64_t i = 0; i < n; i++) {
        double lbl = labels[idx[i]];
        int found = 0;
        for (uint64_t c = 0; c < ncls; c++) {
            if (cls_buf[c] == lbl) { cnt_buf[c]++; found = 1; break; }
        }
        if (!found && ncls < 256) {
            cls_buf[ncls]  = lbl;
            cnt_buf[ncls]  = 1;
            ncls++;
        }
    }

    double g = 1.0;
    for (uint64_t c = 0; c < ncls; c++) {
        double p = (double)cnt_buf[c] / (double)n;
        g -= p * p;
    }
    return g;
}

/* Majority class label for a subset. */
static double majority_class(double *labels, uint64_t *idx, uint64_t n)
{
    double  cls_buf[256];
    uint64_t cnt_buf[256];
    uint64_t ncls = 0;

    for (uint64_t i = 0; i < n; i++) {
        double lbl = labels[idx[i]];
        int found = 0;
        for (uint64_t c = 0; c < ncls; c++) {
            if (cls_buf[c] == lbl) { cnt_buf[c]++; found = 1; break; }
        }
        if (!found && ncls < 256) {
            cls_buf[ncls] = lbl;
            cnt_buf[ncls] = 1;
            ncls++;
        }
    }

    double best_lbl = 0.0;
    uint64_t best_cnt = 0;
    for (uint64_t c = 0; c < ncls; c++) {
        if (cnt_buf[c] > best_cnt) { best_cnt = cnt_buf[c]; best_lbl = cls_buf[c]; }
    }
    return best_lbl;
}

/* -----------------------------------------------------------------------
 * Tree builder — recursive, stores nodes into flat arrays.
 * ----------------------------------------------------------------------- */

typedef struct {
    int     *feature_indices;
    double  *thresholds;
    int     *left_children;
    int     *right_children;
    double  *leaf_values;
    uint64_t capacity;
    uint64_t nnodes;
} TreeBuilder;

static int tb_new_node(TreeBuilder *tb)
{
    if (tb->nnodes >= tb->capacity) {
        uint64_t new_cap = tb->capacity * 2;
        tb->feature_indices = (int    *)realloc(tb->feature_indices, new_cap * sizeof(int));
        tb->thresholds      = (double *)realloc(tb->thresholds,      new_cap * sizeof(double));
        tb->left_children   = (int    *)realloc(tb->left_children,   new_cap * sizeof(int));
        tb->right_children  = (int    *)realloc(tb->right_children,  new_cap * sizeof(int));
        tb->leaf_values     = (double *)realloc(tb->leaf_values,     new_cap * sizeof(double));
        tb->capacity = new_cap;
    }
    int nid = (int)tb->nnodes++;
    tb->feature_indices[nid] = -1;
    tb->thresholds[nid]      = 0.0;
    tb->left_children[nid]   = -1;
    tb->right_children[nid]  = -1;
    tb->leaf_values[nid]     = 0.0;
    return nid;
}

/*
 * Recursively build the tree.
 *
 *   feature_cols – ncols column arrays
 *   labels       – one label per row
 *   idx          – row indices for this node (we may reorder in place)
 *   n            – number of rows in idx
 *   depth        – current depth (root = 0)
 *   max_depth    – stop splitting at this depth
 */
static int build_node(TreeBuilder *tb,
                       F64Array *feature_cols, uint64_t nfeatures,
                       double *labels,
                       uint64_t *idx, uint64_t n,
                       uint64_t depth, uint64_t max_depth)
{
    int nid = tb_new_node(tb);
    tb->leaf_values[nid] = majority_class(labels, idx, n);

    /* Stopping criteria: pure node, too few samples, or max depth reached. */
    if (n < 2 || depth >= max_depth || gini(labels, idx, n) < 1e-12) {
        /* Leaf: left/right already -1. */
        return nid;
    }

    /* Find the best (feature, threshold) split. */
    double   best_gain   = -1.0;
    uint64_t best_feat   = 0;
    double   best_thresh = 0.0;
    double   parent_gini = gini(labels, idx, n);

    /* Temp arrays for partitioning. */
    uint64_t *left_idx  = (uint64_t *)malloc(n * sizeof(uint64_t));
    uint64_t *right_idx = (uint64_t *)malloc(n * sizeof(uint64_t));

    for (uint64_t f = 0; f < nfeatures; f++) {
        /* Collect unique thresholds (midpoints between consecutive sorted
         * values).  For small n we sort a copy. */
        double *vals = (double *)malloc(n * sizeof(double));
        for (uint64_t i = 0; i < n; i++) vals[i] = feature_cols[f].data[idx[i]];

        /* Insertion sort (n is small in practice). */
        for (uint64_t i = 1; i < n; i++) {
            double key = vals[i]; uint64_t j = i;
            while (j > 0 && vals[j-1] > key) { vals[j] = vals[j-1]; j--; }
            vals[j] = key;
        }

        /* Try each unique midpoint as threshold. */
        for (uint64_t i = 0; i + 1 < n; i++) {
            if (vals[i] == vals[i+1]) continue; /* skip duplicates */
            double thresh = (vals[i] + vals[i+1]) / 2.0;

            uint64_t nl = 0, nr = 0;
            for (uint64_t r = 0; r < n; r++) {
                if (feature_cols[f].data[idx[r]] <= thresh)
                    left_idx[nl++]  = idx[r];
                else
                    right_idx[nr++] = idx[r];
            }
            if (nl == 0 || nr == 0) continue;

            double wg = ((double)nl * gini(labels, left_idx, nl) +
                         (double)nr * gini(labels, right_idx, nr)) / (double)n;
            double gain = parent_gini - wg;
            if (gain > best_gain) {
                best_gain   = gain;
                best_feat   = f;
                best_thresh = thresh;
            }
        }
        free(vals);
    }

    if (best_gain <= 0.0) {
        /* No useful split found; remain a leaf. */
        free(left_idx); free(right_idx);
        return nid;
    }

    /* Partition idx into left and right. */
    uint64_t nl = 0, nr = 0;
    for (uint64_t r = 0; r < n; r++) {
        if (feature_cols[best_feat].data[idx[r]] <= best_thresh)
            left_idx[nl++]  = idx[r];
        else
            right_idx[nr++] = idx[r];
    }

    tb->feature_indices[nid] = (int)best_feat;
    tb->thresholds[nid]      = best_thresh;

    int left_nid  = build_node(tb, feature_cols, nfeatures, labels,
                                left_idx,  nl, depth + 1, max_depth);
    int right_nid = build_node(tb, feature_cols, nfeatures, labels,
                                right_idx, nr, depth + 1, max_depth);

    tb->left_children[nid]  = left_nid;
    tb->right_children[nid] = right_nid;

    free(left_idx); free(right_idx);
    return nid;
}

/* ml_dtreefit — fit a CART classification tree. */
DTreeModel ml_dtreefit(F64Array *feature_cols, uint64_t nfeatures,
                        double *labels, uint64_t nrows, uint64_t max_depth)
{
    DTreeModel m;
    m.feature_indices = NULL;
    m.thresholds      = NULL;
    m.left_children   = NULL;
    m.right_children  = NULL;
    m.leaf_values     = NULL;
    m.nnodes          = 0;

    uint64_t initial_cap = 64;
    TreeBuilder tb;
    tb.feature_indices = (int    *)malloc(initial_cap * sizeof(int));
    tb.thresholds      = (double *)malloc(initial_cap * sizeof(double));
    tb.left_children   = (int    *)malloc(initial_cap * sizeof(int));
    tb.right_children  = (int    *)malloc(initial_cap * sizeof(int));
    tb.leaf_values     = (double *)malloc(initial_cap * sizeof(double));
    tb.capacity        = initial_cap;
    tb.nnodes          = 0;

    uint64_t *idx = (uint64_t *)malloc(nrows * sizeof(uint64_t));
    for (uint64_t i = 0; i < nrows; i++) idx[i] = i;

    build_node(&tb, feature_cols, nfeatures, labels, idx, nrows, 0, max_depth);
    free(idx);

    m.feature_indices = tb.feature_indices;
    m.thresholds      = tb.thresholds;
    m.left_children   = tb.left_children;
    m.right_children  = tb.right_children;
    m.leaf_values     = tb.leaf_values;
    m.nnodes          = tb.nnodes;
    return m;
}

/* ml_dtreepredict — walk the tree for a single query point. */
double ml_dtreepredict(DTreeModel *m, double *point)
{
    int node = 0;
    while (node >= 0 && (uint64_t)node < m->nnodes) {
        int l = m->left_children[node];
        int r = m->right_children[node];
        if (l == -1 && r == -1) break; /* leaf */
        int feat = m->feature_indices[node];
        if (point[feat] <= m->thresholds[node])
            node = l;
        else
            node = r;
    }
    return (node >= 0 && (uint64_t)node < m->nnodes) ? m->leaf_values[node] : 0.0;
}

/* ml_dtree_free — release all heap memory inside *m. */
void ml_dtree_free(DTreeModel *m)
{
    if (!m) return;
    free(m->feature_indices); m->feature_indices = NULL;
    free(m->thresholds);      m->thresholds      = NULL;
    free(m->left_children);   m->left_children   = NULL;
    free(m->right_children);  m->right_children  = NULL;
    free(m->leaf_values);     m->leaf_values     = NULL;
    m->nnodes = 0;
}

/* =========================================================================
 * K-nearest neighbours (brute-force L2, majority vote)
 * ========================================================================= */

/*
 * ml_knnpredict — find the k nearest training points to query_point (L2)
 * and return the majority label among them.
 *
 * Partial selection: we maintain a sorted list of (distance, label) pairs
 * for the k closest points seen so far — O(ntrain * k) which is fine for
 * the expected dataset sizes.
 */
double ml_knnpredict(F64Array *train_cols, uint64_t nfeatures,
                      double *train_labels, uint64_t ntrain,
                      double *query_point, uint64_t k)
{
    if (ntrain == 0 || k == 0) return 0.0;
    if (k > ntrain) k = ntrain;

    double   *kd  = (double *)malloc(k * sizeof(double));   /* distances */
    double   *kl  = (double *)malloc(k * sizeof(double));   /* labels    */
    uint64_t  filled = 0;

    for (uint64_t t = 0; t < ntrain; t++) {
        /* Compute L2^2 distance to training point t. */
        double dist = 0.0;
        for (uint64_t d = 0; d < nfeatures; d++) {
            double diff = query_point[d] - train_cols[d].data[t];
            dist += diff * diff;
        }

        if (filled < k) {
            /* Slots still available — insert and keep sorted. */
            uint64_t pos = filled;
            while (pos > 0 && kd[pos-1] > dist) {
                kd[pos] = kd[pos-1]; kl[pos] = kl[pos-1]; pos--;
            }
            kd[pos] = dist; kl[pos] = train_labels[t];
            filled++;
        } else if (dist < kd[k-1]) {
            /* Better than the current worst neighbour — replace and re-sort. */
            uint64_t pos = k - 1;
            while (pos > 0 && kd[pos-1] > dist) {
                kd[pos] = kd[pos-1]; kl[pos] = kl[pos-1]; pos--;
            }
            kd[pos] = dist; kl[pos] = train_labels[t];
        }
    }

    /* Majority vote over the k labels collected. */
    double  cls_buf[256];
    uint64_t cnt_buf[256];
    uint64_t ncls = 0;

    for (uint64_t i = 0; i < k; i++) {
        double lbl = kl[i];
        int found = 0;
        for (uint64_t c = 0; c < ncls; c++) {
            if (cls_buf[c] == lbl) { cnt_buf[c]++; found = 1; break; }
        }
        if (!found && ncls < 256) {
            cls_buf[ncls] = lbl;
            cnt_buf[ncls] = 1;
            ncls++;
        }
    }

    double best_lbl = 0.0;
    uint64_t best_cnt = 0;
    for (uint64_t c = 0; c < ncls; c++) {
        if (cnt_buf[c] > best_cnt) { best_cnt = cnt_buf[c]; best_lbl = cls_buf[c]; }
    }

    free(kd); free(kl);
    return best_lbl;
}

/* =========================================================================
 * Train/test split and evaluation metrics (Story 31.3.1)
 * ========================================================================= */

/*
 * ml_train_test_split — shuffle n indices with Fisher-Yates using an LCG
 * PRNG, then assign the first ceil(n * test_size) shuffled indices to the
 * test set and the remainder to the train set.
 */
SplitResult ml_train_test_split(uint64_t n, double test_size, uint64_t seed)
{
    SplitResult s;
    s.train_idx = NULL;
    s.ntrain    = 0;
    s.test_idx  = NULL;
    s.ntest     = 0;

    if (n == 0) return s;

    /* Build index array [0..n-1]. */
    uint64_t *idx = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!idx) return s;
    for (uint64_t i = 0; i < n; i++) idx[i] = i;

    /* Fisher-Yates shuffle with LCG. */
    uint64_t rng = seed;
    for (uint64_t i = n - 1; i > 0; i--) {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        uint64_t j = rng % (i + 1);
        uint64_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }

    /* Determine split sizes. */
    uint64_t ntest = (uint64_t)(n * test_size + 0.9999999999); /* ceil */
    if (ntest > n) ntest = n;
    uint64_t ntrain = n - ntest;

    s.test_idx  = (uint64_t *)malloc(ntest  * sizeof(uint64_t));
    s.train_idx = (uint64_t *)malloc(ntrain * sizeof(uint64_t));
    if (!s.test_idx || !s.train_idx) {
        free(idx); free(s.test_idx); free(s.train_idx);
        s.test_idx = NULL; s.train_idx = NULL;
        return s;
    }

    /* First ntest shuffled indices → test set, the rest → train set. */
    for (uint64_t i = 0; i < ntest;  i++) s.test_idx[i]  = idx[i];
    for (uint64_t i = 0; i < ntrain; i++) s.train_idx[i] = idx[ntest + i];

    s.ntrain = ntrain;
    s.ntest  = ntest;
    free(idx);
    return s;
}

/* ml_confusion_matrix — count TP, TN, FP, FN for binary labels (0/1). */
ConfusionMatrix ml_confusion_matrix(const uint64_t *y_true,
                                    const uint64_t *y_pred, uint64_t n)
{
    ConfusionMatrix cm = {0, 0, 0, 0};
    for (uint64_t i = 0; i < n; i++) {
        uint64_t t = y_true[i];
        uint64_t p = y_pred[i];
        if      (t == 1 && p == 1) cm.tp++;
        else if (t == 0 && p == 0) cm.tn++;
        else if (t == 0 && p == 1) cm.fp++;
        else if (t == 1 && p == 0) cm.fn++;
    }
    return cm;
}

/* ml_precision_recall_f1 — derive precision, recall and F1 from counts. */
PRF1Result ml_precision_recall_f1(const uint64_t *y_true,
                                  const uint64_t *y_pred, uint64_t n)
{
    ConfusionMatrix cm = ml_confusion_matrix(y_true, y_pred, n);
    PRF1Result r = {0.0, 0.0, 0.0};

    double tp = (double)cm.tp;
    double fp = (double)cm.fp;
    double fn = (double)cm.fn;

    double prec_denom = tp + fp;
    double rec_denom  = tp + fn;

    r.precision = (prec_denom > 0.0) ? tp / prec_denom : 0.0;
    r.recall    = (rec_denom  > 0.0) ? tp / rec_denom  : 0.0;

    double f1_denom = r.precision + r.recall;
    r.f1 = (f1_denom > 0.0) ? 2.0 * r.precision * r.recall / f1_denom : 0.0;
    return r;
}

/* ml_accuracy — fraction of matching labels. */
double ml_accuracy(const uint64_t *y_true, const uint64_t *y_pred,
                   uint64_t n)
{
    if (n == 0) return 0.0;
    uint64_t correct = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (y_true[i] == y_pred[i]) correct++;
    }
    return (double)correct / (double)n;
}

/* ml_standardize — z-score: z = (x - mean) / stddev.
 * Returns NULL on allocation failure.  If stddev == 0, all zeros. */
double *ml_standardize(const double *xs, uint64_t n)
{
    if (n == 0) return NULL;
    double *out = (double *)malloc(n * sizeof(double));
    if (!out) return NULL;

    double mean = 0.0;
    for (uint64_t i = 0; i < n; i++) mean += xs[i];
    mean /= (double)n;

    double var = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        double d = xs[i] - mean;
        var += d * d;
    }
    var /= (double)n;
    double sd = sqrt(var);

    if (sd < 1e-15) {
        for (uint64_t i = 0; i < n; i++) out[i] = 0.0;
    } else {
        for (uint64_t i = 0; i < n; i++) out[i] = (xs[i] - mean) / sd;
    }
    return out;
}

/* ml_normalize — min-max to [0,1].
 * Returns NULL on allocation failure.  If max == min, all zeros. */
double *ml_normalize(const double *xs, uint64_t n)
{
    if (n == 0) return NULL;
    double *out = (double *)malloc(n * sizeof(double));
    if (!out) return NULL;

    double mn = xs[0], mx = xs[0];
    for (uint64_t i = 1; i < n; i++) {
        if (xs[i] < mn) mn = xs[i];
        if (xs[i] > mx) mx = xs[i];
    }

    double range = mx - mn;
    if (fabs(range) < 1e-15) {
        for (uint64_t i = 0; i < n; i++) out[i] = 0.0;
    } else {
        for (uint64_t i = 0; i < n; i++) out[i] = (xs[i] - mn) / range;
    }
    return out;
}

/* ml_split_free — release the index arrays inside a SplitResult. */
void ml_split_free(SplitResult *s)
{
    if (!s) return;
    free(s->train_idx); s->train_idx = NULL; s->ntrain = 0;
    free(s->test_idx);  s->test_idx  = NULL; s->ntest  = 0;
}

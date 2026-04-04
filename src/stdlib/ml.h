#ifndef TK_STDLIB_ML_H
#define TK_STDLIB_ML_H

/*
 * ml.h — C interface for the std.ml standard library module.
 *
 * Type mappings:
 *   [f64]  = F64Array  (struct { const double *data; uint64_t len; })
 *   f64    = double
 *   u64    = uint64_t
 *
 * malloc is permitted; callers own returned pointers unless noted otherwise.
 * Use ml_kmeans_free() / ml_dtree_free() to release model memory.
 *
 * Link with -lm.
 *
 * Story: 16.1.5
 */

#include <stdint.h>

/* Re-use the F64Array type from math.h if already included; otherwise define
 * it here.  The guard prevents a duplicate-typedef error. */
#ifndef TK_F64ARRAY_DEFINED
#define TK_F64ARRAY_DEFINED
typedef struct { const double *data; uint64_t len; } F64Array;
#endif

/* -------------------------------------------------------------------------
 * Model types
 * ------------------------------------------------------------------------- */

typedef struct {
    double slope;
    double intercept;
} LinearModel;

typedef struct {
    double  *centroids; /* flat row-major: k * ndim doubles */
    uint64_t k;
    uint64_t ndim;
} KMeansModel;

typedef struct {
    int     *feature_indices; /* which column to split on at each node  */
    double  *thresholds;      /* split threshold                        */
    int     *left_children;   /* child node index, -1 = leaf            */
    int     *right_children;  /* child node index, -1 = leaf            */
    double  *leaf_values;     /* majority class value (leaf nodes only) */
    uint64_t nnodes;
} DTreeModel;

/* Result type for operations that can fail (returns a single f64). */
typedef struct {
    double      ok;
    int         is_err;
    const char *err_msg; /* static or heap string; valid when is_err != 0 */
} F64Result;

/* -------------------------------------------------------------------------
 * Linear regression
 * ------------------------------------------------------------------------- */

/* ml.linregfit: fit a simple (1-feature) linear model via least squares. */
LinearModel ml_linregfit(F64Array xs, F64Array ys);

/* ml.linregpredict: evaluate the model at x. */
double      ml_linregpredict(LinearModel m, double x);

/* -------------------------------------------------------------------------
 * K-means clustering
 * ------------------------------------------------------------------------- */

/* ml_kmeanstrain: Lloyd's algorithm.
 *   cols    – array of ncols F64Arrays, each of length nrows
 *   k       – number of clusters
 *   max_iter – iteration limit
 * Centroids are initialised to the first k data points (deterministic). */
KMeansModel ml_kmeanstrain(F64Array *cols, uint64_t ncols, uint64_t nrows,
                            uint64_t k, uint64_t max_iter);

/* ml_kmeansassign: return the index of the nearest centroid (L2). */
uint64_t    ml_kmeansassign(KMeansModel m, double *point);

/* ml_kmeans_free: release all memory inside *m (not m itself). */
void        ml_kmeans_free(KMeansModel *m);

/* -------------------------------------------------------------------------
 * Decision tree (CART, classification)
 * ------------------------------------------------------------------------- */

/* ml_dtreefit: greedy binary CART tree.
 *   feature_cols – array of nfeatures F64Arrays, each of length nrows
 *   labels       – class labels (one per row; expected to be integer-valued)
 *   max_depth    – maximum tree depth (root = depth 0) */
DTreeModel  ml_dtreefit(F64Array *feature_cols, uint64_t nfeatures,
                         double *labels, uint64_t nrows, uint64_t max_depth);

/* ml_dtreepredict: walk the tree for a single query point. */
double      ml_dtreepredict(DTreeModel *m, double *point);

/* ml_dtree_free: release all memory inside *m (not m itself). */
void        ml_dtree_free(DTreeModel *m);

/* -------------------------------------------------------------------------
 * K-nearest neighbours (regression / classification, majority vote)
 * ------------------------------------------------------------------------- */

/* ml_knnpredict: brute-force L2 KNN, majority vote over k nearest labels.
 *   train_cols   – array of nfeatures F64Arrays, each of length ntrain
 *   train_labels – class labels for each training point
 *   query_point  – nfeatures-dimensional query
 *   k            – number of neighbours */
double      ml_knnpredict(F64Array *train_cols, uint64_t nfeatures,
                           double *train_labels, uint64_t ntrain,
                           double *query_point, uint64_t k);

#endif /* TK_STDLIB_ML_H */

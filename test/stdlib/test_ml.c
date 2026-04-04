/*
 * test_ml.c — Unit tests for the std.ml C library (Story 16.1.5).
 *
 * Build and run: make test-stdlib-ml
 *
 * Synthetic datasets used:
 *   - Linear regression: xs=[1,2,3], ys=[2,4,6]  → slope=2, intercept=0
 *   - K-means (1D):      [0, 0.1, 0.2, 10, 10.1, 10.2], k=2
 *   - Decision tree:     XOR-like (0,0)→0 (0,1)→1 (1,0)→1 (1,1)→0, max_depth=3
 *   - KNN:               4 well-separated 2D points
 */

#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include "../../src/stdlib/ml.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_NEAR(a, b, eps, msg) \
    ASSERT(fabs((a) - (b)) < (eps), msg)

/* =========================================================================
 * 1. Linear regression — slope and intercept
 * ========================================================================= */

static void test_linreg(void)
{
    double xs_data[] = {1.0, 2.0, 3.0};
    double ys_data[] = {2.0, 4.0, 6.0};
    F64Array xs = { xs_data, 3 };
    F64Array ys = { ys_data, 3 };

    LinearModel m = ml_linregfit(xs, ys);

    /* Test 1: slope == 2.0 */
    ASSERT_NEAR(m.slope, 2.0, 1e-10, "linregfit slope=2.0 for y=2x");

    /* Test 2: intercept == 0.0 */
    ASSERT_NEAR(m.intercept, 0.0, 1e-10, "linregfit intercept=0.0 for y=2x");

    /* Test 3: predict at x=4 returns 8.0 */
    double pred = ml_linregpredict(m, 4.0);
    ASSERT_NEAR(pred, 8.0, 1e-10, "linregpredict(m,4.0)==8.0");

    /* Test 4: perfect correlation — predict training points exactly */
    ASSERT_NEAR(ml_linregpredict(m, 1.0), 2.0, 1e-10, "linregpredict recovers training point y(1)=2");
    ASSERT_NEAR(ml_linregpredict(m, 2.0), 4.0, 1e-10, "linregpredict recovers training point y(2)=4");
    ASSERT_NEAR(ml_linregpredict(m, 3.0), 6.0, 1e-10, "linregpredict recovers training point y(3)=6");
}

/* =========================================================================
 * 2. K-means — two clearly separated 1D clusters
 * ========================================================================= */

static void test_kmeans(void)
{
    /* Six points in 1-D: three near 0, three near 10. */
    double col_data[] = {0.0, 0.1, 0.2, 10.0, 10.1, 10.2};
    F64Array cols[1];
    cols[0].data = col_data;
    cols[0].len  = 6;

    KMeansModel model = ml_kmeanstrain(cols, 1, 6, 2, 100);

    /* Test 5: centroids should be near 0.1 and 10.1 (in some order). */
    double c0 = model.centroids[0]; /* centroid 0 */
    double c1 = model.centroids[1]; /* centroid 1 */

    /* One centroid near 0.1, the other near 10.1. */
    int c0_low = fabs(c0 - 0.1) < 1.0;
    int c1_high = fabs(c1 - 10.1) < 1.0;
    int c0_high = fabs(c0 - 10.1) < 1.0;
    int c1_low  = fabs(c1 - 0.1) < 1.0;
    ASSERT((c0_low && c1_high) || (c0_high && c1_low),
           "kmeans: centroids near 0.1 and 10.1");

    /* Test 6: assign point near 0 and point near 10 to different clusters. */
    double pt_low[]  = {0.05};
    double pt_high[] = {10.05};
    uint64_t cl_low  = ml_kmeansassign(model, pt_low);
    uint64_t cl_high = ml_kmeansassign(model, pt_high);
    ASSERT(cl_low != cl_high, "kmeansassign: point near 0 and point near 10 in different clusters");

    /* Test 7: the cluster of the low point contains c0 or c1 correctly. */
    double assigned_centroid = model.centroids[cl_low];
    ASSERT(fabs(assigned_centroid - 0.1) < 1.0,
           "kmeansassign: low point assigned to low centroid");

    ml_kmeans_free(&model);
}

/* =========================================================================
 * 3. Decision tree — XOR-like dataset
 * ========================================================================= */

static void test_dtree(void)
{
    /*
     * XOR training data:
     *   (x0, x1) → label
     *   (0,  0)  → 0
     *   (0,  1)  → 1
     *   (1,  0)  → 1
     *   (1,  1)  → 0
     */
    double f0_data[] = {0.0, 0.0, 1.0, 1.0};
    double f1_data[] = {0.0, 1.0, 0.0, 1.0};
    double labels[]  = {0.0, 1.0, 1.0, 0.0};

    F64Array feat_cols[2];
    feat_cols[0].data = f0_data; feat_cols[0].len = 4;
    feat_cols[1].data = f1_data; feat_cols[1].len = 4;

    DTreeModel tree = ml_dtreefit(feat_cols, 2, labels, 4, 3);

    /* Test 8: predict all four training points correctly. */
    double p00[] = {0.0, 0.0};
    double p01[] = {0.0, 1.0};
    double p10[] = {1.0, 0.0};
    double p11[] = {1.0, 1.0};

    ASSERT_NEAR(ml_dtreepredict(&tree, p00), 0.0, 1e-10, "dtree XOR (0,0)->0");
    ASSERT_NEAR(ml_dtreepredict(&tree, p01), 1.0, 1e-10, "dtree XOR (0,1)->1");
    ASSERT_NEAR(ml_dtreepredict(&tree, p10), 1.0, 1e-10, "dtree XOR (1,0)->1");
    ASSERT_NEAR(ml_dtreepredict(&tree, p11), 0.0, 1e-10, "dtree XOR (1,1)->0");

    /* Test 9: leaf values come from majority class — pure nodes have exact label. */
    ASSERT(tree.nnodes > 1, "dtree produced more than one node for XOR data");

    ml_dtree_free(&tree);
}

/* =========================================================================
 * 4. KNN — 2D well-separated points
 * ========================================================================= */

static void test_knn(void)
{
    /*
     * Four training points in 2-D:
     *   (0,0) → class 0
     *   (0,1) → class 1
     *   (5,5) → class 2
     *   (5,6) → class 3
     */
    double tc0_data[] = {0.0, 0.0, 5.0, 5.0};
    double tc1_data[] = {0.0, 1.0, 5.0, 6.0};
    double tlabels[]  = {0.0, 1.0, 2.0, 3.0};

    F64Array tcols[2];
    tcols[0].data = tc0_data; tcols[0].len = 4;
    tcols[1].data = tc1_data; tcols[1].len = 4;

    /* Test 10: 1-NN — nearest neighbour should be exact. */
    double q0[] = {0.1, 0.1};   /* closest to (0,0) → 0 */
    double q1[] = {0.1, 0.9};   /* closest to (0,1) → 1 */
    double q2[] = {4.9, 4.9};   /* closest to (5,5) → 2 */

    ASSERT_NEAR(ml_knnpredict(tcols, 2, tlabels, 4, q0, 1), 0.0, 1e-10,
                "knn 1-NN: query near (0,0) -> class 0");
    ASSERT_NEAR(ml_knnpredict(tcols, 2, tlabels, 4, q1, 1), 1.0, 1e-10,
                "knn 1-NN: query near (0,1) -> class 1");
    ASSERT_NEAR(ml_knnpredict(tcols, 2, tlabels, 4, q2, 1), 2.0, 1e-10,
                "knn 1-NN: query near (5,5) -> class 2");

    /* Test 11: 3-NN majority vote.
     * Query at (0.1, 0.5): three nearest are (0,0), (0,1), and (5,5)/(5,6).
     * Both low points are very close; the third nearest is far away.
     * With k=2 the two nearest are (0,0) and (0,1) (both different classes),
     * tie-broken by insertion order. With k=3 the third point is (5,5).
     * Use a query clearly closest to the cluster [0,0], [0,1] so 2 of 3 are class 0/1.
     *
     * Simpler: use k=2 on (0.05, 0.05) — nearest 2 are (0,0) class 0 and (0,1) class 1,
     * tie.  Instead verify k=3 for (0.05, 0.05): neighbours are class 0, class 1, class 0
     * if we had duplicates.  Use a dataset designed for a clear majority.
     */

    /*
     * Cleaner 3-NN test: 5 training points where 3 of the nearest to a query
     * are the same class.
     *
     *   (0,0)→A  (0.1,0)→A  (0,0.1)→A   (10,10)→B  (10,11)→B
     * Query at (0.05, 0.05), k=3 → three nearest all class A=0.
     */
    double knn3_c0[] = {0.0, 0.1, 0.0, 10.0, 10.0};
    double knn3_c1[] = {0.0, 0.0, 0.1, 10.0, 11.0};
    double knn3_lb[] = {0.0, 0.0, 0.0,  1.0,  1.0};

    F64Array knn3_cols[2];
    knn3_cols[0].data = knn3_c0; knn3_cols[0].len = 5;
    knn3_cols[1].data = knn3_c1; knn3_cols[1].len = 5;

    double qmaj[] = {0.05, 0.05};
    double result = ml_knnpredict(knn3_cols, 2, knn3_lb, 5, qmaj, 3);
    ASSERT_NEAR(result, 0.0, 1e-10, "knn 3-NN majority vote: three class-A neighbours -> class 0");
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    test_linreg();
    test_kmeans();
    test_dtree();
    test_knn();

    if (failures == 0) {
        printf("All ml tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}

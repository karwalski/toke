/*
 * test_ml.c — Unit tests for the std.ml C library (Story 16.1.5, 20.1.9).
 *
 * Build and run: make test-stdlib-ml
 *
 * Synthetic datasets used:
 *   - Linear regression: xs=[1,2,3], ys=[2,4,6]  -> slope=2, intercept=0
 *   - K-means (1D):      [0, 0.1, 0.2, 10, 10.1, 10.2], k=2
 *   - Decision tree:     XOR-like (0,0)->0 (0,1)->1 (1,0)->1 (1,1)->0, max_depth=3
 *   - KNN:               4 well-separated 2D points
 *
 * Story 20.1.9 additions:
 *   - Convergence tests on well-separated clusters
 *   - Degenerate inputs: k>n, k=1, single-point clusters, empty sets
 *   - Decision tree stumps (max_depth=1), identical features
 *   - KNN with k=n_train, k=1 (nearest neighbour)
 *   - Linear regression: single point, collinear/duplicate points
 *   - Iris-like synthetic 3-class dataset for KNN
 *   - NULL / empty input handling
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
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

    ASSERT_NEAR(m.slope, 2.0, 1e-10, "linregfit slope=2.0 for y=2x");
    ASSERT_NEAR(m.intercept, 0.0, 1e-10, "linregfit intercept=0.0 for y=2x");

    double pred = ml_linregpredict(m, 4.0);
    ASSERT_NEAR(pred, 8.0, 1e-10, "linregpredict(m,4.0)==8.0");

    ASSERT_NEAR(ml_linregpredict(m, 1.0), 2.0, 1e-10, "linregpredict recovers training point y(1)=2");
    ASSERT_NEAR(ml_linregpredict(m, 2.0), 4.0, 1e-10, "linregpredict recovers training point y(2)=4");
    ASSERT_NEAR(ml_linregpredict(m, 3.0), 6.0, 1e-10, "linregpredict recovers training point y(3)=6");
}

/* =========================================================================
 * 1b. Linear regression — degenerate inputs (Story 20.1.9)
 * ========================================================================= */

static void test_linreg_single_point(void)
{
    /* Single point: n < 2, returns zero model. */
    double xs_data[] = {5.0};
    double ys_data[] = {10.0};
    F64Array xs = { xs_data, 1 };
    F64Array ys = { ys_data, 1 };

    LinearModel m = ml_linregfit(xs, ys);
    ASSERT_NEAR(m.slope, 0.0, 1e-10, "linreg single point: slope=0");
    ASSERT_NEAR(m.intercept, 0.0, 1e-10, "linreg single point: intercept=0");
}

static void test_linreg_collinear(void)
{
    /* Duplicate/collinear x-values: all x=3.0, denom is zero -> zero model. */
    double xs_data[] = {3.0, 3.0, 3.0};
    double ys_data[] = {1.0, 2.0, 3.0};
    F64Array xs = { xs_data, 3 };
    F64Array ys = { ys_data, 3 };

    LinearModel m = ml_linregfit(xs, ys);
    ASSERT_NEAR(m.slope, 0.0, 1e-10, "linreg collinear x: slope=0 (zero denom)");
    ASSERT_NEAR(m.intercept, 0.0, 1e-10, "linreg collinear x: intercept=0 (zero denom)");
}

static void test_linreg_duplicate_points(void)
{
    /* Two identical points plus one different: y = 3x + 1, with duplicate. */
    double xs_data[] = {1.0, 1.0, 2.0};
    double ys_data[] = {4.0, 4.0, 7.0};
    F64Array xs = { xs_data, 3 };
    F64Array ys = { ys_data, 3 };

    LinearModel m = ml_linregfit(xs, ys);
    /* With duplicate (1,4) and point (2,7), slope should be 3.0. */
    ASSERT_NEAR(m.slope, 3.0, 1e-10, "linreg duplicate points: slope=3.0");
    double pred = ml_linregpredict(m, 1.0);
    ASSERT_NEAR(pred, 4.0, 1e-10, "linreg duplicate points: predict(1)=4.0");
}

/* =========================================================================
 * 2. K-means — two clearly separated 1D clusters
 * ========================================================================= */

static void test_kmeans(void)
{
    double col_data[] = {0.0, 0.1, 0.2, 10.0, 10.1, 10.2};
    F64Array cols[1];
    cols[0].data = col_data;
    cols[0].len  = 6;

    KMeansModel model = ml_kmeanstrain(cols, 1, 6, 2, 100);

    double c0 = model.centroids[0];
    double c1 = model.centroids[1];

    int c0_low = fabs(c0 - 0.1) < 1.0;
    int c1_high = fabs(c1 - 10.1) < 1.0;
    int c0_high = fabs(c0 - 10.1) < 1.0;
    int c1_low  = fabs(c1 - 0.1) < 1.0;
    ASSERT((c0_low && c1_high) || (c0_high && c1_low),
           "kmeans: centroids near 0.1 and 10.1");

    double pt_low[]  = {0.05};
    double pt_high[] = {10.05};
    uint64_t cl_low  = ml_kmeansassign(model, pt_low);
    uint64_t cl_high = ml_kmeansassign(model, pt_high);
    ASSERT(cl_low != cl_high, "kmeansassign: point near 0 and point near 10 in different clusters");

    double assigned_centroid = model.centroids[cl_low];
    ASSERT(fabs(assigned_centroid - 0.1) < 1.0,
           "kmeansassign: low point assigned to low centroid");

    ml_kmeans_free(&model);
}

/* =========================================================================
 * 2b. K-means — degenerate and convergence tests (Story 20.1.9)
 * ========================================================================= */

static void test_kmeans_k_greater_than_n(void)
{
    /* k=5 but only 3 points. Should not crash; init_k is clamped to nrows. */
    double col_data[] = {1.0, 2.0, 3.0};
    F64Array cols[1];
    cols[0].data = col_data;
    cols[0].len  = 3;

    KMeansModel model = ml_kmeanstrain(cols, 1, 3, 5, 100);
    ASSERT(model.centroids != NULL, "kmeans k>n: does not crash, returns model");
    ASSERT(model.k == 5, "kmeans k>n: model.k preserved as requested k");

    /* Assignment should still work (extra centroids are zero-initialised). */
    double pt[] = {2.0};
    uint64_t cl = ml_kmeansassign(model, pt);
    ASSERT(cl < model.k, "kmeans k>n: assign returns valid cluster index");

    ml_kmeans_free(&model);
}

static void test_kmeans_k_equals_1(void)
{
    /* k=1: all points belong to one cluster, centroid = mean. */
    double col_data[] = {1.0, 3.0, 5.0};
    F64Array cols[1];
    cols[0].data = col_data;
    cols[0].len  = 3;

    KMeansModel model = ml_kmeanstrain(cols, 1, 3, 1, 100);
    ASSERT(model.k == 1, "kmeans k=1: model has 1 cluster");
    /* Centroid should converge to mean = 3.0. */
    ASSERT_NEAR(model.centroids[0], 3.0, 1e-10, "kmeans k=1: centroid = mean of all points");

    /* All points assigned to cluster 0. */
    double pt1[] = {1.0};
    double pt2[] = {5.0};
    ASSERT(ml_kmeansassign(model, pt1) == 0, "kmeans k=1: point assigned to cluster 0");
    ASSERT(ml_kmeansassign(model, pt2) == 0, "kmeans k=1: all points in cluster 0");

    ml_kmeans_free(&model);
}

static void test_kmeans_single_point_clusters(void)
{
    /* 3 points, k=3: each point is its own cluster. */
    double col_data[] = {0.0, 50.0, 100.0};
    F64Array cols[1];
    cols[0].data = col_data;
    cols[0].len  = 3;

    KMeansModel model = ml_kmeanstrain(cols, 1, 3, 3, 100);
    /* Each centroid should be exactly one of the data points. */
    double c0 = model.centroids[0];
    double c1 = model.centroids[1];
    double c2 = model.centroids[2];

    /* Check that the set {c0, c1, c2} is a permutation of {0, 50, 100}. */
    int found0 = (fabs(c0) < 1e-10) || (fabs(c1) < 1e-10) || (fabs(c2) < 1e-10);
    int found50 = (fabs(c0 - 50.0) < 1e-10) || (fabs(c1 - 50.0) < 1e-10) || (fabs(c2 - 50.0) < 1e-10);
    int found100 = (fabs(c0 - 100.0) < 1e-10) || (fabs(c1 - 100.0) < 1e-10) || (fabs(c2 - 100.0) < 1e-10);
    ASSERT(found0 && found50 && found100,
           "kmeans k=n: each point is its own centroid");

    ml_kmeans_free(&model);
}

static void test_kmeans_convergence_well_separated(void)
{
    /*
     * 2D dataset with 3 well-separated clusters:
     *   Cluster A: (0,0), (0.1,0.1), (-0.1,0.1)
     *   Cluster B: (10,10), (10.1,10.1), (9.9,10.1)
     *   Cluster C: (20,0), (20.1,0.1), (19.9,0.1)
     */
    double c0_data[] = {0.0, 0.1, -0.1, 10.0, 10.1, 9.9, 20.0, 20.1, 19.9};
    double c1_data[] = {0.0, 0.1,  0.1, 10.0, 10.1, 10.1, 0.0,  0.1,  0.1};
    F64Array cols[2];
    cols[0].data = c0_data; cols[0].len = 9;
    cols[1].data = c1_data; cols[1].len = 9;

    KMeansModel model = ml_kmeanstrain(cols, 2, 9, 3, 200);

    /* Verify that each of the 9 points assigns to the correct cluster group.
     * Points 0-2 should share a cluster, 3-5 should share, 6-8 should share. */
    double pts[9][2] = {
        {0.0, 0.0}, {0.1, 0.1}, {-0.1, 0.1},
        {10.0, 10.0}, {10.1, 10.1}, {9.9, 10.1},
        {20.0, 0.0}, {20.1, 0.1}, {19.9, 0.1}
    };
    uint64_t clusters[9];
    for (int i = 0; i < 9; i++) {
        clusters[i] = ml_kmeansassign(model, pts[i]);
    }

    /* Within each group, all assignments must be the same. */
    ASSERT(clusters[0] == clusters[1] && clusters[1] == clusters[2],
           "kmeans convergence: cluster A points grouped together");
    ASSERT(clusters[3] == clusters[4] && clusters[4] == clusters[5],
           "kmeans convergence: cluster B points grouped together");
    ASSERT(clusters[6] == clusters[7] && clusters[7] == clusters[8],
           "kmeans convergence: cluster C points grouped together");
    /* Different groups must have different cluster IDs. */
    ASSERT(clusters[0] != clusters[3] && clusters[3] != clusters[6] && clusters[0] != clusters[6],
           "kmeans convergence: three distinct clusters found");

    ml_kmeans_free(&model);
}

/* =========================================================================
 * 3. Decision tree — XOR-like dataset
 * ========================================================================= */

static void test_dtree(void)
{
    double f0_data[] = {0.0, 0.0, 1.0, 1.0};
    double f1_data[] = {0.0, 1.0, 0.0, 1.0};
    double labels[]  = {0.0, 1.0, 1.0, 0.0};

    F64Array feat_cols[2];
    feat_cols[0].data = f0_data; feat_cols[0].len = 4;
    feat_cols[1].data = f1_data; feat_cols[1].len = 4;

    DTreeModel tree = ml_dtreefit(feat_cols, 2, labels, 4, 3);

    double p00[] = {0.0, 0.0};
    double p01[] = {0.0, 1.0};
    double p10[] = {1.0, 0.0};
    double p11[] = {1.0, 1.0};

    ASSERT_NEAR(ml_dtreepredict(&tree, p00), 0.0, 1e-10, "dtree XOR (0,0)->0");
    ASSERT_NEAR(ml_dtreepredict(&tree, p01), 1.0, 1e-10, "dtree XOR (0,1)->1");
    ASSERT_NEAR(ml_dtreepredict(&tree, p10), 1.0, 1e-10, "dtree XOR (1,0)->1");
    ASSERT_NEAR(ml_dtreepredict(&tree, p11), 0.0, 1e-10, "dtree XOR (1,1)->0");

    ASSERT(tree.nnodes > 1, "dtree produced more than one node for XOR data");

    ml_dtree_free(&tree);
}

/* =========================================================================
 * 3b. Decision tree — degenerate inputs (Story 20.1.9)
 * ========================================================================= */

static void test_dtree_stump(void)
{
    /*
     * max_depth=1 (stump): linearly separable data.
     *   x0 < 5 -> class 0, x0 >= 5 -> class 1
     */
    double f0_data[] = {1.0, 2.0, 3.0, 7.0, 8.0, 9.0};
    double labels[]  = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};

    F64Array feat_cols[1];
    feat_cols[0].data = f0_data; feat_cols[0].len = 6;

    DTreeModel tree = ml_dtreefit(feat_cols, 1, labels, 6, 1);

    /* A stump should have exactly 3 nodes: root + 2 leaves. */
    ASSERT(tree.nnodes == 3, "dtree stump: exactly 3 nodes (root + 2 leaves)");

    /* Predict: low value -> 0, high value -> 1. */
    double pt_low[]  = {2.0};
    double pt_high[] = {8.0};
    ASSERT_NEAR(ml_dtreepredict(&tree, pt_low), 0.0, 1e-10,
                "dtree stump: low value -> class 0");
    ASSERT_NEAR(ml_dtreepredict(&tree, pt_high), 1.0, 1e-10,
                "dtree stump: high value -> class 1");

    ml_dtree_free(&tree);
}

static void test_dtree_identical_features(void)
{
    /*
     * All feature values identical: no valid split possible.
     * Tree should be a single leaf (majority class).
     */
    double f0_data[] = {5.0, 5.0, 5.0, 5.0};
    double labels[]  = {0.0, 0.0, 1.0, 0.0}; /* majority class 0 */

    F64Array feat_cols[1];
    feat_cols[0].data = f0_data; feat_cols[0].len = 4;

    DTreeModel tree = ml_dtreefit(feat_cols, 1, labels, 4, 10);

    /* No split possible, so tree is a single leaf. */
    ASSERT(tree.nnodes == 1, "dtree identical features: single leaf node");
    /* Leaf should predict majority class = 0. */
    double pt[] = {5.0};
    ASSERT_NEAR(ml_dtreepredict(&tree, pt), 0.0, 1e-10,
                "dtree identical features: predicts majority class 0");

    ml_dtree_free(&tree);
}

/* =========================================================================
 * 4. KNN — 2D well-separated points
 * ========================================================================= */

static void test_knn(void)
{
    double tc0_data[] = {0.0, 0.0, 5.0, 5.0};
    double tc1_data[] = {0.0, 1.0, 5.0, 6.0};
    double tlabels[]  = {0.0, 1.0, 2.0, 3.0};

    F64Array tcols[2];
    tcols[0].data = tc0_data; tcols[0].len = 4;
    tcols[1].data = tc1_data; tcols[1].len = 4;

    double q0[] = {0.1, 0.1};
    double q1[] = {0.1, 0.9};
    double q2[] = {4.9, 4.9};

    ASSERT_NEAR(ml_knnpredict(tcols, 2, tlabels, 4, q0, 1), 0.0, 1e-10,
                "knn 1-NN: query near (0,0) -> class 0");
    ASSERT_NEAR(ml_knnpredict(tcols, 2, tlabels, 4, q1, 1), 1.0, 1e-10,
                "knn 1-NN: query near (0,1) -> class 1");
    ASSERT_NEAR(ml_knnpredict(tcols, 2, tlabels, 4, q2, 1), 2.0, 1e-10,
                "knn 1-NN: query near (5,5) -> class 2");

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
 * 4b. KNN — degenerate and extended tests (Story 20.1.9)
 * ========================================================================= */

static void test_knn_k_equals_ntrain(void)
{
    /*
     * k = n_train: use all training points for majority vote.
     * 3 of class A, 2 of class B -> majority is A.
     */
    double c0_data[] = {0.0, 1.0, 2.0, 100.0, 101.0};
    double c1_data[] = {0.0, 0.0, 0.0,   0.0,   0.0};
    double labels[]  = {0.0, 0.0, 0.0,   1.0,   1.0};

    F64Array cols[2];
    cols[0].data = c0_data; cols[0].len = 5;
    cols[1].data = c1_data; cols[1].len = 5;

    double query[] = {50.0, 0.0};
    double pred = ml_knnpredict(cols, 2, labels, 5, query, 5);
    ASSERT_NEAR(pred, 0.0, 1e-10, "knn k=n_train: majority vote over all points -> class 0");
}

static void test_knn_k1_nearest_neighbour(void)
{
    /*
     * k=1 on a 1D dataset: should return the label of the nearest point.
     */
    double c0_data[] = {0.0, 10.0, 20.0};
    F64Array cols[1];
    cols[0].data = c0_data; cols[0].len = 3;

    double labels[] = {0.0, 1.0, 2.0};

    double q_near0[]  = {0.5};
    double q_near10[] = {9.5};
    double q_near20[] = {19.5};

    ASSERT_NEAR(ml_knnpredict(cols, 1, labels, 3, q_near0, 1), 0.0, 1e-10,
                "knn k=1: nearest to 0 -> class 0");
    ASSERT_NEAR(ml_knnpredict(cols, 1, labels, 3, q_near10, 1), 1.0, 1e-10,
                "knn k=1: nearest to 10 -> class 1");
    ASSERT_NEAR(ml_knnpredict(cols, 1, labels, 3, q_near20, 1), 2.0, 1e-10,
                "knn k=1: nearest to 20 -> class 2");
}

static void test_knn_iris_like_3class(void)
{
    /*
     * Synthetic iris-like 3-class dataset (2D, linearly separable):
     *   Class 0 (setosa):     cluster around (1, 1)
     *   Class 1 (versicolor): cluster around (5, 5)
     *   Class 2 (virginica):  cluster around (9, 9)
     * 4 points per class = 12 training points.
     */
    double f0_data[] = {0.8, 1.0, 1.2, 1.1,   4.8, 5.0, 5.2, 5.1,   8.8, 9.0, 9.2, 9.1};
    double f1_data[] = {0.9, 1.1, 0.8, 1.2,   4.9, 5.1, 4.8, 5.2,   8.9, 9.1, 8.8, 9.2};
    double labels[]  = {0.0, 0.0, 0.0, 0.0,   1.0, 1.0, 1.0, 1.0,   2.0, 2.0, 2.0, 2.0};

    F64Array cols[2];
    cols[0].data = f0_data; cols[0].len = 12;
    cols[1].data = f1_data; cols[1].len = 12;

    /* Query points near each cluster centre. */
    double q_class0[] = {1.05, 1.05};
    double q_class1[] = {5.05, 5.05};
    double q_class2[] = {9.05, 9.05};

    /* k=3: all 3 nearest neighbours should be from the correct class. */
    ASSERT_NEAR(ml_knnpredict(cols, 2, labels, 12, q_class0, 3), 0.0, 1e-10,
                "knn iris-like: query near class 0 -> class 0");
    ASSERT_NEAR(ml_knnpredict(cols, 2, labels, 12, q_class1, 3), 1.0, 1e-10,
                "knn iris-like: query near class 1 -> class 1");
    ASSERT_NEAR(ml_knnpredict(cols, 2, labels, 12, q_class2, 3), 2.0, 1e-10,
                "knn iris-like: query near class 2 -> class 2");
}

/* =========================================================================
 * 5. Empty and edge-case inputs (Story 20.1.9)
 * ========================================================================= */

static void test_knn_empty_training_set(void)
{
    /* ntrain=0: should return 0.0 gracefully. */
    F64Array cols[1];
    double dummy_data[] = {0.0};
    cols[0].data = dummy_data;
    cols[0].len = 0;

    double query[] = {1.0};
    double pred = ml_knnpredict(cols, 1, NULL, 0, query, 3);
    ASSERT_NEAR(pred, 0.0, 1e-10, "knn empty training set: returns 0.0");
}

static void test_knn_k_greater_than_ntrain(void)
{
    /* k > ntrain: implementation clamps k to ntrain. */
    double c0_data[] = {1.0, 2.0};
    double labels[]  = {0.0, 0.0};
    F64Array cols[1];
    cols[0].data = c0_data; cols[0].len = 2;

    double query[] = {1.5};
    double pred = ml_knnpredict(cols, 1, labels, 2, query, 10);
    ASSERT_NEAR(pred, 0.0, 1e-10, "knn k>ntrain: clamped k, returns class 0");
}

static void test_linreg_empty(void)
{
    /* Empty arrays: n < 2, returns zero model. */
    F64Array xs = { NULL, 0 };
    F64Array ys = { NULL, 0 };

    LinearModel m = ml_linregfit(xs, ys);
    ASSERT_NEAR(m.slope, 0.0, 1e-10, "linreg empty: slope=0");
    ASSERT_NEAR(m.intercept, 0.0, 1e-10, "linreg empty: intercept=0");
}

/* =========================================================================
 * 6. Train/test split (Story 31.3.1)
 * ========================================================================= */

static void test_train_test_split(void)
{
    /* Split 10 samples with 30% test -> ntest=3, ntrain=7. */
    SplitResult s = ml_train_test_split(10, 0.3, 42);

    ASSERT(s.ntrain == 7, "train_test_split(10, 0.3): ntrain==7");
    ASSERT(s.ntest  == 3, "train_test_split(10, 0.3): ntest==3");

    /* All indices present exactly once across train + test. */
    uint8_t seen[10] = {0};
    for (uint64_t i = 0; i < s.ntrain; i++) {
        ASSERT(s.train_idx[i] < 10, "train_test_split: train index in range");
        seen[s.train_idx[i]]++;
    }
    for (uint64_t i = 0; i < s.ntest; i++) {
        ASSERT(s.test_idx[i] < 10, "train_test_split: test index in range");
        seen[s.test_idx[i]]++;
    }
    int all_unique = 1;
    for (int i = 0; i < 10; i++) { if (seen[i] != 1) { all_unique = 0; break; } }
    ASSERT(all_unique, "train_test_split: all 10 indices appear exactly once");

    ml_split_free(&s);
    ASSERT(s.train_idx == NULL, "ml_split_free: train_idx set to NULL");
    ASSERT(s.test_idx  == NULL, "ml_split_free: test_idx set to NULL");
}

/* =========================================================================
 * 7. Confusion matrix (Story 31.3.1)
 * ========================================================================= */

static void test_confusion_matrix(void)
{
    /* y_true=[1,1,0,0], y_pred=[1,0,1,0] -> tp=1, fn=1, fp=1, tn=1 */
    uint64_t y_true[] = {1, 1, 0, 0};
    uint64_t y_pred[] = {1, 0, 1, 0};
    ConfusionMatrix cm = ml_confusion_matrix(y_true, y_pred, 4);
    ASSERT(cm.tp == 1, "confusion_matrix: tp==1");
    ASSERT(cm.tn == 1, "confusion_matrix: tn==1");
    ASSERT(cm.fp == 1, "confusion_matrix: fp==1");
    ASSERT(cm.fn == 1, "confusion_matrix: fn==1");
}

/* =========================================================================
 * 8. Accuracy (Story 31.3.1)
 * ========================================================================= */

static void test_accuracy(void)
{
    /* y_true=[1,1,0,0], y_pred=[1,0,0,0] -> 3/4 correct = 0.75 */
    uint64_t y_true[] = {1, 1, 0, 0};
    uint64_t y_pred[] = {1, 0, 0, 0};
    double acc = ml_accuracy(y_true, y_pred, 4);
    ASSERT_NEAR(acc, 0.75, 1e-10, "accuracy([1,1,0,0],[1,0,0,0])==0.75");
}

/* =========================================================================
 * 9. Precision / recall / F1 (Story 31.3.1)
 * ========================================================================= */

static void test_prf1_perfect(void)
{
    /* Perfect predictor: precision=1, recall=1, f1=1. */
    uint64_t y_true[] = {1, 1, 0, 0};
    uint64_t y_pred[] = {1, 1, 0, 0};
    PRF1Result r = ml_precision_recall_f1(y_true, y_pred, 4);
    ASSERT_NEAR(r.precision, 1.0, 1e-10, "prf1 perfect: precision==1.0");
    ASSERT_NEAR(r.recall,    1.0, 1e-10, "prf1 perfect: recall==1.0");
    ASSERT_NEAR(r.f1,        1.0, 1e-10, "prf1 perfect: f1==1.0");
}

static void test_prf1_zero_division(void)
{
    /* All true negatives: TP=0, FP=0, FN=0 -> precision and recall are 0. */
    uint64_t y_true[] = {0, 0, 0};
    uint64_t y_pred[] = {0, 0, 0};
    PRF1Result r = ml_precision_recall_f1(y_true, y_pred, 3);
    ASSERT_NEAR(r.precision, 0.0, 1e-10, "prf1 zero-div: precision==0.0");
    ASSERT_NEAR(r.recall,    0.0, 1e-10, "prf1 zero-div: recall==0.0");
    ASSERT_NEAR(r.f1,        0.0, 1e-10, "prf1 zero-div: f1==0.0");
}

/* =========================================================================
 * 10. Standardize (Story 31.3.1)
 * ========================================================================= */

static void test_standardize(void)
{
    double xs[] = {1.0, 2.0, 3.0, 4.0, 5.0};
    double *z = ml_standardize(xs, 5);
    ASSERT(z != NULL, "standardize: returns non-NULL");

    /* Mean of z-scores must be ~0, stddev ~1. */
    double mean = 0.0;
    for (int i = 0; i < 5; i++) mean += z[i];
    mean /= 5.0;
    ASSERT_NEAR(mean, 0.0, 1e-10, "standardize: mean of z-scores ~0");

    double var = 0.0;
    for (int i = 0; i < 5; i++) { double d = z[i] - mean; var += d * d; }
    var /= 5.0;
    ASSERT_NEAR(var, 1.0, 1e-10, "standardize: variance of z-scores ~1");

    free(z);
}

static void test_standardize_constant(void)
{
    /* Constant input: stddev=0 -> all zeros. */
    double xs[] = {7.0, 7.0, 7.0};
    double *z = ml_standardize(xs, 3);
    ASSERT(z != NULL, "standardize constant: returns non-NULL");
    ASSERT_NEAR(z[0], 0.0, 1e-10, "standardize constant: output is 0.0");
    ASSERT_NEAR(z[1], 0.0, 1e-10, "standardize constant: output is 0.0 (idx 1)");
    free(z);
}

/* =========================================================================
 * 11. Normalize (Story 31.3.1)
 * ========================================================================= */

static void test_normalize(void)
{
    double xs[] = {0.0, 5.0, 10.0};
    double *n = ml_normalize(xs, 3);
    ASSERT(n != NULL, "normalize: returns non-NULL");
    ASSERT_NEAR(n[0], 0.0, 1e-10, "normalize [0,5,10]: n[0]==0.0");
    ASSERT_NEAR(n[1], 0.5, 1e-10, "normalize [0,5,10]: n[1]==0.5");
    ASSERT_NEAR(n[2], 1.0, 1e-10, "normalize [0,5,10]: n[2]==1.0");
    free(n);
}

static void test_normalize_constant(void)
{
    /* Constant input: max==min -> all zeros. */
    double xs[] = {3.0, 3.0, 3.0};
    double *n = ml_normalize(xs, 3);
    ASSERT(n != NULL, "normalize constant: returns non-NULL");
    ASSERT_NEAR(n[0], 0.0, 1e-10, "normalize constant: output is 0.0");
    free(n);
}

/* =========================================================================
 * 12. Cross-validation split (Story 31.3.2)
 * ========================================================================= */

static void test_cross_val_split_10_5(void)
{
    /*
     * 10 samples, 5 folds, seed=42.
     * Each fold should have exactly 2 indices.
     * All indices 0-9 should appear exactly once across all folds.
     */
    CrossValSplit s = ml_cross_validation_split(10, 5, 42);

    ASSERT(s.k == 5, "crossval(10,5,42): s.k == 5");
    ASSERT(s.folds != NULL, "crossval(10,5,42): folds != NULL");
    ASSERT(s.fold_sizes != NULL, "crossval(10,5,42): fold_sizes != NULL");

    /* Each fold size = 10/5 = 2. */
    int sizes_ok = 1;
    for (uint64_t fi = 0; fi < 5; fi++) {
        if (s.fold_sizes[fi] != 2) { sizes_ok = 0; break; }
    }
    ASSERT(sizes_ok, "crossval(10,5,42): each fold has size 2");

    /* Count how many times each index [0..9] appears. */
    int seen[10];
    for (int i = 0; i < 10; i++) seen[i] = 0;
    for (uint64_t fi = 0; fi < 5; fi++) {
        for (uint64_t j = 0; j < s.fold_sizes[fi]; j++) {
            uint64_t idx = s.folds[fi][j];
            if (idx < 10) seen[idx]++;
        }
    }
    int all_once = 1;
    for (int i = 0; i < 10; i++) {
        if (seen[i] != 1) { all_once = 0; break; }
    }
    ASSERT(all_once, "crossval(10,5,42): all indices appear exactly once");

    ml_cross_validation_free(&s);
}

static void test_cross_val_split_7_3(void)
{
    /*
     * 7 samples, 3 folds, seed=1.
     * fold_sizes: 7/3 = 2 rem 1 -> [3, 2, 2] (first fold gets the extra).
     * Total must equal 7.
     */
    CrossValSplit s = ml_cross_validation_split(7, 3, 1);

    ASSERT(s.k == 3, "crossval(7,3,1): s.k == 3");

    uint64_t total = 0;
    for (uint64_t fi = 0; fi < 3; fi++) {
        total += s.fold_sizes[fi];
    }
    ASSERT(total == 7, "crossval(7,3,1): fold sizes sum to 7");

    /* Fold 0 has the extra element (3), rest have 2. */
    ASSERT(s.fold_sizes[0] == 3, "crossval(7,3,1): fold 0 size == 3");
    ASSERT(s.fold_sizes[1] == 2, "crossval(7,3,1): fold 1 size == 2");
    ASSERT(s.fold_sizes[2] == 2, "crossval(7,3,1): fold 2 size == 2");

    ml_cross_validation_free(&s);
}

/* =========================================================================
 * 13. Random forest (Story 31.3.2)
 * ========================================================================= */

static void test_random_forest_fit_predict(void)
{
    /*
     * Linearly separable 2D data (same structure as the dtree stump test):
     *   x0 < 5 -> class 0, x0 >= 5 -> class 1
     * Use 12 training points so bootstrap sampling has enough variety.
     */
    double f0_data[] = {1.0, 1.5, 2.0, 2.5, 3.0, 3.5,
                        6.0, 6.5, 7.0, 7.5, 8.0, 8.5};
    double f1_data[] = {0.1, 0.2, 0.3, 0.4, 0.5, 0.6,
                        0.7, 0.8, 0.9, 1.0, 1.1, 1.2};
    double lbl_data[]= {0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                        1.0, 1.0, 1.0, 1.0, 1.0, 1.0};

    F64Array feat_cols[2];
    feat_cols[0].data = f0_data; feat_cols[0].len = 12;
    feat_cols[1].data = f1_data; feat_cols[1].len = 12;

    F64Array labels;
    labels.data = lbl_data;
    labels.len  = 12;

    TkRandomForest *forest = ml_random_forest_fit(feat_cols, 2, labels,
                                                   10, 3, 99);
    ASSERT(forest != NULL, "random_forest_fit: returns non-NULL");

    /* Predict on the training points and count correct. */
    double test_pts[12][2] = {
        {1.0,0.1},{1.5,0.2},{2.0,0.3},{2.5,0.4},{3.0,0.5},{3.5,0.6},
        {6.0,0.7},{6.5,0.8},{7.0,0.9},{7.5,1.0},{8.0,1.1},{8.5,1.2}
    };
    double expected[] = {0,0,0,0,0,0, 1,1,1,1,1,1};
    int correct = 0;
    for (int i = 0; i < 12; i++) {
        double pred = ml_random_forest_predict(forest, test_pts[i]);
        if (fabs(pred - expected[i]) < 0.5) correct++;
    }
    /* Require >= 80% accuracy (>=10/12). */
    ASSERT(correct >= 10, "random_forest_predict: accuracy >= 80% on linearly separable data");

    ml_random_forest_free(forest);
}

static void test_random_forest_no_crash(void)
{
    /* Minimal: 4-point XOR dataset, single tree.  Just verifies no crash. */
    double f0_data[] = {0.0, 0.0, 1.0, 1.0};
    double f1_data[] = {0.0, 1.0, 0.0, 1.0};
    double lbl_data[] = {0.0, 1.0, 1.0, 0.0};

    F64Array feat_cols[2];
    feat_cols[0].data = f0_data; feat_cols[0].len = 4;
    feat_cols[1].data = f1_data; feat_cols[1].len = 4;

    F64Array labels;
    labels.data = lbl_data;
    labels.len  = 4;

    TkRandomForest *forest = ml_random_forest_fit(feat_cols, 2, labels,
                                                   1, 3, 7);
    ASSERT(forest != NULL, "random_forest no_crash: fit returns non-NULL");

    double pt[] = {0.0, 1.0};
    double pred = ml_random_forest_predict(forest, pt);
    (void)pred; /* result value not asserted — just must not crash */
    ASSERT(1, "random_forest no_crash: predict does not crash");

    ml_random_forest_free(forest);
    ASSERT(1, "random_forest no_crash: free does not crash");
}

static void test_random_forest_free_null(void)
{
    /* Calling free on NULL must not crash. */
    ml_random_forest_free(NULL);
    ASSERT(1, "random_forest_free(NULL): does not crash");
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    /* Original tests */
    test_linreg();
    test_kmeans();
    test_dtree();
    test_knn();

    /* Story 20.1.9: hardened tests */
    test_linreg_single_point();
    test_linreg_collinear();
    test_linreg_duplicate_points();
    test_linreg_empty();
    test_kmeans_k_greater_than_n();
    test_kmeans_k_equals_1();
    test_kmeans_single_point_clusters();
    test_kmeans_convergence_well_separated();
    test_dtree_stump();
    test_dtree_identical_features();
    test_knn_k_equals_ntrain();
    test_knn_k1_nearest_neighbour();
    test_knn_iris_like_3class();
    test_knn_empty_training_set();
    test_knn_k_greater_than_ntrain();

    /* Story 31.3.1: train/test split and evaluation metrics */
    test_train_test_split();
    test_confusion_matrix();
    test_accuracy();
    test_prf1_perfect();
    test_prf1_zero_division();
    test_standardize();
    test_standardize_constant();
    test_normalize();
    test_normalize_constant();

    /* Story 31.3.2: cross-validation and random forest */
    test_cross_val_split_10_5();
    test_cross_val_split_7_3();
    test_random_forest_fit_predict();
    test_random_forest_no_crash();
    test_random_forest_free_null();

    if (failures == 0) {
        printf("All ml tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}

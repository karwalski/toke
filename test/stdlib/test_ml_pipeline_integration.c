/*
 * test_ml_pipeline_integration.c — Real-world integration test: ML prediction
 * pipeline exercising CSV → train → predict → evaluate accuracy.
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Werror -lm \
 *      -I../../src/stdlib \
 *      ../../src/stdlib/ml.c ../../src/stdlib/csv.c ../../src/stdlib/str.c \
 *      ../../src/stdlib/dataframe.c \
 *      test_ml_pipeline_integration.c -o test_ml_pipeline_integration
 *
 * Scenarios:
 *   1. Synthetic linearly-separable classification (100 rows, 2 features,
 *      2 classes) — CSV parse → dtree train → predict → accuracy > 70%
 *   2. Same data with KNN (k=3) — predict → compare accuracy
 *   3. Linear regression on y = 2x + 1 — verify slope ≈ 2, intercept ≈ 1
 *
 * Story: 21.1.7
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "../../src/stdlib/ml.h"
#include "../../src/stdlib/csv.h"
#include "../../src/stdlib/dataframe.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_NEAR(a, b, eps, msg) \
    ASSERT(fabs((a) - (b)) < (eps), msg)

/* =========================================================================
 * Helper: generate a linearly-separable CSV dataset
 *
 * 100 rows, columns: f1, f2, label
 * Class 0: f1 in [0,5), f2 in [0,5)   (f1+f2 < 5  => label 0)
 * Class 1: f1 in [5,10), f2 in [5,10) (f1+f2 >= 10 => label 1)
 *
 * Uses a simple deterministic PRNG (LCG) for reproducibility.
 * ========================================================================= */

static uint32_t lcg_state = 42;

static double lcg_rand(void)
{
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (double)(lcg_state >> 16) / 65536.0; /* 0..1 */
}

static char *generate_classification_csv(void)
{
    /* Pre-allocate a generous buffer (100 rows * ~30 chars each + header). */
    char *buf = (char *)malloc(8192);
    if (!buf) return NULL;
    int off = 0;
    off += sprintf(buf + off, "f1,f2,label\n");

    lcg_state = 42; /* reset PRNG */

    /* 50 class-0 points: f1 in [0,4], f2 in [0,4] */
    for (int i = 0; i < 50; i++) {
        double f1 = lcg_rand() * 4.0;
        double f2 = lcg_rand() * 4.0;
        off += sprintf(buf + off, "%.6f,%.6f,0\n", f1, f2);
    }
    /* 50 class-1 points: f1 in [6,10], f2 in [6,10] */
    for (int i = 0; i < 50; i++) {
        double f1 = 6.0 + lcg_rand() * 4.0;
        double f2 = 6.0 + lcg_rand() * 4.0;
        off += sprintf(buf + off, "%.6f,%.6f,1\n", f1, f2);
    }
    return buf;
}

/* =========================================================================
 * Helper: generate a regression CSV dataset (y = 2x + 1, slight noise)
 * ========================================================================= */

static char *generate_regression_csv(void)
{
    char *buf = (char *)malloc(4096);
    if (!buf) return NULL;
    int off = 0;
    off += sprintf(buf + off, "x,y\n");

    lcg_state = 7; /* reset PRNG */

    for (int i = 0; i < 50; i++) {
        double x = (double)i * 0.5;
        double noise = (lcg_rand() - 0.5) * 0.2; /* tiny noise */
        double y = 2.0 * x + 1.0 + noise;
        off += sprintf(buf + off, "%.6f,%.6f\n", x, y);
    }
    return buf;
}

/* =========================================================================
 * Test 1: CSV → DataFrame → Decision tree → Accuracy
 * ========================================================================= */

static void test_dtree_pipeline(void)
{
    printf("\n--- Decision tree pipeline ---\n");

    /* Step 1: Generate CSV and parse via dataframe. */
    char *csv = generate_classification_csv();
    ASSERT(csv != NULL, "CSV generation succeeded");

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(res.is_err == 0, "df_fromcsv parsed classification CSV");
    ASSERT(res.ok != NULL, "dataframe is non-NULL");

    TkDataframe *df = res.ok;

    uint64_t nrows_out, ncols_out;
    df_shape(df, &nrows_out, &ncols_out);
    ASSERT(nrows_out == 100, "dataframe has 100 rows");
    ASSERT(ncols_out == 3,   "dataframe has 3 columns");

    /* Step 2: Extract columns into F64Arrays. */
    DfCol *col_f1    = df_column(df, "f1");
    DfCol *col_f2    = df_column(df, "f2");
    DfCol *col_label = df_column(df, "label");
    ASSERT(col_f1 != NULL,    "found column f1");
    ASSERT(col_f2 != NULL,    "found column f2");
    ASSERT(col_label != NULL, "found column label");

    /* Build column arrays for ML (train: first 80, test: last 20). */
    uint64_t ntrain = 80;
    uint64_t ntest  = 20;

    F64Array train_cols[2];
    train_cols[0] = (F64Array){ col_f1->f64_data,  ntrain };
    train_cols[1] = (F64Array){ col_f2->f64_data,  ntrain };
    double *train_labels = col_label->f64_data; /* first 80 */

    /* Step 3: Train a decision tree. */
    DTreeModel dtree = ml_dtreefit(train_cols, 2, train_labels, ntrain, 5);
    ASSERT(dtree.nnodes > 0, "dtree has at least one node");

    /* Step 4: Predict on test set and compute accuracy. */
    int correct = 0;
    for (uint64_t i = 0; i < ntest; i++) {
        uint64_t row = ntrain + i;
        double point[2] = { col_f1->f64_data[row], col_f2->f64_data[row] };
        double pred = ml_dtreepredict(&dtree, point);
        double actual = col_label->f64_data[row];
        if (fabs(pred - actual) < 0.5) correct++;
    }
    double accuracy = (double)correct / (double)ntest;
    printf("  dtree accuracy: %.1f%% (%d/%d)\n", accuracy * 100.0,
           correct, (int)ntest);
    ASSERT(accuracy > 0.70,
           "dtree accuracy > 70% on linearly-separable data");

    ml_dtree_free(&dtree);
    df_free(df);
    free(csv);
}

/* =========================================================================
 * Test 2: KNN (k=3) on same classification data
 * ========================================================================= */

static void test_knn_pipeline(void)
{
    printf("\n--- KNN pipeline ---\n");

    char *csv = generate_classification_csv();
    ASSERT(csv != NULL, "KNN: CSV generation succeeded");

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(res.is_err == 0, "KNN: df_fromcsv succeeded");
    TkDataframe *df = res.ok;

    DfCol *col_f1    = df_column(df, "f1");
    DfCol *col_f2    = df_column(df, "f2");
    DfCol *col_label = df_column(df, "label");

    uint64_t ntrain = 80;
    uint64_t ntest  = 20;

    F64Array train_cols[2];
    train_cols[0] = (F64Array){ col_f1->f64_data,  ntrain };
    train_cols[1] = (F64Array){ col_f2->f64_data,  ntrain };
    double *train_labels = col_label->f64_data;

    /* Predict with KNN (k=3). */
    int correct = 0;
    for (uint64_t i = 0; i < ntest; i++) {
        uint64_t row = ntrain + i;
        double point[2] = { col_f1->f64_data[row], col_f2->f64_data[row] };
        double pred = ml_knnpredict(train_cols, 2, train_labels, ntrain,
                                    point, 3);
        double actual = col_label->f64_data[row];
        if (fabs(pred - actual) < 0.5) correct++;
    }
    double knn_accuracy = (double)correct / (double)ntest;
    printf("  knn(k=3) accuracy: %.1f%% (%d/%d)\n", knn_accuracy * 100.0,
           correct, (int)ntest);
    ASSERT(knn_accuracy > 0.70,
           "knn accuracy > 70% on linearly-separable data");

    df_free(df);
    free(csv);
}

/* =========================================================================
 * Test 3: Linear regression on y = 2x + 1
 * ========================================================================= */

static void test_linreg_pipeline(void)
{
    printf("\n--- Linear regression pipeline ---\n");

    char *csv = generate_regression_csv();
    ASSERT(csv != NULL, "linreg: CSV generation succeeded");

    DfResult res = df_fromcsv(csv, (uint64_t)strlen(csv), 1);
    ASSERT(res.is_err == 0, "linreg: df_fromcsv succeeded");
    TkDataframe *df = res.ok;

    uint64_t nrows_out, ncols_out;
    df_shape(df, &nrows_out, &ncols_out);
    ASSERT(nrows_out == 50, "regression dataframe has 50 rows");
    ASSERT(ncols_out == 2,  "regression dataframe has 2 columns");

    DfCol *col_x = df_column(df, "x");
    DfCol *col_y = df_column(df, "y");
    ASSERT(col_x != NULL, "found column x");
    ASSERT(col_y != NULL, "found column y");

    F64Array xs = { col_x->f64_data, nrows_out };
    F64Array ys = { col_y->f64_data, nrows_out };

    LinearModel m = ml_linregfit(xs, ys);

    printf("  slope=%.6f  intercept=%.6f\n", m.slope, m.intercept);
    ASSERT_NEAR(m.slope, 2.0, 0.1,
                "linreg slope ~ 2.0 (within 0.1)");
    ASSERT_NEAR(m.intercept, 1.0, 0.5,
                "linreg intercept ~ 1.0 (within 0.5)");

    /* Spot-check a prediction. */
    double pred = ml_linregpredict(m, 10.0);
    ASSERT_NEAR(pred, 21.0, 1.5,
                "linregpredict(10.0) ~ 21.0 (within 1.5)");

    df_free(df);
    free(csv);
}

/* ========================================================================= */

int main(void)
{
    printf("=== ML Pipeline Integration Tests (Story 21.1.7) ===\n");

    test_dtree_pipeline();
    test_knn_pipeline();
    test_linreg_pipeline();

    printf("\n=== %s (%d failure%s) ===\n",
           failures ? "FAIL" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}

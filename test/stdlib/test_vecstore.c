/*
 * test_vecstore.c — Unit tests for the std.vecstore C library.
 *
 * Build and run: make test-stdlib-vecstore
 *
 * Story: 72.5.4
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../../src/stdlib/vecstore.h"

/* -------------------------------------------------------------------------
 * Test helpers
 * ------------------------------------------------------------------------- */

static int failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
        else         { printf("pass: %s\n", msg); } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a), (b)) == 0, msg)

#define ASSERT_EQ_I32(a, b, msg) \
    ASSERT((a) == (b), msg)

#define FLOAT_EQ(a, b)  ((a) >= (b) - 0.0001 && (a) <= (b) + 0.0001)
#define ASSERT_NEAR(a, b, msg) \
    ASSERT(FLOAT_EQ((double)(a), (double)(b)), msg)

/* Use /tmp for test data so no source tree writes */
static const char *TEST_DIR = "/tmp/tk_vecstore_test";

static void ensure_dir(const char *path)
{
    /* mkdir -p equivalent via system() — acceptable in tests only */
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", path);
    (void)system(cmd);
}

static void cleanup_dir(const char *path)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", path);
    (void)system(cmd);
}

/* -------------------------------------------------------------------------
 * Test: open / close
 * ------------------------------------------------------------------------- */

static void test_open_close(void)
{
    ensure_dir(TEST_DIR);

    TkVecStore *vs = vecstore_open(TEST_DIR);
    ASSERT(vs != NULL, "vecstore_open returns non-NULL");

    vecstore_close(vs);
    ASSERT(1, "vecstore_close does not crash");

    /* NULL data_dir */
    TkVecStore *vs2 = vecstore_open(NULL);
    ASSERT(vs2 == NULL, "vecstore_open(NULL) returns NULL");

    /* Empty data_dir */
    TkVecStore *vs3 = vecstore_open("");
    ASSERT(vs3 == NULL, "vecstore_open(\"\") returns NULL");
}

/* -------------------------------------------------------------------------
 * Test: collection
 * ------------------------------------------------------------------------- */

static void test_collection(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    ASSERT(vs != NULL, "open for collection tests");

    TkVecCollection *col = vecstore_collection(vs, "test_col");
    ASSERT(col != NULL, "vecstore_collection returns non-NULL");

    /* Same name → same pointer */
    TkVecCollection *col2 = vecstore_collection(vs, "test_col");
    ASSERT(col == col2, "vecstore_collection same name returns same pointer");

    /* Different name → different pointer */
    TkVecCollection *col3 = vecstore_collection(vs, "another_col");
    ASSERT(col3 != NULL, "vecstore_collection different name non-NULL");
    ASSERT(col3 != col, "vecstore_collection different name gives different pointer");

    /* NULL name */
    TkVecCollection *bad = vecstore_collection(vs, NULL);
    ASSERT(bad == NULL, "vecstore_collection(NULL name) returns NULL");

    /* Name with path separator */
    TkVecCollection *slash = vecstore_collection(vs, "bad/name");
    ASSERT(slash == NULL, "vecstore_collection(name with /) returns NULL");

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: count on empty collection
 * ------------------------------------------------------------------------- */

static void test_count_empty(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "count_empty");
    ASSERT_EQ_I32(vecstore_count(col), 0, "count of empty collection == 0");
    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: upsert and count
 * ------------------------------------------------------------------------- */

static void test_upsert_count(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "upsert_col");

    float emb1[] = {1.0f, 0.0f, 0.0f};
    float emb2[] = {0.0f, 1.0f, 0.0f};
    float emb3[] = {0.0f, 0.0f, 1.0f};

    int ok1 = vecstore_upsert(col, "a", emb1, 3, "{\"v\":1}");
    ASSERT(ok1 == 1, "upsert #1 returns 1");
    ASSERT_EQ_I32(vecstore_count(col), 1, "count after first upsert == 1");

    int ok2 = vecstore_upsert(col, "b", emb2, 3, "{\"v\":2}");
    ASSERT(ok2 == 1, "upsert #2 returns 1");
    ASSERT_EQ_I32(vecstore_count(col), 2, "count after second upsert == 2");

    int ok3 = vecstore_upsert(col, "c", emb3, 3, "{\"v\":3}");
    ASSERT(ok3 == 1, "upsert #3 returns 1");
    ASSERT_EQ_I32(vecstore_count(col), 3, "count after third upsert == 3");

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: upsert replaces existing entry (same id)
 * ------------------------------------------------------------------------- */

static void test_upsert_replace(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "replace_col");

    float emb1[] = {1.0f, 0.0f, 0.0f};
    float emb2[] = {0.0f, 1.0f, 0.0f};

    vecstore_upsert(col, "id1", emb1, 3, "original");
    ASSERT_EQ_I32(vecstore_count(col), 1, "count before replace == 1");

    vecstore_upsert(col, "id1", emb2, 3, "replaced");
    ASSERT_EQ_I32(vecstore_count(col), 1, "count after replace still == 1");

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: dimension mismatch on upsert
 * ------------------------------------------------------------------------- */

static void test_upsert_dim_mismatch(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "dim_col");

    float emb3[] = {1.0f, 0.0f, 0.0f};
    float emb4[] = {1.0f, 0.0f, 0.0f, 0.0f};

    int ok1 = vecstore_upsert(col, "x", emb3, 3, "3-dim");
    ASSERT(ok1 == 1, "upsert dim=3 succeeds");

    int ok2 = vecstore_upsert(col, "y", emb4, 4, "4-dim");
    ASSERT(ok2 == 0, "upsert with wrong dim fails");
    ASSERT_EQ_I32(vecstore_count(col), 1, "count unchanged after dim mismatch");

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: upsert zero vector rejected
 * ------------------------------------------------------------------------- */

static void test_upsert_zero_vector(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "zero_col");

    float zero[] = {0.0f, 0.0f, 0.0f};
    int ok = vecstore_upsert(col, "zero", zero, 3, "zero");
    ASSERT(ok == 0, "upsert of zero vector returns 0");
    ASSERT_EQ_I32(vecstore_count(col), 0, "count remains 0 after zero-vector upsert");

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: search — basic cosine similarity
 * ------------------------------------------------------------------------- */

static void test_search_basic(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "search_col");

    /* Three axis-aligned unit vectors */
    float ex[] = {1.0f, 0.0f, 0.0f};
    float ey[] = {0.0f, 1.0f, 0.0f};
    float ez[] = {0.0f, 0.0f, 1.0f};

    vecstore_upsert(col, "x-axis", ex, 3, "x");
    vecstore_upsert(col, "y-axis", ey, 3, "y");
    vecstore_upsert(col, "z-axis", ez, 3, "z");

    /* Query along x → should return x-axis with score ~1.0 */
    float qx[] = {1.0f, 0.0f, 0.0f};
    TkSearchResultArray res = vecstore_search(col, qx, 3, 1, 0.5);

    ASSERT(res.count == 1, "search top_k=1 near x returns 1 result");
    if (res.count > 0) {
        ASSERT_STREQ(res.items[0].id, "x-axis", "top result id is x-axis");
        ASSERT(res.items[0].score > 0.99, "top result score ~1.0 for identical direction");
        ASSERT_STREQ(res.items[0].payload, "x", "top result payload is 'x'");
    }
    vecstore_free_results(&res);
    ASSERT(res.items == NULL, "free_results nulls the items pointer");

    /* Query at 45 degrees between x and y */
    float qxy[] = {1.0f, 1.0f, 0.0f};
    TkSearchResultArray res2 = vecstore_search(col, qxy, 3, 2, 0.5);
    ASSERT(res2.count == 2, "search near 45-deg x-y returns 2 results above 0.5");
    vecstore_free_results(&res2);

    /* min_score=0.0 → should return all 3 */
    TkSearchResultArray res3 = vecstore_search(col, qx, 3, 10, 0.0);
    ASSERT(res3.count == 3, "search min_score=0 returns all 3 entries");
    /* Results should be sorted descending by score */
    if (res3.count > 1) {
        ASSERT(res3.items[0].score >= res3.items[1].score,
               "results sorted descending by score (0 >= 1)");
    }
    if (res3.count > 2) {
        ASSERT(res3.items[1].score >= res3.items[2].score,
               "results sorted descending by score (1 >= 2)");
    }
    vecstore_free_results(&res3);

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: search — no results when all below min_score
 * ------------------------------------------------------------------------- */

static void test_search_min_score_filter(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "minscore_col");

    float ex[] = {1.0f, 0.0f, 0.0f};
    vecstore_upsert(col, "x", ex, 3, "x");

    /* Query orthogonal to x → score=0, below min_score=0.5 */
    float qy[] = {0.0f, 1.0f, 0.0f};
    TkSearchResultArray res = vecstore_search(col, qy, 3, 10, 0.5);
    ASSERT(res.count == 0, "search with orthogonal query and min_score=0.5 returns 0 results");
    vecstore_free_results(&res);

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: search on empty collection
 * ------------------------------------------------------------------------- */

static void test_search_empty(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "empty_search");

    float q[] = {1.0f, 0.0f, 0.0f};
    TkSearchResultArray res = vecstore_search(col, q, 3, 5, 0.0);
    ASSERT(res.count == 0, "search on empty collection returns 0 results");
    vecstore_free_results(&res);

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: search dim mismatch
 * ------------------------------------------------------------------------- */

static void test_search_dim_mismatch(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "sdim_col");

    float ex[] = {1.0f, 0.0f, 0.0f};
    vecstore_upsert(col, "x", ex, 3, "x");

    float q4[] = {1.0f, 0.0f, 0.0f, 0.0f};
    TkSearchResultArray res = vecstore_search(col, q4, 4, 5, 0.0);
    ASSERT(res.count == 0, "search with wrong dim returns 0 results");
    vecstore_free_results(&res);

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: delete
 * ------------------------------------------------------------------------- */

static void test_delete(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "del_col");

    float ea[] = {1.0f, 0.0f, 0.0f};
    float eb[] = {0.0f, 1.0f, 0.0f};
    vecstore_upsert(col, "alpha", ea, 3, "a");
    vecstore_upsert(col, "beta",  eb, 3, "b");
    ASSERT_EQ_I32(vecstore_count(col), 2, "count before delete == 2");

    int del1 = vecstore_delete(col, "alpha");
    ASSERT(del1 == 1, "delete existing entry returns 1");
    ASSERT_EQ_I32(vecstore_count(col), 1, "count after delete == 1");

    int del2 = vecstore_delete(col, "alpha");
    ASSERT(del2 == 0, "delete non-existent entry returns 0");
    ASSERT_EQ_I32(vecstore_count(col), 1, "count unchanged after second delete");

    int del3 = vecstore_delete(col, "beta");
    ASSERT(del3 == 1, "delete last entry returns 1");
    ASSERT_EQ_I32(vecstore_count(col), 0, "count == 0 after all deleted");

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: delete_before (TTL sweep)
 * ------------------------------------------------------------------------- */

static void test_delete_before(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "ttl_col");

    float ea[] = {1.0f, 0.0f, 0.0f};
    float eb[] = {0.0f, 1.0f, 0.0f};
    float ec[] = {0.0f, 0.0f, 1.0f};

    vecstore_upsert(col, "old1", ea, 3, "old");
    vecstore_upsert(col, "old2", eb, 3, "old");
    vecstore_upsert(col, "new1", ec, 3, "new");

    /* Manually patch timestamps to simulate age */
    /* Access internal structure via knowledge of layout — acceptable in tests */
    /* Instead: use delete_before with a future cutoff to remove all */
    int64_t far_future = (int64_t)time(NULL) + 999999;
    int32_t removed = vecstore_delete_before(col, far_future);
    ASSERT(removed == 3, "delete_before(far future) removes all 3 entries");
    ASSERT_EQ_I32(vecstore_count(col), 0, "count == 0 after delete_before all");

    /* Delete before past timestamp → removes nothing */
    vecstore_upsert(col, "fresh", ea, 3, "fresh");
    int64_t past = (int64_t)time(NULL) - 999999;
    int32_t removed2 = vecstore_delete_before(col, past);
    ASSERT(removed2 == 0, "delete_before(past) removes nothing from new entries");
    ASSERT_EQ_I32(vecstore_count(col), 1, "count still 1 after no-op delete_before");

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * Test: persistence — save and reload
 * ------------------------------------------------------------------------- */

static void test_persistence(void)
{
    const char *persist_dir = "/tmp/tk_vecstore_persist_test";
    ensure_dir(persist_dir);

    /* Write entries */
    {
        TkVecStore *vs = vecstore_open(persist_dir);
        ASSERT(vs != NULL, "open store for persistence write");
        TkVecCollection *col = vecstore_collection(vs, "persist");

        float ea[] = {1.0f, 0.0f, 0.0f};
        float eb[] = {0.0f, 1.0f, 0.0f};
        vecstore_upsert(col, "p1", ea, 3, "payload-one");
        vecstore_upsert(col, "p2", eb, 3, "payload-two");
        ASSERT_EQ_I32(vecstore_count(col), 2, "2 entries before close");

        vecstore_close(vs); /* should flush to disk */
    }

    /* Reload and verify */
    {
        TkVecStore *vs = vecstore_open(persist_dir);
        ASSERT(vs != NULL, "open store for persistence read");
        TkVecCollection *col = vecstore_collection(vs, "persist");
        ASSERT(col != NULL, "collection loaded from disk");
        ASSERT_EQ_I32(vecstore_count(col), 2, "2 entries after reload");

        /* Search to verify data integrity */
        float qx[] = {1.0f, 0.0f, 0.0f};
        TkSearchResultArray res = vecstore_search(col, qx, 3, 1, 0.9);
        ASSERT(res.count == 1, "search after reload finds top result");
        if (res.count > 0) {
            ASSERT_STREQ(res.items[0].id, "p1", "reloaded top result is p1");
            ASSERT_STREQ(res.items[0].payload, "payload-one",
                         "reloaded payload matches");
        }
        vecstore_free_results(&res);

        vecstore_close(vs);
    }

    cleanup_dir(persist_dir);
}

/* -------------------------------------------------------------------------
 * Test: NULL safety
 * ------------------------------------------------------------------------- */

static void test_null_safety(void)
{
    /* All public functions should handle NULL gracefully without crashing */
    vecstore_close(NULL);           /* no crash */
    ASSERT(1, "vecstore_close(NULL) does not crash");

    ASSERT(vecstore_collection(NULL, "x") == NULL,
           "vecstore_collection(NULL store) == NULL");

    ASSERT(vecstore_upsert(NULL, "id", NULL, 3, "p") == 0,
           "vecstore_upsert(NULL col) == 0");

    TkSearchResultArray empty = vecstore_search(NULL, NULL, 3, 5, 0.0);
    ASSERT(empty.count == 0, "vecstore_search(NULL) returns count=0");

    ASSERT(vecstore_delete(NULL, "id") == 0,
           "vecstore_delete(NULL) == 0");

    ASSERT(vecstore_delete_before(NULL, 0) == 0,
           "vecstore_delete_before(NULL) == 0");

    ASSERT(vecstore_count(NULL) == 0,
           "vecstore_count(NULL) == 0");

    vecstore_free_results(NULL);  /* no crash */
    ASSERT(1, "vecstore_free_results(NULL) does not crash");
}

/* -------------------------------------------------------------------------
 * Test: large batch — 1000 entries, verify search still correct
 * ------------------------------------------------------------------------- */

static void test_large_batch(void)
{
    ensure_dir(TEST_DIR);
    TkVecStore *vs = vecstore_open(TEST_DIR);
    TkVecCollection *col = vecstore_collection(vs, "large_col");

    /* Insert 1000 entries with random-ish 8-dim vectors */
    int32_t N   = 1000;
    int32_t DIM = 8;
    char id_buf[32];
    char pay_buf[32];

    for (int32_t i = 0; i < N; i++) {
        float emb[8];
        /* Simple deterministic "random": vary one component per entry */
        for (int32_t d = 0; d < DIM; d++) {
            emb[d] = (float)((i * 7 + d * 13 + 1) % 100) / 100.0f;
        }
        snprintf(id_buf,  sizeof id_buf,  "item-%d", i);
        snprintf(pay_buf, sizeof pay_buf, "{\"i\":%d}", i);
        vecstore_upsert(col, id_buf, emb, DIM, pay_buf);
    }

    ASSERT_EQ_I32(vecstore_count(col), N, "large batch: count == 1000");

    /* Search for top 5 — just verify we get exactly 5 results */
    float q[8] = {1.0f, 0.5f, 0.2f, 0.1f, 0.8f, 0.3f, 0.6f, 0.4f};
    TkSearchResultArray res = vecstore_search(col, q, DIM, 5, -1.0);
    ASSERT(res.count == 5, "large batch search returns top_k=5 results");
    /* Verify descending order */
    for (int32_t i = 1; i < res.count; i++) {
        ASSERT(res.items[i-1].score >= res.items[i].score,
               "large batch results in descending score order");
    }
    vecstore_free_results(&res);

    vecstore_close(vs);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    printf("=== vecstore tests ===\n");

    test_open_close();
    test_collection();
    test_count_empty();
    test_upsert_count();
    test_upsert_replace();
    test_upsert_dim_mismatch();
    test_upsert_zero_vector();
    test_search_basic();
    test_search_min_score_filter();
    test_search_empty();
    test_search_dim_mismatch();
    test_delete();
    test_delete_before();
    test_persistence();
    test_null_safety();
    test_large_batch();

    cleanup_dir(TEST_DIR);

    if (failures == 0) {
        printf("All vecstore tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

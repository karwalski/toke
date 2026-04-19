/*
 * test_mlx.c — Unit tests for the std.mlx C library (Story 72.6.3).
 *
 * Tests cover all public API functions.  No real bridge server is required:
 * network-dependent tests expect a connection failure and verify that the
 * correct error path is taken.  The is_available() test verifies the
 * platform/sysctl detection logic.
 *
 * Build and run: make test-stdlib-mlx
 *
 * Test categories:
 *   1. Memory management — mlx_model_free, mlx_str_result_free,
 *                          mlx_embed_result_free on NULL and populated structs.
 *   2. is_available     — returns int (0 or 1), does not crash.
 *   3. mlx_load         — NULL/empty path returns error; bridge absent returns
 *                          error with non-NULL err_msg.
 *   4. mlx_unload       — NULL model, NULL id return 0; bridge absent returns 0.
 *   5. mlx_generate     — NULL model, NULL prompt, zero tokens all return error.
 *   6. mlx_embed        — NULL model, NULL text return error.
 *   7. Error result fields — is_err == 1 and err_msg != NULL on all failure paths.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../src/stdlib/mlx.h"

static int failures = 0;

/* -------------------------------------------------------------------------
 * Test macros
 * ------------------------------------------------------------------------- */

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL: %s\n", (msg)); \
            failures++; \
        } else { \
            printf("pass: %s\n", (msg)); \
        } \
    } while (0)

#define ASSERT_NULL(p, msg)    ASSERT((p) == NULL,  (msg))
#define ASSERT_NOTNULL(p, msg) ASSERT((p) != NULL,  (msg))
#define ASSERT_ZERO(v, msg)    ASSERT((v) == 0,     (msg))
#define ASSERT_NONZERO(v, msg) ASSERT((v) != 0,     (msg))
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, (msg))

/* =========================================================================
 * 1. Memory management
 * ========================================================================= */

static void test_memory(void)
{
    /* mlx_model_free: NULL pointer must not crash */
    mlx_model_free(NULL);
    ASSERT(1, "mlx_model_free(NULL) does not crash");

    /* mlx_model_free: zero-initialised struct must not crash */
    MlxModel m;
    memset(&m, 0, sizeof(m));
    mlx_model_free(&m);
    ASSERT(1, "mlx_model_free(zero-struct) does not crash");

    /* mlx_str_result_free: NULL pointer must not crash */
    mlx_str_result_free(NULL);
    ASSERT(1, "mlx_str_result_free(NULL) does not crash");

    /* mlx_str_result_free: error result */
    {
        MlxStrResult r;
        memset(&r, 0, sizeof(r));
        r.is_err  = 1;
        r.err_msg = (char *)malloc(8);
        if (r.err_msg) strcpy(r.err_msg, "errtest");
        mlx_str_result_free(&r);
        ASSERT(r.text == NULL && r.err_msg == NULL,
               "mlx_str_result_free: pointers NULLed after free");
    }

    /* mlx_embed_result_free: NULL pointer must not crash */
    mlx_embed_result_free(NULL);
    ASSERT(1, "mlx_embed_result_free(NULL) does not crash");

    /* mlx_embed_result_free: populated result */
    {
        MlxEmbedResult r;
        memset(&r, 0, sizeof(r));
        r.embedding.data = (float *)malloc(3 * sizeof(float));
        r.embedding.len  = 3;
        mlx_embed_result_free(&r);
        ASSERT(r.embedding.data == NULL && r.embedding.len == 0,
               "mlx_embed_result_free: data NULLed, len zeroed after free");
    }
}

/* =========================================================================
 * 2. mlx_is_available
 * ========================================================================= */

static void test_is_available(void)
{
    int avail = mlx_is_available();

    /* Must return 0 or 1 */
    ASSERT(avail == 0 || avail == 1, "mlx_is_available returns 0 or 1");

    /* Calling it twice must return the same value */
    int avail2 = mlx_is_available();
    ASSERT(avail == avail2, "mlx_is_available is stable across calls");

#ifndef __APPLE__
    ASSERT_ZERO(avail, "mlx_is_available is 0 on non-Apple platform");
#endif
}

/* =========================================================================
 * 3. mlx_load
 * ========================================================================= */

static void test_load(void)
{
    /* NULL path must return an error */
    {
        MlxModelResult r = mlx_load(NULL);
        ASSERT_NONZERO(r.is_err, "mlx_load(NULL): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_load(NULL): err_msg non-NULL");
        mlx_model_free(&r.model);
        free(r.err_msg);
    }

    /* Empty path must return an error */
    {
        MlxModelResult r = mlx_load("");
        ASSERT_NONZERO(r.is_err, "mlx_load(''): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_load(''): err_msg non-NULL");
        mlx_model_free(&r.model);
        free(r.err_msg);
    }

    /* Valid path but bridge absent (or non-Apple) must return an error */
    {
        MlxModelResult r = mlx_load("/tmp/nonexistent-model");
        ASSERT_NONZERO(r.is_err, "mlx_load(absent bridge): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_load(absent bridge): err_msg non-NULL");
        mlx_model_free(&r.model);
        free(r.err_msg);
    }
}

/* =========================================================================
 * 4. mlx_unload
 * ========================================================================= */

static void test_unload(void)
{
    /* NULL pointer must not crash and must return 0 */
    int ok = mlx_unload(NULL);
    ASSERT_ZERO(ok, "mlx_unload(NULL) returns 0");

    /* Model with NULL id must return 0 */
    MlxModel m;
    memset(&m, 0, sizeof(m));
    ok = mlx_unload(&m);
    ASSERT_ZERO(ok, "mlx_unload(null-id model) returns 0");

    /* Model with a valid-looking id but no bridge returns 0 */
    m.id   = (char *)"fake-id-001";
    m.path = (char *)"/tmp/fake-model";
    ok = mlx_unload(&m);
    ASSERT_ZERO(ok, "mlx_unload(absent bridge): returns 0");
}

/* =========================================================================
 * 5. mlx_generate
 * ========================================================================= */

static void test_generate(void)
{
    /* NULL model handle */
    {
        MlxStrResult r = mlx_generate(NULL, "hello", 10);
        ASSERT_NONZERO(r.is_err, "mlx_generate(NULL model): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_generate(NULL model): err_msg non-NULL");
        mlx_str_result_free(&r);
    }

    /* NULL prompt */
    {
        MlxModel m;
        memset(&m, 0, sizeof(m));
        m.id   = (char *)"fake-id-001";
        m.path = (char *)"/tmp/fake-model";
        MlxStrResult r = mlx_generate(&m, NULL, 10);
        ASSERT_NONZERO(r.is_err, "mlx_generate(NULL prompt): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_generate(NULL prompt): err_msg non-NULL");
        mlx_str_result_free(&r);
    }

    /* Valid handle and prompt but bridge absent (or non-Apple) */
    {
        MlxModel m;
        memset(&m, 0, sizeof(m));
        m.id   = (char *)"fake-id-001";
        m.path = (char *)"/tmp/fake-model";
        MlxStrResult r = mlx_generate(&m, "Say hello.", 32);
        ASSERT_NONZERO(r.is_err, "mlx_generate(absent bridge): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_generate(absent bridge): err_msg non-NULL");
        mlx_str_result_free(&r);
    }

    /* max_tokens == 0 with absent bridge still returns is_err */
    {
        MlxModel m;
        memset(&m, 0, sizeof(m));
        m.id   = (char *)"fake-id-001";
        m.path = (char *)"/tmp/fake-model";
        MlxStrResult r = mlx_generate(&m, "ping", 0);
        ASSERT_NONZERO(r.is_err, "mlx_generate(max_tokens=0, absent bridge): is_err set");
        mlx_str_result_free(&r);
    }
}

/* =========================================================================
 * 6. mlx_embed
 * ========================================================================= */

static void test_embed(void)
{
    /* NULL model handle */
    {
        MlxEmbedResult r = mlx_embed(NULL, "hello");
        ASSERT_NONZERO(r.is_err, "mlx_embed(NULL model): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_embed(NULL model): err_msg non-NULL");
        mlx_embed_result_free(&r);
    }

    /* NULL text */
    {
        MlxModel m;
        memset(&m, 0, sizeof(m));
        m.id   = (char *)"fake-id-001";
        m.path = (char *)"/tmp/fake-model";
        MlxEmbedResult r = mlx_embed(&m, NULL);
        ASSERT_NONZERO(r.is_err, "mlx_embed(NULL text): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_embed(NULL text): err_msg non-NULL");
        mlx_embed_result_free(&r);
    }

    /* Valid handle and text but bridge absent (or non-Apple) */
    {
        MlxModel m;
        memset(&m, 0, sizeof(m));
        m.id   = (char *)"fake-id-001";
        m.path = (char *)"/tmp/fake-model";
        MlxEmbedResult r = mlx_embed(&m, "Hello world");
        ASSERT_NONZERO(r.is_err, "mlx_embed(absent bridge): is_err set");
        ASSERT_NOTNULL(r.err_msg, "mlx_embed(absent bridge): err_msg non-NULL");
        ASSERT_NULL(r.embedding.data,
                    "mlx_embed(absent bridge): embedding.data is NULL");
        ASSERT_ZERO((int)r.embedding.len,
                    "mlx_embed(absent bridge): embedding.len is 0");
        mlx_embed_result_free(&r);
    }
}

/* =========================================================================
 * 7. Error result consistency
 * ========================================================================= */

static void test_error_consistency(void)
{
    /* Every error result must have is_err != 0 AND err_msg != NULL. */

    MlxModelResult mr = mlx_load("/tmp/no-such-model");
    ASSERT(mr.is_err != 0 && mr.err_msg != NULL,
           "mlx_load error: is_err && err_msg both set");
    mlx_model_free(&mr.model);
    free(mr.err_msg);

    MlxModel fake;
    memset(&fake, 0, sizeof(fake));
    fake.id   = (char *)"x";
    fake.path = (char *)"/tmp/x";

    MlxStrResult sr = mlx_generate(&fake, "hi", 1);
    ASSERT(sr.is_err != 0 && sr.err_msg != NULL,
           "mlx_generate error: is_err && err_msg both set");
    mlx_str_result_free(&sr);

    MlxEmbedResult er = mlx_embed(&fake, "hi");
    ASSERT(er.is_err != 0 && er.err_msg != NULL,
           "mlx_embed error: is_err && err_msg both set");
    mlx_embed_result_free(&er);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    printf("=== test_mlx: std.mlx unit tests ===\n\n");

    test_memory();
    test_is_available();
    test_load();
    test_unload();
    test_generate();
    test_embed();
    test_error_consistency();

    printf("\n");
    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}

/*
 * test_infer_stream.c — unit tests for the std.infer disk-streaming
 * extension (Epic 72.7).
 *
 * Build (stub mode — no llama.cpp required):
 *   cc -std=c99 -Wall -Wextra -Wpedantic \
 *       -Isrc -Isrc/stdlib \
 *       src/stdlib/infer_stream.c \
 *       test/stdlib/test_infer_stream.c \
 *       -lpthread -o /tmp/test_infer_stream
 *   /tmp/test_infer_stream
 *
 * Tests are written for the stub build (TK_HAVE_LLAMACPP not defined).
 * Story: 72.7.5
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../src/stdlib/infer_stream.h"

/* =========================================================================
 * Link-time stub for tk_infer_unload (implemented in infer.c).
 *
 * When this test is compiled standalone (without infer.c), we supply a
 * minimal stub so the linker is satisfied.  In the full build, infer.c's
 * definition takes precedence via link order.
 * -------------------------------------------------------------------------
 */
#ifndef TK_HAVE_INFER_UNLOAD_STUB
#define TK_HAVE_INFER_UNLOAD_STUB
int tk_infer_unload(TkModelHandle *h)
{
    (void)h;
    return 0;
}
#endif

/* =========================================================================
 * Minimal test harness
 * ========================================================================= */

static int g_failures = 0;
static int g_passes   = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
            g_failures++; \
        } else { \
            printf("pass: %s\n", (msg)); \
            g_passes++; \
        } \
    } while (0)

#define ASSERT_STR_EQ(a, b, msg) \
    ASSERT(strcmp((a), (b)) == 0, msg)

#define ASSERT_NOT_NULL(p, msg) \
    ASSERT((p) != NULL, msg)

#define ASSERT_NULL(p, msg) \
    ASSERT((p) == NULL, msg)

/* =========================================================================
 * 72.7.2 — detect_storage_type tests
 * ========================================================================= */

/*
 * test_detect_null_path
 *
 * Passing NULL or an empty string must return "unknown" without crashing.
 */
static void test_detect_null_path(void)
{
    const char *r1 = tk_infer_detect_storage_type(NULL);
    ASSERT_NOT_NULL(r1, "detect_storage_type(NULL) returns non-NULL");
    ASSERT_STR_EQ(r1, "unknown",
                  "detect_storage_type(NULL) returns \"unknown\"");

    const char *r2 = tk_infer_detect_storage_type("");
    ASSERT_NOT_NULL(r2, "detect_storage_type(\"\") returns non-NULL");
    ASSERT_STR_EQ(r2, "unknown",
                  "detect_storage_type(\"\") returns \"unknown\"");
}

/*
 * test_detect_nonexistent_path
 *
 * A path that does not exist must return "unknown" without crashing.
 */
static void test_detect_nonexistent_path(void)
{
    const char *r = tk_infer_detect_storage_type(
                        "/does/not/exist/at/all/7x3q");
    ASSERT_NOT_NULL(r, "detect_storage_type(missing path) returns non-NULL");
    ASSERT_STR_EQ(r, "unknown",
                  "detect_storage_type(missing path) returns \"unknown\"");
}

/*
 * test_detect_valid_path
 *
 * A real path (the current executable's directory) must return one of the
 * four valid classifications without crashing.  We do not assert a specific
 * value because the test host's storage type is unknown at compile time.
 */
static void test_detect_valid_path(void)
{
    const char *r = tk_infer_detect_storage_type("/tmp");
    ASSERT_NOT_NULL(r, "detect_storage_type(\"/tmp\") returns non-NULL");

    int valid = (strcmp(r, "nvme")    == 0 ||
                 strcmp(r, "ssd")     == 0 ||
                 strcmp(r, "hdd")     == 0 ||
                 strcmp(r, "unknown") == 0);
    ASSERT(valid,
           "detect_storage_type(\"/tmp\") returns one of nvme/ssd/hdd/unknown");
}

/*
 * test_detect_return_is_static
 *
 * The returned string must have static lifetime (safe to use after the call
 * without copying).  We verify by calling twice and checking the pointer is
 * the same constant string, or at minimum that the content is stable.
 */
static void test_detect_return_is_static(void)
{
    const char *r1 = tk_infer_detect_storage_type(NULL);
    const char *r2 = tk_infer_detect_storage_type(NULL);
    ASSERT_STR_EQ(r1, r2,
                  "detect_storage_type returns consistent value for same input");
}

/* =========================================================================
 * 72.7.3 — load_streaming stub tests
 * ========================================================================= */

/*
 * test_stream_load_null_dir
 *
 * Passing NULL for model_dir must return is_err=1 without crashing.
 */
static void test_stream_load_null_dir(void)
{
    TkStreamOpts opts = {4.0f, 2, 0};
    TkModelHandleResult r = tk_infer_load_streaming(NULL, opts);

    ASSERT(r.is_err,
           "load_streaming(NULL, opts) returns is_err=1");
    ASSERT_NULL(r.handle,
                "load_streaming(NULL, opts) returns NULL handle");
    ASSERT_NOT_NULL(r.err.msg,
                    "load_streaming(NULL, opts) provides an error message");
}

/*
 * test_stream_load_empty_dir
 *
 * An empty dir string must also fail gracefully.
 */
static void test_stream_load_empty_dir(void)
{
    TkStreamOpts opts = {4.0f, 2, 0};
    TkModelHandleResult r = tk_infer_load_streaming("", opts);

    ASSERT(r.is_err,
           "load_streaming(\"\", opts) returns is_err=1");
    ASSERT_NULL(r.handle,
                "load_streaming(\"\", opts) returns NULL handle");
}

/*
 * test_stream_load_nonexistent_dir
 *
 * A directory that does not exist must fail with a meaningful error, not a
 * crash.
 */
static void test_stream_load_nonexistent_dir(void)
{
    TkStreamOpts opts = {4.0f, 1, 0};
    TkModelHandleResult r =
        tk_infer_load_streaming("/no/such/directory/7x3qfoo", opts);

    ASSERT(r.is_err,
           "load_streaming(nonexistent dir) returns is_err=1");
    ASSERT_NULL(r.handle,
                "load_streaming(nonexistent dir) returns NULL handle");
    ASSERT_NOT_NULL(r.err.msg,
                    "load_streaming(nonexistent dir) provides error message");
}

/*
 * test_stream_load_empty_dir_no_shards
 *
 * A directory that exists but contains no layer_NNN.gguf files must fail
 * gracefully.
 */
static void test_stream_load_empty_dir_no_shards(void)
{
    /* /tmp always exists and will never contain layer_NNN.gguf files in a
     * clean test environment. */
    TkStreamOpts opts = {4.0f, 1, 0};
    TkModelHandleResult r = tk_infer_load_streaming("/tmp", opts);

#ifdef TK_HAVE_LLAMACPP
    /* With llama.cpp, no shards means is_err. */
    ASSERT(r.is_err,
           "load_streaming(/tmp, no shards) returns is_err=1");
    ASSERT_NULL(r.handle,
                "load_streaming(/tmp, no shards) returns NULL handle");
#else
    /* Without llama.cpp, stub always returns is_err. */
    ASSERT(r.is_err,
           "stub load_streaming(/tmp) returns is_err=1");
    ASSERT_NULL(r.handle,
                "stub load_streaming(/tmp) returns NULL handle");
    ASSERT(r.err.code == -2,
           "stub load_streaming returns error code -2");
    ASSERT_NOT_NULL(r.err.msg,
                    "stub load_streaming returns non-NULL error message");
#endif
}

/*
 * test_stream_load_requires_nvme_warns
 *
 * When requires_nvme=1 and the detected storage type is not nvme, the
 * function should still complete (not crash, not refuse for stub), and
 * the result should be is_err=1 in stub mode.  We are testing that no
 * crash occurs and the API contract is honoured.
 */
static void test_stream_load_requires_nvme_warns(void)
{
    TkStreamOpts opts = {2.0f, 1, 1 /* requires_nvme */};
    TkModelHandleResult r = tk_infer_load_streaming("/tmp", opts);

    /* In stub mode this is always is_err; in llama.cpp mode with no shards
     * it is also is_err.  Either way the call must not crash. */
    ASSERT(r.is_err || r.handle != NULL,
           "load_streaming with requires_nvme does not crash");
    /* Clean up if somehow a handle came back. */
    if (r.handle)
        tk_infer_unload(r.handle);
}

/* =========================================================================
 * 72.7.4 — throughput monitor tests
 * ========================================================================= */

/*
 * test_throughput_null_handle
 *
 * Querying throughput for a NULL handle must return a zeroed struct.
 */
static void test_throughput_null_handle(void)
{
    TkStreamThroughput t = tk_infer_stream_throughput(NULL);
    ASSERT(t.tokens_per_sec == 0.0,
           "throughput(NULL).tokens_per_sec == 0.0");
    ASSERT(t.bytes_loaded == 0,
           "throughput(NULL).bytes_loaded == 0");
    ASSERT(t.ram_used_bytes == 0,
           "throughput(NULL).ram_used_bytes == 0");
    ASSERT(t.warn_slow == 0,
           "throughput(NULL).warn_slow == 0");
}

/*
 * test_throughput_non_stream_handle
 *
 * Querying throughput for a handle that was not created by
 * tk_infer_load_streaming must return a zeroed struct (registry miss).
 *
 * We use a stack-allocated TkModelHandle-shaped buffer for this test.
 * Because TkModelHandle is opaque, we cast a dummy pointer — the registry
 * will simply not find it.
 */
static void test_throughput_non_stream_handle(void)
{
    /* Use a known-invalid address that is non-NULL. */
    const TkModelHandle *fake = (const TkModelHandle *)(uintptr_t)0xdeadbeef;
    TkStreamThroughput t = tk_infer_stream_throughput(fake);
    ASSERT(t.tokens_per_sec == 0.0,
           "throughput(fake handle).tokens_per_sec == 0.0");
    ASSERT(t.bytes_loaded == 0,
           "throughput(fake handle).bytes_loaded == 0");
}

/*
 * test_slow_threshold_constant
 *
 * The slow-warning threshold must be exactly 0.1 tok/s as specified.
 */
static void test_slow_threshold_constant(void)
{
    ASSERT(TK_STREAM_SLOW_THRESHOLD_TOK_S == 0.1,
           "TK_STREAM_SLOW_THRESHOLD_TOK_S == 0.1");
}

/* =========================================================================
 * 72.7.1 — API shape / type tests
 * ========================================================================= */

/*
 * test_stream_opts_fields
 *
 * Verify TkStreamOpts has the fields mandated by the spec and that they
 * accept the correct types.
 */
static void test_stream_opts_fields(void)
{
    TkStreamOpts opts;
    opts.ram_ceiling_gb  = 8.0f;
    opts.prefetch_layers = 3;
    opts.requires_nvme   = 1;

    ASSERT(opts.ram_ceiling_gb == 8.0f,
           "TkStreamOpts.ram_ceiling_gb is float-assignable");
    ASSERT(opts.prefetch_layers == 3,
           "TkStreamOpts.prefetch_layers is int32_t-assignable");
    ASSERT(opts.requires_nvme == 1,
           "TkStreamOpts.requires_nvme is bool-assignable");
}

/*
 * test_throughput_struct_fields
 *
 * Verify TkStreamThroughput has all specified fields.
 */
static void test_throughput_struct_fields(void)
{
    TkStreamThroughput t;
    t.tokens_per_sec = 0.05;
    t.bytes_loaded   = 1024 * 1024;
    t.ram_used_bytes = 512 * 1024;
    t.warn_slow      = 1;

    ASSERT(t.warn_slow == 1,
           "TkStreamThroughput.warn_slow is int-assignable");
    ASSERT(t.tokens_per_sec < TK_STREAM_SLOW_THRESHOLD_TOK_S,
           "0.05 tok/s is below the slow threshold");
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void)
{
    printf("=== test_infer_stream ===\n\n");

    printf("-- 72.7.1 API shape --\n");
    test_stream_opts_fields();
    test_throughput_struct_fields();
    test_slow_threshold_constant();

    printf("\n-- 72.7.2 detect_storage_type --\n");
    test_detect_null_path();
    test_detect_nonexistent_path();
    test_detect_valid_path();
    test_detect_return_is_static();

    printf("\n-- 72.7.3 load_streaming --\n");
    test_stream_load_null_dir();
    test_stream_load_empty_dir();
    test_stream_load_nonexistent_dir();
    test_stream_load_empty_dir_no_shards();
    test_stream_load_requires_nvme_warns();

    printf("\n-- 72.7.4 throughput monitor --\n");
    test_throughput_null_handle();
    test_throughput_non_stream_handle();

    printf("\n=== Results: %d passed, %d failed ===\n",
           g_passes, g_failures);

    return g_failures > 0 ? 1 : 0;
}

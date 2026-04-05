/*
 * test_http_tls.c — Unit tests for TLS/HTTPS support (Story 27.1.2).
 *
 * Tests are deliberately free of live network I/O:
 *
 *   1. http_tls_ctx_new with a non-existent cert path must return NULL.
 *   2. http_serve_tls with NULL tls_ctx must return TK_HTTP_ERR_BIND.
 *   3. http_serve_tls_workers with NULL tls_ctx must return TK_HTTP_ERR_BIND.
 *   4. http_tls_ctx_free(NULL) must not crash.
 *
 * Build and run: make test-stdlib-http-tls
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "../../src/stdlib/http.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
         else { printf("pass: %s\n", (msg)); } } while (0)

int main(void)
{
    /* ── Test 1: http_tls_ctx_new with non-existent cert returns NULL ── */
    {
        TkTlsCtx *ctx = http_tls_ctx_new("/nonexistent/cert.pem",
                                          "/nonexistent/key.pem");
        ASSERT(ctx == NULL,
               "http_tls_ctx_new with non-existent cert returns NULL");
    }

    /* ── Test 2: http_tls_ctx_new with NULL args returns NULL ─────────── */
    {
        TkTlsCtx *ctx = http_tls_ctx_new(NULL, NULL);
        ASSERT(ctx == NULL,
               "http_tls_ctx_new with NULL args returns NULL");
    }

    /* ── Test 3: http_serve_tls with NULL tls_ctx returns error ─────── */
    {
        TkHttpRouter *r = http_router_new();
        TkHttpErr err = http_serve_tls(r, NULL, 9443, NULL);
        ASSERT(err != TK_HTTP_OK,
               "http_serve_tls with NULL tls returns non-OK");
        http_router_free(r);
    }

    /* ── Test 4: http_serve_tls_workers with NULL tls returns error ──── */
    {
        TkHttpRouter *r = http_router_new();
        TkHttpErr err = http_serve_tls_workers(r, NULL, 9444, NULL, 1);
        ASSERT(err != TK_HTTP_OK,
               "http_serve_tls_workers with NULL tls returns non-OK");
        http_router_free(r);
    }

    /* ── Test 5: http_tls_ctx_free(NULL) is safe ────────────────────── */
    {
        http_tls_ctx_free(NULL); /* must not crash */
        ASSERT(1, "http_tls_ctx_free(NULL) does not crash");
    }

    /* ── Test 6: http_serve_tls with NULL router returns error ──────── */
    {
        TkHttpErr err = http_serve_tls(NULL, NULL, 9445, NULL);
        ASSERT(err != TK_HTTP_OK,
               "http_serve_tls with NULL router returns non-OK");
    }

    if (failures == 0) {
        printf("All TLS tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d TLS test(s) failed.\n", failures);
    return 1;
}

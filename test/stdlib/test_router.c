/*
 * test_router.c — Unit tests for the std.router C library (Story 15.1.3).
 *
 * Build and run: make test-stdlib-router
 *
 * Tests:
 *   1.  Exact route: GET /hello matches /hello handler
 *   2.  Param route: GET /users/42 captures id="42"
 *   3.  Two params:  /a/:x/b/:y captures x and y
 *   4.  Wildcard:    /files/* matches /files/a/b/c
 *   5.  No match:    /missing → status 404
 *   6.  Method mismatch: POST on GET-only route → status 404
 *   7.  Multiple routes: correct handler called per path
 *   8.  router_query_get: simple key lookup
 *   9.  router_query_get: '+' → space
 *   10. router_query_get: %XX percent-decode
 *   11. router_resp_json: status=200 and content_type="application/json"
 *   12. Exact beats param for same depth
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include "../../src/stdlib/router.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

#define ASSERT_INTEQ(a, b, msg) \
    ASSERT((a) == (b), msg)

/* ----------------------------------------------------------------------- */
/* Minimal handlers                                                          */
/* ----------------------------------------------------------------------- */

static TkRouteResp handler_hello(TkRouteCtx ctx)
{
    (void)ctx;
    return router_resp_ok("hello", "text/plain");
}

static TkRouteResp handler_user(TkRouteCtx ctx)
{
    /* Capture id param into a static buffer so the test can inspect it.    */
    static char captured_id[64];
    captured_id[0] = '\0';
    for (uint64_t i = 0; i < ctx.nparam; i++) {
        if (strcmp(ctx.param_names[i], "id") == 0) {
            strncpy(captured_id, ctx.param_values[i], sizeof(captured_id) - 1);
        }
    }
    (void)captured_id;
    return router_resp_ok(captured_id, "text/plain");
}

/* Captured params for two-param test */
static char cap_x[64];
static char cap_y[64];

static TkRouteResp handler_two_params(TkRouteCtx ctx)
{
    cap_x[0] = cap_y[0] = '\0';
    for (uint64_t i = 0; i < ctx.nparam; i++) {
        if (strcmp(ctx.param_names[i], "x") == 0)
            strncpy(cap_x, ctx.param_values[i], sizeof(cap_x) - 1);
        if (strcmp(ctx.param_names[i], "y") == 0)
            strncpy(cap_y, ctx.param_values[i], sizeof(cap_y) - 1);
    }
    return router_resp_ok("two", "text/plain");
}

static TkRouteResp handler_wildcard(TkRouteCtx ctx)
{
    (void)ctx;
    return router_resp_ok("wildcard", "text/plain");
}

static TkRouteResp handler_alpha(TkRouteCtx ctx)
{
    (void)ctx;
    return router_resp_ok("alpha", "text/plain");
}

static TkRouteResp handler_beta(TkRouteCtx ctx)
{
    (void)ctx;
    return router_resp_ok("beta", "text/plain");
}

/* Exact /users/me — should beat /users/:id */
static TkRouteResp handler_me(TkRouteCtx ctx)
{
    (void)ctx;
    return router_resp_ok("me", "text/plain");
}

/* ----------------------------------------------------------------------- */
/* Tests                                                                     */
/* ----------------------------------------------------------------------- */

int main(void)
{
    /* ------------------------------------------------------------------ */
    /* Test 1: exact route matched                                          */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/hello", handler_hello);
        TkRouteResp resp = router_dispatch(r, "GET", "/hello", NULL, NULL);
        ASSERT_INTEQ(resp.status, 200, "T1: exact route status=200");
        ASSERT_STREQ(resp.body, "hello", "T1: exact route body='hello'");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Test 2: param route captures id                                      */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/users/:id", handler_user);
        TkRouteResp resp = router_dispatch(r, "GET", "/users/42", NULL, NULL);
        ASSERT_INTEQ(resp.status, 200, "T2: param route status=200");
        ASSERT_STREQ(resp.body, "42", "T2: param route body='42'");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Test 3: two params captured                                          */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/a/:x/b/:y", handler_two_params);
        TkRouteResp resp = router_dispatch(r, "GET", "/a/foo/b/bar", NULL, NULL);
        ASSERT_INTEQ(resp.status, 200, "T3: two-param route status=200");
        ASSERT_STREQ(cap_x, "foo", "T3: param x='foo'");
        ASSERT_STREQ(cap_y, "bar", "T3: param y='bar'");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Test 4: wildcard matches deep path                                   */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/files/*", handler_wildcard);
        TkRouteResp resp = router_dispatch(r, "GET", "/files/a/b/c", NULL, NULL);
        ASSERT_INTEQ(resp.status, 200, "T4: wildcard status=200");
        ASSERT_STREQ(resp.body, "wildcard", "T4: wildcard body='wildcard'");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Test 5: no match → 404                                               */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/hello", handler_hello);
        TkRouteResp resp = router_dispatch(r, "GET", "/missing", NULL, NULL);
        ASSERT_INTEQ(resp.status, 404, "T5: no-match status=404");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Test 6: method mismatch → 404                                        */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/hello", handler_hello);
        TkRouteResp resp = router_dispatch(r, "POST", "/hello", NULL, NULL);
        ASSERT_INTEQ(resp.status, 404, "T6: method-mismatch status=404");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Test 7: multiple routes — correct handler called per path            */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/alpha", handler_alpha);
        router_get(r, "/beta",  handler_beta);
        TkRouteResp ra = router_dispatch(r, "GET", "/alpha", NULL, NULL);
        TkRouteResp rb = router_dispatch(r, "GET", "/beta",  NULL, NULL);
        ASSERT_STREQ(ra.body, "alpha", "T7: /alpha → 'alpha'");
        ASSERT_STREQ(rb.body, "beta",  "T7: /beta  → 'beta'");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Test 8: router_query_get — simple lookup                             */
    /* ------------------------------------------------------------------ */
    {
        const char *val = router_query_get("name=Alice&age=30", "name");
        ASSERT_STREQ(val, "Alice", "T8: query_get name='Alice'");
        free((char *)val);
    }

    /* ------------------------------------------------------------------ */
    /* Test 9: router_query_get — '+' decoded to space                      */
    /* ------------------------------------------------------------------ */
    {
        const char *val = router_query_get("q=hello+world", "q");
        ASSERT_STREQ(val, "hello world", "T9: query_get '+' → space");
        free((char *)val);
    }

    /* ------------------------------------------------------------------ */
    /* Test 10: router_query_get — %XX percent-decode                       */
    /* ------------------------------------------------------------------ */
    {
        const char *val = router_query_get("x=%41", "x");
        ASSERT_STREQ(val, "A", "T10: query_get %%41 → 'A'");
        free((char *)val);
    }

    /* ------------------------------------------------------------------ */
    /* Test 11: router_resp_json sets status=200 and application/json       */
    /* ------------------------------------------------------------------ */
    {
        TkRouteResp resp = router_resp_json("{\"ok\":true}");
        ASSERT_INTEQ(resp.status, 200, "T11: resp_json status=200");
        ASSERT_STREQ(resp.content_type, "application/json",
                     "T11: resp_json content_type='application/json'");
        ASSERT_STREQ(resp.body, "{\"ok\":true}", "T11: resp_json body correct");
    }

    /* ------------------------------------------------------------------ */
    /* Test 12: exact segment beats param segment of the same depth         */
    /* ------------------------------------------------------------------ */
    {
        TkRouter *r = router_new();
        router_get(r, "/users/:id", handler_user);
        router_get(r, "/users/me",  handler_me);
        TkRouteResp resp = router_dispatch(r, "GET", "/users/me", NULL, NULL);
        ASSERT_STREQ(resp.body, "me", "T12: exact beats param at same depth");
        router_free(r);
    }

    /* ------------------------------------------------------------------ */
    /* Summary                                                               */
    /* ------------------------------------------------------------------ */
    if (failures == 0) {
        printf("All router tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) failed.\n", failures);
        return 1;
    }
}

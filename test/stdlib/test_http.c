/*
 * test_http.c — Unit tests for the std.http C library (Story 1.3.2).
 *
 * Tests non-server functions: response constructors, route registration,
 * param/header accessors. The full server loop requires a live network.
 *
 * Build and run: make test-stdlib
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/http.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

static Res dummy_handler(Req req) { (void)req; return http_Res_ok("ok"); }

int main(void)
{
    /* ── Response constructors ─────────────────────────────────────── */

    Res r200 = http_Res_ok("hello");
    ASSERT(r200.status == 200,         "Res.ok status==200");
    ASSERT_STREQ(r200.body, "hello",   "Res.ok body==hello");

    Res r201 = http_Res_json(201, "{}");
    ASSERT(r201.status == 201,         "Res.json status==201");
    ASSERT_STREQ(r201.body, "{}",      "Res.json body=={}");

    Res r400 = http_Res_bad("oops");
    ASSERT(r400.status == 400,         "Res.bad status==400");
    ASSERT_STREQ(r400.body, "oops",    "Res.bad body==oops");

    Res r500 = http_Res_err("boom");
    ASSERT(r500.status == 500,         "Res.err status==500");
    ASSERT_STREQ(r500.body, "boom",    "Res.err body==boom");

    /* ── Route registration ────────────────────────────────────────── */

    int before = http_route_count();
    http_GET("/", dummy_handler);
    ASSERT(http_route_count() == before + 1, "GET / increments route_count");

    http_POST  ("/items",     dummy_handler);
    http_PUT   ("/items/:id", dummy_handler);
    http_DELETE("/items/:id", dummy_handler);
    http_PATCH ("/items/:id", dummy_handler);
    ASSERT(http_route_count() == before + 5, "POST/PUT/DELETE/PATCH registered");

    /* ── http_param — matching ─────────────────────────────────────── */

    StrPair pairs[2] = { {"id", "42"}, {"name", "alice"} };
    Req req; memset(&req, 0, sizeof(req));
    req.method  = "GET"; req.path = "/items/42"; req.body = "";
    req.params.data = pairs; req.params.len = 2;
    req.headers.data = NULL; req.headers.len = 0;

    HttpResult pr = http_param(req, "id");
    ASSERT(!pr.is_err,              "http_param(id) ok");
    ASSERT_STREQ(pr.ok, "42",       "http_param(id)==42");

    /* ── http_param — missing ──────────────────────────────────────── */

    HttpResult pr2 = http_param(req, "missing");
    ASSERT(pr2.is_err,              "http_param(missing) is err");
    ASSERT(pr2.err.kind == HTTP_ERR_NOT_FOUND, "http_param err==NOT_FOUND");

    /* ── http_header — case-insensitive match ─────────────────────── */

    StrPair hdrs[1] = { {"Content-Type", "application/json"} };
    req.headers.data = hdrs; req.headers.len = 1;

    HttpResult hr = http_header(req, "content-type");
    ASSERT(!hr.is_err,                              "http_header ok");
    ASSERT_STREQ(hr.ok, "application/json",         "http_header value correct");

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

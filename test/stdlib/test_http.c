/*
 * test_http.c — Unit tests for the std.http C library (Story 1.3.2).
 *
 * Tests non-server functions: response constructors, route registration,
 * param/header accessors. The full server loop requires a live network.
 *
 * Build and run: make test-stdlib
 */

#include <stdio.h>
#include <stdlib.h>
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

    /* ── Client API (Story 35.1.2) ────────────────────────────────── */

    /* http_client constructor */
    HttpClient *cli = http_client("http://localhost:9999");
    ASSERT(cli != NULL,                          "http_client returns non-NULL");
    ASSERT(strcmp(cli->base_url, "http://localhost:9999") == 0,
                                                 "http_client base_url set");
    ASSERT(cli->timeout_ms == 30000,             "http_client default timeout");
    ASSERT(cli->pool_size  == 4,                 "http_client default pool_size");

    /* http_get to unreachable host gives err */
    HttpClientResp gr = http_get(cli, "/test");
    ASSERT(gr.is_err,                            "http_get unreachable is_err");
    ASSERT(gr.err_msg != NULL,                   "http_get err has message");
    free(gr.err_msg);

    /* http_post to unreachable host gives err */
    const uint8_t post_body[] = "{\"x\":1}";
    HttpClientResp pr3 = http_post(cli, "/data",
                                   post_body, sizeof(post_body) - 1,
                                   "application/json");
    ASSERT(pr3.is_err,                           "http_post unreachable is_err");
    free(pr3.err_msg);

    /* http_put to unreachable host gives err */
    HttpClientResp pu = http_put(cli, "/data",
                                 post_body, sizeof(post_body) - 1,
                                 "application/json");
    ASSERT(pu.is_err,                            "http_put unreachable is_err");
    free(pu.err_msg);

    /* http_delete to unreachable host gives err */
    HttpClientResp dr = http_delete(cli, "/data");
    ASSERT(dr.is_err,                            "http_delete unreachable is_err");
    free(dr.err_msg);

    /* NULL client gives err */
    HttpClientResp nr = http_get(NULL, "/x");
    ASSERT(nr.is_err,                            "http_get NULL client is_err");
    free(nr.err_msg);

    /* HTTPS rejected (no TLS compiled in) */
    HttpClient *tls = http_client("https://example.com");
    HttpClientResp tr = http_get(tls, "/");
    ASSERT(tr.is_err,                            "http_get HTTPS is_err");
    free(tr.err_msg);
    http_client_free(tls);

    /* stream with NULL client gives err */
    HttpClientReq sreq;
    memset(&sreq, 0, sizeof(sreq));
    sreq.method = "GET";
    sreq.url    = "/stream";
    HttpStreamResult sr = http_stream(NULL, sreq);
    ASSERT(sr.is_err,                            "http_stream NULL client is_err");
    free(sr.err_msg);

    /* streamnext with closed stream gives err */
    HttpStream closed_s;
    memset(&closed_s, 0, sizeof(closed_s));
    closed_s.open = 0;
    HttpChunkResult cr = http_streamnext(&closed_s);
    ASSERT(cr.is_err,                            "http_streamnext closed is_err");
    free(cr.err_msg);

    http_client_free(cli);

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

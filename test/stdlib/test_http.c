/*
 * test_http.c — Unit tests for the std.http C library (Story 1.3.2).
 *
 * Tests non-server functions: response constructors, route registration,
 * param/header accessors. The full server loop requires a live network.
 *
 * Story 27.1.9 adds limit-enforcement tests using socketpair()+fork().
 *
 * Build and run: make test-stdlib-http
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>
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

    /* ── Keep-alive constants (Story 27.1.3) ───────────────────────── */

    ASSERT(HTTP_KEEPALIVE_IDLE_TIMEOUT_S == 30,
           "HTTP_KEEPALIVE_IDLE_TIMEOUT_S default == 30");
    ASSERT(HTTP_KEEPALIVE_MAX_REQUESTS == 1000,
           "HTTP_KEEPALIVE_MAX_REQUESTS default == 1000");

    /* ── Connection: close header is parsed and accessible via http_header */

    StrPair conn_close_hdr[1] = { {"Connection", "close"} };
    Req req_close; memset(&req_close, 0, sizeof(req_close));
    req_close.method         = "GET";
    req_close.path           = "/";
    req_close.body           = "";
    req_close.headers.data   = conn_close_hdr;
    req_close.headers.len    = 1;

    HttpResult conn_close_r = http_header(req_close, "Connection");
    ASSERT(!conn_close_r.is_err,
           "http_header finds Connection header");
    ASSERT_STREQ(conn_close_r.ok, "close",
                 "Connection header value == close");

    /* ── Connection: keep-alive header is parsed and accessible ─────── */

    StrPair conn_ka_hdr[1] = { {"Connection", "keep-alive"} };
    Req req_ka; memset(&req_ka, 0, sizeof(req_ka));
    req_ka.method         = "GET";
    req_ka.path           = "/";
    req_ka.body           = "";
    req_ka.headers.data   = conn_ka_hdr;
    req_ka.headers.len    = 1;

    HttpResult conn_ka_r = http_header(req_ka, "connection"); /* lower-case */
    ASSERT(!conn_ka_r.is_err,
           "http_header finds Connection case-insensitively");
    ASSERT_STREQ(conn_ka_r.ok, "keep-alive",
                 "Connection header value == keep-alive");

    /* ── Connection header absent: http_header returns err ──────────── */

    Req req_no_conn; memset(&req_no_conn, 0, sizeof(req_no_conn));
    req_no_conn.method       = "GET";
    req_no_conn.path         = "/";
    req_no_conn.body         = "";
    req_no_conn.headers.data = NULL;
    req_no_conn.headers.len  = 0;

    HttpResult conn_absent_r = http_header(req_no_conn, "Connection");
    ASSERT(conn_absent_r.is_err,
           "http_header missing Connection is_err");
    ASSERT(conn_absent_r.err.kind == HTTP_ERR_NOT_FOUND,
           "http_header missing Connection err==NOT_FOUND");

    /* ── Pre-fork worker pool (Story 27.1.1) ───────────────────────── */

    /* http_router_new captures the current route table */
    TkHttpRouter *router = http_router_new();
    ASSERT(router != NULL, "http_router_new returns non-NULL");

    /* http_serve_workers(nworkers=1) must not crash or fork.
     * We do NOT actually call it because nworkers==1 enters an infinite
     * accept loop.  Instead we verify the preconditions it relies on:
     * the router is valid and http_router_free does not crash. */
    ASSERT(router != NULL, "TkHttpRouter is valid before http_serve_workers");
    http_router_free(router);
    ASSERT(1, "http_router_free did not crash");

    /* http_serve_workers with NULL router returns TK_HTTP_ERR_SOCKET */
    TkHttpErr wret = http_serve_workers(NULL, NULL, 19080, 1);
    ASSERT(wret == TK_HTTP_ERR_SOCKET,
           "http_serve_workers(NULL router) returns TK_HTTP_ERR_SOCKET");

    /* ── Request size limits (Story 27.1.9) ────────────────────────── */

    /* http_set_limits — verify defaults are reflected in constants */
    ASSERT(HTTP_DEFAULT_MAX_HEADER_SIZE == 8192U,
           "HTTP_DEFAULT_MAX_HEADER_SIZE == 8192");
    ASSERT(HTTP_DEFAULT_MAX_BODY_SIZE == 1048576U,
           "HTTP_DEFAULT_MAX_BODY_SIZE == 1048576");
    ASSERT(HTTP_DEFAULT_TIMEOUT_SECS == 30U,
           "HTTP_DEFAULT_TIMEOUT_SECS == 30");

    /*
     * Test: oversized header → 431 Request Header Fields Too Large
     *
     * Strategy: set a tiny header cap (256 bytes), build a request whose
     * headers block exceeds 256 bytes, send it through a socketpair to a
     * forked child that calls http_handle_fd().  The parent then reads the
     * response and checks for status 431.
     */
    {
        /* Reduce the header limit to 256 bytes for this test */
        http_set_limits(256, 0, 1);

        int sv[2];
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0,
               "oversized-header: socketpair created");

        pid_t pid = fork();
        if (pid == 0) {
            /* child: act as server side */
            close(sv[0]);
            http_handle_fd(sv[1]); /* closes sv[1] on return */
            _exit(0);
        }

        /* parent: send a request with headers > 256 bytes */
        close(sv[1]);

        /* Build a request whose header block exceeds the 256-byte limit.
         * We pad with a long X-Padding header (300 bytes of 'A'). */
        char big_req[512];
        memset(big_req, 0, sizeof(big_req));
        {
            /* Request line + Host header = ~30 bytes */
            int off = snprintf(big_req, sizeof(big_req),
                "GET / HTTP/1.1\r\n"
                "Host: localhost\r\n"
                "X-Padding: ");
            /* Pad to well over 256 bytes total */
            int pad = 300;
            if (off + pad + 4 < (int)sizeof(big_req)) {
                memset(big_req + off, 'A', (size_t)pad);
                off += pad;
                big_req[off++] = '\r'; big_req[off++] = '\n';
                big_req[off++] = '\r'; big_req[off++] = '\n';
            }
            write(sv[0], big_req, (size_t)off);
        }

        /* Read response */
        char resp[256];
        memset(resp, 0, sizeof(resp));
        ssize_t rn = read(sv[0], resp, sizeof(resp) - 1);
        if (rn > 0) resp[rn] = '\0';

        close(sv[0]);
        waitpid(pid, NULL, 0);

        ASSERT(strstr(resp, "431") != NULL,
               "oversized header → 431 response");

        /* Restore default limits for subsequent tests */
        http_set_limits(HTTP_DEFAULT_MAX_HEADER_SIZE,
                        HTTP_DEFAULT_MAX_BODY_SIZE,
                        HTTP_DEFAULT_TIMEOUT_SECS);
    }

    /*
     * Test: Content-Length > max_body → 413 Payload Too Large
     *
     * Set the body limit to 512 bytes, then send a well-formed request
     * with Content-Length: 2000000 (> 512).  The server must reject with 413
     * without reading the body.
     */
    {
        /* Reduce body limit to 512 bytes */
        http_set_limits(0, 512, 1);

        int sv[2];
        ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0,
               "oversized-body: socketpair created");

        pid_t pid = fork();
        if (pid == 0) {
            close(sv[0]);
            http_handle_fd(sv[1]);
            _exit(0);
        }

        close(sv[1]);

        /* Request with Content-Length well above the 512-byte limit */
        const char *big_cl_req =
            "POST /upload HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "Content-Length: 2000000\r\n"
            "Connection: close\r\n"
            "\r\n";
        write(sv[0], big_cl_req, strlen(big_cl_req));

        char resp[256];
        memset(resp, 0, sizeof(resp));
        ssize_t rn = read(sv[0], resp, sizeof(resp) - 1);
        if (rn > 0) resp[rn] = '\0';

        close(sv[0]);
        waitpid(pid, NULL, 0);

        ASSERT(strstr(resp, "413") != NULL,
               "Content-Length > max_body → 413 response");

        /* Restore defaults */
        http_set_limits(HTTP_DEFAULT_MAX_HEADER_SIZE,
                        HTTP_DEFAULT_MAX_BODY_SIZE,
                        HTTP_DEFAULT_TIMEOUT_SECS);
    }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

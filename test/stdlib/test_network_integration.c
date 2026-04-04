/*
 * test_network_integration.c — Integration tests for router + sse + ws
 *                              working together (Story 15.1.5).
 *
 * No actual network I/O; exercises protocol layers in combination.
 *
 * Build:
 *   $(CC) $(CFLAGS) -o test_network_integration \
 *       test_network_integration.c \
 *       ../src/stdlib/router.c \
 *       ../src/stdlib/sse.c \
 *       ../src/stdlib/ws.c
 *
 * RFC 6455 handshake test vector (§1.3):
 *   ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "../../src/stdlib/router.h"
#include "../../src/stdlib/sse.h"
#include "../../src/stdlib/ws.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

#define ASSERT_INTEQ(a, b, msg) \
    ASSERT((a) == (b), msg)

/* Assert that needle appears somewhere inside haystack. */
#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && strstr((haystack), (needle)) != NULL, msg)

/* ----------------------------------------------------------------------- */
/* Shared handler state — static globals let handlers report back to tests  */
/* ----------------------------------------------------------------------- */

static int g_handler_a_called = 0;
static int g_handler_b_called = 0;
static int g_handler_c_called = 0;

static char g_captured_version[64];
static char g_captured_id[64];
static char g_captured_query[64];

/* ----------------------------------------------------------------------- */
/* Handlers                                                                  */
/* ----------------------------------------------------------------------- */

static TkRouteResp handler_a(TkRouteCtx ctx)
{
    (void)ctx;
    g_handler_a_called++;
    return router_resp_ok("response_a", "text/plain");
}

static TkRouteResp handler_b(TkRouteCtx ctx)
{
    (void)ctx;
    g_handler_b_called++;
    return router_resp_ok("response_b", "text/plain");
}

static TkRouteResp handler_c(TkRouteCtx ctx)
{
    (void)ctx;
    g_handler_c_called++;
    return router_resp_json("{\"route\":\"c\"}");
}

static TkRouteResp handler_stream(TkRouteCtx ctx)
{
    (void)ctx;
    /* Demonstrate router + SSE composition: build an SSE body in the handler */
    TkSseCtx *sse = sse_new();
    const char *ev1 = sse_emitdata(sse, "stream-line-1");
    const char *ev2 = sse_emitdata(sse, "stream-line-2");

    /* Concatenate both events into a single buffer */
    static char body[512];
    body[0] = '\0';
    if (ev1) { strncat(body, ev1, sizeof(body) - strlen(body) - 1); free((void *)ev1); }
    if (ev2) { strncat(body, ev2, sizeof(body) - strlen(body) - 1); free((void *)ev2); }

    sse_free(sse);
    return router_resp_ok(body, "text/event-stream");
}

static TkRouteResp handler_ws_upgrade(TkRouteCtx ctx)
{
    (void)ctx;
    return router_resp_status(101, "Switching Protocols");
}

static TkRouteResp handler_versioned(TkRouteCtx ctx)
{
    g_captured_version[0] = '\0';
    g_captured_id[0]      = '\0';
    g_captured_query[0]   = '\0';

    for (uint64_t i = 0; i < ctx.nparam; i++) {
        if (strcmp(ctx.param_names[i], "version") == 0)
            strncpy(g_captured_version, ctx.param_values[i],
                    sizeof(g_captured_version) - 1);
        if (strcmp(ctx.param_names[i], "id") == 0)
            strncpy(g_captured_id, ctx.param_values[i],
                    sizeof(g_captured_id) - 1);
    }

    if (ctx.query) {
        const char *fmt = router_query_get(ctx.query, "format");
        if (fmt) strncpy(g_captured_query, fmt, sizeof(g_captured_query) - 1);
    }

    return router_resp_json("{}");
}

static int g_exact_called  = 0;
static int g_param_called  = 0;

static TkRouteResp handler_exact_index(TkRouteCtx ctx)
{
    (void)ctx;
    g_exact_called++;
    return router_resp_ok("exact-index", "text/html");
}

static TkRouteResp handler_param_file(TkRouteCtx ctx)
{
    (void)ctx;
    g_param_called++;
    return router_resp_ok("param-file", "text/plain");
}

/* ----------------------------------------------------------------------- */
/* Test 1: Router + response pipeline — 3 routes, all dispatched correctly  */
/* ----------------------------------------------------------------------- */
static void test_router_response_pipeline(void)
{
    printf("-- test_router_response_pipeline\n");

    TkRouter *r = router_new();
    router_get(r,  "/alpha",        handler_a);
    router_post(r, "/beta",         handler_b);
    router_get(r,  "/gamma",        handler_c);

    TkRouteResp ra = router_dispatch(r, "GET",  "/alpha", NULL, NULL);
    TkRouteResp rb = router_dispatch(r, "POST", "/beta",  NULL, NULL);
    TkRouteResp rc = router_dispatch(r, "GET",  "/gamma", NULL, NULL);

    ASSERT(g_handler_a_called == 1, "handler_a called once for GET /alpha");
    ASSERT(g_handler_b_called == 1, "handler_b called once for POST /beta");
    ASSERT(g_handler_c_called == 1, "handler_c called once for GET /gamma");

    ASSERT_INTEQ(ra.status, 200,     "GET /alpha -> status 200");
    ASSERT_STREQ(ra.body, "response_a", "GET /alpha -> body 'response_a'");

    ASSERT_INTEQ(rb.status, 200,     "POST /beta -> status 200");
    ASSERT_STREQ(rb.body, "response_b", "POST /beta -> body 'response_b'");

    ASSERT_INTEQ(rc.status, 200,     "GET /gamma -> status 200");
    ASSERT_CONTAINS(rc.body, "route", "GET /gamma -> JSON body contains 'route'");
    ASSERT_STREQ(rc.content_type, "application/json",
                 "GET /gamma -> content_type application/json");

    router_free(r);
}

/* ----------------------------------------------------------------------- */
/* Test 2: SSE event sequence — 5 data events + keepalive + close           */
/* ----------------------------------------------------------------------- */
static void test_sse_event_sequence(void)
{
    printf("-- test_sse_event_sequence\n");

    TkSseCtx *ctx = sse_new();
    ASSERT(ctx != NULL, "sse_new returns non-NULL");
    ASSERT(sse_is_closed(ctx) == 0, "sse_is_closed == 0 after sse_new");

    const char *events[5];
    char label[64];
    char data_str[32];
    for (int i = 0; i < 5; i++) {
        snprintf(data_str, sizeof(data_str), "event-%d", i);
        events[i] = sse_emitdata(ctx, data_str);
        snprintf(label, sizeof(label), "sse_emitdata event-%d not NULL", i);
        ASSERT(events[i] != NULL, label);
        snprintf(label, sizeof(label), "sse_emitdata event-%d contains 'data:'", i);
        ASSERT_CONTAINS(events[i], "data:", label);
    }

    /* Verify each emission contains the right payload */
    ASSERT_CONTAINS(events[0], "event-0", "sse event 0 payload correct");
    ASSERT_CONTAINS(events[4], "event-4", "sse event 4 payload correct");

    /* Keepalive */
    const char *ka = sse_keepalive(ctx);
    ASSERT(ka != NULL, "sse_keepalive returns non-NULL");
    ASSERT_CONTAINS(ka, ": keepalive", "sse_keepalive contains ': keepalive'");
    free((void *)ka);

    /* Close */
    sse_close(ctx);
    ASSERT(sse_is_closed(ctx) == 1, "sse_is_closed == 1 after sse_close");

    /* Emit after close returns NULL */
    const char *after = sse_emitdata(ctx, "should-not-emit");
    ASSERT(after == NULL, "sse_emitdata after close returns NULL");

    for (int i = 0; i < 5; i++) free((void *)events[i]);
    sse_free(ctx);
}

/* ----------------------------------------------------------------------- */
/* Test 3: Router + SSE composition                                         */
/* ----------------------------------------------------------------------- */
static void test_router_sse_composition(void)
{
    printf("-- test_router_sse_composition\n");

    TkRouter *r = router_new();
    router_get(r, "/stream", handler_stream);

    TkRouteResp resp = router_dispatch(r, "GET", "/stream", NULL, NULL);

    ASSERT_INTEQ(resp.status, 200, "GET /stream -> status 200");
    ASSERT_STREQ(resp.content_type, "text/event-stream",
                 "GET /stream -> content_type text/event-stream");
    ASSERT_CONTAINS(resp.body, "data:", "GET /stream body contains 'data:'");
    ASSERT_CONTAINS(resp.body, "stream-line-1",
                    "GET /stream body contains 'stream-line-1'");
    ASSERT_CONTAINS(resp.body, "stream-line-2",
                    "GET /stream body contains 'stream-line-2'");

    router_free(r);
}

/* ----------------------------------------------------------------------- */
/* Test 4: WebSocket framing pipeline — text, binary, ping round-trips      */
/* ----------------------------------------------------------------------- */
static void test_ws_framing_pipeline(void)
{
    printf("-- test_ws_framing_pipeline\n");

    /* --- Text frame --- */
    const uint8_t *txt_payload = (const uint8_t *)"hello ws";
    WsEncodeResult enc_txt = ws_encode_frame(WS_TEXT, txt_payload, 8, 0);
    ASSERT(!enc_txt.is_err, "encode text frame: no error");

    uint64_t consumed = 0;
    WsFrameResult dec_txt = ws_decode_frame(enc_txt.data, enc_txt.len, &consumed);
    ASSERT(!dec_txt.is_err,                    "decode text frame: no error");
    ASSERT(dec_txt.frame != NULL,              "decode text frame: frame not NULL");
    ASSERT(dec_txt.frame->opcode == WS_TEXT,   "decode text frame: opcode WS_TEXT");
    ASSERT(dec_txt.frame->payload_len == 8,    "decode text frame: payload_len 8");
    ASSERT(memcmp(dec_txt.frame->payload, "hello ws", 8) == 0,
           "decode text frame: payload intact");
    ws_frame_free(dec_txt.frame);
    ws_encode_result_free(&enc_txt);

    /* --- Binary frame --- */
    const uint8_t bin_payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    WsEncodeResult enc_bin = ws_encode_frame(WS_BINARY, bin_payload, 4, 0);
    ASSERT(!enc_bin.is_err, "encode binary frame: no error");

    consumed = 0;
    WsFrameResult dec_bin = ws_decode_frame(enc_bin.data, enc_bin.len, &consumed);
    ASSERT(!dec_bin.is_err,                    "decode binary frame: no error");
    ASSERT(dec_bin.frame->opcode == WS_BINARY, "decode binary frame: opcode WS_BINARY");
    ASSERT(dec_bin.frame->payload_len == 4,    "decode binary frame: payload_len 4");
    ASSERT(memcmp(dec_bin.frame->payload, bin_payload, 4) == 0,
           "decode binary frame: payload intact");
    ws_frame_free(dec_bin.frame);
    ws_encode_result_free(&enc_bin);

    /* --- Ping frame --- */
    const uint8_t *ping_payload = (const uint8_t *)"ping";
    WsEncodeResult enc_ping = ws_encode_frame(WS_PING, ping_payload, 4, 0);
    ASSERT(!enc_ping.is_err, "encode ping frame: no error");

    consumed = 0;
    WsFrameResult dec_ping = ws_decode_frame(enc_ping.data, enc_ping.len, &consumed);
    ASSERT(!dec_ping.is_err,                   "decode ping frame: no error");
    ASSERT(dec_ping.frame->opcode == WS_PING,  "decode ping frame: opcode WS_PING");
    ASSERT(dec_ping.frame->payload_len == 4,   "decode ping frame: payload_len 4");
    ws_frame_free(dec_ping.frame);
    ws_encode_result_free(&enc_ping);
}

/* ----------------------------------------------------------------------- */
/* Test 5: WS handshake key — RFC 6455 §1.3 test vector                    */
/* ----------------------------------------------------------------------- */
static void test_ws_accept_key_rfc_vector(void)
{
    printf("-- test_ws_accept_key_rfc_vector\n");

    const char *client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *expected   = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";

    const char *accept = ws_accept_key(client_key);
    ASSERT(accept != NULL, "ws_accept_key returns non-NULL");
    ASSERT_STREQ(accept, expected, "ws_accept_key matches RFC 6455 §1.3 vector");

    free((void *)accept);
}

/* ----------------------------------------------------------------------- */
/* Test 6: Router + WS upgrade detection                                    */
/* ----------------------------------------------------------------------- */
static void test_router_ws_upgrade_detection(void)
{
    printf("-- test_router_ws_upgrade_detection\n");

    TkRouter *r = router_new();
    router_get(r, "/ws", handler_ws_upgrade);

    /* Simulate a WS upgrade request header block */
    const char *upgrade_headers =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    const char *plain_headers =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "\r\n";

    ASSERT(ws_is_upgrade_request(upgrade_headers) == 1,
           "ws_is_upgrade_request detects Upgrade: websocket");
    ASSERT(ws_is_upgrade_request(plain_headers) == 0,
           "ws_is_upgrade_request rejects plain GET");

    /* Dispatch the route and verify upgrade handler responds 101 */
    TkRouteResp resp = router_dispatch(r, "GET", "/ws", NULL, NULL);
    ASSERT_INTEQ(resp.status, 101, "GET /ws route -> status 101 Switching Protocols");

    router_free(r);
}

/* ----------------------------------------------------------------------- */
/* Test 7: Multi-param route + query string                                 */
/* ----------------------------------------------------------------------- */
static void test_multi_param_route_query_string(void)
{
    printf("-- test_multi_param_route_query_string\n");

    TkRouter *r = router_new();
    router_get(r, "/api/:version/users/:id", handler_versioned);

    TkRouteResp resp = router_dispatch(r, "GET", "/api/v2/users/99",
                                       "format=json&limit=10", NULL);

    ASSERT_INTEQ(resp.status, 200, "multi-param route -> status 200");
    ASSERT_STREQ(g_captured_version, "v2",   "multi-param: version param == 'v2'");
    ASSERT_STREQ(g_captured_id,      "99",   "multi-param: id param == '99'");
    ASSERT_STREQ(g_captured_query,   "json", "multi-param: query format == 'json'");

    /* Also verify limit query param via router_query_get directly */
    const char *limit = router_query_get("format=json&limit=10", "limit");
    ASSERT(limit != NULL, "router_query_get 'limit' not NULL");
    ASSERT_STREQ(limit, "10", "router_query_get 'limit' == '10'");

    router_free(r);
}

/* ----------------------------------------------------------------------- */
/* Test 8: SSE multi-line data — '\n' in data splits into two data: lines   */
/* ----------------------------------------------------------------------- */
static void test_sse_multiline_data(void)
{
    printf("-- test_sse_multiline_data\n");

    TkSseCtx *ctx = sse_new();
    const char *out = sse_emitdata(ctx, "line one\nline two");

    ASSERT(out != NULL, "sse_emitdata multi-line returns non-NULL");
    ASSERT_CONTAINS(out, "data: line one\n",  "multi-line sse: first data: line");
    ASSERT_CONTAINS(out, "data: line two\n",  "multi-line sse: second data: line");

    /* Both data: lines should appear */
    const char *first  = strstr(out, "data: line one");
    const char *second = strstr(out, "data: line two");
    ASSERT(first != NULL && second != NULL && second > first,
           "multi-line sse: second data: line follows first");

    free((void *)out);
    sse_free(ctx);
}

/* ----------------------------------------------------------------------- */
/* Test 9: WS large payload — 200-byte text frame uses 16-bit extended len  */
/* ----------------------------------------------------------------------- */
static void test_ws_large_payload(void)
{
    printf("-- test_ws_large_payload\n");

    /* Build a 200-byte payload */
    uint8_t large[200];
    for (int i = 0; i < 200; i++) large[i] = (uint8_t)('A' + (i % 26));

    WsEncodeResult enc = ws_encode_frame(WS_TEXT, large, 200, 0);
    ASSERT(!enc.is_err,    "encode 200-byte frame: no error");
    ASSERT(enc.data != NULL, "encode 200-byte frame: data not NULL");

    /* For payloads 126–65535 bytes the second byte is 126 (0x7E),
     * followed by a 2-byte big-endian length.  Minimum wire size is
     * 2 (base header) + 2 (extended length) + 200 (payload) = 204. */
    ASSERT(enc.len >= 204, "encode 200-byte frame: wire length >= 204 (16-bit ext)");
    /* The second byte's payload length field should be 126 */
    ASSERT((enc.data[1] & 0x7F) == 126,
           "encode 200-byte frame: second byte payload field == 126");

    uint64_t consumed = 0;
    WsFrameResult dec = ws_decode_frame(enc.data, enc.len, &consumed);
    ASSERT(!dec.is_err,                  "decode 200-byte frame: no error");
    ASSERT(dec.frame != NULL,            "decode 200-byte frame: frame not NULL");
    ASSERT(dec.frame->opcode == WS_TEXT, "decode 200-byte frame: opcode WS_TEXT");
    ASSERT(dec.frame->payload_len == 200,"decode 200-byte frame: payload_len 200");
    ASSERT(memcmp(dec.frame->payload, large, 200) == 0,
           "decode 200-byte frame: payload intact");
    ASSERT(consumed == enc.len,          "decode 200-byte frame: consumed == enc.len");

    ws_frame_free(dec.frame);
    ws_encode_result_free(&enc);
}

/* ----------------------------------------------------------------------- */
/* Test 10: Router priority — exact match wins over param match             */
/* ----------------------------------------------------------------------- */
static void test_router_exact_beats_param(void)
{
    printf("-- test_router_exact_beats_param\n");

    TkRouter *r = router_new();
    router_get(r, "/files/:name",       handler_param_file);
    router_get(r, "/files/index.html",  handler_exact_index);

    g_exact_called = 0;
    g_param_called = 0;

    TkRouteResp resp_exact = router_dispatch(r, "GET", "/files/index.html", NULL, NULL);
    ASSERT_INTEQ(resp_exact.status, 200,
                 "GET /files/index.html -> status 200");
    ASSERT_STREQ(resp_exact.body, "exact-index",
                 "GET /files/index.html -> exact handler body 'exact-index'");
    ASSERT(g_exact_called == 1, "exact handler called once");
    ASSERT(g_param_called == 0, "param handler not called for exact match");

    /* Param handler still reachable for other names */
    TkRouteResp resp_param = router_dispatch(r, "GET", "/files/readme.txt", NULL, NULL);
    ASSERT_INTEQ(resp_param.status, 200,
                 "GET /files/readme.txt -> status 200");
    ASSERT_STREQ(resp_param.body, "param-file",
                 "GET /files/readme.txt -> param handler body 'param-file'");
    ASSERT(g_param_called == 1, "param handler called for non-exact path");

    router_free(r);
}

/* ----------------------------------------------------------------------- */
/* main                                                                      */
/* ----------------------------------------------------------------------- */
int main(void)
{
    printf("=== test_network_integration ===\n");

    test_router_response_pipeline();
    test_sse_event_sequence();
    test_router_sse_composition();
    test_ws_framing_pipeline();
    test_ws_accept_key_rfc_vector();
    test_router_ws_upgrade_detection();
    test_multi_param_route_query_string();
    test_sse_multiline_data();
    test_ws_large_payload();
    test_router_exact_beats_param();

    printf("=== %s ===\n", failures == 0 ? "ALL PASS" : "FAILURES");
    if (failures != 0) fprintf(stderr, "%d test(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}

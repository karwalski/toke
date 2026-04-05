/*
 * test_ws_handshake_integration.c — WebSocket protocol handshake simulation
 * (Story 21.1.6).
 *
 * All in-memory using ws encode/decode functions — no network connections.
 *
 * Build: cc -std=c99 -Wall -Wextra -Wpedantic -Werror -o test_ws_handshake_integration \
 *            test_ws_handshake_integration.c ../../src/stdlib/ws.c -I../../src/stdlib
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../src/stdlib/ws.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

/* -----------------------------------------------------------------------
 * Helper: encode then decode a frame, returning the decoded frame.
 * Caller must ws_frame_free the returned frame.
 * ----------------------------------------------------------------------- */
static WsFrame *roundtrip(WsOpcode opcode, const uint8_t *payload,
                           uint64_t plen, int mask)
{
    WsEncodeResult enc = ws_encode_frame(opcode, payload, plen, mask);
    WsFrameResult dec;
    uint64_t consumed = 0;

    if (enc.is_err || !enc.data) return NULL;

    dec = ws_decode_frame(enc.data, enc.len, &consumed);
    ws_encode_result_free(&enc);

    if (dec.is_err || !dec.frame) return NULL;
    return dec.frame;
}

/* -----------------------------------------------------------------------
 * Test 1: Build WS upgrade request headers and validate
 * ----------------------------------------------------------------------- */
static void test_upgrade_request_headers(void)
{
    const char *headers =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ASSERT(ws_is_upgrade_request(headers),
           "upgrade request: detected as upgrade");

    /* Non-upgrade request */
    ASSERT(!ws_is_upgrade_request("GET / HTTP/1.1\r\nHost: x\r\n\r\n"),
           "upgrade request: plain GET is not upgrade");

    /* Case-insensitive detection */
    ASSERT(ws_is_upgrade_request("UPGRADE: WebSocket\r\n"),
           "upgrade request: case-insensitive header name");
    ASSERT(ws_is_upgrade_request("upgrade: WEBSOCKET\r\n"),
           "upgrade request: case-insensitive value");

    /* NULL headers */
    ASSERT(!ws_is_upgrade_request(NULL),
           "upgrade request: NULL returns 0");
}

/* -----------------------------------------------------------------------
 * Test 2: Compute the Sec-WebSocket-Accept key (RFC 6455 test vector)
 * ----------------------------------------------------------------------- */
static void test_accept_key_computation(void)
{
    /* RFC 6455 section 1.3 example */
    const char *client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *expected   = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    const char *result;

    result = ws_accept_key(client_key);
    ASSERT(result != NULL, "accept key: not NULL");
    ASSERT_STREQ(result, expected,
                 "accept key: matches RFC 6455 test vector");
    free((void *)result);

    /* NULL input */
    ASSERT(ws_accept_key(NULL) == NULL, "accept key: NULL input returns NULL");

    /* Another key to verify the computation is general */
    {
        const char *key2 = "x3JJHMbDL1EzLkh9GBhXDw==";
        const char *accept2 = ws_accept_key(key2);
        ASSERT(accept2 != NULL, "accept key: second key not NULL");
        ASSERT(strlen(accept2) > 0, "accept key: second key non-empty");
        free((void *)accept2);
    }
}

/* -----------------------------------------------------------------------
 * Test 3: Text frame encode + decode round-trip
 * ----------------------------------------------------------------------- */
static void test_text_frame_roundtrip(void)
{
    const char *msg = "Hello, WebSocket!";
    uint64_t len = (uint64_t)strlen(msg);
    WsFrame *f = roundtrip(WS_TEXT, (const uint8_t *)msg, len, 0);

    ASSERT(f != NULL, "text roundtrip: frame not NULL");
    if (f) {
        ASSERT(f->opcode == WS_TEXT, "text roundtrip: opcode=WS_TEXT");
        ASSERT(f->payload_len == len, "text roundtrip: payload length matches");
        ASSERT(memcmp(f->payload, msg, (size_t)len) == 0,
               "text roundtrip: payload matches");
        ASSERT(f->is_final == 1, "text roundtrip: FIN bit set");
        ws_frame_free(f);
    }
}

/* -----------------------------------------------------------------------
 * Test 4: Binary frame encode + decode round-trip
 * ----------------------------------------------------------------------- */
static void test_binary_frame_roundtrip(void)
{
    uint8_t data[] = {0x00, 0x01, 0x02, 0xFF, 0xFE, 0x80, 0x7F, 0x00};
    uint64_t len = sizeof(data);
    WsFrame *f = roundtrip(WS_BINARY, data, len, 0);

    ASSERT(f != NULL, "binary roundtrip: frame not NULL");
    if (f) {
        ASSERT(f->opcode == WS_BINARY, "binary roundtrip: opcode=WS_BINARY");
        ASSERT(f->payload_len == len, "binary roundtrip: payload length matches");
        ASSERT(memcmp(f->payload, data, (size_t)len) == 0,
               "binary roundtrip: payload matches");
        ws_frame_free(f);
    }
}

/* -----------------------------------------------------------------------
 * Test 5: Ping frame encode, decode as pong response
 * ----------------------------------------------------------------------- */
static void test_ping_pong(void)
{
    const uint8_t ping_data[] = "ping-payload";
    uint64_t plen = (uint64_t)strlen((const char *)ping_data);
    WsFrame *ping_frame;
    WsFrame *pong_frame;

    /* Encode and decode a ping */
    ping_frame = roundtrip(WS_PING, ping_data, plen, 0);
    ASSERT(ping_frame != NULL, "ping: frame not NULL");
    if (ping_frame) {
        ASSERT(ping_frame->opcode == WS_PING, "ping: opcode=WS_PING");
        ASSERT(ping_frame->payload_len == plen, "ping: payload length matches");
        ASSERT(memcmp(ping_frame->payload, ping_data, (size_t)plen) == 0,
               "ping: payload matches");

        /* Respond with pong carrying same payload (RFC 6455 section 5.5.3) */
        pong_frame = roundtrip(WS_PONG, ping_frame->payload,
                               ping_frame->payload_len, 0);
        ASSERT(pong_frame != NULL, "pong: frame not NULL");
        if (pong_frame) {
            ASSERT(pong_frame->opcode == WS_PONG, "pong: opcode=WS_PONG");
            ASSERT(pong_frame->payload_len == plen,
                   "pong: payload length matches ping");
            ASSERT(memcmp(pong_frame->payload, ping_data, (size_t)plen) == 0,
                   "pong: payload matches ping payload");
            ws_frame_free(pong_frame);
        }
        ws_frame_free(ping_frame);
    }
}

/* -----------------------------------------------------------------------
 * Test 6: Close frame with status code 1000
 * ----------------------------------------------------------------------- */
static void test_close_frame(void)
{
    /* RFC 6455 section 5.5.1: status code is first 2 bytes, big-endian */
    uint8_t close_payload[2];
    WsFrame *f;

    close_payload[0] = (uint8_t)(1000 >> 8);   /* 0x03 */
    close_payload[1] = (uint8_t)(1000 & 0xFF);  /* 0xE8 */

    f = roundtrip(WS_CLOSE, close_payload, 2, 0);
    ASSERT(f != NULL, "close: frame not NULL");
    if (f) {
        uint16_t status;
        ASSERT(f->opcode == WS_CLOSE, "close: opcode=WS_CLOSE");
        ASSERT(f->payload_len == 2, "close: payload length=2");
        status = (uint16_t)((f->payload[0] << 8) | f->payload[1]);
        ASSERT(status == 1000, "close: status code=1000");
        ws_frame_free(f);
    }

    /* Close with reason string */
    {
        const char *reason = "Normal Closure";
        uint64_t reason_len = (uint64_t)strlen(reason);
        uint64_t total = 2 + reason_len;
        uint8_t *buf = (uint8_t *)malloc(total);
        WsFrame *f2;

        buf[0] = (uint8_t)(1000 >> 8);
        buf[1] = (uint8_t)(1000 & 0xFF);
        memcpy(buf + 2, reason, (size_t)reason_len);

        f2 = roundtrip(WS_CLOSE, buf, total, 0);
        ASSERT(f2 != NULL, "close+reason: frame not NULL");
        if (f2) {
            uint16_t s = (uint16_t)((f2->payload[0] << 8) | f2->payload[1]);
            ASSERT(s == 1000, "close+reason: status code=1000");
            ASSERT(memcmp(f2->payload + 2, reason, (size_t)reason_len) == 0,
                   "close+reason: reason string matches");
            ws_frame_free(f2);
        }
        free(buf);
    }
}

/* -----------------------------------------------------------------------
 * Test 7: Masked vs unmasked frames
 * ----------------------------------------------------------------------- */
static void test_masked_vs_unmasked(void)
{
    const char *msg = "mask test";
    uint64_t len = (uint64_t)strlen(msg);
    WsEncodeResult enc_unmasked, enc_masked;
    WsFrameResult dec;
    uint64_t consumed;

    /* Unmasked: byte[1] bit 7 should be 0 */
    enc_unmasked = ws_encode_frame(WS_TEXT, (const uint8_t *)msg, len, 0);
    ASSERT(!enc_unmasked.is_err, "unmasked: encode ok");
    ASSERT((enc_unmasked.data[1] & 0x80) == 0,
           "unmasked: MASK bit is clear");

    /* Masked: byte[1] bit 7 should be 1 */
    enc_masked = ws_encode_frame(WS_TEXT, (const uint8_t *)msg, len, 1);
    ASSERT(!enc_masked.is_err, "masked: encode ok");
    ASSERT((enc_masked.data[1] & 0x80) != 0,
           "masked: MASK bit is set");

    /* Masked frame is larger (4 extra bytes for masking key) */
    ASSERT(enc_masked.len == enc_unmasked.len + 4,
           "masked: 4 bytes larger than unmasked");

    /* Decode masked frame should recover original payload */
    consumed = 0;
    dec = ws_decode_frame(enc_masked.data, enc_masked.len, &consumed);
    ASSERT(!dec.is_err, "masked: decode ok");
    if (dec.frame) {
        ASSERT(dec.frame->payload_len == len,
               "masked: decoded payload length matches");
        ASSERT(memcmp(dec.frame->payload, msg, (size_t)len) == 0,
               "masked: decoded payload matches original");
        ws_frame_free(dec.frame);
    }

    ws_encode_result_free(&enc_unmasked);
    ws_encode_result_free(&enc_masked);
}

/* -----------------------------------------------------------------------
 * Test 8: Various payload sizes (0, 125, 126, 65536 bytes)
 * ----------------------------------------------------------------------- */
static void test_payload_sizes(void)
{
    uint8_t *buf;
    uint64_t i;
    WsFrame *f;

    /* Size 0: empty payload */
    f = roundtrip(WS_TEXT, NULL, 0, 0);
    ASSERT(f != NULL, "size 0: frame not NULL");
    if (f) {
        ASSERT(f->payload_len == 0, "size 0: payload_len=0");
        ws_frame_free(f);
    }

    /* Size 125: maximum 7-bit length (fits in single byte) */
    buf = (uint8_t *)malloc(125);
    for (i = 0; i < 125; i++) buf[i] = (uint8_t)(i & 0xFF);
    f = roundtrip(WS_BINARY, buf, 125, 0);
    ASSERT(f != NULL, "size 125: frame not NULL");
    if (f) {
        ASSERT(f->payload_len == 125, "size 125: payload_len=125");
        ASSERT(memcmp(f->payload, buf, 125) == 0, "size 125: payload matches");
        ws_frame_free(f);
    }
    free(buf);

    /* Verify the encoded header for 125-byte payload uses 7-bit length */
    {
        uint8_t tmp[125];
        WsEncodeResult enc;
        memset(tmp, 'A', 125);
        enc = ws_encode_frame(WS_TEXT, tmp, 125, 0);
        ASSERT(!enc.is_err, "size 125 header: encode ok");
        /* byte[1] lower 7 bits should be 125 (no extended length) */
        ASSERT((enc.data[1] & 0x7F) == 125,
               "size 125 header: 7-bit length=125");
        /* total = 2 header + 125 payload */
        ASSERT(enc.len == 2 + 125, "size 125 header: total length=127");
        ws_encode_result_free(&enc);
    }

    /* Size 126: triggers 16-bit extended length */
    buf = (uint8_t *)malloc(126);
    for (i = 0; i < 126; i++) buf[i] = (uint8_t)(i & 0xFF);
    f = roundtrip(WS_BINARY, buf, 126, 0);
    ASSERT(f != NULL, "size 126: frame not NULL");
    if (f) {
        ASSERT(f->payload_len == 126, "size 126: payload_len=126");
        ASSERT(memcmp(f->payload, buf, 126) == 0, "size 126: payload matches");
        ws_frame_free(f);
    }
    free(buf);

    /* Verify the encoded header for 126-byte payload uses 16-bit length */
    {
        uint8_t tmp[126];
        WsEncodeResult enc;
        memset(tmp, 'B', 126);
        enc = ws_encode_frame(WS_TEXT, tmp, 126, 0);
        ASSERT(!enc.is_err, "size 126 header: encode ok");
        /* byte[1] lower 7 bits should be 126 (16-bit extended) */
        ASSERT((enc.data[1] & 0x7F) == 126,
               "size 126 header: extended length marker=126");
        /* total = 2 base + 2 extended + 126 payload = 130 */
        ASSERT(enc.len == 4 + 126, "size 126 header: total length=130");
        ws_encode_result_free(&enc);
    }

    /* Size 65536: triggers 64-bit extended length */
    buf = (uint8_t *)malloc(65536);
    if (buf) {
        for (i = 0; i < 65536; i++) buf[i] = (uint8_t)(i & 0xFF);
        f = roundtrip(WS_BINARY, buf, 65536, 0);
        ASSERT(f != NULL, "size 65536: frame not NULL");
        if (f) {
            ASSERT(f->payload_len == 65536, "size 65536: payload_len=65536");
            ASSERT(memcmp(f->payload, buf, 65536) == 0,
                   "size 65536: payload matches");
            ws_frame_free(f);
        }

        /* Verify the encoded header uses 64-bit extended length */
        {
            WsEncodeResult enc = ws_encode_frame(WS_BINARY, buf, 65536, 0);
            ASSERT(!enc.is_err, "size 65536 header: encode ok");
            /* byte[1] lower 7 bits should be 127 (64-bit extended) */
            ASSERT((enc.data[1] & 0x7F) == 127,
                   "size 65536 header: extended length marker=127");
            /* total = 2 base + 8 extended + 65536 payload */
            ASSERT(enc.len == 10 + 65536,
                   "size 65536 header: total length=65546");
            ws_encode_result_free(&enc);
        }
        free(buf);
    } else {
        fprintf(stderr, "SKIP: size 65536 (malloc failed)\n");
    }

    /* Masked versions of boundary sizes */
    buf = (uint8_t *)malloc(126);
    for (i = 0; i < 126; i++) buf[i] = (uint8_t)(i & 0xFF);
    f = roundtrip(WS_BINARY, buf, 126, 1);
    ASSERT(f != NULL, "size 126 masked: frame not NULL");
    if (f) {
        ASSERT(f->payload_len == 126, "size 126 masked: payload_len=126");
        ASSERT(memcmp(f->payload, buf, 126) == 0,
               "size 126 masked: payload matches");
        ws_frame_free(f);
    }
    free(buf);
}

/* -----------------------------------------------------------------------
 * Test 9: Full protocol simulation — open, text, ping/pong, close
 * ----------------------------------------------------------------------- */
static void test_full_protocol_simulation(void)
{
    /*
     * Simulate a complete WebSocket session, all in-memory:
     *   1. Client builds upgrade request, server validates
     *   2. Server computes accept key
     *   3. Client sends text message (masked)
     *   4. Server sends text reply (unmasked)
     *   5. Server sends ping (unmasked)
     *   6. Client sends pong with same payload (masked)
     *   7. Client sends close (masked)
     *   8. Server sends close (unmasked)
     */
    const char *client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *expected_accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    const char *accept;
    WsFrame *f;
    uint8_t close_payload[2];

    printf("\n--- Full protocol simulation ---\n");

    /* Step 1: Client upgrade request */
    {
        const char *request =
            "GET /chat HTTP/1.1\r\n"
            "Host: server.example.com\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n";
        ASSERT(ws_is_upgrade_request(request),
               "sim: server detects upgrade request");
    }

    /* Step 2: Server computes accept key */
    accept = ws_accept_key(client_key);
    ASSERT_STREQ(accept, expected_accept,
                 "sim: accept key matches RFC 6455");
    free((void *)accept);

    /* Step 3: Client sends masked text */
    f = roundtrip(WS_TEXT, (const uint8_t *)"Hello server", 12, 1);
    ASSERT(f != NULL, "sim: client text frame decoded");
    if (f) {
        ASSERT(f->opcode == WS_TEXT, "sim: client msg opcode=TEXT");
        ASSERT(memcmp(f->payload, "Hello server", 12) == 0,
               "sim: client msg payload matches");
        ws_frame_free(f);
    }

    /* Step 4: Server sends unmasked text reply */
    f = roundtrip(WS_TEXT, (const uint8_t *)"Hello client", 12, 0);
    ASSERT(f != NULL, "sim: server text frame decoded");
    if (f) {
        ASSERT(f->opcode == WS_TEXT, "sim: server msg opcode=TEXT");
        ASSERT(memcmp(f->payload, "Hello client", 12) == 0,
               "sim: server msg payload matches");
        ws_frame_free(f);
    }

    /* Step 5: Server sends ping */
    f = roundtrip(WS_PING, (const uint8_t *)"heartbeat", 9, 0);
    ASSERT(f != NULL, "sim: server ping decoded");
    if (f) {
        ASSERT(f->opcode == WS_PING, "sim: ping opcode=PING");

        /* Step 6: Client sends pong with same payload (masked) */
        {
            WsFrame *pong = roundtrip(WS_PONG, f->payload,
                                      f->payload_len, 1);
            ASSERT(pong != NULL, "sim: client pong decoded");
            if (pong) {
                ASSERT(pong->opcode == WS_PONG, "sim: pong opcode=PONG");
                ASSERT(pong->payload_len == f->payload_len,
                       "sim: pong payload length matches ping");
                ASSERT(memcmp(pong->payload, f->payload,
                              (size_t)f->payload_len) == 0,
                       "sim: pong payload matches ping payload");
                ws_frame_free(pong);
            }
        }
        ws_frame_free(f);
    }

    /* Step 7: Client sends close with 1000 (masked) */
    close_payload[0] = (uint8_t)(1000 >> 8);
    close_payload[1] = (uint8_t)(1000 & 0xFF);
    f = roundtrip(WS_CLOSE, close_payload, 2, 1);
    ASSERT(f != NULL, "sim: client close decoded");
    if (f) {
        uint16_t status = (uint16_t)((f->payload[0] << 8) | f->payload[1]);
        ASSERT(f->opcode == WS_CLOSE, "sim: client close opcode=CLOSE");
        ASSERT(status == 1000, "sim: client close status=1000");
        ws_frame_free(f);
    }

    /* Step 8: Server sends close with 1000 (unmasked) */
    f = roundtrip(WS_CLOSE, close_payload, 2, 0);
    ASSERT(f != NULL, "sim: server close decoded");
    if (f) {
        uint16_t status = (uint16_t)((f->payload[0] << 8) | f->payload[1]);
        ASSERT(f->opcode == WS_CLOSE, "sim: server close opcode=CLOSE");
        ASSERT(status == 1000, "sim: server close status=1000");
        ws_frame_free(f);
    }

    printf("--- End protocol simulation ---\n\n");
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("=== WebSocket Handshake Integration Tests (Story 21.1.6) ===\n\n");

    test_upgrade_request_headers();
    test_accept_key_computation();
    test_text_frame_roundtrip();
    test_binary_frame_roundtrip();
    test_ping_pong();
    test_close_frame();
    test_masked_vs_unmasked();
    test_payload_sizes();
    test_full_protocol_simulation();

    printf("\n=== %d failure(s) ===\n", failures);
    return failures ? 1 : 0;
}

/*
 * test_ws.c — Unit tests for the std.ws C library (Story 15.1.1).
 *
 * Build and run: make test-stdlib-ws
 *
 * RFC 6455 test vectors:
 *   ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
 *   (from RFC 6455 §1.3)
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
 * Test 1: unmasked text frame encode+decode round-trip
 * ----------------------------------------------------------------------- */
static void test_encode_decode_text_roundtrip(void)
{
    const uint8_t *payload = (const uint8_t *)"hello";
    WsEncodeResult enc = ws_encode_frame(WS_TEXT, payload, 5, 0);
    uint64_t consumed = 0;
    WsFrameResult dec;

    ASSERT(!enc.is_err, "encode text frame: no error");
    ASSERT(enc.data != NULL, "encode text frame: data not NULL");

    dec = ws_decode_frame(enc.data, enc.len, &consumed);
    ASSERT(!dec.is_err, "decode text frame: no error");
    ASSERT(dec.frame != NULL, "decode text frame: frame not NULL");
    ASSERT(dec.frame->opcode == WS_TEXT, "decode text frame: opcode=WS_TEXT");
    ASSERT(dec.frame->payload_len == 5, "decode text frame: payload_len=5");
    ASSERT(dec.frame->payload != NULL &&
           memcmp(dec.frame->payload, "hello", 5) == 0,
           "decode text frame: payload='hello'");
    ASSERT(consumed == enc.len, "decode text frame: consumed==enc.len");

    ws_frame_free(dec.frame);
    ws_encode_result_free(&enc);
}

/* -----------------------------------------------------------------------
 * Test 2: unmasked frame — first byte has FIN+opcode=0x81, MASK bit=0
 * ----------------------------------------------------------------------- */
static void test_unmasked_frame_header_bytes(void)
{
    const uint8_t *payload = (const uint8_t *)"hello";
    WsEncodeResult enc = ws_encode_frame(WS_TEXT, payload, 5, 0);

    ASSERT(!enc.is_err, "unmasked header bytes: no error");
    ASSERT(enc.data != NULL && enc.len >= 2, "unmasked header bytes: sufficient length");
    ASSERT(enc.data[0] == 0x81,
           "unmasked header bytes: byte[0] == 0x81 (FIN + opcode TEXT)");
    /* Byte 1: MASK bit must be 0, payload_len=5 */
    ASSERT((enc.data[1] & 0x80) == 0,
           "unmasked header bytes: MASK bit == 0");
    ASSERT((enc.data[1] & 0x7F) == 5,
           "unmasked header bytes: payload_len field == 5");

    ws_encode_result_free(&enc);
}

/* -----------------------------------------------------------------------
 * Test 3: masked frame — MASK bit set, decode unmasks correctly
 * ----------------------------------------------------------------------- */
static void test_masked_frame_roundtrip(void)
{
    const uint8_t *payload = (const uint8_t *)"hello";
    WsEncodeResult enc = ws_encode_frame(WS_TEXT, payload, 5, 1);
    uint64_t consumed = 0;
    WsFrameResult dec;

    ASSERT(!enc.is_err, "masked frame: encode no error");
    ASSERT(enc.data != NULL && enc.len >= 2, "masked frame: data not NULL");
    ASSERT((enc.data[1] & 0x80) != 0, "masked frame: MASK bit is set in byte[1]");

    dec = ws_decode_frame(enc.data, enc.len, &consumed);
    ASSERT(!dec.is_err, "masked frame: decode no error");
    ASSERT(dec.frame != NULL, "masked frame: frame not NULL");
    ASSERT(dec.frame->opcode == WS_TEXT, "masked frame: opcode=WS_TEXT");
    ASSERT(dec.frame->payload_len == 5, "masked frame: payload_len=5");
    ASSERT(dec.frame->payload != NULL &&
           memcmp(dec.frame->payload, "hello", 5) == 0,
           "masked frame: payload unmasked to 'hello'");

    ws_frame_free(dec.frame);
    ws_encode_result_free(&enc);
}

/* -----------------------------------------------------------------------
 * Test 4: small payload (<=125 bytes) uses 7-bit length field
 * ----------------------------------------------------------------------- */
static void test_small_payload_7bit_length(void)
{
    uint8_t payload[100];
    WsEncodeResult enc;
    memset(payload, 0x41, sizeof(payload));  /* fill with 'A' */
    enc = ws_encode_frame(WS_BINARY, payload, 100, 0);

    ASSERT(!enc.is_err, "7-bit length: no error");
    ASSERT(enc.data != NULL && enc.len >= 2, "7-bit length: data not NULL");
    /* Byte 1 without MASK bit should be 100 directly (7-bit encoding) */
    ASSERT((enc.data[1] & 0x7F) == 100,
           "7-bit length: payload_len field == 100 (direct 7-bit)");
    /* Total wire size: 2 byte header + 100 payload */
    ASSERT(enc.len == 102, "7-bit length: total encoded length == 102");

    ws_encode_result_free(&enc);
}

/* -----------------------------------------------------------------------
 * Test 5: 126-byte payload uses 16-bit extended length in decode
 * ----------------------------------------------------------------------- */
static void test_126_byte_payload_16bit_length(void)
{
    uint8_t payload[126];
    WsEncodeResult enc;
    uint64_t consumed = 0;
    WsFrameResult dec;

    memset(payload, 0x42, sizeof(payload));  /* fill with 'B' */
    enc = ws_encode_frame(WS_BINARY, payload, 126, 0);

    ASSERT(!enc.is_err, "16-bit length encode: no error");
    ASSERT(enc.data != NULL, "16-bit length encode: data not NULL");
    /* Byte 1 lower 7 bits must be 126 (signals 16-bit extended length) */
    ASSERT((enc.data[1] & 0x7F) == 126,
           "16-bit length encode: byte[1]&0x7F == 126");
    /* Total wire size: 2 + 2 (extended len) + 126 payload = 130 */
    ASSERT(enc.len == 130, "16-bit length encode: total encoded length == 130");

    dec = ws_decode_frame(enc.data, enc.len, &consumed);
    ASSERT(!dec.is_err, "16-bit length decode: no error");
    ASSERT(dec.frame != NULL, "16-bit length decode: frame not NULL");
    ASSERT(dec.frame->payload_len == 126,
           "16-bit length decode: payload_len == 126");
    ASSERT(dec.frame->payload != NULL &&
           memcmp(dec.frame->payload, payload, 126) == 0,
           "16-bit length decode: payload contents correct");

    ws_frame_free(dec.frame);
    ws_encode_result_free(&enc);
}

/* -----------------------------------------------------------------------
 * Test 6: close frame survives encode/decode round-trip
 * ----------------------------------------------------------------------- */
static void test_close_frame_roundtrip(void)
{
    WsEncodeResult enc = ws_encode_frame(WS_CLOSE, NULL, 0, 0);
    uint64_t consumed = 0;
    WsFrameResult dec;

    ASSERT(!enc.is_err, "close frame encode: no error");

    dec = ws_decode_frame(enc.data, enc.len, &consumed);
    ASSERT(!dec.is_err, "close frame decode: no error");
    ASSERT(dec.frame != NULL, "close frame decode: frame not NULL");
    ASSERT(dec.frame->opcode == WS_CLOSE,
           "close frame decode: opcode == WS_CLOSE");
    ASSERT(dec.frame->payload_len == 0,
           "close frame decode: payload_len == 0");

    ws_frame_free(dec.frame);
    ws_encode_result_free(&enc);
}

/* -----------------------------------------------------------------------
 * Test 7: ping frame survives encode/decode round-trip
 * ----------------------------------------------------------------------- */
static void test_ping_frame_roundtrip(void)
{
    const uint8_t *payload = (const uint8_t *)"ping!";
    WsEncodeResult enc = ws_encode_frame(WS_PING, payload, 5, 0);
    uint64_t consumed = 0;
    WsFrameResult dec;

    ASSERT(!enc.is_err, "ping frame encode: no error");

    dec = ws_decode_frame(enc.data, enc.len, &consumed);
    ASSERT(!dec.is_err, "ping frame decode: no error");
    ASSERT(dec.frame != NULL, "ping frame decode: frame not NULL");
    ASSERT(dec.frame->opcode == WS_PING,
           "ping frame decode: opcode == WS_PING");
    ASSERT(dec.frame->payload_len == 5,
           "ping frame decode: payload_len == 5");
    ASSERT(dec.frame->payload != NULL &&
           memcmp(dec.frame->payload, "ping!", 5) == 0,
           "ping frame decode: payload == 'ping!'");

    ws_frame_free(dec.frame);
    ws_encode_result_free(&enc);
}

/* -----------------------------------------------------------------------
 * Test 8: ws_accept_key — RFC 6455 §1.3 known vector
 * ----------------------------------------------------------------------- */
static void test_accept_key_rfc_vector(void)
{
    const char *client_key = "dGhlIHNhbXBsZSBub25jZQ==";
    const char *expected   = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
    const char *result     = ws_accept_key(client_key);

    ASSERT(result != NULL, "ws_accept_key: result not NULL");
    ASSERT_STREQ(result, expected,
                 "ws_accept_key: RFC 6455 §1.3 vector matches");

    free((void *)result);
}

/* -----------------------------------------------------------------------
 * Test 9: ws_is_upgrade_request — valid upgrade headers
 * ----------------------------------------------------------------------- */
static void test_is_upgrade_request_valid(void)
{
    const char *headers =
        "GET /chat HTTP/1.1\r\n"
        "Host: server.example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    ASSERT(ws_is_upgrade_request(headers) == 1,
           "ws_is_upgrade_request: valid upgrade headers returns 1");
}

/* -----------------------------------------------------------------------
 * Test 10: ws_is_upgrade_request — plain GET returns 0
 * ----------------------------------------------------------------------- */
static void test_is_upgrade_request_plain_get(void)
{
    const char *headers =
        "GET /index.html HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Accept: text/html\r\n"
        "\r\n";

    ASSERT(ws_is_upgrade_request(headers) == 0,
           "ws_is_upgrade_request: plain GET returns 0");
}

/* -----------------------------------------------------------------------
 * Test 11: ws_decode_frame on truncated buffer returns is_err=1
 * ----------------------------------------------------------------------- */
static void test_decode_truncated_buffer(void)
{
    /* Only 1 byte — cannot form a valid frame (need at least 2) */
    const uint8_t buf[1] = { 0x81 };
    uint64_t consumed = 0;
    WsFrameResult res = ws_decode_frame(buf, 1, &consumed);

    ASSERT(res.is_err == 1,
           "ws_decode_frame: truncated 1-byte buffer returns is_err=1");
    ASSERT(res.frame == NULL,
           "ws_decode_frame: truncated buffer returns NULL frame");
}

/* -----------------------------------------------------------------------
 * Test 12: ws_decode_frame — truncated payload returns is_err=1
 * ----------------------------------------------------------------------- */
static void test_decode_truncated_payload(void)
{
    /*
     * Manually craft a frame header claiming 10-byte payload, but supply
     * only the header (2 bytes total, 0 payload bytes).
     * byte[0]: 0x81 (FIN + TEXT)
     * byte[1]: 0x0A (MASK=0, payload_len=10)
     */
    const uint8_t buf[2] = { 0x81, 0x0A };
    uint64_t consumed = 0;
    WsFrameResult res = ws_decode_frame(buf, 2, &consumed);

    ASSERT(res.is_err == 1,
           "ws_decode_frame: truncated payload returns is_err=1");
    ASSERT(res.frame == NULL,
           "ws_decode_frame: truncated payload returns NULL frame");
}

/* -----------------------------------------------------------------------
 * Test 13: ws_is_upgrade_request — case-insensitive upgrade header value
 * ----------------------------------------------------------------------- */
static void test_is_upgrade_request_case_insensitive(void)
{
    const char *headers =
        "GET /ws HTTP/1.1\r\n"
        "Upgrade: WebSocket\r\n"
        "Connection: Upgrade\r\n"
        "\r\n";

    ASSERT(ws_is_upgrade_request(headers) == 1,
           "ws_is_upgrade_request: 'WebSocket' (mixed-case value) returns 1");
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("=== test_ws ===\n");

    test_encode_decode_text_roundtrip();
    test_unmasked_frame_header_bytes();
    test_masked_frame_roundtrip();
    test_small_payload_7bit_length();
    test_126_byte_payload_16bit_length();
    test_close_frame_roundtrip();
    test_ping_frame_roundtrip();
    test_accept_key_rfc_vector();
    test_is_upgrade_request_valid();
    test_is_upgrade_request_plain_get();
    test_decode_truncated_buffer();
    test_decode_truncated_payload();
    test_is_upgrade_request_case_insensitive();

    printf("=== %s ===\n", failures == 0 ? "ALL PASS" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

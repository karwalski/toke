/*
 * test_http2.c — Unit tests for HTTP/2 binary framing layer.
 *
 * Story: 60.1.1  Branch: feature/http2-framing
 */

#include "http2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static int tests_run  = 0;
static int tests_pass = 0;

#define TEST(name) do { \
    tests_run++; \
    printf("  %-50s", name); \
} while(0)

#define PASS() do { tests_pass++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ── Frame header serialisation ───────────────────────────────────────── */

static void test_frame_header_roundtrip(void)
{
    TEST("frame header roundtrip");

    H2FrameHeader hdr = {
        .length    = 16384,
        .type      = H2_FRAME_DATA,
        .flags     = H2_FLAG_END_STREAM,
        .stream_id = 1
    };

    uint8_t buf[9];
    h2_frame_header_write(buf, &hdr);

    H2FrameHeader out;
    h2_frame_header_read(buf, &out);

    assert(out.length    == 16384);
    assert(out.type      == H2_FRAME_DATA);
    assert(out.flags     == H2_FLAG_END_STREAM);
    assert(out.stream_id == 1);
    PASS();
}

static void test_frame_header_max_values(void)
{
    TEST("frame header max values");

    H2FrameHeader hdr = {
        .length    = 0xFFFFFF,          /* max 24-bit */
        .type      = H2_FRAME_CONTINUATION,
        .flags     = 0xFF,
        .stream_id = 0x7FFFFFFF        /* max 31-bit */
    };

    uint8_t buf[9];
    h2_frame_header_write(buf, &hdr);

    H2FrameHeader out;
    h2_frame_header_read(buf, &out);

    assert(out.length    == 0xFFFFFF);
    assert(out.type      == H2_FRAME_CONTINUATION);
    assert(out.flags     == 0xFF);
    assert(out.stream_id == 0x7FFFFFFF);
    PASS();
}

static void test_frame_header_reserved_bit(void)
{
    TEST("frame header clears reserved bit");

    H2FrameHeader hdr = {
        .length = 0, .type = 0, .flags = 0,
        .stream_id = 0xFFFFFFFF  /* top bit set */
    };

    uint8_t buf[9];
    h2_frame_header_write(buf, &hdr);

    H2FrameHeader out;
    h2_frame_header_read(buf, &out);

    assert(out.stream_id == 0x7FFFFFFF); /* reserved bit cleared */
    PASS();
}

/* ── Settings ─────────────────────────────────────────────────────────── */

static void test_settings_default(void)
{
    TEST("settings default values");

    H2Settings s;
    h2_settings_default(&s);

    assert(s.header_table_size      == 4096);
    assert(s.enable_push            == 0);
    assert(s.max_concurrent_streams == 128);
    assert(s.initial_window_size    == 65535);
    assert(s.max_frame_size         == 16384);
    assert(s.max_header_list_size   == 65536);
    PASS();
}

static void test_settings_apply(void)
{
    TEST("settings apply");

    H2Conn *conn = h2_conn_new();
    assert(conn != NULL);

    /* Encode a SETTINGS payload: max_concurrent=256, window=32768 */
    uint8_t payload[12];
    payload[0]  = 0x00; payload[1]  = H2_SETTINGS_MAX_CONCURRENT_STREAMS;
    payload[2]  = 0x00; payload[3]  = 0x00; payload[4]  = 0x01; payload[5]  = 0x00;
    payload[6]  = 0x00; payload[7]  = H2_SETTINGS_INITIAL_WINDOW_SIZE;
    payload[8]  = 0x00; payload[9]  = 0x00; payload[10] = 0x80; payload[11] = 0x00;

    int r = h2_apply_settings(conn, payload, 12);
    assert(r == 0);
    assert(conn->peer_settings.max_concurrent_streams == 256);
    assert(conn->peer_settings.initial_window_size    == 32768);

    h2_conn_free(conn);
    PASS();
}

static void test_settings_invalid_length(void)
{
    TEST("settings reject non-multiple-of-6 payload");

    H2Conn *conn = h2_conn_new();
    uint8_t payload[5] = {0};
    assert(h2_apply_settings(conn, payload, 5) == -1);
    h2_conn_free(conn);
    PASS();
}

/* ── Connection preface ───────────────────────────────────────────────── */

static void test_preface_valid(void)
{
    TEST("connection preface valid");

    const uint8_t *preface = (const uint8_t *)H2_CONNECTION_PREFACE;
    assert(h2_validate_preface(preface, H2_CONNECTION_PREFACE_LEN) == 0);
    PASS();
}

static void test_preface_invalid(void)
{
    TEST("connection preface invalid");

    uint8_t bad[24] = {0};
    assert(h2_validate_preface(bad, 24) == -1);
    PASS();
}

static void test_preface_short(void)
{
    TEST("connection preface too short");
    assert(h2_validate_preface((const uint8_t *)"PRI", 3) == -1);
    PASS();
}

/* ── Stream management ────────────────────────────────────────────────── */

static void test_stream_create(void)
{
    TEST("stream create and lookup");

    H2Conn *conn = h2_conn_new();
    H2Stream *s1 = h2_stream_get(conn, 1);
    assert(s1 != NULL);
    assert(s1->id == 1);
    assert(s1->state == H2_STREAM_IDLE);

    /* Same ID returns same stream */
    H2Stream *s1b = h2_stream_get(conn, 1);
    assert(s1b == s1);

    /* Different ID creates new stream */
    H2Stream *s3 = h2_stream_get(conn, 3);
    assert(s3 != s1);
    assert(s3->id == 3);
    assert(conn->stream_count == 2);

    h2_conn_free(conn);
    PASS();
}

static void test_stream_transitions(void)
{
    TEST("stream state transitions");

    H2Conn *conn = h2_conn_new();
    H2Stream *s = h2_stream_get(conn, 1);

    /* idle -> open (recv HEADERS) */
    assert(h2_stream_transition(s, H2_FRAME_HEADERS, 0, 0) == 0);
    assert(s->state == H2_STREAM_OPEN);

    /* open -> half_closed_remote (recv END_STREAM) */
    assert(h2_stream_transition(s, H2_FRAME_DATA, H2_FLAG_END_STREAM, 0) == 0);
    assert(s->state == H2_STREAM_HALF_CLOSED_REMOTE);

    /* half_closed_remote -> closed (send END_STREAM) */
    assert(h2_stream_transition(s, H2_FRAME_DATA, H2_FLAG_END_STREAM, 1) == 0);
    assert(s->state == H2_STREAM_CLOSED);

    h2_conn_free(conn);
    PASS();
}

static void test_stream_idle_to_half_closed(void)
{
    TEST("stream idle -> half_closed via HEADERS+END_STREAM");

    H2Conn *conn = h2_conn_new();
    H2Stream *s = h2_stream_get(conn, 1);

    /* idle -> half_closed_remote (recv HEADERS + END_STREAM) */
    assert(h2_stream_transition(s, H2_FRAME_HEADERS, H2_FLAG_END_STREAM, 0) == 0);
    assert(s->state == H2_STREAM_HALF_CLOSED_REMOTE);

    h2_conn_free(conn);
    PASS();
}

static void test_stream_rst(void)
{
    TEST("stream RST_STREAM transitions to closed");

    H2Conn *conn = h2_conn_new();
    H2Stream *s = h2_stream_get(conn, 1);
    h2_stream_transition(s, H2_FRAME_HEADERS, 0, 0); /* open */

    assert(h2_stream_transition(s, H2_FRAME_RST_STREAM, 0, 0) == 0);
    assert(s->state == H2_STREAM_CLOSED);

    h2_conn_free(conn);
    PASS();
}

/* ── HPACK ────────────────────────────────────────────────────────────── */

static void test_hpack_table_init(void)
{
    TEST("hpack table init and free");

    HpackTable t;
    hpack_table_init(&t, 4096);
    assert(t.max_size == 4096);
    assert(t.size == 0);
    assert(t.count == 0);
    hpack_table_free(&t);
    PASS();
}

static void test_hpack_encode_decode_indexed(void)
{
    TEST("hpack encode/decode indexed header");

    HpackTable enc, dec;
    hpack_table_init(&enc, 4096);
    hpack_table_init(&dec, 4096);

    /* Encode :method GET (static index 2) */
    const char *names[]  = { ":method" };
    const char *values[] = { "GET" };
    uint8_t buf[256];
    int len = hpack_encode(&enc, names, values, 1, buf, sizeof(buf));
    assert(len > 0);
    assert(len == 1); /* single byte for indexed */
    assert(buf[0] == (0x80 | 2)); /* index 2 */

    /* Decode */
    char **dnames, **dvalues;
    int dcount = hpack_decode(&dec, buf, (size_t)len, &dnames, &dvalues);
    assert(dcount == 1);
    assert(strcmp(dnames[0],  ":method") == 0);
    assert(strcmp(dvalues[0], "GET")     == 0);

    free(dnames[0]); free(dvalues[0]);
    free(dnames); free(dvalues);
    hpack_table_free(&enc);
    hpack_table_free(&dec);
    PASS();
}

static void test_hpack_encode_decode_literal(void)
{
    TEST("hpack encode/decode literal header");

    HpackTable enc, dec;
    hpack_table_init(&enc, 4096);
    hpack_table_init(&dec, 4096);

    const char *names[]  = { "x-custom" };
    const char *values[] = { "hello" };
    uint8_t buf[256];
    int len = hpack_encode(&enc, names, values, 1, buf, sizeof(buf));
    assert(len > 0);

    char **dnames, **dvalues;
    int dcount = hpack_decode(&dec, buf, (size_t)len, &dnames, &dvalues);
    assert(dcount == 1);
    assert(strcmp(dnames[0],  "x-custom") == 0);
    assert(strcmp(dvalues[0], "hello")    == 0);

    free(dnames[0]); free(dvalues[0]);
    free(dnames); free(dvalues);
    hpack_table_free(&enc);
    hpack_table_free(&dec);
    PASS();
}

static void test_hpack_dynamic_table(void)
{
    TEST("hpack dynamic table indexing");

    HpackTable enc, dec;
    hpack_table_init(&enc, 4096);
    hpack_table_init(&dec, 4096);

    /* Encode custom header twice — second should be indexed */
    const char *names[]  = { "x-token" };
    const char *values[] = { "abc123" };
    uint8_t buf1[256], buf2[256];

    int len1 = hpack_encode(&enc, names, values, 1, buf1, sizeof(buf1));
    int len2 = hpack_encode(&enc, names, values, 1, buf2, sizeof(buf2));

    assert(len1 > 0);
    assert(len2 > 0);
    assert(len2 < len1); /* second encoding should be shorter (indexed) */

    /* Decode first to populate table */
    char **dn, **dv;
    int dc = hpack_decode(&dec, buf1, (size_t)len1, &dn, &dv);
    assert(dc == 1);
    free(dn[0]); free(dv[0]); free(dn); free(dv);

    /* Decode second — should resolve from dynamic table */
    dc = hpack_decode(&dec, buf2, (size_t)len2, &dn, &dv);
    assert(dc == 1);
    assert(strcmp(dn[0], "x-token") == 0);
    assert(strcmp(dv[0], "abc123")  == 0);
    free(dn[0]); free(dv[0]); free(dn); free(dv);

    hpack_table_free(&enc);
    hpack_table_free(&dec);
    PASS();
}

static void test_hpack_table_eviction(void)
{
    TEST("hpack dynamic table eviction");

    HpackTable t;
    hpack_table_init(&t, 128); /* small table */

    /* Insert entries until eviction happens */
    const char *n1 = "x-header-one";
    const char *v1 = "value-that-takes-space-aaaa";
    const char *n2 = "x-header-two";
    const char *v2 = "another-value-bbbbbbbbb";

    /* Each entry size = name_len + value_len + 32 */
    /* Entry 1: 12 + 26 + 32 = 70 */
    /* Entry 2: 12 + 23 + 32 = 67 */
    /* Total: 137 > 128, so entry 1 should be evicted */

    HpackTable enc;
    hpack_table_init(&enc, 128);

    const char *names1[]  = { n1 };
    const char *values1[] = { v1 };
    const char *names2[]  = { n2 };
    const char *values2[] = { v2 };

    uint8_t buf[256];
    hpack_encode(&enc, names1, values1, 1, buf, sizeof(buf));
    assert(enc.count == 1);

    hpack_encode(&enc, names2, values2, 1, buf, sizeof(buf));
    /* After inserting second, first should be evicted */
    assert(enc.count == 1);
    assert(strcmp(enc.entries[0].name, n2) == 0);

    hpack_table_free(&enc);
    hpack_table_free(&t);
    PASS();
}

static void test_hpack_multiple_headers(void)
{
    TEST("hpack encode/decode multiple headers");

    HpackTable enc, dec;
    hpack_table_init(&enc, 4096);
    hpack_table_init(&dec, 4096);

    const char *names[]  = { ":method", ":path", ":scheme", "content-type" };
    const char *values[] = { "GET",     "/api",  "https",   "application/json" };
    uint8_t buf[512];
    int len = hpack_encode(&enc, names, values, 4, buf, sizeof(buf));
    assert(len > 0);

    char **dn, **dv;
    int dc = hpack_decode(&dec, buf, (size_t)len, &dn, &dv);
    assert(dc == 4);
    assert(strcmp(dn[0], ":method") == 0);
    assert(strcmp(dv[0], "GET") == 0);
    assert(strcmp(dn[1], ":path") == 0);
    assert(strcmp(dv[1], "/api") == 0);
    assert(strcmp(dn[2], ":scheme") == 0);
    assert(strcmp(dv[2], "https") == 0);
    assert(strcmp(dn[3], "content-type") == 0);
    assert(strcmp(dv[3], "application/json") == 0);

    for (int i = 0; i < dc; i++) { free(dn[i]); free(dv[i]); }
    free(dn); free(dv);
    hpack_table_free(&enc);
    hpack_table_free(&dec);
    PASS();
}

/* ── Connection lifecycle ─────────────────────────────────────────────── */

static void test_conn_lifecycle(void)
{
    TEST("connection create and free");

    H2Conn *conn = h2_conn_new();
    assert(conn != NULL);
    assert(conn->conn_window_recv == H2_DEFAULT_INITIAL_WINDOW_SIZE);
    assert(conn->conn_window_send == H2_DEFAULT_INITIAL_WINDOW_SIZE);
    assert(conn->next_stream_id   == 2);
    assert(conn->stream_count     == 0);
    assert(conn->goaway_sent      == 0);

    h2_conn_free(conn);
    PASS();
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("\n=== HTTP/2 Framing Layer Tests ===\n\n");

    /* Frame header */
    test_frame_header_roundtrip();
    test_frame_header_max_values();
    test_frame_header_reserved_bit();

    /* Settings */
    test_settings_default();
    test_settings_apply();
    test_settings_invalid_length();

    /* Connection preface */
    test_preface_valid();
    test_preface_invalid();
    test_preface_short();

    /* Stream management */
    test_stream_create();
    test_stream_transitions();
    test_stream_idle_to_half_closed();
    test_stream_rst();

    /* HPACK */
    test_hpack_table_init();
    test_hpack_encode_decode_indexed();
    test_hpack_encode_decode_literal();
    test_hpack_dynamic_table();
    test_hpack_table_eviction();
    test_hpack_multiple_headers();

    /* Connection */
    test_conn_lifecycle();

    printf("\n%d/%d tests passed.\n\n", tests_pass, tests_run);
    return tests_pass == tests_run ? 0 : 1;
}

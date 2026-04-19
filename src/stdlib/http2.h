/*
 * http2.h — HTTP/2 binary framing layer (RFC 7540 / RFC 9113).
 *
 * Implements frame parsing, serialisation, HPACK header compression,
 * stream multiplexing, and flow control.
 *
 * Story: 60.1.1  Branch: feature/http2-framing
 */

#ifndef TK_STDLIB_HTTP2_H
#define TK_STDLIB_HTTP2_H

#include <stddef.h>
#include <stdint.h>

/* ── HTTP/2 Frame Types (RFC 9113 §4) ─────────────────────────────────── */

#define H2_FRAME_DATA          0x00
#define H2_FRAME_HEADERS       0x01
#define H2_FRAME_PRIORITY      0x02
#define H2_FRAME_RST_STREAM    0x03
#define H2_FRAME_SETTINGS      0x04
#define H2_FRAME_PUSH_PROMISE  0x05
#define H2_FRAME_PING          0x06
#define H2_FRAME_GOAWAY        0x07
#define H2_FRAME_WINDOW_UPDATE 0x08
#define H2_FRAME_CONTINUATION  0x09

/* ── Frame Flags ──────────────────────────────────────────────────────── */

#define H2_FLAG_END_STREAM  0x01
#define H2_FLAG_END_HEADERS 0x04
#define H2_FLAG_PADDED      0x08
#define H2_FLAG_PRIORITY    0x20
#define H2_FLAG_ACK         0x01  /* SETTINGS and PING only */

/* ── Error Codes (RFC 9113 §7) ────────────────────────────────────────── */

#define H2_ERR_NONE                0x00
#define H2_ERR_PROTOCOL            0x01
#define H2_ERR_INTERNAL            0x02
#define H2_ERR_FLOW_CONTROL        0x03
#define H2_ERR_SETTINGS_TIMEOUT    0x04
#define H2_ERR_STREAM_CLOSED       0x05
#define H2_ERR_FRAME_SIZE          0x06
#define H2_ERR_REFUSED_STREAM      0x07
#define H2_ERR_CANCEL              0x08
#define H2_ERR_COMPRESSION         0x09
#define H2_ERR_CONNECT             0x0a
#define H2_ERR_ENHANCE_YOUR_CALM   0x0b
#define H2_ERR_INADEQUATE_SECURITY 0x0c
#define H2_ERR_HTTP_1_1_REQUIRED   0x0d

/* ── Settings Parameters (RFC 9113 §6.5.2) ───────────────────────────── */

#define H2_SETTINGS_HEADER_TABLE_SIZE      0x01
#define H2_SETTINGS_ENABLE_PUSH            0x02
#define H2_SETTINGS_MAX_CONCURRENT_STREAMS 0x03
#define H2_SETTINGS_INITIAL_WINDOW_SIZE    0x04
#define H2_SETTINGS_MAX_FRAME_SIZE         0x05
#define H2_SETTINGS_MAX_HEADER_LIST_SIZE   0x06

/* ── Defaults ─────────────────────────────────────────────────────────── */

#define H2_DEFAULT_HEADER_TABLE_SIZE    4096
#define H2_DEFAULT_INITIAL_WINDOW_SIZE  65535
#define H2_DEFAULT_MAX_FRAME_SIZE       16384
#define H2_MAX_FRAME_SIZE_LIMIT         16777215  /* 2^24 - 1 */
#define H2_DEFAULT_MAX_CONCURRENT       128
#define H2_MAX_WINDOW_SIZE              2147483647 /* 2^31 - 1 */

#define H2_CONNECTION_PREFACE "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
#define H2_CONNECTION_PREFACE_LEN 24

#define H2_FRAME_HEADER_SIZE 9

/* ── Frame structure ──────────────────────────────────────────────────── */

typedef struct {
    uint32_t length;     /* 24-bit payload length */
    uint8_t  type;       /* frame type */
    uint8_t  flags;      /* frame flags */
    uint32_t stream_id;  /* 31-bit stream identifier */
} H2FrameHeader;

/* ── Settings ─────────────────────────────────────────────────────────── */

typedef struct {
    uint16_t id;
    uint32_t value;
} H2Setting;

typedef struct {
    uint32_t header_table_size;
    uint32_t enable_push;
    uint32_t max_concurrent_streams;
    uint32_t initial_window_size;
    uint32_t max_frame_size;
    uint32_t max_header_list_size;
} H2Settings;

/* ── Stream states (RFC 9113 §5.1) ────────────────────────────────────── */

typedef enum {
    H2_STREAM_IDLE,
    H2_STREAM_RESERVED_LOCAL,
    H2_STREAM_RESERVED_REMOTE,
    H2_STREAM_OPEN,
    H2_STREAM_HALF_CLOSED_LOCAL,
    H2_STREAM_HALF_CLOSED_REMOTE,
    H2_STREAM_CLOSED
} H2StreamState;

typedef struct {
    uint32_t      id;
    H2StreamState state;
    int32_t       window;          /* flow-control window */
    uint8_t      *header_block;    /* accumulated HEADERS/CONTINUATION */
    size_t        header_block_len;
    size_t        header_block_cap;
} H2Stream;

/* ── HPACK dynamic table entry ────────────────────────────────────────── */

typedef struct {
    char    *name;
    char    *value;
    size_t   entry_size;  /* name_len + value_len + 32 (RFC 7541 §4.1) */
} HpackEntry;

typedef struct {
    HpackEntry *entries;
    size_t      count;
    size_t      capacity;
    size_t      size;        /* current total size */
    size_t      max_size;    /* SETTINGS_HEADER_TABLE_SIZE */
} HpackTable;

/* ── Connection state ─────────────────────────────────────────────────── */

#define H2_MAX_STREAMS 256

typedef struct {
    H2Settings  local_settings;    /* our settings */
    H2Settings  peer_settings;     /* peer's settings */
    int32_t     conn_window_recv;  /* our receive window */
    int32_t     conn_window_send;  /* peer's receive window */
    H2Stream    streams[H2_MAX_STREAMS];
    size_t      stream_count;
    uint32_t    last_stream_id;    /* highest stream ID seen */
    uint32_t    next_stream_id;    /* next stream ID to issue (server: even) */
    int         goaway_sent;
    uint32_t    goaway_last_stream;
    HpackTable  hpack_decode;      /* decoder dynamic table */
    HpackTable  hpack_encode;      /* encoder dynamic table */
    uint8_t    *frame_buf;         /* reusable frame buffer */
    size_t      frame_buf_cap;
} H2Conn;

/* ── Frame serialisation / parsing ────────────────────────────────────── */

/* Serialise a 9-byte frame header into buf (must be ≥ 9 bytes). */
void h2_frame_header_write(uint8_t *buf, const H2FrameHeader *hdr);

/* Parse a 9-byte frame header from buf. Returns 0 on success, -1 on error. */
int h2_frame_header_read(const uint8_t *buf, H2FrameHeader *out);

/* Build and write a complete frame (header + payload) to fd/SSL.
 * Returns bytes written or -1 on error. */
int h2_frame_send(H2Conn *conn, int fd, void *ssl,
                  uint8_t type, uint8_t flags, uint32_t stream_id,
                  const uint8_t *payload, uint32_t length);

/* Read one complete frame from fd/SSL into conn->frame_buf.
 * Populates hdr. Returns payload pointer or NULL on error. */
uint8_t *h2_frame_recv(H2Conn *conn, int fd, void *ssl, H2FrameHeader *hdr);

/* ── Connection lifecycle ─────────────────────────────────────────────── */

/* Initialise a new HTTP/2 connection (server side). */
H2Conn *h2_conn_new(void);

/* Free connection state. */
void h2_conn_free(H2Conn *conn);

/* Initialise default settings. */
void h2_settings_default(H2Settings *s);

/* Send our SETTINGS frame. Returns 0 on success. */
int h2_send_settings(H2Conn *conn, int fd, void *ssl);

/* Send SETTINGS ACK. */
int h2_send_settings_ack(H2Conn *conn, int fd, void *ssl);

/* Apply received SETTINGS to peer_settings. */
int h2_apply_settings(H2Conn *conn, const uint8_t *payload, uint32_t len);

/* Validate client connection preface. Returns 0 on success. */
int h2_validate_preface(const uint8_t *buf, size_t len);

/* ── GOAWAY ───────────────────────────────────────────────────────────── */

int h2_send_goaway(H2Conn *conn, int fd, void *ssl,
                   uint32_t last_stream_id, uint32_t error_code);

/* ── RST_STREAM ───────────────────────────────────────────────────────── */

int h2_send_rst_stream(H2Conn *conn, int fd, void *ssl,
                       uint32_t stream_id, uint32_t error_code);

/* ── PING ─────────────────────────────────────────────────────────────── */

int h2_send_ping(H2Conn *conn, int fd, void *ssl,
                 const uint8_t *opaque_data, int is_ack);

/* ── WINDOW_UPDATE ────────────────────────────────────────────────────── */

int h2_send_window_update(H2Conn *conn, int fd, void *ssl,
                          uint32_t stream_id, uint32_t increment);

/* ── Stream management ────────────────────────────────────────────────── */

/* Find or create a stream by ID. Returns NULL if too many streams. */
H2Stream *h2_stream_get(H2Conn *conn, uint32_t stream_id);

/* Transition stream state. Returns 0 on valid transition, -1 on error. */
int h2_stream_transition(H2Stream *stream, uint8_t frame_type, uint8_t flags,
                         int is_sender);

/* ── HPACK (RFC 7541) ─────────────────────────────────────────────────── */

/* Initialise HPACK table. */
void hpack_table_init(HpackTable *t, size_t max_size);

/* Free HPACK table. */
void hpack_table_free(HpackTable *t);

/* Decode a header block into name-value pairs.
 * Returns number of headers decoded, or -1 on error.
 * Caller owns the returned arrays (must free each string). */
int hpack_decode(HpackTable *t, const uint8_t *buf, size_t len,
                 char ***names, char ***values);

/* Encode a set of headers into a header block.
 * Returns encoded length, or -1 on error.
 * out must point to a buffer of sufficient size. */
int hpack_encode(HpackTable *t, const char **names, const char **values,
                 size_t count, uint8_t *out, size_t out_cap);

/* Resize HPACK dynamic table (on SETTINGS_HEADER_TABLE_SIZE change). */
void hpack_table_resize(HpackTable *t, size_t new_max);

#endif /* TK_STDLIB_HTTP2_H */

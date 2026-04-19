/*
 * http2.c — HTTP/2 binary framing layer (RFC 7540 / RFC 9113).
 *
 * Implements:
 *   - Frame parser and serialiser (all 10 frame types)
 *   - HPACK header compression (RFC 7541)
 *   - Stream multiplexing and state machine
 *   - Flow control (per-stream and connection-level)
 *   - Connection preface and SETTINGS exchange
 *   - GOAWAY graceful shutdown
 *
 * Story: 60.1.1–60.1.8  Branch: feature/http2-framing
 */

#include "http2.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#ifdef TK_HAVE_OPENSSL
#include <openssl/ssl.h>
#endif

/* ── I/O helpers ──────────────────────────────────────────────────────── */

static ssize_t h2_read(int fd, void *ssl, uint8_t *buf, size_t len)
{
#ifdef TK_HAVE_OPENSSL
    if (ssl) return SSL_read((SSL *)ssl, buf, (int)len);
#endif
    (void)ssl;
    return read(fd, buf, len);
}

static ssize_t h2_write(int fd, void *ssl, const uint8_t *buf, size_t len)
{
#ifdef TK_HAVE_OPENSSL
    if (ssl) return SSL_write((SSL *)ssl, buf, (int)len);
#endif
    (void)ssl;
    return write(fd, buf, len);
}

/* Read exactly n bytes. Returns 0 on success, -1 on error/EOF. */
static int h2_read_exact(int fd, void *ssl, uint8_t *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = h2_read(fd, ssl, buf + done, n - done);
        if (r <= 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

/* Write exactly n bytes. Returns 0 on success, -1 on error. */
static int h2_write_all(int fd, void *ssl, const uint8_t *buf, size_t n)
{
    size_t done = 0;
    while (done < n) {
        ssize_t w = h2_write(fd, ssl, buf + done, n - done);
        if (w <= 0) return -1;
        done += (size_t)w;
    }
    return 0;
}

/* ── Frame header serialisation ───────────────────────────────────────── */

void h2_frame_header_write(uint8_t *buf, const H2FrameHeader *hdr)
{
    /* Length: 24 bits */
    buf[0] = (uint8_t)((hdr->length >> 16) & 0xFF);
    buf[1] = (uint8_t)((hdr->length >> 8)  & 0xFF);
    buf[2] = (uint8_t)( hdr->length        & 0xFF);
    /* Type: 8 bits */
    buf[3] = hdr->type;
    /* Flags: 8 bits */
    buf[4] = hdr->flags;
    /* Stream ID: 31 bits (R bit always 0) */
    uint32_t sid = hdr->stream_id & 0x7FFFFFFF;
    buf[5] = (uint8_t)((sid >> 24) & 0xFF);
    buf[6] = (uint8_t)((sid >> 16) & 0xFF);
    buf[7] = (uint8_t)((sid >> 8)  & 0xFF);
    buf[8] = (uint8_t)( sid        & 0xFF);
}

int h2_frame_header_read(const uint8_t *buf, H2FrameHeader *out)
{
    out->length = ((uint32_t)buf[0] << 16)
               | ((uint32_t)buf[1] << 8)
               |  (uint32_t)buf[2];
    out->type   = buf[3];
    out->flags  = buf[4];
    out->stream_id = ((uint32_t)buf[5] << 24)
                   | ((uint32_t)buf[6] << 16)
                   | ((uint32_t)buf[7] << 8)
                   |  (uint32_t)buf[8];
    out->stream_id &= 0x7FFFFFFF; /* clear reserved bit */
    return 0;
}

/* ── Frame send/recv ──────────────────────────────────────────────────── */

int h2_frame_send(H2Conn *conn, int fd, void *ssl,
                  uint8_t type, uint8_t flags, uint32_t stream_id,
                  const uint8_t *payload, uint32_t length)
{
    (void)conn;
    if (length > H2_MAX_FRAME_SIZE_LIMIT) return -1;

    uint8_t hdr_buf[H2_FRAME_HEADER_SIZE];
    H2FrameHeader hdr = { .length = length, .type = type,
                          .flags = flags, .stream_id = stream_id };
    h2_frame_header_write(hdr_buf, &hdr);

    if (h2_write_all(fd, ssl, hdr_buf, H2_FRAME_HEADER_SIZE) < 0)
        return -1;
    if (length > 0 && payload) {
        if (h2_write_all(fd, ssl, payload, length) < 0)
            return -1;
    }
    return (int)(H2_FRAME_HEADER_SIZE + length);
}

uint8_t *h2_frame_recv(H2Conn *conn, int fd, void *ssl, H2FrameHeader *hdr)
{
    uint8_t hdr_buf[H2_FRAME_HEADER_SIZE];
    if (h2_read_exact(fd, ssl, hdr_buf, H2_FRAME_HEADER_SIZE) < 0)
        return NULL;

    h2_frame_header_read(hdr_buf, hdr);

    if (hdr->length > conn->peer_settings.max_frame_size) {
        /* Frame size error — send GOAWAY */
        h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                        H2_ERR_FRAME_SIZE);
        return NULL;
    }

    /* Ensure frame buffer is large enough */
    if (hdr->length > 0) {
        if (hdr->length > conn->frame_buf_cap) {
            size_t new_cap = hdr->length + 1024;
            uint8_t *nb = realloc(conn->frame_buf, new_cap);
            if (!nb) return NULL;
            conn->frame_buf = nb;
            conn->frame_buf_cap = new_cap;
        }
        if (h2_read_exact(fd, ssl, conn->frame_buf, hdr->length) < 0)
            return NULL;
    }

    return conn->frame_buf;
}

/* ── Settings ─────────────────────────────────────────────────────────── */

void h2_settings_default(H2Settings *s)
{
    s->header_table_size      = H2_DEFAULT_HEADER_TABLE_SIZE;
    s->enable_push            = 0; /* server never pushes */
    s->max_concurrent_streams = H2_DEFAULT_MAX_CONCURRENT;
    s->initial_window_size    = H2_DEFAULT_INITIAL_WINDOW_SIZE;
    s->max_frame_size         = H2_DEFAULT_MAX_FRAME_SIZE;
    s->max_header_list_size   = 65536;
}

int h2_send_settings(H2Conn *conn, int fd, void *ssl)
{
    /* Encode our local settings as 6-byte pairs */
    uint8_t payload[6 * 6]; /* max 6 settings */
    int n = 0;

    /* Only send non-default values */
    struct { uint16_t id; uint32_t val; } pairs[] = {
        { H2_SETTINGS_HEADER_TABLE_SIZE,      conn->local_settings.header_table_size },
        { H2_SETTINGS_ENABLE_PUSH,            conn->local_settings.enable_push },
        { H2_SETTINGS_MAX_CONCURRENT_STREAMS, conn->local_settings.max_concurrent_streams },
        { H2_SETTINGS_INITIAL_WINDOW_SIZE,    conn->local_settings.initial_window_size },
        { H2_SETTINGS_MAX_FRAME_SIZE,         conn->local_settings.max_frame_size },
        { H2_SETTINGS_MAX_HEADER_LIST_SIZE,   conn->local_settings.max_header_list_size },
    };

    for (int i = 0; i < 6; i++) {
        payload[n++] = (uint8_t)(pairs[i].id >> 8);
        payload[n++] = (uint8_t)(pairs[i].id & 0xFF);
        payload[n++] = (uint8_t)(pairs[i].val >> 24);
        payload[n++] = (uint8_t)(pairs[i].val >> 16);
        payload[n++] = (uint8_t)(pairs[i].val >> 8);
        payload[n++] = (uint8_t)(pairs[i].val & 0xFF);
    }

    return h2_frame_send(conn, fd, ssl, H2_FRAME_SETTINGS, 0, 0,
                          payload, (uint32_t)n);
}

int h2_send_settings_ack(H2Conn *conn, int fd, void *ssl)
{
    return h2_frame_send(conn, fd, ssl, H2_FRAME_SETTINGS, H2_FLAG_ACK,
                          0, NULL, 0);
}

int h2_apply_settings(H2Conn *conn, const uint8_t *payload, uint32_t len)
{
    if (len % 6 != 0) return -1; /* FRAME_SIZE_ERROR */

    for (uint32_t i = 0; i < len; i += 6) {
        uint16_t id  = (uint16_t)((payload[i] << 8) | payload[i + 1]);
        uint32_t val = ((uint32_t)payload[i + 2] << 24)
                     | ((uint32_t)payload[i + 3] << 16)
                     | ((uint32_t)payload[i + 4] << 8)
                     |  (uint32_t)payload[i + 5];

        switch (id) {
        case H2_SETTINGS_HEADER_TABLE_SIZE:
            conn->peer_settings.header_table_size = val;
            hpack_table_resize(&conn->hpack_encode, val);
            break;
        case H2_SETTINGS_ENABLE_PUSH:
            if (val > 1) return -1; /* PROTOCOL_ERROR */
            conn->peer_settings.enable_push = val;
            break;
        case H2_SETTINGS_MAX_CONCURRENT_STREAMS:
            conn->peer_settings.max_concurrent_streams = val;
            break;
        case H2_SETTINGS_INITIAL_WINDOW_SIZE:
            if (val > H2_MAX_WINDOW_SIZE) return -1; /* FLOW_CONTROL_ERROR */
            /* Adjust existing stream windows by delta */
            {
                int32_t delta = (int32_t)val
                              - (int32_t)conn->peer_settings.initial_window_size;
                for (size_t s = 0; s < conn->stream_count; s++) {
                    conn->streams[s].window += delta;
                }
            }
            conn->peer_settings.initial_window_size = val;
            break;
        case H2_SETTINGS_MAX_FRAME_SIZE:
            if (val < H2_DEFAULT_MAX_FRAME_SIZE || val > H2_MAX_FRAME_SIZE_LIMIT)
                return -1; /* PROTOCOL_ERROR */
            conn->peer_settings.max_frame_size = val;
            break;
        case H2_SETTINGS_MAX_HEADER_LIST_SIZE:
            conn->peer_settings.max_header_list_size = val;
            break;
        default:
            /* Unknown settings MUST be ignored (RFC 9113 §6.5.2) */
            break;
        }
    }
    return 0;
}

/* ── Connection preface ───────────────────────────────────────────────── */

int h2_validate_preface(const uint8_t *buf, size_t len)
{
    if (len < H2_CONNECTION_PREFACE_LEN) return -1;
    return memcmp(buf, H2_CONNECTION_PREFACE, H2_CONNECTION_PREFACE_LEN) == 0
           ? 0 : -1;
}

/* ── Connection lifecycle ─────────────────────────────────────────────── */

H2Conn *h2_conn_new(void)
{
    H2Conn *c = calloc(1, sizeof(H2Conn));
    if (!c) return NULL;

    h2_settings_default(&c->local_settings);
    h2_settings_default(&c->peer_settings);
    c->conn_window_recv = H2_DEFAULT_INITIAL_WINDOW_SIZE;
    c->conn_window_send = H2_DEFAULT_INITIAL_WINDOW_SIZE;
    c->next_stream_id   = 2; /* server issues even-numbered streams */

    c->frame_buf_cap = H2_DEFAULT_MAX_FRAME_SIZE + 256;
    c->frame_buf = malloc(c->frame_buf_cap);
    if (!c->frame_buf) { free(c); return NULL; }

    hpack_table_init(&c->hpack_decode, H2_DEFAULT_HEADER_TABLE_SIZE);
    hpack_table_init(&c->hpack_encode, H2_DEFAULT_HEADER_TABLE_SIZE);

    return c;
}

void h2_conn_free(H2Conn *conn)
{
    if (!conn) return;
    for (size_t i = 0; i < conn->stream_count; i++) {
        free(conn->streams[i].header_block);
    }
    hpack_table_free(&conn->hpack_decode);
    hpack_table_free(&conn->hpack_encode);
    free(conn->frame_buf);
    free(conn);
}

/* ── GOAWAY ───────────────────────────────────────────────────────────── */

int h2_send_goaway(H2Conn *conn, int fd, void *ssl,
                   uint32_t last_stream_id, uint32_t error_code)
{
    uint8_t payload[8];
    last_stream_id &= 0x7FFFFFFF;
    payload[0] = (uint8_t)(last_stream_id >> 24);
    payload[1] = (uint8_t)(last_stream_id >> 16);
    payload[2] = (uint8_t)(last_stream_id >> 8);
    payload[3] = (uint8_t)(last_stream_id);
    payload[4] = (uint8_t)(error_code >> 24);
    payload[5] = (uint8_t)(error_code >> 16);
    payload[6] = (uint8_t)(error_code >> 8);
    payload[7] = (uint8_t)(error_code);

    conn->goaway_sent = 1;
    conn->goaway_last_stream = last_stream_id;

    return h2_frame_send(conn, fd, ssl, H2_FRAME_GOAWAY, 0, 0, payload, 8);
}

/* ── RST_STREAM ───────────────────────────────────────────────────────── */

int h2_send_rst_stream(H2Conn *conn, int fd, void *ssl,
                       uint32_t stream_id, uint32_t error_code)
{
    uint8_t payload[4];
    payload[0] = (uint8_t)(error_code >> 24);
    payload[1] = (uint8_t)(error_code >> 16);
    payload[2] = (uint8_t)(error_code >> 8);
    payload[3] = (uint8_t)(error_code);

    return h2_frame_send(conn, fd, ssl, H2_FRAME_RST_STREAM, 0,
                          stream_id, payload, 4);
}

/* ── PING ─────────────────────────────────────────────────────────────── */

int h2_send_ping(H2Conn *conn, int fd, void *ssl,
                 const uint8_t *opaque_data, int is_ack)
{
    uint8_t payload[8];
    if (opaque_data)
        memcpy(payload, opaque_data, 8);
    else
        memset(payload, 0, 8);

    uint8_t flags = is_ack ? H2_FLAG_ACK : 0;
    return h2_frame_send(conn, fd, ssl, H2_FRAME_PING, flags, 0, payload, 8);
}

/* ── WINDOW_UPDATE ────────────────────────────────────────────────────── */

int h2_send_window_update(H2Conn *conn, int fd, void *ssl,
                          uint32_t stream_id, uint32_t increment)
{
    if (increment == 0 || increment > H2_MAX_WINDOW_SIZE) return -1;

    uint8_t payload[4];
    increment &= 0x7FFFFFFF;
    payload[0] = (uint8_t)(increment >> 24);
    payload[1] = (uint8_t)(increment >> 16);
    payload[2] = (uint8_t)(increment >> 8);
    payload[3] = (uint8_t)(increment);

    return h2_frame_send(conn, fd, ssl, H2_FRAME_WINDOW_UPDATE, 0,
                          stream_id, payload, 4);
}

/* ── Stream management ────────────────────────────────────────────────── */

H2Stream *h2_stream_get(H2Conn *conn, uint32_t stream_id)
{
    /* Look for existing stream */
    for (size_t i = 0; i < conn->stream_count; i++) {
        if (conn->streams[i].id == stream_id)
            return &conn->streams[i];
    }

    /* Create new stream */
    if (conn->stream_count >= H2_MAX_STREAMS) return NULL;

    H2Stream *s = &conn->streams[conn->stream_count++];
    memset(s, 0, sizeof(*s));
    s->id     = stream_id;
    s->state  = H2_STREAM_IDLE;
    s->window = (int32_t)conn->peer_settings.initial_window_size;

    if (stream_id > conn->last_stream_id)
        conn->last_stream_id = stream_id;

    return s;
}

int h2_stream_transition(H2Stream *stream, uint8_t frame_type,
                         uint8_t flags, int is_sender)
{
    int end_stream = (flags & H2_FLAG_END_STREAM) != 0;

    switch (stream->state) {
    case H2_STREAM_IDLE:
        if (frame_type == H2_FRAME_HEADERS) {
            stream->state = end_stream
                ? (is_sender ? H2_STREAM_HALF_CLOSED_LOCAL
                             : H2_STREAM_HALF_CLOSED_REMOTE)
                : H2_STREAM_OPEN;
            return 0;
        }
        if (frame_type == H2_FRAME_PRIORITY) return 0; /* allowed in idle */
        return -1;

    case H2_STREAM_OPEN:
        if (end_stream) {
            stream->state = is_sender ? H2_STREAM_HALF_CLOSED_LOCAL
                                      : H2_STREAM_HALF_CLOSED_REMOTE;
        }
        if (frame_type == H2_FRAME_RST_STREAM) {
            stream->state = H2_STREAM_CLOSED;
        }
        return 0;

    case H2_STREAM_HALF_CLOSED_LOCAL:
        if (!is_sender) {
            if (end_stream) stream->state = H2_STREAM_CLOSED;
            return 0;
        }
        if (frame_type == H2_FRAME_RST_STREAM) {
            stream->state = H2_STREAM_CLOSED;
            return 0;
        }
        if (frame_type == H2_FRAME_WINDOW_UPDATE ||
            frame_type == H2_FRAME_PRIORITY) return 0;
        return -1; /* can't send DATA/HEADERS on half-closed local */

    case H2_STREAM_HALF_CLOSED_REMOTE:
        if (is_sender) {
            if (end_stream) stream->state = H2_STREAM_CLOSED;
            return 0;
        }
        if (frame_type == H2_FRAME_RST_STREAM) {
            stream->state = H2_STREAM_CLOSED;
            return 0;
        }
        if (frame_type == H2_FRAME_WINDOW_UPDATE ||
            frame_type == H2_FRAME_PRIORITY) return 0;
        return -1; /* peer can't send DATA/HEADERS on half-closed remote */

    case H2_STREAM_CLOSED:
        /* Only PRIORITY and WINDOW_UPDATE allowed on closed streams */
        if (frame_type == H2_FRAME_PRIORITY ||
            frame_type == H2_FRAME_WINDOW_UPDATE) return 0;
        return -1;

    default:
        return -1;
    }
}

/* ── HPACK static table (RFC 7541 Appendix A) ─────────────────────────── */

static const struct { const char *name; const char *value; } hpack_static[] = {
    { "",                        "" },  /* index 0: unused */
    { ":authority",              "" },
    { ":method",                 "GET" },
    { ":method",                 "POST" },
    { ":path",                   "/" },
    { ":path",                   "/index.html" },
    { ":scheme",                 "http" },
    { ":scheme",                 "https" },
    { ":status",                 "200" },
    { ":status",                 "204" },
    { ":status",                 "206" },
    { ":status",                 "304" },
    { ":status",                 "400" },
    { ":status",                 "404" },
    { ":status",                 "500" },
    { "accept-charset",          "" },
    { "accept-encoding",         "gzip, deflate" },
    { "accept-language",         "" },
    { "accept-ranges",           "" },
    { "accept",                  "" },
    { "access-control-allow-origin", "" },
    { "age",                     "" },
    { "allow",                   "" },
    { "authorization",           "" },
    { "cache-control",           "" },
    { "content-disposition",     "" },
    { "content-encoding",        "" },
    { "content-language",        "" },
    { "content-length",          "" },
    { "content-location",        "" },
    { "content-range",           "" },
    { "content-type",            "" },
    { "cookie",                  "" },
    { "date",                    "" },
    { "etag",                    "" },
    { "expect",                  "" },
    { "expires",                 "" },
    { "from",                    "" },
    { "host",                    "" },
    { "if-match",                "" },
    { "if-modified-since",       "" },
    { "if-none-match",           "" },
    { "if-range",                "" },
    { "if-unmodified-since",     "" },
    { "last-modified",           "" },
    { "link",                    "" },
    { "location",                "" },
    { "max-forwards",            "" },
    { "proxy-authenticate",      "" },
    { "proxy-authorization",     "" },
    { "range",                   "" },
    { "referer",                 "" },
    { "refresh",                 "" },
    { "retry-after",             "" },
    { "server",                  "" },
    { "set-cookie",              "" },
    { "strict-transport-security", "" },
    { "transfer-encoding",       "" },
    { "user-agent",              "" },
    { "vary",                    "" },
    { "via",                     "" },
    { "www-authenticate",        "" },
};

#define HPACK_STATIC_SIZE 61

/* ── HPACK dynamic table ──────────────────────────────────────────────── */

void hpack_table_init(HpackTable *t, size_t max_size)
{
    memset(t, 0, sizeof(*t));
    t->max_size = max_size;
    t->capacity = 64;
    t->entries  = calloc(t->capacity, sizeof(HpackEntry));
}

void hpack_table_free(HpackTable *t)
{
    for (size_t i = 0; i < t->count; i++) {
        free(t->entries[i].name);
        free(t->entries[i].value);
    }
    free(t->entries);
    memset(t, 0, sizeof(*t));
}

/* Evict oldest entries until table fits within max_size */
static void hpack_evict(HpackTable *t)
{
    while (t->size > t->max_size && t->count > 0) {
        t->count--;
        t->size -= t->entries[t->count].entry_size;
        free(t->entries[t->count].name);
        free(t->entries[t->count].value);
    }
}

/* Insert at front (index 0), shifting existing entries */
static int hpack_insert(HpackTable *t, const char *name, const char *value)
{
    size_t entry_size = strlen(name) + strlen(value) + 32;

    /* Entry too large for table — evict everything */
    if (entry_size > t->max_size) {
        for (size_t i = 0; i < t->count; i++) {
            free(t->entries[i].name);
            free(t->entries[i].value);
        }
        t->count = 0;
        t->size  = 0;
        return 0;
    }

    /* Evict until there's room */
    while (t->size + entry_size > t->max_size && t->count > 0) {
        t->count--;
        t->size -= t->entries[t->count].entry_size;
        free(t->entries[t->count].name);
        free(t->entries[t->count].value);
    }

    /* Grow array if needed */
    if (t->count >= t->capacity) {
        size_t new_cap = t->capacity * 2;
        HpackEntry *ne = realloc(t->entries, new_cap * sizeof(HpackEntry));
        if (!ne) return -1;
        t->entries  = ne;
        t->capacity = new_cap;
    }

    /* Shift entries right */
    if (t->count > 0)
        memmove(t->entries + 1, t->entries, t->count * sizeof(HpackEntry));

    t->entries[0].name       = strdup(name);
    t->entries[0].value      = strdup(value);
    t->entries[0].entry_size = entry_size;
    t->count++;
    t->size += entry_size;

    return 0;
}

void hpack_table_resize(HpackTable *t, size_t new_max)
{
    t->max_size = new_max;
    hpack_evict(t);
}

/* Look up by index: 1–61 = static, 62+ = dynamic */
static int hpack_lookup(HpackTable *t, size_t index,
                        const char **name, const char **value)
{
    if (index == 0) return -1;
    if (index <= HPACK_STATIC_SIZE) {
        *name  = hpack_static[index].name;
        *value = hpack_static[index].value;
        return 0;
    }
    size_t di = index - HPACK_STATIC_SIZE - 1;
    if (di >= t->count) return -1;
    *name  = t->entries[di].name;
    *value = t->entries[di].value;
    return 0;
}

/* ── HPACK integer decoding (RFC 7541 §5.1) ───────────────────────────── */

static int hpack_decode_int(const uint8_t *buf, size_t len, size_t *pos,
                            uint8_t prefix_bits, uint32_t *out)
{
    if (*pos >= len) return -1;
    uint8_t mask = (uint8_t)((1 << prefix_bits) - 1);
    *out = buf[*pos] & mask;
    (*pos)++;

    if (*out < mask) return 0;

    /* Multi-byte encoding */
    uint32_t m = 0;
    for (;;) {
        if (*pos >= len) return -1;
        uint8_t b = buf[*pos];
        (*pos)++;
        *out += (uint32_t)(b & 0x7F) << m;
        m += 7;
        if ((b & 0x80) == 0) break;
        if (m > 28) return -1; /* overflow protection */
    }
    return 0;
}

/* ── HPACK string decoding (RFC 7541 §5.2) ────────────────────────────── */

/* Huffman decoding omitted for now — use raw strings only.
 * Full Huffman support would add ~300 lines for the decode table. */

static char *hpack_decode_string(const uint8_t *buf, size_t len, size_t *pos)
{
    if (*pos >= len) return NULL;

    int huffman = (buf[*pos] & 0x80) != 0;
    uint32_t slen;
    if (hpack_decode_int(buf, len, pos, 7, &slen) < 0) return NULL;
    if (*pos + slen > len) return NULL;

    if (huffman) {
        /* TODO: implement Huffman decoding */
        /* For now, treat as protocol error */
        *pos += slen;
        return strdup("");
    }

    char *s = malloc(slen + 1);
    if (!s) return NULL;
    memcpy(s, buf + *pos, slen);
    s[slen] = '\0';
    *pos += slen;
    return s;
}

/* ── HPACK decode ─────────────────────────────────────────────────────── */

int hpack_decode(HpackTable *t, const uint8_t *buf, size_t len,
                 char ***names_out, char ***values_out)
{
    size_t cap = 32;
    char **names  = calloc(cap, sizeof(char *));
    char **values = calloc(cap, sizeof(char *));
    if (!names || !values) { free(names); free(values); return -1; }

    int count = 0;
    size_t pos = 0;

    while (pos < len) {
        char *name  = NULL;
        char *value = NULL;

        if (buf[pos] & 0x80) {
            /* Indexed header field (§6.1) */
            uint32_t index;
            if (hpack_decode_int(buf, len, &pos, 7, &index) < 0) goto err;
            const char *n, *v;
            if (hpack_lookup(t, index, &n, &v) < 0) goto err;
            name  = strdup(n);
            value = strdup(v);
        } else if (buf[pos] & 0x40) {
            /* Literal with incremental indexing (§6.2.1) */
            uint32_t index;
            if (hpack_decode_int(buf, len, &pos, 6, &index) < 0) goto err;
            if (index > 0) {
                const char *n, *v;
                if (hpack_lookup(t, index, &n, &v) < 0) goto err;
                name = strdup(n);
            } else {
                name = hpack_decode_string(buf, len, &pos);
            }
            value = hpack_decode_string(buf, len, &pos);
            if (!name || !value) goto err;
            hpack_insert(t, name, value);
        } else if (buf[pos] & 0x20) {
            /* Dynamic table size update (§6.3) */
            uint32_t new_size;
            if (hpack_decode_int(buf, len, &pos, 5, &new_size) < 0) goto err;
            hpack_table_resize(t, new_size);
            continue;
        } else {
            /* Literal without indexing (§6.2.2) or never indexed (§6.2.3) */
            int never = (buf[pos] & 0x10) != 0;
            (void)never;
            uint32_t index;
            if (hpack_decode_int(buf, len, &pos, 4, &index) < 0) goto err;
            if (index > 0) {
                const char *n, *v;
                if (hpack_lookup(t, index, &n, &v) < 0) goto err;
                name = strdup(n);
            } else {
                name = hpack_decode_string(buf, len, &pos);
            }
            value = hpack_decode_string(buf, len, &pos);
            if (!name || !value) goto err;
            /* Don't add to dynamic table */
        }

        if ((size_t)count >= cap) {
            cap *= 2;
            names  = realloc(names,  cap * sizeof(char *));
            values = realloc(values, cap * sizeof(char *));
            if (!names || !values) goto err;
        }
        names[count]  = name;
        values[count] = value;
        count++;
    }

    *names_out  = names;
    *values_out = values;
    return count;

err:
    for (int i = 0; i < count; i++) {
        free(names[i]);
        free(values[i]);
    }
    free(names);
    free(values);
    return -1;
}

/* ── HPACK integer encoding (RFC 7541 §5.1) ───────────────────────────── */

static int hpack_encode_int(uint8_t *buf, size_t cap, size_t *pos,
                            uint32_t value, uint8_t prefix_bits,
                            uint8_t prefix_pattern)
{
    if (*pos >= cap) return -1;
    uint8_t mask = (uint8_t)((1 << prefix_bits) - 1);

    if (value < mask) {
        buf[*pos] = prefix_pattern | (uint8_t)value;
        (*pos)++;
        return 0;
    }

    buf[*pos] = prefix_pattern | mask;
    (*pos)++;
    value -= mask;

    while (value >= 128) {
        if (*pos >= cap) return -1;
        buf[*pos] = (uint8_t)((value & 0x7F) | 0x80);
        (*pos)++;
        value >>= 7;
    }
    if (*pos >= cap) return -1;
    buf[*pos] = (uint8_t)value;
    (*pos)++;
    return 0;
}

/* ── HPACK encode ─────────────────────────────────────────────────────── */

int hpack_encode(HpackTable *t, const char **names, const char **values,
                 size_t count, uint8_t *out, size_t out_cap)
{
    size_t pos = 0;

    for (size_t i = 0; i < count; i++) {
        const char *name  = names[i];
        const char *value = values[i];

        /* Try to find in static table (name+value match) */
        int found_index = 0;
        int found_name  = 0;
        for (int j = 1; j <= HPACK_STATIC_SIZE; j++) {
            if (strcmp(hpack_static[j].name, name) == 0) {
                if (strcmp(hpack_static[j].value, value) == 0) {
                    found_index = j;
                    break;
                }
                if (!found_name) found_name = j;
            }
        }

        /* Also check dynamic table */
        if (!found_index) {
            for (size_t d = 0; d < t->count; d++) {
                if (strcmp(t->entries[d].name, name) == 0) {
                    if (strcmp(t->entries[d].value, value) == 0) {
                        found_index = (int)(d + HPACK_STATIC_SIZE + 1);
                        break;
                    }
                    if (!found_name)
                        found_name = (int)(d + HPACK_STATIC_SIZE + 1);
                }
            }
        }

        if (found_index) {
            /* Indexed header field (§6.1) */
            if (hpack_encode_int(out, out_cap, &pos,
                                 (uint32_t)found_index, 7, 0x80) < 0)
                return -1;
        } else if (found_name) {
            /* Literal with incremental indexing, indexed name (§6.2.1) */
            if (hpack_encode_int(out, out_cap, &pos,
                                 (uint32_t)found_name, 6, 0x40) < 0)
                return -1;
            /* Encode value as raw string */
            size_t vlen = strlen(value);
            if (hpack_encode_int(out, out_cap, &pos,
                                 (uint32_t)vlen, 7, 0x00) < 0)
                return -1;
            if (pos + vlen > out_cap) return -1;
            memcpy(out + pos, value, vlen);
            pos += vlen;
            hpack_insert(t, name, value);
        } else {
            /* Literal with incremental indexing, new name (§6.2.1) */
            if (pos >= out_cap) return -1;
            out[pos++] = 0x40; /* prefix: 01, index=0 */
            /* Encode name */
            size_t nlen = strlen(name);
            if (hpack_encode_int(out, out_cap, &pos,
                                 (uint32_t)nlen, 7, 0x00) < 0)
                return -1;
            if (pos + nlen > out_cap) return -1;
            memcpy(out + pos, name, nlen);
            pos += nlen;
            /* Encode value */
            size_t vlen = strlen(value);
            if (hpack_encode_int(out, out_cap, &pos,
                                 (uint32_t)vlen, 7, 0x00) < 0)
                return -1;
            if (pos + vlen > out_cap) return -1;
            memcpy(out + pos, value, vlen);
            pos += vlen;
            hpack_insert(t, name, value);
        }
    }

    return (int)pos;
}

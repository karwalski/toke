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
#include "http.h"   /* 127.144: http_write_all */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <time.h>

#ifdef TK_HAVE_OPENSSL
#include <openssl/ssl.h>
#endif

/* ── I/O helpers ──────────────────────────────────────────────────────── */

static ssize_t h2_write(int fd, void *ssl, const uint8_t *buf, size_t len)
{
#ifdef TK_HAVE_OPENSSL
    if (ssl) return SSL_write((SSL *)ssl, buf, (int)len);
#endif
    (void)ssl;
    /* 127.144: the cleartext-h2 path writes to a client socket, so it goes
     * through the same funnel as every other response write — a peer that
     * resets mid-frame must give us EPIPE, not a fatal signal. */
    return http_write_all(fd, buf, len);
}

/* ── Reading against a clock (121.44) ─────────────────────────────────
 *
 * The only clock on this path used to be SO_RCVTIMEO, which restarts on
 * every byte: a peer that trickled the preface, one frame, or a header block
 * of small CONTINUATION frames held the worker for as long as it kept going,
 * and over TLS a record trickled a byte at a time blocked inside SSL_read.
 * Every read now waits with poll() against an absolute deadline, and the TLS
 * socket is non-blocking while it does, so SSL_read cannot sit inside a
 * record (the HTTP/1.1 reader, http_read_request, does the same). */

#define H2_RD_TIMEOUT (-2)   /* the deadline passed                     */
#define H2_RD_IDLE    (-3)   /* no deadline, and no byte within the idle */

long h2_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

static ssize_t h2_read_dl_(H2Conn *c, int fd, void *ssl, uint8_t *buf,
                           size_t len, long deadline_ms)
{
    for (;;) {
        long wait;
        if (deadline_ms) {
            wait = deadline_ms - h2_now_ms();
            if (wait <= 0) return H2_RD_TIMEOUT;
        } else {
            wait = (long)c->idle_timeout_ms;
        }
        short ev = POLLIN;
#ifdef TK_HAVE_OPENSSL
        if (ssl) {
            int r = SSL_read((SSL *)ssl, buf, (int)(len > INT_MAX ? INT_MAX : len));
            if (r > 0) return r;
            int e = SSL_get_error((SSL *)ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            if (e == SSL_ERROR_WANT_WRITE) ev = POLLOUT;
            else if (e != SSL_ERROR_WANT_READ) return r == 0 ? 0 : -1;
        }
#endif
        struct pollfd p;
        p.fd = fd; p.events = ev; p.revents = 0;
        int w = poll(&p, 1, (int)(wait > INT_MAX ? INT_MAX : wait));
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        if (w == 0) {
            if (deadline_ms) continue;          /* re-checked above */
            return H2_RD_IDLE;
        }
        if (!ssl) {
            ssize_t r = read(fd, buf, len);
            if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
                continue;
            return r;
        }
    }
}

/* h2_read_dl — read at most len bytes by deadline_ms (0: wait for the first
 * byte at most idle_timeout_ms).  Returns bytes read, 0 at end of stream,
 * -1 on error, H2_RD_TIMEOUT or H2_RD_IDLE. */
static ssize_t h2_read_dl(H2Conn *c, int fd, void *ssl, uint8_t *buf,
                          size_t len, long deadline_ms)
{
    int fl = -1;
    if (ssl) {
        fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0 && !(fl & O_NONBLOCK)) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        else fl = -1;                           /* nothing to restore */
    }
    ssize_t r = h2_read_dl_(c, fd, ssl, buf, len, deadline_ms);
    if (fl >= 0) {
        int saved = errno;
        fcntl(fd, F_SETFL, fl);
        errno = saved;
    }
    return r;
}

/* Read exactly n bytes by deadline_ms (never 0 here).  Returns 0 on
 * success, -1 on error or end of stream (errno ETIMEDOUT for a deadline). */
static int h2_read_exact(H2Conn *c, int fd, void *ssl, uint8_t *buf, size_t n,
                         long deadline_ms)
{
    size_t done = 0;
    while (done < n) {
        ssize_t r = h2_read_dl(c, fd, ssl, buf + done, n - done, deadline_ms);
        if (r <= 0) {
            errno = (r == H2_RD_TIMEOUT || r == H2_RD_IDLE) ? ETIMEDOUT
                  : (r == 0 ? 0 : (errno == EAGAIN ? EIO : errno));
            return -1;
        }
        done += (size_t)r;
    }
    return 0;
}

int h2_conn_read(H2Conn *conn, int fd, void *ssl, uint8_t *buf, size_t n)
{
    long dl = conn->deadline_ms ? conn->deadline_ms
                                : h2_now_ms() + (long)conn->read_timeout_ms;
    return h2_read_exact(conn, fd, ssl, buf, n, dl);
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
    /* 121.44: the first byte is waited for until the open phase's deadline,
     * or -- with none open -- for the idle timeout, which is not an error.
     * From that byte on the whole frame must arrive within read_timeout_ms
     * (and within the phase deadline, if that is sooner). */
    long phase = conn->deadline_ms;
    ssize_t got = h2_read_dl(conn, fd, ssl, hdr_buf, H2_FRAME_HEADER_SIZE, phase);
    if (got <= 0) {
        errno = got == H2_RD_IDLE ? EAGAIN
              : got == H2_RD_TIMEOUT ? ETIMEDOUT
              : (got == 0 ? 0 : (errno == EAGAIN ? EIO : errno));
        return NULL;
    }
    long frame_dl = h2_now_ms() + (long)conn->read_timeout_ms;
    if (phase && phase < frame_dl) frame_dl = phase;
    if ((size_t)got < H2_FRAME_HEADER_SIZE &&
        h2_read_exact(conn, fd, ssl, hdr_buf + got,
                      H2_FRAME_HEADER_SIZE - (size_t)got, frame_dl) < 0)
        return NULL;

    h2_frame_header_read(hdr_buf, hdr);

    /* 121.17 (HTT-05): a frame we RECEIVE is bounded by the size WE
     * advertised (RFC 9113 4.2), not by the peer's own SETTINGS value.  The
     * peer may raise its max_frame_size to 16 MiB, and this used to let it
     * send us 16 MiB frames -- a buffer realloc'd to fit before the
     * connection loop's own local-limit check ran. */
    if (hdr->length > conn->local_settings.max_frame_size) {
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
        if (h2_read_exact(conn, fd, ssl, conn->frame_buf, hdr->length,
                          frame_dl) < 0)
            return NULL;
    }

    /* 135.3j (6): NULL means "failed", always.  frame_buf is NULL until the
     * first non-empty frame, so a zero-length frame (SETTINGS ACK, an empty
     * DATA) arriving first used to come back as NULL, indistinguishable from
     * a failed read. */
    static uint8_t h2_no_payload[1];
    return conn->frame_buf ? conn->frame_buf : h2_no_payload;
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
            /* 121.50 (2): resized AND signalled.  It used to be resized
             * only, so a peer that lowered the size never saw the dynamic
             * table size update RFC 7541 4.2 requires (strict decoders
             * reject the next block), and one that raised it had our table
             * grow past the 4096 bytes its decoder still used. */
            hpack_encoder_set_limit(&conn->hpack_encode, val);
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
                /* 121.50 (1): the windows now gate DATA, so the adjustment
                 * is checked -- a window pushed past 2^31-1 is a
                 * FLOW_CONTROL_ERROR (RFC 9113 6.9.2), not a signed
                 * overflow. */
                int64_t delta = (int64_t)val
                              - (int64_t)conn->peer_settings.initial_window_size;
                for (size_t s = 0; s < conn->stream_count; s++)
                    if ((int64_t)conn->streams[s].window + delta >
                        (int64_t)H2_MAX_WINDOW_SIZE)
                        return -1;
                for (size_t s = 0; s < conn->stream_count; s++)
                    conn->streams[s].window =
                        (int32_t)((int64_t)conn->streams[s].window + delta);
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
    c->read_timeout_ms  = H2_DEFAULT_READ_TIMEOUT_MS;   /* 121.44 */
    c->idle_timeout_ms  = H2_DEFAULT_IDLE_TIMEOUT_MS;
    c->deadline_ms      = 0;

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
    for (size_t i = 0; i < conn->stream_count; i++)
        h2_stream_release(&conn->streams[i]);
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

    /* Create new stream -- in a closed, empty slot if there is one */
    H2Stream *s = NULL;
    for (size_t i = 0; i < conn->stream_count && !s; i++) {
        H2Stream *c = &conn->streams[i];
        if (c->state == H2_STREAM_CLOSED && !c->header_block &&
            !c->hnames && !c->body && !c->out)
            s = c;
    }
    if (!s) {
        if (conn->stream_count >= H2_MAX_STREAMS) return NULL;
        s = &conn->streams[conn->stream_count++];
    }
    memset(s, 0, sizeof(*s));
    s->id     = stream_id;
    s->state  = H2_STREAM_IDLE;
    s->window = (int32_t)conn->peer_settings.initial_window_size;

    if (stream_id > conn->last_stream_id)
        conn->last_stream_id = stream_id;

    return s;
}

H2Stream *h2_stream_find(H2Conn *conn, uint32_t stream_id)
{
    for (size_t i = 0; i < conn->stream_count; i++)
        if (conn->streams[i].id == stream_id &&
            conn->streams[i].state != H2_STREAM_CLOSED)
            return &conn->streams[i];
    return NULL;
}

void h2_stream_release(H2Stream *s)
{
    free(s->header_block);
    s->header_block = NULL;
    s->header_block_len = s->header_block_cap = 0;
    for (int i = 0; i < s->hcount; i++) {
        free(s->hnames[i]);
        free(s->hvalues[i]);
    }
    free(s->hnames);
    free(s->hvalues);
    s->hnames = s->hvalues = NULL;
    s->hcount = 0;
    free(s->body);
    s->body = NULL;
    s->body_len = s->body_cap = 0;
    s->receiving = 0;
    s->block_end_stream = s->block_trailers = 0;
    s->deadline_ms = 0;
    free(s->out);
    s->out = NULL;
    s->out_len = s->out_off = 0;
    s->state = H2_STREAM_CLOSED;
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
    t->limit    = max_size;
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

    /* 135.3j: the copies are made FIRST.  They were strdup'd into
     * entries[0] after the shift, unchecked, so an allocation failure left
     * a counted entry with a NULL name/value that the next lookup handed
     * out and the encoder's strcmp dereferenced.  On failure the table is
     * unchanged and the caller is told (-1), so encoder and decoder cannot
     * silently disagree about the dynamic table. */
    char *ncopy = strdup(name);
    char *vcopy = strdup(value);
    if (!ncopy || !vcopy) { free(ncopy); free(vcopy); return -1; }

    /* Entry too large for table — evict everything */
    if (entry_size > t->max_size) {
        for (size_t i = 0; i < t->count; i++) {
            free(t->entries[i].name);
            free(t->entries[i].value);
        }
        t->count = 0;
        t->size  = 0;
        free(ncopy); free(vcopy);
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
        /* 135.3c: a zero capacity (table never given its initial 64) must
         * not realloc to zero bytes and then write entries[0]. */
        size_t new_cap = t->capacity ? t->capacity * 2 : 64;
        HpackEntry *ne = realloc(t->entries, new_cap * sizeof(HpackEntry));
        if (!ne) { free(ncopy); free(vcopy); return -1; }
        t->entries  = ne;
        t->capacity = new_cap;
    }

    /* Shift entries right */
    if (t->count > 0)
        memmove(t->entries + 1, t->entries, t->count * sizeof(HpackEntry));

    t->entries[0].name       = ncopy;
    t->entries[0].value      = vcopy;
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

void hpack_encoder_set_limit(HpackTable *t, size_t peer_max)
{
    size_t want = peer_max < H2_DEFAULT_HEADER_TABLE_SIZE
                ? peer_max : H2_DEFAULT_HEADER_TABLE_SIZE;
    if (!t->update_pending) {
        if (want == t->max_size) return;
        t->update_pending = 1;
        t->update_min = want;
    } else if (want < t->update_min) {
        t->update_min = want;
    }
    hpack_table_resize(t, want);
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

/* ── HPACK Huffman decode table (RFC 7541 Appendix B) ─────────────────── */

typedef struct { uint32_t code; uint8_t bits; } HuffEntry;

/* RFC 7541 Appendix B: Huffman code table for HPACK.
 * 257 entries: symbols 0-255 + EOS (256). */
static const HuffEntry huff_table[257] = {
    /* 0-31: control characters */
    {0x1ff8,13},{0x7fffd8,23},{0xfffffe2,28},{0xfffffe3,28},{0xfffffe4,28},
    {0xfffffe5,28},{0xfffffe6,28},{0xfffffe7,28},{0xfffffe8,28},{0xffffea,24},
    {0x3ffffffc,30},{0xfffffe9,28},{0xfffffea,28},{0x3ffffffd,30},{0xfffffeb,28},
    {0xfffffec,28},{0xfffffed,28},{0xfffffee,28},{0xfffffef,28},{0xffffff0,28},
    {0xffffff1,28},{0xffffff2,28},{0x3ffffffe,30},{0xffffff3,28},{0xffffff4,28},
    {0xffffff5,28},{0xffffff6,28},{0xffffff7,28},{0xffffff8,28},{0xffffff9,28},
    {0xffffffa,28},{0xffffffb,28},
    /* 32-63: space ! " # $ % & ' ( ) * + , - . / 0-9 : ; < = > ? */
    {0x14,6},{0x3f8,10},{0x3f9,10},{0xffa,12},{0x1ff9,13},{0x15,6},{0xf8,8},
    {0x7fa,11},{0x3fa,10},{0x3fb,10},{0xf9,8},{0x7fb,11},{0xfa,8},{0x16,6},
    {0x17,6},{0x18,6},{0x0,5},{0x1,5},{0x2,5},{0x19,6},{0x1a,6},{0x1b,6},
    {0x1c,6},{0x1d,6},{0x1e,6},{0x1f,6},{0x5c,7},{0xfb,8},{0x7ffc,15},
    {0x20,6},{0xffb,12},{0x3fc,10},
    /* 64-95: @ A-Z [ \ ] ^ _ */
    {0x1ffa,13},{0x21,6},{0x5d,7},{0x5e,7},{0x5f,7},{0x60,7},{0x61,7},{0x62,7},
    {0x63,7},{0x64,7},{0x65,7},{0x66,7},{0x67,7},{0x68,7},{0x69,7},{0x6a,7},
    {0x6b,7},{0x6c,7},{0x6d,7},{0x6e,7},{0x6f,7},{0x70,7},{0x71,7},{0x72,7},
    {0xfc,8},{0x73,7},{0xfd,8},{0x1ffb,13},{0x7fff0,19},{0x1ffc,13},{0x3ffc,14},
    {0x22,6},
    /* 96-127: ` a-z { | } ~ DEL */
    {0x7ffd,15},{0x3,5},{0x23,6},{0x4,5},{0x24,6},{0x5,5},{0x25,6},{0x26,6},
    {0x27,6},{0x6,5},{0x74,7},{0x75,7},{0x28,6},{0x29,6},{0x2a,6},{0x7,5},
    {0x2b,6},{0x76,7},{0x2c,6},{0x8,5},{0x9,5},{0x2d,6},{0x77,7},{0x78,7},
    {0x79,7},{0x7a,7},{0x7b,7},{0x7ffe,15},{0x7fc,11},{0x3ffd,14},{0x1ffd,13},
    {0xffffffc,28},
    /* 128-255: extended ASCII */
    {0xfffe6,20},{0x3fffd2,22},{0xfffe7,20},{0xfffe8,20},{0x3fffd3,22},
    {0x3fffd4,22},{0x3fffd5,22},{0x7fffd9,23},{0x3fffd6,22},{0x7fffda,23},
    {0x7fffdb,23},{0x7fffdc,23},{0x7fffdd,23},{0x7fffde,23},{0xffffeb,24},
    {0x7fffdf,23},{0xffffec,24},{0xffffed,24},{0x3fffd7,22},{0x7fffe0,23},
    {0xffffee,24},{0x7fffe1,23},{0x7fffe2,23},{0x7fffe3,23},{0x7fffe4,23},
    {0x1fffdc,21},{0x3fffd8,22},{0x7fffe5,23},{0x3fffd9,22},{0x7fffe6,23},
    {0x7fffe7,23},{0xffffef,24},{0x3fffda,22},{0x1fffdd,21},{0xfffe9,20},
    {0x3fffdb,22},{0x3fffdc,22},{0x7fffe8,23},{0x7fffe9,23},{0x1fffde,21},
    {0x7fffea,23},{0x3fffdd,22},{0x3fffde,22},{0xfffff0,24},{0x1fffdf,21},
    {0x3fffdf,22},{0x7fffeb,23},{0x7fffec,23},{0x1fffe0,21},{0x1fffe1,21},
    {0x3fffe0,22},{0x1fffe2,21},{0x7fffed,23},{0x3fffe1,22},{0x7fffee,23},
    {0x7fffef,23},{0xfffea,20},{0x3fffe2,22},{0x3fffe3,22},{0x3fffe4,22},
    {0x7ffff0,23},{0x3fffe5,22},{0x3fffe6,22},{0x7ffff1,23},{0x3ffffe0,26},
    {0x3ffffe1,26},{0xfffeb,20},{0x7fff1,19},{0x3fffe7,22},{0x7ffff2,23},
    {0x3fffe8,22},{0x1ffffec,25},{0x3ffffe2,26},{0x3ffffe3,26},{0x3ffffe4,26},
    {0x7ffffde,27},{0x7ffffdf,27},{0x3ffffe5,26},{0xfffff1,24},{0x1ffffed,25},
    {0x7fff2,19},{0x1fffe3,21},{0x3ffffe6,26},{0x7ffffe0,27},{0x7ffffe1,27},
    {0x3ffffe7,26},{0x7ffffe2,27},{0xfffff2,24},{0x1fffe4,21},{0x1fffe5,21},
    {0x3ffffe8,26},{0x3ffffe9,26},{0xffffffd,28},{0x7ffffe3,27},{0x7ffffe4,27},
    {0x7ffffe5,27},{0xfffec,20},{0xfffff3,24},{0xfffed,20},{0x1fffe6,21},
    {0x3fffe9,22},{0x1fffe7,21},{0x1fffe8,21},{0x7ffff3,23},{0x3fffea,22},
    {0x3fffeb,22},{0x1ffffee,25},{0x1ffffef,25},{0xfffff4,24},{0xfffff5,24},
    {0x3ffffea,26},{0x7ffff4,23},{0x3ffffeb,26},{0x7ffffe6,27},{0x3ffffec,26},
    {0x3ffffed,26},{0x7ffffe7,27},{0x7ffffe8,27},{0x7ffffe9,27},{0x7ffffea,27},
    {0x7ffffeb,27},{0xffffffe,28},{0x7ffffec,27},{0x7ffffed,27},{0x7ffffee,27},
    {0x7ffffef,27},{0x7fffff0,27},{0x3ffffee,26},
    /* 256: EOS */
    {0x3fffffff,30}
};

/* Decode a single Huffman symbol: given a candidate code of the specified
 * bit length, return the symbol (0-256) or -1 if no match. */
static int hpack_huff_decode_sym(uint32_t code, int bits)
{
    for (int i = 0; i < 257; i++) {
        if (huff_table[i].bits == (uint8_t)bits && huff_table[i].code == code)
            return i;
    }
    return -1;
}

/* ── HPACK string decoding (RFC 7541 §5.2) ────────────────────────────── */

static char *hpack_decode_string(const uint8_t *buf, size_t len, size_t *pos)
{
    if (*pos >= len) return NULL;

    int huffman = (buf[*pos] & 0x80) != 0;
    uint32_t slen;
    if (hpack_decode_int(buf, len, pos, 7, &slen) < 0) return NULL;
    if (*pos + slen > len) return NULL;

    if (huffman) {
        /* RFC 7541 Appendix B — HPACK Huffman decode */
        char *out = malloc(slen * 2 + 1); /* worst case: each byte → 1 char */
        if (!out) { *pos += slen; return NULL; }
        size_t olen = 0;
        uint64_t bits = 0;
        int nbits = 0;
        for (size_t si = 0; si < slen; si++) {
            bits = (bits << 8) | buf[*pos + si];
            nbits += 8;
            while (nbits >= 5) {
                /* Decode one symbol using the RFC 7541 Appendix B table.
                 * We use a flat lookup: extract top bits and match. */
                int found = 0;
                for (int blen = 5; blen <= 30 && blen <= nbits; blen++) {
                    uint32_t code = (uint32_t)((bits >> (nbits - blen)) &
                                               ((1UL << blen) - 1));
                    int sym = hpack_huff_decode_sym(code, blen);
                    if (sym >= 0) {
                        if (sym == 256) goto huff_done; /* EOS */
                        out[olen++] = (char)sym;
                        nbits -= blen;
                        bits &= ((1ULL << nbits) - 1);
                        found = 1;
                        break;
                    }
                }
                if (!found) break;
            }
        }
huff_done:
        out[olen] = '\0';
        *pos += slen;
        return out;
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
    /* 135.3c: outside the loop so `err` can free a half-decoded pair.  A
     * header whose name decoded and whose value did not (malformed input,
     * i.e. any peer) used to leak the name on every such block. */
    char *name  = NULL;
    char *value = NULL;

    while (pos < len) {
        name  = NULL;
        value = NULL;

        if (buf[pos] & 0x80) {
            /* Indexed header field (§6.1) */
            uint32_t index;
            if (hpack_decode_int(buf, len, &pos, 7, &index) < 0) goto err;
            const char *n, *v;
            if (hpack_lookup(t, index, &n, &v) < 0) goto err;
            name  = strdup(n);
            value = strdup(v);
            /* 121.17 (HTT-08): an unchecked strdup here put NULL into the
             * header list the caller strcasecmp()s. */
            if (!name || !value) goto err;
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
            if (hpack_insert(t, name, value) < 0) goto err;
        } else if (buf[pos] & 0x20) {
            /* Dynamic table size update (§6.3) */
            uint32_t new_size;
            if (hpack_decode_int(buf, len, &pos, 5, &new_size) < 0) goto err;
            /* 121.17 (HTT-05): RFC 7541 6.3 -- an update above the size this
             * side advertised is a decoding error.  It was applied as sent
             * (up to ~2^32), so the peer could grow our table past the limit
             * we told it. */
            if ((size_t)new_size > t->limit) goto err;
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
            /* 135.3c: through temporaries -- assigning realloc's NULL straight
             * back lost the arrays, then `err` indexed through NULL. */
            char **nn = realloc(names, cap * 2 * sizeof(char *));
            if (!nn) goto err;
            names = nn;
            char **nv = realloc(values, cap * 2 * sizeof(char *));
            if (!nv) goto err;
            values = nv;
            cap *= 2;
        }
        names[count]  = name;
        values[count] = value;
        count++;
    }

    *names_out  = names;
    *values_out = values;
    return count;

err:
    free(name);
    free(value);
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

    /* 121.50 (2): a size change opens the block (RFC 7541 4.2). */
    if (t->update_pending) {
        if (t->update_min < t->max_size &&
            hpack_encode_int(out, out_cap, &pos, (uint32_t)t->update_min,
                             5, 0x20) < 0)
            return -1;
        if (hpack_encode_int(out, out_cap, &pos, (uint32_t)t->max_size,
                             5, 0x20) < 0)
            return -1;
        t->update_pending = 0;
    }

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
            if (hpack_insert(t, name, value) < 0) return -1;
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
            if (hpack_insert(t, name, value) < 0) return -1;
        }
    }

    return (int)pos;
}

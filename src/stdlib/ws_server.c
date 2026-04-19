/*
 * ws_server.c — WebSocket & streaming integration for std.http.
 *
 * Epic 68: WebSocket upgrade in HTTP handler, WS proxy to backend,
 * ping/pong keepalive, frame size limits, SSE response helper,
 * SSE keep-alive, HTTP/2 streamed responses.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

/* ── WebSocket opcodes and close codes ──────────────────────────────── */

#define WS_OP_TEXT    0x01
#define WS_OP_BINARY 0x02
#define WS_OP_CLOSE  0x08
#define WS_OP_PING   0x09
#define WS_OP_PONG   0x0A

#define WS_CLOSE_NORMAL      1000
#define WS_CLOSE_GOING_AWAY  1001
#define WS_CLOSE_PROTOCOL    1002
#define WS_CLOSE_TOO_BIG     1009

/* ── WebSocket server configuration (68.1.3, 68.1.4) ───────────────── */

typedef struct {
    uint64_t idle_timeout_s;     /* max idle time before close (default 300) */
    uint64_t ping_interval_s;    /* PING interval (default 30) */
    uint64_t pong_timeout_s;     /* max wait for PONG (default 10) */
    uint64_t max_frame_size;     /* max single frame (default 1MB) */
    uint64_t max_message_size;   /* max assembled message (default 16MB) */
} WsServerConfig;

static WsServerConfig g_ws_config = {
    .idle_timeout_s   = 300,
    .ping_interval_s  = 30,
    .pong_timeout_s   = 10,
    .max_frame_size   = 1048576,     /* 1 MB */
    .max_message_size = 16777216,    /* 16 MB */
};

void ws_server_set_config(const WsServerConfig *cfg)
{
    if (cfg) g_ws_config = *cfg;
}

void ws_server_set_idle_timeout(uint64_t secs)
{
    g_ws_config.idle_timeout_s = secs;
}

void ws_server_set_ping_interval(uint64_t secs)
{
    g_ws_config.ping_interval_s = secs;
}

void ws_server_set_max_frame_size(uint64_t bytes)
{
    g_ws_config.max_frame_size = bytes;
}

void ws_server_set_max_message_size(uint64_t bytes)
{
    g_ws_config.max_message_size = bytes;
}

/* ── SHA-1 for WebSocket handshake ──────────────────────────────────── */

/* Minimal SHA-1 for WebSocket accept key derivation.
 * Only used for the 20-byte hash of the concatenated key+GUID. */
static void sha1_block(uint32_t *h, const uint8_t *block)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16) |
               ((uint32_t)block[i*4+2] << 8) | block[i*4+3];
    for (int i = 16; i < 80; i++) {
        uint32_t t = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (t << 1) | (t >> 31);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }
        uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    size_t i;
    for (i = 0; i + 64 <= len; i += 64)
        sha1_block(h, data + i);

    uint8_t pad[128];
    memset(pad, 0, sizeof(pad));
    size_t rem = len - i;
    memcpy(pad, data + i, rem);
    pad[rem] = 0x80;
    size_t padlen = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int j = 0; j < 8; j++)
        pad[padlen - 1 - j] = (uint8_t)(bits >> (j * 8));
    for (size_t j = 0; j < padlen; j += 64)
        sha1_block(h, pad + j);

    for (int j = 0; j < 5; j++) {
        out[j*4]   = (uint8_t)(h[j] >> 24);
        out[j*4+1] = (uint8_t)(h[j] >> 16);
        out[j*4+2] = (uint8_t)(h[j] >> 8);
        out[j*4+3] = (uint8_t)(h[j]);
    }
}

/* ── Base64 encode ──────────────────────────────────────────────────── */

static const char b64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t i, j = 0;
    for (i = 0; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i+1] << 8) | in[i+2];
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = b64_table[(v >> 6)  & 0x3F];
        out[j++] = b64_table[v & 0x3F];
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i+1] << 8;
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }
    out[j] = '\0';
}

/* ── WebSocket upgrade in HTTP handler (68.1.1) ─────────────────────── */

#define WS_GUID "258EAFA5-E914-47DA-95CA-5AB5DC11D685"

/*
 * ws_server_accept_key — compute the Sec-WebSocket-Accept value.
 * client_key is the Sec-WebSocket-Key header value.
 * Returns heap-allocated accept key string. Caller owns.
 */
char *ws_server_accept_key(const char *client_key)
{
    if (!client_key) return NULL;

    size_t klen = strlen(client_key);
    size_t glen = strlen(WS_GUID);
    char *concat = malloc(klen + glen + 1);
    if (!concat) return NULL;
    memcpy(concat, client_key, klen);
    memcpy(concat + klen, WS_GUID, glen + 1);

    uint8_t hash[20];
    sha1((const uint8_t *)concat, klen + glen, hash);
    free(concat);

    /* Base64 of 20 bytes = 28 chars + NUL */
    char *accept = malloc(32);
    if (!accept) return NULL;
    base64_encode(hash, 20, accept);
    return accept;
}

/*
 * ws_server_handshake — perform WebSocket upgrade handshake on fd.
 * Sends the 101 Switching Protocols response.
 * Returns 0 on success, -1 on error.
 */
int ws_server_handshake(int fd, const char *client_key)
{
    char *accept = ws_server_accept_key(client_key);
    if (!accept) return -1;

    char resp[512];
    int n = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    free(accept);

    ssize_t w = write(fd, resp, (size_t)n);
    return (w == n) ? 0 : -1;
}

/* ── WebSocket frame I/O (68.1.3, 68.1.4) ──────────────────────────── */

/*
 * ws_server_send_frame — send a WebSocket frame.
 * opcode: WS_OP_TEXT, WS_OP_BINARY, WS_OP_PING, WS_OP_PONG, WS_OP_CLOSE
 * Server frames are NOT masked (per RFC 6455).
 */
int ws_server_send_frame(int fd, uint8_t opcode,
                          const uint8_t *data, size_t len)
{
    uint8_t hdr[10];
    size_t hlen = 2;

    hdr[0] = 0x80 | (opcode & 0x0F); /* FIN + opcode */

    if (len < 126) {
        hdr[1] = (uint8_t)len;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (uint8_t)(len >> 8);
        hdr[3] = (uint8_t)(len);
        hlen = 4;
    } else {
        hdr[1] = 127;
        for (int i = 0; i < 8; i++)
            hdr[2 + i] = (uint8_t)(len >> ((7 - i) * 8));
        hlen = 10;
    }

    if (write(fd, hdr, hlen) != (ssize_t)hlen) return -1;
    if (len > 0 && write(fd, data, len) != (ssize_t)len) return -1;
    return 0;
}

/*
 * ws_server_send_ping — send a PING frame.
 */
int ws_server_send_ping(int fd)
{
    return ws_server_send_frame(fd, WS_OP_PING, NULL, 0);
}

/*
 * ws_server_send_pong — send a PONG frame with the given payload.
 */
int ws_server_send_pong(int fd, const uint8_t *data, size_t len)
{
    return ws_server_send_frame(fd, WS_OP_PONG, data, len);
}

/*
 * ws_server_send_close — send a CLOSE frame with status code.
 */
int ws_server_send_close(int fd, uint16_t code)
{
    uint8_t payload[2];
    payload[0] = (uint8_t)(code >> 8);
    payload[1] = (uint8_t)(code);
    return ws_server_send_frame(fd, WS_OP_CLOSE, payload, 2);
}

/* ── Read a client frame (masked) ───────────────────────────────────── */

typedef struct {
    uint8_t  opcode;
    uint8_t *data;
    size_t   len;
    int      error;   /* 0 = ok, -1 = read error, 1 = close, 2 = too big */
} WsFrame;

static ssize_t read_full(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

WsFrame ws_server_read_frame(int fd)
{
    WsFrame f;
    memset(&f, 0, sizeof(f));

    uint8_t hdr[2];
    if (read_full(fd, hdr, 2) < 0) { f.error = -1; return f; }

    f.opcode = hdr[0] & 0x0F;
    int masked = (hdr[1] & 0x80) != 0;
    uint64_t payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        uint8_t ext[2];
        if (read_full(fd, ext, 2) < 0) { f.error = -1; return f; }
        payload_len = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payload_len == 127) {
        uint8_t ext[8];
        if (read_full(fd, ext, 8) < 0) { f.error = -1; return f; }
        payload_len = 0;
        for (int i = 0; i < 8; i++)
            payload_len = (payload_len << 8) | ext[i];
    }

    /* Frame size limit (68.1.4) */
    if (payload_len > g_ws_config.max_frame_size) {
        ws_server_send_close(fd, WS_CLOSE_TOO_BIG);
        f.error = 2;
        return f;
    }

    uint8_t mask_key[4] = {0};
    if (masked) {
        if (read_full(fd, mask_key, 4) < 0) { f.error = -1; return f; }
    }

    f.data = NULL;
    f.len = (size_t)payload_len;
    if (payload_len > 0) {
        f.data = malloc((size_t)payload_len);
        if (!f.data) { f.error = -1; return f; }
        if (read_full(fd, f.data, (size_t)payload_len) < 0) {
            free(f.data); f.data = NULL; f.error = -1; return f;
        }
        if (masked) {
            for (size_t i = 0; i < (size_t)payload_len; i++)
                f.data[i] ^= mask_key[i & 3];
        }
    }

    if (f.opcode == WS_OP_CLOSE) f.error = 1;
    return f;
}

void ws_frame_result_free(WsFrame *f)
{
    if (f && f->data) { free(f->data); f->data = NULL; }
}

/* ── WebSocket proxy to backend (68.1.2) ────────────────────────────── */

/*
 * ws_proxy_connect — connect to a backend WebSocket server.
 * Returns fd on success, -1 on error.
 */
int ws_proxy_connect(const char *host, uint16_t port, const char *path)
{
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    struct hostent *he = gethostbyname(host);
    if (!he) return -1;
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    /* Send WebSocket upgrade request */
    char req[1024];
    int n = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        path ? path : "/", host, (unsigned)port);

    if (write(fd, req, (size_t)n) != n) { close(fd); return -1; }

    /* Read 101 response (simplified — just check status line) */
    char resp[1024];
    ssize_t r = read(fd, resp, sizeof(resp) - 1);
    if (r <= 0) { close(fd); return -1; }
    resp[r] = '\0';
    if (strstr(resp, "101") == NULL) { close(fd); return -1; }

    return fd;
}

/*
 * ws_proxy_relay — bidirectionally relay WebSocket frames between
 * client_fd and backend_fd. Blocks until one side closes.
 */
void ws_proxy_relay(int client_fd, int backend_fd)
{
    fd_set fds;
    int maxfd = (client_fd > backend_fd ? client_fd : backend_fd) + 1;

    for (;;) {
        FD_ZERO(&fds);
        FD_SET(client_fd, &fds);
        FD_SET(backend_fd, &fds);

        struct timeval tv;
        tv.tv_sec = (long)g_ws_config.idle_timeout_s;
        tv.tv_usec = 0;

        int n = select(maxfd, &fds, NULL, NULL, &tv);
        if (n <= 0) break; /* timeout or error */

        uint8_t buf[65536];

        if (FD_ISSET(client_fd, &fds)) {
            ssize_t r = read(client_fd, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(backend_fd, buf, (size_t)r) != r) break;
        }

        if (FD_ISSET(backend_fd, &fds)) {
            ssize_t r = read(backend_fd, buf, sizeof(buf));
            if (r <= 0) break;
            if (write(client_fd, buf, (size_t)r) != r) break;
        }
    }

    /* Send close to both sides */
    ws_server_send_close(client_fd, WS_CLOSE_GOING_AWAY);
    ws_server_send_close(backend_fd, WS_CLOSE_GOING_AWAY);
}

/* ── SSE response helper (68.2.1) ───────────────────────────────────── */

typedef struct {
    int      fd;
    uint64_t event_id;
    time_t   last_send;
} SseResponse;

/*
 * sse_response_begin — initiate an SSE response on the given fd.
 * Sends the required headers. Returns a handle for sending events.
 */
SseResponse *sse_response_begin(int fd)
{
    const char *headers =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "X-Accel-Buffering: no\r\n\r\n";

    size_t hlen = strlen(headers);
    if (write(fd, headers, hlen) != (ssize_t)hlen) return NULL;

    SseResponse *sse = calloc(1, sizeof(SseResponse));
    if (!sse) return NULL;
    sse->fd = fd;
    sse->event_id = 0;
    sse->last_send = time(NULL);
    return sse;
}

/*
 * sse_response_send — send an SSE event.
 */
int sse_response_send(SseResponse *sse, const char *event_type,
                       const char *data)
{
    if (!sse || !data) return -1;
    char buf[8192];
    int n;

    if (event_type) {
        n = snprintf(buf, sizeof(buf), "id: %llu\nevent: %s\ndata: %s\n\n",
                     (unsigned long long)++sse->event_id, event_type, data);
    } else {
        n = snprintf(buf, sizeof(buf), "id: %llu\ndata: %s\n\n",
                     (unsigned long long)++sse->event_id, data);
    }

    sse->last_send = time(NULL);
    ssize_t w = write(sse->fd, buf, (size_t)n);
    return (w == n) ? 0 : -1;
}

/* ── SSE keep-alive (68.2.2) ────────────────────────────────────────── */

static uint64_t g_sse_keepalive_interval = 15; /* seconds */

void sse_set_keepalive_interval(uint64_t secs)
{
    g_sse_keepalive_interval = secs;
}

/*
 * sse_response_keepalive — send a keepalive comment if enough time
 * has elapsed since the last send. Returns 0 on success, -1 on error.
 */
int sse_response_keepalive(SseResponse *sse)
{
    if (!sse) return -1;
    time_t now = time(NULL);
    if ((uint64_t)(now - sse->last_send) < g_sse_keepalive_interval)
        return 0; /* not yet */

    const char *keepalive = ": keepalive\n\n";
    size_t len = strlen(keepalive);
    sse->last_send = now;
    ssize_t w = write(sse->fd, keepalive, len);
    return (w == (ssize_t)len) ? 0 : -1;
}

void sse_response_close(SseResponse *sse)
{
    free(sse);
}

/* ── HTTP/2 streamed responses (68.2.3) ─────────────────────────────── */

/*
 * h2_stream_response — configuration for HTTP/2 long-lived streaming.
 * Sets up the stream for SSE or chunked dynamic content with proper
 * flow control awareness.
 */
typedef struct {
    uint32_t stream_id;
    int      fd;                /* underlying socket (or SSL) */
    uint32_t peer_window;       /* peer's flow control window */
    uint32_t initial_window;    /* initial window size */
} H2StreamWriter;

H2StreamWriter *h2_stream_writer_new(int fd, uint32_t stream_id,
                                      uint32_t initial_window)
{
    H2StreamWriter *w = calloc(1, sizeof(H2StreamWriter));
    if (!w) return NULL;
    w->fd = fd;
    w->stream_id = stream_id;
    w->peer_window = initial_window;
    w->initial_window = initial_window;
    return w;
}

/*
 * h2_stream_write — write data to an HTTP/2 stream, respecting flow control.
 * If the peer window is exhausted, this returns the amount written
 * (may be less than len). Caller should wait for WINDOW_UPDATE.
 */
size_t h2_stream_write(H2StreamWriter *w, const uint8_t *data, size_t len)
{
    if (!w || !data || len == 0) return 0;

    /* Respect flow control window */
    size_t can_send = (w->peer_window < len) ? w->peer_window : len;
    if (can_send == 0) return 0;

    /* Build HTTP/2 DATA frame: 9-byte header + payload */
    uint8_t hdr[9];
    hdr[0] = (uint8_t)(can_send >> 16);
    hdr[1] = (uint8_t)(can_send >> 8);
    hdr[2] = (uint8_t)(can_send);
    hdr[3] = 0x00; /* DATA frame type */
    hdr[4] = 0x00; /* no flags (not END_STREAM) */
    hdr[5] = (uint8_t)(w->stream_id >> 24);
    hdr[6] = (uint8_t)(w->stream_id >> 16);
    hdr[7] = (uint8_t)(w->stream_id >> 8);
    hdr[8] = (uint8_t)(w->stream_id);

    if (write(w->fd, hdr, 9) != 9) return 0;
    ssize_t wr = write(w->fd, data, can_send);
    if (wr <= 0) return 0;

    w->peer_window -= (uint32_t)wr;
    return (size_t)wr;
}

/*
 * h2_stream_update_window — called when a WINDOW_UPDATE frame is received.
 */
void h2_stream_update_window(H2StreamWriter *w, uint32_t increment)
{
    if (w) w->peer_window += increment;
}

void h2_stream_writer_free(H2StreamWriter *w)
{
    free(w);
}

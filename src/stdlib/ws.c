/*
 * ws.c — Implementation of the std.ws standard library module.
 *
 * WebSocket framing is implemented per RFC 6455.
 * SHA-1 is implemented inline per RFC 3174 (no external crypto deps).
 * Base64 encoding is implemented inline.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own returned pointers.
 *
 * Story: 15.1.1
 */

#include "ws.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <time.h>

/* -----------------------------------------------------------------------
 * SHA-1 core (RFC 3174)
 * Portable, self-contained, C99 conformant.
 * ----------------------------------------------------------------------- */

#define SHA1_DIGEST_SIZE 20
#define SHA1_BLOCK_SIZE  64

typedef struct {
    uint32_t  h[5];
    uint64_t  count;       /* total bits processed */
    uint8_t   buf[SHA1_BLOCK_SIZE];
    uint32_t  buf_used;    /* bytes in buf */
} Sha1Ctx;

static uint32_t sha1_rotl32(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static void sha1_process_block(Sha1Ctx *ctx, const uint8_t *block)
{
    uint32_t w[80];
    uint32_t a, b, c, d, e, f, k, temp;
    int i;

    /* Prepare message schedule */
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4+0] << 24)
             | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] <<  8)
             | ((uint32_t)block[i*4+3]);
    }
    for (i = 16; i < 80; i++) {
        w[i] = sha1_rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    }

    a = ctx->h[0];
    b = ctx->h[1];
    c = ctx->h[2];
    d = ctx->h[3];
    e = ctx->h[4];

    for (i = 0; i < 80; i++) {
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        temp = sha1_rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = sha1_rotl32(b, 30);
        b = a;
        a = temp;
    }

    ctx->h[0] += a;
    ctx->h[1] += b;
    ctx->h[2] += c;
    ctx->h[3] += d;
    ctx->h[4] += e;
}

static void sha1_init(Sha1Ctx *ctx)
{
    ctx->h[0] = 0x67452301u;
    ctx->h[1] = 0xEFCDAB89u;
    ctx->h[2] = 0x98BADCFEu;
    ctx->h[3] = 0x10325476u;
    ctx->h[4] = 0xC3D2E1F0u;
    ctx->count    = 0;
    ctx->buf_used = 0;
}

static void sha1_update(Sha1Ctx *ctx, const uint8_t *data, uint64_t len)
{
    uint64_t i;
    for (i = 0; i < len; i++) {
        ctx->buf[ctx->buf_used++] = data[i];
        ctx->count += 8;
        if (ctx->buf_used == SHA1_BLOCK_SIZE) {
            sha1_process_block(ctx, ctx->buf);
            ctx->buf_used = 0;
        }
    }
}

static void sha1_final(Sha1Ctx *ctx, uint8_t digest[SHA1_DIGEST_SIZE])
{
    uint64_t bit_count = ctx->count;
    int i;

    /* Append 0x80 byte */
    ctx->buf[ctx->buf_used++] = 0x80;

    /* Pad to 56 bytes (leaving 8 for the 64-bit length) */
    if (ctx->buf_used > 56) {
        /* Not enough room — pad to end of block, process, then pad again */
        while (ctx->buf_used < SHA1_BLOCK_SIZE)
            ctx->buf[ctx->buf_used++] = 0x00;
        sha1_process_block(ctx, ctx->buf);
        ctx->buf_used = 0;
    }
    while (ctx->buf_used < 56)
        ctx->buf[ctx->buf_used++] = 0x00;

    /* Append bit length (big-endian 64-bit) */
    ctx->buf[56] = (uint8_t)(bit_count >> 56);
    ctx->buf[57] = (uint8_t)(bit_count >> 48);
    ctx->buf[58] = (uint8_t)(bit_count >> 40);
    ctx->buf[59] = (uint8_t)(bit_count >> 32);
    ctx->buf[60] = (uint8_t)(bit_count >> 24);
    ctx->buf[61] = (uint8_t)(bit_count >> 16);
    ctx->buf[62] = (uint8_t)(bit_count >>  8);
    ctx->buf[63] = (uint8_t)(bit_count      );
    sha1_process_block(ctx, ctx->buf);

    /* Produce big-endian digest */
    for (i = 0; i < 5; i++) {
        digest[i*4+0] = (uint8_t)(ctx->h[i] >> 24);
        digest[i*4+1] = (uint8_t)(ctx->h[i] >> 16);
        digest[i*4+2] = (uint8_t)(ctx->h[i] >>  8);
        digest[i*4+3] = (uint8_t)(ctx->h[i]      );
    }
}

/* Convenience: hash a single buffer */
static void sha1_oneshot(const uint8_t *data, uint64_t len,
                         uint8_t digest[SHA1_DIGEST_SIZE])
{
    Sha1Ctx ctx;
    sha1_init(&ctx);
    sha1_update(&ctx, data, len);
    sha1_final(&ctx, digest);
}

/* -----------------------------------------------------------------------
 * Base64 encoding
 * ----------------------------------------------------------------------- */

static const char B64_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * base64_encode: encode src[0..srclen) into a heap-allocated NUL-terminated
 * string.  Caller must free the result.
 */
static char *base64_encode(const uint8_t *src, uint64_t srclen)
{
    uint64_t out_len = ((srclen + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(out_len);
    uint64_t i, j;

    if (!out) return NULL;

    for (i = 0, j = 0; i + 2 < srclen; i += 3) {
        out[j++] = B64_CHARS[(src[i]   >> 2) & 0x3F];
        out[j++] = B64_CHARS[((src[i]   & 0x03) << 4) | ((src[i+1] >> 4) & 0x0F)];
        out[j++] = B64_CHARS[((src[i+1] & 0x0F) << 2) | ((src[i+2] >> 6) & 0x03)];
        out[j++] = B64_CHARS[src[i+2]  & 0x3F];
    }
    if (i < srclen) {
        out[j++] = B64_CHARS[(src[i] >> 2) & 0x3F];
        if (i + 1 < srclen) {
            out[j++] = B64_CHARS[((src[i]   & 0x03) << 4) | ((src[i+1] >> 4) & 0x0F)];
            out[j++] = B64_CHARS[ (src[i+1] & 0x0F) << 2];
        } else {
            out[j++] = B64_CHARS[(src[i] & 0x03) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

/* -----------------------------------------------------------------------
 * Frame encoding (RFC 6455 §5.2)
 * ----------------------------------------------------------------------- */

WsEncodeResult ws_encode_frame(WsOpcode opcode, const uint8_t *payload,
                               uint64_t plen, int mask)
{
    WsEncodeResult res;
    uint8_t header[14]; /* max header size: 2 + 8 + 4 */
    uint32_t header_len = 0;
    uint8_t masking_key[4];
    uint8_t *out;
    uint64_t total;
    uint64_t i;

    res.data   = NULL;
    res.len    = 0;
    res.is_err = 0;
    res.err_msg = NULL;

    /* Byte 0: FIN=1, RSV1-3=0, opcode */
    header[header_len++] = (uint8_t)(0x80 | (opcode & 0x0F));

    /* Byte 1: MASK bit + payload length */
    if (plen <= 125) {
        header[header_len++] = (uint8_t)((mask ? 0x80 : 0x00) | (uint8_t)plen);
    } else if (plen <= 65535) {
        header[header_len++] = (uint8_t)((mask ? 0x80 : 0x00) | 126);
        header[header_len++] = (uint8_t)(plen >> 8);
        header[header_len++] = (uint8_t)(plen     );
    } else {
        header[header_len++] = (uint8_t)((mask ? 0x80 : 0x00) | 127);
        header[header_len++] = (uint8_t)(plen >> 56);
        header[header_len++] = (uint8_t)(plen >> 48);
        header[header_len++] = (uint8_t)(plen >> 40);
        header[header_len++] = (uint8_t)(plen >> 32);
        header[header_len++] = (uint8_t)(plen >> 24);
        header[header_len++] = (uint8_t)(plen >> 16);
        header[header_len++] = (uint8_t)(plen >>  8);
        header[header_len++] = (uint8_t)(plen      );
    }

    if (mask) {
        /* Generate 4-byte masking key using rand() — sufficient for tests */
        masking_key[0] = (uint8_t)(rand() & 0xFF);
        masking_key[1] = (uint8_t)(rand() & 0xFF);
        masking_key[2] = (uint8_t)(rand() & 0xFF);
        masking_key[3] = (uint8_t)(rand() & 0xFF);
        header[header_len++] = masking_key[0];
        header[header_len++] = masking_key[1];
        header[header_len++] = masking_key[2];
        header[header_len++] = masking_key[3];
    }

    total = header_len + plen;
    out = (uint8_t *)malloc(total);
    if (!out) {
        res.is_err  = 1;
        res.err_msg = "ws_encode_frame: malloc failed";
        return res;
    }

    memcpy(out, header, header_len);
    if (plen > 0 && payload) {
        if (mask) {
            for (i = 0; i < plen; i++)
                out[header_len + i] = payload[i] ^ masking_key[i & 3];
        } else {
            memcpy(out + header_len, payload, (size_t)plen);
        }
    }

    res.data = out;
    res.len  = total;
    return res;
}

/* -----------------------------------------------------------------------
 * Frame decoding (RFC 6455 §5.2)
 * ----------------------------------------------------------------------- */

WsFrameResult ws_decode_frame(const uint8_t *buf, uint64_t buflen,
                              uint64_t *consumed_out)
{
    WsFrameResult res;
    WsFrame *frame;
    uint8_t  b0, b1;
    int      fin, masked;
    uint64_t payload_len;
    uint64_t header_len;
    const uint8_t *masking_key_ptr;
    uint8_t *payload;
    uint64_t i;

    res.frame   = NULL;
    res.is_err  = 0;
    res.err_msg = NULL;
    if (consumed_out) *consumed_out = 0;

    /* Need at least 2 bytes for the base header */
    if (buflen < 2) {
        res.is_err  = 1;
        res.err_msg = "ws_decode_frame: truncated buffer (< 2 bytes)";
        return res;
    }

    b0 = buf[0];
    b1 = buf[1];

    fin    = (b0 >> 7) & 0x01;
    masked = (b1 >> 7) & 0x01;
    payload_len = b1 & 0x7F;
    header_len  = 2;

    if (payload_len == 126) {
        /* 16-bit extended length */
        if (buflen < 4) {
            res.is_err  = 1;
            res.err_msg = "ws_decode_frame: truncated buffer (16-bit length)";
            return res;
        }
        payload_len = ((uint64_t)buf[2] << 8) | (uint64_t)buf[3];
        header_len  = 4;
    } else if (payload_len == 127) {
        /* 64-bit extended length */
        if (buflen < 10) {
            res.is_err  = 1;
            res.err_msg = "ws_decode_frame: truncated buffer (64-bit length)";
            return res;
        }
        payload_len = ((uint64_t)buf[2] << 56)
                    | ((uint64_t)buf[3] << 48)
                    | ((uint64_t)buf[4] << 40)
                    | ((uint64_t)buf[5] << 32)
                    | ((uint64_t)buf[6] << 24)
                    | ((uint64_t)buf[7] << 16)
                    | ((uint64_t)buf[8] <<  8)
                    | ((uint64_t)buf[9]      );
        header_len  = 10;
    }

    /* Masking key (4 bytes, if MASK bit set) */
    if (masked) {
        if (buflen < header_len + 4) {
            res.is_err  = 1;
            res.err_msg = "ws_decode_frame: truncated buffer (masking key)";
            return res;
        }
        masking_key_ptr = buf + header_len;
        header_len += 4;
    } else {
        masking_key_ptr = NULL;
    }

    /* Verify enough bytes remain for payload */
    if (buflen < header_len + payload_len) {
        res.is_err  = 1;
        res.err_msg = "ws_decode_frame: truncated buffer (payload)";
        return res;
    }

    /* Allocate frame + payload */
    frame = (WsFrame *)malloc(sizeof(WsFrame));
    if (!frame) {
        res.is_err  = 1;
        res.err_msg = "ws_decode_frame: malloc failed (frame)";
        return res;
    }
    payload = (uint8_t *)malloc(payload_len + 1); /* +1 for convenience NUL */
    if (!payload) {
        free(frame);
        res.is_err  = 1;
        res.err_msg = "ws_decode_frame: malloc failed (payload)";
        return res;
    }

    /* Copy and unmask payload */
    if (masked && masking_key_ptr) {
        for (i = 0; i < payload_len; i++)
            payload[i] = buf[header_len + i] ^ masking_key_ptr[i & 3];
    } else {
        if (payload_len > 0)
            memcpy(payload, buf + header_len, (size_t)payload_len);
    }
    payload[payload_len] = '\0';

    frame->opcode      = (WsOpcode)(b0 & 0x0F);
    frame->payload     = payload;
    frame->payload_len = payload_len;
    frame->is_final    = fin;

    if (consumed_out)
        *consumed_out = header_len + payload_len;

    res.frame = frame;
    return res;
}

/* -----------------------------------------------------------------------
 * Handshake helpers
 * ----------------------------------------------------------------------- */

/* RFC 6455 §4.1: the fixed GUID appended to the client key */
static const char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

const char *ws_accept_key(const char *client_key)
{
    uint8_t  digest[SHA1_DIGEST_SIZE];
    uint64_t key_len;
    uint64_t guid_len;
    uint64_t concat_len;
    uint8_t *concat;
    char    *result;

    if (!client_key) return NULL;

    key_len    = (uint64_t)strlen(client_key);
    guid_len   = (uint64_t)strlen(WS_GUID);
    concat_len = key_len + guid_len;

    concat = (uint8_t *)malloc(concat_len);
    if (!concat) return NULL;

    memcpy(concat,           client_key, (size_t)key_len);
    memcpy(concat + key_len, WS_GUID,    (size_t)guid_len);

    sha1_oneshot(concat, concat_len, digest);
    free(concat);

    result = base64_encode(digest, SHA1_DIGEST_SIZE);
    return (const char *)result;
}

int ws_is_upgrade_request(const char *http_headers)
{
    /*
     * Search for "upgrade:" header (case-insensitive) with value
     * containing "websocket" (case-insensitive).
     * We scan line by line for a header whose name is "upgrade".
     */
    const char *p = http_headers;

    if (!p) return 0;

    while (*p) {
        /* Skip to start of header name (skip CRLF or LF) */
        const char *line_start = p;
        const char *colon;
        const char *val_start;
        const char *line_end;
        size_t name_len;
        char name_buf[32];
        size_t i;

        /* Find end of line */
        line_end = p;
        while (*line_end && *line_end != '\r' && *line_end != '\n')
            line_end++;

        /* Find colon in this line */
        colon = p;
        while (colon < line_end && *colon != ':') colon++;

        if (colon < line_end) {
            name_len = (size_t)(colon - line_start);
            if (name_len < sizeof(name_buf)) {
                for (i = 0; i < name_len; i++)
                    name_buf[i] = (char)tolower((unsigned char)line_start[i]);
                name_buf[name_len] = '\0';

                if (strcmp(name_buf, "upgrade") == 0) {
                    /* Check value contains "websocket" */
                    val_start = colon + 1;
                    /* skip optional whitespace */
                    while (val_start < line_end && *val_start == ' ')
                        val_start++;

                    /* case-insensitive search for "websocket" in value */
                    {
                        const char *v = val_start;
                        while (v < line_end) {
                            if ((line_end - v) >= 9) {
                                char tmp[10];
                                size_t j;
                                for (j = 0; j < 9; j++)
                                    tmp[j] = (char)tolower((unsigned char)v[j]);
                                tmp[9] = '\0';
                                if (strcmp(tmp, "websocket") == 0)
                                    return 1;
                            }
                            v++;
                        }
                    }
                }
            }
        }

        /* Advance past line ending */
        p = line_end;
        while (*p == '\r' || *p == '\n') p++;
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Memory release helpers
 * ----------------------------------------------------------------------- */

void ws_frame_free(WsFrame *f)
{
    if (!f) return;
    free(f->payload);
    free(f);
}

void ws_encode_result_free(WsEncodeResult *r)
{
    if (!r) return;
    free(r->data);
    r->data = NULL;
    r->len  = 0;
}

/* -----------------------------------------------------------------------
 * High-level connection API (Story 35.1.4)
 * ----------------------------------------------------------------------- */

/* Generate a random 16-byte base64-encoded Sec-WebSocket-Key. */
static char *ws_generate_key(void)
{
    uint8_t raw[16];
    int i;
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = 1;
    }
    for (i = 0; i < 16; i++)
        raw[i] = (uint8_t)(rand() & 0xFF);
    return base64_encode(raw, 16);
}

/*
 * Parse a ws:// URL into host, port, and path components.
 * Returns 0 on success, -1 on error.
 * host_out must be freed by caller.
 * path_out must be freed by caller.
 */
static int ws_parse_url(const char *url, char **host_out,
                        uint16_t *port_out, char **path_out)
{
    const char *p;
    const char *host_start;
    const char *host_end;
    const char *port_start;
    size_t host_len;
    char port_buf[8];
    size_t port_len;
    const char *path_start;
    size_t path_len;

    *host_out = NULL;
    *path_out = NULL;
    *port_out = 80;

    /* Must start with ws:// */
    if (strncmp(url, "ws://", 5) != 0)
        return -1;

    p = url + 5;
    host_start = p;

    /* Find end of host (: or / or end) */
    host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/')
        host_end++;

    host_len = (size_t)(host_end - host_start);
    if (host_len == 0)
        return -1;

    *host_out = (char *)malloc(host_len + 1);
    if (!*host_out) return -1;
    memcpy(*host_out, host_start, host_len);
    (*host_out)[host_len] = '\0';

    p = host_end;

    /* Optional port */
    if (*p == ':') {
        p++;
        port_start = p;
        while (*p >= '0' && *p <= '9') p++;
        port_len = (size_t)(p - port_start);
        if (port_len == 0 || port_len > 5) {
            free(*host_out);
            *host_out = NULL;
            return -1;
        }
        memcpy(port_buf, port_start, port_len);
        port_buf[port_len] = '\0';
        *port_out = (uint16_t)atoi(port_buf);
    }

    /* Path (default to "/") */
    if (*p == '/') {
        path_start = p;
        path_len = strlen(path_start);
    } else {
        path_start = "/";
        path_len = 1;
    }
    *path_out = (char *)malloc(path_len + 1);
    if (!*path_out) {
        free(*host_out);
        *host_out = NULL;
        return -1;
    }
    memcpy(*path_out, path_start, path_len);
    (*path_out)[path_len] = '\0';

    return 0;
}

/* Send all bytes over a socket. Returns 0 on success, -1 on error. */
static int ws_send_all(int fd, const uint8_t *buf, uint64_t len)
{
    uint64_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, buf + sent, (size_t)(len - sent));
        if (n <= 0) return -1;
        sent += (uint64_t)n;
    }
    return 0;
}

/* Read exactly `len` bytes from socket. Returns 0 on success, -1 on error. */
static int ws_recv_all(int fd, uint8_t *buf, uint64_t len)
{
    uint64_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, (size_t)(len - got));
        if (n <= 0) return -1;
        got += (uint64_t)n;
    }
    return 0;
}

WsConnResult ws_connect(const char *url)
{
    WsConnResult res;
    char *host = NULL;
    char *path = NULL;
    uint16_t port = 80;
    int fd = -1;
    struct addrinfo hints, *ai = NULL, *rp;
    char port_str[8];
    char *ws_key = NULL;
    const char *expected_accept = NULL;
    char request[1024];
    int req_len;
    char response[4096];
    int resp_len = 0;
    ssize_t n;
    const char *accept_hdr;

    res.conn    = NULL;
    res.is_err  = 0;
    res.err_msg = NULL;

    if (!url) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: NULL url";
        return res;
    }

    if (ws_parse_url(url, &host, &port, &path) != 0) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: invalid URL (only ws:// supported)";
        return res;
    }

    /* DNS resolve + TCP connect */
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", (int)port);

    if (getaddrinfo(host, port_str, &hints, &ai) != 0) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: DNS resolution failed";
        goto cleanup;
    }

    for (rp = ai; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: TCP connect failed";
        goto cleanup;
    }

    /* Build HTTP Upgrade request */
    ws_key = ws_generate_key();
    if (!ws_key) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: key generation failed";
        close(fd);
        fd = -1;
        goto cleanup;
    }

    req_len = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n",
        path, host, (int)port, ws_key);

    if (ws_send_all(fd, (const uint8_t *)request, (uint64_t)req_len) != 0) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: failed to send upgrade request";
        close(fd);
        fd = -1;
        goto cleanup;
    }

    /* Read HTTP response headers (look for \r\n\r\n) */
    resp_len = 0;
    while (resp_len < (int)sizeof(response) - 1) {
        n = read(fd, response + resp_len, 1);
        if (n <= 0) {
            res.is_err  = 1;
            res.err_msg = "ws_connect: failed to read upgrade response";
            close(fd);
            fd = -1;
            goto cleanup;
        }
        resp_len += (int)n;
        if (resp_len >= 4 &&
            response[resp_len-4] == '\r' && response[resp_len-3] == '\n' &&
            response[resp_len-2] == '\r' && response[resp_len-1] == '\n')
            break;
    }
    response[resp_len] = '\0';

    /* Verify HTTP 101 status */
    if (strstr(response, "101") == NULL) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: server did not return 101 Switching Protocols";
        close(fd);
        fd = -1;
        goto cleanup;
    }

    /* Verify Sec-WebSocket-Accept */
    expected_accept = ws_accept_key(ws_key);
    if (!expected_accept) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: failed to compute expected accept key";
        close(fd);
        fd = -1;
        goto cleanup;
    }

    accept_hdr = strstr(response, "Sec-WebSocket-Accept: ");
    if (!accept_hdr) {
        /* Try lowercase */
        accept_hdr = strstr(response, "sec-websocket-accept: ");
    }
    if (!accept_hdr || strncmp(accept_hdr + 22, expected_accept,
                                strlen(expected_accept)) != 0) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: accept key mismatch";
        close(fd);
        fd = -1;
        goto cleanup;
    }

    /* Success — allocate connection */
    res.conn = (WsConn *)malloc(sizeof(WsConn));
    if (!res.conn) {
        res.is_err  = 1;
        res.err_msg = "ws_connect: malloc failed";
        close(fd);
        fd = -1;
        goto cleanup;
    }
    res.conn->fd     = fd;
    res.conn->mask   = 1;   /* client always masks */
    res.conn->closed = 0;
    fd = -1;  /* ownership transferred */

cleanup:
    free(host);
    free(path);
    free(ws_key);
    free((void *)expected_accept);
    if (ai) freeaddrinfo(ai);
    return res;
}

WsSendResult ws_send(WsConn *conn, const char *text)
{
    WsSendResult res;
    WsEncodeResult enc;
    uint64_t text_len;

    res.is_err  = 0;
    res.err_msg = NULL;

    if (!conn || conn->closed) {
        res.is_err  = 1;
        res.err_msg = "ws_send: connection closed or NULL";
        return res;
    }
    if (!text) {
        res.is_err  = 1;
        res.err_msg = "ws_send: NULL text";
        return res;
    }

    text_len = (uint64_t)strlen(text);
    enc = ws_encode_frame(WS_TEXT, (const uint8_t *)text, text_len, conn->mask);
    if (enc.is_err) {
        res.is_err  = 1;
        res.err_msg = enc.err_msg;
        return res;
    }

    if (ws_send_all(conn->fd, enc.data, enc.len) != 0) {
        res.is_err  = 1;
        res.err_msg = "ws_send: write failed";
    }

    ws_encode_result_free(&enc);
    return res;
}

WsSendResult ws_sendbytes(WsConn *conn, const uint8_t *data, uint64_t len)
{
    WsSendResult res;
    WsEncodeResult enc;

    res.is_err  = 0;
    res.err_msg = NULL;

    if (!conn || conn->closed) {
        res.is_err  = 1;
        res.err_msg = "ws_sendbytes: connection closed or NULL";
        return res;
    }

    enc = ws_encode_frame(WS_BINARY, data, len, conn->mask);
    if (enc.is_err) {
        res.is_err  = 1;
        res.err_msg = enc.err_msg;
        return res;
    }

    if (ws_send_all(conn->fd, enc.data, enc.len) != 0) {
        res.is_err  = 1;
        res.err_msg = "ws_sendbytes: write failed";
    }

    ws_encode_result_free(&enc);
    return res;
}

WsRecvResult ws_recv(WsConn *conn)
{
    WsRecvResult res;
    uint8_t hdr[2];
    uint8_t ext_len_buf[8];
    uint64_t payload_len;
    uint8_t mask_key[4];
    int masked;
    uint8_t *payload;
    uint64_t i;

    res.frame   = NULL;
    res.is_err  = 0;
    res.err_msg = NULL;

    if (!conn || conn->closed) {
        res.is_err  = 1;
        res.err_msg = "ws_recv: connection closed or NULL";
        return res;
    }

    /* Read 2-byte header */
    if (ws_recv_all(conn->fd, hdr, 2) != 0) {
        res.is_err  = 1;
        res.err_msg = "ws_recv: failed to read frame header";
        return res;
    }

    masked = (hdr[1] >> 7) & 1;
    payload_len = hdr[1] & 0x7F;

    if (payload_len == 126) {
        if (ws_recv_all(conn->fd, ext_len_buf, 2) != 0) {
            res.is_err  = 1;
            res.err_msg = "ws_recv: failed to read 16-bit length";
            return res;
        }
        payload_len = ((uint64_t)ext_len_buf[0] << 8)
                    |  (uint64_t)ext_len_buf[1];
    } else if (payload_len == 127) {
        if (ws_recv_all(conn->fd, ext_len_buf, 8) != 0) {
            res.is_err  = 1;
            res.err_msg = "ws_recv: failed to read 64-bit length";
            return res;
        }
        payload_len = ((uint64_t)ext_len_buf[0] << 56)
                    | ((uint64_t)ext_len_buf[1] << 48)
                    | ((uint64_t)ext_len_buf[2] << 40)
                    | ((uint64_t)ext_len_buf[3] << 32)
                    | ((uint64_t)ext_len_buf[4] << 24)
                    | ((uint64_t)ext_len_buf[5] << 16)
                    | ((uint64_t)ext_len_buf[6] <<  8)
                    |  (uint64_t)ext_len_buf[7];
    }

    /* Read masking key if present */
    if (masked) {
        if (ws_recv_all(conn->fd, mask_key, 4) != 0) {
            res.is_err  = 1;
            res.err_msg = "ws_recv: failed to read masking key";
            return res;
        }
    }

    /* Read payload */
    payload = (uint8_t *)malloc(payload_len + 1);
    if (!payload) {
        res.is_err  = 1;
        res.err_msg = "ws_recv: malloc failed";
        return res;
    }

    if (payload_len > 0) {
        if (ws_recv_all(conn->fd, payload, payload_len) != 0) {
            free(payload);
            res.is_err  = 1;
            res.err_msg = "ws_recv: failed to read payload";
            return res;
        }
    }

    /* Unmask if needed */
    if (masked) {
        for (i = 0; i < payload_len; i++)
            payload[i] ^= mask_key[i & 3];
    }
    payload[payload_len] = '\0';

    /* Build frame */
    res.frame = (WsFrame *)malloc(sizeof(WsFrame));
    if (!res.frame) {
        free(payload);
        res.is_err  = 1;
        res.err_msg = "ws_recv: malloc failed (frame)";
        return res;
    }

    res.frame->opcode      = (WsOpcode)(hdr[0] & 0x0F);
    res.frame->payload     = payload;
    res.frame->payload_len = payload_len;
    res.frame->is_final    = (hdr[0] >> 7) & 1;

    /* Auto-detect close frame */
    if (res.frame->opcode == WS_CLOSE)
        conn->closed = 1;

    return res;
}

void ws_close(WsConn *conn)
{
    WsEncodeResult enc;

    if (!conn) return;
    if (!conn->closed) {
        /* Send close frame (best-effort) */
        enc = ws_encode_frame(WS_CLOSE, NULL, 0, conn->mask);
        if (!enc.is_err && enc.data) {
            (void)ws_send_all(conn->fd, enc.data, enc.len);
            ws_encode_result_free(&enc);
        }
        conn->closed = 1;
    }
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
}

void ws_broadcast(WsConn **conns, uint64_t count, const char *text)
{
    uint64_t i;
    if (!conns || !text) return;
    for (i = 0; i < count; i++) {
        if (conns[i] && !conns[i]->closed)
            (void)ws_send(conns[i], text);
    }
}

void ws_conn_free(WsConn *conn)
{
    if (!conn) return;
    if (conn->fd >= 0) {
        close(conn->fd);
        conn->fd = -1;
    }
    free(conn);
}

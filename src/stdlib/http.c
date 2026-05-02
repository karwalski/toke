/*
 * http.c — Implementation of the std.http standard library module.
 *
 * Uses POSIX sockets; no external HTTP library.
 * malloc is permitted: stdlib boundary, not arena-managed compiler code.
 *
 * Story: 1.3.2  Branch: feature/stdlib-http
 */

#include "http.h"
#include "log.h"
#include "encoding.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <zlib.h>

/* ── Graceful shutdown flag (Story 27.1.10) ─────────────────────────── */

static volatile sig_atomic_t g_shutdown_requested = 0;

void http_shutdown(void)
{
    g_shutdown_requested = 1;
}

/* ── CORS configuration (Story 7.7.4) ────────────────────────────────── */

static char *g_cors_origins = NULL;  /* heap-allocated; NULL = disabled */
static const char *g_cors_matched = NULL; /* per-request matched origin (set before send_response) */

void http_set_cors(const char *origins)
{
    free(g_cors_origins);
    g_cors_origins = origins ? strdup(origins) : NULL;
}

/* ── Server request-size limits (Story 27.1.9) ───────────────────────── */

typedef struct {
    uint32_t max_header;   /* bytes before \r\n\r\n         */
    uint32_t max_body;     /* Content-Length ceiling         */
    uint32_t timeout_secs; /* SO_RCVTIMEO on accepted socket */
} SrvLimits;

static SrvLimits srv_limits = {
    HTTP_DEFAULT_MAX_HEADER_SIZE,
    HTTP_DEFAULT_MAX_BODY_SIZE,
    HTTP_DEFAULT_TIMEOUT_SECS
};

void http_set_limits(uint32_t max_header, uint32_t max_body,
                     uint32_t timeout_secs)
{
    if (max_header   > 0) srv_limits.max_header   = max_header;
    if (max_body     > 0) srv_limits.max_body     = max_body;
    if (timeout_secs > 0) srv_limits.timeout_secs = timeout_secs;
}

/* ── Route table ─────────────────────────────────────────────────────── */

#define MAX_ROUTES 512

typedef struct { const char *method; const char *pattern; RouteHandler h; } Route;

static Route route_table[MAX_ROUTES];
static int   route_count = 0;

int http_route_count(void) { return route_count; }

static void reg(const char *m, const char *p, RouteHandler h) {
    if (route_count < MAX_ROUTES) {
        route_table[route_count].method  = m;
        route_table[route_count].pattern = p;
        route_table[route_count].h       = h;
        route_count++;
    }
}

void http_GET   (const char *p, RouteHandler h) { reg("GET",    p, h); }
void http_POST  (const char *p, RouteHandler h) { reg("POST",   p, h); }
void http_PUT   (const char *p, RouteHandler h) { reg("PUT",    p, h); }
void http_DELETE(const char *p, RouteHandler h) { reg("DELETE", p, h); }
void http_PATCH (const char *p, RouteHandler h) { reg("PATCH",  p, h); }

/* ── Response constructors ──────────────────────────────────────────── */

static Res make_res(uint16_t s, const char *b) {
    Res r; r.status = s; r.body = b ? b : "";
    r.headers.data = NULL; r.headers.len = 0; return r;
}
Res http_Res_ok  (const char *b)            { return make_res(200, b); }
Res http_Res_json(uint16_t s, const char *b){ return make_res(s,   b); }
Res http_Res_bad (const char *m)            { return make_res(400, m ? m : "Bad Request"); }
Res http_Res_err (const char *m)            { return make_res(500, m ? m : "Internal Server Error"); }

/* ── Accessors ──────────────────────────────────────────────────────── */

HttpResult http_param(Req req, const char *name) {
    for (uint64_t i = 0; i < req.params.len; i++)
        if (strcmp(req.params.data[i].key, name) == 0) {
            HttpResult r = {req.params.data[i].val, 0, {0,NULL,0}}; return r;
        }
    HttpResult r = {NULL, 1, {HTTP_ERR_NOT_FOUND, "param not found", 0}}; return r;
}

HttpResult http_header(Req req, const char *name) {
    for (uint64_t i = 0; i < req.headers.len; i++)
        if (strcasecmp(req.headers.data[i].key, name) == 0) {
            HttpResult r = {req.headers.data[i].val, 0, {0,NULL,0}}; return r;
        }
    HttpResult r = {NULL, 1, {HTTP_ERR_NOT_FOUND, "header not found", 0}}; return r;
}

/* ── Pattern matching ───────────────────────────────────────────────── */

static int match_pattern(const char *pat, const char *path,
                         StrPair *out, int *cnt)
{
    /* Wildcard catch-all: pattern "*" matches any path */
    if (strcmp(pat, "*") == 0) { *cnt = 0; return 1; }
    *cnt = 0;
    while (*pat && *path) {
        if (*pat == ':') {
            pat++;
            const char *ks = pat; while (*pat && *pat != '/') pat++;
            size_t kn = (size_t)(pat - ks);
            char *key = malloc(kn + 1); if (!key) return 0;
            memcpy(key, ks, kn); key[kn] = '\0';
            const char *vs = path; while (*path && *path != '/') path++;
            size_t vn = (size_t)(path - vs);
            char *val = malloc(vn + 1); if (!val) { free(key); return 0; }
            memcpy(val, vs, vn); val[vn] = '\0';
            out[(*cnt)].key = key; out[(*cnt)].val = val; (*cnt)++;
        } else {
            if (*pat != *path) return 0;
            pat++; path++;
        }
    }
    return *pat == '\0' && *path == '\0';
}

/* ── Request parsing ────────────────────────────────────────────────── */

static Req parse_request(const char *raw) {
    Req req; memset(&req, 0, sizeof(req));
    char *buf = malloc(strlen(raw) + 1); if (!buf) return req;
    strcpy(buf, raw);
    char *nl = strstr(buf, "\r\n"); if (!nl) { free(buf); return req; }
    *nl = '\0';
    char *sp1 = strchr(buf, ' '); if (!sp1) { free(buf); return req; }
    *sp1 = '\0'; req.method = strdup(buf);
    char *sp2 = strchr(sp1+1, ' '); if (sp2) *sp2 = '\0';
    req.path = strdup(sp1+1);
    StrPair *hdrs = malloc(64 * sizeof(StrPair)); int hc = 0;
    char *p = nl + 2;
    while (1) {
        char *e = strstr(p, "\r\n"); if (!e || e == p) break;
        *e = '\0';
        char *col = strchr(p, ':');
        if (col && hc < 64) {
            *col = '\0';
            hdrs[hc].key = strdup(p);
            hdrs[hc].val = strdup(col+1 + (*(col+1)==' ' ? 1 : 0));
            hc++;
        }
        p = e + 2;
    }
    req.headers.data = hdrs; req.headers.len = (uint64_t)hc;
    const char *bs = strstr(raw, "\r\n\r\n");
    req.body = bs ? strdup(bs+4) : strdup("");
    free(buf); return req;
}

/* ── Gzip response compression (Story 59.4.5) ────────────────────────── */

#define GZIP_MIN_SIZE 256

/*
 * http_gzip_compress — gzip-compress src into a malloc'd buffer.
 * Returns 0 on success and sets *dst / *dst_len; -1 on failure.
 */
static int http_gzip_compress(const char *src, size_t srclen,
                               char **dst, size_t *dst_len)
{
    uLong bound = compressBound((uLong)srclen) + 32UL;
    char *buf = malloc(bound);
    if (!buf) return -1;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(buf); return -1;
    }
    zs.next_in   = (Bytef *)(uintptr_t)src;
    zs.avail_in  = (uInt)srclen;
    zs.next_out  = (Bytef *)buf;
    zs.avail_out = (uInt)bound;

    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&zs); free(buf); return -1;
    }
    *dst_len = (size_t)zs.total_out;
    deflateEnd(&zs);
    *dst = buf;
    return 0;
}

/* req_accepts_gzip — return 1 if the parsed request has Accept-Encoding: gzip */
static int req_accepts_gzip(Req req)
{
    for (uint64_t i = 0; i < req.headers.len; i++) {
        if (req.headers.data[i].key &&
            strcasecmp(req.headers.data[i].key, "Accept-Encoding") == 0) {
            return req.headers.data[i].val &&
                   strstr(req.headers.data[i].val, "gzip") != NULL;
        }
    }
    return 0;
}

/*
 * maybe_gzip_body — if the response body is eligible, replace it with
 * gzip-compressed version and add Content-Encoding + Vary headers.
 * Returns 1 if compressed (caller must free compressed_buf), 0 if not.
 */
static int maybe_gzip_body(Req req, Res *res,
                            char **compressed_buf, size_t *compressed_len)
{
    *compressed_buf = NULL;
    *compressed_len = 0;
    if (!res->body) return 0;
    size_t bodylen = strlen(res->body);
    if (bodylen < GZIP_MIN_SIZE) return 0;
    if (res->status < 200 || res->status >= 300) return 0;
    if (!req_accepts_gzip(req)) return 0;

    /* Skip if Content-Encoding already set */
    for (uint64_t i = 0; i < res->headers.len; i++) {
        if (res->headers.data[i].key &&
            strcasecmp(res->headers.data[i].key, "Content-Encoding") == 0)
            return 0;
    }

    if (http_gzip_compress(res->body, bodylen, compressed_buf, compressed_len) != 0)
        return 0;
    return 1;
}

/* ── Chunked transfer encoding (Story 27.1.4) ───────────────────────── */

/*
 * http_chunked_write — write body as HTTP/1.1 chunked encoding to fd.
 *
 * The body is split into chunks of at most chunk_size bytes.  Each chunk is
 * sent as:  <hex-length>\r\n<data>\r\n
 * Followed by a terminating chunk:  0\r\n\r\n
 *
 * chunk_size == 0 is treated as a single chunk containing the whole body.
 *
 * Returns 0 on success, -1 on write error.
 */
int http_chunked_write(int fd, const char *data, size_t len, size_t chunk_size)
{
    if (chunk_size == 0) chunk_size = len ? len : 1;

    size_t off = 0;
    while (off < len) {
        size_t csz = len - off;
        if (csz > chunk_size) csz = chunk_size;

        /* Write chunk header: <hex-size>\r\n */
        char hdr[32];
        int hl = snprintf(hdr, sizeof(hdr), "%zx\r\n", csz);
        if (hl < 0) return -1;
        if (write(fd, hdr, (size_t)hl) < 0) return -1;

        /* Write chunk data */
        if (write(fd, data + off, csz) < 0) return -1;

        /* Write chunk trailer \r\n */
        if (write(fd, "\r\n", 2) < 0) return -1;

        off += csz;
    }

    /* Terminating chunk */
    if (write(fd, "0\r\n\r\n", 5) < 0) return -1;

    return 0;
}

/*
 * http_chunked_read — read a chunked-encoded body from fd.
 *
 * Parses the sequence:
 *   <hex-len>[chunk-ext]\r\n
 *   <data>\r\n
 *   ...
 *   0\r\n\r\n
 *
 * Chunk extensions after the hex length (e.g. ";name=value") are ignored.
 *
 * Returns a malloc'd buffer containing the reassembled body, and sets
 * *out_len to its length.  Returns NULL on allocation failure or protocol
 * error.  Caller owns the returned buffer.
 */
char *http_chunked_read(int fd, size_t *out_len)
{
    size_t  buf_cap = 4096;
    size_t  buf_len = 0;
    char   *buf     = malloc(buf_cap);
    if (!buf) return NULL;

    /* Line-reading buffer: holds at most one "hex\r\n" line */
    char line[128];

    for (;;) {
        /* Read chunk-size line byte-by-byte until \r\n */
        size_t li = 0;
        for (;;) {
            if (li + 1 >= sizeof(line)) { free(buf); return NULL; }
            ssize_t n = read(fd, line + li, 1);
            if (n <= 0) { free(buf); return NULL; }
            li++;
            if (li >= 2 && line[li - 2] == '\r' && line[li - 1] == '\n')
                break;
        }
        line[li - 2] = '\0'; /* trim \r\n */

        /* Ignore chunk extensions: truncate at first ';' */
        char *semi = strchr(line, ';');
        if (semi) *semi = '\0';

        /* Parse hex length */
        size_t csz = 0;
        {
            const char *endp = line;
            while (*endp) {
                char c = *endp;
                size_t nibble;
                if      (c >= '0' && c <= '9') nibble = (size_t)(c - '0');
                else if (c >= 'a' && c <= 'f') nibble = (size_t)(c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') nibble = (size_t)(c - 'A' + 10);
                else { free(buf); return NULL; }
                csz = csz * 16u + nibble;
                endp++;
            }
        }

        if (csz == 0) {
            /* Last chunk: consume trailing \r\n */
            char tail[2];
            ssize_t n = read(fd, tail, 2);
            (void)n;
            break;
        }

        /* Grow output buffer if needed (+1 for NUL terminator) */
        if (buf_len + csz + 1 > buf_cap) {
            while (buf_cap < buf_len + csz + 1) buf_cap *= 2;
            char *tmp = realloc(buf, buf_cap);
            if (!tmp) { free(buf); return NULL; }
            buf = tmp;
        }

        /* Read csz bytes of chunk data */
        size_t rread = 0;
        while (rread < csz) {
            ssize_t n = read(fd, buf + buf_len + rread, csz - rread);
            if (n <= 0) { free(buf); return NULL; }
            rread += (size_t)n;
        }
        buf_len += csz;

        /* Consume trailing \r\n after chunk data */
        char crlf[2];
        ssize_t n = read(fd, crlf, 2);
        if (n < 2) { free(buf); return NULL; }
    }

    buf[buf_len] = '\0';
    *out_len = buf_len;
    return buf;
}

/* ── Response serialisation ─────────────────────────────────────────── */

static const char *reason(uint16_t s) {
    switch (s) {
        case 200: return "OK"; case 201: return "Created";
        case 204: return "No Content"; case 206: return "Partial Content";
        case 301: return "Moved Permanently"; case 302: return "Found";
        case 304: return "Not Modified";
        case 400: return "Bad Request"; case 403: return "Forbidden";
        case 404: return "Not Found"; case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 416: return "Range Not Satisfiable";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway"; case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "Unknown";
    }
}

/*
 * res_has_content_length — return 1 if res.headers contains a Content-Length
 * header, 0 otherwise.
 */
static int res_has_content_length(Res res)
{
    for (uint64_t i = 0; i < res.headers.len; i++) {
        if (res.headers.data[i].key &&
            strcasecmp(res.headers.data[i].key, "Content-Length") == 0)
            return 1;
    }
    return 0;
}

/*
 * send_response — serialise and send a Res to fd.
 *
 * If Res.body is non-NULL and Res.headers contains no Content-Length entry,
 * the body is sent with Transfer-Encoding: chunked (Story 27.1.4).
 * If Res.headers already carries Content-Length (set by the handler), the
 * body is sent with that fixed length instead.
 */
/* write_res_headers — emit all custom headers from res.headers to fd. */
static void write_res_headers(int fd, Res res)
{
    for (uint64_t i = 0; i < res.headers.len; i++) {
        const char *k = res.headers.data[i].key;
        const char *v = res.headers.data[i].val;
        if (!k || !v) continue;
        char line[512];
        int ln = snprintf(line, sizeof(line), "%s: %s\r\n", k, v);
        if (ln > 0) write(fd, line, (size_t)ln);
    }
}

/* write_security_headers — emit hardened security headers on every response. */
static void write_security_headers(int fd)
{
    static const char hdrs[] =
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: strict-origin-when-cross-origin\r\n";
    write(fd, hdrs, sizeof(hdrs) - 1);
}

/*
 * cors_match_origin — find the request Origin in the allowed list.
 * g_cors_origins is a comma-separated string like "http://localhost:3000,http://example.com".
 * Returns the matched origin (pointer into g_cors_origins) or NULL.
 * The browser expects exactly one matching origin, not the whole list.
 */
static const char *cors_match_origin(const char *request_origin)
{
    if (!g_cors_origins || !request_origin) return NULL;
    if (strcmp(g_cors_origins, "*") == 0) return "*";
    size_t orig_len = strlen(request_origin);
    const char *p = g_cors_origins;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',') p++;
        size_t entry_len = (size_t)(p - start);
        while (entry_len > 0 && start[entry_len-1] == ' ') entry_len--;
        if (entry_len == orig_len && strncmp(start, request_origin, entry_len) == 0)
            return request_origin;
    }
    return NULL;
}

/* write_cors_headers — emit CORS headers when configured (Story 7.7.4).
 * Only sends back the matching origin, not the full allowed list. */
static void write_cors_headers_for(int fd, const char *matched_origin)
{
    if (!matched_origin) return;
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "Access-Control-Allow-Origin: %s\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, PATCH, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Vary: Origin\r\n",
        matched_origin);
    if (n > 0) write(fd, buf, (size_t)n);
}
static void write_cors_headers(int fd) { write_cors_headers_for(fd, g_cors_matched); }

static void send_response(int fd, Res res, int keep_alive) {
    const char *conn_hdr = keep_alive ? "keep-alive" : "close";

    if (res.body != NULL && !res_has_content_length(res)) {
        /* Chunked path */
        char hdr[256];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %u %s\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: %s\r\n",
            res.status, reason(res.status), conn_hdr);
        write(fd, hdr, (size_t)hl);
        write_res_headers(fd, res);
        write_security_headers(fd);
        write_cors_headers(fd);
        write(fd, "\r\n", 2);
        /* Default chunk size: 4096 bytes */
        http_chunked_write(fd, res.body, strlen(res.body), 4096);
    } else {
        /* Fixed Content-Length path (body may be NULL for empty responses) */
        size_t bl = res.body ? strlen(res.body) : 0;
        char hdr[256];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %u %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n",
            res.status, reason(res.status), bl, conn_hdr);
        write(fd, hdr, (size_t)hl);
        write_res_headers(fd, res);
        write_security_headers(fd);
        write_cors_headers(fd);
        write(fd, "\r\n", 2);
        if (bl) write(fd, res.body, bl);
    }
}

/* ── Keep-alive connection constants ────────────────────────────────── */

#define KEEPALIVE_IDLE_TIMEOUT_S 2
#define KEEPALIVE_MAX_REQUESTS   1000

/* ── Access log helpers ──────────────────────────────────────────────── */

/*
 * fd_peer_ip — write the remote IP of fd into buf (at least INET6_ADDRSTRLEN).
 * Falls back to "-" on any error.
 */
static void fd_peer_ip(int fd, char *buf, size_t bufsz)
{
    struct sockaddr_storage ss;
    socklen_t sslen = sizeof ss;
    if (getpeername(fd, (struct sockaddr *)&ss, &sslen) != 0) {
        snprintf(buf, bufsz, "-"); return;
    }
    if (ss.ss_family == AF_INET)
        inet_ntop(AF_INET,  &((struct sockaddr_in  *)&ss)->sin_addr,  buf, (socklen_t)bufsz);
    else if (ss.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, buf, (socklen_t)bufsz);
    else
        snprintf(buf, bufsz, "-");
}

/*
 * log_request — write one Combined Log Format line for the given request/response.
 * No-op when no global access log is configured.
 * Also writes to the error log when status >= 400 (Story 47.1.2).
 */
static void log_request(const char *client_ip, const Req *req,
                        uint16_t status, size_t body_len)
{
    TkAccessLog *al = tk_access_log_get_global();
    TkAccessLog *el = tk_error_log_get_global();
    if (!al && !el) return;

    const char *referer = "-", *ua = "-";
    for (uint64_t i = 0; i < req->headers.len; i++) {
        if (req->headers.data[i].key) {
            if (strcasecmp(req->headers.data[i].key, "referer")    == 0) referer = req->headers.data[i].val;
            if (strcasecmp(req->headers.data[i].key, "user-agent") == 0) ua      = req->headers.data[i].val;
        }
    }

    const char *method = req->method ? req->method : "-";
    const char *path   = req->path   ? req->path   : "-";

    if (al) {
        tk_access_log_write(al, client_ip, method, path,
                            (int)status, body_len, referer, ua);
    }
    /* 4xx/5xx → also write to the error log */
    if (el && status >= 400) {
        tk_error_log_write(el, client_ip, method, path,
                           (int)status, body_len, referer, ua);
    }
}

/* ── Connection handler (single accepted socket) ────────────────────── */

/*
 * req_wants_close — return 1 if the request signals Connection: close,
 * 0 if it signals keep-alive (or is silent, defaulting to keep-alive
 * for HTTP/1.1).
 */
static int req_wants_close(const char *raw)
{
    /* Walk header lines looking for Connection: */
    const char *p = strstr(raw, "\r\n");
    while (p) {
        p += 2; /* skip \r\n */
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol == p) break; /* blank line = end of headers */
        if (strncasecmp(p, "Connection:", 11) == 0) {
            const char *v = p + 11;
            while (*v == ' ') v++;
            if (strncasecmp(v, "close", 5) == 0) return 1;
            return 0; /* "keep-alive" or anything else */
        }
        p = eol;
    }
    return 0; /* HTTP/1.1 default: keep-alive */
}

/*
 * send_error — write a minimal error response and close.
 * Used for limit violations detected before a full Req is parsed.
 * Also writes to the global error log when configured (Story 47.1.2).
 */
static void send_error(int fd, uint16_t status, const char *client_ip)
{
    Res r = make_res(status, reason(status));
    send_response(fd, r, 0 /* Connection: close */);

    /* Write to error log if configured */
    TkAccessLog *el = tk_error_log_get_global();
    if (el) {
        tk_error_log_write(el, client_ip ? client_ip : "-",
                           "-", "-", (int)status, 0, "-", "-");
    }
}

/* ── Per-IP rate limiting (Story 59.4.4) ──────────────────────────────── */

#define RATE_LIMIT_BUCKETS 1024
#define RATE_LIMIT_WINDOW  60     /* seconds */
#define RATE_LIMIT_MAX     200    /* requests per window per IP */

typedef struct {
    char     ip[64];
    uint32_t count;
    time_t   window_start;
} RateBucket;

static RateBucket g_rate_table[RATE_LIMIT_BUCKETS];

static uint32_t rate_hash(const char *ip)
{
    uint32_t h = 2166136261u;
    for (const char *p = ip; *p; p++)
        h = (h ^ (uint8_t)*p) * 16777619u;
    return h % RATE_LIMIT_BUCKETS;
}

/* rate_check — return 1 if request is allowed, 0 if rate-limited */
static int rate_check(const char *ip)
{
    uint32_t idx = rate_hash(ip);
    RateBucket *b = &g_rate_table[idx];
    time_t now = time(NULL);

    if (strcmp(b->ip, ip) != 0 || (now - b->window_start) >= RATE_LIMIT_WINDOW) {
        /* New IP in this bucket or window expired: reset */
        strncpy(b->ip, ip, sizeof(b->ip) - 1);
        b->ip[sizeof(b->ip) - 1] = '\0';
        b->count = 1;
        b->window_start = now;
        return 1;
    }

    b->count++;
    return b->count <= RATE_LIMIT_MAX;
}

#ifdef TK_HAVE_OPENSSL
/* Forward declaration for h2c upgrade path (60.1.7) */
static void handle_h2_connection(int fd, void *ssl, const char *client_ip);
#endif

static void handle_connection(int fd)
{
    char client_ip[64];
    fd_peer_ip(fd, client_ip, sizeof client_ip);

    /* Rate limiting: reject with 429 if threshold exceeded */
    if (!rate_check(client_ip)) {
        send_error(fd, 429, client_ip);
        close(fd);
        return;
    }

    /* Apply the (possibly user-overridden) receive timeout. */
    struct timeval tv;
    tv.tv_sec  = (long)srv_limits.timeout_secs;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /*
     * Allocate a per-connection read buffer large enough for the maximum
     * allowed header block plus the maximum allowed body, plus a null byte.
     */
    size_t buf_cap = (size_t)srv_limits.max_header
                   + (size_t)srv_limits.max_body + 8;
    char *raw = malloc(buf_cap);
    if (!raw) { close(fd); return; }

    int requests = 0;

    for (;;) {
        memset(raw, 0, buf_cap);

        /* Use a short timeout while waiting for keep-alive requests
         * so idle connections don't block workers from accepting new ones */
        if (requests > 0) {
            struct timeval ka_tv;
            ka_tv.tv_sec  = KEEPALIVE_IDLE_TIMEOUT_S;
            ka_tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ka_tv, sizeof(ka_tv));
        }

        /* ── Phase 1: read headers, enforcing max_header ── */
        size_t hdr_used = 0;
        int    hdr_ok   = 0; /* set to 1 when \r\n\r\n is found */

        while (hdr_used < (size_t)srv_limits.max_header) {
            size_t space = (size_t)srv_limits.max_header - hdr_used;
            ssize_t n = read(fd, raw + hdr_used, space);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    /* Keep-alive idle timeout: silently close, not an error */
                    if (requests > 0) { free(raw); close(fd); return; }
                    send_error(fd, 408, client_ip);
                }
                free(raw); close(fd); return;
            }
            /* Restore full timeout once data is flowing */
            if (hdr_used == 0 && requests > 0) {
                struct timeval full_tv;
                full_tv.tv_sec  = (long)srv_limits.timeout_secs;
                full_tv.tv_usec = 0;
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &full_tv, sizeof(full_tv));
            }
            hdr_used += (size_t)n;
            raw[hdr_used] = '\0';
            if (strstr(raw, "\r\n\r\n")) { hdr_ok = 1; break; }
        }

        if (!hdr_ok) {
            /* Filled max_header bytes without finding end-of-headers */
            send_error(fd, 431, client_ip);
            free(raw); close(fd); return;
        }

        /* ── Phase 2: enforce Content-Length limit ── */
        const char *cl_hdr = NULL;
        {
            char *p = raw;
            while (p && (size_t)(p - raw) < hdr_used) {
                if (strncasecmp(p, "Content-Length:", 15) == 0) {
                    cl_hdr = p + 15;
                    break;
                }
                p = strstr(p, "\r\n");
                if (p) p += 2;
            }
        }

        /* ── Phase 2b: detect Transfer-Encoding: chunked ── */
        int req_is_chunked = 0;
        {
            const char *p2 = raw;
            while (p2 && (size_t)(p2 - raw) < hdr_used) {
                if (strncasecmp(p2, "Transfer-Encoding:", 18) == 0) {
                    const char *v = p2 + 18;
                    while (*v == ' ') v++;
                    if (strncasecmp(v, "chunked", 7) == 0)
                        req_is_chunked = 1;
                    break;
                }
                p2 = strstr(p2, "\r\n");
                if (p2) p2 += 2;
            }
        }

        /* Reject ambiguous CL + TE: chunked (request-smuggling defence) */
        if (cl_hdr && req_is_chunked) {
            send_error(fd, 400, client_ip);
            free(raw); close(fd); return;
        }

        if (cl_hdr && !req_is_chunked) {
            long long cl = atoll(cl_hdr);
            if (cl < 0 ||
                (unsigned long long)cl >
                (unsigned long long)srv_limits.max_body) {
                send_error(fd, 413, client_ip);
                free(raw); close(fd); return;
            }

            /* ── Phase 3: read remaining body bytes ── */
            const char *sep = strstr(raw, "\r\n\r\n");
            if (sep) {
                size_t hlen      = (size_t)(sep - raw) + 4;
                size_t body_have = hdr_used - hlen;
                size_t content_len = (size_t)cl;

                while (body_have < content_len) {
                    size_t need  = content_len - body_have;
                    size_t space = buf_cap - hdr_used - 1;
                    if (space == 0) break;
                    ssize_t n = read(fd, raw + hdr_used,
                                     need < space ? need : space);
                    if (n <= 0) {
                        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                            send_error(fd, 408, client_ip);
                        free(raw); close(fd); return;
                    }
                    hdr_used   += (size_t)n;
                    body_have  += (size_t)n;
                    raw[hdr_used] = '\0';
                }
            }
        }

        requests++;

        int client_wants_close = req_wants_close(raw);
        int max_reached        = (requests >= KEEPALIVE_MAX_REQUESTS);
        int keep_alive         = (!client_wants_close && !max_reached);

        Req req = parse_request(raw);

        /* ── h2c cleartext upgrade (60.1.7) ── */
#ifdef TK_HAVE_OPENSSL
        {
            const char *upgrade = NULL;
            for (uint64_t i = 0; i < req.headers.len; i++) {
                if (strcasecmp(req.headers.data[i].key, "Upgrade") == 0) {
                    upgrade = req.headers.data[i].val;
                    break;
                }
            }
            if (upgrade && strcasecmp(upgrade, "h2c") == 0) {
                const char *resp101 =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: h2c\r\n\r\n";
                write(fd, resp101, strlen(resp101));
                handle_h2_connection(fd, NULL, client_ip);
                free(raw);
                close(fd);
                return;
            }
        }
#endif

        /* ── Phase 4: reassemble chunked request body ── */
        if (req_is_chunked) {
            size_t decoded_len = 0;
            char  *decoded     = http_chunked_read(fd, &decoded_len);
            if (decoded) {
                /* Replace the parsed body with the decoded one */
                free((void *)req.body);
                req.body = decoded;
            }
        }

        /* ── CORS: match request Origin against allowed list ── */
        g_cors_matched = NULL;
        if (g_cors_origins) {
            for (uint64_t ci = 0; ci < req.headers.len; ci++) {
                if (req.headers.data[ci].key &&
                    strcasecmp(req.headers.data[ci].key, "Origin") == 0) {
                    g_cors_matched = cors_match_origin(req.headers.data[ci].val);
                    break;
                }
            }
        }

        /* ── CORS preflight: OPTIONS → 204 with CORS headers (Story 7.7.4) ── */
        if (req.method && strcmp(req.method, "OPTIONS") == 0 && g_cors_matched) {
            Res res;
            res.status       = 204;
            res.body         = NULL;
            res.headers.data = NULL;
            res.headers.len  = 0;
            send_response(fd, res, keep_alive);
            log_request(client_ip, &req, res.status, 0);
            if (!keep_alive) break;
            continue;
        }

        /* ── Reject disallowed methods early ── */
        int is_head = (req.method && strcmp(req.method, "HEAD") == 0);
        if (req.method &&
            strcmp(req.method, "GET")    != 0 &&
            strcmp(req.method, "POST")   != 0 &&
            strcmp(req.method, "PUT")    != 0 &&
            strcmp(req.method, "DELETE") != 0 &&
            strcmp(req.method, "PATCH")  != 0 &&
            strcmp(req.method, "OPTIONS") != 0 &&
            !is_head) {
            Res res = make_res(405, "Method Not Allowed");
            send_response(fd, res, keep_alive);
            log_request(client_ip, &req, res.status, 0);
            if (!keep_alive) break;
            continue;
        }

        /* ── Route dispatch (HEAD falls back to GET handler) ── */
        Res res = make_res(404, "Not Found");
        StrPair params[32]; int pc = 0;
        int path_matched = 0;
        int handler_called = 0;
        for (int i = 0; i < route_count; i++) {
            if (match_pattern(route_table[i].pattern,
                              req.path ? req.path : "", params, &pc)) {
                path_matched = 1;
                const char *check_method = is_head ? "GET" : req.method;
                if (check_method &&
                    strcmp(route_table[i].method, check_method) == 0) {
                    req.params.data = params; req.params.len = (uint64_t)pc;
                    res = route_table[i].h(req);
                    handler_called = 1;
                    if (is_head) {
                        /* HEAD: send headers only, suppress body */
                        res.body = NULL;
                    }
                    break;
                }
                pc = 0; /* reset params for next pattern attempt */
            }
        }
        /* Path existed but no method matched → 405 (only if handler wasn't called) */
        if (path_matched && !handler_called && res.status == 404) {
            res = make_res(405, "Method Not Allowed");
        }

        /* Gzip compress if eligible — write directly (binary-safe) */
        char  *gz_buf = NULL;
        size_t gz_len = 0;
        int    did_gz = maybe_gzip_body(req, &res, &gz_buf, &gz_len);
        if (did_gz) {
            const char *conn_hdr = keep_alive ? "keep-alive" : "close";
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 %u %s\r\n"
                "Content-Length: %zu\r\n"
                "Content-Encoding: gzip\r\n"
                "Vary: Accept-Encoding\r\n"
                "Connection: %s\r\n",
                res.status, reason(res.status), gz_len, conn_hdr);
            write(fd, hdr, (size_t)hl);
            write_res_headers(fd, res);
            write_security_headers(fd);
            write_cors_headers(fd);
            write(fd, "\r\n", 2);
            write(fd, gz_buf, gz_len);
            log_request(client_ip, &req, res.status, gz_len);
            free(gz_buf);
        } else {
            send_response(fd, res, keep_alive);
            log_request(client_ip, &req,
                        res.status, res.body ? strlen(res.body) : 0);
        }

        if (!keep_alive) break;
    }

    free(raw);
    close(fd);
}

/* ── Server ─────────────────────────────────────────────────────────── */

static void serve_sighandler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

int http_serve(uint16_t port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0); if (srv < 0) return -1;
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){close(srv);return -1;}
    if (listen(srv, 8)<0){close(srv);return -1;}

    signal(SIGTERM, serve_sighandler);
    signal(SIGINT,  serve_sighandler);

    for (;;) {
        int fd = accept(srv, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                if (g_shutdown_requested) break;
                continue;
            }
            continue;
        }
        if (g_shutdown_requested) { close(fd); break; }
        handle_connection(fd);
    }
    close(srv); return 0;
}

/* ── http_handle_fd — test / embedding hook (Story 27.1.9) ─────────── */

void http_handle_fd(int fd)
{
    handle_connection(fd);
}

/* ── Pre-fork worker pool (Story 27.1.1) ────────────────────────────── */

/*
 * TkHttpRouter — snapshot of the global route table captured at
 * http_router_new() call time.  Each worker gets its own copy via fork
 * COW; there is no shared mutable state between workers.
 */
struct TkHttpRouter {
    Route routes[MAX_ROUTES];
    int   count;
};

TkHttpRouter *http_router_new(void)
{
    TkHttpRouter *r = malloc(sizeof(TkHttpRouter));
    if (!r) return NULL;
    r->count = route_count;
    if (r->count > MAX_ROUTES) r->count = MAX_ROUTES;
    memcpy(r->routes, route_table, (size_t)r->count * sizeof(Route));
    return r;
}

void http_router_free(TkHttpRouter *r)
{
    free(r);
}

/* g_worker_pids — written before any fork(), read-only in parent after. */
#define TK_MAX_WORKERS 256

static pid_t    g_worker_pids[TK_MAX_WORKERS];
static uint64_t g_nworkers_global = 0;

static volatile sig_atomic_t g_reload_requested = 0;

static void workers_parent_sighandler(int sig)
{
    if (sig == SIGHUP)
        g_reload_requested = 1;
    else
        g_shutdown_requested = 1;
}

/* bind_listen — create, bind, and listen a TCP socket.
 * host == NULL means INADDR_ANY.  Returns fd or -1 on error. */
static int bind_listen(const char *host, uint64_t port)
{
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return -1;

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(srv, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (host == NULL || host[0] == '\0') {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            close(srv); return -1;
        }
    }

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv); return -1;
    }
    if (listen(srv, 128) < 0) {
        close(srv); return -1;
    }
    return srv;
}

/* worker_loop — accept and handle connections; exits when shutdown is set. */
static void worker_loop(int srv_fd, TkHttpRouter *r)
{
    route_count = r->count;
    memcpy(route_table, r->routes, (size_t)r->count * sizeof(Route));

    for (;;) {
        int fd = accept(srv_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                if (g_shutdown_requested) break;
                continue;
            }
            continue;
        }
        if (g_shutdown_requested) { close(fd); break; }
        handle_connection(fd);
    }
}

TkHttpErr http_serve_workers(TkHttpRouter *r, const char *host,
                              uint64_t port, uint64_t nworkers)
{
    if (!r) return TK_HTTP_ERR_SOCKET;

    /* nworkers == 0: auto-detect CPU count (Story 49.6.5) */
    if (nworkers == 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        nworkers = (ncpu > 1) ? (uint64_t)ncpu : 1;
        const char *env_w = getenv("TK_HTTP_WORKERS");
        if (env_w) { long v = atol(env_w); if (v > 0) nworkers = (uint64_t)v; }
    }

    /* nworkers == 1: single-process path, no fork */
    if (nworkers <= 1) {
        int srv = bind_listen(host, port);
        if (srv < 0) return TK_HTTP_ERR_BIND;
        worker_loop(srv, r); /* does not return */
        close(srv);
        return TK_HTTP_OK;
    }

    /* Multi-worker pre-fork path */
    if (nworkers > TK_MAX_WORKERS) nworkers = TK_MAX_WORKERS;

    int srv = bind_listen(host, port);
    if (srv < 0) return TK_HTTP_ERR_BIND;

    g_nworkers_global = nworkers;
    signal(SIGTERM, workers_parent_sighandler);
    signal(SIGINT,  workers_parent_sighandler);
    signal(SIGHUP,  workers_parent_sighandler);

    for (uint64_t i = 0; i < nworkers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            for (uint64_t j = 0; j < i; j++)
                kill(g_worker_pids[j], SIGTERM);
            close(srv);
            return TK_HTTP_ERR_FORK;
        }
        if (pid == 0) {
            worker_loop(srv, r);
            _exit(0);
        }
        g_worker_pids[i] = pid;
    }

    /* Parent keeps srv open so respawned workers can inherit it. */

    /* Parent: supervise, restart workers that die abnormally */
    for (;;) {
        int   wstatus = 0;
        pid_t died    = waitpid(-1, &wstatus, WNOHANG);

        if (died < 0) {
            if (errno == EINTR) {
                /* Check shutdown flag set by signal handler */
            } else {
                break;
            }
        }

        /* ── SIGHUP graceful reload (Story 59.4.8) ── */
        if (g_reload_requested) {
            g_reload_requested = 0;
            /* Send SIGTERM to old workers, then fork fresh ones */
            for (uint64_t i = 0; i < nworkers; i++) {
                if (g_worker_pids[i] > 0)
                    kill(g_worker_pids[i], SIGTERM);
            }
            /* Wait for old workers to drain (up to drain timeout) */
            time_t rd = time(NULL) + HTTP_DRAIN_TIMEOUT_SECS;
            for (;;) {
                int all = 1;
                for (uint64_t i = 0; i < nworkers; i++) {
                    if (g_worker_pids[i] <= 0) continue;
                    int ws2 = 0;
                    pid_t p2 = waitpid(g_worker_pids[i], &ws2, WNOHANG);
                    if (p2 == g_worker_pids[i])
                        g_worker_pids[i] = -1;
                    else
                        all = 0;
                }
                if (all) break;
                if (time(NULL) >= rd) {
                    for (uint64_t i = 0; i < nworkers; i++) {
                        if (g_worker_pids[i] > 0) {
                            kill(g_worker_pids[i], SIGKILL);
                            waitpid(g_worker_pids[i], NULL, 0);
                            g_worker_pids[i] = -1;
                        }
                    }
                    break;
                }
                struct timespec rts;
                rts.tv_sec = 0; rts.tv_nsec = 10000000;
                nanosleep(&rts, NULL);
            }
            /* Fork new workers */
            for (uint64_t i = 0; i < nworkers; i++) {
                pid_t pid = fork();
                if (pid < 0) { g_worker_pids[i] = -1; continue; }
                if (pid == 0) { worker_loop(srv, r); _exit(0); }
                g_worker_pids[i] = pid;
            }
            continue;
        }

        if (g_shutdown_requested) {
            /* Send SIGTERM to all live workers */
            for (uint64_t i = 0; i < nworkers; i++) {
                if (g_worker_pids[i] > 0)
                    kill(g_worker_pids[i], SIGTERM);
            }

            /* Wait up to HTTP_DRAIN_TIMEOUT_SECS for workers to exit */
            time_t deadline = time(NULL) + HTTP_DRAIN_TIMEOUT_SECS;
            for (;;) {
                int all_done = 1;
                for (uint64_t i = 0; i < nworkers; i++) {
                    if (g_worker_pids[i] <= 0) continue;
                    int ws = 0;
                    pid_t p = waitpid(g_worker_pids[i], &ws, WNOHANG);
                    if (p == g_worker_pids[i])
                        g_worker_pids[i] = -1;
                    else
                        all_done = 0;
                }
                if (all_done) break;
                if (time(NULL) >= deadline) {
                    /* Drain timeout: SIGKILL remaining workers */
                    for (uint64_t i = 0; i < nworkers; i++) {
                        if (g_worker_pids[i] > 0) {
                            kill(g_worker_pids[i], SIGKILL);
                            waitpid(g_worker_pids[i], NULL, 0);
                            g_worker_pids[i] = -1;
                        }
                    }
                    break;
                }
                /* Brief yield to avoid busy-spin */
                {
                    struct timespec ts;
                    ts.tv_sec  = 0;
                    ts.tv_nsec = 10000000; /* 10 ms */
                    nanosleep(&ts, NULL);
                }
            }
            break; /* exit supervision loop */
        }

        if (died <= 0) {
            /* WNOHANG: no child exited yet; yield and retry */
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = 10000000; /* 10 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        /* A worker exited — record and optionally restart */
        uint64_t slot = nworkers; /* sentinel: not found */
        for (uint64_t i = 0; i < nworkers; i++) {
            if (g_worker_pids[i] == died) { slot = i; break; }
        }
        if (slot >= nworkers) continue;

        if (WIFSIGNALED(wstatus) ||
            (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0)) {
            /* Respawn worker using the shared listen socket */
            pid_t pid = fork();
            if (pid < 0) {
                g_worker_pids[slot] = -1;
                continue;
            }
            if (pid == 0) {
                worker_loop(srv, r);
                _exit(0);
            }
            g_worker_pids[slot] = pid;
        } else {
            g_worker_pids[slot] = -1;
        }
    }

    close(srv);
    return TK_HTTP_OK;
}

/* ── Cookie support (Story 27.1.7) ──────────────────────────────────── */

/* url_decode_char — decode %XX escape; returns decoded byte or -1 on error. */
static int url_decode_char(const char *p)
{
    int hi, lo;
    if (!p[0] || !p[1]) return -1;
    hi = (p[0] >= '0' && p[0] <= '9') ? (p[0] - '0')
       : (p[0] >= 'A' && p[0] <= 'F') ? (p[0] - 'A' + 10)
       : (p[0] >= 'a' && p[0] <= 'f') ? (p[0] - 'a' + 10)
       : -1;
    lo = (p[1] >= '0' && p[1] <= '9') ? (p[1] - '0')
       : (p[1] >= 'A' && p[1] <= 'F') ? (p[1] - 'A' + 10)
       : (p[1] >= 'a' && p[1] <= 'f') ? (p[1] - 'a' + 10)
       : -1;
    if (hi < 0 || lo < 0) return -1;
    return (hi << 4) | lo;
}

/* url_decode — decode percent-encoded cookie value; caller owns result. */
static char *url_decode(const char *s, size_t len)
{
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t wi = 0;
    for (size_t ri = 0; ri < len; ri++) {
        if (s[ri] == '%' && ri + 2 < len) {
            int c = url_decode_char(s + ri + 1);
            if (c > 0) { out[wi++] = (char)c; ri += 2; continue; }
            if (c == 0) { ri += 2; continue; } /* reject %00 null bytes */
        }
        out[wi++] = s[ri];
    }
    out[wi] = '\0';
    return out;
}

const char *http_cookie(const char *cookie_header, const char *name)
{
    if (!cookie_header || !name) return NULL;
    size_t namelen = strlen(name);
    const char *p = cookie_header;

    while (*p) {
        /* skip leading whitespace */
        while (*p == ' ' || *p == '\t') p++;

        /* find '=' separating name from value */
        const char *eq = strchr(p, '=');
        if (!eq) break;

        /* trim trailing whitespace from key */
        const char *key_end = eq;
        while (key_end > p &&
               (*(key_end - 1) == ' ' || *(key_end - 1) == '\t'))
            key_end--;
        size_t klen = (size_t)(key_end - p);

        const char *val_start = eq + 1;
        /* skip leading whitespace in value */
        while (*val_start == ' ' || *val_start == '\t') val_start++;

        /* value ends at ';' or end of string */
        const char *semi = strchr(val_start, ';');
        size_t vlen = semi ? (size_t)(semi - val_start) : strlen(val_start);

        /* trim trailing whitespace from value */
        while (vlen > 0 && (val_start[vlen - 1] == ' ' ||
                             val_start[vlen - 1] == '\t'))
            vlen--;

        if (klen == namelen && memcmp(p, name, klen) == 0)
            return url_decode(val_start, vlen);

        /* advance past this pair */
        if (semi) p = semi + 1;
        else break;
    }
    return NULL;
}

const char *http_set_cookie_header(const char *name, const char *value,
                                    TkCookieOpts opts)
{
    if (!name || !value) return NULL;

    /* estimate buffer: name=value + all optional attributes */
    size_t cap = strlen(name) + strlen(value) + 256;
    if (opts.path)      cap += strlen(opts.path)      + 8;
    if (opts.domain)    cap += strlen(opts.domain)    + 10;
    if (opts.same_site) cap += strlen(opts.same_site) + 12;

    char *buf = malloc(cap);
    if (!buf) return NULL;

    int n = snprintf(buf, cap, "%s=%s", name, value);

    if (opts.path)
        n += snprintf(buf + n, cap - (size_t)n, "; Path=%s", opts.path);

    if (opts.domain)
        n += snprintf(buf + n, cap - (size_t)n, "; Domain=%s", opts.domain);

    if (opts.max_age >= 0)
        n += snprintf(buf + n, cap - (size_t)n,
                      "; Max-Age=%lld", (long long)opts.max_age);

    if (opts.secure)
        n += snprintf(buf + n, cap - (size_t)n, "; Secure");

    if (opts.http_only)
        n += snprintf(buf + n, cap - (size_t)n, "; HttpOnly");

    if (opts.same_site)
        n += snprintf(buf + n, cap - (size_t)n,
                      "; SameSite=%s", opts.same_site);

    (void)n;
    return buf;
}

/* ── URL-encoded form body parsing (Story 27.1.14) ──────────────────── */

TkFormResult http_form_parse(const char *body)
{
    TkFormResult result;
    result.fields  = NULL;
    result.nfields = 0;

    if (!body) return result;

    size_t cap = 16;
    TkFormField *fields = malloc(cap * sizeof(TkFormField));
    if (!fields) return result;

    const char *p = body;

    while (*p != '\0') {
        /* Find end of this pair: '&' or ';' or end-of-string */
        const char *amp = p;
        while (*amp != '\0' && *amp != '&' && *amp != ';') amp++;

        size_t pair_len = (size_t)(amp - p);

        /* Split on first '=' */
        const char *eq = p;
        while (eq < amp && *eq != '=') eq++;

        /* Build NUL-terminated raw key and value strings */
        size_t key_raw_len = (size_t)(eq - p);
        char *key_raw = malloc(key_raw_len + 1);
        if (!key_raw) break;
        memcpy(key_raw, p, key_raw_len);
        key_raw[key_raw_len] = '\0';

        const char *val_start = (eq < amp) ? eq + 1 : eq;
        size_t val_raw_len = (size_t)(amp - val_start);
        char *val_raw = malloc(val_raw_len + 1);
        if (!val_raw) { free(key_raw); break; }
        memcpy(val_raw, val_start, val_raw_len);
        val_raw[val_raw_len] = '\0';

        /* URL-decode both key and value */
        const char *key_dec = encoding_urldecode(key_raw);
        const char *val_dec = encoding_urldecode(val_raw);
        free(key_raw);
        free(val_raw);

        if (!key_dec || !val_dec) {
            free((void *)(uintptr_t)key_dec);
            free((void *)(uintptr_t)val_dec);
            /* skip malformed pair */
            p = (*amp != '\0') ? amp + 1 : amp;
            continue;
        }

        /* Grow array if needed */
        if (result.nfields >= cap) {
            size_t new_cap = cap * 2;
            TkFormField *tmp = realloc(fields, new_cap * sizeof(TkFormField));
            if (!tmp) {
                free((void *)(uintptr_t)key_dec);
                free((void *)(uintptr_t)val_dec);
                break;
            }
            fields = tmp;
            cap = new_cap;
        }

        fields[result.nfields].key   = key_dec;
        fields[result.nfields].value = val_dec;
        result.nfields++;

        /* Advance past separator */
        p = (*amp != '\0') ? amp + 1 : amp;

        (void)pair_len; /* suppress unused-variable warning */
    }

    result.fields = fields;
    return result;
}

const char *http_form_get(TkFormResult form, const char *key)
{
    if (!key) return NULL;
    for (size_t i = 0; i < form.nfields; i++) {
        if (form.fields[i].key && strcmp(form.fields[i].key, key) == 0)
            return form.fields[i].value;
    }
    return NULL;
}

void http_form_free(TkFormResult form)
{
    for (size_t i = 0; i < form.nfields; i++) {
        free((void *)(uintptr_t)form.fields[i].key);
        free((void *)(uintptr_t)form.fields[i].value);
    }
    free(form.fields);
}

/* ── Multipart/form-data parsing (Story 27.1.8) ─────────────────────── */

#define MP_MAX_BODY_BYTES  (50UL * 1024UL * 1024UL)   /* 50 MiB */
#define MP_MAX_PART_BYTES  (10UL * 1024UL * 1024UL)   /* 10 MiB */

/*
 * mp_find — memmem-style search without POSIX extension dependency.
 */
static const char *mp_find(const char *hay, size_t hlen,
                            const char *needle, size_t nlen)
{
    if (nlen == 0 || hlen < nlen) return NULL;
    for (size_t i = 0; i <= hlen - nlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

static char *mp_strdup_n(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

static const char *mp_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* mp_parse_cd_param — extract param="value" from Content-Disposition line. */
static char *mp_parse_cd_param(const char *hdr, const char *param)
{
    size_t plen = strlen(param);
    const char *p = hdr;
    while (*p) {
        p = strchr(p, ';');
        if (!p) break;
        p++;
        p = mp_skip_ws(p);
        if (strncasecmp(p, param, plen) == 0 && p[plen] == '=') {
            p += plen + 1;
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) return mp_strdup_n(p, strlen(p));
                return mp_strdup_n(p, (size_t)(end - p));
            }
            const char *end = p;
            while (*end && *end != ';' && *end != ' ' && *end != '\t')
                end++;
            return mp_strdup_n(p, (size_t)(end - p));
        }
    }
    return NULL;
}

/* mp_parse_headers — extract name/filename/content-type from part headers. */
static int mp_parse_headers(const char *hdr_block, size_t hdr_len,
                             char **name_out, char **filename_out,
                             char **content_type_out)
{
    *name_out         = NULL;
    *filename_out     = NULL;
    *content_type_out = NULL;

    const char *p   = hdr_block;
    const char *end = hdr_block + hdr_len;

    while (p < end) {
        const char *eol     = mp_find(p, (size_t)(end - p), "\r\n", 2);
        size_t      linelen = eol ? (size_t)(eol - p) : (size_t)(end - p);
        if (linelen == 0) break;

        if (strncasecmp(p, "Content-Disposition:", 20) == 0) {
            char *line = mp_strdup_n(p, linelen);
            if (line) {
                *name_out     = mp_parse_cd_param(line, "name");
                *filename_out = mp_parse_cd_param(line, "filename");
                free(line);
            }
        } else if (strncasecmp(p, "Content-Type:", 13) == 0) {
            const char *v    = mp_skip_ws(p + 13);
            const char *vend = p + linelen;
            while (vend > v && (*(vend-1) == ' ' || *(vend-1) == '\t'))
                vend--;
            *content_type_out = mp_strdup_n(v, (size_t)(vend - v));
        }

        if (!eol) break;
        p = eol + 2;
    }

    if (!*name_out) return -1;
    return 0;
}

const char *http_multipart_boundary(const char *content_type)
{
    if (!content_type) return NULL;
    const char *p = content_type;
    while (*p) {
        if (strncasecmp(p, "boundary=", 9) == 0) {
            p += 9;
            if (*p == '"') {
                p++;
                const char *end = strchr(p, '"');
                if (!end) return mp_strdup_n(p, strlen(p));
                return mp_strdup_n(p, (size_t)(end - p));
            }
            const char *end = p;
            while (*end && *end != ';' && *end != ' ' && *end != '\t')
                end++;
            return mp_strdup_n(p, (size_t)(end - p));
        }
        p++;
    }
    return NULL;
}

TkMultipartResult http_multipart_parse(const char *boundary,
                                        const char *body, size_t body_len)
{
    TkMultipartResult result;
    result.parts  = NULL;
    result.nparts = 0;

    if (!boundary || !body || body_len == 0 || body_len > MP_MAX_BODY_BYTES)
        return result;

    size_t blen = strlen(boundary);
    if (blen == 0) return result;

    /* Build delimiter: "--" + boundary */
    size_t delim_len = blen + 2;
    char *delim = malloc(delim_len + 1);
    if (!delim) return result;
    memcpy(delim, "--", 2);
    memcpy(delim + 2, boundary, blen);
    delim[delim_len] = '\0';

    /* Build final delimiter: "--" + boundary + "--" */
    size_t final_len = delim_len + 2;
    char *final_delim = malloc(final_len + 1);
    if (!final_delim) { free(delim); return result; }
    memcpy(final_delim, delim, delim_len);
    memcpy(final_delim + delim_len, "--", 2);
    final_delim[final_len] = '\0';

    size_t cap = 64;
    TkMultipart *parts = malloc(cap * sizeof(TkMultipart));
    if (!parts) { free(delim); free(final_delim); return result; }

    /* Find and skip the opening boundary line */
    const char *pos = mp_find(body, body_len, delim, delim_len);
    if (!pos) { free(delim); free(final_delim); free(parts); return result; }

    pos += delim_len;
    {
        size_t used = (size_t)(pos - body);
        size_t left = body_len - used;
        if (left >= 2 && pos[0] == '\r' && pos[1] == '\n') {
            pos += 2;
        } else if (left >= 1 && pos[0] == '\n') {
            pos += 1;
        }
    }

    for (;;) {
        size_t rem = body_len - (size_t)(pos - body);
        if (rem == 0) break;

        if (rem >= final_len && memcmp(pos, final_delim, final_len) == 0)
            break;

        const char *hdr_end = mp_find(pos, rem, "\r\n\r\n", 4);
        if (!hdr_end) break;

        size_t hdr_len2 = (size_t)(hdr_end - pos);
        char *name         = NULL;
        char *filename     = NULL;
        char *content_type = NULL;

        if (mp_parse_headers(pos, hdr_len2,
                             &name, &filename, &content_type) != 0) {
            free(name); free(filename); free(content_type);
            const char *next = mp_find(hdr_end + 4,
                                       rem - hdr_len2 - 4, delim, delim_len);
            if (!next) break;
            pos = next + delim_len;
            rem = body_len - (size_t)(pos - body);
            if (rem >= 2 && pos[0] == '\r' && pos[1] == '\n') { pos += 2; }
            else if (rem >= 1 && pos[0] == '\n') { pos += 1; }
            continue;
        }

        const char *data_start = hdr_end + 4;
        size_t      data_rem   = rem - hdr_len2 - 4;

        /* Delimiter that ends part data: CRLF + "--" + boundary */
        size_t crlf_delim_len = 2 + delim_len;
        char *crlf_delim = malloc(crlf_delim_len + 1);
        if (!crlf_delim) {
            free(name); free(filename); free(content_type); break;
        }
        memcpy(crlf_delim, "\r\n", 2);
        memcpy(crlf_delim + 2, delim, delim_len);
        crlf_delim[crlf_delim_len] = '\0';

        const char *data_end = mp_find(data_start, data_rem,
                                        crlf_delim, crlf_delim_len);
        free(crlf_delim);

        if (!data_end) {
            free(name); free(filename); free(content_type); break;
        }

        size_t part_data_len = (size_t)(data_end - data_start);

        if (part_data_len > MP_MAX_PART_BYTES) {
            free(name); free(filename); free(content_type);
            pos = data_end + 2 + delim_len;
            rem = body_len - (size_t)(pos - body);
            if (rem >= 2 && pos[0] == '\r' && pos[1] == '\n') { pos += 2; }
            else if (rem >= 1 && pos[0] == '\n') { pos += 1; }
            continue;
        }

        char *data_copy = malloc(part_data_len + 1);
        if (!data_copy) {
            free(name); free(filename); free(content_type); break;
        }
        memcpy(data_copy, data_start, part_data_len);
        data_copy[part_data_len] = '\0';

        if (result.nparts >= cap) {
            size_t new_cap = cap * 2;
            TkMultipart *tmp = realloc(parts, new_cap * sizeof(TkMultipart));
            if (!tmp) {
                free(name); free(filename); free(content_type);
                free(data_copy); break;
            }
            parts = tmp;
            cap   = new_cap;
        }

        parts[result.nparts].name         = name;
        parts[result.nparts].filename     = filename;
        parts[result.nparts].content_type = content_type;
        parts[result.nparts].data         = data_copy;
        parts[result.nparts].data_len     = part_data_len;
        result.nparts++;

        /* Advance past: CRLF (part of CRLF+delim match) + delim */
        pos = data_end + 2 + delim_len;
        rem = body_len - (size_t)(pos - body);

        /* Final boundary suffix "--" means we're done */
        if (rem >= 2 && pos[0] == '-' && pos[1] == '-')
            break;

        if (rem >= 2 && pos[0] == '\r' && pos[1] == '\n') { pos += 2; }
        else if (rem >= 1 && pos[0] == '\n') { pos += 1; }
        else { break; }
    }

    free(delim);
    free(final_delim);
    result.parts = parts;
    return result;
}

void http_multipart_free(TkMultipartResult r)
{
    for (size_t i = 0; i < r.nparts; i++) {
        free((void *)(uintptr_t)r.parts[i].name);
        free((void *)(uintptr_t)r.parts[i].filename);
        free((void *)(uintptr_t)r.parts[i].content_type);
        free((void *)(uintptr_t)r.parts[i].data);
    }
    free(r.parts);
}

/* ── Client API (Story 35.1.2) ─────────────────────────────────────── */

/* strdup_safe — portable strdup; returns NULL on NULL input. */
static char *hstrdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* err_resp_client — build an error HttpClientResp. */
static HttpClientResp err_resp_client(const char *msg, uint64_t code)
{
    HttpClientResp r;
    memset(&r, 0, sizeof(r));
    r.is_err   = 1;
    r.err_msg  = hstrdup(msg);
    r.err_code = code;
    return r;
}

/* ── URL parsing ───────────────────────────────────────────────────── */

static int client_parse_url(const char *base_url, const char *path_suffix,
                            char *host_out, size_t host_cap,
                            char *port_out, size_t port_cap,
                            char *path_out, size_t path_cap,
                            int  *is_https_out)
{
    const char *p = base_url;
    *is_https_out = 0;

    if (strncmp(p, "https://", 8) == 0) {
        *is_https_out = 1;
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else {
        return -1;
    }

    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : (p + strlen(p));

    const char *colon = NULL;
    for (const char *q = p; q < hostend; q++) {
        if (*q == ':') { colon = q; break; }
    }

    if (colon) {
        size_t hlen = (size_t)(colon - p);
        if (hlen >= host_cap) return -1;
        memcpy(host_out, p, hlen);
        host_out[hlen] = '\0';
        size_t plen = (size_t)(hostend - colon - 1);
        if (plen >= port_cap) return -1;
        memcpy(port_out, colon + 1, plen);
        port_out[plen] = '\0';
    } else {
        size_t hlen = (size_t)(hostend - p);
        if (hlen >= host_cap) return -1;
        memcpy(host_out, p, hlen);
        host_out[hlen] = '\0';
        snprintf(port_out, port_cap, "%s", *is_https_out ? "443" : "80");
    }

    const char *base_path = slash ? slash : "";
    snprintf(path_out, path_cap, "%s%s", base_path,
             path_suffix ? path_suffix : "");
    if (path_out[0] == '\0') {
        snprintf(path_out, path_cap, "/");
    }
    return 0;
}

/* ── TCP connect ───────────────────────────────────────────────────── */

static int client_tcp_connect(const char *host, const char *port,
                              uint64_t timeout_ms)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &res) != 0) return -1;

    int fd = -1;
    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec  = (long)(timeout_ms / 1000);
            tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        }
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* ── Raw HTTP request helper ───────────────────────────────────────── */

/*
 * client_do_request — send method+path request via POSIX sockets.
 * For non-streaming requests, reads entire response and returns it.
 * body may be NULL for GET/DELETE.
 */
static HttpClientResp client_do_request(HttpClient *c,
                                        const char *method,
                                        const char *path,
                                        const uint8_t *body,
                                        uint64_t body_len,
                                        const char *content_type)
{
    if (!c || !c->base_url) return err_resp_client("null client", 0);

    if (strncmp(c->base_url, "https://", 8) == 0) {
        return err_resp_client(
            "HTTPS requires TLS (not compiled in); use http://", 0);
    }

    char host[256], port[16], full_path[1024];
    int is_https = 0;
    if (client_parse_url(c->base_url, path,
                         host, sizeof(host), port, sizeof(port),
                         full_path, sizeof(full_path), &is_https) != 0) {
        return err_resp_client("failed to parse URL", 0);
    }

    uint64_t timeout = c->timeout_ms ? c->timeout_ms : 30000;

    /* ── Proxy support (Story 42.1.3) ──────────────────────────────── */
    char proxy_host[256], proxy_port[16];
    int  use_proxy = 0;
    if (c->proxy_url && c->proxy_url[0]) {
        /* Parse proxy URL — must be http:// */
        char dummy_path[16];
        int  dummy_https = 0;
        if (client_parse_url(c->proxy_url, NULL,
                             proxy_host, sizeof(proxy_host),
                             proxy_port, sizeof(proxy_port),
                             dummy_path, sizeof(dummy_path),
                             &dummy_https) != 0) {
            return err_resp_client("failed to parse proxy URL", 0);
        }
        use_proxy = 1;
    }

    int fd;
    if (use_proxy) {
        fd = client_tcp_connect(proxy_host, proxy_port, timeout);
    } else {
        fd = client_tcp_connect(host, port, timeout);
    }
    if (fd < 0) return err_resp_client("TCP connect failed", 0);

    /* For HTTPS through proxy: send CONNECT tunnel request */
    if (use_proxy && is_https) {
        char connect_buf[512];
        int clen = snprintf(connect_buf, sizeof(connect_buf),
            "CONNECT %s:%s HTTP/1.1\r\n"
            "Host: %s:%s\r\n"
            "\r\n",
            host, port, host, port);
        if (clen < 0 || (size_t)clen >= sizeof(connect_buf) ||
            send(fd, connect_buf, (size_t)clen, 0) < 0) {
            close(fd);
            return err_resp_client("proxy CONNECT send failed", 0);
        }
        /* Read proxy 200 response */
        char cbuf[1024];
        ssize_t cn = recv(fd, cbuf, sizeof(cbuf) - 1, 0);
        if (cn <= 0) {
            close(fd);
            return err_resp_client("proxy CONNECT recv failed", 0);
        }
        cbuf[cn] = '\0';
        if (strncmp(cbuf, "HTTP/1.1 200", 12) != 0 &&
            strncmp(cbuf, "HTTP/1.0 200", 12) != 0) {
            close(fd);
            return err_resp_client("proxy CONNECT rejected", 0);
        }
        /* Tunnel established — subsequent I/O goes direct to target */
    }

    /* build request header */
    char req_buf[4096];
    int req_len;

    /* For HTTP through proxy: use absolute URI in request line */
    char request_uri[2048];
    if (use_proxy && !is_https) {
        snprintf(request_uri, sizeof(request_uri),
                 "http://%s:%s%s", host, port, full_path);
    } else {
        snprintf(request_uri, sizeof(request_uri), "%s", full_path);
    }

    if (body && body_len > 0) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %llu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, request_uri, host,
            content_type ? content_type : "application/octet-stream",
            (unsigned long long)body_len);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, request_uri, host);
    }

    if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
        close(fd);
        return err_resp_client("request header too large", 0);
    }

    if (send(fd, req_buf, (size_t)req_len, 0) < 0) {
        close(fd);
        return err_resp_client("send header failed", 0);
    }
    if (body && body_len > 0) {
        if (send(fd, body, (size_t)body_len, 0) < 0) {
            close(fd);
            return err_resp_client("send body failed", 0);
        }
    }

    /* read full response */
    size_t cap = 65536, used = 0;
    char *resp = malloc(cap);
    if (!resp) { close(fd); return err_resp_client("out of memory", 0); }

    ssize_t n;
    while ((n = recv(fd, resp + used, cap - used - 1, 0)) > 0) {
        used += (size_t)n;
        if (used + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(resp, cap);
            if (!tmp) { free(resp); close(fd);
                        return err_resp_client("out of memory", 0); }
            resp = tmp;
        }
    }
    resp[used] = '\0';
    close(fd);

    /* parse status line: "HTTP/1.x NNN ..." */
    uint64_t status = 0;
    {
        const char *sp = strchr(resp, ' ');
        if (sp) status = (uint64_t)atoi(sp + 1);
    }

    /* parse response headers */
    StrPair *rhdrs = NULL;
    uint64_t rhdr_count = 0;
    {
        const char *hdr_start = strstr(resp, "\r\n");
        const char *hdr_end   = strstr(resp, "\r\n\r\n");
        if (hdr_start && hdr_end && hdr_start < hdr_end) {
            hdr_start += 2; /* skip status line */
            rhdrs = malloc(64 * sizeof(StrPair));
            if (rhdrs) {
                const char *hp = hdr_start;
                while (hp < hdr_end && rhdr_count < 64) {
                    const char *eol = strstr(hp, "\r\n");
                    if (!eol || eol > hdr_end) break;
                    const char *col = memchr(hp, ':', (size_t)(eol - hp));
                    if (col) {
                        size_t klen = (size_t)(col - hp);
                        char *k = malloc(klen + 1);
                        if (k) { memcpy(k, hp, klen); k[klen] = '\0'; }
                        const char *vs = col + 1;
                        while (vs < eol && *vs == ' ') vs++;
                        size_t vlen = (size_t)(eol - vs);
                        char *v = malloc(vlen + 1);
                        if (v) { memcpy(v, vs, vlen); v[vlen] = '\0'; }
                        if (k && v) {
                            rhdrs[rhdr_count].key = k;
                            rhdrs[rhdr_count].val = v;
                            rhdr_count++;
                        } else {
                            free(k); free(v);
                        }
                    }
                    hp = eol + 2;
                }
            }
        }
    }

    /* extract body */
    const char *sep = strstr(resp, "\r\n\r\n");
    uint8_t *resp_body = NULL;
    uint64_t resp_body_len = 0;
    if (sep) {
        sep += 4;
        resp_body_len = (uint64_t)(used - (size_t)(sep - resp));
        if (resp_body_len > 0) {
            resp_body = malloc((size_t)resp_body_len);
            if (resp_body) memcpy(resp_body, sep, (size_t)resp_body_len);
        }
    }

    free(resp);

    HttpClientResp r;
    memset(&r, 0, sizeof(r));
    r.status       = status;
    r.headers.data = rhdrs;
    r.headers.len  = rhdr_count;
    r.body         = resp_body;
    r.body_len     = resp_body_len;
    r.is_err       = 0;
    return r;
}

/* ── Public client API ─────────────────────────────────────────────── */

HttpClient *http_client(const char *base_url)
{
    HttpClient *c = malloc(sizeof(HttpClient));
    if (!c) return NULL;
    c->base_url   = hstrdup(base_url);
    c->pool_size  = 4;
    c->timeout_ms = 30000;
    c->proxy_url  = NULL;
    return c;
}

void http_client_free(HttpClient *c)
{
    if (!c) return;
    free(c->base_url);
    free(c->proxy_url);
    free(c);
}

/* ── http_withproxy (Story 42.1.3) ─────────────────────────────────── */

HttpClient *http_withproxy(HttpClient *c, const char *proxy_url)
{
    if (!c) return NULL;
    HttpClient *n = malloc(sizeof(HttpClient));
    if (!n) return NULL;
    n->base_url   = hstrdup(c->base_url);
    n->pool_size  = c->pool_size;
    n->timeout_ms = c->timeout_ms;
    n->proxy_url  = proxy_url ? hstrdup(proxy_url) : NULL;
    return n;
}

HttpClientResp http_get(HttpClient *c, const char *path)
{
    return client_do_request(c, "GET", path, NULL, 0, NULL);
}

HttpClientResp http_post(HttpClient *c, const char *path,
                         const uint8_t *body, uint64_t body_len,
                         const char *content_type)
{
    return client_do_request(c, "POST", path, body, body_len, content_type);
}

HttpClientResp http_put(HttpClient *c, const char *path,
                        const uint8_t *body, uint64_t body_len,
                        const char *content_type)
{
    return client_do_request(c, "PUT", path, body, body_len, content_type);
}

HttpClientResp http_delete(HttpClient *c, const char *path)
{
    return client_do_request(c, "DELETE", path, NULL, 0, NULL);
}

/* ── Streaming ─────────────────────────────────────────────────────── */

static uint64_t next_stream_id = 1;

HttpStreamResult http_stream(HttpClient *c, HttpClientReq req)
{
    HttpStreamResult result;
    memset(&result, 0, sizeof(result));

    if (!c || !c->base_url) {
        result.is_err  = 1;
        result.err_msg = hstrdup("null client");
        return result;
    }
    if (strncmp(c->base_url, "https://", 8) == 0) {
        result.is_err  = 1;
        result.err_msg = hstrdup("HTTPS requires TLS (not compiled in)");
        return result;
    }

    char host[256], port[16], full_path[1024];
    int is_https = 0;
    if (client_parse_url(c->base_url, req.url,
                         host, sizeof(host), port, sizeof(port),
                         full_path, sizeof(full_path), &is_https) != 0) {
        result.is_err  = 1;
        result.err_msg = hstrdup("failed to parse URL");
        return result;
    }

    uint64_t timeout = c->timeout_ms ? c->timeout_ms : 30000;

    /* ── Proxy support for streaming (Story 42.1.3) ──────────────── */
    char proxy_host[256], proxy_port[16];
    int  use_proxy = 0;
    if (c->proxy_url && c->proxy_url[0]) {
        char dummy_path[16];
        int  dummy_https = 0;
        if (client_parse_url(c->proxy_url, NULL,
                             proxy_host, sizeof(proxy_host),
                             proxy_port, sizeof(proxy_port),
                             dummy_path, sizeof(dummy_path),
                             &dummy_https) != 0) {
            result.is_err  = 1;
            result.err_msg = hstrdup("failed to parse proxy URL");
            return result;
        }
        use_proxy = 1;
    }

    int fd;
    if (use_proxy) {
        fd = client_tcp_connect(proxy_host, proxy_port, timeout);
    } else {
        fd = client_tcp_connect(host, port, timeout);
    }
    if (fd < 0) {
        result.is_err  = 1;
        result.err_msg = hstrdup("TCP connect failed");
        return result;
    }

    if (use_proxy && is_https) {
        char connect_buf[512];
        int clen = snprintf(connect_buf, sizeof(connect_buf),
            "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\n\r\n",
            host, port, host, port);
        if (clen < 0 || (size_t)clen >= sizeof(connect_buf) ||
            send(fd, connect_buf, (size_t)clen, 0) < 0) {
            close(fd);
            result.is_err  = 1;
            result.err_msg = hstrdup("proxy CONNECT send failed");
            return result;
        }
        char cbuf[1024];
        ssize_t cn = recv(fd, cbuf, sizeof(cbuf) - 1, 0);
        if (cn <= 0) {
            close(fd);
            result.is_err  = 1;
            result.err_msg = hstrdup("proxy CONNECT recv failed");
            return result;
        }
        cbuf[cn] = '\0';
        if (strncmp(cbuf, "HTTP/1.1 200", 12) != 0 &&
            strncmp(cbuf, "HTTP/1.0 200", 12) != 0) {
            close(fd);
            result.is_err  = 1;
            result.err_msg = hstrdup("proxy CONNECT rejected");
            return result;
        }
    }

    /* build + send request */
    const char *method = req.method ? req.method : "GET";
    char req_buf[4096];
    int req_len;

    char request_uri[2048];
    if (use_proxy && !is_https) {
        snprintf(request_uri, sizeof(request_uri),
                 "http://%s:%s%s", host, port, full_path);
    } else {
        snprintf(request_uri, sizeof(request_uri), "%s", full_path);
    }

    if (req.body && req.body_len > 0) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Length: %llu\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            method, request_uri, host,
            (unsigned long long)req.body_len);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            method, request_uri, host);
    }

    if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
        close(fd);
        result.is_err  = 1;
        result.err_msg = hstrdup("request header too large");
        return result;
    }

    if (send(fd, req_buf, (size_t)req_len, 0) < 0) {
        close(fd);
        result.is_err  = 1;
        result.err_msg = hstrdup("send failed");
        return result;
    }
    if (req.body && req.body_len > 0) {
        if (send(fd, req.body, (size_t)req.body_len, 0) < 0) {
            close(fd);
            result.is_err  = 1;
            result.err_msg = hstrdup("send body failed");
            return result;
        }
    }

    /* skip HTTP response headers to get to streaming body */
    size_t buf_cap = 8192;
    char *buf = malloc(buf_cap);
    if (!buf) { close(fd); result.is_err = 1;
                result.err_msg = hstrdup("out of memory"); return result; }
    size_t buf_used = 0;

    /* read until we find \r\n\r\n */
    while (1) {
        if (buf_used + 1 >= buf_cap) {
            buf_cap *= 2;
            char *tmp = realloc(buf, buf_cap);
            if (!tmp) { free(buf); close(fd); result.is_err = 1;
                        result.err_msg = hstrdup("out of memory");
                        return result; }
            buf = tmp;
        }
        ssize_t n = recv(fd, buf + buf_used, buf_cap - buf_used - 1, 0);
        if (n <= 0) { free(buf); close(fd); result.is_err = 1;
                      result.err_msg = hstrdup("connection closed before headers");
                      return result; }
        buf_used += (size_t)n;
        buf[buf_used] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    /* find start of body data after headers */
    char *body_start = strstr(buf, "\r\n\r\n");
    size_t hdr_len = (size_t)(body_start - buf) + 4;
    size_t leftover = buf_used - hdr_len;

    /* shift leftover to start of buffer */
    if (leftover > 0) memmove(buf, buf + hdr_len, leftover);
    buf_used = leftover;

    result.stream.id      = next_stream_id++;
    result.stream.open    = 1;
    result.stream.fd      = fd;
    result.stream.buf     = buf;
    result.stream.buf_len = (uint64_t)buf_used;
    result.stream.buf_cap = (uint64_t)buf_cap;
    result.is_err         = 0;
    return result;
}

HttpChunkResult http_streamnext(HttpStream *s)
{
    HttpChunkResult result;
    memset(&result, 0, sizeof(result));

    if (!s || !s->open) {
        result.is_err  = 1;
        result.err_msg = hstrdup("stream not open");
        return result;
    }

    /* try to read more data */
    if (s->buf_len + 1 >= s->buf_cap) {
        s->buf_cap = s->buf_cap ? s->buf_cap * 2 : 8192;
        char *tmp = realloc(s->buf, (size_t)s->buf_cap);
        if (!tmp) {
            result.is_err  = 1;
            result.err_msg = hstrdup("out of memory");
            return result;
        }
        s->buf = tmp;
    }

    ssize_t n = recv(s->fd, s->buf + s->buf_len,
                     (size_t)(s->buf_cap - s->buf_len - 1), 0);
    if (n <= 0) {
        /* end of stream or error */
        s->open = 0;
        close(s->fd);
        s->fd = -1;
        if (s->buf_len > 0) {
            /* return remaining buffered data */
            result.data = malloc((size_t)s->buf_len);
            if (result.data) {
                memcpy(result.data, s->buf, (size_t)s->buf_len);
                result.len = s->buf_len;
            }
            s->buf_len = 0;
            return result;
        }
        result.is_err   = 1;
        result.err_msg  = hstrdup("stream closed");
        result.err_code = 0;
        free(s->buf); s->buf = NULL;
        return result;
    }

    s->buf_len += (uint64_t)n;

    /* return everything we have */
    result.data = malloc((size_t)s->buf_len);
    if (!result.data) {
        result.is_err  = 1;
        result.err_msg = hstrdup("out of memory");
        return result;
    }
    memcpy(result.data, s->buf, (size_t)s->buf_len);
    result.len = s->buf_len;
    s->buf_len = 0;
    return result;
}

/* ── File download (Story 42.1.4) ────────────────────────────────────── */

#define DL_CHUNK_SIZE 8192

/*
 * parse_content_length — scan raw header block for Content-Length value.
 * Returns 0 if not found or unparseable (caller treats as chunked/unknown).
 */
static uint64_t parse_content_length(const char *hdrs, size_t hdr_len)
{
    const char *p = hdrs;
    const char *end = hdrs + hdr_len;
    while (p < end) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > end) break;
        if ((size_t)(eol - p) > 15 &&
            strncasecmp(p, "Content-Length:", 15) == 0)
        {
            const char *vs = p + 15;
            while (vs < eol && *vs == ' ') vs++;
            return (uint64_t)strtoull(vs, NULL, 10);
        }
        p = eol + 2;
    }
    return 0;
}

/*
 * is_chunked_encoding — check if Transfer-Encoding: chunked is present.
 */
static int is_chunked_encoding(const char *hdrs, size_t hdr_len)
{
    const char *p = hdrs;
    const char *end = hdrs + hdr_len;
    while (p < end) {
        const char *eol = strstr(p, "\r\n");
        if (!eol || eol > end) break;
        if ((size_t)(eol - p) > 18 &&
            strncasecmp(p, "Transfer-Encoding:", 18) == 0)
        {
            const char *vs = p + 18;
            while (vs < eol && *vs == ' ') vs++;
            size_t vlen = (size_t)(eol - vs);
            if (vlen >= 7 && strncasecmp(vs, "chunked", 7) == 0)
                return 1;
        }
        p = eol + 2;
    }
    return 0;
}

/*
 * read_chunked_to_file — decode chunked transfer-encoding directly to file.
 * fd = socket, fp = output file, leftover/leftover_len = data already read
 * past the header boundary.  Calls progress_fn after each decoded chunk.
 * Returns total bytes written on success, (uint64_t)-1 on error.
 */
static uint64_t read_chunked_to_file(int fd, FILE *fp,
                                     const char *leftover,
                                     size_t leftover_len,
                                     HttpProgressFn progress_fn)
{
    size_t cap = leftover_len + DL_CHUNK_SIZE + 1;
    char *buf = malloc(cap);
    if (!buf) return (uint64_t)-1;
    if (leftover_len > 0) memcpy(buf, leftover, leftover_len);
    size_t used = leftover_len;
    uint64_t total_written = 0;

    while (1) {
        buf[used] = '\0';

        /* find chunk-size line ending in \r\n */
        char *crlf = strstr(buf, "\r\n");
        if (!crlf) {
            if (used + DL_CHUNK_SIZE + 1 > cap) {
                cap = used + DL_CHUNK_SIZE + 1;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(buf); return (uint64_t)-1; }
                buf = tmp;
            }
            ssize_t n = recv(fd, buf + used, DL_CHUNK_SIZE, 0);
            if (n <= 0) { free(buf); return (uint64_t)-1; }
            used += (size_t)n;
            continue;
        }

        /* parse hex chunk size */
        unsigned long chunk_sz = strtoul(buf, NULL, 16);
        size_t hdr_bytes = (size_t)(crlf - buf) + 2;

        if (chunk_sz == 0) {
            /* final chunk */
            free(buf);
            return total_written;
        }

        /* need hdr_bytes + chunk_sz + 2 (trailing \r\n) in buffer */
        size_t need = hdr_bytes + (size_t)chunk_sz + 2;
        while (used < need) {
            if (need + 1 > cap) {
                cap = need + 1;
                char *tmp = realloc(buf, cap);
                if (!tmp) { free(buf); return (uint64_t)-1; }
                buf = tmp;
            }
            ssize_t n = recv(fd, buf + used, need - used, 0);
            if (n <= 0) { free(buf); return (uint64_t)-1; }
            used += (size_t)n;
        }

        if (fwrite(buf + hdr_bytes, 1, (size_t)chunk_sz, fp) !=
            (size_t)chunk_sz)
        {
            free(buf);
            return (uint64_t)-1;
        }
        total_written += chunk_sz;

        if (progress_fn) progress_fn(total_written, 0);

        size_t consumed = need;
        if (used > consumed)
            memmove(buf, buf + consumed, used - consumed);
        used -= consumed;
    }
}

int http_downloadfile(HttpClient *c, const char *url,
                      const char *dest_path, HttpProgressFn progress_fn)
{
    if (!c || !c->base_url || !url || !dest_path) return -1;

    if (strncmp(c->base_url, "https://", 8) == 0) return -1;

    char host[256], port[16], full_path[1024];
    int is_https = 0;
    if (client_parse_url(c->base_url, url,
                         host, sizeof(host), port, sizeof(port),
                         full_path, sizeof(full_path), &is_https) != 0)
        return -1;

    uint64_t timeout = c->timeout_ms ? c->timeout_ms : 30000;
    int fd = client_tcp_connect(host, port, timeout);
    if (fd < 0) return -1;

    /* send GET request */
    char req_buf[4096];
    int req_len = snprintf(req_buf, sizeof(req_buf),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        full_path, host);

    if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
        close(fd); return -1;
    }
    if (send(fd, req_buf, (size_t)req_len, 0) < 0) {
        close(fd); return -1;
    }

    /* read response headers */
    size_t hdr_cap = DL_CHUNK_SIZE;
    char *hdr_buf = malloc(hdr_cap);
    if (!hdr_buf) { close(fd); return -1; }
    size_t hdr_used = 0;

    while (1) {
        if (hdr_used + 1 >= hdr_cap) {
            hdr_cap *= 2;
            char *tmp = realloc(hdr_buf, hdr_cap);
            if (!tmp) { free(hdr_buf); close(fd); return -1; }
            hdr_buf = tmp;
        }
        ssize_t n = recv(fd, hdr_buf + hdr_used,
                         hdr_cap - hdr_used - 1, 0);
        if (n <= 0) { free(hdr_buf); close(fd); return -1; }
        hdr_used += (size_t)n;
        hdr_buf[hdr_used] = '\0';
        if (strstr(hdr_buf, "\r\n\r\n")) break;
    }

    /* locate header/body boundary */
    char *sep = strstr(hdr_buf, "\r\n\r\n");
    size_t headers_len = (size_t)(sep - hdr_buf) + 4;
    size_t leftover_len = hdr_used - headers_len;

    /* check HTTP status (must be 2xx) */
    {
        const char *sp = strchr(hdr_buf, ' ');
        if (!sp) { free(hdr_buf); close(fd); return -1; }
        int status = atoi(sp + 1);
        if (status < 200 || status >= 300) {
            free(hdr_buf); close(fd); return -1;
        }
    }

    uint64_t content_length = parse_content_length(hdr_buf, headers_len);
    int chunked = is_chunked_encoding(hdr_buf, headers_len);

    /* open tmp file */
    size_t tmp_path_len = strlen(dest_path) + 5;
    char *tmp_path = malloc(tmp_path_len);
    if (!tmp_path) { free(hdr_buf); close(fd); return -1; }
    snprintf(tmp_path, tmp_path_len, "%s.tmp", dest_path);

    FILE *fp = fopen(tmp_path, "wb");
    if (!fp) { free(tmp_path); free(hdr_buf); close(fd); return -1; }

    int ok = 0;

    if (chunked) {
        uint64_t written = read_chunked_to_file(
            fd, fp, hdr_buf + headers_len, leftover_len, progress_fn);
        ok = (written != (uint64_t)-1) ? 0 : -1;
    } else {
        uint64_t bytes_total = content_length;
        uint64_t bytes_done = 0;

        /* write any leftover data from header read */
        if (leftover_len > 0) {
            if (fwrite(hdr_buf + headers_len, 1, leftover_len, fp) !=
                leftover_len)
            {
                ok = -1;
                goto dl_cleanup;
            }
            bytes_done += (uint64_t)leftover_len;
            if (progress_fn) progress_fn(bytes_done, bytes_total);
        }

        /* stream remaining body in chunks */
        char chunk[DL_CHUNK_SIZE];
        while (1) {
            if (bytes_total > 0 && bytes_done >= bytes_total) break;

            size_t to_read = DL_CHUNK_SIZE;
            if (bytes_total > 0 && (bytes_total - bytes_done) < to_read)
                to_read = (size_t)(bytes_total - bytes_done);

            ssize_t n = recv(fd, chunk, to_read, 0);
            if (n <= 0) break;

            if (fwrite(chunk, 1, (size_t)n, fp) != (size_t)n) {
                ok = -1;
                goto dl_cleanup;
            }
            bytes_done += (uint64_t)n;
            if (progress_fn) progress_fn(bytes_done, bytes_total);
        }

        if (bytes_total > 0 && bytes_done < bytes_total)
            ok = -1;
    }

dl_cleanup:
    free(hdr_buf);
    fclose(fp);
    close(fd);

    if (ok == 0) {
        if (rename(tmp_path, dest_path) != 0) ok = -1;
    }

    if (ok != 0) remove(tmp_path);

    free(tmp_path);
    return ok;
}

/* ── TLS/HTTPS support (Story 27.1.2) ───────────────────────────────── */

#ifdef TK_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/ocsp.h>
#include <openssl/x509v3.h>

/* SNI entry for per-vhost certificate selection (61.2.4) */
typedef struct {
    char    *hostname;
    SSL_CTX *ssl_ctx;
} TlsSniHost;

#define TLS_MAX_SNI_HOSTS 32

struct TkTlsCtx {
    SSL_CTX    *ssl_ctx;
    TlsSniHost  sni_hosts[TLS_MAX_SNI_HOSTS];
    int         sni_count;
    /* OCSP stapling (61.2.1) */
    uint8_t    *ocsp_response;
    size_t      ocsp_response_len;
};

/* ── ALPN callback (Story 60.1.5) ─────────────────────────────────────── */

/* Preferred protocols: h2 first, then http/1.1 */
static const uint8_t alpn_protos[] = {
    2, 'h', '2',
    8, 'h', 't', 't', 'p', '/', '1', '.', '1',
};

static int alpn_select_cb(SSL *ssl, const unsigned char **out,
                          unsigned char *outlen,
                          const unsigned char *in, unsigned int inlen,
                          void *arg)
{
    (void)ssl; (void)arg;
    if (SSL_select_next_proto((unsigned char **)out, outlen,
                              alpn_protos, sizeof(alpn_protos),
                              in, inlen) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    /* Fallback: accept whatever the client offers (http/1.1) */
    return SSL_TLSEXT_ERR_NOACK;
}

/* Forward declarations for OCSP stapling (61.2.1) */
static int  ocsp_staple_cb(SSL *ssl, void *arg);
static void tls_enable_ocsp_stapling(TkTlsCtx *tls);

TkTlsCtx *http_tls_ctx_new(const char *cert_path, const char *key_path)
{
    if (!cert_path || !key_path) {
        fprintf(stderr, "http_tls_ctx_new: cert_path and key_path required\n");
        return NULL;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
        fprintf(stderr, "http_tls_ctx_new: SSL_CTX_new failed\n");
        return NULL;
    }

    if (SSL_CTX_use_certificate_file(ctx, cert_path, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "http_tls_ctx_new: failed to load cert: %s\n",
                cert_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_use_PrivateKey_file(ctx, key_path, SSL_FILETYPE_PEM) != 1) {
        fprintf(stderr, "http_tls_ctx_new: failed to load key: %s\n",
                key_path);
        SSL_CTX_free(ctx);
        return NULL;
    }

    if (SSL_CTX_check_private_key(ctx) != 1) {
        fprintf(stderr, "http_tls_ctx_new: cert/key mismatch\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* TLS hardening (Story 57.4.4) */
    /* Minimum TLS 1.2 — disable SSLv3, TLS 1.0, TLS 1.1 */
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

    /* Strong cipher suites: ECDHE + AEAD only, no CBC, no RC4 */
    SSL_CTX_set_cipher_list(ctx,
        "ECDHE-ECDSA-AES256-GCM-SHA384:"
        "ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-AES128-GCM-SHA256:"
        "ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:"
        "ECDHE-RSA-CHACHA20-POLY1305");

    /* TLS 1.3 cipher suites (if available) */
#ifdef TLS1_3_VERSION
    SSL_CTX_set_ciphersuites(ctx,
        "TLS_AES_256_GCM_SHA384:"
        "TLS_AES_128_GCM_SHA256:"
        "TLS_CHACHA20_POLY1305_SHA256");
#endif

    /* Prefer server cipher order */
    SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);

    /* ALPN negotiation (Story 60.1.5): advertise h2 and http/1.1.
     * During TLS handshake, the callback selects h2 if the client
     * offers it; otherwise falls back to http/1.1. */
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);

    /* Enable session tickets by default (61.2.2) */
    SSL_CTX_set_num_tickets(ctx, 2);

    TkTlsCtx *tls = malloc(sizeof(TkTlsCtx));
    if (!tls) {
        SSL_CTX_free(ctx);
        return NULL;
    }
    tls->ssl_ctx = ctx;
    tls->sni_count = 0;
    tls->ocsp_response = NULL;
    tls->ocsp_response_len = 0;
    tls_enable_ocsp_stapling(tls);
    return tls;
}

/* ── OCSP stapling callback (Story 61.2.1) ────────────────────────────── */

static int ocsp_staple_cb(SSL *ssl, void *arg)
{
    TkTlsCtx *tls = (TkTlsCtx *)arg;
    if (!tls->ocsp_response || tls->ocsp_response_len == 0)
        return SSL_TLSEXT_ERR_NOACK;

    /* OpenSSL takes ownership of this copy */
    uint8_t *copy = OPENSSL_malloc(tls->ocsp_response_len);
    if (!copy) return SSL_TLSEXT_ERR_NOACK;
    memcpy(copy, tls->ocsp_response, tls->ocsp_response_len);
    SSL_set_tlsext_status_ocsp_resp(ssl, copy, (int)tls->ocsp_response_len);
    return SSL_TLSEXT_ERR_OK;
}

/* http_tls_set_ocsp_response — set the cached OCSP response for stapling.
 * The data is copied. Call this periodically to refresh. */
int http_tls_set_ocsp_response(TkTlsCtx *ctx,
                                const uint8_t *data, size_t len)
{
    if (!ctx) return -1;
    free(ctx->ocsp_response);
    ctx->ocsp_response = NULL;
    ctx->ocsp_response_len = 0;
    if (data && len > 0) {
        ctx->ocsp_response = malloc(len);
        if (!ctx->ocsp_response) return -1;
        memcpy(ctx->ocsp_response, data, len);
        ctx->ocsp_response_len = len;
    }
    return 0;
}

/* Enable OCSP stapling on a TLS context */
static void tls_enable_ocsp_stapling(TkTlsCtx *tls)
{
    SSL_CTX_set_tlsext_status_cb(tls->ssl_ctx, ocsp_staple_cb);
    SSL_CTX_set_tlsext_status_arg(tls->ssl_ctx, tls);
}

/* ── SNI callback (Story 61.2.4) ──────────────────────────────────────── */

static int sni_callback(SSL *ssl, int *al, void *arg)
{
    (void)al;
    TkTlsCtx *tls = (TkTlsCtx *)arg;
    const char *name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (!name) return SSL_TLSEXT_ERR_OK; /* no SNI — use default */

    for (int i = 0; i < tls->sni_count; i++) {
        if (strcasecmp(tls->sni_hosts[i].hostname, name) == 0) {
            SSL_set_SSL_CTX(ssl, tls->sni_hosts[i].ssl_ctx);
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_OK; /* unknown host — use default cert */
}

int http_tls_add_sni(TkTlsCtx *ctx, const char *hostname,
                     const char *cert_path, const char *key_path)
{
    if (!ctx || !hostname || !cert_path || !key_path) return -1;
    if (ctx->sni_count >= TLS_MAX_SNI_HOSTS) return -1;

    SSL_CTX *sctx = SSL_CTX_new(TLS_server_method());
    if (!sctx) return -1;

    if (SSL_CTX_use_certificate_file(sctx, cert_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(sctx, key_path, SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(sctx) != 1) {
        SSL_CTX_free(sctx);
        return -1;
    }

    /* Copy ALPN callback from primary context */
    SSL_CTX_set_alpn_select_cb(sctx, alpn_select_cb, NULL);

    int idx = ctx->sni_count;
    ctx->sni_hosts[idx].hostname = strdup(hostname);
    ctx->sni_hosts[idx].ssl_ctx  = sctx;
    ctx->sni_count++;

    /* Register SNI callback on the primary context (once) */
    if (ctx->sni_count == 1) {
        SSL_CTX_set_tlsext_servername_callback(ctx->ssl_ctx, sni_callback);
        SSL_CTX_set_tlsext_servername_arg(ctx->ssl_ctx, ctx);
    }
    return 0;
}

/* ── http_tls_ctx_new_config (Story 61.2.3) ───────────────────────────── */

static int tls_version_from_str(const char *s)
{
    if (!s || strcmp(s, "1.2") == 0) return TLS1_2_VERSION;
    if (strcmp(s, "1.3") == 0) return TLS1_3_VERSION;
    return TLS1_2_VERSION;
}

TkTlsCtx *http_tls_ctx_new_config(const TkTlsConfig *config)
{
    if (!config || !config->cert_path || !config->key_path) {
        fprintf(stderr, "http_tls_ctx_new_config: cert/key required\n");
        return NULL;
    }

    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return NULL;

    /* Set minimum TLS version (61.2.3) */
    SSL_CTX_set_min_proto_version(ctx, tls_version_from_str(config->min_version));

    /* Cipher suites (61.2.3) */
    if (config->ciphers) {
        /* TLS 1.2 ciphers */
        SSL_CTX_set_cipher_list(ctx, config->ciphers);
        /* TLS 1.3 ciphersuites */
        SSL_CTX_set_ciphersuites(ctx, config->ciphers);
    }

    /* Elliptic curves (61.2.3) */
    if (config->curves) {
        SSL_CTX_set1_curves_list(ctx, config->curves);
    }

    /* Session tickets (61.2.2) */
    if (config->session_tickets) {
        SSL_CTX_set_num_tickets(ctx, 2);
        long lifetime = config->ticket_lifetime > 0
                        ? (long)config->ticket_lifetime : 7200L;
        SSL_CTX_set_timeout(ctx, lifetime);
    } else {
        SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    }

    /* Load cert + key */
    if (SSL_CTX_use_certificate_file(ctx, config->cert_path,
                                     SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, config->key_path,
                                    SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(ctx) != 1) {
        fprintf(stderr, "http_tls_ctx_new_config: cert/key error\n");
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* ALPN */
    SSL_CTX_set_alpn_select_cb(ctx, alpn_select_cb, NULL);

    TkTlsCtx *tls = malloc(sizeof(TkTlsCtx));
    if (!tls) { SSL_CTX_free(ctx); return NULL; }
    tls->ssl_ctx = ctx;
    tls->sni_count = 0;
    tls->ocsp_response = NULL;
    tls->ocsp_response_len = 0;
    tls_enable_ocsp_stapling(tls);
    return tls;
}

void http_tls_ctx_free(TkTlsCtx *ctx)
{
    if (!ctx) return;
    free(ctx->ocsp_response);
    for (int i = 0; i < ctx->sni_count; i++) {
        SSL_CTX_free(ctx->sni_hosts[i].ssl_ctx);
        free(ctx->sni_hosts[i].hostname);
    }
    SSL_CTX_free(ctx->ssl_ctx);
    free(ctx);
}

/* http_tls_reload_cert — hot-swap cert+key in a running TLS context. */
int http_tls_reload_cert(TkTlsCtx *ctx, const char *cert_path,
                          const char *key_path)
{
    if (!ctx || !cert_path || !key_path) return -1;
    if (SSL_CTX_use_certificate_file(ctx->ssl_ctx, cert_path,
                                      SSL_FILETYPE_PEM) != 1)
        return -1;
    if (SSL_CTX_use_PrivateKey_file(ctx->ssl_ctx, key_path,
                                     SSL_FILETYPE_PEM) != 1)
        return -1;
    if (SSL_CTX_check_private_key(ctx->ssl_ctx) != 1)
        return -1;
    return 0;
}

/*
 * write_security_headers_ssl — emit hardened headers via SSL_write.
 */
static void write_security_headers_ssl(SSL *ssl)
{
    static const char hdrs[] =
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: strict-origin-when-cross-origin\r\n"
        "Strict-Transport-Security: max-age=63072000; includeSubDomains; preload\r\n";
    SSL_write(ssl, hdrs, (int)(sizeof(hdrs) - 1));
}

/* write_cors_headers_ssl — emit CORS headers over TLS when configured (Story 7.7.4).
 * Uses g_cors_matched (the single matching origin), not the full allowed list. */
static void write_cors_headers_ssl(SSL *ssl)
{
    if (!g_cors_matched) return;
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "Access-Control-Allow-Origin: %s\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, PATCH, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Vary: Origin\r\n",
        g_cors_matched);
    if (n > 0) SSL_write(ssl, buf, n);
}

/*
 * handle_tls_connection — accept a TLS handshake on fd, then process
 * HTTP keep-alive requests over the TLS session.  Mirrors handle_connection
 * but uses SSL_read/SSL_write instead of read/write.
 */
/* ssl_shutdown_quick — shut down TLS with a short timeout to avoid
 * blocking the worker while waiting for the peer's close_notify. */
/* ── HTTP/2 connection handler (Story 60.2.1) ────────────────────────── */

#include "http2.h"

/*
 * handle_h2_connection — process an HTTP/2 connection.
 * Validates connection preface, exchanges SETTINGS, then loops
 * reading frames. HEADERS frames are decoded via HPACK into Req
 * structs and dispatched through the existing route table.
 */
static void handle_h2_connection(int fd, void *ssl, const char *client_ip)
{
    H2Conn *conn = h2_conn_new();
    if (!conn) return;

    /* Read and validate the 24-byte client connection preface */
    uint8_t preface[H2_CONNECTION_PREFACE_LEN];
    {
        size_t done = 0;
        while (done < H2_CONNECTION_PREFACE_LEN) {
            int r = SSL_read(ssl, preface + done,
                             (int)(H2_CONNECTION_PREFACE_LEN - done));
            if (r <= 0) { h2_conn_free(conn); return; }
            done += (size_t)r;
        }
    }
    if (h2_validate_preface(preface, H2_CONNECTION_PREFACE_LEN) != 0) {
        h2_conn_free(conn);
        return;
    }

    /* Send our SETTINGS frame */
    if (h2_send_settings(conn, fd, ssl) < 0) {
        h2_conn_free(conn);
        return;
    }

    /* Set idle timeout for HTTP/2 connection (60.2.3) */
#define H2_IDLE_TIMEOUT_S       30
#define H2_MAX_STREAMS_TOTAL   1000
#define H2_PING_INTERVAL_S      15
    {
        struct timeval tv;
        tv.tv_sec  = H2_IDLE_TIMEOUT_S;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    uint32_t total_streams = 0;
    time_t last_activity = time(NULL);
    int ping_outstanding = 0;

    /* Main frame processing loop */
    for (;;) {
        H2FrameHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        uint8_t *payload = h2_frame_recv(conn, fd, ssl, &hdr);
        if (!payload && hdr.length == 0 && hdr.type == 0) {
            /* Timeout or connection closed — check if we should ping or close */
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                time_t now = time(NULL);
                if (ping_outstanding) {
                    /* No response to PING — peer is dead */
                    break;
                }
                if (now - last_activity >= H2_IDLE_TIMEOUT_S) {
                    /* Idle too long — send PING keepalive */
                    uint8_t ping_data[8] = {0};
                    h2_send_ping(conn, fd, ssl, ping_data, 0);
                    ping_outstanding = 1;
                    last_activity = now;
                    continue;
                }
                continue;
            }
            /* Real read error or connection closed */
            break;
        }

        last_activity = time(NULL);
        ping_outstanding = 0;

        /* Frame size validation (60.2.4) — reject oversized frames */
        if (hdr.length > conn->local_settings.max_frame_size) {
            h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                           H2_ERR_FRAME_SIZE);
            goto done;
        }

        switch (hdr.type) {
        case H2_FRAME_SETTINGS:
            if (hdr.flags & H2_FLAG_ACK) {
                /* SETTINGS ACK — nothing to do */
                break;
            }
            if (hdr.stream_id != 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            if (h2_apply_settings(conn, payload, hdr.length) < 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            h2_send_settings_ack(conn, fd, ssl);
            break;

        case H2_FRAME_HEADERS: {
            if (hdr.stream_id == 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }

            /* Enforce max total streams (60.2.3) */
            total_streams++;
            if (total_streams > H2_MAX_STREAMS_TOTAL) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_ENHANCE_YOUR_CALM);
                goto done;
            }

            H2Stream *stream = h2_stream_get(conn, hdr.stream_id);
            if (!stream) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_REFUSED_STREAM);
                goto done;
            }
            h2_stream_transition(stream, H2_FRAME_HEADERS, hdr.flags, 0);

            /* Skip padding and priority if present */
            uint8_t *hblock = payload;
            uint32_t hblock_len = hdr.length;
            uint8_t pad_len = 0;

            if (hdr.flags & H2_FLAG_PADDED) {
                if (hblock_len < 1) goto done;
                pad_len = hblock[0];
                hblock++;
                hblock_len--;
                if (pad_len >= hblock_len) goto done;
                hblock_len -= pad_len;
            }
            if (hdr.flags & H2_FLAG_PRIORITY) {
                if (hblock_len < 5) goto done;
                hblock += 5; /* skip 4-byte dep + 1-byte weight */
                hblock_len -= 5;
            }

            /* Accumulate header block fragments */
            if (stream->header_block == NULL) {
                stream->header_block = malloc(hblock_len);
                if (!stream->header_block) goto done;
                memcpy(stream->header_block, hblock, hblock_len);
                stream->header_block_len = hblock_len;
                stream->header_block_cap = hblock_len;
            } else {
                size_t newlen = stream->header_block_len + hblock_len;
                if (newlen > stream->header_block_cap) {
                    uint8_t *nb = realloc(stream->header_block, newlen);
                    if (!nb) goto done;
                    stream->header_block = nb;
                    stream->header_block_cap = newlen;
                }
                memcpy(stream->header_block + stream->header_block_len,
                       hblock, hblock_len);
                stream->header_block_len = newlen;
            }

            if (!(hdr.flags & H2_FLAG_END_HEADERS)) {
                /* Wait for CONTINUATION frames */
                break;
            }

            /* Decode HPACK header block into request */
            char **names = NULL, **values = NULL;
            int nheaders = hpack_decode(&conn->hpack_decode,
                                        stream->header_block,
                                        stream->header_block_len,
                                        &names, &values);
            free(stream->header_block);
            stream->header_block = NULL;
            stream->header_block_len = 0;

            if (nheaders < 0) {
                h2_send_rst_stream(conn, fd, ssl, hdr.stream_id,
                                   H2_ERR_COMPRESSION);
                break;
            }

            /* Build Req from pseudo-headers */
            const char *method = "GET";
            const char *path   = "/";
            StrPair req_headers[64];
            int req_hcount = 0;

            for (int i = 0; i < nheaders; i++) {
                if (names[i][0] == ':') {
                    if (strcmp(names[i], ":method") == 0) method = values[i];
                    else if (strcmp(names[i], ":path") == 0) path = values[i];
                } else if (req_hcount < 64) {
                    req_headers[req_hcount].key = names[i];
                    req_headers[req_hcount].val = values[i];
                    req_hcount++;
                }
            }

            Req req;
            memset(&req, 0, sizeof(req));
            req.method  = method;
            req.path    = path;
            req.body    = "";
            req.headers.data = req_headers;
            req.headers.len  = (uint64_t)req_hcount;

            /* Dispatch through route table */
            Res res = {0};
            int matched = 0;
            for (int r = 0; r < route_count; r++) {
                if (strcmp(route_table[r].method, method) == 0 ||
                    strcmp(route_table[r].method, "*") == 0) {
                    /* Simple path matching (TODO: use router.c scoring) */
                    if (strcmp(route_table[r].pattern, path) == 0 ||
                        strcmp(route_table[r].pattern, "*") == 0) {
                        res = route_table[r].h(req);
                        matched = 1;
                        break;
                    }
                }
            }
            if (!matched) {
                res.status = 404;
                res.body   = "Not Found";
            }

            /* Send response as HEADERS + DATA frames */
            char status_str[4];
            snprintf(status_str, sizeof(status_str), "%u", res.status);
            size_t body_len = res.body ? strlen(res.body) : 0;
            char cl_str[32];
            snprintf(cl_str, sizeof(cl_str), "%zu", body_len);

            const char *resp_names[]  = { ":status", "content-length",
                                          "content-type" };
            const char *resp_values[] = { status_str, cl_str,
                                          "text/html; charset=utf-8" };
            uint8_t hbuf[4096];
            int hlen = hpack_encode(&conn->hpack_encode,
                                    resp_names, resp_values, 3,
                                    hbuf, sizeof(hbuf));

            if (hlen > 0) {
                uint8_t flags = H2_FLAG_END_HEADERS;
                if (body_len == 0) flags |= H2_FLAG_END_STREAM;
                h2_frame_send(conn, fd, ssl, H2_FRAME_HEADERS, flags,
                              hdr.stream_id, hbuf, (uint32_t)hlen);
            }

            /* Send body as DATA frame(s) */
            if (body_len > 0) {
                uint32_t max_data = conn->peer_settings.max_frame_size;
                size_t sent = 0;
                while (sent < body_len) {
                    uint32_t chunk = (uint32_t)(body_len - sent);
                    if (chunk > max_data) chunk = max_data;
                    uint8_t dflags = (sent + chunk >= body_len)
                                     ? H2_FLAG_END_STREAM : 0;
                    h2_frame_send(conn, fd, ssl, H2_FRAME_DATA, dflags,
                                  hdr.stream_id,
                                  (const uint8_t *)res.body + sent, chunk);
                    sent += chunk;
                }
            }

            log_request(client_ip, &req, res.status, body_len);

            /* Clean up decoded headers */
            for (int i = 0; i < nheaders; i++) {
                free(names[i]);
                free(values[i]);
            }
            free(names);
            free(values);
            break;
        }

        case H2_FRAME_CONTINUATION: {
            if (hdr.stream_id == 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            H2Stream *stream = h2_stream_get(conn, hdr.stream_id);
            if (!stream || !stream->header_block) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            /* Append to header block */
            size_t newlen = stream->header_block_len + hdr.length;
            if (newlen > stream->header_block_cap) {
                uint8_t *nb = realloc(stream->header_block, newlen);
                if (!nb) goto done;
                stream->header_block = nb;
                stream->header_block_cap = newlen;
            }
            memcpy(stream->header_block + stream->header_block_len,
                   payload, hdr.length);
            stream->header_block_len = newlen;

            /* If END_HEADERS, process as if we just got the full HEADERS */
            /* (would need to refactor — for now, only single-frame headers) */
            break;
        }

        case H2_FRAME_DATA: {
            if (hdr.stream_id == 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            /* Update flow control window */
            conn->conn_window_recv -= (int32_t)hdr.length;
            H2Stream *stream = h2_stream_get(conn, hdr.stream_id);
            if (stream) {
                stream->window -= (int32_t)hdr.length;
                h2_stream_transition(stream, H2_FRAME_DATA, hdr.flags, 0);
            }
            /* Send WINDOW_UPDATE to replenish */
            if (hdr.length > 0) {
                h2_send_window_update(conn, fd, ssl, 0, hdr.length);
                h2_send_window_update(conn, fd, ssl, hdr.stream_id,
                                      hdr.length);
            }
            break;
        }

        case H2_FRAME_PING:
            if (hdr.stream_id != 0 || hdr.length != 8) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            if (!(hdr.flags & H2_FLAG_ACK)) {
                h2_send_ping(conn, fd, ssl, payload, 1);
            }
            break;

        case H2_FRAME_WINDOW_UPDATE: {
            if (hdr.length != 4) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_FRAME_SIZE);
                goto done;
            }
            uint32_t increment = ((uint32_t)payload[0] << 24)
                               | ((uint32_t)payload[1] << 16)
                               | ((uint32_t)payload[2] << 8)
                               |  (uint32_t)payload[3];
            increment &= 0x7FFFFFFF;
            if (increment == 0) {
                if (hdr.stream_id == 0) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_PROTOCOL);
                } else {
                    h2_send_rst_stream(conn, fd, ssl, hdr.stream_id,
                                       H2_ERR_PROTOCOL);
                }
                break;
            }
            if (hdr.stream_id == 0) {
                conn->conn_window_send += (int32_t)increment;
            } else {
                H2Stream *stream = h2_stream_get(conn, hdr.stream_id);
                if (stream) stream->window += (int32_t)increment;
            }
            break;
        }

        case H2_FRAME_RST_STREAM: {
            if (hdr.stream_id == 0 || hdr.length != 4) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            H2Stream *stream = h2_stream_get(conn, hdr.stream_id);
            if (stream) stream->state = H2_STREAM_CLOSED;
            break;
        }

        case H2_FRAME_GOAWAY:
            /* Peer is shutting down — stop sending new streams */
            goto done;

        case H2_FRAME_PRIORITY:
            /* Advisory only — ignore */
            break;

        case H2_FRAME_PUSH_PROMISE:
            /* Server never accepts PUSH_PROMISE from clients */
            h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                           H2_ERR_PROTOCOL);
            goto done;

        default:
            /* Unknown frame types MUST be ignored (RFC 9113 §4.1) */
            break;
        }

        if (conn->goaway_sent) break;
    }

done:
    h2_conn_free(conn);
}

static void ssl_shutdown_quick(SSL *ssl, int fd)
{
    struct timeval tv;
    tv.tv_sec  = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    SSL_shutdown(ssl);
    SSL_free(ssl);
}

static void handle_tls_connection(int fd, SSL_CTX *ssl_ctx)
{
    char client_ip[64];
    fd_peer_ip(fd, client_ip, sizeof client_ip);

    /* Set socket timeouts BEFORE SSL_accept to prevent workers from
     * blocking indefinitely on incomplete TLS handshakes (e.g., port
     * scanners, bots sending plaintext to TLS port). */
    struct timeval tv;
    tv.tv_sec  = 5;   /* 5s handshake timeout */
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    SSL *ssl = SSL_new(ssl_ctx);
    if (!ssl) { close(fd); return; }

    SSL_set_fd(ssl, fd);

    if (SSL_accept(ssl) != 1) {
        SSL_free(ssl);
        close(fd);
        return;
    }

    /* Check ALPN result — if h2 was negotiated, handle as HTTP/2
     * (Story 60.1.5 / 60.2.1) */
    {
        const unsigned char *alpn = NULL;
        unsigned int alpn_len = 0;
        SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
        if (alpn_len == 2 && alpn[0] == 'h' && alpn[1] == '2') {
            handle_h2_connection(fd, ssl, client_ip);
            ssl_shutdown_quick(ssl, fd);
            close(fd);
            return;
        }
    }

    if (!rate_check(client_ip)) {
        const char *r429 = "HTTP/1.1 429 Too Many Requests\r\n"
                           "Content-Length: 0\r\nConnection: close\r\n\r\n";
        SSL_write(ssl, r429, (int)strlen(r429));
        ssl_shutdown_quick(ssl, fd);
        close(fd);
        return;
    }

    /* Extend timeout for request/response processing */
    tv.tv_sec  = (long)srv_limits.timeout_secs;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t buf_cap = (size_t)srv_limits.max_header
                   + (size_t)srv_limits.max_body + 8;
    char *raw = malloc(buf_cap);
    if (!raw) { SSL_free(ssl); close(fd); return; }

    int requests = 0;

    for (;;) {
        memset(raw, 0, buf_cap);

        /* Use a short timeout while waiting for keep-alive requests
         * so idle connections don't block workers from accepting new ones */
        if (requests > 0) {
            struct timeval ka_tv;
            ka_tv.tv_sec  = KEEPALIVE_IDLE_TIMEOUT_S;
            ka_tv.tv_usec = 0;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ka_tv, sizeof(ka_tv));
        }

        /* Phase 1: read headers over TLS */
        size_t hdr_used = 0;
        int    hdr_ok   = 0;

        while (hdr_used < (size_t)srv_limits.max_header) {
            size_t space = (size_t)srv_limits.max_header - hdr_used;
            int n = SSL_read(ssl, raw + hdr_used, (int)space);
            if (n <= 0) {
                free(raw);
                ssl_shutdown_quick(ssl, fd);
                close(fd);
                return;
            }
            /* Restore full timeout once data is flowing */
            if (hdr_used == 0 && requests > 0) {
                struct timeval full_tv;
                full_tv.tv_sec  = (long)srv_limits.timeout_secs;
                full_tv.tv_usec = 0;
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &full_tv, sizeof(full_tv));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &full_tv, sizeof(full_tv));
            }
            hdr_used += (size_t)n;
            raw[hdr_used] = '\0';
            if (strstr(raw, "\r\n\r\n")) { hdr_ok = 1; break; }
        }

        if (!hdr_ok) {
            ssl_shutdown_quick(ssl, fd);
            free(raw);
            close(fd);
            return;
        }

        /* Phase 2: Content-Length check */
        const char *cl_hdr = NULL;
        {
            char *p = raw;
            while (p && (size_t)(p - raw) < hdr_used) {
                if (strncasecmp(p, "Content-Length:", 15) == 0) {
                    cl_hdr = p + 15;
                    break;
                }
                p = strstr(p, "\r\n");
                if (p) p += 2;
            }
        }

        /* Phase 2b: detect Transfer-Encoding: chunked */
        int tls_req_chunked = 0;
        {
            const char *p2 = raw;
            while (p2 && (size_t)(p2 - raw) < hdr_used) {
                if (strncasecmp(p2, "Transfer-Encoding:", 18) == 0) {
                    const char *v = p2 + 18;
                    while (*v == ' ') v++;
                    if (strncasecmp(v, "chunked", 7) == 0)
                        tls_req_chunked = 1;
                    break;
                }
                p2 = strstr(p2, "\r\n");
                if (p2) p2 += 2;
            }
        }

        /* Reject ambiguous CL + TE: chunked (request-smuggling defence) */
        if (cl_hdr && tls_req_chunked) {
            ssl_shutdown_quick(ssl, fd);
            free(raw); close(fd); return;
        }

        if (cl_hdr) {
            long long cl = atoll(cl_hdr);
            if (cl < 0 ||
                (unsigned long long)cl >
                (unsigned long long)srv_limits.max_body) {
                ssl_shutdown_quick(ssl, fd);
                free(raw);
                close(fd);
                return;
            }

            const char *sep = strstr(raw, "\r\n\r\n");
            if (sep) {
                size_t hlen      = (size_t)(sep - raw) + 4;
                size_t body_have = hdr_used - hlen;
                size_t content_len = (size_t)cl;

                while (body_have < content_len) {
                    size_t need  = content_len - body_have;
                    size_t space = buf_cap - hdr_used - 1;
                    if (space == 0) break;
                    int n = SSL_read(ssl, raw + hdr_used,
                                     (int)(need < space ? need : space));
                    if (n <= 0) {
                        ssl_shutdown_quick(ssl, fd);
                        free(raw);
                        close(fd);
                        return;
                    }
                    hdr_used   += (size_t)n;
                    body_have  += (size_t)n;
                    raw[hdr_used] = '\0';
                }
            }
        }

        requests++;
        int client_wants_close = req_wants_close(raw);
        int max_reached        = (requests >= KEEPALIVE_MAX_REQUESTS);
        int keep_alive         = (!client_wants_close && !max_reached);

        Req req = parse_request(raw);

        /* ── CORS: match request Origin against allowed list (TLS path) ── */
        g_cors_matched = NULL;
        if (g_cors_origins) {
            for (uint64_t ci = 0; ci < req.headers.len; ci++) {
                if (req.headers.data[ci].key &&
                    strcasecmp(req.headers.data[ci].key, "Origin") == 0) {
                    g_cors_matched = cors_match_origin(req.headers.data[ci].val);
                    break;
                }
            }
        }

        /* ── CORS preflight: OPTIONS → 204 with CORS headers (Story 7.7.4) ── */
        if (req.method && strcmp(req.method, "OPTIONS") == 0 && g_cors_matched) {
            const char *ch = keep_alive ? "keep-alive" : "close";
            char hb[256];
            int hl2 = snprintf(hb, sizeof(hb),
                "HTTP/1.1 204 No Content\r\n"
                "Content-Length: 0\r\nConnection: %s\r\n", ch);
            if (hl2 > 0) SSL_write(ssl, hb, hl2);
            write_security_headers_ssl(ssl);
            write_cors_headers_ssl(ssl);
            SSL_write(ssl, "\r\n", 2);
            log_request(client_ip, &req, 204, 0);
            if (!keep_alive) break;
            continue;
        }

        /* ── Reject disallowed methods early ── */
        int tls_is_head = (req.method && strcmp(req.method, "HEAD") == 0);
        if (req.method &&
            strcmp(req.method, "GET")    != 0 &&
            strcmp(req.method, "POST")   != 0 &&
            strcmp(req.method, "PUT")    != 0 &&
            strcmp(req.method, "DELETE") != 0 &&
            strcmp(req.method, "PATCH")  != 0 &&
            strcmp(req.method, "OPTIONS") != 0 &&
            !tls_is_head) {
            Res bad = make_res(405, "Method Not Allowed");
            /* inline TLS send for 405 */
            {
                const char *ch = keep_alive ? "keep-alive" : "close";
                size_t bl = bad.body ? strlen(bad.body) : 0;
                char hb[256];
                int hl2 = snprintf(hb, sizeof(hb),
                    "HTTP/1.1 405 Method Not Allowed\r\n"
                    "Content-Length: %zu\r\nConnection: %s\r\n", bl, ch);
                if (hl2 > 0) SSL_write(ssl, hb, hl2);
                write_security_headers_ssl(ssl);
                write_cors_headers_ssl(ssl);
                SSL_write(ssl, "\r\n", 2);
                if (bl) SSL_write(ssl, bad.body, (int)bl);
                log_request(client_ip, &req, 405, bl);
            }
            if (!keep_alive) break;
            continue;
        }

        /* Dispatch to route table (HEAD falls back to GET handler) */
        Res res = make_res(404, "Not Found");
        StrPair params[32]; int pc = 0;
        int tls_path_matched = 0;
        int tls_handler_called = 0;
        for (int i = 0; i < route_count; i++) {
            if (match_pattern(route_table[i].pattern,
                              req.path ? req.path : "", params, &pc)) {
                tls_path_matched = 1;
                const char *check_method = tls_is_head ? "GET" : req.method;
                if (check_method &&
                    strcmp(route_table[i].method, check_method) == 0) {
                    req.params.data = params; req.params.len = (uint64_t)pc;
                    res = route_table[i].h(req);
                    tls_handler_called = 1;
                    if (tls_is_head) res.body = NULL;
                    break;
                }
                pc = 0;
            }
        }
        if (tls_path_matched && !tls_handler_called && res.status == 404) {
            res = make_res(405, "Method Not Allowed");
        }

        /* Gzip compress if eligible — write directly (binary-safe) */
        char  *tls_gz_buf = NULL;
        size_t tls_gz_len = 0;
        int    tls_did_gz = maybe_gzip_body(req, &res, &tls_gz_buf, &tls_gz_len);
        if (tls_did_gz) {
            const char *conn_hdr = keep_alive ? "keep-alive" : "close";
            char hdr_buf[256];
            int hl = snprintf(hdr_buf, sizeof(hdr_buf),
                "HTTP/1.1 %u %s\r\n"
                "Content-Length: %zu\r\n"
                "Content-Encoding: gzip\r\n"
                "Vary: Accept-Encoding\r\n"
                "Connection: %s\r\n",
                res.status, reason(res.status), tls_gz_len, conn_hdr);
            if (hl > 0) SSL_write(ssl, hdr_buf, hl);
            for (uint64_t hi = 0; hi < res.headers.len; hi++) {
                const char *hk = res.headers.data[hi].key;
                const char *hv = res.headers.data[hi].val;
                if (!hk || !hv) continue;
                char hline[512];
                int ln = snprintf(hline, sizeof(hline), "%s: %s\r\n", hk, hv);
                if (ln > 0) SSL_write(ssl, hline, ln);
            }
            write_security_headers_ssl(ssl);
            write_cors_headers_ssl(ssl);
            SSL_write(ssl, "\r\n", 2);
            SSL_write(ssl, tls_gz_buf, (int)tls_gz_len);
            log_request(client_ip, &req, res.status, tls_gz_len);
            free(tls_gz_buf);
        } else {
            /* Serialise response to a buffer and write via SSL_write */
            const char *conn_hdr = keep_alive ? "keep-alive" : "close";
            size_t body_len = res.body ? strlen(res.body) : 0;
            char hdr_buf[256];
            int hl = snprintf(hdr_buf, sizeof(hdr_buf),
                "HTTP/1.1 %u %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: %s\r\n",
                res.status, reason(res.status), body_len, conn_hdr);
            if (hl > 0) SSL_write(ssl, hdr_buf, hl);
            for (uint64_t hi = 0; hi < res.headers.len; hi++) {
                const char *hk = res.headers.data[hi].key;
                const char *hv = res.headers.data[hi].val;
                if (!hk || !hv) continue;
                char hline[512];
                int ln = snprintf(hline, sizeof(hline), "%s: %s\r\n", hk, hv);
                if (ln > 0) SSL_write(ssl, hline, ln);
            }
            write_security_headers_ssl(ssl);
            write_cors_headers_ssl(ssl);
            SSL_write(ssl, "\r\n", 2);
            if (body_len) SSL_write(ssl, res.body, (int)body_len);
            log_request(client_ip, &req, res.status, body_len);
        }

        if (!keep_alive) break;
    }

    free(raw);
    ssl_shutdown_quick(ssl, fd);
    close(fd);
}

/*
 * tls_worker_loop — accept connections and handle them over TLS.
 * Mirrors worker_loop but uses handle_tls_connection.
 */
static void tls_worker_loop(int srv_fd, TkHttpRouter *r, SSL_CTX *ssl_ctx)
{
    route_count = r->count;
    memcpy(route_table, r->routes, (size_t)r->count * sizeof(Route));

    for (;;) {
        int fd = accept(srv_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                if (g_shutdown_requested) break;
                continue;
            }
            continue;
        }
        if (g_shutdown_requested) { close(fd); break; }
        handle_tls_connection(fd, ssl_ctx);
    }
}

TkHttpErr http_serve_tls(TkHttpRouter *r, const char *host,
                          uint64_t port, TkTlsCtx *tls)
{
    /* Default workers = CPU count; override with TK_HTTP_WORKERS env var */
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    uint64_t nw = (ncpu > 1) ? (uint64_t)ncpu : 1;
    const char *env_w = getenv("TK_HTTP_WORKERS");
    if (env_w) { long v = atol(env_w); if (v > 0) nw = (uint64_t)v; }
    return http_serve_tls_workers(r, host, port, tls, nw);
}

TkHttpErr http_serve_tls_workers(TkHttpRouter *r, const char *host,
                                  uint64_t port, TkTlsCtx *tls,
                                  uint64_t nworkers)
{
    if (!r)   return TK_HTTP_ERR_SOCKET;
    if (!tls) return TK_HTTP_ERR_BIND;

    if (nworkers <= 1) {
        int srv = bind_listen(host, port);
        if (srv < 0) return TK_HTTP_ERR_BIND;
        tls_worker_loop(srv, r, tls->ssl_ctx);
        close(srv);
        return TK_HTTP_OK;
    }

    if (nworkers > TK_MAX_WORKERS) nworkers = TK_MAX_WORKERS;

    int srv = bind_listen(host, port);
    if (srv < 0) return TK_HTTP_ERR_BIND;

    signal(SIGTERM, workers_parent_sighandler);
    signal(SIGINT,  workers_parent_sighandler);

    pid_t tls_pids[TK_MAX_WORKERS];
    for (uint64_t i = 0; i < nworkers; i++) {
        pid_t pid = fork();
        if (pid < 0) {
            for (uint64_t j = 0; j < i; j++) kill(tls_pids[j], SIGTERM);
            close(srv);
            return TK_HTTP_ERR_FORK;
        }
        if (pid == 0) {
            tls_worker_loop(srv, r, tls->ssl_ctx);
            _exit(0);
        }
        tls_pids[i] = pid;
    }

    /* Parent keeps srv open so respawned workers can inherit it. */

    /* Parent: supervise, restart workers that die abnormally */
    for (;;) {
        int   wstatus = 0;
        pid_t died    = waitpid(-1, &wstatus, WNOHANG);

        if (died < 0) {
            if (errno == EINTR) {
                /* Check shutdown flag below */
            } else {
                break;
            }
        }

        if (g_shutdown_requested) {
            for (uint64_t i = 0; i < nworkers; i++) {
                if (tls_pids[i] > 0) kill(tls_pids[i], SIGTERM);
            }
            time_t deadline = time(NULL) + HTTP_DRAIN_TIMEOUT_SECS;
            for (;;) {
                int all_done = 1;
                for (uint64_t i = 0; i < nworkers; i++) {
                    if (tls_pids[i] <= 0) continue;
                    int ws = 0;
                    pid_t p = waitpid(tls_pids[i], &ws, WNOHANG);
                    if (p == tls_pids[i])
                        tls_pids[i] = -1;
                    else
                        all_done = 0;
                }
                if (all_done) break;
                if (time(NULL) >= deadline) {
                    for (uint64_t i = 0; i < nworkers; i++) {
                        if (tls_pids[i] > 0) {
                            kill(tls_pids[i], SIGKILL);
                            waitpid(tls_pids[i], NULL, 0);
                            tls_pids[i] = -1;
                        }
                    }
                    break;
                }
                struct timespec ts2;
                ts2.tv_sec = 0; ts2.tv_nsec = 10000000;
                nanosleep(&ts2, NULL);
            }
            break;
        }

        if (died <= 0) {
            struct timespec ts;
            ts.tv_sec  = 0;
            ts.tv_nsec = 10000000; /* 10 ms */
            nanosleep(&ts, NULL);
            continue;
        }

        /* A worker exited — find its slot */
        uint64_t slot = nworkers;
        for (uint64_t i = 0; i < nworkers; i++) {
            if (tls_pids[i] == died) { slot = i; break; }
        }
        if (slot >= nworkers) continue;

        if (WIFSIGNALED(wstatus) ||
            (WIFEXITED(wstatus) && WEXITSTATUS(wstatus) != 0)) {
            /* Respawn worker using the shared listen socket */
            pid_t pid = fork();
            if (pid < 0) {
                tls_pids[slot] = -1;
                continue;
            }
            if (pid == 0) {
                tls_worker_loop(srv, r, tls->ssl_ctx);
                _exit(0);
            }
            tls_pids[slot] = pid;
        } else {
            tls_pids[slot] = -1;
        }
    }

    close(srv);
    return TK_HTTP_OK;
}

#else /* TK_HAVE_OPENSSL not defined — stub implementation */

struct TkTlsCtx { int _unused; };

TkTlsCtx *http_tls_ctx_new(const char *cert_path, const char *key_path)
{
    (void)cert_path; (void)key_path;
    fprintf(stderr,
            "http_tls_ctx_new: TLS not available: rebuild with OpenSSL\n");
    return NULL;
}

TkTlsCtx *http_tls_ctx_new_config(const TkTlsConfig *config)
{
    (void)config;
    fprintf(stderr,
            "http_tls_ctx_new_config: TLS not available\n");
    return NULL;
}

int http_tls_add_sni(TkTlsCtx *ctx, const char *hostname,
                     const char *cert_path, const char *key_path)
{
    (void)ctx; (void)hostname; (void)cert_path; (void)key_path;
    return -1;
}

int http_tls_set_ocsp_response(TkTlsCtx *ctx,
                                const uint8_t *data, size_t len)
{
    (void)ctx; (void)data; (void)len;
    return -1;
}

int http_tls_reload_cert(TkTlsCtx *ctx, const char *cert_path,
                          const char *key_path)
{
    (void)ctx; (void)cert_path; (void)key_path;
    return -1;
}

void http_tls_ctx_free(TkTlsCtx *ctx)
{
    free(ctx);
}

TkHttpErr http_serve_tls(TkHttpRouter *r, const char *host,
                          uint64_t port, TkTlsCtx *tls)
{
    (void)r; (void)host; (void)port; (void)tls;
    fprintf(stderr,
            "http_serve_tls: TLS not available: rebuild with OpenSSL\n");
    return TK_HTTP_ERR_BIND;
}

TkHttpErr http_serve_tls_workers(TkHttpRouter *r, const char *host,
                                  uint64_t port, TkTlsCtx *tls,
                                  uint64_t nworkers)
{
    (void)r; (void)host; (void)port; (void)tls; (void)nworkers;
    fprintf(stderr,
            "http_serve_tls_workers: TLS not available: rebuild with OpenSSL\n");
    return TK_HTTP_ERR_BIND;
}

#endif /* TK_HAVE_OPENSSL */

/* ── ETag generation and conditional requests (Story 27.1.13) ─────────── */

/*
 * http_etag_fnv — compute a weak ETag for a response body using FNV-1a.
 *
 * FNV-1a 64-bit:
 *   offset_basis = 14695981039346656037ULL
 *   prime        = 1099511628211ULL
 *   for each byte b: hash = (hash ^ b) * prime
 *
 * The result is formatted as W/"<16 hex digits>" and returned as a
 * heap-allocated, NUL-terminated string.  The caller owns the string.
 */
const char *http_etag_fnv(const char *body, size_t len)
{
    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    const unsigned char *p = (const unsigned char *)body;

    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= prime;
    }

    /* Format: W/"<16 hex chars>" — 3 + 16 + 1 + 1 = 21 chars + NUL */
    char *out = malloc(24);
    if (!out) return NULL;
    snprintf(out, 24, "W/\"%016llx\"", (unsigned long long)hash);
    return out;
}

/*
 * http_etag_matches — return 1 if etag matches the If-None-Match header value.
 *
 * Handles:
 *   - NULL in either argument: returns 0 (no match / cannot tell)
 *   - Wildcard "*": always matches (returns 1)
 *   - Exact string comparison of the ETag token within a comma-separated list.
 *     Surrounding whitespace is trimmed; each token is compared verbatim.
 */
int http_etag_matches(const char *etag, const char *if_none_match)
{
    if (!etag || !if_none_match) return 0;

    /* Wildcard */
    if (strcmp(if_none_match, "*") == 0) return 1;

    /* Walk the comma-separated list */
    const char *p = if_none_match;
    while (*p) {
        /* skip leading whitespace / commas */
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '\0') break;

        /* find end of this token (next comma or end) */
        const char *tok_start = p;
        while (*p && *p != ',') p++;
        const char *tok_end = p;

        /* trim trailing whitespace */
        while (tok_end > tok_start &&
               (*(tok_end - 1) == ' ' || *(tok_end - 1) == '\t'))
            tok_end--;

        size_t tok_len = (size_t)(tok_end - tok_start);
        if (tok_len == strlen(etag) &&
            memcmp(tok_start, etag, tok_len) == 0)
            return 1;
    }
    return 0;
}

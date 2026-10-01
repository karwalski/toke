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
#include "capabilities.h"   /* 124.4c2: net capability gate (server + client) */
#include "tk_bind.h"       /* 135.32/135.33: tk_bind_exclusive() */
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
#include <poll.h>     /* 135.32: bind probe */
#include <fcntl.h>    /* 135.32: bind probe */
#include <signal.h>
#include <time.h>
#include <zlib.h>
#ifdef TK_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>   /* 138.8: X509_VERIFY_PARAM_set1_ip_asc */
#endif

/* ── SIGPIPE: a client that leaves must not kill the server (127.144) ─ */

/*
 * THE DEFECT.  Nothing in src/ mentioned SIGPIPE, SO_NOSIGPIPE or
 * MSG_NOSIGNAL — zero occurrences in the whole tree — so every write to a
 * client socket carried the default disposition, which terminates the
 * process.  One client that sends a request and resets before reading is
 * enough.  Measured on this tree, macOS/arm64, one abandoned request:
 *
 *   http.serveworkers(p;1)   SERVER EXIT=141          (128+SIGPIPE)
 *   http.servetls(p;…)       SERVER EXIT=141
 *   http.serve(p)            parent survives; BOTH worker pids replaced —
 *                            one worker killed per abandoned request, and
 *                            the supervisor respawns into the next one.
 *
 * This is what a browser does on navigation: it has read part of a response,
 * the user clicks away, the socket closes with data still queued, and the
 * peer sees RST.  It also corrupted the 127.72 leak measurement, because a
 * worker that is killed and respawned carries its leaked memory away with it.
 *
 * THE CHOICE, AND WHY.  Three mechanisms were on the table.
 *
 *   signal(SIGPIPE, SIG_IGN) at init — rejected as the primary mechanism.
 *     It is process-wide: it changes the error path of every write in every
 *     toke program, not just the server's.  A program that writes to a
 *     closed pipe would stop dying and start looping on EPIPE, and because
 *     an *ignored* disposition survives execve() as well as fork(), every
 *     child a toke program spawns would inherit it too.  That is a language-
 *     wide semantic change to pay for a server bug.
 *
 *   SO_NOSIGPIPE on the accepted socket — the primary mechanism where it
 *     exists (macOS and the BSDs).  It is per-socket, so nothing outside the
 *     connection is affected, and because it is a socket property it also
 *     covers writes this file does not make: OpenSSL's BIO writes through
 *     plain write(2), and SO_NOSIGPIPE covers those too.
 *
 *   MSG_NOSIGNAL on each send — the primary mechanism on Linux, which has no
 *     SO_NOSIGPIPE.  It is per-call, so it only covers calls we make.
 *
 * So: both, applied together, and http_write_all is the single funnel that
 * carries the flag.  The one gap the pair cannot close is OpenSSL's own
 * write(2) on a platform without SO_NOSIGPIPE — i.e. TLS on Linux.  There,
 * and only there, SIGPIPE is set to SIG_IGN, and only from inside
 * http_serve_tls_workers(), which never returns.  The cost is stated in
 * docs/stdlib/http.md under "What servetls does to SIGPIPE on Linux".
 */

#ifdef MSG_NOSIGNAL
#define HTTP_SEND_FLAGS MSG_NOSIGNAL
#else
#define HTTP_SEND_FLAGS 0
#endif

void http_sock_nosigpipe(int fd)
{
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
    (void)fd;
#endif
}

/*
 * http_write_all — write the whole buffer to a client socket, never raising
 * SIGPIPE, and never reporting a short write as success.
 *
 * send() rather than write() so MSG_NOSIGNAL can be passed.  http_handle_fd()
 * is an embedding/test hook that is also handed pipes and regular files, on
 * which send() fails with ENOTSOCK — that one errno falls back to write(),
 * which is safe because a non-socket cannot be the RST case this exists for.
 *
 * Returns the byte count on success and -1 on error (EPIPE, ECONNRESET);
 * errno is left set.  Partial writes are retried: a blocking socket
 * interrupted by a signal can return short, and the previous code treated
 * every write() as complete.
 */
ssize_t http_write_all(int fd, const void *buf, size_t len)
{
    const char *p   = (const char *)buf;
    size_t      off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, HTTP_SEND_FLAGS);
        if (n < 0 && errno == ENOTSOCK)
            n = write(fd, p + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        off += (size_t)n;
    }
    return (ssize_t)off;
}

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

/* ── Custom 404 body (121.62) ───────────────────────────────────────────
 *
 * http.setnotfound's body.  It used to live only in tk_web_glue.c, read by
 * tk_static_dispatch -- which runs only for a path that matched a registered
 * getstatic route -- so every genuinely unrouted path got the plain
 * "Not Found" below and the set body was never served.  It now lives here and
 * is what all three dispatchers (HTTP/1.1, TLS, HTTP/2) answer an unrouted
 * path with.  The string is copied: the caller's may be freed.  NULL (the
 * default) keeps the plain-text "Not Found". */
static char *g_notfound_body = NULL;
static StrPair g_notfound_ct = { "Content-Type", "text/html; charset=utf-8" };

int http_set_notfound(const char *body)
{
    char *copy = NULL;
    if (body) {
        copy = strdup(body);
        if (!copy) return -1;        /* keep the previous body */
    }
    free(g_notfound_body);
    g_notfound_body = copy;
    return 0;
}

const char *http_notfound_body(void) { return g_notfound_body; }

static Res notfound_res(void)
{
    Res r = make_res(404, "Not Found");
    if (g_notfound_body) {
        r.body = g_notfound_body;
        r.headers.data = &g_notfound_ct;
        r.headers.len  = 1;
    }
    return r;
}

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

/* ── Request-target path component (story 127.100) ──────────── */

/*
 * http_path_only — copy the request target up to the first '?' or '#'.
 *
 * Route matching and static-file resolution both need the path alone.  Before
 * this existed every dispatch loop matched patterns against the raw target,
 * so http.getstatic("/style.css") never matched "/style.css?v=abc" and
 * http.servedir's wildcard handler built the filename "…/style.css?v=abc",
 * which no filesystem has.  Either way: 404 on a versioned asset URL.
 *
 * req.path itself is deliberately left raw — it is the only access a toke
 * handler has to the query string — so the stripping happens at each point
 * of use, not in parse_request.
 */
const char *http_path_only(const char *target, char *buf, size_t buflen)
{
    if (!target) return "";
    size_t n = strcspn(target, "?#");
    if (target[n] == '\0') return target;   /* no query, no fragment */
    if (!buf || buflen == 0) return target;
    if (n > buflen - 1) n = buflen - 1;
    memcpy(buf, target, n);
    buf[n] = '\0';
    return buf;
}

/* ── Pattern matching ───────────────────────────────────────────────── */

/*
 * http_free_params — release the key/value pairs http_match_pattern bound
 * into `out` and reset the count (story 127.72).
 *
 * Every `:param` segment costs two malloc()s.  Nothing released them: the
 * routing loop dropped non-selected bindings with a bare `pc = 0`, the
 * selected route's bindings were overwritten by a second match, and the
 * bindings the handler actually used were abandoned when the request ended.
 * A `:param` route therefore grew the server on every request while a plain
 * route — which allocates nothing — showed no growth at all, which is why it
 * went unread for so long.  Measured before the fix on macOS/arm64 with one
 * four-parameter route: 266.4 bytes per request, and 764.6 with four
 * path-matching routes ahead of the winner.
 *
 * Safe on an already-empty block, and leaves `out` re-usable.
 */
void http_free_params(StrPair *out, int *cnt)
{
    if (!out || !cnt) return;
    for (int i = 0; i < *cnt; i++) {
        free((void *)(uintptr_t)out[i].key);
        free((void *)(uintptr_t)out[i].val);
        out[i].key = NULL;
        out[i].val = NULL;
    }
    *cnt = 0;
}

/*
 * http_defer_free / http_run_deferred_frees — release a pointer once the
 * response has been written (story 127.72).
 *
 * WHY THIS EXISTS, AND IT IS NOT DECORATION.  A `:param` value belongs to the
 * matcher, but a handler may hand that exact pointer back as the response
 * body: `http.resok(http.param(req;"name"))` is the obvious way to write a
 * `:param` handler, and http_Res_ok stores the pointer without copying it.
 * Freeing the bindings the moment the handler returns therefore turns the
 * body into freed memory — measured while fixing this story: every response
 * came back EMPTY, where the same handler with the value copied into a new
 * string answered correctly.  Trading a leak for a use-after-free is not a
 * fix, so the selected route's bindings are handed to this list and released
 * at the same point free_req() releases the parsed request: after the
 * response, the gzip buffer and the log line have all been written.
 *
 * The dispatchers in http.c keep their block on the stack and could simply
 * free late; the handler dispatchers in tk_web_glue.c cannot, because their
 * block goes out of scope while the Res they return is still travelling, so
 * the list is shared and exported.
 *
 * Capacity is twice what one request can produce: a selected route's bindings
 * from http.c plus a handler dispatcher's is at most 2 x HTTP_MAX_PARAMS keys
 * and values, so overflow is unreachable by construction.  Should it happen
 * anyway — a drain missed because a connection was torn down between the
 * handler and the write, twice over — the pointer is DROPPED, not freed.
 * Freeing it would be freeing something the response in flight may be about
 * to write, which is the exact failure this list exists to prevent; a bounded
 * leak on an unreachable path is the cheaper of the two.
 */
/*
 * 127.148 raised this.  The 4 x HTTP_MAX_PARAMS above is exactly what the
 * two parameter blocks can produce, with nothing left over, so the moment a
 * handler started handing its response body to this list as well the worst
 * case could overflow and silently DROP — i.e. leak — the body.  The extra
 * 16 slots are the per-request handler budget: the largest producer was the
 * vhost static handler at nine pointers (body, five header values, two
 * header arrays, the StrPair block).  136.69 removed that handler with the
 * unused vhost surface; the budget is kept as headroom.
 */
#define HTTP_MAX_DEFERRED (4 * HTTP_MAX_PARAMS + 16)

static void *g_deferred[HTTP_MAX_DEFERRED];
static int   g_deferred_n;

void http_defer_free(void *p)
{
    if (!p) return;
    if (g_deferred_n >= HTTP_MAX_DEFERRED) return;   /* drop, never free */
    g_deferred[g_deferred_n++] = p;
}

void http_run_deferred_frees(void)
{
    for (int i = 0; i < g_deferred_n; i++) free(g_deferred[i]);
    g_deferred_n = 0;
}

/*
 * http_defer_params — hand `out`'s bindings to the deferred list and reset
 * the count.  Use this for the bindings the HANDLER saw; use
 * http_free_params for bindings no handler ever received, which nothing can
 * alias and which are cheaper to release at once.
 */
void http_defer_params(StrPair *out, int *cnt)
{
    if (!out || !cnt) return;
    for (int i = 0; i < *cnt; i++) {
        http_defer_free((void *)(uintptr_t)out[i].key);
        http_defer_free((void *)(uintptr_t)out[i].val);
        out[i].key = NULL;
        out[i].val = NULL;
    }
    *cnt = 0;
}

/*
 * http_match_pattern — match a route pattern against a request path,
 * binding each `:param` segment into `out` (at most HTTP_MAX_PARAMS).
 * Returns 1 on a match, 0 otherwise.
 *
 * On ANY non-match — a literal mismatch part way through, a length
 * mismatch at the end, or a failed malloc — the bindings made so far are
 * released before returning, so a caller only ever has to free after a 1.
 *
 * 127.72: this used to exist twice, once here and once as a verbatim static
 * copy `tk_match_pattern` in tk_web_glue.c.  Both leaked, identically, and
 * the copy had the HTTP_MAX_PARAMS bound this one was missing — so the two
 * descriptions of one behaviour had already drifted in the direction of a
 * stack overflow on a route with more than 32 parameters.  One function now.
 */
int http_match_pattern(const char *pat, const char *path,
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
            char *key = malloc(kn + 1);
            if (!key) { http_free_params(out, cnt); return 0; }
            memcpy(key, ks, kn); key[kn] = '\0';
            const char *vs = path; while (*path && *path != '/') path++;
            size_t vn = (size_t)(path - vs);
            char *val = malloc(vn + 1);
            if (!val) { free(key); http_free_params(out, cnt); return 0; }
            memcpy(val, vs, vn); val[vn] = '\0';
            /* Bound the write: `out` is a caller-owned HTTP_MAX_PARAMS block
             * and this copy used to write past it without checking.
             * 127.146: a route with more `:param` segments than that is a
             * no-match, as in router.c.  It used to match with the extra
             * bindings dropped, so the handler read "" for them. */
            if (*cnt >= HTTP_MAX_PARAMS) {
                free(key); free(val); http_free_params(out, cnt); return 0;
            }
            out[(*cnt)].key = key; out[(*cnt)].val = val; (*cnt)++;
        } else {
            if (*pat != *path) { http_free_params(out, cnt); return 0; }
            pat++; path++;
        }
    }
    if (*pat == '\0' && *path == '\0') return 1;
    http_free_params(out, cnt);
    return 0;
}

/* ── Request parsing ────────────────────────────────────────────────── */

#define HTTP_MAX_REQ_HEADERS 64

/*
 * count_req_headers — how many `Key: value` header lines the header block of
 * `raw` actually carries, capped at HTTP_MAX_REQ_HEADERS.
 *
 * Story 127.65: parse_request used to allocate HTTP_MAX_REQ_HEADERS StrPairs
 * (1024 B on a 64-bit target) for every request regardless of how many
 * headers arrived.  A typical request carries 4-8, so ~90% of that block was
 * never written.  Sizing the block to the real count is a straight win now
 * that the block is freed per request.
 */
static int count_req_headers(const char *raw)
{
    const char *p = strstr(raw, "\r\n");
    int n = 0;
    if (!p) return 0;
    p += 2;                       /* skip the request line */
    while (n < HTTP_MAX_REQ_HEADERS) {
        const char *e = strstr(p, "\r\n");
        if (!e || e == p) break;  /* end of buffer, or the blank line */
        if (memchr(p, ':', (size_t)(e - p))) n++;
        p = e + 2;
    }
    return n;
}

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
    /* 127.65: size the header block to the headers that actually arrived. */
    int hmax = count_req_headers(raw);
    StrPair *hdrs = hmax > 0 ? malloc((size_t)hmax * sizeof(StrPair)) : NULL;
    int hc = 0;
    char *p = nl + 2;
    while (hdrs) {
        char *e = strstr(p, "\r\n"); if (!e || e == p) break;
        *e = '\0';
        char *col = strchr(p, ':');
        if (col && hc < hmax) {
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

/*
 * free_req — release everything parse_request() allocated.
 *
 * Story 127.65: handle_connection()'s keep-alive loop called parse_request()
 * once per request and never released the result, so a long-lived server grew
 * by ~1.1-1.3 KB per request with no plateau — reproducible on a 404 path
 * where no toke code runs at all, which is what rules out the runtime arena.
 *
 * req.params is deliberately NOT touched: the dispatchers point it at a
 * stack array of StrPair whose key/val strings http_match_pattern owns, and
 * each dispatcher releases them with http_free_params as soon as the handler
 * has returned (127.72 — before that, nobody released them at all).
 */
static void free_req(Req *req)
{
    if (!req) return;
    free((void *)(uintptr_t)req->method);
    free((void *)(uintptr_t)req->path);
    free((void *)(uintptr_t)req->body);
    for (uint64_t i = 0; i < req->headers.len; i++) {
        free((void *)(uintptr_t)req->headers.data[i].key);
        free((void *)(uintptr_t)req->headers.data[i].val);
    }
    free(req->headers.data);
    memset(req, 0, sizeof(*req));
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
        if (http_write_all(fd, hdr, (size_t)hl) < 0) return -1;

        /* Write chunk data */
        if (http_write_all(fd, data + off, csz) < 0) return -1;

        /* Write chunk trailer \r\n */
        if (http_write_all(fd, "\r\n", 2) < 0) return -1;

        off += csz;
    }

    /* Terminating chunk */
    if (http_write_all(fd, "0\r\n\r\n", 5) < 0) return -1;

    return 0;
}

/* 121.44: http_chunked_read(fd) was here.  It read a chunked body FRESH
 * from the socket, so it could not see body bytes already buffered with the
 * headers (audit-120 HTT-02), and it read the peer's stream a byte at a time
 * with no deadline.  No server path had called it since 121.17; it is gone
 * from http.h so no C caller picks it up.  The server decodes chunked bodies
 * in place with http_read_chunked(), below. */

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
        case 501: return "Not Implemented";
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
        if (ln > 0) http_write_all(fd, line, (size_t)ln);
    }
}

/* The hardened headers every response carries, as "Name: value\r\n" lines;
 * the last (HSTS) only over TLS. */
static const char *const sec_hdr_lines[] = {
    "X-Content-Type-Options: nosniff\r\n",
    "X-Frame-Options: DENY\r\n",
    "Referrer-Policy: strict-origin-when-cross-origin\r\n",
    "Strict-Transport-Security: max-age=63072000; includeSubDomains; preload\r\n",
};

/* res_sets_header — 1 when the handler's response (res may be NULL) already
 * carries the field that the line "Name: ..." sets (names compare without
 * regard to case, RFC 9110 5.1). */
static int res_sets_header(const Res *res, const char *line)
{
    if (!res) return 0;
    size_t n = (size_t)(strchr(line, ':') - line);
    for (uint64_t i = 0; i < res->headers.len; i++) {
        const char *k = res->headers.data[i].key;
        if (k && strlen(k) == n && strncasecmp(k, line, n) == 0) return 1;
    }
    return 0;
}

/* write_security_headers — emit hardened security headers on every response.
 * 121.50 (3): a field the handler set itself is not sent again -- the
 * handler's value wins, as it does on HTTP/2 (h2_send_response).  It used to
 * be sent twice (e.g. X-Frame-Options: SAMEORIGIN then DENY), and a browser
 * facing two X-Frame-Options values applies neither. */
static void write_security_headers(int fd, const Res *res)
{
    for (size_t i = 0; i < 3; i++)
        if (!res_sets_header(res, sec_hdr_lines[i]))
            http_write_all(fd, sec_hdr_lines[i], strlen(sec_hdr_lines[i]));
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
    if (n > 0) http_write_all(fd, buf, (size_t)n);
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
        http_write_all(fd, hdr, (size_t)hl);
        write_res_headers(fd, res);
        write_security_headers(fd, &res);
        write_cors_headers(fd);
        http_write_all(fd, "\r\n", 2);
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
        http_write_all(fd, hdr, (size_t)hl);
        write_res_headers(fd, res);
        write_security_headers(fd, &res);
        write_cors_headers(fd);
        http_write_all(fd, "\r\n", 2);
        if (bl) http_write_all(fd, res.body, bl);
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

/* ── Request framing, shared by the plain and TLS loops (121.17) ─────────
 *
 * audit-120 HTT-02 / HTT-03 / HTT-06.  Both connection loops used to frame a
 * request with their own copy of the same scan, and the copies were wrong in
 * the same ways:
 *
 *   - The header read pulls whatever the socket has.  A chunked body that
 *     arrived in the same segment as its headers was discarded, because the
 *     decoder then read FRESH from the socket, mid-body; a failed decode kept
 *     the connection alive, so the rest of the chunk stream was parsed as the
 *     next request.  The bytes of a pipelined request that shared a segment
 *     with the one before it were thrown away the same way.  The TLS loop did
 *     not decode chunked bodies at all: over TLS the chunk stream WAS the next
 *     request.
 *   - Content-Length was the first matching line only, needed the colon
 *     straight after the name, and went through atoll() ("5x" is 5, "+5" is
 *     5).  Transfer-Encoding was the first header's prefix only, so
 *     "gzip, chunked" was a request with no body.  Each is a place where this
 *     server and a proxy in front of it can disagree about where a request
 *     ends, which is request smuggling.
 *   - The only clock was SO_RCVTIMEO, which restarts on every byte, so one
 *     byte every (timeout - 1) seconds held a worker for max_header x timeout.
 *
 * Now one reader frames every request on both loops.  Bytes past the end of a
 * request stay in the buffer and start the next one.  The header block is
 * validated as RFC 9112 requires (sections 5, 6.1, 6.3) before any byte of
 * body is read, and every request has a deadline: the header block must
 * arrive within timeout_secs of the request starting, and the body within
 * timeout_secs plus one second per HTTP_MIN_BODY_BPS bytes it announced.
 */

/* The slowest body rate a request may use: its deadline grows by one second
 * per this many bytes.  1 MiB (the default max_body) therefore gets 17
 * minutes, and a 100-byte body gets the bare timeout. */
#define HTTP_MIN_BODY_BPS   1024u

/* Longest chunk-size line (hex + extensions) or trailer line accepted.  RFC
 * 9112 sets no limit; 256 is well past any real chunk extension. */
#define HTTP_CHUNK_LINE_MAX 256u

/* Extra bytes in the per-connection buffer beyond max_header + max_body, so
 * that chunk-size and trailer lines always have room to land. */
#define HTTP_FRAME_SLACK    (2u * HTTP_CHUNK_LINE_MAX + 8u)

/* http_read_request result: close the connection without answering. */
#define HTTP_RR_CLOSE (-1)

/* 121.29 REL-02: the per-connection buffer starts at this size and grows
 * on demand (doubling) up to http_frame_cap().  It used to be allocated at
 * the full max_header + max_body + slack -- about 1 MB with the defaults --
 * for every connection, so N idle keep-alive connections held N MB whether
 * or not any of them ever sent a body.  After a request that grew it, the
 * next request on the connection shrinks it back when what is buffered fits. */
#define HTTP_IN_INIT_CAP    16384u

typedef struct {
    int    fd;
    void  *ssl;         /* SSL* on the TLS loop, NULL on the plain one      */
    char  *raw;         /* cap bytes; grows up to maxcap (121.29 REL-02)    */
    size_t cap;
    size_t maxcap;      /* max_header + max_body + HTTP_FRAME_SLACK         */
    size_t used;        /* bytes held in raw                                */
    size_t next_off;    /* where the next request's bytes start in raw      */
    char   next_first;  /* raw[next_off] before the NUL terminator hid it   */
    long   deadline_ms; /* monotonic; the current phase must end by then    */
} HttpIn;

static size_t http_frame_cap(void)
{
    return (size_t)srv_limits.max_header + (size_t)srv_limits.max_body
         + HTTP_FRAME_SLACK;
}

/* httpin_open -- allocate the reader's first buffer: HTTP_IN_INIT_CAP bytes,
 * or the whole frame cap if the limits make that smaller.  Returns 0, or -1
 * when it cannot be allocated. */
static int httpin_open(HttpIn *in, int fd, void *ssl)
{
    memset(in, 0, sizeof *in);
    in->fd = fd; in->ssl = ssl;
    in->maxcap = http_frame_cap();
    in->cap = in->maxcap < HTTP_IN_INIT_CAP ? in->maxcap : HTTP_IN_INIT_CAP;
    in->raw = malloc(in->cap);
    return in->raw ? 0 : -1;
}

/* httpin_grow -- make room for at least `want` bytes plus the terminator,
 * doubling, never past maxcap.  Returns 0, or -1 when the buffer is already
 * at maxcap or realloc fails (the old buffer is kept either way). */
static int httpin_grow(HttpIn *in, size_t want)
{
    if (want + 1 <= in->cap) return 0;
    if (in->cap >= in->maxcap) return -1;
    size_t nc = in->cap;
    while (nc < want + 1 && nc < in->maxcap)
        nc = nc > in->maxcap / 2 ? in->maxcap : nc * 2;
    char *nr = realloc(in->raw, nc);
    if (!nr) return -1;
    in->raw = nr; in->cap = nc;
    return 0;
}

static long http_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + (long)(ts.tv_nsec / 1000000L);
}

/* ── Lingering close (121.52) ─────────────────────────────────────────────
 *
 * When the server ends a connection because of what the peer sent -- an
 * HTTP/1.1 400/413, an HTTP/2 GOAWAY -- the peer may still be writing: the
 * rest of an oversized body, the rest of a CONTINUATION flood.  close() with
 * unread bytes in the receive queue, or with more arriving after it, makes
 * the kernel answer with a TCP RST, and an RST lets the peer's stack discard
 * our final response or GOAWAY before its application reads it.  C491's
 * continuation-flood case (1.6 MB unread) saw "closed" instead of GOAWAY in
 * 6 of 20 runs under CPU load.
 *
 * So: half-close our side (the peer reads our last bytes, then EOF), then
 * read and discard what the peer still sends until it closes too -- bounded
 * in time and in bytes, so a peer that never stops cannot hold a worker.
 * Not after a 408: that peer is slow by definition, it has at most a trickle
 * in flight, and lingering would only extend the hold the deadline ended. */
#define HTTP_LINGER_MS        1000L              /* whole linger, wall time */
#define HTTP_LINGER_MAX_BYTES (8u * 1024u * 1024u)  /* then give up and RST  */

static void http_linger_close(int fd)
{
    char sink[16384];
    size_t got = 0;
    long end = http_now_ms() + HTTP_LINGER_MS;

    if (shutdown(fd, SHUT_WR) == 0) {
        while (got < HTTP_LINGER_MAX_BYTES) {
            long left = end - http_now_ms();
            if (left <= 0) break;
            struct pollfd p;
            p.fd = fd; p.events = POLLIN; p.revents = 0;
            int pr = poll(&p, 1, (int)left);
            if (pr < 0 && errno == EINTR) continue;
            if (pr <= 0) break;
            ssize_t n = recv(fd, sink, sizeof sink, MSG_DONTWAIT);
            if (n < 0 && (errno == EINTR || errno == EAGAIN ||
                          errno == EWOULDBLOCK)) continue;
            if (n <= 0) break;          /* peer closed, or an error */
            got += (size_t)n;
        }
    }
    close(fd);
}

/* http_wait — wait until fd is readable (or writable) or wait_ms passes.
 * Returns 1 ready, 0 timed out, -1 error. */
static int http_wait(int fd, short events, long wait_ms)
{
    for (;;) {
        struct pollfd p;
        p.fd = fd; p.events = events; p.revents = 0;
        int r = poll(&p, 1, (int)(wait_ms > 0 ? wait_ms : 0));
        if (r < 0 && errno == EINTR) continue;
        return r < 0 ? -1 : (r > 0 ? 1 : 0);
    }
}

/* httpin_read — read at most n bytes before in->deadline_ms, and never wait
 * longer than timeout_secs for any one read.
 * Returns bytes read (> 0), 0 at end of stream, -1 on error, -2 on timeout. */
static ssize_t httpin_read(HttpIn *in, char *buf, size_t n)
{
    for (;;) {
        long left = in->deadline_ms - http_now_ms();
        if (left <= 0) return -2;
        long idle = (long)srv_limits.timeout_secs * 1000L;
        long wait = left < idle ? left : idle;
#ifdef TK_HAVE_OPENSSL
        if (in->ssl) {
            /* The socket is non-blocking for the whole read phase (see
             * http_read_request), so SSL_read cannot sit inside a record
             * that a client trickles one byte at a time. */
            int r = SSL_read((SSL *)in->ssl, buf, (int)(n > 0x7fffffff ? 0x7fffffff : n));
            if (r > 0) return r;
            int e = SSL_get_error((SSL *)in->ssl, r);
            if (e == SSL_ERROR_ZERO_RETURN) return 0;
            short ev;
            if (e == SSL_ERROR_WANT_READ)       ev = POLLIN;
            else if (e == SSL_ERROR_WANT_WRITE) ev = POLLOUT;
            else return (r == 0) ? 0 : -1;
            int w = http_wait(in->fd, ev, wait);
            if (w == 0) return -2;
            if (w < 0)  return -1;
            continue;
        }
#endif
        int w = http_wait(in->fd, POLLIN, wait);
        if (w == 0) return -2;
        if (w < 0)  return -1;
        ssize_t r = read(in->fd, buf, n);
        if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        return r;
    }
}

/* httpin_fill — append more bytes to raw, up to `limit` bytes held in all.
 * The buffer grows toward limit as it fills (121.29 REL-02); a limit past
 * maxcap - 1 means maxcap - 1.  Returns httpin_read's result, or -1 when the
 * limit is reached or the buffer cannot grow. */
static ssize_t httpin_fill(HttpIn *in, size_t limit)
{
    if (limit > in->maxcap - 1) limit = in->maxcap - 1;
    if (in->used >= limit) return -1;
    if (in->used >= in->cap - 1) {
        size_t want = in->used + (in->used < 4096 ? 4096 : in->used);
        if (want > limit) want = limit;
        if (httpin_grow(in, want) != 0) return -1;
    }
    if (limit > in->cap - 1) limit = in->cap - 1;
    ssize_t r = httpin_read(in, in->raw + in->used, limit - in->used);
    if (r > 0) { in->used += (size_t)r; in->raw[in->used] = '\0'; }
    return r;
}

/* find_crlfcrlf — offset of the first "\r\n\r\n" in buf[0..len), or -1. */
static long find_crlfcrlf(const char *buf, size_t len)
{
    for (size_t i = 0; i + 3 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return (long)i;
    return -1;
}

/* find_crlf — offset of the first "\r\n" in buf[0..len), or -1. */
static long find_crlf(const char *buf, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++)
        if (buf[i] == '\r' && buf[i+1] == '\n') return (long)i;
    return -1;
}

static int is_ows(char c) { return c == ' ' || c == '\t'; }

/* te_token — handle one Transfer-Encoding coding.  *nchunked counts
 * "chunked"; *other is set by any other coding; *last_chunked says whether
 * the most recent coding was "chunked". */
static void te_token(const char *t, size_t n, int *nchunked, int *other,
                     int *last_chunked)
{
    while (n && is_ows(*t)) { t++; n--; }
    while (n && is_ows(t[n-1])) n--;
    if (!n) return;                          /* empty list element: ignored */
    if (n == 7 && strncasecmp(t, "chunked", 7) == 0) {
        (*nchunked)++; *last_chunked = 1;
    } else {
        *other = 1; *last_chunked = 0;
    }
}

/*
 * http_frame_check — validate the framing headers of the header block
 * raw[0..hlen) (hlen includes the blank line).  Sets *cl (-1 when absent)
 * and *chunked.  Returns 0, or the status to answer with:
 *
 *   400  a header line with no colon, an empty name, or whitespace before
 *        the colon (RFC 9112 5.1: MUST be rejected); a folded line (5.2);
 *        a Content-Length that is not 1*DIGIT, or two that differ (6.3 p5);
 *        Transfer-Encoding together with Content-Length (6.1: a server MAY
 *        reject, and a proxy may have framed it by either); "chunked" that is
 *        not the final coding, or applied twice (6.1: MUST respond 400)
 *   413  a Content-Length above max_body
 *   501  any transfer coding other than chunked -- none is implemented, and
 *        reading such a body as if it were plain would be a framing guess
 */
static int http_frame_check(const char *raw, size_t hlen, long long *cl,
                            int *chunked)
{
    *cl = -1; *chunked = 0;
    int te_seen = 0, nchunked = 0, other = 0, last_chunked = 0;
    unsigned long long clv = 0; int cl_seen = 0;
    long rl = find_crlf(raw, hlen);
    if (rl < 0) return 400;
    size_t p = (size_t)rl + 2;
    while (p < hlen) {
        long e = find_crlf(raw + p, hlen - p);
        if (e < 0) return 400;
        size_t len = (size_t)e;
        if (len == 0) break;                          /* the blank line */
        const char *line = raw + p;
        if (is_ows(line[0])) return 400;              /* obs-fold */
        const char *colon = memchr(line, ':', len);
        if (!colon || colon == line) return 400;
        size_t nlen = (size_t)(colon - line);
        for (size_t i = 0; i < nlen; i++) {
            unsigned char c = (unsigned char)line[i];
            if (c <= 0x20 || c == 0x7f) return 400;   /* space before colon */
        }
        const char *v = colon + 1;
        size_t vlen = len - nlen - 1;
        while (vlen && is_ows(*v)) { v++; vlen--; }
        while (vlen && is_ows(v[vlen-1])) vlen--;
        if (nlen == 14 && strncasecmp(line, "Content-Length", 14) == 0) {
            if (vlen == 0 || vlen > 19) return 400;
            unsigned long long n = 0;
            for (size_t i = 0; i < vlen; i++) {
                if (v[i] < '0' || v[i] > '9') return 400;
                n = n * 10u + (unsigned)(v[i] - '0');
            }
            if (cl_seen && n != clv) return 400;
            cl_seen = 1; clv = n;
        } else if (nlen == 17 && strncasecmp(line, "Transfer-Encoding", 17) == 0) {
            te_seen = 1;
            size_t s = 0;
            for (size_t i = 0; i <= vlen; i++) {
                if (i == vlen || v[i] == ',') {
                    te_token(v + s, i - s, &nchunked, &other, &last_chunked);
                    s = i + 1;
                }
            }
        }
        p += len + 2;
    }
    if (te_seen) {
        if (cl_seen) return 400;
        if (nchunked == 0 && !other) return 400;       /* empty value */
        if (!last_chunked || nchunked > 1) return 400;
        if (other) return 501;
        *chunked = 1;
        return 0;
    }
    if (cl_seen) {
        if (clv > (unsigned long long)srv_limits.max_body) return 413;
        *cl = (long long)clv;
    }
    return 0;
}

/*
 * http_read_chunked — decode the chunked body that starts at raw[hlen], in
 * place: decoded bytes are written behind the read cursor, which the chunk
 * framing always keeps ahead.  On success the body is raw[hlen..hlen+*out)
 * and *next is where the bytes after the last CRLF begin.
 * Returns 0 or the status to answer with (400, 408, 413; HTTP_RR_CLOSE when
 * the peer went away).
 */
static int http_read_chunked(HttpIn *in, size_t hlen, size_t *out, size_t *next)
{
    enum { SIZE, DATA, DATA_CRLF, TRAILER } st = SIZE;
    size_t pos = hlen, o = 0, csz = 0, trailer = 0;
    for (;;) {
        size_t avail = in->used - pos;
        int need_more = 0;
        if (st == SIZE || st == TRAILER) {
            long e = find_crlf(in->raw + pos, avail);
            if (e < 0) {
                if (avail > HTTP_CHUNK_LINE_MAX) return 400;
                need_more = 1;
            } else if (st == TRAILER) {
                if (e == 0) { *out = o; *next = pos + 2; return 0; }
                trailer += (size_t)e + 2;
                if ((size_t)e > HTTP_CHUNK_LINE_MAX ||
                    trailer > (size_t)srv_limits.max_header) return 431;
                pos += (size_t)e + 2;
            } else {
                const char *l = in->raw + pos;
                size_t n = (size_t)e, i = 0;
                if ((size_t)e > HTTP_CHUNK_LINE_MAX) return 400;
                csz = 0;
                for (; i < n && isxdigit((unsigned char)l[i]); i++) {
                    if (i >= 16) return 413;           /* > 2^64 */
                    csz = csz * 16u + (size_t)(isdigit((unsigned char)l[i])
                          ? l[i] - '0' : (tolower((unsigned char)l[i]) - 'a' + 10));
                }
                if (i == 0) return 400;                 /* no chunk-size */
                while (i < n && is_ows(l[i])) i++;
                if (i < n && l[i] != ';') return 400;   /* junk after size */
                if (csz > (size_t)srv_limits.max_body ||
                    o + csz > (size_t)srv_limits.max_body) return 413;
                pos += n + 2;
                st = csz ? DATA : TRAILER;
            }
        } else if (st == DATA) {
            if (avail == 0) need_more = 1;
            else {
                size_t take = avail < csz ? avail : csz;
                memmove(in->raw + hlen + o, in->raw + pos, take);
                o += take; pos += take; csz -= take;
                if (!csz) st = DATA_CRLF;
            }
        } else {                                        /* DATA_CRLF */
            if (avail < 2) need_more = 1;
            else {
                if (in->raw[pos] != '\r' || in->raw[pos+1] != '\n') return 400;
                pos += 2; st = SIZE;
            }
        }
        if (!need_more) continue;
        /* Out of room: slide the unread input down onto the decoded body
         * (only once the buffer is at its largest; before that it grows). */
        if (in->used >= in->maxcap - 1 && pos > hlen + o) {
            memmove(in->raw + hlen + o, in->raw + pos, in->used - pos);
            in->used -= pos - (hlen + o);
            pos = hlen + o;
        }
        ssize_t r = httpin_fill(in, in->maxcap - 1);
        if (r == -2) return 408;
        if (r <= 0) return HTTP_RR_CLOSE;
    }
}

/*
 * http_read_request — frame the next request into in->raw, carrying over any
 * bytes of it that arrived with the previous one.  On success returns 0 and
 * raw holds "<header block><body>\0" with a chunked body already decoded;
 * otherwise the status to answer with before closing, or HTTP_RR_CLOSE to
 * close without answering (the peer went away, or an idle keep-alive
 * connection timed out).
 */
static int http_read_request_(HttpIn *in, int requests)
{
    /* Carry the bytes of this request that arrived with the last one. */
    if (in->next_off < in->used) {
        in->raw[in->next_off] = in->next_first;
        memmove(in->raw, in->raw + in->next_off, in->used - in->next_off);
        in->used -= in->next_off;
    } else {
        in->used = 0;
    }
    in->next_off = 0;
    /* 121.29 REL-02: a buffer grown by an earlier request goes back to its
     * initial size when what is carried over fits (a failed shrink keeps
     * the larger buffer). */
    if (in->cap > HTTP_IN_INIT_CAP && in->used < HTTP_IN_INIT_CAP &&
        HTTP_IN_INIT_CAP <= in->maxcap) {
        char *nr = realloc(in->raw, HTTP_IN_INIT_CAP);
        if (nr) { in->raw = nr; in->cap = HTTP_IN_INIT_CAP; }
    }
    in->raw[in->used] = '\0';

    size_t mh = (size_t)srv_limits.max_header;
    long now = http_now_ms();
    if (requests > 0 && in->used == 0) {
        /* Idle keep-alive: wait for the first byte, then start the clock. */
        in->deadline_ms = now + (long)KEEPALIVE_IDLE_TIMEOUT_S * 1000L;
        ssize_t r = httpin_fill(in, mh);
        if (r <= 0) return HTTP_RR_CLOSE;
        now = http_now_ms();
    }
    in->deadline_ms = now + (long)srv_limits.timeout_secs * 1000L;

    long term;
    for (;;) {
        term = find_crlfcrlf(in->raw, in->used < mh ? in->used : mh);
        if (term >= 0) break;
        if (in->used >= mh) return 431;
        ssize_t r = httpin_fill(in, mh);
        if (r == -2) return (requests == 0 || in->used > 0) ? 408 : HTTP_RR_CLOSE;
        if (r <= 0) return HTTP_RR_CLOSE;
    }
    size_t hlen = (size_t)term + 4;

    long long cl; int chunked;
    int st = http_frame_check(in->raw, hlen, &cl, &chunked);
    if (st) return st;

    size_t expect = chunked ? (size_t)srv_limits.max_body
                            : (cl > 0 ? (size_t)cl : 0);
    in->deadline_ms = http_now_ms() + (long)srv_limits.timeout_secs * 1000L
                    + (long)(expect / HTTP_MIN_BODY_BPS) * 1000L;

    size_t body_end;
    if (chunked) {
        size_t out = 0, next = 0;
        st = http_read_chunked(in, hlen, &out, &next);
        if (st) return st;
        body_end = hlen + out;
        in->next_off = next;
    } else {
        body_end = hlen + (cl > 0 ? (size_t)cl : 0);
        while (in->used < body_end) {
            ssize_t r = httpin_fill(in, in->maxcap - 1);
            if (r == -2) return 408;
            if (r <= 0) return HTTP_RR_CLOSE;
        }
        in->next_off = body_end;
    }
    in->next_first = in->raw[in->next_off];
    in->raw[body_end] = '\0';
    return 0;
}

static int http_read_request(HttpIn *in, int requests)
{
#ifdef TK_HAVE_OPENSSL
    int fl = 0;
    if (in->ssl) {
        fl = fcntl(in->fd, F_GETFL, 0);
        if (fl >= 0) fcntl(in->fd, F_SETFL, fl | O_NONBLOCK);
    }
    int rc = http_read_request_(in, requests);
    if (in->ssl && fl >= 0) fcntl(in->fd, F_SETFL, fl);
    return rc;
#else
    return http_read_request_(in, requests);
#endif
}

/* ── Rate limiting: removed, deliberately (Story 127.60) ──────────────────
 *
 * A fixed-size per-IP rate limiter used to live here: a 200-per-60-seconds
 * cap as two #defines, a 1024-bucket table, and a check at the top of
 * handle_connection() and handle_tls_connection().  It is gone, and this note
 * is here so nobody reinstates that shape.
 *
 * IT DID NOT THROTTLE.  Two things, both measured against the shipped code:
 *
 *   - It counted CONNECTIONS, not requests, despite its own comment reading
 *     "requests per window per IP".  The check ran once before the keep-alive
 *     loop, so one client on one socket was never limited: 886 of 900
 *     requests served, zero 429s, while 199 separate connections from the
 *     same address were enough to trip it.
 *   - It reset the bucket whenever the stored address differed from the
 *     caller's, so two addresses that hashed to the same bucket wiped each
 *     other's window on every call and NEITHER was ever limited.  Driving the
 *     real function: 10.0.1.172 and 10.0.2.78 collide in a 1024-bucket table,
 *     and 100000 alternating calls each were allowed in full, where a single
 *     address is cut off at 200.  Finding such a pair costs nothing.
 *
 * IT DID THROTTLE THE WRONG TRAFFIC.  Client identity came only from
 * fd_peer_ip(), the peer of the socket.  There was no X-Forwarded-For
 * handling anywhere, so behind a proxy or a CDN — the normal deployment —
 * every client in the world shared one bucket and the whole site was capped
 * at 200 connections a minute.
 *
 * AND THE NUMBER WAS NOT 200.  http_serve_workers() pre-forks N workers and
 * the bucket table was copied into each, so the effective cap was 200 x N,
 * decided by whichever worker happened to accept: measured at 400 with the
 * two workers http.serve() uses.  HTTP/2 over TLS skipped the check entirely,
 * because handle_tls_connection() dispatched to handle_h2_connection() before
 * reaching it.  None of this appeared in http.tki, http.tk or http.md, and
 * there was no env var and no setter, so nothing could rely on it and nothing
 * could tune it.
 *
 * WHY REMOVED RATHER THAN FIXED.  Correct in-process rate limiting is not two
 * #defines.  It needs state shared across the pre-fork workers, a table that
 * does not evict on collision, per-request counting, and — the part that
 * decides it — a TRUSTED-PROXY POLICY for client identity.  Reading
 * X-Forwarded-For without one turns today's shared-bucket bug into a
 * one-header total bypass, which is strictly worse than shipping nothing.
 * That is a subsystem with its own security surface; it should be specified
 * and designed, not bolted on inside a defect fix.  Until it exists the
 * honest position is the one every production HTTP server takes: rate
 * limiting belongs in front of the server, where the client identity is
 * actually known.  See docs/stdlib/http.md, "Rate limiting".
 */

static void handle_connection(int fd)
{
    char client_ip[64];
    fd_peer_ip(fd, client_ip, sizeof client_ip);

    /* 127.144: before anything is written to this socket.  A peer that
     * resets mid-response must give us EPIPE, not a fatal signal. */
    http_sock_nosigpipe(fd);

    /* 127.60: the per-IP rate limiter that stood here is gone — see the note
     * where it was defined.  Rate limiting belongs in front of the server. */

    /* Apply the (possibly user-overridden) receive timeout.  121.17: the
     * request reader waits with poll() against a per-request deadline, so
     * this only bounds reads outside it. */
    struct timeval tv;
    tv.tv_sec  = (long)srv_limits.timeout_secs;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* The per-connection buffer holds one whole request (headers + body),
     * plus room for chunk-size lines and the next pipelined request.  It
     * starts small and grows to that bound on demand (121.29 REL-02); since
     * it may move, it is always in.raw, never a copy of the pointer. */
    HttpIn in;
    if (httpin_open(&in, fd, NULL) != 0) { close(fd); return; }

    int requests = 0;
    int linger = 0;   /* 121.52: closing on the peer's error -- see http_linger_close */

    for (;;) {
        /* 121.17: one reader frames the request -- headers, then a
         * Content-Length or chunked body -- and keeps any bytes of the next
         * request.  Any status it returns is answered and the connection
         * closed: after a framing error the stream position is unknown. */
        int rr = http_read_request(&in, requests);
        if (rr == HTTP_RR_CLOSE) break;
        if (rr > 0) { send_error(fd, (uint16_t)rr, client_ip); linger = rr != 408; break; }

        requests++;

        int client_wants_close = req_wants_close(in.raw);
        int max_reached        = (requests >= KEEPALIVE_MAX_REQUESTS);
        int keep_alive         = (!client_wants_close && !max_reached);

        Req req = parse_request(in.raw);

        /* 121.44: no h2c upgrade.  An "Upgrade: h2c" request used to get a
         * 101 and then handle_h2_connection(fd, NULL, ...), which read the
         * connection preface with SSL_read() on a NULL SSL: the read failed
         * at once and the connection closed.  So the path never served a
         * request -- not even the upgrade request itself, which RFC 7540 3.2
         * says is answered on stream 1 -- and any HTTP/2 bytes the reader
         * had already buffered behind the request were dropped with it.
         * RFC 9113 3.1 deprecates the h2c upgrade; RFC 9110 7.8 lets a
         * server ignore Upgrade.  We do: the request is answered over
         * HTTP/1.1 like any other, and bytes behind it stay in the reader.
         * HTTP/2 is offered over TLS, via ALPN (handle_tls_connection). */

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
            free_req(&req);                     /* 127.65 */
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
            free_req(&req);                     /* 127.65 */
            if (!keep_alive) break;
            continue;
        }

        /* ── Route dispatch (HEAD falls back to GET handler) ── */
        if (getenv("TK_HTTP_DEBUG")) {
            fprintf(stderr, "[http] route_count=%d req.path=%s req.method=%s\n",
                    route_count, req.path ? req.path : "(null)",
                    req.method ? req.method : "(null)");
            for (int _d = 0; _d < route_count && _d < 10; _d++)
                fprintf(stderr, "[http]   [%d] %s %s\n", _d,
                        route_table[_d].method ? route_table[_d].method : "?",
                        route_table[_d].pattern ? route_table[_d].pattern : "?");
        }
        Res res = notfound_res();   /* 121.62 */
        StrPair params[HTTP_MAX_PARAMS]; int pc = 0;
        /* 127.100: route on the path component; the query string is not
         * part of the path and must not take part in matching. */
        char pathbuf_h1[2048];
        const char *rpath_h1 =
            http_path_only(req.path, pathbuf_h1, sizeof pathbuf_h1);
        int path_matched = 0;
        int handler_called = 0;
        /* Two-pass routing: exact matches first, then wildcard/param routes.
         * This ensures http.getstatic("/docs") takes priority over
         * http.servedir("*") even if registered later. */
        int best = -1;
        /* 127.72: set when `params` already holds `best`'s bindings, so the
         * winner is matched once rather than twice. */
        int have_params = 0;
        for (int i = 0; i < route_count; i++) {
            if (http_match_pattern(route_table[i].pattern,
                                   rpath_h1, params, &pc)) {
                path_matched = 1;
                const char *check_method = is_head ? "GET" : req.method;
                if (check_method &&
                    strcmp(route_table[i].method, check_method) == 0) {
                    /* Prefer non-wildcard routes over "*" */
                    if (strcmp(route_table[i].pattern, "*") != 0) {
                        best = i; have_params = 1; break; /* exact — done */
                    }
                    if (best < 0) best = i; /* wildcard — keep as fallback */
                }
                /* 127.72: was a bare `pc = 0`, which dropped one malloc'd
                 * key and value per parameter of every route whose path
                 * matched but whose method did not. */
                http_free_params(params, &pc);
            }
        }
        if (best >= 0) {
            if (!have_params)
                http_match_pattern(route_table[best].pattern,
                                   rpath_h1, params, &pc);
            req.params.data = params; req.params.len = (uint64_t)pc;
            res = route_table[best].h(req);
            handler_called = 1;
        }
        /* 127.72: the bindings the handler borrowed are released with the
         * request, not here — res.body may BE one of them. */
        http_defer_params(params, &pc);
        req.params.data = NULL; req.params.len = 0;
        /* Path existed but no method matched → 405 (only if handler wasn't called) */
        if (path_matched && !handler_called && res.status == 404) {
            res = make_res(405, "Method Not Allowed");
        }

        /* HEAD: send headers with correct Content-Length, but no body
         * (RFC 9110 §9.3.2) */
        if (is_head) {
            size_t head_body_len = res.body ? strlen(res.body) : 0;
            const char *conn_hdr = keep_alive ? "keep-alive" : "close";
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 %u %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: %s\r\n",
                res.status, reason(res.status), head_body_len, conn_hdr);
            http_write_all(fd, hdr, (size_t)hl);
            write_res_headers(fd, res);
            write_security_headers(fd, &res);
            write_cors_headers(fd);
            http_write_all(fd, "\r\n", 2);
            /* No body for HEAD */
            log_request(client_ip, &req, res.status, head_body_len);
        }

        /* Gzip compress if eligible — write directly (binary-safe) */
        else {
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
            http_write_all(fd, hdr, (size_t)hl);
            write_res_headers(fd, res);
            write_security_headers(fd, &res);
            write_cors_headers(fd);
            http_write_all(fd, "\r\n", 2);
            http_write_all(fd, gz_buf, gz_len);
            log_request(client_ip, &req, res.status, gz_len);
            free(gz_buf);
        } else {
            send_response(fd, res, keep_alive);
            log_request(client_ip, &req,
                        res.status, res.body ? strlen(res.body) : 0);
        }
        }

        /* 127.65: release the parsed request before the next keep-alive
         * iteration.  Everything that may alias it (res.body, the gzip
         * buffer, the access/error log lines) has already been written.
         * 127.72: the route bindings go with it, for the same reason — a
         * handler may return a :param value as the response body. */
        free_req(&req);
        http_run_deferred_frees();

        if (!keep_alive) break;
    }

    free(in.raw);
    if (linger) http_linger_close(fd);
    else        close(fd);
}

/* ── Server ─────────────────────────────────────────────────────────── */

/*
 * install_shutdown_handler — install `h` for `sig` via sigaction() with
 * sa_flags deliberately left at 0, i.e. WITHOUT SA_RESTART.
 *
 * Story 127.69: signal() on macOS/BSD (and glibc) installs the handler with
 * SA_RESTART, so the blocking accept() in worker_loop()/tls_worker_loop()
 * was transparently restarted by the kernel instead of returning EINTR.  A
 * worker parked in accept() therefore never reached its
 * `if (g_shutdown_requested) break;` check, so SIGTERM was only observed
 * when the next connection happened to arrive — and http_serve_workers took
 * the full HTTP_DRAIN_TIMEOUT_SECS and then SIGKILLed its own workers.
 * Without SA_RESTART accept() returns -1/EINTR and the loop exits at once.
 */
static void install_shutdown_handler(int sig, void (*h)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = h;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   /* NOT SA_RESTART — see above */
    sigaction(sig, &sa, NULL);
}

static void serve_sighandler(int sig)
{
    (void)sig;
    g_shutdown_requested = 1;
}

/* 121.53 (1): SIGHUP in a serving process -- the single-process server, a
 * worker, or the TLS supervisor -- asks for the logs to be reopened
 * (logrotate's postrotate `kill -HUP`).  It used to have no handler there,
 * so the default action killed a single-process server outright (and a TLS
 * supervisor, orphaning its workers).  Installed WITH SA_RESTART, unlike the
 * shutdown handlers: a log reopen must not cut short a read in progress, so
 * the flag is acted on at the next accept(). */
static volatile sig_atomic_t g_reopen_requested = 0;

static void serve_sighup_handler(int sig)
{
    (void)sig;
    g_reopen_requested = 1;
}

static void install_reopen_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = serve_sighup_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGHUP, &sa, NULL);
}

static void reopen_logs_if_requested(void)
{
    if (!g_reopen_requested) return;
    g_reopen_requested = 0;
    tk_log_reopen_files();
}

/* 135.32 -- bind a listening address without shadowing a live listener:
 * tk_bind_exclusive() in tk_bind.h (moved there by 135.33 so router.c,
 * net.c, net_glue.c and server_ops.c share it). */

int http_serve(uint16_t port) {
    TK_REQUIRE(TK_CAP_NET);
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    int srv = tk_bind_exclusive(&addr);   /* 135.32 */
    if (srv < 0) {
        fprintf(stderr, "error: could not bind to port %d: %s\n",
                (int)port, strerror(errno));
        return -1;
    }
    if (listen(srv, 8)<0){
        fprintf(stderr, "error: listen failed on port %d: %s\n",
                (int)port, strerror(errno));
        close(srv);return -1;
    }

    install_shutdown_handler(SIGTERM, serve_sighandler);   /* 127.69 */
    install_shutdown_handler(SIGINT,  serve_sighandler);   /* 127.69 */
    install_reopen_handler();                              /* 121.53 */

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
        reopen_logs_if_requested();
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
    TK_REQUIRE(TK_CAP_NET);

    /* 127.203: never SO_REUSEPORT. It let a second server on a busy port
     * bind it and silently split its connections with the first. The
     * pre-fork pool never needed it: the supervisor binds once here and every
     * worker (and every SIGHUP respawn) accept()s on the inherited fd.
     * 135.32: SO_REUSEADDR only after a probe says no listener is live, so a
     * restart can rebind a port held in TIME_WAIT but a bind cannot shadow a
     * live listener on macOS -- see tk_bind_exclusive() (tk_bind.h). */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (host == NULL || host[0] == '\0') {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            fprintf(stderr, "error: invalid host address '%s'\n", host);
            return -1;
        }
    }

    int srv = tk_bind_exclusive(&addr);
    if (srv < 0) {
        fprintf(stderr, "error: could not bind to port %llu: %s\n",
                (unsigned long long)port, strerror(errno));
        return -1;
    }
    if (listen(srv, 128) < 0) {
        fprintf(stderr, "error: listen failed on port %llu: %s\n",
                (unsigned long long)port, strerror(errno));
        close(srv); return -1;
    }
    /* 135.24 item 2: port 0 asked the kernel for a free port.  Say which
     * one, once, now that it is listening -- the only way the caller can
     * learn it, since serve/serveworkers do not return until shutdown. */
    if (port == 0) {
        struct sockaddr_in got;
        socklen_t glen = sizeof got;
        memset(&got, 0, sizeof got);
        if (getsockname(srv, (struct sockaddr *)&got, &glen) == 0) {
            fprintf(stderr, "DIAGNOSTIC: kind=server_listening port=%u\n",
                    (unsigned)ntohs(got.sin_port));
            fflush(stderr);
        }
    }
    return srv;
}

/* worker_loop — accept and handle connections; exits when shutdown is set. */
static void worker_loop(int srv_fd, TkHttpRouter *r)
{
    /* 127.69: own the shutdown disposition explicitly.  A forked worker
     * inherits it from http_serve_workers(), but the nworkers<=1 path runs
     * this loop in a process that never installed one. */
    install_shutdown_handler(SIGTERM, serve_sighandler);
    install_shutdown_handler(SIGINT,  serve_sighandler);
    install_reopen_handler();                              /* 121.53 */

    /* If a router is provided, copy its routes into the global table.
     * If NULL, use the global route_table as-is (already populated by
     * http_GET/http_POST/tk_http_get_handler calls). */
    if (r) {
        route_count = r->count;
        memcpy(route_table, r->routes, (size_t)r->count * sizeof(Route));
    }

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
        reopen_logs_if_requested();
        handle_connection(fd);
    }
}

TkHttpErr http_serve_workers(TkHttpRouter *r, const char *host,
                              uint64_t port, uint64_t nworkers)
{

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
    install_shutdown_handler(SIGTERM, workers_parent_sighandler);  /* 127.69 */
    install_shutdown_handler(SIGINT,  workers_parent_sighandler);  /* 127.69 */
    install_shutdown_handler(SIGHUP,  workers_parent_sighandler);  /* 127.69 */

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
            /* 121.53 (1): reopen the logs here, so the new workers inherit
             * the files now at the logs' paths (logrotate + SIGHUP). */
            tk_log_reopen_files();
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
                /* 135.3c: a part may repeat the header; the earlier values
                 * were overwritten and leaked (any client can send this). */
                free(*name_out);
                free(*filename_out);
                *name_out     = mp_parse_cd_param(line, "name");
                *filename_out = mp_parse_cd_param(line, "filename");
                free(line);
            }
        } else if (strncasecmp(p, "Content-Type:", 13) == 0) {
            const char *v    = mp_skip_ws(p + 13);
            const char *vend = p + linelen;
            while (vend > v && (*(vend-1) == ' ' || *(vend-1) == '\t'))
                vend--;
            free(*content_type_out);   /* 135.3c: same, for a repeated header */
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
    TK_REQUIRE(TK_CAP_NET);
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

/*
 * proxy_connect_tunnel — ask the http proxy on `fd` for a tunnel to
 * host:port (135.26).  Returns NULL once the proxy has answered 2xx, or a
 * message for err_resp_client.  It must run before any TLS state touches
 * the socket.
 *
 * The reply is read one byte at a time up to its blank line: a proxy may
 * send the status line and the rest of its header block in separate writes
 * (the old single recv() then left the tail in the socket for the TLS
 * handshake to read as garbage), and nothing past the blank line may be
 * consumed, because it belongs to the origin.  The socket's receive timeout
 * (client_tcp_connect) bounds each read.
 */
static const char *proxy_connect_tunnel(int fd, const char *host, const char *port)
{
    char req[600];
    int n = snprintf(req, sizeof req,
                     "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\n\r\n",
                     host, port, host, port);
    if (n < 0 || (size_t)n >= sizeof req)
        return "proxy CONNECT: host name too long";
    if (http_write_all(fd, req, (size_t)n) < 0)
        return "proxy CONNECT send failed";
    char rep[8192];
    size_t got = 0;
    for (;;) {
        if (got + 1 >= sizeof rep) return "proxy CONNECT reply too long";
        ssize_t r = recv(fd, rep + got, 1, 0);
        if (r <= 0) return "proxy CONNECT recv failed";
        got++;
        if (got >= 4 && memcmp(rep + got - 4, "\r\n\r\n", 4) == 0) break;
    }
    rep[got] = '\0';
    /* "HTTP/1.x 2xx" -- any 2xx opens the tunnel (RFC 9110 9.3.6) */
    if (got < 12 || strncmp(rep, "HTTP/1.", 7) != 0 || rep[8] != ' ' || rep[9] != '2')
        return "proxy CONNECT rejected";
    return NULL;
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
                                        const char *content_type,
                                        const StrPair *xh,
                                        uint64_t nxh)
{
    if (!c || !c->base_url) return err_resp_client("null client", 0);

    /* 138.8: caller-supplied headers.  A name or value carrying CR or LF
     * would end the header early and let the caller write a second header
     * or request (header injection), so it is refused; so is a Content-Length
     * or Host of the caller's own, which would contradict the ones sent
     * here.  A Content-Type among them REPLACES the default one -- it used
     * to be sent in addition, so the server saw two. */
    int caller_ct = 0;
    size_t xh_len = 0;
    for (uint64_t i = 0; i < nxh; i++) {
        const char *k = xh[i].key ? xh[i].key : "";
        const char *v = xh[i].val ? xh[i].val : "";
        if (!k[0] || strpbrk(k, "\r\n:") || strpbrk(v, "\r\n"))
            return err_resp_client("invalid header name or value (empty, or contains CR, LF or ':')", 0);
        if (!strcasecmp(k, "content-length") || !strcasecmp(k, "host"))
            return err_resp_client("Content-Length and Host are set by the client, not the caller", 0);
        if (!strcasecmp(k, "content-type")) caller_ct = 1;
        xh_len += strlen(k) + strlen(v) + 4;
    }
    char ct_line[512] = "";
    if (body && body_len > 0 && !caller_ct)
        snprintf(ct_line, sizeof ct_line, "Content-Type: %s\r\n",
                 content_type ? content_type : "application/octet-stream");

#ifndef TK_HAVE_OPENSSL
    if (strncmp(c->base_url, "https://", 8) == 0) {
        return err_resp_client(
            "HTTPS requires TLS (not compiled in); use http://", 0);
    }
#endif

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

    /* HTTPS through a proxy: open the CONNECT tunnel on the bare socket,
     * THEN negotiate TLS with the origin inside it (135.26).  This block
     * used to sit after the TLS setup below, so SSL_connect sent its
     * ClientHello to the proxy -- which speaks plain HTTP -- and every https
     * request through http.withproxy failed.  Measured by C341 against a
     * local proxy: the proxy's first line was TLS bytes, never CONNECT. */
    if (use_proxy && is_https) {
        const char *cerr = proxy_connect_tunnel(fd, host, port);
        if (cerr) { close(fd); return err_resp_client(cerr, 0); }
    }

#ifdef TK_HAVE_OPENSSL
    SSL_CTX *ssl_ctx = NULL;
    SSL *ssl = NULL;
    if (is_https) {
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx) { close(fd); return err_resp_client("SSL_CTX_new failed", 0); }
        /* 138.8: VERIFY THE SERVER.  This context used to be created and
         * used with OpenSSL's default of SSL_VERIFY_NONE: any certificate
         * was accepted for any host, so every https:// request -- API keys
         * in headers included -- could be read by whoever answered the
         * connection.  Measured 2026-09-27: a POST to a local server with a
         * self-signed certificate nothing trusts succeeded.  Now the chain
         * must verify against the trust store (the system default paths;
         * SSL_CERT_FILE / SSL_CERT_DIR override them) and the certificate
         * must name the host (DNS name) or address (IP literal). */
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_PEER, NULL);
        if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
            SSL_CTX_free(ssl_ctx); close(fd);
            return err_resp_client("TLS: no trust store to verify the server against", 0);
        }
        ssl = SSL_new(ssl_ctx);
        if (!ssl) { SSL_CTX_free(ssl_ctx); close(fd); return err_resp_client("SSL_new failed", 0); }
        SSL_set_fd(ssl, fd);
        {
            unsigned char ipbuf[16];
            int is_ip = inet_pton(AF_INET, host, ipbuf) == 1
                     || inet_pton(AF_INET6, host, ipbuf) == 1;
            int named = is_ip
                ? X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host)
                : SSL_set1_host(ssl, host);
            if (!is_ip) SSL_set_tlsext_host_name(ssl, host);   /* SNI: names only */
            if (named != 1) {
                SSL_free(ssl); SSL_CTX_free(ssl_ctx); close(fd);
                return err_resp_client("TLS: cannot set the name to verify", 0);
            }
        }
        if (SSL_connect(ssl) <= 0) {
            long vr = SSL_get_verify_result(ssl);
            char m[200];
            if (vr != X509_V_OK)
                snprintf(m, sizeof m, "TLS certificate verification failed: %s",
                         X509_verify_cert_error_string(vr));
            else
                snprintf(m, sizeof m, "SSL handshake failed");
            SSL_free(ssl); SSL_CTX_free(ssl_ctx); close(fd);
            return err_resp_client(m, 0);
        }
    }
#define CLIENT_SEND(buf, len) (is_https ? SSL_write(ssl, buf, (int)(len)) : send(fd, buf, len, 0))
#define CLIENT_RECV(buf, len) (is_https ? SSL_read(ssl, buf, (int)(len)) : (int)recv(fd, buf, len, 0))
#define CLIENT_CLOSE() do { if (ssl) { SSL_shutdown(ssl); SSL_free(ssl); } if (ssl_ctx) SSL_CTX_free(ssl_ctx); close(fd); } while(0)
#else
#define CLIENT_SEND(buf, len) send(fd, buf, len, 0)
#define CLIENT_RECV(buf, len) (int)recv(fd, buf, len, 0)
#define CLIENT_CLOSE() close(fd)
#endif

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

    /* 138.8: the caller's header lines, validated at the top. */
    char *xh_buf = (char *)malloc(xh_len + 1);
    if (!xh_buf) { CLIENT_CLOSE(); return err_resp_client("out of memory", 0); }
    {
        size_t o = 0;
        for (uint64_t i = 0; i < nxh; i++)
            o += (size_t)snprintf(xh_buf + o, xh_len + 1 - o, "%s: %s\r\n",
                                  xh[i].key, xh[i].val ? xh[i].val : "");
        xh_buf[o] = '\0';
    }
    if (body && body_len > 0) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "%s"
            "Content-Length: %llu\r\n"
            "%s"
            "Connection: close\r\n"
            "\r\n",
            method, request_uri, host, ct_line,
            (unsigned long long)body_len, xh_buf);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "%s"
            "Connection: close\r\n"
            "\r\n",
            method, request_uri, host, xh_buf);
    }
    free(xh_buf);

    if (req_len < 0 || (size_t)req_len >= sizeof(req_buf)) {
        CLIENT_CLOSE();
        return err_resp_client("request header too large", 0);
    }

    if (CLIENT_SEND(req_buf, (size_t)req_len) < 0) {
        CLIENT_CLOSE();
        return err_resp_client("send header failed", 0);
    }
    if (body && body_len > 0) {
        if (CLIENT_SEND(body, (size_t)body_len) < 0) {
            CLIENT_CLOSE();
            return err_resp_client("send body failed", 0);
        }
    }

    /* read full response (headers + body) with proper framing */
    size_t cap = 65536, used = 0;
    char *resp = malloc(cap);
    if (!resp) { CLIENT_CLOSE(); return err_resp_client("out of memory", 0); }

    ssize_t n;
    char *hdr_end_ptr = NULL;
    size_t hdr_end_off = 0;   /* offset of first byte after \r\n\r\n */
    int body_mode = 0;        /* 0=unknown, 1=content-length, 2=chunked, 3=close */
    size_t content_length = 0;

    /* Phase 1: read until we have full headers (\r\n\r\n) */
    while (!hdr_end_ptr) {
        n = CLIENT_RECV(resp + used, cap - used - 1);
        if (n <= 0) break; /* timeout or close */
        used += (size_t)n;
        resp[used] = '\0';
        if (used + 1 >= cap) {
            cap *= 2;
            char *tmp = realloc(resp, cap);
            if (!tmp) { free(resp); CLIENT_CLOSE();
                        return err_resp_client("out of memory", 0); }
            resp = tmp;
        }
        hdr_end_ptr = strstr(resp, "\r\n\r\n");
    }

    if (hdr_end_ptr) {
        hdr_end_off = (size_t)(hdr_end_ptr - resp) + 4;

        /* Determine body framing from headers */
        /* Check Transfer-Encoding: chunked (case-insensitive scan) */
        const char *te = resp;
        while ((te = strstr(te, "\r\n")) != NULL && te < hdr_end_ptr) {
            te += 2;
            if ((te + 18 <= hdr_end_ptr) &&
                (strncasecmp(te, "Transfer-Encoding:", 18) == 0)) {
                const char *val = te + 18;
                while (*val == ' ') val++;
                if (strncasecmp(val, "chunked", 7) == 0) {
                    body_mode = 2;
                }
                break;
            }
        }
        /* Check Content-Length if not chunked */
        if (body_mode == 0) {
            const char *cl = resp;
            while ((cl = strstr(cl, "\r\n")) != NULL && cl < hdr_end_ptr) {
                cl += 2;
                if ((cl + 15 <= hdr_end_ptr) &&
                    (strncasecmp(cl, "Content-Length:", 15) == 0)) {
                    const char *val = cl + 15;
                    while (*val == ' ') val++;
                    content_length = (size_t)strtoul(val, NULL, 10);
                    body_mode = 1;
                    break;
                }
            }
        }
        /* If neither, fall back to read-until-close */
        if (body_mode == 0) body_mode = 3;
    } else {
        /* Never found headers — treat what we have as the full response */
        body_mode = 3;
    }

    /* Phase 2: read body according to framing mode */
    if (body_mode == 1) {
        /* Content-Length: read exactly content_length bytes of body */
        size_t body_have = used - hdr_end_off;
        while (body_have < content_length) {
            if (used + 1 >= cap) {
                cap *= 2;
                char *tmp = realloc(resp, cap);
                if (!tmp) { free(resp); CLIENT_CLOSE();
                            return err_resp_client("out of memory", 0); }
                resp = tmp;
            }
            n = CLIENT_RECV(resp + used, cap - used - 1);
            if (n <= 0) break;
            used += (size_t)n;
            body_have += (size_t)n;
        }
    } else if (body_mode == 2) {
        /* Chunked: read until final chunk marker "0\r\n\r\n" appears in body */
        for (;;) {
            resp[used] = '\0';
            /* Look for terminal chunk in body portion */
            const char *body_start = resp + hdr_end_off;
            size_t body_len_so_far = used - hdr_end_off;
            /* Terminal chunk: "0\r\n\r\n" (could also be "0\r\n" + trailers + "\r\n") */
            if (body_len_so_far >= 5) {
                const char *p = body_start;
                const char *end = resp + used - 4;
                while (p <= end) {
                    if (p[0] == '0' && p[1] == '\r' && p[2] == '\n' &&
                        p[3] == '\r' && p[4] == '\n') {
                        goto chunked_done;
                    }
                    p++;
                }
            }
            if (used + 1 >= cap) {
                cap *= 2;
                char *tmp = realloc(resp, cap);
                if (!tmp) { free(resp); CLIENT_CLOSE();
                            return err_resp_client("out of memory", 0); }
                resp = tmp;
            }
            n = CLIENT_RECV(resp + used, cap - used - 1);
            if (n <= 0) break;
            used += (size_t)n;
        }
        chunked_done: ;
    } else {
        /* mode 3: read until connection close / timeout (original behavior) */
        while ((n = CLIENT_RECV(resp + used, cap - used - 1)) > 0) {
            used += (size_t)n;
            if (used + 1 >= cap) {
                cap *= 2;
                char *tmp = realloc(resp, cap);
                if (!tmp) { free(resp); CLIENT_CLOSE();
                            return err_resp_client("out of memory", 0); }
                resp = tmp;
            }
        }
    }

    resp[used] = '\0';
    CLIENT_CLOSE();
#undef CLIENT_SEND
#undef CLIENT_RECV
#undef CLIENT_CLOSE

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
        size_t raw_body_len = used - (size_t)(sep - resp);
        if (raw_body_len > 0 && body_mode == 2) {
            /* Decode chunked transfer encoding:
             * Format: <hex-size>\r\n<data>\r\n ... 0\r\n\r\n */
            uint8_t *decoded = malloc(raw_body_len); /* upper bound */
            if (decoded) {
                size_t dpos = 0;
                const char *cp = sep;
                const char *raw_end = sep + raw_body_len;
                while (cp < raw_end) {
                    /* Parse chunk size (hex) */
                    char *nl = NULL;
                    unsigned long chunk_sz = strtoul(cp, &nl, 16);
                    if (!nl || nl == cp) break; /* malformed */
                    /* Skip to data (past \r\n after size) */
                    if (nl + 2 <= raw_end && nl[0] == '\r' && nl[1] == '\n')
                        cp = nl + 2;
                    else if (nl + 1 <= raw_end && nl[0] == '\n')
                        cp = nl + 1;
                    else
                        break;
                    if (chunk_sz == 0) break; /* final chunk */
                    /* Copy chunk data */
                    size_t avail = (size_t)(raw_end - cp);
                    size_t to_copy = chunk_sz < avail ? (size_t)chunk_sz : avail;
                    memcpy(decoded + dpos, cp, to_copy);
                    dpos += to_copy;
                    cp += to_copy;
                    /* Skip trailing \r\n after chunk data */
                    if (cp + 2 <= raw_end && cp[0] == '\r' && cp[1] == '\n')
                        cp += 2;
                    else if (cp + 1 <= raw_end && cp[0] == '\n')
                        cp += 1;
                }
                resp_body = decoded;
                resp_body_len = (uint64_t)dpos;
            }
        } else if (raw_body_len > 0) {
            resp_body_len = (uint64_t)raw_body_len;
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
    return client_do_request(c, "GET", path, NULL, 0, NULL, NULL, 0);
}

HttpClientResp http_post(HttpClient *c, const char *path,
                         const uint8_t *body, uint64_t body_len,
                         const char *content_type)
{
    return client_do_request(c, "POST", path, body, body_len, content_type, NULL, 0);
}

HttpClientResp http_put(HttpClient *c, const char *path,
                        const uint8_t *body, uint64_t body_len,
                        const char *content_type)
{
    return client_do_request(c, "PUT", path, body, body_len, content_type, NULL, 0);
}

HttpClientResp http_delete(HttpClient *c, const char *path)
{
    return client_do_request(c, "DELETE", path, NULL, 0, NULL, NULL, 0);
}

/* 138.8: any method, with caller headers (see client_do_request). */
HttpClientResp http_request_headers(HttpClient *c, const char *method,
                                    const char *path,
                                    const uint8_t *body, uint64_t body_len,
                                    const char *content_type,
                                    const StrPair *headers, uint64_t nheaders)
{
    return client_do_request(c, method, path, body, body_len, content_type,
                             headers, nheaders);
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
    /* 121.42 (3): the download is written to dest_path -- fs.write, as well
     * as the net grant client_tcp_connect takes.  Checked before anything
     * is fetched, so a denied program has touched neither. */
    TK_REQUIRE(TK_CAP_FS_WRITE);

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

/* Preferred protocols: http/1.1 only — h2 has bugs with large responses
 * (CSS 24KB causes worker hang). Disable h2 until frame handling fixed. */
static const uint8_t alpn_protos[] = {
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
static void write_security_headers_ssl(SSL *ssl, const Res *res)
{
    /* 121.50 (3): the handler's own value wins, as over plain HTTP. */
    for (size_t i = 0; i < 4; i++)
        if (!res_sets_header(res, sec_hdr_lines[i]))
            SSL_write(ssl, sec_hdr_lines[i], (int)strlen(sec_hdr_lines[i]));
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
 * 121.46: an HTTP/2 connection with no request in progress is closed with
 * GOAWAY(NO_ERROR) after H2_IDLE_TIMEOUT_S.  Only request frames (HEADERS,
 * CONTINUATION, DATA) count as activity: the server used to PING an idle
 * peer and take the ACK as activity, so any client that answered PINGs --
 * every HTTP/2 client -- held a pre-fork worker for as long as it stayed
 * connected.  30 s matches the old PING interval; a build may override it
 * (C500 builds with 3).
 */
#ifndef H2_IDLE_TIMEOUT_S
#define H2_IDLE_TIMEOUT_S       30
#endif
/* Streams one connection may open in its lifetime (60.2.3). */
#define H2_MAX_STREAMS_TOTAL  1000

/* h2_field_name — copy a handler's header name into out, lower-cased
 * (RFC 9113 8.2.1: an upper-case field name makes the response malformed).
 * Returns 0 for a name that is not a token, is too long, or is one HTTP/2
 * forbids (connection-specific fields, RFC 9113 8.2.2) or computes itself
 * (content-length, from the body actually sent). */
static int h2_field_name(const char *k, char *out, size_t cap)
{
    static const char *const drop[] = {
        "connection", "keep-alive", "proxy-connection", "transfer-encoding",
        "upgrade", "te", "content-length", NULL
    };
    size_t n = strlen(k);
    if (n == 0 || n >= cap) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)k[i];
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if (!isalnum(c) && !strchr("!#$%&'*+-.^_`|~", (int)c)) return 0;
        out[i] = (char)c;
    }
    out[n] = '\0';
    for (int i = 0; drop[i]; i++)
        if (strcmp(out, drop[i]) == 0) return 0;
    return 1;
}

/* h2_field_value_ok — a field value may not carry CR or LF (RFC 9113 8.2.1). */
static int h2_field_value_ok(const char *v)
{
    return strpbrk(v, "\r\n") == NULL;
}

static int h2_has_field(const char **names, size_t n, const char *name)
{
    for (size_t i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) return 1;
    return 0;
}

/* h2_send_header_block — send an encoded header block as one HEADERS frame
 * and as many CONTINUATION frames as the peer's SETTINGS_MAX_FRAME_SIZE
 * requires.  END_STREAM, when asked for, rides on the HEADERS frame. */
static int h2_send_header_block(H2Conn *conn, int fd, void *ssl, uint32_t sid,
                                const uint8_t *blk, size_t len, int end_stream)
{
    uint32_t maxf = conn->peer_settings.max_frame_size;
    size_t off = 0;
    int first = 1;
    do {
        size_t chunk = len - off;
        if (chunk > maxf) chunk = maxf;
        uint8_t flags = (off + chunk >= len) ? H2_FLAG_END_HEADERS : 0;
        if (first && end_stream) flags |= H2_FLAG_END_STREAM;
        if (h2_frame_send(conn, fd, ssl,
                          first ? H2_FRAME_HEADERS : H2_FRAME_CONTINUATION,
                          flags, sid, blk + off, (uint32_t)chunk) < 0)
            return -1;
        off += chunk;
        first = 0;
    } while (off < len);
    return 0;
}

/*
 * h2_send_response — answer stream sid with res.  121.46: the header block
 * carries what HTTP/1.1 over TLS sends -- the handler's own headers, the
 * security headers and HSTS (unless the handler set that field itself), and
 * CORS when an allowed Origin matched -- where it used to carry :status,
 * content-length and content-type only.  Returns 0, or -1 when the block
 * could not be encoded (the HPACK encoder's table may then disagree with the
 * peer's, so the caller must end the connection).
 */
/* h2_flush_stream -- send as much of st's pending body as the peer's
 * windows allow (121.50 (1)): each DATA frame is at most the peer's
 * SETTINGS_MAX_FRAME_SIZE and at most what is left of both the stream's and
 * the connection's send window, and the frames it sends are taken off both
 * (RFC 9113 6.9.1).  END_STREAM rides on the last byte.  Returns 1 when the
 * whole body is out, 0 when the window closed first (st keeps the rest),
 * -1 on a write error. */
static int h2_flush_stream(H2Conn *conn, int fd, void *ssl, H2Stream *st)
{
    int progressed = 0;
    while (st->out_off < st->out_len) {
        int64_t room = conn->conn_window_send;
        if (st->window < room) room = st->window;
        if (room <= 0) break;
        size_t chunk = st->out_len - st->out_off;
        if (chunk > (size_t)room) chunk = (size_t)room;
        if (chunk > conn->peer_settings.max_frame_size)
            chunk = conn->peer_settings.max_frame_size;
        uint8_t flags = (st->out_off + chunk >= st->out_len)
                      ? H2_FLAG_END_STREAM : 0;
        if (h2_frame_send(conn, fd, ssl, H2_FRAME_DATA, flags, st->id,
                          st->out + st->out_off, (uint32_t)chunk) < 0)
            return -1;
        conn->conn_window_send -= (int32_t)chunk;
        st->window -= (int32_t)chunk;
        st->out_off += chunk;
        progressed = 1;
    }
    if (st->out_off >= st->out_len) return 1;
    /* A peer that never opens its window must not hold the worker: the
     * wait for the next WINDOW_UPDATE is bounded like any other phase of a
     * request, from the last byte that went out. */
    if (progressed || !st->deadline_ms)
        st->deadline_ms = h2_now_ms() + (long)conn->read_timeout_ms;
    return 0;
}

/* h2_flush_pending -- after the peer opened a window (WINDOW_UPDATE or a
 * larger SETTINGS_INITIAL_WINDOW_SIZE), send what the waiting streams can,
 * and release each one whose body is now complete.  -1 on a write error. */
static int h2_flush_pending(H2Conn *conn, int fd, void *ssl)
{
    for (size_t i = 0; i < conn->stream_count; i++) {
        H2Stream *st = &conn->streams[i];
        if (st->state == H2_STREAM_CLOSED || !st->out) continue;
        if (conn->conn_window_send <= 0) break;
        int r = h2_flush_stream(conn, fd, ssl, st);
        if (r < 0) return -1;
        if (r > 0) h2_stream_release(st);
    }
    return 0;
}

static int h2_send_response(H2Conn *conn, int fd, void *ssl, H2Stream *st,
                            Res res, int is_head)
{
    uint32_t sid = st->id;
    static const char *const sec[][2] = {
        { "x-content-type-options",    "nosniff" },
        { "x-frame-options",           "DENY" },
        { "referrer-policy",           "strict-origin-when-cross-origin" },
        { "strict-transport-security",
          "max-age=63072000; includeSubDomains; preload" },
    };
    size_t body_len = res.body ? strlen(res.body) : 0;
    int no_body = res.status < 200 || res.status == 204 || res.status == 304;
    if (no_body) body_len = 0;
    size_t cap = (size_t)res.headers.len + 16;
    const char **nm = calloc(cap, sizeof *nm);
    const char **vl = calloc(cap, sizeof *vl);
    char **own = calloc(cap, sizeof *own);
    uint8_t *blk = NULL;
    int rc = -1;
    if (!nm || !vl || !own) goto out;

    char stat[8], cl[32];
    snprintf(stat, sizeof stat, "%u", res.status);
    snprintf(cl, sizeof cl, "%zu", body_len);
    size_t k = 0;
    nm[k] = ":status"; vl[k++] = stat;
    if (!no_body) { nm[k] = "content-length"; vl[k++] = cl; }
    for (uint64_t i = 0; i < res.headers.len; i++) {
        const char *hk = res.headers.data[i].key;
        const char *hv = res.headers.data[i].val;
        char lname[256];
        if (!hk || !hv || !h2_field_name(hk, lname, sizeof lname) ||
            !h2_field_value_ok(hv))
            continue;
        if (!(own[k] = strdup(lname))) goto out;
        nm[k] = own[k]; vl[k++] = hv;
    }
    if (!no_body && !h2_has_field(nm, k, "content-type")) {
        nm[k] = "content-type"; vl[k++] = "text/html; charset=utf-8";
    }
    for (size_t i = 0; i < sizeof sec / sizeof sec[0]; i++)
        if (!h2_has_field(nm, k, sec[i][0])) {
            nm[k] = sec[i][0]; vl[k++] = sec[i][1];
        }
    if (g_cors_matched) {
        nm[k] = "access-control-allow-origin"; vl[k++] = g_cors_matched;
        nm[k] = "access-control-allow-methods";
        vl[k++] = "GET, POST, PUT, DELETE, PATCH, OPTIONS";
        nm[k] = "access-control-allow-headers";
        vl[k++] = "Content-Type, Authorization";
        nm[k] = "vary"; vl[k++] = "Origin";
    }

    /* Room for every field as a literal: a 1-byte prefix and two lengths of
     * at most 5 bytes each (RFC 7541 5.1).  An encode that ran out of room
     * would already have changed the encoder's dynamic table. */
    size_t need = 16;
    for (size_t i = 0; i < k; i++) need += strlen(nm[i]) + strlen(vl[i]) + 12;
    if (!(blk = malloc(need))) goto out;
    int hlen = hpack_encode(&conn->hpack_encode, nm, vl, k, blk, need);
    if (hlen < 0) goto out;

    int end_stream = is_head || body_len == 0;
    if (h2_send_header_block(conn, fd, ssl, sid, blk, (size_t)hlen,
                             end_stream) < 0)
        goto out;
    rc = 0;
    if (!end_stream) {
        /* 121.50 (1): the body goes out as the peer's flow-control windows
         * allow.  It used to be written whole, ignoring them, so a body over
         * 64 KiB overran a client on the default window (RFC 9113 6.9: a
         * FLOW_CONTROL_ERROR, and the stream failed).  What the window does
         * not take now waits on the stream -- copied, since res.body is the
         * handler's -- for the WINDOW_UPDATE frames that open it; the
         * connection loop sends it (h2_flush_pending) and serves other
         * streams meanwhile. */
        free(st->out);
        if (!(st->out = malloc(body_len))) { rc = -1; goto out; }
        memcpy(st->out, res.body, body_len);
        st->out_len = body_len;
        st->out_off = 0;
        st->deadline_ms = 0;
        if (h2_flush_stream(conn, fd, ssl, st) > 0) {
            free(st->out);
            st->out = NULL;
            st->out_len = st->out_off = 0;
        }
    }
out:
    if (own) for (size_t i = 0; i < cap; i++) free(own[i]);
    free(own);
    free(nm);
    free(vl);
    free(blk);
    return rc;
}

/*
 * h2_dispatch — build a Req from a whole request (decoded fields and body),
 * route it the way the HTTP/1.1 TLS path does (Story 75.2: exact match over
 * wildcard, HEAD falls back to GET, 405 for a path with no matching method,
 * CORS preflight), and send the response.  Returns 0, or -1 when the
 * connection must end.
 */
static int h2_dispatch(H2Conn *conn, int fd, void *ssl, const char *client_ip,
                       H2Stream *st)
{
    const char *method = NULL, *path = NULL, *authority = NULL;
    StrPair req_headers[64];
    int req_hcount = 0, have_host = 0;
    char *cookie = NULL;   /* RFC 9113 8.2.3: crumbs rejoined with "; " */

    for (int i = 0; i < st->hcount; i++) {
        const char *n = st->hnames[i], *v = st->hvalues[i];
        if (n[0] == ':') {
            if (strcmp(n, ":method") == 0) method = v;
            else if (strcmp(n, ":path") == 0) path = v;
            else if (strcmp(n, ":authority") == 0) authority = v;
            continue;
        }
        if (strcmp(n, "cookie") == 0) {
            if (!cookie) { cookie = strdup(v); continue; }
            size_t a = strlen(cookie), b = strlen(v);
            char *j = realloc(cookie, a + b + 3);
            if (!j) continue;
            memcpy(j + a, "; ", 2); memcpy(j + a + 2, v, b + 1);
            cookie = j;
            continue;
        }
        if (strcmp(n, "host") == 0) have_host = 1;
        if (req_hcount < 60) {
            req_headers[req_hcount].key = n;
            req_headers[req_hcount].val = v;
            req_hcount++;
        }
    }
    if (cookie) {
        req_headers[req_hcount].key = "cookie";
        req_headers[req_hcount].val = cookie;
        req_hcount++;
    }
    if (authority && !have_host) {   /* RFC 9113 8.3.1: :authority is Host */
        req_headers[req_hcount].key = "host";
        req_headers[req_hcount].val = authority;
        req_hcount++;
    }
    /* RFC 9113 8.3.1: a request without :method or :path is malformed.  It
     * used to be served as GET /. */
    if (!method || !path) {
        free(cookie);
        h2_send_rst_stream(conn, fd, ssl, st->id, H2_ERR_PROTOCOL);
        return 0;
    }

    Req req;
    memset(&req, 0, sizeof(req));
    req.method  = method;
    req.path    = path;
    req.body    = st->body ? st->body : "";
    req.headers.data = req_headers;
    req.headers.len  = (uint64_t)req_hcount;

    g_cors_matched = NULL;
    if (g_cors_origins)
        for (int i = 0; i < req_hcount; i++)
            if (strcasecmp(req_headers[i].key, "origin") == 0) {
                g_cors_matched = cors_match_origin(req_headers[i].val);
                break;
            }

    Res res = notfound_res();   /* 121.62 */
    int is_head = (strcmp(method, "HEAD") == 0);
    if (strcmp(method, "OPTIONS") == 0 && g_cors_matched) {
        res = make_res(204, NULL);                 /* CORS preflight (7.7.4) */
    } else {
        StrPair h2_params[HTTP_MAX_PARAMS]; int h2_pc = 0;
        /* 127.100: :path carries the query string; route on the path. */
        char pathbuf_h2[2048];
        const char *rpath_h2 = http_path_only(path, pathbuf_h2, sizeof pathbuf_h2);
        int matched = 0, called = 0, best = -1, have_params = 0; /* 127.72 */
        for (int r = 0; r < route_count; r++) {
            if (http_match_pattern(route_table[r].pattern, rpath_h2,
                                   h2_params, &h2_pc)) {
                matched = 1;
                const char *cm = is_head ? "GET" : method;
                if (strcmp(route_table[r].method, cm) == 0) {
                    if (strcmp(route_table[r].pattern, "*") != 0) {
                        best = r; have_params = 1; break;
                    }
                    if (best < 0) best = r;
                }
                http_free_params(h2_params, &h2_pc);   /* 127.72 */
            }
        }
        if (best >= 0) {
            if (!have_params)
                http_match_pattern(route_table[best].pattern, rpath_h2,
                                   h2_params, &h2_pc);
            req.params.data = h2_params;
            req.params.len  = (uint64_t)h2_pc;
            res = route_table[best].h(req);
            called = 1;
        }
        http_defer_params(h2_params, &h2_pc);          /* 127.72 */
        req.params.data = NULL; req.params.len = 0;
        if (matched && !called && res.status == 404)
            res = make_res(405, "Method Not Allowed");
    }

    int rc = h2_send_response(conn, fd, ssl, st, res, is_head);
    log_request(client_ip, &req, res.status, res.body ? strlen(res.body) : 0);
    free(cookie);
    /* 127.72: response written and logged -- the route bindings it may
     * alias can go now. */
    http_run_deferred_frees();
    return rc;
}

/* h2_refuse_body — answer a stream whose body is over max_body with 413 and
 * reset it with NO_ERROR, which asks the client to stop sending without
 * treating the request as failed in transit (RFC 9113 8.1). */
static int h2_refuse_body(H2Conn *conn, int fd, void *ssl, H2Stream *st)
{
    int rc = h2_send_response(conn, fd, ssl, st,
                              make_res(413, "Payload Too Large"), 0);
    h2_send_rst_stream(conn, fd, ssl, st->id, H2_ERR_NONE);
    h2_stream_release(st);
    return rc;
}

/* h2_finish — the request on st is whole: check its content-length against
 * the body received (RFC 9113 8.1.1: a mismatch is malformed), dispatch it,
 * and release the stream. */
static int h2_finish(H2Conn *conn, int fd, void *ssl, const char *client_ip,
                     H2Stream *st)
{
    int rc = 0;
    for (int i = 0; i < st->hcount; i++)
        if (strcmp(st->hnames[i], "content-length") == 0) {
            char *e = NULL;
            unsigned long long v = strtoull(st->hvalues[i], &e, 10);
            if (!e || *e || e == st->hvalues[i] || v != st->body_len) {
                h2_send_rst_stream(conn, fd, ssl, st->id, H2_ERR_PROTOCOL);
                h2_stream_release(st);
                return 0;
            }
        }
    rc = h2_dispatch(conn, fd, ssl, client_ip, st);
    /* 121.50 (1): a stream whose body is still waiting for the peer's
     * window stays open until the last byte is out (h2_flush_pending). */
    if (st->out && rc == 0) return 0;
    h2_stream_release(st);
    return rc;
}

/* h2_block_done — a header block ended (END_HEADERS).  Decode it; a request
 * that also ended (END_STREAM) is dispatched, one with a body waits for it;
 * trailers are decoded -- to keep the HPACK table in step -- and dropped.
 * Returns 0, or -1 when the connection must end (GOAWAY already sent). */
static int h2_block_done(H2Conn *conn, int fd, void *ssl,
                         const char *client_ip, H2Stream *st)
{
    char **names = NULL, **values = NULL;
    int n = hpack_decode(&conn->hpack_decode, st->header_block,
                         st->header_block_len, &names, &values);
    free(st->header_block);
    st->header_block = NULL;
    st->header_block_len = st->header_block_cap = 0;
    if (n < 0) {
        /* RFC 9113 4.3: a header block that cannot be decoded is a
         * connection error -- the decoder's table is no longer the peer's. */
        h2_send_goaway(conn, fd, ssl, conn->last_stream_id, H2_ERR_COMPRESSION);
        return -1;
    }
    if (st->block_trailers) {
        for (int i = 0; i < n; i++) { free(names[i]); free(values[i]); }
        free(names); free(values);
        return h2_finish(conn, fd, ssl, client_ip, st);
    }
    st->hnames = names; st->hvalues = values; st->hcount = n;
    size_t expect = (size_t)srv_limits.max_body;
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], "content-length") == 0) {
            unsigned long long v = strtoull(values[i], NULL, 10);
            if (v > (unsigned long long)srv_limits.max_body)
                return h2_refuse_body(conn, fd, ssl, st);
            expect = (size_t)v;
        }
    if (st->block_end_stream)
        return h2_finish(conn, fd, ssl, client_ip, st);
    /* The body's clock is HTTP/1.1's (121.17): the timeout, plus one second
     * per HTTP_MIN_BODY_BPS bytes announced -- or, with no content-length,
     * as many as the body limit would take at that rate. */
    st->deadline_ms = h2_now_ms() + (long)conn->read_timeout_ms
                    + (long)(expect / HTTP_MIN_BODY_BPS) * 1000L;
    st->receiving = 1;
    return 0;
}

/* h2_append_block — append a header-block fragment to st, within the
 * SETTINGS_MAX_HEADER_LIST_SIZE we advertised (121.44). */
static int h2_append_block(H2Conn *conn, H2Stream *st,
                           const uint8_t *p, size_t n)
{
    size_t newlen = st->header_block_len + n;
    if (newlen > conn->local_settings.max_header_list_size) return -1;
    if (newlen > st->header_block_cap) {
        uint8_t *nb = realloc(st->header_block, newlen ? newlen : 1);
        if (!nb) return -1;
        st->header_block = nb;
        st->header_block_cap = newlen;
    }
    if (n) memcpy(st->header_block + st->header_block_len, p, n);
    st->header_block_len = newlen;
    return 0;
}

/* h2_body_buffered — request-body bytes held on the connection. */
static size_t h2_body_buffered(const H2Conn *conn)
{
    size_t t = 0;
    for (size_t i = 0; i < conn->stream_count; i++) t += conn->streams[i].body_len;
    return t;
}

/* h2_open_streams — streams with a request in progress. */
static uint32_t h2_open_streams(const H2Conn *conn)
{
    uint32_t n = 0;
    for (size_t i = 0; i < conn->stream_count; i++)
        if (conn->streams[i].state != H2_STREAM_CLOSED) n++;
    return n;
}

/* h2_next_deadline — the earliest deadline in force: the preface phase's,
 * or that of a request in progress (0 when none). */
static long h2_next_deadline(const H2Conn *conn, long settings_dl)
{
    long dl = settings_dl;
    for (size_t i = 0; i < conn->stream_count; i++) {
        long s = conn->streams[i].deadline_ms;
        if (conn->streams[i].state != H2_STREAM_CLOSED && s && (!dl || s < dl))
            dl = s;
    }
    return dl;
}

/*
 * handle_h2_connection — process an HTTP/2 connection.
 * Validates connection preface, exchanges SETTINGS, then loops reading
 * frames.  A request is dispatched through the route table when it is whole:
 * its header block decoded (HEADERS [+ CONTINUATION]) and its body ended.
 */
static int handle_h2_connection(int fd, void *ssl, const char *client_ip)
{
    H2Conn *conn = h2_conn_new();
    if (!conn) return 0;

    /* 121.44: the HTTP/2 counterpart of HTT-06.  A frame must be whole
     * within timeout_secs of its first byte, and two phases that span frames
     * have a deadline of their own: the preface through the client's first
     * SETTINGS frame, and (121.46) a request, from its first HEADERS frame
     * to the end of its body.  The reads wait with poll() against these
     * deadlines (http2.c); SO_RCVTIMEO, which restarts on every byte, no
     * longer bounds anything here. */
    conn->read_timeout_ms = srv_limits.timeout_secs * 1000u;
    long settings_dl = h2_now_ms() + (long)conn->read_timeout_ms;
    conn->deadline_ms = settings_dl;
    const long idle_ms = (long)H2_IDLE_TIMEOUT_S * 1000L;

    /* Read and validate the 24-byte client connection preface */
    uint8_t preface[H2_CONNECTION_PREFACE_LEN];
    if (h2_conn_read(conn, fd, ssl, preface, H2_CONNECTION_PREFACE_LEN) != 0 ||
        h2_validate_preface(preface, H2_CONNECTION_PREFACE_LEN) != 0) {
        h2_conn_free(conn);
        return 0;
    }

    /* Send our SETTINGS frame */
    if (h2_send_settings(conn, fd, ssl) < 0) {
        h2_conn_free(conn);
        return 0;
    }

    uint32_t total_streams = 0;
    uint32_t hb_sid = 0;          /* stream whose header block is open */
    long last_request = h2_now_ms();

    /* Main frame processing loop */
    for (;;) {
        conn->deadline_ms = h2_next_deadline(conn, settings_dl);
        if (!conn->deadline_ms) {
            /* 121.46: nothing in progress -- the connection is idle, and
             * only a request ends that. */
            long left = last_request + idle_ms - h2_now_ms();
            if (left <= 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id, H2_ERR_NONE);
                goto done;
            }
            conn->idle_timeout_ms = (uint32_t)left;
        }

        H2FrameHeader hdr;
        memset(&hdr, 0, sizeof(hdr));
        errno = 0;
        uint8_t *payload = h2_frame_recv(conn, fd, ssl, &hdr);
        /* 135.3j (6): NULL is always the end of the connection -- the peer
         * closed, a deadline passed, the frame was over our size limit
         * (GOAWAY already sent) or the buffer could not grow -- except for
         * EAGAIN with nothing read: no frame began within the idle wait,
         * which the check above turns into a GOAWAY. */
        if (!payload) {
            if (errno == EAGAIN && !conn->goaway_sent &&
                hdr.length == 0 && hdr.type == 0 && hdr.stream_id == 0)
                continue;
            goto done;
        }

        /* Frame size validation (60.2.4) — reject oversized frames */
        if (hdr.length > conn->local_settings.max_frame_size) {
            h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                           H2_ERR_FRAME_SIZE);
            goto done;
        }

        /* 121.44: the preface is followed by the client's SETTINGS frame
         * (RFC 9113 3.4); that frame closes the preface phase. */
        if (settings_dl) {
            if (hdr.type != H2_FRAME_SETTINGS || (hdr.flags & H2_FLAG_ACK)) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            settings_dl = 0;
            last_request = h2_now_ms();
        }

        /* 121.46: RFC 9113 6.10 -- a header block is a contiguous sequence
         * of frames.  Anything but a CONTINUATION on the same stream while
         * one is open, or a CONTINUATION with none open, is a connection
         * error. */
        if ((hb_sid && (hdr.type != H2_FRAME_CONTINUATION ||
                        hdr.stream_id != hb_sid)) ||
            (!hb_sid && hdr.type == H2_FRAME_CONTINUATION)) {
            h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                           H2_ERR_PROTOCOL);
            goto done;
        }
        if (hdr.type == H2_FRAME_HEADERS || hdr.type == H2_FRAME_CONTINUATION ||
            hdr.type == H2_FRAME_DATA)
            last_request = h2_now_ms();

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
            /* 121.50 (1): a larger INITIAL_WINDOW_SIZE opens every stream's
             * window. */
            if (h2_flush_pending(conn, fd, ssl) < 0) goto done;
            break;

        case H2_FRAME_HEADERS: {
            uint32_t sid = hdr.stream_id;
            if (sid == 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }

            /* Skip padding and priority if present */
            uint8_t *hblock = payload;
            uint32_t hblock_len = hdr.length;
            if (hdr.flags & H2_FLAG_PADDED) {
                uint8_t pad_len = hblock_len ? hblock[0] : 0;
                if (hblock_len < 1 || pad_len >= hblock_len) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_PROTOCOL);
                    goto done;
                }
                hblock++;
                hblock_len -= 1u + pad_len;
            }
            if (hdr.flags & H2_FLAG_PRIORITY) {
                if (hblock_len < 5) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_PROTOCOL);
                    goto done;
                }
                hblock += 5; /* skip 4-byte dep + 1-byte weight */
                hblock_len -= 5;
            }

            H2Stream *stream = h2_stream_find(conn, sid);
            if (stream) {
                /* A second header block on a stream is trailers: only after
                 * the request headers, and it must end the stream. */
                if (!stream->receiving || !(hdr.flags & H2_FLAG_END_STREAM)) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_PROTOCOL);
                    goto done;
                }
                stream->block_trailers = 1;
                stream->block_end_stream = 1;
            } else {
                /* A new stream: client-initiated streams are odd and
                 * increasing (RFC 9113 5.1.1); a lower one is closed. */
                if (!(sid & 1u) || sid <= conn->last_stream_id) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   (sid & 1u) ? H2_ERR_STREAM_CLOSED
                                              : H2_ERR_PROTOCOL);
                    goto done;
                }
                /* Enforce max total streams (60.2.3) and the
                 * MAX_CONCURRENT_STREAMS we advertised. */
                if (++total_streams > H2_MAX_STREAMS_TOTAL) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_ENHANCE_YOUR_CALM);
                    goto done;
                }
                if (h2_open_streams(conn) >=
                        conn->local_settings.max_concurrent_streams ||
                    !(stream = h2_stream_get(conn, sid))) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_REFUSED_STREAM);
                    goto done;
                }
                h2_stream_transition(stream, H2_FRAME_HEADERS, hdr.flags, 0);
                stream->block_end_stream = (hdr.flags & H2_FLAG_END_STREAM) != 0;
                stream->deadline_ms = h2_now_ms() + (long)conn->read_timeout_ms;
            }

            if (h2_append_block(conn, stream, hblock, hblock_len) < 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_ENHANCE_YOUR_CALM);
                goto done;
            }
            if (!(hdr.flags & H2_FLAG_END_HEADERS)) {
                hb_sid = sid;            /* wait for CONTINUATION frames */
                break;
            }
            if (h2_block_done(conn, fd, ssl, client_ip, stream) < 0) goto done;
            break;
        }

        case H2_FRAME_CONTINUATION: {
            /* hb_sid == hdr.stream_id here (checked above) */
            H2Stream *stream = h2_stream_find(conn, hdr.stream_id);
            if (!stream) goto done;
            /* 121.44: the same ceiling as HEADERS. */
            if (h2_append_block(conn, stream, payload, hdr.length) < 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_ENHANCE_YOUR_CALM);
                goto done;
            }
            if (!(hdr.flags & H2_FLAG_END_HEADERS)) break;
            /* 121.46: the block is whole -- decode it, as a HEADERS frame
             * with END_HEADERS would have been.  This used to stop here
             * ("only single-frame headers") and the request was never
             * answered. */
            hb_sid = 0;
            if (h2_block_done(conn, fd, ssl, client_ip, stream) < 0) goto done;
            break;
        }

        case H2_FRAME_DATA: {
            uint32_t sid = hdr.stream_id;
            if (sid == 0) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            /* The whole frame, padding included, counts against the
             * connection window; give it back. */
            conn->conn_window_recv -= (int32_t)hdr.length;
            if (hdr.length > 0) {
                h2_send_window_update(conn, fd, ssl, 0, hdr.length);
                conn->conn_window_recv += (int32_t)hdr.length;
            }
            H2Stream *stream = h2_stream_find(conn, sid);
            if (!stream) {
                /* DATA on a stream never opened is a connection error; on
                 * one already closed (answered, refused, reset) it is late
                 * and dropped. */
                if (!(sid & 1u) || sid > conn->last_stream_id) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_PROTOCOL);
                    goto done;
                }
                break;
            }
            if (!stream->receiving) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            const uint8_t *data = payload;
            uint32_t dlen = hdr.length;
            if (hdr.flags & H2_FLAG_PADDED) {
                uint8_t pad_len = dlen ? data[0] : 0;
                if (dlen < 1 || pad_len >= dlen) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_PROTOCOL);
                    goto done;
                }
                data++;
                dlen -= 1u + pad_len;
            }
            /* 121.46: the body is kept for the handler (it was counted and
             * dropped).  Over max_body is 413; over max_body buffered across
             * the connection's streams is REFUSED_STREAM (safe to retry). */
            if (stream->body_len + dlen > (size_t)srv_limits.max_body) {
                if (h2_refuse_body(conn, fd, ssl, stream) < 0) goto done;
                break;
            }
            if (h2_body_buffered(conn) + dlen > (size_t)srv_limits.max_body) {
                h2_send_rst_stream(conn, fd, ssl, sid, H2_ERR_REFUSED_STREAM);
                h2_stream_release(stream);
                break;
            }
            if (stream->body_len + dlen + 1 > stream->body_cap) {
                size_t nc = stream->body_cap ? stream->body_cap * 2 : 1024;
                while (nc < stream->body_len + dlen + 1) nc *= 2;
                if (nc > (size_t)srv_limits.max_body + 1)
                    nc = (size_t)srv_limits.max_body + 1;
                char *nb = realloc(stream->body, nc);
                if (!nb) goto done;
                stream->body = nb;
                stream->body_cap = nc;
            }
            if (dlen) memcpy(stream->body + stream->body_len, data, dlen);
            stream->body_len += dlen;
            stream->body[stream->body_len] = '\0';

            if (hdr.flags & H2_FLAG_END_STREAM) {
                if (h2_finish(conn, fd, ssl, client_ip, stream) < 0) goto done;
            } else if (hdr.length > 0) {
                h2_send_window_update(conn, fd, ssl, sid, hdr.length);
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
            /* 121.50 (1): a window may not pass 2^31-1 (RFC 9113 6.9.1):
             * FLOW_CONTROL_ERROR for the connection's, RST_STREAM for a
             * stream's.  The sum is taken wide -- it used to be a signed
             * 32-bit overflow. */
            if (hdr.stream_id == 0) {
                if ((int64_t)conn->conn_window_send + increment >
                    (int64_t)H2_MAX_WINDOW_SIZE) {
                    h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                                   H2_ERR_FLOW_CONTROL);
                    goto done;
                }
                conn->conn_window_send += (int32_t)increment;
            } else {
                H2Stream *stream = h2_stream_find(conn, hdr.stream_id);
                if (stream) {
                    if ((int64_t)stream->window + increment >
                        (int64_t)H2_MAX_WINDOW_SIZE) {
                        h2_send_rst_stream(conn, fd, ssl, hdr.stream_id,
                                           H2_ERR_FLOW_CONTROL);
                        h2_stream_release(stream);
                        break;
                    }
                    stream->window += (int32_t)increment;
                }
            }
            if (h2_flush_pending(conn, fd, ssl) < 0) goto done;
            break;
        }

        case H2_FRAME_RST_STREAM: {
            if (hdr.stream_id == 0 || hdr.length != 4) {
                h2_send_goaway(conn, fd, ssl, conn->last_stream_id,
                               H2_ERR_PROTOCOL);
                goto done;
            }
            H2Stream *stream = h2_stream_find(conn, hdr.stream_id);
            if (stream) h2_stream_release(stream);
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

        if (hdr.type == H2_FRAME_HEADERS || hdr.type == H2_FRAME_CONTINUATION ||
            hdr.type == H2_FRAME_DATA)
            last_request = h2_now_ms();   /* a handler's run time is not idle */
        if (conn->goaway_sent) break;
    }

done:;
    int goaway = conn->goaway_sent;
    h2_conn_free(conn);
    return goaway;
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

/* tls_send_error — answer a request the TLS loop could not frame, then the
 * caller closes (121.17; send_error's counterpart for an SSL session). */
static void tls_send_error(SSL *ssl, uint16_t status)
{
    char hb[160];
    const char *why = reason(status);
    int n = snprintf(hb, sizeof hb,
        "HTTP/1.1 %u %s\r\nContent-Type: text/plain\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
        status, why, strlen(why), why);
    if (n > 0 && (size_t)n < sizeof hb) SSL_write(ssl, hb, n);
}

static void handle_tls_connection(int fd, SSL_CTX *ssl_ctx)
{
    char client_ip[64];
    fd_peer_ip(fd, client_ip, sizeof client_ip);

    /* 127.144: before SSL_accept, so the handshake's own writes are covered
     * too.  OpenSSL writes through write(2), which MSG_NOSIGNAL cannot
     * reach — on macOS/BSD this socket option is what protects the TLS
     * path; see http_serve_tls_workers for what happens where it is absent. */
    http_sock_nosigpipe(fd);

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
            /* 121.52: after a GOAWAY the peer may still be sending (a
             * CONTINUATION flood, an oversized frame); close lingering so
             * the GOAWAY is not lost to a TCP reset. */
            int goaway = handle_h2_connection(fd, ssl, client_ip);
            ssl_shutdown_quick(ssl, fd);
            if (goaway) http_linger_close(fd);
            else        close(fd);
            return;
        }
    }

    /* 127.60: rate limiting removed here too.  Note that this call sat AFTER
     * the ALPN branch above, so an h2 client never reached it — one more way
     * the limit was not the limit. */

    /* Extend timeout for request/response processing */
    tv.tv_sec  = (long)srv_limits.timeout_secs;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    HttpIn in;   /* grows on demand; always in.raw (121.29 REL-02) */
    if (httpin_open(&in, fd, ssl) != 0) { SSL_free(ssl); close(fd); return; }

    int requests = 0;
    int linger = 0;   /* 121.52: see http_linger_close */

    for (;;) {
        /* 121.17: the same reader as the plain loop.  This loop used to
         * detect chunked bodies and then never decode them, so the chunk
         * stream was read as the next request; and it closed every framing
         * error without a word.  Now the peer gets the status. */
        int rr = http_read_request(&in, requests);
        if (rr == HTTP_RR_CLOSE) break;
        if (rr > 0) { tls_send_error(ssl, (uint16_t)rr); linger = rr != 408; break; }

        requests++;
        int client_wants_close = req_wants_close(in.raw);
        int max_reached        = (requests >= KEEPALIVE_MAX_REQUESTS);
        int keep_alive         = (!client_wants_close && !max_reached);

        Req req = parse_request(in.raw);

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
            write_security_headers_ssl(ssl, NULL);
            write_cors_headers_ssl(ssl);
            SSL_write(ssl, "\r\n", 2);
            log_request(client_ip, &req, 204, 0);
            free_req(&req);                     /* 127.65 */
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
                write_security_headers_ssl(ssl, &bad);
                write_cors_headers_ssl(ssl);
                SSL_write(ssl, "\r\n", 2);
                if (bl) SSL_write(ssl, bad.body, (int)bl);
                log_request(client_ip, &req, 405, bl);
            }
            free_req(&req);                     /* 127.65 */
            if (!keep_alive) break;
            continue;
        }

        /* Dispatch to route table (HEAD falls back to GET handler) */
        if (getenv("TK_HTTP_DEBUG")) {
            fprintf(stderr, "[tls] route_count=%d req.path=%s req.method=%s\n",
                    route_count, req.path ? req.path : "(null)",
                    req.method ? req.method : "(null)");
            for (int _d = 0; _d < route_count && _d < 10; _d++)
                fprintf(stderr, "[tls]   [%d] %s %s\n", _d,
                        route_table[_d].method ? route_table[_d].method : "?",
                        route_table[_d].pattern ? route_table[_d].pattern : "?");
        }
        Res res = notfound_res();   /* 121.62 */
        StrPair params[HTTP_MAX_PARAMS]; int pc = 0;
        /* 127.100: route on the path component; the query string is not
         * part of the path and must not take part in matching. */
        char pathbuf_tls[2048];
        const char *rpath_tls =
            http_path_only(req.path, pathbuf_tls, sizeof pathbuf_tls);
        int tls_path_matched = 0;
        int tls_handler_called = 0;
        int tls_best = -1;
        int tls_have_params = 0;   /* 127.72 */
        for (int i = 0; i < route_count; i++) {
            if (http_match_pattern(route_table[i].pattern,
                                   rpath_tls, params, &pc)) {
                tls_path_matched = 1;
                const char *check_method = tls_is_head ? "GET" : req.method;
                if (check_method &&
                    strcmp(route_table[i].method, check_method) == 0) {
                    if (strcmp(route_table[i].pattern, "*") != 0) {
                        tls_best = i; tls_have_params = 1; break;
                    }
                    if (tls_best < 0) tls_best = i;
                }
                http_free_params(params, &pc);   /* 127.72 */
            }
        }
        if (tls_best >= 0) {
            if (!tls_have_params)
                http_match_pattern(route_table[tls_best].pattern,
                                   rpath_tls, params, &pc);
            req.params.data = params; req.params.len = (uint64_t)pc;
            res = route_table[tls_best].h(req);
            tls_handler_called = 1;
        }
        http_defer_params(params, &pc);          /* 127.72 */
        req.params.data = NULL; req.params.len = 0;
        if (tls_path_matched && !tls_handler_called && res.status == 404) {
            res = make_res(405, "Method Not Allowed");
        }

        /* HEAD: send headers with correct Content-Length, but no body
         * (RFC 9110 §9.3.2) */
        if (tls_is_head) {
            size_t head_body_len = res.body ? strlen(res.body) : 0;
            const char *conn_hdr = keep_alive ? "keep-alive" : "close";
            char hdr_buf[256];
            int hl = snprintf(hdr_buf, sizeof(hdr_buf),
                "HTTP/1.1 %u %s\r\n"
                "Content-Length: %zu\r\n"
                "Connection: %s\r\n",
                res.status, reason(res.status), head_body_len, conn_hdr);
            if (hl > 0) SSL_write(ssl, hdr_buf, hl);
            for (uint64_t hi = 0; hi < res.headers.len; hi++) {
                const char *hk = res.headers.data[hi].key;
                const char *hv = res.headers.data[hi].val;
                if (!hk || !hv) continue;
                char hline[512];
                int ln = snprintf(hline, sizeof(hline), "%s: %s\r\n", hk, hv);
                if (ln > 0) SSL_write(ssl, hline, ln);
            }
            write_security_headers_ssl(ssl, &res);
            write_cors_headers_ssl(ssl);
            SSL_write(ssl, "\r\n", 2);
            /* No body for HEAD */
            log_request(client_ip, &req, res.status, head_body_len);
        }

        /* Gzip compress if eligible — write directly (binary-safe) */
        else {
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
            write_security_headers_ssl(ssl, &res);
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
            write_security_headers_ssl(ssl, &res);
            write_cors_headers_ssl(ssl);
            SSL_write(ssl, "\r\n", 2);
            if (body_len) SSL_write(ssl, res.body, (int)body_len);
            log_request(client_ip, &req, res.status, body_len);
        }
        }

        /* 127.65: release the parsed request before the next keep-alive
         * iteration (same reasoning as the cleartext loop above).
         * 127.72: and the route bindings with it — res.body may be one. */
        free_req(&req);
        http_run_deferred_frees();

        if (!keep_alive) break;
    }

    free(in.raw);
    ssl_shutdown_quick(ssl, fd);
    if (linger) http_linger_close(fd);
    else        close(fd);
}

/*
 * tls_worker_loop — accept connections and handle them over TLS.
 * Mirrors worker_loop but uses handle_tls_connection.
 */
static void tls_worker_loop(int srv_fd, TkHttpRouter *r, SSL_CTX *ssl_ctx)
{
    install_shutdown_handler(SIGTERM, serve_sighandler);   /* 127.69 */
    install_shutdown_handler(SIGINT,  serve_sighandler);   /* 127.69 */
    install_reopen_handler();                              /* 121.53 */

    if (r) {
        route_count = r->count;
        memcpy(route_table, r->routes, (size_t)r->count * sizeof(Route));
    }

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
        reopen_logs_if_requested();
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
    if (!tls) return TK_HTTP_ERR_BIND;

#ifndef SO_NOSIGPIPE
    /*
     * 127.144 — the one gap the per-socket/per-send pair cannot close.
     *
     * OpenSSL writes the record to the socket with write(2) from inside its
     * own BIO, so MSG_NOSIGNAL never reaches that call, and this platform has
     * no SO_NOSIGPIPE to set on the socket instead.  Linux with TLS is
     * therefore the only configuration left where an abandoned request still
     * kills the process — measured at exit 141 — and the only remaining
     * remedy is the process-wide disposition.
     *
     * It is installed HERE, not at program init: http_serve_tls_workers()
     * does not return, so the change is scoped to a process that has already
     * committed to being a TLS server.  The cost is real and is documented:
     * writes to any broken pipe in this process, and in any child it execs,
     * now return EPIPE instead of terminating it.  Nothing else in the
     * runtime relies on SIGPIPE's default disposition, but a handler that
     * shells out to a pipeline will see the difference.
     */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = SIG_IGN;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPIPE, &sa, NULL);
    }
#endif

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

    install_shutdown_handler(SIGTERM, workers_parent_sighandler);  /* 127.69 */
    install_shutdown_handler(SIGINT,  workers_parent_sighandler);  /* 127.69 */
    /* 121.53 (1): SIGHUP killed the TLS supervisor (no handler), orphaning
     * its workers on the port.  It now reopens the logs and passes the
     * signal on, and each worker reopens its own at its next accept(). */
    install_shutdown_handler(SIGHUP,  workers_parent_sighandler);

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

        if (g_reload_requested) {                 /* 121.53 (1) */
            g_reload_requested = 0;
            tk_log_reopen_files();
            for (uint64_t i = 0; i < nworkers; i++)
                if (tls_pids[i] > 0) kill(tls_pids[i], SIGHUP);
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

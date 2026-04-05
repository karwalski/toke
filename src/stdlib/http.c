/*
 * http.c — Implementation of the std.http standard library module.
 *
 * Uses POSIX sockets; no external HTTP library.
 * malloc is permitted: stdlib boundary, not arena-managed compiler code.
 *
 * Story: 1.3.2  Branch: feature/stdlib-http
 */

#include "http.h"
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

/* ── Graceful shutdown flag (Story 27.1.10) ─────────────────────────── */

static volatile sig_atomic_t g_shutdown_requested = 0;

void http_shutdown(void)
{
    g_shutdown_requested = 1;
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

#define MAX_ROUTES 64

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
        case 400: return "Bad Request"; case 404: return "Not Found";
        case 408: return "Request Timeout";
        case 413: return "Payload Too Large";
        case 431: return "Request Header Fields Too Large";
        default:  return "Internal Server Error";
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
static void send_response(int fd, Res res, int keep_alive) {
    const char *conn_hdr = keep_alive ? "keep-alive" : "close";

    if (res.body != NULL && !res_has_content_length(res)) {
        /* Chunked path */
        char hdr[320];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %u %s\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: %s\r\n"
            "\r\n",
            res.status, reason(res.status), conn_hdr);
        write(fd, hdr, (size_t)hl);
        /* Default chunk size: 4096 bytes */
        http_chunked_write(fd, res.body, strlen(res.body), 4096);
    } else {
        /* Fixed Content-Length path (body may be NULL for empty responses) */
        size_t bl = res.body ? strlen(res.body) : 0;
        char hdr[320];
        int hl = snprintf(hdr, sizeof(hdr),
            "HTTP/1.1 %u %s\r\n"
            "Content-Length: %zu\r\n"
            "Connection: %s\r\n"
            "\r\n",
            res.status, reason(res.status), bl, conn_hdr);
        write(fd, hdr, (size_t)hl);
        if (bl) write(fd, res.body, bl);
    }
}

/* ── Keep-alive connection constants ────────────────────────────────── */

#define KEEPALIVE_IDLE_TIMEOUT_S 30
#define KEEPALIVE_MAX_REQUESTS   1000

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
 */
static void send_error(int fd, uint16_t status)
{
    Res r = make_res(status, reason(status));
    send_response(fd, r, 0 /* Connection: close */);
}

static void handle_connection(int fd)
{
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

        /* ── Phase 1: read headers, enforcing max_header ── */
        size_t hdr_used = 0;
        int    hdr_ok   = 0; /* set to 1 when \r\n\r\n is found */

        while (hdr_used < (size_t)srv_limits.max_header) {
            size_t space = (size_t)srv_limits.max_header - hdr_used;
            ssize_t n = read(fd, raw + hdr_used, space);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
                    send_error(fd, 408);
                free(raw); close(fd); return;
            }
            hdr_used += (size_t)n;
            raw[hdr_used] = '\0';
            if (strstr(raw, "\r\n\r\n")) { hdr_ok = 1; break; }
        }

        if (!hdr_ok) {
            /* Filled max_header bytes without finding end-of-headers */
            send_error(fd, 431);
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

        if (cl_hdr && !req_is_chunked) {
            long long cl = atoll(cl_hdr);
            if (cl < 0 ||
                (unsigned long long)cl >
                (unsigned long long)srv_limits.max_body) {
                send_error(fd, 413);
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
                            send_error(fd, 408);
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

        Res res = make_res(404, "Not Found");
        StrPair params[32]; int pc = 0;
        for (int i = 0; i < route_count; i++) {
            if (req.method && strcmp(route_table[i].method, req.method) != 0)
                continue;
            if (match_pattern(route_table[i].pattern,
                              req.path ? req.path : "", params, &pc)) {
                req.params.data = params; req.params.len = (uint64_t)pc;
                res = route_table[i].h(req); break;
            }
        }

        send_response(fd, res, keep_alive);

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

static void workers_parent_sighandler(int sig)
{
    (void)sig;
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

    close(srv); /* parent closes; workers hold their inherited copies */

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
            int new_srv = bind_listen(host, port);
            if (new_srv < 0) { g_worker_pids[slot] = -1; continue; }
            pid_t pid = fork();
            if (pid < 0) {
                close(new_srv);
                g_worker_pids[slot] = -1;
                continue;
            }
            if (pid == 0) {
                worker_loop(new_srv, r);
                _exit(0);
            }
            close(new_srv);
            g_worker_pids[slot] = pid;
        } else {
            g_worker_pids[slot] = -1;
        }
    }

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
            if (c >= 0) { out[wi++] = (char)c; ri += 2; continue; }
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
    int fd = client_tcp_connect(host, port, timeout);
    if (fd < 0) return err_resp_client("TCP connect failed", 0);

    /* build request header */
    char req_buf[4096];
    int req_len;
    if (body && body_len > 0) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %llu\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, full_path, host,
            content_type ? content_type : "application/octet-stream",
            (unsigned long long)body_len);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: close\r\n"
            "\r\n",
            method, full_path, host);
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
    return c;
}

void http_client_free(HttpClient *c)
{
    if (!c) return;
    free(c->base_url);
    free(c);
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
    int fd = client_tcp_connect(host, port, timeout);
    if (fd < 0) {
        result.is_err  = 1;
        result.err_msg = hstrdup("TCP connect failed");
        return result;
    }

    /* build + send request */
    const char *method = req.method ? req.method : "GET";
    char req_buf[4096];
    int req_len;
    if (req.body && req.body_len > 0) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Length: %llu\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            method, full_path, host,
            (unsigned long long)req.body_len);
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Connection: keep-alive\r\n"
            "\r\n",
            method, full_path, host);
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

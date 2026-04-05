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
#include <netdb.h>

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

/* ── Response serialisation ─────────────────────────────────────────── */

static const char *reason(uint16_t s) {
    switch (s) {
        case 200: return "OK"; case 201: return "Created";
        case 400: return "Bad Request"; case 404: return "Not Found";
        default:  return "Internal Server Error";
    }
}

static void send_response(int fd, Res res) {
    size_t bl = res.body ? strlen(res.body) : 0;
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %u %s\r\nContent-Length: %zu\r\n\r\n",
        res.status, reason(res.status), bl);
    write(fd, hdr, (size_t)hl);
    if (bl) write(fd, res.body, bl);
}

/* ── Server ─────────────────────────────────────────────────────────── */

int http_serve(uint16_t port) {
    int srv = socket(AF_INET, SOCK_STREAM, 0); if (srv < 0) return -1;
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(srv,(struct sockaddr*)&addr,sizeof(addr))<0){close(srv);return -1;}
    if (listen(srv, 8)<0){close(srv);return -1;}
    for (;;) {
        int fd = accept(srv, NULL, NULL); if (fd < 0) continue;
        char raw[8192] = {0}; ssize_t n = read(fd, raw, sizeof(raw)-1);
        if (n <= 0) { close(fd); continue; }
        Req req = parse_request(raw);
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
        send_response(fd, res); close(fd);
    }
    close(srv); return 0;
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

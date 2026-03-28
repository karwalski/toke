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
#include <sys/socket.h>
#include <netinet/in.h>

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

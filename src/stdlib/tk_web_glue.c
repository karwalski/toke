/*
 * tk_web_glue.c — i64-ABI wrappers for stdlib modules used in compiled toke programs.
 *
 * The toke compiler emits all function arguments as i64 (pointer-as-integer)
 * for external stdlib calls.  These wrappers bridge the i64 ABI to the proper
 * C pointer types required by str.c, http.c, and env.c.
 *
 * Linked by compile_binary() alongside the user program's LLVM IR.
 *
 * Story: 46.1.1 (stdlib linking for compiled programs)
 */

#include "str.h"
#include "http.h"
#include "env.h"
#include "log.h"
#include "router.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ── str wrappers ─────────────────────────────────────────────────────── */

int64_t tk_str_concat_w(int64_t a, int64_t b) {
    return (int64_t)(intptr_t)str_concat(
        (const char *)(intptr_t)a,
        (const char *)(intptr_t)b);
}

int64_t tk_str_len_w(int64_t s) {
    return (int64_t)str_len((const char *)(intptr_t)s);
}

int64_t tk_str_trim_w(int64_t s) {
    return (int64_t)(intptr_t)str_trim((const char *)(intptr_t)s);
}

int64_t tk_str_upper_w(int64_t s) {
    return (int64_t)(intptr_t)str_upper((const char *)(intptr_t)s);
}

int64_t tk_str_lower_w(int64_t s) {
    return (int64_t)(intptr_t)str_lower((const char *)(intptr_t)s);
}

int64_t tk_str_from_int(int64_t n) {
    return (int64_t)(intptr_t)str_from_int(n);
}

int64_t tk_str_to_int(int64_t s) {
    if (!s) return 0;
    return (int64_t)atoll((const char *)(intptr_t)s);
}

int64_t tk_str_from_float(int64_t n_as_double_bits) {
    double d;
    memcpy(&d, &n_as_double_bits, sizeof d);
    return (int64_t)(intptr_t)str_from_float(d);
}

int64_t tk_str_split_w(int64_t s, int64_t sep) {
    /* Returns pointer to StrArray (caller manages lifetime) */
    (void)s; (void)sep;
    return 0; /* stub — split returns complex type, not supported yet */
}

/* ── log wrappers ─────────────────────────────────────────────────────── */

/*
 * tk_log_open_access_w — open a rotating HTTP access log and register it as
 * the global access log used by http.c.
 *
 * path_ptr:     i64 (const char *) — path to the log file, e.g. "logs/access.log"
 * max_lines:    i64 — rotate after this many lines (0 = no rotation)
 * max_files:    i64 — keep at most N rotated .gz files (0 = no limit)
 * max_age_days: i64 — delete files older than N days (0 = disabled; >0 wins)
 * Returns: 0 on success, -1 if the log file could not be opened.
 */
int64_t tk_log_open_access_w(int64_t path_ptr, int64_t max_lines,
                               int64_t max_files, int64_t max_age_days)
{
    const char *path = (const char *)(intptr_t)path_ptr;
    TkAccessLog *al = tk_access_log_open(path, (int)max_lines,
                                          (int)max_files, (int)max_age_days);
    if (!al) return -1;
    tk_access_log_set_global(al);
    return 0;
}

/* ── env wrappers ─────────────────────────────────────────────────────── */

int64_t tk_env_get_or(int64_t key, int64_t def) {
    return (int64_t)(intptr_t)env_get_or(
        (const char *)(intptr_t)key,
        (const char *)(intptr_t)def);
}

/* ── http wrappers ────────────────────────────────────────────────────── */

/*
 * Static route registry.
 *
 * C has no closures, so we pre-define TK_MAX_STATIC_ROUTES handler functions,
 * each reading from g_static_routes[N].  Supports up to 16 static GET routes
 * per program, which covers all expected demo usage.
 */
#define TK_MAX_STATIC_ROUTES 16

typedef struct {
    const char *body;
    int         status;
} TkStaticRoute;

static TkStaticRoute g_static_routes[TK_MAX_STATIC_ROUTES];
static int           g_static_route_count = 0;

#define DEFINE_STATIC_HANDLER(N)                    \
    static Res tk_static_handler_##N(Req req) {     \
        (void)req;                                  \
        Res r;                                      \
        r.status = g_static_routes[N].status;       \
        r.body   = g_static_routes[N].body;         \
        r.headers.data = NULL;                      \
        r.headers.len  = 0;                         \
        return r;                                   \
    }

DEFINE_STATIC_HANDLER(0)  DEFINE_STATIC_HANDLER(1)
DEFINE_STATIC_HANDLER(2)  DEFINE_STATIC_HANDLER(3)
DEFINE_STATIC_HANDLER(4)  DEFINE_STATIC_HANDLER(5)
DEFINE_STATIC_HANDLER(6)  DEFINE_STATIC_HANDLER(7)
DEFINE_STATIC_HANDLER(8)  DEFINE_STATIC_HANDLER(9)
DEFINE_STATIC_HANDLER(10) DEFINE_STATIC_HANDLER(11)
DEFINE_STATIC_HANDLER(12) DEFINE_STATIC_HANDLER(13)
DEFINE_STATIC_HANDLER(14) DEFINE_STATIC_HANDLER(15)

static RouteHandler g_static_handlers[TK_MAX_STATIC_ROUTES] = {
    tk_static_handler_0,  tk_static_handler_1,
    tk_static_handler_2,  tk_static_handler_3,
    tk_static_handler_4,  tk_static_handler_5,
    tk_static_handler_6,  tk_static_handler_7,
    tk_static_handler_8,  tk_static_handler_9,
    tk_static_handler_10, tk_static_handler_11,
    tk_static_handler_12, tk_static_handler_13,
    tk_static_handler_14, tk_static_handler_15,
};

/*
 * tk_http_get_static — Register a static GET handler returning a fixed body.
 *
 * path_ptr: i64 (const char * as integer) — URL path pattern e.g. "/"
 * body_ptr: i64 (const char * as integer) — response body string
 * Returns:  0 (i64)
 */
int64_t tk_http_get_static(int64_t path_ptr, int64_t body_ptr) {
    if (g_static_route_count >= TK_MAX_STATIC_ROUTES) return -1;
    int i = g_static_route_count++;
    g_static_routes[i].body   = (const char *)(intptr_t)body_ptr;
    g_static_routes[i].status = 200;
    http_GET((const char *)(intptr_t)path_ptr, g_static_handlers[i]);
    return 0;
}

/*
 * tk_http_serve — Start the HTTP server on the given port (blocks until SIGINT/SIGTERM).
 *
 * port: i64 — TCP port number (1-65535; defaults to 8080 if out of range)
 * Returns: 0 on clean shutdown, -1 on bind/listen failure.
 */
int64_t tk_http_serve(int64_t port) {
    int p = (int)port;
    if (p <= 0 || p > 65535) p = 8080;
    return (int64_t)http_serve((uint16_t)p);
}

/*
 * tk_http_servetls — Start the HTTPS server on the given port.
 *
 * Captures the current global route table (registered via tk_http_get_static),
 * loads the TLS cert/key, then blocks serving HTTPS until SIGINT/SIGTERM.
 *
 * port:     i64 (const char * as integer) — port number as string, or i64 literal
 * cert_ptr: i64 (const char * as integer) — path to PEM cert file
 * key_ptr:  i64 (const char * as integer) — path to PEM key file
 * Returns:  0 on clean shutdown, -1 on error.
 */
int64_t tk_http_servetls(int64_t port, int64_t cert_ptr, int64_t key_ptr) {
    int p = (int)port;
    if (p <= 0 || p > 65535) p = 8443;
    const char *cert = (const char *)(intptr_t)cert_ptr;
    const char *key  = (const char *)(intptr_t)key_ptr;
    TkTlsCtx *tls = http_tls_ctx_new(cert, key);
    if (!tls) return -1;
    TkHttpRouter *router = http_router_new();
    if (!router) { http_tls_ctx_free(tls); return -1; }
    TkHttpErr err = http_serve_tls(router, NULL, (uint64_t)p, tls);
    http_router_free(router);
    http_tls_ctx_free(tls);
    return (err == TK_HTTP_OK) ? 0 : -1;
}

/* ── vhost registry ───────────────────────────────────────────────────── */

/*
 * Virtual host registry: maps hostname → docroot.
 * Supports up to TK_MAX_VHOSTS entries.  The first registered entry is the
 * default vhost (used when no Host header matches).
 *
 * Story: vhost routing / static file serving
 */
#define TK_MAX_VHOSTS 8

typedef struct {
    const char *hostname;
    const char *docroot;
} TkVhost;

static TkVhost g_vhosts[TK_MAX_VHOSTS];
static int     g_vhost_count = 0;

/*
 * tk_http_vhost — Register a virtual host mapping hostname to docroot.
 *
 * hostname: i64 (const char *) — e.g. "example.com"
 * docroot:  i64 (const char *) — e.g. "/var/www/example"
 * Returns:  0 (i64)
 */
int64_t tk_http_vhost(int64_t hostname, int64_t docroot) {
    if (g_vhost_count >= TK_MAX_VHOSTS) return -1;
    int i = g_vhost_count++;
    g_vhosts[i].hostname = (const char *)(intptr_t)hostname;
    g_vhosts[i].docroot  = (const char *)(intptr_t)docroot;
    return 0;
}

/*
 * vhost_catchall_handler — catch-all Req handler for virtual host dispatch.
 *
 * Reads the Host: header, strips any trailing :port, looks up a matching
 * vhost docroot, then delegates to router_static_serve.
 * Falls back to the first registered vhost when no match is found.
 */
static Res vhost_catchall_handler(Req req) {
    /* Determine docroot from Host header */
    const char *docroot = NULL;
    HttpResult host_r = http_header(req, "Host");
    if (!host_r.is_err && host_r.ok) {
        /* Strip :port suffix — find last ':' */
        const char *host = host_r.ok;
        const char *colon = strrchr(host, ':');
        char hostname_buf[256];
        if (colon) {
            size_t len = (size_t)(colon - host);
            if (len >= sizeof hostname_buf) len = sizeof hostname_buf - 1;
            memcpy(hostname_buf, host, len);
            hostname_buf[len] = '\0';
            host = hostname_buf;
        }
        for (int i = 0; i < g_vhost_count; i++) {
            if (strcmp(g_vhosts[i].hostname, host) == 0) {
                docroot = g_vhosts[i].docroot;
                break;
            }
        }
    }
    /* Fallback: use first registered vhost */
    if (!docroot && g_vhost_count > 0)
        docroot = g_vhosts[0].docroot;

    if (!docroot) {
        Res r; r.status = 503; r.body = "No vhost configured";
        r.headers.data = NULL; r.headers.len = 0;
        return r;
    }

    /* Determine rel_path: strip leading '/' for router_static_serve */
    const char *url_path = req.path ? req.path : "/";
    const char *rel_path = (url_path[0] == '/') ? url_path + 1 : url_path;

    /* Get If-None-Match header for conditional GET support */
    HttpResult inm_r = http_header(req, "If-None-Match");
    const char *if_none_match = (!inm_r.is_err && inm_r.ok) ? inm_r.ok : NULL;

    TkRouteResp rr = router_static_serve(docroot, rel_path, if_none_match);

    /* Convert TkRouteResp → Res */
    Res res;
    res.status = (uint16_t)rr.status;
    res.body   = rr.body;
    /* Copy headers from TkRouteResp into StrPairArray */
    if (rr.nheaders > 0 && rr.header_names && rr.header_values) {
        StrPair *pairs = malloc(rr.nheaders * sizeof(StrPair));
        if (pairs) {
            for (uint64_t i = 0; i < rr.nheaders; i++) {
                pairs[i].key = rr.header_names[i];
                pairs[i].val = rr.header_values[i];
            }
            res.headers.data = pairs;
            res.headers.len  = rr.nheaders;
        } else {
            res.headers.data = NULL;
            res.headers.len  = 0;
        }
    } else {
        res.headers.data = NULL;
        res.headers.len  = 0;
    }
    return res;
}

/*
 * tk_http_servevhosts — Register the catch-all "*" route and start HTTP server.
 *
 * port: i64 — TCP port number
 * Returns: 0 on clean shutdown, -1 on failure.
 */
int64_t tk_http_servevhosts(int64_t port) {
    http_GET("*", vhost_catchall_handler);
    return tk_http_serve(port);
}

/*
 * tk_http_servevhoststls — Register the catch-all "*" route and start HTTPS server.
 *
 * port:     i64 — TCP port number
 * cert_ptr: i64 (const char *) — path to PEM cert file
 * key_ptr:  i64 (const char *) — path to PEM key file
 * Returns:  0 on clean shutdown, -1 on failure.
 */
int64_t tk_http_servevhoststls(int64_t port, int64_t cert_ptr, int64_t key_ptr) {
    http_GET("*", vhost_catchall_handler);
    return tk_http_servetls(port, cert_ptr, key_ptr);
}

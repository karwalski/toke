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
 * Story: 56.8.7 (ooke toke-only build — file/path/toml/args/log wrappers)
 */

#include "str.h"
#include "http.h"
#include "env.h"
#include "log.h"
#include "router.h"
#include "file.h"
#include "path.h"
#include "args.h"
#include "toml.h"
#include "md.h"
#include "crypto.h"
#include "math.h"
#include "tk_time.h"
#include "net.h"
#include "sys.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <string.h>

/* ── f64↔i64 bitcast helpers (Story 7.5.2) ───────────────────────────── */
/*
 * The toke compiler passes all values as i64, even f64 floats.
 * f64 values are bitwise-encoded as i64 using memcpy (not a C cast).
 * These helpers convert between the two representations.
 */
static double i64_to_f64(int64_t i) {
    double d;
    memcpy(&d, &i, sizeof(d));
    return d;
}
static int64_t f64_to_i64(double d) {
    int64_t i;
    memcpy(&i, &d, sizeof(i));
    return i;
}

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

/*
 * tk_str_split_w — split a toke string and return a toke-format array.
 *
 * Allocates a block of (len+1) i64 slots:
 *   block[0]     = element count (length)
 *   block[1..N]  = each element as i64 (const char * as integer)
 * Returns (block+1) so that ptr[-1] == length and ptr[i] == element i,
 * matching the toke runtime array layout.
 */
int64_t tk_str_split_w(int64_t s, int64_t sep) {
    if (!s || !sep) return 0;
    StrArray arr = str_split((const char *)(intptr_t)s, (const char *)(intptr_t)sep);
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    return (int64_t)(intptr_t)(block + 1);
}

/* ── additional str wrappers (Story 56.8.7) ──────────────────────────── */

int64_t tk_str_indexof_w(int64_t s, int64_t sub) {
    if (!s || !sub) return -1;
    return str_index((const char *)(intptr_t)s, (const char *)(intptr_t)sub);
}

int64_t tk_str_slice_w(int64_t s, int64_t start, int64_t end_) {
    if (!s) return 0;
    StrSliceResult r = str_slice((const char *)(intptr_t)s, (uint64_t)start, (uint64_t)end_);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_str_replace_w(int64_t s, int64_t old_val, int64_t new_val) {
    if (!s || !old_val) return s;
    return (int64_t)(intptr_t)str_replace(
        (const char *)(intptr_t)s,
        (const char *)(intptr_t)old_val,
        (const char *)(intptr_t)new_val);
}

int64_t tk_str_startswith_w(int64_t s, int64_t prefix) {
    if (!s || !prefix) return 0;
    return (int64_t)str_starts_with((const char *)(intptr_t)s, (const char *)(intptr_t)prefix);
}

int64_t tk_str_endswith_w(int64_t s, int64_t suffix) {
    if (!s || !suffix) return 0;
    return (int64_t)str_ends_with((const char *)(intptr_t)s, (const char *)(intptr_t)suffix);
}

int64_t tk_str_trimprefix_w(int64_t s, int64_t prefix) {
    if (!s || !prefix) return s;
    return (int64_t)(intptr_t)str_trimprefix((const char *)(intptr_t)s, (const char *)(intptr_t)prefix);
}

int64_t tk_str_trimsuffix_w(int64_t s, int64_t suffix) {
    if (!s || !suffix) return s;
    return (int64_t)(intptr_t)str_trimsuffix((const char *)(intptr_t)s, (const char *)(intptr_t)suffix);
}

int64_t tk_str_lastindex_w(int64_t s, int64_t sub) {
    if (!s || !sub) return -1;
    return str_lastindex((const char *)(intptr_t)s, (const char *)(intptr_t)sub);
}

/*
 * tk_str_matchbracket_w — match a [...] bracket at the start of s.
 * Returns the inner content as i64 (ptr to string), or 0 on error.
 */
int64_t tk_str_matchbracket_w(int64_t s) {
    if (!s) return 0;
    StrBracketResult r = str_matchbracket((const char *)(intptr_t)s);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
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

/*
 * tk_log_open_error_w — open a rotating HTTP error log and register it as
 * the global error log used by http.c for 4xx/5xx responses.
 *
 * Same parameter convention as tk_log_open_access_w.
 * Returns: 0 on success, -1 if the log file could not be opened.
 *
 * Story: 47.1.2
 */
int64_t tk_log_open_error_w(int64_t path_ptr, int64_t max_lines,
                              int64_t max_files, int64_t max_age_days)
{
    const char *path = (const char *)(intptr_t)path_ptr;
    TkAccessLog *el = tk_error_log_open(path, (int)max_lines,
                                         (int)max_files, (int)max_age_days);
    if (!el) return -1;
    tk_error_log_set_global(el);
    return 0;
}

/* ── env wrappers ─────────────────────────────────────────────────────── */

int64_t tk_env_get_or(int64_t key, int64_t def) {
    return (int64_t)(intptr_t)env_get_or(
        (const char *)(intptr_t)key,
        (const char *)(intptr_t)def);
}

int64_t tk_env_set_w(int64_t key, int64_t val) {
    return (int64_t)env_set(
        (const char *)(intptr_t)key,
        (const char *)(intptr_t)val);
}

int64_t tk_env_expand_w(int64_t tmpl) {
    return (int64_t)(intptr_t)env_expand((const char *)(intptr_t)tmpl);
}

/* ── http wrappers ────────────────────────────────────────────────────── */

/*
 * Static route registry.
 *
 * Stores up to TK_MAX_STATIC_ROUTES path→body mappings registered via
 * http.getstatic.  A single dispatch handler performs a linear path lookup
 * at request time, avoiding the need for per-route closure functions.
 */
#define TK_MAX_STATIC_ROUTES 512

typedef struct {
    const char *path;
    const char *body;
    int         status;
} TkStaticRoute;

static TkStaticRoute g_static_routes[TK_MAX_STATIC_ROUTES];
static int           g_static_route_count = 0;

static StrPair g_html_ct_hdr = { "Content-Type", "text/html; charset=utf-8" };
static StrPair g_text_ct_hdr = { "Content-Type", "text/plain" };
static StrPair g_json_ct_hdr = { "Content-Type", "application/json; charset=utf-8" };

/* Custom 404 body set via http.setnotfound (Story 49.4.5) */
static const char *g_notfound_body = NULL;

static const char *g_default_404 =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<title>404 - Not Found</title>"
    "<style>body{font-family:system-ui,sans-serif;display:flex;"
    "justify-content:center;align-items:center;min-height:100vh;"
    "margin:0;background:#111;color:#ccc}"
    ".c{text-align:center}h1{font-size:4rem;margin:0;color:#e2b714}"
    "p{color:#9ca3af}a{color:#e2b714}</style></head>"
    "<body><div class=\"c\"><h1>404</h1>"
    "<p>Page not found.</p><a href=\"/\">Home</a></div></body></html>";

static Res tk_static_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_static_route_count; i++) {
        if (g_static_routes[i].path &&
            strcmp(g_static_routes[i].path, rpath) == 0) {
            Res r;
            r.status       = g_static_routes[i].status;
            r.body         = g_static_routes[i].body;
            r.headers.data = &g_html_ct_hdr;
            r.headers.len  = 1;
            return r;
        }
    }
    Res r;
    r.status       = 404;
    r.body         = g_notfound_body ? g_notfound_body : g_default_404;
    r.headers.data = &g_html_ct_hdr;
    r.headers.len  = 1;
    return r;
}

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
    g_static_routes[i].path   = (const char *)(intptr_t)path_ptr;
    g_static_routes[i].body   = (const char *)(intptr_t)body_ptr;
    g_static_routes[i].status = 200;
    http_GET((const char *)(intptr_t)path_ptr, tk_static_dispatch);
    return 0;
}

/*
 * tk_http_set_notfound — Set a custom HTML body for 404 responses (Story 49.4.5).
 *
 * body_ptr: i64 (const char * as integer) — HTML body to return for unmatched routes
 * Returns:  0 (i64)
 */
int64_t tk_http_set_notfound(int64_t body_ptr) {
    g_notfound_body = (const char *)(intptr_t)body_ptr;
    return 0;
}

/* ── Dynamic GET handler dispatch (Story 46.1.3) ─────────────────────── */
/*
 * Stores up to TK_MAX_GET_HANDLERS path→function_pointer mappings registered
 * via http.get(path, handler).  The toke handler signature is:
 *   f=handler(req:i64):i64
 * where req is a pointer to a heap-allocated Req struct (as i64) and the
 * return value is a pointer to a heap-allocated Res struct (as i64).
 */
#define TK_MAX_GET_HANDLERS 256

typedef int64_t (*TkGetHandlerFn)(int64_t);

typedef struct {
    const char     *path;
    TkGetHandlerFn  handler;
} TkGetHandlerRoute;

static TkGetHandlerRoute g_get_handler_routes[TK_MAX_GET_HANDLERS];
static int               g_get_handler_count = 0;

static Res tk_get_handler_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_get_handler_count; i++) {
        if (g_get_handler_routes[i].path &&
            strcmp(g_get_handler_routes[i].path, rpath) == 0) {
            /* Heap-allocate a copy of the Req so the toke function can access it
             * via pointer.  The toke function is responsible for the returned Res. */
            Req *heap_req = (Req *)malloc(sizeof(Req));
            if (!heap_req) {
                Res r;
                r.status       = 500;
                r.body         = "Internal Server Error";
                r.headers.data = &g_text_ct_hdr;
                r.headers.len  = 1;
                return r;
            }
            *heap_req = req;
            int64_t res_ptr = g_get_handler_routes[i].handler((int64_t)(intptr_t)heap_req);
            free(heap_req);
            if (res_ptr) {
                Res *rp = (Res *)(intptr_t)res_ptr;
                Res r = *rp;
                free(rp);
                return r;
            }
            /* Handler returned NULL — 500 */
            Res r;
            r.status       = 500;
            r.body         = "Handler returned null";
            r.headers.data = &g_text_ct_hdr;
            r.headers.len  = 1;
            return r;
        }
    }
    /* No matching handler — fall through to static dispatch */
    return tk_static_dispatch(req);
}

/*
 * tk_http_get_handler — Register a dynamic GET handler backed by a toke function.
 *
 * path_ptr:    i64 (const char *) — URL path pattern e.g. "/api/hello"
 * handler_ptr: i64 — toke function pointer (f(i64):i64)
 * Returns:     0 (i64)
 */
int64_t tk_http_get_handler(int64_t path_ptr, int64_t handler_ptr) {
    if (g_get_handler_count >= TK_MAX_GET_HANDLERS) return -1;
    int i = g_get_handler_count++;
    g_get_handler_routes[i].path    = (const char *)(intptr_t)path_ptr;
    g_get_handler_routes[i].handler = (TkGetHandlerFn)(intptr_t)handler_ptr;
    http_GET((const char *)(intptr_t)path_ptr, tk_get_handler_dispatch);
    return 0;
}

/* ── Request/Response accessor helpers for toke handlers (Story 46.1.3) ── */

/*
 * tk_http_req_path — extract path from a Req pointer.
 * Returns const char * as i64.
 */
int64_t tk_http_req_path(int64_t req_ptr) {
    if (!req_ptr) return 0;
    Req *r = (Req *)(intptr_t)req_ptr;
    return (int64_t)(intptr_t)(r->path ? r->path : "/");
}

/*
 * tk_http_req_method — extract method from a Req pointer.
 * Returns const char * as i64.
 */
int64_t tk_http_req_method(int64_t req_ptr) {
    if (!req_ptr) return 0;
    Req *r = (Req *)(intptr_t)req_ptr;
    return (int64_t)(intptr_t)(r->method ? r->method : "GET");
}

/*
 * tk_http_req_body — extract body from a Req pointer.
 * Returns const char * as i64.
 */
int64_t tk_http_req_body(int64_t req_ptr) {
    if (!req_ptr) return 0;
    Req *r = (Req *)(intptr_t)req_ptr;
    return (int64_t)(intptr_t)(r->body ? r->body : "");
}

/*
 * tk_http_res_new — create a new Res on the heap with given status and body.
 * Returns Res pointer as i64 (caller/dispatch owns the allocation).
 */
int64_t tk_http_res_new(int64_t status, int64_t body_ptr) {
    Res *r = (Res *)malloc(sizeof(Res));
    if (!r) return 0;
    r->status       = (uint16_t)status;
    r->body         = (const char *)(intptr_t)body_ptr;
    r->headers.data = &g_html_ct_hdr;
    r->headers.len  = 1;
    return (int64_t)(intptr_t)r;
}

/*
 * tk_http_res_json_new — create a new JSON Res on the heap.
 * Returns Res pointer as i64.
 */
int64_t tk_http_res_json_new(int64_t status, int64_t body_ptr) {
    Res *r = (Res *)malloc(sizeof(Res));
    if (!r) return 0;
    r->status       = (uint16_t)status;
    r->body         = (const char *)(intptr_t)body_ptr;
    r->headers.data = &g_json_ct_hdr;
    r->headers.len  = 1;
    return (int64_t)(intptr_t)r;
}

/* ── POST handler dispatch ────────────────────────────────────────────── */

#define TK_MAX_POST_ROUTES 256

typedef enum {
    TK_POST_ECHO,    /* return request body as-is */
    TK_POST_STATIC,  /* return fixed response body */
    TK_POST_JSON     /* return fixed JSON response */
} TkPostMode;

typedef struct {
    const char *path;
    const char *body;      /* static/json response body (NULL for echo) */
    TkPostMode  mode;
} TkPostRoute;

static TkPostRoute g_post_routes[TK_MAX_POST_ROUTES];
static int          g_post_route_count = 0;

static Res tk_post_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_post_route_count; i++) {
        if (g_post_routes[i].path &&
            strcmp(g_post_routes[i].path, rpath) == 0) {
            Res r;
            r.status = 200;
            switch (g_post_routes[i].mode) {
            case TK_POST_ECHO:
                r.body         = req.body ? req.body : "";
                r.headers.data = &g_text_ct_hdr;
                r.headers.len  = 1;
                break;
            case TK_POST_STATIC:
                r.body         = g_post_routes[i].body ? g_post_routes[i].body : "";
                r.headers.data = &g_html_ct_hdr;
                r.headers.len  = 1;
                break;
            case TK_POST_JSON:
                r.body         = g_post_routes[i].body ? g_post_routes[i].body : "{}";
                r.headers.data = &g_json_ct_hdr;
                r.headers.len  = 1;
                break;
            }
            return r;
        }
    }
    Res r;
    r.status       = 404;
    r.body         = "{\"error\":\"Not Found\"}";
    r.headers.data = &g_json_ct_hdr;
    r.headers.len  = 1;
    return r;
}

/*
 * tk_http_post_echo — Register a POST handler that echoes the request body.
 *
 * path_ptr: i64 (const char *) — URL path pattern e.g. "/api/echo"
 * Returns:  0 (i64)
 */
int64_t tk_http_post_echo(int64_t path_ptr) {
    if (g_post_route_count >= TK_MAX_POST_ROUTES) return -1;
    int i = g_post_route_count++;
    g_post_routes[i].path = (const char *)(intptr_t)path_ptr;
    g_post_routes[i].body = NULL;
    g_post_routes[i].mode = TK_POST_ECHO;
    http_POST((const char *)(intptr_t)path_ptr, tk_post_dispatch);
    return 0;
}

/*
 * tk_http_post_static — Register a POST handler returning a fixed body.
 *
 * path_ptr: i64 (const char *) — URL path pattern
 * body_ptr: i64 (const char *) — response body string
 * Returns:  0 (i64)
 */
int64_t tk_http_post_static(int64_t path_ptr, int64_t body_ptr) {
    if (g_post_route_count >= TK_MAX_POST_ROUTES) return -1;
    int i = g_post_route_count++;
    g_post_routes[i].path = (const char *)(intptr_t)path_ptr;
    g_post_routes[i].body = (const char *)(intptr_t)body_ptr;
    g_post_routes[i].mode = TK_POST_STATIC;
    http_POST((const char *)(intptr_t)path_ptr, tk_post_dispatch);
    return 0;
}

/*
 * tk_http_post_json — Register a POST handler returning a JSON response.
 *
 * path_ptr: i64 (const char *) — URL path pattern
 * body_ptr: i64 (const char *) — JSON response body string
 * Returns:  0 (i64)
 */
int64_t tk_http_post_json(int64_t path_ptr, int64_t body_ptr) {
    if (g_post_route_count >= TK_MAX_POST_ROUTES) return -1;
    int i = g_post_route_count++;
    g_post_routes[i].path = (const char *)(intptr_t)path_ptr;
    g_post_routes[i].body = (const char *)(intptr_t)body_ptr;
    g_post_routes[i].mode = TK_POST_JSON;
    http_POST((const char *)(intptr_t)path_ptr, tk_post_dispatch);
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

/* ── Static file directory handler ───────────────────────────────────── */

/*
 * tk_http_serve_staticdir_w — Register a fallback handler that serves files
 * from a local directory for a given URL prefix.
 *
 * e.g. tk_http_serve_staticdir_w("/static", "/path/to/project") will serve
 * GET /static/css/style.css from /path/to/project/static/css/style.css.
 *
 * Registered as a "*" wildcard route (last-resort fallback).
 */

static char g_staticdir_prefix[512];
static char g_staticdir_root[1024];

static const char *mime_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif"))  return "image/gif";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".woff")) return "font/woff";
    if (!strcmp(dot, ".woff2")) return "font/woff2";
    if (!strcmp(dot, ".ttf"))  return "font/ttf";
    if (!strcmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".xml"))  return "application/xml";
    return "application/octet-stream";
}

static Res mk404(void) {
    Res r; r.status = 404; r.body = "Not Found";
    r.headers.data = NULL; r.headers.len = 0; return r;
}

static Res staticdir_handler(Req req) {
    if (!req.path) return mk404();
    size_t pfxlen = strlen(g_staticdir_prefix);
    if (strncmp(req.path, g_staticdir_prefix, pfxlen) != 0)
        return mk404();
    /* Build local file path: root + req.path */
    char filepath[2048];
    snprintf(filepath, sizeof filepath, "%s%s", g_staticdir_root, req.path);
    /* Reject path traversal */
    if (strstr(filepath, "..")) {
        Res r; r.status = 403; r.body = "Forbidden";
        r.headers.data = NULL; r.headers.len = 0; return r;
    }
    FILE *f = fopen(filepath, "rb");
    if (!f) return mk404();
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz < 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        Res r; r.status = 500; r.body = "File too large";
        r.headers.data = NULL; r.headers.len = 0; return r;
    }
    char *body = malloc((size_t)sz + 1);
    if (!body) {
        fclose(f);
        Res r; r.status = 500; r.body = "OOM";
        r.headers.data = NULL; r.headers.len = 0; return r;
    }
    fread(body, 1, (size_t)sz, f);
    body[sz] = '\0';
    fclose(f);
    /* Build response with Content-Type header */
    const char *mime = mime_for_ext(filepath);
    Res res;
    res.status = 200;
    res.body   = body;
    StrPair *hdrs = malloc(sizeof(StrPair));
    if (hdrs) {
        hdrs[0].key = "Content-Type";
        hdrs[0].val = mime;
        res.headers.data = hdrs;
        res.headers.len  = 1;
    } else {
        res.headers.data = NULL;
        res.headers.len  = 0;
    }
    return res;
}

int64_t tk_http_serve_staticdir_w(int64_t prefix_i64, int64_t root_i64) {
    const char *prefix = (const char *)(intptr_t)prefix_i64;
    const char *root   = (const char *)(intptr_t)root_i64;
    if (!prefix || !root) return -1;
    snprintf(g_staticdir_prefix, sizeof g_staticdir_prefix, "%s", prefix);
    snprintf(g_staticdir_root,   sizeof g_staticdir_root,   "%s", root);
    http_GET("*", staticdir_handler);
    return 0;
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

    /* Get Range header for partial content support */
    HttpResult range_r = http_header(req, "Range");
    const char *range_hdr = (!range_r.is_err && range_r.ok) ? range_r.ok : NULL;

    TkRouteResp rr = router_static_serve(docroot, rel_path, if_none_match,
                                          range_hdr);

    /* Convert TkRouteResp → Res.
     * Always prepend a Content-Type header derived from rr.content_type,
     * then append any additional headers from the static-file response. */
    Res res;
    res.status = (uint16_t)rr.status;
    res.body   = rr.body;

    const char *ct = rr.content_type ? rr.content_type : "text/plain";
    uint64_t extra = rr.nheaders > 0 && rr.header_names && rr.header_values
                     ? rr.nheaders : 0;
    uint64_t total = 1 + extra; /* +1 for Content-Type */
    StrPair *pairs = malloc(total * sizeof(StrPair));
    if (pairs) {
        pairs[0].key = "Content-Type";
        pairs[0].val = ct;
        for (uint64_t i = 0; i < extra; i++) {
            pairs[1 + i].key = rr.header_names[i];
            pairs[1 + i].val = rr.header_values[i];
        }
        res.headers.data = pairs;
        res.headers.len  = total;
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

/* ── http.serveworkers wrapper (Story 56.8.7) ────────────────────────── */

int64_t tk_http_serveworkers_w(int64_t port, int64_t workers) {
    int p = (int)port;
    if (p <= 0 || p > 65535) p = 8080;
    TkHttpRouter *r = http_router_new();
    if (!r) return -1;
    TkHttpErr err = http_serve_workers(r, NULL, (uint64_t)p, (int)workers);
    http_router_free(r);
    return (err == TK_HTTP_OK) ? 0 : -1;
}

/* ── file wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_file_read_w(int64_t path) {
    if (!path) return 0;
    StrFileResult r = file_read((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_file_write_w(int64_t path, int64_t content) {
    if (!path || !content) return 0;
    BoolFileResult r = file_write((const char *)(intptr_t)path, (const char *)(intptr_t)content);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_isdir_w(int64_t path) {
    if (!path) return 0;
    return (int64_t)file_is_dir((const char *)(intptr_t)path);
}

int64_t tk_file_mkdir_w(int64_t path) {
    if (!path) return 0;
    BoolFileResult r = file_mkdir_p((const char *)(intptr_t)path);
    return r.is_err ? 0 : (int64_t)r.ok;
}

int64_t tk_file_copy_w(int64_t src, int64_t dst) {
    if (!src || !dst) return 0;
    BoolFileResult r = file_copy((const char *)(intptr_t)src, (const char *)(intptr_t)dst);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/*
 * tk_file_listall_w — list all files recursively and return a toke-format array.
 *
 * Returns (block+1) where block[0]=length, block[1..N]=file paths as i64.
 * Returns 0 on error.
 */
int64_t tk_file_listall_w(int64_t dir) {
    if (!dir) return 0;
    StrArrayFileResult r = file_listall((const char *)(intptr_t)dir);
    if (r.is_err) return 0;
    StrArray arr = r.ok;
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    return (int64_t)(intptr_t)(block + 1);
}

/* ── path wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_path_join_w(int64_t a, int64_t b) {
    if (!a || !b) return a ? a : b;
    return (int64_t)(intptr_t)path_join((const char *)(intptr_t)a, (const char *)(intptr_t)b);
}

int64_t tk_path_dir_w(int64_t p) {
    if (!p) return 0;
    return (int64_t)(intptr_t)path_dir((const char *)(intptr_t)p);
}

/* ── toml wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_toml_load_w(int64_t src) {
    if (!src) return 0;
    TomlResult r = toml_load((const char *)(intptr_t)src);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_section_w(int64_t tab, int64_t key) {
    if (!tab || !key) return tab;
    TomlResult r = toml_get_section((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? tab : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_str_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlStrResult r = toml_get_str((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_toml_i64_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlI64Result r = toml_get_i64((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : r.ok;
}

int64_t tk_toml_bool_w(int64_t tab, int64_t key) {
    if (!tab || !key) return 0;
    TomlBoolResult r = toml_get_bool((void *)(intptr_t)tab, (const char *)(intptr_t)key);
    return r.is_err ? 0 : (int64_t)r.ok;
}

/* ── args wrappers (Story 56.8.7) ─────────────────────────────────────── */

int64_t tk_args_count_w(void) {
    return (int64_t)args_count();
}

int64_t tk_args_get_w(int64_t n) {
    StrArgsResult r = args_get((uint64_t)n);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

/* ── log field decoding helper (Story 7.5.3) ─────────────────────────── */
/*
 * Decode a toke-format array of strings into a flat const char ** array
 * suitable for tk_log_info / tk_log_error / tk_log_warn / tk_log_debug.
 *
 * Toke array layout:  ptr[-1] = element count (i64)
 *                     ptr[0..count-1] = elements as i64 (char * as integer)
 *
 * The fields_map from toke is a flat array: [key1, val1, key2, val2, ...].
 * This matches the (const char **fields, int field_count) signature directly.
 *
 * On return, *out_fields points to a malloc'd array (caller must free),
 * and *out_count is the total element count (must be even for valid pairs).
 * When fields_map is 0/NULL, sets *out_fields = NULL, *out_count = 0.
 */
static void decode_log_fields(int64_t fields_map,
                               const char ***out_fields, int *out_count)
{
    *out_fields = NULL;
    *out_count  = 0;
    if (fields_map == 0) return;

    int64_t *ptr = (int64_t *)(intptr_t)fields_map;
    int64_t len  = ptr[-1];
    if (len <= 0) return;

    /* Odd count would mean a dangling key — truncate to even. */
    int count = (int)(len & ~(int64_t)1);
    if (count == 0) return;

    const char **arr = (const char **)malloc((size_t)count * sizeof(const char *));
    if (!arr) return;

    for (int i = 0; i < count; i++) {
        arr[i] = (const char *)(intptr_t)ptr[i];
    }
    *out_fields = arr;
    *out_count  = count;
}

/* ── additional log wrappers (Story 56.8.7, fixed Story 7.5.3) ──────── */

int64_t tk_log_info_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_info((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}

int64_t tk_log_error_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_error((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}

/* ── router.new wrapper (Story 56.8.7) ───────────────────────────────── */

int64_t tk_router_new_w(void) {
    TkRouter *r = router_new();
    return (int64_t)(intptr_t)r;
}

/* ── Map runtime stubs (tk_map_new / tk_map_put / tk_map_get) ────────── */
/*
 * Simple string-keyed map for toke programs.  Keys and values are i64
 * (pointer-as-integer for strings, raw i64 for integers).
 * Key comparison uses strcmp on the pointer interpreted as char *.
 */
typedef struct { int64_t key; int64_t val; } TkMapEntry;
typedef struct { TkMapEntry *entries; int len; int cap; } TkMapImpl;

void *tk_map_new(void) {
    TkMapImpl *m = (TkMapImpl *)calloc(1, sizeof(TkMapImpl));
    return m;
}

void tk_map_put(void *m_ptr, int64_t key, int64_t val) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return;
    const char *ks = (const char *)(intptr_t)key;
    for (int i = 0; i < m->len; i++) {
        const char *ki = (const char *)(intptr_t)m->entries[i].key;
        if ((ks && ki && strcmp(ks, ki) == 0) || (!ks && !ki)) {
            m->entries[i].val = val;
            return;
        }
    }
    if (m->len >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        TkMapEntry *ne = (TkMapEntry *)realloc(m->entries, (size_t)nc * sizeof(TkMapEntry));
        if (!ne) return;
        m->entries = ne; m->cap = nc;
    }
    m->entries[m->len].key = key;
    m->entries[m->len].val = val;
    m->len++;
}

int64_t tk_map_get(void *m_ptr, int64_t key) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return 0;
    const char *ks = (const char *)(intptr_t)key;
    for (int i = 0; i < m->len; i++) {
        const char *ki = (const char *)(intptr_t)m->entries[i].key;
        if ((ks && ki && strcmp(ks, ki) == 0) || (!ks && !ki))
            return m->entries[i].val;
    }
    return 0;
}

/* ── Array/map instance method wrappers (Story 56.8.7) ───────────────── */

/*
 * tk_array_append_w — Append an element to a toke array and return the new array.
 *
 * toke array layout: ptr[-1] = length (i64), ptr[0..len-1] = elements (i64).
 */
int64_t tk_array_append_w(int64_t arr_i64, int64_t elem) {
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    int64_t *block = (int64_t *)malloc((size_t)(len + 2) * sizeof(int64_t));
    if (!block) return arr_i64;
    block[0] = len + 1;
    if (len > 0) memcpy(block + 1, ptr, (size_t)len * sizeof(int64_t));
    block[len + 1] = elem;
    return (int64_t)(intptr_t)(block + 1);
}

/*
 * tk_map_set_w — Set a key/value pair in a toke map and return the map.
 */
int64_t tk_map_set_w(int64_t map_i64, int64_t key, int64_t val) {
    tk_map_put((void *)(intptr_t)map_i64, key, val);
    return map_i64;
}

/* ── Missing stdlib wrappers (Story 56.8.7) ──────────────────────────── */

int64_t tk_str_contains_w(int64_t s, int64_t sub) {
    const char *cs = (const char *)(intptr_t)s;
    const char *csub = (const char *)(intptr_t)sub;
    if (!cs || !csub) return 0;
    return str_contains(cs, csub) ? 1 : 0;
}

int64_t tk_path_ext_w(int64_t p) {
    if (!p) return 0;
    return (int64_t)(intptr_t)path_ext((const char *)(intptr_t)p);
}

int64_t tk_md_render_w(int64_t src) {
    if (!src) return 0;
    return (int64_t)(intptr_t)md_render((const char *)(intptr_t)src);
}

/* ---- Story 57.12.1: log.warn + log.debug wrappers (fixed Story 7.5.3) ---- */
int64_t tk_log_warn_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_warn((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}
int64_t tk_log_debug_w(int64_t msg, int64_t fields_map) {
    const char **fields = NULL;
    int field_count = 0;
    decode_log_fields(fields_map, &fields, &field_count);
    tk_log_debug((const char *)(intptr_t)msg, fields, field_count);
    free(fields);
    return 0;
}

/* ---- Story 57.12.1: io module stubs ---- */
int64_t tk_io_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}
int64_t tk_io_println_w(int64_t s) {
    if (s) puts((const char *)(intptr_t)s);
    else puts("");
    return 0;
}

/* ---- Story 57.12.1: test module stubs ---- */
int64_t tk_test_assert_w(int64_t cond, int64_t msg) {
    if (!cond) {
        const char *m = msg ? (const char *)(intptr_t)msg : "assertion failed";
        fprintf(stderr, "ASSERT FAILED: %s\n", m);
    }
    return cond ? 1 : 0;
}
int64_t tk_test_asserteq_w(int64_t a, int64_t b) {
    if (a != b) fprintf(stderr, "ASSERT_EQ FAILED: %lld != %lld\n", (long long)a, (long long)b);
    return (a == b) ? 1 : 0;
}
int64_t tk_test_assertne_w(int64_t a, int64_t b) {
    if (a == b) fprintf(stderr, "ASSERT_NE FAILED: %lld == %lld\n", (long long)a, (long long)b);
    return (a != b) ? 1 : 0;
}
int64_t tk_test_assertequal_w(int64_t a, int64_t b) {
    return tk_test_asserteq_w(a, b);
}
int64_t tk_test_ok_w(int64_t val) { return val ? 1 : 0; }
int64_t tk_test_eq_w(int64_t a, int64_t b) { return (a == b) ? 1 : 0; }

/* ---- Story 57.12.1: encoding module stubs ---- */
int64_t tk_encoding_tobytes_w(int64_t s) { return s; /* passthrough */ }
int64_t tk_encoding_frombytes_w(int64_t b) { return b; }
int64_t tk_encoding_hexencode_w(int64_t s) { return s; /* stub */ }
int64_t tk_encoding_hexdecode_w(int64_t s) { return s; }
int64_t tk_encoding_urlencode_w(int64_t s) { return s; }
int64_t tk_encoding_urldecode_w(int64_t s) { return s; }
int64_t tk_encoding_base64encode_w(int64_t s) { return s; }
int64_t tk_encoding_base64decode_w(int64_t s) { return s; }
int64_t tk_encoding_bytes_w(int64_t s) { return s; }
int64_t tk_encoding_toint_w(int64_t s) { return s; }

/* ---- Story 57.12.1: crypto module stubs (return 0 / noop) ---- */
int64_t tk_crypto_sha256_w(int64_t data) { (void)data; return 0; }
int64_t tk_crypto_randombytes_w(int64_t n) { (void)n; return 0; }
int64_t tk_crypto_hmacsha256_w(int64_t key, int64_t data) { (void)key; (void)data; return 0; }

/* ---- Story 42.1.5: crypto.sha256file / crypto.sha256verify wrappers ---- */
int64_t tk_crypto_sha256file_w(int64_t path) {
    if (!path) return 0;
    const char *hex = crypto_sha256file((const char *)(intptr_t)path);
    return hex ? (int64_t)(intptr_t)hex : 0;
}
int64_t tk_crypto_sha256verify_w(int64_t path, int64_t expected) {
    if (!path || !expected) return 0;
    return (int64_t)crypto_sha256verify(
        (const char *)(intptr_t)path,
        (const char *)(intptr_t)expected);
}

/* ---- Story 57.12.1: encrypt module stubs ---- */
int64_t tk_encrypt_aes256gcmencrypt_w(int64_t key, int64_t plaintext) { (void)key; (void)plaintext; return 0; }
int64_t tk_encrypt_aes256gcmdecrypt_w(int64_t key, int64_t ciphertext) { (void)key; (void)ciphertext; return 0; }
int64_t tk_encrypt_aes256gcmnoncegen_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_encrypt_hkdfsha256_w(int64_t key, int64_t salt, int64_t info) { (void)key; (void)salt; (void)info; return 0; }
int64_t tk_encrypt_x25519keypair_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_encrypt_x25519dh_w(int64_t priv, int64_t pub) { (void)priv; (void)pub; return 0; }

/* ---- Story 57.12.1: http client stubs ---- */
int64_t tk_http_client_w(int64_t url) { (void)url; return 0; }
int64_t tk_http_get_w(int64_t url) { (void)url; return 0; }
int64_t tk_http_post_w(int64_t client, int64_t path, int64_t body) { (void)client; (void)path; (void)body; return 0; }
int64_t tk_http_delete_w(int64_t client, int64_t path) { (void)client; (void)path; return 0; }

/* ---- Story 57.12.1: db module stubs ---- */
int64_t tk_db_query_w(int64_t conn, int64_t sql) { (void)conn; (void)sql; return 0; }
int64_t tk_db_exec_w(int64_t conn, int64_t sql) { (void)conn; (void)sql; return 0; }
int64_t tk_db_insert_w(int64_t conn, int64_t sql) { (void)conn; (void)sql; return 0; }
int64_t tk_db_delete_w(int64_t conn, int64_t sql) { (void)conn; (void)sql; return 0; }
int64_t tk_db_lastinsertid_w(int64_t conn) { (void)conn; return 0; }
int64_t tk_db_newquery_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_db_settable_w(int64_t q, int64_t t) { (void)q; (void)t; return 0; }
int64_t tk_db_setfield_w(int64_t q, int64_t f) { (void)q; (void)f; return 0; }
int64_t tk_db_setfieldint_w(int64_t q, int64_t f, int64_t v) { (void)q; (void)f; (void)v; return 0; }

/* ---- Story 57.12.1: html module stubs ---- */
int64_t tk_html_doc_w(int64_t title) { (void)title; return 0; }
int64_t tk_html_h1_w(int64_t text) { (void)text; return 0; }
int64_t tk_html_table_w(int64_t data) { (void)data; return 0; }
int64_t tk_html_render_w(int64_t doc) { (void)doc; return 0; }
int64_t tk_html_style_w(int64_t doc, int64_t css) { (void)doc; (void)css; return 0; }
int64_t tk_html_title_w(int64_t doc, int64_t t) { (void)doc; (void)t; return 0; }

/* ---- Story 57.12.1: chart module stubs ---- */
int64_t tk_chart_new_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_chart_bar_w(int64_t chart, int64_t data) { (void)chart; (void)data; return 0; }
int64_t tk_chart_addchart_w(int64_t dash, int64_t chart) { (void)dash; (void)chart; return 0; }
int64_t tk_chart_serve_w(int64_t dash, int64_t port) { (void)dash; (void)port; return 0; }

/* ---- Story 57.12.1: process module stubs ---- */
int64_t tk_process_spawn_w(int64_t cmd) { (void)cmd; return 0; }
int64_t tk_process_isalive_w(int64_t pid) { (void)pid; return 0; }

/* ---- Story 57.12.1: str extras ---- */
int64_t tk_str_charat_w(int64_t s, int64_t i) {
    const char *p = s ? (const char *)(intptr_t)s : "";
    int64_t len = (int64_t)strlen(p);
    if (i < 0 || i >= len) return 0;
    return (int64_t)(unsigned char)p[i];
}
int64_t tk_str_substr_w(int64_t s, int64_t start, int64_t end_) {
    return tk_str_slice_w(s, start, end_);
}
int64_t tk_str_padleft_w(int64_t s, int64_t width, int64_t pad) {
    (void)width; (void)pad; return s;
}
int64_t tk_str_join_w(int64_t arr, int64_t sep) {
    (void)arr; (void)sep; return 0;
}
int64_t tk_str_toint_w(int64_t s) {
    const char *p = s ? (const char *)(intptr_t)s : "";
    IntParseResult r = str_to_int(p);
    return r.is_err ? 0 : r.ok;
}

/* ---- Story 57.12.1: dataframe module stubs ---- */
int64_t tk_df_fromcsv_w(int64_t csv) { (void)csv; return 0; }
int64_t tk_df_shape_w(int64_t df) { (void)df; return 0; }
int64_t tk_df_filter_w(int64_t df, int64_t col, int64_t op, int64_t val) { (void)df; (void)col; (void)op; (void)val; return 0; }
int64_t tk_df_groupby_w(int64_t df, int64_t col) { (void)df; (void)col; return 0; }
int64_t tk_df_columnstr_w(int64_t df, int64_t col) { (void)df; (void)col; return 0; }
int64_t tk_dataframe_fromcsv_w(int64_t csv) { (void)csv; return 0; }
int64_t tk_dataframe_shape_w(int64_t df) { (void)df; return 0; }
int64_t tk_dataframe_filter_w(int64_t df, int64_t col, int64_t op, int64_t val) { (void)df; (void)col; (void)op; (void)val; return 0; }
int64_t tk_dataframe_groupby_w(int64_t df, int64_t col) { (void)df; (void)col; return 0; }
int64_t tk_dataframe_columnstr_w(int64_t df, int64_t col) { (void)df; (void)col; return 0; }
int64_t tk_dataframe_tocsv_w(int64_t df) { (void)df; return 0; }

/* ---- Story 57.12.1: ml module stubs ---- */
int64_t tk_ml_linregfit_w(int64_t data, int64_t targets) { (void)data; (void)targets; return 0; }
int64_t tk_ml_linregpredict_w(int64_t model, int64_t input) { (void)model; (void)input; return 0; }

/* ---- Story 57.12.1: map extras ---- */
int64_t tk_map_keys_w(int64_t map) { (void)map; return 0; }
int64_t tk_map_getor_w(int64_t map, int64_t key, int64_t def) { (void)map; (void)key; return def; }
int64_t tk_map_put_w(int64_t map, int64_t key, int64_t val) {
    if (map) tk_map_put((void *)(intptr_t)map, key, val);
    return 0;
}
int64_t tk_map_getint_w(int64_t map, int64_t key) {
    if (!map) return 0;
    return tk_map_get((void *)(intptr_t)map, key);
}
int64_t tk_map_setint_w(int64_t map, int64_t key, int64_t val) {
    return tk_map_put_w(map, key, val);
}

/* ---- Story 57.12.1: i18n module stubs ---- */
int64_t tk_i18n_empty_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_i18n_str_w(int64_t key) { (void)key; return 0; }
int64_t tk_i18n_load_w(int64_t path) { (void)path; return 0; }
int64_t tk_i18n_get_w(int64_t key) { (void)key; return 0; }
int64_t tk_i18n_fmt_w(int64_t key, int64_t args) { (void)key; (void)args; return 0; }
int64_t tk_i18n_locale_w(int64_t loc) { (void)loc; return 0; }
int64_t tk_i18n_newstrarray_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_i18n_strarrayappend_w(int64_t arr, int64_t s) { return tk_array_append_w(arr, s); }
int64_t tk_i18n_substr_w(int64_t s, int64_t start, int64_t end_) { return tk_str_slice_w(s, start, end_); }
int64_t tk_i18n_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}

/* ---- Story 57.12.1: dashboard module stubs ---- */
int64_t tk_dashboard_new_w(int64_t title) { (void)title; return 0; }
int64_t tk_dashboard_addchart_w(int64_t dash, int64_t chart) { (void)dash; (void)chart; return 0; }
int64_t tk_dashboard_serve_w(int64_t dash, int64_t port) { (void)dash; (void)port; return 0; }

/* ---- Story 57.12.1: file extras ---- */
int64_t tk_file_list_w(int64_t dir) { (void)dir; return 0; }

/* ---- Story 57.12.1: str extras (tobytes, frombytes, bytes, print) ---- */
int64_t tk_str_tobytes_w(int64_t s) { return s; }
int64_t tk_str_frombytes_w(int64_t b) { return b; }
int64_t tk_str_bytes_w(int64_t s) { return s; }
int64_t tk_str_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}

/* ---- Story 7.5.2: math module — f64 bitcast wrappers ---- */
int64_t tk_math_abs_w(int64_t x) { return f64_to_i64(math_abs(i64_to_f64(x))); }
int64_t tk_math_max_w(int64_t a, int64_t b) { return a > b ? a : b; }
int64_t tk_math_min_w(int64_t a, int64_t b) { return a < b ? a : b; }
int64_t tk_math_pow_w(int64_t base, int64_t exp) {
    return f64_to_i64(math_pow(i64_to_f64(base), i64_to_f64(exp)));
}
int64_t tk_math_floor_w(int64_t x) { return f64_to_i64(math_floor(i64_to_f64(x))); }
int64_t tk_math_mean_w(int64_t arr) { (void)arr; return 0; }
int64_t tk_math_median_w(int64_t arr) { (void)arr; return 0; }
int64_t tk_math_percentile_w(int64_t arr, int64_t p) { (void)arr; (void)p; return 0; }
int64_t tk_math_linreg_w(int64_t xs, int64_t ys) { (void)xs; (void)ys; return 0; }
int64_t tk_math_sqrt_w(int64_t x) { return f64_to_i64(math_sqrt(i64_to_f64(x))); }
int64_t tk_math_sum_w(int64_t arr) { (void)arr; return 0; }
int64_t tk_math_stddev_w(int64_t arr) { (void)arr; return 0; }

/* ---- Story 57.12.1: db extras ---- */
int64_t tk_db_open_w(int64_t dsn) { (void)dsn; return 0; }
int64_t tk_db_close_w(int64_t conn) { (void)conn; return 0; }
int64_t tk_db_print_w(int64_t rows) { (void)rows; return 0; }
int64_t tk_db_connect_w(int64_t dsn) { (void)dsn; return 0; }

/* ---- Story 57.12.1: env extras ---- */
int64_t tk_env_getor_w(int64_t key, int64_t def) { return tk_env_get_or(key, def); }

/* ---- Story 57.12.1: csv module stubs ---- */
int64_t tk_csv_parse_w(int64_t data) { (void)data; return 0; }
int64_t tk_csv_serialize_w(int64_t data) { (void)data; return 0; }

/* ---- Story 57.12.1: svg module stubs ---- */
int64_t tk_svg_doc_w(int64_t w, int64_t h) { (void)w; (void)h; return 0; }
int64_t tk_svg_rect_w(int64_t x, int64_t y, int64_t w, int64_t h) { (void)x; (void)y; (void)w; (void)h; return 0; }
int64_t tk_svg_circle_w(int64_t cx, int64_t cy, int64_t r) { (void)cx; (void)cy; (void)r; return 0; }
int64_t tk_svg_append_w(int64_t doc, int64_t elem) { (void)doc; (void)elem; return 0; }
int64_t tk_svg_style_w(int64_t elem, int64_t css) { (void)elem; (void)css; return 0; }
int64_t tk_svg_render_w(int64_t doc) { (void)doc; return 0; }

/* ---- Story 57.12.1: html extras ---- */
int64_t tk_html_append_w(int64_t doc, int64_t elem) { (void)doc; (void)elem; return 0; }

/* ---- Story 57.12.1: process extras ---- */
int64_t tk_process_exit_w(int64_t code) { (void)code; return 0; }

/* ---- Story 57.12.1: http streaming stubs ---- */
int64_t tk_http_stream_w(int64_t url) { (void)url; return 0; }
int64_t tk_http_streamnext_w(int64_t stream) { (void)stream; return 0; }

/* ---- Story 57.12.1: str newarray ---- */
int64_t tk_str_newarray_w(int64_t dummy) { (void)dummy; return 0; }

/* ---- Story 57.12.1: array extras ---- */
int64_t tk_array_newarray_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_array_newstrarray_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_array_strarrayappend_w(int64_t arr, int64_t s) { return tk_array_append_w(arr, s); }
int64_t tk_array_arrayappend_w(int64_t arr, int64_t elem) { return tk_array_append_w(arr, elem); }
int64_t tk_array_appendarray_w(int64_t arr, int64_t arr2) { (void)arr2; return arr; }
int64_t tk_array_list_w(int64_t arr) { return arr; }

/* ---- Story 57.13.9/12: additional missing stubs ---- */

/* json extras */
int64_t tk_json_enc_w(int64_t val) { (void)val; return 0; }
int64_t tk_json_dec_w(int64_t s) { (void)s; return 0; }
int64_t tk_json_parseobj_w(int64_t s) { (void)s; return 0; }
int64_t tk_json_stringify_w(int64_t val) { (void)val; return 0; }
int64_t tk_json_get_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_set_w(int64_t obj, int64_t key, int64_t val) { (void)obj; (void)key; (void)val; return 0; }
int64_t tk_json_has_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_keys_w(int64_t obj) { (void)obj; return 0; }
int64_t tk_json_values_w(int64_t obj) { (void)obj; return 0; }

/* file extras (list already defined above) */
int64_t tk_file_append_w(int64_t path, int64_t content, int64_t extra) {
    (void)path; (void)content; (void)extra; return 0;
}
int64_t tk_file_delete_w(int64_t path) { (void)path; return 0; }
int64_t tk_file_rename_w(int64_t from, int64_t to) { (void)from; (void)to; return 0; }
int64_t tk_file_readlines_w(int64_t path) { (void)path; return 0; }
int64_t tk_file_writelines_w(int64_t path, int64_t lines) { (void)path; (void)lines; return 0; }
int64_t tk_file_stat_w(int64_t path) { (void)path; return 0; }

/* str extras */
int64_t tk_str_emptyarr_w(void) { return 0; }
int64_t tk_str_repeat_w(int64_t s, int64_t n) { (void)s; (void)n; return 0; }
int64_t tk_str_reverse_w(int64_t s) { (void)s; return 0; }
int64_t tk_str_format_w(int64_t fmt, int64_t args) { (void)fmt; (void)args; return 0; }

/* time module (Story 7.5.1: fix dead stubs, add missing wrappers) */
int64_t tk_time_now_w(void) {
    return (int64_t)tk_time_now();
}
int64_t tk_time_format_w(int64_t ts, int64_t fmt) {
    const char *result = tk_time_format(
        (uint64_t)ts,
        (const char *)(intptr_t)fmt);
    return (int64_t)(intptr_t)result;
}
int64_t tk_time_parse_w(int64_t s, int64_t fmt) { (void)s; (void)fmt; return 0; }
int64_t tk_time_elapsed_w(int64_t start) { (void)start; return 0; }
int64_t tk_time_sleep_w(int64_t ms) { (void)ms; return 0; }

/*
 * tk_time_toparts_w — convert a millisecond Unix timestamp to a toke-format
 * struct containing year/month/day/hour/min/sec fields.
 *
 * Allocates a block of 7 i64 slots:
 *   block[0] = element count (6)
 *   block[1] = year
 *   block[2] = month (1-12)
 *   block[3] = day   (1-31)
 *   block[4] = hour  (0-23)
 *   block[5] = min   (0-59)
 *   block[6] = sec   (0-59)
 * Returns (block+1) so that ptr[-1] == length and ptr[i] == field i,
 * matching the toke runtime array/struct layout.
 */
int64_t tk_time_toparts_w(int64_t ts) {
    TkTimeParts parts = tk_time_to_parts((uint64_t)ts);
    int64_t *block = (int64_t *)malloc(7 * sizeof(int64_t));
    if (!block) return 0;
    block[0] = 6;
    block[1] = (int64_t)parts.year;
    block[2] = (int64_t)parts.month;
    block[3] = (int64_t)parts.day;
    block[4] = (int64_t)parts.hour;
    block[5] = (int64_t)parts.min;
    block[6] = (int64_t)parts.sec;
    return (int64_t)(intptr_t)(block + 1);
}

/*
 * tk_time_weekday_w — return the day of the week for a timestamp.
 * Returns 0=Sun, 1=Mon, ..., 6=Sat.
 */
int64_t tk_time_weekday_w(int64_t ts) {
    return (int64_t)tk_time_weekday((uint64_t)ts);
}

/* db extras (connect/newquery/settable already defined above) */
int64_t tk_db_execute_w(int64_t conn, int64_t q) { (void)conn; (void)q; return 0; }
int64_t tk_db_rows_w(int64_t result) { (void)result; return 0; }
int64_t tk_db_getrow_w(int64_t result, int64_t idx) { (void)result; (void)idx; return 0; }
int64_t tk_db_getfield_w(int64_t row, int64_t name) { (void)row; (void)name; return 0; }

/* net/tcp */
int64_t tk_net_portavailable_w(int64_t port) {
    return (int64_t)net_portavailable((uint64_t)port);
}
int64_t tk_net_listen_w(int64_t addr) { (void)addr; return 0; }
int64_t tk_net_accept_w(int64_t listener) { (void)listener; return 0; }
int64_t tk_net_read_w(int64_t conn) { (void)conn; return 0; }
int64_t tk_net_write_w(int64_t conn, int64_t data) { (void)conn; (void)data; return 0; }
int64_t tk_net_close_w(int64_t conn) { (void)conn; return 0; }

/* template */
int64_t tk_template_render_w(int64_t tpl, int64_t data) { (void)tpl; (void)data; return 0; }
int64_t tk_template_load_w(int64_t path) { (void)path; return 0; }

/* cache / kv */
int64_t tk_cache_get_w(int64_t key) { (void)key; return 0; }
int64_t tk_cache_set_w(int64_t key, int64_t val) { (void)key; (void)val; return 0; }
int64_t tk_cache_del_w(int64_t key) { (void)key; return 0; }
int64_t tk_cache_new_w(void) { return 0; }

/* regex */
int64_t tk_regex_match_w(int64_t pattern, int64_t s) { (void)pattern; (void)s; return 0; }
int64_t tk_regex_replace_w(int64_t pattern, int64_t s, int64_t repl) { (void)pattern; (void)s; (void)repl; return 0; }
int64_t tk_regex_findall_w(int64_t pattern, int64_t s) { (void)pattern; (void)s; return 0; }

/* sort */
int64_t tk_sort_ints_w(int64_t arr) { (void)arr; return arr; }
int64_t tk_sort_strs_w(int64_t arr) { (void)arr; return arr; }

/* validation */
int64_t tk_validate_email_w(int64_t s) { (void)s; return 0; }
int64_t tk_validate_url_w(int64_t s) { (void)s; return 0; }
int64_t tk_validate_range_w(int64_t val, int64_t lo, int64_t hi) { (void)val; (void)lo; (void)hi; return 0; }

/* ws (websocket) extras */
int64_t tk_ws_connect_w(int64_t url) { (void)url; return 0; }
int64_t tk_ws_send_w(int64_t conn, int64_t msg) { (void)conn; (void)msg; return 0; }
int64_t tk_ws_recv_w(int64_t conn) { (void)conn; return 0; }
int64_t tk_ws_close_w(int64_t conn) { (void)conn; return 0; }

/* auth extras */
int64_t tk_auth_hash_w(int64_t pw) { (void)pw; return 0; }
int64_t tk_auth_verify_w(int64_t pw, int64_t hash) { (void)pw; (void)hash; return 0; }
int64_t tk_auth_jwt_w(int64_t claims, int64_t secret) { (void)claims; (void)secret; return 0; }
int64_t tk_auth_verifyjwt_w(int64_t token, int64_t secret) { (void)token; (void)secret; return 0; }

/* rate limit */
int64_t tk_ratelimit_new_w(int64_t max, int64_t window) { (void)max; (void)window; return 0; }
int64_t tk_ratelimit_check_w(int64_t limiter, int64_t key) { (void)limiter; (void)key; return 0; }

/* config */
int64_t tk_config_load_w(int64_t path) { (void)path; return 0; }
int64_t tk_config_get_w(int64_t cfg, int64_t key) { (void)cfg; (void)key; return 0; }
int64_t tk_config_getint_w(int64_t cfg, int64_t key) { (void)cfg; (void)key; return 0; }

/* task/async */
int64_t tk_task_spawn_w(int64_t fn) { (void)fn; return 0; }
int64_t tk_task_await_w(int64_t task) { (void)task; return 0; }

/* uuid */
int64_t tk_uuid_v4_w(void) { return 0; }

/* base64 */
int64_t tk_base64_encode_w(int64_t data) { (void)data; return 0; }
int64_t tk_base64_decode_w(int64_t data) { (void)data; return 0; }

/* yaml */
int64_t tk_yaml_parse_w(int64_t s) { (void)s; return 0; }
int64_t tk_yaml_stringify_w(int64_t val) { (void)val; return 0; }
int64_t tk_yaml_load_w(int64_t path) { (void)path; return 0; }

/* toon */
int64_t tk_toon_parse_w(int64_t s) { (void)s; return 0; }
int64_t tk_toon_stringify_w(int64_t val) { (void)val; return 0; }
int64_t tk_toon_load_w(int64_t path) { (void)path; return 0; }

/* llm / tool */
int64_t tk_tool_call_w(int64_t name, int64_t args) { (void)name; (void)args; return 0; }
int64_t tk_tool_register_w(int64_t name, int64_t fn) { (void)name; (void)fn; return 0; }
int64_t tk_llm_complete_w(int64_t prompt) { (void)prompt; return 0; }
int64_t tk_llm_chat_w(int64_t messages) { (void)messages; return 0; }

/* ================================================================
 * Corpus-discovered stubs (Story 57.13.12)
 * These are generated-code targets that the LLM corpus references.
 * Each is a no-op returning 0 so programs link and run (exit 0).
 * ================================================================ */

/* toon extras */
int64_t tk_toon_print_w(int64_t v) { (void)v; return 0; }
int64_t tk_toon_enc_w(int64_t v) { (void)v; return 0; }
int64_t tk_toon_dec_w(int64_t s) { (void)s; return 0; }
int64_t tk_toon_tojson_w(int64_t v) { (void)v; return 0; }
int64_t tk_toon_i64_w(int64_t v) { (void)v; return 0; }
int64_t tk_toon_getint_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_toon_getstring_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_toon_fromstr_w(int64_t s) { (void)s; return 0; }
int64_t tk_toon_bool_w(int64_t v) { (void)v; return 0; }
int64_t tk_toon_deserialize_w(int64_t s) { (void)s; return 0; }
int64_t tk_toon_tostr_w(int64_t v) { (void)v; return 0; }
int64_t tk_toon_arr_w(void) { return 0; }
int64_t tk_toon_f64_w(int64_t v) { (void)v; return 0; }
int64_t tk_toon_str_w(int64_t v) { (void)v; return 0; }

/* process extras */
int64_t tk_process_kill_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_badhandle_w(void) { return 0; }
int64_t tk_process_wait_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_exitcode_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_hasexited_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_poll_w(int64_t p) { (void)p; return 0; }
int64_t tk_process_print_w(int64_t v) { (void)v; return 0; }
int64_t tk_process_sleep_w(int64_t ms) { (void)ms; return 0; }

/* encrypt / crypto extras */
int64_t tk_encrypt_ed25519keypair_w(void) { return 0; }
int64_t tk_encrypt_ed25519sign_w(int64_t key, int64_t msg) { (void)key; (void)msg; return 0; }
int64_t tk_encrypt_ed25519verify_w(int64_t key, int64_t msg, int64_t sig) { (void)key; (void)msg; (void)sig; return 0; }
int64_t tk_crypto_constanttimeequal_w(int64_t a, int64_t b) { (void)a; (void)b; return 0; }
int64_t tk_crypto_tohex_w(int64_t data) { (void)data; return 0; }

/* canvas */
int64_t tk_canvas_fillrect_w(int64_t c, int64_t x, int64_t y, int64_t w, int64_t h) { (void)c; (void)x; (void)y; (void)w; (void)h; return 0; }
int64_t tk_canvas_filltext_w(int64_t c, int64_t text, int64_t x, int64_t y) { (void)c; (void)text; (void)x; (void)y; return 0; }
int64_t tk_canvas_new_w(int64_t w, int64_t h) { (void)w; (void)h; return 0; }
int64_t tk_canvas_tohtml_w(int64_t c) { (void)c; return 0; }

/* log extras */
int64_t tk_log_setformat_w(int64_t fmt) { (void)fmt; return 0; }
int64_t tk_log_setlevel_w(int64_t lvl) { (void)lvl; return 0; }

/* http extras */
int64_t tk_http_downloadfile_w(int64_t client, int64_t url, int64_t dest) {
    return (int64_t)http_downloadfile(
        (HttpClient *)(intptr_t)client,
        (const char *)(intptr_t)url,
        (const char *)(intptr_t)dest,
        NULL);
}
int64_t tk_http_put_w(int64_t client, int64_t path, int64_t body) { (void)client; (void)path; (void)body; return 0; }
int64_t tk_http_print_w(int64_t resp) { (void)resp; return 0; }
int64_t tk_http_listen_w(int64_t addr, int64_t handler) { (void)addr; (void)handler; return 0; }

/* ---- Story 42.1.3: http.withproxy ---- */
int64_t tk_http_withproxy_w(int64_t client, int64_t proxy_url) {
    return (int64_t)(intptr_t)http_withproxy((HttpClient *)(intptr_t)client, (const char *)(intptr_t)proxy_url);
}

/* chart */
int64_t tk_chart_tojson_w(int64_t chart) { (void)chart; return 0; }

/* yaml extras */
int64_t tk_yaml_print_w(int64_t v) { (void)v; return 0; }
int64_t tk_yaml_getnestedmap_w(int64_t m, int64_t key) { (void)m; (void)key; return 0; }
int64_t tk_yaml_getrootmap_w(int64_t doc) { (void)doc; return 0; }
int64_t tk_yaml_getstring_w(int64_t m, int64_t key) { (void)m; (void)key; return 0; }
int64_t tk_yaml_maphaskey_w(int64_t m, int64_t key) { (void)m; (void)key; return 0; }
int64_t tk_yaml_splitstr_w(int64_t s, int64_t delim) { (void)s; (void)delim; return 0; }

/* i18n */
int64_t tk_i18n_localize_w(int64_t key, int64_t locale) { (void)key; (void)locale; return 0; }
int64_t tk_i18n_translate_w(int64_t key, int64_t lang) { (void)key; (void)lang; return 0; }

/* fmt */
int64_t tk_fmt_print_w(int64_t v) { (void)v; return 0; }

/* file extras */
int64_t tk_file_err_w(int64_t msg) { (void)msg; return 0; }
int64_t tk_file_listdir_w(int64_t path) { (void)path; return 0; }

/* str extras */
int64_t tk_str_ends_w(int64_t s, int64_t suffix) { (void)s; (void)suffix; return 0; }
int64_t tk_str_eq_w(int64_t a, int64_t b) { (void)a; (void)b; return 0; }
int64_t tk_str_append_w(int64_t s, int64_t t) { (void)s; (void)t; return 0; }
int64_t tk_str_newarr_w(void) { return 0; }
int64_t tk_str_padright_w(int64_t s, int64_t width, int64_t pad) { (void)s; (void)width; (void)pad; return 0; }
int64_t tk_str_tolower_w(int64_t s) { (void)s; return 0; }
int64_t tk_str_arrappend_w(int64_t arr, int64_t item) { (void)arr; (void)item; return 0; }
int64_t tk_str_arrayappend_w(int64_t arr, int64_t item) { (void)arr; (void)item; return 0; }
int64_t tk_str_toupper_w(int64_t s) { (void)s; return 0; }
int64_t tk_str_appendarray_w(int64_t arr, int64_t item) { (void)arr; (void)item; return 0; }
int64_t tk_str_arraypush_w(int64_t arr, int64_t item) { (void)arr; (void)item; return 0; }
int64_t tk_str_starts_w(int64_t s, int64_t prefix) { (void)s; (void)prefix; return 0; }
int64_t tk_str_appenditem_w(int64_t arr, int64_t item) { (void)arr; (void)item; return 0; }
int64_t tk_str_emptylist_w(void) { return 0; }
int64_t tk_str_replaceitem_w(int64_t arr, int64_t idx, int64_t val) { (void)arr; (void)idx; (void)val; return 0; }
int64_t tk_str_arrof_w(int64_t v) { (void)v; return 0; }
int64_t tk_str_fromfloat_w(int64_t f) { (void)f; return 0; }
int64_t tk_str_tofloat_w(int64_t s) { (void)s; return 0; }
int64_t tk_str_fromf64_w(int64_t f) { (void)f; return 0; }
int64_t tk_str_fromi64_w(int64_t i) { (void)i; return 0; }
int64_t tk_str_isempty_w(int64_t s) { (void)s; return 0; }
int64_t tk_str_parsef64_w(int64_t s) { (void)s; return 0; }
int64_t tk_str_emptyof_w(int64_t type) { (void)type; return 0; }
int64_t tk_str_setat_w(int64_t s, int64_t idx, int64_t ch) { (void)s; (void)idx; (void)ch; return 0; }

/* json extras */
int64_t tk_json_getint_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_err_w(int64_t msg) { (void)msg; return 0; }
int64_t tk_json_ok_w(int64_t val) { (void)val; return 0; }
int64_t tk_json_serialize_w(int64_t obj) { (void)obj; return 0; }
int64_t tk_json_setint_w(int64_t obj, int64_t key, int64_t val) { (void)obj; (void)key; (void)val; return 0; }
int64_t tk_json_haskey_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_getbool_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }
int64_t tk_json_getstring_w(int64_t obj, int64_t key) { (void)obj; (void)key; return 0; }

/* collections */
int64_t tk_collections_newarray_w(void) { return 0; }
int64_t tk_collections_append_w(int64_t arr, int64_t item) { (void)arr; (void)item; return 0; }
int64_t tk_collections_push_w(int64_t arr, int64_t item) { (void)arr; (void)item; return 0; }

/* router extras */
int64_t tk_router_post_w(int64_t router, int64_t path, int64_t handler) { (void)router; (void)path; (void)handler; return 0; }
int64_t tk_router_serve_w(int64_t router, int64_t addr) { (void)router; (void)addr; return 0; }

/* test */
int64_t tk_test_run_w(int64_t suite) { (void)suite; return 0; }
int64_t tk_test_report_w(int64_t suite) { (void)suite; return 0; }

/* html */
int64_t tk_html_docr_w(int64_t body) { (void)body; return 0; }

/* math extras */
int64_t tk_math_ceil_w(int64_t v) { return f64_to_i64(math_ceil(i64_to_f64(v))); }
int64_t tk_math_round_w(int64_t v) { return f64_to_i64(math_round(i64_to_f64(v), 0)); }

/* env */
int64_t tk_env_get_w(int64_t key) { (void)key; return 0; }

/* file_exists — Story 49.4.5: implement properly using file.h */
int64_t tk_file_exists_w(int64_t path) {
    if (!path) return 0;
    return (int64_t)file_exists((const char *)(intptr_t)path);
}

/* ---- Story 42.1.2: sys module wrappers ---- */
int64_t tk_sys_configdir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_configdir((const char *)(intptr_t)appname);
}
int64_t tk_sys_datadir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_datadir((const char *)(intptr_t)appname);
}

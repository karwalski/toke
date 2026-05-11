/*
 * tk_web_glue.c — i64-ABI wrappers for HTTP/routing layer.
 *
 * This file contains ONLY the HTTP-specific wrappers (static routes,
 * dynamic GET/POST/PUT/DELETE/PATCH handlers, vhost routing, static
 * file serving, etc.).
 *
 * Per-module wrappers have been split into separate *_glue.c files:
 *   io_glue.c, str_glue.c, test_glue.c, args_glue.c, file_glue.c,
 *   env_glue.c, path_glue.c, math_glue.c, json_glue.c, log_glue.c,
 *   time_glue.c, db_glue.c, collections_glue.c, crypto_glue.c,
 *   encoding_glue.c, process_glue.c, toml_glue.c, md_glue.c,
 *   csv_glue.c, template_glue.c
 *
 * Story: 46.1.1 (stdlib linking for compiled programs)
 * Story: 56.8.7 (ooke toke-only build)
 */

#include "http.h"
#include "router.h"
#include "net.h"
#include "sys.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

/* ── f64<->i64 bitcast helpers (Story 7.5.2) ───────────────────────── */
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

/* ── Story 76.1.9d: Closure-aware dispatch helpers ────────────────────── */

static int64_t call_closure_1(int64_t fn_val, int64_t arg) {
    int64_t *pair = (int64_t *)(intptr_t)fn_val;
    int64_t real_fn = pair[0];
    int64_t env     = pair[1];
    typedef int64_t (*ClosureFn1)(int64_t, int64_t);
    ClosureFn1 f = (ClosureFn1)(intptr_t)real_fn;
    return f(env, arg);
}

static int64_t call_closure_2(int64_t fn_val, int64_t a, int64_t b) {
    int64_t *pair = (int64_t *)(intptr_t)fn_val;
    int64_t real_fn = pair[0];
    int64_t env     = pair[1];
    typedef int64_t (*ClosureFn2)(int64_t, int64_t, int64_t);
    ClosureFn2 f = (ClosureFn2)(intptr_t)real_fn;
    return f(env, a, b);
}

/* ── Static route registry ─────────────────────────────────────────── */

#define TK_MAX_STATIC_ROUTES 512

typedef struct {
    const char *path;
    const char *body;
    const char *content_type;
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

static StrPair g_mime_ct_hdr;

static Res tk_static_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_static_route_count; i++) {
        if (g_static_routes[i].path &&
            strcmp(g_static_routes[i].path, rpath) == 0) {
            Res r;
            r.status       = g_static_routes[i].status;
            r.body         = g_static_routes[i].body;
            if (g_static_routes[i].content_type) {
                g_mime_ct_hdr.key   = "Content-Type";
                g_mime_ct_hdr.val = g_static_routes[i].content_type;
                r.headers.data = &g_mime_ct_hdr;
            } else {
                r.headers.data = &g_html_ct_hdr;
            }
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

int64_t tk_http_get_static(int64_t path_ptr, int64_t body_ptr) {
    if (g_static_route_count >= TK_MAX_STATIC_ROUTES) return -1;
    int i = g_static_route_count++;
    const char *p = (const char *)(intptr_t)path_ptr;
    const char *b = (const char *)(intptr_t)body_ptr;
    g_static_routes[i].path   = p ? strdup(p) : NULL;
    g_static_routes[i].body   = b ? strdup(b) : NULL;
    g_static_routes[i].status = 200;
    http_GET(g_static_routes[i].path, tk_static_dispatch);
    return 0;
}

int64_t tk_http_get_static_mime(int64_t path_ptr, int64_t body_ptr, int64_t mime_ptr) {
    if (g_static_route_count >= TK_MAX_STATIC_ROUTES) return -1;
    int i = g_static_route_count++;
    const char *p = (const char *)(intptr_t)path_ptr;
    const char *b = (const char *)(intptr_t)body_ptr;
    const char *m = (const char *)(intptr_t)mime_ptr;
    g_static_routes[i].path         = p ? strdup(p) : NULL;
    g_static_routes[i].body         = b ? strdup(b) : NULL;
    g_static_routes[i].content_type = m ? strdup(m) : NULL;
    g_static_routes[i].status       = 200;
    http_GET(g_static_routes[i].path, tk_static_dispatch);
    return 0;
}

int64_t tk_http_set_notfound(int64_t body_ptr) {
    g_notfound_body = (const char *)(intptr_t)body_ptr;
    return 0;
}

int64_t tk_http_set_cors(int64_t origins_ptr) {
    http_set_cors((const char *)(intptr_t)origins_ptr);
    return 0;
}

/* ── Dynamic GET handler dispatch (Story 46.1.3) ─────────────────────── */

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
            Res r;
            r.status       = 500;
            r.body         = "Handler returned null";
            r.headers.data = &g_text_ct_hdr;
            r.headers.len  = 1;
            return r;
        }
    }
    return tk_static_dispatch(req);
}

int64_t tk_http_get_handler(int64_t path_ptr, int64_t handler_ptr) {
    if (g_get_handler_count >= TK_MAX_GET_HANDLERS) return -1;
    int i = g_get_handler_count++;
    g_get_handler_routes[i].path    = (const char *)(intptr_t)path_ptr;
    g_get_handler_routes[i].handler = (TkGetHandlerFn)(intptr_t)handler_ptr;
    http_GET((const char *)(intptr_t)path_ptr, tk_get_handler_dispatch);
    return 0;
}

/* ── Request/Response accessor helpers (Story 46.1.3) ──────────────── */

int64_t tk_http_req_path(int64_t req_ptr) {
    if (!req_ptr) return 0;
    Req *r = (Req *)(intptr_t)req_ptr;
    return (int64_t)(intptr_t)(r->path ? r->path : "/");
}

int64_t tk_http_req_method(int64_t req_ptr) {
    if (!req_ptr) return 0;
    Req *r = (Req *)(intptr_t)req_ptr;
    return (int64_t)(intptr_t)(r->method ? r->method : "GET");
}

int64_t tk_http_req_body(int64_t req_ptr) {
    if (!req_ptr) return 0;
    Req *r = (Req *)(intptr_t)req_ptr;
    return (int64_t)(intptr_t)(r->body ? r->body : "");
}

int64_t tk_http_res_new(int64_t status, int64_t body_ptr) {
    Res *r = (Res *)malloc(sizeof(Res));
    if (!r) return 0;
    r->status       = (uint16_t)status;
    r->body         = (const char *)(intptr_t)body_ptr;
    r->headers.data = &g_html_ct_hdr;
    r->headers.len  = 1;
    return (int64_t)(intptr_t)r;
}

int64_t tk_http_res_json_new(int64_t status, int64_t body_ptr) {
    Res *r = (Res *)malloc(sizeof(Res));
    if (!r) return 0;
    r->status       = (uint16_t)status;
    r->body         = (const char *)(intptr_t)body_ptr;
    r->headers.data = &g_json_ct_hdr;
    r->headers.len  = 1;
    return (int64_t)(intptr_t)r;
}

int64_t tk_http_req_param(int64_t req_ptr, int64_t name_ptr) {
    if (!req_ptr || !name_ptr) return (int64_t)(intptr_t)"";
    Req *r = (Req *)(intptr_t)req_ptr;
    const char *name = (const char *)(intptr_t)name_ptr;
    for (uint64_t i = 0; i < r->params.len; i++) {
        if (r->params.data[i].key && strcmp(r->params.data[i].key, name) == 0) {
            return (int64_t)(intptr_t)(r->params.data[i].val ? r->params.data[i].val : "");
        }
    }
    return (int64_t)(intptr_t)"";
}

int64_t tk_http_req_header(int64_t req_ptr, int64_t name_ptr) {
    if (!req_ptr || !name_ptr) return (int64_t)(intptr_t)"";
    Req *r = (Req *)(intptr_t)req_ptr;
    const char *name = (const char *)(intptr_t)name_ptr;
    for (uint64_t i = 0; i < r->headers.len; i++) {
        if (r->headers.data[i].key && strcasecmp(r->headers.data[i].key, name) == 0) {
            return (int64_t)(intptr_t)(r->headers.data[i].val ? r->headers.data[i].val : "");
        }
    }
    return (int64_t)(intptr_t)"";
}

int64_t tk_http_res_ok(int64_t body_ptr) {
    Res *r = (Res *)malloc(sizeof(Res));
    if (!r) return 0;
    r->status       = 200;
    r->body         = (const char *)(intptr_t)body_ptr;
    r->headers.data = &g_html_ct_hdr;
    r->headers.len  = 1;
    return (int64_t)(intptr_t)r;
}

int64_t tk_http_res_bad(int64_t msg_ptr) {
    Res *r = (Res *)malloc(sizeof(Res));
    if (!r) return 0;
    r->status       = 400;
    r->body         = (const char *)(intptr_t)msg_ptr;
    r->headers.data = &g_text_ct_hdr;
    r->headers.len  = 1;
    return (int64_t)(intptr_t)r;
}

int64_t tk_http_res_err(int64_t msg_ptr) {
    Res *r = (Res *)malloc(sizeof(Res));
    if (!r) return 0;
    r->status       = 500;
    r->body         = (const char *)(intptr_t)msg_ptr;
    r->headers.data = &g_text_ct_hdr;
    r->headers.len  = 1;
    return (int64_t)(intptr_t)r;
}

/* ── POST handler dispatch ────────────────────────────────────────────── */

#define TK_MAX_POST_ROUTES 256

typedef enum {
    TK_POST_ECHO,
    TK_POST_STATIC,
    TK_POST_JSON
} TkPostMode;

typedef struct {
    const char *path;
    const char *body;
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

int64_t tk_http_post_echo(int64_t path_ptr) {
    if (g_post_route_count >= TK_MAX_POST_ROUTES) return -1;
    int i = g_post_route_count++;
    g_post_routes[i].path = (const char *)(intptr_t)path_ptr;
    g_post_routes[i].body = NULL;
    g_post_routes[i].mode = TK_POST_ECHO;
    http_POST((const char *)(intptr_t)path_ptr, tk_post_dispatch);
    return 0;
}

int64_t tk_http_post_static(int64_t path_ptr, int64_t body_ptr) {
    if (g_post_route_count >= TK_MAX_POST_ROUTES) return -1;
    int i = g_post_route_count++;
    g_post_routes[i].path = (const char *)(intptr_t)path_ptr;
    g_post_routes[i].body = (const char *)(intptr_t)body_ptr;
    g_post_routes[i].mode = TK_POST_STATIC;
    http_POST((const char *)(intptr_t)path_ptr, tk_post_dispatch);
    return 0;
}

int64_t tk_http_post_json(int64_t path_ptr, int64_t body_ptr) {
    if (g_post_route_count >= TK_MAX_POST_ROUTES) return -1;
    int i = g_post_route_count++;
    g_post_routes[i].path = (const char *)(intptr_t)path_ptr;
    g_post_routes[i].body = (const char *)(intptr_t)body_ptr;
    g_post_routes[i].mode = TK_POST_JSON;
    http_POST((const char *)(intptr_t)path_ptr, tk_post_dispatch);
    return 0;
}

/* ── Dynamic POST handler dispatch (Story 73.1.5) ────────────────────── */

#define TK_MAX_POST_HANDLERS 256

typedef struct {
    const char         *path;
    TkGetHandlerFn      handler;
} TkPostHandlerRoute;

static TkPostHandlerRoute g_post_handler_routes[TK_MAX_POST_HANDLERS];
static int                 g_post_handler_count = 0;

static Res tk_post_handler_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_post_handler_count; i++) {
        if (g_post_handler_routes[i].path &&
            strcmp(g_post_handler_routes[i].path, rpath) == 0) {
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
            int64_t res_ptr = g_post_handler_routes[i].handler((int64_t)(intptr_t)heap_req);
            free(heap_req);
            if (res_ptr) {
                Res *rp = (Res *)(intptr_t)res_ptr;
                Res r = *rp;
                free(rp);
                return r;
            }
            Res r;
            r.status       = 500;
            r.body         = "Handler returned null";
            r.headers.data = &g_text_ct_hdr;
            r.headers.len  = 1;
            return r;
        }
    }
    return tk_post_dispatch(req);
}

int64_t tk_http_post_handler(int64_t path_ptr, int64_t handler_ptr) {
    if (g_post_handler_count >= TK_MAX_POST_HANDLERS) return -1;
    int i = g_post_handler_count++;
    g_post_handler_routes[i].path    = (const char *)(intptr_t)path_ptr;
    g_post_handler_routes[i].handler = (TkGetHandlerFn)(intptr_t)handler_ptr;
    http_POST((const char *)(intptr_t)path_ptr, tk_post_handler_dispatch);
    return 0;
}

/* ── Dynamic PUT handler dispatch (Story 73.1.5) ─────────────────────── */

#define TK_MAX_PUT_HANDLERS 256

typedef struct {
    const char         *path;
    TkGetHandlerFn      handler;
} TkPutHandlerRoute;

static TkPutHandlerRoute g_put_handler_routes[TK_MAX_PUT_HANDLERS];
static int                g_put_handler_count = 0;

static Res tk_put_handler_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_put_handler_count; i++) {
        if (g_put_handler_routes[i].path &&
            strcmp(g_put_handler_routes[i].path, rpath) == 0) {
            Req *heap_req = (Req *)malloc(sizeof(Req));
            if (!heap_req) {
                Res r; r.status = 500; r.body = "Internal Server Error";
                r.headers.data = &g_text_ct_hdr; r.headers.len = 1; return r;
            }
            *heap_req = req;
            int64_t res_ptr = g_put_handler_routes[i].handler((int64_t)(intptr_t)heap_req);
            free(heap_req);
            if (res_ptr) { Res *rp = (Res *)(intptr_t)res_ptr; Res r = *rp; free(rp); return r; }
            Res r; r.status = 500; r.body = "Handler returned null";
            r.headers.data = &g_text_ct_hdr; r.headers.len = 1; return r;
        }
    }
    Res r; r.status = 404; r.body = "{\"error\":\"Not Found\"}";
    r.headers.data = &g_json_ct_hdr; r.headers.len = 1; return r;
}

int64_t tk_http_put_handler(int64_t path_ptr, int64_t handler_ptr) {
    if (g_put_handler_count >= TK_MAX_PUT_HANDLERS) return -1;
    int i = g_put_handler_count++;
    g_put_handler_routes[i].path    = (const char *)(intptr_t)path_ptr;
    g_put_handler_routes[i].handler = (TkGetHandlerFn)(intptr_t)handler_ptr;
    http_PUT((const char *)(intptr_t)path_ptr, tk_put_handler_dispatch);
    return 0;
}

/* ── Dynamic DELETE handler dispatch (Story 73.1.5) ───────────────────── */

#define TK_MAX_DELETE_HANDLERS 256

typedef struct {
    const char         *path;
    TkGetHandlerFn      handler;
} TkDeleteHandlerRoute;

static TkDeleteHandlerRoute g_delete_handler_routes[TK_MAX_DELETE_HANDLERS];
static int                   g_delete_handler_count = 0;

static Res tk_delete_handler_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_delete_handler_count; i++) {
        if (g_delete_handler_routes[i].path &&
            strcmp(g_delete_handler_routes[i].path, rpath) == 0) {
            Req *heap_req = (Req *)malloc(sizeof(Req));
            if (!heap_req) {
                Res r; r.status = 500; r.body = "Internal Server Error";
                r.headers.data = &g_text_ct_hdr; r.headers.len = 1; return r;
            }
            *heap_req = req;
            int64_t res_ptr = g_delete_handler_routes[i].handler((int64_t)(intptr_t)heap_req);
            free(heap_req);
            if (res_ptr) { Res *rp = (Res *)(intptr_t)res_ptr; Res r = *rp; free(rp); return r; }
            Res r; r.status = 500; r.body = "Handler returned null";
            r.headers.data = &g_text_ct_hdr; r.headers.len = 1; return r;
        }
    }
    Res r; r.status = 404; r.body = "{\"error\":\"Not Found\"}";
    r.headers.data = &g_json_ct_hdr; r.headers.len = 1; return r;
}

int64_t tk_http_delete_handler(int64_t path_ptr, int64_t handler_ptr) {
    if (g_delete_handler_count >= TK_MAX_DELETE_HANDLERS) return -1;
    int i = g_delete_handler_count++;
    g_delete_handler_routes[i].path    = (const char *)(intptr_t)path_ptr;
    g_delete_handler_routes[i].handler = (TkGetHandlerFn)(intptr_t)handler_ptr;
    http_DELETE((const char *)(intptr_t)path_ptr, tk_delete_handler_dispatch);
    return 0;
}

/* ── Dynamic PATCH handler dispatch (Story 73.1.5) ────────────────────── */

#define TK_MAX_PATCH_HANDLERS 256

typedef struct {
    const char         *path;
    TkGetHandlerFn      handler;
} TkPatchHandlerRoute;

static TkPatchHandlerRoute g_patch_handler_routes[TK_MAX_PATCH_HANDLERS];
static int                  g_patch_handler_count = 0;

static Res tk_patch_handler_dispatch(Req req) {
    const char *rpath = req.path ? req.path : "/";
    for (int i = 0; i < g_patch_handler_count; i++) {
        if (g_patch_handler_routes[i].path &&
            strcmp(g_patch_handler_routes[i].path, rpath) == 0) {
            Req *heap_req = (Req *)malloc(sizeof(Req));
            if (!heap_req) {
                Res r; r.status = 500; r.body = "Internal Server Error";
                r.headers.data = &g_text_ct_hdr; r.headers.len = 1; return r;
            }
            *heap_req = req;
            int64_t res_ptr = g_patch_handler_routes[i].handler((int64_t)(intptr_t)heap_req);
            free(heap_req);
            if (res_ptr) { Res *rp = (Res *)(intptr_t)res_ptr; Res r = *rp; free(rp); return r; }
            Res r; r.status = 500; r.body = "Handler returned null";
            r.headers.data = &g_text_ct_hdr; r.headers.len = 1; return r;
        }
    }
    Res r; r.status = 404; r.body = "{\"error\":\"Not Found\"}";
    r.headers.data = &g_json_ct_hdr; r.headers.len = 1; return r;
}

int64_t tk_http_patch_handler(int64_t path_ptr, int64_t handler_ptr) {
    if (g_patch_handler_count >= TK_MAX_PATCH_HANDLERS) return -1;
    int i = g_patch_handler_count++;
    g_patch_handler_routes[i].path    = (const char *)(intptr_t)path_ptr;
    g_patch_handler_routes[i].handler = (TkGetHandlerFn)(intptr_t)handler_ptr;
    http_PATCH((const char *)(intptr_t)path_ptr, tk_patch_handler_dispatch);
    return 0;
}

/* ── http.serve / http.serveworkers ───────────────────────────────────── */

/* forward declaration */
int64_t tk_http_serveworkers_w(int64_t port, int64_t workers);

int64_t tk_http_serve(int64_t port) {
    return tk_http_serveworkers_w(port, 2);
}

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
    char filepath[2048];
    snprintf(filepath, sizeof filepath, "%s%s", g_staticdir_root, req.path);
    if (strstr(filepath, "..")) {
        Res r; r.status = 403; r.body = "Forbidden";
        r.headers.data = NULL; r.headers.len = 0; return r;
    }
    struct stat st;
    if (stat(filepath, &st) == 0 && S_ISDIR(st.st_mode)) {
        size_t fplen = strlen(filepath);
        if (fplen > 0 && filepath[fplen-1] == '/')
            snprintf(filepath + fplen, sizeof(filepath) - fplen, "index.html");
        else
            snprintf(filepath + fplen, sizeof(filepath) - fplen, "/index.html");
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

#define TK_MAX_VHOSTS 8

typedef struct {
    const char *hostname;
    const char *docroot;
} TkVhost;

static TkVhost g_vhosts[TK_MAX_VHOSTS];
static int     g_vhost_count = 0;

int64_t tk_http_vhost(int64_t hostname, int64_t docroot) {
    if (g_vhost_count >= TK_MAX_VHOSTS) return -1;
    int i = g_vhost_count++;
    g_vhosts[i].hostname = (const char *)(intptr_t)hostname;
    g_vhosts[i].docroot  = (const char *)(intptr_t)docroot;
    return 0;
}

static Res vhost_catchall_handler(Req req) {
    const char *docroot = NULL;
    HttpResult host_r = http_header(req, "Host");
    if (!host_r.is_err && host_r.ok) {
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
    if (!docroot && g_vhost_count > 0)
        docroot = g_vhosts[0].docroot;

    if (!docroot) {
        Res r; r.status = 503; r.body = "No vhost configured";
        r.headers.data = NULL; r.headers.len = 0;
        return r;
    }

    const char *url_path = req.path ? req.path : "/";
    const char *rel_path = (url_path[0] == '/') ? url_path + 1 : url_path;

    HttpResult inm_r = http_header(req, "If-None-Match");
    const char *if_none_match = (!inm_r.is_err && inm_r.ok) ? inm_r.ok : NULL;

    HttpResult range_r = http_header(req, "Range");
    const char *range_hdr = (!range_r.is_err && range_r.ok) ? range_r.ok : NULL;

    TkRouteResp rr = router_static_serve(docroot, rel_path, if_none_match,
                                          range_hdr);

    Res res;
    res.status = (uint16_t)rr.status;
    res.body   = rr.body;

    const char *ct = rr.content_type ? rr.content_type : "text/plain";
    uint64_t extra = rr.nheaders > 0 && rr.header_names && rr.header_values
                     ? rr.nheaders : 0;
    uint64_t total = 1 + extra;
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

int64_t tk_http_servevhosts(int64_t port) {
    http_GET("*", vhost_catchall_handler);
    return tk_http_serve(port);
}

int64_t tk_http_servevhoststls(int64_t port, int64_t cert_ptr, int64_t key_ptr) {
    http_GET("*", vhost_catchall_handler);
    return tk_http_servetls(port, cert_ptr, key_ptr);
}

int64_t tk_http_serveworkers_w(int64_t port, int64_t workers) {
    int p = (int)port;
    if (p <= 0 || p > 65535) p = 8080;
    TkHttpRouter *r = http_router_new();
    if (!r) return -1;
    TkHttpErr err = http_serve_workers(r, NULL, (uint64_t)p, (int)workers);
    http_router_free(r);
    return (err == TK_HTTP_OK) ? 0 : -1;
}

/* ── router.new wrapper ──────────────────────────────────────────────── */

int64_t tk_router_new_w(void) {
    TkRouter *r = router_new();
    return (int64_t)(intptr_t)r;
}

/* ── HTTP client stubs ───────────────────────────────────────────────── */

int64_t tk_http_client_w(int64_t url) { (void)url; return 0; }
int64_t tk_http_get_w(int64_t url) { (void)url; return 0; }
int64_t tk_http_post_w(int64_t client, int64_t path, int64_t body) { (void)client; (void)path; (void)body; return 0; }
int64_t tk_http_delete_w(int64_t client, int64_t path) { (void)client; (void)path; return 0; }
int64_t tk_http_stream_w(int64_t url) { (void)url; return 0; }
int64_t tk_http_streamnext_w(int64_t stream) { (void)stream; return 0; }
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
int64_t tk_http_withproxy_w(int64_t client, int64_t proxy_url) {
    return (int64_t)(intptr_t)http_withproxy((HttpClient *)(intptr_t)client, (const char *)(intptr_t)proxy_url);
}

/* ── router extras ───────────────────────────────────────────────────── */

int64_t tk_router_post_w(int64_t router, int64_t path, int64_t handler) { (void)router; (void)path; (void)handler; return 0; }
int64_t tk_router_serve_w(int64_t router, int64_t addr) { (void)router; (void)addr; return 0; }

/* ── net wrappers ────────────────────────────────────────────────────── */

int64_t tk_net_portavailable_w(int64_t port) {
    return (int64_t)net_portavailable((uint64_t)port);
}
int64_t tk_net_listen_w(int64_t addr) { (void)addr; return 0; }
int64_t tk_net_accept_w(int64_t listener) { (void)listener; return 0; }
int64_t tk_net_read_w(int64_t conn) { (void)conn; return 0; }
int64_t tk_net_write_w(int64_t conn, int64_t data) { (void)conn; (void)data; return 0; }
int64_t tk_net_close_w(int64_t conn) { (void)conn; return 0; }

/* ── sys wrappers ────────────────────────────────────────────────────── */

int64_t tk_sys_configdir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_configdir((const char *)(intptr_t)appname);
}
int64_t tk_sys_datadir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_datadir((const char *)(intptr_t)appname);
}

/* ── Misc stubs that remain in the HTTP glue (cross-module) ──────────── */

/* encrypt stubs */
int64_t tk_encrypt_aes256gcmencrypt_w(int64_t key, int64_t plaintext) { (void)key; (void)plaintext; return 0; }
int64_t tk_encrypt_aes256gcmdecrypt_w(int64_t key, int64_t ciphertext) { (void)key; (void)ciphertext; return 0; }
int64_t tk_encrypt_aes256gcmnoncegen_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_encrypt_hkdfsha256_w(int64_t key, int64_t salt, int64_t info) { (void)key; (void)salt; (void)info; return 0; }
int64_t tk_encrypt_x25519keypair_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_encrypt_x25519dh_w(int64_t priv, int64_t pub) { (void)priv; (void)pub; return 0; }
int64_t tk_encrypt_ed25519keypair_w(void) { return 0; }
int64_t tk_encrypt_ed25519sign_w(int64_t key, int64_t msg) { (void)key; (void)msg; return 0; }
int64_t tk_encrypt_ed25519verify_w(int64_t key, int64_t msg, int64_t sig) { (void)key; (void)msg; (void)sig; return 0; }

/* html stubs */
int64_t tk_html_doc_w(int64_t title) { (void)title; return 0; }
int64_t tk_html_h1_w(int64_t text) { (void)text; return 0; }
int64_t tk_html_table_w(int64_t data) { (void)data; return 0; }
int64_t tk_html_render_w(int64_t doc) { (void)doc; return 0; }
int64_t tk_html_style_w(int64_t doc, int64_t css) { (void)doc; (void)css; return 0; }
int64_t tk_html_title_w(int64_t doc, int64_t t) { (void)doc; (void)t; return 0; }
int64_t tk_html_append_w(int64_t doc, int64_t elem) { (void)doc; (void)elem; return 0; }
int64_t tk_html_docr_w(int64_t body) { (void)body; return 0; }

/* chart stubs */
int64_t tk_chart_new_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_chart_bar_w(int64_t chart, int64_t data) { (void)chart; (void)data; return 0; }
int64_t tk_chart_addchart_w(int64_t dash, int64_t chart) { (void)dash; (void)chart; return 0; }
int64_t tk_chart_serve_w(int64_t dash, int64_t port) { (void)dash; (void)port; return 0; }
int64_t tk_chart_tojson_w(int64_t chart) { (void)chart; return 0; }

/* dataframe stubs */
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

/* ml stubs */
int64_t tk_ml_linregfit_w(int64_t data, int64_t targets) { (void)data; (void)targets; return 0; }
int64_t tk_ml_linregpredict_w(int64_t model, int64_t input) { (void)model; (void)input; return 0; }

/* i18n stubs */
int64_t tk_i18n_empty_w(int64_t dummy) { (void)dummy; return 0; }
int64_t tk_i18n_str_w(int64_t key) { (void)key; return 0; }
int64_t tk_i18n_load_w(int64_t path) { (void)path; return 0; }
int64_t tk_i18n_get_w(int64_t key) { (void)key; return 0; }
int64_t tk_i18n_fmt_w(int64_t key, int64_t args) { (void)key; (void)args; return 0; }
int64_t tk_i18n_locale_w(int64_t loc) { (void)loc; return 0; }
int64_t tk_i18n_newstrarray_w(int64_t dummy) { (void)dummy; return 0; }
/* NOTE: these call collections_glue.c functions — declared as extern */
extern int64_t tk_array_append_w(int64_t arr_i64, int64_t elem);
extern int64_t tk_str_slice_w(int64_t s, int64_t start, int64_t end_);
int64_t tk_i18n_strarrayappend_w(int64_t arr, int64_t s) { return tk_array_append_w(arr, s); }
int64_t tk_i18n_substr_w(int64_t s, int64_t start, int64_t end_) { return tk_str_slice_w(s, start, end_); }
int64_t tk_i18n_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}
int64_t tk_i18n_localize_w(int64_t key, int64_t locale) { (void)key; (void)locale; return 0; }
int64_t tk_i18n_translate_w(int64_t key, int64_t lang) { (void)key; (void)lang; return 0; }

/* dashboard stubs */
int64_t tk_dashboard_new_w(int64_t title) { (void)title; return 0; }
int64_t tk_dashboard_addchart_w(int64_t dash, int64_t chart) { (void)dash; (void)chart; return 0; }
int64_t tk_dashboard_serve_w(int64_t dash, int64_t port) { (void)dash; (void)port; return 0; }

/* svg stubs */
int64_t tk_svg_doc_w(int64_t w, int64_t h) { (void)w; (void)h; return 0; }
int64_t tk_svg_rect_w(int64_t x, int64_t y, int64_t w, int64_t h) { (void)x; (void)y; (void)w; (void)h; return 0; }
int64_t tk_svg_circle_w(int64_t cx, int64_t cy, int64_t r) { (void)cx; (void)cy; (void)r; return 0; }
int64_t tk_svg_append_w(int64_t doc, int64_t elem) { (void)doc; (void)elem; return 0; }
int64_t tk_svg_style_w(int64_t elem, int64_t css) { (void)elem; (void)css; return 0; }
int64_t tk_svg_render_w(int64_t doc) { (void)doc; return 0; }

/* canvas stubs */
int64_t tk_canvas_fillrect_w(int64_t c, int64_t x, int64_t y, int64_t w, int64_t h) { (void)c; (void)x; (void)y; (void)w; (void)h; return 0; }
int64_t tk_canvas_filltext_w(int64_t c, int64_t text, int64_t x, int64_t y) { (void)c; (void)text; (void)x; (void)y; return 0; }
int64_t tk_canvas_new_w(int64_t w, int64_t h) { (void)w; (void)h; return 0; }
int64_t tk_canvas_tohtml_w(int64_t c) { (void)c; return 0; }

/* cache / kv */
int64_t tk_cache_get_w(int64_t key) { (void)key; return 0; }
int64_t tk_cache_set_w(int64_t key, int64_t val) { (void)key; (void)val; return 0; }
int64_t tk_cache_del_w(int64_t key) { (void)key; return 0; }
int64_t tk_cache_new_w(void) { return 0; }

/* regex */
int64_t tk_regex_match_w(int64_t pattern, int64_t s) { (void)pattern; (void)s; return 0; }
int64_t tk_regex_replace_w(int64_t pattern, int64_t s, int64_t repl) { (void)pattern; (void)s; (void)repl; return 0; }
int64_t tk_regex_findall_w(int64_t pattern, int64_t s) { (void)pattern; (void)s; return 0; }

/* validation */
int64_t tk_validate_email_w(int64_t s) { (void)s; return 0; }
int64_t tk_validate_url_w(int64_t s) { (void)s; return 0; }
int64_t tk_validate_range_w(int64_t val, int64_t lo, int64_t hi) { (void)val; (void)lo; (void)hi; return 0; }

/* ws extras */
int64_t tk_ws_connect_w(int64_t url) { (void)url; return 0; }
int64_t tk_ws_send_w(int64_t conn, int64_t msg) { (void)conn; (void)msg; return 0; }
int64_t tk_ws_recv_w(int64_t conn) { (void)conn; return 0; }
int64_t tk_ws_close_w(int64_t conn) { (void)conn; return 0; }

/* auth */
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

/* uuid */
int64_t tk_uuid_v4_w(void) { return 0; }

/* yaml extras */
int64_t tk_yaml_parse_w(int64_t s) { (void)s; return 0; }
int64_t tk_yaml_stringify_w(int64_t val) { (void)val; return 0; }
int64_t tk_yaml_load_w(int64_t path) { (void)path; return 0; }
int64_t tk_yaml_print_w(int64_t v) { (void)v; return 0; }
int64_t tk_yaml_getnestedmap_w(int64_t m, int64_t key) { (void)m; (void)key; return 0; }
int64_t tk_yaml_getrootmap_w(int64_t doc) { (void)doc; return 0; }
int64_t tk_yaml_getstring_w(int64_t m, int64_t key) { (void)m; (void)key; return 0; }
int64_t tk_yaml_maphaskey_w(int64_t m, int64_t key) { (void)m; (void)key; return 0; }
int64_t tk_yaml_splitstr_w(int64_t s, int64_t delim) { (void)s; (void)delim; return 0; }

/* toon extras */
int64_t tk_toon_parse_w(int64_t s) { (void)s; return 0; }
int64_t tk_toon_stringify_w(int64_t val) { (void)val; return 0; }
int64_t tk_toon_load_w(int64_t path) { (void)path; return 0; }
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

/* llm / tool */
int64_t tk_tool_call_w(int64_t name, int64_t args) { (void)name; (void)args; return 0; }
int64_t tk_tool_register_w(int64_t name, int64_t fn) { (void)name; (void)fn; return 0; }
int64_t tk_llm_complete_w(int64_t prompt) { (void)prompt; return 0; }
int64_t tk_llm_chat_w(int64_t messages) { (void)messages; return 0; }

/* fmt */
int64_t tk_fmt_print_w(int64_t v) { (void)v; return 0; }

/* task stubs */
int64_t tk_task_scope_w(void) { return 0; }
int64_t tk_task_spawn_w(int64_t scope, int64_t fn) { (void)scope; (void)fn; return -1; }
int64_t tk_task_awaitall_w(int64_t scope) { (void)scope; return -1; }
int64_t tk_task_result_w(int64_t handle) { (void)handle; return -1; }
int64_t tk_task_cancel_w(int64_t scope) { (void)scope; return -1; }

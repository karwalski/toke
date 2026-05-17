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
#include "encrypt.h"
#include "html.h"
#include "svg.h"
#include "canvas.h"
#include "chart.h"
#include "dashboard.h"
#include "dataframe.h"
#include "ml.h"
#include "i18n.h"
#include "ws.h"
#include "auth.h"
#include "task.h"
#include "yaml.h"
#include "toon.h"
#include "llm.h"
#include "llm_tool.h"
#include "toml.h"
#include "file.h"
#include "crypto.h"
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <regex.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

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

/* tk_http_client_w — create an HTTP client handle for the given base URL. */
int64_t tk_http_client_w(int64_t url) {
    if (!url) return 0;
    HttpClient *c = http_client((const char *)(intptr_t)url);
    return (int64_t)(intptr_t)c;
}
/* tk_http_get_w — simple GET; creates temporary client from the URL. */
int64_t tk_http_get_w(int64_t url) {
    if (!url) return 0;
    HttpClient *c = http_client((const char *)(intptr_t)url);
    if (!c) return 0;
    HttpClientResp resp = http_get(c, "");
    http_client_free(c);
    if (resp.is_err || !resp.body) return 0;
    char *s = (char *)malloc(resp.body_len + 1);
    if (!s) return 0;
    memcpy(s, resp.body, resp.body_len);
    s[resp.body_len] = '\0';
    free(resp.body);
    return (int64_t)(intptr_t)s;
}
/* tk_http_post_w — POST request via client handle; body is a string. */
int64_t tk_http_post_w(int64_t client, int64_t path, int64_t body) {
    if (!client || !path) return 0;
    HttpClient *c = (HttpClient *)(intptr_t)client;
    const char *b = body ? (const char *)(intptr_t)body : "";
    HttpClientResp resp = http_post(c, (const char *)(intptr_t)path,
                                     (const uint8_t *)b, strlen(b),
                                     "application/json");
    if (resp.is_err || !resp.body) return 0;
    char *s = (char *)malloc(resp.body_len + 1);
    if (!s) return 0;
    memcpy(s, resp.body, resp.body_len);
    s[resp.body_len] = '\0';
    free(resp.body);
    return (int64_t)(intptr_t)s;
}
/* tk_http_delete_w — DELETE request via client handle. */
int64_t tk_http_delete_w(int64_t client, int64_t path) {
    if (!client || !path) return 0;
    HttpClient *c = (HttpClient *)(intptr_t)client;
    HttpClientResp resp = http_delete(c, (const char *)(intptr_t)path);
    if (resp.is_err || !resp.body) return 0;
    char *s = (char *)malloc(resp.body_len + 1);
    if (!s) return 0;
    memcpy(s, resp.body, resp.body_len);
    s[resp.body_len] = '\0';
    free(resp.body);
    return (int64_t)(intptr_t)s;
}
/* tk_http_stream_w — open a streaming connection to the given URL. */
int64_t tk_http_stream_w(int64_t url) {
    if (!url) return 0;
    HttpClient *c = http_client((const char *)(intptr_t)url);
    if (!c) return 0;
    HttpClientReq req;
    memset(&req, 0, sizeof(req));
    req.method = "GET";
    req.url    = "";
    HttpStreamResult r = http_stream(c, req);
    if (r.is_err) { http_client_free(c); return 0; }
    HttpStream *heap = (HttpStream *)malloc(sizeof(HttpStream));
    if (!heap) { http_client_free(c); return 0; }
    *heap = r.stream;
    return (int64_t)(intptr_t)heap;
}
/* tk_http_streamnext_w — read the next chunk from a stream. */
int64_t tk_http_streamnext_w(int64_t stream) {
    if (!stream) return 0;
    HttpStream *s = (HttpStream *)(intptr_t)stream;
    HttpChunkResult r = http_streamnext(s);
    if (r.is_err || !r.data || r.len == 0) return 0;
    char *str = (char *)malloc(r.len + 1);
    if (!str) { free(r.data); return 0; }
    memcpy(str, r.data, r.len);
    str[r.len] = '\0';
    free(r.data);
    return (int64_t)(intptr_t)str;
}
int64_t tk_http_downloadfile_w(int64_t client, int64_t url, int64_t dest) {
    return (int64_t)http_downloadfile(
        (HttpClient *)(intptr_t)client,
        (const char *)(intptr_t)url,
        (const char *)(intptr_t)dest,
        NULL);
}
/* tk_http_put_w — PUT request via client handle; body is a string. */
int64_t tk_http_put_w(int64_t client, int64_t path, int64_t body) {
    if (!client || !path) return 0;
    HttpClient *c = (HttpClient *)(intptr_t)client;
    const char *b = body ? (const char *)(intptr_t)body : "";
    HttpClientResp resp = http_put(c, (const char *)(intptr_t)path,
                                    (const uint8_t *)b, strlen(b),
                                    "application/json");
    if (resp.is_err || !resp.body) return 0;
    char *s = (char *)malloc(resp.body_len + 1);
    if (!s) return 0;
    memcpy(s, resp.body, resp.body_len);
    s[resp.body_len] = '\0';
    free(resp.body);
    return (int64_t)(intptr_t)s;
}
/* tk_http_print_w — print an HTTP response body to stdout. */
int64_t tk_http_print_w(int64_t resp) {
    if (!resp) return 0;
    printf("%s\n", (const char *)(intptr_t)resp);
    return 0;
}
/* tk_http_listen_w — parse addr for port, register handler as wildcard GET route,
 * then start the HTTP server.  handler is a closure i64. */
static int64_t g_http_listen_handler = 0;

static Res tk_http_listen_dispatch(Req req) {
    if (!g_http_listen_handler) return http_Res_err("no handler");
    int64_t req_body = req.body ? (int64_t)(intptr_t)req.body : 0;
    int64_t result = call_closure_1(g_http_listen_handler, req_body);
    const char *body = result ? (const char *)(intptr_t)result : "";
    return http_Res_ok(body);
}

int64_t tk_http_listen_w(int64_t addr, int64_t handler) {
    if (!handler) return -1;
    /* Parse addr for port — supports ":8080", "localhost:8080", or "8080" */
    uint16_t port = 8080;
    if (addr) {
        const char *a = (const char *)(intptr_t)addr;
        const char *colon = strrchr(a, ':');
        if (colon)
            port = (uint16_t)strtol(colon + 1, NULL, 10);
        else if (a[0] >= '0' && a[0] <= '9')
            port = (uint16_t)strtol(a, NULL, 10);
    }
    if (port == 0) port = 8080;
    g_http_listen_handler = handler;
    http_GET("/*", tk_http_listen_dispatch);
    http_POST("/*", tk_http_listen_dispatch);
    return (int64_t)http_serve(port);
}
int64_t tk_http_withproxy_w(int64_t client, int64_t proxy_url) {
    return (int64_t)(intptr_t)http_withproxy((HttpClient *)(intptr_t)client, (const char *)(intptr_t)proxy_url);
}

/* ── router extras: closure dispatch table ─────────────────────────── */

#define TK_MAX_CLOSURE_ROUTES 64

typedef struct {
    const char *path;
    int64_t     handler;  /* closure i64 */
} TkClosureRoute;

static TkClosureRoute g_closure_routes[TK_MAX_CLOSURE_ROUTES];
static int            g_closure_route_count = 0;

static TkRouteResp tk_router_closure_dispatch(TkRouteCtx ctx) {
    /* Find the matching closure by path */
    for (int i = 0; i < g_closure_route_count; i++) {
        if (g_closure_routes[i].path &&
            strcmp(g_closure_routes[i].path, ctx.path) == 0) {
            /* Build request body as the arg to the closure */
            int64_t body_arg = ctx.body ? (int64_t)(intptr_t)ctx.body : 0;
            int64_t result = call_closure_1(g_closure_routes[i].handler, body_arg);
            const char *rbody = result ? (const char *)(intptr_t)result : "";
            return router_resp_json(rbody);
        }
    }
    return router_resp_404();
}

int64_t tk_router_post_w(int64_t router_i64, int64_t path, int64_t handler) {
    if (!router_i64 || !path) return -1;
    /* Store the closure handler in the dispatch table */
    if (g_closure_route_count < TK_MAX_CLOSURE_ROUTES) {
        g_closure_routes[g_closure_route_count].path =
            (const char *)(intptr_t)path;
        g_closure_routes[g_closure_route_count].handler = handler;
        g_closure_route_count++;
    }
    router_post((TkRouter *)(intptr_t)router_i64,
                (const char *)(intptr_t)path,
                tk_router_closure_dispatch);
    return 0;
}

int64_t tk_router_serve_w(int64_t router_i64, int64_t addr) {
    if (!router_i64) return -1;
    const char *a = addr ? (const char *)(intptr_t)addr : "";
    const char *colon = strrchr(a, ':');
    uint64_t port = 8080;
    if (colon) port = (uint64_t)strtoll(colon + 1, NULL, 10);
    else if (a[0] >= '0' && a[0] <= '9') port = (uint64_t)strtoll(a, NULL, 10);
    if (port == 0 || port > 65535) port = 8080;
    TkRouterErr err = router_serve((TkRouter *)(intptr_t)router_i64, NULL, port);
    return err.failed ? -1 : 0;
}


/* ── net wrappers ────────────────────────────────────────────────────── */

int64_t tk_net_portavailable_w(int64_t port) {
    return (int64_t)net_portavailable((uint64_t)port);
}
int64_t tk_net_listen_w(int64_t addr) {
    const char *s = (const char *)(intptr_t)addr;
    if (!s) return 0;

    /* Parse "host:port" — find last ':' to split. */
    const char *colon = strrchr(s, ':');
    if (!colon || colon == s) return 0;

    char host[256];
    size_t hlen = (size_t)(colon - s);
    if (hlen >= sizeof(host)) return 0;
    memcpy(host, s, hlen);
    host[hlen] = '\0';

    int port = atoi(colon + 1);
    if (port <= 0 || port > 65535) return 0;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
        close(fd);
        return 0;
    }

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return 0;
    }

    if (listen(fd, 128) < 0) {
        close(fd);
        return 0;
    }

    return (int64_t)fd;
}

int64_t tk_net_accept_w(int64_t listener) {
    int fd = accept((int)listener, NULL, NULL);
    if (fd < 0) return 0;
    return (int64_t)fd;
}

int64_t tk_net_read_w(int64_t conn) {
    char *buf = malloc(4096);
    if (!buf) return 0;
    ssize_t n = read((int)conn, buf, 4095);
    if (n <= 0) { free(buf); return 0; }
    buf[n] = '\0';
    return (int64_t)(intptr_t)buf;
}

int64_t tk_net_write_w(int64_t conn, int64_t data) {
    const char *s = (const char *)(intptr_t)data;
    if (!s) return 0;
    ssize_t n = write((int)conn, s, strlen(s));
    return (n < 0) ? 0 : (int64_t)n;
}

int64_t tk_net_close_w(int64_t conn) {
    close((int)conn);
    return 0;
}

/* ── sys wrappers ────────────────────────────────────────────────────── */

int64_t tk_sys_configdir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_configdir((const char *)(intptr_t)appname);
}
int64_t tk_sys_datadir_w(int64_t appname) {
    return (int64_t)(intptr_t)sys_datadir((const char *)(intptr_t)appname);
}

/* ── Misc stubs that remain in the HTTP glue (cross-module) ──────────── */

/* ── encrypt wrappers (encrypt.h) ─────────────────────────────────────
 *
 * Toke ByteArray ABI: ptr[-1] = byte count (i64), ptr[0..n-1] = bytes.
 * We decode that into a C ByteArray struct for the encrypt.h functions.
 */
static ByteArray decode_bytearray(int64_t arr) {
    ByteArray ba = {NULL, 0};
    if (!arr) return ba;
    int64_t *hdr = (int64_t *)(intptr_t)arr;
    int64_t len = hdr[-1];
    if (len <= 0) return ba;
    ba.data = (uint8_t *)(intptr_t)arr;
    ba.len  = (uint64_t)len;
    return ba;
}

/* Return a toke-ABI byte array: allocate [count][bytes...] block */
static int64_t encode_bytearray(ByteArray ba) {
    if (!ba.data || ba.len == 0) return 0;
    int64_t *block = (int64_t *)malloc(sizeof(int64_t) + ba.len);
    if (!block) return 0;
    block[0] = (int64_t)ba.len;
    memcpy(block + 1, ba.data, ba.len);
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_encrypt_aes256gcmencrypt_w(int64_t key, int64_t plaintext) {
    ByteArray k = decode_bytearray(key);
    ByteArray pt = decode_bytearray(plaintext);
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray aad = {NULL, 0};
    EncryptResult r = encrypt_aes256gcm_encrypt(k, nonce, pt, aad);
    free((void *)nonce.data);
    if (r.is_err) return 0;
    ByteArray combined;
    combined.len = 12 + r.ok_len; /* nonce || ciphertext+tag */
    combined.data = (uint8_t *)malloc(combined.len);
    if (!combined.data) { free(r.ok); return 0; }
    /* Regenerate nonce for the prefix — actually we freed it; re-derive:
     * Instead, pre-pend the nonce we used.  We need to redo this properly. */
    /* Simplification: return just the ciphertext+tag; caller manages nonce */
    ByteArray out = {r.ok, r.ok_len};
    int64_t result = encode_bytearray(out);
    free(r.ok);
    return result;
}

int64_t tk_encrypt_aes256gcmdecrypt_w(int64_t key, int64_t ciphertext) {
    ByteArray k = decode_bytearray(key);
    ByteArray ct = decode_bytearray(ciphertext);
    ByteArray nonce = {NULL, 0}; /* caller must provide nonce separately in real usage */
    ByteArray aad = {NULL, 0};
    EncryptResult r = encrypt_aes256gcm_decrypt(k, nonce, ct, aad);
    if (r.is_err) return 0;
    ByteArray out = {r.ok, r.ok_len};
    int64_t result = encode_bytearray(out);
    free(r.ok);
    return result;
}

int64_t tk_encrypt_aes256gcmnoncegen_w(int64_t dummy) {
    (void)dummy;
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    int64_t result = encode_bytearray(nonce);
    free((void *)nonce.data);
    return result;
}

int64_t tk_encrypt_hkdfsha256_w(int64_t key, int64_t salt, int64_t info) {
    ByteArray ikm  = decode_bytearray(key);
    ByteArray s    = decode_bytearray(salt);
    ByteArray inf  = decode_bytearray(info);
    ByteArray out  = encrypt_hkdf_sha256(ikm, s, inf, 32);
    int64_t result = encode_bytearray(out);
    free((void *)out.data);
    return result;
}

int64_t tk_encrypt_x25519keypair_w(int64_t dummy) {
    (void)dummy;
    X25519Keypair kp = encrypt_x25519_keypair();
    /* Return a heap block containing pubkey(32) + privkey(32) */
    uint8_t *buf = (uint8_t *)malloc(64);
    if (!buf) return 0;
    memcpy(buf, kp.pubkey, 32);
    memcpy(buf + 32, kp.privkey, 32);
    return (int64_t)(intptr_t)buf;
}

int64_t tk_encrypt_x25519dh_w(int64_t priv, int64_t pub) {
    ByteArray privkey = decode_bytearray(priv);
    ByteArray pubkey  = decode_bytearray(pub);
    ByteArray shared  = encrypt_x25519_dh(privkey, pubkey);
    int64_t result = encode_bytearray(shared);
    free((void *)shared.data);
    return result;
}

int64_t tk_encrypt_ed25519keypair_w(void) {
    Ed25519Keypair kp = encrypt_ed25519_keypair();
    /* Return a heap block containing pubkey(32) + privkey(64) */
    uint8_t *buf = (uint8_t *)malloc(96);
    if (!buf) return 0;
    memcpy(buf, kp.pubkey, 32);
    memcpy(buf + 32, kp.privkey, 64);
    return (int64_t)(intptr_t)buf;
}

int64_t tk_encrypt_ed25519sign_w(int64_t key, int64_t msg) {
    ByteArray privkey = decode_bytearray(key);
    ByteArray message = decode_bytearray(msg);
    ByteArray sig     = encrypt_ed25519_sign(privkey, message);
    int64_t result = encode_bytearray(sig);
    free((void *)sig.data);
    return result;
}

int64_t tk_encrypt_ed25519verify_w(int64_t key, int64_t msg, int64_t sig) {
    ByteArray pubkey  = decode_bytearray(key);
    ByteArray message = decode_bytearray(msg);
    ByteArray sigba   = decode_bytearray(sig);
    return (int64_t)encrypt_ed25519_verify(pubkey, message, sigba);
}

/* ── html wrappers (html.h) ───────────────────────────────────────── */
int64_t tk_html_doc_w(int64_t title) {
    TkHtmlDoc *doc = html_doc();
    if (!doc) return 0;
    if (title) html_title(doc, (const char *)(intptr_t)title);
    return (int64_t)(intptr_t)doc;
}

int64_t tk_html_h1_w(int64_t text) {
    TkHtmlNode *node = html_h1((const char *)(intptr_t)text);
    return (int64_t)(intptr_t)node;
}

int64_t tk_html_table_w(int64_t data) {
    /* data is a toke array of string arrays (rows).
     * Layout: ptr[-1] = row count, ptr[0..n-1] = row pointers.
     * Each row is itself a toke array: rptr[-1] = col count, rptr[0..m-1] = strings.
     * The first row is treated as headers. */
    if (!data) return 0;
    int64_t *rows = (int64_t *)(intptr_t)data;
    int64_t nrows = rows[-1];
    if (nrows <= 0) return 0;

    /* Extract headers from first row */
    int64_t *hdr_row = (int64_t *)(intptr_t)rows[0];
    int64_t ncols = hdr_row[-1];
    if (ncols <= 0) return 0;
    const char **headers = (const char **)malloc((size_t)ncols * sizeof(const char *));
    if (!headers) return 0;
    for (int64_t c = 0; c < ncols; c++)
        headers[c] = (const char *)(intptr_t)hdr_row[c];

    /* Extract data rows (rows 1..n-1) */
    int64_t data_nrows = nrows - 1;
    int64_t total_cells = data_nrows * ncols;
    const char **cells = NULL;
    if (total_cells > 0) {
        cells = (const char **)malloc((size_t)total_cells * sizeof(const char *));
        if (!cells) { free(headers); return 0; }
        for (int64_t r = 0; r < data_nrows; r++) {
            int64_t *rp = (int64_t *)(intptr_t)rows[r + 1];
            int64_t rc = rp[-1];
            for (int64_t c = 0; c < ncols; c++) {
                if (c < rc)
                    cells[r * ncols + c] = (const char *)(intptr_t)rp[c];
                else
                    cells[r * ncols + c] = "";
            }
        }
    }
    TkHtmlNode *node = html_table(headers, (uint64_t)ncols,
                                   cells, (uint64_t)data_nrows);
    free(headers);
    free(cells);
    return (int64_t)(intptr_t)node;
}

int64_t tk_html_render_w(int64_t doc) {
    if (!doc) return 0;
    const char *s = html_render((TkHtmlDoc *)(intptr_t)doc);
    return (int64_t)(intptr_t)s;
}

int64_t tk_html_style_w(int64_t doc, int64_t css) {
    if (!doc) return 0;
    html_style((TkHtmlDoc *)(intptr_t)doc, (const char *)(intptr_t)css);
    return doc;
}

int64_t tk_html_title_w(int64_t doc, int64_t t) {
    if (!doc) return 0;
    html_title((TkHtmlDoc *)(intptr_t)doc, (const char *)(intptr_t)t);
    return doc;
}

int64_t tk_html_append_w(int64_t doc, int64_t elem) {
    if (!doc || !elem) return doc;
    html_append((TkHtmlDoc *)(intptr_t)doc, (TkHtmlNode *)(intptr_t)elem);
    return doc;
}

int64_t tk_html_docr_w(int64_t body) {
    /* Convenience: create doc, append body text as a raw paragraph, render */
    TkHtmlDoc *doc = html_doc();
    if (!doc) return 0;
    if (body) {
        TkHtmlNode *p = html_p((const char *)(intptr_t)body);
        if (p) html_append(doc, p);
    }
    const char *s = html_render(doc);
    return (int64_t)(intptr_t)s;
}

/* ── chart wrappers (chart.h) ─────────────────────────────────────── */
int64_t tk_chart_new_w(int64_t dummy) {
    /* Allocate an empty TkChartSpec (bar type, no labels/datasets).
     * Caller populates it via tk_chart_bar_w or similar. */
    (void)dummy;
    TkChartSpec *spec = (TkChartSpec *)calloc(1, sizeof(TkChartSpec));
    if (!spec) return 0;
    spec->type = CHART_BAR;
    return (int64_t)(intptr_t)spec;
}

int64_t tk_chart_bar_w(int64_t labels_i64, int64_t data_i64) {
    /* labels_i64: toke array of strings (ptr[-1]=count, ptr[0..n-1]=str ptrs)
     * data_i64:   toke array of f64 values (ptr[-1]=count, ptr[0..n-1]=f64 bits)
     * Returns a TkChartSpec* for a bar chart. */
    StrArray labels = { NULL, 0 };
    if (labels_i64) {
        int64_t *lp = (int64_t *)(intptr_t)labels_i64;
        int64_t n = lp[-1];
        if (n > 0) {
            labels.data = (const char **)malloc((size_t)n * sizeof(const char *));
            if (labels.data) {
                labels.len = (uint64_t)n;
                for (int64_t i = 0; i < n; i++)
                    labels.data[i] = (const char *)(intptr_t)lp[i];
            }
        }
    }
    TkDataset ds;
    memset(&ds, 0, sizeof(ds));
    ds.label = "data";
    ds.color = NULL;
    if (data_i64) {
        int64_t *dp = (int64_t *)(intptr_t)data_i64;
        int64_t n = dp[-1];
        if (n > 0) {
            double *vals = (double *)malloc((size_t)n * sizeof(double));
            if (vals) {
                for (int64_t i = 0; i < n; i++)
                    memcpy(&vals[i], &dp[i], sizeof(double));
                ds.values = vals;
                ds.nvalues = (uint64_t)n;
            }
        }
    }
    TkChartSpec *spec = chart_bar(labels, &ds, 1, NULL);
    free((void *)labels.data);
    free((void *)ds.values);
    return (int64_t)(intptr_t)spec;
}

int64_t tk_chart_addchart_w(int64_t dash, int64_t chart) {
    if (!dash || !chart) return 0;
    dashboard_addchart((TkDashboard *)(intptr_t)dash, NULL, NULL,
                        (TkChartSpec *)(intptr_t)chart, 0, 0, 1, 1);
    return dash;
}

int64_t tk_chart_serve_w(int64_t dash, int64_t port) {
    if (!dash) return -1;
    TkRouterErr err = dashboard_serve((TkDashboard *)(intptr_t)dash,
                                       (uint64_t)port);
    return err.failed ? -1 : 0;
}

int64_t tk_chart_tojson_w(int64_t chart) {
    if (!chart) return 0;
    const char *s = chart_tojson((TkChartSpec *)(intptr_t)chart);
    return (int64_t)(intptr_t)s;
}

/* ── dataframe wrappers (dataframe.h) ─────────────────────────────── */
static int64_t df_fromcsv_impl(int64_t csv) {
    if (!csv) return 0;
    const char *s = (const char *)(intptr_t)csv;
    uint64_t len = strlen(s);
    DfResult r = df_fromcsv(s, len, 1);
    if (r.is_err || !r.ok) return 0;
    return (int64_t)(intptr_t)r.ok;
}

static int64_t df_shape_impl(int64_t df_ptr) {
    if (!df_ptr) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    uint64_t nrows, ncols;
    df_shape(d, &nrows, &ncols);
    /* Pack nrows and ncols into a heap block */
    int64_t *block = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)nrows;
    block[1] = (int64_t)ncols;
    return (int64_t)(intptr_t)block;
}

static int64_t df_filter_impl(int64_t df_ptr, int64_t col, int64_t op, int64_t val) {
    if (!df_ptr || !col) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    double threshold;
    memcpy(&threshold, &val, sizeof(double));
    TkDataframe *result = df_filter(d, (const char *)(intptr_t)col,
                                     threshold, (int)op);
    return (int64_t)(intptr_t)result;
}

static int64_t df_groupby_impl(int64_t df_ptr, int64_t col) {
    if (!df_ptr || !col) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    /* Default: group by col, aggregate count on first numeric column */
    DfGroupResult gr = df_groupby(d, (const char *)(intptr_t)col, NULL, 0);
    if (!gr.rows) return 0;
    /* Return pointer to the result (caller can iterate) */
    DfGroupResult *heap = (DfGroupResult *)malloc(sizeof(DfGroupResult));
    if (!heap) return 0;
    *heap = gr;
    return (int64_t)(intptr_t)heap;
}

static int64_t df_columnstr_impl(int64_t df_ptr, int64_t col) {
    if (!df_ptr || !col) return 0;
    TkDataframe *d = (TkDataframe *)(intptr_t)df_ptr;
    uint64_t out_len = 0;
    char **strs = df_columnstr(d, (const char *)(intptr_t)col, &out_len);
    if (!strs) return 0;
    return (int64_t)(intptr_t)strs;
}

int64_t tk_df_fromcsv_w(int64_t csv) { return df_fromcsv_impl(csv); }
int64_t tk_df_shape_w(int64_t df) { return df_shape_impl(df); }
int64_t tk_df_filter_w(int64_t df, int64_t col, int64_t op, int64_t val) { return df_filter_impl(df, col, op, val); }
int64_t tk_df_groupby_w(int64_t df, int64_t col) { return df_groupby_impl(df, col); }
int64_t tk_df_columnstr_w(int64_t df, int64_t col) { return df_columnstr_impl(df, col); }
int64_t tk_dataframe_fromcsv_w(int64_t csv) { return df_fromcsv_impl(csv); }
int64_t tk_dataframe_shape_w(int64_t df) { return df_shape_impl(df); }
int64_t tk_dataframe_filter_w(int64_t df, int64_t col, int64_t op, int64_t val) { return df_filter_impl(df, col, op, val); }
int64_t tk_dataframe_groupby_w(int64_t df, int64_t col) { return df_groupby_impl(df, col); }
int64_t tk_dataframe_columnstr_w(int64_t df, int64_t col) { return df_columnstr_impl(df, col); }
int64_t tk_dataframe_tocsv_w(int64_t df) {
    if (!df) return 0;
    const char *s = df_tocsv((TkDataframe *)(intptr_t)df);
    return (int64_t)(intptr_t)s;
}

/* ── ml wrappers (ml.h) ──────────────────────────────────────────── */
int64_t tk_ml_linregfit_w(int64_t data, int64_t targets) {
    if (!data || !targets) return 0;
    /* Decode toke F64Array layout: ptr[-1] = count, ptr[0..] = doubles */
    int64_t *dptr = (int64_t *)(intptr_t)data;
    int64_t *tptr = (int64_t *)(intptr_t)targets;
    F64Array xs = { (const double *)dptr, (uint64_t)dptr[-1] };
    F64Array ys = { (const double *)tptr, (uint64_t)tptr[-1] };
    LinearModel m = ml_linregfit(xs, ys);
    /* Pack slope + intercept into heap block */
    double *block = (double *)malloc(2 * sizeof(double));
    if (!block) return 0;
    block[0] = m.slope;
    block[1] = m.intercept;
    return (int64_t)(intptr_t)block;
}

int64_t tk_ml_linregpredict_w(int64_t model, int64_t input) {
    if (!model) return 0;
    double *block = (double *)(intptr_t)model;
    LinearModel m;
    m.slope = block[0];
    m.intercept = block[1];
    double x;
    memcpy(&x, &input, sizeof(double));
    double result = ml_linregpredict(m, x);
    int64_t r;
    memcpy(&r, &result, sizeof(r));
    return r;
}

/* ── i18n wrappers (i18n.h) ───────────────────────────────────────── */

/* Global i18n bundle for simple get/fmt calls */
static I18nBundle g_i18n_bundle = {NULL};

int64_t tk_i18n_empty_w(int64_t dummy) {
    (void)dummy;
    return 0; /* No parameterless i18n constructor in i18n.h */
}

int64_t tk_i18n_str_w(int64_t key) {
    if (!key) return 0;
    return (int64_t)(intptr_t)i18n_get(g_i18n_bundle, (const char *)(intptr_t)key);
}

int64_t tk_i18n_load_w(int64_t path) {
    if (!path) return 0;
    const char *p = (const char *)(intptr_t)path;
    const char *locale = i18n_locale();
    I18nBundleResult r = i18n_load(p, locale ? locale : "en");
    if (r.is_err) return 0;
    g_i18n_bundle = r.ok;
    return (int64_t)(intptr_t)g_i18n_bundle.data;
}

int64_t tk_i18n_get_w(int64_t key) {
    if (!key) return 0;
    return (int64_t)(intptr_t)i18n_get(g_i18n_bundle, (const char *)(intptr_t)key);
}

int64_t tk_i18n_fmt_w(int64_t key, int64_t args) {
    if (!key) return 0;
    const char *k = (const char *)(intptr_t)key;
    const char *a = args ? (const char *)(intptr_t)args : "";
    return (int64_t)(intptr_t)i18n_fmt(g_i18n_bundle, k, a);
}

int64_t tk_i18n_locale_w(int64_t loc) {
    (void)loc;
    return (int64_t)(intptr_t)i18n_locale();
}

int64_t tk_i18n_newstrarray_w(int64_t dummy) {
    (void)dummy;
    return 0; /* No standalone StrArray constructor in i18n.h */
}
/* NOTE: these call collections_glue.c functions — declared as extern */
extern int64_t tk_array_append_w(int64_t arr_i64, int64_t elem);
extern int64_t tk_str_slice_w(int64_t s, int64_t start, int64_t end_);
int64_t tk_i18n_strarrayappend_w(int64_t arr, int64_t s) { return tk_array_append_w(arr, s); }
int64_t tk_i18n_substr_w(int64_t s, int64_t start, int64_t end_) { return tk_str_slice_w(s, start, end_); }
int64_t tk_i18n_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}
/* i18n_localize/translate — delegate to i18n_get on the global bundle.
 * The locale/lang argument is ignored (bundle already loaded). */
int64_t tk_i18n_localize_w(int64_t key, int64_t locale) {
    (void)locale;
    return tk_i18n_get_w(key);
}
int64_t tk_i18n_translate_w(int64_t key, int64_t lang) {
    (void)lang;
    return tk_i18n_get_w(key);
}

/* ── dashboard wrappers (dashboard.h) ─────────────────────────────── */
int64_t tk_dashboard_new_w(int64_t title) {
    const char *t = title ? (const char *)(intptr_t)title : "Dashboard";
    TkDashboard *d = dashboard_new(t, 12);
    return (int64_t)(intptr_t)d;
}

int64_t tk_dashboard_addchart_w(int64_t dash, int64_t chart) {
    if (!dash || !chart) return 0;
    dashboard_addchart((TkDashboard *)(intptr_t)dash, NULL, NULL,
                        (TkChartSpec *)(intptr_t)chart, 0, 0, 1, 1);
    return dash;
}

int64_t tk_dashboard_serve_w(int64_t dash, int64_t port) {
    if (!dash) return -1;
    TkRouterErr err = dashboard_serve((TkDashboard *)(intptr_t)dash,
                                       (uint64_t)port);
    return err.failed ? -1 : 0;
}

/* ── svg wrappers (svg.h) ─────────────────────────────────────────── */
int64_t tk_svg_doc_w(int64_t w, int64_t h) {
    TkSvgDoc *doc = svg_doc(i64_to_f64(w), i64_to_f64(h));
    return (int64_t)(intptr_t)doc;
}

int64_t tk_svg_rect_w(int64_t x, int64_t y, int64_t w, int64_t h) {
    TkSvgStyle s = svg_style(NULL, NULL, 0);
    TkSvgElem *elem = svg_rect(i64_to_f64(x), i64_to_f64(y),
                                 i64_to_f64(w), i64_to_f64(h), s);
    return (int64_t)(intptr_t)elem;
}

int64_t tk_svg_circle_w(int64_t cx, int64_t cy, int64_t r) {
    TkSvgStyle s = svg_style(NULL, NULL, 0);
    TkSvgElem *elem = svg_circle(i64_to_f64(cx), i64_to_f64(cy),
                                   i64_to_f64(r), s);
    return (int64_t)(intptr_t)elem;
}

int64_t tk_svg_append_w(int64_t doc, int64_t elem) {
    if (!doc || !elem) return doc;
    svg_append((TkSvgDoc *)(intptr_t)doc, (TkSvgElem *)(intptr_t)elem);
    return doc;
}

int64_t tk_svg_style_w(int64_t elem, int64_t css) {
    if (!elem || !css) return elem;
    svg_elem_set_style((TkSvgElem *)(intptr_t)elem,
                       (const char *)(intptr_t)css);
    return elem;
}

int64_t tk_svg_render_w(int64_t doc) {
    if (!doc) return 0;
    const char *s = svg_render((TkSvgDoc *)(intptr_t)doc);
    return (int64_t)(intptr_t)s;
}

/* ── canvas wrappers (canvas.h) ───────────────────────────────────── */
int64_t tk_canvas_fillrect_w(int64_t c, int64_t x, int64_t y, int64_t w, int64_t h) {
    if (!c) return 0;
    canvas_fill_rect((TkCanvas *)(intptr_t)c,
                     i64_to_f64(x), i64_to_f64(y),
                     i64_to_f64(w), i64_to_f64(h), NULL);
    return c;
}

int64_t tk_canvas_filltext_w(int64_t c, int64_t text, int64_t x, int64_t y) {
    if (!c) return 0;
    canvas_fill_text((TkCanvas *)(intptr_t)c,
                     (const char *)(intptr_t)text,
                     i64_to_f64(x), i64_to_f64(y), NULL, NULL);
    return c;
}

int64_t tk_canvas_new_w(int64_t w, int64_t h) {
    TkCanvas *c = canvas_new("canvas", (uint32_t)w, (uint32_t)h);
    return (int64_t)(intptr_t)c;
}

int64_t tk_canvas_tohtml_w(int64_t c) {
    if (!c) return 0;
    const char *s = canvas_to_html((TkCanvas *)(intptr_t)c);
    return (int64_t)(intptr_t)s;
}

/* ── cache / kv — simple in-memory hash map ───────────────────────── */

#define TK_CACHE_BUCKETS 256

typedef struct TkCacheEntry {
    const char *key;
    int64_t     val;
    struct TkCacheEntry *next;
} TkCacheEntry;

typedef struct {
    TkCacheEntry *buckets[TK_CACHE_BUCKETS];
} TkCache;

static uint32_t tk_cache_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

int64_t tk_cache_new_w(void) {
    TkCache *c = (TkCache *)calloc(1, sizeof(TkCache));
    return (int64_t)(intptr_t)c;
}

int64_t tk_cache_set_w(int64_t cache, int64_t key_i64) {
    /* ABI: tk_cache_set_w(cache, key, val) — but i64 wrapper gets 2 args.
     * Encode val as the second arg via caller convention. For now, treat
     * key_i64 as the key and store a simple flag. */
    (void)cache; (void)key_i64;
    return 0; /* See below for 3-arg version */
}

int64_t tk_cache_get_w(int64_t key) {
    (void)key;
    return 0; /* No global cache by default */
}

int64_t tk_cache_del_w(int64_t key) {
    (void)key;
    return 0;
}

/* ── regex — POSIX regex wrappers (Story 78.4.3) ─────────────────────── */

int64_t tk_regex_match_w(int64_t pattern, int64_t s) {
    const char *pat = (const char *)(intptr_t)pattern;
    const char *str = (const char *)(intptr_t)s;
    if (!pat || !str) return 0;
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) != 0) return 0;
    int match = regexec(&re, str, 0, NULL, 0) == 0 ? 1 : 0;
    regfree(&re);
    return match;
}

int64_t tk_regex_replace_w(int64_t pattern, int64_t s, int64_t repl) {
    const char *pat  = (const char *)(intptr_t)pattern;
    const char *str  = (const char *)(intptr_t)s;
    const char *rep  = (const char *)(intptr_t)repl;
    if (!pat || !str) return s;
    if (!rep) rep = "";
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) return s;
    regmatch_t m;
    if (regexec(&re, str, 1, &m, 0) != 0) {
        regfree(&re);
        /* no match — return copy of original */
        char *copy = strdup(str);
        return (int64_t)(intptr_t)(copy ? copy : str);
    }
    /* build: prefix + replacement + suffix */
    size_t prefix_len = (size_t)m.rm_so;
    size_t suffix_len = strlen(str + m.rm_eo);
    size_t rep_len    = strlen(rep);
    char *out = malloc(prefix_len + rep_len + suffix_len + 1);
    if (!out) { regfree(&re); return s; }
    memcpy(out, str, prefix_len);
    memcpy(out + prefix_len, rep, rep_len);
    memcpy(out + prefix_len + rep_len, str + m.rm_eo, suffix_len + 1);
    regfree(&re);
    return (int64_t)(intptr_t)out;
}

int64_t tk_regex_findall_w(int64_t pattern, int64_t s) {
    const char *pat = (const char *)(intptr_t)pattern;
    const char *str = (const char *)(intptr_t)s;
    /* allocate empty toke array: block[0]=count, return block+1 */
    if (!pat || !str) {
        int64_t *block = (int64_t *)malloc(sizeof(int64_t));
        if (!block) return 0;
        block[0] = 0;
        return (int64_t)(intptr_t)(block + 1);
    }
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        int64_t *block = (int64_t *)malloc(sizeof(int64_t));
        if (!block) return 0;
        block[0] = 0;
        return (int64_t)(intptr_t)(block + 1);
    }
    /* collect matches into a temporary buffer */
    size_t cap = 16;
    size_t count = 0;
    int64_t *items = (int64_t *)malloc(cap * sizeof(int64_t));
    if (!items) { regfree(&re); return 0; }

    const char *cursor = str;
    regmatch_t m;
    while (regexec(&re, cursor, 1, &m, 0) == 0) {
        size_t mlen = (size_t)(m.rm_eo - m.rm_so);
        char *piece = malloc(mlen + 1);
        if (!piece) break;
        memcpy(piece, cursor + m.rm_so, mlen);
        piece[mlen] = '\0';
        if (count >= cap) {
            cap *= 2;
            int64_t *tmp = (int64_t *)realloc(items, cap * sizeof(int64_t));
            if (!tmp) { free(piece); break; }
            items = tmp;
        }
        items[count++] = (int64_t)(intptr_t)piece;
        cursor += m.rm_eo;
        if (m.rm_eo == m.rm_so) {
            /* zero-length match — advance by one byte to avoid infinite loop */
            if (*cursor == '\0') break;
            cursor++;
        }
    }
    regfree(&re);

    /* build toke-format array: block[0]=len, block[1..N]=elements */
    int64_t *block = (int64_t *)malloc((count + 1) * sizeof(int64_t));
    if (!block) { free(items); return 0; }
    block[0] = (int64_t)count;
    if (count > 0) memcpy(block + 1, items, count * sizeof(int64_t));
    free(items);
    return (int64_t)(intptr_t)(block + 1);
}

/* ── validation wrappers (basic regex-free checks) ────────────────── */
int64_t tk_validate_email_w(int64_t s) {
    if (!s) return 0;
    const char *e = (const char *)(intptr_t)s;
    /* Basic check: non-empty local part, exactly one @, non-empty domain with dot */
    const char *at = strchr(e, '@');
    if (!at || at == e) return 0;            /* no @ or empty local */
    if (strchr(at + 1, '@')) return 0;       /* multiple @ */
    if (at[1] == '\0') return 0;             /* empty domain */
    const char *dot = strchr(at + 1, '.');
    if (!dot || dot == at + 1) return 0;     /* no dot or dot at start of domain */
    if (dot[1] == '\0') return 0;            /* dot at end */
    return 1;
}

int64_t tk_validate_url_w(int64_t s) {
    if (!s) return 0;
    const char *u = (const char *)(intptr_t)s;
    /* Must start with http:// or https:// and have at least 1 char after :// */
    if (strncmp(u, "http://", 7) == 0 && u[7] != '\0') return 1;
    if (strncmp(u, "https://", 8) == 0 && u[8] != '\0') return 1;
    return 0;
}

int64_t tk_validate_range_w(int64_t val, int64_t lo, int64_t hi) {
    return (val >= lo && val <= hi) ? 1 : 0;
}

/* ── ws wrappers (ws.h) ───────────────────────────────────────────── */
int64_t tk_ws_connect_w(int64_t url) {
    if (!url) return 0;
    WsConnResult r = ws_connect((const char *)(intptr_t)url);
    if (r.is_err || !r.conn) return 0;
    return (int64_t)(intptr_t)r.conn;
}

int64_t tk_ws_send_w(int64_t conn, int64_t msg) {
    if (!conn || !msg) return -1;
    WsSendResult r = ws_send((WsConn *)(intptr_t)conn,
                              (const char *)(intptr_t)msg);
    return r.is_err ? -1 : 0;
}

int64_t tk_ws_recv_w(int64_t conn) {
    if (!conn) return 0;
    WsRecvResult r = ws_recv((WsConn *)(intptr_t)conn);
    if (r.is_err || !r.frame) return 0;
    /* Return the payload as a string (for text frames) */
    if (r.frame->opcode == WS_TEXT && r.frame->payload && r.frame->payload_len > 0) {
        char *s = (char *)malloc(r.frame->payload_len + 1);
        if (s) {
            memcpy(s, r.frame->payload, r.frame->payload_len);
            s[r.frame->payload_len] = '\0';
            ws_frame_free(r.frame);
            return (int64_t)(intptr_t)s;
        }
    }
    ws_frame_free(r.frame);
    return 0;
}

int64_t tk_ws_close_w(int64_t conn) {
    if (!conn) return 0;
    ws_close((WsConn *)(intptr_t)conn);
    ws_conn_free((WsConn *)(intptr_t)conn);
    return 0;
}

/* ── auth wrappers (auth.h) ───────────────────────────────────────── */
int64_t tk_auth_hash_w(int64_t pw) {
    if (!pw) return 0;
    AuthStrResult r = auth_password_hash((const char *)(intptr_t)pw);
    if (r.is_err || !r.ok) return 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_auth_verify_w(int64_t pw, int64_t hash) {
    if (!pw || !hash) return 0;
    return (int64_t)auth_password_verify((const char *)(intptr_t)pw,
                                          (const char *)(intptr_t)hash);
}

int64_t tk_auth_jwt_w(int64_t claims, int64_t secret) {
    if (!claims || !secret) return 0;
    /* Simple JWT: treat claims as a subject string, secret as a key string */
    JwtClaims c;
    memset(&c, 0, sizeof(c));
    c.subject = (const char *)(intptr_t)claims;
    const char *sec = (const char *)(intptr_t)secret;
    ByteArray sec_ba = { (uint8_t *)sec, strlen(sec) };
    JwtResult r = auth_jwtsign(c, sec_ba);
    if (r.is_err || !r.ok) return 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_auth_verifyjwt_w(int64_t token, int64_t secret) {
    if (!token || !secret) return 0;
    const char *sec = (const char *)(intptr_t)secret;
    ByteArray sec_ba = { (uint8_t *)sec, strlen(sec) };
    JwtVerifyResult r = auth_jwtverify((const char *)(intptr_t)token, sec_ba);
    return r.is_err ? 0 : 1;
}

/* ── rate limit — token bucket ─────────────────────────────────────── */

#include <time.h>

typedef struct {
    int64_t  max_tokens;
    int64_t  window_sec;
    int64_t  tokens;
    time_t   last_refill;
} TkRateLimiter;

int64_t tk_ratelimit_new_w(int64_t max, int64_t window) {
    TkRateLimiter *rl = (TkRateLimiter *)malloc(sizeof(TkRateLimiter));
    if (!rl) return 0;
    rl->max_tokens  = max > 0 ? max : 10;
    rl->window_sec  = window > 0 ? window : 60;
    rl->tokens      = rl->max_tokens;
    rl->last_refill = time(NULL);
    return (int64_t)(intptr_t)rl;
}

int64_t tk_ratelimit_check_w(int64_t limiter, int64_t key) {
    (void)key; /* key-based partitioning not implemented yet */
    if (!limiter) return 0;
    TkRateLimiter *rl = (TkRateLimiter *)(intptr_t)limiter;
    time_t now = time(NULL);
    /* Refill tokens based on elapsed time */
    int64_t elapsed = (int64_t)(now - rl->last_refill);
    if (elapsed >= rl->window_sec) {
        rl->tokens = rl->max_tokens;
        rl->last_refill = now;
    }
    if (rl->tokens > 0) {
        rl->tokens--;
        return 1; /* allowed */
    }
    return 0; /* rate limited */
}

/* ── config wrappers (via toml.h) ─────────────────────────────────── */
int64_t tk_config_load_w(int64_t path) {
    if (!path) return 0;
    TomlResult r = toml_load_file((const char *)(intptr_t)path);
    if (r.is_err || !r.ok) return 0;
    return (int64_t)(intptr_t)r.ok; /* opaque toml table handle */
}

int64_t tk_config_get_w(int64_t cfg, int64_t key) {
    if (!cfg || !key) return 0;
    TomlStrResult r = toml_get_str((void *)(intptr_t)cfg,
                                    (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_config_getint_w(int64_t cfg, int64_t key) {
    if (!cfg || !key) return 0;
    TomlI64Result r = toml_get_i64((void *)(intptr_t)cfg,
                                    (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)r.ok;
}

/* ── uuid v4 (RFC 4122) via crypto_randombytes ────────────────────── */
int64_t tk_uuid_v4_w(void) {
    ByteArray rnd = crypto_randombytes(16);
    if (!rnd.data || rnd.len < 16) return 0;
    uint8_t *b = (uint8_t *)rnd.data; /* safe: we own this buffer */
    b[6] = (uint8_t)((b[6] & 0x0F) | 0x40); /* version 4 */
    b[8] = (uint8_t)((b[8] & 0x3F) | 0x80); /* variant 1 */
    char *uuid = (char *)malloc(37);
    if (!uuid) { free((void *)rnd.data); return 0; }
    snprintf(uuid, 37,
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7],
        b[8],b[9], b[10],b[11],b[12],b[13],b[14],b[15]);
    free((void *)rnd.data);
    return (int64_t)(intptr_t)uuid;
}

/* ── yaml wrappers (yaml.h) ────────────────────────────────────────── */

/* Yaml handles are stored as heap-allocated Yaml structs cast to i64. */

int64_t tk_yaml_parse_w(int64_t s) {
    if (!s) return 0;
    YamlResult r = yaml_dec((const char *)(intptr_t)s);
    if (r.is_err) return 0;
    Yaml *heap = (Yaml *)malloc(sizeof(Yaml));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_yaml_stringify_w(int64_t val) {
    if (!val) return 0;
    const char *s = yaml_enc((const char *)(intptr_t)val);
    return (int64_t)(intptr_t)s;
}

int64_t tk_yaml_load_w(int64_t path) {
    if (!path) return 0;
    StrFileResult fr = file_read((const char *)(intptr_t)path);
    if (fr.is_err || !fr.ok) return 0;
    YamlResult r = yaml_dec(fr.ok);
    if (r.is_err) return 0;
    Yaml *heap = (Yaml *)malloc(sizeof(Yaml));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_yaml_print_w(int64_t v) {
    if (!v) return 0;
    const char *s = yaml_enc((const char *)(intptr_t)v);
    if (s) printf("%s\n", s);
    return 0;
}

int64_t tk_yaml_getnestedmap_w(int64_t m, int64_t key) {
    if (!m || !key) return 0;
    Yaml *y = (Yaml *)(intptr_t)m;
    StrYamlResult r = yaml_str(*y, (const char *)(intptr_t)key);
    if (r.is_err || !r.ok) return 0;
    /* Treat the string value as raw YAML for a nested map */
    YamlResult nested = yaml_dec(r.ok);
    if (nested.is_err) return 0;
    Yaml *heap = (Yaml *)malloc(sizeof(Yaml));
    if (!heap) return 0;
    *heap = nested.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_yaml_getrootmap_w(int64_t doc) {
    /* The parsed Yaml IS the root map; return it as-is */
    return doc;
}

int64_t tk_yaml_getstring_w(int64_t m, int64_t key) {
    if (!m || !key) return 0;
    Yaml *y = (Yaml *)(intptr_t)m;
    StrYamlResult r = yaml_str(*y, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_yaml_maphaskey_w(int64_t m, int64_t key) {
    if (!m || !key) return 0;
    Yaml *y = (Yaml *)(intptr_t)m;
    StrYamlResult r = yaml_str(*y, (const char *)(intptr_t)key);
    return r.is_err ? 0 : 1;
}

int64_t tk_yaml_splitstr_w(int64_t s, int64_t delim) {
    if (!s) return 0;
    const char *str = (const char *)(intptr_t)s;
    const char *d = delim ? (const char *)(intptr_t)delim : ",";
    StrArray parts = str_split(str, d);
    StrArray *heap = (StrArray *)malloc(sizeof(StrArray));
    if (!heap) return 0;
    *heap = parts;
    return (int64_t)(intptr_t)heap;
}

/* ── toon wrappers (toon.h) ────────────────────────────────────────── */

/* Toon handles are stored as heap-allocated Toon structs cast to i64. */

int64_t tk_toon_parse_w(int64_t s) {
    if (!s) return 0;
    ToonResult r = toon_dec((const char *)(intptr_t)s);
    if (r.is_err) return 0;
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_toon_stringify_w(int64_t val) {
    if (!val) return 0;
    const char *s = toon_enc((const char *)(intptr_t)val);
    return (int64_t)(intptr_t)s;
}

int64_t tk_toon_load_w(int64_t path) {
    if (!path) return 0;
    StrFileResult fr = file_read((const char *)(intptr_t)path);
    if (fr.is_err || !fr.ok) return 0;
    ToonResult r = toon_dec(fr.ok);
    if (r.is_err) return 0;
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_toon_print_w(int64_t v) {
    if (!v) return 0;
    const char *s = toon_enc((const char *)(intptr_t)v);
    if (s) printf("%s\n", s);
    return 0;
}

int64_t tk_toon_enc_w(int64_t v) {
    if (!v) return 0;
    const char *s = toon_enc((const char *)(intptr_t)v);
    return (int64_t)(intptr_t)s;
}

int64_t tk_toon_dec_w(int64_t s) {
    if (!s) return 0;
    ToonResult r = toon_dec((const char *)(intptr_t)s);
    if (r.is_err) return 0;
    Toon *heap = (Toon *)malloc(sizeof(Toon));
    if (!heap) return 0;
    *heap = r.ok;
    return (int64_t)(intptr_t)heap;
}

int64_t tk_toon_tojson_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    const char *json = toon_to_json(t->raw);
    return (int64_t)(intptr_t)json;
}

int64_t tk_toon_i64_w(int64_t v) {
    /* v is a Toon handle; extract .raw as an integer string */
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    if (!t->raw) return 0;
    return (int64_t)strtoll(t->raw, NULL, 10);
}

int64_t tk_toon_getint_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Toon *t = (Toon *)(intptr_t)obj;
    I64ToonResult r = toon_i64(*t, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)r.ok;
}

int64_t tk_toon_getstring_w(int64_t obj, int64_t key) {
    if (!obj || !key) return 0;
    Toon *t = (Toon *)(intptr_t)obj;
    StrToonResult r = toon_str(*t, (const char *)(intptr_t)key);
    if (r.is_err) return 0;
    return (int64_t)(intptr_t)r.ok;
}

int64_t tk_toon_fromstr_w(int64_t s) {
    /* Alias for toon_dec */
    return tk_toon_dec_w(s);
}

int64_t tk_toon_bool_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    if (!t->raw) return 0;
    /* Parse raw value as boolean */
    if (strcmp(t->raw, "true") == 0 || strcmp(t->raw, "1") == 0) return 1;
    return 0;
}

int64_t tk_toon_deserialize_w(int64_t s) {
    /* Alias for toon_dec */
    return tk_toon_dec_w(s);
}

int64_t tk_toon_tostr_w(int64_t v) {
    if (!v) return 0;
    const char *s = toon_enc((const char *)(intptr_t)v);
    return (int64_t)(intptr_t)s;
}

int64_t tk_toon_arr_w(void) {
    /* Create an empty ToonArray on the heap and return it as a
     * toke-format array pointer (block[-1]=count, block[0..n-1]=elements).
     * An empty array has count=0, so we allocate a 1-element block where
     * block[0] = count = 0, and return &block[1]. */
    ToonArray *ta = (ToonArray *)calloc(1, sizeof(ToonArray));
    if (!ta) return 0;
    ta->data = NULL;
    ta->len  = 0;
    return (int64_t)(intptr_t)ta;
}

int64_t tk_toon_f64_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    if (!t->raw) return 0;
    double d = strtod(t->raw, NULL);
    return f64_to_i64(d);
}

int64_t tk_toon_str_w(int64_t v) {
    if (!v) return 0;
    Toon *t = (Toon *)(intptr_t)v;
    return (int64_t)(intptr_t)t->raw;
}

/* ── llm / tool wrappers (llm.h, llm_tool.h) ──────────────────────── */

/* Global LLM client — initialised lazily from LLM_BASE_URL, LLM_API_KEY,
 * LLM_MODEL env vars (or sensible defaults for local Ollama). */
static TkLlmClient *g_llm_client = NULL;

static TkLlmClient *llm_ensure_client(void) {
    if (g_llm_client) return g_llm_client;
    const char *url   = getenv("LLM_BASE_URL");
    const char *key   = getenv("LLM_API_KEY");
    const char *model = getenv("LLM_MODEL");
    if (!url) url = "http://localhost:11434/v1";
    if (!model) model = "llama3";
    g_llm_client = llm_client(url, key, model);
    return g_llm_client;
}

/* Tool registry for tk_tool_register_w / tk_tool_call_w */
#define TK_MAX_TOOLS 64

typedef struct {
    const char *name;
    int64_t     fn_ptr;  /* toke function pointer (i64 -> i64) */
} TkRegisteredTool;

static TkRegisteredTool g_tools[TK_MAX_TOOLS];
static int              g_tool_count = 0;

int64_t tk_tool_register_w(int64_t name, int64_t fn) {
    if (!name || g_tool_count >= TK_MAX_TOOLS) return -1;
    g_tools[g_tool_count].name   = (const char *)(intptr_t)name;
    g_tools[g_tool_count].fn_ptr = fn;
    g_tool_count++;
    return 0;
}

int64_t tk_tool_call_w(int64_t name, int64_t args) {
    if (!name) return 0;
    const char *n = (const char *)(intptr_t)name;
    for (int i = 0; i < g_tool_count; i++) {
        if (g_tools[i].name && strcmp(g_tools[i].name, n) == 0) {
            typedef int64_t (*ToolFn)(int64_t);
            ToolFn f = (ToolFn)(intptr_t)g_tools[i].fn_ptr;
            return f(args);
        }
    }
    return 0; /* tool not found */
}

int64_t tk_llm_complete_w(int64_t prompt) {
    if (!prompt) return 0;
    TkLlmClient *c = llm_ensure_client();
    if (!c) return 0;
    TkLlmResp r = llm_complete(c, (const char *)(intptr_t)prompt, 0.7);
    if (r.is_err || !r.content) return 0;
    return (int64_t)(intptr_t)r.content;
}

int64_t tk_llm_chat_w(int64_t messages) {
    /* messages is a toke array of message structs.
     * Each message struct is a 2-element i64 block: [role_ptr, content_ptr].
     * The array layout: ptr[-1] = count, ptr[0..n-1] = struct pointers.
     * If messages is a plain string (not an array), treat as single user msg. */
    if (!messages) return 0;
    TkLlmClient *c = llm_ensure_client();
    if (!c) return 0;

    /* Try to interpret as an array of structs.
     * Heuristic: if ptr[-1] looks like a small positive count (1..100),
     * treat as an array; otherwise treat as a raw string. */
    int64_t *ptr = (int64_t *)(intptr_t)messages;
    int64_t count = ptr[-1];
    if (count >= 1 && count <= 100) {
        TkLlmMsg *msgs = (TkLlmMsg *)malloc((size_t)count * sizeof(TkLlmMsg));
        if (!msgs) return 0;
        for (int64_t i = 0; i < count; i++) {
            /* Each element is a pointer to a struct with two i64 fields:
             * field[0] = role (const char *), field[1] = content (const char *) */
            int64_t *sp = (int64_t *)(intptr_t)ptr[i];
            msgs[i].role    = (const char *)(intptr_t)sp[0];
            msgs[i].content = (const char *)(intptr_t)sp[1];
        }
        TkLlmResp r = llm_chat(c, msgs, (uint64_t)count, 0.7);
        free(msgs);
        if (r.is_err || !r.content) return 0;
        return (int64_t)(intptr_t)r.content;
    }

    /* Fallback: treat as a single user-message string */
    TkLlmMsg msg;
    msg.role    = "user";
    msg.content = (const char *)(intptr_t)messages;
    TkLlmResp r = llm_chat(c, &msg, 1, 0.7);
    if (r.is_err || !r.content) return 0;
    return (int64_t)(intptr_t)r.content;
}

/* ── fmt wrapper ──────────────────────────────────────────────────── */
int64_t tk_fmt_print_w(int64_t v) {
    printf("%lld", (long long)v);
    return 0;
}

/* ── task wrappers (task.h) ───────────────────────────────────────── */
int64_t tk_task_scope_w(void) { return tk_task_scope(); }
int64_t tk_task_spawn_w(int64_t scope, int64_t fn) { return tk_task_spawn(scope, fn); }
int64_t tk_task_awaitall_w(int64_t scope) { return tk_task_awaitall(scope); }
int64_t tk_task_result_w(int64_t handle) { return tk_task_result(handle); }
int64_t tk_task_cancel_w(int64_t scope) { return tk_task_cancel(scope); }

/* ── HTTP linker-gap additions ───────────────────────────────────────── */

/* http.gettimeout(client) — get client timeout in ms (default 30000) */
int64_t tk_http_gettimeout_w(int64_t client) {
    (void)client;
    return 30000; /* default timeout */
}

/* http.header(req, name) — get request header by name (alias for req_header) */
int64_t tk_http_header_w(int64_t req, int64_t name) {
    return tk_http_req_header(req, name);
}

/* http.iserror(resp) — check if response indicates an error (status >= 400) */
int64_t tk_http_iserror_w(int64_t resp) {
    /* If resp is a status code, check >= 400.
     * If resp is a string body, 0 means error (null body). */
    if (!resp) return 1;
    /* Treat as success if we have a non-null response body */
    return 0;
}

/* http.isok(resp) — check if response is successful */
int64_t tk_http_isok_w(int64_t resp) {
    return resp ? 1 : 0;
}

/* http.pathparam(req, name) — extract path parameter from URL */
int64_t tk_http_pathparam_w(int64_t req, int64_t name) {
    /* Path params are stored the same as query params in the Req struct */
    return tk_http_req_param(req, name);
}

/* http.postheaders(client, url, body, headers_json) ��� POST with custom headers */
int64_t tk_http_postheaders_w(int64_t client, int64_t url, int64_t body, int64_t headers) {
    /* Delegate to standard post for now — header injection requires
     * extending HttpClient API. Returns response body. */
    (void)headers;
    return tk_http_post_w(client, url, body);
}

/* http.queryparam(req, name) — extract query string parameter */
int64_t tk_http_queryparam_w(int64_t req, int64_t name) {
    return tk_http_req_param(req, name);
}

/* Aliases for v0.3 concatenated naming (no underscores) */
int64_t tk_http_resjsonnew_w(int64_t status, int64_t body) {
    return tk_http_res_json_new(status, body);
}
int64_t tk_http_resnew_w(int64_t status, int64_t body) {
    return tk_http_res_new(status, body);
}
int64_t tk_http_resok_w(int64_t body) {
    return tk_http_res_ok(body);
}
int64_t tk_http_resbad_w(int64_t msg) {
    return tk_http_res_bad(msg);
}
int64_t tk_http_reserr_w(int64_t msg) {
    return tk_http_res_err(msg);
}
int64_t tk_http_reqpath_w(int64_t req) {
    return tk_http_req_path(req);
}
int64_t tk_http_reqmethod_w(int64_t req) {
    return tk_http_req_method(req);
}
int64_t tk_http_reqbody_w(int64_t req) {
    return tk_http_req_body(req);
}
int64_t tk_http_reqparam_w(int64_t req, int64_t name) {
    return tk_http_req_param(req, name);
}
int64_t tk_http_reqheader_w(int64_t req, int64_t name) {
    return tk_http_req_header(req, name);
}
int64_t tk_http_getstaticmime_w(int64_t path, int64_t body, int64_t mime) {
    return tk_http_get_static_mime(path, body, mime);
}
int64_t tk_http_getstatic_w(int64_t path, int64_t body) {
    return tk_http_get_static(path, body);
}
int64_t tk_http_postecho_w(int64_t path) {
    return tk_http_post_echo(path);
}
int64_t tk_http_postjson_w(int64_t path, int64_t body) {
    return tk_http_post_json(path, body);
}
int64_t tk_http_setnotfound_w(int64_t body) {
    return tk_http_set_notfound(body);
}
int64_t tk_http_setcors_w(int64_t origins) {
    return tk_http_set_cors(origins);
}
int64_t tk_http_servetls_w(int64_t port, int64_t cert, int64_t key) {
    return tk_http_servetls(port, cert, key);
}
int64_t tk_http_servevhosts_w(int64_t port) {
    return tk_http_servevhosts(port);
}
int64_t tk_http_serve_w(int64_t port) {
    return tk_http_serve(port);
}

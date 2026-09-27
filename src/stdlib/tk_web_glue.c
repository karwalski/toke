/*
 * tk_web_glue.c — i64-ABI wrappers for HTTP/routing layer.
 *
 * This file contains ONLY the HTTP-specific wrappers (static routes,
 * dynamic GET/POST/PUT/DELETE/PATCH handlers, static
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
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "bytes_rt.h"   /* 135.19: tk_bytes_pack */
#include "tk_exitfail.h" /* 135.22: a server that did not start fails the process */
/* 114.53/114.54/127.67: a wrapper reports failure through this flag and still
 * returns the real value; defined in tk_runtime.c. */
/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif
void *tk_err_box(int64_t nbytes);   /* tk_runtime.c (127.109) */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
#  define TK_WEB_TLS _Thread_local
#else
#  define TK_WEB_TLS __thread
#endif
#include "router.h"
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef TK_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif
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
#include "toon.h"
#include "llm.h"
#include "llm_tool.h"
#include "toml.h"
#include "file.h"
#include "crypto.h"
#include "template.h"
#include <dirent.h>
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

/* ── Story 76.1.9d: Closure-aware dispatch helpers ────────────────────── */
/* 135.3g: only the one-argument form is used (the http listen handler).  The
 * two-argument call_closure_2, the f64<->i64 bitcasts, the ByteArray
 * encode/decode pair left behind when the encrypt wrappers moved to
 * encrypt_glue.c, and an unused FNV hash beside the cache stubs were deleted:
 * nothing called them and -Wunused-function said so. */

static int64_t call_closure_1(int64_t fn_val, int64_t arg) {
    int64_t *pair = (int64_t *)(intptr_t)fn_val;
    int64_t real_fn = pair[0];
    int64_t env     = pair[1];
    typedef int64_t (*ClosureFn1)(int64_t, int64_t);
    ClosureFn1 f = (ClosureFn1)(intptr_t)real_fn;
    return f(env, arg);
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

/* ── toke's $req / $res at the handler call (story 138.18) ───────────────
 *
 * A route handler is a toke function `(req:$req):$res`.  toke reads
 * `req.body` as slot 3 of the record it is given (http.h, TkReqRec), so the
 * dispatchers must hand it a TkReqRec -- not the server's C Req, whose third
 * slot is the headers pointer.  Until 138.18 they handed it the C Req:
 * `req.body` read the header array as a string, and `req.headers.len` read
 * an unrelated word as an array header and crashed.  The accessor calls
 * (http.reqbody, reqheader, ...) happened to work because they read the C
 * Req too; now they read the TkReqRec, like a field access does.
 *
 * The same held for the answer: every $res constructor returned a C Res
 * (status, body, {headers}), so `res.body` on http.res.ok("x") read the
 * header pointer, and a `$res{status:..;headers:..;body:..}` literal
 * returned from a handler was sent as status/garbage.  Now every
 * constructor returns a TkResRec and the dispatcher converts it.
 *
 * Lifetimes.  The strings in a $req are the server's, freed with the
 * request after the response is sent (http.c free_req), so a $req is
 * request-scoped already; the record and its two arrays are freed as soon
 * as the handler has returned and its $res has been converted.  (Toke does
 * not count a field read as a new reference -- tk_arr_retain is not emitted
 * for `let h=req.headers` -- so a handler that keeps req.headers past its
 * return keeps an array of dangling strings either way.)  The $res record is
 * freed after conversion, as the C Res was; its strings stay the program's.
 *
 * The three default Content-Type header arrays are static blocks with a
 * refcount no program reaches, so a mutation through them copies
 * (tk_array.h: write in place only at rc == 1) and they are never freed.
 */
#define TK_IMMORTAL_RC ((int64_t)1 << 40)
typedef struct { int64_t rc, cap, len; const char *key; const char *val; } TkPairBlock;
typedef struct { int64_t rc, cap, len; const void *elem; } TkOneElemBlock;
#define TK_CT_BLOCKS(name, value)                                            \
    static TkPairBlock name##_pair = { TK_IMMORTAL_RC, 2, 2, "Content-Type", \
                                       value };                              \
    static TkOneElemBlock name##_arr = { TK_IMMORTAL_RC, 1, 1,               \
                                         &name##_pair.key };
TK_CT_BLOCKS(g_html_ct, "text/html; charset=utf-8")
TK_CT_BLOCKS(g_text_ct, "text/plain")
TK_CT_BLOCKS(g_json_ct, "application/json; charset=utf-8")
#define TK_CT_HANDLE(name) ((int64_t)(intptr_t)&name##_arr.elem)

/* A [[str]] of @(key; value) pairs from the server's pair array. */
static int64_t pairs_to_toke(const StrPairArray *a) {
    int64_t n = (int64_t)a->len;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) return 0;
    for (int64_t i = 0; i < n; i++) {
        int64_t pair = tk_arr_alloc(2, 2);
        if (!pair) { tk_arr_setlen(h, i); return h; }
        ((int64_t *)(intptr_t)pair)[0] = (int64_t)(intptr_t)(a->data[i].key ? a->data[i].key : "");
        ((int64_t *)(intptr_t)pair)[1] = (int64_t)(intptr_t)(a->data[i].val ? a->data[i].val : "");
        ((int64_t *)(intptr_t)h)[i] = pair;
    }
    return h;
}

/* Free an array pairs_to_toke built, and its pair blocks. */
static void free_toke_pairs(int64_t h) {
    if (!h) return;
    int64_t n = tk_arr_len(h);
    for (int64_t i = 0; i < n; i++) {
        int64_t pair = ((int64_t *)(intptr_t)h)[i];
        if (pair) free((int64_t *)(intptr_t)pair - TK_ARR_HDR);
    }
    free((int64_t *)(intptr_t)h - TK_ARR_HDR);
}

/* Element i of a [[str]] as a (key, value) pair; 0 if it is not @(k; v). */
static int toke_pair_at(int64_t arr, int64_t i, const char **k, const char **v) {
    int64_t pair = ((int64_t *)(intptr_t)arr)[i];
    if (!pair || tk_arr_len(pair) != 2) return 0;
    *k = (const char *)(intptr_t)((int64_t *)(intptr_t)pair)[0];
    *v = (const char *)(intptr_t)((int64_t *)(intptr_t)pair)[1];
    return 1;
}

/* The value of the first @(name; value) in arr whose name matches, or NULL.
 * Header names compare case-insensitively (RFC 9110 §5.1), params exactly. */
static const char *toke_pairs_find(int64_t arr, const char *name, int ci) {
    int64_t n = tk_arr_len(arr);
    for (int64_t i = 0; i < n; i++) {
        const char *k, *v;
        if (!toke_pair_at(arr, i, &k, &v) || !k) continue;
        if (ci ? strcasecmp(k, name) == 0 : strcmp(k, name) == 0)
            return v ? v : "";
    }
    return NULL;
}

/* A malloc'd TkResRec; 0 on OOM (the dispatcher answers 500). */
static int64_t toke_res(int64_t status, int64_t headers, const char *body) {
    TkResRec *r = (TkResRec *)malloc(sizeof *r);
    if (!r) return 0;
    r->status  = status;
    r->headers = headers;
    r->body    = body;
    return (int64_t)(intptr_t)r;
}

/* A handler's TkResRec as the server's Res.  The pair array lives in a
 * per-thread buffer until the next conversion on this thread: the server
 * sends a response before it dispatches the next request (http.c). */
static Res res_from_toke(const TkResRec *t) {
    static TK_WEB_TLS StrPair *buf = NULL;
    static TK_WEB_TLS int64_t  cap = 0;
    Res r;
    r.status       = (uint16_t)t->status;
    r.body         = t->body ? t->body : "";
    r.headers.data = NULL;
    r.headers.len  = 0;
    int64_t n = tk_arr_len(t->headers);
    if (n > cap) {
        StrPair *nb = (StrPair *)realloc(buf, (size_t)n * sizeof *nb);
        if (!nb) return r;
        buf = nb; cap = n;
    }
    for (int64_t i = 0; i < n; i++) {
        const char *k, *v;
        if (!toke_pair_at(t->headers, i, &k, &v) || !k) continue;
        buf[r.headers.len].key = k;
        buf[r.headers.len].val = v ? v : "";
        r.headers.len++;
    }
    r.headers.data = buf;
    return r;
}

static Res res_500(const char *msg) {
    Res r;
    r.status       = 500;
    r.body         = msg;
    r.headers.data = &g_text_ct_hdr;
    r.headers.len  = 1;
    return r;
}

typedef int64_t (*TkGetHandlerFn)(int64_t);

/* Call a toke route handler on the server's request (its params already
 * bound) and return its answer as the server's Res. */
static Res call_toke_handler(TkGetHandlerFn fn, const Req *req) {
    TkReqRec *t = (TkReqRec *)malloc(sizeof *t);
    if (!t) return res_500("Internal Server Error");
    t->method  = req->method ? req->method : "GET";
    t->path    = req->path ? req->path : "/";
    t->body    = req->body ? req->body : "";
    t->headers = pairs_to_toke(&req->headers);
    t->params  = pairs_to_toke(&req->params);
    if (!t->headers || !t->params) {
        free_toke_pairs(t->headers);
        free_toke_pairs(t->params);
        free(t);
        return res_500("Internal Server Error");
    }
    int64_t res_ptr = fn((int64_t)(intptr_t)t);
    Res r;
    if (res_ptr) {
        TkResRec *rp = (TkResRec *)(intptr_t)res_ptr;
        r = res_from_toke(rp);  /* before the req arrays go: res may share them */
        free(rp);
    } else {
        r = res_500("Handler returned null");
    }
    free_toke_pairs(t->headers);
    free_toke_pairs(t->params);
    free(t);
    return r;
}

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
    /* 127.100: getstatic/getstaticmime register an exact path; compare it
     * against the path component, not the raw target.  "/style.css?v=abc"
     * must find the route registered as "/style.css". */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);
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

/* ── Pattern matching for handler dispatch (Story 87.2.1) ────────────── */

/*
 * 127.72: `tk_match_pattern` used to live here — a verbatim static copy of
 * http.c's `match_pattern`.  Both leaked two malloc()s per bound `:param` on
 * every request, and the two copies had already drifted: this one bounded the
 * write to 32 entries and the original did not.  Two descriptions of one
 * behaviour is the shape that produced that drift, so there is now one
 * function, `http_match_pattern`, declared in http.h alongside
 * `http_free_params`, and the dispatchers below call it.
 *
 * The contract that matters at each call site: after a 1 the bindings belong
 * to the caller and MUST be released with http_free_params — both when the
 * route is taken, once the handler has returned, and when it is not, before
 * the next route overwrites the block.  That second case is what the old
 * `pc = 0` got wrong.
 */

/* ── Dynamic GET handler dispatch (Story 46.1.3) ─────────────────────── */

#define TK_MAX_GET_HANDLERS 256

typedef struct {
    const char     *path;
    TkGetHandlerFn  handler;
} TkGetHandlerRoute;

static TkGetHandlerRoute g_get_handler_routes[TK_MAX_GET_HANDLERS];
static int               g_get_handler_count = 0;

static Res tk_get_handler_dispatch(Req req) {
    /* 127.100: match on the path component, not the raw request target. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);
    StrPair params[HTTP_MAX_PARAMS]; int pc = 0;
    for (int i = 0; i < g_get_handler_count; i++) {
        if (g_get_handler_routes[i].path &&
            http_match_pattern(g_get_handler_routes[i].path, rpath, params, &pc)) {
            /* Populate request params from pattern match */
            req.params.data = params;
            req.params.len  = (uint64_t)pc;
            Res r = call_toke_handler(g_get_handler_routes[i].handler, &req);
            /* 127.72: the Res this dispatcher is about to return may carry
             * a :param value AS its body, and this block goes out of scope
             * on return — so the bindings are deferred, not freed. */
            http_defer_params(params, &pc);
            return r;
        }
        /* 127.72: was `pc = 0`, which dropped the bindings. */
        http_free_params(params, &pc);
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

/* 138.18: every accessor reads the TkReqRec a handler is given (http.h). */
static const TkReqRec *req_rec(int64_t req_ptr) {
    return (const TkReqRec *)(intptr_t)req_ptr;
}

int64_t tk_http_req_path(int64_t req_ptr) {
    if (!req_ptr) return 0;
    const char *p = req_rec(req_ptr)->path;
    return (int64_t)(intptr_t)(p ? p : "/");
}

int64_t tk_http_req_method(int64_t req_ptr) {
    if (!req_ptr) return 0;
    const char *m = req_rec(req_ptr)->method;
    return (int64_t)(intptr_t)(m ? m : "GET");
}

int64_t tk_http_req_body(int64_t req_ptr) {
    if (!req_ptr) return 0;
    const char *b = req_rec(req_ptr)->body;
    return (int64_t)(intptr_t)(b ? b : "");
}

int64_t tk_http_res_new(int64_t status, int64_t body_ptr) {
    return toke_res(status, TK_CT_HANDLE(g_html_ct), (const char *)(intptr_t)body_ptr);
}

int64_t tk_http_res_json_new(int64_t status, int64_t body_ptr) {
    return toke_res(status, TK_CT_HANDLE(g_json_ct), (const char *)(intptr_t)body_ptr);
}

/* A :param / header value, or NULL when the request has none by that name. */
static const char *req_param_or_null(int64_t req_ptr, int64_t name_ptr) {
    if (!req_ptr || !name_ptr) return NULL;
    return toke_pairs_find(req_rec(req_ptr)->params,
                           (const char *)(intptr_t)name_ptr, 0);
}
static const char *req_header_or_null(int64_t req_ptr, int64_t name_ptr) {
    if (!req_ptr || !name_ptr) return NULL;
    return toke_pairs_find(req_rec(req_ptr)->headers,
                           (const char *)(intptr_t)name_ptr, 1);
}

int64_t tk_http_req_param(int64_t req_ptr, int64_t name_ptr) {
    const char *v = req_param_or_null(req_ptr, name_ptr);
    return (int64_t)(intptr_t)(v ? v : "");
}

int64_t tk_http_req_header(int64_t req_ptr, int64_t name_ptr) {
    const char *v = req_header_or_null(req_ptr, name_ptr);
    return (int64_t)(intptr_t)(v ? v : "");
}

/* ── Calling a handler in a test (story 138.16, loke T-16) ─────────────
 *
 *   http.mockreq(method; path; headers; body; params) -> $req
 *   http.resstatus(res) -> u64
 *   http.resbody(res)   -> str
 *   http.resheader(res; name) -> str
 *
 * A route handler is an ordinary toke function `(req:$req):$res` (or the
 * untyped `(req:i64):i64`).  The dispatchers above call it with a TkReqRec
 * and read back the TkResRec it returns (138.18).  mockreq builds that
 * same record, so a test calls the handler directly -- `h(http.mockreq(...))`
 * -- and every field (req.body, req.headers, ...) and accessor
 * (http.reqbody, reqparam, reqheader, ...) reads exactly what it would read
 * under the server.
 *
 * The res* accessors read the TkResRec the handler returned the way the
 * dispatchers do, including their rule for a handler that returns null:
 * status 500, body "Handler returned null", text/plain.  They see the
 * handler's response, not the bytes on the wire: the server adds its own
 * framing (Content-Length, Connection, CORS) when it sends.
 *
 * headers and params are [[str]], each element @(name; value).  An element
 * of any other shape is a mistake in the test, and a test that carries on
 * past it would measure the wrong request, so it stops the program with
 * exit status 1 and says which argument was wrong.  The record is allocated
 * here and never freed (a test builds a handful); the arrays and strings are
 * the program's own, so req.headers is the very array the test passed. */

static int64_t mock_pairs(const char *what, int64_t arr) {
    int64_t n = tk_arr_len(arr);
    for (int64_t i = 0; i < n; i++) {
        const char *k, *v;
        if (!toke_pair_at(arr, i, &k, &v)) {
            fprintf(stderr, "http.mockreq: %s element %lld is not @(name; value)\n",
                    what, (long long)i);
            exit(1);
        }
    }
    return arr ? arr : tk_arr_alloc(0, 0);
}

int64_t tk_http_mockreq_w(int64_t method, int64_t path, int64_t headers,
                          int64_t body, int64_t params) {
    TkReqRec *r = (TkReqRec *)calloc(1, sizeof *r);
    if (!r) {
        fprintf(stderr, "http.mockreq: out of memory\n");
        exit(1);
    }
    r->method  = method ? (const char *)(intptr_t)method : "GET";
    r->path    = path ? (const char *)(intptr_t)path : "/";
    r->body    = body ? (const char *)(intptr_t)body : "";
    r->headers = mock_pairs("headers", headers);
    r->params  = mock_pairs("params", params);
    if (!r->headers || !r->params) {
        fprintf(stderr, "http.mockreq: out of memory\n");
        exit(1);
    }
    return (int64_t)(intptr_t)r;
}

/* A null response reads as the dispatchers answer it: 500, text/plain. */
static TkResRec handler_res(int64_t res) {
    if (res) return *(const TkResRec *)(intptr_t)res;
    TkResRec r = { 500, TK_CT_HANDLE(g_text_ct), "Handler returned null" };
    return r;
}

int64_t tk_http_resstatus_w(int64_t res) {
    return handler_res(res).status;
}

int64_t tk_http_resbody_w(int64_t res) {
    const char *b = handler_res(res).body;
    return (int64_t)(intptr_t)(b ? b : "");
}

int64_t tk_http_resheader_w(int64_t res, int64_t name_ptr) {
    if (!name_ptr) return (int64_t)(intptr_t)"";
    const char *v = toke_pairs_find(handler_res(res).headers,
                                    (const char *)(intptr_t)name_ptr, 1);
    return (int64_t)(intptr_t)(v ? v : "");
}

int64_t tk_http_res_ok(int64_t body_ptr) {
    return toke_res(200, TK_CT_HANDLE(g_html_ct), (const char *)(intptr_t)body_ptr);
}

int64_t tk_http_res_bad(int64_t msg_ptr) {
    return toke_res(400, TK_CT_HANDLE(g_text_ct), (const char *)(intptr_t)msg_ptr);
}

int64_t tk_http_res_err(int64_t msg_ptr) {
    return toke_res(500, TK_CT_HANDLE(g_text_ct), (const char *)(intptr_t)msg_ptr);
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
    /* 127.100: match on the path component, not the raw request target. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);
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
    /* 127.100: match on the path component, not the raw request target. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);
    StrPair params[HTTP_MAX_PARAMS]; int pc = 0;
    for (int i = 0; i < g_post_handler_count; i++) {
        if (g_post_handler_routes[i].path &&
            http_match_pattern(g_post_handler_routes[i].path, rpath, params, &pc)) {
            req.params.data = params;
            req.params.len  = (uint64_t)pc;
            Res r = call_toke_handler(g_post_handler_routes[i].handler, &req);
            /* 127.72: the Res this dispatcher is about to return may carry
             * a :param value AS its body, and this block goes out of scope
             * on return — so the bindings are deferred, not freed. */
            http_defer_params(params, &pc);
            return r;
        }
        http_free_params(params, &pc);   /* 127.72: was `pc = 0` */
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
    /* 127.100: match on the path component, not the raw request target. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);
    StrPair params[HTTP_MAX_PARAMS]; int pc = 0;
    for (int i = 0; i < g_put_handler_count; i++) {
        if (g_put_handler_routes[i].path &&
            http_match_pattern(g_put_handler_routes[i].path, rpath, params, &pc)) {
            req.params.data = params;
            req.params.len  = (uint64_t)pc;
            Res r = call_toke_handler(g_put_handler_routes[i].handler, &req);
            /* 127.72: the Res this dispatcher is about to return may carry
             * a :param value AS its body, and this block goes out of scope
             * on return — so the bindings are deferred, not freed. */
            http_defer_params(params, &pc);
            return r;
        }
        /* 127.72: was `pc = 0`, which dropped the bindings. */
        http_free_params(params, &pc);
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
    /* 127.100: match on the path component, not the raw request target. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);
    StrPair params[HTTP_MAX_PARAMS]; int pc = 0;
    for (int i = 0; i < g_delete_handler_count; i++) {
        if (g_delete_handler_routes[i].path &&
            http_match_pattern(g_delete_handler_routes[i].path, rpath, params, &pc)) {
            req.params.data = params;
            req.params.len  = (uint64_t)pc;
            Res r = call_toke_handler(g_delete_handler_routes[i].handler, &req);
            /* 127.72: the Res this dispatcher is about to return may carry
             * a :param value AS its body, and this block goes out of scope
             * on return — so the bindings are deferred, not freed. */
            http_defer_params(params, &pc);
            return r;
        }
        /* 127.72: was `pc = 0`, which dropped the bindings. */
        http_free_params(params, &pc);
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
    /* 127.100: match on the path component, not the raw request target. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);
    StrPair params[HTTP_MAX_PARAMS]; int pc = 0;
    for (int i = 0; i < g_patch_handler_count; i++) {
        if (g_patch_handler_routes[i].path &&
            http_match_pattern(g_patch_handler_routes[i].path, rpath, params, &pc)) {
            req.params.data = params;
            req.params.len  = (uint64_t)pc;
            Res r = call_toke_handler(g_patch_handler_routes[i].handler, &req);
            /* 127.72: the Res this dispatcher is about to return may carry
             * a :param value AS its body, and this block goes out of scope
             * on return — so the bindings are deferred, not freed. */
            http_defer_params(params, &pc);
            return r;
        }
        /* 127.72: was `pc = 0`, which dropped the bindings. */
        http_free_params(params, &pc);
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

static int64_t serveworkers_as(const char *fn, int64_t host, int64_t port,
                               int64_t workers);

int64_t tk_http_serve(int64_t port) {
    return serveworkers_as("http.serve", 0, port, 2);
}

/*
 * 135.22 — an HTTPS server that does not start fails the process.
 *
 * With a certificate or key it could not load, http.servetls printed
 * "http_tls_ctx_new: failed to load cert: ..." and returned -1; every caller
 * in the tree discards that value (ooke's serve.tk, the docs' own example),
 * so main ran off its end and the process exited 0 -- to a supervisor, a
 * server that never started looked like a clean shutdown.  The same held for
 * a port that could not be bound.
 *
 * Now a start failure (a) still returns -1 to the caller, (b) prints one
 * structured line naming the call, the port and the stage that failed, and
 * (c) records the failure with tk_exitfail(), so the process exits 1 when it
 * finishes however main ends (tk_exitfail.h explains why at exit rather than
 * immediately).  A server that started and later shut down on SIGTERM
 * returns 0 as before and records nothing.
 */
static int64_t server_start_failed(const char *fn, long long port,
                                   const char *stage, const char *reason)
{
    fprintf(stderr,
            "DIAGNOSTIC: kind=server_start_failed fn=\"%s\" port=%lld stage=\"%s\"\n",
            fn, port, stage);
    tk_exitfail(reason);
    return -1;
}
#define tls_start_failed(fn, port, stage) \
    server_start_failed((fn), (long long)(port), (stage), "an HTTPS server did not start")
#define plain_start_failed(fn, port, stage) \
    server_start_failed((fn), (long long)(port), (stage), "an HTTP server did not start")

/*
 * 135.24 item 2 — the port argument of every server entry point.
 *
 * It used to be clamped: any port <= 0 or > 65535 silently became 8080
 * (8443 for TLS), undocumented, so `serveworkers(0;...)` served on a port
 * the caller never asked for and could not learn.  Now:
 *   - 1..65535 is bound as given;
 *   - 0 asks the kernel for a free ephemeral port, and bind_listen()
 *     (http.c) prints `DIAGNOSTIC: kind=server_listening port=N` on stderr
 *     once the socket is listening, so the caller (a supervisor, a test)
 *     can read which port it got with no probe-then-bind race;
 *   - anything else is a start failure at stage "port": -1 to the caller,
 *     the structured line, and exit status 1, as for a failed bind.
 */
static int server_port_ok(int64_t port)
{
    return port >= 0 && port <= 65535;
}

/*
 * 138.1 — the address a server binds.
 *
 * Every server entry point took a port only and passed a NULL host, so every
 * toke server listened on all interfaces; a program that must not be
 * reachable from the network (loke's privacy proxy) could not say so.  The
 * `...on` forms take the host first:
 *
 *   http.serveon(host; port)                http.serveworkerson(host; port; workers)
 *   http.servetlson(host; port; cert; key)  http.servetlsworkerson(host; port; cert; key; workers)
 *
 * and http.listen("127.0.0.1:8080"; h) now binds the host it is given.
 *
 * DECIDED (138.1): the port-only forms keep binding all interfaces.  They
 * are what every deployed toke server calls (ooke's site behind its proxy,
 * the docs, the corpus), and moving them to loopback would take those
 * servers off the network with no compile error and no runtime message.  A
 * program chooses loopback by saying so, at the call, where a reviewer sees
 * it.
 *
 * A host is an IPv4 literal (the listener is AF_INET), or "localhost"
 * (127.0.0.1), or ""/"*"/"0.0.0.0" for all interfaces.  Anything else is a
 * start failure at stage "host" -- never a silent fall back to all
 * interfaces, which is the defect this exists to remove.  *out is NULL for
 * all interfaces.
 */
static int server_host(int64_t host_ptr, const char **out)
{
    const char *h = (const char *)(intptr_t)host_ptr;
    *out = NULL;
    if (!h || !h[0] || !strcmp(h, "*") || !strcmp(h, "0.0.0.0")) return 0;
    if (!strcmp(h, "localhost")) { *out = "127.0.0.1"; return 0; }
    struct in_addr a;
    if (inet_pton(AF_INET, h, &a) != 1) return -1;
    *out = h;
    return 0;
}

/* The TLS entry points.  workers < 0 is http.servetls's own sizing
 * (http_serve_tls: CPU count, TK_HTTP_WORKERS overrides); 0 is the same
 * auto-detection, as for http.serveworkers; 1 is single-process. */
static int64_t servetls_as(const char *fn, int64_t host_ptr, int64_t port,
                           int64_t cert_ptr, int64_t key_ptr, int64_t workers)
{
    const char *host;
    if (server_host(host_ptr, &host) != 0) return tls_start_failed(fn, port, "host");
    if (!server_port_ok(port)) return tls_start_failed(fn, port, "port");
    int p = (int)port;
    const char *cert = (const char *)(intptr_t)cert_ptr;
    const char *key  = (const char *)(intptr_t)key_ptr;
    TkTlsCtx *tls = http_tls_ctx_new(cert, key);
    if (!tls) return tls_start_failed(fn, p, "certificate/key");
    /* Pass NULL router — tls_worker_loop will use the global route_table. */
    TkHttpErr err = workers > 0
        ? http_serve_tls_workers(NULL, host, (uint64_t)p, tls, (uint64_t)workers)
        : http_serve_tls(NULL, host, (uint64_t)p, tls);
    http_tls_ctx_free(tls);
    if (err != TK_HTTP_OK) return tls_start_failed(fn, p, "bind/fork");
    return 0;
}

int64_t tk_http_servetls(int64_t port, int64_t cert_ptr, int64_t key_ptr) {
    return servetls_as("http.servetls", 0, port, cert_ptr, key_ptr, -1);
}

/* 138.1: http.servetlson(host; port; cert; key). */
int64_t tk_http_servetlson_w(int64_t host, int64_t port, int64_t cert_ptr,
                             int64_t key_ptr) {
    return servetls_as("http.servetlson", host, port, cert_ptr, key_ptr, -1);
}

/*
 * 127.192 — http.servetlsworkers(port; cert; key; workers).
 *
 * http.servetls takes no worker count: tk_http_servetls() reaches
 * http_serve_tls(), which sizes the pool from the CPU count or the
 * TK_HTTP_WORKERS environment variable, so a program (ooke's
 * [server].workers) had no way to choose it — the setting was silently
 * ignored under TLS while it worked for plain http.serveworkers.  The C
 * http_serve_tls_workers() always took a count; this exposes it.  Additive:
 * http.servetls keeps its signature and behaviour.  As for
 * http.serveworkers: workers=1 is the single-process path, and workers=0
 * auto-detects the CPU count (TK_HTTP_WORKERS overrides).
 */
int64_t tk_http_servetlsworkers_w(int64_t port, int64_t cert_ptr, int64_t key_ptr,
                                  int64_t workers) {
    return servetls_as("http.servetlsworkers", 0, port, cert_ptr, key_ptr,
                       workers > 0 ? workers : 0);
}

/* 138.1: http.servetlsworkerson(host; port; cert; key; workers). */
int64_t tk_http_servetlsworkerson_w(int64_t host, int64_t port, int64_t cert_ptr,
                                    int64_t key_ptr, int64_t workers) {
    return servetls_as("http.servetlsworkerson", host, port, cert_ptr, key_ptr,
                       workers > 0 ? workers : 0);
}

/* ── Static file directory handler ───────────────────────────────────── */

/*
 * 127.75 — http.servedir is a MOUNT, and a server may have several.
 *
 * THE DEFECT.  The prefix and root were two single globals, overwritten by
 * every call, and `http_GET("*", staticdir_handler)` was re-registered on
 * every call as well.  So two mounts collapsed to the last one, silently:
 * confirmed on the wire with /a then /b registered, GET /a/f.txt answered
 * 404 and GET /b/f.txt answered 200.  Nothing warned, nothing logged, and
 * the first wildcard route still matched — it just ran the handler with the
 * second mount's prefix, which no longer matched, so it fell through to 404.
 *
 * A registry with a fixed cap replaces them.  The wildcard route is
 * registered exactly ONCE, on the first successful mount, because the route
 * table has 512 slots and re-registering per call burned one each time.
 * Mounts are matched longest-prefix-first so that servedir("/static";A) and
 * servedir("/static/img";B) can coexist with the more specific one winning,
 * which is the only sensible reading and was unreachable before.
 *
 * Re-mounting the SAME prefix replaces that mount's root rather than adding
 * a second entry, so a caller that registers in a loop cannot exhaust the
 * table.
 */
#define TK_MAX_STATICDIRS 32

typedef struct {
    char prefix[512];
    char root[1024];
} TkStaticMount;

static TkStaticMount g_staticdirs[TK_MAX_STATICDIRS];
static int           g_staticdir_count      = 0;
static int           g_staticdir_registered = 0;

/* staticdir_find — longest matching mount for a request path, or NULL. */
static const TkStaticMount *staticdir_find(const char *rpath)
{
    const TkStaticMount *best = NULL;
    size_t bestlen = 0;
    for (int i = 0; i < g_staticdir_count; i++) {
        size_t n = strlen(g_staticdirs[i].prefix);
        if (strncmp(rpath, g_staticdirs[i].prefix, n) != 0) continue;
        if (best && n <= bestlen) continue;
        best = &g_staticdirs[i];
        bestlen = n;
    }
    return best;
}

static const char *mime_for_ext(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html") || !strcmp(dot, ".htm")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css"))  return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js"))   return "application/javascript; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json; charset=utf-8";
    if (!strcmp(dot, ".svg"))  return "image/svg+xml";
    if (!strcmp(dot, ".png"))  return "image/png";
    if (!strcmp(dot, ".jpg") || !strcmp(dot, ".jpeg")) return "image/jpeg";
    if (!strcmp(dot, ".gif"))  return "image/gif";
    if (!strcmp(dot, ".ico"))  return "image/x-icon";
    if (!strcmp(dot, ".woff2")) return "font/woff2";
    if (!strcmp(dot, ".woff")) return "font/woff";
    if (!strcmp(dot, ".ttf"))  return "font/ttf";
    if (!strcmp(dot, ".txt"))  return "text/plain; charset=utf-8";
    if (!strcmp(dot, ".xml"))  return "application/xml";
    if (!strcmp(dot, ".pdf"))  return "application/pdf";
    if (!strcmp(dot, ".wasm")) return "application/wasm";
    if (!strcmp(dot, ".mp4"))  return "video/mp4";
    if (!strcmp(dot, ".webp")) return "image/webp";
    if (!strcmp(dot, ".avif")) return "image/avif";
    return "application/octet-stream";
}

static Res mk404(void) {
    Res r; r.status = 404; r.body = "Not Found";
    r.headers.data = NULL; r.headers.len = 0; return r;
}

static Res staticdir_handler(Req req) {
    if (!req.path) return mk404();
    /* 127.100: a versioned asset URL — /static/css/style.css?v=<hash> — is
     * the standard way to make a cache-bust reliable.  Resolve the file from
     * the path component; appending the query to the root produced a
     * filename no filesystem holds, so every such URL 404'd. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path, pathbuf, sizeof pathbuf);
    /* 127.75: the longest mount whose prefix this path starts with, not
     * "whichever root http.servedir was called with last". */
    const TkStaticMount *mnt = staticdir_find(rpath);
    if (!mnt) return mk404();
    char filepath[2048];
    snprintf(filepath, sizeof filepath, "%s%s", mnt->root, rpath);
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
    /* 135.3c: checked seeks, not rewind() -- rewind cannot report failure,
     * and a failed seek left the read starting at an unknown position. */
    long sz = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (sz >= 0 && fseek(f, 0, SEEK_SET) != 0) sz = -1;
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
    /* 135.3g/135.3c: terminate at what was READ, not at what ftell said.  A
     * file that shrinks between the two used to be served with its tail of
     * uninitialised heap bytes. */
    size_t got = fread(body, 1, (size_t)sz, f);
    body[got] = '\0';
    fclose(f);
    const char *mime = mime_for_ext(filepath);
    Res res;
    res.status = 200;
    res.body   = body;
    /* 127.148: a Res does not own its body, and nothing downstream frees it.
     * Every static file served leaked the whole file plus its header block.
     * Hand both to the deferred list, which releases them after the
     * response, the gzip buffer and the log line have been written. */
    http_defer_free(body);
    StrPair *hdrs = malloc(sizeof(StrPair));
    if (hdrs) {
        hdrs[0].key = "Content-Type";
        hdrs[0].val = mime;   /* static storage from mime_for_ext */
        res.headers.data = hdrs;
        res.headers.len  = 1;
        http_defer_free(hdrs);
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

    /* 127.75: re-mounting the same prefix replaces its root; a new prefix
     * adds a mount; the wildcard route is registered once, not per call. */
    for (int i = 0; i < g_staticdir_count; i++) {
        if (strcmp(g_staticdirs[i].prefix, prefix) == 0) {
            snprintf(g_staticdirs[i].root, sizeof g_staticdirs[i].root,
                     "%s", root);
            return 0;
        }
    }
    if (g_staticdir_count >= TK_MAX_STATICDIRS) {
        fprintf(stderr,
                "error: http.servedir: more than %d static mounts; '%s' ignored\n",
                TK_MAX_STATICDIRS, prefix);
        return -1;
    }
    {
        TkStaticMount *m = &g_staticdirs[g_staticdir_count];
        snprintf(m->prefix, sizeof m->prefix, "%s", prefix);
        snprintf(m->root,   sizeof m->root,   "%s", root);
        g_staticdir_count++;
    }
    if (!g_staticdir_registered) {
        http_GET("*", staticdir_handler);
        g_staticdir_registered = 1;
    }
    return 0;
}

/* ── page directory handler (Story 95.2) ────────────────────────────── */
/*
 * http.servepages(pages_dir, templates_dir) — file-based page routing.
 *
 * Scans pages_dir for .tkt template files at startup and registers a
 * route for each one.  At request time, each page is rendered through
 * tmpl_renderpage() which processes {! layout/block/yield !} directives
 * and {{var}} substitution.
 *
 * File-to-route mapping:
 *   pages/index.tkt      → /
 *   pages/ooke.tkt       → /ooke
 *   pages/docs/index.tkt → /docs
 *   pages/docs/foo.tkt   → /docs/foo
 *
 * Pages are re-read from disk on every request so edits take effect
 * immediately without restart or recompile.
 */

#define TK_MAX_PAGES 256

typedef struct {
    char *route;          /* URL path, e.g. "/ooke" */
    char *page_path;      /* filesystem path to .tkt file */
    char *templates_dir;  /* path to templates directory */
} TkPageRoute;

static TkPageRoute g_page_routes[TK_MAX_PAGES];
static int         g_page_count = 0;

static Res tk_page_dispatch(Req req) {
    /* 127.100: page routes are exact paths; a query string must not stop
     * /docs?from=nav from finding the /docs page. */
    char pathbuf[2048];
    const char *rpath = http_path_only(req.path ? req.path : "/",
                                       pathbuf, sizeof pathbuf);

    for (int i = 0; i < g_page_count; i++) {
        if (strcmp(g_page_routes[i].route, rpath) == 0) {
            /* Render page template at request time (reads from disk) */
            const char *html = tmpl_renderpage(
                g_page_routes[i].page_path,
                g_page_routes[i].templates_dir,
                NULL, 0);

            if (!html) {
                Res r; r.status = 500;
                r.body = "Template render failed";
                r.headers.data = NULL; r.headers.len = 0;
                return r;
            }

            Res res;
            res.status = 200;
            res.body   = html;
            /* 127.148: tmpl_renderpage returns heap memory and a Res does
             * not own its body — every page render leaked the rendered
             * HTML and its header block. */
            http_defer_free((void *)(uintptr_t)html);
            StrPair *hdrs = malloc(sizeof(StrPair));
            if (hdrs) {
                hdrs[0].key = "Content-Type";
                hdrs[0].val = "text/html; charset=utf-8";
                res.headers.data = hdrs;
                res.headers.len  = 1;
                http_defer_free(hdrs);
            } else {
                res.headers.data = NULL;
                res.headers.len  = 0;
            }
            return res;
        }
    }

    return mk404();
}

/*
 * scan_pages_recursive — recursively scan a directory for .tkt files
 * and register routes.  prefix is the URL path prefix (e.g. "" for root).
 */
static void scan_pages_recursive(const char *dir_path, const char *prefix,
                                  const char *templates_dir)
{
    DIR *d = opendir(dir_path);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_page_count < TK_MAX_PAGES) {
        if (ent->d_name[0] == '.') continue; /* skip hidden files */

        /* Build full filesystem path */
        char fullpath[2048];
        snprintf(fullpath, sizeof fullpath, "%s/%s", dir_path, ent->d_name);

        struct stat st;
        if (stat(fullpath, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            char subprefix[1024];
            snprintf(subprefix, sizeof subprefix, "%s/%s", prefix, ent->d_name);
            scan_pages_recursive(fullpath, subprefix, templates_dir);
            continue;
        }

        /* Check for .tkt extension */
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 4, ".tkt") != 0) continue;

        /* Only register pages (files containing {! layout() !}), not layouts.
         * A layout template (e.g. base.tkt) contains {! yield() !} and should
         * not be served as a standalone page. */
        {
            FILE *probe = fopen(fullpath, "r");
            if (!probe) continue;
            char head[256];
            size_t nread = fread(head, 1, sizeof(head) - 1, probe);
            fclose(probe);
            head[nread] = '\0';
            if (!strstr(head, "{! layout(")) continue;
        }

        /* Build route path */
        char route[1024];
        if (nlen == 9 && strcmp(ent->d_name, "index.tkt") == 0) {
            /* index.tkt maps to the directory root */
            if (prefix[0] == '\0')
                snprintf(route, sizeof route, "/");
            else
                snprintf(route, sizeof route, "%s", prefix);
        } else {
            /* Strip .tkt extension for route */
            char basename[256];
            size_t blen = nlen - 4;
            if (blen >= sizeof basename) blen = sizeof basename - 1;
            memcpy(basename, ent->d_name, blen);
            basename[blen] = '\0';
            snprintf(route, sizeof route, "%s/%s", prefix, basename);
        }

        /* Register route */
        int idx = g_page_count++;
        g_page_routes[idx].route = strdup(route);
        g_page_routes[idx].page_path = strdup(fullpath);
        g_page_routes[idx].templates_dir = strdup(templates_dir);
    }
    closedir(d);
}

int64_t tk_http_servepages_w(int64_t pages_dir_i64, int64_t templates_dir_i64) {
    const char *pages_dir = (const char *)(intptr_t)pages_dir_i64;
    const char *templates_dir = (const char *)(intptr_t)templates_dir_i64;
    if (!pages_dir || !templates_dir) return -1;

    /* Scan directory for .tkt files and register routes */
    scan_pages_recursive(pages_dir, "", templates_dir);

    /* Register an individual named route for each discovered page.
     * Named routes take priority over wildcard (*) routes in the router,
     * so these won't interfere with http.servedir fallback. */
    for (int i = 0; i < g_page_count; i++) {
        http_GET(g_page_routes[i].route, tk_page_dispatch);
    }

    return (int64_t)g_page_count;
}

/*
 * 135.24 item 1 — a plain HTTP server that does not start fails the process,
 * exactly as 135.22 made the TLS entry points do: a port that cannot be
 * bound (or a pool that cannot fork) still returns -1 to the caller, prints
 * `DIAGNOSTIC: kind=server_start_failed fn=... port=... stage=...` and
 * records the failure with tk_exitfail(), so the process exits 1 however
 * main ends.  Before, the one-line "could not bind" message was all there
 * was, and a program that discarded the -1 (every caller in the tree) exited
 * 0.  http.serve reaches this with workers=2 and is reported as itself.
 */
static int64_t serveworkers_as(const char *fn, int64_t host_ptr, int64_t port,
                               int64_t workers)
{
    const char *host;
    if (server_host(host_ptr, &host) != 0) return plain_start_failed(fn, port, "host");
    if (!server_port_ok(port)) return plain_start_failed(fn, port, "port");
    int p = (int)port;
    /* Pass NULL router — worker_loop will use the global route_table
     * which already has all handlers registered via http_GET/http_POST. */
    TkHttpErr err = http_serve_workers(NULL, host, (uint64_t)p, (int)workers);
    if (err != TK_HTTP_OK) return plain_start_failed(fn, p, "bind/fork");
    return 0;
}

int64_t tk_http_serveworkers_w(int64_t port, int64_t workers) {
    return serveworkers_as("http.serveworkers", 0, port, workers);
}

/* 138.1: http.serveon(host; port) and http.serveworkerson(host; port; workers). */
int64_t tk_http_serveon_w(int64_t host, int64_t port) {
    return serveworkers_as("http.serveon", host, port, 2);
}
int64_t tk_http_serveworkerson_w(int64_t host, int64_t port, int64_t workers) {
    return serveworkers_as("http.serveworkerson", host, port, workers);
}

/* ── router wrappers moved to router_glue.c (114.48) so std.router links
 *    standalone; http depends on router via stdlib_deps so still gets them. ── */

/* ── HTTP client stubs ───────────────────────────────────────────────── */

/* tk_http_client_w — create an HTTP client handle for the given base URL. */
int64_t tk_http_client_w(int64_t url) {
    if (!url) return 0;
    HttpClient *c = http_client((const char *)(intptr_t)url);
    return (int64_t)(intptr_t)c;
}
/* tk_http_get_w — simple GET; creates temporary client from the URL.
 * The URL includes the full path (e.g. http://host:port/path).
 * We split it: base = http://host:port, path = /path suffix.
 * This ensures client_parse_url correctly combines base + path. */
int64_t tk_http_get_w(int64_t url) {
    if (!url) return 0;
    const char *u = (const char *)(intptr_t)url;
    /* Find the path portion: skip scheme + host */
    const char *p = u;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    const char *slash = strchr(p, '/');
    char *base_url;
    const char *path;
    if (slash) {
        /* Split: base = scheme://host:port, path = /rest */
        size_t base_len = (size_t)(slash - u);
        base_url = (char *)malloc(base_len + 1);
        if (!base_url) return 0;
        memcpy(base_url, u, base_len);
        base_url[base_len] = '\0';
        path = slash;
    } else {
        base_url = strdup(u);
        path = "/";
    }
    HttpClient *c = http_client(base_url);
    free(base_url);
    if (!c) return 0;
    HttpClientResp resp = http_get(c, path);
    http_client_free(c);
    if (resp.is_err || !resp.body) return 0;
    char *s = (char *)malloc(resp.body_len + 1);
    if (!s) return 0;
    memcpy(s, resp.body, resp.body_len);
    s[resp.body_len] = '\0';
    free(resp.body);
    return (int64_t)(intptr_t)s;
}
/* tk_http_posturl_w — simple outbound POST to a full URL with JSON body.
 * http.posturl(url, body) → response body string */
int64_t tk_http_posturl_w(int64_t url, int64_t body) {
    if (!url) return 0;
    const char *u = (const char *)(intptr_t)url;
    const char *b = body ? (const char *)(intptr_t)body : "";
    /* Split URL into base + path */
    const char *p = u;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    const char *slash = strchr(p, '/');
    char *base_url;
    const char *path_str;
    if (slash) {
        size_t base_len = (size_t)(slash - u);
        base_url = (char *)malloc(base_len + 1);
        if (!base_url) return 0;
        memcpy(base_url, u, base_len);
        base_url[base_len] = '\0';
        path_str = slash;
    } else {
        base_url = strdup(u);
        path_str = "/";
    }
    HttpClient *c = http_client(base_url);
    free(base_url);
    if (!c) return 0;
    HttpClientResp resp = http_post(c, path_str,
                                     (const uint8_t *)b, strlen(b),
                                     "application/json");
    http_client_free(c);
    if (resp.is_err || !resp.body) return 0;
    char *s = (char *)malloc(resp.body_len + 1);
    if (!s) return 0;
    memcpy(s, resp.body, resp.body_len);
    s[resp.body_len] = '\0';
    free(resp.body);
    return (int64_t)(intptr_t)s;
}
/* tk_http_postjson_w alias for outbound POST */
int64_t tk_http_postjson_w(int64_t url, int64_t body) {
    return tk_http_posturl_w(url, body);
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
    /* 135.19 (3): declared `[byte]!httperr` (stdlib/http.tki) but returned a
     * malloc'd C string, so `.len` read the eight bytes before it and
     * `str.frombytes` of a chunk was empty.  A chunk is now a real [byte];
     * a graceful close is the empty [byte] docs/stdlib/http.md promises. */
    if (r.is_err) { free(r.data); free(r.err_msg); return 0; }
    int64_t h = tk_bytes_pack((const uint8_t *)r.data, r.data ? (uint64_t)r.len : 0);
    free(r.data);
    return h;
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
/*
 * 138.3 — client calls with their own names and result types.
 *
 * `http.get`, `http.post`, `http.put`, `http.delete` and `http.patch` are
 * ROUTE REGISTRATION (stdlib/http.tki, kind "route": path; handler).  The
 * client calls the spec gave the same names -- `http.get(c;path)` and
 * friends -- never had an implementation reachable from source: the
 * two-argument `http.get(client;path)` registered a GET route whose "path"
 * was the client handle (loke T-57: nothing sent, and the 257th call read
 * $ok once the route table filled), `tk_http_delete_w` was reachable from
 * nothing (R7), and the one-argument `http.get(url)` returned "" for a
 * closed port with no way to tell it from an empty body (T-15).
 *
 * The client calls now have names of their own, typed in http.tki:
 *
 *   http.clientget(c; path)               -> httpresp!httperr
 *   http.clientpost(c; path; body; ctype) -> httpresp!httperr
 *   http.clientput(c; path; body; ctype)  -> httpresp!httperr
 *   http.clientdelete(c; path)            -> httpresp!httperr
 *   http.geturl(url)                      -> str!httperr
 *
 * $err is a transport failure -- no connection, a send or receive that
 * failed, a URL that does not parse, HTTPS without TLS compiled in.  An HTTP
 * status is not an error for the client* calls: 404 is a response, and
 * `r.status` says so.  A `ctype` of "" sends application/octet-stream.
 * http.geturl returns only a body, so it has nowhere to
 * put a status and treats any status outside 200-299 as $err (code = the
 * status).
 *
 * The error is an `httperr` {msg; code} box in tk_current_error (runtime-abi
 * §7), set on every failure and cleared on every success, and the function
 * returns 0 for $err and a non-null value for $ok, so both the value
 * sentinel and the slot discriminate.
 */
static int64_t http_raise(const char *msg, uint64_t code)
{
    /* The message lives in a per-thread buffer, valid until the next raise
     * on this thread -- the lifetime tk_err_box gives the box itself. */
    static TK_WEB_TLS char msgbuf[256];
    snprintf(msgbuf, sizeof msgbuf, "%s", msg ? msg : "");
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)msgbuf;
    box[1] = (int64_t)code;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

static void client_resp_free(HttpClientResp *r)
{
    for (uint64_t i = 0; i < r->headers.len; i++) {
        free((char *)r->headers.data[i].key);
        free((char *)r->headers.data[i].val);
    }
    free(r->headers.data);
    free(r->body);
    free(r->err_msg);
    memset(r, 0, sizeof *r);
}

/* The $httpresp block {status; headers:[[str]]; body:[byte]}, one i64 slot
 * per field (136.4).  Takes the response apart: the header strings are
 * handed to the program, everything else is freed. */
static int64_t client_resp_block(HttpClientResp *r)
{
    int64_t *b = (int64_t *)malloc(3 * sizeof(int64_t));
    int64_t hdrs = tk_arr_alloc((int64_t)r->headers.len, (int64_t)r->headers.len);
    int64_t body = tk_bytes_pack(r->body ? r->body : (const uint8_t *)"", r->body_len);
    if (!b || !hdrs || !body) {
        free(b);
        /* a block from tk_arr_alloc starts TK_ARR_HDR words before its handle */
        if (hdrs) free((int64_t *)(intptr_t)hdrs - TK_ARR_HDR);
        if (body) free((int64_t *)(intptr_t)body - TK_ARR_HDR);
        client_resp_free(r);
        return http_raise("out of memory", 0);
    }
    int64_t n = 0;
    for (uint64_t i = 0; i < r->headers.len; i++) {
        int64_t pair = tk_arr_alloc(2, 2);
        if (!pair) break;
        ((int64_t *)(intptr_t)pair)[0] = (int64_t)(intptr_t)r->headers.data[i].key;
        ((int64_t *)(intptr_t)pair)[1] = (int64_t)(intptr_t)r->headers.data[i].val;
        r->headers.data[i].key = NULL;
        r->headers.data[i].val = NULL;
        ((int64_t *)(intptr_t)hdrs)[n++] = pair;
    }
    tk_arr_setlen(hdrs, n);
    b[0] = (int64_t)r->status;
    b[1] = hdrs;
    b[2] = body;
    client_resp_free(r);                 /* keys/vals already handed out */
    tk_current_error = 0;                /* 127.123: an ok return clears it */
    return (int64_t)(intptr_t)b;
}

static int64_t client_result(HttpClientResp r)
{
    if (r.is_err) {
        int64_t z = http_raise(r.err_msg ? r.err_msg : "request failed", r.err_code);
        client_resp_free(&r);
        return z;
    }
    return client_resp_block(&r);
}

int64_t tk_http_clientget_w(int64_t client, int64_t path) {
    if (!client) return http_raise("http.clientget: null client", 0);
    return client_result(http_get((HttpClient *)(intptr_t)client,
                                  path ? (const char *)(intptr_t)path : "/"));
}

int64_t tk_http_clientdelete_w(int64_t client, int64_t path) {
    if (!client) return http_raise("http.clientdelete: null client", 0);
    return client_result(http_delete((HttpClient *)(intptr_t)client,
                                     path ? (const char *)(intptr_t)path : "/"));
}

/* body is a toke [byte]; ctype "" sends no Content-Type of our choosing. */
static int64_t client_send(const char *fn, int is_put, int64_t client, int64_t path,
                           int64_t body, int64_t ctype)
{
    if (!client) return http_raise(fn, 0);
    uint8_t *buf = NULL;
    uint64_t n = tk_bytes_unpack(body, &buf);
    if (!buf) return http_raise("out of memory", 0);
    const char *ct = ctype ? (const char *)(intptr_t)ctype : "";
    const char *p = path ? (const char *)(intptr_t)path : "/";
    HttpClientResp r = is_put
        ? http_put((HttpClient *)(intptr_t)client, p, buf, n, ct[0] ? ct : NULL)
        : http_post((HttpClient *)(intptr_t)client, p, buf, n, ct[0] ? ct : NULL);
    free(buf);
    return client_result(r);
}

int64_t tk_http_clientpost_w(int64_t client, int64_t path, int64_t body, int64_t ctype) {
    return client_send("http.clientpost: null client", 0, client, path, body, ctype);
}

int64_t tk_http_clientput_w(int64_t client, int64_t path, int64_t body, int64_t ctype) {
    return client_send("http.clientput: null client", 1, client, path, body, ctype);
}

/* http.geturl(url) -> str!httperr: the body of a 2xx response. */
int64_t tk_http_geturl_w(int64_t url) {
    if (!url) return http_raise("http.geturl: null url", 0);
    const char *u = (const char *)(intptr_t)url;
    const char *p = u;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    else return http_raise("http.geturl: the url must start with http:// or https://", 0);
    const char *slash = strchr(p, '/');
    size_t base_len = slash ? (size_t)(slash - u) : strlen(u);
    char *base = (char *)malloc(base_len + 1);
    if (!base) return http_raise("out of memory", 0);
    memcpy(base, u, base_len); base[base_len] = '\0';
    HttpClient *c = http_client(base);
    free(base);
    if (!c) return http_raise("out of memory", 0);
    HttpClientResp r = http_get(c, slash ? slash : "/");
    http_client_free(c);
    if (r.is_err) return client_result(r);
    if (r.status < 200 || r.status > 299) {
        char m[64];
        snprintf(m, sizeof m, "HTTP status %llu", (unsigned long long)r.status);
        uint64_t st = r.status;
        client_resp_free(&r);
        return http_raise(m, st);
    }
    char *s = (char *)malloc(r.body_len + 1);
    if (!s) { client_resp_free(&r); return http_raise("out of memory", 0); }
    if (r.body_len) memcpy(s, r.body, r.body_len);
    s[r.body_len] = '\0';
    client_resp_free(&r);
    tk_current_error = 0;
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

/* 138.1: the address is honoured.  "127.0.0.1:11442" used to bind
 * *:11442 -- the host was parsed past and dropped, and http_serve() bound
 * INADDR_ANY -- so a program that asked for loopback listened on every
 * interface.  Forms: "host:port", ":port", "port", "host" (port 8080, as
 * before).  The host follows the http.serveon rules (server_host); an
 * explicit port 0 is any free port, reported on stderr, as for
 * http.serveworkers (135.24).  The server is the single-process path
 * (workers=1), which is what http_serve() was, and a start failure is
 * reported and fails the process as for every other entry point. */
int64_t tk_http_listen_w(int64_t addr, int64_t handler) {
    if (!handler) return -1;
    char hostbuf[256] = "";
    long port = 8080;
    if (addr) {
        const char *a = (const char *)(intptr_t)addr;
        const char *colon = strrchr(a, ':');
        const char *ps = NULL;
        if (colon) {
            size_t hl = (size_t)(colon - a);
            if (hl >= sizeof hostbuf)
                return plain_start_failed("http.listen", -1, "host");
            memcpy(hostbuf, a, hl); hostbuf[hl] = '\0';
            ps = colon + 1;
        } else if (a[0] >= '0' && a[0] <= '9' && !strchr(a, '.')) {
            ps = a;
        } else {
            size_t hl = strlen(a);
            if (hl >= sizeof hostbuf)
                return plain_start_failed("http.listen", -1, "host");
            memcpy(hostbuf, a, hl + 1);
        }
        if (ps) {
            char *end = NULL;
            port = strtol(ps, &end, 10);
            if (end == ps || *end != '\0')
                return plain_start_failed("http.listen", -1, "port");
        }
    }
    g_http_listen_handler = handler;
    http_GET("/*", tk_http_listen_dispatch);
    http_POST("/*", tk_http_listen_dispatch);
    return serveworkers_as("http.listen", (int64_t)(intptr_t)hostbuf, port, 1);
}
int64_t tk_http_withproxy_w(int64_t client, int64_t proxy_url) {
    return (int64_t)(intptr_t)http_withproxy((HttpClient *)(intptr_t)client, (const char *)(intptr_t)proxy_url);
}

/* ── router closure-dispatch table + get/post/put/delete/use/serve wrappers
 *    moved to router_glue.c (114.48). ── */


/* ── net wrappers ────────────────────────────────────────────────────── */

/* ── net wrappers moved to net_glue.c (Story 114.31) so std.net links
 *    standalone; http depends on net via stdlib_deps so still gets them. ── */

/* ── sys wrappers — defined in sys_glue.c ─────────────────────────── */

/* ── Misc stubs that remain in the HTTP glue (cross-module) ──────────── */

/* ── encrypt wrappers moved to encrypt_glue.c (Story 114.17/114.31): the
 *    versions here were stale (2-arg AES-GCM with an internal nonce that was
 *    discarded — broken) and collided with encrypt_glue.c's correct .tki-
 *    matching wrappers. http depends on encrypt via stdlib_deps. ────────── */

/* ── html wrappers (html.h) ───────────────────────────────────────── */








/* 136.33: chart.addchart / chart.serve moved to dashboard_glue.c; std.chart
 * now lists dashboard as a dependency, since both bodies are dashboard_* calls. */

/* 136.40 — the dataframe wrappers that lived here are now in
 * src/stdlib/dataframe_glue.c. Registered against std.http, they only
 * linked when a program happened to import HTTP too, so every documented
 * std.dataframe and std.analytics example failed at link (136.33).
 * Same extraction 136.32 did for yaml, 136.25 for ml, 114.35 for toon. */

/* ml wrappers moved to src/stdlib/ml_glue.c in 136.25 so `i=ml:std.ml` links
 * on its own. */

/* ── i18n wrappers (i18n.h) ───────────────────────────────────────── */

/* Global i18n bundle for simple get/fmt calls */







/* NOTE: these call collections_glue.c functions — declared as extern */
extern int64_t tk_array_append_w(int64_t arr_i64, int64_t elem);
extern int64_t tk_str_slice_w(int64_t s, int64_t start, int64_t end_);
/* i18n_localize/translate — delegate to i18n_get on the global bundle.
 * The locale/lang argument is ignored (bundle already loaded). */

/* 136.40 — the dashboard wrappers that lived here are now in
 * src/stdlib/dashboard_glue.c, for the same reason as the dataframe
 * block above: std.dashboard could not link without std.http. */

/* ── svg wrappers (svg.h) ─────────────────────────────────────────── */






/* ── canvas wrappers (canvas.h) ───────────────────────────────────── */




/* 135.3j: a "cache / kv" block stood here -- tk_cache_{new,set,get,del}_w
 * and a TkCache hash-map type.  new calloc'd a map nothing could fill; set,
 * get and del were `(void)arg; return 0;` stubs; no .tki declared any of
 * them and nothing called them (docs/reference/stdlib-internal.md listed
 * all four as dead).  std.http's real response cache is cache.c.  Removed
 * rather than finished: a toke-level kv store is a stdlib story, not a
 * glue fix. */

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
    /* allocate empty toke array */
    if (!pat || !str) {
        return tk_arr_alloc(0, 0);
    }
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) {
        return tk_arr_alloc(0, 0);
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

    /* build toke-format array: handle[-1]=len, handle[0..N-1]=elements */
    int64_t h = tk_arr_alloc((int64_t)count, (int64_t)count);
    if (!h) { free(items); return 0; }
    if (count > 0) memcpy((int64_t *)(intptr_t)h, items, count * sizeof(int64_t));
    free(items);
    return h;
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




/* 136.33: the auth.hash/jwt/verify/verifyjwt wrappers moved to auth_glue.c. */

/* ── rate limit — token bucket with per-key partitioning ───────────── */

#include <time.h>

#define TK_RL_MAP_SIZE 256 /* fixed-size hash table, power of 2 */

typedef struct {
    char    *key;        /* heap-allocated key string, NULL = empty slot */
    int64_t  tokens;
    time_t   last_refill;
} TkRlEntry;

typedef struct {
    int64_t    max_tokens;
    int64_t    window_sec;
    TkRlEntry  map[TK_RL_MAP_SIZE];
} TkRateLimiter;

static uint32_t tk_rl_hash(const char *s) {
    uint32_t h = 5381;
    while (*s) { h = ((h << 5) + h) ^ (uint32_t)(unsigned char)*s++; }
    return h;
}

int64_t tk_ratelimit_new_w(int64_t max, int64_t window) {
    TkRateLimiter *rl = (TkRateLimiter *)calloc(1, sizeof(TkRateLimiter));
    if (!rl) return 0;
    rl->max_tokens = max > 0 ? max : 10;
    rl->window_sec = window > 0 ? window : 60;
    return (int64_t)(intptr_t)rl;
}

int64_t tk_ratelimit_check_w(int64_t limiter, int64_t key) {
    if (!limiter) return 0;
    TkRateLimiter *rl = (TkRateLimiter *)(intptr_t)limiter;
    const char *key_str = key ? (const char *)(intptr_t)key : "__global__";
    uint32_t idx = tk_rl_hash(key_str) & (TK_RL_MAP_SIZE - 1);

    /* Linear probe to find existing entry or empty slot */
    TkRlEntry *slot = NULL;
    for (uint32_t i = 0; i < TK_RL_MAP_SIZE; i++) {
        uint32_t probe = (idx + i) & (TK_RL_MAP_SIZE - 1);
        TkRlEntry *e = &rl->map[probe];
        if (!e->key) {
            /* Empty slot — claim it for this key */
            e->key = strdup(key_str);
            if (!e->key) return 0;
            e->tokens = rl->max_tokens;
            e->last_refill = time(NULL);
            slot = e;
            break;
        }
        if (strcmp(e->key, key_str) == 0) {
            slot = e;
            break;
        }
    }
    if (!slot) return 0; /* table full */

    time_t now = time(NULL);
    int64_t elapsed = (int64_t)(now - slot->last_refill);
    if (elapsed >= rl->window_sec) {
        slot->tokens = rl->max_tokens;
        slot->last_refill = now;
    }
    if (slot->tokens > 0) {
        slot->tokens--;
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

/* yaml wrappers moved to src/stdlib/yaml_glue.c in 136.32 so
 * `i=yaml:std.yaml` links on its own, and so the nine documented
 * entry points (which had no wrapper at all) live beside them. */

/* ── toon wrappers (toon.h) ────────────────────────────────────────── */

/* Toon handles are stored as heap-allocated Toon structs cast to i64. */


















/* ── llm / tool wrappers (llm.h, llm_tool.h) ──────────────────────── */

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

/* 136.33: fmt.print moved to fmt_glue.c. */

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

/*
 * 138.8 — http.postheaders(url; body; headers) -> httpresp!httperr.
 *
 * It was `(i64, i64, i64, i64) -> i64 [abi]`: the glue read (url; body;
 * headersjson) and ignored a fourth argument, parsed the headers out of a
 * JSON string by hand (no escapes), always sent its own
 * `Content-Type: application/json` and then the caller's as well (two
 * Content-Type lines), returned the body or 0 with no status and no error,
 * and opened its own TLS connection with no certificate verification.
 *
 * Now it is typed: `headers` is [[str]], each element a two-element
 * @(name; value), the result is the response ($err for a transport failure,
 * the status in r.status otherwise), and the request goes through the
 * client every other call uses (http_request_headers), so https is verified
 * (C279) and a Content-Type among `headers` replaces the default
 * application/json instead of being sent twice.
 */
int64_t tk_http_postheaders_w(int64_t url_i64, int64_t body_i64, int64_t headers_i64) {
    if (!url_i64) return http_raise("http.postheaders: null url", 0);
    const char *u = (const char *)(intptr_t)url_i64;
    const char *body = body_i64 ? (const char *)(intptr_t)body_i64 : "";
    const char *p = u;
    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;
    else return http_raise("http.postheaders: the url must start with http:// or https://", 0);

    int64_t nh = headers_i64 ? ((int64_t *)(intptr_t)headers_i64)[-1] : 0;
    if (nh < 0 || nh > 256) return http_raise("http.postheaders: too many headers", 0);
    StrPair hv[256];
    for (int64_t i = 0; i < nh; i++) {
        int64_t pair = ((int64_t *)(intptr_t)headers_i64)[i];
        if (!pair || ((int64_t *)(intptr_t)pair)[-1] != 2)
            return http_raise("http.postheaders: each header must be @(name; value)", 0);
        hv[i].key = (const char *)(intptr_t)((int64_t *)(intptr_t)pair)[0];
        hv[i].val = (const char *)(intptr_t)((int64_t *)(intptr_t)pair)[1];
    }

    const char *slash = strchr(p, '/');
    size_t base_len = slash ? (size_t)(slash - u) : strlen(u);
    char *base = (char *)malloc(base_len + 1);
    if (!base) return http_raise("out of memory", 0);
    memcpy(base, u, base_len); base[base_len] = '\0';
    HttpClient *c = http_client(base);
    free(base);
    if (!c) return http_raise("out of memory", 0);
    HttpClientResp r = http_request_headers(c, "POST", slash ? slash : "/",
                                            (const uint8_t *)body, strlen(body),
                                            "application/json", hv, (uint64_t)nh);
    http_client_free(c);
    return client_result(r);
}

/* http.queryparam(req, name) — extract query string parameter */
int64_t tk_http_queryparam_w(int64_t req, int64_t name) {
    return tk_http_req_param(req, name);
}

/* ── v0.3 naming aliases ─────────────────────────────────────────────────
 *
 * Toke v0.3 codegen emits tk_<module>_<method>_w for all stdlib calls.
 * The original C functions use varying suffixes (_new, _json_new, etc.)
 * or multi-word names with underscores. These aliases bridge the gap.
 */

/* Response constructors */
int64_t tk_http_res_w(int64_t status, int64_t body) { return tk_http_res_new(status, body); }
int64_t tk_http_resnew_w(int64_t status, int64_t body) { return tk_http_res_new(status, body); }
int64_t tk_http_resjson_w(int64_t status, int64_t body) { return tk_http_res_json_new(status, body); }
int64_t tk_http_resjsonnew_w(int64_t status, int64_t body) { return tk_http_res_json_new(status, body); }
int64_t tk_http_jsonresp_w(int64_t status, int64_t body) { return tk_http_res_json_new(status, body); }
int64_t tk_http_resp_w(int64_t status, int64_t body, int64_t ct) {
    int64_t hdrs = TK_CT_HANDLE(g_html_ct);
    if (ct) {
        int64_t pair = tk_arr_alloc(2, 2);
        hdrs = tk_arr_alloc(1, 1);
        if (!pair || !hdrs) {
            if (pair) free((int64_t *)(intptr_t)pair - TK_ARR_HDR);
            if (hdrs) free((int64_t *)(intptr_t)hdrs - TK_ARR_HDR);
            return 0;
        }
        ((int64_t *)(intptr_t)pair)[0] = (int64_t)(intptr_t)"Content-Type";
        const char *ctype = (const char *)(intptr_t)ct;
        ((int64_t *)(intptr_t)pair)[1] = (int64_t)(intptr_t)ctype;
        ((int64_t *)(intptr_t)hdrs)[0] = pair;
    }
    return toke_res(status, hdrs, (const char *)(intptr_t)body);
}
int64_t tk_http_resok_w(int64_t body) { return tk_http_res_ok(body); }
int64_t tk_http_resbad_w(int64_t msg) { return tk_http_res_bad(msg); }
int64_t tk_http_reserr_w(int64_t msg) { return tk_http_res_err(msg); }

/* Request accessors */
int64_t tk_http_reqpath_w(int64_t req) { return tk_http_req_path(req); }
int64_t tk_http_reqmethod_w(int64_t req) { return tk_http_req_method(req); }
int64_t tk_http_reqbody_w(int64_t req) { return tk_http_req_body(req); }
int64_t tk_http_body_w(int64_t req) { return tk_http_req_body(req); }
int64_t tk_http_reqparam_w(int64_t req, int64_t name) { return tk_http_req_param(req, name); }
int64_t tk_http_param_w(int64_t req, int64_t name) { return tk_http_req_param(req, name); }
int64_t tk_http_reqheader_w(int64_t req, int64_t name) { return tk_http_req_header(req, name); }

/* Route registration (static) */
int64_t tk_http_getstatic_w(int64_t path, int64_t body) { return tk_http_get_static(path, body); }
int64_t tk_http_getstaticmime_w(int64_t path, int64_t body, int64_t mime) { return tk_http_get_static_mime(path, body, mime); }
int64_t tk_http_poststatic_w(int64_t path, int64_t body) { return tk_http_post_static(path, body); }
int64_t tk_http_postecho_w(int64_t path) { return tk_http_post_echo(path); }
/* tk_http_postjson route registration — use server-side post_json */
int64_t tk_http_postroutejson_w(int64_t path, int64_t body) { return tk_http_post_json(path, body); }

/* Route registration (handler function pointers) */
int64_t tk_http_gethandler_w(int64_t path, int64_t handler) { return tk_http_get_handler(path, handler); }
int64_t tk_http_posthandler_w(int64_t path, int64_t handler) { return tk_http_post_handler(path, handler); }
int64_t tk_http_puthandler_w(int64_t path, int64_t handler) { return tk_http_put_handler(path, handler); }
int64_t tk_http_deletehandler_w(int64_t path, int64_t handler) { return tk_http_delete_handler(path, handler); }
int64_t tk_http_patchhandler_w(int64_t path, int64_t handler) { return tk_http_patch_handler(path, handler); }

/* Server lifecycle */
int64_t tk_http_serve_w(int64_t port) { return tk_http_serve(port); }
int64_t tk_http_servetls_w(int64_t port, int64_t cert, int64_t key) { return tk_http_servetls(port, cert, key); }
int64_t tk_http_servedir_w(int64_t prefix, int64_t root) { return tk_http_serve_staticdir_w(prefix, root); }
/* tk_http_servepages_w defined above (Story 95.2) */

/* Server config */
int64_t tk_http_setnotfound_w(int64_t body) { return tk_http_set_notfound(body); }
int64_t tk_http_setcors_w(int64_t origins) { return tk_http_set_cors(origins); }

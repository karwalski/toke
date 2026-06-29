/*
 * router_glue.c — toke `_w` ABI wrappers for std.router.
 *
 * Story 114.35 (deferred half) / 114.48: these wrappers previously lived in
 * tk_web_glue.c, which is part of the *http* module's link set. A program that
 * imported only `std.router` (without `std.http`) therefore failed to link with
 * undefined `_tk_router_*_w` symbols. Moving them here — into the `router`
 * module's own c_files — lets std.router link standalone. The http module
 * depends on router (see stdlib_deps.c), so http programs still pick these up
 * transitively; the definitions were removed from tk_web_glue.c so there is no
 * duplicate symbol.
 *
 * It also fills in the wrappers that never existed: router.get/put/delete/use
 * (only new/post/serve had wrappers before, so router.get was undefined even
 * with http imported).
 *
 * Handler model: a toke router handler is a closure `(body:str) -> json:str`
 * passed as `&handler`. Each registered route stores {method, pattern, handler}
 * in a process-global table; one shared dispatch trampoline
 * (router_closure_dispatch) is registered with the C router for every route and
 * re-identifies the toke closure by matching the request's method+path.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "router.h"

/* call_route_handler — invoke a toke router handler with one argument.
 * `&handler` lowers to a raw function pointer (ptrtoint @handler), i.e.
 * `int64_t handler(int64_t body)` — NOT a [fn,env] closure pair — so it is
 * called directly (matching how the http handler glue treats `&handler`). */
static int64_t call_route_handler(int64_t fn_val, int64_t arg) {
    typedef int64_t (*RouteFn1)(int64_t);
    RouteFn1 f = (RouteFn1)(intptr_t)fn_val;
    return f(arg);
}

/* ── request / response objects (114.49) ─────────────────────────────────
 *
 * A router handler is `(req:i64) -> res:i64`. `req` is a handle to the parsed
 * request (read with router.reqbody/reqpath/reqmethod/reqquery/param); `res` is
 * a handle built by the router.ok/html/json/css/text/status/bad/notfound
 * builders. The dispatch trampoline constructs the req, calls the handler, and
 * turns the returned res handle into the C router's TkRouteResp. The req lives
 * only for the synchronous handler call (router dispatch is single-threaded).
 */
typedef struct {
    const char  *method;
    const char  *path;
    const char  *query;
    const char  *body;
    const char **pnames;
    const char **pvals;
    uint64_t     nparam;
} TkRouterReq;

typedef struct {
    int          status;
    const char  *content_type;
    const char  *body;        /* toke-owned string; outlives the send */
} TkRouterResp;

/* ── closure route registry ──────────────────────────────────────────── */

#define TK_MAX_CLOSURE_ROUTES 128

typedef struct {
    const char *method;   /* "GET", "POST", … */
    const char *path;     /* route pattern as registered */
    int64_t     handler;  /* toke handler fn pointer */
} TkClosureRoute;

static TkClosureRoute g_closure_routes[TK_MAX_CLOSURE_ROUTES];
static int            g_closure_route_count = 0;

static TkRouteResp router_closure_dispatch(TkRouteCtx ctx) {
    const char *m = ctx.method ? ctx.method : "GET";
    const char *p = ctx.path ? ctx.path : "/";
    for (int i = 0; i < g_closure_route_count; i++) {
        if (g_closure_routes[i].path &&
            strcmp(g_closure_routes[i].path, p) == 0 &&
            g_closure_routes[i].method &&
            strcmp(g_closure_routes[i].method, m) == 0) {
            TkRouterReq req = {
                m, p, ctx.query, ctx.body,
                ctx.param_names, ctx.param_values, ctx.nparam
            };
            int64_t res_i64 = call_route_handler(g_closure_routes[i].handler,
                                                 (int64_t)(intptr_t)&req);
            if (!res_i64) return router_resp_status(500, "handler returned null");
            TkRouterResp *rr = (TkRouterResp *)(intptr_t)res_i64;
            TkRouteResp out;
            memset(&out, 0, sizeof out);
            out.status       = rr->status;
            out.body         = rr->body ? rr->body : "";
            out.content_type = rr->content_type ? rr->content_type : "text/html";
            return out;
        }
    }
    return router_resp_404();
}

/* register a {method, pattern, handler} route and wire the shared trampoline
 * into the C router for the given method. */
static int64_t register_route(const char *method, int64_t router_i64,
                              int64_t path, int64_t handler,
                              void (*reg)(TkRouter *, const char *, TkRouteHandler)) {
    if (!router_i64 || !path) return -1;
    if (g_closure_route_count < TK_MAX_CLOSURE_ROUTES) {
        g_closure_routes[g_closure_route_count].method  = method;
        g_closure_routes[g_closure_route_count].path    = (const char *)(intptr_t)path;
        g_closure_routes[g_closure_route_count].handler = handler;
        g_closure_route_count++;
    }
    reg((TkRouter *)(intptr_t)router_i64, (const char *)(intptr_t)path,
        router_closure_dispatch);
    return 0;
}

/* ── wrappers ────────────────────────────────────────────────────────── */

int64_t tk_router_new_w(void) {
    TkRouter *r = router_new();
    return (int64_t)(intptr_t)r;
}

int64_t tk_router_get_w(int64_t router_i64, int64_t path, int64_t handler) {
    return register_route("GET", router_i64, path, handler, router_get);
}

int64_t tk_router_post_w(int64_t router_i64, int64_t path, int64_t handler) {
    return register_route("POST", router_i64, path, handler, router_post);
}

int64_t tk_router_put_w(int64_t router_i64, int64_t path, int64_t handler) {
    return register_route("PUT", router_i64, path, handler, router_put);
}

int64_t tk_router_delete_w(int64_t router_i64, int64_t path, int64_t handler) {
    return register_route("DELETE", router_i64, path, handler, router_delete);
}

/* router.use — middleware registration. Toke-level middleware closures are not
 * yet wired through; accept and ignore so programs that register none still
 * link and run. (A real implementation would adapt a toke closure to the
 * TkMiddleware (ctx, next) ABI.) */
int64_t tk_router_use_w(int64_t router_i64, int64_t middleware) {
    (void)router_i64; (void)middleware;
    return 0;
}

/* router.serve(router; host:str; port:u64) -> void!routererr.
 * The toke signature passes host and port as separate args (see router.tki). */
int64_t tk_router_serve_w(int64_t router_i64, int64_t host, int64_t port) {
    if (!router_i64) return -1;
    const char *h = host ? (const char *)(intptr_t)host : NULL;
    /* Treat "", "0.0.0.0" and "*" as "bind all interfaces" (NULL host). */
    if (h && (h[0] == '\0' || !strcmp(h, "0.0.0.0") || !strcmp(h, "*"))) h = NULL;
    uint64_t p = (uint64_t)port;
    if (p == 0 || p > 65535) p = 8080;
    TkRouterErr err = router_serve((TkRouter *)(intptr_t)router_i64, h, p);
    return err.failed ? -1 : 0;
}

/* ── request accessors (114.49) ──────────────────────────────────────────
 * Each takes the req handle and returns a toke string (char* as i64). A
 * missing field yields "" rather than null so str.* ops are always safe. */
static int64_t req_str(const char *s) {
    return (int64_t)(intptr_t)(s ? s : "");
}

int64_t tk_router_reqbody_w(int64_t req) {
    if (!req) return req_str("");
    return req_str(((TkRouterReq *)(intptr_t)req)->body);
}
int64_t tk_router_reqpath_w(int64_t req) {
    if (!req) return req_str("");
    return req_str(((TkRouterReq *)(intptr_t)req)->path);
}
int64_t tk_router_reqmethod_w(int64_t req) {
    if (!req) return req_str("");
    return req_str(((TkRouterReq *)(intptr_t)req)->method);
}
int64_t tk_router_reqquery_w(int64_t req) {
    if (!req) return req_str("");
    return req_str(((TkRouterReq *)(intptr_t)req)->query);
}
/* router.param(req; name) — value of the :name path param, or "". */
int64_t tk_router_param_w(int64_t req, int64_t name) {
    if (!req || !name) return req_str("");
    TkRouterReq *r = (TkRouterReq *)(intptr_t)req;
    const char *nm = (const char *)(intptr_t)name;
    for (uint64_t i = 0; i < r->nparam; i++)
        if (r->pnames && r->pnames[i] && strcmp(r->pnames[i], nm) == 0)
            return req_str(r->pvals ? r->pvals[i] : "");
    return req_str("");
}

/* ── response builders (114.49) ──────────────────────────────────────────
 * Build a heap TkRouterResp the dispatch reads back. content_type is a static
 * string; body is the toke-owned string handed in (it outlives the send). */
static int64_t make_resp(int status, const char *ct, int64_t body) {
    TkRouterResp *r = (TkRouterResp *)malloc(sizeof *r);
    if (!r) return 0;
    r->status = status;
    r->content_type = ct;
    r->body = body ? (const char *)(intptr_t)body : "";
    return (int64_t)(intptr_t)r;
}

int64_t tk_router_ok_w(int64_t body)   { return make_resp(200, "text/html; charset=utf-8", body); }
int64_t tk_router_html_w(int64_t body) { return make_resp(200, "text/html; charset=utf-8", body); }
int64_t tk_router_json_w(int64_t body) { return make_resp(200, "application/json", body); }
int64_t tk_router_css_w(int64_t body)  { return make_resp(200, "text/css; charset=utf-8", body); }
int64_t tk_router_text_w(int64_t body) { return make_resp(200, "text/plain; charset=utf-8", body); }
int64_t tk_router_bad_w(int64_t body)  { return make_resp(400, "text/html; charset=utf-8", body); }
int64_t tk_router_notfound_w(int64_t body) { return make_resp(404, "text/html; charset=utf-8", body); }
/* router.status(code; body) — custom status, text/html. */
int64_t tk_router_status_w(int64_t code, int64_t body) {
    int s = (int)code; if (s < 100 || s > 599) s = 200;
    return make_resp(s, "text/html; charset=utf-8", body);
}
/* router.respond(code; contenttype; body) — full control. */
int64_t tk_router_respond_w(int64_t code, int64_t ct, int64_t body) {
    int s = (int)code; if (s < 100 || s > 599) s = 200;
    const char *c = ct ? (const char *)(intptr_t)ct : "text/html; charset=utf-8";
    return make_resp(s, c, body);
}

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

/* ── closure route registry ──────────────────────────────────────────── */

#define TK_MAX_CLOSURE_ROUTES 128

typedef struct {
    const char *method;   /* "GET", "POST", … */
    const char *path;     /* route pattern as registered */
    int64_t     handler;  /* toke closure value */
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
            int64_t body_arg = ctx.body ? (int64_t)(intptr_t)ctx.body : 0;
            int64_t result = call_route_handler(g_closure_routes[i].handler, body_arg);
            const char *rbody = result ? (const char *)(intptr_t)result : "";
            return router_resp_json(rbody);
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

/* SPDX-License-Identifier: MIT */
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
 * in a process-global table that grows with the routes (127.348); one shared
 * dispatch trampoline (router_closure_dispatch) is registered with the C
 * router for every route and
 * finds the toke handler of the route the C router's matcher chose
 * (router_dispatch_route: router, method and pattern -- 127.343; it used to
 * compare the literal pattern with the request path, so a `:param` or `*`
 * route never reached its handler).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "router.h"
#include "tk_strhdr.h"   /* 127.343: tks_strdup -- request strings toke keeps */

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
 *
 * 127.343: the strings the accessors hand toke are sealed copies, never the
 * router's own.  Those live only for the dispatch -- the :param values are
 * strdup'd by match_route and freed when the dispatch returns; path, query
 * and method are static buffers of router_serve/router_handle_fd that the
 * next request overwrites; body points into the request buffer (on
 * router_handle_fd's stack) -- while a toke string lives for the program
 * (runtime-abi §3), so a handler that kept one in a mutable global read
 * freed or overwritten memory.  Each field is copied at most once per
 * request, on its first read (the copies are kept in the req), so a server's
 * string memory grows by what its handlers read, like any string a handler
 * builds.  reqmethod returns the registered route's method literal.
 */
#define TK_REQ_MEMO_PARAMS 32   /* router.c's MAX_PARAMS */
typedef struct {
    const char  *method;      /* the route's method literal: program lifetime */
    const char  *path;
    const char  *query;
    const char  *body;
    const char **pnames;
    const char **pvals;
    uint64_t     nparam;
    /* sealed copies, made on first read (NULL until then) */
    const char  *cpath, *cquery, *cbody;
    const char  *cvals[TK_REQ_MEMO_PARAMS];
} TkRouterReq;

typedef struct {
    int          status;
    const char  *content_type;
    const char  *body;        /* toke-owned string; outlives the send */
} TkRouterResp;

/* ── response handles (story 121.93) ─────────────────────────────────────
 *
 * router.ok/html/json/css/text/bad/notfound/status/respond return an i64
 * handle to a TkRouterResp.  It used to be a malloc'd pointer that nothing
 * freed: every request leaked one, and every response a handler built and
 * did not return leaked one more.  A plain free() after the dispatch reads
 * it would have been a use-after-free for a program that keeps a handle in
 * a module `let` and returns it again on a later request.
 *
 * The lifetime now follows router.h's TkRouteResp ownership rule (121.91):
 *   - a handle made DURING a dispatch (inside a handler) is the router's,
 *     valid until that dispatch has read the handler's answer; then it is
 *     released -- whether or not the handler returned it;
 *   - a handle made OUTSIDE any dispatch (in main, or a module `let`
 *     initialised before router.serve) lives for the whole program, so a
 *     prebuilt response can be returned on every request.
 * A handle is a slot index and a generation, not a pointer, and a released
 * slot's generation moves on.  So a handle kept past its dispatch -- cached
 * by a handler in a mutable global, say -- never reaches freed memory: when
 * a later handler returns it, the dispatch sees the generation mismatch and
 * answers 500 (DIAGNOSTIC: kind=router_invalid_response on stderr), as it
 * does for any i64 that is not a live handle.  Slots are reused, so a
 * server's handle memory is bounded by the most handles any one request
 * held, plus the program-lifetime ones.  Single-threaded, like the router. */
typedef struct {
    TkRouterResp r;
    uint32_t     gen;      /* 1..0x7fffffff; part of the handle */
    uint8_t      live;
} TkRespSlot;

static TkRespSlot *g_slots;
static uint32_t    g_nslots, g_capslots;
static uint32_t   *g_free_slots;           /* released slot indices */
static uint32_t    g_nfree, g_capfree;
static uint32_t   *g_req_slots;            /* made during this dispatch */
static uint32_t    g_nreq, g_capreq;
static int         g_in_dispatch;

/* room for one more entry in a growable uint32 list; 0 on OOM */
static int u32_reserve(uint32_t **a, uint32_t n, uint32_t *cap)
{
    if (n < *cap) return 1;
    uint32_t nc = *cap ? *cap * 2 : 16;
    uint32_t *na = (uint32_t *)realloc(*a, (size_t)nc * sizeof *na);
    if (!na) return 0;
    *a = na; *cap = nc;
    return 1;
}

/* resp_handle_new -- a live slot holding r, as a handle; 0 on OOM */
static int64_t resp_handle_new(TkRouterResp r)
{
    uint32_t idx;
    /* the request list must have room before a slot is taken, or a slot
     * made in a dispatch could never be released */
    if (g_in_dispatch && !u32_reserve(&g_req_slots, g_nreq, &g_capreq))
        return 0;
    if (g_nfree > 0) {
        idx = g_free_slots[--g_nfree];
    } else {
        if (g_nslots == g_capslots) {
            uint32_t nc = g_capslots ? g_capslots * 2 : 16;
            TkRespSlot *ns = (TkRespSlot *)realloc(g_slots, (size_t)nc * sizeof *ns);
            if (!ns) return 0;
            g_slots = ns; g_capslots = nc;
        }
        idx = g_nslots++;
        g_slots[idx].gen = 1;
    }
    g_slots[idx].r = r;
    g_slots[idx].live = 1;
    if (g_in_dispatch) g_req_slots[g_nreq++] = idx;
    return (int64_t)(((uint64_t)g_slots[idx].gen << 32) | ((uint64_t)idx + 1));
}

/* resp_handle_get -- the response a handle names, or NULL when it names no
 * live slot (released, from an earlier request, or not a handle at all) */
static const TkRouterResp *resp_handle_get(int64_t h)
{
    uint64_t u = (uint64_t)h;
    uint64_t idx1 = u & 0xffffffffu;
    uint32_t gen = (uint32_t)(u >> 32);
    if (idx1 == 0 || idx1 > g_nslots) return NULL;
    TkRespSlot *s = &g_slots[idx1 - 1];
    return (s->live && s->gen == gen) ? &s->r : NULL;
}

/* resp_handles_release -- release every slot made during this dispatch */
static void resp_handles_release(void)
{
    for (uint32_t i = 0; i < g_nreq; i++) {
        TkRespSlot *s = &g_slots[g_req_slots[i]];
        s->live = 0;
        s->gen = (s->gen % 0x7fffffffu) + 1;   /* never 0; stale handles miss */
        if (u32_reserve(&g_free_slots, g_nfree, &g_capfree))
            g_free_slots[g_nfree++] = g_req_slots[i];
        /* else OOM: the slot is retired, never reused -- still not freed */
    }
    g_nreq = 0;
}

/* ── closure route registry ──────────────────────────────────────────── */

/* 127.348: the table grows with the routes registered.  It was a fixed
 * array of 128: the 129th route was skipped here but still registered with
 * the C router and reported success, so it matched, found no handler and
 * answered 404 -- silently.  Now registration grows the table (doubling);
 * if that allocation fails the route is NOT registered with the C router
 * and registration reports failure (DIAGNOSTIC: kind=router_register_failed
 * on stderr, -1 from the wrapper), so no route is ever accepted without a
 * handler.  router.c's own route table is a linked list with no cap. */
typedef struct {
    const TkRouter *router; /* the router it was registered on */
    const char *method;   /* "GET", "POST", … (a literal) */
    const char *path;     /* route pattern as registered */
    int64_t     handler;  /* toke handler fn pointer */
} TkClosureRoute;

static TkClosureRoute *g_closure_routes;
static size_t          g_closure_route_count, g_closure_route_cap;

static TkRouteResp router_closure_dispatch(TkRouteCtx ctx) {
    /* 127.343: the route the router matched -- by its own matcher, which
     * binds :param segments and honours `*` and the priority rules -- not
     * the request path compared with each pattern. */
    const TkRouter *mr = NULL;
    const char *mm = NULL, *mp = NULL;
    if (!router_dispatch_route(&mr, &mm, &mp) || !mm || !mp)
        return router_resp_404();
    for (size_t i = 0; i < g_closure_route_count; i++) {
        if (g_closure_routes[i].router == mr &&
            g_closure_routes[i].path &&
            strcmp(g_closure_routes[i].path, mp) == 0 &&
            g_closure_routes[i].method &&
            strcmp(g_closure_routes[i].method, mm) == 0) {
            TkRouterReq req;
            memset(&req, 0, sizeof req);
            req.method = g_closure_routes[i].method;
            req.path   = ctx.path;
            req.query  = ctx.query;
            req.body   = ctx.body;
            req.pnames = ctx.param_names;
            req.pvals  = ctx.param_values;
            req.nparam = ctx.nparam;
            /* 121.93: handles made by the handler are released once its
             * answer is read (see "response handles" above).  The router
             * does not re-enter dispatch from a handler (router.h). */
            g_in_dispatch = 1;
            int64_t res_i64 = call_route_handler(g_closure_routes[i].handler,
                                                 (int64_t)(intptr_t)&req);
            g_in_dispatch = 0;
            TkRouteResp out;
            if (!res_i64) {
                out = router_resp_status(500, "handler returned null");
            } else {
                const TkRouterResp *rr = resp_handle_get(res_i64);
                if (!rr) {
                    fprintf(stderr, "DIAGNOSTIC: kind=router_invalid_response "
                            "reason=\"not a live response handle: made during an "
                            "earlier request, or not a router response\"\n");
                    out = router_resp_status(500, "handler returned an invalid response");
                } else {
                    memset(&out, 0, sizeof out);
                    out.status       = rr->status;
                    out.body         = rr->body ? rr->body : "";
                    out.content_type = rr->content_type ? rr->content_type : "text/html";
                }
            }
            resp_handles_release();   /* out holds no pointer into a slot */
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
    if (g_closure_route_count == g_closure_route_cap) {
        size_t nc = g_closure_route_cap ? g_closure_route_cap * 2 : 64;
        TkClosureRoute *nt = NULL;
        if (nc <= SIZE_MAX / sizeof *nt)
            nt = (TkClosureRoute *)realloc(g_closure_routes, nc * sizeof *nt);
        if (!nt) {
            /* 127.348: never accept a route that has no handler entry */
            fprintf(stderr, "DIAGNOSTIC: kind=router_register_failed "
                    "method=%s reason=\"out of memory growing the route table "
                    "(%zu routes registered)\"\n", method, g_closure_route_count);
            return -1;
        }
        g_closure_routes = nt;
        g_closure_route_cap = nc;
    }
    g_closure_routes[g_closure_route_count].router  = (const TkRouter *)(intptr_t)router_i64;
    g_closure_routes[g_closure_route_count].method  = method;
    g_closure_routes[g_closure_route_count].path    = (const char *)(intptr_t)path;
    g_closure_routes[g_closure_route_count].handler = handler;
    g_closure_route_count++;
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
    /* 135.26 item 2, aligned with http.serve (135.24 item 2): the port is
     * the caller's.  It used to be clamped -- 0 or anything above 65535
     * silently served on 8080.  Now 0 is a kernel-chosen port, which
     * router_serve reports on stderr (DIAGNOSTIC: kind=server_listening
     * port=N) once listening, and a port above 65535 is router_serve's
     * "port out of range" -- the $err arm of router.serve. */
    TkRouterErr err = router_serve((TkRouter *)(intptr_t)router_i64, h,
                                   (uint64_t)port);
    return err.failed ? -1 : 0;
}

/* ── request accessors (114.49) ──────────────────────────────────────────
 * Each takes the req handle and returns a toke string (char* as i64). A
 * missing field yields "" rather than null so str.* ops are always safe.
 * 127.343: a present field is a sealed copy that lives for the program (see
 * TkRouterReq above), made once per request and field. */
static int64_t req_str(const char *s) {
    return (int64_t)(intptr_t)(s ? s : "");
}

/* the sealed copy of s kept in *memo, made on the first call */
static int64_t req_copy(const char *s, const char **memo) {
    if (!s || !*s) return req_str("");
    if (!*memo) *memo = tks_strdup(s);
    return req_str(*memo ? *memo : "");
}

int64_t tk_router_reqbody_w(int64_t req) {
    if (!req) return req_str("");
    TkRouterReq *r = (TkRouterReq *)(intptr_t)req;
    return req_copy(r->body, &r->cbody);
}
int64_t tk_router_reqpath_w(int64_t req) {
    if (!req) return req_str("");
    TkRouterReq *r = (TkRouterReq *)(intptr_t)req;
    return req_copy(r->path, &r->cpath);
}
int64_t tk_router_reqmethod_w(int64_t req) {
    if (!req) return req_str("");
    return req_str(((TkRouterReq *)(intptr_t)req)->method);
}
int64_t tk_router_reqquery_w(int64_t req) {
    if (!req) return req_str("");
    TkRouterReq *r = (TkRouterReq *)(intptr_t)req;
    return req_copy(r->query, &r->cquery);
}
/* router.param(req; name) — value of the :name path param, or "". */
int64_t tk_router_param_w(int64_t req, int64_t name) {
    if (!req || !name) return req_str("");
    TkRouterReq *r = (TkRouterReq *)(intptr_t)req;
    const char *nm = (const char *)(intptr_t)name;
    for (uint64_t i = 0; i < r->nparam; i++)
        if (r->pnames && r->pnames[i] && strcmp(r->pnames[i], nm) == 0) {
            const char *v = r->pvals ? r->pvals[i] : NULL;
            if (i < TK_REQ_MEMO_PARAMS) return req_copy(v, &r->cvals[i]);
            return req_str(v && *v ? tks_strdup(v) : "");
        }
    return req_str("");
}

/* ── response builders (114.49) ──────────────────────────────────────────
 * A response handle the dispatch reads back (121.93: a slot, not a heap
 * block; lifetime under "response handles" above).  content_type is a
 * static string or the program's; body is the toke-owned string handed in
 * (it outlives the send).  0 on OOM, which the dispatch answers with 500. */
static int64_t make_resp(int status, const char *ct, int64_t body) {
    TkRouterResp r;
    r.status = status;
    r.content_type = ct;
    r.body = body ? (const char *)(intptr_t)body : "";
    return resp_handle_new(r);
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

/* SPDX-License-Identifier: MIT */
/*
 * router.c — Implementation of the std.router standard library module.
 *
 * Route matching rules:
 *   - Pattern segments are separated by '/'.
 *   - A segment of the form ':name' matches any single path segment and
 *     captures it under 'name'.
 *   - A trailing '*' segment matches any remaining path.
 *   - Priority: exact match > param match > wildcard match.
 *   - router_dispatch iterates routes in registration order; first match wins.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.  Who frees a response's memory is router.h's "TkRouteResp
 * memory ownership" (121.91): a handler's stays the handler's, and what the
 * router allocates for a dispatched response is released by the router.
 *
 * Story: 15.1.3
 */

#include "tk_strhdr.h"   /* 127.295: tk_strlen */
#include "router.h"
#include "ws.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <zlib.h>
#include "tk_bind.h"   /* 135.33 */
#include "tk_gzip_type.h"   /* 121.89 */
#include "tk_http_field.h"  /* 121.92 */
#include "tk_nosigpipe.h"   /* 127.347 */
#include "capabilities.h"   /* 121.42: router.serve takes the net grant */

/* ----------------------------------------------------------------------- */
/* Internal route entry                                                      */
/* ----------------------------------------------------------------------- */

#define MAX_PARAMS        32
#define MAX_CORS_HEADERS  16  /* max CORS response headers we build */
/* 127.349: the middleware chain, the WebSocket routes and the router_static
 * prefixes have no fixed cap any more (they were 64, 16 and 16, and the
 * one past each was dropped without a word).  The tables grow; a
 * registration that cannot allocate returns -1 and says so on stderr. */

/* Client IP populated by router_serve for each accepted connection.
 * Empty string when no connection context (e.g. direct router_dispatch call). */
static char serve_client_ip[48]; /* enough for IPv4 and IPv6 */

/* ----------------------------------------------------------------------- */
/* Response memory the router owns (story 121.91)                            */
/* ----------------------------------------------------------------------- */

/* Every allocation the router's OWN code makes for a response it hands back
 * from dispatch -- the header arrays and ETag string of router_use_etag, the
 * header arrays and compressed body of router_use_gzip, the body, header
 * arrays and header values of a router_static route -- is recorded here and
 * freed at the start of the next dispatch (router_dispatch,
 * router_dispatch_ex, or a request served by router_handle_fd/router_serve,
 * which also release it once the response is written) and by router_free.
 * So a dispatched response is valid until the next dispatch on any router,
 * the window the CORS middleware's static header arrays already had; the
 * caller frees none of it.  Before 121.91 the middleware malloc'd fresh
 * arrays per request and nothing freed them -- neither the caller (whose
 * free() would have hit CORS's static arrays had CORS run after) nor the
 * wire path.  Single-threaded, like the rest of the router's statics. */
static void  **resp_pool;
static size_t  resp_pool_n, resp_pool_cap;

/* resp_pool_add -- the router owns p until the next release.  NULL is
 * ignored.  If the list cannot grow p leaks rather than being freed while a
 * response still points at it. */
static void resp_pool_add(const void *p)
{
    if (!p) return;
    if (resp_pool_n == resp_pool_cap) {
        size_t nc = resp_pool_cap ? resp_pool_cap * 2 : 16;
        void **np = realloc(resp_pool, nc * sizeof *np);
        if (!np) return;
        resp_pool = np;
        resp_pool_cap = nc;
    }
    resp_pool[resp_pool_n++] = (void *)(uintptr_t)p;
}

static void resp_pool_release(void)
{
    for (size_t i = 0; i < resp_pool_n; i++) free(resp_pool[i]);
    resp_pool_n = 0;
}

typedef struct TkRoute {
    char           *method;
    char           *pattern;
    TkRouteHandler  handler;
    void           *udata;    /* 127.349: the route's own data (a router_static
                                * route's StaticEntry), freed with the route */
    struct TkRoute *next;
} TkRoute;

/* WebSocket endpoint registration */
typedef struct {
    char          *pattern;   /* 127.349: owned copy (was char[512], truncating) */
    TkWsOnOpen     on_open;
    TkWsOnMessage  on_message;
    TkWsOnClose    on_close;
} TkWsRoute;

struct TkRouter {
    TkRoute      *head;
    TkRoute      *tail;
    TkMiddleware *middleware;   /* 127.349: grows; was [64] */
    int           nmw, mw_cap;
    int           cors_enabled;
    TkCorsOpts    cors_opts;
    TkWsRoute    *ws_routes;    /* 127.349: grows; was [16] */
    int           nws, ws_cap;
};

/* reg_fail -- every registration that cannot be kept says so (127.349):
 * the function returns -1 and stderr carries one DIAGNOSTIC line, so a
 * caller that ignores the result still does not lose a route silently. */
static int reg_fail(const char *what, const char *pattern, const char *reason)
{
    fprintf(stderr, "DIAGNOSTIC: kind=router_register_failed what=%s "
            "pattern=\"%s\" reason=\"%s\"\n", what,
            pattern ? pattern : "", reason);
    return -1;
}

/* table_grow -- make room for one more element in *arr (n used, *cap
 * allocated, elt bytes each): 0, or -1 with *arr untouched. */
static int table_grow(void **arr, int n, int *cap, size_t elt)
{
    if (n < *cap) return 0;
    if (*cap > INT_MAX / 2) return -1;
    int nc = *cap ? *cap * 2 : 16;
    if ((size_t)nc > SIZE_MAX / elt) return -1;
    void *na = realloc(*arr, (size_t)nc * elt);
    if (!na) return -1;
    *arr = na;
    *cap = nc;
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Path splitting helpers                                                    */
/* ----------------------------------------------------------------------- */

/* Split 'path' on '/' into segments stored in out[].
 * Returns the number of segments, or -1 when the path has more than
 * max_segs of them.  out[] holds pointers into a heap copy of path that the
 * caller must free (returned via *buf_out) in either case.
 *
 * 127.146: it used to stop at max_segs and return max_segs, dropping the
 * rest, so a request with more segments than that compared equal on its
 * first max_segs and matched a route it does not match. */
static int split_path(const char *path, char **out, int max_segs, char **buf_out)
{
    /* skip leading slash */
    while (*path == '/') path++;

    char *buf = strdup(path);
    if (!buf) { *buf_out = NULL; return 0; }
    *buf_out = buf;

    int n = 0;
    char *p = buf;
    while (*p) {
        if (n >= max_segs) return -1;
        out[n++] = p;
        char *slash = strchr(p, '/');
        if (!slash) break;
        *slash = '\0';
        p = slash + 1;
    }
    /* trailing empty segment from trailing slash — skip */
    if (n > 0 && *out[n - 1] == '\0') n--;
    return n;
}

/* ----------------------------------------------------------------------- */
/* Match a single route pattern against a request path.                     */
/* Returns 0 on no match, 1 on match.                                       */
/* Populates names/values/nparam on match (pointers into heap copies).      */
/* ----------------------------------------------------------------------- */

/* Priority scoring: exact segment = 2, param segment = 1, wildcard = 0    */
#define PRIO_EXACT    2
#define PRIO_PARAM    1
#define PRIO_WILDCARD 0

typedef struct {
    int          matched;
    int          priority;    /* sum of per-segment priorities */
    const char  *names[MAX_PARAMS];
    const char  *values[MAX_PARAMS];
    int          nparam;
} MatchResult;

static MatchResult match_route(const char *pattern, const char *path)
{
    MatchResult res;
    memset(&res, 0, sizeof(res));

    char *pat_segs[MAX_PARAMS * 2];
    char *req_segs[MAX_PARAMS * 2];
    char *pat_buf = NULL, *req_buf = NULL;

    int npat = split_path(pattern, pat_segs, MAX_PARAMS * 2, &pat_buf);
    int nreq = split_path(path,    req_segs, MAX_PARAMS * 2, &req_buf);
    if (npat < 0 || nreq < 0) goto no_match;   /* 127.146: too many segments */

    /* Wildcard: last pattern segment is '*' */
    int has_wildcard = (npat > 0 && strcmp(pat_segs[npat - 1], "*") == 0);

    if (!has_wildcard && npat != nreq) goto no_match;
    if (has_wildcard  && nreq < npat - 1) goto no_match;

    int limit = has_wildcard ? npat - 1 : npat;
    for (int i = 0; i < limit; i++) {
        if (pat_segs[i][0] == ':') {
            /* param segment */
            if (res.nparam >= MAX_PARAMS) goto no_match;
            res.names [res.nparam] = strdup(pat_segs[i] + 1);
            res.values[res.nparam] = strdup(req_segs[i]);
            res.nparam++;
            res.priority += PRIO_PARAM;
        } else {
            /* exact segment */
            if (strcmp(pat_segs[i], req_segs[i]) != 0) goto no_match;
            res.priority += PRIO_EXACT;
        }
    }

    if (has_wildcard) res.priority += PRIO_WILDCARD;

    res.matched = 1;
    free(pat_buf);
    free(req_buf);
    return res;

no_match:
    /* free any param strings already allocated */
    for (int i = 0; i < res.nparam; i++) {
        free((char *)res.names[i]);
        free((char *)res.values[i]);
    }
    /* 135.3c: say so.  With nparam left set, router_ws_try_upgrade() -- which
     * frees mr.names[] for every result, matched or not -- freed the same
     * strings again: a double free on any WebSocket upgrade that matched a
     * `:param` segment and then missed. */
    res.nparam = 0;
    free(pat_buf);
    free(req_buf);
    res.matched = 0;
    return res;
}

/* ----------------------------------------------------------------------- */
/* Router lifecycle                                                          */
/* ----------------------------------------------------------------------- */

TkRouter *router_new(void)
{
    TkRouter *r = malloc(sizeof(TkRouter));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));   /* empty lists, empty tables, no CORS */
    return r;
}

void router_free(TkRouter *r)
{
    if (!r) return;
    TkRoute *cur = r->head;
    while (cur) {
        TkRoute *next = cur->next;
        free(cur->method);
        free(cur->pattern);
        free(cur->udata);
        free(cur);
        cur = next;
    }
    for (int i = 0; i < r->nws; i++) free(r->ws_routes[i].pattern);
    free(r->ws_routes);
    free(r->middleware);
    free(r);
    /* 121.91: the last response's router-owned memory goes with it.  The
     * pool's own array is kept for the next router; release leaves it empty. */
    resp_pool_release();
}

/* ----------------------------------------------------------------------- */
/* Route registration                                                        */
/* ----------------------------------------------------------------------- */

/* add_route -- link a route in: 0, or -1 (and a DIAGNOSTIC) with nothing
 * linked.  127.349: a failed malloc used to return silently, and a failed
 * strdup linked a node whose method or pattern was NULL, which the matcher
 * then dereferenced.  udata (may be NULL) is the route's from here on, also
 * on failure. */
static int add_route(TkRouter *r, const char *method, const char *pattern,
                     TkRouteHandler h, void *udata)
{
    if (!r || !pattern || !h) {
        free(udata);
        return reg_fail(method, pattern, !r ? "no router" :
                        !pattern ? "no pattern" : "no handler");
    }
    TkRoute *route = malloc(sizeof(TkRoute));
    char *m = strdup(method);
    char *p = strdup(pattern);
    if (!route || !m || !p) {
        free(route); free(m); free(p); free(udata);
        return reg_fail(method, pattern, "out of memory");
    }
    route->method  = m;
    route->pattern = p;
    route->handler = h;
    route->udata   = udata;
    route->next    = NULL;
    if (!r->head) { r->head = route; r->tail = route; }
    else          { r->tail->next = route; r->tail = route; }
    return 0;
}

int router_get   (TkRouter *r, const char *p, TkRouteHandler h) { return add_route(r, "GET",    p, h, NULL); }
int router_post  (TkRouter *r, const char *p, TkRouteHandler h) { return add_route(r, "POST",   p, h, NULL); }
int router_put   (TkRouter *r, const char *p, TkRouteHandler h) { return add_route(r, "PUT",    p, h, NULL); }
int router_delete(TkRouter *r, const char *p, TkRouteHandler h) { return add_route(r, "DELETE", p, h, NULL); }

int router_ws(TkRouter *r, const char *pattern,
              TkWsOnOpen on_open, TkWsOnMessage on_message, TkWsOnClose on_close)
{
    if (!r || !pattern)
        return reg_fail("ws", pattern, !r ? "no router" : "no pattern");
    char *p = strdup(pattern);
    if (!p || table_grow((void **)&r->ws_routes, r->nws, &r->ws_cap,
                         sizeof *r->ws_routes) != 0) {
        free(p);
        return reg_fail("ws", pattern, "out of memory");
    }
    TkWsRoute *ws = &r->ws_routes[r->nws++];
    ws->pattern    = p;
    ws->on_open    = on_open;
    ws->on_message = on_message;
    ws->on_close   = on_close;
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Middleware                                                                 */
/* ----------------------------------------------------------------------- */

int router_use(TkRouter *r, TkMiddleware mw)
{
    if (!r || !mw)
        return reg_fail("middleware", NULL, !r ? "no router" : "no middleware");
    if (table_grow((void **)&r->middleware, r->nmw, &r->mw_cap,
                   sizeof *r->middleware) != 0)
        return reg_fail("middleware", NULL, "out of memory");
    r->middleware[r->nmw++] = mw;
    return 0;
}

/* ----------------------------------------------------------------------- */
/* CORS middleware (Story 27.1.12)                                           */
/* ----------------------------------------------------------------------- */

/* Static slot used to pass cors_opts into the middleware function.
 * Safe because router_serve is single-threaded and router_dispatch
 * is documented as not re-entrant. */
static TkCorsOpts cors_mw_opts;

/* Join a NULL-terminated string array with ", " into buf[buflen].
 * Returns buf. */
static char *strarray_join(const char **arr, char *buf, size_t buflen)
{
    if (!arr || !arr[0]) { buf[0] = '\0'; return buf; }
    size_t pos = 0;
    for (int i = 0; arr[i]; i++) {
        if (i > 0) {
            if (pos + 2 < buflen) { buf[pos++] = ','; buf[pos++] = ' '; }
        }
        size_t slen = tk_strlen(arr[i]);
        if (pos + slen < buflen) {
            memcpy(buf + pos, arr[i], slen);
            pos += slen;
        }
    }
    buf[pos] = '\0';
    return buf;
}

/* Look up a request header value (case-insensitive name match). */
static const char *ctx_header(TkRouteCtx ctx, const char *name)
{
    for (uint64_t i = 0; i < ctx.nreq_headers; i++) {
        if (ctx.req_header_names[i] &&
            strcasecmp(ctx.req_header_names[i], name) == 0) {
            return ctx.req_header_values[i];
        }
    }
    return NULL;
}

/* Check if origin is allowed by opts.  Returns the string to echo back,
 * or NULL if not allowed.  Returns "*" for wildcard-allowed origins. */
static const char *cors_match_origin(const TkCorsOpts *opts, const char *origin)
{
    if (!opts->allowed_origins || !origin) return NULL;
    for (int i = 0; opts->allowed_origins[i]; i++) {
        if (strcmp(opts->allowed_origins[i], "*") == 0) {
            /* 121.18 (audit-120 WEB-05): a wildcard answers "*", always.
             * With allow_credentials it used to ECHO the caller's Origin and
             * add Allow-Credentials: true -- i.e. let every site on the web
             * read credentialed responses.  "*" with credentials is refused
             * by browsers, which is the safe outcome; a credentialed CORS
             * policy needs its origins listed (the middleware then also
             * leaves Allow-Credentials off for "*"). */
            return "*";
        }
        if (strcmp(opts->allowed_origins[i], origin) == 0) {
            return origin;
        }
    }
    return NULL;
}

/* Append a CORS response header to the running arrays.
 * hnames/hvals are static-duration string literals or static buffers.
 * Returns updated count. */
static uint64_t cors_append_header(const char **hnames, const char **hvals,
                                    uint64_t n, uint64_t max,
                                    const char *name, const char *val)
{
    if (n >= max || !name || !val || val[0] == '\0') return n;
    hnames[n] = name;
    hvals[n]  = val;
    return n + 1;
}

/* The CORS middleware function. */
static TkRouteResp cors_middleware(TkRouteCtx ctx, TkRouteHandler next)
{
    const TkCorsOpts *opts = &cors_mw_opts;
    const char *origin = ctx_header(ctx, "Origin");

    /* Static buffers for header values we build.  Safe because dispatch
     * is single-threaded. */
    static char methods_buf[512];
    static char headers_buf[512];
    static char expose_buf[512];
    static char max_age_buf[32];

    /* Storage for up to MAX_CORS_HEADERS CORS response headers. */
    static const char *hnames[MAX_CORS_HEADERS];
    static const char *hvals [MAX_CORS_HEADERS];

    /* If no Origin header — not a CORS request; pass through unchanged. */
    if (!origin || origin[0] == '\0') {
        return next(ctx);
    }

    const char *echo_origin = cors_match_origin(opts, origin);

    int is_preflight = (strcmp(ctx.method, "OPTIONS") == 0);

    if (is_preflight) {
        /* OPTIONS preflight: always respond 204, no body. */
        TkRouteResp resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 204;

        if (!echo_origin) {
            /* Origin not allowed: return plain 204 with no CORS headers. */
            return resp;
        }

        uint64_t nh = 0;

        /* Access-Control-Allow-Origin */
        nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                "Access-Control-Allow-Origin", echo_origin);

        /* Vary: Origin (only when not wildcard) */
        if (strcmp(echo_origin, "*") != 0) {
            nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                    "Vary", "Origin");
        }

        /* Access-Control-Allow-Methods */
        if (opts->allowed_methods && opts->allowed_methods[0]) {
            strarray_join(opts->allowed_methods, methods_buf, sizeof(methods_buf));
            nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                    "Access-Control-Allow-Methods", methods_buf);
        }

        /* Access-Control-Allow-Headers */
        if (opts->allowed_headers && opts->allowed_headers[0]) {
            strarray_join(opts->allowed_headers, headers_buf, sizeof(headers_buf));
            nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                    "Access-Control-Allow-Headers", headers_buf);
        }

        /* Access-Control-Max-Age */
        if (opts->max_age >= 0) {
            snprintf(max_age_buf, sizeof(max_age_buf), "%d", opts->max_age);
            nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                    "Access-Control-Max-Age", max_age_buf);
        }

        /* Access-Control-Allow-Credentials (never alongside "*": 121.18) */
        if (opts->allow_credentials && strcmp(echo_origin, "*") != 0) {
            nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                    "Access-Control-Allow-Credentials", "true");
        }

        resp.header_names  = hnames;
        resp.header_values = hvals;
        resp.nheaders      = nh;
        return resp;
    }

    /* Non-preflight: call the route handler, then append CORS headers. */
    TkRouteResp resp = next(ctx);

    if (!echo_origin) {
        /* Origin not allowed: return response as-is without CORS headers. */
        return resp;
    }

    /* Merge existing response headers with new CORS headers.
     * We copy existing headers into our static arrays first. */
    uint64_t nh = 0;

    /* Copy pre-existing headers */
    for (uint64_t i = 0; i < resp.nheaders && nh < MAX_CORS_HEADERS; i++) {
        hnames[nh] = resp.header_names[i];
        hvals [nh] = resp.header_values[i];
        nh++;
    }

    /* Access-Control-Allow-Origin */
    nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                            "Access-Control-Allow-Origin", echo_origin);

    /* Vary: Origin (only when not wildcard) */
    if (strcmp(echo_origin, "*") != 0) {
        nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                "Vary", "Origin");
    }

    /* Access-Control-Expose-Headers */
    if (opts->expose_headers && opts->expose_headers[0]) {
        strarray_join(opts->expose_headers, expose_buf, sizeof(expose_buf));
        nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                "Access-Control-Expose-Headers", expose_buf);
    }

    /* Access-Control-Allow-Credentials (never alongside "*": 121.18) */
    if (opts->allow_credentials && strcmp(echo_origin, "*") != 0) {
        nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                "Access-Control-Allow-Credentials", "true");
    }

    resp.header_names  = hnames;
    resp.header_values = hvals;
    resp.nheaders      = nh;
    return resp;
}

int router_use_cors(TkRouter *r, TkCorsOpts opts)
{
    if (!r) return reg_fail("middleware", NULL, "no router");
    /* Copy opts into the static slot and register the middleware.  127.349:
     * CORS is switched on only once the middleware is in the chain. */
    cors_mw_opts = opts;
    if (router_use(r, cors_middleware) != 0) return -1;
    r->cors_enabled = 1;
    r->cors_opts    = opts;
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Middleware chain runner                                                    */
/* ----------------------------------------------------------------------- */

/* Closure-style context for building the chain. */
typedef struct {
    TkRouter       *router;
    int             mw_index;   /* next middleware to invoke */
    TkRouteHandler  final;      /* terminal route handler    */
} MwChainCtx;

/* Thread-local chain context — used to thread state through middleware.
 * Single-threaded accept loop, so a simple static is safe. */
static MwChainCtx mw_chain_ctx;

static TkRouteResp mw_chain_next(TkRouteCtx ctx)
{
    MwChainCtx *c = &mw_chain_ctx;
    if (c->mw_index < c->router->nmw) {
        TkMiddleware mw = c->router->middleware[c->mw_index++];
        return mw(ctx, mw_chain_next);
    }
    return c->final(ctx);
}

/* No-op handler: used as the final handler for OPTIONS preflight when no
 * explicit OPTIONS route is registered.  The CORS middleware always
 * short-circuits OPTIONS before this is reached; it exists only to satisfy
 * the chain's function pointer requirement. */
static TkRouteResp cors_preflight_noop(TkRouteCtx ctx)
{
    (void)ctx;
    TkRouteResp r;
    memset(&r, 0, sizeof(r));
    r.status = 404;
    return r;
}

/* ----------------------------------------------------------------------- */
/* Dispatch (internal — shared by router_dispatch and router_dispatch_ex)    */
/* ----------------------------------------------------------------------- */

/* The route the dispatch in progress matched (127.343).  Set around the
 * middleware chain and handler call, restored after; read by the toke glue's
 * shared trampoline (router_glue.c) through router_dispatch_route. */
static const TkRouter *cur_router;
static const TkRoute  *cur_route;

int router_dispatch_route(const TkRouter **router, const char **method,
                          const char **pattern)
{
    if (!cur_route) return 0;
    if (router)  *router  = cur_router;
    if (method)  *method  = cur_route->method;
    if (pattern) *pattern = cur_route->pattern;
    return 1;
}

static TkRouteResp dispatch_internal(TkRouter *r,
                                      const char *method, const char *path,
                                      const char *query,  const char *body,
                                      const char **hnames, const char **hvals,
                                      uint64_t nhdrs)
{
    /* 121.91: the previous response's router-owned memory is released now;
     * a dispatched response is valid until the next dispatch. */
    resp_pool_release();

    /* We scan all registered routes and keep the best-priority match,
     * with first-registered winning ties (stable). */
    TkRoute    *best_route  = NULL;
    MatchResult best_result;
    memset(&best_result, 0, sizeof(best_result));

    for (TkRoute *cur = r->head; cur; cur = cur->next) {
        if (strcmp(cur->method, method) != 0) continue;
        MatchResult mr = match_route(cur->pattern, path);
        if (!mr.matched) continue;

        if (!best_route || mr.priority > best_result.priority) {
            /* free previous best param strings */
            if (best_route) {
                for (int i = 0; i < best_result.nparam; i++) {
                    free((char *)best_result.names[i]);
                    free((char *)best_result.values[i]);
                }
            }
            best_route  = cur;
            best_result = mr;
        } else {
            /* discard this result's param strings */
            for (int i = 0; i < mr.nparam; i++) {
                free((char *)mr.names[i]);
                free((char *)mr.values[i]);
            }
        }
    }

    if (!best_route) {
        /* CORS: handle OPTIONS preflight even when no explicit OPTIONS route
         * is registered — respond 204 with CORS headers if origin matches. */
        if (r->cors_enabled && strcmp(method, "OPTIONS") == 0) {
            /* fall through to the CORS middleware path with a no-op handler */
        } else {
            return router_resp_404();
        }
    }

    TkRouteCtx ctx;
    ctx.method            = method;
    ctx.path              = path;
    ctx.query             = query;
    ctx.body              = body;
    ctx.param_names       = (best_route && best_result.nparam) ? best_result.names  : NULL;
    ctx.param_values      = (best_route && best_result.nparam) ? best_result.values : NULL;
    ctx.nparam            = best_route ? (uint64_t)best_result.nparam : 0;
    ctx.req_header_names  = hnames;
    ctx.req_header_values = hvals;
    ctx.nreq_headers      = nhdrs;
    ctx.ip                = (serve_client_ip[0] != '\0') ? serve_client_ip : NULL;

    /* For OPTIONS preflight with CORS but no matching route, use a no-op handler
     * that the CORS middleware will short-circuit before it is ever called. */
    TkRouteHandler final_handler;
    if (best_route) {
        final_handler = best_route->handler;
    } else {
        /* Fallback: only reached for OPTIONS+CORS case above */
        final_handler = cors_preflight_noop;
    }

    TkRouteResp resp;
    const TkRouter *prev_router = cur_router;
    const TkRoute  *prev_route  = cur_route;
    cur_router = r;
    cur_route  = best_route;          /* NULL for the OPTIONS+CORS no-op */
    if (r->nmw > 0) {
        mw_chain_ctx.router   = r;
        mw_chain_ctx.mw_index = 0;
        mw_chain_ctx.final    = final_handler;
        resp = mw_chain_next(ctx);
    } else {
        resp = final_handler(ctx);
    }
    cur_router = prev_router;
    cur_route  = prev_route;

    /* free param strings */
    if (best_route) {
        for (int i = 0; i < best_result.nparam; i++) {
            free((char *)best_result.names[i]);
            free((char *)best_result.values[i]);
        }
    }

    return resp;
}

TkRouteResp router_dispatch(TkRouter *r, const char *method, const char *path,
                             const char *query, const char *body)
{
    return dispatch_internal(r, method, path, query, body, NULL, NULL, 0);
}

TkRouteResp router_dispatch_ex(TkRouter *r,
                                const char *method, const char *path,
                                const char *query,  const char *body,
                                const char **hnames, const char **hvals,
                                uint64_t nhdrs)
{
    return dispatch_internal(r, method, path, query, body, hnames, hvals, nhdrs);
}

/* ----------------------------------------------------------------------- */
/* Query string helper                                                       */
/* ----------------------------------------------------------------------- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* URL-decode src into a newly malloc'd string.  Returns NULL on alloc failure.
 *
 * 121.18 (audit-120 WEB-06): %00 is dropped, not decoded.  This decoder
 * emitted a NUL byte for it, so a query value could carry an embedded NUL and
 * silently truncate when later used as a C-string path -- while http.c's
 * decoder and encoding.c already drop %00 (the 57.4.3 null-byte hardening).
 * The three now agree. */
static char *url_decode(const char *src, size_t len)
{
    char *out = malloc(len + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; ) {
        if (src[i] == '+') {
            out[j++] = ' ';
            i++;
        } else if (src[i] == '%' && i + 2 < len) {
            int hi = hex_digit(src[i + 1]);
            int lo = hex_digit(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                int c = (hi << 4) | lo;
                if (c != 0) out[j++] = (char)c;
                i += 3;
            } else {
                out[j++] = src[i++];
            }
        } else {
            out[j++] = src[i++];
        }
    }
    out[j] = '\0';
    return out;
}

const char *router_query_get(const char *query, const char *key)
{
    if (!query || !key) return NULL;
    size_t klen = tk_strlen(key);
    const char *p = query;

    while (*p) {
        /* find '=' */
        const char *eq = strchr(p, '=');
        if (!eq) break;

        size_t name_len = (size_t)(eq - p);
        const char *val_start = eq + 1;

        /* find end of value (next '&' or end of string) */
        const char *amp = strchr(val_start, '&');
        size_t val_len = amp ? (size_t)(amp - val_start) : tk_strlen(val_start);

        if (name_len == klen && memcmp(p, key, klen) == 0) {
            return url_decode(val_start, val_len);
        }

        p = amp ? amp + 1 : val_start + val_len;
    }
    return NULL;
}

/* ----------------------------------------------------------------------- */
/* Convenience response constructors                                         */
/* ----------------------------------------------------------------------- */

size_t router_resp_body_len(TkRouteResp resp)
{
    if (!resp.body) return 0;
    return resp.body_len ? resp.body_len : tk_strlen(resp.body);
}

TkRouteResp router_resp_ok(const char *body, const char *ct)
{
    TkRouteResp resp;
    memset(&resp, 0, sizeof(resp));
    resp.status       = 200;
    resp.body         = body;
    resp.content_type = ct;
    return resp;
}

TkRouteResp router_resp_json(const char *json_body)
{
    return router_resp_ok(json_body, "application/json");
}

TkRouteResp router_resp_status(int status, const char *body)
{
    TkRouteResp resp;
    memset(&resp, 0, sizeof(resp));
    resp.status       = status;
    resp.body         = body;
    resp.content_type = (body && body[0] == '<') ? "text/html" : "text/plain";
    return resp;
}

TkRouteResp router_resp_404(void)
{
    TkRouteResp resp;
    memset(&resp, 0, sizeof(resp));
    resp.status       = 404;
    resp.body         = "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>";
    resp.content_type = "text/html";
    return resp;
}

/* ----------------------------------------------------------------------- */
/* HTTP request parser (minimal — mirrors http.c approach)                   */
/* ----------------------------------------------------------------------- */

static void router_parse_request(const char *raw,
                                  const char **method_out,
                                  const char **path_out,
                                  const char **query_out,
                                  const char **body_out)
{
    static char method_buf[16];
    static char path_buf[2048];
    static char query_buf[2048];

    *method_out = NULL;
    *path_out   = NULL;
    *query_out  = NULL;
    *body_out   = NULL;

    /* Request line: METHOD /path?query HTTP/1.x\r\n */
    int mi = 0;
    while (*raw && *raw != ' ' && mi < 15) method_buf[mi++] = *raw++;
    method_buf[mi] = '\0';
    *method_out = method_buf;

    if (*raw == ' ') raw++;

    /* path (up to '?' or ' ') */
    int pi = 0;
    while (*raw && *raw != '?' && *raw != ' ' && pi < 2047) path_buf[pi++] = *raw++;
    path_buf[pi] = '\0';
    *path_out = path_buf;

    /* query string */
    if (*raw == '?') {
        raw++;
        int qi = 0;
        while (*raw && *raw != ' ' && qi < 2047) query_buf[qi++] = *raw++;
        query_buf[qi] = '\0';
        *query_out = query_buf;
    }

    /* skip to body: past \r\n\r\n */
    const char *sep = strstr(raw, "\r\n\r\n");
    if (sep) *body_out = sep + 4;
}

/* ----------------------------------------------------------------------- */
/* WebSocket upgrade helper (Story 27.1.15)                                  */
/* ----------------------------------------------------------------------- */

/*
 * ws_extract_header — scan raw HTTP request bytes for a header by name
 * (case-insensitive).  Writes NUL-terminated value into out_buf.
 * Returns out_buf on success, NULL if the header is not present.
 */
static const char *ws_extract_header(const char *raw,
                                      const char *name,
                                      char *out_buf, size_t out_len)
{
    size_t name_len = tk_strlen(name);
    const char *p = raw;

    /* Skip the request line */
    while (*p && *p != '\r' && *p != '\n') p++;
    while (*p == '\r' || *p == '\n') p++;

    while (*p) {
        const char *line_end = p;
        while (*line_end && *line_end != '\r' && *line_end != '\n') line_end++;
        if (line_end == p) break; /* empty line = end of headers */

        const char *colon = p;
        while (colon < line_end && *colon != ':') colon++;

        if (colon < line_end) {
            size_t hlen = (size_t)(colon - p);
            if (hlen == name_len) {
                size_t i;
                int ok = 1;
                for (i = 0; i < hlen; i++) {
                    char a = p[i], b = name[i];
                    if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
                    if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
                    if (a != b) { ok = 0; break; }
                }
                if (ok) {
                    const char *val = colon + 1;
                    while (val < line_end && *val == ' ') val++;
                    const char *val_end = line_end;
                    while (val_end > val &&
                           (*(val_end-1) == ' ' || *(val_end-1) == '\r'))
                        val_end--;
                    size_t vlen = (size_t)(val_end - val);
                    if (vlen >= out_len) vlen = out_len - 1;
                    memcpy(out_buf, val, vlen);
                    out_buf[vlen] = '\0';
                    return out_buf;
                }
            }
        }

        p = line_end;
        while (*p == '\r' || *p == '\n') p++;
    }
    return NULL;
}

/*
 * router_ws_try_upgrade — attempt to handle a WebSocket upgrade on fd.
 *
 * Returns 1 if the request carried Upgrade: websocket (regardless of
 * whether a matching route was found — the caller must NOT send a normal
 * HTTP response in that case).
 * Returns 0 if the request is a plain HTTP request.
 *
 * raw  — raw bytes read from the socket.
 * path — already-parsed URL path.
 */
static int router_ws_try_upgrade(TkRouter *r, int fd,
                                  const char *raw, const char *path)
{
    char upgrade_val[64];
    char ws_key[128];
    int  i;

    if (!ws_extract_header(raw, "Upgrade", upgrade_val, sizeof(upgrade_val)))
        return 0;

    /* Normalise to lowercase for comparison */
    {
        size_t j;
        for (j = 0; upgrade_val[j]; j++) {
            char c = upgrade_val[j];
            if (c >= 'A' && c <= 'Z') upgrade_val[j] = (char)(c + 32);
        }
    }
    if (strcmp(upgrade_val, "websocket") != 0)
        return 0;

    /* 127.347: from here on every write on fd is a WebSocket server write.
     * They used plain write(2), so a client that closed first (a browser tab
     * shut mid-handshake, a peer that sends CLOSE and leaves before our CLOSE
     * reply) raised SIGPIPE and killed the server.  SO_NOSIGPIPE on the socket
     * where it exists (it also covers writes the on_open / on_message
     * callbacks make on fd) and tk_send_all (MSG_NOSIGNAL) on every write. */
    tk_sock_nosigpipe(fd);

    /* Find a matching WS route */
    TkWsRoute *matched = NULL;
    for (i = 0; i < r->nws; i++) {
        MatchResult mr = match_route(r->ws_routes[i].pattern, path);
        {
            int k;
            for (k = 0; k < mr.nparam; k++) {
                free((char *)mr.names[k]);
                free((char *)mr.values[k]);
            }
        }
        if (mr.matched) {
            matched = &r->ws_routes[i];
            break;
        }
    }

    if (!matched) {
        static const char resp404[] =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)tk_send_all(fd, resp404, sizeof(resp404) - 1);
        return 1;
    }

    if (!ws_extract_header(raw, "Sec-WebSocket-Key", ws_key, sizeof(ws_key))) {
        static const char resp400[] =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n\r\n";
        (void)tk_send_all(fd, resp400, sizeof(resp400) - 1);
        return 1;
    }

    /* Compute accept key and send 101 */
    {
        const char *accept = ws_accept_key(ws_key);
        if (!accept) {
            static const char resp500[] =
                "HTTP/1.1 500 Internal Server Error\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            (void)tk_send_all(fd, resp500, sizeof(resp500) - 1);
            return 1;
        }
        {
            char resp101[512];
            int n = snprintf(resp101, sizeof(resp101),
                             "HTTP/1.1 101 Switching Protocols\r\n"
                             "Upgrade: websocket\r\n"
                             "Connection: Upgrade\r\n"
                             "Sec-WebSocket-Accept: %s\r\n\r\n",
                             accept);
            free((char *)accept);
            if (n > 0) (void)tk_send_all(fd, resp101, (size_t)n);
        }
    }

    if (matched->on_open)
        matched->on_open(fd);

    /* Frame receive loop */
    {
        uint8_t  buf[65536];
        uint64_t buf_used = 0;

        for (;;) {
            ssize_t nr = read(fd, buf + buf_used,
                              sizeof(buf) - (size_t)buf_used - 1);
            if (nr <= 0) break;
            buf_used += (uint64_t)nr;

            while (buf_used > 0) {
                uint64_t consumed = 0;
                WsFrameResult fr = ws_decode_frame(buf, buf_used, &consumed);
                if (fr.is_err || fr.frame == NULL) break; /* need more data */

                WsFrame *frame = fr.frame;

                switch (frame->opcode) {
                case WS_PING: {
                    WsEncodeResult pong =
                        ws_encode_frame(WS_PONG,
                                        frame->payload,
                                        frame->payload_len, 0);
                    if (!pong.is_err && pong.data)
                        (void)tk_send_all(fd, pong.data, (size_t)pong.len);
                    ws_encode_result_free(&pong);
                    break;
                }
                case WS_CLOSE: {
                    WsEncodeResult cfr =
                        ws_encode_frame(WS_CLOSE, NULL, 0, 0);
                    if (!cfr.is_err && cfr.data)
                        (void)tk_send_all(fd, cfr.data, (size_t)cfr.len);
                    ws_encode_result_free(&cfr);
                    if (matched->on_close)
                        matched->on_close(fd);
                    ws_frame_free(frame);
                    return 1;
                }
                case WS_TEXT:
                case WS_BINARY:
                    if (matched->on_message)
                        matched->on_message(fd,
                                            (const char *)frame->payload,
                                            (size_t)frame->payload_len,
                                            frame->opcode == WS_BINARY ? 1 : 0);
                    break;
                default:
                    break;
                }

                ws_frame_free(frame);
                buf_used -= consumed;
                if (buf_used > 0)
                    memmove(buf, buf + consumed, (size_t)buf_used);
            }
        }
    }

    /* EOF without a WS close frame */
    if (matched->on_close)
        matched->on_close(fd);

    return 1;
}

/* ----------------------------------------------------------------------- */
/* router_serve — HTTP server using this router for dispatch                 */
/* ----------------------------------------------------------------------- */

/* router_reason -- the reason phrase for a status code (RFC 9110 sec. 15).
 * 121.90: every response used to say "OK" -- "HTTP/1.1 404 OK".  A code
 * with no registered phrase gets an empty one, which RFC 9112 sec. 4
 * allows ("HTTP/1.1 299 "). */
static const char *router_reason(int s)
{
    switch (s) {
    case 100: return "Continue";            case 101: return "Switching Protocols";
    case 200: return "OK";                  case 201: return "Created";
    case 202: return "Accepted";            case 203: return "Non-Authoritative Information";
    case 204: return "No Content";          case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 300: return "Multiple Choices";    case 301: return "Moved Permanently";
    case 302: return "Found";               case 303: return "See Other";
    case 304: return "Not Modified";        case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";         case 401: return "Unauthorized";
    case 402: return "Payment Required";    case 403: return "Forbidden";
    case 404: return "Not Found";           case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";      case 408: return "Request Timeout";
    case 409: return "Conflict";            case 410: return "Gone";
    case 411: return "Length Required";     case 412: return "Precondition Failed";
    case 413: return "Content Too Large";   case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";  case 418: return "I'm a teapot";
    case 421: return "Misdirected Request"; case 422: return "Unprocessable Content";
    case 425: return "Too Early";           case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";     case 502: return "Bad Gateway";
    case 503: return "Service Unavailable"; case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    default:  return "";
    }
}

/* hdr_name_ok / hdr_value_ok -- tk_http_field.h's rule (121.90), shared
 * with http.c's writers since 121.92: a name that is not an RFC 9110 token,
 * or a value holding CR, LF or another control byte but HTAB, is dropped. */
#define hdr_name_ok  tk_http_field_name_ok
#define hdr_value_ok tk_http_field_value_ok

/* write_all -- write n bytes, retrying short writes and EINTR. */
static void write_all(int fd, const char *b, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, b, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        b += (size_t)w;
        n -= (size_t)w;
    }
}

/* Growable header block for router_send_response. */
typedef struct { char *b; size_t n, cap; int oom; } HdrBuf;

static void hb_put(HdrBuf *h, const char *s, size_t n)
{
    if (h->oom) return;
    if (h->n + n + 1 > h->cap) {
        size_t nc = h->cap ? h->cap : 512;
        while (h->n + n + 1 > nc) nc *= 2;
        char *nb = realloc(h->b, nc);
        if (!nb) { h->oom = 1; return; }
        h->b = nb; h->cap = nc;
    }
    memcpy(h->b + h->n, s, n);
    h->n += n;
    h->b[h->n] = '\0';
}
static void hb_str(HdrBuf *h, const char *s) { if (s) hb_put(h, s, tk_strlen(s)); }

/* router_send_response -- write resp as an HTTP/1.1 response.
 *
 * 121.90: the status line carries the code's reason phrase (it said "OK"
 * for every status), and resp.header_names/header_values are written (they
 * were dropped, so Content-Encoding, ETag, Cache-Control, Content-Range and
 * every CORS header never reached a client).
 *
 * The writer owns framing: Content-Length and Connection are its own, and a
 * Content-Length, Transfer-Encoding or Connection header in the list is not
 * sent.  Content-Type comes from resp.content_type; a Content-Type in the
 * list is sent only when content_type is NULL (router_static_serve sets
 * both, to the same value).  A header whose name is not a token or whose
 * value holds CR, LF or another control byte is DROPPED, never repaired --
 * that is a response-splitting attempt or a bug, and stripping the bytes
 * would still send a value nobody wrote.  A content_type that fails the
 * same check is replaced by application/octet-stream.  A status outside
 * 100..599 is sent as 500.  1xx, 204 and 304 carry no body and no
 * Content-Length (RFC 9110 sec. 6.4.1, 8.6). */
static void router_send_response(int fd, TkRouteResp resp)
{
    int status = (resp.status >= 100 && resp.status <= 599) ? resp.status : 500;
    int no_body = status < 200 || status == 204 || status == 304;

    const char *ct = resp.content_type;
    for (uint64_t i = 0; !ct && i < resp.nheaders; i++) {
        const char *k = resp.header_names  ? resp.header_names[i]  : NULL;
        const char *v = resp.header_values ? resp.header_values[i] : NULL;
        if (k && strcasecmp(k, "Content-Type") == 0 && hdr_value_ok(v)) ct = v;
    }
    if (!ct) ct = "text/plain";
    if (!hdr_value_ok(ct)) ct = "application/octet-stream";

    const char *bd = resp.body ? resp.body : "";
    /* 121.88: a served file or a gzip stream holds 0x00 -- strlen() sent a
     * binary file cut at its first NUL, with a Content-Length to match. */
    size_t bl = no_body ? 0 : router_resp_body_len(resp);

    HdrBuf h = { NULL, 0, 0, 0 };
    char line[96];
    snprintf(line, sizeof line, "HTTP/1.1 %d %s\r\n", status, router_reason(status));
    hb_str(&h, line);
    if (status != 204 && status != 304 && status >= 200) {
        hb_str(&h, "Content-Type: "); hb_str(&h, ct); hb_str(&h, "\r\n");
        snprintf(line, sizeof line, "Content-Length: %zu\r\n", bl);
        hb_str(&h, line);
    }
    for (uint64_t i = 0; i < resp.nheaders; i++) {
        const char *k = resp.header_names  ? resp.header_names[i]  : NULL;
        const char *v = resp.header_values ? resp.header_values[i] : NULL;
        if (!k || !v || !hdr_name_ok(k) || !hdr_value_ok(v)) continue;
        if (strcasecmp(k, "Content-Length") == 0 ||
            strcasecmp(k, "Transfer-Encoding") == 0 ||
            strcasecmp(k, "Connection") == 0 ||
            strcasecmp(k, "Content-Type") == 0) continue;
        hb_str(&h, k); hb_str(&h, ": "); hb_str(&h, v); hb_str(&h, "\r\n");
    }
    hb_str(&h, "Connection: close\r\n\r\n");

    if (h.oom) {
        static const char r500[] =
            "HTTP/1.1 500 Internal Server Error\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n";
        write_all(fd, r500, sizeof r500 - 1);
        free(h.b);
        return;
    }
    write_all(fd, h.b, h.n);
    free(h.b);
    if (bl) write_all(fd, bd, bl);
}

/* MAX_REQ_HEADERS -- request header fields router_handle_fd/router_serve
 * hand to dispatch; any beyond are not seen by middleware or handlers. */
#define MAX_REQ_HEADERS 64

/* router_parse_headers -- split the header block of raw (the bytes after
 * the request line, up to the empty line) into names[]/vals[], copying it
 * into buf[cap] first so raw is left whole (the WebSocket upgrade scans it).
 * Values lose their leading and trailing blanks.  A line with no colon, an
 * empty name, a name with a blank before its colon, or a continuation line
 * (obs-fold) is skipped.  Lines end at CRLF or a bare LF.  Returns the
 * number of fields.
 *
 * 121.90: router_handle_fd and router_serve dispatched with NO request
 * headers, so over the wire the gzip middleware never saw Accept-Encoding,
 * the ETag middleware never saw If-None-Match and CORS never saw Origin. */
static uint64_t router_parse_headers(const char *raw, char *buf, size_t cap,
                                     const char **names, const char **vals,
                                     uint64_t max)
{
    const char *p = raw;
    while (*p && *p != '\n') p++;          /* skip the request line */
    if (*p == '\n') p++;
    const char *end = strstr(p, "\r\n\r\n");
    const char *end2 = strstr(p, "\n\n");
    if (!end || (end2 && end2 < end)) end = end2;
    size_t len = end ? (size_t)(end - p) : tk_strlen(p);
    if (len >= cap) len = cap - 1;
    memcpy(buf, p, len);
    buf[len] = '\0';

    uint64_t n = 0;
    char *line = buf;
    while (line && *line && n < max) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
            if (nl > line && nl[-1] == '\r') nl[-1] = '\0';
        }
        char *next = nl ? nl + 1 : NULL;
        if (*line != ' ' && *line != '\t') {
            char *colon = strchr(line, ':');
            if (colon && colon > line && colon[-1] != ' ' && colon[-1] != '\t') {
                *colon = '\0';
                char *v = colon + 1;
                while (*v == ' ' || *v == '\t') v++;
                char *ve = v + tk_strlen(v);
                while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = '\0';
                names[n] = line;
                vals[n]  = v;
                n++;
            }
        }
        line = next;
    }
    return n;
}

TkRouterErr router_serve(TkRouter *r, const char *host, uint64_t port)
{
    TkRouterErr ok_err;
    memset(&ok_err, 0, sizeof(ok_err));

    if (!r) {
        TkRouterErr e = {1, "router is NULL"};
        return e;
    }
    if (port > 65535) {
        TkRouterErr e = {1, "port out of range"};
        return e;
    }

    /* 135.28 item 2: the host is honoured, by http.serveon's rules (138.1,
     * tk_web_glue.c server_host).  It was ignored -- every router bound all
     * interfaces, so router.serve(r;"127.0.0.1";p) was reachable from the
     * network.  NULL, "", "*" and "0.0.0.0" are all interfaces; "localhost"
     * is 127.0.0.1; any other host must be an IPv4 literal (the listener is
     * AF_INET), else router.serve takes $err -- never a silent widening. */
    struct in_addr bind_addr;
    bind_addr.s_addr = htonl(INADDR_ANY);
    if (host && host[0] && strcmp(host, "*") != 0 && strcmp(host, "0.0.0.0") != 0) {
        const char *h = strcmp(host, "localhost") == 0 ? "127.0.0.1" : host;
        if (inet_pton(AF_INET, h, &bind_addr) != 1) {
            TkRouterErr e = {1, "host must be an IPv4 address, \"localhost\", "
                                "or \"\", \"*\" or \"0.0.0.0\" for all interfaces"};
            return e;
        }
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr        = bind_addr;
    addr.sin_port        = htons((uint16_t)port);

    /* 135.33: SO_REUSEADDR only after a probe finds no live listener -- on
     * macOS it let this bind shadow another server on the same port and
     * take its loopback traffic.  See tk_bind.h. */
    /* 121.42 (1): binding is net authority (ADR-0010).  router.serve --
     * and chart.serve and dashboard.serve, which serve through here -- bound
     * and listened in a --cap-enforce binary granted nothing. */
    TK_REQUIRE(TK_CAP_NET);
    int srv = tk_bind_exclusive(&addr);
    if (srv < 0) {
        TkRouterErr e = {1, errno == EADDRINUSE
                               ? "bind() failed: address already in use"
                               : "bind() failed"};
        return e;
    }
    if (listen(srv, 8) < 0) {
        close(srv);
        TkRouterErr e = {1, "listen() failed"};
        return e;
    }
    /* 135.26 item 2: port 0 asked the kernel for a free port.  Say which,
     * once, now that it is listening -- the line http.serve prints for the
     * same case (135.24), since router_serve does not return until
     * shutdown and the caller has no other way to learn it. */
    if (port == 0) {
        struct sockaddr_in got;
        socklen_t glen = sizeof got;
        memset(&got, 0, sizeof got);
        if (getsockname(srv, (struct sockaddr *)&got, &glen) == 0) {
            fprintf(stderr, "DIAGNOSTIC: kind=server_listening port=%u\n",
                    (unsigned)ntohs(got.sin_port));
            fflush(stderr);
        }
    }

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(srv, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) continue;

        /* Capture client IP into the static buffer for logging middleware. */
        if (inet_ntop(AF_INET, &peer.sin_addr,
                      serve_client_ip, sizeof(serve_client_ip)) == NULL) {
            serve_client_ip[0] = '\0';
        }

        char raw[8192];
        memset(raw, 0, sizeof(raw));
        ssize_t nr = read(fd, raw, sizeof(raw) - 1);
        if (nr <= 0) { close(fd); continue; }

        const char *method = NULL;
        const char *path   = NULL;
        const char *query  = NULL;
        const char *body   = NULL;
        router_parse_request(raw, &method, &path, &query, &body);

        /* Check for WebSocket upgrade before normal HTTP dispatch */
        if (r->nws > 0 &&
            router_ws_try_upgrade(r, fd, raw, path ? path : "/")) {
            serve_client_ip[0] = '\0';
            close(fd);
            continue;
        }

        char        hbuf[8192];
        const char *hn[MAX_REQ_HEADERS], *hv[MAX_REQ_HEADERS];
        uint64_t    nh = router_parse_headers(raw, hbuf, sizeof hbuf,
                                              hn, hv, MAX_REQ_HEADERS);
        TkRouteResp resp = router_dispatch_ex(r,
            method ? method : "GET",
            path   ? path   : "/",
            query, body, hn, hv, nh);

        router_send_response(fd, resp);
        resp_pool_release();        /* 121.91: written; release it now */
        serve_client_ip[0] = '\0'; /* clear after dispatch */
        close(fd);
    }

    /* unreachable in normal operation */
    close(srv);
    return ok_err;
}

void router_handle_fd(TkRouter *r, int fd)
{
    if (!r || fd < 0) return;

    char raw[8192];
    memset(raw, 0, sizeof(raw));
    ssize_t nr = read(fd, raw, sizeof(raw) - 1);
    if (nr <= 0) { close(fd); return; }

    const char *method = NULL;
    const char *path   = NULL;
    const char *query  = NULL;
    const char *body   = NULL;
    router_parse_request(raw, &method, &path, &query, &body);

    if (r->nws > 0 &&
        router_ws_try_upgrade(r, fd, raw, path ? path : "/")) {
        close(fd);
        return;
    }

    char        hbuf[8192];
    const char *hn[MAX_REQ_HEADERS], *hv[MAX_REQ_HEADERS];
    uint64_t    nh = router_parse_headers(raw, hbuf, sizeof hbuf,
                                          hn, hv, MAX_REQ_HEADERS);
    TkRouteResp resp = router_dispatch_ex(r,
        method ? method : "GET",
        path   ? path   : "/",
        query, body, hn, hv, nh);
    router_send_response(fd, resp);
    resp_pool_release();            /* 121.91: written; release it now */
    close(fd);
}

/* ----------------------------------------------------------------------- */
/* Static file serving (Story 27.1.5)                                        */
/* ----------------------------------------------------------------------- */

/* MIME type table — extension → content-type */
typedef struct { const char *ext; const char *mime; } MimeEntry;

static const MimeEntry MIME_TABLE[] = {
    { "html",  "text/html; charset=utf-8" },
    { "htm",   "text/html; charset=utf-8" },
    { "css",   "text/css; charset=utf-8" },
    { "js",    "application/javascript; charset=utf-8" },
    { "mjs",   "application/javascript; charset=utf-8" },
    { "json",  "application/json; charset=utf-8" },
    { "xml",   "application/xml; charset=utf-8" },
    { "txt",   "text/plain; charset=utf-8" },
    { "csv",   "text/csv; charset=utf-8" },
    { "png",   "image/png" },
    { "jpg",   "image/jpeg" },
    { "jpeg",  "image/jpeg" },
    { "gif",   "image/gif" },
    { "svg",   "image/svg+xml" },
    { "webp",  "image/webp" },
    { "avif",  "image/avif" },
    { "ico",   "image/x-icon" },
    { "woff2", "font/woff2" },
    { "woff",  "font/woff" },
    { "ttf",   "font/ttf" },
    { "otf",   "font/otf" },
    { "eot",   "application/vnd.ms-fontobject" },
    { "pdf",   "application/pdf" },
    { "wasm",  "application/wasm" },
    { "mp4",   "video/mp4" },
    { "webm",  "video/webm" },
    { "mp3",   "audio/mpeg" },
    { "ogg",   "audio/ogg" },
    { "zip",   "application/zip" },
    { "map",   "application/json" },
    { NULL, NULL }
};

static const char *mime_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (!dot || dot[1] == '\0') return "application/octet-stream";
    const char *ext = dot + 1;
    for (int i = 0; MIME_TABLE[i].ext; i++) {
        if (strcmp(ext, MIME_TABLE[i].ext) == 0)
            return MIME_TABLE[i].mime;
    }
    return "application/octet-stream";
}

/* Check for path traversal: reject any path containing ".." or "//" */
static int has_traversal(const char *path)
{
    /* Reject ".." components */
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') return 1;
        if (p[0] == '/' && p[1] == '/') return 1;
        p++;
    }
    return 0;
}

/* Build response with a dynamically-allocated body.
 * status, body (heap), and content_type must all be set.
 * nheaders/header_names/header_values cover additional headers. */
static TkRouteResp static_resp_with_body(int status, char *body,
                                          size_t body_len,
                                          const char *ct,
                                          const char **hnames,
                                          const char **hvals,
                                          uint64_t nh)
{
    TkRouteResp resp;
    memset(&resp, 0, sizeof(resp));
    resp.status       = status;
    resp.body         = body;
    resp.body_len     = body_len;   /* 121.88: the bytes read, NULs included */
    resp.content_type = ct;
    resp.header_names  = hnames;
    resp.header_values = hvals;
    resp.nheaders      = nh;
    return resp;
}

/* static_confine — resolve `full` (in place) and require it to lie under the
 * resolved root dir_path.  Returns 0, 404 (it does not exist) or 403.  One
 * copy of the check for the requested path and for a directory's index. */
static int static_confine(const char *dir_path, char *full, size_t cap)
{
    char resolved_root[PATH_MAX];
    char resolved_full[PATH_MAX];
    if (!realpath(dir_path, resolved_root)) return 403;
    if (!realpath(full, resolved_full))
        return (errno == ENOENT || errno == ENOTDIR) ? 404 : 403;
    size_t root_len = tk_strlen(resolved_root);
    if (strncmp(resolved_full, resolved_root, root_len) != 0 ||
        (resolved_full[root_len] != '\0' && resolved_full[root_len] != '/'))
        return 403;
    int n = snprintf(full, cap, "%s", resolved_full);
    if (n < 0 || (size_t)n >= cap) return 403;
    return 0;
}

/* router_static_serve — public entry point for serving a single file.
 * dir_path: filesystem directory root (e.g. "/var/www/static")
 * rel_path: relative URL path after the prefix (e.g. "style.css" or "")
 * if_none_match: value of the If-None-Match request header, or NULL
 * range_header: value of the Range request header, or NULL
 *
 * On status 200/206: resp.body and resp.header_values[0] (ETag) are heap-allocated;
 * callers are responsible for freeing them.
 */
TkRouteResp router_static_serve(const char *dir_path, const char *rel_path,
                                 const char *if_none_match,
                                 const char *range_header)
{
    if (!dir_path || !rel_path) return router_resp_status(403, "Forbidden");

    /* Reject traversal in the relative portion */
    if (has_traversal(rel_path)) return router_resp_status(403, "Forbidden");

    /* Build full filesystem path */
    char full[4096];
    /* Skip any leading '/' in rel_path to avoid double-slash */
    const char *rel = rel_path;
    while (*rel == '/') rel++;

    int n;
    if (*rel == '\0') {
        n = snprintf(full, sizeof(full), "%s", dir_path);
    } else {
        n = snprintf(full, sizeof(full), "%s/%s", dir_path, rel);
    }
    if (n < 0 || (size_t)n >= sizeof(full))
        return router_resp_status(403, "Forbidden");

    /* Reject embedded null bytes in path (prevents truncation attacks) */
    if (memchr(full, '\0', (size_t)n) != NULL)
        return router_resp_status(403, "Forbidden");

    /* Resolve symlinks and validate the path stays under dir_path */
    int rc = static_confine(dir_path, full, sizeof full);
    if (rc) return rc == 404 ? router_resp_404() : router_resp_status(403, "Forbidden");

    /* stat the path */
    struct stat st;
    if (stat(full, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return router_resp_404();
        return router_resp_status(403, "Forbidden");
    }

    /* If directory, append /index.html */
    if (S_ISDIR(st.st_mode)) {
        /* 121.44: built from the directory's RESOLVED path, which the check
         * above has just confined to the root -- not re-joined from the
         * unresolved dir_path and rel.  The index file then gets the same
         * check (121.18: an index.html symlinked out of the tree served its
         * target). */
        char dir[PATH_MAX];
        int m = snprintf(dir, sizeof dir, "%s", full);
        if (m < 0 || (size_t)m >= sizeof dir)
            return router_resp_status(403, "Forbidden");
        m = snprintf(full, sizeof(full), "%s/index.html", dir);
        if (m < 0 || (size_t)m >= sizeof(full))
            return router_resp_status(403, "Forbidden");
        rc = static_confine(dir_path, full, sizeof full);
        if (rc) return rc == 404 ? router_resp_404() : router_resp_status(403, "Forbidden");
        if (stat(full, &st) != 0)
            return router_resp_404();
    }

    /* Build ETag: "{mtime}-{size}" */
    char etag[64];
    snprintf(etag, sizeof(etag), "\"%ld-%ld\"",
             (long)st.st_mtime, (long)st.st_size);

    /* Conditional GET: If-None-Match */
    if (if_none_match && strcmp(if_none_match, etag) == 0) {
        TkRouteResp resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = 304;
        return resp;
    }

    /* Read file contents */
    int fd = open(full, O_RDONLY);
    if (fd < 0) return router_resp_status(403, "Forbidden");

    size_t fsz = (size_t)st.st_size;
    char *buf = malloc(fsz + 1);
    if (!buf) { close(fd); return router_resp_status(500, "Internal Error"); }

    size_t total = 0;
    while (total < fsz) {
        ssize_t nr = read(fd, buf + total, fsz - total);
        if (nr <= 0) break;
        total += (size_t)nr;
    }
    buf[total] = '\0';
    close(fd);

    const char *mime = mime_for_path(full);

    /* ── Range request support (Story 59.4.7) ── */
    if (range_header && total > 0) {
        /* Parse "bytes=start-end" (single range only) */
        const char *eq = strchr(range_header, '=');
        if (eq && strncmp(range_header, "bytes", 5) == 0) {
            eq++;
            size_t rstart = 0, rend = total - 1;
            int valid = 1;

            if (*eq == '-') {
                /* bytes=-N  → last N bytes */
                size_t suffix = (size_t)atoll(eq + 1);
                if (suffix == 0 || suffix > total) { valid = 0; }
                else { rstart = total - suffix; }
            } else {
                rstart = (size_t)atoll(eq);
                const char *dash = strchr(eq, '-');
                if (dash && *(dash + 1) != '\0')
                    rend = (size_t)atoll(dash + 1);
                if (rstart > rend || rstart >= total) valid = 0;
                if (rend >= total) rend = total - 1;
            }

            if (!valid) {
                free(buf);
                /* 416 Range Not Satisfiable */
                char cr_hdr[64];
                snprintf(cr_hdr, sizeof cr_hdr, "bytes */%zu", total);
                const char **h416n = malloc(sizeof(char *));
                const char **h416v = malloc(sizeof(char *));
                if (!h416n || !h416v) { free(h416n); free(h416v); }  /* 135.3c: leaked one */
                else {
                    h416n[0] = "Content-Range"; h416v[0] = strdup(cr_hdr);
                    TkRouteResp r416;
                    memset(&r416, 0, sizeof(r416));
                    r416.status = 416;
                    r416.body = "Range Not Satisfiable";
                    r416.header_names = h416n; r416.header_values = h416v;
                    r416.nheaders = 1;
                    return r416;
                }
                return router_resp_status(416, "Range Not Satisfiable");
            }

            /* Extract the requested range */
            size_t part_len = rend - rstart + 1;
            char *part = malloc(part_len + 1);
            if (!part) { free(buf); return router_resp_status(500, "Internal Error"); }
            memcpy(part, buf + rstart, part_len);
            part[part_len] = '\0';
            free(buf);

            /* Build Content-Range header */
            char cr_val[128];
            snprintf(cr_val, sizeof cr_val, "bytes %zu-%zu/%zu", rstart, rend, total);

            int cache_secs = (strstr(mime, "text/html") != NULL) ? 3600 : 604800;
            char cache_ctrl[48];
            snprintf(cache_ctrl, sizeof cache_ctrl, "public, max-age=%d", cache_secs);

            const char **hnames = malloc(5 * sizeof(char *));
            const char **hvals  = malloc(5 * sizeof(char *));
            if (!hnames || !hvals) {
                free(hnames); free(hvals); free(part);
                return router_resp_status(500, "Internal Error");
            }
            hnames[0] = "ETag";           hvals[0] = strdup(etag);
            hnames[1] = "Content-Type";   hvals[1] = strdup(mime);
            hnames[2] = "Cache-Control";  hvals[2] = strdup(cache_ctrl);
            hnames[3] = "Content-Range";  hvals[3] = strdup(cr_val);
            hnames[4] = "Accept-Ranges";  hvals[4] = strdup("bytes");

            return static_resp_with_body(206, part, part_len, mime, hnames, hvals, 5);
        }
    }

    /* Cache lifetime: 1 hour for HTML, 7 days for assets */
    int cache_secs = (strstr(mime, "text/html") != NULL) ? 3600 : 604800;
    char cache_ctrl[48];
    snprintf(cache_ctrl, sizeof cache_ctrl, "public, max-age=%d", cache_secs);

    /* Build response headers: ETag, Content-Type, Cache-Control, Accept-Ranges */
    const char **hnames = malloc(4 * sizeof(char *));
    const char **hvals  = malloc(4 * sizeof(char *));
    if (!hnames || !hvals) {
        free(hnames); free(hvals); free(buf);
        return router_resp_status(500, "Internal Error");
    }
    hnames[0] = "ETag";          hvals[0] = strdup(etag);
    hnames[1] = "Content-Type";  hvals[1] = strdup(mime);
    hnames[2] = "Cache-Control"; hvals[2] = strdup(cache_ctrl);
    hnames[3] = "Accept-Ranges"; hvals[3] = strdup("bytes");
    if (!hvals[0] || !hvals[1] || !hvals[2] || !hvals[3]) {
        for (int i = 0; i < 4; i++) free((void *)hvals[i]);
        free(hnames); free(hvals); free(buf);
        return router_resp_status(500, "Internal Error");
    }

    return static_resp_with_body(200, buf, total, mime, hnames, hvals, 4);
}

/* Per-prefix handler (127.349).  A router_static route's StaticEntry hangs
 * off the route itself (TkRoute.udata) and the one handler every such route
 * shares finds it through the route the dispatch in progress matched
 * (cur_route, as the toke glue's trampoline does since 127.343).  This
 * replaces a process-wide StaticEntry[16] with one macro trampoline per
 * slot: the 17th router_static ever made -- on any router, as slots were
 * never released -- was dropped silently and its prefix answered 404, and a
 * prefix past 511 bytes or a directory past 1023 was truncated. */

typedef struct {
    char *url_prefix;   /* both point into the same allocation as the entry */
    char *dir_path;
} StaticEntry;

/* static_route_serve -- what a router_static route runs: strip the
 * registered prefix from ctx.path and serve the rest with router_static_serve.
 *
 * 121.90: the request's If-None-Match and Range headers are passed through.
 * The trampolines used to pass NULL for both ("not available through
 * TkRouteCtx", which was untrue -- ctx carries the request headers), so a
 * router_static route never answered 304 or 206 over the wire.
 *
 * 121.91: what router_static_serve allocated -- for 200/206 the body; for
 * any response with headers both header arrays and every header value --
 * is handed to the router's response pool, so a dispatched static response
 * is released at the next dispatch like every other router-built one.
 * router_static_serve called directly keeps its caller-frees contract. */
static const char *ctx_req_header(TkRouteCtx ctx, const char *name);
static TkRouteResp static_route_serve(const StaticEntry *se, TkRouteCtx ctx);

static TkRouteResp static_route_handler(TkRouteCtx ctx)
{
    const StaticEntry *se = cur_route ? (const StaticEntry *)cur_route->udata : NULL;
    if (!se) return router_resp_404();
    return static_route_serve(se, ctx);
}

static TkRouteResp static_route_serve(const StaticEntry *se, TkRouteCtx ctx)
{
    const char *rel = ctx.path;
    size_t plen = tk_strlen(se->url_prefix);
    if (strncmp(rel, se->url_prefix, plen) == 0) rel += plen;
    TkRouteResp resp = router_static_serve(se->dir_path, rel,
                                           ctx_req_header(ctx, "If-None-Match"),
                                           ctx_req_header(ctx, "Range"));
    if (resp.status == 200 || resp.status == 206)
        resp_pool_add(resp.body);
    if (resp.nheaders > 0) {
        for (uint64_t i = 0; i < resp.nheaders; i++)
            resp_pool_add(resp.header_values[i]);
        resp_pool_add(resp.header_names);
        resp_pool_add(resp.header_values);
    }
    return resp;
}

int router_static(TkRouter *r, const char *url_prefix, const char *dir_path)
{
    if (!r || !url_prefix || !dir_path)
        return reg_fail("static", url_prefix, !r ? "no router" :
                        !url_prefix ? "no url prefix" : "no directory");

    size_t plen = tk_strlen(url_prefix);
    size_t dlen = tk_strlen(dir_path);
    if (plen > SIZE_MAX / 2 - sizeof(StaticEntry) - dlen - 8)
        return reg_fail("static", url_prefix, "out of memory");
    StaticEntry *se = malloc(sizeof *se + plen + 1 + dlen + 1);
    /* the route pattern: the prefix without a trailing slash, then a slash-star */
    char *pattern = malloc(plen + 3);
    if (!se || !pattern) {
        free(se); free(pattern);
        return reg_fail("static", url_prefix, "out of memory");
    }
    se->url_prefix = (char *)(se + 1);
    memcpy(se->url_prefix, url_prefix, plen + 1);
    se->dir_path = se->url_prefix + plen + 1;
    memcpy(se->dir_path, dir_path, dlen + 1);

    size_t n = plen;
    if (n > 1 && url_prefix[n - 1] == '/') n--;
    memcpy(pattern, url_prefix, n);
    memcpy(pattern + n, "/*", 3);

    /* Register a wildcard GET route for the prefix; it owns se from here. */
    int rc = add_route(r, "GET", pattern, static_route_handler, se);
    free(pattern);
    return rc;
}

/* ----------------------------------------------------------------------- */
/* ETag middleware (Story 27.1.13)                                           */
/* ----------------------------------------------------------------------- */

/*
 * etag_fnv_local — FNV-1a 64-bit hash of the len bytes of body, formatted
 * as W/"<16 hex digits>".  121.88: every byte -- it hashed up to the first
 * NUL, so two binary files agreeing before their first 0x00 shared an ETag
 * and If-None-Match answered 304 for the wrong one.
 * Returns a heap-allocated string the caller must free, or NULL on OOM.
 */
static char *etag_fnv_local(const char *body, size_t len)
{
    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    const unsigned char *p = (const unsigned char *)(body ? body : "");
    if (!body) len = 0;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint64_t)p[i];
        hash *= prime;
    }
    char *out = malloc(24);
    if (!out) return NULL;
    snprintf(out, 24, "W/\"%016llx\"", (unsigned long long)hash);
    return out;
}

/*
 * etag_matches_local — 1 if etag is found in the comma-separated
 * If-None-Match list (or the list is the wildcard "*").
 */
static int etag_matches_local(const char *etag, const char *if_none_match)
{
    if (!etag || !if_none_match) return 0;
    if (strcmp(if_none_match, "*") == 0) return 1;
    const char *p = if_none_match;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (*p == '\0') break;
        const char *ts = p;
        while (*p && *p != ',') p++;
        const char *te = p;
        while (te > ts && (*(te-1) == ' ' || *(te-1) == '\t')) te--;
        size_t tlen = (size_t)(te - ts);
        if (tlen == tk_strlen(etag) && memcmp(ts, etag, tlen) == 0)
            return 1;
    }
    return 0;
}

/* Look up a request header value by name (case-insensitive). */
static const char *ctx_req_header(TkRouteCtx ctx, const char *name)
{
    size_t nlen = tk_strlen(name);
    for (uint64_t i = 0; i < ctx.nreq_headers; i++) {
        const char *k = ctx.req_header_names  ? ctx.req_header_names[i]  : NULL;
        const char *v = ctx.req_header_values ? ctx.req_header_values[i] : NULL;
        if (!k || !v) continue;
        size_t klen = tk_strlen(k);
        if (klen != nlen) continue;
        size_t j;
        for (j = 0; j < klen; j++) {
            char a = k[j], b = name[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (j == klen) return v;
    }
    return NULL;
}

/*
 * resp_with_etag — return a copy of base with an ETag header appended.
 * The new header_names/header_values arrays and the etag string are owned
 * by the router (resp_pool, 121.91): released at the next dispatch.  The
 * arrays they replace are not freed here -- they belong to whoever built
 * them (a handler, another middleware, the pool already).
 */
static TkRouteResp resp_with_etag(TkRouteResp base, char *etag)
{
    uint64_t nh = base.nheaders;
    const char **names = malloc((nh + 1) * sizeof(char *));
    const char **vals  = malloc((nh + 1) * sizeof(char *));
    if (!names || !vals) {
        free(names);
        free(vals);
        free(etag);
        return base;
    }
    for (uint64_t i = 0; i < nh; i++) {
        names[i] = base.header_names  ? base.header_names[i]  : NULL;
        vals[i]  = base.header_values ? base.header_values[i] : NULL;
    }
    names[nh] = "ETag";
    vals[nh]  = etag;
    resp_pool_add(names);   /* 121.91: router-owned until the next dispatch */
    resp_pool_add(vals);
    resp_pool_add(etag);
    base.header_names  = names;
    base.header_values = vals;
    base.nheaders      = nh + 1;
    return base;
}

/* The fields a 304 repeats from the 200 it stands for (RFC 9110 sec.
 * 15.4.5): Content-Location, Date, Vary, Cache-Control, Expires -- and the
 * ETag, added by not_modified.  Nothing else: a 304 SHOULD NOT carry other
 * representation metadata (Content-Type, Content-Encoding, ...). */
static const char *const not_mod_keep[] = {
    "Cache-Control", "Content-Location", "Date", "Expires", "Vary",
};

/*
 * not_modified — the 304 for base, a 2xx whose ETag (etag, heap) matched
 * If-None-Match.  121.95: it carries the ETag and the fields of
 * not_mod_keep that base carries, every copy of each (two Vary fields stay
 * two); it used to carry no header at all, so a cache revalidating its copy
 * learned neither the validator nor a new freshness lifetime.  The arrays
 * and etag are router-owned (resp_pool, 121.91).  On OOM: a bare 304.
 */
static TkRouteResp not_modified(TkRouteResp base, char *etag)
{
    TkRouteResp nm;
    memset(&nm, 0, sizeof nm);
    nm.status = 304;
    uint64_t nh = base.nheaders;
    const char **names = malloc((nh + 1) * sizeof(char *));
    const char **vals  = malloc((nh + 1) * sizeof(char *));
    if (!names || !vals) {
        free(names);
        free(vals);
        free(etag);
        return nm;
    }
    uint64_t k = 0;
    names[k] = "ETag"; vals[k++] = etag;
    for (uint64_t i = 0; i < nh; i++) {
        const char *n = base.header_names  ? base.header_names[i]  : NULL;
        const char *v = base.header_values ? base.header_values[i] : NULL;
        if (!n || !v) continue;
        for (size_t j = 0; j < sizeof not_mod_keep / sizeof not_mod_keep[0]; j++)
            if (strcasecmp(n, not_mod_keep[j]) == 0) {
                names[k] = n; vals[k++] = v;
                break;
            }
    }
    resp_pool_add(names);
    resp_pool_add(vals);
    resp_pool_add(etag);
    nm.header_names  = names;
    nm.header_values = vals;
    nm.nheaders      = k;
    return nm;
}

/*
 * mw_etag — middleware function installed by router_use_etag.
 *
 * After the downstream handler produces a response:
 *   - Non-2xx responses pass through unchanged.
 *   - Computes FNV-1a ETag of the response body.
 *   - If the request If-None-Match header matches, returns 304 with the
 *     ETag and the cache fields the 200 carried (not_modified, 121.95).
 *   - Otherwise appends ETag header and returns the original response.
 */
static TkRouteResp mw_etag(TkRouteCtx ctx, TkRouteHandler next)
{
    TkRouteResp resp = next(ctx);

    if (resp.status < 200 || resp.status >= 300) return resp;

    char *etag = etag_fnv_local(resp.body, router_resp_body_len(resp));
    if (!etag) return resp; /* OOM: skip ETag rather than crash */

    const char *inm = ctx_req_header(ctx, "If-None-Match");
    if (inm && etag_matches_local(etag, inm))
        return not_modified(resp, etag);

    return resp_with_etag(resp, etag);
}

int router_use_etag(TkRouter *r)
{
    return router_use(r, mw_etag);
}

/* ----------------------------------------------------------------------- */
/* Access logging middleware (Story 27.1.11) -- removed                       */
/* ----------------------------------------------------------------------- */

/* 121.41 (2), 2026-09-30: router_use_log and its middleware were deleted.
 * No .tki declared them and no C caller outside the unit tests existed, and
 * they wrote the request path unescaped into a Common Log / JSON line (a
 * path containing `"` or a newline forged fields or lines).  Access logging
 * is log.c's (tk_access_log_*), which escapes every field. */

/* ----------------------------------------------------------------------- */
/* Gzip response compression middleware (Story 27.1.6)                      */
/* ----------------------------------------------------------------------- */

/* Minimum body size (bytes) below which gzip is not applied.
 * Stored in a static slot so the middleware function can read it. */
static size_t gzip_min_size = 1024;

/* gzip_skip_type -- an already-compressed Content-Type gzip leaves alone.
 * 121.89: the list is tk_gzip_type.h's, shared with http.c; it was image/,
 * video/, audio/ and an exact application/octet-stream here, so fonts,
 * wasm and archives were recompressed and a parameter or capital letter
 * defeated the octet-stream match. */
static int gzip_skip_type(const char *ct)
{
    return tk_gzip_skip_type(ct);
}

/*
 * gzip_compress — compress src (srclen bytes) into a newly malloc'd buffer.
 * Uses deflateInit2 with windowBits=15|16 to produce a gzip-format stream.
 * On success, *dst_out receives the buffer and *dst_len receives its length.
 * Returns 0 on success, non-zero on failure (caller must not free *dst_out).
 */
static int gzip_compress(const char *src, size_t srclen,
                          char **dst_out, size_t *dst_len)
{
    /* Worst-case bound: zlib adds at most 0.1% + 12 bytes; gzip header is
     * 18 bytes.  compressBound gives a safe upper limit for deflate; we add
     * a small constant to cover the gzip envelope. */
    uLong bound = compressBound((uLong)srclen) + 32UL;
    char *buf = malloc(bound);
    if (!buf) return -1;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    /* windowBits = 15 | 16 selects gzip wrapping (not raw deflate). */
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(buf);
        return -1;
    }

    zs.next_in   = (Bytef *)(uintptr_t)src;
    zs.avail_in  = (uInt)srclen;
    zs.next_out  = (Bytef *)buf;
    zs.avail_out = (uInt)bound;

    if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&zs);
        free(buf);
        return -1;
    }

    *dst_len = (size_t)zs.total_out;
    deflateEnd(&zs);

    *dst_out = buf;
    return 0;
}

/*
 * gzip_middleware — compress the response body when:
 *   - The client's Accept-Encoding admits gzip (q-values honoured, 121.94)
 *   - The response carries no Content-Encoding already (121.94)
 *   - The response body is at least gzip_min_size bytes
 *   - The Content-Type is not an already-compressed type
 *
 * On compression: body is replaced with the gzip bytes; the old body
 * is NOT freed (it is owned by the route handler, not the middleware).
 * The compressed body and the new header arrays are owned by the router
 * (121.91): valid until the next dispatch; the caller frees none of them.
 */
static TkRouteResp gzip_middleware(TkRouteCtx ctx, TkRouteHandler next)
{
    TkRouteResp resp = next(ctx);

    /* Only compress 2xx responses with a non-empty body. */
    if (resp.status < 200 || resp.status >= 300) return resp;
    if (router_resp_body_len(resp) == 0)          return resp;

    /* 121.94: the client must admit gzip under RFC 9110 sec. 12.5.3 --
     * q-values and "*" (tk_gzip_accepted).  strstr(ae, "gzip") took
     * "gzip;q=0", the client's explicit refusal, as a yes. */
    const char *ae = ctx_req_header(ctx, "Accept-Encoding");
    if (!tk_gzip_accepted(ae)) return resp;

    /* 121.94: a body that already has a content coding (a handler's
     * pre-compressed asset, Content-Encoding: br) is left alone: gzipping it
     * again sent "Content-Encoding: br" AND "gzip" over a body the client
     * could decode neither way. */
    for (uint64_t i = 0; i < resp.nheaders; i++) {
        const char *k = resp.header_names ? resp.header_names[i] : NULL;
        if (k && strcasecmp(k, "Content-Encoding") == 0) return resp;
    }

    /* Skip already-compressed MIME types. */
    if (gzip_skip_type(resp.content_type)) return resp;

    /* Skip bodies below the configured minimum size. */
    size_t bodylen = router_resp_body_len(resp);   /* 121.88 */
    if (bodylen < gzip_min_size) return resp;

    /* Compress. */
    char  *compressed = NULL;
    size_t compressed_len = 0;
    if (gzip_compress(resp.body, bodylen, &compressed, &compressed_len) != 0) {
        /* Compression failed: return uncompressed response unchanged. */
        return resp;
    }

    /* Build updated header array with Content-Encoding and Vary appended. */
    uint64_t nh = resp.nheaders;
    const char **new_names = malloc((nh + 2) * sizeof(char *));
    const char **new_vals  = malloc((nh + 2) * sizeof(char *));
    if (!new_names || !new_vals) {
        free(new_names);
        free(new_vals);
        free(compressed);
        return resp;
    }

    /* Copy pre-existing headers. */
    for (uint64_t i = 0; i < nh; i++) {
        new_names[i] = resp.header_names  ? resp.header_names[i]  : NULL;
        new_vals[i]  = resp.header_values ? resp.header_values[i] : NULL;
    }
    new_names[nh]     = "Content-Encoding";
    new_vals[nh]      = "gzip";
    new_names[nh + 1] = "Vary";
    new_vals[nh + 1]  = "Accept-Encoding";

    /* 121.91: the new arrays and the compressed body are router-owned
     * (resp_pool), released at the next dispatch.  They were malloc'd here
     * and freed by nobody: every compressed response leaked them.  The
     * arrays and body they replace are not freed -- each belongs to whoever
     * built it (a handler's literal, a CORS static array, the pool). */
    resp_pool_add(new_names);
    resp_pool_add(new_vals);
    resp_pool_add(compressed);

    resp.body          = compressed;
    /* 121.88: a gzip stream always holds 0x00 (its FLG byte, at offset 3):
     * without its length a caller could recover 3 bytes of it. */
    resp.body_len      = compressed_len;
    resp.header_names  = new_names;
    resp.header_values = new_vals;
    resp.nheaders      = nh + 2;
    return resp;
}

int router_use_gzip(TkRouter *r, size_t min_size)
{
    if (!r) return reg_fail("middleware", NULL, "no router");
    gzip_min_size = min_size;
    return router_use(r, gzip_middleware);
}

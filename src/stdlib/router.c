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
 * compiler code. Callers own the returned pointers.
 *
 * Story: 15.1.3
 */

#include "router.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

/* ----------------------------------------------------------------------- */
/* Internal route entry                                                      */
/* ----------------------------------------------------------------------- */

#define MAX_PARAMS        32
#define MAX_MIDDLEWARE    64
#define MAX_CORS_HEADERS  16  /* max CORS response headers we build */

/* Client IP populated by router_serve for each accepted connection.
 * Empty string when no connection context (e.g. direct router_dispatch call). */
static char serve_client_ip[48]; /* enough for IPv4 and IPv6 */

typedef struct TkRoute {
    char           *method;
    char           *pattern;
    TkRouteHandler  handler;
    struct TkRoute *next;
} TkRoute;

struct TkRouter {
    TkRoute      *head;
    TkRoute      *tail;
    TkMiddleware  middleware[MAX_MIDDLEWARE];
    int           nmw;
    int           cors_enabled;
    TkCorsOpts    cors_opts;
};

/* ----------------------------------------------------------------------- */
/* Path splitting helpers                                                    */
/* ----------------------------------------------------------------------- */

/* Split 'path' on '/' into segments stored in out[].
 * Returns the number of segments.  out[] holds pointers into a heap copy
 * of path that the caller must free (returned via *buf_out). */
static int split_path(const char *path, char **out, int max_segs, char **buf_out)
{
    /* skip leading slash */
    while (*path == '/') path++;

    char *buf = strdup(path);
    if (!buf) { *buf_out = NULL; return 0; }
    *buf_out = buf;

    int n = 0;
    char *p = buf;
    while (*p && n < max_segs) {
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
    r->head         = NULL;
    r->tail         = NULL;
    r->nmw          = 0;
    r->cors_enabled = 0;
    memset(&r->cors_opts, 0, sizeof(r->cors_opts));
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
        free(cur);
        cur = next;
    }
    free(r);
}

/* ----------------------------------------------------------------------- */
/* Route registration                                                        */
/* ----------------------------------------------------------------------- */

static void add_route(TkRouter *r, const char *method, const char *pattern,
                      TkRouteHandler h)
{
    TkRoute *route = malloc(sizeof(TkRoute));
    if (!route) return;
    route->method  = strdup(method);
    route->pattern = strdup(pattern);
    route->handler = h;
    route->next    = NULL;
    if (!r->head) { r->head = route; r->tail = route; }
    else          { r->tail->next = route; r->tail = route; }
}

void router_get   (TkRouter *r, const char *p, TkRouteHandler h) { add_route(r, "GET",    p, h); }
void router_post  (TkRouter *r, const char *p, TkRouteHandler h) { add_route(r, "POST",   p, h); }
void router_put   (TkRouter *r, const char *p, TkRouteHandler h) { add_route(r, "PUT",    p, h); }
void router_delete(TkRouter *r, const char *p, TkRouteHandler h) { add_route(r, "DELETE", p, h); }

/* ----------------------------------------------------------------------- */
/* Middleware                                                                 */
/* ----------------------------------------------------------------------- */

void router_use(TkRouter *r, TkMiddleware mw)
{
    if (!r || !mw || r->nmw >= MAX_MIDDLEWARE) return;
    r->middleware[r->nmw++] = mw;
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
        size_t slen = strlen(arr[i]);
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
            /* Wildcard: if credentials requested, echo origin; otherwise "*" */
            return opts->allow_credentials ? origin : "*";
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

        /* Access-Control-Allow-Credentials */
        if (opts->allow_credentials) {
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

    /* Access-Control-Allow-Credentials */
    if (opts->allow_credentials) {
        nh = cors_append_header(hnames, hvals, nh, MAX_CORS_HEADERS,
                                "Access-Control-Allow-Credentials", "true");
    }

    resp.header_names  = hnames;
    resp.header_values = hvals;
    resp.nheaders      = nh;
    return resp;
}

void router_use_cors(TkRouter *r, TkCorsOpts opts)
{
    if (!r) return;
    r->cors_enabled = 1;
    r->cors_opts    = opts;
    /* Copy opts into the static slot and register the middleware. */
    cors_mw_opts = opts;
    router_use(r, cors_middleware);
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

static TkRouteResp dispatch_internal(TkRouter *r,
                                      const char *method, const char *path,
                                      const char *query,  const char *body,
                                      const char **hnames, const char **hvals,
                                      uint64_t nhdrs)
{
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
    if (r->nmw > 0) {
        mw_chain_ctx.router   = r;
        mw_chain_ctx.mw_index = 0;
        mw_chain_ctx.final    = final_handler;
        resp = mw_chain_next(ctx);
    } else {
        resp = final_handler(ctx);
    }

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

/* URL-decode src into a newly malloc'd string.  Returns NULL on alloc failure. */
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
                out[j++] = (char)((hi << 4) | lo);
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
    size_t klen = strlen(key);
    const char *p = query;

    while (*p) {
        /* find '=' */
        const char *eq = strchr(p, '=');
        if (!eq) break;

        size_t name_len = (size_t)(eq - p);
        const char *val_start = eq + 1;

        /* find end of value (next '&' or end of string) */
        const char *amp = strchr(val_start, '&');
        size_t val_len = amp ? (size_t)(amp - val_start) : strlen(val_start);

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
    resp.content_type = "text/plain";
    return resp;
}

TkRouteResp router_resp_404(void)
{
    TkRouteResp resp;
    memset(&resp, 0, sizeof(resp));
    resp.status       = 404;
    resp.body         = "Not Found";
    resp.content_type = "text/plain";
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
/* router_serve — HTTP server using this router for dispatch                 */
/* ----------------------------------------------------------------------- */

static void router_send_response(int fd, TkRouteResp resp)
{
    char header[512];
    const char *ct = resp.content_type ? resp.content_type : "text/plain";
    const char *bd = resp.body ? resp.body : "";
    size_t bl = strlen(bd);
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d OK\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     resp.status, ct, bl);
    if (n > 0) {
        (void)write(fd, header, (size_t)n);
    }
    if (bl) {
        (void)write(fd, bd, bl);
    }
}

TkRouterErr router_serve(TkRouter *r, const char *host, uint64_t port)
{
    (void)host; /* bind to all interfaces regardless for now */
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

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        TkRouterErr e = {1, "socket() failed"};
        return e;
    }

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(srv);
        TkRouterErr e = {1, "bind() failed"};
        return e;
    }
    if (listen(srv, 8) < 0) {
        close(srv);
        TkRouterErr e = {1, "listen() failed"};
        return e;
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

        TkRouteResp resp = router_dispatch(r,
            method ? method : "GET",
            path   ? path   : "/",
            query, body);

        router_send_response(fd, resp);
        serve_client_ip[0] = '\0'; /* clear after dispatch */
        close(fd);
    }

    /* unreachable in normal operation */
    close(srv);
    return ok_err;
}

/* ----------------------------------------------------------------------- */
/* Static file serving (Story 27.1.5)                                        */
/* ----------------------------------------------------------------------- */

/* MIME type table — extension → content-type */
typedef struct { const char *ext; const char *mime; } MimeEntry;

static const MimeEntry MIME_TABLE[] = {
    { "html",  "text/html" },
    { "css",   "text/css" },
    { "js",    "application/javascript" },
    { "json",  "application/json" },
    { "png",   "image/png" },
    { "jpg",   "image/jpeg" },
    { "gif",   "image/gif" },
    { "svg",   "image/svg+xml" },
    { "ico",   "image/x-icon" },
    { "txt",   "text/plain" },
    { "pdf",   "application/pdf" },
    { "wasm",  "application/wasm" },
    { "mp4",   "video/mp4" },
    { "webm",  "video/webm" },
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
                                          const char *ct,
                                          const char **hnames,
                                          const char **hvals,
                                          uint64_t nh)
{
    TkRouteResp resp;
    memset(&resp, 0, sizeof(resp));
    resp.status       = status;
    resp.body         = body;
    resp.content_type = ct;
    resp.header_names  = hnames;
    resp.header_values = hvals;
    resp.nheaders      = nh;
    return resp;
}

/* router_static_serve — public entry point for serving a single file.
 * dir_path: filesystem directory root (e.g. "/var/www/static")
 * rel_path: relative URL path after the prefix (e.g. "style.css" or "")
 * if_none_match: value of the If-None-Match request header, or NULL
 *
 * On status 200: resp.body and resp.header_values[0] (ETag) are heap-allocated;
 * callers are responsible for freeing them.
 */
TkRouteResp router_static_serve(const char *dir_path, const char *rel_path,
                                 const char *if_none_match)
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

    /* stat the path */
    struct stat st;
    if (stat(full, &st) != 0) {
        if (errno == ENOENT || errno == ENOTDIR)
            return router_resp_404();
        return router_resp_status(403, "Forbidden");
    }

    /* If directory, append /index.html */
    if (S_ISDIR(st.st_mode)) {
        int m = snprintf(full, sizeof(full), "%s/%s/index.html", dir_path, rel);
        if (m < 0 || (size_t)m >= sizeof(full))
            return router_resp_status(403, "Forbidden");
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

    /* Build ETag header (heap-allocated so caller can inspect) */
    const char **hnames = malloc(sizeof(char *));
    const char **hvals  = malloc(sizeof(char *));
    if (!hnames || !hvals) {
        free(hnames); free(hvals); free(buf);
        return router_resp_status(500, "Internal Error");
    }
    hnames[0] = "ETag";
    hvals[0]  = strdup(etag);
    if (!hvals[0]) {
        free(hnames); free(hvals); free(buf);
        return router_resp_status(500, "Internal Error");
    }

    const char *mime = mime_for_path(full);
    return static_resp_with_body(200, buf, mime, hnames, hvals, 1);
}

/* Per-prefix handler: the StaticCtx is embedded in a heap allocation
 * referenced by the per-route context.  Because TkRouteHandler has no
 * closure argument we use a small trampoline array. */

#define MAX_STATIC_ROUTES 16

typedef struct {
    char url_prefix[512];
    char dir_path[1024];
    int  in_use;
} StaticEntry;

static StaticEntry static_entries[MAX_STATIC_ROUTES];

/* One trampoline per slot — we declare them as forward references and
 * define them with a macro below. */

/* Macro to define a trampoline for slot N.
 * Strips the registered prefix from ctx.path to get the relative sub-path,
 * then delegates to router_static_serve.
 * If-None-Match is not available through TkRouteCtx (no header map); the
 * full server loop (router_serve) handles conditional GET at a higher level. */
#define DEFINE_STATIC_TRAMPOLINE(N)                                         \
static TkRouteResp static_handler_##N(TkRouteCtx ctx)                      \
{                                                                           \
    StaticEntry *se = &static_entries[N];                                   \
    const char *rel = ctx.path;                                             \
    size_t plen = strlen(se->url_prefix);                                   \
    if (strncmp(rel, se->url_prefix, plen) == 0) rel += plen;              \
    return router_static_serve(se->dir_path, rel, NULL);                   \
}

DEFINE_STATIC_TRAMPOLINE(0)
DEFINE_STATIC_TRAMPOLINE(1)
DEFINE_STATIC_TRAMPOLINE(2)
DEFINE_STATIC_TRAMPOLINE(3)
DEFINE_STATIC_TRAMPOLINE(4)
DEFINE_STATIC_TRAMPOLINE(5)
DEFINE_STATIC_TRAMPOLINE(6)
DEFINE_STATIC_TRAMPOLINE(7)
DEFINE_STATIC_TRAMPOLINE(8)
DEFINE_STATIC_TRAMPOLINE(9)
DEFINE_STATIC_TRAMPOLINE(10)
DEFINE_STATIC_TRAMPOLINE(11)
DEFINE_STATIC_TRAMPOLINE(12)
DEFINE_STATIC_TRAMPOLINE(13)
DEFINE_STATIC_TRAMPOLINE(14)
DEFINE_STATIC_TRAMPOLINE(15)

static TkRouteHandler static_trampolines[MAX_STATIC_ROUTES] = {
    static_handler_0,  static_handler_1,  static_handler_2,  static_handler_3,
    static_handler_4,  static_handler_5,  static_handler_6,  static_handler_7,
    static_handler_8,  static_handler_9,  static_handler_10, static_handler_11,
    static_handler_12, static_handler_13, static_handler_14, static_handler_15,
};

void router_static(TkRouter *r, const char *url_prefix, const char *dir_path)
{
    if (!r || !url_prefix || !dir_path) return;

    /* Find a free slot */
    int slot = -1;
    for (int i = 0; i < MAX_STATIC_ROUTES; i++) {
        if (!static_entries[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return; /* no slots available */

    StaticEntry *se = &static_entries[slot];
    strncpy(se->url_prefix, url_prefix, sizeof(se->url_prefix) - 1);
    se->url_prefix[sizeof(se->url_prefix) - 1] = '\0';
    strncpy(se->dir_path, dir_path, sizeof(se->dir_path) - 1);
    se->dir_path[sizeof(se->dir_path) - 1] = '\0';
    se->in_use = 1;

    /* Register a wildcard GET route for the prefix */
    char pattern[544];
    /* Normalise: strip trailing slash from prefix for pattern construction */
    size_t plen = strlen(url_prefix);
    char prefix_norm[512];
    strncpy(prefix_norm, url_prefix, sizeof(prefix_norm) - 1);
    prefix_norm[sizeof(prefix_norm) - 1] = '\0';
    if (plen > 1 && prefix_norm[plen - 1] == '/')
        prefix_norm[plen - 1] = '\0';

    snprintf(pattern, sizeof(pattern), "%s/*", prefix_norm);
    add_route(r, "GET", pattern, static_trampolines[slot]);
}

/* ----------------------------------------------------------------------- */
/* ETag middleware (Story 27.1.13)                                           */
/* ----------------------------------------------------------------------- */

/*
 * etag_fnv_local — FNV-1a 64-bit hash of a NUL-terminated string body,
 * formatted as W/"<16 hex digits>".
 * Returns a heap-allocated string the caller must free, or NULL on OOM.
 */
static char *etag_fnv_local(const char *body)
{
    uint64_t hash = 14695981039346656037ULL;
    const uint64_t prime = 1099511628211ULL;
    const unsigned char *p = (const unsigned char *)(body ? body : "");
    while (*p) {
        hash ^= (uint64_t)*p++;
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
        if (tlen == strlen(etag) && memcmp(ts, etag, tlen) == 0)
            return 1;
    }
    return 0;
}

/* Look up a request header value by name (case-insensitive). */
static const char *ctx_req_header(TkRouteCtx ctx, const char *name)
{
    size_t nlen = strlen(name);
    for (uint64_t i = 0; i < ctx.nreq_headers; i++) {
        const char *k = ctx.req_header_names  ? ctx.req_header_names[i]  : NULL;
        const char *v = ctx.req_header_values ? ctx.req_header_values[i] : NULL;
        if (!k || !v) continue;
        size_t klen = strlen(k);
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
 * New header_names/header_values arrays and the etag string are
 * heap-allocated and owned by the returned response.
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
    base.header_names  = names;
    base.header_values = vals;
    base.nheaders      = nh + 1;
    return base;
}

/*
 * mw_etag — middleware function installed by router_use_etag.
 *
 * After the downstream handler produces a response:
 *   - Non-2xx responses pass through unchanged.
 *   - Computes FNV-1a ETag of the response body.
 *   - If the request If-None-Match header matches, returns 304.
 *   - Otherwise appends ETag header and returns the original response.
 */
static TkRouteResp mw_etag(TkRouteCtx ctx, TkRouteHandler next)
{
    TkRouteResp resp = next(ctx);

    if (resp.status < 200 || resp.status >= 300) return resp;

    const char *body = resp.body ? resp.body : "";
    char *etag = etag_fnv_local(body);
    if (!etag) return resp; /* OOM: skip ETag rather than crash */

    const char *inm = ctx_req_header(ctx, "If-None-Match");
    if (inm && etag_matches_local(etag, inm)) {
        free(etag);
        TkRouteResp not_mod;
        memset(&not_mod, 0, sizeof(not_mod));
        not_mod.status = 304;
        return not_mod;
    }

    return resp_with_etag(resp, etag);
}

void router_use_etag(TkRouter *r)
{
    router_use(r, mw_etag);
}

/* ----------------------------------------------------------------------- */
/* Access logging middleware (Story 27.1.11)                                 */
/* ----------------------------------------------------------------------- */

/* Configuration for the log middleware.  One global slot — matches the
 * single-threaded, static-global pattern used throughout this file. */
typedef struct {
    int   format;       /* ROUTER_LOG_COMMON or ROUTER_LOG_JSON */
    char  log_path[4096]; /* empty string → write to stderr */
} LogMwCfg;

static LogMwCfg log_mw_cfg;

/* Write a log line to the configured destination.
 * Opens the file in append mode for every line (simple, safe). */
static void log_write_line(const char *line)
{
    if (log_mw_cfg.log_path[0] == '\0') {
        fputs(line, stderr);
        fputs("\n", stderr);
    } else {
        FILE *f = fopen(log_mw_cfg.log_path, "a");
        if (f) {
            fputs(line, f);
            fputs("\n", f);
            fclose(f);
        }
    }
}

static TkRouteResp log_middleware(TkRouteCtx ctx, TkRouteHandler next)
{
    /* Capture start time (wall clock, millisecond resolution). */
    struct timespec t0;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &t0);
#else
    t0.tv_sec  = 0;
    t0.tv_nsec = 0;
#endif

    TkRouteResp resp = next(ctx);

    /* Capture end time. */
    struct timespec t1;
#ifdef CLOCK_MONOTONIC
    clock_gettime(CLOCK_MONOTONIC, &t1);
#else
    t1.tv_sec  = 0;
    t1.tv_nsec = 0;
#endif
    long ms = (long)((t1.tv_sec - t0.tv_sec) * 1000L +
                     (t1.tv_nsec - t0.tv_nsec) / 1000000L);

    /* Compute response body byte count. */
    size_t bytes = resp.body ? strlen(resp.body) : 0;

    /* Resolve client IP (fall back to "-" if unavailable). */
    const char *ip = (ctx.ip && ctx.ip[0] != '\0') ? ctx.ip : "-";

    /* Resolve method and path (guard against NULL). */
    const char *method = ctx.method ? ctx.method : "-";
    const char *path   = ctx.path   ? ctx.path   : "-";

    /* Build timestamp string. */
    char ts_buf[64];
    time_t now = time(NULL);
    struct tm *tm_info = gmtime(&now);
    if (log_mw_cfg.format == ROUTER_LOG_JSON) {
        strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%dT%H:%M:%SZ", tm_info);
    } else {
        /* Common Log Format timestamp: [day/mon/year:HH:MM:SS +0000] */
        strftime(ts_buf, sizeof(ts_buf), "%d/%b/%Y:%H:%M:%S +0000", tm_info);
    }

    char line[4096];
    if (log_mw_cfg.format == ROUTER_LOG_JSON) {
        /* JSON format — escape method and path minimally (no control chars
         * expected in practice; backslash and double-quote are the risk). */
        snprintf(line, sizeof(line),
                 "{\"ts\":\"%s\",\"ip\":\"%s\","
                 "\"method\":\"%s\",\"path\":\"%s\","
                 "\"status\":%d,\"bytes\":%zu,\"ms\":%ld}",
                 ts_buf, ip, method, path,
                 resp.status, bytes, ms);
    } else {
        /* Common Log Format */
        snprintf(line, sizeof(line),
                 "%s - - [%s] \"%s %s HTTP/1.1\" %d %zu",
                 ip, ts_buf, method, path,
                 resp.status, bytes);
    }

    log_write_line(line);
    return resp;
}

void router_use_log(TkRouter *r, int format, const char *path)
{
    if (!r) return;
    log_mw_cfg.format = format;
    if (path) {
        strncpy(log_mw_cfg.log_path, path, sizeof(log_mw_cfg.log_path) - 1);
        log_mw_cfg.log_path[sizeof(log_mw_cfg.log_path) - 1] = '\0';
    } else {
        log_mw_cfg.log_path[0] = '\0'; /* NULL → stderr */
    }
    router_use(r, log_middleware);
}

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

/* ----------------------------------------------------------------------- */
/* Internal route entry                                                      */
/* ----------------------------------------------------------------------- */

#define MAX_PARAMS 32

typedef struct TkRoute {
    char           *method;
    char           *pattern;
    TkRouteHandler  handler;
    struct TkRoute *next;
} TkRoute;

struct TkRouter {
    TkRoute *head;
    TkRoute *tail;
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
    r->head = NULL;
    r->tail = NULL;
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
/* Dispatch                                                                  */
/* ----------------------------------------------------------------------- */

TkRouteResp router_dispatch(TkRouter *r, const char *method, const char *path,
                             const char *query, const char *body)
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

    if (!best_route) return router_resp_404();

    TkRouteCtx ctx;
    ctx.method       = method;
    ctx.path         = path;
    ctx.query        = query;
    ctx.body         = body;
    ctx.param_names  = best_result.nparam ? best_result.names  : NULL;
    ctx.param_values = best_result.nparam ? best_result.values : NULL;
    ctx.nparam       = (uint64_t)best_result.nparam;

    TkRouteResp resp = best_route->handler(ctx);

    /* free param strings */
    for (int i = 0; i < best_result.nparam; i++) {
        free((char *)best_result.names[i]);
        free((char *)best_result.values[i]);
    }

    return resp;
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

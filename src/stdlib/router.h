#ifndef TK_STDLIB_ROUTER_H
#define TK_STDLIB_ROUTER_H

/*
 * router.h — C interface for the std.router standard library module.
 *
 * Type mappings:
 *   Str     = const char *  (null-terminated UTF-8)
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * Story: 15.1.3
 */

#include <stdint.h>

typedef struct TkRouter TkRouter;

/* Parsed request context */
typedef struct {
    const char  *method;       /* "GET", "POST", etc. */
    const char  *path;         /* URL path */
    const char  *query;        /* raw query string, may be NULL */
    const char  *body;         /* request body, may be NULL */
    const char **param_names;  /* extracted :param names */
    const char **param_values; /* extracted :param values */
    uint64_t     nparam;
} TkRouteCtx;

typedef struct {
    int          status;        /* HTTP status code */
    const char  *body;
    const char  *content_type;
    const char **header_names;
    const char **header_values;
    uint64_t     nheaders;
} TkRouteResp;

/* Handler function pointer */
typedef TkRouteResp (*TkRouteHandler)(TkRouteCtx ctx);

/* Middleware function pointer.
 * Receives the request context and a 'next' handler that invokes the
 * remaining middleware chain (and ultimately the route handler).
 * Returning a response directly short-circuits the chain. */
typedef TkRouteResp (*TkMiddleware)(TkRouteCtx ctx, TkRouteHandler next);

/* Router error — returned by router_serve on failure */
typedef struct {
    int         failed;    /* non-zero when an error occurred */
    const char *msg;
} TkRouterErr;

TkRouter   *router_new(void);
void        router_free(TkRouter *r);
void        router_get(TkRouter *r, const char *pattern, TkRouteHandler h);
void        router_post(TkRouter *r, const char *pattern, TkRouteHandler h);
void        router_put(TkRouter *r, const char *pattern, TkRouteHandler h);
void        router_delete(TkRouter *r, const char *pattern, TkRouteHandler h);

/* Add middleware to the router's chain (called before route handlers) */
void        router_use(TkRouter *r, TkMiddleware mw);

/* Match a request — returns handler result or 404 response */
TkRouteResp router_dispatch(TkRouter *r, const char *method, const char *path,
                             const char *query, const char *body);

/* Start an HTTP server using this router for dispatch.
 * Binds to the given host:port and blocks, serving requests.
 * host may be NULL or "" to bind to all interfaces.
 * Returns TkRouterErr (failed==0 on clean shutdown). */
TkRouterErr router_serve(TkRouter *r, const char *host, uint64_t port);

/* Query string helpers */
const char *router_query_get(const char *query, const char *key); /* URL-decode value for key */

/* router_static — serve files from dir_path under url_prefix.
 * e.g. router_static(r, "/static/", "/var/www/static")
 * Features: MIME detection, index.html for directories,
 * path traversal protection, ETag (mtime+size), conditional GET (304). */
void router_static(TkRouter *r, const char *url_prefix, const char *dir_path);

/* router_static_serve — directly serve a single request for a file under
 * dir_path.  rel_path is the URL path component after stripping the prefix
 * (e.g. "style.css").  if_none_match may be NULL.
 * Returns a TkRouteResp with status 200/304/403/404.
 * When status==200, resp.body is a heap-allocated buffer the caller must free.
 * resp.header_values[0] (the ETag string) is also heap-allocated. */
TkRouteResp router_static_serve(const char *dir_path, const char *rel_path,
                                 const char *if_none_match);

/* Convenience response constructors */
TkRouteResp router_resp_ok(const char *body, const char *ct);
TkRouteResp router_resp_json(const char *json_body);
TkRouteResp router_resp_status(int status, const char *body);
TkRouteResp router_resp_404(void);

#endif /* TK_STDLIB_ROUTER_H */

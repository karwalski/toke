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

#include <stddef.h>
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
    /* Optional request headers (may be NULL/0 if not provided) */
    const char **req_header_names;
    const char **req_header_values;
    uint64_t     nreq_headers;
    const char  *ip;           /* client IP string, or NULL if unavailable */
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

/* CORS configuration (Story 27.1.12).
 * All array fields are NULL-terminated arrays of C strings.
 * Pass allowed_origins={"*",NULL} to allow any origin.
 * Set max_age=-1 to omit Access-Control-Max-Age.
 * Set allow_credentials=1 to send Access-Control-Allow-Credentials: true
 * (incompatible with wildcard origin — per RFC 6454 / Fetch spec). */
typedef struct {
    const char **allowed_origins;  /* NULL-terminated; {"*",NULL} for all  */
    const char **allowed_methods;  /* e.g. {"GET","POST","PUT","DELETE",NULL} */
    const char **allowed_headers;  /* e.g. {"Content-Type","Authorization",NULL} */
    const char **expose_headers;   /* headers to expose to browser, or NULL */
    int          max_age;          /* preflight cache seconds; -1 to omit   */
    int          allow_credentials;/* 1 = send Allow-Credentials: true      */
} TkCorsOpts;

/* Register CORS middleware on the router.
 * opts is copied by value; the caller's string arrays must remain valid
 * for the lifetime of the router. */
void router_use_cors(TkRouter *r, TkCorsOpts opts);

/* Match a request — returns handler result or 404 response */
TkRouteResp router_dispatch(TkRouter *r, const char *method, const char *path,
                             const char *query, const char *body);

/* Extended dispatch — also accepts request headers (NULL-terminated parallel
 * arrays of length nhdrs).  Use this when CORS middleware needs the Origin
 * header.  Either hnames or hvals may be NULL (treated as nhdrs==0). */
TkRouteResp router_dispatch_ex(TkRouter *r,
                                const char *method, const char *path,
                                const char *query,  const char *body,
                                const char **hnames, const char **hvals,
                                uint64_t nhdrs);

/* Start an HTTP server using this router for dispatch.
 * Binds to the given host:port and blocks, serving requests.
 * host may be NULL or "" to bind to all interfaces.
 * Returns TkRouterErr (failed==0 on clean shutdown). */
TkRouterErr router_serve(TkRouter *r, const char *host, uint64_t port);

/* router_handle_fd — process one HTTP or WebSocket request/response cycle
 * on an already-connected socket fd.  The socket is closed before returning.
 * Intended for use in tests via socketpair(2). */
void router_handle_fd(TkRouter *r, int fd);

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

/* ── ETag middleware (Story 27.1.13) ────────────────────────────────── */

/* router_use_etag — middleware that auto-generates ETags for all responses.
 * Computes FNV-1a of response body, sets ETag header.
 * If request has If-None-Match matching the ETag, returns 304. */
void router_use_etag(TkRouter *r);

/* Convenience response constructors */
TkRouteResp router_resp_ok(const char *body, const char *ct);
TkRouteResp router_resp_json(const char *json_body);
TkRouteResp router_resp_status(int status, const char *body);
TkRouteResp router_resp_404(void);

/* ── WebSocket upgrade (Story 27.1.15) ──────────────────────────────── */

typedef void (*TkWsOnOpen)(int fd);
typedef void (*TkWsOnMessage)(int fd, const char *data, size_t len, int is_binary);
typedef void (*TkWsOnClose)(int fd);

/* router_ws — register a WebSocket endpoint.
 * When a GET request arrives on pattern with Upgrade: websocket,
 * performs the HTTP→WS handshake and invokes the callbacks. */
void router_ws(TkRouter *r, const char *pattern,
               TkWsOnOpen on_open, TkWsOnMessage on_message, TkWsOnClose on_close);

/* ── Gzip response compression middleware (Story 27.1.6) ───────────── */

/* router_use_gzip — add gzip compression middleware.
 * Compresses responses >= min_size bytes when client sends
 * Accept-Encoding: gzip.  Skips already-compressed MIME types
 * (image, video, audio, application/octet-stream subtypes).
 * Sets Content-Encoding: gzip and Vary: Accept-Encoding on
 * compressed responses. */
void router_use_gzip(TkRouter *r, size_t min_size);

/* ── Access logging middleware (Story 27.1.11) ──────────────────────── */

#define ROUTER_LOG_COMMON 0  /* Common Log Format */
#define ROUTER_LOG_JSON   1  /* JSON format        */

/* router_use_log — add access logging middleware.
 * format: ROUTER_LOG_COMMON or ROUTER_LOG_JSON
 * path:   file path for log output, or NULL for stderr
 * Log lines are written AFTER the response is produced.
 * Common Log Format:
 *   {ip} - - [{timestamp}] "{method} {path} HTTP/1.1" {status} {bytes}
 * JSON format:
 *   {"ts":"...","ip":"...","method":"...","path":"...","status":N,"bytes":N,"ms":N} */
void router_use_log(TkRouter *r, int format, const char *path);

#endif /* TK_STDLIB_ROUTER_H */

#ifndef TK_STDLIB_HTTP_H
#define TK_STDLIB_HTTP_H

/*
 * http.h — C interface for the std.http standard library module.
 *
 * Type mappings (spec Section 17.2):
 *   [[Str]]      = StrPairArray   Req = struct Req   Res = struct Res
 *   Str!HttpErr  = HttpResult
 *
 * Story: 1.3.2  Branch: feature/stdlib-http
 */

#include <stddef.h>
#include <stdint.h>

typedef struct { const char *key; const char *val; } StrPair;
typedef struct { StrPair *data; uint64_t len; }       StrPairArray;

typedef struct {
    const char  *method;
    const char  *path;
    const char  *body;
    StrPairArray headers;
    StrPairArray params;
} Req;

typedef struct {
    uint16_t     status;
    const char  *body;
    StrPairArray headers;
} Res;

typedef enum {
    HTTP_ERR_BAD_REQUEST,
    HTTP_ERR_NOT_FOUND,
    HTTP_ERR_INTERNAL,
    HTTP_ERR_TIMEOUT,
} HttpErrKind;

typedef struct {
    HttpErrKind  kind;
    const char  *msg;
    uint32_t     timeout_ms;
} HttpErr;

typedef struct {
    const char *ok;
    int         is_err;
    HttpErr     err;
} HttpResult;

typedef Res (*RouteHandler)(Req req);

/* Route registration */
void http_GET   (const char *pattern, RouteHandler h);
void http_POST  (const char *pattern, RouteHandler h);
void http_PUT   (const char *pattern, RouteHandler h);
void http_DELETE(const char *pattern, RouteHandler h);
void http_PATCH (const char *pattern, RouteHandler h);

/* Response constructors */
Res http_Res_ok  (const char *body);
Res http_Res_json(uint16_t status, const char *body);
Res http_Res_bad (const char *msg);
Res http_Res_err (const char *msg);

/* Accessors */
HttpResult http_param (Req req, const char *name);
HttpResult http_header(Req req, const char *name);

/* Route count (for testing) */
int http_route_count(void);

/* ── URL-encoded form body parsing (Story 27.1.14) ──────────────────── */

typedef struct {
    const char *key;
    const char *value; /* URL-decoded */
} TkFormField;

typedef struct {
    TkFormField *fields;
    size_t       nfields;
} TkFormResult;

/* http_form_parse — parse application/x-www-form-urlencoded body.
 * body is the raw request body string.
 * Returns TkFormResult; caller must call http_form_free(). */
TkFormResult http_form_parse(const char *body);

/* http_form_get — look up a field by key. Returns value or NULL. */
const char  *http_form_get(TkFormResult form, const char *key);

/* http_form_free — release all resources. */
void         http_form_free(TkFormResult form);

/* ── Multipart/form-data parsing (Story 27.1.8) ─────────────────────── */

typedef struct {
    const char *name;         /* field name from Content-Disposition */
    const char *filename;     /* NULL if not a file upload */
    const char *content_type; /* NULL if not specified */
    const char *data;         /* field data (may contain \0) */
    size_t      data_len;
} TkMultipart;

typedef struct {
    TkMultipart *parts;
    size_t       nparts;
} TkMultipartResult;

/* http_multipart_parse — parse a multipart/form-data body.
 * boundary is the boundary string from Content-Type header.
 * body/body_len is the raw request body.
 * Returns a TkMultipartResult; caller must call http_multipart_free(). */
TkMultipartResult http_multipart_parse(const char *boundary,
                                        const char *body, size_t body_len);

/* http_multipart_free — release all resources. */
void http_multipart_free(TkMultipartResult r);

/* http_multipart_boundary — extract boundary from Content-Type header value.
 * e.g. "multipart/form-data; boundary=----WebKitFormBoundary..."
 * Returns heap-allocated boundary string or NULL. */
const char *http_multipart_boundary(const char *content_type);

/* ── Cookie support (Story 27.1.7) ─────────────────────────────────── */

/* http_cookie — extract cookie value for 'name' from Cookie: header.
 * Returns heap-allocated string (caller owns) or NULL if not found. */
const char *http_cookie(const char *cookie_header, const char *name);

typedef struct {
    const char *path;       /* e.g. "/" — NULL means omit */
    const char *domain;     /* NULL means omit */
    int64_t     max_age;    /* -1 means omit */
    int         secure;     /* 0 = omit, 1 = add Secure */
    int         http_only;  /* 0 = omit, 1 = add HttpOnly */
    const char *same_site;  /* "Strict", "Lax", "None" or NULL */
} TkCookieOpts;

/* http_set_cookie_header — build a Set-Cookie header value string.
 * Returns heap-allocated string (caller owns). */
const char *http_set_cookie_header(const char *name, const char *value,
                                    TkCookieOpts opts);

/* ── Chunked transfer encoding (Story 27.1.4) ────────────────────────── */

/* http_chunked_write — write body as chunked encoding to fd.
 * data/len is split into chunks of at most chunk_size bytes.
 * Returns 0 on success, -1 on write error. */
int http_chunked_write(int fd, const char *data, size_t len, size_t chunk_size);

/* http_chunked_read — read a chunked body from fd into a malloc'd buffer.
 * Returns heap-allocated buffer (caller owns) and sets *out_len.
 * Returns NULL on error. */
char *http_chunked_read(int fd, size_t *out_len);

/* Keep-alive defaults (Story 27.1.3) */
#define HTTP_KEEPALIVE_IDLE_TIMEOUT_S 30
#define HTTP_KEEPALIVE_MAX_REQUESTS   1000

/* ── Server request-size limits (Story 27.1.9) ───────────────────────── */

/* Default limits applied to every incoming server request. */
#define HTTP_DEFAULT_MAX_HEADER_SIZE  8192U      /* 8 KiB  */
#define HTTP_DEFAULT_MAX_BODY_SIZE    1048576U   /* 1 MiB  */
#define HTTP_DEFAULT_TIMEOUT_SECS     10U

/*
 * Override the server request-size limits before calling http_serve().
 *
 *   max_header   – maximum total header bytes before \r\n\r\n is found.
 *                  0 means keep the current value.
 *   max_body     – maximum Content-Length accepted.
 *                  0 means keep the current value.
 *   timeout_secs – SO_RCVTIMEO applied to each accepted socket.
 *                  0 means keep the current value.
 */
void http_set_limits(uint32_t max_header, uint32_t max_body,
                     uint32_t timeout_secs);

/* http_shutdown — signal the server to stop accepting new connections.
 * In-flight requests complete normally. */
void http_shutdown(void);

#define HTTP_DRAIN_TIMEOUT_SECS 10

/* ── ETag generation and conditional requests (Story 27.1.13) ────────── */

/* http_etag_fnv — compute weak ETag (FNV-1a) of body, return as W/"hex" string.
 * Returns heap-allocated string; caller owns. */
const char *http_etag_fnv(const char *body, size_t len);

/* http_etag_matches — check if ETag matches If-None-Match header value.
 * Returns 1 if they match (should return 304), 0 otherwise. */
int http_etag_matches(const char *etag, const char *if_none_match);

/* Server — blocks; dispatches to registered routes */
int http_serve(uint16_t port);

/*
 * http_handle_fd — process one HTTP request/response cycle on an already-
 * connected socket fd, applying the current srv_limits.  The socket is
 * closed before returning.  Intended for use in tests via socketpair(2).
 */
void http_handle_fd(int fd);

/* ── Pre-fork worker pool (Story 27.1.1) ────────────────────────────── */

/* TkHttpRouter — opaque handle passed to http_serve_workers.
 * Internally wraps a snapshot of the global route table taken at
 * http_router_new() call time. */
typedef struct TkHttpRouter TkHttpRouter;

/* TkHttpErr — server-level error codes returned by http_serve_workers. */
typedef enum {
    TK_HTTP_OK         = 0,
    TK_HTTP_ERR_BIND   = 1,
    TK_HTTP_ERR_LISTEN = 2,
    TK_HTTP_ERR_FORK   = 3,
    TK_HTTP_ERR_SOCKET = 4
} TkHttpErr;

/* http_router_new — capture the current global route table into a
 * TkHttpRouter.  The caller owns the returned pointer; free with
 * http_router_free(). */
TkHttpRouter *http_router_new(void);

/* http_router_free — release a TkHttpRouter. */
void http_router_free(TkHttpRouter *r);

/* http_serve_workers — start HTTP server with pre-fork worker pool.
 * nworkers=1 is equivalent to http_serve (single process).
 * host may be NULL for all interfaces. */
TkHttpErr http_serve_workers(TkHttpRouter *r, const char *host,
                              uint64_t port, uint64_t nworkers);

/* ── TLS/HTTPS support (Story 27.1.2) ───────────────────────────────── */

typedef struct TkTlsCtx TkTlsCtx;

/* TLS configuration (Epic 61.2) */
typedef struct {
    const char *cert_path;       /* PEM certificate file (required) */
    const char *key_path;        /* PEM private key file (required) */
    const char *min_version;     /* "1.2" or "1.3" (default: "1.2") */
    const char *ciphers;         /* OpenSSL cipher string (NULL=default) */
    const char *curves;          /* e.g. "X25519:P-256" (NULL=default) */
    int         session_tickets; /* 1=enable, 0=disable (default: 1) */
    uint64_t    ticket_lifetime; /* seconds (default: 7200) */
} TkTlsConfig;

/* SNI virtual host entry (Story 61.2.4) */
typedef struct {
    const char *hostname;   /* server name (e.g. "example.com") */
    const char *cert_path;  /* PEM certificate */
    const char *key_path;   /* PEM private key */
} TkTlsSniEntry;

/* http_tls_ctx_new — load cert and key PEM files, return TLS context.
 * Returns NULL on error (prints reason to stderr). */
TkTlsCtx *http_tls_ctx_new(const char *cert_path, const char *key_path);

/* http_tls_ctx_new_config — create TLS context with advanced configuration.
 * Supports cipher suite selection, TLS version, session tickets. */
TkTlsCtx *http_tls_ctx_new_config(const TkTlsConfig *config);

/* http_tls_add_sni — add SNI-based virtual host cert/key pair.
 * Hostname matching is case-insensitive. Returns 0 on success. */
int http_tls_add_sni(TkTlsCtx *ctx, const char *hostname,
                     const char *cert_path, const char *key_path);

/* http_tls_set_ocsp_response — set cached OCSP response for stapling.
 * Data is copied. Call periodically to refresh. */
int http_tls_set_ocsp_response(TkTlsCtx *ctx,
                                const uint8_t *data, size_t len);

/* http_tls_reload_cert — hot-swap cert+key in a running TLS context.
 * Returns 0 on success. Used by ACME auto-renewal. */
int http_tls_reload_cert(TkTlsCtx *ctx, const char *cert_path,
                          const char *key_path);

/* http_tls_ctx_free — release a TkTlsCtx. */
void       http_tls_ctx_free(TkTlsCtx *ctx);

/* http_serve_tls — start HTTPS server (TLS termination).
 * Same as http_serve_workers but with TLS wrapping each connection.
 * host may be NULL for all interfaces. */
TkHttpErr http_serve_tls(TkHttpRouter *r, const char *host,
                          uint64_t port, TkTlsCtx *tls);

/* http_serve_tls_workers — TLS + pre-fork worker pool.
 * nworkers=1 is single-process. */
TkHttpErr http_serve_tls_workers(TkHttpRouter *r, const char *host,
                                  uint64_t port, TkTlsCtx *tls,
                                  uint64_t nworkers);

/* ── Client types (Story 35.1.2) ──────────────────────────────────── */

typedef struct {
    char    *base_url;
    uint64_t pool_size;
    uint64_t timeout_ms;
    char    *proxy_url;     /* NULL = direct connect (Story 42.1.3) */
} HttpClient;

typedef struct {
    const char  *method;
    const char  *url;
    StrPairArray headers;
    uint8_t     *body;
    uint64_t     body_len;
} HttpClientReq;

typedef struct {
    uint64_t     status;
    StrPairArray headers;
    uint8_t     *body;
    uint64_t     body_len;
    int          is_err;
    char        *err_msg;
    uint64_t     err_code;
} HttpClientResp;

typedef struct {
    uint64_t id;
    int      open;
    int      fd;         /* internal: socket file descriptor */
    char    *buf;        /* internal: leftover read buffer   */
    uint64_t buf_len;
    uint64_t buf_cap;
} HttpStream;

typedef struct {
    HttpStream stream;
    int        is_err;
    char      *err_msg;
    uint64_t   err_code;
} HttpStreamResult;

typedef struct {
    uint8_t *data;
    uint64_t len;
    int      is_err;
    char    *err_msg;
    uint64_t err_code;
} HttpChunkResult;

/* Client constructor */
HttpClient *http_client(const char *base_url);

/* Client destructor */
void http_client_free(HttpClient *c);

/* http_withproxy — return a new HttpClient with proxy_url set (Story 42.1.3).
 * Does NOT mutate the original. */
HttpClient *http_withproxy(HttpClient *c, const char *proxy_url);

/* HTTP methods */
HttpClientResp http_get   (HttpClient *c, const char *path);
HttpClientResp http_post  (HttpClient *c, const char *path,
                           const uint8_t *body, uint64_t body_len,
                           const char *content_type);
HttpClientResp http_put   (HttpClient *c, const char *path,
                           const uint8_t *body, uint64_t body_len,
                           const char *content_type);
HttpClientResp http_delete(HttpClient *c, const char *path);

/* Streaming */
HttpStreamResult http_stream    (HttpClient *c, HttpClientReq req);
HttpChunkResult  http_streamnext(HttpStream *s);

/* ── File download (Story 42.1.4) ────────────────────────────────────── */

typedef void (*HttpProgressFn)(uint64_t bytes_done, uint64_t bytes_total);

/* http_downloadfile — download URL to dest_path via streaming GET.
 * Writes to dest_path.tmp then renames on success.
 * progress_fn may be NULL; called after each 8 KiB chunk.
 * Returns 0 on success, -1 on error. */
int http_downloadfile(HttpClient *c, const char *url,
                      const char *dest_path, HttpProgressFn progress_fn);

#endif /* TK_STDLIB_HTTP_H */

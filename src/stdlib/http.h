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

/* Server — blocks; dispatches to registered routes */
int http_serve(uint16_t port);

/* ── Client types (Story 35.1.2) ──────────────────────────────────── */

typedef struct {
    char    *base_url;
    uint64_t pool_size;
    uint64_t timeout_ms;
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

#endif /* TK_STDLIB_HTTP_H */

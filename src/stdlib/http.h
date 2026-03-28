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

#endif /* TK_STDLIB_HTTP_H */

#ifndef TK_STDLIB_JSON_H
#define TK_STDLIB_JSON_H

/*
 * json.h — C interface for the std.json standard library module.
 *
 * Type mappings:
 *   Json           = struct { const char *raw; }
 *   JsonErr        = struct { JsonErrKind kind; const char *msg; }
 *   Json!JsonErr   = JsonResult
 *   Str!JsonErr    = StrJsonResult
 *   u64!JsonErr    = U64JsonResult
 *   i64!JsonErr    = I64JsonResult
 *   f64!JsonErr    = F64JsonResult
 *   bool!JsonErr   = BoolJsonResult
 *   [Json]!JsonErr = JsonArrayResult
 *
 * Story: 1.3.4  Branch: feature/stdlib-json
 */

#include <stdint.h>

typedef struct { const char *raw; } Json;
typedef struct { Json *data; uint64_t len; } JsonArray;

typedef enum {
    JSON_ERR_PARSE   = 0,
    JSON_ERR_TYPE    = 1,
    JSON_ERR_MISSING = 2
} JsonErrKind;

typedef struct { JsonErrKind kind; const char *msg; } JsonErr;

typedef struct { Json      ok; int is_err; JsonErr err; } JsonResult;
typedef struct { const char *ok; int is_err; JsonErr err; } StrJsonResult;
typedef struct { uint64_t  ok; int is_err; JsonErr err; } U64JsonResult;
typedef struct { int64_t   ok; int is_err; JsonErr err; } I64JsonResult;
typedef struct { double    ok; int is_err; JsonErr err; } F64JsonResult;
typedef struct { int       ok; int is_err; JsonErr err; } BoolJsonResult;
typedef struct { JsonArray ok; int is_err; JsonErr err; } JsonArrayResult;

const char    *json_enc(const char *v);
JsonResult     json_dec(const char *s);
StrJsonResult  json_str(Json j, const char *key);
U64JsonResult  json_u64(Json j, const char *key);
I64JsonResult  json_i64(Json j, const char *key);
F64JsonResult  json_f64(Json j, const char *key);
BoolJsonResult json_bool(Json j, const char *key);
JsonArrayResult json_arr(Json j, const char *key);

#endif /* TK_STDLIB_JSON_H */

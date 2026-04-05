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
typedef struct { const char **data; uint64_t len; } StrArray;

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
typedef struct { StrArray  ok; int is_err; JsonErr err; } StrArrayJsonResult;

const char    *json_enc(const char *v);
JsonResult     json_dec(const char *s);
StrJsonResult  json_str(Json j, const char *key);
U64JsonResult  json_u64(Json j, const char *key);
I64JsonResult  json_i64(Json j, const char *key);
F64JsonResult  json_f64(Json j, const char *key);
BoolJsonResult json_bool(Json j, const char *key);
JsonArrayResult json_arr(Json j, const char *key);

/* ------------------------------------------------------------------ */
/* Object inspection and manipulation — Story 29.1.1                  */
/* ------------------------------------------------------------------ */

/* json_keys — return heap-allocated StrArray of top-level key names.
 * Caller owns the StrArray and each string inside it.
 * Returns is_err=1 with JSON_ERR_TYPE if j is not an object. */
StrArrayJsonResult json_keys(Json j);

/* json_has — return 1 if key exists in top-level object, 0 otherwise. */
int json_has(Json j, const char *key);

/* json_len — number of elements in an array, or number of keys in an object.
 * Returns is_err=1 with JSON_ERR_TYPE for any other type. */
U64JsonResult json_len(Json j);

/* json_type — return a string literal describing j's type:
 * "null", "object", "array", "string", "number", or "bool". */
StrJsonResult json_type(Json j);

/* json_pretty — return a malloc'd string with 2-space-indented JSON.
 * Caller owns the returned string. */
StrJsonResult json_pretty(Json j);

/* json_is_null — return 1 if j[key] is null or the key is missing, 0 otherwise. */
int json_is_null(Json j, const char *key);

/* ------------------------------------------------------------------ */
/* Streaming API — Story 35.1.3                                        */
/* ------------------------------------------------------------------ */

/*
 * JsonToken — SAX-style event/token from the streaming parser.
 *
 * Maps to .tki sum type JsonToken with variants:
 *   ObjectStart, ObjectEnd, ArrayStart, ArrayEnd,
 *   Key(str), Str(str), U64(u64), I64(i64), F64(f64),
 *   Bool(bool), Null, End
 */
typedef enum {
    JSON_TOK_OBJECT_START = 0,
    JSON_TOK_OBJECT_END   = 1,
    JSON_TOK_ARRAY_START  = 2,
    JSON_TOK_ARRAY_END    = 3,
    JSON_TOK_KEY          = 4,
    JSON_TOK_STR          = 5,
    JSON_TOK_U64          = 6,
    JSON_TOK_I64          = 7,
    JSON_TOK_F64          = 8,
    JSON_TOK_BOOL         = 9,
    JSON_TOK_NULL         = 10,
    JSON_TOK_END          = 11
} JsonTokenKind;

typedef struct {
    JsonTokenKind kind;
    union {
        const char *str;   /* Key, Str */
        uint64_t    u64;   /* U64 */
        int64_t     i64;   /* I64 */
        double      f64;   /* F64 */
        int         b;     /* Bool */
    } val;
} JsonToken;

/*
 * JsonStreamErr — error from streaming operations.
 *
 * Maps to .tki sum type JsonStreamErr with variants:
 *   Truncated(str), Invalid(str), Overflow(str)
 */
typedef enum {
    JSON_STREAM_ERR_TRUNCATED = 0,
    JSON_STREAM_ERR_INVALID   = 1,
    JSON_STREAM_ERR_OVERFLOW  = 2
} JsonStreamErrKind;

typedef struct {
    JsonStreamErrKind kind;
    const char       *msg;
} JsonStreamErr;

typedef struct {
    JsonToken ok;
    int       is_err;
    JsonStreamErr err;
} JsonTokenResult;

typedef struct {
    int       is_err;
    JsonStreamErr err;
} JsonStreamVoidResult;

/*
 * JsonStream — opaque SAX-style streaming parser state.
 *
 * Created with json_streamparser(), advanced with json_streamnext().
 */
#define JSON_STREAM_MAX_DEPTH 256

typedef enum {
    JSON_STATE_VALUE = 0,       /* expecting a value */
    JSON_STATE_OBJ_KEY_OR_END,  /* expecting key or '}' */
    JSON_STATE_OBJ_COLON,       /* expecting ':' */
    JSON_STATE_OBJ_VALUE,       /* expecting value after ':' */
    JSON_STATE_OBJ_COMMA,       /* expecting ',' or '}' */
    JSON_STATE_ARR_VALUE_OR_END,/* expecting value or ']' */
    JSON_STATE_ARR_COMMA,       /* expecting ',' or ']' */
    JSON_STATE_DONE             /* top-level value consumed */
} JsonStreamState;

typedef struct {
    const uint8_t  *buf;
    uint64_t        len;
    uint64_t        pos;
    uint64_t        depth;
    /* State stack: each entry records state to return to after value */
    JsonStreamState stack[JSON_STREAM_MAX_DEPTH];
} JsonStream;

/*
 * JsonWriter — streaming JSON writer that accumulates output.
 *
 * Created with json_newwriter(), emit with json_streamemit(),
 * retrieve bytes with json_writerbytes().
 */
typedef struct {
    uint8_t  *buf;
    uint64_t  cap;
    uint64_t  pos;
} JsonWriter;

typedef struct {
    uint8_t  *data;
    uint64_t  len;
} JsonBytes;

/* Streaming parser */
JsonStream         json_streamparser(const uint8_t *buf, uint64_t len);
JsonTokenResult    json_streamnext(JsonStream *s);

/* Streaming writer */
JsonWriter         json_newwriter(uint64_t initial_cap);
JsonStreamVoidResult json_streamemit(JsonWriter *w, Json j);
JsonBytes          json_writerbytes(const JsonWriter *w);

#endif /* TK_STDLIB_JSON_H */

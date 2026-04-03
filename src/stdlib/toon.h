#ifndef TK_STDLIB_TOON_H
#define TK_STDLIB_TOON_H

/*
 * toon.h — C interface for the std.toon standard library module.
 *
 * TOON (Token-Oriented Object Notation) parser/emitter for toke.
 * Optimised for uniform tabular data — 30-60% fewer tokens than JSON.
 *
 * TOON format:
 *   Schema declaration:  name[count]{field1,field2,...}:
 *   Value rows:          val1|val2|...
 *
 * Type mappings:
 *   Toon           = struct { const char *raw; }
 *   ToonErr        = struct { ToonErrKind kind; const char *msg; }
 *   Toon!ToonErr   = ToonResult
 *
 * Story: 6.3.2
 */

#include <stdint.h>

typedef struct { const char *raw; } Toon;
typedef struct { Toon *data; uint64_t len; } ToonArray;

typedef enum {
    TOON_ERR_PARSE   = 0,
    TOON_ERR_TYPE    = 1,
    TOON_ERR_MISSING = 2
} ToonErrKind;

typedef struct { ToonErrKind kind; const char *msg; } ToonErr;

typedef struct { Toon       ok; int is_err; ToonErr err; } ToonResult;
typedef struct { const char *ok; int is_err; ToonErr err; } StrToonResult;
typedef struct { int64_t    ok; int is_err; ToonErr err; } I64ToonResult;
typedef struct { double     ok; int is_err; ToonErr err; } F64ToonResult;
typedef struct { int        ok; int is_err; ToonErr err; } BoolToonResult;
typedef struct { ToonArray  ok; int is_err; ToonErr err; } ToonArrayResult;

/* Core API */
const char     *toon_enc(const char *v);
ToonResult      toon_dec(const char *s);
StrToonResult   toon_str(Toon t, const char *key);
I64ToonResult   toon_i64(Toon t, const char *key);
F64ToonResult   toon_f64(Toon t, const char *key);
BoolToonResult  toon_bool(Toon t, const char *key);
ToonArrayResult toon_arr(Toon t, const char *key);

/* Conversion */
const char *toon_from_json(const char *json);
const char *toon_to_json(const char *toon);

#endif /* TK_STDLIB_TOON_H */

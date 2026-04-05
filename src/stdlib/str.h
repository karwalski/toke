#ifndef TK_STDLIB_STR_H
#define TK_STDLIB_STR_H

/*
 * str.h — C interface for the std.str standard library module.
 *
 * Type mappings:
 *   Str        = const char *   (null-terminated UTF-8)
 *   u64        = uint64_t
 *   i64        = int64_t
 *   f64        = double
 *   bool       = int            (0 = false, 1 = true)
 *   [Str]      = StrArray
 *   [Byte]     = ByteArray
 *   Str!SliceErr    = StrSliceResult
 *   i64!ParseErr    = IntParseResult
 *   f64!ParseErr    = FloatParseResult
 *   Str!EncodingErr = StrEncResult
 *
 * Story: 1.3.1  Branch: feature/stdlib-str
 */

#include <stdint.h>

#ifndef TK_STRARRAY_DEFINED
#define TK_STRARRAY_DEFINED
typedef struct { const char **data; uint64_t len; } StrArray;
#endif
typedef struct { const uint8_t *data; uint64_t len; } ByteArray;

typedef struct { const char *ok; int is_err; const char *err_msg; } StrSliceResult;
typedef struct { int64_t ok;     int is_err; const char *err_msg; } IntParseResult;
typedef struct { double   ok;    int is_err; const char *err_msg; } FloatParseResult;
typedef struct { const char *ok; int is_err; const char *err_msg; } StrEncResult;

uint64_t        str_len(const char *s);
const char     *str_concat(const char *a, const char *b);
StrSliceResult  str_slice(const char *s, uint64_t start, uint64_t end);
const char     *str_from_int(int64_t n);
const char     *str_from_float(double n);
IntParseResult  str_to_int(const char *s);
FloatParseResult str_to_float(const char *s);
int             str_contains(const char *s, const char *sub);
StrArray        str_split(const char *s, const char *sep);
const char     *str_trim(const char *s);
const char     *str_upper(const char *s);
const char     *str_lower(const char *s);
ByteArray       str_bytes(const char *s);
StrEncResult    str_from_bytes(ByteArray b);

#endif /* TK_STDLIB_STR_H */

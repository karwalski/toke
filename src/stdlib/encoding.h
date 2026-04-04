#ifndef TK_STDLIB_ENCODING_H
#define TK_STDLIB_ENCODING_H

/*
 * encoding.h — C interface for the std.encoding standard library module.
 *
 * Type mappings:
 *   [Byte]  = ByteArray  (defined in str.h)
 *   Str     = const char *  (null-terminated UTF-8)
 *
 * On invalid input, decode functions return a ByteArray with .len == 0
 * and .data == NULL rather than aborting.
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * Story: 14.1.1
 */

#include "str.h"

/* encoding.b64encode(data:[Byte]):Str
 * Returns a heap-allocated standard base64 string (RFC 4648, with padding).
 * Caller owns the returned pointer. */
const char *encoding_b64encode(ByteArray data);

/* encoding.b64decode(s:Str):[Byte]
 * Decodes standard base64. Returns ByteArray with .len==0 on invalid input.
 * Caller owns the returned .data pointer. */
ByteArray encoding_b64decode(const char *s);

/* encoding.b64urlencode(data:[Byte]):Str
 * Returns a heap-allocated URL-safe base64 string (RFC 4648 §5, no padding).
 * Uses '-' and '_' instead of '+' and '/'.
 * Caller owns the returned pointer. */
const char *encoding_b64urlencode(ByteArray data);

/* encoding.b64urldecode(s:Str):[Byte]
 * Decodes URL-safe base64 (accepts with or without padding).
 * Returns ByteArray with .len==0 on invalid input.
 * Caller owns the returned .data pointer. */
ByteArray encoding_b64urldecode(const char *s);

/* encoding.hexencode(data:[Byte]):Str
 * Returns a heap-allocated lowercase hex string (2 chars per byte + NUL).
 * Caller owns the returned pointer. */
const char *encoding_hexencode(ByteArray data);

/* encoding.hexdecode(s:Str):[Byte]
 * Decodes a lowercase or uppercase hex string. Returns ByteArray with
 * .len==0 on odd-length input or non-hex characters.
 * Caller owns the returned .data pointer. */
ByteArray encoding_hexdecode(const char *s);

/* encoding.urlencode(s:Str):Str
 * Percent-encodes all characters except unreserved (ALPHA / DIGIT / - . _ ~).
 * Caller owns the returned pointer. */
const char *encoding_urlencode(const char *s);

/* encoding.urldecode(s:Str):Str
 * Decodes percent-encoded sequences; '+' is decoded as space.
 * Caller owns the returned pointer. */
const char *encoding_urldecode(const char *s);

#endif /* TK_STDLIB_ENCODING_H */

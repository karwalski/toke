#ifndef TK_STDLIB_CRYPTO_H
#define TK_STDLIB_CRYPTO_H

/*
 * crypto.h — C interface for the std.crypto standard library module.
 *
 * Type mappings:
 *   [Byte]  = ByteArray  (defined in str.h)
 *   Str     = const char *  (null-terminated UTF-8)
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * Story: 2.7.3  Branch: feature/stdlib-2.7-crypto-time-test
 */

#include "str.h"

/* crypto.sha256(data:[Byte]):[Byte]
 * Returns a heap-allocated ByteArray containing the 32-byte SHA-256 digest.
 * Caller owns the returned .data pointer. */
ByteArray crypto_sha256(ByteArray data);

/* crypto.hmac_sha256(key:[Byte];data:[Byte]):[Byte]
 * Returns a heap-allocated ByteArray containing the 32-byte HMAC-SHA-256 tag.
 * Caller owns the returned .data pointer. */
ByteArray crypto_hmac_sha256(ByteArray key, ByteArray data);

/* crypto.to_hex(data:[Byte]):Str
 * Returns a heap-allocated lowercase hex string (2 chars per byte + NUL).
 * Caller owns the returned pointer. */
const char *crypto_to_hex(ByteArray data);

#endif /* TK_STDLIB_CRYPTO_H */

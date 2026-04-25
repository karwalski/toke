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

/* crypto.sha512(data:[Byte]):[Byte]
 * Returns a heap-allocated ByteArray containing the 64-byte SHA-512 digest.
 * Caller owns the returned .data pointer. */
ByteArray crypto_sha512(ByteArray data);

/* crypto.hmac_sha512(key:[Byte];data:[Byte]):[Byte]
 * Returns a heap-allocated ByteArray containing the 64-byte HMAC-SHA-512 tag.
 * Caller owns the returned .data pointer. */
ByteArray crypto_hmac_sha512(ByteArray key, ByteArray data);

/* crypto.constanteq(a:[Byte];b:[Byte]):bool
 * Constant-time comparison of two byte arrays.  Returns 1 iff both arrays
 * have equal length and identical contents.  Timing is independent of content
 * when lengths match. */
int crypto_constanteq(ByteArray a, ByteArray b);

/* crypto.randombytes(n:u64):[Byte]
 * Returns a heap-allocated ByteArray containing n cryptographically secure
 * random bytes (via arc4random_buf).  Caller owns the returned .data pointer. */
ByteArray crypto_randombytes(uint64_t n);

/* crypto.to_hex(data:[Byte]):Str
 * Returns a heap-allocated lowercase hex string (2 chars per byte + NUL).
 * Caller owns the returned pointer. */
const char *crypto_to_hex(ByteArray data);

/* crypto.from_hex(hex:Str):[Byte]!CryptoErr
 * Decode a lowercase or uppercase hex string into bytes.
 * On success, returns NULL and sets *out (heap-allocated .data, caller owns).
 * On error (odd length, invalid chars), returns a static error message string
 * and sets out->data = NULL, out->len = 0.
 * Story: 29.6.1 */
const char *crypto_from_hex(const char *hex, ByteArray *out);

/* Result type for bcrypt (mirrors StrSliceResult pattern).
 * Story: 29.6.1 */
typedef struct { const char *ok; int is_err; const char *err_msg; } CryptoStrResult;

/* crypto.bcrypt_hash(password:Str;cost:i64):Str!CryptoErr
 * Hash a password with bcrypt.  cost must be 4-31 (log2 of iterations).
 * Returns CryptoStrResult.ok pointing to a heap-allocated 60-char string
 * "$2b$NN$<22-char-salt><31-char-hash>" on success (is_err=0).
 * Caller owns the returned ok pointer.
 * Story: 29.6.1 */
CryptoStrResult crypto_bcrypt_hash(const char *password, int cost);

/* crypto.bcrypt_verify(password:Str;hash:Str):bool
 * Verify a plaintext password against a bcrypt hash string.
 * Returns 1 on match, 0 on mismatch or malformed hash.
 * Story: 29.6.1 */
int crypto_bcrypt_verify(const char *password, const char *hash);

/* crypto.sha256file(path:Str):Str
 * Returns a heap-allocated lowercase hex string of the SHA-256 digest of
 * the file at path.  Returns NULL if the file cannot be opened.
 * Caller owns the returned pointer.
 * Story: 42.1.5 */
const char *crypto_sha256file(const char *path);

/* crypto.sha256verify(path:Str;expected:Str):bool
 * Compute SHA-256 of the file at path and compare (case-insensitive) with
 * expected_hex.  Returns 1 on match, 0 on mismatch or error.
 * Story: 42.1.5 */
int crypto_sha256verify(const char *path, const char *expected_hex);

/* .tki compatibility aliases (crypto.hmacsha256 → crypto_hmacsha256) */
#define crypto_hmacsha256 crypto_hmac_sha256
#define crypto_hmacsha512 crypto_hmac_sha512

#endif /* TK_STDLIB_CRYPTO_H */

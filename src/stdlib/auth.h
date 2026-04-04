#ifndef TK_STDLIB_AUTH_H
#define TK_STDLIB_AUTH_H

/*
 * auth.h — C interface for the std.auth standard library module.
 *
 * Type mappings:
 *   [Byte]  = ByteArray  (defined in str.h)
 *   Str     = const char *  (null-terminated UTF-8)
 *
 * JWT HS256 sign/verify and API key generation/validation.
 *
 * Implementation is self-contained (no external dependencies beyond libc).
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 14.1.3
 */

#include "str.h"

/* Claims for JWT construction and extraction. */
typedef struct {
    const char *subject;
    const char *issuer;
    int64_t     issued_at;    /* Unix timestamp, 0 = use time(NULL) */
    int64_t     expires_at;   /* Unix timestamp, 0 = no expiry */
    const char *audience;     /* may be NULL */
} JwtClaims;

/* Result of auth_jwtsign. */
typedef struct {
    const char *ok;     /* heap-allocated JWT string; NULL on error */
    int         is_err;
    const char *err_msg;
} JwtResult;

/* Result of auth_jwtverify. */
typedef struct {
    JwtClaims   ok;
    int         is_err;
    const char *err_msg;
} JwtVerifyResult;

/* -----------------------------------------------------------------------
 * JWT HS256
 * ----------------------------------------------------------------------- */

/* auth.jwtsign — build a signed HS256 JWT from claims and secret.
 * Returns JwtResult; on success .ok is a heap-allocated string the caller
 * must free(), .is_err == 0.  On error .is_err == 1, .ok == NULL. */
JwtResult       auth_jwtsign(JwtClaims claims, ByteArray secret);

/* auth.jwtverify — verify token signature and parse claims.
 * Returns JwtVerifyResult; on success .is_err == 0 and .ok is populated.
 * On error .is_err == 1 and .err_msg is a static string. */
JwtVerifyResult auth_jwtverify(const char *token, ByteArray secret);

/* auth_jwtexpired — return 1 if the token's exp claim is in the past or
 * the token is malformed, 0 if no exp field or exp is in the future. */
int             auth_jwtexpired(const char *token);

/* -----------------------------------------------------------------------
 * API keys
 * ----------------------------------------------------------------------- */

/* auth_apikeygen — generate a fresh API key: 32 random bytes encoded as
 * URL-safe base64 (no padding).  Returns a heap-allocated string; caller
 * must free(). */
const char     *auth_apikeygen(void);

/* auth_apikeyvalidate — constant-time comparison of two null-terminated
 * strings.  Returns 1 if equal, 0 if not.  Both pointers must be non-NULL. */
int             auth_apikeyvalidate(const char *provided, const char *stored);

#endif /* TK_STDLIB_AUTH_H */

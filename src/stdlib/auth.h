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

/* auth_apikeygenerate — generate a fresh API key: 32 random bytes encoded
 * as URL-safe base64 (no padding).  Returns a heap-allocated string; caller
 * must free().  Canonical name per std.auth contract. */
const char     *auth_apikeygenerate(void);

/* auth_apikeygen — alias for auth_apikeygenerate (legacy name). */
const char     *auth_apikeygen(void);

/* auth_apikeyvalidate — constant-time comparison of two null-terminated
 * strings.  Returns 1 if equal, 0 if not.  Both pointers must be non-NULL. */
int             auth_apikeyvalidate(const char *provided, const char *stored);

/* -----------------------------------------------------------------------
 * Bearer token extraction
 * ----------------------------------------------------------------------- */

/* Result of auth_bearerextract. */
typedef struct {
    const char *ok;      /* heap-allocated token string; NULL on error */
    int         is_err;
    const char *err_msg;
} BearerResult;

/* auth_bearerextract — extract the Bearer token from an Authorization
 * header value (e.g. "Bearer eyJhbG..." → "eyJhbG...").
 * Returns BearerResult; on success .ok is heap-allocated, caller must
 * free().  On error .is_err == 1 and .ok == NULL. */
BearerResult    auth_bearerextract(const char *header_value);

/* -----------------------------------------------------------------------
 * JWT decode without verification (Story 30.2.1)
 * ----------------------------------------------------------------------- */

/* JWT inspection — decode without signature verification, for reading claims.
 * All string fields are heap-allocated; caller must free each non-NULL field
 * and the raw_payload field when done. */
typedef struct {
    const char *sub;         /* subject claim; NULL if absent */
    const char *iss;         /* issuer claim; NULL if absent */
    const char *aud;         /* audience claim; NULL if absent */
    int64_t     exp;         /* expiry (Unix seconds), 0 if absent */
    int64_t     iat;         /* issued-at (Unix seconds), 0 if absent */
    const char *raw_payload; /* full decoded payload JSON string */
} JwtDecodedClaims;

/* Result of auth_jwtdecode_claims. */
typedef struct {
    JwtDecodedClaims ok;     /* populated on success */
    int              is_err;
    const char      *err_msg;
} JwtDecodeResult;

/* auth_jwtdecode_claims — decode a JWT payload without verifying the
 * signature.  Use only for inspection; never for access control.
 * On success .is_err == 0 and .ok fields are populated.
 * On error .is_err == 1 and .err_msg is a static string.
 * Caller must free all non-NULL string fields in .ok when done. */
JwtDecodeResult auth_jwtdecode_claims(const char *token);

/* -----------------------------------------------------------------------
 * OAuth2 helpers (Story 30.2.1)
 * ----------------------------------------------------------------------- */

/* auth_oauth2_authorize_url — build an OAuth2 authorization URL.
 *
 * provider  : well-known name ("google", "github") or a base URL.
 * client_id : OAuth2 client identifier.
 * redirect_uri : callback URL; will be percent-encoded.
 * scopes    : space-separated scope list; spaces encoded as %20.
 *
 * Returns a heap-allocated URL string; caller must free().
 * Returns NULL on allocation failure or NULL inputs. */
const char *auth_oauth2_authorize_url(const char *provider,
                                      const char *client_id,
                                      const char *redirect_uri,
                                      const char *scopes);

/* Result of auth_oauth2_token_exchange. */
typedef struct {
    const char *access_token;  /* heap-allocated; NULL on error */
    const char *refresh_token; /* heap-allocated; may be NULL */
    int64_t     expires_in;    /* seconds until expiry, 0 if absent */
    int         is_err;
    const char *err_msg;
} OAuth2TokenResult;

/* auth_oauth2_token_exchange — exchange an authorization code for tokens.
 *
 * NOTE: This stub always returns is_err=1 with err_msg
 * "auth_oauth2_token_exchange: HTTP client required".
 * Callers should use std.http to perform the actual POST to the token
 * endpoint and parse the JSON response themselves. */
OAuth2TokenResult auth_oauth2_token_exchange(const char *code,
                                             const char *client_id,
                                             const char *client_secret,
                                             const char *redirect_uri);

/* -----------------------------------------------------------------------
 * TOTP (RFC 6238 — Google Authenticator compatible)
 * Story: 30.2.2
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *uri;        /* otpauth://totp/... URI (heap-allocated) */
    const char *secret_b32; /* base32-encoded secret (heap-allocated) */
} TotpSetup;

typedef struct {
    TotpSetup   ok;
    int         is_err;
    const char *err_msg;
} TotpSetupResult;

/* auth_totp_generate — generate a new TOTP secret and otpauth URI.
 * issuer and account are URL-encoded in the URI.
 * On success .is_err == 0 and both .ok.uri and .ok.secret_b32 are
 * heap-allocated strings; caller must free() both.
 * On error .is_err == 1 and .err_msg is a static string. */
TotpSetupResult auth_totp_generate(const char *issuer, const char *account);

/* auth_totp_verify — verify a 6-digit TOTP token against a base32 secret.
 * window: number of 30-second steps to check on each side (typically 1).
 * Returns 1 if the token matches any step in [counter-window..counter+window],
 * 0 otherwise. */
int auth_totp_verify(const char *secret_b32, const char *token, int window);

/* -----------------------------------------------------------------------
 * Password hashing — thin wrappers around crypto_bcrypt
 * Story: 30.2.2
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *ok;      /* heap-allocated hash string; NULL on error */
    int         is_err;
    const char *err_msg;
} AuthStrResult;

/* auth_password_hash — hash a password using bcrypt with cost=12.
 * Returns AuthStrResult; on success .ok is heap-allocated, caller must
 * free().  On error .is_err == 1. */
AuthStrResult auth_password_hash(const char *password);

/* auth_password_verify — verify a plaintext password against a bcrypt hash.
 * Returns 1 on match, 0 on mismatch or error. */
int auth_password_verify(const char *password, const char *hash);

#endif /* TK_STDLIB_AUTH_H */

/*
 * auth_glue.c — i64-ABI wrappers for std.auth module.
 *
 * Story 114.29: std.auth had no `_w` glue, so any auth.* call failed to link.
 * These wrappers mirror encrypt_glue.c / file_glue.c: every value crosses the
 * boundary as int64_t. A toke str is a (const char*) reinterpreted as i64; a
 * [byte] is marshalled via bytes_rt.h; a record (JwtClaims) is a heap i64-slot
 * array (one slot per field, declaration order). Error unions (T!E) follow the
 * established convention: return the ok-value, or 0 on error (the Err payload
 * is dropped — same as file_glue.c).
 *
 * The .tki was reconciled to the C impl (auth.h): HS256-only sign (no JwtAlg),
 * the standard sub/iss/iat/exp/aud claim set (matching the C JwtClaims struct
 * order), and (str,str)->bool apikey validation. Wrapper names use the
 * no-underscore (Profile-1) method spelling the compiler emits.
 */

#include "auth.h"
#include "bytes_rt.h"   /* tk_bytes_unpack */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int64_t dup_str_i64(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return 0;
    memcpy(out, s, n + 1);
    return (int64_t)(intptr_t)out;
}

/* auth.jwtsign(JwtClaims, [byte] secret) -> str!AuthErr (HS256).
 * JwtClaims record slots: [0]=sub [1]=iss [2]=iat [3]=exp [4]=aud. */
int64_t tk_auth_jwtsign_w(int64_t claims_i64, int64_t secret_i64) {
    const int64_t *c = (const int64_t *)(intptr_t)claims_i64;
    JwtClaims claims;
    claims.subject    = c ? (const char *)(intptr_t)c[0] : NULL;
    claims.issuer     = c ? (const char *)(intptr_t)c[1] : NULL;
    claims.issued_at  = c ? c[2] : 0;
    claims.expires_at = c ? c[3] : 0;
    claims.audience   = c ? (const char *)(intptr_t)c[4] : NULL;
    uint8_t *sb; uint64_t sn = tk_bytes_unpack(secret_i64, &sb);
    ByteArray secret = { sb, sn };
    JwtResult r = auth_jwtsign(claims, secret);
    free(sb);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

/* auth.jwtverify(str token, [byte] secret) -> JwtClaims!AuthErr.
 * On success returns a JwtClaims record (slots match jwtsign). */
int64_t tk_auth_jwtverify_w(int64_t token_i64, int64_t secret_i64) {
    const char *token = (const char *)(intptr_t)token_i64;
    uint8_t *sb; uint64_t sn = tk_bytes_unpack(secret_i64, &sb);
    ByteArray secret = { sb, sn };
    JwtVerifyResult r = auth_jwtverify(token ? token : "", secret);
    free(sb);
    if (r.is_err) return 0;
    int64_t *rec = (int64_t *)malloc(5 * sizeof(int64_t));
    if (!rec) return 0;
    rec[0] = dup_str_i64(r.ok.subject);
    rec[1] = dup_str_i64(r.ok.issuer);
    rec[2] = (int64_t)r.ok.issued_at;
    rec[3] = (int64_t)r.ok.expires_at;
    rec[4] = dup_str_i64(r.ok.audience);
    return (int64_t)(intptr_t)rec;
}

/* auth.jwtexpired(str token) -> bool */
int64_t tk_auth_jwtexpired_w(int64_t token_i64) {
    const char *t = (const char *)(intptr_t)token_i64;
    return (int64_t)auth_jwtexpired(t ? t : "");
}

/* auth.apikeygenerate() -> str */
int64_t tk_auth_apikeygenerate_w(void) {
    const char *k = auth_apikeygenerate();
    return k ? (int64_t)(intptr_t)k : 0;
}

/* auth.apikeyvalidate(str provided, str stored) -> bool */
int64_t tk_auth_apikeyvalidate_w(int64_t provided_i64, int64_t stored_i64) {
    const char *p = (const char *)(intptr_t)provided_i64;
    const char *s = (const char *)(intptr_t)stored_i64;
    if (!p || !s) return 0;
    return (int64_t)auth_apikeyvalidate(p, s);
}

/* auth.bearerextract(str header) -> str!AuthErr */
int64_t tk_auth_bearerextract_w(int64_t header_i64) {
    const char *h = (const char *)(intptr_t)header_i64;
    BearerResult r = auth_bearerextract(h ? h : "");
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

/* auth.passwordhash(str password) -> str!AuthErr (bcrypt) */
int64_t tk_auth_passwordhash_w(int64_t pw_i64) {
    const char *p = (const char *)(intptr_t)pw_i64;
    AuthStrResult r = auth_password_hash(p ? p : "");
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

/* auth.passwordverify(str password, str hash) -> bool */
int64_t tk_auth_passwordverify_w(int64_t pw_i64, int64_t hash_i64) {
    const char *p = (const char *)(intptr_t)pw_i64;
    const char *h = (const char *)(intptr_t)hash_i64;
    if (!p || !h) return 0;
    return (int64_t)auth_password_verify(p, h);
}

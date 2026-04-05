/*
 * auth.c — Implementation of the std.auth standard library module.
 *
 * Provides JWT HS256 sign/verify and API key generation/validation.
 * Depends on std.encoding (b64urlencode/b64urldecode) and std.crypto
 * (hmac_sha256).
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 14.1.3
 */

#include "auth.h"
#include "encoding.h"
#include "crypto.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */

/* Concatenate two strings with a separator character.  Caller frees result. */
static char *concat3(const char *a, char sep, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    char  *out = (char *)malloc(la + 1 + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    out[la] = sep;
    memcpy(out + la + 1, b, lb);
    out[la + 1 + lb] = '\0';
    return out;
}

/* Build a ByteArray view over a null-terminated string (no copy). */
static ByteArray ba_of_str(const char *s)
{
    ByteArray b;
    b.data = (const uint8_t *)s;
    b.len  = (uint64_t)strlen(s);
    return b;
}

/* Build a ByteArray view over raw bytes (no copy). */
static ByteArray ba_of_bytes(const uint8_t *data, uint64_t len)
{
    ByteArray b;
    b.data = data;
    b.len  = len;
    return b;
}

/* -----------------------------------------------------------------------
 * JWT HS256 — sign
 * ----------------------------------------------------------------------- */

JwtResult auth_jwtsign(JwtClaims claims, ByteArray secret)
{
    JwtResult result;
    result.ok      = NULL;
    result.is_err  = 1;
    result.err_msg = "unknown error";

    /* ---- Header ---- */
    const char *header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    ByteArray   hj = ba_of_str(header_json);
    const char *header_b64 = encoding_b64urlencode(hj);
    if (!header_b64) {
        result.err_msg = "failed to encode header";
        return result;
    }

    /* ---- Payload ---- */
    int64_t iat = claims.issued_at  ? claims.issued_at  : (int64_t)time(NULL);
    int64_t exp = claims.expires_at;

    /* Build payload JSON.  We keep it minimal and spec-correct.
     * Allocate generously: field names + values + delimiters. */
    size_t sub_len = claims.subject  ? strlen(claims.subject)  : 0;
    size_t iss_len = claims.issuer   ? strlen(claims.issuer)   : 0;
    size_t aud_len = claims.audience ? strlen(claims.audience) : 0;

    /* Max payload length estimate:
     *   {"sub":"<sub>","iss":"<iss>","iat":<20d>,"exp":<20d>,"aud":"<aud>"}
     *   plus some slack. */
    size_t buf_size = 64 + sub_len + iss_len + aud_len + 128;
    char  *payload_json = (char *)malloc(buf_size);
    if (!payload_json) {
        free((void *)header_b64);
        result.err_msg = "out of memory";
        return result;
    }

    int pos = 0;
    pos += snprintf(payload_json + pos, buf_size - (size_t)pos, "{");

    int need_comma = 0;

    if (claims.subject) {
        pos += snprintf(payload_json + pos, buf_size - (size_t)pos,
                        "\"sub\":\"%s\"", claims.subject);
        need_comma = 1;
    }
    if (claims.issuer) {
        if (need_comma) pos += snprintf(payload_json + pos, buf_size - (size_t)pos, ",");
        pos += snprintf(payload_json + pos, buf_size - (size_t)pos,
                        "\"iss\":\"%s\"", claims.issuer);
        need_comma = 1;
    }
    /* iat is always emitted */
    if (need_comma) pos += snprintf(payload_json + pos, buf_size - (size_t)pos, ",");
    pos += snprintf(payload_json + pos, buf_size - (size_t)pos,
                    "\"iat\":%" PRId64, iat);
    need_comma = 1;

    if (exp != 0) {
        pos += snprintf(payload_json + pos, buf_size - (size_t)pos,
                        ",\"exp\":%" PRId64, exp);
    }
    if (claims.audience) {
        pos += snprintf(payload_json + pos, buf_size - (size_t)pos,
                        ",\"aud\":\"%s\"", claims.audience);
    }
    pos += snprintf(payload_json + pos, buf_size - (size_t)pos, "}");
    (void)pos; /* suppress unused-variable warning */

    ByteArray   pj         = ba_of_str(payload_json);
    const char *payload_b64 = encoding_b64urlencode(pj);
    free(payload_json);

    if (!payload_b64) {
        free((void *)header_b64);
        result.err_msg = "failed to encode payload";
        return result;
    }

    /* ---- Signing input: header_b64 + "." + payload_b64 ---- */
    char *signing_input = concat3(header_b64, '.', payload_b64);
    if (!signing_input) {
        free((void *)header_b64);
        free((void *)payload_b64);
        result.err_msg = "out of memory";
        return result;
    }

    ByteArray   si_ba  = ba_of_str(signing_input);
    ByteArray   mac    = crypto_hmac_sha256(secret, si_ba);
    if (!mac.data) {
        free(signing_input);
        free((void *)header_b64);
        free((void *)payload_b64);
        result.err_msg = "HMAC computation failed";
        return result;
    }

    const char *sig_b64 = encoding_b64urlencode(mac);
    free((void *)mac.data);

    if (!sig_b64) {
        free(signing_input);
        free((void *)header_b64);
        free((void *)payload_b64);
        result.err_msg = "failed to encode signature";
        return result;
    }

    /* ---- Assemble token: header.payload.signature ---- */
    size_t token_len = strlen(header_b64) + 1 + strlen(payload_b64) + 1 + strlen(sig_b64) + 1;
    char  *token = (char *)malloc(token_len);
    if (!token) {
        free(signing_input);
        free((void *)header_b64);
        free((void *)payload_b64);
        free((void *)sig_b64);
        result.err_msg = "out of memory";
        return result;
    }
    snprintf(token, token_len, "%s.%s.%s", header_b64, payload_b64, sig_b64);

    free(signing_input);
    free((void *)header_b64);
    free((void *)payload_b64);
    free((void *)sig_b64);

    result.ok     = token;
    result.is_err = 0;
    result.err_msg = NULL;
    return result;
}

/* -----------------------------------------------------------------------
 * JWT HS256 — internal: split token into three parts
 *
 * Fills part[0], part[1], part[2] with heap-allocated copies.
 * Returns 1 on success, 0 on failure.  Caller frees all three on success.
 * ----------------------------------------------------------------------- */
static int jwt_split(const char *token, char **part)
{
    part[0] = part[1] = part[2] = NULL;

    const char *p = token;
    const char *dot1 = strchr(p, '.');
    if (!dot1) return 0;

    const char *dot2 = strchr(dot1 + 1, '.');
    if (!dot2) return 0;

    /* Make sure there is no third dot in the remainder */
    if (strchr(dot2 + 1, '.') != NULL) return 0;

    size_t l0 = (size_t)(dot1 - p);
    size_t l1 = (size_t)(dot2 - dot1 - 1);
    size_t l2 = strlen(dot2 + 1);

    part[0] = (char *)malloc(l0 + 1);
    part[1] = (char *)malloc(l1 + 1);
    part[2] = (char *)malloc(l2 + 1);

    if (!part[0] || !part[1] || !part[2]) {
        free(part[0]); free(part[1]); free(part[2]);
        return 0;
    }

    memcpy(part[0], p,         l0); part[0][l0] = '\0';
    memcpy(part[1], dot1 + 1,  l1); part[1][l1] = '\0';
    memcpy(part[2], dot2 + 1,  l2); part[2][l2] = '\0';

    return 1;
}

/* -----------------------------------------------------------------------
 * JWT HS256 — internal: constant-time byte-array comparison
 * ----------------------------------------------------------------------- */
static int ct_memeq(const uint8_t *a, const uint8_t *b, size_t len)
{
    volatile uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= (uint8_t)(a[i] ^ b[i]);
    }
    return acc == 0;
}

/* -----------------------------------------------------------------------
 * JWT HS256 — internal: parse a string field from minimally-formatted JSON.
 *
 * Looks for  "key":"value"  and copies value into out (up to out_size-1
 * bytes, NUL-terminated).  Returns 1 on success, 0 if not found.
 * ----------------------------------------------------------------------- */
static int json_get_str(const char *json, const char *key,
                        char *out, size_t out_size)
{
    /* Build search pattern: "key":" */
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);  /* now points at first char of value */

    const char *end = strchr(p, '"');
    if (!end) return 0;

    size_t val_len = (size_t)(end - p);
    if (val_len >= out_size) val_len = out_size - 1;
    memcpy(out, p, val_len);
    out[val_len] = '\0';
    return 1;
}

/* -----------------------------------------------------------------------
 * JWT HS256 — internal: parse an integer field from minimally-formatted JSON.
 *
 * Looks for  "key":<integer>  and stores the value.
 * Returns 1 on success, 0 if not found.
 * ----------------------------------------------------------------------- */
static int json_get_int64(const char *json, const char *key, int64_t *out)
{
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);

    const char *p = strstr(json, pattern);
    if (!p) return 0;

    p += strlen(pattern);
    /* skip optional whitespace */
    while (*p == ' ' || *p == '\t') p++;

    char *endptr = NULL;
    long long val = strtoll(p, &endptr, 10);
    if (endptr == p) return 0;

    *out = (int64_t)val;
    return 1;
}

/* -----------------------------------------------------------------------
 * JWT HS256 — verify
 * ----------------------------------------------------------------------- */

JwtVerifyResult auth_jwtverify(const char *token, ByteArray secret)
{
    JwtVerifyResult result;
    memset(&result, 0, sizeof(result));
    result.is_err  = 1;
    result.err_msg = "invalid token";

    if (!token) return result;

    /* Split into header_b64 . payload_b64 . sig_b64 */
    char *part[3];
    if (!jwt_split(token, part)) {
        result.err_msg = "malformed token: wrong number of segments";
        return result;
    }

    /* Re-compute expected signature over "header.payload" */
    char *signing_input = concat3(part[0], '.', part[1]);
    if (!signing_input) {
        free(part[0]); free(part[1]); free(part[2]);
        result.err_msg = "out of memory";
        return result;
    }

    ByteArray si_ba      = ba_of_str(signing_input);
    ByteArray expected   = crypto_hmac_sha256(secret, si_ba);
    free(signing_input);

    if (!expected.data) {
        free(part[0]); free(part[1]); free(part[2]);
        result.err_msg = "HMAC computation failed";
        return result;
    }

    /* Decode provided signature */
    ByteArray provided = encoding_b64urldecode(part[2]);

    int sig_ok = 0;
    if (provided.data && provided.len == expected.len) {
        sig_ok = ct_memeq(provided.data, expected.data, (size_t)expected.len);
    }

    free((void *)expected.data);
    free((void *)provided.data);
    free(part[0]);
    free(part[2]);

    if (!sig_ok) {
        free(part[1]);
        result.err_msg = "signature mismatch";
        return result;
    }

    /* Decode payload */
    ByteArray payload_bytes = encoding_b64urldecode(part[1]);
    free(part[1]);

    if (!payload_bytes.data) {
        result.err_msg = "failed to decode payload";
        return result;
    }

    /* NUL-terminate for string operations */
    char *payload_json = (char *)malloc(payload_bytes.len + 1);
    if (!payload_json) {
        free((void *)payload_bytes.data);
        result.err_msg = "out of memory";
        return result;
    }
    memcpy(payload_json, payload_bytes.data, payload_bytes.len);
    payload_json[payload_bytes.len] = '\0';
    free((void *)payload_bytes.data);

    /* Parse claims */
    char sub_buf[512] = {0};
    char iss_buf[512] = {0};
    int64_t iat_val = 0, exp_val = 0;

    int has_sub = json_get_str(payload_json, "sub", sub_buf, sizeof(sub_buf));
    int has_iss = json_get_str(payload_json, "iss", iss_buf, sizeof(iss_buf));
    json_get_int64(payload_json, "iat", &iat_val);
    int has_exp = json_get_int64(payload_json, "exp", &exp_val);
    free(payload_json);

    JwtClaims claims;
    memset(&claims, 0, sizeof(claims));
    claims.subject    = has_sub ? strdup(sub_buf) : NULL;
    claims.issuer     = has_iss ? strdup(iss_buf) : NULL;
    claims.issued_at  = iat_val;
    claims.expires_at = has_exp ? exp_val : 0;
    claims.audience   = NULL;

    result.ok      = claims;
    result.is_err  = 0;
    result.err_msg = NULL;
    return result;
}

/* -----------------------------------------------------------------------
 * JWT HS256 — expired check
 * ----------------------------------------------------------------------- */

int auth_jwtexpired(const char *token)
{
    if (!token) return 1;

    char *part[3];
    if (!jwt_split(token, part)) return 1;

    free(part[0]);
    free(part[2]);

    ByteArray payload_bytes = encoding_b64urldecode(part[1]);
    free(part[1]);

    if (!payload_bytes.data) return 1;

    char *payload_json = (char *)malloc(payload_bytes.len + 1);
    if (!payload_json) {
        free((void *)payload_bytes.data);
        return 1;
    }
    memcpy(payload_json, payload_bytes.data, payload_bytes.len);
    payload_json[payload_bytes.len] = '\0';
    free((void *)payload_bytes.data);

    int64_t exp_val = 0;
    int has_exp = json_get_int64(payload_json, "exp", &exp_val);
    free(payload_json);

    if (!has_exp) return 0;   /* no expiry → not expired */

    return exp_val < (int64_t)time(NULL) ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * API key generation
 * ----------------------------------------------------------------------- */

const char *auth_apikeygenerate(void)
{
    uint8_t raw[32];

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(raw, sizeof(raw));
#else
    /* Fall back to /dev/urandom on other platforms. */
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return NULL;
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (n != sizeof(raw)) return NULL;
#endif

    ByteArray   ba  = ba_of_bytes(raw, sizeof(raw));
    const char *key = encoding_b64urlencode(ba);
    return key;   /* heap-allocated, caller frees */
}

/* Legacy alias. */
const char *auth_apikeygen(void)
{
    return auth_apikeygenerate();
}

/* -----------------------------------------------------------------------
 * API key validation (constant-time)
 * ----------------------------------------------------------------------- */

int auth_apikeyvalidate(const char *provided, const char *stored)
{
    if (!provided || !stored) return 0;

    size_t lp = strlen(provided);
    size_t ls = strlen(stored);

    /* We must compare all bytes of the longer string to avoid length leaks.
     * Accumulate any differences and also flag length mismatch. */
    volatile uint8_t acc = (uint8_t)(lp != ls);  /* 1 if lengths differ */

    size_t max_len = lp > ls ? lp : ls;
    for (size_t i = 0; i < max_len; i++) {
        uint8_t a = i < lp ? (uint8_t)provided[i] : 0;
        uint8_t b = i < ls ? (uint8_t)stored[i]   : 0;
        acc |= (uint8_t)(a ^ b);
    }

    return acc == 0 ? 1 : 0;
}

/* -----------------------------------------------------------------------
 * Bearer token extraction
 * ----------------------------------------------------------------------- */

BearerResult auth_bearerextract(const char *header_value)
{
    BearerResult result;
    result.ok      = NULL;
    result.is_err  = 1;
    result.err_msg = "invalid header";

    if (!header_value) {
        result.err_msg = "NULL header value";
        return result;
    }

    /* Must start with "Bearer " (case-sensitive, single space). */
    const char *prefix = "Bearer ";
    size_t prefix_len = 7;

    if (strncmp(header_value, prefix, prefix_len) != 0) {
        result.err_msg = "not a Bearer token";
        return result;
    }

    const char *token_start = header_value + prefix_len;

    /* Token must be non-empty. */
    if (*token_start == '\0') {
        result.err_msg = "empty Bearer token";
        return result;
    }

    char *token = strdup(token_start);
    if (!token) {
        result.err_msg = "out of memory";
        return result;
    }

    result.ok      = token;
    result.is_err  = 0;
    result.err_msg = NULL;
    return result;
}

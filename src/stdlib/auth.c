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
 * JWT decode without verification (Story 30.2.1)
 * ----------------------------------------------------------------------- */

JwtDecodeResult auth_jwtdecode_claims(const char *token)
{
    JwtDecodeResult result;
    memset(&result, 0, sizeof(result));
    result.is_err  = 1;
    result.err_msg = "invalid token";

    if (!token || *token == '\0') {
        result.err_msg = "NULL or empty token";
        return result;
    }

    /* Split into header . payload . signature (exactly 2 dots). */
    char *part[3];
    if (!jwt_split(token, part)) {
        result.err_msg = "malformed token: wrong number of segments";
        return result;
    }

    free(part[0]); /* header — not needed for decode */
    free(part[2]); /* signature — not verified */

    /* Decode the payload (part[1]). */
    ByteArray payload_bytes = encoding_b64urldecode(part[1]);
    free(part[1]);

    if (!payload_bytes.data) {
        result.err_msg = "failed to base64url-decode payload";
        return result;
    }

    /* NUL-terminate the decoded payload JSON. */
    char *payload_json = (char *)malloc(payload_bytes.len + 1);
    if (!payload_json) {
        free((void *)payload_bytes.data);
        result.err_msg = "out of memory";
        return result;
    }
    memcpy(payload_json, payload_bytes.data, payload_bytes.len);
    payload_json[payload_bytes.len] = '\0';
    free((void *)payload_bytes.data);

    /* Extract string fields. */
    char sub_buf[512] = {0};
    char iss_buf[512] = {0};
    char aud_buf[512] = {0};
    int64_t exp_val = 0, iat_val = 0;

    int has_sub = json_get_str(payload_json, "sub", sub_buf, sizeof(sub_buf));
    int has_iss = json_get_str(payload_json, "iss", iss_buf, sizeof(iss_buf));
    int has_aud = json_get_str(payload_json, "aud", aud_buf, sizeof(aud_buf));
    int has_exp = json_get_int64(payload_json, "exp", &exp_val);
    int has_iat = json_get_int64(payload_json, "iat", &iat_val);

    JwtDecodedClaims claims;
    memset(&claims, 0, sizeof(claims));
    claims.sub         = has_sub ? strdup(sub_buf) : NULL;
    claims.iss         = has_iss ? strdup(iss_buf) : NULL;
    claims.aud         = has_aud ? strdup(aud_buf) : NULL;
    claims.exp         = has_exp ? exp_val : 0;
    claims.iat         = has_iat ? iat_val : 0;
    claims.raw_payload = payload_json; /* caller owns this */

    result.ok      = claims;
    result.is_err  = 0;
    result.err_msg = NULL;
    return result;
}

/* -----------------------------------------------------------------------
 * OAuth2 helpers (Story 30.2.1)
 * ----------------------------------------------------------------------- */

/* Resolve a well-known provider name to its authorization endpoint URL.
 * Returns a static string for known providers, or the input itself as a
 * base URL for unknown values. */
static const char *oauth2_provider_url(const char *provider)
{
    if (!provider) return NULL;
    if (strcmp(provider, "google") == 0)
        return "https://accounts.google.com/o/oauth2/auth";
    if (strcmp(provider, "github") == 0)
        return "https://github.com/login/oauth/authorize";
    /* Treat anything else as a literal base URL. */
    return provider;
}

const char *auth_oauth2_authorize_url(const char *provider,
                                      const char *client_id,
                                      const char *redirect_uri,
                                      const char *scopes)
{
    if (!provider || !client_id || !redirect_uri || !scopes) return NULL;

    const char *base_url = oauth2_provider_url(provider);
    if (!base_url) return NULL;

    /* Percent-encode redirect_uri and scopes using encoding_urlencode. */
    const char *enc_redirect = encoding_urlencode(redirect_uri);
    if (!enc_redirect) return NULL;

    /* Encode scopes: spaces become %20 via urlencode. */
    const char *enc_scopes = encoding_urlencode(scopes);
    if (!enc_scopes) {
        free((void *)enc_redirect);
        return NULL;
    }

    /* Build: {base_url}?client_id={id}&redirect_uri={uri}&scope={scopes}&response_type=code */
    size_t url_len = strlen(base_url)
                   + strlen("?client_id=") + strlen(client_id)
                   + strlen("&redirect_uri=") + strlen(enc_redirect)
                   + strlen("&scope=") + strlen(enc_scopes)
                   + strlen("&response_type=code")
                   + 1; /* NUL */

    char *url = (char *)malloc(url_len);
    if (!url) {
        free((void *)enc_redirect);
        free((void *)enc_scopes);
        return NULL;
    }

    snprintf(url, url_len,
             "%s?client_id=%s&redirect_uri=%s&scope=%s&response_type=code",
             base_url, client_id, enc_redirect, enc_scopes);

    free((void *)enc_redirect);
    free((void *)enc_scopes);

    return url;
}

OAuth2TokenResult auth_oauth2_token_exchange(const char *code,
                                             const char *client_id,
                                             const char *client_secret,
                                             const char *redirect_uri)
{
    /* Suppress unused-parameter warnings under -Wextra. */
    (void)code;
    (void)client_id;
    (void)client_secret;
    (void)redirect_uri;

    OAuth2TokenResult result;
    memset(&result, 0, sizeof(result));
    result.is_err  = 1;
    result.err_msg = "auth_oauth2_token_exchange: HTTP client required";
    return result;
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

/* -----------------------------------------------------------------------
 * SHA1 — pure C99 implementation (needed for HMAC-SHA1 / TOTP)
 * Story: 30.2.2
 * ----------------------------------------------------------------------- */

/* Rotate-left 32-bit */
#define SHA1_ROL32(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sha1_hash(const uint8_t *data, size_t len, uint8_t out[20])
{
    /* Initial hash values (H0-H4). */
    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xEFCDAB89U;
    uint32_t h2 = 0x98BADCFEU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xC3D2E1F0U;

    /* Pre-processing: build padded message in a scratch buffer.
     * padded length = ceil((len+9)/64)*64 */
    size_t bit_len = len * 8;
    size_t pad_len = ((len + 9 + 63) / 64) * 64;

    uint8_t *msg = (uint8_t *)malloc(pad_len);
    if (!msg) return; /* allocation failure — out stays zeroed by caller */

    memcpy(msg, data, len);
    msg[len] = 0x80;
    memset(msg + len + 1, 0, pad_len - len - 9);
    /* Append original bit-length as 64-bit big-endian. */
    for (int i = 0; i < 8; i++) {
        msg[pad_len - 8 + i] = (uint8_t)((bit_len >> (56 - i * 8)) & 0xFF);
    }

    /* Process each 512-bit (64-byte) block. */
    for (size_t offset = 0; offset < pad_len; offset += 64) {
        uint32_t w[80];
        /* Prepare message schedule. */
        for (int i = 0; i < 16; i++) {
            w[i] = ((uint32_t)msg[offset + i*4    ] << 24)
                 | ((uint32_t)msg[offset + i*4 + 1] << 16)
                 | ((uint32_t)msg[offset + i*4 + 2] <<  8)
                 | ((uint32_t)msg[offset + i*4 + 3]);
        }
        for (int i = 16; i < 80; i++) {
            w[i] = SHA1_ROL32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
        }

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;

        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            uint32_t temp = SHA1_ROL32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = SHA1_ROL32(b, 30); b = a; a = temp;
        }

        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    free(msg);

    /* Write big-endian digest. */
    uint32_t digest[5] = { h0, h1, h2, h3, h4 };
    for (int i = 0; i < 5; i++) {
        out[i*4    ] = (uint8_t)((digest[i] >> 24) & 0xFF);
        out[i*4 + 1] = (uint8_t)((digest[i] >> 16) & 0xFF);
        out[i*4 + 2] = (uint8_t)((digest[i] >>  8) & 0xFF);
        out[i*4 + 3] = (uint8_t)( digest[i]        & 0xFF);
    }
}

#undef SHA1_ROL32

/* -----------------------------------------------------------------------
 * HMAC-SHA1 — needed for TOTP (RFC 2104 + RFC 6238)
 * Story: 30.2.2
 * ----------------------------------------------------------------------- */

static void hmac_sha1(const uint8_t *key, size_t key_len,
                      const uint8_t *data, size_t data_len,
                      uint8_t out[20])
{
    uint8_t k_block[64];
    uint8_t k_hashed[20];

    memset(k_block, 0, sizeof(k_block));

    /* If key > 64 bytes, hash it down to 20 bytes. */
    if (key_len > 64) {
        sha1_hash(key, key_len, k_hashed);
        memcpy(k_block, k_hashed, 20);
    } else {
        memcpy(k_block, key, key_len);
    }

    /* ipad and opad. */
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; i++) {
        ipad[i] = (uint8_t)(k_block[i] ^ 0x36);
        opad[i] = (uint8_t)(k_block[i] ^ 0x5C);
    }

    /* inner = SHA1(ipad || data) */
    size_t inner_len = 64 + data_len;
    uint8_t *inner_msg = (uint8_t *)malloc(inner_len);
    if (!inner_msg) return;
    memcpy(inner_msg, ipad, 64);
    memcpy(inner_msg + 64, data, data_len);
    uint8_t inner_hash[20];
    sha1_hash(inner_msg, inner_len, inner_hash);
    free(inner_msg);

    /* outer = SHA1(opad || inner_hash) */
    uint8_t outer_msg[64 + 20];
    memcpy(outer_msg, opad, 64);
    memcpy(outer_msg + 64, inner_hash, 20);
    sha1_hash(outer_msg, 64 + 20, out);
}

/* -----------------------------------------------------------------------
 * TOTP — generate and verify (Story 30.2.2)
 * ----------------------------------------------------------------------- */

TotpSetupResult auth_totp_generate(const char *issuer, const char *account)
{
    TotpSetupResult result;
    memset(&result, 0, sizeof(result));
    result.is_err  = 1;
    result.err_msg = "unknown error";

    if (!issuer || !account) {
        result.err_msg = "issuer and account must be non-NULL";
        return result;
    }

    /* Generate 20 random bytes for the secret. */
    uint8_t secret_raw[20];
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    arc4random_buf(secret_raw, sizeof(secret_raw));
#else
    {
        FILE *f = fopen("/dev/urandom", "rb");
        if (!f) { result.err_msg = "failed to open /dev/urandom"; return result; }
        size_t n = fread(secret_raw, 1, sizeof(secret_raw), f);
        fclose(f);
        if (n != sizeof(secret_raw)) { result.err_msg = "failed to read random bytes"; return result; }
    }
#endif

    /* Base32-encode the secret. */
    ByteArray raw_ba;
    raw_ba.data = secret_raw;
    raw_ba.len  = sizeof(secret_raw);
    const char *secret_b32 = encoding_base32_encode(raw_ba);
    if (!secret_b32) {
        result.err_msg = "failed to base32-encode secret";
        return result;
    }

    /* URL-encode issuer and account. */
    const char *enc_issuer  = encoding_urlencode(issuer);
    const char *enc_account = encoding_urlencode(account);
    if (!enc_issuer || !enc_account) {
        free((void *)secret_b32);
        free((void *)enc_issuer);
        free((void *)enc_account);
        result.err_msg = "out of memory";
        return result;
    }

    /* Build otpauth URI:
     * otpauth://totp/{issuer}:{account}?secret={b32}&issuer={issuer}&algorithm=SHA1&digits=6&period=30 */
    size_t uri_len = strlen("otpauth://totp/")
                   + strlen(enc_issuer) + 1 /* ':' */
                   + strlen(enc_account)
                   + strlen("?secret=") + strlen(secret_b32)
                   + strlen("&issuer=") + strlen(enc_issuer)
                   + strlen("&algorithm=SHA1&digits=6&period=30")
                   + 1; /* NUL */

    char *uri = (char *)malloc(uri_len);
    if (!uri) {
        free((void *)secret_b32);
        free((void *)enc_issuer);
        free((void *)enc_account);
        result.err_msg = "out of memory";
        return result;
    }

    snprintf(uri, uri_len,
             "otpauth://totp/%s:%s?secret=%s&issuer=%s&algorithm=SHA1&digits=6&period=30",
             enc_issuer, enc_account, secret_b32, enc_issuer);

    free((void *)enc_issuer);
    free((void *)enc_account);

    result.ok.uri        = uri;
    result.ok.secret_b32 = secret_b32;
    result.is_err        = 0;
    result.err_msg       = NULL;
    return result;
}

int auth_totp_verify(const char *secret_b32, const char *token, int window)
{
    if (!secret_b32 || !token) return 0;

    /* Decode base32 secret. */
    EncBytesResult dec = encoding_base32_decode(secret_b32);
    if (dec.is_err || !dec.ok.data) return 0;

    const uint8_t *key     = dec.ok.data;
    size_t         key_len = (size_t)dec.ok.len;

    /* Current TOTP counter. */
    int64_t now     = (int64_t)time(NULL);
    int64_t counter = now / 30;

    int matched = 0;
    for (int step = -window; step <= window && !matched; step++) {
        int64_t c = counter + step;

        /* Pack counter as 8-byte big-endian. */
        uint8_t msg[8];
        for (int i = 7; i >= 0; i--) {
            msg[i] = (uint8_t)(c & 0xFF);
            c >>= 8;
        }

        /* HMAC-SHA1(secret, counter). */
        uint8_t hmac[20];
        hmac_sha1(key, key_len, msg, 8, hmac);

        /* Dynamic truncation. */
        int offset = hmac[19] & 0x0F;
        uint32_t code = ((uint32_t)(hmac[offset    ] & 0x7F) << 24)
                      | ((uint32_t)(hmac[offset + 1] & 0xFF) << 16)
                      | ((uint32_t)(hmac[offset + 2] & 0xFF) <<  8)
                      | ((uint32_t)(hmac[offset + 3] & 0xFF));
        code = code % 1000000U;

        /* Format as 6-digit zero-padded string and compare. */
        char expected[7];
        snprintf(expected, sizeof(expected), "%06u", (unsigned int)code);
        if (strcmp(expected, token) == 0) matched = 1;
    }

    free((void *)dec.ok.data);
    return matched;
}

/* -----------------------------------------------------------------------
 * Password hash wrappers (Story 30.2.2)
 * ----------------------------------------------------------------------- */

AuthStrResult auth_password_hash(const char *password)
{
    AuthStrResult result;
    result.ok      = NULL;
    result.is_err  = 1;
    result.err_msg = "unknown error";

    if (!password) {
        result.err_msg = "NULL password";
        return result;
    }

    CryptoStrResult cr = crypto_bcrypt_hash(password, 12);
    if (cr.is_err) {
        result.err_msg = cr.err_msg ? cr.err_msg : "bcrypt failed";
        return result;
    }

    result.ok      = cr.ok; /* heap-allocated by crypto_bcrypt_hash */
    result.is_err  = 0;
    result.err_msg = NULL;
    return result;
}

int auth_password_verify(const char *password, const char *hash)
{
    if (!password || !hash) return 0;
    return crypto_bcrypt_verify(password, hash);
}

/*
 * test_auth.c — Unit tests for the std.auth C library (Story 14.1.3).
 *
 * Build and run: make test-stdlib-auth
 *
 * Coverage:
 *   - auth_jwtsign returns non-NULL token
 *   - token has exactly 2 dots (three parts)
 *   - auth_jwtverify of valid token returns is_err=0
 *   - auth_jwtverify recovers subject from claims
 *   - auth_jwtverify recovers issuer from claims
 *   - tampered token (char flipped in signature) → is_err=1
 *   - wrong secret → is_err=1
 *   - auth_jwtexpired on token with exp in past → returns 1
 *   - auth_jwtexpired on token with no exp → returns 0
 *   - auth_apikeygen returns non-NULL string of length > 0
 *   - two calls to auth_apikeygen return different strings
 *   - auth_apikeyvalidate("abc","abc") = 1
 *   - auth_apikeyvalidate("abc","abd") = 0
 *   - auth_apikeyvalidate("","") = 1
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "../../src/stdlib/auth.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

/* Count occurrences of a character in a string. */
static int count_char(const char *s, char c)
{
    int n = 0;
    while (*s) { if (*s == c) n++; s++; }
    return n;
}

/* Build a ByteArray view over a string literal (no NUL). */
static ByteArray ba_from_str(const char *s)
{
    ByteArray b;
    b.data = (const uint8_t *)s;
    b.len  = (uint64_t)strlen(s);
    return b;
}

int main(void)
{
    /* ------------------------------------------------------------------ */
    /* auth_jwtsign                                                        */
    /* ------------------------------------------------------------------ */

    ByteArray secret = ba_from_str("supersecretkey");

    JwtClaims claims;
    claims.subject    = "user-42";
    claims.issuer     = "toke-test";
    claims.issued_at  = 0;   /* use time(NULL) */
    claims.expires_at = 0;   /* no expiry */
    claims.audience   = NULL;

    JwtResult sign_result = auth_jwtsign(claims, secret);

    ASSERT(sign_result.is_err == 0,   "jwtsign is_err==0");
    ASSERT(sign_result.ok != NULL,    "jwtsign returns non-NULL token");

    const char *token = sign_result.ok;

    ASSERT(count_char(token, '.') == 2, "token has exactly 2 dots");

    /* ------------------------------------------------------------------ */
    /* auth_jwtverify — happy path                                         */
    /* ------------------------------------------------------------------ */

    JwtVerifyResult verify_result = auth_jwtverify(token, secret);

    ASSERT(verify_result.is_err == 0,  "jwtverify valid token is_err==0");

    ASSERT_STREQ(verify_result.ok.subject, "user-42",
                 "jwtverify recovers subject");

    ASSERT_STREQ(verify_result.ok.issuer, "toke-test",
                 "jwtverify recovers issuer");

    /* ------------------------------------------------------------------ */
    /* auth_jwtverify — tampered signature                                 */
    /* ------------------------------------------------------------------ */

    /* Flip one character in the signature (third segment). */
    char *tampered = strdup(token);
    if (tampered) {
        size_t tlen = strlen(tampered);
        /* The last character of the token is in the signature. */
        tampered[tlen - 1] ^= 0x01;
    }

    JwtVerifyResult tampered_result = auth_jwtverify(tampered, secret);
    ASSERT(tampered_result.is_err == 1,
           "jwtverify tampered signature returns is_err=1");
    free(tampered);

    /* ------------------------------------------------------------------ */
    /* auth_jwtverify — wrong secret                                       */
    /* ------------------------------------------------------------------ */

    ByteArray wrong_secret = ba_from_str("wrongkey");
    JwtVerifyResult wrong_result = auth_jwtverify(token, wrong_secret);
    ASSERT(wrong_result.is_err == 1,
           "jwtverify wrong secret returns is_err=1");

    /* ------------------------------------------------------------------ */
    /* auth_jwtexpired                                                     */
    /* ------------------------------------------------------------------ */

    /* Token with exp=1 (well in the past). */
    JwtClaims past_claims;
    past_claims.subject    = "user-42";
    past_claims.issuer     = "toke-test";
    past_claims.issued_at  = 1;
    past_claims.expires_at = 1;   /* Unix epoch + 1 second — always past */
    past_claims.audience   = NULL;

    JwtResult past_sign = auth_jwtsign(past_claims, secret);
    ASSERT(past_sign.is_err == 0, "jwtsign past-exp token is_err==0");

    int expired = auth_jwtexpired(past_sign.ok);
    ASSERT(expired == 1, "jwtexpired on past-exp token returns 1");
    free((void *)past_sign.ok);

    /* Token with no exp. */
    int not_expired = auth_jwtexpired(token);
    ASSERT(not_expired == 0, "jwtexpired on no-exp token returns 0");

    /* ------------------------------------------------------------------ */
    /* auth_apikeygen                                                      */
    /* ------------------------------------------------------------------ */

    const char *key1 = auth_apikeygen();
    ASSERT(key1 != NULL,       "apikeygen returns non-NULL");
    ASSERT(key1 && strlen(key1) > 0, "apikeygen returns non-empty string");

    const char *key2 = auth_apikeygen();
    ASSERT(key2 != NULL,       "apikeygen second call returns non-NULL");

    /* Two random keys should not be equal (probability 2^-256 they collide). */
    ASSERT(key1 && key2 && strcmp(key1, key2) != 0,
           "two apikeygen calls return different strings");

    free((void *)key1);
    free((void *)key2);

    /* ------------------------------------------------------------------ */
    /* auth_apikeyvalidate                                                 */
    /* ------------------------------------------------------------------ */

    ASSERT(auth_apikeyvalidate("abc", "abc") == 1,
           "apikeyvalidate equal keys returns 1");

    ASSERT(auth_apikeyvalidate("abc", "abd") == 0,
           "apikeyvalidate different keys returns 0");

    ASSERT(auth_apikeyvalidate("", "") == 1,
           "apikeyvalidate empty strings returns 1");

    ASSERT(auth_apikeyvalidate("abc", "ab") == 0,
           "apikeyvalidate different lengths returns 0");

    /* ------------------------------------------------------------------ */
    /* Cleanup and summary                                                 */
    /* ------------------------------------------------------------------ */

    free((void *)token);
    /* Note: verify_result.ok.subject and .issuer are owned by the verify
     * result; in a production wrapper they would be freed here. */
    free((void *)verify_result.ok.subject);
    free((void *)verify_result.ok.issuer);

    if (failures == 0) {
        printf("\nAll tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d test(s) FAILED.\n", failures);
        return 1;
    }
}

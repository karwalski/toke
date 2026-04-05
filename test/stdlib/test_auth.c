/*
 * test_auth.c — Unit tests for the std.auth C library.
 *
 * Stories: 14.1.3 (initial), 20.1.3 (hardened edge cases)
 *
 * Build and run: make test-stdlib-auth
 *
 * Coverage (original — Story 14.1.3):
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
 *   - auth_apikeygenerate returns non-NULL non-empty string
 *   - auth_bearerextract("Bearer <token>") extracts token
 *   - auth_bearerextract rejects non-Bearer, NULL, empty, lowercase, no-space
 *
 * Coverage (hardened — Story 20.1.3):
 *   - JWT with extra dots (4+ segments) rejected
 *   - JWT with too few segments (0 dots, 1 dot) rejected
 *   - JWT with empty segments rejected
 *   - NULL/empty token to jwtverify rejected
 *   - Non-base64 signature segment rejected
 *   - Verify with empty secret (sign + verify round-trip, cross-secret fail)
 *   - Expired-by-1-second boundary → expired
 *   - Token with future exp → not expired
 *   - jwtexpired with NULL/malformed token → returns 1
 *   - API key is URL-safe base64 (no +/=/), correct length (43 chars)
 *   - apikeyvalidate with NULL inputs → returns 0
 *   - bearerextract: empty string, "Bearer" without space, double-space,
 *     token with spaces, all-uppercase BEARER
 *   - JWT sign/verify with minimal claims (no subject/issuer)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
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
    /* auth_apikeygenerate (canonical name)                                */
    /* ------------------------------------------------------------------ */

    const char *key3 = auth_apikeygenerate();
    ASSERT(key3 != NULL,       "apikeygenerate returns non-NULL");
    ASSERT(key3 && strlen(key3) > 0, "apikeygenerate returns non-empty string");
    free((void *)key3);

    /* ------------------------------------------------------------------ */
    /* auth_bearerextract                                                  */
    /* ------------------------------------------------------------------ */

    /* Happy path: standard Bearer header. */
    BearerResult br1 = auth_bearerextract("Bearer eyJhbGciOiJIUzI1NiJ9");
    ASSERT(br1.is_err == 0, "bearerextract valid header is_err==0");
    ASSERT_STREQ(br1.ok, "eyJhbGciOiJIUzI1NiJ9",
                 "bearerextract extracts token");
    free((void *)br1.ok);

    /* Not a Bearer token (Basic auth). */
    BearerResult br2 = auth_bearerextract("Basic dXNlcjpwYXNz");
    ASSERT(br2.is_err == 1, "bearerextract Basic scheme returns is_err=1");
    ASSERT(br2.ok == NULL,  "bearerextract Basic scheme ok==NULL");

    /* NULL input. */
    BearerResult br3 = auth_bearerextract(NULL);
    ASSERT(br3.is_err == 1, "bearerextract NULL returns is_err=1");

    /* Empty Bearer token ("Bearer " with nothing after). */
    BearerResult br4 = auth_bearerextract("Bearer ");
    ASSERT(br4.is_err == 1, "bearerextract empty token returns is_err=1");

    /* Case mismatch ("bearer" lowercase). */
    BearerResult br5 = auth_bearerextract("bearer token123");
    ASSERT(br5.is_err == 1, "bearerextract lowercase bearer returns is_err=1");

    /* No space after Bearer. */
    BearerResult br6 = auth_bearerextract("BearerToken123");
    ASSERT(br6.is_err == 1, "bearerextract no space returns is_err=1");

    /* ------------------------------------------------------------------ */
    /* Story 20.1.3 — Hardened edge-case tests                             */
    /* ------------------------------------------------------------------ */

    /* -- JWT with extra dots (4+ segments) -- */
    {
        JwtVerifyResult r = auth_jwtverify("a.b.c.d", secret);
        ASSERT(r.is_err == 1,
               "jwtverify rejects token with 4 segments (extra dot)");
    }

    {
        JwtVerifyResult r = auth_jwtverify("a.b.c.d.e", secret);
        ASSERT(r.is_err == 1,
               "jwtverify rejects token with 5 segments");
    }

    /* -- JWT with too few segments -- */
    {
        JwtVerifyResult r = auth_jwtverify("nodots", secret);
        ASSERT(r.is_err == 1,
               "jwtverify rejects token with 0 dots");
    }

    {
        JwtVerifyResult r = auth_jwtverify("one.dot", secret);
        ASSERT(r.is_err == 1,
               "jwtverify rejects token with 1 dot");
    }

    /* -- JWT with empty segments -- */
    {
        JwtVerifyResult r = auth_jwtverify("..", secret);
        ASSERT(r.is_err == 1,
               "jwtverify rejects token with all-empty segments");
    }

    {
        JwtVerifyResult r = auth_jwtverify("a..c", secret);
        ASSERT(r.is_err == 1,
               "jwtverify rejects token with empty middle segment");
    }

    /* -- NULL token to jwtverify -- */
    {
        JwtVerifyResult r = auth_jwtverify(NULL, secret);
        ASSERT(r.is_err == 1,
               "jwtverify NULL token returns is_err=1");
    }

    /* -- Empty string token to jwtverify -- */
    {
        JwtVerifyResult r = auth_jwtverify("", secret);
        ASSERT(r.is_err == 1,
               "jwtverify empty string returns is_err=1");
    }

    /* -- JWT with non-base64 signature segment -- */
    {
        /* Take a valid token's header.payload, append garbage signature */
        JwtClaims nc;
        memset(&nc, 0, sizeof(nc));
        nc.subject   = "nonb64";
        nc.issuer    = "test";
        nc.issued_at = 100;
        JwtResult nr = auth_jwtsign(nc, secret);
        ASSERT(nr.is_err == 0, "jwtsign for non-b64 test ok");
        if (nr.ok) {
            /* Replace signature with non-base64 characters */
            char *mangled = strdup(nr.ok);
            /* Find last dot */
            char *last_dot = strrchr(mangled, '.');
            if (last_dot && strlen(last_dot + 1) >= 4) {
                /* Insert invalid chars */
                last_dot[1] = '!';
                last_dot[2] = '@';
                last_dot[3] = '#';
                last_dot[4] = '$';
            }
            JwtVerifyResult vr = auth_jwtverify(mangled, secret);
            ASSERT(vr.is_err == 1,
                   "jwtverify rejects non-base64 signature chars");
            free(mangled);
            free((void *)nr.ok);
        }
    }

    /* -- Verify with empty secret -- */
    {
        ByteArray empty_secret;
        empty_secret.data = (const uint8_t *)"";
        empty_secret.len  = 0;

        /* Sign with empty secret, then verify with empty secret */
        JwtClaims ec;
        memset(&ec, 0, sizeof(ec));
        ec.subject   = "empty-sec";
        ec.issuer    = "test";
        ec.issued_at = 100;
        JwtResult er = auth_jwtsign(ec, empty_secret);
        ASSERT(er.is_err == 0,
               "jwtsign with empty secret succeeds");
        if (er.ok) {
            JwtVerifyResult evr = auth_jwtverify(er.ok, empty_secret);
            ASSERT(evr.is_err == 0,
                   "jwtverify with matching empty secret succeeds");
            free((void *)evr.ok.subject);
            free((void *)evr.ok.issuer);

            /* Verify same token with non-empty secret should fail */
            JwtVerifyResult evr2 = auth_jwtverify(er.ok, secret);
            ASSERT(evr2.is_err == 1,
                   "jwtverify empty-secret token fails with real secret");
            free((void *)er.ok);
        }
    }

    /* -- Expired-by-1-second boundary -- */
    {
        int64_t now = (int64_t)time(NULL);
        JwtClaims bc;
        memset(&bc, 0, sizeof(bc));
        bc.subject    = "boundary";
        bc.issuer     = "test";
        bc.issued_at  = now - 10;
        bc.expires_at = now - 1;  /* expired 1 second ago */

        JwtResult br = auth_jwtsign(bc, secret);
        ASSERT(br.is_err == 0, "jwtsign boundary-exp token ok");
        if (br.ok) {
            int exp_check = auth_jwtexpired(br.ok);
            ASSERT(exp_check == 1,
                   "jwtexpired returns 1 for token expired by 1 second");
            free((void *)br.ok);
        }
    }

    /* -- Token with exp far in the future -- */
    {
        int64_t now = (int64_t)time(NULL);
        JwtClaims fc;
        memset(&fc, 0, sizeof(fc));
        fc.subject    = "future";
        fc.issuer     = "test";
        fc.issued_at  = now;
        fc.expires_at = now + 86400;  /* 24 hours from now */

        JwtResult fr = auth_jwtsign(fc, secret);
        ASSERT(fr.is_err == 0, "jwtsign future-exp token ok");
        if (fr.ok) {
            int exp_check = auth_jwtexpired(fr.ok);
            ASSERT(exp_check == 0,
                   "jwtexpired returns 0 for token with future exp");
            free((void *)fr.ok);
        }
    }

    /* -- auth_jwtexpired with NULL token -- */
    {
        ASSERT(auth_jwtexpired(NULL) == 1,
               "jwtexpired NULL token returns 1");
    }

    /* -- auth_jwtexpired with malformed token -- */
    {
        ASSERT(auth_jwtexpired("not-a-jwt") == 1,
               "jwtexpired malformed token returns 1");
    }

    /* -- API key generation: keys are URL-safe base64 (no +/= chars) -- */
    {
        const char *k = auth_apikeygenerate();
        ASSERT(k != NULL, "apikeygenerate result is non-NULL");
        if (k) {
            ASSERT(strchr(k, '+') == NULL,
                   "api key has no '+' (URL-safe base64)");
            ASSERT(strchr(k, '/') == NULL,
                   "api key has no '/' (URL-safe base64)");
            ASSERT(strchr(k, '=') == NULL,
                   "api key has no '=' (no padding)");
            /* 32 bytes in URL-safe base64 without padding = 43 chars */
            ASSERT(strlen(k) == 43,
                   "api key is 43 chars (32 bytes in base64url)");
            free((void *)k);
        }
    }

    /* -- API key validate: NULL inputs -- */
    {
        ASSERT(auth_apikeyvalidate(NULL, "abc") == 0,
               "apikeyvalidate NULL provided returns 0");
        ASSERT(auth_apikeyvalidate("abc", NULL) == 0,
               "apikeyvalidate NULL stored returns 0");
        ASSERT(auth_apikeyvalidate(NULL, NULL) == 0,
               "apikeyvalidate both NULL returns 0");
    }

    /* -- Bearer extraction: empty string input -- */
    {
        BearerResult br_empty = auth_bearerextract("");
        ASSERT(br_empty.is_err == 1,
               "bearerextract empty string returns is_err=1");
    }

    /* -- Bearer extraction: "Bearer" without trailing space -- */
    {
        BearerResult br_nospace = auth_bearerextract("Bearer");
        ASSERT(br_nospace.is_err == 1,
               "bearerextract 'Bearer' without space returns is_err=1");
    }

    /* -- Bearer extraction: extra whitespace -- */
    {
        BearerResult br_extra = auth_bearerextract("Bearer  token123");
        ASSERT(br_extra.is_err == 0,
               "bearerextract double-space is_err==0 (preserves extra space)");
        if (br_extra.ok) {
            ASSERT_STREQ(br_extra.ok, " token123",
                         "bearerextract double-space includes leading space in token");
            free((void *)br_extra.ok);
        }
    }

    /* -- Bearer extraction: token with spaces -- */
    {
        BearerResult br_sp = auth_bearerextract("Bearer abc def ghi");
        ASSERT(br_sp.is_err == 0,
               "bearerextract token with spaces is_err==0");
        if (br_sp.ok) {
            ASSERT_STREQ(br_sp.ok, "abc def ghi",
                         "bearerextract preserves remainder after 'Bearer '");
            free((void *)br_sp.ok);
        }
    }

    /* -- Bearer extraction: "BEARER" all uppercase -- */
    {
        BearerResult br_upper = auth_bearerextract("BEARER token123");
        ASSERT(br_upper.is_err == 1,
               "bearerextract all-uppercase BEARER returns is_err=1");
    }

    /* -- JWT sign with minimal claims (no subject, no issuer) -- */
    {
        JwtClaims mc;
        memset(&mc, 0, sizeof(mc));
        mc.issued_at = 100;

        JwtResult mr = auth_jwtsign(mc, secret);
        ASSERT(mr.is_err == 0,
               "jwtsign with no subject/issuer succeeds");
        if (mr.ok) {
            JwtVerifyResult mvr = auth_jwtverify(mr.ok, secret);
            ASSERT(mvr.is_err == 0,
                   "jwtverify minimal claims is_err==0");
            ASSERT(mvr.ok.subject == NULL,
                   "jwtverify minimal claims subject is NULL");
            ASSERT(mvr.ok.issuer == NULL,
                   "jwtverify minimal claims issuer is NULL");
            free((void *)mr.ok);
        }
    }

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

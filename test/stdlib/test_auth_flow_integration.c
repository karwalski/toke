/*
 * test_auth_flow_integration.c — Real-world integration test:
 * Auth-protected API flow: keygen -> JWT -> verify -> encrypt payload.
 *
 * Story: 21.1.2
 *
 * Build:
 *   $(CC) -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *       -o test_auth_flow_integration test_auth_flow_integration.c \
 *       ../../src/stdlib/auth.c ../../src/stdlib/encrypt.c \
 *       ../../src/stdlib/encoding.c ../../src/stdlib/crypto.c \
 *       ../../src/stdlib/json.c
 *
 * Scenarios:
 *   1. Generate API key, sign JWT, build Bearer header, build JSON payload,
 *      encrypt with AES-256-GCM, decrypt, verify JWT, check payload match.
 *   2. Expired JWT should fail expiry check.
 *   3. Wrong key should fail decrypt.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../../src/stdlib/auth.h"
#include "../../src/stdlib/encrypt.h"
#include "../../src/stdlib/encoding.h"
#include "../../src/stdlib/crypto.h"
#include "../../src/stdlib/json.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

/* Build a ByteArray from a string literal (no NUL included). */
static ByteArray ba_from_str(const char *s)
{
    ByteArray b;
    b.data = (const uint8_t *)s;
    b.len  = (uint64_t)strlen(s);
    return b;
}

/* Build a ByteArray from raw bytes. */
static ByteArray ba_from_bytes(const uint8_t *data, uint64_t len)
{
    ByteArray b;
    b.data = data;
    b.len  = len;
    return b;
}

/* -----------------------------------------------------------------------
 * Test 1: Full auth-protected API flow (happy path)
 *
 * Simulates: client generates API key, signs a JWT for auth, builds a
 * Bearer header, constructs a JSON payload, encrypts it with AES-256-GCM,
 * then the "server side" decrypts and verifies the JWT.
 * ----------------------------------------------------------------------- */
static void test_auth_protected_api_flow(void)
{
    printf("\n--- Test 1: Auth-protected API flow (happy path) ---\n");

    /* Step 1: Generate an API key */
    const char *api_key = auth_apikeygenerate();
    ASSERT(api_key != NULL, "apikeygenerate returns non-NULL");
    ASSERT(strlen(api_key) > 0, "apikeygenerate returns non-empty key");

    /* Step 2: Sign a JWT with subject, issuer, and expiry */
    ByteArray jwt_secret = ba_from_str("integration-test-secret-key-256!");

    JwtClaims claims;
    memset(&claims, 0, sizeof(claims));
    claims.subject    = "user:42";
    claims.issuer     = "api-gateway";
    claims.issued_at  = (int64_t)time(NULL);
    claims.expires_at = (int64_t)time(NULL) + 3600;
    claims.audience   = NULL;

    JwtResult jwt_res = auth_jwtsign(claims, jwt_secret);
    ASSERT(jwt_res.is_err == 0, "jwtsign succeeds");
    ASSERT(jwt_res.ok != NULL, "jwtsign returns token");

    /* Step 3: Encode JWT into Authorization: Bearer header */
    size_t header_len = strlen("Bearer ") + strlen(jwt_res.ok) + 1;
    char *auth_header = (char *)malloc(header_len);
    ASSERT(auth_header != NULL, "malloc for auth header succeeds");
    snprintf(auth_header, header_len, "Bearer %s", jwt_res.ok);

    /* Extract token back from the header */
    BearerResult bearer = auth_bearerextract(auth_header);
    ASSERT(bearer.is_err == 0, "bearerextract succeeds");
    ASSERT(bearer.ok != NULL, "bearerextract returns token");
    ASSERT_STREQ(bearer.ok, jwt_res.ok,
                 "extracted token matches original JWT");

    /* Step 4: Build a JSON payload string */
    const char *payload_str =
        "{\"api_key\":\"REDACTED\",\"action\":\"transfer\","
        "\"amount\":1500,\"currency\":\"USD\"}";

    /* Step 5: Encrypt the payload with AES-256-GCM */
    ByteArray aes_key = encrypt_aes256gcm_keygen();
    ASSERT(aes_key.len == 32, "AES keygen returns 32-byte key");

    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ASSERT(nonce.len == 12, "AES noncegen returns 12-byte nonce");

    ByteArray plaintext = ba_from_str(payload_str);
    ByteArray aad = ba_from_str("api-v1");

    EncryptResult enc = encrypt_aes256gcm_encrypt(aes_key, nonce,
                                                   plaintext, aad);
    ASSERT(enc.is_err == 0, "AES-256-GCM encrypt succeeds");
    ASSERT(enc.ok != NULL, "AES-256-GCM encrypt returns ciphertext");
    ASSERT(enc.ok_len > plaintext.len,
           "ciphertext is longer than plaintext (includes tag)");

    /* Step 6: Decrypt on the "server side" */
    ByteArray ciphertext = ba_from_bytes(enc.ok, enc.ok_len);
    EncryptResult dec = encrypt_aes256gcm_decrypt(aes_key, nonce,
                                                   ciphertext, aad);
    ASSERT(dec.is_err == 0, "AES-256-GCM decrypt succeeds");
    ASSERT(dec.ok != NULL, "AES-256-GCM decrypt returns plaintext");
    ASSERT(dec.ok_len == plaintext.len,
           "decrypted length matches original payload length");

    /* Step 7: Verify the JWT on the server side */
    JwtVerifyResult verify = auth_jwtverify(bearer.ok, jwt_secret);
    ASSERT(verify.is_err == 0, "jwtverify succeeds with correct secret");
    ASSERT_STREQ(verify.ok.subject, "user:42",
                 "verified JWT subject matches 'user:42'");
    ASSERT_STREQ(verify.ok.issuer, "api-gateway",
                 "verified JWT issuer matches 'api-gateway'");
    ASSERT(verify.ok.expires_at > 0,
           "verified JWT has non-zero expiry");

    /* Confirm token is not expired */
    int expired = auth_jwtexpired(bearer.ok);
    ASSERT(expired == 0, "JWT with future expiry is not expired");

    /* Step 8: Check the decrypted payload matches original */
    ASSERT(memcmp(dec.ok, payload_str, dec.ok_len) == 0,
           "decrypted payload matches original JSON payload");

    /* Validate the API key against itself */
    ASSERT(auth_apikeyvalidate(api_key, api_key) == 1,
           "API key validates against itself");

    /* Cleanup */
    free((void *)api_key);
    free((void *)jwt_res.ok);
    free(auth_header);
    free((void *)bearer.ok);
    free((void *)aes_key.data);
    free((void *)nonce.data);
    free(enc.ok);
    free(dec.ok);
    free((void *)verify.ok.subject);
    free((void *)verify.ok.issuer);
}

/* -----------------------------------------------------------------------
 * Test 2: Expired JWT should fail expiry check
 * ----------------------------------------------------------------------- */
static void test_expired_jwt_fails_verify(void)
{
    printf("\n--- Test 2: Expired JWT fails expiry check ---\n");

    ByteArray secret = ba_from_str("expired-jwt-test-secret-key-256!");

    /* Sign a JWT that expired 10 seconds ago */
    JwtClaims claims;
    memset(&claims, 0, sizeof(claims));
    claims.subject    = "expired-user";
    claims.issuer     = "api-gateway";
    claims.issued_at  = (int64_t)time(NULL) - 3600;
    claims.expires_at = (int64_t)time(NULL) - 10;
    claims.audience   = NULL;

    JwtResult jwt_res = auth_jwtsign(claims, secret);
    ASSERT(jwt_res.is_err == 0, "jwtsign for expired token succeeds (signing is allowed)");
    ASSERT(jwt_res.ok != NULL, "jwtsign returns expired token");

    /* Signature verification still passes (jwtverify does not check expiry) */
    JwtVerifyResult verify = auth_jwtverify(jwt_res.ok, secret);
    ASSERT(verify.is_err == 0,
           "jwtverify signature check passes for expired token");

    /* But the expiry check should detect it */
    int expired = auth_jwtexpired(jwt_res.ok);
    ASSERT(expired == 1,
           "auth_jwtexpired returns 1 for token expired 10 seconds ago");

    /* Build a Bearer header and extract — should still work structurally */
    size_t hdr_len = strlen("Bearer ") + strlen(jwt_res.ok) + 1;
    char *auth_header = (char *)malloc(hdr_len);
    ASSERT(auth_header != NULL, "malloc for expired bearer header succeeds");
    snprintf(auth_header, hdr_len, "Bearer %s", jwt_res.ok);

    BearerResult bearer = auth_bearerextract(auth_header);
    ASSERT(bearer.is_err == 0, "bearerextract works on expired token structurally");

    /* But a real server would reject after checking expiry */
    int expired_again = auth_jwtexpired(bearer.ok);
    ASSERT(expired_again == 1,
           "extracted expired token still fails expiry check");

    /* Cleanup */
    free((void *)jwt_res.ok);
    free(auth_header);
    free((void *)bearer.ok);
    free((void *)verify.ok.subject);
    free((void *)verify.ok.issuer);
}

/* -----------------------------------------------------------------------
 * Test 3: Wrong AES key fails decrypt
 * ----------------------------------------------------------------------- */
static void test_wrong_key_fails_decrypt(void)
{
    printf("\n--- Test 3: Wrong AES key fails decrypt ---\n");

    /* Encrypt with key A */
    ByteArray key_a = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ASSERT(key_a.len == 32, "key_a is 32 bytes");
    ASSERT(nonce.len == 12, "nonce is 12 bytes");

    const char *payload_str = "{\"secret\":\"launch_codes\",\"value\":42}";
    ByteArray plaintext = ba_from_str(payload_str);
    ByteArray aad = ba_from_str("wrong-key-test");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key_a, nonce,
                                                   plaintext, aad);
    ASSERT(enc.is_err == 0, "encrypt with key_a succeeds");
    ASSERT(enc.ok != NULL, "encrypt returns ciphertext");

    /* Try to decrypt with key B — should fail */
    ByteArray key_b = encrypt_aes256gcm_keygen();
    ASSERT(key_b.len == 32, "key_b is 32 bytes");

    /* Ensure the two keys are actually different */
    ASSERT(memcmp(key_a.data, key_b.data, 32) != 0,
           "key_a and key_b are different (random)");

    ByteArray ciphertext = ba_from_bytes(enc.ok, enc.ok_len);
    EncryptResult dec = encrypt_aes256gcm_decrypt(key_b, nonce,
                                                   ciphertext, aad);
    ASSERT(dec.is_err == 1,
           "decrypt with wrong key returns is_err=1 (auth failure)");

    /* Decrypt with the correct key — should succeed */
    EncryptResult dec_ok = encrypt_aes256gcm_decrypt(key_a, nonce,
                                                      ciphertext, aad);
    ASSERT(dec_ok.is_err == 0,
           "decrypt with correct key_a succeeds");
    ASSERT(dec_ok.ok_len == plaintext.len,
           "correct-key decrypt length matches original");
    ASSERT(memcmp(dec_ok.ok, payload_str, dec_ok.ok_len) == 0,
           "correct-key decrypted payload matches original");

    /* Cleanup */
    free((void *)key_a.data);
    free((void *)key_b.data);
    free((void *)nonce.data);
    free(enc.ok);
    if (dec.ok) free(dec.ok);
    free(dec_ok.ok);
}

/* -----------------------------------------------------------------------
 * Test 4: Wrong JWT secret fails signature verify
 * ----------------------------------------------------------------------- */
static void test_wrong_jwt_secret_fails_verify(void)
{
    printf("\n--- Test 4: Wrong JWT secret fails signature verify ---\n");

    ByteArray secret_a = ba_from_str("correct-jwt-secret-for-signing!");
    ByteArray secret_b = ba_from_str("wrong-jwt-secret-should-not-work");

    JwtClaims claims;
    memset(&claims, 0, sizeof(claims));
    claims.subject    = "secure-user";
    claims.issuer     = "auth-service";
    claims.issued_at  = (int64_t)time(NULL);
    claims.expires_at = (int64_t)time(NULL) + 3600;
    claims.audience   = NULL;

    JwtResult jwt_res = auth_jwtsign(claims, secret_a);
    ASSERT(jwt_res.is_err == 0, "jwtsign with secret_a succeeds");
    ASSERT(jwt_res.ok != NULL, "jwtsign returns token");

    /* Verify with correct secret */
    JwtVerifyResult verify_ok = auth_jwtverify(jwt_res.ok, secret_a);
    ASSERT(verify_ok.is_err == 0,
           "jwtverify with correct secret succeeds");
    ASSERT_STREQ(verify_ok.ok.subject, "secure-user",
                 "correct-secret verify recovers subject");

    /* Verify with wrong secret — should fail */
    JwtVerifyResult verify_fail = auth_jwtverify(jwt_res.ok, secret_b);
    ASSERT(verify_fail.is_err == 1,
           "jwtverify with wrong secret returns is_err=1");

    /* Cleanup */
    free((void *)jwt_res.ok);
    free((void *)verify_ok.ok.subject);
    free((void *)verify_ok.ok.issuer);
}

/* -----------------------------------------------------------------------
 * Test 5: End-to-end with JSON round-trip through encryption
 * ----------------------------------------------------------------------- */
static void test_json_payload_encrypt_roundtrip(void)
{
    printf("\n--- Test 5: JSON payload encrypt round-trip ---\n");

    /* Build a JSON payload using json_enc for a value */
    const char *encoded_action = json_enc("transfer");
    ASSERT(encoded_action != NULL, "json_enc returns non-NULL");

    /* Construct the full JSON object manually */
    const char *json_payload =
        "{\"action\":\"transfer\",\"from\":\"alice\",\"to\":\"bob\","
        "\"amount\":250,\"memo\":\"rent payment\"}";

    /* Encrypt */
    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray pt    = ba_from_str(json_payload);
    ByteArray aad   = ba_from_str("json-roundtrip-v1");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, pt, aad);
    ASSERT(enc.is_err == 0, "encrypt JSON payload succeeds");

    /* Decrypt */
    ByteArray ct = ba_from_bytes(enc.ok, enc.ok_len);
    EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonce, ct, aad);
    ASSERT(dec.is_err == 0, "decrypt JSON payload succeeds");
    ASSERT(dec.ok_len == pt.len, "decrypted length matches");

    /* NUL-terminate the decrypted bytes for JSON parsing */
    char *dec_str = (char *)malloc(dec.ok_len + 1);
    ASSERT(dec_str != NULL, "malloc for decrypted string succeeds");
    memcpy(dec_str, dec.ok, dec.ok_len);
    dec_str[dec.ok_len] = '\0';

    /* Parse the decrypted JSON and verify fields */
    JsonResult parsed = json_dec(dec_str);
    ASSERT(parsed.is_err == 0, "json_dec parses decrypted payload");

    StrJsonResult action = json_str(parsed.ok, "action");
    ASSERT(action.is_err == 0, "json_str extracts 'action'");
    ASSERT_STREQ(action.ok, "transfer",
                 "decrypted JSON 'action' is 'transfer'");

    StrJsonResult from = json_str(parsed.ok, "from");
    ASSERT(from.is_err == 0, "json_str extracts 'from'");
    ASSERT_STREQ(from.ok, "alice", "decrypted JSON 'from' is 'alice'");

    StrJsonResult to = json_str(parsed.ok, "to");
    ASSERT(to.is_err == 0, "json_str extracts 'to'");
    ASSERT_STREQ(to.ok, "bob", "decrypted JSON 'to' is 'bob'");

    I64JsonResult amount = json_i64(parsed.ok, "amount");
    ASSERT(amount.is_err == 0, "json_i64 extracts 'amount'");
    ASSERT(amount.ok == 250, "decrypted JSON 'amount' is 250");

    /* Cleanup */
    free((void *)encoded_action);
    free((void *)key.data);
    free((void *)nonce.data);
    free(enc.ok);
    free(dec.ok);
    free(dec_str);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("=== Auth-protected API flow integration tests (Story 21.1.2) ===\n");

    test_auth_protected_api_flow();
    test_expired_jwt_fails_verify();
    test_wrong_key_fails_decrypt();
    test_wrong_jwt_secret_fails_verify();
    test_json_payload_encrypt_roundtrip();

    printf("\n=== Results: %d failure(s) ===\n", failures);
    return failures > 0 ? 1 : 0;
}

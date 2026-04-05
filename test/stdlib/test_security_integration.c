/*
 * test_security_integration.c — Integration tests chaining all 3 security
 * stdlib modules: encoding, encrypt, auth, crypto.  Story 14.1.4.
 *
 * Build:
 *   $(CC) $(CFLAGS) -o test_security_integration test_security_integration.c \
 *       ../src/stdlib/encoding.c ../src/stdlib/encrypt.c \
 *       ../src/stdlib/auth.c ../src/stdlib/crypto.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../../src/stdlib/encoding.h"
#include "../../src/stdlib/encrypt.h"
#include "../../src/stdlib/auth.h"
#include "../../src/stdlib/crypto.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_CONTAINS(haystack, needle, msg) \
    ASSERT((haystack) && (needle) && strstr((haystack), (needle)) != NULL, msg)

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
 * Test 1: API key → JWT → verify pipeline
 * ----------------------------------------------------------------------- */
static void test_apikey_jwt_pipeline(void)
{
    printf("\n--- Test 1: API key -> JWT -> verify pipeline ---\n");

    const char *apikey = auth_apikeygen();
    ASSERT(apikey != NULL, "apikeygen returns non-NULL");
    ASSERT(strlen(apikey) > 0, "apikeygen returns non-empty string");

    ByteArray secret = ba_from_str(apikey);

    JwtClaims claims;
    claims.subject    = "alice";
    claims.issuer     = "test-service";
    claims.issued_at  = 0;
    claims.expires_at = time(NULL) + 3600;
    claims.audience   = NULL;

    JwtResult signed_jwt = auth_jwtsign(claims, secret);
    ASSERT(signed_jwt.is_err == 0, "jwtsign with apikey secret succeeds");
    ASSERT(signed_jwt.ok != NULL, "jwtsign returns token string");

    JwtVerifyResult verified = auth_jwtverify(signed_jwt.ok, secret);
    ASSERT(verified.is_err == 0, "jwtverify with apikey secret succeeds");
    ASSERT(verified.ok.subject != NULL, "verified claims have subject");
    ASSERT(verified.ok.issuer  != NULL, "verified claims have issuer");
    ASSERT(strcmp(verified.ok.subject, "alice")        == 0, "subject matches");
    ASSERT(strcmp(verified.ok.issuer,  "test-service") == 0, "issuer matches");

    free((void *)apikey);
    free((void *)signed_jwt.ok);
}

/* -----------------------------------------------------------------------
 * Test 2: Encoding round-trips
 * ----------------------------------------------------------------------- */
static void test_encoding_roundtrips(void)
{
    printf("\n--- Test 2: Encoding round-trips ---\n");

    /* Use a fixed 32-byte value so the test is deterministic. */
    static const uint8_t raw[32] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18,
        0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
    };

    ByteArray src = ba_from_bytes(raw, 32);

    /* b64url round-trip */
    const char *b64url = encoding_b64urlencode(src);
    ASSERT(b64url != NULL, "b64urlencode returns non-NULL");
    ASSERT(strlen(b64url) > 0, "b64urlencode returns non-empty");

    ByteArray decoded_b64 = encoding_b64urldecode(b64url);
    ASSERT(decoded_b64.len == 32, "b64urldecode gives back 32 bytes");
    ASSERT(decoded_b64.data != NULL, "b64urldecode gives back non-NULL data");
    ASSERT(memcmp(decoded_b64.data, raw, 32) == 0,
           "b64url round-trip bytes match original");

    /* hex round-trip */
    const char *hexstr = encoding_hexencode(src);
    ASSERT(hexstr != NULL, "hexencode returns non-NULL");
    ASSERT(strlen(hexstr) == 64, "hexencode produces 64 chars for 32 bytes");

    ByteArray decoded_hex = encoding_hexdecode(hexstr);
    ASSERT(decoded_hex.len == 32, "hexdecode gives back 32 bytes");
    ASSERT(decoded_hex.data != NULL, "hexdecode gives back non-NULL data");
    ASSERT(memcmp(decoded_hex.data, raw, 32) == 0,
           "hex round-trip bytes match original");

    free((void *)b64url);
    free((void *)decoded_b64.data);
    free((void *)hexstr);
    free((void *)decoded_hex.data);
}

/* -----------------------------------------------------------------------
 * Test 3: HKDF key derivation pipeline
 * ----------------------------------------------------------------------- */
static void test_hkdf_key_derivation(void)
{
    printf("\n--- Test 3: HKDF key derivation pipeline ---\n");

    static const uint8_t shared_secret[32] = {
        0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00, 0x11,
        0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x90, 0xa0, 0xb0, 0xc0, 0xd0, 0xe0, 0xf0, 0x01
    };

    static const uint8_t salt[16] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10
    };

    ByteArray ikm  = ba_from_bytes(shared_secret, 32);
    ByteArray salt_ba = ba_from_bytes(salt, 16);
    ByteArray info_enc = ba_from_str("encrypt-key");
    ByteArray info_mac = ba_from_str("mac-key");

    ByteArray key_enc = encrypt_hkdf_sha256(ikm, salt_ba, info_enc, 32);
    ByteArray key_mac = encrypt_hkdf_sha256(ikm, salt_ba, info_mac, 32);

    ASSERT(key_enc.len == 32, "HKDF encrypt-key is 32 bytes");
    ASSERT(key_mac.len == 32, "HKDF mac-key is 32 bytes");
    ASSERT(key_enc.data != NULL, "HKDF encrypt-key data non-NULL");
    ASSERT(key_mac.data != NULL, "HKDF mac-key data non-NULL");
    ASSERT(memcmp(key_enc.data, key_mac.data, 32) != 0,
           "HKDF keys derived with different info labels are not equal");

    free((void *)key_enc.data);
    free((void *)key_mac.data);
}

/* -----------------------------------------------------------------------
 * Test 4: X25519 key exchange → AES-GCM encryption
 * ----------------------------------------------------------------------- */
static void test_x25519_aesgcm(void)
{
    printf("\n--- Test 4: X25519 key exchange -> AES-GCM encryption ---\n");

    X25519Keypair alice = encrypt_x25519_keypair();
    X25519Keypair bob   = encrypt_x25519_keypair();

    ByteArray alice_priv = ba_from_bytes(alice.privkey, 32);
    ByteArray alice_pub  = ba_from_bytes(alice.pubkey,  32);
    ByteArray bob_priv   = ba_from_bytes(bob.privkey,   32);
    ByteArray bob_pub    = ba_from_bytes(bob.pubkey,    32);

    ByteArray shared_ab = encrypt_x25519_dh(alice_priv, bob_pub);
    ByteArray shared_ba = encrypt_x25519_dh(bob_priv,   alice_pub);

    ASSERT(shared_ab.len == 32, "DH(Alice, Bob) is 32 bytes");
    ASSERT(shared_ba.len == 32, "DH(Bob, Alice) is 32 bytes");
    ASSERT(memcmp(shared_ab.data, shared_ba.data, 32) == 0,
           "X25519 DH shared secrets are equal (ECDH symmetry)");

    /* Use the shared secret directly as AES-256-GCM key */
    ByteArray aes_key = shared_ab;
    ByteArray nonce   = encrypt_aes256gcm_noncegen();
    ASSERT(nonce.len == 12, "noncegen produces 12 bytes");

    const char *plaintext_str = "Hello from X25519+AES-GCM!";
    ByteArray plaintext = ba_from_str(plaintext_str);
    ByteArray aad       = ba_from_str("x25519-test");

    EncryptResult enc = encrypt_aes256gcm_encrypt(aes_key, nonce, plaintext, aad);
    ASSERT(enc.is_err == 0, "AES-GCM encrypt with X25519 shared secret succeeds");
    ASSERT(enc.ok != NULL,  "AES-GCM encrypt returns ciphertext");

    ByteArray ciphertext = ba_from_bytes(enc.ok, enc.ok_len);
    EncryptResult dec = encrypt_aes256gcm_decrypt(aes_key, nonce, ciphertext, aad);
    ASSERT(dec.is_err == 0, "AES-GCM decrypt with X25519 shared secret succeeds");
    ASSERT(dec.ok_len == (uint64_t)strlen(plaintext_str),
           "Decrypted length matches original plaintext length");
    ASSERT(memcmp(dec.ok, plaintext_str, dec.ok_len) == 0,
           "X25519 -> AES-GCM round-trip plaintext matches");

    free((void *)shared_ab.data);
    free((void *)shared_ba.data);
    free((void *)nonce.data);
    free(enc.ok);
    free(dec.ok);
}

/* -----------------------------------------------------------------------
 * Test 5: Ed25519 sign + hex encode
 * ----------------------------------------------------------------------- */
static void test_ed25519_hex_encode(void)
{
    printf("\n--- Test 5: Ed25519 sign + hex encode ---\n");

    Ed25519Keypair kp = encrypt_ed25519_keypair();

    ByteArray priv_ba = ba_from_bytes(kp.privkey, 64);
    ByteArray pub_ba  = ba_from_bytes(kp.pubkey,  32);

    const char *msg_str = "toke security integration test message";
    ByteArray msg = ba_from_str(msg_str);

    ByteArray sig = encrypt_ed25519_sign(priv_ba, msg);
    ASSERT(sig.len == 64, "Ed25519 signature is 64 bytes");
    ASSERT(sig.data != NULL, "Ed25519 signature data non-NULL");

    /* Encode the signature as hex */
    const char *sig_hex = encoding_hexencode(sig);
    ASSERT(sig_hex != NULL, "hex-encoded signature non-NULL");
    ASSERT(strlen(sig_hex) == 128, "hex-encoded 64-byte signature is 128 chars");

    /* Decode hex back to bytes */
    ByteArray sig_decoded = encoding_hexdecode(sig_hex);
    ASSERT(sig_decoded.len == 64, "hex-decoded signature is 64 bytes");
    ASSERT(memcmp(sig_decoded.data, sig.data, 64) == 0,
           "hex round-trip of Ed25519 signature bytes match");

    /* Verify the decoded signature bytes */
    int valid = encrypt_ed25519_verify(pub_ba, msg, sig_decoded);
    ASSERT(valid == 1,
           "Ed25519 verify with hex-round-tripped signature succeeds");

    free((void *)sig.data);
    free((void *)sig_hex);
    free((void *)sig_decoded.data);
}

/* -----------------------------------------------------------------------
 * Test 6: JWT expiry flow
 * ----------------------------------------------------------------------- */
static void test_jwt_expiry(void)
{
    printf("\n--- Test 6: JWT expiry flow ---\n");

    static const uint8_t secret_bytes[32] = {
        0x6a, 0x77, 0x74, 0x2d, 0x65, 0x78, 0x70, 0x69,
        0x72, 0x79, 0x2d, 0x74, 0x65, 0x73, 0x74, 0x2d,
        0x73, 0x65, 0x63, 0x72, 0x65, 0x74, 0x2d, 0x6b,
        0x65, 0x79, 0x21, 0x21, 0x21, 0x21, 0x21, 0x21
    };
    ByteArray secret = ba_from_bytes(secret_bytes, 32);

    /* Token that expired 1 second ago */
    JwtClaims expired_claims;
    expired_claims.subject    = "expiry-test";
    expired_claims.issuer     = "toke";
    expired_claims.issued_at  = 0;
    expired_claims.expires_at = (int64_t)time(NULL) - 1;
    expired_claims.audience   = NULL;

    JwtResult expired_tok = auth_jwtsign(expired_claims, secret);
    ASSERT(expired_tok.is_err == 0, "jwtsign for already-expired token succeeds");
    ASSERT(expired_tok.ok != NULL,  "jwtsign returns token for expired claims");

    int is_expired = auth_jwtexpired(expired_tok.ok);
    ASSERT(is_expired == 1, "auth_jwtexpired returns 1 for past exp");

    /* Token that expires in 1 hour */
    JwtClaims future_claims;
    future_claims.subject    = "expiry-test";
    future_claims.issuer     = "toke";
    future_claims.issued_at  = 0;
    future_claims.expires_at = (int64_t)time(NULL) + 3600;
    future_claims.audience   = NULL;

    JwtResult future_tok = auth_jwtsign(future_claims, secret);
    ASSERT(future_tok.is_err == 0, "jwtsign for future-expiry token succeeds");
    ASSERT(future_tok.ok != NULL,  "jwtsign returns token for future claims");

    int not_expired = auth_jwtexpired(future_tok.ok);
    ASSERT(not_expired == 0, "auth_jwtexpired returns 0 for future exp");

    free((void *)expired_tok.ok);
    free((void *)future_tok.ok);
}

/* -----------------------------------------------------------------------
 * Test 7: HMAC → base64url workflow
 * ----------------------------------------------------------------------- */
static void test_hmac_b64url_workflow(void)
{
    printf("\n--- Test 7: HMAC -> base64url workflow ---\n");

    ByteArray key     = ba_from_str("hmac-test-key-for-integration");
    ByteArray message = ba_from_str("The quick brown fox jumps over the lazy dog");

    ByteArray hmac = crypto_hmac_sha256(key, message);
    ASSERT(hmac.len == 32,       "HMAC-SHA-256 produces 32 bytes");
    ASSERT(hmac.data != NULL,    "HMAC-SHA-256 data non-NULL");

    const char *encoded = encoding_b64urlencode(hmac);
    ASSERT(encoded != NULL,      "b64urlencode of HMAC returns non-NULL");
    ASSERT(strlen(encoded) > 0,  "b64urlencode of HMAC non-empty");

    ByteArray decoded = encoding_b64urldecode(encoded);
    ASSERT(decoded.len == 32,    "b64urldecode gives back 32 bytes");
    ASSERT(decoded.data != NULL, "b64urldecode gives back non-NULL data");
    ASSERT(memcmp(decoded.data, hmac.data, 32) == 0,
           "HMAC -> b64url -> decode round-trip matches");

    free((void *)hmac.data);
    free((void *)encoded);
    free((void *)decoded.data);
}

/* -----------------------------------------------------------------------
 * Test 8: Constant-time key comparison
 * ----------------------------------------------------------------------- */
static void test_constant_time_key_comparison(void)
{
    printf("\n--- Test 8: Constant-time key comparison ---\n");

    const char *key_a = auth_apikeygen();
    const char *key_b = auth_apikeygen();
    ASSERT(key_a != NULL, "apikeygen returns key_a");
    ASSERT(key_b != NULL, "apikeygen returns key_b");

    /* Identical key vs itself */
    ASSERT(auth_apikeyvalidate(key_a, key_a) == 1,
           "apikeyvalidate: same key vs itself returns 1");

    /* Two different generated keys should not match */
    /* (Extremely unlikely to collide for 32-byte random keys.) */
    ASSERT(auth_apikeyvalidate(key_a, key_b) == 0,
           "apikeyvalidate: two distinct random keys returns 0");

    /* One character different: copy key_a and flip one byte */
    size_t key_len = strlen(key_a);
    char *key_a_mut = malloc(key_len + 1);
    ASSERT(key_a_mut != NULL, "malloc for mutated key succeeds");
    memcpy(key_a_mut, key_a, key_len + 1);
    /* Flip the last character: change it to something else */
    key_a_mut[key_len - 1] ^= 0x01;
    /* If the XOR produced a NUL, just use 'X' instead */
    if (key_a_mut[key_len - 1] == '\0') key_a_mut[key_len - 1] = 'X';

    ASSERT(auth_apikeyvalidate(key_a_mut, key_a) == 0,
           "apikeyvalidate: key with one char changed returns 0");

    /* Empty vs empty */
    ASSERT(auth_apikeyvalidate("", "") == 1,
           "apikeyvalidate: empty vs empty returns 1");

    /* Different lengths */
    ASSERT(auth_apikeyvalidate("short", key_a) == 0,
           "apikeyvalidate: different lengths returns 0");

    free((void *)key_a);
    free((void *)key_b);
    free(key_a_mut);
}

/* -----------------------------------------------------------------------
 * Test 9: AES-GCM AAD integrity
 * ----------------------------------------------------------------------- */
static void test_aesgcm_aad_integrity(void)
{
    printf("\n--- Test 9: AES-GCM AAD integrity ---\n");

    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ASSERT(key.len   == 32, "keygen returns 32-byte key");
    ASSERT(nonce.len == 12, "noncegen returns 12-byte nonce");

    ByteArray plaintext = ba_from_str("AES-GCM AAD integrity test payload");
    ByteArray aad_v1    = ba_from_str("v1");
    ByteArray aad_v2    = ba_from_str("v2");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plaintext, aad_v1);
    ASSERT(enc.is_err == 0, "AES-GCM encrypt with AAD v1 succeeds");
    ASSERT(enc.ok != NULL,  "AES-GCM encrypt returns ciphertext");

    ByteArray ciphertext = ba_from_bytes(enc.ok, enc.ok_len);

    /* Decrypt with correct AAD → success */
    EncryptResult dec_ok = encrypt_aes256gcm_decrypt(key, nonce, ciphertext, aad_v1);
    ASSERT(dec_ok.is_err == 0, "AES-GCM decrypt with matching AAD v1 succeeds");

    /* Decrypt with wrong AAD → auth failure */
    EncryptResult dec_fail = encrypt_aes256gcm_decrypt(key, nonce, ciphertext, aad_v2);
    ASSERT(dec_fail.is_err == 1,
           "AES-GCM decrypt with mismatched AAD v2 returns is_err=1");

    free((void *)key.data);
    free((void *)nonce.data);
    free(enc.ok);
    if (dec_ok.ok)   free(dec_ok.ok);
    if (dec_fail.ok) free(dec_fail.ok);
}

/* -----------------------------------------------------------------------
 * Test 10: Full auth flow
 * ----------------------------------------------------------------------- */
static void test_full_auth_flow(void)
{
    printf("\n--- Test 10: Full auth flow ---\n");

    /* Generate API key */
    const char *apikey = auth_apikeygen();
    ASSERT(apikey != NULL, "apikeygen returns key");

    /* Build JWT with sub and iss */
    ByteArray secret = ba_from_str(apikey);

    JwtClaims claims;
    claims.subject    = "alice";
    claims.issuer     = "test-service";
    claims.issued_at  = 0;
    claims.expires_at = (int64_t)time(NULL) + 3600;
    claims.audience   = NULL;

    JwtResult jwt_result = auth_jwtsign(claims, secret);
    ASSERT(jwt_result.is_err == 0, "Full auth flow: jwtsign succeeds");
    ASSERT(jwt_result.ok != NULL,  "Full auth flow: jwtsign returns token");

    /* Verify JWT → confirm sub/iss */
    JwtVerifyResult verify = auth_jwtverify(jwt_result.ok, secret);
    ASSERT(verify.is_err == 0,   "Full auth flow: jwtverify succeeds");
    ASSERT(verify.ok.subject != NULL, "Full auth flow: verified subject non-NULL");
    ASSERT(verify.ok.issuer  != NULL, "Full auth flow: verified issuer non-NULL");
    ASSERT(strcmp(verify.ok.subject, "alice")        == 0,
           "Full auth flow: subject is 'alice'");
    ASSERT(strcmp(verify.ok.issuer,  "test-service") == 0,
           "Full auth flow: issuer is 'test-service'");

    /* Validate API key against itself → 1 */
    ASSERT(auth_apikeyvalidate(apikey, apikey) == 1,
           "Full auth flow: apikey validates against itself");

    /* Validate against a different key → 0 */
    const char *other_key = auth_apikeygen();
    ASSERT(other_key != NULL, "Full auth flow: second apikeygen succeeds");
    ASSERT(auth_apikeyvalidate(apikey, other_key) == 0,
           "Full auth flow: apikey does not validate against different key");

    free((void *)apikey);
    free((void *)jwt_result.ok);
    free((void *)other_key);
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    printf("=== Security stdlib integration tests (Story 14.1.4) ===\n");

    test_apikey_jwt_pipeline();
    test_encoding_roundtrips();
    test_hkdf_key_derivation();
    test_x25519_aesgcm();
    test_ed25519_hex_encode();
    test_jwt_expiry();
    test_hmac_b64url_workflow();
    test_constant_time_key_comparison();
    test_aesgcm_aad_integrity();
    test_full_auth_flow();

    printf("\n=== Results: %d failure(s) ===\n", failures);
    return failures > 0 ? 1 : 0;
}

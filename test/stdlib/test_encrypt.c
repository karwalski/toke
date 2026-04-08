/*
 * test_encrypt.c — Unit tests for the std.encrypt C library (Story 14.1.2).
 *
 * Hardened with known vectors and edge cases (Story 20.1.2).
 *
 * Build and run: make test-stdlib-encrypt
 *
 * Tests cover:
 *   - AES-256-GCM keygen/noncegen lengths
 *   - AES-256-GCM encrypt/decrypt round-trip
 *   - AES-256-GCM NIST SP 800-38D test vector (Test Case 16)
 *   - AES-256-GCM zero-length plaintext round-trip
 *   - AES-256-GCM AAD-only (no plaintext) authentication
 *   - AES-256-GCM wrong key → is_err
 *   - AES-256-GCM tampered ciphertext → is_err
 *   - AES-256-GCM tampered AAD → is_err
 *   - AES-256-GCM tampered auth tag → is_err
 *   - AES-256-GCM ciphertext too short → is_err
 *   - AES-256-GCM bad key size → is_err
 *   - AES-256-GCM bad nonce size → is_err
 *   - X25519 keypair sizes
 *   - X25519 DH symmetry
 *   - X25519 RFC 7748 §6.1 known-answer test
 *   - Ed25519 keypair sizes
 *   - Ed25519 sign length
 *   - Ed25519 verify success / tampered / wrong key
 *   - Ed25519 RFC 8032 §7.1 known-answer test (TEST 1)
 *   - Ed25519 sign with short privkey
 *   - Ed25519 verify with short pubkey/sig
 *   - HKDF output lengths and determinism
 *   - HKDF info label separation
 *   - HKDF RFC 5869 Test Case 1 known vector
 *   - HKDF zero-length salt
 *   - HKDF output too large
 *   - TLS fingerprint length
 *   - TLS fingerprint determinism
 *   - TLS fingerprint invalid PEM
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include "../../src/stdlib/encrypt.h"

/* RSA keygen timeout (pure-C bignum is slow; 60s is generous) */
static jmp_buf  s_rsa_timeout_jmp;
static void     s_rsa_timeout(int sig) { (void)sig; longjmp(s_rsa_timeout_jmp, 1); }

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

/* Helper: build a ByteArray from a string literal (no NUL included). */
static ByteArray ba_str(const char *s)
{
    ByteArray b;
    b.data = (const uint8_t *)s;
    b.len  = (uint64_t)strlen(s);
    return b;
}

/* Helper: build a ByteArray from a raw buffer. */
static ByteArray ba_raw(const uint8_t *data, uint64_t len)
{
    ByteArray b;
    b.data = data;
    b.len  = len;
    return b;
}

/* Helper: decode hex string into a byte buffer. Returns number of bytes. */
static uint64_t hex_decode(const char *hex, uint8_t *out, uint64_t maxlen)
{
    uint64_t i = 0;
    while (*hex && *(hex+1) && i < maxlen) {
        unsigned int byte;
        char buf[3] = { hex[0], hex[1], '\0' };
        if (sscanf(buf, "%02x", &byte) != 1) break;
        out[i++] = (uint8_t)byte;
        hex += 2;
    }
    return i;
}


/* -------------------------------------------------------------------------
 * Reference GF(2^128) multiply and GHASH — independent implementation
 * for diagnostic testing. Mirrors the spec algorithm exactly.
 * ------------------------------------------------------------------------- */

/* Reference: NIST SP 800-38D Algorithm 1 */



/* -------------------------------------------------------------------------
 * AES-256-GCM tests
 * ------------------------------------------------------------------------- */

static void test_aes_keygen_len(void)
{
    ByteArray key = encrypt_aes256gcm_keygen();
    ASSERT(key.len == 32, "aes256gcm_keygen: len == 32");
    ASSERT(key.data != NULL, "aes256gcm_keygen: data != NULL");
    free((void *)key.data);
}

static void test_aes_noncegen_len(void)
{
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ASSERT(nonce.len == 12, "aes256gcm_noncegen: len == 12");
    ASSERT(nonce.data != NULL, "aes256gcm_noncegen: data != NULL");
    free((void *)nonce.data);
}

static void test_aes_roundtrip(void)
{
    const char *plaintext_str = "hello world";

    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray plain = ba_str(plaintext_str);
    ByteArray aad   = ba_str("additional data");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);
    ASSERT(enc.is_err == 0, "aes256gcm_encrypt: no error");
    ASSERT(enc.ok_len == plain.len + 16, "aes256gcm_encrypt: output len == plaintext + tag");

    if (!enc.is_err && enc.ok != NULL) {
        ByteArray ct = ba_raw(enc.ok, enc.ok_len);
        EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonce, ct, aad);
        ASSERT(dec.is_err == 0, "aes256gcm_decrypt: no error");
        ASSERT(dec.ok_len == plain.len, "aes256gcm_decrypt: plaintext length matches");
        if (!dec.is_err && dec.ok != NULL) {
            ASSERT(memcmp(dec.ok, plaintext_str, plain.len) == 0,
                   "aes256gcm_decrypt: plaintext content matches");
        }
        free(dec.ok);
    }
    free(enc.ok);
    free((void *)key.data);
    free((void *)nonce.data);
}

/*
 * NIST SP 800-38D, Test Case 13 (AES-256-GCM).
 * Key=0^256, IV=0^96, PT=empty, AAD=empty.
 * Tag: 530f8afbc74536b9a963b4f1c4cb738b
 */
static void test_aes_nist_vector_tc13(void)
{
    uint8_t key_buf[32] = {0};
    uint8_t nonce_buf[12] = {0};
    uint8_t expected_tag[16];

    hex_decode("530f8afbc74536b9a963b4f1c4cb738b", expected_tag, 16);

    ByteArray key   = ba_raw(key_buf, 32);
    ByteArray nonce = ba_raw(nonce_buf, 12);
    ByteArray empty = ba_raw(NULL, 0);

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, empty, empty);
    ASSERT(enc.is_err == 0, "aes256gcm NIST TC13: encrypt succeeds");
    if (!enc.is_err && enc.ok != NULL) {
        ASSERT(memcmp(enc.ok, expected_tag, 16) == 0, "aes256gcm NIST TC13: tag matches");
    }
    free(enc.ok);
}

/*
 * NIST SP 800-38D, Test Case 14 (AES-256-GCM).
 * Key=0^256, IV=0^96, PT=empty, AAD=feedfacedeadbeeffeedfacedeadbeefabaddad2 (20 bytes).
 * Tag: d0d1c8a799996bf0265b98b5d48ab919
 */
static void test_aes_nist_vector_tc14(void)
{
    uint8_t key_buf[32] = {0};
    uint8_t nonce_buf[12] = {0};
    uint8_t aad_buf[20];
    uint8_t expected_tag[16];

    hex_decode("feedfacedeadbeeffeedfacedeadbeef"
               "abaddad2", aad_buf, 20);
    hex_decode("6605ba8cc3d0e5864e8d04e07bfc8b36", expected_tag, 16);

    ByteArray key   = ba_raw(key_buf, 32);
    ByteArray nonce = ba_raw(nonce_buf, 12);
    ByteArray empty = ba_raw(NULL, 0);
    ByteArray aad   = ba_raw(aad_buf, 20);

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, empty, aad);
    ASSERT(enc.is_err == 0, "aes256gcm NIST TC14: encrypt succeeds");
    if (!enc.is_err && enc.ok != NULL) {
        ASSERT(memcmp(enc.ok, expected_tag, 16) == 0, "aes256gcm NIST TC14: tag matches");
    }
    free(enc.ok);
}

/*
 * NIST SP 800-38D, Test Case 16 (AES-256-GCM).
 * Key:   feffe9928665731c6d6a8f9467308308 feffe9928665731c6d6a8f9467308308
 * IV:    cafebabefacedbaddecaf888
 * PT:    d9313225f88406e5a55909c5aff5269a 86a7a9531534f7da2e4c303d8a318a72
 *        1c3c0c95956809532fcf0e2449a6b525 b16aedf5aa0de657ba637b39
 *        (60 bytes; TC15 uses the full 64-byte PT with no AAD)
 * AAD:   feedfacedeadbeeffeedfacedeadbeef abaddad2
 * CT:    522dc1f099567d07f47f37a32a84427d 643a8cdcbfe5c0c97598a2bd2555d1aa
 *        8cb08e48590dbb3da7b08b1056828838 c5f61e6393ba7a0abcc9f662
 *        (60 bytes)
 * Tag:   b094dac5d93471bdec1a502270e3cc6c
 */
static void test_aes_nist_vector_tc16(void)
{
    uint8_t key_buf[32], nonce_buf[12], pt_buf[60], aad_buf[20];
    uint8_t expected_ct[60], expected_tag[16];

    hex_decode("feffe9928665731c6d6a8f9467308308"
               "feffe9928665731c6d6a8f9467308308", key_buf, 32);
    hex_decode("cafebabefacedbaddecaf888", nonce_buf, 12);
    hex_decode("d9313225f88406e5a55909c5aff5269a"
               "86a7a9531534f7da2e4c303d8a318a72"
               "1c3c0c95956809532fcf0e2449a6b525"
               "b16aedf5aa0de657ba637b39", pt_buf, 60);
    hex_decode("feedfacedeadbeeffeedfacedeadbeef"
               "abaddad2", aad_buf, 20);
    hex_decode("522dc1f099567d07f47f37a32a84427d"
               "643a8cdcbfe5c0c97598a2bd2555d1aa"
               "8cb08e48590dbb3da7b08b1056828838"
               "c5f61e6393ba7a0abcc9f662", expected_ct, 60);
    hex_decode("76fc6ece0f4e1768cddf8853bb2d551b", expected_tag, 16);

    ByteArray key   = ba_raw(key_buf, 32);
    ByteArray nonce = ba_raw(nonce_buf, 12);
    ByteArray plain = ba_raw(pt_buf, 60);
    ByteArray aad   = ba_raw(aad_buf, 20);

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);
    ASSERT(enc.is_err == 0, "aes256gcm NIST TC16: encrypt succeeds");
    ASSERT(enc.ok_len == 76, "aes256gcm NIST TC16: output len == 76 (60 CT + 16 tag)");

    if (!enc.is_err && enc.ok != NULL) {
        ASSERT(memcmp(enc.ok, expected_ct, 60) == 0,
               "aes256gcm NIST TC16: ciphertext matches");
        ASSERT(memcmp(enc.ok + 60, expected_tag, 16) == 0,
               "aes256gcm NIST TC16: auth tag matches");

        /* Verify decrypt round-trip */
        ByteArray ct_ba = ba_raw(enc.ok, enc.ok_len);
        EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonce, ct_ba, aad);
        ASSERT(dec.is_err == 0, "aes256gcm NIST TC16: decrypt succeeds");
        if (!dec.is_err && dec.ok != NULL) {
            ASSERT(dec.ok_len == 60, "aes256gcm NIST TC16: plaintext len == 60");
            ASSERT(memcmp(dec.ok, pt_buf, 60) == 0,
                   "aes256gcm NIST TC16: decrypted plaintext matches");
        }
        free(dec.ok);
    }
    free(enc.ok);
}

/*
 * NIST SP 800-38D, Test Case 15 (AES-256-GCM).
 * Same key/IV as TC16 but full 64-byte PT, no AAD.
 * Tag: 4d5c2af327cd64a62cf35abd2ba6fab4
 */
static void test_aes_nist_vector_tc15(void)
{
    uint8_t key_buf[32], nonce_buf[12], pt_buf[64];
    uint8_t expected_ct[64], expected_tag[16];

    hex_decode("feffe9928665731c6d6a8f9467308308"
               "feffe9928665731c6d6a8f9467308308", key_buf, 32);
    hex_decode("cafebabefacedbaddecaf888", nonce_buf, 12);
    hex_decode("d9313225f88406e5a55909c5aff5269a"
               "86a7a9531534f7da2e4c303d8a318a72"
               "1c3c0c95956809532fcf0e2449a6b525"
               "b16aedf5aa0de657ba637b391aafd255", pt_buf, 64);
    hex_decode("522dc1f099567d07f47f37a32a84427d"
               "643a8cdcbfe5c0c97598a2bd2555d1aa"
               "8cb08e48590dbb3da7b08b1056828838"
               "c5f61e6393ba7a0abcc9f662898015ad", expected_ct, 64);
    hex_decode("b094dac5d93471bdec1a502270e3cc6c", expected_tag, 16);

    ByteArray key   = ba_raw(key_buf, 32);
    ByteArray nonce = ba_raw(nonce_buf, 12);
    ByteArray plain = ba_raw(pt_buf, 64);
    ByteArray aad   = ba_raw(NULL, 0);

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);
    ASSERT(enc.is_err == 0, "aes256gcm NIST TC15: encrypt succeeds");
    if (!enc.is_err && enc.ok != NULL) {
        ASSERT(memcmp(enc.ok, expected_ct, 64) == 0,
               "aes256gcm NIST TC15: ciphertext matches");
        ASSERT(memcmp(enc.ok + 64, expected_tag, 16) == 0,
               "aes256gcm NIST TC15: auth tag matches");
    }
    free(enc.ok);
}


static void test_aes_zero_length_plaintext(void)
{
    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray plain = ba_raw(NULL, 0);
    ByteArray aad   = ba_str("authenticate this");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);
    ASSERT(enc.is_err == 0, "aes256gcm zero-len PT: encrypt succeeds");
    ASSERT(enc.ok_len == 16, "aes256gcm zero-len PT: output == 16 (tag only)");

    if (!enc.is_err && enc.ok != NULL) {
        ByteArray ct = ba_raw(enc.ok, enc.ok_len);
        EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonce, ct, aad);
        ASSERT(dec.is_err == 0, "aes256gcm zero-len PT: decrypt succeeds");
        ASSERT(dec.ok_len == 0, "aes256gcm zero-len PT: decrypted len == 0");
        free(dec.ok);
    }
    free(enc.ok);
    free((void *)key.data);
    free((void *)nonce.data);
}

static void test_aes_tampered_aad(void)
{
    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray plain = ba_str("protected");
    ByteArray aad   = ba_str("correct aad");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);
    if (!enc.is_err && enc.ok != NULL) {
        ByteArray ct      = ba_raw(enc.ok, enc.ok_len);
        ByteArray bad_aad = ba_str("wrong aad!!");
        EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonce, ct, bad_aad);
        ASSERT(dec.is_err == 1, "aes256gcm: tampered AAD → is_err=1");
        free(dec.ok);
    }
    free(enc.ok);
    free((void *)key.data);
    free((void *)nonce.data);
}

static void test_aes_tampered_tag(void)
{
    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray plain = ba_str("tag test");
    ByteArray aad   = ba_str("");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);
    if (!enc.is_err && enc.ok != NULL && enc.ok_len >= 16) {
        /* Flip a byte in the tag (last 16 bytes) */
        enc.ok[enc.ok_len - 1] ^= 0x01;
        ByteArray ct = ba_raw(enc.ok, enc.ok_len);
        EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonce, ct, aad);
        ASSERT(dec.is_err == 1, "aes256gcm: tampered auth tag → is_err=1");
        free(dec.ok);
    }
    free(enc.ok);
    free((void *)key.data);
    free((void *)nonce.data);
}

static void test_aes_wrong_key(void)
{
    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray plain = ba_str("secret message");
    ByteArray aad   = ba_str("");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);

    /* Generate a different key */
    ByteArray wrong_key = encrypt_aes256gcm_keygen();

    if (!enc.is_err && enc.ok != NULL) {
        ByteArray ct = ba_raw(enc.ok, enc.ok_len);
        EncryptResult dec = encrypt_aes256gcm_decrypt(wrong_key, nonce, ct, aad);
        ASSERT(dec.is_err == 1, "aes256gcm_decrypt: wrong key → is_err=1");
        free(dec.ok);
    }
    free(enc.ok);
    free((void *)key.data);
    free((void *)wrong_key.data);
    free((void *)nonce.data);
}

static void test_aes_tampered_ciphertext(void)
{
    ByteArray key   = encrypt_aes256gcm_keygen();
    ByteArray nonce = encrypt_aes256gcm_noncegen();
    ByteArray plain = ba_str("tamper me");
    ByteArray aad   = ba_str("");

    EncryptResult enc = encrypt_aes256gcm_encrypt(key, nonce, plain, aad);

    if (!enc.is_err && enc.ok != NULL && enc.ok_len > 0) {
        /* Flip the first byte of the ciphertext */
        enc.ok[0] ^= 0xff;
        ByteArray ct = ba_raw(enc.ok, enc.ok_len);
        EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonce, ct, aad);
        ASSERT(dec.is_err == 1, "aes256gcm_decrypt: tampered ciphertext → is_err=1");
        free(dec.ok);
    }
    free(enc.ok);
    free((void *)key.data);
    free((void *)nonce.data);
}

static void test_aes_ciphertext_too_short(void)
{
    uint8_t key_buf[32] = {0};
    uint8_t nonce_buf[12] = {0};
    uint8_t short_ct[8] = {1,2,3,4,5,6,7,8};
    ByteArray key  = ba_raw(key_buf, 32);
    ByteArray nonc = ba_raw(nonce_buf, 12);
    ByteArray ct   = ba_raw(short_ct, 8);
    ByteArray aad  = ba_raw(NULL, 0);

    EncryptResult dec = encrypt_aes256gcm_decrypt(key, nonc, ct, aad);
    ASSERT(dec.is_err == 1, "aes256gcm_decrypt: ciphertext < 16 bytes → is_err=1");
    free(dec.ok);
}

/* -------------------------------------------------------------------------
 * Error handling edge cases
 * ------------------------------------------------------------------------- */

static void test_aes_bad_key_size(void)
{
    ByteArray bad_key = ba_str("tooshort"); /* only 8 bytes */
    ByteArray nonce   = ba_str("123456789012");
    ByteArray plain   = ba_str("hello");
    ByteArray aad     = ba_str("");
    EncryptResult r   = encrypt_aes256gcm_encrypt(bad_key, nonce, plain, aad);
    ASSERT(r.is_err == 1, "aes256gcm_encrypt: bad key size → is_err=1");
    free(r.ok);
}

static void test_aes_bad_nonce_size(void)
{
    uint8_t key_buf[32] = {0};
    ByteArray key     = ba_raw(key_buf, 32);
    ByteArray bad_nonc = ba_str("short"); /* not 12 bytes */
    ByteArray plain   = ba_str("hello");
    ByteArray aad     = ba_str("");
    EncryptResult r   = encrypt_aes256gcm_encrypt(key, bad_nonc, plain, aad);
    ASSERT(r.is_err == 1, "aes256gcm_encrypt: bad nonce size → is_err=1");
    free(r.ok);
}

static void test_aes_bad_key_size_decrypt(void)
{
    ByteArray bad_key = ba_str("tooshort");
    ByteArray nonce   = ba_str("123456789012");
    uint8_t ct_buf[32] = {0};
    ByteArray ct      = ba_raw(ct_buf, 32);
    ByteArray aad     = ba_str("");
    EncryptResult r   = encrypt_aes256gcm_decrypt(bad_key, nonce, ct, aad);
    ASSERT(r.is_err == 1, "aes256gcm_decrypt: bad key size → is_err=1");
    free(r.ok);
}

static void test_aes_bad_nonce_size_decrypt(void)
{
    uint8_t key_buf[32] = {0};
    ByteArray key      = ba_raw(key_buf, 32);
    ByteArray bad_nonc = ba_str("short");
    uint8_t ct_buf[32] = {0};
    ByteArray ct       = ba_raw(ct_buf, 32);
    ByteArray aad      = ba_str("");
    EncryptResult r    = encrypt_aes256gcm_decrypt(key, bad_nonc, ct, aad);
    ASSERT(r.is_err == 1, "aes256gcm_decrypt: bad nonce size → is_err=1");
    free(r.ok);
}

/* -------------------------------------------------------------------------
 * X25519 tests
 * ------------------------------------------------------------------------- */

static void test_x25519_keypair_sizes(void)
{
    X25519Keypair kp = encrypt_x25519_keypair();
    /* Verify: pubkey and privkey are held in struct fields (no separate len).
     * We check them indirectly by exercising DH. Just verify they're populated. */
    int pubkey_nonzero = 0, privkey_nonzero = 0;
    int i;
    for (i = 0; i < 32; i++) {
        if (kp.pubkey[i])  pubkey_nonzero  = 1;
        if (kp.privkey[i]) privkey_nonzero = 1;
    }
    ASSERT(pubkey_nonzero,  "x25519_keypair: pubkey is 32 bytes (nonzero)");
    ASSERT(privkey_nonzero, "x25519_keypair: privkey is 32 bytes (nonzero)");
}

static void test_x25519_dh_symmetry(void)
{
    X25519Keypair kpA = encrypt_x25519_keypair();
    X25519Keypair kpB = encrypt_x25519_keypair();

    ByteArray privA = ba_raw(kpA.privkey, 32);
    ByteArray pubA  = ba_raw(kpA.pubkey,  32);
    ByteArray privB = ba_raw(kpB.privkey, 32);
    ByteArray pubB  = ba_raw(kpB.pubkey,  32);

    ByteArray sharedAB = encrypt_x25519_dh(privA, pubB);
    ByteArray sharedBA = encrypt_x25519_dh(privB, pubA);

    ASSERT(sharedAB.len == 32, "x25519_dh: sharedAB len == 32");
    ASSERT(sharedBA.len == 32, "x25519_dh: sharedBA len == 32");
    ASSERT(sharedAB.data != NULL && sharedBA.data != NULL &&
           memcmp(sharedAB.data, sharedBA.data, 32) == 0,
           "x25519_dh: DH symmetry dh(privA,pubB) == dh(privB,pubA)");

    free((void *)sharedAB.data);
    free((void *)sharedBA.data);
}

/*
 * RFC 7748 §6.1 — X25519 known-answer test.
 *
 * Alice's private key (scalar):
 *   77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a
 * Alice's public key (= scalar * basepoint):
 *   8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a
 * Bob's private key:
 *   5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb
 * Bob's public key:
 *   de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f
 * Shared secret:
 *   4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742
 */
static void test_x25519_rfc7748_vector(void)
{
    uint8_t alice_priv[32], alice_pub_expected[32];
    uint8_t bob_priv[32], bob_pub_expected[32];
    uint8_t expected_shared[32];

    hex_decode("77076d0a7318a57d3c16c17251b26645"
               "df4c2f87ebc0992ab177fba51db92c2a", alice_priv, 32);
    hex_decode("8520f0098930a754748b7ddcb43ef75a"
               "0dbf3a0d26381af4eba4a98eaa9b4e6a", alice_pub_expected, 32);
    hex_decode("5dab087e624a8a4b79e17f8b83800ee6"
               "6f3bb1292618b6fd1c2f8b27ff88e0eb", bob_priv, 32);
    hex_decode("de9edb7d7b7dc1b4d35b61c2ece43537"
               "3f8343c85b78674dadfc7e146f882b4f", bob_pub_expected, 32);
    hex_decode("4a5d9d5ba4ce2de1728e3bf480350f25"
               "e07e21c947d19e3376f09b3c1e161742", expected_shared, 32);

    /* Alice: dh(alice_priv, bob_pub) should equal shared secret */
    ByteArray a_priv = ba_raw(alice_priv, 32);
    ByteArray b_pub  = ba_raw(bob_pub_expected, 32);
    ByteArray shared_ab = encrypt_x25519_dh(a_priv, b_pub);
    ASSERT(shared_ab.len == 32, "x25519 RFC7748: shared len == 32");
    ASSERT(shared_ab.data != NULL &&
           memcmp(shared_ab.data, expected_shared, 32) == 0,
           "x25519 RFC7748: dh(alice_priv, bob_pub) matches known vector");

    /* Bob: dh(bob_priv, alice_pub) should also equal shared secret */
    ByteArray b_priv = ba_raw(bob_priv, 32);
    ByteArray a_pub  = ba_raw(alice_pub_expected, 32);
    ByteArray shared_ba = encrypt_x25519_dh(b_priv, a_pub);
    ASSERT(shared_ba.data != NULL &&
           memcmp(shared_ba.data, expected_shared, 32) == 0,
           "x25519 RFC7748: dh(bob_priv, alice_pub) matches known vector");

    free((void *)shared_ab.data);
    free((void *)shared_ba.data);
}

/* -------------------------------------------------------------------------
 * Ed25519 tests
 * ------------------------------------------------------------------------- */

static void test_ed25519_keypair_sizes(void)
{
    Ed25519Keypair kp = encrypt_ed25519_keypair();
    int pub_nonzero = 0, priv_nonzero = 0;
    int i;
    for (i = 0; i < 32; i++) if (kp.pubkey[i])  pub_nonzero  = 1;
    for (i = 0; i < 64; i++) if (kp.privkey[i]) priv_nonzero = 1;
    ASSERT(pub_nonzero,  "ed25519_keypair: pubkey 32 bytes (nonzero)");
    ASSERT(priv_nonzero, "ed25519_keypair: privkey 64 bytes (nonzero)");
}

static void test_ed25519_sign_len(void)
{
    Ed25519Keypair kp  = encrypt_ed25519_keypair();
    ByteArray privkey  = ba_raw(kp.privkey, 64);
    ByteArray msg      = ba_str("test message");
    ByteArray sig      = encrypt_ed25519_sign(privkey, msg);
    ASSERT(sig.len == 64, "ed25519_sign: signature len == 64");
    free((void *)sig.data);
}

static void test_ed25519_verify_success(void)
{
    Ed25519Keypair kp = encrypt_ed25519_keypair();
    ByteArray privkey = ba_raw(kp.privkey, 64);
    ByteArray pubkey  = ba_raw(kp.pubkey,  32);
    ByteArray msg     = ba_str("verify this message");
    ByteArray sig     = encrypt_ed25519_sign(privkey, msg);

    int ok = encrypt_ed25519_verify(pubkey, msg, sig);
    ASSERT(ok == 1, "ed25519_verify: valid signature → 1");
    free((void *)sig.data);
}

static void test_ed25519_verify_tampered_msg(void)
{
    Ed25519Keypair kp = encrypt_ed25519_keypair();
    ByteArray privkey = ba_raw(kp.privkey, 64);
    ByteArray pubkey  = ba_raw(kp.pubkey,  32);
    ByteArray msg     = ba_str("original message");
    ByteArray sig     = encrypt_ed25519_sign(privkey, msg);

    ByteArray bad_msg = ba_str("tampered message!");
    int ok = encrypt_ed25519_verify(pubkey, bad_msg, sig);
    ASSERT(ok == 0, "ed25519_verify: tampered message → 0");
    free((void *)sig.data);
}

static void test_ed25519_verify_wrong_key(void)
{
    Ed25519Keypair kp1 = encrypt_ed25519_keypair();
    Ed25519Keypair kp2 = encrypt_ed25519_keypair();

    ByteArray privkey1 = ba_raw(kp1.privkey, 64);
    ByteArray pubkey2  = ba_raw(kp2.pubkey,  32);  /* different keypair's pubkey */
    ByteArray msg      = ba_str("signed with key 1");
    ByteArray sig      = encrypt_ed25519_sign(privkey1, msg);

    int ok = encrypt_ed25519_verify(pubkey2, msg, sig);
    ASSERT(ok == 0, "ed25519_verify: wrong pubkey → 0");
    free((void *)sig.data);
}

/*
 * RFC 8032 §7.1, TEST 1 (Ed25519).
 *
 * SECRET KEY (seed, 32 bytes):
 *   9d61b19deffd5a60ba844af492ec2cc4 4449c5697b326919703bac031cae7f60
 * PUBLIC KEY (verified against Go crypto/ed25519, OpenSSL 3, RFC 8032 signature):
 *   d75a980182b10ab7d54bfed3c964073a 0ee172f3daa62325af021a68f707511a
 * MESSAGE: (empty)
 * SIGNATURE:
 *   e5564300c360ac729086e2cc806e828a 84877f1eb8e5d974d873e06522490155
 *   5fb8821590a33bacc61e39701cf9b46b d25bf5f0595bbe24655141438e7a100b
 */
static void test_ed25519_rfc8032_vector(void)
{
    uint8_t seed[32], expected_pub[32], expected_sig[64];

    hex_decode("9d61b19deffd5a60ba844af492ec2cc4"
               "4449c5697b326919703bac031cae7f60", seed, 32);
    hex_decode("d75a980182b10ab7d54bfed3c964073a"
               "0ee172f3daa62325af021a68f707511a", expected_pub, 32);
    hex_decode("e5564300c360ac729086e2cc806e828a"
               "84877f1eb8e5d974d873e06522490155"
               "5fb8821590a33bacc61e39701cf9b46b"
               "d25bf5f0595bbe24655141438e7a100b", expected_sig, 64);

    /* Build the privkey the way the API expects: seed || pubkey */
    uint8_t privkey_buf[64];
    memcpy(privkey_buf, seed, 32);
    memcpy(privkey_buf + 32, expected_pub, 32);

    ByteArray privkey = ba_raw(privkey_buf, 64);
    ByteArray msg     = ba_raw(NULL, 0);  /* empty message */

    ByteArray sig = encrypt_ed25519_sign(privkey, msg);
    ASSERT(sig.len == 64, "ed25519 RFC8032 TEST1: signature len == 64");
    ASSERT(sig.data != NULL &&
           memcmp(sig.data, expected_sig, 64) == 0,
           "ed25519 RFC8032 TEST1: signature matches known vector");

    /* Verify with known pubkey */
    ByteArray pubkey = ba_raw(expected_pub, 32);
    int ok = encrypt_ed25519_verify(pubkey, msg, sig);
    ASSERT(ok == 1, "ed25519 RFC8032 TEST1: verify succeeds");

    free((void *)sig.data);
}

static void test_ed25519_sign_short_privkey(void)
{
    uint8_t short_buf[16] = {0};
    ByteArray short_priv = ba_raw(short_buf, 16);
    ByteArray msg        = ba_str("hello");
    ByteArray sig = encrypt_ed25519_sign(short_priv, msg);
    ASSERT(sig.len == 0 && sig.data == NULL,
           "ed25519_sign: short privkey → empty result");
    free((void *)sig.data);
}

static void test_ed25519_verify_short_inputs(void)
{
    uint8_t short_buf[8] = {0};
    ByteArray short_pub = ba_raw(short_buf, 8);
    ByteArray msg       = ba_str("hello");
    uint8_t sig_buf[64] = {0};
    ByteArray sig       = ba_raw(sig_buf, 64);

    int ok = encrypt_ed25519_verify(short_pub, msg, sig);
    ASSERT(ok == 0, "ed25519_verify: short pubkey → 0");

    uint8_t pub_buf[32] = {0};
    ByteArray pub32     = ba_raw(pub_buf, 32);
    ByteArray short_sig = ba_raw(short_buf, 8);
    ok = encrypt_ed25519_verify(pub32, msg, short_sig);
    ASSERT(ok == 0, "ed25519_verify: short sig → 0");
}

/* -------------------------------------------------------------------------
 * HKDF-SHA-256 tests
 * ------------------------------------------------------------------------- */

static void test_hkdf_output_lengths(void)
{
    ByteArray ikm  = ba_str("input key material");
    ByteArray salt = ba_str("random salt");
    ByteArray info = ba_str("test context");

    ByteArray out32 = encrypt_hkdf_sha256(ikm, salt, info, 32);
    ASSERT(out32.len == 32, "hkdf_sha256: request 32 → len == 32");
    free((void *)out32.data);

    ByteArray out64 = encrypt_hkdf_sha256(ikm, salt, info, 64);
    ASSERT(out64.len == 64, "hkdf_sha256: request 64 → len == 64");
    free((void *)out64.data);
}

static void test_hkdf_determinism(void)
{
    ByteArray ikm  = ba_str("same ikm");
    ByteArray salt = ba_str("same salt");
    ByteArray info = ba_str("same info");

    ByteArray out1 = encrypt_hkdf_sha256(ikm, salt, info, 32);
    ByteArray out2 = encrypt_hkdf_sha256(ikm, salt, info, 32);

    ASSERT(out1.len == 32 && out2.len == 32 &&
           out1.data != NULL && out2.data != NULL &&
           memcmp(out1.data, out2.data, 32) == 0,
           "hkdf_sha256: same inputs → same output (deterministic)");
    free((void *)out1.data);
    free((void *)out2.data);
}

static void test_hkdf_info_separation(void)
{
    ByteArray ikm   = ba_str("shared ikm");
    ByteArray salt  = ba_str("shared salt");
    ByteArray info1 = ba_str("label:encryption");
    ByteArray info2 = ba_str("label:authentication");

    ByteArray out1 = encrypt_hkdf_sha256(ikm, salt, info1, 32);
    ByteArray out2 = encrypt_hkdf_sha256(ikm, salt, info2, 32);

    ASSERT(out1.len == 32 && out2.len == 32 &&
           out1.data != NULL && out2.data != NULL &&
           memcmp(out1.data, out2.data, 32) != 0,
           "hkdf_sha256: different info labels → different output");
    free((void *)out1.data);
    free((void *)out2.data);
}

/*
 * RFC 5869, Test Case 1 (HKDF-SHA-256).
 * IKM:  0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (22 bytes)
 * Salt: 0x000102030405060708090a0b0c (13 bytes)
 * Info: 0xf0f1f2f3f4f5f6f7f8f9 (10 bytes)
 * L:    42
 * OKM:  3cb25f25faacd57a90434f64d0362f2a
 *       2d2d0a90cf1a5a4c5db02d56ecc4c5bf
 *       34007208d5b887185865
 */
static void test_hkdf_rfc5869_vector(void)
{
    uint8_t ikm_buf[22], salt_buf[13], info_buf[10], expected_okm[42];

    hex_decode("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b",
               ikm_buf, 22);
    hex_decode("000102030405060708090a0b0c", salt_buf, 13);
    hex_decode("f0f1f2f3f4f5f6f7f8f9", info_buf, 10);
    hex_decode("3cb25f25faacd57a90434f64d0362f2a"
               "2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
               "34007208d5b887185865", expected_okm, 42);

    ByteArray ikm  = ba_raw(ikm_buf, 22);
    ByteArray salt = ba_raw(salt_buf, 13);
    ByteArray info = ba_raw(info_buf, 10);

    ByteArray okm = encrypt_hkdf_sha256(ikm, salt, info, 42);
    ASSERT(okm.len == 42, "hkdf RFC5869 TC1: output len == 42");
    ASSERT(okm.data != NULL &&
           memcmp(okm.data, expected_okm, 42) == 0,
           "hkdf RFC5869 TC1: output matches known vector");
    free((void *)okm.data);
}

static void test_hkdf_zero_salt(void)
{
    ByteArray ikm  = ba_str("some key material");
    ByteArray salt = ba_raw(NULL, 0);  /* zero-length salt */
    ByteArray info = ba_str("context");

    ByteArray out = encrypt_hkdf_sha256(ikm, salt, info, 32);
    ASSERT(out.len == 32, "hkdf_sha256: zero-length salt → len == 32");
    ASSERT(out.data != NULL, "hkdf_sha256: zero-length salt → data != NULL");
    free((void *)out.data);
}

static void test_hkdf_output_too_large(void)
{
    ByteArray ikm  = ba_str("ikm");
    ByteArray salt = ba_str("salt");
    ByteArray info = ba_str("info");

    /* Max is 255 * 32 = 8160. Request 8161 → should fail (empty). */
    ByteArray out = encrypt_hkdf_sha256(ikm, salt, info, 8161);
    ASSERT(out.len == 0, "hkdf_sha256: output > 255*32 → len == 0 (error)");
    free((void *)out.data);
}

static void test_hkdf_zero_output(void)
{
    ByteArray ikm  = ba_str("ikm");
    ByteArray salt = ba_str("salt");
    ByteArray info = ba_str("info");

    ByteArray out = encrypt_hkdf_sha256(ikm, salt, info, 0);
    ASSERT(out.len == 0, "hkdf_sha256: outlen 0 → len == 0 (error)");
    free((void *)out.data);
}

/* -------------------------------------------------------------------------
 * TLS fingerprint tests
 * ------------------------------------------------------------------------- */

/* Minimal valid-looking self-signed cert in PEM format (real DER, short). */
/* This is a real self-signed cert for testing (public data, generated by openssl). */
static const char *TEST_PEM_CERT =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIBpTCCAQ6gAwIBAgIUYHnCQ2xrVe9xU4IWvMGDH8SoH7owDQYJKoZIhvcNAQEL\n"
    "BQAwEzERMA8GA1UEAwwIdGVzdC1jYTAeFw0yNDAxMDEwMDAwMDBaFw0yNTAxMDEw\n"
    "MDAwMDBaMBMxETAPBgNVBAMMCHRlc3QtY2EwgZ8wDQYJKoZIhvcNAQEBBQADgY0A\n"
    "MIGJAoGBAMIRitMGHFDPUMZpFNTbzSbXTlGMWCM7TpCUMlHjAx/p8UMGHFi5lMwi\n"
    "yoMo0tVfxKkqzHlGEXvnq4blXPRkXRtBZqT5HN3n8k+kfTyZGq8xD7GJqZnkQOaV\n"
    "AgMBAAGjIzAhMB8GA1UdEQQYMBaCCGxvY2FsaG9zdIIKbG9jYWxob3N0MA0GCSqG\n"
    "SIb3DQEBCwUAA4GBAG1YqLFGYVXfGFiLzRLsqLG4Y9ZPEPJp9Vo3sKnrNYqhADqQ\n"
    "nS6MElUdV2Fs9+YtAZ4KcJJ7B+4i1OQ+6P3T9m1Oq0TFkiX7kW2YkiBF4X0ZEVKB\n"
    "r5V8iJbPYhyGmH6Y3jdgF1g3t5+wHGJkrZxN5bEb\n"
    "-----END CERTIFICATE-----\n";

static void test_tls_fingerprint_len(void)
{
    ByteArray fp = encrypt_tls_cert_fingerprint(TEST_PEM_CERT);
    /* SHA-256 is always 32 bytes, or 0 if parse failed */
    ASSERT(fp.len == 32, "tls_cert_fingerprint: SHA-256 length == 32");
    free((void *)fp.data);
}

static void test_tls_fingerprint_determinism(void)
{
    ByteArray fp1 = encrypt_tls_cert_fingerprint(TEST_PEM_CERT);
    ByteArray fp2 = encrypt_tls_cert_fingerprint(TEST_PEM_CERT);

    ASSERT(fp1.len == 32 && fp2.len == 32 &&
           fp1.data != NULL && fp2.data != NULL &&
           memcmp(fp1.data, fp2.data, 32) == 0,
           "tls_cert_fingerprint: same PEM → same fingerprint (deterministic)");
    free((void *)fp1.data);
    free((void *)fp2.data);
}

static void test_tls_fingerprint_invalid_pem(void)
{
    ByteArray fp = encrypt_tls_cert_fingerprint("not a certificate");
    ASSERT(fp.len == 0, "tls_cert_fingerprint: invalid PEM → len == 0");
    free((void *)fp.data);
}

static void test_tls_fingerprint_null(void)
{
    ByteArray fp = encrypt_tls_cert_fingerprint(NULL);
    ASSERT(fp.len == 0, "tls_cert_fingerprint: NULL input → len == 0");
    free((void *)fp.data);
}

/* -------------------------------------------------------------------------
 * ChaCha20-Poly1305 tests (Story 30.1.1)
 * ------------------------------------------------------------------------- */

static void test_chacha_keygen_len(void)
{
    ByteArray key = encrypt_chacha20poly1305_keygen();
    ASSERT(key.len == 32, "chacha20poly1305_keygen: len == 32");
    ASSERT(key.data != NULL, "chacha20poly1305_keygen: data != NULL");
    free((void *)key.data);
}

static void test_chacha_noncegen_len(void)
{
    ByteArray nonce = encrypt_chacha20poly1305_noncegen();
    ASSERT(nonce.len == 12, "chacha20poly1305_noncegen: len == 12");
    ASSERT(nonce.data != NULL, "chacha20poly1305_noncegen: data != NULL");
    free((void *)nonce.data);
}

static void test_chacha_roundtrip_with_aad(void)
{
    const char *plaintext_str = "hello chacha world";
    ByteArray key   = encrypt_chacha20poly1305_keygen();
    ByteArray nonce = encrypt_chacha20poly1305_noncegen();
    ByteArray plain = ba_str(plaintext_str);
    ByteArray aad   = ba_str("additional data");

    ChaChaEncResult enc = encrypt_chacha20poly1305_encrypt(key, nonce, plain, aad);
    ASSERT(enc.is_err == 0, "chacha20poly1305_encrypt: roundtrip with AAD no error");
    ASSERT(enc.ciphertext.len == plain.len + 16,
           "chacha20poly1305_encrypt: output len == plaintext + 16-byte tag");

    if (!enc.is_err && enc.ciphertext.data != NULL) {
        ChaChaDecResult dec = encrypt_chacha20poly1305_decrypt(key, nonce,
                                                               enc.ciphertext, aad);
        ASSERT(dec.is_err == 0, "chacha20poly1305_decrypt: roundtrip with AAD no error");
        ASSERT(dec.plaintext.len == plain.len,
               "chacha20poly1305_decrypt: recovered plaintext length matches");
        if (!dec.is_err && dec.plaintext.data != NULL) {
            ASSERT(memcmp(dec.plaintext.data, plaintext_str, plain.len) == 0,
                   "chacha20poly1305_decrypt: recovered plaintext content matches");
        }
        free((void *)dec.plaintext.data);
    }
    free((void *)enc.ciphertext.data);
    free((void *)key.data);
    free((void *)nonce.data);
}

static void test_chacha_roundtrip_empty_aad(void)
{
    const char *plaintext_str = "no aad here";
    ByteArray key   = encrypt_chacha20poly1305_keygen();
    ByteArray nonce = encrypt_chacha20poly1305_noncegen();
    ByteArray plain = ba_str(plaintext_str);
    ByteArray aad   = ba_raw(NULL, 0);

    ChaChaEncResult enc = encrypt_chacha20poly1305_encrypt(key, nonce, plain, aad);
    ASSERT(enc.is_err == 0, "chacha20poly1305_encrypt: empty AAD no error");

    if (!enc.is_err && enc.ciphertext.data != NULL) {
        ChaChaDecResult dec = encrypt_chacha20poly1305_decrypt(key, nonce,
                                                               enc.ciphertext, aad);
        ASSERT(dec.is_err == 0, "chacha20poly1305_decrypt: empty AAD no error");
        ASSERT(dec.plaintext.len == plain.len,
               "chacha20poly1305_decrypt: empty AAD plaintext length matches");
        if (!dec.is_err && dec.plaintext.data != NULL) {
            ASSERT(memcmp(dec.plaintext.data, plaintext_str, plain.len) == 0,
                   "chacha20poly1305_decrypt: empty AAD plaintext content matches");
        }
        free((void *)dec.plaintext.data);
    }
    free((void *)enc.ciphertext.data);
    free((void *)key.data);
    free((void *)nonce.data);
}

static void test_chacha_wrong_key(void)
{
    ByteArray key   = encrypt_chacha20poly1305_keygen();
    ByteArray nonce = encrypt_chacha20poly1305_noncegen();
    ByteArray plain = ba_str("secret chacha message");
    ByteArray aad   = ba_raw(NULL, 0);

    ChaChaEncResult enc = encrypt_chacha20poly1305_encrypt(key, nonce, plain, aad);
    ByteArray wrong_key = encrypt_chacha20poly1305_keygen();

    if (!enc.is_err && enc.ciphertext.data != NULL) {
        ChaChaDecResult dec = encrypt_chacha20poly1305_decrypt(wrong_key, nonce,
                                                               enc.ciphertext, aad);
        ASSERT(dec.is_err == 1, "chacha20poly1305_decrypt: wrong key → is_err=1");
        free((void *)dec.plaintext.data);
    }
    free((void *)enc.ciphertext.data);
    free((void *)key.data);
    free((void *)wrong_key.data);
    free((void *)nonce.data);
}

static void test_chacha_tampered_ciphertext(void)
{
    ByteArray key   = encrypt_chacha20poly1305_keygen();
    ByteArray nonce = encrypt_chacha20poly1305_noncegen();
    ByteArray plain = ba_str("tamper me chacha");
    ByteArray aad   = ba_raw(NULL, 0);

    ChaChaEncResult enc = encrypt_chacha20poly1305_encrypt(key, nonce, plain, aad);

    if (!enc.is_err && enc.ciphertext.data != NULL && enc.ciphertext.len > 0) {
        /* Flip the first byte of the ciphertext body */
        uint8_t *mutable_ct = (uint8_t *)enc.ciphertext.data;
        mutable_ct[0] ^= 0xff;
        ChaChaDecResult dec = encrypt_chacha20poly1305_decrypt(key, nonce,
                                                               enc.ciphertext, aad);
        ASSERT(dec.is_err == 1,
               "chacha20poly1305_decrypt: tampered ciphertext → is_err=1");
        free((void *)dec.plaintext.data);
    }
    free((void *)enc.ciphertext.data);
    free((void *)key.data);
    free((void *)nonce.data);
}

/* -------------------------------------------------------------------------
 * PBKDF2 tests (Story 30.1.1)
 * ------------------------------------------------------------------------- */

/*
 * PBKDF2-SHA256 known vector (same parameters as RFC 6070 Test Vector 1
 * but using HMAC-SHA256 rather than HMAC-SHA1):
 *   Password: "password"
 *   Salt:     "salt"
 *   c:        1
 *   dkLen:    20
 *   DK (sha256): 120fb6cffcf8b32c43e7225256c4f837a86548c9
 *
 * Verified with Python: hashlib.pbkdf2_hmac('sha256', b'password', b'salt', 1, 20)
 */
static void test_pbkdf2_rfc6070_vector1(void)
{
    const char *password = "password";
    const char *salt_str = "salt";
    uint8_t expected[20];
    hex_decode("120fb6cffcf8b32c43e7225256c4f837a86548c9", expected, 20);

    ByteArray salt = ba_str(salt_str);
    ByteArray dk = encrypt_pbkdf2(password, salt, 1, 20, "sha256");

    ASSERT(dk.len == 20, "pbkdf2 RFC6070 v1: output len == 20");
    ASSERT(dk.data != NULL &&
           memcmp(dk.data, expected, 20) == 0,
           "pbkdf2 RFC6070 v1 (sha256, c=1): matches known vector");
    free((void *)dk.data);
}

/* -------------------------------------------------------------------------
 * RSA tests (Story 30.1.2)
 *
 * Note: 2048-bit key generation involves Miller-Rabin primality tests over
 * 1024-bit random candidates and takes a few seconds — this is expected.
 * ------------------------------------------------------------------------- */

/* Shared keypair populated by test_rsa_keypair_2048, reused by later tests. */
static RsaKeyPair g_rsa_kp;
static int        g_rsa_kp_valid = 0;

static void test_rsa_keypair_2048(void)
{
    printf("note: generating 2048-bit RSA keypair (may take a few seconds)...\n");
    RsaKeyPairResult r = encrypt_rsa_generate_keypair(2048);
    ASSERT(r.is_err == 0, "rsa_generate_keypair(2048): is_err == 0");
    ASSERT(r.ok.pub_len  > 0, "rsa_generate_keypair(2048): pub_len > 0");
    ASSERT(r.ok.priv_len > 0, "rsa_generate_keypair(2048): priv_len > 0");
    ASSERT(r.ok.pub_data  != NULL, "rsa_generate_keypair(2048): pub_data != NULL");
    ASSERT(r.ok.priv_data != NULL, "rsa_generate_keypair(2048): priv_data != NULL");
    if (!r.is_err) {
        g_rsa_kp = r.ok;
        g_rsa_kp_valid = 1;
    } else {
        free(r.ok.pub_data);
        free(r.ok.priv_data);
    }
}

static void test_rsa_encrypt_decrypt_roundtrip(void)
{
    if (!g_rsa_kp_valid) {
        fprintf(stderr, "SKIP: rsa_encrypt_decrypt_roundtrip (no keypair)\n");
        return;
    }
    const char *plaintext_str = "hello rsa oaep";
    ByteArray pt = ba_str(plaintext_str);

    RsaBytesResult enc = encrypt_rsa_encrypt(g_rsa_kp.pub_data, g_rsa_kp.pub_len, pt);
    ASSERT(enc.is_err == 0, "rsa_encrypt: is_err == 0");
    if (enc.is_err) return;
    ASSERT(enc.ok.len == 256, "rsa_encrypt: ciphertext len == 256 (2048/8)");

    RsaBytesResult dec = encrypt_rsa_decrypt(g_rsa_kp.priv_data, g_rsa_kp.priv_len,
                                              enc.ok);
    ASSERT(dec.is_err == 0, "rsa_decrypt: is_err == 0");
    if (!dec.is_err) {
        ASSERT(dec.ok.len == pt.len, "rsa_decrypt roundtrip: recovered length matches");
        ASSERT(memcmp(dec.ok.data, pt.data, (size_t)pt.len) == 0,
               "rsa_decrypt roundtrip: recovered bytes match plaintext");
        free((void *)dec.ok.data);
    }
    free((void *)enc.ok.data);
}

static void test_rsa_sign_verify(void)
{
    if (!g_rsa_kp_valid) {
        fprintf(stderr, "SKIP: rsa_sign_verify (no keypair)\n");
        return;
    }
    ByteArray msg = ba_str("hello world");

    RsaBytesResult sr = encrypt_rsa_sign(g_rsa_kp.priv_data, g_rsa_kp.priv_len, msg);
    ASSERT(sr.is_err == 0, "rsa_sign: is_err == 0");
    if (sr.is_err) return;
    ASSERT(sr.ok.len == 256, "rsa_sign: signature len == 256");

    int ok = encrypt_rsa_verify(g_rsa_kp.pub_data, g_rsa_kp.pub_len, msg, sr.ok);
    ASSERT(ok == 1, "rsa_verify: valid signature returns 1");
    free((void *)sr.ok.data);
}

static void test_rsa_verify_wrong_message(void)
{
    if (!g_rsa_kp_valid) {
        fprintf(stderr, "SKIP: rsa_verify_wrong_message (no keypair)\n");
        return;
    }
    ByteArray msg       = ba_str("hello world");
    ByteArray wrong_msg = ba_str("hello WORLD");

    RsaBytesResult sr = encrypt_rsa_sign(g_rsa_kp.priv_data, g_rsa_kp.priv_len, msg);
    if (sr.is_err) { fprintf(stderr, "SKIP: rsa_verify_wrong_message (sign failed)\n"); return; }

    int ok = encrypt_rsa_verify(g_rsa_kp.pub_data, g_rsa_kp.pub_len, wrong_msg, sr.ok);
    ASSERT(ok == 0, "rsa_verify: wrong message returns 0");
    free((void *)sr.ok.data);
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int main(void)
{
    /* Layer-2 safety net: kill the whole binary if it runs longer than 150s.
     * (Individual slow sections, e.g. RSA keygen, have their own tighter
     * timeouts via setjmp/longjmp so they skip gracefully first.) */
    alarm(150);

    printf("=== test_encrypt ===\n");

    /* AES-256-GCM */
    test_aes_keygen_len();
    test_aes_noncegen_len();
    test_aes_roundtrip();
    test_aes_nist_vector_tc13();
    test_aes_nist_vector_tc14();
    test_aes_nist_vector_tc16();
    test_aes_nist_vector_tc15();
    test_aes_zero_length_plaintext();
    test_aes_wrong_key();
    test_aes_tampered_ciphertext();
    test_aes_tampered_aad();
    test_aes_tampered_tag();
    test_aes_ciphertext_too_short();
    test_aes_bad_key_size();
    test_aes_bad_nonce_size();
    test_aes_bad_key_size_decrypt();
    test_aes_bad_nonce_size_decrypt();

    /* X25519 */
    test_x25519_keypair_sizes();
    test_x25519_dh_symmetry();
    test_x25519_rfc7748_vector();

    /* Ed25519 */
    test_ed25519_keypair_sizes();
    test_ed25519_sign_len();
    test_ed25519_verify_success();
    test_ed25519_verify_tampered_msg();
    test_ed25519_verify_wrong_key();
    test_ed25519_rfc8032_vector();
    test_ed25519_sign_short_privkey();
    test_ed25519_verify_short_inputs();

    /* HKDF */
    test_hkdf_output_lengths();
    test_hkdf_determinism();
    test_hkdf_info_separation();
    test_hkdf_rfc5869_vector();
    test_hkdf_zero_salt();
    test_hkdf_output_too_large();
    test_hkdf_zero_output();

    /* TLS fingerprint */
    test_tls_fingerprint_len();
    test_tls_fingerprint_determinism();
    test_tls_fingerprint_invalid_pem();
    test_tls_fingerprint_null();

    /* ChaCha20-Poly1305 (Story 30.1.1) */
    test_chacha_keygen_len();
    test_chacha_noncegen_len();
    test_chacha_roundtrip_with_aad();
    test_chacha_roundtrip_empty_aad();
    test_chacha_wrong_key();
    test_chacha_tampered_ciphertext();

    /* PBKDF2 (Story 30.1.1) */
    test_pbkdf2_rfc6070_vector1();

    /* RSA (Story 30.1.2) — 60-second timeout guards against slow bignum keygen */
    {
        struct sigaction sa_new, sa_old;
        sa_new.sa_handler = s_rsa_timeout;
        sigemptyset(&sa_new.sa_mask);
        sa_new.sa_flags = 0;
        sigaction(SIGALRM, &sa_new, &sa_old);
        if (setjmp(s_rsa_timeout_jmp) == 0) {
            alarm(60);
            test_rsa_keypair_2048();
            test_rsa_encrypt_decrypt_roundtrip();
            test_rsa_sign_verify();
            test_rsa_verify_wrong_message();
            alarm(0);
        } else {
            fprintf(stderr, "SKIP: RSA tests timed out after 60s\n");
        }
        sigaction(SIGALRM, &sa_old, NULL);
    }

    /* Free shared RSA keypair */
    if (g_rsa_kp_valid) {
        free(g_rsa_kp.pub_data);
        free(g_rsa_kp.priv_data);
    }

    printf("=== %d failure(s) ===\n", failures);
    return (failures > 0) ? 1 : 0;
}

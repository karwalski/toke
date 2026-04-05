/*
 * test_crypto.c — Unit tests for the std.crypto C library (Story 2.7.3, 35.1.1).
 *
 * Build and run: make test-stdlib-crypto
 *
 * Known-vector sources (verified with Python3 hashlib):
 *   SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
 *   SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 *   HMAC-SHA-256(key="key", data="The quick brown fox jumps over the lazy dog") =
 *       f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
 *   SHA-512("") = cf83e1357eefb8bd...  (FIPS 180-4)
 *   SHA-512("abc") = ddaf35a193617aba...  (FIPS 180-4)
 *   HMAC-SHA-512(key="key", data="The quick brown fox jumps over the lazy dog") =
 *       b42af09057bac1e2c215...  (RFC 4231 / Python3 hmac)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/crypto.h"

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

int main(void)
{
    /* --- sha256: empty string --- */
    ByteArray empty = {NULL, 0};
    ByteArray h_empty = crypto_sha256(empty);
    ASSERT(h_empty.len == 32, "sha256(empty) len==32");
    const char *hex_empty = crypto_to_hex(h_empty);
    ASSERT_STREQ(hex_empty,
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855",
        "sha256(empty) known vector");

    /* --- sha256: "abc" (verified with Python3 hashlib) --- */
    ByteArray abc = ba_from_str("abc");
    ByteArray h_abc = crypto_sha256(abc);
    ASSERT(h_abc.len == 32, "sha256(abc) len==32");
    const char *hex_abc = crypto_to_hex(h_abc);
    ASSERT_STREQ(hex_abc,
        "ba7816bf8f01cfea414140de5dae2223"
        "b00361a396177a9cb410ff61f20015ad",
        "sha256(abc) known vector");

    /* --- sha256: output bytes differ for different inputs --- */
    ASSERT(h_empty.data && h_abc.data &&
           memcmp(h_empty.data, h_abc.data, 32) != 0,
           "sha256 distinct inputs produce distinct digests");

    /* --- hmac_sha256 known vector --- */
    /* HMAC-SHA-256(key="key", msg="The quick brown fox jumps over the lazy dog") */
    ByteArray key  = ba_from_str("key");
    ByteArray msg  = ba_from_str("The quick brown fox jumps over the lazy dog");
    ByteArray hmac = crypto_hmac_sha256(key, msg);
    ASSERT(hmac.len == 32, "hmac_sha256 len==32");
    const char *hex_hmac = crypto_to_hex(hmac);
    ASSERT_STREQ(hex_hmac,
        "f7bc83f430538424b13298e6aa6fb143"
        "ef4d59a14946175997479dbc2d1a3cd8",
        "hmac_sha256 known vector");

    /* --- hmac differs when key differs --- */
    ByteArray key2   = ba_from_str("other");
    ByteArray hmac2  = crypto_hmac_sha256(key2, msg);
    ASSERT(memcmp(hmac.data, hmac2.data, 32) != 0,
           "hmac_sha256 key change produces different tag");

    /* --- to_hex: single byte --- */
    const uint8_t one_byte[] = {0xde};
    ByteArray one = ba_from_bytes(one_byte, 1);
    const char *hex_one = crypto_to_hex(one);
    ASSERT_STREQ(hex_one, "de", "to_hex single byte 0xde");

    /* --- to_hex: empty --- */
    const char *hex_mt = crypto_to_hex(empty);
    ASSERT(hex_mt && strlen(hex_mt) == 0, "to_hex(empty) == \"\"");

    /* --- to_hex length: 32 bytes -> 64 hex chars --- */
    ASSERT(strlen(hex_abc) == 64, "to_hex(sha256) length == 64");

    /* =================================================================
     * SHA-512 tests (Story 35.1.1)
     * ================================================================= */

    /* --- sha512: empty string --- */
    ByteArray h512_empty = crypto_sha512(empty);
    ASSERT(h512_empty.len == 64, "sha512(empty) len==64");
    const char *hex512_empty = crypto_to_hex(h512_empty);
    ASSERT_STREQ(hex512_empty,
        "cf83e1357eefb8bdf1542850d66d8007"
        "d620e4050b5715dc83f4a921d36ce9ce"
        "47d0d13c5d85f2b0ff8318d2877eec2f"
        "63b931bd47417a81a538327af927da3e",
        "sha512(empty) known vector");

    /* --- sha512: "abc" (FIPS 180-4 test vector) --- */
    ByteArray h512_abc = crypto_sha512(abc);
    ASSERT(h512_abc.len == 64, "sha512(abc) len==64");
    const char *hex512_abc = crypto_to_hex(h512_abc);
    ASSERT_STREQ(hex512_abc,
        "ddaf35a193617abacc417349ae204131"
        "12e6fa4e89a97ea20a9eeee64b55d39a"
        "2192992a274fc1a836ba3c23a3feebbd"
        "454d4423643ce80e2a9ac94fa54ca49f",
        "sha512(abc) known vector");

    /* --- sha512: distinct inputs produce distinct digests --- */
    ASSERT(h512_empty.data && h512_abc.data &&
           memcmp(h512_empty.data, h512_abc.data, 64) != 0,
           "sha512 distinct inputs produce distinct digests");

    /* =================================================================
     * HMAC-SHA-512 tests (Story 35.1.1)
     * ================================================================= */

    /* HMAC-SHA-512(key="key", msg="The quick brown fox jumps over the lazy dog") */
    ByteArray hmac512 = crypto_hmac_sha512(key, msg);
    ASSERT(hmac512.len == 64, "hmac_sha512 len==64");
    const char *hex_hmac512 = crypto_to_hex(hmac512);
    ASSERT_STREQ(hex_hmac512,
        "b42af09057bac1e2c215281b0a1a9156"
        "0bd4d21b3ce7e9fc5e1cd96790cc1e25"
        "db44b82c5aab0845e87ab0b41b757b43"
        "97e652a06b65f2c73034e8acb5345741",
        "hmac_sha512 known vector");

    /* --- hmac_sha512 differs when key differs --- */
    ByteArray hmac512_2 = crypto_hmac_sha512(key2, msg);
    ASSERT(memcmp(hmac512.data, hmac512_2.data, 64) != 0,
           "hmac_sha512 key change produces different tag");

    /* =================================================================
     * constanteq tests (Story 35.1.1)
     * ================================================================= */

    /* Equal arrays */
    ByteArray eq_a = ba_from_str("hello");
    ByteArray eq_b = ba_from_str("hello");
    ASSERT(crypto_constanteq(eq_a, eq_b) == 1,
           "constanteq equal arrays returns 1");

    /* Unequal arrays (same length) */
    ByteArray eq_c = ba_from_str("hellp");
    ASSERT(crypto_constanteq(eq_a, eq_c) == 0,
           "constanteq unequal arrays returns 0");

    /* Different lengths */
    ByteArray eq_d = ba_from_str("hell");
    ASSERT(crypto_constanteq(eq_a, eq_d) == 0,
           "constanteq different lengths returns 0");

    /* Both empty */
    ByteArray eq_empty1 = {NULL, 0};
    ByteArray eq_empty2 = {NULL, 0};
    ASSERT(crypto_constanteq(eq_empty1, eq_empty2) == 1,
           "constanteq both empty returns 1");

    /* Hash comparison: same hash is equal */
    ASSERT(crypto_constanteq(h_abc, h_abc) == 1,
           "constanteq same sha256 hash returns 1");

    /* Hash comparison: different hashes are not equal */
    ASSERT(crypto_constanteq(h_abc, h_empty) == 0,
           "constanteq different sha256 hashes returns 0");

    /* =================================================================
     * randombytes tests (Story 35.1.1)
     * ================================================================= */

    /* Returns requested length */
    ByteArray rb32 = crypto_randombytes(32);
    ASSERT(rb32.len == 32 && rb32.data != NULL,
           "randombytes(32) len==32 and non-NULL");

    /* Two calls produce different output (vanishingly unlikely to collide) */
    ByteArray rb32b = crypto_randombytes(32);
    ASSERT(memcmp(rb32.data, rb32b.data, 32) != 0,
           "randombytes(32) two calls produce different output");

    /* Zero-length request */
    ByteArray rb0 = crypto_randombytes(0);
    ASSERT(rb0.len == 0, "randombytes(0) len==0");

    /* to_hex length for sha512: 64 bytes -> 128 hex chars */
    ASSERT(strlen(hex512_abc) == 128, "to_hex(sha512) length == 128");

    if (failures == 0) { printf("All crypto tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

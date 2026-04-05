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

    /* =================================================================
     * crypto_from_hex tests (Story 29.6.1)
     * ================================================================= */

    /* Valid hex: "48656c6c6f" -> "Hello" (5 bytes) */
    ByteArray fh_out;
    const char *fh_err = crypto_from_hex("48656c6c6f", &fh_out);
    ASSERT(fh_err == NULL, "from_hex valid: returns NULL error");
    ASSERT(fh_out.len == 5, "from_hex valid: len==5");
    ASSERT(fh_out.data && fh_out.data[0] == 'H', "from_hex valid: data[0]=='H'");
    ASSERT(fh_out.data && fh_out.data[4] == 'o', "from_hex valid: data[4]=='o'");

    /* Invalid chars: "xyz" */
    ByteArray fh_bad1;
    const char *fh_err1 = crypto_from_hex("xyz", &fh_bad1);
    ASSERT(fh_err1 != NULL, "from_hex invalid chars: returns error");
    ASSERT(fh_bad1.data == NULL, "from_hex invalid chars: out.data==NULL");

    /* Odd length: "abc" */
    ByteArray fh_bad2;
    const char *fh_err2 = crypto_from_hex("abc", &fh_bad2);
    ASSERT(fh_err2 != NULL, "from_hex odd length: returns error");
    ASSERT(fh_bad2.data == NULL, "from_hex odd length: out.data==NULL");

    /* Empty hex string -> empty ByteArray, no error */
    ByteArray fh_empty;
    const char *fh_err3 = crypto_from_hex("", &fh_empty);
    ASSERT(fh_err3 == NULL, "from_hex empty string: returns NULL error");
    ASSERT(fh_empty.len == 0, "from_hex empty string: len==0");

    /* Round-trip: to_hex then from_hex */
    ByteArray fh_rt_src;
    fh_rt_src.data = h_abc.data;
    fh_rt_src.len  = 32;
    const char *hex_rt = crypto_to_hex(fh_rt_src);
    ByteArray fh_rt_out;
    const char *fh_rt_err = crypto_from_hex(hex_rt, &fh_rt_out);
    ASSERT(fh_rt_err == NULL, "from_hex round-trip: no error");
    ASSERT(fh_rt_out.len == 32, "from_hex round-trip: len==32");
    ASSERT(fh_rt_out.data && memcmp(fh_rt_out.data, h_abc.data, 32) == 0,
           "from_hex round-trip: data matches original");

    /* =================================================================
     * bcrypt tests (Story 29.6.1)
     * ================================================================= */

    /* Hash "password" at cost 4 */
    CryptoStrResult bcr = crypto_bcrypt_hash("password", 4);
    ASSERT(bcr.is_err == 0, "bcrypt_hash 'password' cost=4: is_err==0");
    ASSERT(bcr.ok != NULL, "bcrypt_hash 'password' cost=4: ok!=NULL");
    ASSERT(bcr.ok && strlen(bcr.ok) == 60,
           "bcrypt_hash 'password' cost=4: length==60");
    ASSERT(bcr.ok && strncmp(bcr.ok, "$2b$04$", 7) == 0,
           "bcrypt_hash 'password' cost=4: starts with $2b$04$");

    /* Verify correct password */
    ASSERT(bcr.ok && crypto_bcrypt_verify("password", bcr.ok) == 1,
           "bcrypt_verify correct password returns 1");

    /* Verify wrong password */
    ASSERT(bcr.ok && crypto_bcrypt_verify("wrong", bcr.ok) == 0,
           "bcrypt_verify wrong password returns 0");

    /* Two hashes of same password differ (different salts) */
    CryptoStrResult bcr2 = crypto_bcrypt_hash("password", 4);
    ASSERT(bcr2.is_err == 0 && bcr2.ok != NULL,
           "bcrypt_hash second call succeeds");
    ASSERT(bcr.ok && bcr2.ok && strcmp(bcr.ok, bcr2.ok) != 0,
           "bcrypt two hashes of same password differ (different salts)");

    /* Hash empty password */
    CryptoStrResult bcr_empty = crypto_bcrypt_hash("", 4);
    ASSERT(bcr_empty.is_err == 0, "bcrypt_hash empty password: is_err==0");
    ASSERT(bcr_empty.ok && strncmp(bcr_empty.ok, "$2b$04$", 7) == 0,
           "bcrypt_hash empty password: starts with $2b$04$");
    ASSERT(bcr_empty.ok && crypto_bcrypt_verify("", bcr_empty.ok) == 1,
           "bcrypt_verify empty password returns 1");
    ASSERT(bcr_empty.ok && crypto_bcrypt_verify("notempty", bcr_empty.ok) == 0,
           "bcrypt_verify nonempty vs empty-hash returns 0");

    /* Verify with mismatched hash returns 0 */
    ASSERT(bcr.ok && crypto_bcrypt_verify("password", "notahash") == 0,
           "bcrypt_verify malformed hash returns 0");

    if (failures == 0) { printf("All crypto tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

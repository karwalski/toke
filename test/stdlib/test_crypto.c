/*
 * test_crypto.c — Unit tests for the std.crypto C library (Story 2.7.3).
 *
 * Build and run: make test-stdlib-crypto
 *
 * Known-vector sources (verified with Python3 hashlib):
 *   SHA-256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
 *   SHA-256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
 *   HMAC-SHA-256(key="key", data="The quick brown fox jumps over the lazy dog") =
 *       f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8
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

    if (failures == 0) { printf("All crypto tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}

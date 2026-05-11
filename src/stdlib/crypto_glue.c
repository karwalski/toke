/*
 * crypto_glue.c — i64-ABI wrappers for std.crypto module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.crypto.
 */

#include "crypto.h"
#include <stdint.h>
#include <string.h>

int64_t tk_crypto_sha256_w(int64_t data) {
    if (!data) return 0;
    const char *s = (const char *)(intptr_t)data;
    ByteArray ba = { (const uint8_t *)s, (uint64_t)strlen(s) };
    ByteArray digest = crypto_sha256(ba);
    const char *hex = crypto_to_hex(digest);
    return hex ? (int64_t)(intptr_t)hex : 0;
}
int64_t tk_crypto_randombytes_w(int64_t n) {
    if (n <= 0) return 0;
    ByteArray ba = crypto_randombytes((uint64_t)n);
    const char *hex = crypto_to_hex(ba);
    return hex ? (int64_t)(intptr_t)hex : 0;
}
int64_t tk_crypto_hmacsha256_w(int64_t key, int64_t data) {
    if (!key || !data) return 0;
    const char *ks = (const char *)(intptr_t)key;
    const char *ds = (const char *)(intptr_t)data;
    ByteArray kba = { (const uint8_t *)ks, (uint64_t)strlen(ks) };
    ByteArray dba = { (const uint8_t *)ds, (uint64_t)strlen(ds) };
    ByteArray tag = crypto_hmac_sha256(kba, dba);
    const char *hex = crypto_to_hex(tag);
    return hex ? (int64_t)(intptr_t)hex : 0;
}

int64_t tk_crypto_sha256file_w(int64_t path) {
    if (!path) return 0;
    const char *hex = crypto_sha256file((const char *)(intptr_t)path);
    return hex ? (int64_t)(intptr_t)hex : 0;
}
int64_t tk_crypto_sha256verify_w(int64_t path, int64_t expected) {
    if (!path || !expected) return 0;
    return (int64_t)crypto_sha256verify(
        (const char *)(intptr_t)path,
        (const char *)(intptr_t)expected);
}

int64_t tk_crypto_constanttimeequal_w(int64_t a, int64_t b) {
    if (!a || !b) return 0;
    const char *as = (const char *)(intptr_t)a;
    const char *bs = (const char *)(intptr_t)b;
    ByteArray aba = { (const uint8_t *)as, (uint64_t)strlen(as) };
    ByteArray bba = { (const uint8_t *)bs, (uint64_t)strlen(bs) };
    return (int64_t)crypto_constanteq(aba, bba);
}
int64_t tk_crypto_tohex_w(int64_t data) {
    if (!data) return 0;
    const char *s = (const char *)(intptr_t)data;
    ByteArray ba = { (const uint8_t *)s, (uint64_t)strlen(s) };
    const char *hex = crypto_to_hex(ba);
    return hex ? (int64_t)(intptr_t)hex : 0;
}

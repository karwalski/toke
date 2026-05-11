/*
 * crypto_glue.c — i64-ABI wrappers for std.crypto module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.crypto.
 */

#include "crypto.h"
#include <stdint.h>

int64_t tk_crypto_sha256_w(int64_t data) { (void)data; return 0; }
int64_t tk_crypto_randombytes_w(int64_t n) { (void)n; return 0; }
int64_t tk_crypto_hmacsha256_w(int64_t key, int64_t data) { (void)key; (void)data; return 0; }

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

int64_t tk_crypto_constanttimeequal_w(int64_t a, int64_t b) { (void)a; (void)b; return 0; }
int64_t tk_crypto_tohex_w(int64_t data) { (void)data; return 0; }

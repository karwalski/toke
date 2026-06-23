/*
 * crypto_glue.c — i64-ABI wrappers for std.crypto module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.crypto.
 */

#include "crypto.h"
#include "bytes_rt.h"   /* Stage 5: [byte] pack/unpack marshalling */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Stage 5 (114.4): crypto hashes now consume real [byte] (binary-safe, holds
 * 0x00) and RETURN raw [byte] digests — not hex strings. Callers render with
 * crypto.tohex(...) / encoding.hexencode(...). This is the breaking change that
 * makes binary input (keys/IVs/digests with NUL bytes) hashable correctly. */
int64_t tk_crypto_sha256_w(int64_t data) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(data, &buf);
    ByteArray ba = { buf, n };
    ByteArray digest = crypto_sha256(ba);
    int64_t out = tk_bytes_pack(digest.data, digest.len);
    free(buf);
    return out;
}
int64_t tk_crypto_randombytes_w(int64_t n) {
    if (n <= 0) return tk_bytes_pack((const uint8_t *)"", 0);
    ByteArray ba = crypto_randombytes((uint64_t)n);
    int64_t out = tk_bytes_pack(ba.data, ba.len);
    free((void *)ba.data);
    return out;
}
int64_t tk_crypto_hmacsha256_w(int64_t key, int64_t data) {
    uint8_t *kb, *db; uint64_t kn = tk_bytes_unpack(key, &kb), dn = tk_bytes_unpack(data, &db);
    ByteArray kba = { kb, kn }, dba = { db, dn };
    ByteArray tag = crypto_hmac_sha256(kba, dba);
    int64_t out = tk_bytes_pack(tag.data, tag.len);
    free(kb); free(db);
    return out;
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
    uint8_t *ab, *bb; uint64_t an = tk_bytes_unpack(a, &ab), bn = tk_bytes_unpack(b, &bb);
    ByteArray aba = { ab, an }, bba = { bb, bn };
    int64_t r = (int64_t)crypto_constanteq(aba, bba);
    free(ab); free(bb);
    return r;
}
/* crypto.tohex([byte]) -> hex str — the canonical way to render a raw digest. */
int64_t tk_crypto_tohex_w(int64_t data) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(data, &buf);
    ByteArray ba = { buf, n };
    const char *hex = crypto_to_hex(ba);
    free(buf);
    return hex ? (int64_t)(intptr_t)hex : 0;
}

/* crypto.randomhex(n) — generate n random bytes, return as hex string */
int64_t tk_crypto_randomhex_w(int64_t n) {
    ByteArray ba = crypto_randombytes((size_t)n);
    const char *hex = crypto_to_hex(ba);
    free((void*)ba.data);
    return (int64_t)(intptr_t)hex;
}

/* crypto.sha512([byte]) -> [byte] raw digest */
int64_t tk_crypto_sha512_w(int64_t data) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(data, &buf);
    ByteArray ba = { buf, n };
    ByteArray digest = crypto_sha512(ba);
    int64_t out = tk_bytes_pack(digest.data, digest.len);
    free(buf);
    return out;
}

/* crypto.hmacsha512(key,data) -> [byte] raw tag */
int64_t tk_crypto_hmacsha512_w(int64_t key, int64_t data) {
    uint8_t *kb, *db; uint64_t kn = tk_bytes_unpack(key, &kb), dn = tk_bytes_unpack(data, &db);
    ByteArray kba = { kb, kn }, dba = { db, dn };
    ByteArray tag = crypto_hmac_sha512(kba, dba);
    int64_t out = tk_bytes_pack(tag.data, tag.len);
    free(kb); free(db);
    return out;
}

/* crypto.constanteq([byte],[byte]) -> bool */
int64_t tk_crypto_constanteq_w(int64_t a, int64_t b) {
    uint8_t *ab, *bb; uint64_t an = tk_bytes_unpack(a, &ab), bn = tk_bytes_unpack(b, &bb);
    ByteArray aba = { ab, an }, bba = { bb, bn };
    int64_t r = (int64_t)crypto_constanteq(aba, bba);
    free(ab); free(bb);
    return r;
}

/* crypto.to_hex(data) — convert bytes to hex string */
int64_t tk_crypto_to_hex_w(int64_t data) {
    return tk_crypto_tohex_w(data);
}

/* crypto.randombase64url(n) — generate n random bytes, return as base64url */
int64_t tk_crypto_randombase64url_w(int64_t n) {
    ByteArray ba = crypto_randombytes((size_t)n);
    /* Simple base64url encode */
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t olen = ((ba.len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(olen);
    if (!out) { free((void*)ba.data); return 0; }
    size_t j = 0;
    for (size_t i = 0; i < ba.len; i += 3) {
        uint32_t v = (uint32_t)ba.data[i] << 16;
        if (i + 1 < ba.len) v |= (uint32_t)ba.data[i+1] << 8;
        if (i + 2 < ba.len) v |= (uint32_t)ba.data[i+2];
        out[j++] = b64[(v >> 18) & 0x3F];
        out[j++] = b64[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < ba.len) ? b64[(v >> 6) & 0x3F] : '=';
        out[j++] = (i + 2 < ba.len) ? b64[v & 0x3F] : '=';
    }
    out[j] = '\0';
    free((void*)ba.data);
    return (int64_t)(intptr_t)out;
}

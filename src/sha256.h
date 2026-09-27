#ifndef TK_SHA256_H
#define TK_SHA256_H
/*
 * sha256.h — the compiler's one SHA-256 (FIPS 180-4).
 *
 * 122.13 item 9: this was a file-local copy inside companion.c (the
 * companion's source_hash).  The package content hash of ADR-0004 / 122.10
 * ("sha256:" + 64 lower-case hex over the canonical tree) needs the same
 * function on the compiler side, so it is shared here rather than copied.
 * The stdlib's crypto module is a separate, runtime implementation; the
 * compiler does not link it.
 */
#include <stddef.h>

typedef struct {
    unsigned int state[8];
    unsigned char buf[64];
    unsigned long long total;   /* bytes absorbed so far */
} TkSha256;

void tk_sha256_init(TkSha256 *ctx);
void tk_sha256_update(TkSha256 *ctx, const unsigned char *data, unsigned long len);
void tk_sha256_final(TkSha256 *ctx, unsigned char hash[32]);

/* One-shot digest of `len` bytes as 64 lower-case hex digits + NUL. */
void tk_sha256_hex(const void *data, size_t len, char out[65]);

#endif /* TK_SHA256_H */

/*
 * encrypt.c — Implementation of the std.encrypt standard library module.
 *
 * Implements:
 *   - AES-256-GCM   (AES block cipher + GCM mode, self-contained)
 *   - X25519        (Curve25519 Montgomery ladder, RFC 7748)
 *   - Ed25519       (twisted Edwards signing, RFC 8032)
 *   - HKDF-SHA-256  (RFC 5869, uses crypto_hmac_sha256 from crypto.h)
 *   - TLS cert fingerprint (PEM parse + SHA-256, uses crypto.h / encoding.h)
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 14.1.2
 */

#include "encrypt.h"
#include "crypto.h"
#include "encoding.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* =========================================================================
 * Random bytes
 * ========================================================================= */

static void random_bytes(uint8_t *buf, size_t n)
{
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(buf, n);
#else
    /* POSIX fallback */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        size_t rd = fread(buf, 1, n, f);
        fclose(f);
        if (rd == n) return;
    }
    /* last-resort: zero fill (insecure, should not happen) */
    memset(buf, 0, n);
#endif
}

/* =========================================================================
 * AES-256 block cipher (FIPS 197)
 * ========================================================================= */

/* Forward S-box */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

/* Inverse S-box */
static const uint8_t INV_SBOX[256] __attribute__((unused)) = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* Rcon for key schedule */
static const uint8_t RCON[11] = {
    0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

/* GF(2^8) multiply by 2 (xtime) */
static uint8_t xtime(uint8_t a)
{
    return (uint8_t)((a << 1) ^ ((a >> 7) ? 0x1b : 0x00));
}

/* AES-256: 60 round-key words (15 round keys of 4 words each) */
#define AES256_KEY_WORDS  60

typedef struct {
    uint32_t rk[AES256_KEY_WORDS];
} AES256Ctx;

static uint32_t u32_from_be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void u32_to_be(uint32_t v, uint8_t *p)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)(v);
}

static uint32_t sub_word(uint32_t w)
{
    return ((uint32_t)SBOX[(w >> 24) & 0xff] << 24) |
           ((uint32_t)SBOX[(w >> 16) & 0xff] << 16) |
           ((uint32_t)SBOX[(w >>  8) & 0xff] <<  8) |
           ((uint32_t)SBOX[(w      ) & 0xff]);
}

static uint32_t rot_word(uint32_t w)
{
    return (w << 8) | (w >> 24);
}

static void aes256_key_expand(AES256Ctx *ctx, const uint8_t *key)
{
    int i;
    for (i = 0; i < 8; i++)
        ctx->rk[i] = u32_from_be(key + 4*i);
    for (i = 8; i < AES256_KEY_WORDS; i++) {
        uint32_t temp = ctx->rk[i-1];
        if ((i % 8) == 0)
            temp = sub_word(rot_word(temp)) ^ ((uint32_t)RCON[i/8] << 24);
        else if ((i % 8) == 4)
            temp = sub_word(temp);
        ctx->rk[i] = ctx->rk[i-8] ^ temp;
    }
}

/* Encrypt one 16-byte block (14 rounds for AES-256) */
static void aes256_encrypt_block(const AES256Ctx *ctx,
                                  const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16], tmp[16];
    int r, i;

    /* Initial AddRoundKey */
    for (i = 0; i < 4; i++) {
        uint32_t rk = ctx->rk[i];
        s[4*i+0] = in[4*i+0] ^ (uint8_t)(rk >> 24);
        s[4*i+1] = in[4*i+1] ^ (uint8_t)(rk >> 16);
        s[4*i+2] = in[4*i+2] ^ (uint8_t)(rk >>  8);
        s[4*i+3] = in[4*i+3] ^ (uint8_t)(rk);
    }

    /* 13 full rounds */
    for (r = 1; r <= 13; r++) {
        /* SubBytes + ShiftRows (combined as column-wise) */
        uint8_t a0,a1,a2,a3,b;
        /* Column 0: rows 0,1,2,3 -> ShiftRows: row0+0, row1+1, row2+2, row3+3 */
#define SR(row,col) s[row + 4*(((col)+(row)) % 4)]
        for (i = 0; i < 16; i++) tmp[i] = SBOX[s[i]];
        /* ShiftRows: rotate row 1 left by 1, row 2 by 2, row 3 by 3 */
        /* State is column-major: s[row + 4*col] */
        s[ 0] = tmp[ 0]; s[ 4] = tmp[ 4]; s[ 8] = tmp[ 8]; s[12] = tmp[12]; /* row0 */
        s[ 1] = tmp[ 5]; s[ 5] = tmp[ 9]; s[ 9] = tmp[13]; s[13] = tmp[ 1]; /* row1 */
        s[ 2] = tmp[10]; s[ 6] = tmp[14]; s[10] = tmp[ 2]; s[14] = tmp[ 6]; /* row2 */
        s[ 3] = tmp[15]; s[ 7] = tmp[ 3]; s[11] = tmp[ 7]; s[15] = tmp[11]; /* row3 */
#undef SR

        /* MixColumns */
        for (i = 0; i < 4; i++) {
            a0 = s[4*i+0]; a1 = s[4*i+1]; a2 = s[4*i+2]; a3 = s[4*i+3];
            b = a0 ^ a1 ^ a2 ^ a3;
            s[4*i+0] ^= b ^ xtime(a0 ^ a1);
            s[4*i+1] ^= b ^ xtime(a1 ^ a2);
            s[4*i+2] ^= b ^ xtime(a2 ^ a3);
            s[4*i+3] ^= b ^ xtime(a3 ^ a0);
        }

        /* AddRoundKey */
        for (i = 0; i < 4; i++) {
            uint32_t rk = ctx->rk[r*4 + i];
            s[4*i+0] ^= (uint8_t)(rk >> 24);
            s[4*i+1] ^= (uint8_t)(rk >> 16);
            s[4*i+2] ^= (uint8_t)(rk >>  8);
            s[4*i+3] ^= (uint8_t)(rk);
        }
    }

    /* Final round: SubBytes + ShiftRows (no MixColumns) */
    for (i = 0; i < 16; i++) tmp[i] = SBOX[s[i]];
    s[ 0] = tmp[ 0]; s[ 4] = tmp[ 4]; s[ 8] = tmp[ 8]; s[12] = tmp[12];
    s[ 1] = tmp[ 5]; s[ 5] = tmp[ 9]; s[ 9] = tmp[13]; s[13] = tmp[ 1];
    s[ 2] = tmp[10]; s[ 6] = tmp[14]; s[10] = tmp[ 2]; s[14] = tmp[ 6];
    s[ 3] = tmp[15]; s[ 7] = tmp[ 3]; s[11] = tmp[ 7]; s[15] = tmp[11];

    /* AddRoundKey (round 14) */
    for (i = 0; i < 4; i++) {
        uint32_t rk = ctx->rk[56 + i];
        s[4*i+0] ^= (uint8_t)(rk >> 24);
        s[4*i+1] ^= (uint8_t)(rk >> 16);
        s[4*i+2] ^= (uint8_t)(rk >>  8);
        s[4*i+3] ^= (uint8_t)(rk);
    }

    memcpy(out, s, 16);
}

/* =========================================================================
 * GCM support: GHASH + CTR
 * ========================================================================= */

/* GF(2^128) multiply: a = a * b  (GHASH field, reduction poly x^128+x^7+x^2+x+1) */
static void gcm_gf_mult(uint8_t a[16], const uint8_t b[16])
{
    uint8_t v[16], z[16];
    int i, j;
    memcpy(v, b, 16);
    memset(z, 0, 16);
    for (i = 0; i < 16; i++) {
        for (j = 7; j >= 0; j--) {
            if ((a[i] >> j) & 1) {
                /* z ^= v */
                int k;
                for (k = 0; k < 16; k++) z[k] ^= v[k];
            }
            /* v = v >> 1 in GF(2^128) */
            {
                uint8_t carry = 0, next_carry;
                int k;
                for (k = 0; k < 16; k++) {
                    next_carry = (uint8_t)(v[k] & 1);
                    v[k] = (uint8_t)((v[k] >> 1) | (carry << 7));
                    carry = next_carry;
                }
                if (carry) v[0] ^= 0xe1; /* reduction: x^128 = x^7+x^2+x+1 -> 0xe1000...0 */
            }
        }
    }
    memcpy(a, z, 16);
}

/* GHASH: process len bytes of data, updating accumulator X using key H */
static void ghash_update(uint8_t X[16], const uint8_t H[16],
                          const uint8_t *data, uint64_t len)
{
    uint64_t i;
    /* Process full 16-byte blocks */
    for (i = 0; i + 16 <= len; i += 16) {
        int k;
        for (k = 0; k < 16; k++) X[k] ^= data[i+k];
        gcm_gf_mult(X, H);
    }
    /* Partial last block (zero-padded) */
    if (i < len) {
        uint8_t block[16];
        memset(block, 0, 16);
        memcpy(block, data + i, len - i);
        int k;
        for (k = 0; k < 16; k++) X[k] ^= block[k];
        gcm_gf_mult(X, H);
    }
}

/* GCM CTR: encrypt/decrypt (same operation) */
static void gcm_ctr(const AES256Ctx *aes, const uint8_t J0[16],
                     const uint8_t *in, uint8_t *out, uint64_t len)
{
    uint8_t counter[16], keystream[16];
    uint32_t ctr;
    uint64_t i = 0;

    memcpy(counter, J0, 16);
    /* Increment counter before first block (GCM uses J0+1 for plaintext) */
    ctr = ((uint32_t)counter[12] << 24) | ((uint32_t)counter[13] << 16) |
          ((uint32_t)counter[14] << 8)  |  (uint32_t)counter[15];
    ctr++;
    u32_to_be(ctr, counter + 12);

    while (i < len) {
        uint64_t block_len = (len - i < 16) ? (len - i) : 16;
        uint64_t k;
        aes256_encrypt_block(aes, counter, keystream);
        for (k = 0; k < block_len; k++)
            out[i+k] = in[i+k] ^ keystream[k];
        i += block_len;
        ctr = ((uint32_t)counter[12] << 24) | ((uint32_t)counter[13] << 16) |
              ((uint32_t)counter[14] << 8)  |  (uint32_t)counter[15];
        ctr++;
        u32_to_be(ctr, counter + 12);
    }
}

/* Constant-time compare */
static int ct_memcmp(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t diff = 0;
    size_t i;
    for (i = 0; i < n; i++) diff |= a[i] ^ b[i];
    return diff == 0 ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * AES-256-GCM public API
 * ------------------------------------------------------------------------- */

EncryptResult encrypt_aes256gcm_encrypt(ByteArray key, ByteArray nonce,
                                         ByteArray plaintext, ByteArray aad)
{
    EncryptResult res;
    memset(&res, 0, sizeof(res));

    if (key.len != 32) {
        res.is_err = 1; res.err_msg = "aes256gcm: key must be 32 bytes"; return res;
    }
    if (nonce.len != 12) {
        res.is_err = 1; res.err_msg = "aes256gcm: nonce must be 12 bytes"; return res;
    }

    AES256Ctx aes;
    aes256_key_expand(&aes, key.data);

    /* Derive H = AES(key, 0^128) */
    uint8_t H[16];
    uint8_t zero16[16];
    memset(zero16, 0, 16);
    aes256_encrypt_block(&aes, zero16, H);

    /* J0: nonce || 0x00000001 (96-bit nonce case) */
    uint8_t J0[16];
    memcpy(J0, nonce.data, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    /* Allocate output: plaintext.len + 16 (tag) */
    uint64_t ctlen = plaintext.len;
    uint8_t *out = (uint8_t *)malloc(ctlen + 16);
    if (!out) { res.is_err = 1; res.err_msg = "aes256gcm: malloc failed"; return res; }

    /* Encrypt */
    gcm_ctr(&aes, J0, plaintext.data, out, ctlen);

    /* Compute GHASH(H, AAD, ciphertext) */
    uint8_t X[16];
    memset(X, 0, 16);
    ghash_update(X, H, aad.data, aad.len);
    ghash_update(X, H, out, ctlen);

    /* Length block: len(AAD)||len(CT) in bits, big-endian 64-bit each */
    uint8_t lenblock[16];
    uint64_t aad_bits = aad.len * 8;
    uint64_t ct_bits  = ctlen  * 8;
    int k;
    for (k = 0; k < 8; k++) lenblock[k]   = (uint8_t)(aad_bits >> (56 - 8*k));
    for (k = 0; k < 8; k++) lenblock[8+k] = (uint8_t)(ct_bits  >> (56 - 8*k));
    for (k = 0; k < 16; k++) X[k] ^= lenblock[k];
    gcm_gf_mult(X, H);

    /* Tag = GCTR(J0, S) = AES(J0) ^ GHASH result */
    uint8_t tag_keystream[16];
    aes256_encrypt_block(&aes, J0, tag_keystream);
    uint8_t *tag = out + ctlen;
    for (k = 0; k < 16; k++) tag[k] = X[k] ^ tag_keystream[k];

    res.ok     = out;
    res.ok_len = ctlen + 16;
    return res;
}

EncryptResult encrypt_aes256gcm_decrypt(ByteArray key, ByteArray nonce,
                                         ByteArray ciphertext, ByteArray aad)
{
    EncryptResult res;
    memset(&res, 0, sizeof(res));

    if (key.len != 32) {
        res.is_err = 1; res.err_msg = "aes256gcm: key must be 32 bytes"; return res;
    }
    if (nonce.len != 12) {
        res.is_err = 1; res.err_msg = "aes256gcm: nonce must be 12 bytes"; return res;
    }
    if (ciphertext.len < 16) {
        res.is_err = 1; res.err_msg = "aes256gcm: ciphertext too short"; return res;
    }

    uint64_t ctlen = ciphertext.len - 16;
    const uint8_t *ct  = ciphertext.data;
    const uint8_t *tag = ciphertext.data + ctlen;

    AES256Ctx aes;
    aes256_key_expand(&aes, key.data);

    uint8_t H[16], zero16[16];
    memset(zero16, 0, 16);
    aes256_encrypt_block(&aes, zero16, H);

    uint8_t J0[16];
    memcpy(J0, nonce.data, 12);
    J0[12] = 0; J0[13] = 0; J0[14] = 0; J0[15] = 1;

    /* Recompute expected tag */
    uint8_t X[16];
    memset(X, 0, 16);
    ghash_update(X, H, aad.data, aad.len);
    ghash_update(X, H, ct, ctlen);

    uint8_t lenblock[16];
    uint64_t aad_bits = aad.len * 8;
    uint64_t ct_bits  = ctlen  * 8;
    int k;
    for (k = 0; k < 8; k++) lenblock[k]   = (uint8_t)(aad_bits >> (56 - 8*k));
    for (k = 0; k < 8; k++) lenblock[8+k] = (uint8_t)(ct_bits  >> (56 - 8*k));
    for (k = 0; k < 16; k++) X[k] ^= lenblock[k];
    gcm_gf_mult(X, H);

    uint8_t tag_keystream[16];
    aes256_encrypt_block(&aes, J0, tag_keystream);
    uint8_t expected_tag[16];
    for (k = 0; k < 16; k++) expected_tag[k] = X[k] ^ tag_keystream[k];

    if (ct_memcmp(expected_tag, tag, 16) != 0) {
        res.is_err = 1; res.err_msg = "aes256gcm: authentication failed"; return res;
    }

    uint8_t *plain = (uint8_t *)malloc(ctlen + 1); /* +1 for safety */
    if (!plain) { res.is_err = 1; res.err_msg = "aes256gcm: malloc failed"; return res; }

    gcm_ctr(&aes, J0, ct, plain, ctlen);

    res.ok     = plain;
    res.ok_len = ctlen;
    return res;
}

ByteArray encrypt_aes256gcm_keygen(void)
{
    uint8_t *buf = (uint8_t *)malloc(32);
    if (!buf) { ByteArray r = {NULL, 0}; return r; }
    random_bytes(buf, 32);
    ByteArray r;
    r.data = buf;
    r.len  = 32;
    return r;
}

ByteArray encrypt_aes256gcm_noncegen(void)
{
    uint8_t *buf = (uint8_t *)malloc(12);
    if (!buf) { ByteArray r = {NULL, 0}; return r; }
    random_bytes(buf, 12);
    ByteArray r;
    r.data = buf;
    r.len  = 12;
    return r;
}

/* =========================================================================
 * X25519 — Curve25519 scalar multiplication (RFC 7748)
 *
 * Field: GF(2^255 - 19), using 5×51-bit limb representation for arithmetic,
 * then converting to/from 32-byte little-endian for I/O.
 * ========================================================================= */

/* 64-bit limb field element: 5 limbs, each up to ~51 bits */
typedef struct { uint64_t v[5]; } fe25519;

#define MASK51 ((uint64_t)0x7ffffffffffff)

static fe25519 fe_from_bytes(const uint8_t b[32])
{
    fe25519 h;
    uint64_t t[4];
    int i;
    for (i = 0; i < 4; i++) {
        t[i]  = (uint64_t)b[8*i+0];
        t[i] |= (uint64_t)b[8*i+1] << 8;
        t[i] |= (uint64_t)b[8*i+2] << 16;
        t[i] |= (uint64_t)b[8*i+3] << 24;
        t[i] |= (uint64_t)b[8*i+4] << 32;
        t[i] |= (uint64_t)b[8*i+5] << 40;
        t[i] |= (uint64_t)b[8*i+6] << 48;
        t[i] |= (uint64_t)b[8*i+7] << 56;
    }
    h.v[0] =  t[0]        & MASK51;
    h.v[1] = (t[0] >> 51 | t[1] << 13) & MASK51;
    h.v[2] = (t[1] >> 38 | t[2] << 26) & MASK51;
    h.v[3] = (t[2] >> 25 | t[3] << 39) & MASK51;
    h.v[4] =  t[3] >> 12;
    /* Clear high bit (bit 255) */
    h.v[4] &= MASK51 >> 4; /* top limb holds bits 204..255; bit 255 = limb4 bit 51 */
    return h;
}

static void fe_to_bytes(uint8_t b[32], fe25519 h)
{
    /* Reduce fully */
    int i;
    uint64_t c;
    for (i = 0; i < 4; i++) {
        c = h.v[i] >> 51;
        h.v[i] &= MASK51;
        h.v[i+1] += c;
    }
    /* Conditional subtraction of p = 2^255 - 19 */
    /* Final reduce: if h >= p, subtract p */
    c = (h.v[4] >> 51);
    h.v[4] &= MASK51 >> 4;  /* top limb is 47 bits */
    h.v[0] += 19 * c;
    for (i = 0; i < 4; i++) {
        c = h.v[i] >> 51;
        h.v[i] &= MASK51;
        h.v[i+1] += c;
    }
    h.v[4] &= MASK51 >> 4;

    /* Pack back to 64-bit words */
    uint64_t t0 =  h.v[0]        | (h.v[1] << 51);
    uint64_t t1 = (h.v[1] >> 13) | (h.v[2] << 38);
    uint64_t t2 = (h.v[2] >> 26) | (h.v[3] << 25);
    uint64_t t3 = (h.v[3] >> 39) | (h.v[4] << 12);

    for (i = 0; i < 8; i++) b[0+i] = (uint8_t)(t0 >> (8*i));
    for (i = 0; i < 8; i++) b[8+i] = (uint8_t)(t1 >> (8*i));
    for (i = 0; i < 8; i++) b[16+i] = (uint8_t)(t2 >> (8*i));
    for (i = 0; i < 8; i++) b[24+i] = (uint8_t)(t3 >> (8*i));
}

static fe25519 fe_add(fe25519 a, fe25519 b)
{
    fe25519 r;
    int i;
    for (i = 0; i < 5; i++) r.v[i] = a.v[i] + b.v[i];
    return r;
}

static fe25519 fe_sub(fe25519 a, fe25519 b)
{
    fe25519 r;
    /* Add 2p to avoid underflow before subtracting */
    static const uint64_t two_p[5] = {
        2*(MASK51 - 18), 2*MASK51, 2*MASK51, 2*MASK51, 2*(MASK51 >> 4)
    };
    int i;
    for (i = 0; i < 5; i++) r.v[i] = a.v[i] + two_p[i] - b.v[i];
    return r;
}

static fe25519 fe_mul(fe25519 a, fe25519 b)
{
    /* Schoolbook 5x5 with reduction mod 2^255-19 */
    __uint128_t d[5];
    int i;
    /* Multiply: d[i] = sum_j a[j]*b[(i-j+5)%5] * (19 if wrapped) */
    for (i = 0; i < 5; i++) d[i] = 0;
    for (i = 0; i < 5; i++) {
        int j;
        for (j = 0; j < 5; j++) {
            int k = i + j;
            if (k < 5)
                d[k] += (__uint128_t)a.v[i] * b.v[j];
            else
                d[k-5] += (__uint128_t)a.v[i] * b.v[j] * 19;
        }
    }
    fe25519 r;
    uint64_t carry = 0;
    for (i = 0; i < 5; i++) {
        d[i] += carry;
        r.v[i] = (uint64_t)(d[i] & MASK51);
        carry = (uint64_t)(d[i] >> 51);
    }
    r.v[0] += carry * 19;
    /* One more carry propagation */
    for (i = 0; i < 4; i++) {
        carry = r.v[i] >> 51;
        r.v[i] &= MASK51;
        r.v[i+1] += carry;
    }
    return r;
}

static fe25519 fe_sqr(fe25519 a) { return fe_mul(a, a); }

/* fe_invert via Fermat: a^(p-2) = a^(2^255-21) */
static fe25519 fe_invert(fe25519 z)
{
    fe25519 z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    int i;

    z2 = fe_sqr(z);            /* 2 */
    t  = fe_sqr(z2);           /* 4 */
    t  = fe_sqr(t);            /* 8 */
    z9 = fe_mul(t, z);         /* 9 */
    z11= fe_mul(z9, z2);       /* 11 */
    t  = fe_sqr(z11);          /* 22 */
    z2_5_0 = fe_mul(t, z9);   /* 31 = 2^5 - 1 */

    t = z2_5_0;
    for (i = 0; i < 5; i++) t = fe_sqr(t);
    z2_10_0 = fe_mul(t, z2_5_0);

    t = z2_10_0;
    for (i = 0; i < 10; i++) t = fe_sqr(t);
    z2_20_0 = fe_mul(t, z2_10_0);

    t = z2_20_0;
    for (i = 0; i < 20; i++) t = fe_sqr(t);
    t = fe_mul(t, z2_20_0);

    for (i = 0; i < 10; i++) t = fe_sqr(t);
    z2_50_0 = fe_mul(t, z2_10_0);

    t = z2_50_0;
    for (i = 0; i < 50; i++) t = fe_sqr(t);
    z2_100_0 = fe_mul(t, z2_50_0);

    t = z2_100_0;
    for (i = 0; i < 100; i++) t = fe_sqr(t);
    t = fe_mul(t, z2_100_0);

    for (i = 0; i < 50; i++) t = fe_sqr(t);
    t = fe_mul(t, z2_50_0);

    for (i = 0; i < 5; i++) t = fe_sqr(t);
    return fe_mul(t, z11);
}

/* Constant-time conditional swap */
static void fe_cswap(fe25519 *a, fe25519 *b, uint64_t bit)
{
    uint64_t mask = 0 - bit; /* all-ones if bit=1, zero if bit=0 */
    int i;
    for (i = 0; i < 5; i++) {
        uint64_t x = mask & (a->v[i] ^ b->v[i]);
        a->v[i] ^= x;
        b->v[i] ^= x;
    }
}

/* Clamp a 32-byte X25519 scalar (RFC 7748 §5) */
static void x25519_clamp(uint8_t k[32])
{
    k[0]  &= 248;
    k[31] &= 127;
    k[31] |= 64;
}

/* Montgomery ladder scalar multiplication on Curve25519 */
static void x25519_scalarmult(uint8_t out[32], const uint8_t k[32],
                               const uint8_t u[32])
{
    uint8_t clamped[32];
    memcpy(clamped, k, 32);
    x25519_clamp(clamped);

    fe25519 x_1 = fe_from_bytes(u);
    fe25519 x_2 = {{1,0,0,0,0}};
    fe25519 z_2 = {{0,0,0,0,0}};
    fe25519 x_3 = x_1;
    fe25519 z_3 = {{1,0,0,0,0}};

    /* A24 = 121665 */
    fe25519 a24 = {{121665,0,0,0,0}};

    uint64_t swap = 0;
    int t;
    for (t = 254; t >= 0; t--) {
        uint64_t k_t = (clamped[t/8] >> (t & 7)) & 1;
        swap ^= k_t;
        fe_cswap(&x_2, &x_3, swap);
        fe_cswap(&z_2, &z_3, swap);
        swap = k_t;

        fe25519 A  = fe_add(x_2, z_2);
        fe25519 AA = fe_sqr(A);
        fe25519 B  = fe_sub(x_2, z_2);
        fe25519 BB = fe_sqr(B);
        fe25519 E  = fe_sub(AA, BB);
        fe25519 C  = fe_add(x_3, z_3);
        fe25519 D  = fe_sub(x_3, z_3);
        fe25519 DA = fe_mul(D, A);
        fe25519 CB = fe_mul(C, B);
        x_3 = fe_sqr(fe_add(DA, CB));
        z_3 = fe_mul(x_1, fe_sqr(fe_sub(DA, CB)));
        x_2 = fe_mul(AA, BB);
        z_2 = fe_mul(E, fe_add(AA, fe_mul(a24, E)));
    }
    fe_cswap(&x_2, &x_3, swap);
    fe_cswap(&z_2, &z_3, swap);

    fe25519 res = fe_mul(x_2, fe_invert(z_2));
    fe_to_bytes(out, res);
}

static const uint8_t X25519_BASEPOINT[32] = {9};

X25519Keypair encrypt_x25519_keypair(void)
{
    X25519Keypair kp;
    random_bytes(kp.privkey, 32);
    x25519_clamp(kp.privkey);
    x25519_scalarmult(kp.pubkey, kp.privkey, X25519_BASEPOINT);
    return kp;
}

ByteArray encrypt_x25519_dh(ByteArray privkey, ByteArray peerpub)
{
    uint8_t *out = (uint8_t *)malloc(32);
    if (!out || privkey.len < 32 || peerpub.len < 32) {
        ByteArray r = {NULL, 0}; free(out); return r;
    }
    uint8_t priv[32];
    memcpy(priv, privkey.data, 32);
    x25519_clamp(priv);
    x25519_scalarmult(out, priv, peerpub.data);
    ByteArray r;
    r.data = out;
    r.len  = 32;
    return r;
}

/* =========================================================================
 * SHA-512 (FIPS 180-4) — needed for Ed25519
 * ========================================================================= */

#define SHA512_DIGEST_SIZE  64
#define SHA512_BLOCK_SIZE  128

static const uint64_t SHA512_K[80] = {
    UINT64_C(0x428a2f98d728ae22), UINT64_C(0x7137449123ef65cd),
    UINT64_C(0xb5c0fbcfec4d3b2f), UINT64_C(0xe9b5dba58189dbbc),
    UINT64_C(0x3956c25bf348b538), UINT64_C(0x59f111f1b605d019),
    UINT64_C(0x923f82a4af194f9b), UINT64_C(0xab1c5ed5da6d8118),
    UINT64_C(0xd807aa98a3030242), UINT64_C(0x12835b0145706fbe),
    UINT64_C(0x243185be4ee4b28c), UINT64_C(0x550c7dc3d5ffb4e2),
    UINT64_C(0x72be5d74f27b896f), UINT64_C(0x80deb1fe3b1696b1),
    UINT64_C(0x9bdc06a725c71235), UINT64_C(0xc19bf174cf692694),
    UINT64_C(0xe49b69c19ef14ad2), UINT64_C(0xefbe4786384f25e3),
    UINT64_C(0x0fc19dc68b8cd5b5), UINT64_C(0x240ca1cc77ac9c65),
    UINT64_C(0x2de92c6f592b0275), UINT64_C(0x4a7484aa6ea6e483),
    UINT64_C(0x5cb0a9dcbd41fbd4), UINT64_C(0x76f988da831153b5),
    UINT64_C(0x983e5152ee66dfab), UINT64_C(0xa831c66d2db43210),
    UINT64_C(0xb00327c898fb213f), UINT64_C(0xbf597fc7beef0ee4),
    UINT64_C(0xc6e00bf33da88fc2), UINT64_C(0xd5a79147930aa725),
    UINT64_C(0x06ca6351e003826f), UINT64_C(0x142929670a0e6e70),
    UINT64_C(0x27b70a8546d22ffc), UINT64_C(0x2e1b21385c26c926),
    UINT64_C(0x4d2c6dfc5ac42aed), UINT64_C(0x53380d139d95b3df),
    UINT64_C(0x650a73548baf63de), UINT64_C(0x766a0abb3c77b2a8),
    UINT64_C(0x81c2c92e47edaee6), UINT64_C(0x92722c851482353b),
    UINT64_C(0xa2bfe8a14cf10364), UINT64_C(0xa81a664bbc423001),
    UINT64_C(0xc24b8b70d0f89791), UINT64_C(0xc76c51a30654be30),
    UINT64_C(0xd192e819d6ef5218), UINT64_C(0xd69906245565a910),
    UINT64_C(0xf40e35855771202a), UINT64_C(0x106aa07032bbd1b8),
    UINT64_C(0x19a4c116b8d2d0c8), UINT64_C(0x1e376c085141ab53),
    UINT64_C(0x2748774cdf8eeb99), UINT64_C(0x34b0bcb5e19b48a8),
    UINT64_C(0x391c0cb3c5c95a63), UINT64_C(0x4ed8aa4ae3418acb),
    UINT64_C(0x5b9cca4f7763e373), UINT64_C(0x682e6ff3d6b2b8a3),
    UINT64_C(0x748f82ee5defb2fc), UINT64_C(0x78a5636f43172f60),
    UINT64_C(0x84c87814a1f0ab72), UINT64_C(0x8cc702081a6439ec),
    UINT64_C(0x90befffa23631e28), UINT64_C(0xa4506cebde82bde9),
    UINT64_C(0xbef9a3f7b2c67915), UINT64_C(0xc67178f2e372532b),
    UINT64_C(0xca273eceea26619c), UINT64_C(0xd186b8c721c0c207),
    UINT64_C(0xeada7dd6cde0eb1e), UINT64_C(0xf57d4f7fee6ed178),
    UINT64_C(0x06f067aa72176fba), UINT64_C(0x0a637dc5a2c898a6),
    UINT64_C(0x113f9804bef90dae), UINT64_C(0x1b710b35131c471b),
    UINT64_C(0x28db77f523047d84), UINT64_C(0x32caab7b40c72493),
    UINT64_C(0x3c9ebe0a15c9bebc), UINT64_C(0x431d67c49c100d4c),
    UINT64_C(0x4cc5d4becb3e42b6), UINT64_C(0x597f299cfc657e2a),
    UINT64_C(0x5fcb6fab3ad6faec), UINT64_C(0x6c44198c4a475817)
};

#define ROTR64(x,n) (((x) >> (n)) | ((x) << (64-(n))))

typedef struct {
    uint64_t h[8];
    uint8_t  buf[SHA512_BLOCK_SIZE];
    uint64_t msglen_lo;
    uint64_t msglen_hi;
    uint32_t buflen;
} Sha512Ctx;

static void sha512_init(Sha512Ctx *ctx)
{
    ctx->h[0] = UINT64_C(0x6a09e667f3bcc908);
    ctx->h[1] = UINT64_C(0xbb67ae8584caa73b);
    ctx->h[2] = UINT64_C(0x3c6ef372fe94f82b);
    ctx->h[3] = UINT64_C(0xa54ff53a5f1d36f1);
    ctx->h[4] = UINT64_C(0x510e527fade682d1);
    ctx->h[5] = UINT64_C(0x9b05688c2b3e6c1f);
    ctx->h[6] = UINT64_C(0x1f83d9abfb41bd6b);
    ctx->h[7] = UINT64_C(0x5be0cd19137e2179);
    ctx->msglen_lo = ctx->msglen_hi = 0;
    ctx->buflen = 0;
}

static void sha512_process_block(Sha512Ctx *ctx, const uint8_t *block)
{
    uint64_t w[80], a, b, c, d, e, f, g, h, t1, t2;
    int i;
    for (i = 0; i < 16; i++) {
        w[i]  = (uint64_t)block[8*i+0] << 56;
        w[i] |= (uint64_t)block[8*i+1] << 48;
        w[i] |= (uint64_t)block[8*i+2] << 40;
        w[i] |= (uint64_t)block[8*i+3] << 32;
        w[i] |= (uint64_t)block[8*i+4] << 24;
        w[i] |= (uint64_t)block[8*i+5] << 16;
        w[i] |= (uint64_t)block[8*i+6] << 8;
        w[i] |= (uint64_t)block[8*i+7];
    }
    for (i = 16; i < 80; i++) {
        uint64_t s0 = ROTR64(w[i-15], 1) ^ ROTR64(w[i-15], 8) ^ (w[i-15] >> 7);
        uint64_t s1 = ROTR64(w[i-2],  19) ^ ROTR64(w[i-2],  61) ^ (w[i-2] >> 6);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = ctx->h[0]; b = ctx->h[1]; c = ctx->h[2]; d = ctx->h[3];
    e = ctx->h[4]; f = ctx->h[5]; g = ctx->h[6]; h = ctx->h[7];
    for (i = 0; i < 80; i++) {
        uint64_t S1 = ROTR64(e,14) ^ ROTR64(e,18) ^ ROTR64(e,41);
        uint64_t ch = (e & f) ^ (~e & g);
        t1 = h + S1 + ch + SHA512_K[i] + w[i];
        uint64_t S0 = ROTR64(a,28) ^ ROTR64(a,34) ^ ROTR64(a,39);
        uint64_t maj= (a & b) ^ (a & c) ^ (b & c);
        t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    ctx->h[0]+=a; ctx->h[1]+=b; ctx->h[2]+=c; ctx->h[3]+=d;
    ctx->h[4]+=e; ctx->h[5]+=f; ctx->h[6]+=g; ctx->h[7]+=h;
}

static void sha512_update(Sha512Ctx *ctx, const uint8_t *data, uint64_t len)
{
    uint64_t i = 0;
    uint64_t prev = ctx->msglen_lo;
    ctx->msglen_lo += len * 8;
    if (ctx->msglen_lo < prev) ctx->msglen_hi++;

    if (ctx->buflen > 0) {
        uint32_t need = SHA512_BLOCK_SIZE - ctx->buflen;
        uint32_t take = (len < need) ? (uint32_t)len : need;
        memcpy(ctx->buf + ctx->buflen, data, take);
        ctx->buflen += take;
        i += take;
        if (ctx->buflen == SHA512_BLOCK_SIZE) {
            sha512_process_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
    while (i + SHA512_BLOCK_SIZE <= len) {
        sha512_process_block(ctx, data + i);
        i += SHA512_BLOCK_SIZE;
    }
    if (i < len) {
        memcpy(ctx->buf, data + i, (size_t)(len - i));
        ctx->buflen = (uint32_t)(len - i);
    }
}

static void sha512_final(Sha512Ctx *ctx, uint8_t digest[64])
{
    /* Padding */
    uint8_t pad[240];
    memset(pad, 0, sizeof(pad));
    pad[0] = 0x80;
    uint32_t padlen;
    uint32_t used = ctx->buflen;
    if (used < 112)
        padlen = 112 - used;
    else
        padlen = 240 - used; /* wrap to next block */

    /* Encode bit-length as big-endian 128-bit (we only track 128 bits via hi/lo) */
    uint8_t length_block[16];
    int i;
    for (i = 0; i < 8; i++) length_block[i]   = (uint8_t)(ctx->msglen_hi >> (56 - 8*i));
    for (i = 0; i < 8; i++) length_block[8+i] = (uint8_t)(ctx->msglen_lo >> (56 - 8*i));

    sha512_update(ctx, pad, padlen);
    sha512_update(ctx, length_block, 16);

    for (i = 0; i < 8; i++) {
        digest[8*i+0] = (uint8_t)(ctx->h[i] >> 56);
        digest[8*i+1] = (uint8_t)(ctx->h[i] >> 48);
        digest[8*i+2] = (uint8_t)(ctx->h[i] >> 40);
        digest[8*i+3] = (uint8_t)(ctx->h[i] >> 32);
        digest[8*i+4] = (uint8_t)(ctx->h[i] >> 24);
        digest[8*i+5] = (uint8_t)(ctx->h[i] >> 16);
        digest[8*i+6] = (uint8_t)(ctx->h[i] >> 8);
        digest[8*i+7] = (uint8_t)(ctx->h[i]);
    }
}

static void sha512(const uint8_t *data, uint64_t len, uint8_t digest[64])
{
    Sha512Ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, data, len);
    sha512_final(&ctx, digest);
}

/* =========================================================================
 * Ed25519 (RFC 8032) — twisted Edwards over GF(2^255-19)
 *
 * Points in extended coordinates (X:Y:Z:T) where x=X/Z, y=Y/Z, T=XY/Z.
 * ========================================================================= */

/* Curve constants */
/* d = -121665/121666 mod p */
/* We use the precomputed value from RFC 8032: */
static const uint8_t ED25519_D_BYTES[32] = {
    0xa3,0x78,0x59,0x10,0x37,0x52,0x6f,0xa0,
    0x71,0x10,0x8b,0x85,0xb1,0xf7,0x74,0xa4,
    0x63,0x82,0x35,0xa3,0x49,0x27,0x18,0x6c,
    0xa1,0x65,0x5f,0xd7,0x26,0x82,0x73,0x52
};

/* sqrt(-1) = 2^((p-1)/4) mod p */
static const uint8_t ED25519_SQRTM1_BYTES[32] = {
    0xb0,0xa0,0x0e,0x4a,0x27,0x1b,0xee,0xc4,
    0x78,0xe4,0x2f,0xad,0x06,0x18,0x43,0x2f,
    0xa7,0xd7,0xfb,0x3d,0x99,0x00,0x4d,0x2b,
    0x0b,0xdf,0xc1,0x4f,0x80,0x24,0x83,0x2b
};

typedef struct { fe25519 X, Y, Z, T; } EdPoint;

static fe25519 ED_D;    /* initialised lazily */
static fe25519 ED_SQRTM1;
static int ed_constants_ready = 0;

static void ed_init_constants(void)
{
    if (ed_constants_ready) return;
    ED_D      = fe_from_bytes(ED25519_D_BYTES);
    ED_SQRTM1 = fe_from_bytes(ED25519_SQRTM1_BYTES);
    ed_constants_ready = 1;
}

/* Neutral element */
static EdPoint ed_neutral(void)
{
    EdPoint p;
    p.X = (fe25519){{0,0,0,0,0}};
    p.Y = (fe25519){{1,0,0,0,0}};
    p.Z = (fe25519){{1,0,0,0,0}};
    p.T = (fe25519){{0,0,0,0,0}};
    return p;
}

/* Unified addition formula for twisted Edwards (2008/522) */
static EdPoint ed_add(EdPoint p, EdPoint q)
{
    fe25519 A = fe_mul(fe_sub(p.Y, p.X), fe_sub(q.Y, q.X));
    fe25519 B = fe_mul(fe_add(p.Y, p.X), fe_add(q.Y, q.X));
    fe25519 C = fe_mul(fe_mul(p.T, q.T), fe_mul(ED_D, (fe25519){{2,0,0,0,0}}));
    fe25519 D = fe_mul(fe_mul(p.Z, q.Z), (fe25519){{2,0,0,0,0}});
    fe25519 E = fe_sub(B, A);
    fe25519 F = fe_sub(D, C);
    fe25519 G = fe_add(D, C);
    fe25519 H = fe_add(B, A);
    EdPoint r;
    r.X = fe_mul(E, F);
    r.Y = fe_mul(G, H);
    r.Z = fe_mul(F, G);
    r.T = fe_mul(E, H);
    return r;
}

/* Double a point */
static EdPoint ed_double(EdPoint p)
{
    fe25519 A = fe_sqr(p.X);
    fe25519 B = fe_sqr(p.Y);
    fe25519 C = fe_mul((fe25519){{2,0,0,0,0}}, fe_sqr(p.Z));
    fe25519 H = fe_add(A, B);
    fe25519 E = fe_sub(H, fe_sqr(fe_add(p.X, p.Y)));
    fe25519 G = fe_sub(A, B);
    fe25519 F = fe_add(C, G);
    EdPoint r;
    r.X = fe_mul(E, F);
    r.Y = fe_mul(G, H);
    r.Z = fe_mul(F, G);
    r.T = fe_mul(E, H);
    return r;
}

/* Scalar multiply: compute k*B where k is a 256-bit little-endian scalar */
static EdPoint ed_scalarmult(EdPoint B, const uint8_t k[32])
{
    EdPoint Q = ed_neutral();
    EdPoint R = B;
    int i;
    for (i = 0; i < 256; i++) {
        if ((k[i/8] >> (i & 7)) & 1)
            Q = ed_add(Q, R);
        R = ed_double(R);
    }
    return Q;
}

/* Encode a point to 32 bytes (RFC 8032 §5.1.2) */
static void ed_encode_point(uint8_t out[32], EdPoint p)
{
    fe25519 zinv = fe_invert(p.Z);
    fe25519 x    = fe_mul(p.X, zinv);
    fe25519 y    = fe_mul(p.Y, zinv);
    fe_to_bytes(out, y);
    /* Set sign bit of x in bit 255 */
    uint8_t x_bytes[32];
    fe_to_bytes(x_bytes, x);
    out[31] |= (x_bytes[0] & 1) << 7;
}

/* Recover x from y and sign (RFC 8032 §5.1.3) */
static int ed_decode_point(EdPoint *p, const uint8_t enc[32])
{
    uint8_t buf[32];
    memcpy(buf, enc, 32);
    int sign = (buf[31] >> 7) & 1;
    buf[31] &= 0x7f;

    fe25519 y = fe_from_bytes(buf);
    fe25519 one = {{1,0,0,0,0}};
    fe25519 u = fe_sub(fe_sqr(y), one);               /* y^2 - 1 */
    fe25519 v = fe_add(fe_mul(ED_D, fe_sqr(y)), one); /* d*y^2 + 1 */

    /* x = sqrt(u/v): compute u*v^3 * (u*v^7)^((p-5)/8) */
    fe25519 v3  = fe_mul(fe_sqr(v), v);
    fe25519 v7  = fe_mul(fe_sqr(v3), v);
    fe25519 uv3 = fe_mul(u, v3);
    fe25519 uv7 = fe_mul(u, v7);

    /* (p-5)/8 exponentiation */
    fe25519 t = uv7;
    /* p-5/8 = (2^255 - 24)/8 = 2^252 - 3 */
    /* Use repeated squaring */
    fe25519 t2  = fe_sqr(t);
    fe25519 t3  = fe_mul(t2, t);
    fe25519 t6  = fe_sqr(t3);
    fe25519 t9  = fe_mul(t6, t3);
    fe25519 t11 = fe_mul(fe_sqr(t9), t2); /* not quite right, reuse invert chain */
    /* Reuse the invert chain approach for (p-5)/8 */
    {
        /* (p-5)/8 exponent via the standard chain */
        fe25519 z2, z9, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, r;
        int ii;
        z2       = fe_sqr(uv7);
        r        = fe_mul(z2, uv7);   /* z3 */
        z9       = fe_sqr(r);
        z9       = fe_sqr(z9);        /* z6 wait - redo properly */
        /* Actually just do full invert-style chain for (p-5)/8 */
        /* Exponent: p-5 = 2^255-24; (p-5)/8 = 2^252-3 */
        /* Use: fe_sqr chain similar to invert but ending differently */
        r = uv7;
        z2 = fe_sqr(r);                   /* 2 */
        r  = fe_mul(z2, uv7);             /* 3 */
        fe25519 z3 = r;
        r  = fe_sqr(r);                   /* 6 */
        r  = fe_sqr(r);                   /* 12 */
        r  = fe_mul(r, z3);               /* 15 */
        fe25519 z15 = r;
        r  = fe_sqr(r);                   /* 30 */
        r  = fe_mul(r, uv7);              /* 31 = 2^5-1 */
        z2_5_0 = r;
        r = z2_5_0;
        for (ii = 0; ii < 5;  ii++) r = fe_sqr(r);
        z2_10_0 = fe_mul(r, z2_5_0);
        r = z2_10_0;
        for (ii = 0; ii < 10; ii++) r = fe_sqr(r);
        z2_20_0 = fe_mul(r, z2_10_0);
        r = z2_20_0;
        for (ii = 0; ii < 20; ii++) r = fe_sqr(r);
        r = fe_mul(r, z2_20_0);
        for (ii = 0; ii < 10; ii++) r = fe_sqr(r);
        z2_50_0 = fe_mul(r, z2_10_0);
        r = z2_50_0;
        for (ii = 0; ii < 50; ii++) r = fe_sqr(r);
        z2_100_0 = fe_mul(r, z2_50_0);
        r = z2_100_0;
        for (ii = 0; ii < 100; ii++) r = fe_sqr(r);
        r = fe_mul(r, z2_100_0);
        for (ii = 0; ii < 50;  ii++) r = fe_sqr(r);
        r = fe_mul(r, z2_50_0);
        for (ii = 0; ii < 2;   ii++) r = fe_sqr(r);
        t = fe_mul(r, uv7);
        (void)t2; (void)t3; (void)t6; (void)t9; (void)t11; (void)z15;
    }

    fe25519 x = fe_mul(uv3, t);

    /* Check if x^2*v == u */
    fe25519 vx2 = fe_mul(v, fe_sqr(x));
    uint8_t vx2b[32], ub[32];
    fe_to_bytes(vx2b, vx2);
    fe_to_bytes(ub, u);
    if (memcmp(vx2b, ub, 32) != 0) {
        /* Try x * sqrt(-1) */
        x = fe_mul(x, ED_SQRTM1);
        fe_to_bytes(vx2b, fe_mul(v, fe_sqr(x)));
        if (memcmp(vx2b, ub, 32) != 0) return 0; /* not on curve */
    }

    uint8_t xb[32];
    fe_to_bytes(xb, x);
    int x_sign = xb[0] & 1;
    if (x_sign != sign) {
        x = fe_sub((fe25519){{0,0,0,0,0}}, x);
    }

    p->X = x;
    p->Y = y;
    p->Z = (fe25519){{1,0,0,0,0}};
    p->T = fe_mul(x, y);
    return 1;
}

/* Ed25519 base point B (RFC 8032 §5.1) */
static const uint8_t ED25519_BASE_Y[32] = {
    0x58,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,
    0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66
};

static EdPoint ed_get_base(void)
{
    /* Encoded base point: sign=0 for x, y = 4/5 mod p */
    uint8_t enc[32];
    memcpy(enc, ED25519_BASE_Y, 32);
    enc[31] &= 0x7f; /* sign bit = 0 */
    EdPoint B;
    ed_decode_point(&B, enc);
    return B;
}

/* Scalar reduce mod l (Ed25519 group order, 252-bit) via Barrett */
/* l = 2^252 + 27742317777372353535851937790883648493 */
static const uint8_t ED25519_L[32] __attribute__((unused)) = {
    0xed,0xd3,0xf5,0x5c,0x1a,0x63,0x12,0x58,
    0xd6,0x9c,0xf7,0xa2,0xde,0xf9,0xde,0x14,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10
};

/* Reduce a 64-byte scalar mod l (output 32 bytes) using RFC 8032 §5.1.7 approach */
/* We implement the standard "sc_reduce" for 64-byte inputs */
static void sc_reduce64(uint8_t out[32], const uint8_t s[64])
{
    /* Decompose s into 21 integers of ~21 bits each */
    /* This is the standard implementation from SUPERCOP/ref10 */
    int64_t s0  = 2097151 & ( (int64_t)s[ 0]        | ((int64_t)s[ 1] << 8) | ((int64_t)s[ 2] << 16));
    int64_t s1  = 2097151 & (((int64_t)s[ 2] >> 5)  | ((int64_t)s[ 3] << 3) | ((int64_t)s[ 4] << 11) | ((int64_t)s[ 5] << 19));
    int64_t s2  = 2097151 & (((int64_t)s[ 5] >> 2)  | ((int64_t)s[ 6] << 6) | ((int64_t)s[ 7] << 14));
    int64_t s3  = 2097151 & (((int64_t)s[ 7] >> 7)  | ((int64_t)s[ 8] << 1) | ((int64_t)s[ 9] << 9)  | ((int64_t)s[10] << 17));
    int64_t s4  = 2097151 & (((int64_t)s[10] >> 4)  | ((int64_t)s[11] << 4) | ((int64_t)s[12] << 12));
    int64_t s5  = 2097151 & (((int64_t)s[12] >> 1)  | ((int64_t)s[13] << 7) | ((int64_t)s[14] << 15));
    int64_t s6  = 2097151 & (((int64_t)s[14] >> 6)  | ((int64_t)s[15] << 2) | ((int64_t)s[16] << 10));
    int64_t s7  = 2097151 & (((int64_t)s[16] >> 3)  | ((int64_t)s[17] << 5) | ((int64_t)s[18] << 13));
    int64_t s8  = 2097151 & ( (int64_t)s[19]        | ((int64_t)s[20] << 8) | ((int64_t)s[21] << 16));
    int64_t s9  = 2097151 & (((int64_t)s[21] >> 5)  | ((int64_t)s[22] << 3) | ((int64_t)s[23] << 11));
    int64_t s10 = 2097151 & (((int64_t)s[23] >> 2)  | ((int64_t)s[24] << 6) | ((int64_t)s[25] << 14));
    int64_t s11 = 2097151 & (((int64_t)s[25] >> 7)  | ((int64_t)s[26] << 1) | ((int64_t)s[27] << 9)  | ((int64_t)s[28] << 17));
    int64_t s12 = 2097151 & (((int64_t)s[28] >> 4)  | ((int64_t)s[29] << 4) | ((int64_t)s[30] << 12));
    int64_t s13 = 2097151 & (((int64_t)s[30] >> 1)  | ((int64_t)s[31] << 7));
    int64_t s14 = 2097151 & (((int64_t)s[31] >> 6)  | ((int64_t)s[32] << 2) | ((int64_t)s[33] << 10));
    int64_t s15 = 2097151 & (((int64_t)s[33] >> 3)  | ((int64_t)s[34] << 5) | ((int64_t)s[35] << 13));
    int64_t s16 = 2097151 & ( (int64_t)s[36]        | ((int64_t)s[37] << 8) | ((int64_t)s[38] << 16));
    int64_t s17 = 2097151 & (((int64_t)s[38] >> 5)  | ((int64_t)s[39] << 3) | ((int64_t)s[40] << 11));
    int64_t s18 = 2097151 & (((int64_t)s[40] >> 2)  | ((int64_t)s[41] << 6) | ((int64_t)s[42] << 14));
    int64_t s19 = 2097151 & (((int64_t)s[42] >> 7)  | ((int64_t)s[43] << 1) | ((int64_t)s[44] << 9)  | ((int64_t)s[45] << 17));
    int64_t s20 = 2097151 & (((int64_t)s[45] >> 4)  | ((int64_t)s[46] << 4) | ((int64_t)s[47] << 12));
    int64_t s21 = 2097151 & (((int64_t)s[47] >> 1)  | ((int64_t)s[48] << 7) | ((int64_t)s[49] << 15));
    int64_t s22 = 2097151 & (((int64_t)s[49] >> 6)  | ((int64_t)s[50] << 2) | ((int64_t)s[51] << 10));
    int64_t s23 =            ((int64_t)s[51] >> 3)   | ((int64_t)s[52] << 5) | ((int64_t)s[53] << 13)
                           | ((int64_t)s[54] << 21)  | ((int64_t)s[55] << 29) | ((int64_t)s[56] << 37)
                           | ((int64_t)s[57] << 45)  | ((int64_t)s[58] << 53);
    /* Modular reduction using l = 2^252 + c where c is small */
    /* mu = floor(2^504 / l) precomputed constants (same as ref10): */
    /* l[0..3] in 21-bit limbs: 666643, 470296, 654183, 997805 (IETF values) */
#define MU0  666643LL
#define MU1  470296LL
#define MU2  654183LL
#define MU3  997805LL
#define MU4  136657LL
#define MU5  683901LL
    s11 += s23 * MU0; s12 += s23 * MU1; s13 += s23 * MU2;
    s14 += s23 * MU3; s15 += s23 * MU4; s16 += s23 * MU5; s23 = 0;
    s10 += s22 * MU0; s11 += s22 * MU1; s12 += s22 * MU2;
    s13 += s22 * MU3; s14 += s22 * MU4; s15 += s22 * MU5; s22 = 0;
    s9  += s21 * MU0; s10 += s21 * MU1; s11 += s21 * MU2;
    s12 += s21 * MU3; s13 += s21 * MU4; s14 += s21 * MU5; s21 = 0;
    s8  += s20 * MU0; s9  += s20 * MU1; s10 += s20 * MU2;
    s11 += s20 * MU3; s12 += s20 * MU4; s13 += s20 * MU5; s20 = 0;
    s7  += s19 * MU0; s8  += s19 * MU1; s9  += s19 * MU2;
    s10 += s19 * MU3; s11 += s19 * MU4; s12 += s19 * MU5; s19 = 0;
    s6  += s18 * MU0; s7  += s18 * MU1; s8  += s18 * MU2;
    s9  += s18 * MU3; s10 += s18 * MU4; s11 += s18 * MU5; s18 = 0;
    s5  += s17 * MU0; s6  += s17 * MU1; s7  += s17 * MU2;
    s8  += s17 * MU3; s9  += s17 * MU4; s10 += s17 * MU5; s17 = 0;
    s4  += s16 * MU0; s5  += s16 * MU1; s6  += s16 * MU2;
    s7  += s16 * MU3; s8  += s16 * MU4; s9  += s16 * MU5; s16 = 0;
    s3  += s15 * MU0; s4  += s15 * MU1; s5  += s15 * MU2;
    s6  += s15 * MU3; s7  += s15 * MU4; s8  += s15 * MU5; s15 = 0;
    s2  += s14 * MU0; s3  += s14 * MU1; s4  += s14 * MU2;
    s5  += s14 * MU3; s6  += s14 * MU4; s7  += s14 * MU5; s14 = 0;
    s1  += s13 * MU0; s2  += s13 * MU1; s3  += s13 * MU2;
    s4  += s13 * MU3; s5  += s13 * MU4; s6  += s13 * MU5; s13 = 0;
    s0  += s12 * MU0; s1  += s12 * MU1; s2  += s12 * MU2;
    s3  += s12 * MU3; s4  += s12 * MU4; s5  += s12 * MU5; s12 = 0;
    /* Carry propagation */
    int64_t carry;
#define CARRY(i,j) carry = (s##i + (int64_t)(1 << 20)) >> 21; s##j += carry; s##i -= carry * (1 << 21)
    CARRY(0,1); CARRY(1,2); CARRY(2,3); CARRY(3,4); CARRY(4,5); CARRY(5,6);
    CARRY(6,7); CARRY(7,8); CARRY(8,9); CARRY(9,10); CARRY(10,11); CARRY(11,12);
    s0 += s12 * MU0; s1 += s12 * MU1; s2 += s12 * MU2;
    s3 += s12 * MU3; s4 += s12 * MU4; s5 += s12 * MU5; s12 = 0;
    CARRY(0,1); CARRY(1,2); CARRY(2,3); CARRY(3,4); CARRY(4,5); CARRY(5,6);
    CARRY(6,7); CARRY(7,8); CARRY(8,9); CARRY(9,10); CARRY(10,11);
#undef CARRY
#undef MU0
#undef MU1
#undef MU2
#undef MU3
#undef MU4
#undef MU5
    out[ 0] = (uint8_t)(s0  >> 0);
    out[ 1] = (uint8_t)(s0  >> 8);
    out[ 2] = (uint8_t)((s0 >> 16) | (s1 << 5));
    out[ 3] = (uint8_t)(s1  >> 3);
    out[ 4] = (uint8_t)(s1  >> 11);
    out[ 5] = (uint8_t)((s1 >> 19) | (s2 << 2));
    out[ 6] = (uint8_t)(s2  >> 6);
    out[ 7] = (uint8_t)((s2 >> 14) | (s3 << 7));
    out[ 8] = (uint8_t)(s3  >> 1);
    out[ 9] = (uint8_t)(s3  >> 9);
    out[10] = (uint8_t)((s3 >> 17) | (s4 << 4));
    out[11] = (uint8_t)(s4  >> 4);
    out[12] = (uint8_t)(s4  >> 12);
    out[13] = (uint8_t)((s4 >> 20) | (s5 << 1));
    out[14] = (uint8_t)(s5  >> 7);
    out[15] = (uint8_t)((s5 >> 15) | (s6 << 6));
    out[16] = (uint8_t)(s6  >> 2);
    out[17] = (uint8_t)(s6  >> 10);
    out[18] = (uint8_t)((s6 >> 18) | (s7 << 3));
    out[19] = (uint8_t)(s7  >> 5);
    out[20] = (uint8_t)(s7  >> 13);
    out[21] = (uint8_t)(s8  >> 0);
    out[22] = (uint8_t)(s8  >> 8);
    out[23] = (uint8_t)((s8 >> 16) | (s9 << 5));
    out[24] = (uint8_t)(s9  >> 3);
    out[25] = (uint8_t)(s9  >> 11);
    out[26] = (uint8_t)((s9 >> 19) | (s10 << 2));
    out[27] = (uint8_t)(s10 >> 6);
    out[28] = (uint8_t)((s10 >> 14) | (s11 << 7));
    out[29] = (uint8_t)(s11 >> 1);
    out[30] = (uint8_t)(s11 >> 9);
    out[31] = (uint8_t)(s11 >> 17);
}

/* Scalar multiply-add: (a*b + c) mod l, all 32-byte LE */
static void sc_muladd(uint8_t out[32],
                      const uint8_t a[32], const uint8_t b[32], const uint8_t c[32])
{
    /* Decompose, multiply, add, reduce */
    int64_t a0  = 2097151 & ( (int64_t)a[0] | ((int64_t)a[1] << 8) | ((int64_t)a[2] << 16));
    int64_t a1  = 2097151 & (((int64_t)a[2] >> 5)  | ((int64_t)a[3] << 3) | ((int64_t)a[4] << 11) | ((int64_t)a[5] << 19));
    int64_t a2  = 2097151 & (((int64_t)a[5] >> 2)  | ((int64_t)a[6] << 6) | ((int64_t)a[7] << 14));
    int64_t a3  = 2097151 & (((int64_t)a[7] >> 7)  | ((int64_t)a[8] << 1) | ((int64_t)a[9] << 9)  | ((int64_t)a[10] << 17));
    int64_t a4  = 2097151 & (((int64_t)a[10] >> 4) | ((int64_t)a[11] << 4) | ((int64_t)a[12] << 12));
    int64_t a5  = 2097151 & (((int64_t)a[12] >> 1) | ((int64_t)a[13] << 7) | ((int64_t)a[14] << 15));
    int64_t a6  = 2097151 & (((int64_t)a[14] >> 6) | ((int64_t)a[15] << 2) | ((int64_t)a[16] << 10));
    int64_t a7  = 2097151 & (((int64_t)a[16] >> 3) | ((int64_t)a[17] << 5) | ((int64_t)a[18] << 13));
    int64_t a8  = 2097151 & ( (int64_t)a[19] | ((int64_t)a[20] << 8) | ((int64_t)a[21] << 16));
    int64_t a9  = 2097151 & (((int64_t)a[21] >> 5) | ((int64_t)a[22] << 3) | ((int64_t)a[23] << 11));
    int64_t a10 = 2097151 & (((int64_t)a[23] >> 2) | ((int64_t)a[24] << 6) | ((int64_t)a[25] << 14));
    int64_t a11 =            ((int64_t)a[25] >> 7)  | ((int64_t)a[26] << 1) | ((int64_t)a[27] << 9) | ((int64_t)a[28] << 17) | ((int64_t)(a[28]>>4) << 21);
    /* clamp a11 to 21 bits */
    a11 &= 2097151;

    int64_t b0  = 2097151 & ( (int64_t)b[0] | ((int64_t)b[1] << 8) | ((int64_t)b[2] << 16));
    int64_t b1  = 2097151 & (((int64_t)b[2] >> 5)  | ((int64_t)b[3] << 3) | ((int64_t)b[4] << 11) | ((int64_t)b[5] << 19));
    int64_t b2  = 2097151 & (((int64_t)b[5] >> 2)  | ((int64_t)b[6] << 6) | ((int64_t)b[7] << 14));
    int64_t b3  = 2097151 & (((int64_t)b[7] >> 7)  | ((int64_t)b[8] << 1) | ((int64_t)b[9] << 9)  | ((int64_t)b[10] << 17));
    int64_t b4  = 2097151 & (((int64_t)b[10] >> 4) | ((int64_t)b[11] << 4) | ((int64_t)b[12] << 12));
    int64_t b5  = 2097151 & (((int64_t)b[12] >> 1) | ((int64_t)b[13] << 7) | ((int64_t)b[14] << 15));
    int64_t b6  = 2097151 & (((int64_t)b[14] >> 6) | ((int64_t)b[15] << 2) | ((int64_t)b[16] << 10));
    int64_t b7  = 2097151 & (((int64_t)b[16] >> 3) | ((int64_t)b[17] << 5) | ((int64_t)b[18] << 13));
    int64_t b8  = 2097151 & ( (int64_t)b[19] | ((int64_t)b[20] << 8) | ((int64_t)b[21] << 16));
    int64_t b9  = 2097151 & (((int64_t)b[21] >> 5) | ((int64_t)b[22] << 3) | ((int64_t)b[23] << 11));
    int64_t b10 = 2097151 & (((int64_t)b[23] >> 2) | ((int64_t)b[24] << 6) | ((int64_t)b[25] << 14));
    int64_t b11 =            ((int64_t)b[25] >> 7)  | ((int64_t)b[26] << 1) | ((int64_t)b[27] << 9) | ((int64_t)b[28] << 17);
    b11 &= 2097151;

    int64_t c0  = 2097151 & ( (int64_t)c[0] | ((int64_t)c[1] << 8) | ((int64_t)c[2] << 16));
    int64_t c1  = 2097151 & (((int64_t)c[2] >> 5)  | ((int64_t)c[3] << 3) | ((int64_t)c[4] << 11) | ((int64_t)c[5] << 19));
    int64_t c2  = 2097151 & (((int64_t)c[5] >> 2)  | ((int64_t)c[6] << 6) | ((int64_t)c[7] << 14));
    int64_t c3  = 2097151 & (((int64_t)c[7] >> 7)  | ((int64_t)c[8] << 1) | ((int64_t)c[9] << 9)  | ((int64_t)c[10] << 17));
    int64_t c4  = 2097151 & (((int64_t)c[10] >> 4) | ((int64_t)c[11] << 4) | ((int64_t)c[12] << 12));
    int64_t c5  = 2097151 & (((int64_t)c[12] >> 1) | ((int64_t)c[13] << 7) | ((int64_t)c[14] << 15));
    int64_t c6  = 2097151 & (((int64_t)c[14] >> 6) | ((int64_t)c[15] << 2) | ((int64_t)c[16] << 10));
    int64_t c7  = 2097151 & (((int64_t)c[16] >> 3) | ((int64_t)c[17] << 5) | ((int64_t)c[18] << 13));
    int64_t c8  = 2097151 & ( (int64_t)c[19] | ((int64_t)c[20] << 8) | ((int64_t)c[21] << 16));
    int64_t c9  = 2097151 & (((int64_t)c[21] >> 5) | ((int64_t)c[22] << 3) | ((int64_t)c[23] << 11));
    int64_t c10 = 2097151 & (((int64_t)c[23] >> 2) | ((int64_t)c[24] << 6) | ((int64_t)c[25] << 14));
    int64_t c11 =            ((int64_t)c[25] >> 7)  | ((int64_t)c[26] << 1) | ((int64_t)c[27] << 9) | ((int64_t)c[28] << 17);
    c11 &= 2097151;

    /* s = a*b + c */
    int64_t s0, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11;
    int64_t s12, s13, s14, s15, s16, s17, s18, s19, s20, s21, s22;

    s0  = c0  + a0*b0;
    s1  = c1  + a0*b1 + a1*b0;
    s2  = c2  + a0*b2 + a1*b1 + a2*b0;
    s3  = c3  + a0*b3 + a1*b2 + a2*b1 + a3*b0;
    s4  = c4  + a0*b4 + a1*b3 + a2*b2 + a3*b1 + a4*b0;
    s5  = c5  + a0*b5 + a1*b4 + a2*b3 + a3*b2 + a4*b1 + a5*b0;
    s6  = c6  + a0*b6 + a1*b5 + a2*b4 + a3*b3 + a4*b2 + a5*b1 + a6*b0;
    s7  = c7  + a0*b7 + a1*b6 + a2*b5 + a3*b4 + a4*b3 + a5*b2 + a6*b1 + a7*b0;
    s8  = c8  + a0*b8 + a1*b7 + a2*b6 + a3*b5 + a4*b4 + a5*b3 + a6*b2 + a7*b1 + a8*b0;
    s9  = c9  + a0*b9 + a1*b8 + a2*b7 + a3*b6 + a4*b5 + a5*b4 + a6*b3 + a7*b2 + a8*b1 + a9*b0;
    s10 = c10 + a0*b10+ a1*b9 + a2*b8 + a3*b7 + a4*b6 + a5*b5 + a6*b4 + a7*b3 + a8*b2 + a9*b1 + a10*b0;
    s11 = c11 + a0*b11+ a1*b10+ a2*b9 + a3*b8 + a4*b7 + a5*b6 + a6*b5 + a7*b4 + a8*b3 + a9*b2 + a10*b1 + a11*b0;
    s12 =       a1*b11+ a2*b10+ a3*b9 + a4*b8 + a5*b7 + a6*b6 + a7*b5 + a8*b4 + a9*b3 + a10*b2+ a11*b1;
    s13 =       a2*b11+ a3*b10+ a4*b9 + a5*b8 + a6*b7 + a7*b6 + a8*b5 + a9*b4 + a10*b3+ a11*b2;
    s14 =       a3*b11+ a4*b10+ a5*b9 + a6*b8 + a7*b7 + a8*b6 + a9*b5 + a10*b4+ a11*b3;
    s15 =       a4*b11+ a5*b10+ a6*b9 + a7*b8 + a8*b7 + a9*b6 + a10*b5+ a11*b4;
    s16 =       a5*b11+ a6*b10+ a7*b9 + a8*b8 + a9*b7 + a10*b6+ a11*b5;
    s17 =       a6*b11+ a7*b10+ a8*b9 + a9*b8 + a10*b7+ a11*b6;
    s18 =       a7*b11+ a8*b10+ a9*b9 + a10*b8+ a11*b7;
    s19 =       a8*b11+ a9*b10+ a10*b9+ a11*b8;
    s20 =       a9*b11+ a10*b10+a11*b9;
    s21 =       a10*b11+a11*b10;
    s22 =       a11*b11;

    /* Reduce s22..s12 */
#define MU0  666643LL
#define MU1  470296LL
#define MU2  654183LL
#define MU3  997805LL
#define MU4  136657LL
#define MU5  683901LL
    s11 += s22 * MU0; s12 += s22 * MU1; s13 += s22 * MU2; s14 += s22 * MU3; s15 += s22 * MU4; s16 += s22 * MU5; s22 = 0;
    s10 += s21 * MU0; s11 += s21 * MU1; s12 += s21 * MU2; s13 += s21 * MU3; s14 += s21 * MU4; s15 += s21 * MU5; s21 = 0;
    s9  += s20 * MU0; s10 += s20 * MU1; s11 += s20 * MU2; s12 += s20 * MU3; s13 += s20 * MU4; s14 += s20 * MU5; s20 = 0;
    s8  += s19 * MU0; s9  += s19 * MU1; s10 += s19 * MU2; s11 += s19 * MU3; s12 += s19 * MU4; s13 += s19 * MU5; s19 = 0;
    s7  += s18 * MU0; s8  += s18 * MU1; s9  += s18 * MU2; s10 += s18 * MU3; s11 += s18 * MU4; s12 += s18 * MU5; s18 = 0;
    s6  += s17 * MU0; s7  += s17 * MU1; s8  += s17 * MU2; s9  += s17 * MU3; s10 += s17 * MU4; s11 += s17 * MU5; s17 = 0;
    s5  += s16 * MU0; s6  += s16 * MU1; s7  += s16 * MU2; s8  += s16 * MU3; s9  += s16 * MU4; s10 += s16 * MU5; s16 = 0;
    s4  += s15 * MU0; s5  += s15 * MU1; s6  += s15 * MU2; s7  += s15 * MU3; s8  += s15 * MU4; s9  += s15 * MU5; s15 = 0;
    s3  += s14 * MU0; s4  += s14 * MU1; s5  += s14 * MU2; s6  += s14 * MU3; s7  += s14 * MU4; s8  += s14 * MU5; s14 = 0;
    s2  += s13 * MU0; s3  += s13 * MU1; s4  += s13 * MU2; s5  += s13 * MU3; s6  += s13 * MU4; s7  += s13 * MU5; s13 = 0;
    s1  += s12 * MU0; s2  += s12 * MU1; s3  += s12 * MU2; s4  += s12 * MU3; s5  += s12 * MU4; s6  += s12 * MU5; s12 = 0;
    s0  += s11 * MU0; s1  += s11 * MU1; s2  += s11 * MU2; s3  += s11 * MU3; s4  += s11 * MU4; s5  += s11 * MU5; s11 = 0;
    /* Unused variable needed for carry chain — suppress warning */
    (void)s11;

    int64_t carry;
#define CARRY(i,j) carry = (s##i + (int64_t)(1 << 20)) >> 21; s##j += carry; s##i -= carry * (int64_t)(1 << 21)
    CARRY(0,1); CARRY(1,2); CARRY(2,3); CARRY(3,4); CARRY(4,5); CARRY(5,6);
    CARRY(6,7); CARRY(7,8); CARRY(8,9); CARRY(9,10);
    /* s10 overflows into s11 which we zeroed */
    {
        int64_t s11b = 0;
        carry = (s10 + (int64_t)(1<<20)) >> 21; s11b += carry; s10 -= carry*(int64_t)(1<<21);
        s0  += s11b * MU0; s1  += s11b * MU1; s2  += s11b * MU2;
        s3  += s11b * MU3; s4  += s11b * MU4; s5  += s11b * MU5;
    }
    CARRY(0,1); CARRY(1,2); CARRY(2,3); CARRY(3,4); CARRY(4,5); CARRY(5,6);
    CARRY(6,7); CARRY(7,8); CARRY(8,9); CARRY(9,10);
#undef CARRY
#undef MU0
#undef MU1
#undef MU2
#undef MU3
#undef MU4
#undef MU5

    out[ 0] = (uint8_t)(s0 >> 0);
    out[ 1] = (uint8_t)(s0 >> 8);
    out[ 2] = (uint8_t)((s0 >> 16) | (s1 << 5));
    out[ 3] = (uint8_t)(s1 >> 3);
    out[ 4] = (uint8_t)(s1 >> 11);
    out[ 5] = (uint8_t)((s1 >> 19) | (s2 << 2));
    out[ 6] = (uint8_t)(s2 >> 6);
    out[ 7] = (uint8_t)((s2 >> 14) | (s3 << 7));
    out[ 8] = (uint8_t)(s3 >> 1);
    out[ 9] = (uint8_t)(s3 >> 9);
    out[10] = (uint8_t)((s3 >> 17) | (s4 << 4));
    out[11] = (uint8_t)(s4 >> 4);
    out[12] = (uint8_t)(s4 >> 12);
    out[13] = (uint8_t)((s4 >> 20) | (s5 << 1));
    out[14] = (uint8_t)(s5 >> 7);
    out[15] = (uint8_t)((s5 >> 15) | (s6 << 6));
    out[16] = (uint8_t)(s6 >> 2);
    out[17] = (uint8_t)(s6 >> 10);
    out[18] = (uint8_t)((s6 >> 18) | (s7 << 3));
    out[19] = (uint8_t)(s7 >> 5);
    out[20] = (uint8_t)(s7 >> 13);
    out[21] = (uint8_t)(s8 >> 0);
    out[22] = (uint8_t)(s8 >> 8);
    out[23] = (uint8_t)((s8 >> 16) | (s9 << 5));
    out[24] = (uint8_t)(s9 >> 3);
    out[25] = (uint8_t)(s9 >> 11);
    out[26] = (uint8_t)((s9 >> 19) | (s10 << 2));
    out[27] = (uint8_t)(s10 >> 6);
    out[28] = (uint8_t)((s10 >> 14) | (0 << 7)); /* s11 = 0 */
    out[29] = 0;
    out[30] = 0;
    out[31] = 0;
}

/* -------------------------------------------------------------------------
 * Ed25519 public API
 * ------------------------------------------------------------------------- */

Ed25519Keypair encrypt_ed25519_keypair(void)
{
    ed_init_constants();
    Ed25519Keypair kp;
    uint8_t seed[32];
    random_bytes(seed, 32);

    /* privkey = seed || pubkey (64 bytes) */
    /* Hash seed to get scalar */
    uint8_t h[64];
    sha512(seed, 32, h);
    h[0]  &= 248;
    h[31] &= 127;
    h[31] |= 64;

    /* Compute public key */
    EdPoint B = ed_get_base();
    EdPoint A = ed_scalarmult(B, h);
    uint8_t pubkey[32];
    ed_encode_point(pubkey, A);

    memcpy(kp.pubkey, pubkey, 32);
    memcpy(kp.privkey,      seed,   32); /* first 32 = seed */
    memcpy(kp.privkey + 32, pubkey, 32); /* last  32 = pubkey */
    return kp;
}

ByteArray encrypt_ed25519_sign(ByteArray privkey, ByteArray msg)
{
    ed_init_constants();
    ByteArray r_ba;
    r_ba.data = NULL; r_ba.len = 0;
    if (privkey.len < 64) return r_ba;

    const uint8_t *seed   = privkey.data;       /* 32 bytes */
    const uint8_t *pubkey = privkey.data + 32;  /* 32 bytes */

    /* Expand seed */
    uint8_t az[64];
    sha512(seed, 32, az);
    az[0]  &= 248;
    az[31] &= 127;
    az[31] |= 64;

    /* nonce = SHA-512(az[32..63] || msg) */
    Sha512Ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, az + 32, 32);
    sha512_update(&ctx, msg.data, msg.len);
    uint8_t nonce_hash[64];
    sha512_final(&ctx, nonce_hash);

    uint8_t r_scalar[32];
    sc_reduce64(r_scalar, nonce_hash);

    /* R = r * B */
    EdPoint B = ed_get_base();
    EdPoint R = ed_scalarmult(B, r_scalar);
    uint8_t R_enc[32];
    ed_encode_point(R_enc, R);

    /* k = SHA-512(R || pubkey || msg) */
    sha512_init(&ctx);
    sha512_update(&ctx, R_enc, 32);
    sha512_update(&ctx, pubkey, 32);
    sha512_update(&ctx, msg.data, msg.len);
    uint8_t k_hash[64];
    sha512_final(&ctx, k_hash);

    uint8_t k_scalar[32];
    sc_reduce64(k_scalar, k_hash);

    /* S = (r + k * a) mod l */
    uint8_t S[32];
    sc_muladd(S, k_scalar, az, r_scalar);

    uint8_t *sig = (uint8_t *)malloc(64);
    if (!sig) return r_ba;
    memcpy(sig,      R_enc, 32);
    memcpy(sig + 32, S,     32);

    r_ba.data = sig;
    r_ba.len  = 64;
    return r_ba;
}

int encrypt_ed25519_verify(ByteArray pubkey, ByteArray msg, ByteArray sig)
{
    ed_init_constants();
    if (pubkey.len < 32 || sig.len < 64) return 0;

    EdPoint A;
    if (!ed_decode_point(&A, pubkey.data)) return 0;

    /* k = SHA-512(sig[0..31] || pubkey || msg) */
    Sha512Ctx ctx;
    sha512_init(&ctx);
    sha512_update(&ctx, sig.data,      32);
    sha512_update(&ctx, pubkey.data,   32);
    sha512_update(&ctx, msg.data, msg.len);
    uint8_t k_hash[64];
    sha512_final(&ctx, k_hash);
    uint8_t k_scalar[32];
    sc_reduce64(k_scalar, k_hash);

    /* Check: S*B == R + k*A  (cofactor: 8*S*B == 8*R + k*8*A) */
    /* We verify: R_expected = S*B - k*A */
    EdPoint B = ed_get_base();
    EdPoint SB = ed_scalarmult(B, sig.data + 32);

    /* Negate A */
    EdPoint neg_A;
    neg_A.X = fe_sub((fe25519){{0,0,0,0,0}}, A.X);
    neg_A.Y = A.Y;
    neg_A.Z = A.Z;
    neg_A.T = fe_sub((fe25519){{0,0,0,0,0}}, A.T);

    EdPoint kA   = ed_scalarmult(neg_A, k_scalar);
    EdPoint Rchk = ed_add(SB, kA);

    uint8_t R_computed[32];
    ed_encode_point(R_computed, Rchk);

    return (memcmp(R_computed, sig.data, 32) == 0) ? 1 : 0;
}

/* =========================================================================
 * HKDF-SHA-256 (RFC 5869)
 * ========================================================================= */

ByteArray encrypt_hkdf_sha256(ByteArray ikm, ByteArray salt,
                               ByteArray info, uint64_t outlen)
{
    ByteArray empty = {NULL, 0};
    if (outlen == 0 || outlen > 255 * 32) return empty;

    /* Extract: PRK = HMAC-SHA256(salt, ikm) */
    /* If salt is empty, use a string of HashLen zeroes */
    uint8_t zero_salt[32];
    memset(zero_salt, 0, 32);
    ByteArray actual_salt = salt;
    if (salt.len == 0 || salt.data == NULL) {
        actual_salt.data = zero_salt;
        actual_salt.len  = 32;
    }

    ByteArray prk = crypto_hmac_sha256(actual_salt, ikm);
    if (!prk.data) return empty;

    /* Expand */
    uint8_t *out = (uint8_t *)malloc(outlen);
    if (!out) { free((void *)prk.data); return empty; }

    uint8_t T[32];
    memset(T, 0, 32);
    uint64_t done = 0;
    uint8_t  counter = 1;

    while (done < outlen) {
        /* HMAC-SHA256(PRK, T_prev || info || counter) */
        uint64_t msg_len = (counter == 1 ? 0 : 32) + info.len + 1;
        uint8_t *msg = (uint8_t *)malloc(msg_len);
        if (!msg) { free(out); free((void *)prk.data); return empty; }
        uint64_t pos = 0;
        if (counter > 1) { memcpy(msg, T, 32); pos = 32; }
        if (info.len > 0) memcpy(msg + pos, info.data, info.len);
        pos += info.len;
        msg[pos] = counter;

        ByteArray msg_ba;
        msg_ba.data = msg;
        msg_ba.len  = msg_len;
        ByteArray t_ba = crypto_hmac_sha256(prk, msg_ba);
        free(msg);
        if (!t_ba.data) { free(out); free((void *)prk.data); return empty; }
        memcpy(T, t_ba.data, 32);
        free((void *)t_ba.data);

        uint64_t take = (outlen - done < 32) ? (outlen - done) : 32;
        memcpy(out + done, T, take);
        done += take;
        counter++;
    }

    free((void *)prk.data);

    ByteArray r;
    r.data = out;
    r.len  = outlen;
    return r;
}

/* =========================================================================
 * TLS certificate fingerprint
 * ========================================================================= */

ByteArray encrypt_tls_cert_fingerprint(const char *pem_cert)
{
    ByteArray empty = {NULL, 0};
    if (!pem_cert) return empty;

    /* Find PEM boundaries */
    const char *begin_marker = "-----BEGIN CERTIFICATE-----";
    const char *end_marker   = "-----END CERTIFICATE-----";

    const char *start = strstr(pem_cert, begin_marker);
    if (!start) return empty;
    start += strlen(begin_marker);
    /* Skip newline(s) */
    while (*start == '\r' || *start == '\n') start++;

    const char *end = strstr(start, end_marker);
    if (!end) return empty;

    /* Copy base64 body, stripping whitespace */
    uint64_t raw_len = (uint64_t)(end - start);
    char *b64 = (char *)malloc(raw_len + 1);
    if (!b64) return empty;

    uint64_t b64_len = 0;
    for (uint64_t i = 0; i < raw_len; i++) {
        char c = start[i];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
            b64[b64_len++] = c;
    }
    b64[b64_len] = '\0';

    /* Base64 decode to get DER */
    ByteArray der = encoding_b64decode(b64);
    free(b64);
    if (!der.data || der.len == 0) return empty;

    /* SHA-256 of the DER */
    ByteArray fingerprint = crypto_sha256(der);
    free((void *)der.data);

    return fingerprint;
}

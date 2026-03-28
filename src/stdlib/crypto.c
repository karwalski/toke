/*
 * crypto.c — Implementation of the std.crypto standard library module.
 *
 * SHA-256 and HMAC-SHA-256 are implemented using a self-contained public
 * domain implementation (based on the B-LGPL reference implementation by
 * Olivier Gay <olivier.gay@a3.epfl.ch>), adapted to use only C99 and
 * the toke stdlib types.
 *
 * No external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 2.7.3  Branch: feature/stdlib-2.7-crypto-time-test
 */

#include "crypto.h"
#include <stdlib.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * SHA-256 core (FIPS 180-4)
 * Portable, self-contained, C99 conformant.
 * ----------------------------------------------------------------------- */

#define SHA256_DIGEST_SIZE  32
#define SHA256_BLOCK_SIZE   64

typedef struct {
    uint32_t h[8];
    uint8_t  block[SHA256_BLOCK_SIZE];
    uint32_t block_len;
    uint64_t total_len;  /* total bytes processed */
} Sha256State;

static const uint32_t SHA256_K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static void sha256_init_state(Sha256State *s)
{
    s->h[0] = 0x6a09e667UL;
    s->h[1] = 0xbb67ae85UL;
    s->h[2] = 0x3c6ef372UL;
    s->h[3] = 0xa54ff53aUL;
    s->h[4] = 0x510e527fUL;
    s->h[5] = 0x9b05688cUL;
    s->h[6] = 0x1f83d9abUL;
    s->h[7] = 0x5be0cd19UL;
    s->block_len = 0;
    s->total_len = 0;
}

#define SHR(x, n)   ((x) >> (n))
#define ROTR(x, n)  (SHR(x, n) | ((x) << (32 - (n))))

#define S0(x) (ROTR(x,  7) ^ ROTR(x, 18) ^ SHR(x,  3))
#define S1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ SHR(x, 10))
#define E0(x) (ROTR(x,  2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define E1(x) (ROTR(x,  6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define F0(x, y, z) (((x) & (y)) | ((z) & ((x) | (y))))
#define F1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))

#define SHA256_SCR(i) \
    w[i & 15] += S1(w[(i - 2) & 15]) + w[(i - 7) & 15] + S0(w[(i - 15) & 15])

#define P(a, b, c, d, e, f, g, h, x, K) \
    do {                                 \
        tmp1  = (h) + E1(e) + F1(e,f,g) + (K) + (x); \
        tmp2  = E0(a) + F0(a,b,c);       \
        (d)  += tmp1;                    \
        (h)  = tmp1 + tmp2;              \
    } while (0)

static void sha256_process_block(Sha256State *s, const uint8_t data[SHA256_BLOCK_SIZE])
{
    uint32_t tmp1, tmp2, w[16];
    uint32_t a, b, c, d, e, f, g, h;
    int i;

    /* Load block into w[0..15] big-endian */
    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)data[i * 4]     << 24)
             | ((uint32_t)data[i * 4 + 1] << 16)
             | ((uint32_t)data[i * 4 + 2] <<  8)
             | ((uint32_t)data[i * 4 + 3]);
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];

    P(a,b,c,d,e,f,g,h, w[ 0], SHA256_K[ 0]);
    P(h,a,b,c,d,e,f,g, w[ 1], SHA256_K[ 1]);
    P(g,h,a,b,c,d,e,f, w[ 2], SHA256_K[ 2]);
    P(f,g,h,a,b,c,d,e, w[ 3], SHA256_K[ 3]);
    P(e,f,g,h,a,b,c,d, w[ 4], SHA256_K[ 4]);
    P(d,e,f,g,h,a,b,c, w[ 5], SHA256_K[ 5]);
    P(c,d,e,f,g,h,a,b, w[ 6], SHA256_K[ 6]);
    P(b,c,d,e,f,g,h,a, w[ 7], SHA256_K[ 7]);
    P(a,b,c,d,e,f,g,h, w[ 8], SHA256_K[ 8]);
    P(h,a,b,c,d,e,f,g, w[ 9], SHA256_K[ 9]);
    P(g,h,a,b,c,d,e,f, w[10], SHA256_K[10]);
    P(f,g,h,a,b,c,d,e, w[11], SHA256_K[11]);
    P(e,f,g,h,a,b,c,d, w[12], SHA256_K[12]);
    P(d,e,f,g,h,a,b,c, w[13], SHA256_K[13]);
    P(c,d,e,f,g,h,a,b, w[14], SHA256_K[14]);
    P(b,c,d,e,f,g,h,a, w[15], SHA256_K[15]);

    SHA256_SCR(16); P(a,b,c,d,e,f,g,h, w[16 & 15], SHA256_K[16]);
    SHA256_SCR(17); P(h,a,b,c,d,e,f,g, w[17 & 15], SHA256_K[17]);
    SHA256_SCR(18); P(g,h,a,b,c,d,e,f, w[18 & 15], SHA256_K[18]);
    SHA256_SCR(19); P(f,g,h,a,b,c,d,e, w[19 & 15], SHA256_K[19]);
    SHA256_SCR(20); P(e,f,g,h,a,b,c,d, w[20 & 15], SHA256_K[20]);
    SHA256_SCR(21); P(d,e,f,g,h,a,b,c, w[21 & 15], SHA256_K[21]);
    SHA256_SCR(22); P(c,d,e,f,g,h,a,b, w[22 & 15], SHA256_K[22]);
    SHA256_SCR(23); P(b,c,d,e,f,g,h,a, w[23 & 15], SHA256_K[23]);
    SHA256_SCR(24); P(a,b,c,d,e,f,g,h, w[24 & 15], SHA256_K[24]);
    SHA256_SCR(25); P(h,a,b,c,d,e,f,g, w[25 & 15], SHA256_K[25]);
    SHA256_SCR(26); P(g,h,a,b,c,d,e,f, w[26 & 15], SHA256_K[26]);
    SHA256_SCR(27); P(f,g,h,a,b,c,d,e, w[27 & 15], SHA256_K[27]);
    SHA256_SCR(28); P(e,f,g,h,a,b,c,d, w[28 & 15], SHA256_K[28]);
    SHA256_SCR(29); P(d,e,f,g,h,a,b,c, w[29 & 15], SHA256_K[29]);
    SHA256_SCR(30); P(c,d,e,f,g,h,a,b, w[30 & 15], SHA256_K[30]);
    SHA256_SCR(31); P(b,c,d,e,f,g,h,a, w[31 & 15], SHA256_K[31]);
    SHA256_SCR(32); P(a,b,c,d,e,f,g,h, w[32 & 15], SHA256_K[32]);
    SHA256_SCR(33); P(h,a,b,c,d,e,f,g, w[33 & 15], SHA256_K[33]);
    SHA256_SCR(34); P(g,h,a,b,c,d,e,f, w[34 & 15], SHA256_K[34]);
    SHA256_SCR(35); P(f,g,h,a,b,c,d,e, w[35 & 15], SHA256_K[35]);
    SHA256_SCR(36); P(e,f,g,h,a,b,c,d, w[36 & 15], SHA256_K[36]);
    SHA256_SCR(37); P(d,e,f,g,h,a,b,c, w[37 & 15], SHA256_K[37]);
    SHA256_SCR(38); P(c,d,e,f,g,h,a,b, w[38 & 15], SHA256_K[38]);
    SHA256_SCR(39); P(b,c,d,e,f,g,h,a, w[39 & 15], SHA256_K[39]);
    SHA256_SCR(40); P(a,b,c,d,e,f,g,h, w[40 & 15], SHA256_K[40]);
    SHA256_SCR(41); P(h,a,b,c,d,e,f,g, w[41 & 15], SHA256_K[41]);
    SHA256_SCR(42); P(g,h,a,b,c,d,e,f, w[42 & 15], SHA256_K[42]);
    SHA256_SCR(43); P(f,g,h,a,b,c,d,e, w[43 & 15], SHA256_K[43]);
    SHA256_SCR(44); P(e,f,g,h,a,b,c,d, w[44 & 15], SHA256_K[44]);
    SHA256_SCR(45); P(d,e,f,g,h,a,b,c, w[45 & 15], SHA256_K[45]);
    SHA256_SCR(46); P(c,d,e,f,g,h,a,b, w[46 & 15], SHA256_K[46]);
    SHA256_SCR(47); P(b,c,d,e,f,g,h,a, w[47 & 15], SHA256_K[47]);
    SHA256_SCR(48); P(a,b,c,d,e,f,g,h, w[48 & 15], SHA256_K[48]);
    SHA256_SCR(49); P(h,a,b,c,d,e,f,g, w[49 & 15], SHA256_K[49]);
    SHA256_SCR(50); P(g,h,a,b,c,d,e,f, w[50 & 15], SHA256_K[50]);
    SHA256_SCR(51); P(f,g,h,a,b,c,d,e, w[51 & 15], SHA256_K[51]);
    SHA256_SCR(52); P(e,f,g,h,a,b,c,d, w[52 & 15], SHA256_K[52]);
    SHA256_SCR(53); P(d,e,f,g,h,a,b,c, w[53 & 15], SHA256_K[53]);
    SHA256_SCR(54); P(c,d,e,f,g,h,a,b, w[54 & 15], SHA256_K[54]);
    SHA256_SCR(55); P(b,c,d,e,f,g,h,a, w[55 & 15], SHA256_K[55]);
    SHA256_SCR(56); P(a,b,c,d,e,f,g,h, w[56 & 15], SHA256_K[56]);
    SHA256_SCR(57); P(h,a,b,c,d,e,f,g, w[57 & 15], SHA256_K[57]);
    SHA256_SCR(58); P(g,h,a,b,c,d,e,f, w[58 & 15], SHA256_K[58]);
    SHA256_SCR(59); P(f,g,h,a,b,c,d,e, w[59 & 15], SHA256_K[59]);
    SHA256_SCR(60); P(e,f,g,h,a,b,c,d, w[60 & 15], SHA256_K[60]);
    SHA256_SCR(61); P(d,e,f,g,h,a,b,c, w[61 & 15], SHA256_K[61]);
    SHA256_SCR(62); P(c,d,e,f,g,h,a,b, w[62 & 15], SHA256_K[62]);
    SHA256_SCR(63); P(b,c,d,e,f,g,h,a, w[63 & 15], SHA256_K[63]);

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha256_update_state(Sha256State *s, const uint8_t *data, uint64_t len)
{
    uint64_t i;
    for (i = 0; i < len; i++) {
        s->block[s->block_len++] = data[i];
        if (s->block_len == SHA256_BLOCK_SIZE) {
            sha256_process_block(s, s->block);
            s->block_len = 0;
        }
    }
    s->total_len += len;
}

static void sha256_finish(Sha256State *s, uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint64_t bit_len;
    uint32_t i;

    /* Append 0x80 */
    s->block[s->block_len++] = 0x80;

    /* If not enough room for 8-byte length field, flush block */
    if (s->block_len > 56) {
        while (s->block_len < SHA256_BLOCK_SIZE)
            s->block[s->block_len++] = 0x00;
        sha256_process_block(s, s->block);
        s->block_len = 0;
    }

    /* Zero-pad to byte 56 */
    while (s->block_len < 56)
        s->block[s->block_len++] = 0x00;

    /* Append big-endian bit length */
    bit_len = s->total_len * 8;
    s->block[56] = (uint8_t)(bit_len >> 56);
    s->block[57] = (uint8_t)(bit_len >> 48);
    s->block[58] = (uint8_t)(bit_len >> 40);
    s->block[59] = (uint8_t)(bit_len >> 32);
    s->block[60] = (uint8_t)(bit_len >> 24);
    s->block[61] = (uint8_t)(bit_len >> 16);
    s->block[62] = (uint8_t)(bit_len >>  8);
    s->block[63] = (uint8_t)(bit_len);

    sha256_process_block(s, s->block);

    /* Output digest in big-endian */
    for (i = 0; i < 8; i++) {
        digest[i * 4]     = (uint8_t)(s->h[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(s->h[i] >>  8);
        digest[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

/* Single-call SHA-256. */
static void sha256_oneshot(const uint8_t *data, uint64_t len,
                           uint8_t digest[SHA256_DIGEST_SIZE])
{
    Sha256State s;
    sha256_init_state(&s);
    if (data && len > 0)
        sha256_update_state(&s, data, len);
    sha256_finish(&s, digest);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */

ByteArray crypto_sha256(ByteArray data)
{
    ByteArray result = {NULL, 0};
    uint8_t *digest = malloc(SHA256_DIGEST_SIZE);
    if (!digest) return result;
    sha256_oneshot(data.data, data.len, digest);
    result.data = digest;
    result.len  = SHA256_DIGEST_SIZE;
    return result;
}

ByteArray crypto_hmac_sha256(ByteArray key, ByteArray data)
{
    /* RFC 2104: HMAC = H( (K XOR opad) || H( (K XOR ipad) || msg ) ) */
    uint8_t k_block[SHA256_BLOCK_SIZE];
    uint8_t inner[SHA256_DIGEST_SIZE];
    uint8_t *mac = malloc(SHA256_DIGEST_SIZE);
    ByteArray result = {NULL, 0};
    Sha256State s;
    int i;

    if (!mac) return result;

    /* Derive key block */
    memset(k_block, 0, sizeof(k_block));
    if (key.data && key.len > 0) {
        if (key.len > SHA256_BLOCK_SIZE) {
            sha256_oneshot(key.data, key.len, k_block);
        } else {
            memcpy(k_block, key.data, (size_t)key.len);
        }
    }

    /* Inner hash: SHA-256( (K XOR ipad) || message ) */
    sha256_init_state(&s);
    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        k_block[i] ^= 0x36;
    sha256_update_state(&s, k_block, SHA256_BLOCK_SIZE);
    if (data.data && data.len > 0)
        sha256_update_state(&s, data.data, data.len);
    sha256_finish(&s, inner);

    /* Restore opad: k_block currently has ipad applied, un-apply then apply opad */
    for (i = 0; i < SHA256_BLOCK_SIZE; i++)
        k_block[i] ^= (0x36 ^ 0x5c);

    /* Outer hash: SHA-256( (K XOR opad) || inner ) */
    sha256_init_state(&s);
    sha256_update_state(&s, k_block, SHA256_BLOCK_SIZE);
    sha256_update_state(&s, inner, SHA256_DIGEST_SIZE);
    sha256_finish(&s, mac);

    result.data = mac;
    result.len  = SHA256_DIGEST_SIZE;
    return result;
}

const char *crypto_to_hex(ByteArray data)
{
    static const char hex_chars[] = "0123456789abcdef";
    uint64_t i;
    char *out;

    if (!data.data || data.len == 0) {
        out = malloc(1);
        if (out) out[0] = '\0';
        return out;
    }

    out = malloc((size_t)data.len * 2 + 1);
    if (!out) return NULL;

    for (i = 0; i < data.len; i++) {
        out[i * 2]     = hex_chars[(data.data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = hex_chars[data.data[i] & 0x0F];
    }
    out[data.len * 2] = '\0';
    return out;
}

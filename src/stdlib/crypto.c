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
#include <stdlib.h>   /* malloc, arc4random_buf (macOS/BSD) */
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
 * SHA-512 core (FIPS 180-4)
 * Portable, self-contained, C99 conformant.
 * Uses 64-bit words and 80 rounds (vs SHA-256's 32-bit/64 rounds).
 * ----------------------------------------------------------------------- */

#define SHA512_DIGEST_SIZE  64
#define SHA512_BLOCK_SIZE  128

typedef struct {
    uint64_t h[8];
    uint8_t  block[SHA512_BLOCK_SIZE];
    uint32_t block_len;
    uint64_t total_len;  /* total bytes processed (low 64 bits) */
} Sha512State;

static const uint64_t SHA512_K[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL,
    0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
    0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
    0xd807aa98a3030242ULL, 0x12835b0145706fbeULL,
    0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL,
    0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
    0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
    0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL,
    0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
    0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
    0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL,
    0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL,
    0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
    0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
    0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL,
    0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL,
    0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
    0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
    0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL,
    0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL,
    0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
    0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
    0xca273eceea26619cULL, 0xd186b8c721c0c207ULL,
    0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL,
    0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
    0x28db77f523047d84ULL, 0x32caab7b40c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
    0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL,
    0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static void sha512_init_state(Sha512State *s)
{
    s->h[0] = 0x6a09e667f3bcc908ULL;
    s->h[1] = 0xbb67ae8584caa73bULL;
    s->h[2] = 0x3c6ef372fe94f82bULL;
    s->h[3] = 0xa54ff53a5f1d36f1ULL;
    s->h[4] = 0x510e527fade682d1ULL;
    s->h[5] = 0x9b05688c2b3e6c1fULL;
    s->h[6] = 0x1f83d9abfb41bd6bULL;
    s->h[7] = 0x5be0cd19137e2179ULL;
    s->block_len = 0;
    s->total_len = 0;
}

/* SHA-512 uses 64-bit rotations */
#define SHR64(x, n)   ((x) >> (n))
#define ROTR64(x, n)  (SHR64(x, n) | ((x) << (64 - (n))))

#define S512_0(x) (ROTR64(x,  1) ^ ROTR64(x,  8) ^ SHR64(x, 7))
#define S512_1(x) (ROTR64(x, 19) ^ ROTR64(x, 61) ^ SHR64(x, 6))
#define E512_0(x) (ROTR64(x, 28) ^ ROTR64(x, 34) ^ ROTR64(x, 39))
#define E512_1(x) (ROTR64(x, 14) ^ ROTR64(x, 18) ^ ROTR64(x, 41))
/* F0 and F1 are the same logic as SHA-256, reused via macro names */
#define F512_0(x, y, z) (((x) & (y)) | ((z) & ((x) | (y))))
#define F512_1(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))

static void sha512_process_block(Sha512State *s, const uint8_t data[SHA512_BLOCK_SIZE])
{
    uint64_t w[16];
    uint64_t a, b, c, d, e, f, g, h, tmp1, tmp2;
    int i;

    /* Load block into w[0..15] big-endian */
    for (i = 0; i < 16; i++) {
        w[i] = ((uint64_t)data[i * 8]     << 56)
             | ((uint64_t)data[i * 8 + 1] << 48)
             | ((uint64_t)data[i * 8 + 2] << 40)
             | ((uint64_t)data[i * 8 + 3] << 32)
             | ((uint64_t)data[i * 8 + 4] << 24)
             | ((uint64_t)data[i * 8 + 5] << 16)
             | ((uint64_t)data[i * 8 + 6] <<  8)
             | ((uint64_t)data[i * 8 + 7]);
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];

    for (i = 0; i < 80; i++) {
        if (i >= 16) {
            w[i & 15] += S512_1(w[(i - 2) & 15])
                       + w[(i - 7) & 15]
                       + S512_0(w[(i - 15) & 15]);
        }
        tmp1 = h + E512_1(e) + F512_1(e, f, g) + SHA512_K[i] + w[i & 15];
        tmp2 = E512_0(a) + F512_0(a, b, c);
        h = g; g = f; f = e; e = d + tmp1;
        d = c; c = b; b = a; a = tmp1 + tmp2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha512_update_state(Sha512State *s, const uint8_t *data, uint64_t len)
{
    uint64_t i;
    for (i = 0; i < len; i++) {
        s->block[s->block_len++] = data[i];
        if (s->block_len == SHA512_BLOCK_SIZE) {
            sha512_process_block(s, s->block);
            s->block_len = 0;
        }
    }
    s->total_len += len;
}

static void sha512_finish(Sha512State *s, uint8_t digest[SHA512_DIGEST_SIZE])
{
    uint64_t bit_len;
    uint32_t i;

    /* Append 0x80 */
    s->block[s->block_len++] = 0x80;

    /* If not enough room for 16-byte length field, flush block */
    if (s->block_len > 112) {
        while (s->block_len < SHA512_BLOCK_SIZE)
            s->block[s->block_len++] = 0x00;
        sha512_process_block(s, s->block);
        s->block_len = 0;
    }

    /* Zero-pad to byte 112 */
    while (s->block_len < 112)
        s->block[s->block_len++] = 0x00;

    /* Append big-endian bit length (128-bit, high 64 bits always 0 here) */
    bit_len = s->total_len * 8;
    s->block[112] = 0;  /* high 64 bits = 0 */
    s->block[113] = 0;
    s->block[114] = 0;
    s->block[115] = 0;
    s->block[116] = 0;
    s->block[117] = 0;
    s->block[118] = 0;
    s->block[119] = 0;
    s->block[120] = (uint8_t)(bit_len >> 56);
    s->block[121] = (uint8_t)(bit_len >> 48);
    s->block[122] = (uint8_t)(bit_len >> 40);
    s->block[123] = (uint8_t)(bit_len >> 32);
    s->block[124] = (uint8_t)(bit_len >> 24);
    s->block[125] = (uint8_t)(bit_len >> 16);
    s->block[126] = (uint8_t)(bit_len >>  8);
    s->block[127] = (uint8_t)(bit_len);

    sha512_process_block(s, s->block);

    /* Output digest in big-endian */
    for (i = 0; i < 8; i++) {
        digest[i * 8]     = (uint8_t)(s->h[i] >> 56);
        digest[i * 8 + 1] = (uint8_t)(s->h[i] >> 48);
        digest[i * 8 + 2] = (uint8_t)(s->h[i] >> 40);
        digest[i * 8 + 3] = (uint8_t)(s->h[i] >> 32);
        digest[i * 8 + 4] = (uint8_t)(s->h[i] >> 24);
        digest[i * 8 + 5] = (uint8_t)(s->h[i] >> 16);
        digest[i * 8 + 6] = (uint8_t)(s->h[i] >>  8);
        digest[i * 8 + 7] = (uint8_t)(s->h[i]);
    }
}

/* Single-call SHA-512. */
static void sha512_oneshot(const uint8_t *data, uint64_t len,
                           uint8_t digest[SHA512_DIGEST_SIZE])
{
    Sha512State s;
    sha512_init_state(&s);
    if (data && len > 0)
        sha512_update_state(&s, data, len);
    sha512_finish(&s, digest);
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

ByteArray crypto_sha512(ByteArray data)
{
    ByteArray result = {NULL, 0};
    uint8_t *digest = malloc(SHA512_DIGEST_SIZE);
    if (!digest) return result;
    sha512_oneshot(data.data, data.len, digest);
    result.data = digest;
    result.len  = SHA512_DIGEST_SIZE;
    return result;
}

ByteArray crypto_hmac_sha512(ByteArray key, ByteArray data)
{
    /* RFC 2104: HMAC = H( (K XOR opad) || H( (K XOR ipad) || msg ) ) */
    uint8_t k_block[SHA512_BLOCK_SIZE];
    uint8_t inner[SHA512_DIGEST_SIZE];
    uint8_t *mac = malloc(SHA512_DIGEST_SIZE);
    ByteArray result = {NULL, 0};
    Sha512State s;
    int i;

    if (!mac) return result;

    /* Derive key block */
    memset(k_block, 0, sizeof(k_block));
    if (key.data && key.len > 0) {
        if (key.len > SHA512_BLOCK_SIZE) {
            sha512_oneshot(key.data, key.len, k_block);
        } else {
            memcpy(k_block, key.data, (size_t)key.len);
        }
    }

    /* Inner hash: SHA-512( (K XOR ipad) || message ) */
    sha512_init_state(&s);
    for (i = 0; i < SHA512_BLOCK_SIZE; i++)
        k_block[i] ^= 0x36;
    sha512_update_state(&s, k_block, SHA512_BLOCK_SIZE);
    if (data.data && data.len > 0)
        sha512_update_state(&s, data.data, data.len);
    sha512_finish(&s, inner);

    /* Restore opad: k_block currently has ipad applied, un-apply then apply opad */
    for (i = 0; i < SHA512_BLOCK_SIZE; i++)
        k_block[i] ^= (0x36 ^ 0x5c);

    /* Outer hash: SHA-512( (K XOR opad) || inner ) */
    sha512_init_state(&s);
    sha512_update_state(&s, k_block, SHA512_BLOCK_SIZE);
    sha512_update_state(&s, inner, SHA512_DIGEST_SIZE);
    sha512_finish(&s, mac);

    result.data = mac;
    result.len  = SHA512_DIGEST_SIZE;
    return result;
}

int crypto_constanteq(ByteArray a, ByteArray b)
{
    uint64_t i;
    volatile uint8_t diff = 0;

    if (a.len != b.len) return 0;
    if (a.len == 0)     return 1;
    if (!a.data || !b.data) return 0;

    for (i = 0; i < a.len; i++)
        diff |= a.data[i] ^ b.data[i];

    return diff == 0 ? 1 : 0;
}

ByteArray crypto_randombytes(uint64_t n)
{
    ByteArray result = {NULL, 0};
    uint8_t *buf;

    if (n == 0) {
        result.len = 0;
        return result;
    }

    buf = malloc((size_t)n);
    if (!buf) return result;

    arc4random_buf(buf, (size_t)n);
    result.data = buf;
    result.len  = n;
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

/* -----------------------------------------------------------------------
 * crypto_from_hex — Story 29.6.1
 * ----------------------------------------------------------------------- */

const char *crypto_from_hex(const char *hex, ByteArray *out)
{
    size_t hex_len;
    size_t byte_len;
    uint8_t *buf;
    size_t i;

    out->data = NULL;
    out->len  = 0;

    if (!hex) return "crypto_from_hex: NULL input";

    hex_len = strlen(hex);
    if (hex_len % 2 != 0)
        return "crypto_from_hex: odd-length hex string";

    byte_len = hex_len / 2;
    if (byte_len == 0) {
        /* empty hex string -> empty ByteArray (data=NULL, len=0 is fine) */
        return NULL;
    }

    buf = (uint8_t *)malloc(byte_len);
    if (!buf) return "crypto_from_hex: out of memory";

    for (i = 0; i < byte_len; i++) {
        unsigned int hi, lo;
        char ch;

        ch = hex[i * 2];
        if      (ch >= '0' && ch <= '9') hi = (unsigned int)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') hi = (unsigned int)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') hi = (unsigned int)(ch - 'A' + 10);
        else { free(buf); return "crypto_from_hex: invalid hex character"; }

        ch = hex[i * 2 + 1];
        if      (ch >= '0' && ch <= '9') lo = (unsigned int)(ch - '0');
        else if (ch >= 'a' && ch <= 'f') lo = (unsigned int)(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') lo = (unsigned int)(ch - 'A' + 10);
        else { free(buf); return "crypto_from_hex: invalid hex character"; }

        buf[i] = (uint8_t)((hi << 4) | lo);
    }

    out->data = buf;
    out->len  = (uint64_t)byte_len;
    return NULL;
}

/* -----------------------------------------------------------------------
 * Blowfish — used by bcrypt (Story 29.6.1)
 *
 * Constants are the fractional parts of pi, taken from the public-domain
 * Blowfish specification by Bruce Schneier.  These values are also used
 * verbatim in OpenBSD's blowfish.c (public domain).
 * ----------------------------------------------------------------------- */

#define BF_ROUNDS 16

typedef struct {
    uint32_t P[18];
    uint32_t S[4][256];
} BlowfishCtx;

/* P-array initialization (pi fractional digits) */
static const uint32_t BF_P_INIT[18] = {
    0x243f6a88UL, 0x85a308d3UL, 0x13198a2eUL, 0x03707344UL,
    0xa4093822UL, 0x299f31d0UL, 0x082efa98UL, 0xec4e6c89UL,
    0x452821e6UL, 0x38d01377UL, 0xbe5466cfUL, 0x34e90c6cUL,
    0xc0ac29b7UL, 0xc97c50ddUL, 0x3f84d5b5UL, 0xb5470917UL,
    0x9216d5d9UL, 0x8979fb1bUL
};

/* S-box initialization (pi fractional digits, continued) */
static const uint32_t BF_S_INIT[4][256] = {
  { /* S[0] */
    0xd1310ba6UL, 0x98dfb5acUL, 0x2ffd72dbUL, 0xd01adfb7UL,
    0xb8e1afedUL, 0x6a267e96UL, 0xba7c9045UL, 0xf12c7f99UL,
    0x24a19947UL, 0xb3916cf7UL, 0x0801f2e2UL, 0x858efc16UL,
    0x636920d8UL, 0x71574e69UL, 0xa458fea3UL, 0xf4933d7eUL,
    0x0d95748fUL, 0x728eb658UL, 0x718bcd58UL, 0x82154aeeUL,
    0x7b54a41dUL, 0xc25a59b5UL, 0x9c30d539UL, 0x2af26013UL,
    0xc5d1b023UL, 0x286085f0UL, 0xca417918UL, 0xb8db38efUL,
    0x8e79dcb0UL, 0x603a180eUL, 0x6c9e0e8bUL, 0xb01e8a3eUL,
    0xd71577c1UL, 0xbd314b27UL, 0x78af2fdaUL, 0x55605c60UL,
    0xe65525f3UL, 0xaa55ab94UL, 0x57489862UL, 0x63e81440UL,
    0x55ca396aUL, 0x2aab10b6UL, 0xb4cc5c34UL, 0x1141e8ceUL,
    0xa15486afUL, 0x7c72e993UL, 0xb3ee1411UL, 0x636fbc2aUL,
    0x2ba9c55dUL, 0x741831f6UL, 0xce5c3e16UL, 0x9b87931eUL,
    0xafd6ba33UL, 0x6c24cf5cUL, 0x7a325381UL, 0x28958677UL,
    0x3b8f4898UL, 0x6b4bb9afUL, 0xc4bfe81bUL, 0x66282193UL,
    0x61d809ccUL, 0xfb21a991UL, 0x487cac60UL, 0x5dec8032UL,
    0xef845d5dUL, 0xe98575b1UL, 0xdc262302UL, 0xeb651b88UL,
    0x23893e81UL, 0xd396acc5UL, 0x0f6d6ff3UL, 0x83f44239UL,
    0x2e0b4482UL, 0xa4842004UL, 0x69c8f04aUL, 0x9e1f9b5eUL,
    0x21c66842UL, 0xf6e96c9aUL, 0x670c9c61UL, 0xabd388f0UL,
    0x6a51a0d2UL, 0xd8542f68UL, 0x960fa728UL, 0xab5133a3UL,
    0x6eef0b6cUL, 0x137a3be4UL, 0xba3bf050UL, 0x7efb2a98UL,
    0xa1f1651dUL, 0x39af0176UL, 0x66ca593eUL, 0x82430e88UL,
    0x8cee8619UL, 0x456f9fb4UL, 0x7d84a5c3UL, 0x3b8b5ebeUL,
    0xe06f75d8UL, 0x85c12073UL, 0x401a449fUL, 0x56c16aa6UL,
    0x4ed3aa62UL, 0x363f7706UL, 0x1bfedf72UL, 0x429b023dUL,
    0x37d0d724UL, 0xd00a1248UL, 0xdb0fead3UL, 0x49f1c09bUL,
    0x075372c9UL, 0x80991b7bUL, 0x25d479d8UL, 0xf6e8def7UL,
    0xe3fe501aUL, 0xb6794c3bUL, 0x976ce0bdUL, 0x04c006baUL,
    0xc1a94fb6UL, 0x409f60c4UL, 0x5e5c9ec2UL, 0x196a2463UL,
    0x68fb6fafUL, 0x3e6c53b5UL, 0x1339b2ebUL, 0x3b52ec6fUL,
    0x6dfc511fUL, 0x9b30952cUL, 0xcc814544UL, 0xaf5ebd09UL,
    0xbee3d004UL, 0xde334afdUL, 0x660f2807UL, 0x192e4bb3UL,
    0xc0cba857UL, 0x45c8740fUL, 0xd20b5f39UL, 0xb9d3fbdbUL,
    0x5579c0bdUL, 0x1a60320aUL, 0xd6a100c6UL, 0x402c7279UL,
    0x679f25feUL, 0xfb1fa3ccUL, 0x8ea5e9f8UL, 0xdb3222f8UL,
    0x3c7516dfUL, 0xfd616b15UL, 0x2f501ec8UL, 0xad0552abUL,
    0x323db5faUL, 0xfd238760UL, 0x53317b48UL, 0x3e00df82UL,
    0x9e5c57bbUL, 0xca6f8ca0UL, 0x1a87562eUL, 0xdf1769dbUL,
    0xd542a8f6UL, 0x287effc3UL, 0xac6732c6UL, 0x8c4f5573UL,
    0x695b27b0UL, 0xbbca58c8UL, 0xe1ffa35dUL, 0xb8f011a0UL,
    0x10fa3d98UL, 0xfd2183b8UL, 0x4afcb56cUL, 0x2dd1d35bUL,
    0x9a53e479UL, 0xb6f84565UL, 0xd28e49bcUL, 0x4bfb9790UL,
    0xe1ddf2daUL, 0xa4cb7e33UL, 0x62fb1341UL, 0xcee4c6e8UL,
    0xef20cadaUL, 0x36774c01UL, 0xd07e9efeUL, 0x2bf11fb4UL,
    0x95dbda4dUL, 0xae909198UL, 0xeaad8e71UL, 0x6b93d5a0UL,
    0xd08ed1d0UL, 0xafc725e0UL, 0x8e3c5b2fUL, 0x8e7594b7UL,
    0x8ff6e2fbUL, 0xf2122b64UL, 0x8888b812UL, 0x900df01cUL,
    0x4fad5ea0UL, 0x688fc31cUL, 0xd1cff191UL, 0xb3a8c1adUL,
    0x2f2f2218UL, 0xbe0e1777UL, 0xea752dfeUL, 0x8b021fa1UL,
    0xe5a0cc0fUL, 0xb56f74e8UL, 0x18acf3d6UL, 0xce89e299UL,
    0xb4a84fe0UL, 0xfd13e0b7UL, 0x7cc43b81UL, 0xd2ada8d9UL,
    0x165fa266UL, 0x80957705UL, 0x93cc7314UL, 0x211a1477UL,
    0xe6ad2065UL, 0x77b5fa86UL, 0xc75442f5UL, 0xfb9d35cfUL,
    0xebcdaf0cUL, 0x7b3e89a0UL, 0xd6411bd3UL, 0xae1e7e49UL,
    0x00250e2dUL, 0x2071b35eUL, 0x226800bbUL, 0x57b8e0afUL,
    0x2464369bUL, 0xf009b91eUL, 0x5563911dUL, 0x59dfa6aaUL,
    0x78c14389UL, 0xd95a537fUL, 0x207d5ba2UL, 0x02e5b9c5UL,
    0x83260376UL, 0x6295cfa9UL, 0x11c81968UL, 0x4e734a41UL,
    0xb3472dcaUL, 0x7b14a94aUL, 0x1b510052UL, 0x9a532915UL,
    0xd60f573fUL, 0xbc9bc6e4UL, 0x2b60a476UL, 0x81e67400UL,
    0x08ba6fb5UL, 0x571be91fUL, 0xf296ec6bUL, 0x2a0dd915UL,
    0xb6636521UL, 0xe7b9f9b6UL, 0xff34052eUL, 0xc5855664UL,
    0x53b02d5dUL, 0xa99f8fa1UL, 0x08ba4799UL, 0x6e85076aUL
  },
  { /* S[1] */
    0x4b7a70e9UL, 0xb5b32944UL, 0xdb75092eUL, 0xc4192623UL,
    0xad6ea6b0UL, 0x49a7df7dUL, 0x9cee60b8UL, 0x8fedb266UL,
    0xecaa8c71UL, 0x699a17ffUL, 0x5664526cUL, 0xc2b19ee1UL,
    0x193602a5UL, 0x75094c29UL, 0xa0591340UL, 0xe4183a3eUL,
    0x3f54989aUL, 0x5b429d65UL, 0x6b8fe4d6UL, 0x99f73fd6UL,
    0xa1d29c07UL, 0xefe830f5UL, 0x4d2d38e6UL, 0xf0255dc1UL,
    0x4cdd2086UL, 0x8470eb26UL, 0x6382e9c6UL, 0x021ecc5eUL,
    0x09686b3fUL, 0x3ebaefc9UL, 0x3c971814UL, 0x6b6a70a1UL,
    0x687f3584UL, 0x52a0e286UL, 0xb79c5305UL, 0xaa500737UL,
    0x3e07841cUL, 0x7fdeae5cUL, 0x8e7d44ecUL, 0x5716f2b8UL,
    0xb03ada37UL, 0xf0500c0dUL, 0xf01c1f04UL, 0x0200b3ffUL,
    0xae0cf51aUL, 0x3cb574b2UL, 0x25837a58UL, 0xdc0921bdUL,
    0xd19113f9UL, 0x7ca92ff6UL, 0x94324773UL, 0x22f54701UL,
    0x3ae5e581UL, 0x37c2dadfUL, 0xc8b57634UL, 0x9af3dda7UL,
    0xa9446146UL, 0x0fd0030eUL, 0xecc8c73eUL, 0xa4751e41UL,
    0xe238cd99UL, 0x3bea0e2fUL, 0x3280bba1UL, 0x183eb331UL,
    0x4e548b38UL, 0x4f6db908UL, 0x6f420d03UL, 0xf60a04bfUL,
    0x2cb81290UL, 0x24977c79UL, 0x5679b072UL, 0xbcaf89afUL,
    0xde9a771fUL, 0xd9930810UL, 0xb38bae12UL, 0xdccf3f2eUL,
    0x5512721fUL, 0x2e6b7124UL, 0x501adde6UL, 0x9f84cd87UL,
    0x7a584718UL, 0x7408da17UL, 0xbc9f9abcUL, 0xe94b7d8cUL,
    0xec7aec3aUL, 0xdb851dfaUL, 0x63094366UL, 0xc464c3d2UL,
    0xef1c1847UL, 0x3215d908UL, 0xdd433b37UL, 0x24c2ba16UL,
    0x12a14d43UL, 0x2a65c451UL, 0x50940002UL, 0x133ae4ddUL,
    0x71dff89eUL, 0x10314e55UL, 0x81ac77d6UL, 0x5f11199bUL,
    0x043556f1UL, 0xd7a3c76bUL, 0x3c11183bUL, 0x5924a509UL,
    0xf28fe6edUL, 0x97f1fbfaUL, 0x9ebabf2cUL, 0x1e153c6eUL,
    0x86e34570UL, 0xeae96fb1UL, 0x860e5e0aUL, 0x5a3e2ab3UL,
    0x771fe71cUL, 0x4e3d06faUL, 0x2965dcb9UL, 0x99e71d0fUL,
    0x803e89d6UL, 0x5266c825UL, 0x2e4cc978UL, 0x9c10b36aUL,
    0xc6150ebaUL, 0x94e2ea78UL, 0xa5fc3c53UL, 0x1e0a2df4UL,
    0xf2f74ea7UL, 0x361d2b3dUL, 0x1939260fUL, 0x19c27960UL,
    0x5223a708UL, 0xf71312b6UL, 0xebadfe6eUL, 0xeac31f66UL,
    0xe3bc4595UL, 0xa67bc883UL, 0xb17f37d1UL, 0x018cff28UL,
    0xc332ddefUL, 0xbe6c5aa5UL, 0x65582185UL, 0x68ab9802UL,
    0xeecea50fUL, 0xdb2f953bUL, 0x2aef7dadUL, 0x5b6e2f84UL,
    0x1521b628UL, 0x29076170UL, 0xecdd4775UL, 0x619f1510UL,
    0x13cca830UL, 0xeb61bd96UL, 0x0334fe1eUL, 0xaa0363cfUL,
    0xb5735c90UL, 0x4c70a239UL, 0xd59e9e0bUL, 0xcbaade14UL,
    0xeecc86bcUL, 0x60622ca7UL, 0x9cab5cabUL, 0xb2f3846eUL,
    0x648b1eafUL, 0x19bdf0caUL, 0xa02369b9UL, 0x655abb50UL,
    0x40685a32UL, 0x3c2ab4b3UL, 0x319ee9d5UL, 0xc021b8f7UL,
    0x9b540b19UL, 0x875fa099UL, 0x95f7997eUL, 0x623d7da8UL,
    0xf837889aUL, 0x97e32d77UL, 0x11ed935fUL, 0x16681281UL,
    0x0e358829UL, 0xc7e61fd6UL, 0x96dedfa1UL, 0x7858ba99UL,
    0x57f584a5UL, 0x1b227263UL, 0x9b83c3ffUL, 0x1ac24696UL,
    0xcdb30aebUL, 0x532e3054UL, 0x8fd948e4UL, 0x6dbc3128UL,
    0x58ebf2efUL, 0x34c6ffeaUL, 0xfe28ed61UL, 0xee7c3c73UL,
    0x5d4a14d9UL, 0xe864b7e3UL, 0x42105d14UL, 0x203e13e0UL,
    0x45eee2b6UL, 0xa3aaabeaUL, 0xdb6c4f15UL, 0xfacb4fd0UL,
    0xc742f442UL, 0xef6abbb5UL, 0x654f3b1dUL, 0x41cd2105UL,
    0xd81e799eUL, 0x86854dc7UL, 0xe44b476aUL, 0x3d816250UL,
    0xcf62a1f2UL, 0x5b8d2646UL, 0xfc8883a0UL, 0xc1c7b6a3UL,
    0x7f1524c3UL, 0x69cb7492UL, 0x47848a0bUL, 0x5692b285UL,
    0x095bbf00UL, 0xad19489dUL, 0x1462b174UL, 0x23820e00UL,
    0x58428d2aUL, 0x0c55f5eaUL, 0x1dadf43eUL, 0x233f7061UL,
    0x3372f092UL, 0x8d937e41UL, 0xd65fecf1UL, 0x6c223bdbUL,
    0x7cde3759UL, 0xcbee7460UL, 0x4085f2a7UL, 0xce77326eUL,
    0xa6078084UL, 0x19f8509eUL, 0xe8efd855UL, 0x61d99735UL,
    0xa969a7aaUL, 0xc50c06c2UL, 0x5a04abfcUL, 0x800bcadcUL,
    0x9e447a2eUL, 0xc3453484UL, 0xfdd56705UL, 0x0e1e9ec9UL,
    0xdb73dbd3UL, 0x105588cdUL, 0x675fda79UL, 0xe3674340UL,
    0xc5c43465UL, 0x713e38d8UL, 0x3d28f89eUL, 0xf16dff20UL,
    0x153e21e7UL, 0x8fb03d4aUL, 0xe6e39f2bUL, 0xdb83adf7UL
  },
  { /* S[2] */
    0xe93d5a68UL, 0x948140f7UL, 0xf64c261cUL, 0x94692934UL,
    0x411520f7UL, 0x7602d4f7UL, 0xbcf46b2eUL, 0xd4a20068UL,
    0xd4082471UL, 0x3320f46aUL, 0x43b7d4b7UL, 0x500061afUL,
    0x1e39f62eUL, 0x97244546UL, 0x14214f74UL, 0xbf8b8840UL,
    0x4d95fc1dUL, 0x96b591afUL, 0x70f4ddd3UL, 0x66a02f45UL,
    0xbfbc09ecUL, 0x03bd9785UL, 0x7fac6dd0UL, 0x31cb8504UL,
    0x96eb27b3UL, 0x55fd3941UL, 0xda2547e6UL, 0xabca0a9aUL,
    0x28507825UL, 0x530429f4UL, 0x0a2c86daUL, 0xe9b66dfbUL,
    0x68dc1462UL, 0xd7486900UL, 0x680ec0a4UL, 0x27a18deeUL,
    0x4f3ffea2UL, 0xe887ad8cUL, 0xb58ce006UL, 0x7af4d6b6UL,
    0xaace1e7cUL, 0xd3375fecUL, 0xce78a399UL, 0x406b2a42UL,
    0x20fe9e35UL, 0xd9f385b9UL, 0xee39d7abUL, 0x3b124e8bUL,
    0x1dc9faf7UL, 0x4b6d1856UL, 0x26a36631UL, 0xeae397b2UL,
    0x3a6efa74UL, 0xdd5b4332UL, 0x6841e7f7UL, 0xca7820fbUL,
    0xfb0af54eUL, 0xd8feb397UL, 0x454056acUL, 0xba489527UL,
    0x55533a3aUL, 0x20838d87UL, 0xfe6ba9b7UL, 0xd096954bUL,
    0x55a867bcUL, 0xa1159a58UL, 0xcca92963UL, 0x99e1db33UL,
    0xa62a4a56UL, 0x3f3125f9UL, 0x5ef47e1cUL, 0x9029317cUL,
    0xfdf8e802UL, 0x04272f70UL, 0x80bb155cUL, 0x05282ce3UL,
    0x95c11548UL, 0xe4c66d22UL, 0x48c1133fUL, 0xc70f86dcUL,
    0x07f9c9eeUL, 0x41041f0fUL, 0x404779a4UL, 0x5d886e17UL,
    0x325f51ebUL, 0xd59bc0d1UL, 0xf2bcc18fUL, 0x41113564UL,
    0x257b7834UL, 0x602a9c60UL, 0xdff8e8a3UL, 0x1f636c1bUL,
    0x0e12b4c2UL, 0x02e1329eUL, 0xaf664fd1UL, 0xcad18115UL,
    0x6b2395e0UL, 0x333e92e1UL, 0x3b240b62UL, 0xeebeb922UL,
    0x85b2a20eUL, 0xe6ba0d99UL, 0xde720c8cUL, 0x2da2f728UL,
    0xd0127845UL, 0x95b794fdUL, 0x647d0862UL, 0xe7ccf5f0UL,
    0x5449a36fUL, 0x877d48faUL, 0xc39dfd27UL, 0xf33e8d1eUL,
    0x0a476341UL, 0x992eff74UL, 0x3a6f6eabUL, 0xf4f8fd37UL,
    0xa812dc60UL, 0xa1ebddf8UL, 0x991be14cUL, 0xdb6e6b0dUL,
    0xc67b5510UL, 0x6d672c37UL, 0x2765d43bUL, 0xdcd0e804UL,
    0xf1290dc7UL, 0xcc00ffa3UL, 0xb5390f92UL, 0x690fed0bUL,
    0x667b9ffbUL, 0xcedb7d9cUL, 0xa091cf0bUL, 0xd9155ea3UL,
    0xbb132f88UL, 0x515bad24UL, 0x7b9479bfUL, 0x763bd6ebUL,
    0x37392eb3UL, 0xcc115979UL, 0x8026e297UL, 0xf42e312dUL,
    0x6842ada7UL, 0xc66a2b3bUL, 0x12754cccUL, 0x782ef11cUL,
    0x6a124237UL, 0xb79251e7UL, 0x06a1bbe6UL, 0x4bfb6350UL,
    0x1a6b1018UL, 0x11caedfaUL, 0x3d25bdd8UL, 0xe2e1c3c9UL,
    0x44421659UL, 0x0a121386UL, 0xd90cec6eUL, 0xd5abea2aUL,
    0x64af674eUL, 0xda86a85fUL, 0xbebfe988UL, 0x64e4c3feUL,
    0x9dbc8057UL, 0xf0f7c086UL, 0x60787bf8UL, 0x6003604dUL,
    0xd1fd8346UL, 0xf6381fb0UL, 0x7745ae04UL, 0xd736fcccUL,
    0x83426b33UL, 0xf01eab71UL, 0xb0804187UL, 0x3c005e5fUL,
    0x77a057beUL, 0xbde8ae24UL, 0x55464299UL, 0xbf582e61UL,
    0x4e58f48fUL, 0xf2ddfda2UL, 0xf474ef38UL, 0x8789bdc2UL,
    0x5366f9c3UL, 0xc8b38e74UL, 0xb475f255UL, 0x46fcd9b9UL,
    0x7aeb2661UL, 0x8b1ddf84UL, 0x846a0e79UL, 0x915f95e2UL,
    0x466e598eUL, 0x20b45770UL, 0x8cd55591UL, 0xc902de4cUL,
    0xb90bace1UL, 0xbb8205d0UL, 0x11a86248UL, 0x7574a99eUL,
    0xb77f19b6UL, 0xe0a9dc09UL, 0x662d09a1UL, 0xc4324633UL,
    0xe85a1f02UL, 0x09f0be8cUL, 0x4a99a025UL, 0x1d6efe10UL,
    0x1ab93d1dUL, 0x0ba5a4dfUL, 0xa186f20fUL, 0x2868f169UL,
    0xdcb7da83UL, 0x573906feUL, 0xa1e2ce9bUL, 0x4fcd7f52UL,
    0x50115e01UL, 0xa70683faUL, 0xa002b5c4UL, 0x0de6d027UL,
    0x9af88c27UL, 0x773f8641UL, 0xc3604c06UL, 0x61a806b5UL,
    0xf0177a28UL, 0xc0f586e0UL, 0x006058aaUL, 0x30dc7d62UL,
    0x11e69ed7UL, 0x2338ea63UL, 0x53c2dd94UL, 0xc2c21634UL,
    0xbbcbee56UL, 0x90bcb6deUL, 0xebfc7da1UL, 0xce591d76UL,
    0x6f05e409UL, 0x4b7c0188UL, 0x39720a3dUL, 0x7c927c24UL,
    0x86e3725fUL, 0x724d9db9UL, 0x1ac15bb4UL, 0xd39eb8fcUL,
    0xed545578UL, 0x08fca5b5UL, 0xd83d7cd3UL, 0x4dad0fc4UL,
    0x1e50ef5eUL, 0xb161e6f8UL, 0xa28514d9UL, 0x6c51133cUL,
    0x6fd5c7e7UL, 0x56e14ec4UL, 0x362abfceUL, 0xddc6c837UL,
    0xd79a3234UL, 0x92638212UL, 0x670efa8eUL, 0x406000e0UL
  },
  { /* S[3] */
    0x3a39ce37UL, 0xd3faf5cfUL, 0xabc27737UL, 0x5ac52d1bUL,
    0x5cb0679eUL, 0x4fa33742UL, 0xd3822740UL, 0x99bc9bbeUL,
    0xd5118e9dUL, 0xbf0f7315UL, 0xd62d1c7eUL, 0xc700c47bUL,
    0xb78c1b6bUL, 0x21a19045UL, 0xb26eb1beUL, 0x6a366eb4UL,
    0x5748ab2fUL, 0xbc946e79UL, 0xc6a376d2UL, 0x6549c2c8UL,
    0x530ff8eeUL, 0x468dde7dUL, 0xd5730a1dUL, 0x4cd04dc6UL,
    0x2939bbdbUL, 0xa9ba4650UL, 0xac9526e8UL, 0xbe5ee304UL,
    0xa1fad5f0UL, 0x6a2d519aUL, 0x63ef8ce2UL, 0x9a86ee22UL,
    0xc089c2b8UL, 0x43242ef6UL, 0xa51e03aaUL, 0x9cf2d0a4UL,
    0x83c061baUL, 0x9be96a4dUL, 0x8fe51550UL, 0xba645bd6UL,
    0x2826a2f9UL, 0xa73a3ae1UL, 0x4ba99586UL, 0xef5562e9UL,
    0xc72fefd3UL, 0xf752f7daUL, 0x3f046f69UL, 0x77fa0a59UL,
    0x80e4a915UL, 0x87b08601UL, 0x9b09e6adUL, 0x3b3ee593UL,
    0xe990fd5aUL, 0x9e34d797UL, 0x2cf0b7d9UL, 0x022b8b51UL,
    0x96d5ac3aUL, 0x017da67dUL, 0xd1cf3ed6UL, 0x7c7d2d28UL,
    0x1f9f25cfUL, 0xadf2b89bUL, 0x5ad6b472UL, 0x5a88f54cUL,
    0xe029ac71UL, 0xe019a5e6UL, 0x47b0acfdUL, 0xed93fa9bUL,
    0xe8d3c48dUL, 0x283b57ccUL, 0xf8d56629UL, 0x79132e28UL,
    0x785f0191UL, 0xed756055UL, 0xf7960e44UL, 0xe3d35e8cUL,
    0x15056dd4UL, 0x88f46dbaUL, 0x03a16125UL, 0x0564f0bdUL,
    0xc3eb9e15UL, 0x3c9057a2UL, 0x97271aecUL, 0xa93a072aUL,
    0x1b3f6d9bUL, 0x1e6321f5UL, 0xf59c66fbUL, 0x26dcf319UL,
    0x7533d928UL, 0xb155fdf5UL, 0x03563482UL, 0x8aba3cbbUL,
    0x28517711UL, 0xc20ad9f8UL, 0xabcc5167UL, 0xccad925fUL,
    0x4de81751UL, 0x3830dc8eUL, 0x379d5862UL, 0x9320f991UL,
    0xea7a90c2UL, 0xfb3e7bceUL, 0x5121ce64UL, 0x774fbe32UL,
    0xa8b6e37eUL, 0xc3293d46UL, 0x48de5369UL, 0x6413e680UL,
    0xa2ae0810UL, 0xdd6db224UL, 0x69852dfdUL, 0x09072166UL,
    0xb39a460aUL, 0x6445c0ddUL, 0x586cdecfUL, 0x1c20c8aeUL,
    0x5bbef7ddUL, 0x1b588d40UL, 0xccd2017fUL, 0x6bb4e3bbUL,
    0xdda26a7eUL, 0x3a59ff45UL, 0x3e350a44UL, 0xbcb4cdd5UL,
    0x72eacea8UL, 0xfa6484bbUL, 0x8d6612aeUL, 0xbf3c6f47UL,
    0xd29be463UL, 0x542f5d9eUL, 0xaec2771bUL, 0xf64e6370UL,
    0x740e0d8dUL, 0xe75b1357UL, 0xf8721671UL, 0xaf537d5dUL,
    0x4040cb08UL, 0x4eb4e2ccUL, 0x34d2466aUL, 0x0115af84UL,
    0xe1b00428UL, 0x95983a1dUL, 0x06b89fb4UL, 0xce6ea048UL,
    0x6f3f3b82UL, 0x3520ab82UL, 0x011a1d4bUL, 0x277227f8UL,
    0x611560b1UL, 0xe7933fdcUL, 0xbb3a792bUL, 0x344525bdUL,
    0xa08839e1UL, 0x51ce794bUL, 0x2f32c9b7UL, 0xa01fbac9UL,
    0xe01cc87eUL, 0xbcc7d1f6UL, 0xcf0111c3UL, 0xa1e8aac7UL,
    0x1a908749UL, 0xd44fbd9aUL, 0xd0dadecbUL, 0xd50ada38UL,
    0x0339c32aUL, 0xc6913667UL, 0x8df9317cUL, 0xe0b12b4fUL,
    0xf79e59b7UL, 0x43f5bb3aUL, 0xf2d519ffUL, 0x27d9459cUL,
    0xbf97222cUL, 0x15e6fc2aUL, 0x0f91fc71UL, 0x9b941525UL,
    0xfae59361UL, 0xceb69cebUL, 0xc2a86459UL, 0x12baa8d1UL,
    0xb6c1075eUL, 0xe3056a0cUL, 0x10d25065UL, 0xcb03a442UL,
    0xe0ec6e0eUL, 0x1698db3bUL, 0x4c98a0beUL, 0x3278e964UL,
    0x9f1f9532UL, 0xe0d392dfUL, 0xd3a0342bUL, 0x8971f21eUL,
    0x1b0a7441UL, 0x4ba3348cUL, 0xc5be7120UL, 0xc37632d8UL,
    0xdf359f8dUL, 0x9b992f2eUL, 0xe60b6f47UL, 0x0fe3f11dUL,
    0xe54cda54UL, 0x1edad891UL, 0xce6279cfUL, 0xcd3e7e6fUL,
    0x1618b166UL, 0xfd2c1d05UL, 0x848fd2c5UL, 0xf6fb2299UL,
    0xf523f357UL, 0xa6327623UL, 0x93a83531UL, 0x56cccd02UL,
    0xacf08162UL, 0x5a75ebb5UL, 0x6e163697UL, 0x88d273ccUL,
    0xde966292UL, 0x81b949d0UL, 0x4c50901bUL, 0x71c65614UL,
    0xe6c6c7bdUL, 0x327a140aUL, 0x45e1d006UL, 0xc3f27b9aUL,
    0xc9aa53fdUL, 0x62a80f00UL, 0xbb25bfe2UL, 0x35bdd2f6UL,
    0x71126905UL, 0xb2040222UL, 0xb6cbcf7cUL, 0xcd769c2bUL,
    0x53113ec0UL, 0x1640e3d3UL, 0x38abbd60UL, 0x2547adf0UL,
    0xba38209cUL, 0xf746ce76UL, 0x77afa1c5UL, 0x20756060UL,
    0x85cbfe4eUL, 0x8ae88dd8UL, 0x7aaaf9b0UL, 0x4cf9aa7eUL,
    0x1948c25cUL, 0x02fb8a8cUL, 0x01c36ae4UL, 0xd6ebe1f9UL,
    0x90d4f869UL, 0xa65cdea0UL, 0x3f09252dUL, 0xc208e69fUL,
    0xb74e6132UL, 0xce77e25bUL, 0x578fdfe3UL, 0x3ac372e6UL
  }
};

/* Blowfish F function */
#define BF_F(ctx, x) \
    ( ((ctx)->S[0][((x) >> 24) & 0xff] + (ctx)->S[1][((x) >> 16) & 0xff]) \
      ^ (ctx)->S[2][((x) >>  8) & 0xff] ) \
    + (ctx)->S[3][(x) & 0xff]

static void bf_encrypt_block(const BlowfishCtx *ctx, uint32_t *xl, uint32_t *xr)
{
    uint32_t l = *xl, r = *xr;
    int i;
    for (i = 0; i < BF_ROUNDS; i += 2) {
        l ^= ctx->P[i];
        r ^= BF_F(ctx, l);
        r ^= ctx->P[i + 1];
        l ^= BF_F(ctx, r);
    }
    l ^= ctx->P[16];
    r ^= ctx->P[17];
    *xl = r;
    *xr = l;
}

static void bf_init(BlowfishCtx *ctx)
{
    int i, j;
    memcpy(ctx->P, BF_P_INIT, sizeof(BF_P_INIT));
    for (i = 0; i < 4; i++)
        memcpy(ctx->S[i], BF_S_INIT[i], sizeof(BF_S_INIT[i]));
    (void)i; (void)j;
}

/* XOR key bytes into P-array (wraps around key) */
static void bf_xor_key(BlowfishCtx *ctx, const uint8_t *key, size_t key_len)
{
    int i;
    size_t j = 0;
    for (i = 0; i < 18; i++) {
        uint32_t word = 0;
        int k;
        for (k = 0; k < 4; k++) {
            word = (word << 8) | (key_len > 0 ? key[j % key_len] : 0);
            j++;
        }
        ctx->P[i] ^= word;
    }
}

static void bf_expand_key(BlowfishCtx *ctx, const uint8_t *key, size_t key_len)
{
    int i;
    uint32_t l = 0, r = 0;

    bf_xor_key(ctx, key, key_len);

    for (i = 0; i < 18; i += 2) {
        bf_encrypt_block(ctx, &l, &r);
        ctx->P[i]     = l;
        ctx->P[i + 1] = r;
    }
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 256; j += 2) {
            bf_encrypt_block(ctx, &l, &r);
            ctx->S[i][j]     = l;
            ctx->S[i][j + 1] = r;
        }
    }
}

/* EksBlowfishSetup: the key schedule used by bcrypt */
static void eks_blowfish_setup(BlowfishCtx *ctx,
                                int cost,
                                const uint8_t salt[16],
                                const uint8_t *key, size_t key_len)
{
    uint32_t rounds;
    uint32_t i;
    uint32_t l, r;

    bf_init(ctx);

    /* XOR password into P-array, then expand with password */
    bf_expand_key(ctx, key, key_len);

    /* Now do 2^cost iterations of ExpandKey with salt and key alternating */
    rounds = (uint32_t)1 << cost;
    for (i = 0; i < rounds; i++) {
        /* ExpandKey(ctx, salt) */
        {
            int p;
            uint32_t ll = 0, rr = 0;
            /* XOR salt into P (salt is 16 bytes, wraps around 18 entries) */
            int q;
            for (q = 0; q < 18; q++) {
                uint32_t word = 0;
                int k;
                for (k = 0; k < 4; k++) {
                    word = (word << 8) | salt[((q * 4) + k) % 16];
                }
                ctx->P[q] ^= word;
            }
            for (p = 0; p < 18; p += 2) {
                bf_encrypt_block(ctx, &ll, &rr);
                ctx->P[p]     = ll;
                ctx->P[p + 1] = rr;
            }
            for (p = 0; p < 4; p++) {
                int j;
                for (j = 0; j < 256; j += 2) {
                    bf_encrypt_block(ctx, &ll, &rr);
                    ctx->S[p][j]     = ll;
                    ctx->S[p][j + 1] = rr;
                }
            }
        }
        /* ExpandKey(ctx, key) */
        {
            int p;
            uint32_t ll = 0, rr = 0;
            bf_xor_key(ctx, key, key_len);
            for (p = 0; p < 18; p += 2) {
                bf_encrypt_block(ctx, &ll, &rr);
                ctx->P[p]     = ll;
                ctx->P[p + 1] = rr;
            }
            for (p = 0; p < 4; p++) {
                int j;
                for (j = 0; j < 256; j += 2) {
                    bf_encrypt_block(ctx, &ll, &rr);
                    ctx->S[p][j]     = ll;
                    ctx->S[p][j + 1] = rr;
                }
            }
        }
    }
    /* suppress unused-variable warnings from loop vars declared outside loops */
    (void)l; (void)r;
}

/* -----------------------------------------------------------------------
 * bcrypt base64 — Story 29.6.1
 *
 * Alphabet: ./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789
 * This is different from standard base64.
 * ----------------------------------------------------------------------- */

static const char BF_B64_CHARS[] =
    "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

/* Encode `n_bytes` bytes from `src` into `dst` using bcrypt base64.
 * `dst` must have room for ceil(n_bytes * 4 / 3) + 1 chars. */
static void bf_b64_encode(const uint8_t *src, size_t n_bytes, char *dst)
{
    size_t i = 0;
    size_t j = 0;
    while (i < n_bytes) {
        uint32_t c1 = src[i++];
        dst[j++] = BF_B64_CHARS[c1 >> 2];
        c1 = (c1 & 0x03) << 4;
        if (i >= n_bytes) {
            dst[j++] = BF_B64_CHARS[c1];
            break;
        }
        uint32_t c2 = src[i++];
        c1 |= (c2 >> 4) & 0x0f;
        dst[j++] = BF_B64_CHARS[c1];
        c1 = (c2 & 0x0f) << 2;
        if (i >= n_bytes) {
            dst[j++] = BF_B64_CHARS[c1];
            break;
        }
        uint32_t c3 = src[i++];
        c1 |= (c3 >> 6) & 0x03;
        dst[j++] = BF_B64_CHARS[c1];
        dst[j++] = BF_B64_CHARS[c3 & 0x3f];
    }
    dst[j] = '\0';
}

/* Decode a single bcrypt base64 character to its 6-bit value, or -1. */
static int bf_b64_decode_char(char c)
{
    int i;
    for (i = 0; i < 64; i++) {
        if (BF_B64_CHARS[i] == c) return i;
    }
    return -1;
}

/* Decode bcrypt base64 string `src` (of `src_len` chars) into `dst`.
 * Returns number of bytes written, or -1 on error. */
static int bf_b64_decode(const char *src, size_t src_len, uint8_t *dst, size_t dst_max)
{
    size_t i = 0;
    size_t j = 0;
    while (i < src_len && j < dst_max) {
        int c1, c2, c3, c4;
        c1 = bf_b64_decode_char(src[i++]);
        if (c1 < 0 || i >= src_len) break;
        c2 = bf_b64_decode_char(src[i++]);
        if (c2 < 0) break;
        if (j < dst_max) dst[j++] = (uint8_t)((c1 << 2) | (c2 >> 4));
        if (i >= src_len || j >= dst_max) break;
        c3 = bf_b64_decode_char(src[i++]);
        if (c3 < 0) break;
        if (j < dst_max) dst[j++] = (uint8_t)(((c2 & 0x0f) << 4) | (c3 >> 2));
        if (i >= src_len || j >= dst_max) break;
        c4 = bf_b64_decode_char(src[i++]);
        if (c4 < 0) break;
        if (j < dst_max) dst[j++] = (uint8_t)(((c3 & 0x03) << 6) | c4);
    }
    return (int)j;
}

/* -----------------------------------------------------------------------
 * crypto_bcrypt_hash / crypto_bcrypt_verify — Story 29.6.1
 * ----------------------------------------------------------------------- */

/* The bcrypt ciphertext plaintext (OpenBSD magic string, 24 bytes / 3 longs) */
static const uint8_t BF_CTEXT[24] = {
    'O','r','p','h','e','a','n','B','e','h','o','l','d','e','r',
    'S','c','r','y','D','o','u','b','t'
};

CryptoStrResult crypto_bcrypt_hash(const char *password, int cost)
{
    CryptoStrResult result;
    uint8_t salt[16];
    uint8_t ctext[24];
    BlowfishCtx ctx;
    size_t pw_len;
    int i;
    char salt_b64[32];  /* 22 chars + NUL */
    char hash_b64[48];  /* 31 chars + NUL */
    char *out;

    result.ok     = NULL;
    result.is_err = 0;
    result.err_msg = NULL;

    /* Clamp cost */
    if (cost < 4)  cost = 4;
    if (cost > 31) cost = 31;

    /* Generate random salt */
    arc4random_buf(salt, 16);

    /* Encode salt as 22 bcrypt base64 chars */
    bf_b64_encode(salt, 16, salt_b64);
    salt_b64[22] = '\0';   /* truncate to 22 chars */

    /* Password is passed including NUL terminator (max 72 bytes per spec) */
    pw_len = strlen(password) + 1; /* include NUL */
    if (pw_len > 72) pw_len = 72;

    /* EksBlowfishSetup */
    eks_blowfish_setup(&ctx, cost, salt,
                       (const uint8_t *)password, pw_len);

    /* Encrypt ciphertext 64 times */
    memcpy(ctext, BF_CTEXT, 24);
    for (i = 0; i < 64; i++) {
        /* encrypt each 64-bit block (3 blocks of 8 bytes = 24 bytes) */
        int b;
        for (b = 0; b < 3; b++) {
            uint32_t l, r;
            l = ((uint32_t)ctext[b * 8 + 0] << 24)
              | ((uint32_t)ctext[b * 8 + 1] << 16)
              | ((uint32_t)ctext[b * 8 + 2] <<  8)
              |  (uint32_t)ctext[b * 8 + 3];
            r = ((uint32_t)ctext[b * 8 + 4] << 24)
              | ((uint32_t)ctext[b * 8 + 5] << 16)
              | ((uint32_t)ctext[b * 8 + 6] <<  8)
              |  (uint32_t)ctext[b * 8 + 7];
            bf_encrypt_block(&ctx, &l, &r);
            ctext[b * 8 + 0] = (uint8_t)(l >> 24);
            ctext[b * 8 + 1] = (uint8_t)(l >> 16);
            ctext[b * 8 + 2] = (uint8_t)(l >>  8);
            ctext[b * 8 + 3] = (uint8_t)(l);
            ctext[b * 8 + 4] = (uint8_t)(r >> 24);
            ctext[b * 8 + 5] = (uint8_t)(r >> 16);
            ctext[b * 8 + 6] = (uint8_t)(r >>  8);
            ctext[b * 8 + 7] = (uint8_t)(r);
        }
    }

    /* Encode 23 bytes (drop last byte, spec says 31 b64 chars from 23 bytes) */
    bf_b64_encode(ctext, 23, hash_b64);
    hash_b64[31] = '\0';

    /* Format: "$2b$NN$<salt22><hash31>" = 7 + 22 + 31 = 60 chars */
    out = (char *)malloc(61);
    if (!out) {
        result.is_err  = 1;
        result.err_msg = "crypto_bcrypt_hash: out of memory";
        return result;
    }

    /* Write prefix "$2b$NN$" */
    out[0] = '$'; out[1] = '2'; out[2] = 'b'; out[3] = '$';
    out[4] = (char)('0' + cost / 10);
    out[5] = (char)('0' + cost % 10);
    out[6] = '$';
    memcpy(out + 7,      salt_b64, 22);
    memcpy(out + 7 + 22, hash_b64, 31);
    out[60] = '\0';

    result.ok = out;
    return result;
}

int crypto_bcrypt_verify(const char *password, const char *hash)
{
    int cost;
    uint8_t salt[16];
    uint8_t ctext[24];
    BlowfishCtx ctx;
    size_t pw_len;
    int i;
    char hash_b64[48];
    char computed_hash[61];
    volatile uint8_t diff;

    if (!password || !hash) return 0;

    /* Validate format: "$2b$NN$<22><31>" — total 60 chars */
    if (hash[0] != '$' || hash[1] != '2' ||
        (hash[2] != 'b' && hash[2] != 'a' && hash[2] != 'y') ||
        hash[3] != '$') return 0;
    if (strlen(hash) != 60) return 0;

    /* Parse cost */
    if (hash[4] < '0' || hash[4] > '3') return 0;
    if (hash[5] < '0' || hash[5] > '9') return 0;
    cost = (hash[4] - '0') * 10 + (hash[5] - '0');
    if (cost < 4 || cost > 31) return 0;
    if (hash[6] != '$') return 0;

    /* Decode 22-char salt from position 7 */
    if (bf_b64_decode(hash + 7, 22, salt, 16) != 16) return 0;

    /* Re-hash */
    pw_len = strlen(password) + 1;
    if (pw_len > 72) pw_len = 72;

    eks_blowfish_setup(&ctx, cost, salt,
                       (const uint8_t *)password, pw_len);

    memcpy(ctext, BF_CTEXT, 24);
    for (i = 0; i < 64; i++) {
        int b;
        for (b = 0; b < 3; b++) {
            uint32_t l, r;
            l = ((uint32_t)ctext[b * 8 + 0] << 24)
              | ((uint32_t)ctext[b * 8 + 1] << 16)
              | ((uint32_t)ctext[b * 8 + 2] <<  8)
              |  (uint32_t)ctext[b * 8 + 3];
            r = ((uint32_t)ctext[b * 8 + 4] << 24)
              | ((uint32_t)ctext[b * 8 + 5] << 16)
              | ((uint32_t)ctext[b * 8 + 6] <<  8)
              |  (uint32_t)ctext[b * 8 + 7];
            bf_encrypt_block(&ctx, &l, &r);
            ctext[b * 8 + 0] = (uint8_t)(l >> 24);
            ctext[b * 8 + 1] = (uint8_t)(l >> 16);
            ctext[b * 8 + 2] = (uint8_t)(l >>  8);
            ctext[b * 8 + 3] = (uint8_t)(l);
            ctext[b * 8 + 4] = (uint8_t)(r >> 24);
            ctext[b * 8 + 5] = (uint8_t)(r >> 16);
            ctext[b * 8 + 6] = (uint8_t)(r >>  8);
            ctext[b * 8 + 7] = (uint8_t)(r);
        }
    }

    bf_b64_encode(ctext, 23, hash_b64);
    hash_b64[31] = '\0';

    /* Reconstruct full hash string for comparison */
    computed_hash[0] = '$'; computed_hash[1] = '2'; computed_hash[2] = 'b';
    computed_hash[3] = '$';
    computed_hash[4] = (char)('0' + cost / 10);
    computed_hash[5] = (char)('0' + cost % 10);
    computed_hash[6] = '$';
    /* Re-encode the salt we decoded (round-trip to normalise) */
    {
        char salt_b64[32];
        bf_b64_encode(salt, 16, salt_b64);
        salt_b64[22] = '\0';
        memcpy(computed_hash + 7, salt_b64, 22);
    }
    memcpy(computed_hash + 29, hash_b64, 31);
    computed_hash[60] = '\0';

    /* Constant-time compare of the 53 chars after the prefix */
    diff = 0;
    for (i = 0; i < 60; i++) {
        diff |= (uint8_t)((unsigned char)computed_hash[i]
                        ^ (unsigned char)hash[i]);
    }
    return diff == 0 ? 1 : 0;
}

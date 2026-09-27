/*
 * sha256.c — the compiler's one SHA-256 (FIPS 180-4).  See sha256.h.
 *
 * Moved verbatim from companion.c (122.13 item 9); only the names changed
 * (tk_ prefix, the context type TkSha256) and tk_sha256_hex was added.
 * Known-answer tests: test/test_pkg.c (the FIPS 180-2 "abc", empty-string
 * and one-million-'a' vectors), run by `make test-pkg`.
 */
#include "sha256.h"

static const unsigned int K256[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
    0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
    0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
    0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
    0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
    0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

#define RR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^((~(x))&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (RR(x,2)^RR(x,13)^RR(x,22))
#define EP1(x) (RR(x,6)^RR(x,11)^RR(x,25))
#define SIG0(x) (RR(x,7)^RR(x,18)^((x)>>3))
#define SIG1(x) (RR(x,17)^RR(x,19)^((x)>>10))

static void sha256_transform(TkSha256 *ctx)
{
    unsigned int w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned int)ctx->buf[i*4]<<24) |
               ((unsigned int)ctx->buf[i*4+1]<<16) |
               ((unsigned int)ctx->buf[i*4+2]<<8) |
               ((unsigned int)ctx->buf[i*4+3]);
    for (int i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (int i = 0; i < 64; i++) {
        unsigned int t1 = h + EP1(e) + CH(e,f,g) + K256[i] + w[i];
        unsigned int t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

void tk_sha256_init(TkSha256 *ctx)
{
    ctx->state[0]=0x6a09e667u; ctx->state[1]=0xbb67ae85u;
    ctx->state[2]=0x3c6ef372u; ctx->state[3]=0xa54ff53au;
    ctx->state[4]=0x510e527fu; ctx->state[5]=0x9b05688cu;
    ctx->state[6]=0x1f83d9abu; ctx->state[7]=0x5be0cd19u;
    ctx->total = 0;
}

void tk_sha256_update(TkSha256 *ctx, const unsigned char *data, unsigned long len)
{
    unsigned long i = 0;
    unsigned int idx = (unsigned int)(ctx->total & 63);
    ctx->total += len;
    for (; i < len; i++) {
        ctx->buf[idx++] = data[i];
        if (idx == 64) { sha256_transform(ctx); idx = 0; }
    }
}

void tk_sha256_final(TkSha256 *ctx, unsigned char hash[32])
{
    unsigned int idx = (unsigned int)(ctx->total & 63);
    ctx->buf[idx++] = 0x80;
    if (idx > 56) {
        while (idx < 64) ctx->buf[idx++] = 0;
        sha256_transform(ctx);
        idx = 0;
    }
    while (idx < 56) ctx->buf[idx++] = 0;
    unsigned long long bits = ctx->total * 8;
    for (int i = 7; i >= 0; i--)
        ctx->buf[56 + (7-i)] = (unsigned char)(bits >> (i*8));
    sha256_transform(ctx);
    for (int i = 0; i < 8; i++) {
        hash[i*4]   = (unsigned char)(ctx->state[i]>>24);
        hash[i*4+1] = (unsigned char)(ctx->state[i]>>16);
        hash[i*4+2] = (unsigned char)(ctx->state[i]>>8);
        hash[i*4+3] = (unsigned char)(ctx->state[i]);
    }
}

void tk_sha256_hex(const void *data, size_t len, char out[65])
{
    static const char hx[] = "0123456789abcdef";
    TkSha256 ctx;
    unsigned char d[32];
    tk_sha256_init(&ctx);
    tk_sha256_update(&ctx, (const unsigned char *)data, (unsigned long)len);
    tk_sha256_final(&ctx, d);
    for (int i = 0; i < 32; i++) {
        out[i * 2]     = hx[d[i] >> 4];
        out[i * 2 + 1] = hx[d[i] & 15];
    }
    out[64] = '\0';
}

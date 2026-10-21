#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
static const u32 K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,
    0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
    0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,
    0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,
    0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
    0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,
    0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,
    0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
    0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static inline u32 rotr32(u32 x, u32 n) { return (x >> n) | (x << (32 - n)); }

struct sha256_ctx {
    u32 state[8];
    u8  buf[64];
    u64 bitlen;
    u32 buflen;
};

static void sha256_init(struct sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

static void sha256_transform(u32 *s, const u8 *data)
{
    u32 a,b,c,d,e,f,g,h,t1,t2,m[64];
    for (u32 i = 0; i < 16; i++) {
        m[i] = ((u32)data[i*4]<<24)|((u32)data[i*4+1]<<16)|
               ((u32)data[i*4+2]<<8)|((u32)data[i*4+3]);
    }
    for (u32 i = 16; i < 64; i++) {
        u32 s0 = rotr32(m[i-15],7)^rotr32(m[i-15],18)^(m[i-15]>>3);
        u32 s1 = rotr32(m[i-2],17)^rotr32(m[i-2],19)^(m[i-2]>>10);
        m[i] = m[i-16]+s0+m[i-7]+s1;
    }
    a=s[0]; b=s[1]; c=s[2]; d=s[3];
    e=s[4]; f=s[5]; g=s[6]; h=s[7];
    for (u32 i = 0; i < 64; i++) {
        u32 S1  = rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
        u32 ch  = (e&f)^((~e)&g);
        t1 = h+S1+ch+K256[i]+m[i];
        u32 S0  = rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
        u32 maj = (a&b)^(a&c)^(b&c);
        t2 = S0+maj;
        h=g; g=f; f=e; e=d+t1;
        d=c; c=b; b=a; a=t1+t2;
    }
    s[0]+=a; s[1]+=b; s[2]+=c; s[3]+=d;
    s[4]+=e; s[5]+=f; s[6]+=g; s[7]+=h;
}

static void sha256_update(struct sha256_ctx *ctx, const u8 *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx->state, ctx->buf);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(struct sha256_ctx *ctx, u8 out[32])
{
    u32 i = ctx->buflen;
    ctx->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->buf[i++] = 0;
        sha256_transform(ctx->state, ctx->buf);
        i = 0;
    }
    while (i < 56) ctx->buf[i++] = 0;
    ctx->bitlen += ctx->buflen * 8;
    u64 bl = ctx->bitlen;
    for (int j = 7; j >= 0; j--) { ctx->buf[56+j] = (u8)(bl & 0xFF); bl >>= 8; }
    sha256_transform(ctx->state, ctx->buf);
    for (int j = 0; j < 8; j++) {
        out[j*4  ] = (u8)(ctx->state[j] >> 24);
        out[j*4+1] = (u8)(ctx->state[j] >> 16);
        out[j*4+2] = (u8)(ctx->state[j] >>  8);
        out[j*4+3] = (u8)(ctx->state[j]      );
    }
}

static void compute_hash(const void *data, size_t len, u8 out[32])
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const u8 *)data, len);
    sha256_final(&ctx, out);
}


int main(int c,char**v){ int n=atoi(v[1]); unsigned char*b=malloc(n+1); u32 x=12345; for(int i=0;i<n;i++){ x=x*1664525u+1013904223u; b[i]=x>>24; }
 u8 o[32]; compute_hash(b,n,o); for(int i=0;i<32;i++) printf("%02x",o[i]); printf("\n"); return 0; }

/*
 * playos-init/src/sha256.c — Self-contained SHA-256 + HMAC-SHA256
 *
 * Public-domain-style implementation. No heap allocation, no external
 * libraries, safe to run in PID 1.
 */
#include "playos-init/sha256.h"

#include <string.h>

#define ROTR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define SHR32(x, n)  ((x) >> (n))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_transform(sha256_ctx *ctx, const unsigned char data[64])
{
    uint32_t m[64];
    int      i;

    for (i = 0; i < 16; i++) {
        m[i] = ((uint32_t)data[i * 4]     << 24)
             | ((uint32_t)data[i * 4 + 1] << 16)
             | ((uint32_t)data[i * 4 + 2] << 8)
             | ((uint32_t)data[i * 4 + 3]);
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = ROTR32(m[i - 15], 7) ^ ROTR32(m[i - 15], 18)
                    ^ SHR32(m[i - 15], 3);
        uint32_t s1 = ROTR32(m[i - 2], 17) ^ ROTR32(m[i - 2], 19)
                    ^ SHR32(m[i - 2], 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    uint32_t a = ctx->state[0];
    uint32_t b = ctx->state[1];
    uint32_t c = ctx->state[2];
    uint32_t d = ctx->state[3];
    uint32_t e = ctx->state[4];
    uint32_t f = ctx->state[5];
    uint32_t g = ctx->state[6];
    uint32_t h = ctx->state[7];

    for (i = 0; i < 64; i++) {
        uint32_t S1    = ROTR32(e, 6) ^ ROTR32(e, 11) ^ ROTR32(e, 25);
        uint32_t ch    = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + S1 + ch + sha256_k[i] + m[i];
        uint32_t S0    = ROTR32(a, 2) ^ ROTR32(a, 13) ^ ROTR32(a, 22);
        uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->bitlen    = 0;
    ctx->buflen    = 0;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void sha256_update(sha256_ctx *ctx, const unsigned char *data, size_t len)
{
    ctx->bitlen += (uint64_t)len * 8;

    while (len > 0) {
        size_t space = 64 - ctx->buflen;
        size_t take  = len < space ? len : space;

        memcpy(ctx->buffer + ctx->buflen, data, take);
        ctx->buflen += take;
        data        += take;
        len         -= take;

        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buffer);
            ctx->buflen = 0;
        }
    }
}

void sha256_final(sha256_ctx *ctx, unsigned char out[32])
{
    uint64_t      bits = ctx->bitlen;
    unsigned char pad  = 0x80;
    unsigned char zero = 0x00;
    unsigned char len_be[8];
    int           i;

    sha256_update(ctx, &pad, 1);
    while (ctx->buflen != 56)
        sha256_update(ctx, &zero, 1);

    for (i = 0; i < 8; i++)
        len_be[i] = (unsigned char)(bits >> (56 - i * 8));
    sha256_update(ctx, len_be, 8);

    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(ctx->state[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(ctx->state[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(ctx->state[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(ctx->state[i]);
    }
}

void sha256_hex(const unsigned char *data, size_t len, char out[65])
{
    static const char hex[] = "0123456789abcdef";
    size_t           i;

    for (i = 0; i < len; i++) {
        out[i * 2]     = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

void sha256_hmac_init(sha256_hmac_ctx *ctx, const unsigned char *key,
                      size_t key_len)
{
    unsigned char block[64];
    unsigned char ipad[64];
    int           i;

    memset(block, 0, sizeof(block));
    if (key_len > 64) {
        sha256_ctx kctx;
        sha256_init(&kctx);
        sha256_update(&kctx, key, key_len);
        sha256_final(&kctx, block); /* writes 32 bytes into block[0..31] */
    } else {
        memcpy(block, key, key_len);
    }

    for (i = 0; i < 64; i++) {
        ipad[i]       = block[i] ^ 0x36;
        ctx->opad[i]  = block[i] ^ 0x5c;
    }

    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, ipad, 64);
}

void sha256_hmac_update(sha256_hmac_ctx *ctx, const unsigned char *data,
                        size_t len)
{
    sha256_update(&ctx->inner, data, len);
}

void sha256_hmac_final(sha256_hmac_ctx *ctx, unsigned char out[32])
{
    unsigned char inner_digest[32];
    sha256_ctx    outer;

    sha256_final(&ctx->inner, inner_digest);

    sha256_init(&outer);
    sha256_update(&outer, ctx->opad, 64);
    sha256_update(&outer, inner_digest, 32);
    sha256_final(&outer, out);
}

void sha256_hmac(const unsigned char *key, size_t key_len,
                 const unsigned char *data, size_t data_len,
                 unsigned char out[32])
{
    sha256_hmac_ctx ctx;

    sha256_hmac_init(&ctx, key, key_len);
    sha256_hmac_update(&ctx, data, data_len);
    sha256_hmac_final(&ctx, out);
}

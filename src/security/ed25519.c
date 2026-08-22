/*
 * playos-init/src/security/ed25519.c — Ed25519 (S12-T8)
 *
 * Self-contained Ed25519 sign + verify for PlayOS. Field arithmetic
 * follows the public-domain TweetNaCl/ref10 design (radix-2^16 limbs,
 * curve25519-donna style), with SHA-512 from sha512.c.
 *
 * Exposed API (see security.h):
 *   int  playos_ed25519_verify(sig64, msg, msglen, pk32);
 * plus test-only helpers:
 *   void playos_ed25519_keypair(pk32, sk64);
 *   void playos_ed25519_sign(sig64, msg, msglen, sk64);
 */
#include "playos-init/security.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int64_t i64;
typedef i64 gf[16];

void playos_sha512(unsigned char *out, const unsigned char *in, size_t inlen);

static const gf gf0 = {0};
static const gf gf1 = {1};
static const gf D = {0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141,
                     0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7,
                     0xfe73, 0x2b6f, 0x6cee, 0x5203};
static const gf D2 = {0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283,
                      0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e,
                      0xfce7, 0x56df, 0xd9dc, 0x2406};
static const gf X = {0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525,
                     0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4,
                     0x53fe, 0xcd6e, 0x36d3, 0x2169};
static const gf Y = {0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                     0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
                     0x6666, 0x6666, 0x6666, 0x6666};
static const gf I = {0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f,
                     0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d,
                     0xdf0b, 0x4fc1, 0x2480, 0x2b83};

/* Group order L, little-endian. */
static const unsigned char L[32] = {
    0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
    0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10
};

/* ── Field helpers ─────────────────────────────────────────────────── */

static void
set25519(gf r, const gf a)
{
    int i;
    for (i = 0; i < 16; i++)
        r[i] = a[i];
}

static void
car25519(gf o)
{
    int i;
    i64 c;
    for (i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void
sel25519(gf p, gf q, int b)
{
    i64 t, i, c = ~(b - 1);
    for (i = 0; i < 16; i++) {
        t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void
pack25519(unsigned char *o, const gf n)
{
    int i, j, b;
    gf  m, t;

    for (i = 0; i < 16; i++)
        t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);

    for (j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        b = (m[15] >> 16) & 1;
        m[14] &= 0xffff;
        sel25519(t, m, 1 - b);
    }

    for (i = 0; i < 16; i++) {
        o[2 * i] = (unsigned char)(t[i] & 0xff);
        o[2 * i + 1] = (unsigned char)(t[i] >> 8);
    }
}

static void
unpack25519(gf o, const unsigned char *n)
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = n[2 * i] + ((i64)n[2 * i + 1] << 8);
    o[15] &= 0x7fff;
}

static int
par25519(const gf a)
{
    unsigned char d[32];
    pack25519(d, a);
    return d[0] & 1;
}

static int
neq25519(const gf a, const gf b)
{
    unsigned char c[32], d[32];
    int           diff = 0;
    int           i;

    pack25519(c, a);
    pack25519(d, b);
    for (i = 0; i < 32; i++)
        diff |= c[i] ^ d[i];
    return (1 & ((diff - 1) >> 8)) - 1; /* 0 if equal, -1 otherwise */
}

static void
A(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = a[i] + b[i];
}

static void
Z(gf o, const gf a, const gf b)
{
    int i;
    for (i = 0; i < 16; i++)
        o[i] = a[i] - b[i];
}

static void
M(gf o, const gf a, const gf b)
{
    i64 i, j, t[31];

    for (i = 0; i < 31; i++)
        t[i] = 0;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++)
            t[i + j] += a[i] * b[j];
    for (i = 0; i < 15; i++)
        t[i] += 38 * t[i + 16];
    for (i = 0; i < 16; i++)
        o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void
S(gf o, const gf a)
{
    M(o, a, a);
}

static void
inv25519(gf o, const gf i)
{
    gf  c;
    int a;

    for (a = 0; a < 16; a++)
        c[a] = i[a];
    for (a = 253; a >= 0; a--) {
        S(c, c);
        if (a != 2 && a != 4)
            M(c, c, i);
    }
    for (a = 0; a < 16; a++)
        o[a] = c[a];
}

static void
pow2523(gf o, const gf i)
{
    gf  c;
    int a;

    for (a = 0; a < 16; a++)
        c[a] = i[a];
    for (a = 250; a >= 0; a--) {
        S(c, c);
        if (a != 1)
            M(c, c, i);
    }
    for (a = 0; a < 16; a++)
        o[a] = c[a];
}

/* Unpack a 32-byte point, negated (y sign folded), into r[0..3].
 * Returns 0 on success, -1 if the encoding is not on the curve. */
static int
unpackneg(gf r[4], const unsigned char p[32])
{
    gf t, chk, num, den, den2, den4, den6;

    set25519(r[2], gf1);
    unpack25519(r[1], p);
    S(num, r[1]);
    M(den, num, D);
    Z(num, num, r[2]);
    A(den, r[2], den);

    S(den2, den);
    S(den4, den2);
    M(den6, den4, den2);
    M(t, den6, num);
    M(t, t, den);

    pow2523(t, t);
    M(t, t, num);
    M(t, t, den);
    M(t, t, den);
    M(r[0], t, den);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num))
        M(r[0], r[0], I);

    S(chk, r[0]);
    M(chk, chk, den);
    if (neq25519(chk, num))
        return -1;

    if (par25519(r[0]) == (p[31] >> 7))
        Z(r[0], gf0, r[0]);

    M(r[3], r[0], r[1]);
    return 0;
}

/* ── Point arithmetic ──────────────────────────────────────────────── */

static void
add(gf p[4], gf q[4])
{
    gf a, b, c, d, t, e, f, g, h;

    Z(a, p[1], p[0]);
    Z(t, q[1], q[0]);
    M(a, a, t);
    A(b, p[0], p[1]);
    A(t, q[0], q[1]);
    M(b, b, t);
    M(c, p[3], q[3]);
    M(c, c, D2);
    M(d, p[2], q[2]);
    A(d, d, d);
    Z(e, b, a);
    Z(f, d, c);
    A(g, d, c);
    A(h, b, a);

    M(p[0], e, f);
    M(p[1], h, g);
    M(p[2], g, f);
    M(p[3], e, h);
}

static void
cswap(gf p[4], gf q[4], unsigned char b)
{
    int i;
    for (i = 0; i < 4; i++)
        sel25519(p[i], q[i], b);
}

static void
pack(unsigned char *r, gf p[4])
{
    gf tx, ty, zi;

    inv25519(zi, p[2]);
    M(tx, p[0], zi);
    M(ty, p[1], zi);
    pack25519(r, ty);
    r[31] ^= (unsigned char)(par25519(tx) << 7);
}

static void
scalarmult(gf p[4], gf q[4], const unsigned char *s)
{
    int i;

    set25519(p[0], gf0);
    set25519(p[1], gf1);
    set25519(p[2], gf1);
    set25519(p[3], gf0);

    for (i = 255; i >= 0; --i) {
        unsigned char b = (unsigned char)((s[i / 8] >> (i & 7)) & 1);
        cswap(p, q, b);
        add(q, p);
        add(p, p);
        cswap(p, q, b);
    }
}

static void
scalarbase(gf p[4], const unsigned char *s)
{
    gf q[4];

    set25519(q[0], X);
    set25519(q[1], Y);
    set25519(q[2], gf1);
    M(q[3], X, Y);
    scalarmult(p, q, s);
}

/* ── Scalar reduction mod L ────────────────────────────────────────── */

static void
modL(unsigned char *r, i64 x[64])
{
    i64 carry, i, j;

    for (i = 63; i >= 32; --i) {
        carry = 0;
        for (j = i - 32; j < i - 12; ++j) {
            x[j] += carry - 16 * x[i] * L[j - (i - 32)];
            carry = (x[j] + 128) >> 8;
            x[j] -= carry << 8;
        }
        x[j] += carry;
        x[i] = 0;
    }
    carry = 0;
    for (j = 0; j < 32; j++) {
        x[j] += carry - (x[31] >> 4) * L[j];
        carry = x[j] >> 8;
        x[j] &= 255;
    }
    for (j = 0; j < 32; j++)
        x[j] -= carry * L[j];
    for (i = 0; i < 32; i++) {
        x[i + 1] += x[i] >> 8;
        r[i] = (unsigned char)(x[i] & 255);
    }
}

static void
reduce(unsigned char *a)
{
    i64 x[64], i;

    for (i = 0; i < 64; i++)
        x[i] = a[i];
    for (i = 0; i < 64; i++)
        a[i] = 0;
    modL(a, x);
}

/* ── Public API ────────────────────────────────────────────────────── */

int
playos_ed25519_verify(const unsigned char *sig, const unsigned char *msg,
                      size_t msglen, const unsigned char *pk)
{
    unsigned char t[32], h[64];
    unsigned char *sm;
    gf             p[4], q[4];
    size_t         smlen;
    size_t         i;

    /* Canonical-S check: the top 3 bits of S must be zero. */
    if (sig[63] & 224)
        return -1;

    if (unpackneg(q, pk))
        return -1;

    /* sm = R(32) || A(32) || M(msglen); hash over all of it. */
    smlen = 64 + msglen;
    sm = (unsigned char *)malloc(smlen);
    if (!sm)
        return -1;

    memcpy(sm, sig, 32);            /* R */
    memcpy(sm + 32, pk, 32);        /* A (overwrites S position) */
    if (msglen)
        memcpy(sm + 64, msg, msglen);

    playos_sha512(h, sm, smlen);
    free(sm);

    reduce(h);
    scalarmult(p, q, h);
    scalarbase(q, sig + 32);
    add(p, q);
    pack(t, p);

    for (i = 0; i < 32; i++)
        if (t[i] != sig[i])
            return -1;

    return 0;
}

/* Test/development helpers (also used to generate the embedded dev key). */

void
playos_ed25519_keypair(unsigned char *pk, unsigned char *sk)
{
    unsigned char d[64];
    gf             p[4];

    /* sk: 32 random bytes. The caller should supply real entropy. */
    playos_sha512(d, sk, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    scalarbase(p, d);
    pack(pk, p);

    memcpy(sk + 32, pk, 32);
}

void
playos_ed25519_sign(unsigned char *sig, const unsigned char *msg,
                    size_t msglen, const unsigned char *sk)
{
    unsigned char d[64], h[64], r[64];
    i64           x[64];
    gf            p[4];
    size_t        i, j;

    playos_sha512(d, sk, 32);
    d[0] &= 248;
    d[31] &= 127;
    d[31] |= 64;

    /* r = H(d[32..63] || msg) mod L */
    {
        unsigned char *buf = (unsigned char *)malloc(32 + msglen);
        if (!buf)
            return;
        memcpy(buf, d + 32, 32);
        if (msglen)
            memcpy(buf + 32, msg, msglen);
        playos_sha512(r, buf, 32 + msglen);
        free(buf);
    }
    reduce(r);

    scalarbase(p, r);
    pack(sig, p); /* R */

    /* k = H(R || A || msg) mod L */
    {
        unsigned char *buf = (unsigned char *)malloc(64 + msglen);
        if (!buf)
            return;
        memcpy(buf, sig, 32);
        memcpy(buf + 32, sk + 32, 32);
        if (msglen)
            memcpy(buf + 64, msg, msglen);
        playos_sha512(h, buf, 64 + msglen);
        free(buf);
    }
    reduce(h);

    /* S = r + k*a mod L */
    for (i = 0; i < 64; i++)
        x[i] = 0;
    for (i = 0; i < 32; i++)
        x[i] = r[i];
    for (i = 0; i < 32; i++)
        for (j = 0; j < 32; j++)
            x[i + j] += (i64)h[i] * d[j];
    modL(sig + 32, x);
}

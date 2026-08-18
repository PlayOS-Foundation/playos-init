/*
 * playos-init/sha256.h — Self-contained SHA-256 + HMAC-SHA256
 *
 * Pure C, no external libraries. Intended for boot.json digests and
 * the dev-only HMAC used to authenticate .playosb update bundles.
 */
#ifndef PLAYOS_SHA256_H
#define PLAYOS_SHA256_H

#include <stddef.h>
#include <stdint.h>

/* ── SHA-256 ─────────────────────────────────────────────────────── */

typedef struct {
    uint32_t       state[8];
    uint64_t       bitlen;
    unsigned char  buffer[64];
    size_t         buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *ctx);
void sha256_update(sha256_ctx *ctx, const unsigned char *data, size_t len);
void sha256_final(sha256_ctx *ctx, unsigned char out[32]);

/* Lowercase hex encoding. `len` must be <= 32 so the result fits in
 * `out` (which is exactly 65 bytes including the NUL terminator). */
void sha256_hex(const unsigned char *data, size_t len, char out[65]);

/* ── HMAC-SHA256 ─────────────────────────────────────────────────── */

typedef struct {
    sha256_ctx     inner;
    unsigned char  opad[64];
} sha256_hmac_ctx;

void sha256_hmac_init(sha256_hmac_ctx *ctx, const unsigned char *key,
                      size_t key_len);
void sha256_hmac_update(sha256_hmac_ctx *ctx, const unsigned char *data,
                        size_t len);
void sha256_hmac_final(sha256_hmac_ctx *ctx, unsigned char out[32]);

/* One-shot convenience wrapper. */
void sha256_hmac(const unsigned char *key, size_t key_len,
                 const unsigned char *data, size_t data_len,
                 unsigned char out[32]);

#endif /* PLAYOS_SHA256_H */

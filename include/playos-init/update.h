/*
 * playos-init/update.h — Immutable update bundles (Sprint 11)
 *
 * A .playosb bundle is a self-describing payload:
 *
 *   [0..3]   magic "PBS1"
 *   [4..7]   header length, uint32 little-endian
 *   [8..]    header JSON (UTF-8, `header length` bytes)
 *   [...]    payload bytes (exactly payload_size)
 *   [...]    signature length, uint32 little-endian (= 64)
 *   [...]    signature as 64 lowercase hex chars (HMAC-SHA256)
 *
 * The dev-only HMAC key is a compile-time placeholder; production images
 * replace it with a per-device key in the bootloader chain.
 */
#ifndef PLAYOS_UPDATE_H
#define PLAYOS_UPDATE_H

#include <stdint.h>

#define PLAYOS_UPDATE_MAGIC       "PBS1"
#define PLAYOS_UPDATE_HEADER_MAX  4096
#define PLAYOS_UPDATE_KEY         "playos-dev-update-key-not-for-production"
#define PLAYOS_UPDATE_SIG_HEX_LEN 64

enum {
    UPDATE_OK                     = 0,
    UPDATE_ERR_NOT_FOUND          = -1,
    UPDATE_ERR_INVALID_BUNDLE     = -2,
    UPDATE_ERR_SIGNATURE_INVALID  = -3,
    UPDATE_ERR_UPDATE_IN_PROGRESS = -4,
    UPDATE_ERR_GAME_RUNNING       = -5,
    UPDATE_ERR_INTERNAL           = -6
};

struct update_info {
    char     format[32];
    char     version[64];
    uint64_t payload_size;
    uint64_t payload_offset;   /* byte offset of the payload in the bundle */
    char     payload_sha256[65];
    char     sig_alg[32];
};

/* Human-readable reason string for an update_* return code. */
const char *update_reason_str(int rc);

/*
 * Parse and cryptographically verify a .playosb bundle at `path`.
 *
 * Does not touch any partition or device — safe to run in host tests.
 * On success fills `out` with the parsed header metadata and returns
 * UPDATE_OK; otherwise returns a negative UPDATE_ERR_* code.
 */
int update_verify(const char *path, struct update_info *out);

/*
 * Verify `path` and, if valid, stream its payload onto the inactive boot
 * partition and advance the A/B boot slot to boot into it next reboot.
 *
 * Returns UPDATE_OK or a negative UPDATE_ERR_* code. A global in-progress
 * guard serializes concurrent applies.
 */
int update_apply(const char *path);

#endif /* PLAYOS_UPDATE_H */

/*
 * playos-init/src/update.c — Immutable update bundles (Sprint 11)
 *
 * verify-only parsing is deliberately side-effect free so it can run under
 * the host test suite; update_apply() is the privileged path that writes
 * to the inactive boot partition and flips the A/B slot.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "playos-init/update.h"
#include "playos-init/sha256.h"
#include "playos-init/boot_slot.h"
#include "playos-init/mount.h"

/* ── Little-endian decoding ───────────────────────────────────────── */

static inline uint32_t le32_dec(const unsigned char *p)
{
    return ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* ── Hex / constant-time helpers ──────────────────────────────────── */

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_decode(const char *hex, size_t hex_len, unsigned char *out,
                      size_t out_len)
{
    if (hex_len != out_len * 2)
        return -1;

    for (size_t i = 0; i < out_len; i++) {
        int hi = hex_nibble(hex[i * 2]);
        int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static int const_time_eq(const unsigned char *a, const unsigned char *b,
                         size_t n)
{
    unsigned char d = 0;
    for (size_t i = 0; i < n; i++)
        d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

/* ── Minimal JSON header parsing ──────────────────────────────────── */

static int json_string(const char *json, const char *key, char *out,
                       size_t outsz)
{
    if (!json || !key || !out || outsz == 0)
        return -1;

    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;

    const char *p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p = strchr(p, '"');
    if (!p)
        return -1;
    p++;

    const char *end = strchr(p, '"');
    if (!end)
        return -1;

    size_t len = (size_t)(end - p);
    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

static int json_u64(const char *json, const char *key, uint64_t *out)
{
    if (!json || !key || !out)
        return -1;

    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;

    const char *p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p)
        return -1;

    *out = (uint64_t)v;
    return 0;
}

/* ── Verification ─────────────────────────────────────────────────── */

int update_verify(const char *path, struct update_info *out)
{
    if (!path || !out)
        return UPDATE_ERR_INTERNAL;

    memset(out, 0, sizeof(*out));

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return UPDATE_ERR_NOT_FOUND;

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) {
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    unsigned char head[8];
    if (pread(fd, head, 8, 0) != 8) {
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }
    if (memcmp(head, PLAYOS_UPDATE_MAGIC, 4) != 0) {
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    uint32_t header_len = le32_dec(head + 4);
    if (header_len == 0 || header_len > PLAYOS_UPDATE_HEADER_MAX) {
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }
    if ((off_t)8 + header_len > st.st_size) {
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    char *header = malloc((size_t)header_len + 1);
    if (!header) {
        close(fd);
        return UPDATE_ERR_INTERNAL;
    }
    if (pread(fd, header, header_len, 8) != (ssize_t)header_len) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }
    header[header_len] = '\0';

    /* Parse header fields into locals before touching `out`. */
    char format[32];
    char version[64];
    uint64_t payload_size = 0;
    char payload_sha256[65];
    char sig_alg[32];

    int bad = 0;
    if (json_string(header, "format", format, sizeof(format)) != 0 ||
        strcmp(format, "playosb-1") != 0)
        bad = 1;
    if (json_string(header, "version", version, sizeof(version)) != 0 ||
        version[0] == '\0')
        bad = 1;
    if (json_u64(header, "payload_size", &payload_size) != 0)
        bad = 1;
    if (json_string(header, "payload_sha256", payload_sha256,
                    sizeof(payload_sha256)) != 0 ||
        strlen(payload_sha256) != 64)
        bad = 1;
    if (json_string(header, "sig_alg", sig_alg, sizeof(sig_alg)) != 0 ||
        strcmp(sig_alg, "hmac-sha256-dev") != 0)
        bad = 1;

    if (bad) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    uint64_t payload_offset = 8 + (uint64_t)header_len;
    uint64_t prefix_len = payload_offset + payload_size;
    if (prefix_len + 4 > (uint64_t)st.st_size) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    unsigned char slen[4];
    if (pread(fd, slen, 4, (off_t)prefix_len) != 4) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }
    uint32_t sig_len = le32_dec(slen);
    if (sig_len != PLAYOS_UPDATE_SIG_HEX_LEN) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    char sig_hex[65];
    if (pread(fd, sig_hex, sig_len, (off_t)(prefix_len + 4)) !=
        (ssize_t)sig_len) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }
    sig_hex[sig_len] = '\0';

    /* Recompute the payload SHA-256 and the HMAC over head + header +
     * payload, streaming the payload once. */
    sha256_ctx payload_ctx;
    sha256_init(&payload_ctx);

    sha256_hmac_ctx hmac;
    sha256_hmac_init(&hmac, (const unsigned char *)PLAYOS_UPDATE_KEY,
                     strlen(PLAYOS_UPDATE_KEY));
    sha256_hmac_update(&hmac, head, 8);
    sha256_hmac_update(&hmac, (const unsigned char *)header, header_len);

    unsigned char buf[65536];
    uint64_t remaining = payload_size;
    off_t pos = (off_t)payload_offset;
    int stream_ok = 1;
    while (remaining > 0) {
        size_t want = remaining < (uint64_t)sizeof(buf)
                    ? (size_t)remaining : sizeof(buf);
        ssize_t got = pread(fd, buf, want, pos);
        if (got != (ssize_t)want) {
            stream_ok = 0;
            break;
        }
        sha256_update(&payload_ctx, buf, (size_t)got);
        sha256_hmac_update(&hmac, buf, (size_t)got);
        remaining -= (uint64_t)got;
        pos += got;
    }

    if (!stream_ok) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    unsigned char payload_digest[32];
    sha256_final(&payload_ctx, payload_digest);

    unsigned char expected_digest[32];
    if (hex_decode(payload_sha256, 64, expected_digest, 32) != 0 ||
        !const_time_eq(payload_digest, expected_digest, 32)) {
        free(header);
        close(fd);
        return UPDATE_ERR_INVALID_BUNDLE;
    }

    unsigned char sig[32];
    sha256_hmac_final(&hmac, sig);

    unsigned char expected_sig[32];
    if (hex_decode(sig_hex, PLAYOS_UPDATE_SIG_HEX_LEN, expected_sig, 32) != 0 ||
        !const_time_eq(sig, expected_sig, 32)) {
        free(header);
        close(fd);
        return UPDATE_ERR_SIGNATURE_INVALID;
    }

    free(header);
    close(fd);

    snprintf(out->format, sizeof(out->format), "%s", format);
    snprintf(out->version, sizeof(out->version), "%s", version);
    out->payload_size = payload_size;
    out->payload_offset = payload_offset;
    snprintf(out->payload_sha256, sizeof(out->payload_sha256), "%s",
             payload_sha256);
    snprintf(out->sig_alg, sizeof(out->sig_alg), "%s", sig_alg);
    return UPDATE_OK;
}

/* ── Applying ─────────────────────────────────────────────────────── */

static int s_update_in_progress = 0;

/* Stream the verified payload from the bundle onto the target partition. */
static int update_write_payload(const char *path, const struct update_info *info,
                                const char *device)
{
    int src = open(path, O_RDONLY);
    if (src < 0) {
        dprintf(STDERR_FILENO,
                "playos-init: update: cannot reopen bundle: %s\n",
                strerror(errno));
        return -1;
    }

    int dst = open(device, O_WRONLY);
    if (dst < 0) {
        dprintf(STDERR_FILENO,
                "playos-init: update: cannot open %s for write: %s\n",
                device, strerror(errno));
        close(src);
        return -1;
    }

    if (lseek(dst, 0, SEEK_SET) != 0) {
        close(src);
        close(dst);
        return -1;
    }

    unsigned char buf[65536];
    uint64_t remaining = info->payload_size;
    off_t pos = (off_t)info->payload_offset;
    int rc = 0;

    while (remaining > 0) {
        size_t want = remaining < (uint64_t)sizeof(buf)
                    ? (size_t)remaining : sizeof(buf);
        ssize_t got = pread(src, buf, want, pos);
        if (got != (ssize_t)want) {
            rc = -1;
            break;
        }

        ssize_t off = 0;
        while (off < got) {
            ssize_t w = write(dst, buf + off, (size_t)(got - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;

        remaining -= (uint64_t)got;
        pos += got;
    }

    if (rc == 0 && fsync(dst) != 0)
        rc = -1;

    close(dst);
    close(src);
    return rc;
}

const char *update_reason_str(int rc)
{
    switch (rc) {
    case UPDATE_OK:                     return "ok";
    case UPDATE_ERR_NOT_FOUND:          return "not_found";
    case UPDATE_ERR_INVALID_BUNDLE:     return "invalid_bundle";
    case UPDATE_ERR_SIGNATURE_INVALID:  return "signature_invalid";
    case UPDATE_ERR_UPDATE_IN_PROGRESS: return "update_in_progress";
    case UPDATE_ERR_GAME_RUNNING:       return "game_running";
    case UPDATE_ERR_INTERNAL:           return "internal_error";
    default:                            return "unknown";
    }
}

/*
 * Testable apply core. `boot_json_path` lets the host test point the A/B
 * accounting at a temp file instead of the real /EFI/playos/boot.json; the
 * production entry point below always uses the real path.
 */
int update_apply_at(const char *path, const char *boot_json_path)
{
    if (!path || !boot_json_path)
        return UPDATE_ERR_INTERNAL;

    if (s_update_in_progress)
        return UPDATE_ERR_UPDATE_IN_PROGRESS;
    s_update_in_progress = 1;

    struct update_info info;
    int rc = update_verify(path, &info);
    if (rc != UPDATE_OK) {
        dprintf(STDERR_FILENO, "playos-init: update_verify failed (%d: %s)\n",
                rc, update_reason_str(rc));
        s_update_in_progress = 0;
        return rc;
    }

    struct boot_slot_state bs;
    (void)boot_slot_read(boot_json_path, &bs);

    char inactive = (bs.active_slot == 'a') ? 'b' : 'a';
    char part_label[16];
    snprintf(part_label, sizeof(part_label), "playos-%c", inactive);

    char device[128];
    if (playos_find_partition_by_label(part_label, device, sizeof(device)) != 0) {
        dprintf(STDERR_FILENO, "playos-init: update: partition %s not found\n",
                part_label);
        s_update_in_progress = 0;
        return UPDATE_ERR_INTERNAL;
    }

    if (update_write_payload(path, &info, device) != 0) {
        dprintf(STDERR_FILENO, "playos-init: update: payload write failed\n");
        s_update_in_progress = 0;
        return UPDATE_ERR_INTERNAL;
    }

    /* EFI-side system files are updated by the bundle tooling out of band;
     * this stub keeps the init-side write path explicit for future work. */
    dprintf(STDERR_FILENO, "playos-init: update: write_efi (stub)\n");

    /* Advance the boot slot to the freshly-written inactive partition. */
    struct boot_slot_info *newactive =
        (inactive == 'b') ? &bs.slot_b : &bs.slot_a;
    snprintf(newactive->version, sizeof(newactive->version), "%s",
             info.version);
    newactive->boot_count = 0;
    snprintf(newactive->health, sizeof(newactive->health), "pending");

    bs.active_slot = inactive;
    if (boot_slot_write(boot_json_path, &bs) != 0) {
        dprintf(STDERR_FILENO, "playos-init: update: failed to write boot.json\n");
        s_update_in_progress = 0;
        return UPDATE_ERR_INTERNAL;
    }

    sync();
    s_update_in_progress = 0;
    return UPDATE_OK;
}

int update_apply(const char *path)
{
    return update_apply_at(path, PLAYOS_BOOT_JSON_PATH);
}

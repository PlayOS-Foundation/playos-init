/*
 * tests/host/test_boot_slot.c — Sprint 11 boot slot + update verification
 *
 * Standalone host test. Links against boot_slot.c, update.c, sha256.c and
 * mount.c; mount.c is pulled in only to satisfy update_apply()'s partition
 * lookup symbol (playos_find_partition_by_label), which this test never
 * exercises.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>

#include "playos-init/boot_slot.h"
#include "playos-init/update.h"
#include "playos-init/sha256.h"

static void le32_enc(uint32_t v, unsigned char out[4])
{
    out[0] = (unsigned char)(v & 0xff);
    out[1] = (unsigned char)((v >> 8) & 0xff);
    out[2] = (unsigned char)((v >> 16) & 0xff);
    out[3] = (unsigned char)((v >> 24) & 0xff);
}

/*
 * Build a well-formed .playosb bundle in memory:
 *
 *   [0..3]   "PBS1"
 *   [4..7]   header length (LE)
 *   [8..]    header JSON
 *   [...]    payload
 *   [...]    sig length = 64 (LE)
 *   [...]    HMAC-SHA256 hex over head + header + payload
 */
static size_t build_bundle(unsigned char **out, const unsigned char *payload,
                           size_t payload_len)
{
    unsigned char digest[32];
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, payload, payload_len);
    sha256_final(&ctx, digest);

    char payload_hex[65];
    sha256_hex(digest, 32, payload_hex);

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "{\"format\":\"playosb-1\",\"version\":\"9.9.9\","
        "\"payload_size\":%zu,\"payload_sha256\":\"%s\","
        "\"sig_alg\":\"hmac-sha256-dev\"}",
        payload_len, payload_hex);
    assert(hlen > 0 && (size_t)hlen < sizeof(header));

    unsigned char head[8];
    memcpy(head, PLAYOS_UPDATE_MAGIC, 4);
    le32_enc((uint32_t)hlen, head + 4);

    unsigned char hmac[32];
    sha256_hmac_ctx hctx;
    sha256_hmac_init(&hctx, (const unsigned char *)PLAYOS_UPDATE_KEY,
                     strlen(PLAYOS_UPDATE_KEY));
    sha256_hmac_update(&hctx, head, 8);
    sha256_hmac_update(&hctx, (const unsigned char *)header, (size_t)hlen);
    sha256_hmac_update(&hctx, payload, payload_len);
    sha256_hmac_final(&hctx, hmac);

    char sig_hex[65];
    sha256_hex(hmac, 32, sig_hex);

    size_t total = 8 + (size_t)hlen + payload_len + 4 + 64;
    unsigned char *buf = malloc(total);
    assert(buf != NULL);

    size_t off = 0;
    memcpy(buf + off, head, 8); off += 8;
    memcpy(buf + off, header, (size_t)hlen); off += (size_t)hlen;
    memcpy(buf + off, payload, payload_len); off += payload_len;

    unsigned char slen[4];
    le32_enc(64, slen);
    memcpy(buf + off, slen, 4); off += 4;
    memcpy(buf + off, sig_hex, 64); off += 64;
    assert(off == total);

    *out = buf;
    return total;
}

static void write_file(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f != NULL);
    assert(fwrite(data, 1, len, f) == len);
    assert(fclose(f) == 0);
}

static void make_default_state(struct boot_slot_state *st)
{
    memset(st, 0, sizeof(*st));
    st->v = 1;
    st->active_slot = 'a';
    snprintf(st->slot_a.version, sizeof(st->slot_a.version), "1.2.3");
    st->slot_a.boot_count = 0;
    snprintf(st->slot_a.health, sizeof(st->slot_a.health), "good");
    snprintf(st->slot_b.version, sizeof(st->slot_b.version), "0.9.0");
    st->slot_b.boot_count = 0;
    snprintf(st->slot_b.health, sizeof(st->slot_b.health), "empty");
}

int main(void)
{
    char tmpl[] = "/tmp/playos-bootslot-XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir != NULL);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/boot.json", dir);

    /* 1. Missing file fills safe defaults and returns -1. */
    struct boot_slot_state st;
    int rc = boot_slot_read(path, &st);
    assert(rc == -1);
    assert(st.v == 1);
    assert(st.active_slot == 'a');
    assert(strcmp(st.slot_a.version, "0.1.0") == 0);
    assert(strcmp(st.slot_a.health, "good") == 0);
    assert(st.slot_a.boot_count == 0);
    assert(st.slot_b.version[0] == '\0');
    assert(strcmp(st.slot_b.health, "empty") == 0);
    printf("PASS: default/corrupt read fills safe defaults\n");

    /* 2. Write + read round-trip. */
    make_default_state(&st);
    st.active_slot = 'b';
    snprintf(st.slot_b.health, sizeof(st.slot_b.health), "pending");
    st.slot_b.boot_count = 2;
    assert(boot_slot_write(path, &st) == 0);

    struct boot_slot_state rt;
    assert(boot_slot_read(path, &rt) == 0);
    assert(rt.v == 1);
    assert(rt.active_slot == 'b');
    assert(strcmp(rt.slot_a.version, "1.2.3") == 0);
    assert(strcmp(rt.slot_a.health, "good") == 0);
    assert(strcmp(rt.slot_b.version, "0.9.0") == 0);
    assert(strcmp(rt.slot_b.health, "pending") == 0);
    assert(rt.slot_b.boot_count == 2);
    printf("PASS: write/read round-trip\n");

    /* 3. increment: healthy slot never rolls back. */
    make_default_state(&st);
    st.active_slot = 'a';
    st.slot_a.boot_count = 2;
    assert(strcmp(st.slot_a.health, "good") == 0);
    assert(boot_slot_write(path, &st) == 0);
    assert(boot_slot_increment(path, &st) == 0);
    assert(st.slot_a.boot_count == 3);
    printf("PASS: increment healthy slot (no rollback)\n");

    /* 4. increment: unhealthy slot rolls back once boot_count >= 3. */
    make_default_state(&st);
    st.active_slot = 'a';
    st.slot_a.boot_count = 3;
    snprintf(st.slot_a.health, sizeof(st.slot_a.health), "pending");
    assert(boot_slot_write(path, &st) == 0);
    assert(boot_slot_increment(path, &st) == 1);
    assert(st.slot_a.boot_count == 4);
    printf("PASS: increment unhealthy slot triggers rollback\n");

    /* 5. mark_good clears the counter and sets health. */
    make_default_state(&st);
    st.active_slot = 'a';
    st.slot_a.boot_count = 5;
    snprintf(st.slot_a.health, sizeof(st.slot_a.health), "bad");
    assert(boot_slot_write(path, &st) == 0);
    assert(boot_slot_mark_good(path, &st) == 0);
    assert(strcmp(st.slot_a.health, "good") == 0);
    assert(st.slot_a.boot_count == 0);
    printf("PASS: mark_good clears boot count\n");

    /* 6. rollback switches slot and puts the new slot in pending. */
    make_default_state(&st);
    st.active_slot = 'a';
    snprintf(st.slot_a.health, sizeof(st.slot_a.health), "good");
    snprintf(st.slot_b.version, sizeof(st.slot_b.version), "0.9.0");
    snprintf(st.slot_b.health, sizeof(st.slot_b.health), "empty");
    assert(boot_slot_rollback(path, &st) == 0);
    assert(st.active_slot == 'b');
    assert(strcmp(st.slot_a.health, "bad") == 0);
    assert(strcmp(st.slot_b.health, "pending") == 0);
    assert(st.slot_b.boot_count == 0);
    assert(strcmp(st.slot_b.version, "0.9.0") == 0);
    printf("PASS: rollback switches slot and sets pending\n");

    /* 7. update_verify accepts a valid bundle. */
    const char payload[] = "playos immutable update payload";
    unsigned char *bundle = NULL;
    size_t bundle_len = build_bundle(&bundle,
                                     (const unsigned char *)payload,
                                     sizeof(payload) - 1);

    char bundle_path[PATH_MAX];
    snprintf(bundle_path, sizeof(bundle_path), "%s/update.playosb", dir);
    write_file(bundle_path, bundle, bundle_len);

    struct update_info info;
    rc = update_verify(bundle_path, &info);
    assert(rc == UPDATE_OK);
    assert(strcmp(info.format, "playosb-1") == 0);
    assert(strcmp(info.version, "9.9.9") == 0);
    assert(info.payload_size == sizeof(payload) - 1);
    assert(strcmp(info.sig_alg, "hmac-sha256-dev") == 0);
    printf("PASS: update_verify accepts valid bundle\n");

    /* 8. Flipping a signature hex char is a signature failure. */
    unsigned char *bad = malloc(bundle_len);
    assert(bad != NULL);
    memcpy(bad, bundle, bundle_len);
    bad[bundle_len - 1] = (bad[bundle_len - 1] == 'a') ? 'b' : 'a';

    char bad_path[PATH_MAX];
    snprintf(bad_path, sizeof(bad_path), "%s/bad-sig.playosb", dir);
    write_file(bad_path, bad, bundle_len);
    rc = update_verify(bad_path, &info);
    assert(rc == UPDATE_ERR_SIGNATURE_INVALID);
    printf("PASS: update_verify rejects bad signature\n");

    /* 9. Corrupting the magic is an invalid bundle. */
    memcpy(bad, bundle, bundle_len);
    bad[0] = 'X';
    snprintf(bad_path, sizeof(bad_path), "%s/bad-magic.playosb", dir);
    write_file(bad_path, bad, bundle_len);
    rc = update_verify(bad_path, &info);
    assert(rc == UPDATE_ERR_INVALID_BUNDLE);
    printf("PASS: update_verify rejects bad magic\n");

    free(bad);
    free(bundle);

    /* Best-effort cleanup. */
    char p[PATH_MAX];
    snprintf(p, sizeof(p), "%s/boot.json", dir); unlink(p);
    snprintf(p, sizeof(p), "%s/update.playosb", dir); unlink(p);
    snprintf(p, sizeof(p), "%s/bad-sig.playosb", dir); unlink(p);
    snprintf(p, sizeof(p), "%s/bad-magic.playosb", dir); unlink(p);
    rmdir(dir);

    printf("\nAll boot slot + update verification tests passed.\n");
    return 0;
}

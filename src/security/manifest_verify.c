/*
 * playos-init/src/security/manifest_verify.c — S12-T8 manifest signatures
 *
 * Warn-only Ed25519 verification of a game manifest:
 *
 *   manifest_path      /data/games/<id>/manifest.json
 *   sig_path           /data/games/<id>/manifest.json.sig  (64 raw bytes)
 *
 * Return codes (never block launch — the caller decides what to log):
 *    0  signature valid
 *    1  signature missing or unreadable
 *   -1  signature present but invalid
 *   -2  verifier error (out of memory, etc.)
 */
#include "playos-init/security.h"
#include "game_key.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* Read a whole file into a freshly allocated buffer. Returns bytes read
 * (>= 0) on success, -1 on error. Sets *out to NULL on failure. */
static long
read_file_alloc(const char *path, unsigned char **out)
{
    long         size;
    long         got;
    FILE        *f;

    *out = NULL;

    f = fopen(path, "rb");
    if (!f)
        return -1;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return -1;
    }
    rewind(f);

    *out = (unsigned char *)malloc((size_t)size + 1);
    if (!*out) {
        fclose(f);
        return -1;
    }

    got = (long)fread(*out, 1, (size_t)size, f);
    fclose(f);
    if (got != size) {
        free(*out);
        *out = NULL;
        return -1;
    }

    return size;
}

int
playos_security_verify_manifest(const char *manifest_path,
                                const char *sig_path)
{
    unsigned char *manifest = NULL;
    unsigned char *sig      = NULL;
    long           mlen;
    long           slen;
    int            rc = -2;

    mlen = read_file_alloc(manifest_path, &manifest);
    if (mlen < 0)
        return -2; /* unreadable manifest is a verifier error */

    slen = read_file_alloc(sig_path, &sig);
    if (slen < 0) {
        rc = 1; /* missing signature */
        goto out;
    }
    if (slen != 64) {
        rc = -1; /* present but malformed */
        goto out;
    }

    rc = playos_ed25519_verify(sig, manifest, (size_t)mlen,
                               PLAYOS_MANIFEST_PUBKEY);
    rc = (rc == 0) ? 0 : -1;

out:
    free(manifest);
    free(sig);
    return rc;
}

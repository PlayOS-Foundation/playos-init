/*
 * playos-init/src/security/game_key.h — PlayOS development manifest key
 *
 * Ed25519 public key embedded in playos-init for warn-only manifest
 * signature verification (S12-T8). The matching private key lives at
 * playos-refdistro/keys/dev/manifest-key.sec (development only — never
 * commit production keys; production signing is post-MVP HSM).
 */
#ifndef PLAYOS_GAME_KEY_H
#define PLAYOS_GAME_KEY_H

static const unsigned char PLAYOS_MANIFEST_PUBKEY[32] = {
    0xac, 0x90, 0x8b, 0xc7, 0xd6, 0x9f, 0x3a, 0x58,
    0x2c, 0x2f, 0x62, 0xc5, 0x4b, 0x23, 0x1a, 0x9f,
    0xf2, 0xb9, 0x81, 0x49, 0x83, 0xaf, 0x33, 0x55,
    0xcf, 0xfc, 0x11, 0x47, 0x3e, 0x0c, 0xc0, 0x5d
};

#endif /* PLAYOS_GAME_KEY_H */

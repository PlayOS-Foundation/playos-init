/*
 * playos-init/security.h — Sprint 12 game sandbox
 *
 * Privilege reduction and OS-level enforcement for untrusted game
 * processes. The sandbox is applied in the game child process, before
 * exec, in this order:
 *
 *   1. PR_SET_NO_NEW_PRIVS                     (playos_security_disable_priv_escalation)
 *   2. Landlock filesystem allowlist           (playos_security_apply_landlock)
 *   3. drop supplementary groups + credentials (playos_security_drop_privileges)
 *   4. seccomp-BPF deny-list                   (playos_security_apply_seccomp)
 *
 * The credential drop must run before seccomp because the filter denies
 * setuid/setgid/capset.
 */
#ifndef PLAYOS_SECURITY_H
#define PLAYOS_SECURITY_H

#include <stddef.h>

/* Game identity (S12-T1). Keep in sync with playos-refdistro's
 * users-table.txt and /etc/group overlay. */
#define PLAYOS_GAME_UID 1001
#define PLAYOS_GAME_GID 1001

/* Supplementary groups the game needs for device access. Kept in sync
 * with br2-external/board/common/rootfs-overlay/etc/group and
 * users-table.txt:
 *   - render (108): /dev/dri/renderD* for client-side EGL/GLES2
 *   - audio  (29):  /dev/snd/ nodes for ALSA
 *   - input  (102): /dev/input/event* for the built-in controller
 *                   (libplayos evdev backend). Reserved buttons are still
 *                   stripped from game-facing snapshots by the backend.
 * Games are intentionally NOT in video/drm — the primary DRM node
 * /dev/dri/card* stays root-only. */
#define PLAYOS_RENDER_GID 108
#define PLAYOS_AUDIO_GID  29
#define PLAYOS_INPUT_GID  102

/* 1. Disable privilege escalation for this process and its children. */
int playos_security_disable_priv_escalation(void);

/* 2. Apply the Landlock filesystem allowlist for `game_id`.
 * Returns 0 on success, 1 if Landlock is unsupported (caller logs an
 * alert but continues), -1 on a hard error. */
int playos_security_apply_landlock(const char *game_id);

/* Landlock path policy, data-driven for Sprint 21 profile scoping. The
 * per-game /data paths are built by playos_security_apply_landlock();
 * this struct exists so host tests can exercise the same ruleset engine
 * against temporary directories. */
struct playos_landlock_paths {
    const char *game_dir;     /* read-only game content (read+execute)  */
    const char *saves_dir;    /* read-write save data                  */
    const char *cache_dir;    /* read-write cache data                 */
    const char *tmp_dir;      /* read-write scratch (usually /tmp)     */
    const char *run_playos;   /* read+execute (Wayland socket path)    */
    const char *lib_dir;      /* read+execute (musl loader)            */
    const char *usr_lib;      /* read+execute (shared libraries)       */
    const char *dev_snd;      /* read+write (ALSA)                     */
    const char *dev_input;    /* read (evdev controller nodes)         */
    const char *dev_shm;      /* read-write (wl_shm)                   */
    const char *asound_conf;  /* read, optional                        */
};

/* Apply a Landlock ruleset from explicit paths. Same return convention
 * as playos_security_apply_landlock(). */
int playos_landlock_apply_ruleset(const struct playos_landlock_paths *p);

/* 3. Restrict supplementary groups to {render, audio, input} and drop
 * credentials to PLAYOS_GAME_UID/GID. Also clears all capabilities from the
 * effective/permitted sets before the uid change. Must run while still
 * root. */
int playos_security_drop_privileges(void);

/* 4. Apply the seccomp-BPF deny-list. Returns 0 on success, -1 on error. */
int playos_security_apply_seccomp(void);

/* ── Manifest signature verification (S12-T8, warn-only) ──────────── */

/* Verify an Ed25519 detached signature over `manifest_path`.
 *  - manifest_path: path to manifest.json
 *  - sig_path:      path to the 64-byte raw signature (manifest.json.sig)
 *
 * Returns:
 *    0  signature valid
 *    1  signature missing or unreadable
 *   -1  signature present but invalid
 *   -2  crypto/verifier error
 *
 * Never blocks launch: the caller treats every return value as a log
 * decision (warn-only in the MVP).
 */
int playos_security_verify_manifest(const char *manifest_path,
                                    const char *sig_path);

/* Ed25519 verify (exposed for host tests). pk is 32 bytes, sig is 64,
 * msg/msglen arbitrary. Returns 0 on valid signature, -1 otherwise. */
int playos_ed25519_verify(const unsigned char *sig,
                          const unsigned char *msg, size_t msglen,
                          const unsigned char *pk);

/* Development/test helpers: generate a keypair and sign a message.
 * Used by host tests and the dev-key generation tool; production code
 * only verifies. */
void playos_ed25519_keypair(unsigned char *pk, unsigned char *sk);
void playos_ed25519_sign(unsigned char *sig, const unsigned char *msg,
                         size_t msglen, const unsigned char *sk);

#endif /* PLAYOS_SECURITY_H */

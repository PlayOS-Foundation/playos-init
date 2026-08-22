/*
 * playos-init/src/security/landlock.c — S12-T2 Landlock filesystem allowlist
 *
 * Builds a default-deny Landlock ruleset granting exactly the paths a game
 * needs (game dir read+execute, per-game saves/cache read-write, /tmp,
 * /run/playos, the musl dynamic-loader paths, ALSA and wl_shm device paths)
 * and restricts the game process to it before exec.
 *
 * Rule construction is data-driven: one path-construction function takes the
 * launch identity (game id now, profile id later) so Sprint 21 can add
 * profile scoping without touching enforcement.
 *
 * Landlock is unprivileged: create/add/restrict work for any process, but
 * restrict_self requires PR_SET_NO_NEW_PRIVS (already applied by the caller
 * before this function runs) or CAP_SYS_ADMIN.
 */
#define _GNU_SOURCE
#include "playos-init/security.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/landlock.h>
#endif

#ifndef SYS_landlock_create_ruleset
#define SYS_landlock_create_ruleset 444
#endif
#ifndef SYS_landlock_add_rule
#define SYS_landlock_add_rule 445
#endif
#ifndef SYS_landlock_restrict_self
#define SYS_landlock_restrict_self 446
#endif

#ifndef O_PATH
#define O_PATH 010000000
#endif

/* Fallback definitions so the code compiles on toolchains whose
 * <linux/landlock.h> predates some ABI versions. */
#ifndef LANDLOCK_ACCESS_FS_REFER
#define LANDLOCK_ACCESS_FS_REFER (1ULL << 13)
#endif
#ifndef LANDLOCK_ACCESS_FS_TRUNCATE
#define LANDLOCK_ACCESS_FS_TRUNCATE (1ULL << 14)
#endif

/* Access rights introduced in each ABI version (man 7 landlock). */
#define LL_BASE_RIGHTS                                                     \
    (LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |          \
     LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |          \
     LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |      \
     LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |          \
     LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |          \
     LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |        \
     LANDLOCK_ACCESS_FS_MAKE_SYM)

/* Read-only game content: files + dynamic traversal. */
#define LL_GAME_RO (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_EXECUTE)

/* Read-write scratch/save/cache: files + create/remove. */
#define LL_RW_DIR                                                          \
    (LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_WRITE_FILE |        \
     LANDLOCK_ACCESS_FS_TRUNCATE | LANDLOCK_ACCESS_FS_MAKE_REG |           \
     LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |        \
     LANDLOCK_ACCESS_FS_REMOVE_DIR)

/* ── Path construction (data-driven for Sprint 21 profile scoping) ──── */

#define PLAYOS_LL_PATH_MAX 512

/*
 * Build the path for one per-game directory kind. `out` must hold
 * PLAYOS_LL_PATH_MAX bytes. Returns 0 on success, -1 on overflow.
 */
static int
ll_game_path(char *out, size_t outsz, const char *game_id, const char *kind)
{
    int n = snprintf(out, outsz, "/data/%s/%s", kind, game_id);
    return (n < 0 || (size_t)n >= outsz) ? -1 : 0;
}

/* ── Raw syscall wrappers ──────────────────────────────────────────── */

static int
landlock_create_ruleset(uint64_t handled_access_fs)
{
    struct landlock_ruleset_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.handled_access_fs = handled_access_fs;

    return (int)syscall(SYS_landlock_create_ruleset, &attr, sizeof(attr), 0);
}

static int
landlock_add_path_rule(int ruleset_fd, uint64_t allowed_access,
                       const char *path)
{
    int                       parent_fd;
    struct landlock_path_beneath_attr attr;

    parent_fd = open(path, O_PATH | O_CLOEXEC);
    if (parent_fd < 0)
        return -1;

    memset(&attr, 0, sizeof(attr));
    attr.allowed_access = allowed_access;
    attr.parent_fd      = parent_fd;

    if (syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH,
                &attr, 0) != 0) {
        int saved = errno;
        close(parent_fd);
        errno = saved;
        return -1;
    }

    close(parent_fd);
    return 0;
}

/*
 * Add a path rule, but treat a missing path as a no-op (0) rather than an
 * error. Used for optional single-file rules such as /etc/asound.conf.
 */
static int
landlock_add_path_rule_optional(int ruleset_fd, uint64_t allowed_access,
                                const char *path)
{
    /* NULL path => rule intentionally not configured. */
    if (!path)
        return 0;

    if (landlock_add_path_rule(ruleset_fd, allowed_access, path) != 0) {
        if (errno == ENOENT)
            return 0;
        return -1;
    }
    return 0;
}

/* Ensure a per-game data directory exists, owned by the game identity so
 * the post-setuid game can create files inside it. */
static int
ensure_game_dir(const char *path)
{
    if (mkdir(path, 0700) != 0 && errno != EEXIST)
        return -1;
    if (chown(path, PLAYOS_GAME_UID, PLAYOS_GAME_GID) != 0)
        return -1;
    if (chmod(path, 0700) != 0)
        return -1;
    return 0;
}

/* ── Ruleset engine (testable with explicit paths) ─────────────────── */

int
playos_landlock_apply_ruleset(const struct playos_landlock_paths *p)
{
    uint64_t  handled;
    uint64_t  abi;
    int       ruleset_fd;

    /* Query the Landlock ABI version (landlock_create_ruleset with the
     * VERSION flag returns the version instead of creating a ruleset). */
    abi = (uint64_t)syscall(SYS_landlock_create_ruleset, NULL, 0,
                            LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) {
        /* 0 or negative: Landlock unsupported/disabled. Loud fallback —
         * the caller logs an alert and continues without the sandbox. */
        return 1;
    }

    handled = LL_BASE_RIGHTS;
    if (abi >= 2)
        handled |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3)
        handled |= LANDLOCK_ACCESS_FS_TRUNCATE;

    ruleset_fd = landlock_create_ruleset(handled);
    if (ruleset_fd < 0)
        return -1;

    /* Allowlist. Any path not covered by one of these rules is denied. */
    if (landlock_add_path_rule(ruleset_fd, LL_GAME_RO, p->game_dir) != 0)
        goto fail;
    if (landlock_add_path_rule(ruleset_fd, LL_RW_DIR, p->saves_dir) != 0)
        goto fail;
    if (landlock_add_path_rule(ruleset_fd, LL_RW_DIR, p->cache_dir) != 0)
        goto fail;
    if (landlock_add_path_rule(ruleset_fd, LL_RW_DIR, p->tmp_dir) != 0)
        goto fail;
    if (landlock_add_path_rule(ruleset_fd,
                               LANDLOCK_ACCESS_FS_READ_FILE |
                                   LANDLOCK_ACCESS_FS_EXECUTE,
                               p->run_playos) != 0)
        goto fail;
    /* musl dynamic loader + shared libraries (libraylib, libplayos, …) */
    if (landlock_add_path_rule(ruleset_fd,
                               LANDLOCK_ACCESS_FS_READ_FILE |
                                   LANDLOCK_ACCESS_FS_EXECUTE,
                               p->lib_dir) != 0)
        goto fail;
    if (landlock_add_path_rule(ruleset_fd,
                               LANDLOCK_ACCESS_FS_READ_FILE |
                                   LANDLOCK_ACCESS_FS_EXECUTE,
                               p->usr_lib) != 0)
        goto fail;
    /* ALSA PCM/control nodes — audio must keep working (Sprint 8). */
    if (landlock_add_path_rule(ruleset_fd,
                               LANDLOCK_ACCESS_FS_READ_FILE |
                                   LANDLOCK_ACCESS_FS_WRITE_FILE,
                               p->dev_snd) != 0)
        goto fail;
    /* Wayland wl_shm fallback. */
    if (landlock_add_path_rule(ruleset_fd, LL_RW_DIR, p->dev_shm) != 0)
        goto fail;
    /* ALSA reads /etc/asound.conf (optional single-file rule). */
    if (landlock_add_path_rule_optional(ruleset_fd,
                                        LANDLOCK_ACCESS_FS_READ_FILE,
                                        p->asound_conf) != 0)
        goto fail;

    if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0)
        goto fail;

    close(ruleset_fd);
    return 0;

fail:
    {
        int saved = errno;
        close(ruleset_fd);
        errno = saved;
    }
    return -1;
}

/* ── Per-game entry point (production path) ────────────────────────── */

int
playos_security_apply_landlock(const char *game_id)
{
    char      saves[PLAYOS_LL_PATH_MAX];
    char      cache[PLAYOS_LL_PATH_MAX];
    char      games[PLAYOS_LL_PATH_MAX];
    struct playos_landlock_paths p;

    /* Per-game data directories must exist before their path rules are
     * added (and before the game, as uid 1001, tries to write them). */
    if (ll_game_path(saves, sizeof(saves), game_id, "saves") != 0 ||
        ll_game_path(cache, sizeof(cache), game_id, "cache") != 0 ||
        ll_game_path(games, sizeof(games), game_id, "games") != 0)
        return -1;

    if (ensure_game_dir(saves) != 0 || ensure_game_dir(cache) != 0)
        return -1;

    memset(&p, 0, sizeof(p));
    p.game_dir    = games;
    p.saves_dir   = saves;
    p.cache_dir   = cache;
    p.tmp_dir     = "/tmp";
    p.run_playos  = "/run/playos";
    p.lib_dir     = "/lib";
    p.usr_lib     = "/usr/lib";
    p.dev_snd     = "/dev/snd";
    p.dev_shm     = "/dev/shm";
    p.asound_conf = "/etc/asound.conf";

    return playos_landlock_apply_ruleset(&p);
}

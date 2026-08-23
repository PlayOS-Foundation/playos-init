/*
 * playos-init/tests/security_selftest.c — on-device sandbox self-test
 *
 * This binary is meant to be launched by playos-init through the normal
 * LaunchGame path so it runs *as the game identity*, inside the Sprint 12
 * sandbox (no_new_privs → Landlock → credential drop → seccomp). It is a
 * passive probe: it does not try to escape the sandbox, it simply reports
 * what the current process is and is not allowed to do.
 *
 * Each check prints one line to stderr:
 *
 *     [PASS]  <check>  <detail>
 *     [FAIL]  <check>  <detail>
 *     [SKIP]  <check>  <detail>
 *
 * stdout/stderr are redirected by playos-init to
 * /data/log/game-security-selftest-stderr.log, so results are visible on
 * the persistent data partition (USB) after the run.
 *
 * Build: it is compiled as part of the playos-init CMake project and
 * installed to /usr/bin/playos-security-selftest. It is NOT part of the
 * production image unless a game manifest references it (see the runbook).
 *
 * C99, static-link safe, no NSS.
 */
#define _GNU_SOURCE

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/capability.h>
#endif

#include "playos-init/init.h"
#include "playos-init/security.h"

#ifndef SYS_capget
#define SYS_capget 125
#endif
#ifndef SYS_mount
#define SYS_mount 165
#endif

static int checks_run = 0;
static int checks_fail = 0;

static void
report(int ok, const char *name, const char *detail)
{
    checks_run++;
    if (!ok)
        checks_fail++;
    fprintf(stderr, "[%s] %s %s\n",
            ok ? "PASS" : "FAIL", name, detail ? detail : "");
}

static void
report_skip(const char *name, const char *detail)
{
    fprintf(stderr, "[SKIP] %s %s\n", name, detail ? detail : "");
}

/* Open the first entry in `dir` whose name starts with `prefix`. Returns the
 * fd on success, or -1 (with errno set) if nothing suitable was found. */
static int
open_first_matching(const char *dir, const char *prefix, int flags)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int fd = -1;

    if (!d) {
        errno = ENOENT;
        return -1;
    }

    while ((e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            fd = open(path, flags);
            if (fd >= 0)
                break;
            /* Record the most recent errno; keep scanning in case a later
             * node is accessible (e.g. event0 vs event1). */
        }
    }
    closedir(d);
    return fd;
}

int
main(void)
{
    char detail[128];

    /* 1. Identity: the game must be the unprivileged uid/gid, not root. */
    uid_t uid = getuid();
    gid_t gid = getgid();
    snprintf(detail, sizeof(detail), "uid=%d gid=%d", (int)uid, (int)gid);
    report(uid == PLAYOS_GAME_UID && gid == PLAYOS_GAME_GID, "identity", detail);

    /* 2. NoNewPrivs must be latched. */
    int nnp = prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0);
    snprintf(detail, sizeof(detail), "no_new_privs=%d", nnp);
    report(nnp == 1, "no_new_privs", detail);

    /* 3. No capabilities remain. */
#ifdef __linux__
    {
        struct __user_cap_header_struct hdr;
        struct __user_cap_data_struct data[2];
        memset(&hdr, 0, sizeof(hdr));
        memset(data, 0, sizeof(data));
        hdr.version = _LINUX_CAPABILITY_VERSION_3;
        hdr.pid = 0;
        if (syscall(SYS_capget, &hdr, data) == 0) {
            unsigned long eff = ((unsigned long)data[1].effective << 32) |
                                (unsigned long)data[0].effective;
            unsigned long perm = ((unsigned long)data[1].permitted << 32) |
                                 (unsigned long)data[0].permitted;
            snprintf(detail, sizeof(detail), "eff=0x%lx perm=0x%lx", eff, perm);
            report(eff == 0 && perm == 0, "capabilities", detail);
        } else {
            snprintf(detail, sizeof(detail), "capget failed: %s",
                     strerror(errno));
            report(0, "capabilities", detail);
        }
    }
#else
    report_skip("capabilities", "non-Linux");
#endif

    /* 4. Primary DRM node must stay root-only (no video/drm membership). */
    {
        int fd = open("/dev/dri/card0", O_RDWR);
        if (fd >= 0) {
            close(fd);
            report(0, "dri_card0_denied", "opened — game reached primary DRM node");
        } else {
            snprintf(detail, sizeof(detail), "open failed: %s", strerror(errno));
            report(1, "dri_card0_denied", detail);
        }
    }

    /* 5. Render node should be reachable for client-side EGL/GLES2. If this
     * FAILs with EACCES it flags a Landlock allowlist gap for /dev/dri. */
    {
        int fd = open("/dev/dri/renderD128", O_RDWR);
        if (fd >= 0) {
            close(fd);
            report(1, "dri_render_allowed", "renderD128 opened");
        } else if (errno == ENOENT) {
            report_skip("dri_render_allowed", "renderD128 not present");
        } else {
            snprintf(detail, sizeof(detail), "open failed: %s", strerror(errno));
            report(0, "dri_render_allowed", detail);
        }
    }

    /* 6. evdev input nodes must be readable for the built-in controller. */
    {
        int fd = open_first_matching("/dev/input", "event", O_RDONLY);
        if (fd >= 0) {
            close(fd);
            report(1, "input_allowed", "evdev event node opened");
        } else if (errno == ENOENT) {
            report_skip("input_allowed", "no /dev/input/event* node");
        } else {
            snprintf(detail, sizeof(detail), "open failed: %s", strerror(errno));
            report(0, "input_allowed", detail);
        }
    }

    /* 7. ALSA PCM nodes must stay readable/writable for audio. */
    {
        int fd = open_first_matching("/dev/snd", "pcm", O_RDWR);
        if (fd >= 0) {
            close(fd);
            report(1, "audio_allowed", "ALSA PCM node opened");
        } else if (errno == ENOENT) {
            report_skip("audio_allowed", "no /dev/snd/pcm* node");
        } else {
            snprintf(detail, sizeof(detail), "open failed: %s", strerror(errno));
            report(0, "audio_allowed", detail);
        }
    }

    /* 8. The trusted control socket must be unreachable from a game. */
    {
        int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (fd < 0) {
            report(0, "control_sock_denied", "socket() failed");
        } else {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, PLAYOS_SOCK_CONTROL,
                    sizeof(addr.sun_path) - 1);

            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(fd);
                report(0, "control_sock_denied",
                       "connected — game reached the trusted control socket");
            } else {
                snprintf(detail, sizeof(detail), "connect failed: %s",
                         strerror(errno));
                close(fd);
                report(1, "control_sock_denied", detail);
            }
        }
    }

    /* 9. mount must be denied (no CAP_SYS_ADMIN; seccomp returns EPERM). */
    {
        long rc = syscall(SYS_mount, "none", "/tmp", "tmpfs", 0, NULL);
        if (rc == 0) {
            report(0, "mount_denied", "mount() succeeded — sandbox broken");
        } else {
            snprintf(detail, sizeof(detail), "mount() failed: %s",
                     strerror(errno));
            report(1, "mount_denied", detail);
        }
    }

    /* 10. /data/config must be outside the Landlock allowlist. */
    {
        int fd = open("/data/config", O_RDONLY | O_DIRECTORY);
        if (fd >= 0) {
            close(fd);
            report(0, "config_denied", "opened — game read /data/config");
        } else {
            snprintf(detail, sizeof(detail), "open failed: %s", strerror(errno));
            report(1, "config_denied", detail);
        }
    }

    /* 11. Another game's saves dir must be unreadable. ENOENT is acceptable
     * (the directory doesn't exist), but a successful open is a FAIL. */
    {
        int fd = open("/data/saves/another-game", O_RDONLY | O_DIRECTORY);
        if (fd >= 0) {
            close(fd);
            report(0, "other_saves_denied",
                   "opened — game read another title's saves");
        } else {
            snprintf(detail, sizeof(detail), "open failed: %s", strerror(errno));
            report(1, "other_saves_denied", detail);
        }
    }

    fprintf(stderr, "[SUMMARY] %d checks, %d failures\n",
            checks_run, checks_fail);
    return checks_fail == 0 ? 0 : 1;
}

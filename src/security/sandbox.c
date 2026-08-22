/*
 * playos-init/src/security/sandbox.c — S12-T1 privilege reduction
 *
 * Runs in the game child process, before exec:
 *   1. PR_SET_NO_NEW_PRIVS
 *   2. clear all capabilities
 *   3. clear supplementary groups
 *   4. setgid + setuid to the unprivileged game identity
 *
 * C99, static-PID-1 safe (no NSS, no getpwnam — uid/gid are hardcoded
 * constants shared with playos-refdistro's users-table.txt).
 */
#define _GNU_SOURCE
#include "playos-init/security.h"

#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/capability.h>
#endif

int
playos_security_disable_priv_escalation(void)
{
    /* No setuid/setgid/file-cap gain for this process or anything it
     * execs. Must run first — later steps rely on it. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
        return -1;
    return 0;
}

static int
drop_capabilities(void)
{
#ifdef __linux__
    /* Drop every capability from the effective, permitted, and
     * inheritable sets. A process is always allowed to drop its own
     * capabilities, so capset() with empty sets succeeds even without
     * CAP_SETPCAP. */
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data[2];

    hdr.version = _LINUX_CAPABILITY_VERSION_3;
    hdr.pid     = 0;

    data[0].effective   = 0;
    data[0].permitted   = 0;
    data[0].inheritable = 0;
    data[1].effective   = 0;
    data[1].permitted   = 0;
    data[1].inheritable = 0;

    if (syscall(SYS_capset, &hdr, data) != 0)
        return -1;
#endif
    return 0;
}

int
playos_security_drop_privileges(void)
{
    /* Capabilities first: after setuid to a non-root uid they are
     * cleared automatically on exec, but dropping them here makes the
     * post-exec CapEff check meaningful and covers the no-exec window. */
    if (drop_capabilities() != 0)
        return -1;

    /* No supplementary groups — games are not in audio/video/input/drm. */
    if (setgroups(0, NULL) != 0)
        return -1;

    if (setgid(PLAYOS_GAME_GID) != 0)
        return -1;

    if (setuid(PLAYOS_GAME_UID) != 0)
        return -1;

    /* Defense-in-depth: verify the kernel did what we asked. */
    if (getuid() != PLAYOS_GAME_UID || getgid() != PLAYOS_GAME_GID)
        return -1;

    return 0;
}

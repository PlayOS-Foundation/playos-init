/*
 * playos-init/src/child_process.c — Child process utilities
 *
 * Helper functions for setting up child process environment and
 * spawning external processes.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

#include "playos-init/init.h"

/* ── External logging ────────────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Child process spawning ──────────────────────────────────────── */

/*
 * Fork and exec a process. Returns child PID, or -1 on error.
 * The child process is spawned with:
 *   - Its own session (setsid)
 *   - stderr redirected to the system log
 */
pid_t playos_spawn_child(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        /* Child */
        setsid();

        /* Redirect stderr to /dev/kmsg so output appears in dmesg */
        int kmsg = open("/dev/kmsg", O_WRONLY);
        if (kmsg >= 0) {
            dup2(kmsg, STDERR_FILENO);
            close(kmsg);
        }

        execv(path, argv);

        /* If we reach here, exec failed */
        dprintf(STDERR_FILENO, "playos-init: exec %s failed: %s\n",
                path, strerror(errno));
        _exit(127);
    }

    return pid;
}

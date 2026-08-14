/*
 * playos-init/src/shutdown.c — Orderly system shutdown
 *
 * Shutdown sequence:
 *   1. Notify active game via lifecycle fd (if any)
 *   2. Wait for game to exit (with timeout)
 *   3. SIGKILL game if still running
 *   4. SIGTERM compositor
 *   5. Sync filesystems
 *   6. Call reboot()
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/reboot.h>

#include "playos-init/init.h"
#include "playos-init/shutdown.h"

/* ── External logging ────────────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Shutdown implementation ─────────────────────────────────────── */

void playos_shutdown(struct playos_init_state *s, int restart)
{
    playos_log_write(s, "shutdown", "initiating %s",
                     restart ? "reboot" : "shutdown");

    /* Step 1: Terminate active game */
    if (s->game_pid > 0 && s->game_state != GAME_NONE) {
        playos_log_write(s, "shutdown", "terminating game %s PID %d",
                         s->game_id, s->game_pid);

        /* Send SIGTERM */
        kill(s->game_pid, SIGTERM);

        /* Wait up to 2 seconds for graceful exit */
        int waited = 0;
        while (waited < 20) { /* 20 × 100ms = 2s */
            usleep(100000);
            waited++;

            /* Check if game is still alive */
            if (kill(s->game_pid, 0) != 0) {
                /* Game exited */
                break;
            }
        }

        /* Force kill if still running */
        if (kill(s->game_pid, 0) == 0) {
            playos_log_write(s, "shutdown", "force-killing game %s",
                             s->game_id);
            kill(s->game_pid, SIGKILL);
            usleep(500000); /* 500ms for cleanup */
        }

        s->game_pid = 0;
        s->game_state = GAME_NONE;
    }

    /* Step 2: Terminate compositor */
    if (s->compositor_pid > 0) {
        playos_log_write(s, "shutdown", "stopping compositor PID %d",
                         s->compositor_pid);

        kill(s->compositor_pid, SIGTERM);

        /* Wait up to 2 seconds */
        int waited = 0;
        while (waited < 20) {
            usleep(100000);
            waited++;
            if (kill(s->compositor_pid, 0) != 0)
                break;
        }

        if (kill(s->compositor_pid, 0) == 0) {
            kill(s->compositor_pid, SIGKILL);
        }

        s->compositor_pid = 0;
    }

    /* Step 3: Sync all filesystems */
    playos_log_write(s, "shutdown", "syncing filesystems");
    sync();
    sleep(1);

    /* Step 4: Final reboot/halt */
    playos_log_write(s, "shutdown", "calling reboot(%s)",
                     restart ? "RB_AUTOBOOT" : "RB_POWER_OFF");
    sync();

    if (restart) {
        reboot(RB_AUTOBOOT);
    } else {
        reboot(RB_POWER_OFF);
    }

    /* If reboot fails, halt as fallback */
    reboot(RB_HALT_SYSTEM);

    /* Should never reach here */
    for (;;) pause();
}

/*
 * playos-init/src/main.c — PID 1 entry point
 *
 * Boot sequence:
 *   1. Init state struct
 *   2. Mount virtual filesystems
 *   3. Initialize logging
 *   4. Discover and mount data partition
 *   5. Set up IPC sockets
 *   6. Spawn and supervise compositor
 *   7. Enter main event loop
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
#include "playos-init/mount.h"
#include "playos-init/supervisor.h"
#include "playos-init/ipc_handler.h"

/* ── Global state ────────────────────────────────────────────────── */

struct playos_init_state g_state;

/* ── Logging helpers (declared in logging.c) ─────────────────────── */

void playos_log_init(struct playos_init_state *s);
void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Boot banner ─────────────────────────────────────────────────── */

static void print_banner(void)
{
    /* Write directly to console before logging is set up */
    dprintf(STDERR_FILENO,
        "\n"
        "  ╔══════════════════════════════════════════════════╗\n"
        "  ║              PlayOS — Sprint 6                   ║\n"
        "  ║      playos-init PID 1 Boot Supervisor           ║\n"
        "  ╚══════════════════════════════════════════════════╝\n"
        "\n");
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void)
{
    struct playos_init_state *s = &g_state;

    /* PID 1 must not exit normally */
    playos_init_state_init(s);

    print_banner();

    /* Stage 1: Mount virtual filesystems */
    playos_boot_stage_write(BOOT_STAGE_MOUNTS);
    if (playos_mount_virtual() != 0) {
        dprintf(STDERR_FILENO, "playos-init: FATAL: virtual mount failed\n");
        /* Halt — we can't function without /proc and /sys */
        sync();
        reboot(RB_HALT_SYSTEM);
    }

    /* Initialize logging now that /run is available */
    playos_log_init(s);
    playos_log_write(s, "init", "playos-init starting as PID %d", getpid());

    /* Set up SIGCHLD handler for zombie reaping */
    playos_supervisor_init_signal_handler();

    /* Stage 2: Discover and mount data partition */
    playos_boot_stage_write(BOOT_STAGE_DATA_DISCOVERY);
    if (playos_mount_data(s) != 0) {
        playos_log_write(s, "init", "WARN: data partition not found — provisioning halt");
        playos_enter_recovery(s, "data partition not found");
    } else {
        playos_boot_stage_write(BOOT_STAGE_DATA_MOUNTED);
        if (playos_data_create_dirs() != 0) {
            playos_log_write(s, "init",
                    "ERROR: /data provisioning failed — halting");
            playos_enter_recovery(s, "data provisioning failed");
        }
        /* /data/log now exists — persist the boot trace to the USB. */
        playos_log_open_persistent(s);
        playos_log_write(s, "init", "persistent log opened on /data/log/init.log");
    }

    /* Stage 3: Set up IPC sockets */
    playos_boot_stage_write(BOOT_STAGE_IPC_READY);
    playos_ipc_server_start(s);
    playos_log_write(s, "init", "IPC server started on /run/playos/control.sock");
    playos_compositor_server_start(s);
    playos_log_write(s, "init",
                     "compositor control server started on /run/playos/compositor.sock");

    /* Stage 4: Spawn compositor */
    playos_boot_stage_write(BOOT_STAGE_COMPOSITOR);
    if (playos_supervisor_spawn_compositor(s) != 0) {
        playos_log_write(s, "init", "WARN: compositor spawn failed");
    } else {
        /* Compositor is running — launch visual test client */
        usleep(500000); /* 500ms grace period for compositor to fully init */
        playos_supervisor_spawn_shell(s);
        playos_supervisor_spawn_overlay(s);
    }

    /* Stage 5: System ready */
    playos_boot_stage_write(BOOT_STAGE_READY);
    playos_log_write(s, "init", "system ready — entering supervision loop");
    dprintf(STDERR_FILENO, "\n  PlayOS Sprint 6 — playos-shell on wlroots DRM/KMS\n");
    dprintf(STDERR_FILENO, "  System ready.\n\n");

    /* Main supervision loop */
    for (;;) {
        static int first_loop = 1;

        if (first_loop) {
            first_loop = 0;

            /* Sprint 2: Wait for compositor readiness before running tests */
            if (s->compositor_state != COMPOSITOR_RUNNING) {
                dprintf(STDERR_FILENO, "playos-init: waiting for compositor...\n");
                /* Compositor not ready yet, skip tests this round */
            } else {
                /* Auto-run IPC integration tests (Sprint 1) */
                if (access("/usr/bin/ipc-test-client", X_OK) == 0) {
                    pid_t test_pid = fork();
                    if (test_pid == 0) {
                        dprintf(STDERR_FILENO, "\n=== Sprint 1 Integration Tests ===\n");
                        execl("/usr/bin/ipc-test-client", "ipc-test-client", "--verbose", NULL);
                        _exit(127);
                    } else if (test_pid > 0) {
                        playos_log_write(s, "test", "spawned IPC test runner PID %d", test_pid);
                    }
                }
            }
        }

        /* Process incoming IPC connections */
        playos_ipc_server_poll(s);
        playos_compositor_server_poll(s);

        /* Reap any zombie children */
        playos_supervisor_reap_children(s);

        /* Non-cooperative game SIGSTOP fallback (Sprint 7) */
        playos_supervisor_lifecycle_tick(s);

        /*
         * Check recovery flag set by IPC handler or supervisor
         */
        if (s->recovery_mode) {
            playos_enter_recovery(s, "shutdown requested via IPC");
        }

        /*
         * Sleep briefly to avoid busy-waiting.
         * SIGCHLD or IPC activity will wake us.
         */
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }

    /* Unreachable — PID 1 never returns */
    return 0;
}

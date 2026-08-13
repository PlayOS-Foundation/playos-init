/*
 * playos-init/src/recovery.c — Recovery mode
 *
 * When the system enters an unrecoverable state, display a diagnostic
 * and halt. Sprint 1 provides a simple text-based recovery display.
 * Post-Sprint 2 may add a framebuffer-based recovery UI.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/reboot.h>

#include "playos-init/init.h"
#include "playos-init/recovery.h"

/* ── External logging ────────────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Recovery entry ──────────────────────────────────────────────── */

void playos_recovery_enter(struct playos_init_state *state, const char *reason)
{
    state->recovery_mode = 1;
    state->boot_stage = BOOT_STAGE_RECOVERY;

    playos_log_write(state, "recovery", "entering recovery: %s", reason);
}

/* ── Recovery loop ───────────────────────────────────────────────── */

void playos_recovery_loop(struct playos_init_state *state)
{
    playos_log_write(state, "recovery",
                     "recovery mode active — system halted");

    /* Kill all other processes */
    kill(-1, SIGTERM);
    sleep(1);
    kill(-1, SIGKILL);

    /* Print diagnostic banner */
    dprintf(STDERR_FILENO,
        "\n"
        "  ╔══════════════════════════════════════════════════╗\n"
        "  ║           PLAYOS RECOVERY MODE                   ║\n"
        "  ╠══════════════════════════════════════════════════╣\n"
        "  ║  The system has entered recovery mode.           ║\n"
        "  ║  This typically indicates a critical component   ║\n"
        "  ║  failure (compositor, storage, etc.).            ║\n"
        "  ╠══════════════════════════════════════════════════╣\n"
        "  ║  Cause: %-40s  ║\n"
        "  ╠══════════════════════════════════════════════════╣\n"
        "  ║  Actions:                                        ║\n"
        "  ║    - Reboot to retry                             ║\n"
        "  ║    - Check /run/playos/log/init.log              ║\n"
        "  ║    - Boot from alternate slot if available       ║\n"
        "  ╚══════════════════════════════════════════════════╝\n"
        "\n",
        "compositor restart limit exceeded");

    /* Halt the system */
    sync();
    reboot(RB_HALT_SYSTEM);

    /* In case reboot fails, spin forever */
    for (;;) {
        pause();
    }
}

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
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>

#include "playos-init/init.h"
#include "playos-init/mount.h"
#include "playos-init/boot_slot.h"
#include "playos-init/supervisor.h"
#include "playos-init/ipc_handler.h"
#include "playos-init/thermal.h"

/* ── Global state ────────────────────────────────────────────────── */

struct playos_init_state g_state;

/* ── Logging helpers (declared in logging.c) ─────────────────────── */

void playos_log_init(struct playos_init_state *s);
void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void playos_audio_debug_dump(void);
void playos_audio_debug_dump_late(void);

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

    /* Sprint 10: detect installer boot before data-mount policy decisions. */
    s->install_mode = playos_install_mode_requested();
    if (s->install_mode)
        playos_log_write(s, "init", "installer mode: playos.mode=install present");

    /* Set up SIGCHLD handler for zombie reaping */
    playos_supervisor_init_signal_handler();

    /* Sprint 11: mount the EFI System Partition (rw) for A/B boot slot
     * accounting. Best-effort — a missing or unmountable ESP never
     * hard-fails boot; we simply skip slot accounting.
     *
     * Installer mode skips this entirely: the target internal disk may
     * already carry an "ESP" label and we are about to repartition it, so
     * mounting its old ESP here would make the later fdisk/mkfs steps fail
     * busy. The installer discovers its payload from playos-a directly and
     * does not use /EFI. */
    if (!s->install_mode) {
        /* Block partition enumeration can lag devtmpfs node creation: the
         * kernel's /proc/partitions may already list the ESP while its
         * /dev/<name><N> node (or the vfat mount) is not ready yet, which
         * surfaces as mount() returning ENODEV. Retry find+mount on a short
         * cooldown so late ESP registration is tolerated. This stays
         * best-effort and never hard-fails boot. */
        char esp_dev[128] = {0};
        for (int attempt = 0; attempt < 10; attempt++) {
            if (attempt > 0)
                usleep(attempt * 100000); /* 100ms, 200ms, ... up to 900ms */

            if (playos_find_partition_by_label("ESP", esp_dev,
                                               sizeof(esp_dev)) != 0)
                continue;

            mkdir("/EFI", 0755);
            if (mount(esp_dev, "/EFI", "vfat", 0, NULL) == 0) {
                s->efi_mounted = 1;
                mkdir("/EFI/playos", 0755);
                playos_log_write(s, "init",
                                 "ESP mounted at /EFI (device %s)", esp_dev);
                break;
            }
            playos_log_write(s, "init",
                             "WARN: ESP mount attempt %d failed: %s",
                             attempt + 1, strerror(errno));
        }

        if (!s->efi_mounted) {
            if (esp_dev[0] != '\0') {
                playos_log_write(s, "init",
                                 "WARN: failed to mount ESP at /EFI after "
                                 "retries: %s", strerror(errno));
            } else {
                dprintf(STDERR_FILENO,
                        "playos-init: WARN: no ESP partition found — "
                        "skipping A/B boot slot accounting\n");
            }
        }

        if (s->efi_mounted) {
            struct boot_slot_state bs;
            if (boot_slot_increment(PLAYOS_BOOT_JSON_PATH, &bs)) {
                playos_log_write(s, "init",
                                 "boot slot %c failed too many times — "
                                 "rolling back", bs.active_slot);
                boot_slot_rollback(PLAYOS_BOOT_JSON_PATH, &bs);
                sync();
                reboot(RB_AUTOBOOT);
            }
        }
    } else {
        playos_log_write(s, "init",
                         "installer mode: skipping ESP mount (target disk may "
                         "already carry an ESP label)");
    }

    /* Sprint 11.5: hand control to the real read-only rootfs slot when the
     * active slot is a raw squashfs. Success never returns (exec /init);
     * failure falls through to the legacy initramfs boot path. */
    playos_pivot_to_active_slot(s);

    /* Stage 2: Discover and mount data partition */
    playos_boot_stage_write(BOOT_STAGE_DATA_DISCOVERY);
    if (playos_mount_data(s) != 0) {
        if (s->install_mode) {
            /* Installer runs entirely from the removable boot medium; the
             * target disk's data partition does not exist yet, so /data is
             * optional here. */
            playos_log_write(s, "init",
                             "installer mode: data partition optional — continuing");
        } else {
            playos_log_write(s, "init", "WARN: data partition not found — provisioning halt");
            playos_enter_recovery(s, "data partition not found");
        }
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

        /* Snapshot the ALSA topology + kernel audio log for on-device
         * diagnostics (best-effort, never fatal). */
        playos_audio_debug_dump();
        playos_log_write(s, "init", "audio diagnostics written to /data/log/audio-debug.log");

        /* Developer SSH (Sprint 11.6): only on the installed OS, never
         * during installer repartitioning (avoids holding /data/ssh
         * busy), and only when the dev-image bring-up script is actually
         * shipped (Sprint 12 production images exclude it). */
        if (!s->install_mode &&
            access("/usr/bin/playos-ssh-bringup", X_OK) == 0)
            playos_supervisor_spawn_ssh(s);
    }

    /* Stage 3: Set up IPC sockets */
    playos_boot_stage_write(BOOT_STAGE_IPC_READY);
    playos_ipc_server_start(s);
    playos_log_write(s, "init", "IPC server started on /run/playos/control.sock");
    playos_compositor_server_start(s);
    playos_log_write(s, "init",
                     "compositor control server started on /run/playos/compositor.sock");

    /* Stage 3b: Thermal thresholds + EPP profile sync (Sprint 9) */
    playos_thermal_init(s);

    /* Stage 4: Spawn compositor */
    playos_boot_stage_write(BOOT_STAGE_COMPOSITOR);
    if (playos_supervisor_spawn_compositor(s) != 0) {
        playos_log_write(s, "init", "WARN: compositor spawn failed");
    } else {
        /* Compositor is running — launch the appropriate Wayland client.
         * In installer mode the installer takes the shell role (Sprint 10);
         * otherwise the shell plus the trusted in-game overlay start. */
        usleep(500000); /* 500ms grace period for compositor to fully init */
        if (s->install_mode) {
            playos_supervisor_spawn_installer(s);
        } else {
            playos_supervisor_spawn_shell(s);
            playos_supervisor_spawn_overlay(s);
        }
    }

    /* Stage 5: System ready */
    playos_boot_stage_write(BOOT_STAGE_READY);
    playos_log_write(s, "init", "system ready — entering supervision loop");
    if (s->install_mode) {
        dprintf(STDERR_FILENO, "\n  PlayOS Sprint 10 — playos-installer on wlroots DRM/KMS\n");
    } else {
        dprintf(STDERR_FILENO, "\n  PlayOS Sprint 6 — playos-shell on wlroots DRM/KMS\n");
    }
    dprintf(STDERR_FILENO, "  System ready.\n\n");

    /* Main supervision loop */
    for (;;) {
        static int first_loop = 1;

        if (first_loop) {
            first_loop = 0;

#ifdef PLAYOS_ENABLE_IPC_TESTS
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
#endif /* PLAYOS_ENABLE_IPC_TESTS */
        }

        /* Process incoming IPC connections */
        playos_ipc_server_poll(s);
        playos_compositor_server_poll(s);

        /* Reap any zombie children */
        playos_supervisor_reap_children(s);

        /* Non-cooperative game SIGSTOP fallback (Sprint 7) */
        playos_supervisor_lifecycle_tick(s);

        /* 1 Hz thermal monitor + EPP profile enforcement (Sprint 9) */
        playos_thermal_tick(s);

        /* One-shot late audio snapshot ~5s into the loop. The Realtek/
         * CS35L41 speaker card (card 1) registers a few seconds after boot,
         * after the early snapshot in Stage 2; this append-only re-dump makes
         * the ALSA topology section actually include the speaker card. */
        static int late_audio_ticks = 0;
        if (late_audio_ticks < 5) {
            late_audio_ticks++;
            if (late_audio_ticks == 5)
                playos_audio_debug_dump_late();
        }

        /* Sprint 11: fallback healthy-boot gate ~60s into the loop. The
         * ShellReady path marks the slot good as soon as the shell registers;
         * this backstop covers a shell that never connects. Idempotent. */
        static int boot_good_ticks = 0;
        if (boot_good_ticks < 60) {
            boot_good_ticks++;
            if (boot_good_ticks == 60)
                playos_boot_mark_good_once(s);
        }

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

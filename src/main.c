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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <poll.h>

#include "playos-init/init.h"
#include "playos-init/mount.h"
#include "playos-init/boot_media.h"
#include "playos-init/boot_slot.h"
#include "playos-init/supervisor.h"
#include "playos-init/ipc_handler.h"
#include "playos-init/thermal.h"
#include "playos-init/recovery.h"

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
        "  ║                      PlayOS                      ║\n"
        "  ║      playos-init PID 1 Boot Supervisor           ║\n"
        "  ╚══════════════════════════════════════════════════╝\n"
        "\n");
}

/* ── Main ────────────────────────────────────────────────────────── */

/* Monotonic milliseconds. The housekeeping ticks below are time-based so an
 * early wake-up on IPC activity cannot fast-forward them. */
static long long
playos_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wait for IPC activity or the next 1 Hz tick, whichever comes first.
 *
 * This replaced an unconditional 1 s nanosleep(). The IPC sockets are polled
 * non-blocking (poll(...,0)) and only serviced once per loop iteration, so a
 * fixed sleep delayed every control request by up to a second — most visibly
 * the in-game COMMAND -> pause-overlay path (shell -> init -> compositor).
 * Waiting on the fds makes init react in milliseconds while still ticking at
 * 1 Hz. SIGCHLD interrupts poll() just as it interrupted nanosleep(). */
static void
playos_wait_for_ipc(struct playos_init_state *s)
{
    struct pollfd fds[3];
    nfds_t n = 0;

    if (s->control_sock_fd >= 0) {
        fds[n].fd = s->control_sock_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    if (s->compositor_sock_fd >= 0) {
        fds[n].fd = s->compositor_sock_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }
    if (s->shell_listener_fd >= 0) {
        fds[n].fd = s->shell_listener_fd;
        fds[n].events = POLLIN;
        fds[n].revents = 0;
        n++;
    }

    if (n == 0) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        return;
    }

    (void)poll(fds, n, 1000);
}

/* S14 P1: boot-time pacing helper. Connect-probe the compositor's Wayland
 * socket so the shell starts the moment the compositor can talk to it. */
static void
playos_wait_for_wayland_socket(struct playos_init_state *s, int timeout_ms)
{
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/playos-0",
             getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/run/playos");

    for (int waited = 0; waited < timeout_ms; waited += 20) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
                close(fd);
                playos_log_write(s, "init",
                                 "compositor Wayland socket ready after %d ms",
                                 waited);
                return;
            }
            close(fd);
        }
        usleep(20000);
    }

    playos_log_write(s, "init",
                     "WARN: Wayland socket not ready after %d ms - starting the "
                     "shell anyway", timeout_ms);
}

int main(void)
{
    struct playos_init_state *s = &g_state;

    /* PID 1 must not exit normally */
    playos_init_state_init(s);

    print_banner();

    /* S14-P1: first marker. Everything from here to the pivot used to be
     * invisible (the persistent log needs /data, which comes later). */
    playos_boot_mark("init start (pid=%d)", getpid());

    /* Stage 1: Mount virtual filesystems */
    playos_boot_stage_write(BOOT_STAGE_MOUNTS);
    if (playos_mount_virtual() != 0) {
        dprintf(STDERR_FILENO, "playos-init: FATAL: virtual mount failed\n");
        /* Halt — we can't function without /proc and /sys */
        sync();
        reboot(RB_HALT_SYSTEM);
    }

    playos_boot_mark("virtual filesystems mounted");

    /* Initialize logging now that /run is available */
    playos_log_init(s);
    playos_boot_mark("persistent log initialised");
    playos_log_write(s, "init", "playos-init starting as PID %d", getpid());

    /* Kernel-cmdline decisions (S14-T6). These read /proc/cmdline, so they must
     * run *after* playos_mount_virtual() — this used to run before /proc
     * existed, which silently made `playos.recovery` a no-op on every boot
     * (the button-hold path uses evdev, so it kept working). Log the cmdline
     * itself too: every boot decision lives there. */
    {
        FILE *cl = fopen("/proc/cmdline", "r");
        if (cl) {
            char line[512] = {0};
            if (fgets(line, sizeof(line) - 1, cl)) {
                line[strcspn(line, "\n")] = '\0';
                playos_log_write(s, "init", "cmdline: %s", line);
            }
            fclose(cl);
        }
    }
    if (playos_recovery_requested()) {
        s->recovery_mode = 1;
        playos_log_write(s, "init",
                         "recovery requested via cmdline (playos.recovery)");
    }

    /* Sprint 12: start udevd and settle the device queue so /dev nodes get
     * their final ownership/group/mode (render, audio, input, ...) before
     * the compositor and games start. Best-effort, never hard-fails boot. */
    playos_boot_mark("udev start");
    playos_udev_start(s);
    playos_boot_mark("udev done");

    /* S14-T6: recovery via holding Volume Down for 5 seconds at boot.
     * S14-P1: marked because this check runs before anything else can be
     * timed, and a stall here would be indistinguishable from udev. */
    playos_boot_mark("recovery button check start");
    if (getenv("PLAYOS_RECOVERY")) {
        /* Decided by the first init before the pivot: skip the (773 ms) evdev
         * check. Absent on images whose embedded initramfs predates the
         * hand-over, in which case the check below still runs. */
        s->recovery_mode = 1;
        playos_boot_mark("recovery inherited from the first init (env)");
    } else if (playos_recovery_button_held()) {
        s->recovery_mode = 1;
        playos_log_write(s, "init",
                         "recovery requested via button hold (volume down)");
    }

    playos_boot_mark("recovery button check done (mode=%d)", s->recovery_mode);

    /* Sprint 10: detect installer boot before data-mount policy decisions. */
    s->install_mode = playos_install_mode_requested();
    if (s->install_mode)
        playos_log_write(s, "init", "installer mode: playos.mode=install present");

    /* Are we the exec'd init already running inside the read-only squashfs
     * slot? The first init (initramfs) counts the boot and records boot.json
     * before pivoting; the second init must not re-count, but still mounts
     * the ESP so mark-good can persist boot.json later. */
    int already_pivoted = playos_root_is_squashfs();

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
        playos_boot_mark("ESP discovery start");
        for (int attempt = 0; attempt < 40; attempt++) {
            /* S14 P1: poll fast (25 ms) instead of backing off (100, 200,
             * 300... ms). A boot whose ESP node appears a moment late was
             * paying ~600 ms in sleeps alone; 40 polls keep the same tolerance
             * (1 s) but cost only the time actually needed. */
            if (attempt > 0)
                usleep(25000);

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

        playos_boot_mark("ESP stage done (mounted=%d dev=%s)", s->efi_mounted, esp_dev);
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

        /* Boot counting happens exactly once, in the first (initramfs) init.
         * The exec'd second init re-mounts the ESP above but must not advance
         * the counter again — doing so would double-count every boot and
         * falsely trigger 3-strike rollback. */
        /* A live-USB (or installer) boot must not touch the *installed*
         * system's A/B counters: init mounts whatever ESP the name lookup
         * finds first, which after an install can be the internal NVMe's even
         * when the firmware booted the USB. Counting those boots would let
         * repeated live sessions trip the 3-strike rollback of a healthy
         * installed slot. */
        if (s->efi_mounted && !already_pivoted &&
            playos_booted_from_usb() != 1) {
            struct boot_slot_state bs;
            if (boot_slot_increment(PLAYOS_BOOT_JSON_PATH, &bs)) {
                playos_log_write(s, "init",
                                 "boot slot %c failed too many times — "
                                 "rolling back", bs.active_slot);
                boot_slot_rollback(PLAYOS_BOOT_JSON_PATH, &bs);
                sync();
                reboot(RB_AUTOBOOT);
            }

            /* S11.5-T5 Case 1: a clean install has no boot.json (the file is
             * only ever created by the update path). Materialize a default
             * slot-A-good state on the first boot so a fresh install's slot
             * accounting is recorded from the start, matching the Sprint 11
             * spec ("missing boot.json → slot A good"). Best-effort — never
             * hard-fail boot over a boot.json write. */
            if (access(PLAYOS_BOOT_JSON_PATH, F_OK) != 0) {
                struct boot_slot_state fresh;
                /* boot_slot_read fills `fresh` with the safe default even
                 * when the file is missing. */
                boot_slot_read(PLAYOS_BOOT_JSON_PATH, &fresh);
                if (boot_slot_write(PLAYOS_BOOT_JSON_PATH, &fresh) != 0) {
                    playos_log_write(s, "init",
                                     "WARN: failed to create boot.json: %s",
                                     strerror(errno));
                } else {
                    playos_log_write(s, "init",
                                     "created default boot.json (slot A good)");
                }
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
    playos_boot_mark("pivot start");
    /* Only reached on failure: a successful pivot execve()s a new init. */
    playos_boot_mark("pivot returned %d (0/1 = stayed in initramfs)",
                     playos_pivot_to_active_slot(s));

    /* Stage 2: Discover and mount data partition */
    playos_boot_stage_write(BOOT_STAGE_DATA_DISCOVERY);
    playos_boot_mark("/data mount start");
    if (playos_mount_data(s) != 0) {
        if (s->install_mode) {
            /* Installer runs entirely from the removable boot medium; the
             * target disk's data partition does not exist yet, so /data is
             * optional here. */
            playos_log_write(s, "init",
                             "installer mode: data partition optional — continuing");
        } else {
            playos_log_write(s, "init", "WARN: data partition not found — recovery UI");
            s->recovery_mode = 1;
        }
    } else {
        playos_boot_stage_write(BOOT_STAGE_DATA_MOUNTED);
        if (playos_data_create_dirs() != 0) {
            playos_log_write(s, "init",
                    "ERROR: /data provisioning failed — recovery UI");
            s->recovery_mode = 1;
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
    playos_boot_mark("/data mount done");
    playos_thermal_init(s);

    /* Sprint 16 / T5: bring up Wi-Fi once /data exists — the profiles live
     * there. No-op on a live/install/recovery boot or without a radio. */
    playos_supervisor_start_network(s);

    /* S14-T6: late recovery detection is handled *non-blocking* in the
     * supervision loop below (see the recovery watch there). The old code
     * blocked here for 4s and printed a console prompt on every boot, which
     * broke the <5s cold-boot target and leaked raw console text; the loop
     * now polls the instant held-check instead, so a normal boot never waits.
     * `s->recovery_mode` may already be set here by the cmdline, the early
     * button check, or a missing /data partition. */

    /* Stage 4: Spawn compositor */
    playos_boot_stage_write(BOOT_STAGE_COMPOSITOR);
    playos_boot_mark("spawning compositor");
    if (playos_supervisor_spawn_compositor(s) != 0) {
        playos_log_write(s, "init", "WARN: compositor spawn failed");
    } else {
        /* Compositor is running — launch the appropriate Wayland client.
         * In installer mode the installer takes the shell role (Sprint 10);
         * otherwise the shell plus the trusted in-game overlay start. */
        /* S14 P1: wait for the compositor's Wayland socket to *accept*
         * connections rather than sleeping a flat 500 ms. The socket is the real
         * precondition and is normally ready within a few ms; the fixed grace
         * period was pure added boot latency. */
        playos_boot_mark("compositor ready; waiting for its Wayland socket");
        playos_wait_for_wayland_socket(s, 2000);
        if (s->install_mode) {
            playos_supervisor_spawn_installer(s);
        } else {
            playos_boot_mark("spawning shell");
            playos_supervisor_spawn_shell(s);
            if (!s->recovery_mode)
                playos_supervisor_spawn_overlay(s);
        }
    }

    /* Stage 5: System ready */
    playos_boot_stage_write(BOOT_STAGE_READY);
    playos_boot_mark("system ready");
    playos_boot_marks_persist(s);
    playos_log_write(s, "init", "system ready — entering supervision loop");
    if (s->install_mode) {
        dprintf(STDERR_FILENO, "\n  PlayOS — playos-installer on wlroots DRM/KMS\n");
    } else {
        dprintf(STDERR_FILENO, "\n  PlayOS — playos-shell on wlroots DRM/KMS\n");
    }
    dprintf(STDERR_FILENO, "  System ready.\n\n");

    /* S14-T6: non-blocking late recovery watch.
     *
     * The ROG Ally internal controller can enumerate after the early check
     * above, so re-check once devices are settled — but without delaying a
     * normal boot. Poll the instant held-check (returns immediately when no
     * trigger is held) once per supervision tick for the first few seconds;
     * on detection the shell is restarted in recovery mode. This keeps the
     * button-hold entry point while leaving the <5s cold-boot path intact and
     * printing nothing to the console unless recovery is actually requested. */
    struct timespec recovery_watch_ts;
    clock_gettime(CLOCK_MONOTONIC, &recovery_watch_ts);
    long long recovery_watch_deadline_ms =
        (long long)recovery_watch_ts.tv_sec * 1000 +
        recovery_watch_ts.tv_nsec / 1000000 + 5000;

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

        /* S13.7: headless runtime installer handoff (QEMU/CI automation).
         * Triggered once the compositor is running; uses the exact same
         * supervisor path as the StartInstaller IPC message. */
        static int auto_install_triggered = 0;
        if (!auto_install_triggered && !s->install_mode &&
            playos_auto_install_requested() &&
            s->compositor_state == COMPOSITOR_RUNNING) {
            auto_install_triggered = 1;
            playos_log_write(s, "init",
                             "auto-install requested — starting runtime installer handoff");
            playos_supervisor_start_runtime_installer(s);
        }

        /* S15-T7: emulator/debug autostart. `playos.autostart=<game-id>` makes
         * init launch the named game through the same path as a LaunchGame IPC
         * request. Wait until the session is actually ready — the compositor
         * control connection and the shell listener both registered — or
         * SetExpectedGame is dropped and the shell never hears GameStarted (the
         * game then renders but the compositor cannot classify it). The SDK
         * emulator profile uses this hook so a device build can be verified
         * without a person driving the shell UI. Any failure is logged and the
         * session continues; production images never set the token. */
        static int autostart_triggered = 0;
        if (!autostart_triggered && !s->install_mode && !s->recovery_mode &&
            s->compositor_state == COMPOSITOR_RUNNING &&
            s->compositor_conn_fd >= 0 && s->shell_listener_fd >= 0) {
            char autostart_id[128];
            if (playos_autostart_game(autostart_id, sizeof(autostart_id)) > 0) {
                autostart_triggered = 1;
                playos_log_write(s, "init",
                                 "autostart requested - launching game %s",
                                 autostart_id);
                if (playos_supervisor_launch_game(s, autostart_id, NULL) <= 0)
                    playos_log_write(s, "init",
                                     "autostart: game %s did not start",
                                     autostart_id);
            }
        }

        /* Process incoming IPC connections */
        playos_ipc_server_poll(s);
        playos_compositor_server_poll(s);

        /* S14-T6: non-blocking late recovery watch (see above). The instant
         * held-check is cheap and returns 0 immediately when nothing is held,
         * so this only costs a syscall per tick and never blocks a normal
         * boot. Detection restarts the shell in recovery mode. */
        if (!s->recovery_mode && !s->install_mode &&
            s->compositor_state == COMPOSITOR_RUNNING) {
            struct timespec now_ts;
            clock_gettime(CLOCK_MONOTONIC, &now_ts);
            long long now_ms = (long long)now_ts.tv_sec * 1000 +
                               now_ts.tv_nsec / 1000000;
            if (now_ms < recovery_watch_deadline_ms &&
                playos_recovery_button_held()) {
                dprintf(STDERR_FILENO,
                        "\n[recovery] DETECTED - entering recovery mode\n");
                playos_supervisor_enter_recovery_ui(s,
                    "button hold (late watch)");
            }
        }

        /* Reap any zombie children */
        playos_supervisor_reap_children(s);

        /* 1 Hz housekeeping. The loop now wakes early on IPC activity, so
         * gate these by time rather than assuming one iteration per second:
         * the thermal monitor walks sysfs and the lifecycle tick drives the
         * non-cooperative game SIGSTOP timer (Sprint 7/9). */
        static long long last_housekeeping_ms = 0;
        long long housekeeping_now_ms = playos_now_ms();
        if (housekeeping_now_ms - last_housekeeping_ms >= 1000) {
            last_housekeeping_ms = housekeeping_now_ms;
            playos_supervisor_lifecycle_tick(s);
            playos_thermal_tick(s);
            /* Idempotent: starts the network stack once the radio's interface
             * exists (mt7921e probes asynchronously, after the first call). */
            playos_supervisor_start_network(s);
        }

        /* Loop start time for the time-based one-shot ticks below. The loop
         * wakes early on IPC activity, so counting iterations is no longer a
         * proxy for elapsed seconds. */
        static long long loop_start_ms = 0;
        if (loop_start_ms == 0)
            loop_start_ms = playos_now_ms();

        /* One-shot late audio snapshot ~5s into the loop. The Realtek/
         * CS35L41 speaker card (card 1) registers a few seconds after boot,
         * after the early snapshot in Stage 2; this append-only re-dump makes
         * the ALSA topology section actually include the speaker card. */
        static int late_audio_done = 0;
        if (!late_audio_done && playos_now_ms() - loop_start_ms >= 5000) {
            late_audio_done = 1;
            playos_audio_debug_dump_late();
        }

        /* Sprint 11: fallback healthy-boot gate ~60s into the loop. The
         * ShellReady path marks the slot good as soon as the shell registers;
         * this backstop covers a shell that never connects. Idempotent. */
        static int boot_good_done = 0;
        if (!boot_good_done && playos_now_ms() - loop_start_ms >= 60000) {
            boot_good_done = 1;
            playos_boot_mark_good_once(s);
        }

        /* Wait for IPC or the next 1 Hz tick — see playos_wait_for_ipc().
         * SIGCHLD still interrupts the wait. */
        playos_wait_for_ipc(s);
    }

    /* Unreachable — PID 1 never returns */
    return 0;
}

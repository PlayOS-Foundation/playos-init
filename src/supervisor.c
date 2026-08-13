/*
 * playos-init/src/supervisor.c — Process supervision
 *
 * Handles compositor spawning, restart policy, game lifecycle,
 * and zombie reaping.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <fcntl.h>

#include "playos-init/init.h"
#include "playos-init/supervisor.h"
#include "playos-init/mount.h"

/* ── External logging ────────────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void playos_log_fatal(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Forward declarations ────────────────────────────────────────── */

static void compositor_restart(struct playos_init_state *s);
static int compositor_should_restart(struct playos_init_state *s);
static void spawn_shell(struct playos_init_state *s);
static void spawn_test_client(struct playos_init_state *s);

/* ── Persistent child logging ────────────────────────────────────── */

/* Redirect the current (child) process's stdout/stderr to a log file
 * on the persistent /data partition. /data/log is created by
 * playos_data_create_dirs() before Stage 4. On failure, output keeps
 * going to the console as before. */
static void child_log_redirect(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
}

/* ── SIGCHLD handler ─────────────────────────────────────────────── */

static volatile sig_atomic_t g_got_sigchld = 0;

static void sigchld_handler(int sig)
{
    (void)sig;
    g_got_sigchld = 1;
}

int playos_supervisor_init_signal_handler(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGCHLD, &sa, NULL) != 0) {
        dprintf(STDERR_FILENO,
                "playos-init: sigaction(SIGCHLD) failed: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

/* ── Zombie reaping ──────────────────────────────────────────────── */

void playos_supervisor_reap_children(struct playos_init_state *s)
{
    if (!g_got_sigchld)
        return;
    g_got_sigchld = 0;

    pid_t pid;
    int wstatus;

    while ((pid = waitpid(-1, &wstatus, WNOHANG)) > 0) {
        if (pid == s->compositor_pid) {
            /* Compositor exited */
            int exit_code = -1;
            int signal_num = 0;

            if (WIFEXITED(wstatus))
                exit_code = WEXITSTATUS(wstatus);
            if (WIFSIGNALED(wstatus))
                signal_num = WTERMSIG(wstatus);

            playos_supervisor_compositor_exited(s, exit_code, signal_num);
        } else if (pid == s->game_pid) {
            /* Game exited */
            int exit_code = -1;
            int signal_num = 0;

            if (WIFEXITED(wstatus))
                exit_code = WEXITSTATUS(wstatus);
            if (WIFSIGNALED(wstatus))
                signal_num = WTERMSIG(wstatus);

            playos_supervisor_game_exited(s, exit_code, signal_num);
        } else if (pid == s->shell_pid) {
            /* Shell exited */
            int exit_code = -1;
            int signal_num = 0;

            if (WIFEXITED(wstatus))
                exit_code = WEXITSTATUS(wstatus);
            if (WIFSIGNALED(wstatus))
                signal_num = WTERMSIG(wstatus);

            playos_supervisor_shell_exited(s, exit_code, signal_num);
        } else {
            /* Unknown child — log and move on */
            playos_log_write(s, "sup", "reaped unknown child PID %d", pid);
        }
    }
}

/* ── Compositor supervision ──────────────────────────────────────── */

int playos_supervisor_spawn_compositor(struct playos_init_state *s)
{
    const char *compositor_path = "/usr/bin/playos-compositor";

    playos_log_write(s, "sup", "spawning compositor: %s", compositor_path);

    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "sup", "fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child: set up Wayland/DRM environment before exec */
        setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);
        setenv("PLAYOS_BACKEND", "drm", 1);

        /* Persist stderr (trace markers, wlr_log) to /data for
         * on-device debugging */
        child_log_redirect("/data/log/compositor-stderr.log");

        /* Child: exec compositor */
        /* For Sprint 1, if the binary doesn't exist, exec a placeholder */
        execl(compositor_path, compositor_path, NULL);

        /* If exec fails, try the placeholder shell script */
        execl("/usr/bin/playos-compositor-placeholder",
              "playos-compositor-placeholder", NULL);

        /* If that also fails, report error and exit */
        dprintf(STDERR_FILENO,
                "playos-init: compositor exec failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    /* Parent */
    s->compositor_pid = pid;
    s->compositor_state = COMPOSITOR_STARTING;

    playos_log_write(s, "sup", "compositor spawned: PID %d, waiting for readiness", pid);

    /* Poll for readiness file: /run/playos/compositor-ready */
    int attempts = 0;
    const int max_attempts = 50; /* 5 seconds total */
    while (attempts < max_attempts) {
        usleep(100000); /* 100ms */
        if (access("/run/playos/compositor-ready", R_OK) == 0) {
            s->compositor_state = COMPOSITOR_RUNNING;
            playos_log_write(s, "sup", "compositor ready (PID %d)", pid);
            return 0;
        }
        attempts++;
    }

    playos_log_write(s, "sup", "WARN: compositor readiness timeout after %d ms",
                     max_attempts * 100);
    s->compositor_state = COMPOSITOR_RUNNING; /* Proceed anyway */

    return 0;
}

void playos_supervisor_compositor_exited(struct playos_init_state *s,
                                          int exit_code, int signal_num)
{
    /* Record the exit */
    s->compositor_state = COMPOSITOR_EXITED;
    s->compositor_restarts.last_exit_code = exit_code;
    s->compositor_restarts.last_signal = signal_num;

    playos_log_write(s, "sup",
                     "compositor PID %d exited: code=%d signal=%d",
                     s->compositor_pid, exit_code, signal_num);

    s->compositor_pid = 0;

    /* Check restart policy */
    if (compositor_should_restart(s)) {
        compositor_restart(s);
    } else {
        playos_log_write(s, "sup",
                         "compositor restart limit exceeded (%d restarts in %ds)",
                         PLAYOS_COMPOSITOR_MAX_RESTARTS,
                         PLAYOS_COMPOSITOR_WINDOW_S);
        playos_enter_recovery(s, "compositor restart limit exceeded");
    }
}

/* ── Restart policy ──────────────────────────────────────────────── */

static int compositor_should_restart(struct playos_init_state *s)
{
    time_t now = time(NULL);
    struct playos_restart_info *r = &s->compositor_restarts;

    /* Reset window if expired */
    if (now - r->window_start > PLAYOS_COMPOSITOR_WINDOW_S) {
        r->count = 0;
        r->window_start = now;
    }

    r->count++;
    return (r->count <= PLAYOS_COMPOSITOR_MAX_RESTARTS);
}

static void compositor_restart(struct playos_init_state *s)
{
    playos_log_write(s, "sup",
                     "restarting compositor in %d ms (attempt %d)",
                     PLAYOS_COMPOSITOR_RESTART_DELAY_MS,
                     s->compositor_restarts.count);

    usleep(PLAYOS_COMPOSITOR_RESTART_DELAY_MS * 1000);

    /* Clear restart counter state for the spawn */
    playos_supervisor_spawn_compositor(s);
}

/* ── Shell restart policy ──────────────────────────────────────────── */

static int shell_should_restart(struct playos_init_state *s)
{
    time_t now = time(NULL);
    struct playos_restart_info *r = &s->shell_restarts;

    /* Reset window if expired */
    if (now - r->window_start > PLAYOS_SHELL_WINDOW_S) {
        r->count = 0;
        r->window_start = now;
    }

    r->count++;
    return (r->count <= PLAYOS_SHELL_MAX_RESTARTS);
}

static void shell_restart(struct playos_init_state *s)
{
    playos_log_write(s, "sup",
                     "restarting shell in %d ms (attempt %d)",
                     PLAYOS_SHELL_RESTART_DELAY_MS,
                     s->shell_restarts.count);

    usleep(PLAYOS_SHELL_RESTART_DELAY_MS * 1000);

    spawn_shell(s);
}

void playos_supervisor_shell_exited(struct playos_init_state *s,
                                     int exit_code, int signal_num)
{
    s->shell_restarts.last_exit_code = exit_code;
    s->shell_restarts.last_signal = signal_num;

    playos_log_write(s, "sup",
                     "shell PID %d exited: code=%d signal=%d",
                     s->shell_pid, exit_code, signal_num);

    s->shell_pid = 0;

    /* Check restart policy */
    if (shell_should_restart(s)) {
        shell_restart(s);
    } else {
        playos_log_write(s, "sup",
                         "shell restart limit exceeded (%d restarts in %ds) — "
                         "leaving compositor running without shell",
                         PLAYOS_SHELL_MAX_RESTARTS,
                         PLAYOS_SHELL_WINDOW_S);
        /* Do NOT enter recovery — the system can still run without the shell.
         * Games can still be launched via IPC, overlay remains available. */
    }
}

/* ── Test client auto-launch ─────────────────────────────────────── */
/* ── Shell auto-launch (Sprint 5) ─────────────────────────────────── */

static void spawn_shell(struct playos_init_state *s)
{
	const char *path = "/usr/bin/playos-shell";

	playos_log_write(s, "sup", "spawning shell: %s", path);

	pid_t pid = fork();
	if (pid < 0) {
		playos_log_write(s, "sup", "shell fork failed: %s",
		                 strerror(errno));
		return;
	}

	if (pid == 0) {
		/* Child: same Wayland env as compositor */
		setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
		setenv("WAYLAND_DISPLAY", "wayland-0", 1);

		/* Persist shell stderr (EGL/Wayland errors, fps) to /data */
		child_log_redirect("/data/log/shell-stderr.log");

		execl(path, path, NULL);

		dprintf(STDERR_FILENO,
		        "playos-init: shell exec failed: %s\n",
		        strerror(errno));
		_exit(127);
	}

	/* Parent: track as child */
	s->shell_pid = pid;
	playos_log_write(s, "sup", "shell launched (PID %d)", pid);
}

void playos_supervisor_spawn_shell(struct playos_init_state *s)
{
	spawn_shell(s);
}


static void spawn_test_client(struct playos_init_state *s)
{
    const char *path = "/usr/bin/playos-test-client";

    playos_log_write(s, "sup", "spawning test client: %s", path);

    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "sup", "test-client fork failed: %s",
                         strerror(errno));
        return;
    }

    if (pid == 0) {
        /* Child: same Wayland env as compositor */
        setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);

        /* Persist client stderr (EGL/Wayland errors, fps) to /data */
        child_log_redirect("/data/log/test-client.log");

        execl(path, path, NULL);

        dprintf(STDERR_FILENO,
                "playos-init: test-client exec failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    /* Parent: track as child, not supervised like compositor */
    playos_log_write(s, "sup", "test client launched (PID %d)", pid);
}

void playos_supervisor_spawn_test_client(struct playos_init_state *s)
{
    spawn_test_client(s);
}

/* ── Game supervision ────────────────────────────────────────────── */

pid_t playos_supervisor_spawn_game(struct playos_init_state *s,
                                    const char *game_id,
                                    const char *manifest_path)
{
    (void)manifest_path; /* Full manifest validation in later sprint */

    if (s->game_state != GAME_NONE) {
        playos_log_write(s, "sup", "game launch rejected: already running");
        return -1;
    }

    /* For Sprint 1: launch a placeholder game process */
    playos_log_write(s, "sup", "spawning game: %s", game_id);

    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "sup", "game fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child: placeholder game — just keep running until killed */
        setsid();
        /* Simple game stub: print ID and sleep */
        dprintf(STDERR_FILENO, "playos-game: %s starting (PID %d)\n",
                game_id, getpid());
        for (;;) {
            sleep(10);
        }
        _exit(0);
    }

    /* Parent */
    s->game_pid = pid;
    s->game_state = GAME_RUNNING;
    strncpy(s->game_id, game_id, sizeof(s->game_id) - 1);
    s->game_id[sizeof(s->game_id) - 1] = '\0';

    playos_log_write(s, "sup", "game spawned: %s PID %d", game_id, pid);
    return pid;
}

int playos_supervisor_terminate_game(struct playos_init_state *s, int force)
{
    if (s->game_pid == 0 || s->game_state == GAME_NONE) {
        playos_log_write(s, "sup", "terminate: no game running");
        return -1;
    }

    playos_log_write(s, "sup", "terminating game %s (force=%d)",
                     s->game_id, force);

    s->game_state = GAME_STOPPING;

    if (force) {
        /* Immediate kill */
        kill(s->game_pid, SIGKILL);
    } else {
        /* Graceful: SIGTERM first, then SIGKILL after timeout */
        kill(s->game_pid, SIGTERM);

        /* TODO S1-T6: Add timeout escalation via timer/alarm
         * For now, rely on the game process handling SIGTERM
         * and the waitpid loop reaping it.
         */
    }

    return 0;
}

void playos_supervisor_game_exited(struct playos_init_state *s,
                                    int exit_code, int signal_num)
{
    playos_log_write(s, "sup",
                     "game %s PID %d exited: code=%d signal=%d",
                     s->game_id, s->game_pid, exit_code, signal_num);

    s->game_pid = 0;
    s->game_id[0] = '\0';
    s->game_state = GAME_NONE;
}

/* ── Recovery ────────────────────────────────────────────────────── */

void playos_enter_recovery(struct playos_init_state *s, const char *reason)
{
    playos_log_write(s, "init", "ENTERING RECOVERY MODE: %s", reason);
    s->recovery_mode = 1;
    s->boot_stage = BOOT_STAGE_RECOVERY;
    playos_boot_stage_write(BOOT_STAGE_RECOVERY);

    /* Kill any supervised children */
    if (s->compositor_pid > 0)
        kill(s->compositor_pid, SIGTERM);
    if (s->game_pid > 0)
        kill(s->game_pid, SIGTERM);

    /* Display diagnostic */
    dprintf(STDERR_FILENO,
        "\n"
        "  ╔══════════════════════════════════════════════════╗\n"
        "  ║           RECOVERY MODE                          ║\n"
        "  ╠══════════════════════════════════════════════════╣\n"
        "  ║  %-46s  ║\n"
        "  ╚══════════════════════════════════════════════════╝\n"
        "\n"
        "  System halted. Reboot to retry.\n",
        reason);

    /* Sync and halt */
    sync();
    reboot(RB_HALT_SYSTEM);
}

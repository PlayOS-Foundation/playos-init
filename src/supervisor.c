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
#include "ipc.h"

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

/* ── Game manifest helpers ──────────────────────────────────────── */

/*
 * Read a whole file into a NUL-terminated buffer. Returns bytes read
 * (>= 0) on success, -1 on error.
 */
static int read_whole_file(const char *path, char *buf, size_t bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, bufsz - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return (int)n;
}

/*
 * Minimal JSON string extractor: return the value of "key" from a flat
 * JSON object. Sufficient for game manifests (no nested objects, arrays,
 * or escapes). Returns length, or -1 if the key is absent/unparseable.
 */
static int json_string_field(const char *json, const char *key, char *out,
                             size_t outsz)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p)
        return -1;
    p += strlen(needle);
    p = strchr(p, ':');
    if (!p)
        return -1;
    p = strchr(p, '"');
    if (!p)
        return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end)
        return -1;
    size_t len = (size_t)(end - p);
    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return (int)len;
}

/* ── Game supervision ────────────────────────────────────────────── */

pid_t playos_supervisor_spawn_game(struct playos_init_state *s,
                                    const char *game_id,
                                    const char *manifest_path)
{
    if (s->game_state != GAME_NONE) {
        playos_log_write(s, "sup", "game launch rejected: already running");
        return -1;
    }

    /* Resolve the manifest: caller-supplied path, else the canonical
     * /data/games/<id>/manifest.json. */
    char manifest[640];
    if (manifest_path && manifest_path[0]) {
        snprintf(manifest, sizeof(manifest), "%s", manifest_path);
    } else {
        snprintf(manifest, sizeof(manifest), "/data/games/%s/manifest.json",
                 game_id);
    }

    /* Read the manifest for the executable (default "bin/game"). */
    char manifest_buf[4096];
    char executable[256] = "bin/game";
    if (read_whole_file(manifest, manifest_buf, sizeof(manifest_buf)) > 0) {
        char exe[256];
        if (json_string_field(manifest_buf, "executable", exe, sizeof(exe)) > 0)
            snprintf(executable, sizeof(executable), "%s", exe);
    } else {
        playos_log_write(s, "sup", "game manifest unreadable: %s", manifest);
    }

    /* The executable path is relative to the game directory. */
    char exe_path[640];
    snprintf(exe_path, sizeof(exe_path), "/data/games/%s/%s",
             game_id, executable);
    if (access(exe_path, X_OK) != 0) {
        playos_log_write(s, "sup", "game executable not runnable: %s",
                         exe_path);
        return -1;
    }

    playos_log_write(s, "sup", "spawning game: %s (%s)", game_id, exe_path);

    /* Lifecycle pipe: init writes single-byte lifecycle events; the game
     * inherits the read end via the PLAYOS_LIFECYCLE_FD environment
     * variable (see playos_lifecycle.c in libplayos). Non-fatal on
     * failure — the game simply runs without lifecycle events. */
    int lifecycle_read_fd = -1;
    int lifecycle_write_fd = -1;
    if (playos_lifecycle_create(&lifecycle_read_fd, &lifecycle_write_fd) != 0) {
        playos_log_write(s, "sup", "lifecycle pipe create failed: %s",
                         strerror(errno));
        lifecycle_read_fd = -1;
        lifecycle_write_fd = -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (lifecycle_read_fd >= 0)
            close(lifecycle_read_fd);
        if (lifecycle_write_fd >= 0)
            close(lifecycle_write_fd);
        playos_log_write(s, "sup", "game fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Child: run the actual game binary. */
        setsid();

        /* Games render through the Wayland compositor exactly like the
         * shell: they must inherit XDG_RUNTIME_DIR and WAYLAND_DISPLAY so
         * raylib's PLATFORM_PLAYOS backend can connect to the compositor
         * socket. Without these the game's InitWindow() fails and the
         * process exits before drawing a single frame. */
        setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
        setenv("WAYLAND_DISPLAY", "wayland-0", 1);

        /* Hand the read end of the lifecycle pipe to the game. */
        if (lifecycle_read_fd >= 0) {
            char fd_str[16];
            snprintf(fd_str, sizeof(fd_str), "%d", lifecycle_read_fd);
            setenv("PLAYOS_LIFECYCLE_FD", fd_str, 1);
        }
        if (lifecycle_write_fd >= 0)
            close(lifecycle_write_fd);

        /* Persist the game's stdout/stderr to the data partition so a
         * crash, assertion, or loader error is visible on the USB. */
        char log_path[256];
        snprintf(log_path, sizeof(log_path),
                 "/data/log/game-%s-stderr.log", game_id);
        child_log_redirect(log_path);

        /* Run from the game directory so relative assets resolve. */
        char game_dir[640];
        snprintf(game_dir, sizeof(game_dir), "/data/games/%s", game_id);
        (void)chdir(game_dir);

        execl(exe_path, exe_path, (char *)NULL);

        /* exec failed */
        dprintf(STDERR_FILENO, "playos-init: game exec %s failed: %s\n",
                exe_path, strerror(errno));
        _exit(127);
    }

    /* Parent */
    if (lifecycle_read_fd >= 0)
        close(lifecycle_read_fd);
    s->lifecycle_write_fd = lifecycle_write_fd;

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
        /* Graceful: signal the game to save state and exit, then
         * SIGTERM as a fallback for games not reading the pipe. */
        if (s->lifecycle_write_fd >= 0)
            playos_lifecycle_send_event(s->lifecycle_write_fd,
                                        PLAYOS_LIFECYCLE_TERMINATE);
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

    if (s->lifecycle_write_fd >= 0) {
        close(s->lifecycle_write_fd);
        s->lifecycle_write_fd = -1;
    }

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

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
#include "playos-init/ipc_handler.h"
#include "playos-init/security.h"
#include "ipc.h"

/* ── External logging ────────────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void playos_log_fatal(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Forward declarations ────────────────────────────────────────── */

static long long
monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void compositor_restart(struct playos_init_state *s);
static int compositor_should_restart(struct playos_init_state *s);
static void spawn_shell(struct playos_init_state *s);
static void spawn_overlay(struct playos_init_state *s);
static void spawn_installer(struct playos_init_state *s);
static void spawn_ssh(struct playos_init_state *s);

/* ── Persistent child logging ────────────────────────────────────── */

/* Redirect the current (child) process's stdout/stderr to a log file
 * on the persistent /data partition. /data/log is created by
 * playos_data_create_dirs() before Stage 4. On failure, output keeps
 * going to the console as before. */
static void child_log_redirect(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
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
        } else if (pid == s->overlay_pid) {
            /* Overlay exited */
            int exit_code = -1;
            int signal_num = 0;

            if (WIFEXITED(wstatus))
                exit_code = WEXITSTATUS(wstatus);
            if (WIFSIGNALED(wstatus))
                signal_num = WTERMSIG(wstatus);

            playos_supervisor_overlay_exited(s, exit_code, signal_num);
        } else if (pid == s->installer_pid) {
            /* Installer exited (Sprint 10) */
            int exit_code = -1;
            int signal_num = 0;

            if (WIFEXITED(wstatus))
                exit_code = WEXITSTATUS(wstatus);
            if (WIFSIGNALED(wstatus))
                signal_num = WTERMSIG(wstatus);

            playos_supervisor_installer_exited(s, exit_code, signal_num);
        } else if (pid == s->ssh_pid) {
            /* SSH bring-up exited (Sprint 11.6) */
            int exit_code = -1;
            int signal_num = 0;

            if (WIFEXITED(wstatus))
                exit_code = WEXITSTATUS(wstatus);
            if (WIFSIGNALED(wstatus))
                signal_num = WTERMSIG(wstatus);

            playos_supervisor_ssh_exited(s, exit_code, signal_num);
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
        setenv("WAYLAND_DISPLAY", "playos-0", 1);
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
		setenv("WAYLAND_DISPLAY", "playos-0", 1);

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

/* ── Overlay supervision (Sprint 7) ───────────────────────────────── */

static void spawn_overlay(struct playos_init_state *s)
{
	const char *path = "/usr/bin/playos-overlay";

	playos_log_write(s, "sup", "spawning overlay: %s", path);

	pid_t pid = fork();
	if (pid < 0) {
		playos_log_write(s, "sup", "overlay fork failed: %s",
		                 strerror(errno));
		return;
	}

	if (pid == 0) {
		/* Child: same Wayland env as compositor */
		setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
		setenv("WAYLAND_DISPLAY", "playos-0", 1);

		/* Persist overlay stderr to /data for debugging */
		child_log_redirect("/data/log/overlay-stderr.log");

		execl(path, path, NULL);

		dprintf(STDERR_FILENO,
		        "playos-init: overlay exec failed: %s\n",
		        strerror(errno));
		_exit(127);
	}

	/* Parent: track as child */
	s->overlay_pid = pid;
	playos_log_write(s, "sup", "overlay launched (PID %d)", pid);
}

static int overlay_should_restart(struct playos_init_state *s)
{
	time_t now = time(NULL);
	struct playos_restart_info *r = &s->overlay_restarts;

	if (now - r->window_start > PLAYOS_OVERLAY_WINDOW_S) {
		r->count = 0;
		r->window_start = now;
	}

	r->count++;
	return (r->count <= PLAYOS_OVERLAY_MAX_RESTARTS);
}

static void overlay_restart(struct playos_init_state *s)
{
	playos_log_write(s, "sup",
	                 "restarting overlay in %d ms (attempt %d)",
	                 PLAYOS_OVERLAY_RESTART_DELAY_MS,
	                 s->overlay_restarts.count);

	usleep(PLAYOS_OVERLAY_RESTART_DELAY_MS * 1000);

	spawn_overlay(s);
}

void playos_supervisor_overlay_exited(struct playos_init_state *s,
                                      int exit_code, int signal_num)
{
	s->overlay_restarts.last_exit_code = exit_code;
	s->overlay_restarts.last_signal = signal_num;

	playos_log_write(s, "sup",
	                 "overlay PID %d exited: code=%d signal=%d",
	                 s->overlay_pid, exit_code, signal_num);

	s->overlay_pid = 0;

	if (overlay_should_restart(s)) {
		overlay_restart(s);
	} else {
		playos_log_write(s, "sup",
		                 "overlay restart limit exceeded (%d restarts in %ds) — "
		                 "leaving system running without overlay",
		                 PLAYOS_OVERLAY_MAX_RESTARTS,
		                 PLAYOS_OVERLAY_WINDOW_S);
	}
}

void playos_supervisor_spawn_overlay(struct playos_init_state *s)
{
	spawn_overlay(s);
}

/* ── Installer supervision (Sprint 10) ───────────────────────────── */

static void spawn_installer(struct playos_init_state *s)
{
	const char *path = "/usr/bin/playos-installer";

	playos_log_write(s, "sup", "spawning installer: %s", path);

	pid_t pid = fork();
	if (pid < 0) {
		playos_log_write(s, "sup", "installer fork failed: %s",
		                 strerror(errno));
		return;
	}

	if (pid == 0) {
		/* Child: same Wayland env as compositor */
		setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
		setenv("WAYLAND_DISPLAY", "playos-0", 1);

		/* /data may not exist on an installer boot; redirect only when
		 * the persistent log directory is actually available. */
		child_log_redirect("/data/log/installer-stderr.log");

		execl(path, path, NULL);

		dprintf(STDERR_FILENO,
		        "playos-init: installer exec failed: %s\n",
		        strerror(errno));
		_exit(127);
	}

	/* Parent: track as child */
	s->installer_pid = pid;
	playos_log_write(s, "sup", "installer launched (PID %d)", pid);
}

static int installer_should_restart(struct playos_init_state *s)
{
	time_t now = time(NULL);
	struct playos_restart_info *r = &s->installer_restarts;

	if (now - r->window_start > PLAYOS_INSTALLER_WINDOW_S) {
		r->count = 0;
		r->window_start = now;
	}

	r->count++;
	return (r->count <= PLAYOS_INSTALLER_MAX_RESTARTS);
}

static void installer_restart(struct playos_init_state *s)
{
	playos_log_write(s, "sup",
	                 "restarting installer in %d ms (attempt %d)",
	                 PLAYOS_INSTALLER_RESTART_DELAY_MS,
	                 s->installer_restarts.count);

	usleep(PLAYOS_INSTALLER_RESTART_DELAY_MS * 1000);

	spawn_installer(s);
}

void playos_supervisor_installer_exited(struct playos_init_state *s,
                                        int exit_code, int signal_num)
{
	s->installer_restarts.last_exit_code = exit_code;
	s->installer_restarts.last_signal = signal_num;

	playos_log_write(s, "sup",
	                 "installer PID %d exited: code=%d signal=%d",
	                 s->installer_pid, exit_code, signal_num);

	s->installer_pid = 0;

	if (installer_should_restart(s)) {
		installer_restart(s);
	} else {
		playos_log_write(s, "sup",
		                 "installer restart limit exceeded (%d restarts in %ds) — "
		                 "leaving compositor running without installer",
		                 PLAYOS_INSTALLER_MAX_RESTARTS,
		                 PLAYOS_INSTALLER_WINDOW_S);
	}
}

void playos_supervisor_spawn_installer(struct playos_init_state *s)
{
	spawn_installer(s);
}

/* ── Developer SSH supervision (Sprint 11.6) ─────────────────────── */

static void spawn_ssh(struct playos_init_state *s)
{
    const char *path = "/usr/bin/playos-ssh-bringup";

    playos_log_write(s, "sup", "spawning SSH bring-up: %s", path);

    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "sup", "ssh fork failed: %s",
                         strerror(errno));
        return;
    }

    if (pid == 0) {
        /* Not a Wayland client — no compositor env needed. */
        child_log_redirect("/data/log/ssh-bringup.log");

        execl(path, path, NULL);

        dprintf(STDERR_FILENO,
                "playos-init: ssh exec failed: %s\n",
                strerror(errno));
        _exit(127);
    }

    /* Parent: track as child */
    s->ssh_pid = pid;
    playos_log_write(s, "sup", "ssh bring-up launched (PID %d)", pid);
}

static int ssh_should_restart(struct playos_init_state *s)
{
    time_t now = time(NULL);
    struct playos_restart_info *r = &s->ssh_restarts;

    if (now - r->window_start > PLAYOS_SSH_WINDOW_S) {
        r->count = 0;
        r->window_start = now;
    }

    r->count++;
    return (r->count <= PLAYOS_SSH_MAX_RESTARTS);
}

static void ssh_restart(struct playos_init_state *s)
{
    playos_log_write(s, "sup",
                     "restarting SSH bring-up in %d ms (attempt %d)",
                     PLAYOS_SSH_RESTART_DELAY_MS,
                     s->ssh_restarts.count);

    usleep(PLAYOS_SSH_RESTART_DELAY_MS * 1000);

    spawn_ssh(s);
}

void playos_supervisor_ssh_exited(struct playos_init_state *s,
                                  int exit_code, int signal_num)
{
    s->ssh_restarts.last_exit_code = exit_code;
    s->ssh_restarts.last_signal = signal_num;

    playos_log_write(s, "sup",
                     "ssh bring-up PID %d exited: code=%d signal=%d",
                     s->ssh_pid, exit_code, signal_num);

    s->ssh_pid = 0;

    if (ssh_should_restart(s)) {
        ssh_restart(s);
    } else {
        playos_log_write(s, "sup",
                         "ssh restart limit exceeded (%d restarts in %ds) — "
                         "leaving system running without SSH",
                         PLAYOS_SSH_MAX_RESTARTS,
                         PLAYOS_SSH_WINDOW_S);
        /* Do NOT enter recovery — SSH is a developer convenience; the
         * system must continue booting without it. */
    }
}

void playos_supervisor_spawn_ssh(struct playos_init_state *s)
{
    spawn_ssh(s);
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

/*
 * Minimal JSON integer extractor: return the value of "key" from a flat
 * JSON object. Returns 0 and stores the value on success, -1 otherwise.
 */
static int json_int_field(const char *json, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (*p == '\0')
        return -1;
    char *end = NULL;
    long val = strtol(p, &end, 10);
    if (end == p)
        return -1;
    *out = (int)val;
    return 0;
}

/* ── Game supervision ────────────────────────────────────────────── */

int playos_supervisor_generate_launch_token(struct playos_init_state *s)
{
    unsigned char rnd[16];
    int got = 0;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, rnd, sizeof(rnd));
        close(fd);
        if (n == (ssize_t)sizeof(rnd))
            got = 1;
    }

    if (!got) {
        /* Deterministic fallback for early-boot when /dev/urandom is not
         * yet available. Still unpredictable enough for a per-launch
         * correlation token. */
        unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
        for (size_t i = 0; i < sizeof(rnd); i++) {
            seed = seed * 1103515245u + 12345u;
            rnd[i] = (unsigned char)((seed >> 16) & 0xff);
        }
    }

    char *p = s->launch_token;
    size_t cap = sizeof(s->launch_token);
    for (size_t i = 0; i < sizeof(rnd) && cap > 2; i++) {
        int n = snprintf(p, cap, "%02x", rnd[i]);
        if (n < 0 || (size_t)n >= cap)
            break;
        p += n;
        cap -= (size_t)n;
    }

    playos_log_write(s, "sup", "generated launch token %s", s->launch_token);
    return 0;
}

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

    /* Read the manifest for the executable and validate api_version. */
    char manifest_buf[4096];
    char executable[256] = "bin/game";
    if (read_whole_file(manifest, manifest_buf, sizeof(manifest_buf)) <= 0) {
        playos_log_write(s, "sup", "game manifest unreadable: %s", manifest);
        return -1;
    }

    int api = 0;
    (void)json_int_field(manifest_buf, "api_version", &api);
    if (api > PLAYOS_API_VERSION) {
        playos_log_write(s, "sup",
                         "game api_version %d exceeds supported %d: %s",
                         api, PLAYOS_API_VERSION, manifest);
        return -1;
    }

    {
        char exe[256];
        if (json_string_field(manifest_buf, "executable", exe, sizeof(exe)) > 0)
            snprintf(executable, sizeof(executable), "%s", exe);
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

    /* S12-T8: warn-only manifest signature verification. Never blocks
     * launch — the result only drives a log line. */
    {
        char sig_path[672];
        int  mrc;

        snprintf(sig_path, sizeof(sig_path), "%s.sig", manifest);
        mrc = playos_security_verify_manifest(manifest, sig_path);

        if (mrc == 0)
            playos_log_write(s, "sup", "manifest signature verified: %s",
                             manifest);
        else if (mrc == 1)
            playos_log_write(s, "sup",
                             "WARN: game manifest is UNSIGNED (no %s.sig): %s",
                             manifest, manifest);
        else if (mrc == -1)
            playos_log_write(s, "sup",
                             "WARN: game manifest signature INVALID: %s",
                             manifest);
        else
            playos_log_write(s, "sup",
                             "WARN: game manifest signature unverifiable: %s",
                             manifest);
    }

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
        setenv("WAYLAND_DISPLAY", "playos-0", 1);
        setenv("PLAYOS_GAME_ID", game_id, 1);
        {
            char p[640];
            snprintf(p, sizeof(p), "/data/games/%s", game_id);
            setenv("PLAYOS_INSTALL_PATH", p, 1);
            snprintf(p, sizeof(p), "/data/saves/%s", game_id);
            setenv("PLAYOS_SAVE_PATH", p, 1);
            snprintf(p, sizeof(p), "/data/cache/%s", game_id);
            setenv("PLAYOS_CACHE_PATH", p, 1);
        }
        setenv("PLAYOS_LAUNCH_TOKEN", s->launch_token, 1);

        /* Hand the read end of the lifecycle pipe to the game. */
        if (lifecycle_read_fd >= 0) {
            char fd_str[16];
            snprintf(fd_str, sizeof(fd_str), "%d", lifecycle_read_fd);
            setenv("PLAYOS_LIFECYCLE_FD", fd_str, 1);
        }
        if (lifecycle_write_fd >= 0)
            close(lifecycle_write_fd);

        /* Persist the game's stdout/stderr to the data partition so a
         * crash, assertion, or loader error is visible on the USB. This
         * open happens BEFORE the Landlock sandbox is applied — the
         * inherited fd stays valid afterwards. */
        char log_path[256];
        snprintf(log_path, sizeof(log_path),
                 "/data/log/game-%s-stderr.log", game_id);
        child_log_redirect(log_path);

        /* ── Sprint 12 game sandbox ──────────────────────────────────
         * Order matters: no_new_privs first (Landlock restrict_self
         * needs it), then Landlock while still root, then the credential
         * drop (seccomp denies setuid/setgid/capset), then seccomp. */
        if (playos_security_disable_priv_escalation() != 0) {
            dprintf(STDERR_FILENO,
                    "playos-init: PR_SET_NO_NEW_PRIVS failed: %s\n",
                    strerror(errno));
            _exit(126);
        }

        {
            int ll = playos_security_apply_landlock(game_id);
            if (ll == 1) {
                dprintf(STDERR_FILENO,
                        "playos-init: WARN: Landlock unsupported on this "
                        "kernel — launching game WITHOUT filesystem sandbox\n");
            } else if (ll != 0) {
                dprintf(STDERR_FILENO,
                        "playos-init: WARN: Landlock setup failed (%s) — "
                        "launching game WITHOUT filesystem sandbox\n",
                        strerror(errno));
            }
        }

        if (playos_security_drop_privileges() != 0) {
            dprintf(STDERR_FILENO,
                    "playos-init: game credential drop failed: %s\n",
                    strerror(errno));
            _exit(126);
        }

        if (playos_security_apply_seccomp() != 0) {
            dprintf(STDERR_FILENO,
                    "playos-init: WARN: seccomp filter failed (%s) — "
                    "launching game WITHOUT syscall filter\n",
                    strerror(errno));
        }

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

    /* Fresh process: clear any stale lifecycle state. */
    s->game_backgrounded = 0;
    s->game_stopped      = 0;
    s->bg_since_ms       = 0;

    /* S7-T8: a cooperative game learns it is live via FOREGROUND. */
    if (s->lifecycle_write_fd >= 0)
        playos_lifecycle_send_event(s->lifecycle_write_fd,
                                    PLAYOS_LIFECYCLE_FOREGROUND);

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
    s->game_backgrounded = 0;
    s->bg_since_ms = 0;

    if (force) {
        /* Immediate kill */
        kill(s->game_pid, SIGKILL);
    } else {
        /* If the game was SIGSTOPped (backgrounded, non-cooperative), it
         * must be SIGCONT'd first or it will never run its SIGTERM
         * handler and the 2s grace escalation below would SIGKILL a
         * process that never got a chance to exit cleanly. */
        if (s->game_stopped) {
            kill(s->game_pid, SIGCONT);
            s->game_stopped = 0;
        }

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

    /* A crash is any abnormal termination: killed by a signal, or a
     * non-zero exit code. Emit to the shell before clearing game_id so
     * the notification can carry the departed game's identity. */
    int crashed = (signal_num != 0) || (exit_code != 0);
    char exit_json[384];

    if (crashed) {
        snprintf(exit_json, sizeof(exit_json),
                 "\"game_id\":\"%s\",\"exit_code\":%d,\"signal\":%d",
                 s->game_id, exit_code, signal_num);
        playos_ipc_emit_to_shell(s, PLAYOS_IPC_TYPE_GAME_CRASHED,
                                 exit_json);
    } else {
        snprintf(exit_json, sizeof(exit_json),
                 "\"game_id\":\"%s\",\"exit_code\":%d",
                 s->game_id, exit_code);
        playos_ipc_emit_to_shell(s, PLAYOS_IPC_TYPE_GAME_EXITED,
                                 exit_json);
    }

    if (s->lifecycle_write_fd >= 0) {
        close(s->lifecycle_write_fd);
        s->lifecycle_write_fd = -1;
    }

    s->game_pid         = 0;
    s->game_id[0]       = '\0';
    s->game_state       = GAME_NONE;
    s->game_backgrounded = 0;
    s->game_stopped      = 0;
    s->bg_since_ms       = 0;
}

/*
 * Background the game (overlay shown over it). Delivers the cooperative
 * BACKGROUND event and arms the non-cooperative SIGSTOP timer, which
 * playos_supervisor_lifecycle_tick() escalates after
 * PLAYOS_GAME_PAUSE_TIMEOUT_MS.
 */
void
playos_supervisor_game_background(struct playos_init_state *s)
{
    if (s->game_pid <= 0) {
        s->game_backgrounded = 0;
        s->game_stopped      = 0;
        return;
    }

    if (s->game_backgrounded)
        return;   /* already backgrounded */

    s->game_backgrounded = 1;
    s->bg_since_ms = monotonic_ms();

    if (s->lifecycle_write_fd >= 0)
        playos_lifecycle_send_event(s->lifecycle_write_fd,
                                    PLAYOS_LIFECYCLE_BACKGROUND);

    playos_log_write(s, "sup", "game backgrounded (SIGSTOP armed in %dms)",
                     PLAYOS_GAME_PAUSE_TIMEOUT_MS);
}

/*
 * Foreground the game (overlay dismissed / game surfaced). Resumes a
 * non-cooperatively stopped game before delivering the cooperative
 * FOREGROUND event so it can actually run its handler.
 */
void
playos_supervisor_game_foreground(struct playos_init_state *s)
{
    if (s->game_pid <= 0) {
        s->game_backgrounded = 0;
        s->game_stopped      = 0;
        return;
    }

    s->game_backgrounded = 0;
    s->bg_since_ms = 0;

    if (s->game_stopped) {
        if (kill(s->game_pid, SIGCONT) == 0) {
            playos_log_write(s, "sup", "game %d SIGCONT sent", s->game_pid);
        } else {
            playos_log_write(s, "sup", "SIGCONT failed: %s", strerror(errno));
        }
        s->game_stopped = 0;
    }

    if (s->lifecycle_write_fd >= 0)
        playos_lifecycle_send_event(s->lifecycle_write_fd,
                                    PLAYOS_LIFECYCLE_FOREGROUND);

    playos_log_write(s, "sup", "game foregrounded");
}

/*
 * Non-cooperative SIGSTOP fallback (S7-T5). Called from the main
 * supervision loop; sends SIGSTOP if a backgrounded game has not paused
 * within PLAYOS_GAME_PAUSE_TIMEOUT_MS of receiving BACKGROUND.
 */
void
playos_supervisor_lifecycle_tick(struct playos_init_state *s)
{
    if (s->game_pid <= 0 || !s->game_backgrounded || s->game_stopped)
        return;

    long long elapsed_ms = monotonic_ms() - s->bg_since_ms;

    if (elapsed_ms < (long long)PLAYOS_GAME_PAUSE_TIMEOUT_MS)
        return;

    if (kill(s->game_pid, SIGSTOP) == 0) {
        s->game_stopped = 1;
        playos_log_write(s, "sup",
                         "game %d SIGSTOP sent (non-cooperative)",
                         s->game_pid);
    } else {
        playos_log_write(s, "sup", "SIGSTOP failed: %s", strerror(errno));
    }
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

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
#include <sys/mount.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#include "playos-init/init.h"
#include "playos-init/supervisor.h"
#include "playos-init/shutdown.h"
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

    /* stderr is a file here, so stdio would block-buffer it and a crashing
     * child would take its last (most useful) log lines to the grave - that is
     * exactly how the compositor's SIGSEGV during the installer handoff looked
     * like "the log just stops at startup". Line-buffer both streams. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);
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

/* ── Network daemons (Sprint 16, T5) ─────────────────────────────────
 * wpa_supplicant, dhcpcd and the playos-net bridge start once /data is mounted
 * (Wi-Fi profiles live there) and are supervised like every other trusted
 * daemon: a crash is logged and restarted within the same window/count policy
 * the shell uses. The wireless interface is discovered, never assumed — the
 * Ally's radio comes up as wlp6s0 (predictable naming), not wlan0. */

#define PLAYOS_NET_WINDOW_S         60
#define PLAYOS_NET_MAX_RESTARTS     5
#define PLAYOS_NET_RESTART_DELAY_MS 1000
#define PLAYOS_NET_CTRL_DIR         "/run/playos/net"

struct playos_net_sup {
    pid_t wpa_pid;
    pid_t dhcpcd_pid;
    pid_t net_pid;
    struct playos_restart_info wpa_restarts;
    struct playos_restart_info dhcpcd_restarts;
    struct playos_restart_info net_restarts;
    char  ifname[32];
    int   started;
    int   attempts;      /* interface-discovery attempts so far */
};

static struct playos_net_sup g_net;

/* The radio is the interface that has a `wireless` attribute. */
static int net_wireless_ifname(char *out, size_t out_sz)
{
    DIR *d = opendir("/sys/class/net");
    if (!d)
        return -1;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;

        /* Interface names are IFNAMSIZ (16) at most, but d_name is not
         * bounded by that — bound both buffers explicitly rather than
         * relying on snprintf truncation. */
        size_t nlen = strlen(e->d_name);
        if (nlen == 0 || nlen >= 64)
            continue;

        char probe[128];
        snprintf(probe, sizeof(probe), "/sys/class/net/%s/wireless", e->d_name);
        if (access(probe, F_OK) == 0) {
            if (nlen >= out_sz)
                continue;
            memcpy(out, e->d_name, nlen + 1);
            closedir(d);
            return 0;
        }
    }

    closedir(d);
    return -1;
}

/* wpa_supplicant needs its control directory to exist, group-owned and 0770, so
 * that only root and playos-trusted reach the socket it creates inside. */
static void net_prepare_runtime_dir(struct playos_init_state *s)
{
    mkdir("/run/playos", 0755);
    if (mkdir(PLAYOS_NET_CTRL_DIR, 0770) != 0 && errno != EEXIST)
        playos_log_write(s, "net", "mkdir %s failed: %s", PLAYOS_NET_CTRL_DIR,
                         strerror(errno));

    chown(PLAYOS_NET_CTRL_DIR, 0, 1000);   /* root:playos-trusted */
    chmod(PLAYOS_NET_CTRL_DIR, 0770);
}

static void net_write_wpa_conf(struct playos_init_state *s)
{
    const char *path = PLAYOS_NET_CTRL_DIR "/wpa.conf";

    FILE *f = fopen(path, "we");
    if (!f) {
        playos_log_write(s, "net", "cannot write %s: %s", path, strerror(errno));
        return;
    }

    fprintf(f,
            "ctrl_interface=" PLAYOS_NET_CTRL_DIR "\n"
            "ctrl_interface_group=playos-trusted\n"
            "ap_scan=1\n"
            "update_config=1\n");
    fclose(f);
    chmod(path, 0600);
}

static void spawn_wpa_supplicant(struct playos_init_state *s)
{
    if (!g_net.ifname[0])
        return;

    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "net", "wpa_supplicant fork failed: %s",
                         strerror(errno));
        return;
    }

    if (pid == 0) {
        child_log_redirect("/data/log/wpa_supplicant-stderr.log");
        execl("/usr/sbin/wpa_supplicant", "wpa_supplicant",
              "-i", g_net.ifname,
              "-c", PLAYOS_NET_CTRL_DIR "/wpa.conf",
              "-P", PLAYOS_NET_CTRL_DIR "/wpa.pid",
              (char *)NULL);
        _exit(127);
    }

    g_net.wpa_pid = pid;
    playos_log_write(s, "net", "wpa_supplicant launched (PID %d, if=%s)",
                     pid, g_net.ifname);
}

static void spawn_playos_net(struct playos_init_state *s)
{
    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "net", "playos-net fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        child_log_redirect("/data/log/playos-net-stderr.log");
        execl("/usr/bin/playos-net", "playos-net", (char *)NULL);
        _exit(127);
    }

    g_net.net_pid = pid;
    playos_log_write(s, "net", "playos-net launched (PID %d)", pid);
}

/* dhcpcd gets the *wireless* interface only: the wired dock NIC is how a
 * developer reaches the device and must not be touched. */
static void spawn_dhcpcd(struct playos_init_state *s)
{
    if (!g_net.ifname[0])
        return;

    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "net", "dhcpcd fork failed: %s", strerror(errno));
        return;
    }

    if (pid == 0) {
        child_log_redirect("/data/log/dhcpcd-stderr.log");
        /* -B keeps dhcpcd in the foreground so init supervises the real PID. */
        execl("/sbin/dhcpcd", "dhcpcd", "-B", "-q", g_net.ifname, (char *)NULL);
        _exit(127);
    }

    g_net.dhcpcd_pid = pid;
    playos_log_write(s, "net", "dhcpcd launched (PID %d, if=%s)",
                     pid, g_net.ifname);
}

static int net_should_restart(struct playos_restart_info *r)
{
    time_t now = time(NULL);

    if (now - r->window_start > PLAYOS_NET_WINDOW_S) {
        r->count = 0;
        r->window_start = now;
    }

    r->count++;
    return (r->count <= PLAYOS_NET_MAX_RESTARTS);
}

void playos_supervisor_start_network(struct playos_init_state *s)
{
    if (g_net.started)
        return;
    /* A live/installer/recovery boot has no business associating: give up for
     * good so the housekeeping tick stops calling us. */
    if (s->install_mode || s->recovery_mode) {
        g_net.started = 1;
        playos_log_write(s, "net", "network stack not started (install/recovery)");
        return;
    }

    net_prepare_runtime_dir(s);

    /* The radio is probed asynchronously: on a cold boot mt7921e creates its
     * interface *after* this runs (measured: init looks at 2.55s, wlp6s0
     * appears at 2.92s). Sampling once and giving up left the whole network
     * stack dead for the session — so wait for it instead. Bounded, and free
     * because the caller is the 1 Hz housekeeping tick. */
    if (net_wireless_ifname(g_net.ifname, sizeof(g_net.ifname)) != 0) {
        g_net.attempts++;
        if (g_net.attempts == 1 || g_net.attempts % 10 == 0)
            playos_log_write(s, "net",
                             "waiting for a wireless interface (attempt %d)",
                             g_net.attempts);
        if (g_net.attempts >= 60) {
            g_net.started = 1;
            playos_log_write(s, "net",
                             "no wireless interface after %d tries - network "
                             "stack not started", g_net.attempts);
        }
        return;
    }

    net_write_wpa_conf(s);
    spawn_wpa_supplicant(s);
    spawn_dhcpcd(s);
    spawn_playos_net(s);      /* retries the wpa control socket itself */
    g_net.started = 1;
    playos_log_write(s, "net", "network stack started (if=%s)", g_net.ifname);
}

void playos_supervisor_network_exited(struct playos_init_state *s, pid_t pid,
                                      int exit_code, int signal_num)
{
    if (pid == 0)
        return;

    if (pid == g_net.wpa_pid) {
        g_net.wpa_restarts.last_exit_code = exit_code;
        g_net.wpa_restarts.last_signal = signal_num;
        playos_log_write(s, "net", "wpa_supplicant exited: code=%d signal=%d",
                         exit_code, signal_num);
        g_net.wpa_pid = 0;

        if (net_should_restart(&g_net.wpa_restarts)) {
            usleep(PLAYOS_NET_RESTART_DELAY_MS * 1000);
            spawn_wpa_supplicant(s);
        } else {
            playos_log_write(s, "net", "wpa_supplicant restart limit reached");
        }
        return;
    }

    if (pid == g_net.dhcpcd_pid) {
        g_net.dhcpcd_restarts.last_exit_code = exit_code;
        g_net.dhcpcd_restarts.last_signal = signal_num;
        playos_log_write(s, "net", "dhcpcd exited: code=%d signal=%d",
                         exit_code, signal_num);
        g_net.dhcpcd_pid = 0;

        if (net_should_restart(&g_net.dhcpcd_restarts)) {
            usleep(PLAYOS_NET_RESTART_DELAY_MS * 1000);
            spawn_dhcpcd(s);
        } else {
            playos_log_write(s, "net", "dhcpcd restart limit reached");
        }
        return;
    }

    if (pid == g_net.net_pid) {
        g_net.net_restarts.last_exit_code = exit_code;
        g_net.net_restarts.last_signal = signal_num;
        playos_log_write(s, "net", "playos-net exited: code=%d signal=%d",
                         exit_code, signal_num);
        g_net.net_pid = 0;

        if (net_should_restart(&g_net.net_restarts)) {
            usleep(PLAYOS_NET_RESTART_DELAY_MS * 1000);
            spawn_playos_net(s);
        } else {
            playos_log_write(s, "net", "playos-net restart limit reached");
        }
        return;
    }
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
        } else if ((g_net.wpa_pid != 0 && pid == g_net.wpa_pid) ||
                   (g_net.dhcpcd_pid != 0 && pid == g_net.dhcpcd_pid) ||
                   (g_net.net_pid != 0 && pid == g_net.net_pid)) {
            /* Network daemons (Sprint 16, T5) */
            int exit_code = -1;
            int signal_num = 0;

            if (WIFEXITED(wstatus))
                exit_code = WEXITSTATUS(wstatus);
            if (WIFSIGNALED(wstatus))
                signal_num = WTERMSIG(wstatus);

            playos_supervisor_network_exited(s, pid, exit_code, signal_num);
        } else {
            /* Unknown child — log and move on */
            playos_log_write(s, "sup", "reaped unknown child PID %d", pid);
        }
    }
}

/* ── Compositor supervision ──────────────────────────────────────── */
/* Read `key=value` from /proc/cmdline into buf. Used for the developer/
 * recovery escape hatches (playos.renderer=pixman) and any future boot option
 * that init forwards to a child as an environment variable. */
static void
cmdline_value(const char *key, char *buf, size_t bufsz)
{
	buf[0] = '\0';

	FILE *f = fopen("/proc/cmdline", "r");
	if (!f)
		return;

	char line[1024] = {0};
	if (!fgets(line, sizeof(line) - 1, f)) {
		fclose(f);
		return;
	}
	fclose(f);

	size_t klen = strlen(key);
	char *p = line;
	while (p && *p) {
		char *tok = p;
		char *sp = strchr(p, ' ');
		if (sp)
			*sp = '\0';
		if (strncmp(tok, key, klen) == 0 && tok[klen] == '=') {
			snprintf(buf, bufsz, "%s", tok + klen + 1);
			return;
		}
		p = sp ? sp + 1 : NULL;
	}
}


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

        /* Escape hatch for testing the software path: playos.renderer=pixman on
         * the kernel cmdline forces the compositor's pixman renderer, which is
         * what a machine with a broken GPU stack ends up with. The shell cannot
         * render that way (it is a GL client), so recovery then falls back to
         * the GL-free recovery client - the path this option exists to test. */
        {
            char renderer[32];
            cmdline_value("playos.renderer", renderer, sizeof(renderer));
            if (renderer[0])
                setenv("PLAYOS_RENDERER", renderer, 1);
        }

        /* NOTE (S14 F3): the recovery UI is deliberately NOT forced onto the
         * software renderer. Measured on the Ally: with WLR_RENDERER=pixman the
         * compositor comes up, but the shell is a GL client and Raylib's EGL
         * cannot create a screen without dmabuf/GL ("failed to get driver name
         * for fd -1" -> eglInitialize 0x3001), so it exits immediately and
         * crash-loops, leaving an empty compositor (a blue screen). Recovery
         * therefore keeps the accelerated renderer; the compositor's software
         * path is a fallback for when EGL/KMS cannot start at all, and a
         * recovery UI that needs no GL at all is still outstanding. */

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
                         "compositor restart limit exceeded (%d restarts in %ds) — recovery UI",
                         PLAYOS_COMPOSITOR_MAX_RESTARTS,
                         PLAYOS_COMPOSITOR_WINDOW_S);
        s->recovery_mode = 1;
        if (playos_supervisor_spawn_compositor(s) == 0) {
            usleep(500000);
            playos_supervisor_spawn_shell(s);
        } else {
            playos_enter_recovery(s, "compositor restart limit exceeded");
        }
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

    /* S13.7: during a runtime installer handoff the shell was stopped on
     * purpose — do not restart it. */
    if (s->installer_runtime_mode)
        return;

    /* Check restart policy */
    if (shell_should_restart(s)) {
        shell_restart(s);
    } else {
        playos_log_write(s, "sup",
                         "shell restart limit exceeded (%d restarts in %ds)",
                         PLAYOS_SHELL_MAX_RESTARTS,
                         PLAYOS_SHELL_WINDOW_S);

        /* S14 F3: in recovery the shell is the only UI, and it is a GL client.
         * If it cannot run (EGL fails because the compositor had to fall back to
         * software rendering, or the shell is otherwise broken), start the
         * GL-free recovery client instead of leaving an empty compositor on
         * screen. Outside recovery the system still runs without a shell (games
         * launch via IPC, the overlay stays available). */
        if (s->recovery_mode && s->recovery_ui_pid <= 0) {
            playos_log_write(s, "sup",
                             "recovery: shell unavailable — starting the "
                             "GL-free recovery client");
            playos_supervisor_spawn_recovery_ui(s);
        } else if (!s->recovery_mode) {
            playos_log_write(s, "sup",
                             "leaving compositor running without shell");
        }
    }
}

/* ── Test client auto-launch ─────────────────────────────────────── */
/* ── GL-free recovery client (S14 F3) ─────────────────────────────── */

void
playos_supervisor_spawn_recovery_ui(struct playos_init_state *s)
{
	const char *path = "/usr/bin/playos-recovery";

	if (s->compositor_state != COMPOSITOR_RUNNING) {
		playos_log_write(s, "sup",
		                 "recovery client needs a compositor; none running");
		return;
	}

	pid_t pid = fork();
	if (pid < 0) {
		playos_log_write(s, "sup", "fork failed for %s", path);
		return;
	}
	if (pid == 0) {
		setsid();
		setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
		setenv("WAYLAND_DISPLAY", "playos-0", 1);
		child_log_redirect("/data/log/recovery-stderr.log");
		execl(path, path, (char *)NULL);
		_exit(127);
	}

	s->recovery_ui_pid = pid;
	playos_log_write(s, "sup", "recovery client launched (PID %d)", pid);
}

/* ── Shell auto-launch (Sprint 5) ─────────────────────────────────── */

static void spawn_shell(struct playos_init_state *s)
{
	const char *path = "/usr/bin/playos-shell";

	/* Test hook: `playos.noshell` on the kernel cmdline makes the shell fail on
	 * purpose, so the F3 path "shell cannot run in recovery -> GL-free recovery
	 * client" can be exercised without a broken GPU (used by
	 * scripts/qemu-recovery-check.sh). Test-only; never set in production. */
	char noshell[8];
	cmdline_value("playos.noshell", noshell, sizeof(noshell));
	if (noshell[0]) {
		playos_log_write(s, "sup",
		                 "TEST: playos.noshell set - shell will fail by design");
		path = "/bin/false";
	}

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
		if (s->recovery_mode)
			setenv("PLAYOS_RECOVERY", "1", 1);

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

	/* S13.7: runtime installer handoff — do not restart the overlay. */
	if (s->installer_runtime_mode)
		return;

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

		/* S14-T10: the shell already asked the user which disk to install
		 * to; hand that answer over so the installer starts the
		 * destructive phase directly instead of re-showing its picker. */
		if (s->installer_target_disk[0])
			setenv("PLAYOS_INSTALL_TARGET", s->installer_target_disk, 1);

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
	s->installer_started_at = time(NULL);
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
	/* S14.5-T3: a shell-driven install whose worker died must surface as an error
	 * rather than a progress bar that never advances - the shell has no other way
	 * to learn the worker is gone. */
	if (s->install_shell_driven && (exit_code != 0 || signal_num != 0)) {
		char reason[128];
		snprintf(reason, sizeof(reason), "install worker exited (code=%d signal=%d)",
		         exit_code, signal_num);
		playos_ipc_emit_to_shell(s, PLAYOS_IPC_TYPE_INSTALL_ERROR, reason);
		playos_log_write(s, "sup", "%s", reason);
	}
	s->install_shell_driven = 0;

	s->installer_restarts.last_exit_code = exit_code;
	s->installer_restarts.last_signal = signal_num;

	playos_log_write(s, "sup",
	                 "installer PID %d exited: code=%d signal=%d",
	                 s->installer_pid, exit_code, signal_num);

	s->installer_pid = 0;

	/* S13.7: a Settings-triggered install reboots into the installed OS;
	 * the boot-time installer keeps the restart policy for QEMU automation.
	 *
	 * S14 follow-up: succeed -> reboot; fail -> hand the session back to the
	 * user. A surprise reboot after a failed install told them nothing and
	 * threw away the shell they were using. The installer logs to
	 * /data/log/installer.log, which now survives (the handoff no longer
	 * unmounts /data), so the failure can be inspected afterwards. */
	if (s->installer_runtime_mode) {
		if (exit_code == 0 && signal_num == 0) {
			playos_log_write(s, "sup",
			                 "runtime installer finished (code=%d) — rebooting",
			                 exit_code);
			playos_shutdown(s, 1); /* never returns */
		}

		long lived = s->installer_started_at
		                 ? (long)(time(NULL) - s->installer_started_at) : -1;
		playos_log_write(s, "sup",
		                 "runtime installer FAILED (code=%d signal=%d, ran %lds) — "
		                 "returning to the shell (stderr: "
		                 "/data/log/installer-stderr.log)",
		                 exit_code, signal_num, lived);
		s->installer_runtime_mode = 0;
		playos_supervisor_remount_installer_efi(s);
		playos_supervisor_spawn_shell(s);
		playos_supervisor_spawn_overlay(s);
		/* SSH was stopped for the handoff: without this the session comes back
		 * with the UI but no way in (S14). */
		spawn_ssh(s);
		return;
	}

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

/* S13.7: stop the live shell + overlay before a runtime installer handoff or
 * a recovery restart. Zeroing the pids makes the pending SIGCHLD reaps go to
 * the "unknown child" path, so their restart policies never respawn them. */
void playos_supervisor_stop_shell_and_overlay(struct playos_init_state *s)
{
	if (s->overlay_pid > 0) {
		playos_log_write(s, "sup", "stopping overlay PID %d",
		                 s->overlay_pid);
		kill(s->overlay_pid, SIGTERM);
		s->overlay_pid = 0;
	}
	if (s->shell_pid > 0) {
		playos_log_write(s, "sup", "stopping shell PID %d",
		                 s->shell_pid);
		kill(s->shell_pid, SIGTERM);
		s->shell_pid = 0;
	}
}

/* S13.7: wait for a SIGTERM'd child to actually exit (closes its /data fds)
 * before trying to unmount /data. Bounded so a stuck child still aborts. */

/* Stop a UI client for the installer handoff. SIGTERM first so it can leave
 * gracefully, then SIGKILL: the installer claims the shell role and the
 * compositor only frees it once the old client's socket closes, so the handoff
 * must not proceed while a client lingers. (A graceful exit used to take longer
 * than the handoff's wait whenever the client was blocked on IPC.) */
static void
stop_client_hard(struct playos_init_state *s, const char *name, pid_t pid)
{
	if (pid <= 0)
		return;

	kill(pid, SIGTERM);

	int waited = 0;
	while (waited < 800) {
		int st;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid || (r < 0 && errno == ECHILD))
			return;
		usleep(50000);
		waited += 50;
	}

	playos_log_write(s, "sup",
	                 "%s PID %d still alive after SIGTERM (%d ms) — killing",
	                 name, pid, waited);
	kill(pid, SIGKILL);

	waited = 0;
	while (waited < 800) {
		int st;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid || (r < 0 && errno == ECHILD))
			return;
		usleep(50000);
		waited += 50;
	}

	playos_log_write(s, "sup", "WARN: %s PID %d survived SIGKILL", name, pid);
}

static void
wait_child_exit(struct playos_init_state *s, pid_t pid, int timeout_ms)
{
	if (pid <= 0)
		return;
	int waited = 0;
	while (waited < timeout_ms) {
		int st;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid)
			return;
		if (r < 0 && errno == ECHILD)
			return;
		usleep(50000);
		waited += 50;
	}
	playos_log_write(s, "sup", "child PID %d did not exit within %d ms",
	                 pid, timeout_ms);
}

/* S13.7: shared runtime installer handoff used by both the StartInstaller IPC
 * handler and the headless playos.install.auto cmdline token. Returns 0 on
 * success (installer spawned, reboot-on-exit armed) or -1 on failure (shell +
 * overlay respawned, live session kept). */
/* S14.5-T3: make the install payload visible at /mnt/payload. init owns this
 * because it knows the boot medium and already has the partition lookup; the
 * worker is deliberately discovery-free. The boot-time installer mounts its own
 * payload, so an already-mounted path is fine. */
static int
mount_install_payload(struct playos_init_state *s, const char *device)
{
	if (access("/mnt/payload/rootfs.squashfs", R_OK) == 0)
		return 0;

	(void)mkdir("/mnt/payload", 0755);

	char dev[128] = {0};
	if (device && device[0]) {
		/* The shell verified this partition by contents; prefer it. */
		snprintf(dev, sizeof(dev), "%s", device);
	} else if (playos_find_partition_by_label("playos-a", dev, sizeof(dev)) != 0) {
		playos_log_write(s, "sup", "install payload: no playos-a partition found");
		return -1;
	}

	if (mount(dev, "/mnt/payload", "ext4", MS_RDONLY, NULL) != 0 &&
	    mount(dev, "/mnt/payload", "ext2", MS_RDONLY, NULL) != 0) {
		playos_log_write(s, "sup", "install payload: mounting %s failed: %s",
		                 dev, strerror(errno));
		return -1;
	}

	if (access("/mnt/payload/rootfs.squashfs", R_OK) != 0) {
		playos_log_write(s, "sup",
		                 "install payload: %s carries no rootfs.squashfs", dev);
		(void)umount("/mnt/payload");
		return -1;
	}

	playos_log_write(s, "sup", "install payload mounted from %s", dev);
	return 0;
}

int
playos_supervisor_start_install_worker(struct playos_init_state *s,
                                       const char *target_disk,
                                       const char *payload_device)
{
	if (!target_disk || !target_disk[0]) {
		playos_log_write(s, "sup", "install worker: no target disk");
		return -1;
	}
	if (s->installer_pid > 0) {
		playos_log_write(s, "sup", "install worker: already running (PID %d)",
		                 s->installer_pid);
		return 0;
	}
	if (mount_install_payload(s, payload_device) != 0)
		return -1;

	pid_t pid = fork();
	if (pid < 0) {
		playos_log_write(s, "sup", "install worker: fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		setsid();
		setenv("XDG_RUNTIME_DIR", "/run/playos", 1);
		setenv("PLAYOS_INSTALL_TARGET", target_disk, 1);
		setenv("PLAYOS_INSTALL_PAYLOAD", "/mnt/payload", 1);
		child_log_redirect("/data/log/install-worker.log");
		execl("/usr/bin/playos-install-worker", "playos-install-worker", (char *)NULL);
		_exit(127);
	}

	s->installer_pid = pid;
	s->install_shell_driven = 1;
	playos_log_write(s, "sup",
	                 "install worker started (PID %d, target %s) - shell keeps the screen",
	                 pid, target_disk);
	return 0;
}

int
playos_supervisor_start_runtime_installer(struct playos_init_state *s)
{
	pid_t shell_pid = s->shell_pid;
	pid_t overlay_pid = s->overlay_pid;
	pid_t ssh_pid = s->ssh_pid;

	/* S14-T10: seamless, console-free handoff. Suppress kernel console
	 * output (console_loglevel 0) and blank the visible VT so no raw
	 * dmesg/init lines appear between the shell and the installer UI. */
	FILE *printk = fopen("/proc/sys/kernel/printk", "w");
	if (printk) {
		fprintf(printk, "0 4 1 7\n");
		fclose(printk);
	}
	int tty = open("/dev/tty0", O_WRONLY | O_NOCTTY);
	if (tty >= 0) {
		write(tty, "\033[2J\033[H", 7);
		close(tty);
	}

	/* Only the UI clients make way. The compositor stays up: stopping it cost
	 * a DRM modeset blink (a black flash between the shell and the installer),
	 * and /data stays mounted — it holds the boot medium's data partition, never
	 * the target (the picker refuses the disk the system booted from), so
	 * unmounting bought nothing and made the installer lose its log. */
	playos_supervisor_stop_shell_and_overlay(s);

	if (ssh_pid > 0) {
		playos_log_write(s, "sup", "stopping ssh-bringup PID %d for installer handoff",
		                 ssh_pid);
		kill(ssh_pid, SIGTERM);
		s->ssh_pid = 0;
	}

	/* SIGTERM is async — wait for the clients to die so the compositor releases
	 * their trusted roles (and their /data/log fds) before the installer claims
	 * the foreground. */
	stop_client_hard(s, "overlay", overlay_pid);
	stop_client_hard(s, "shell", shell_pid);
	wait_child_exit(s, ssh_pid, 2000);

	/* Dev SSH key handoff: /tmp is shared with the installer child and survives
	 * regardless of where /data lives. */
	{
		FILE *key_in = fopen("/data/ssh/authorized_keys", "r");
		if (key_in) {
			int key_out = open("/tmp/playos-install-authorized_keys",
			                   O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
			if (key_out >= 0) {
				char buf[4096];
				size_t n;
				while ((n = fread(buf, 1, sizeof(buf), key_in)) > 0)
					(void)write(key_out, buf, n);
				close(key_out);
				playos_log_write(s, "sup",
				                 "installer SSH key preserved to /tmp");
			}
			fclose(key_in);
		}
	}

	/* Release the target: no partition of the disk being repartitioned may be
	 * mounted. In practice that is the ESP init mounted for A/B accounting.
	 * When the target is a different disk (e.g. an external SSD) the ESP is left
	 * alone and nothing has to be re-mounted afterwards. */
	s->installer_efi_released = 0;
	if (playos_mount_is_on_target("/EFI", s->installer_target_disk)) {
		if (umount("/EFI") == 0 || umount2("/EFI", MNT_DETACH) == 0) {
			s->installer_efi_released = 1;
			playos_log_write(s, "sup",
			                 "released /EFI — it lives on the install target %s",
			                 s->installer_target_disk);
		} else {
			playos_log_write(s, "sup",
			                 "WARN: could not release /EFI (%s) before installing "
			                 "to %s", strerror(errno), s->installer_target_disk);
		}
	} else if (access("/EFI/..", F_OK) == 0) {
		playos_log_write(s, "sup",
		                 "/EFI is not on the install target %s — leaving it as is",
		                 s->installer_target_disk[0]
		                     ? s->installer_target_disk : "(unknown)");
	}

	s->installer_runtime_mode = 1;

	/* The installer registers the trusted *shell* role before it creates its
	 * window, and the compositor rejects a second claim with a protocol error
	 * (libwayland then aborts the client). Waiting for the shell's process to
	 * exit is not quite enough: the compositor still has to process the closed
	 * socket and release the role. Give it an event-loop turn. */
	usleep(300000);

	playos_supervisor_spawn_installer(s);
	return 0;
}

/* Best-effort re-mount of the ESP the handoff released, for the case where the
 * installer failed and the shell comes back: without it /EFI would stay missing
 * and A/B accounting plus the recovery menu's slot display would be degraded. */
void
playos_supervisor_remount_installer_efi(struct playos_init_state *s)
{
	if (!s->installer_efi_released)
		return;

	/* The device may have been repartitioned, so resolve by label again. */
	char dev[128] = {0};
	if (playos_find_partition_by_label("ESP", dev, sizeof(dev)) != 0 ||
	    mount(dev, "/EFI", "vfat", 0, NULL) != 0) {
		playos_log_write(s, "sup",
		                 "could not restore /EFI after the failed install: %s",
		                 strerror(errno));
		return;
	}

	s->efi_mounted = 1;
	s->installer_efi_released = 0;
	playos_log_write(s, "sup", "restored /EFI (%s) after the failed install", dev);
}

/* Base disk name of a device or mount source: "/dev/nvme0n1p1" -> "nvme0n1",
 * "/dev/sda3" -> "sda", "/dev/nvme0n1" -> "nvme0n1".
 *
 * Strip-the-trailing-digits parsing is not enough: "nvme0n1" itself ends in a
 * digit, so a naive version turned the whole disk into "nvme0" and never matched
 * its partitions - which made the installer handoff think the ESP was "not on
 * the target" and leave /EFI mounted, so mkfs.fat later refused to format the
 * ESP ("contains a mounted filesystem", exit 1). Ask the kernel instead: sysfs
 * exposes `partition` for partitions and the parent directory is the disk. */
static void
base_disk_name(const char *dev, char *out, size_t outsz)
{
	const char *b = strrchr(dev, '/');
	b = b ? b + 1 : dev;

	char probe[192];
	snprintf(probe, sizeof(probe), "/sys/class/block/%s/partition", b);
	if (access(probe, F_OK) == 0) {
		char link[192], target[256];
		snprintf(link, sizeof(link), "/sys/class/block/%s", b);
		ssize_t n = readlink(link, target, sizeof(target) - 1);
		if (n > 0) {
			target[n] = '\0';
			char *slash = strrchr(target, '/');
			if (slash && slash != target) {
				*slash = '\0';
				char *disk = strrchr(target, '/');
				disk = disk ? disk + 1 : target;
				snprintf(out, outsz, "%s", disk);
				return;
			}
		}
	}

	/* Already a whole disk (or sysfs is unavailable): use the name as-is. */
	snprintf(out, outsz, "%s", b);
}

/* Is `mountpoint` backed by a partition of `target` ("" = unknown target, in
 * which case we assume the old behaviour and say yes: release it)? */
int
playos_mount_is_on_target(const char *mountpoint, const char *target)
{
	if (!mountpoint)
		return 0;

	if (!target || !target[0]) {
		/* Interactive installer without a preselected disk: we cannot know
		 * where it will write, so release the ESP as before. */
		return 1;
	}

	char dev[128] = {0};
	FILE *f = fopen("/proc/mounts", "r");
	if (f) {
		char line[512];
		while (fgets(line, sizeof(line), f)) {
			char src[128], mnt[256];
			if (sscanf(line, "%127s %255s", src, mnt) != 2)
				continue;
			if (strcmp(mnt, mountpoint) == 0) {
				snprintf(dev, sizeof(dev), "%s", src);
				break;
			}
		}
		fclose(f);
	}

	if (dev[0] == '\0' || strncmp(dev, "/dev/", 5) != 0)
		return 0;                     /* not mounted / not a block device */

	char a[64], b[64];
	base_disk_name(dev, a, sizeof(a));
	base_disk_name(target, b, sizeof(b));
	return strcmp(a, b) == 0;
}

/* S13.7: stop the compositor before a runtime installer handoff so its
 * /data/log/compositor-stderr.log fd no longer pins /data. */
void
playos_supervisor_stop_compositor(struct playos_init_state *s)
{
	if (s->compositor_pid > 0) {
		playos_log_write(s, "sup", "stopping compositor PID %d for installer handoff",
		                 s->compositor_pid);
		kill(s->compositor_pid, SIGTERM);
		s->compositor_pid = 0;
	}
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

/*
 * S15-T7: launch a game by id through the same path as the shell's LaunchGame
 * IPC request. Shared by the IPC handler and the `playos.autostart` emulator
 * hook. Returns the game PID, or -1 when a game is already running or the
 * spawn fails.
 */
pid_t playos_supervisor_launch_game(struct playos_init_state *s,
                                    const char *game_id,
                                    const char *manifest_path)
{
    if (!s)
        return -1;

    if (!game_id || game_id[0] == '\0') {
        playos_log_write(s, "sup", "launch rejected: missing game id");
        return -1;
    }

    if (s->game_pid != 0 || s->game_state != GAME_NONE) {
        playos_log_write(s, "sup", "launch rejected: game already running");
        return -1;
    }

    playos_supervisor_generate_launch_token(s);

    /* Tell the compositor which game to expect before the process starts. */
    char expected_json[384];
    snprintf(expected_json, sizeof(expected_json),
             "\"launch_token\":\"%s\",\"game_id\":\"%s\"",
             s->launch_token, game_id);
    playos_compositor_send(s, PLAYOS_IPC_TYPE_SET_EXPECTED_GAME, expected_json);

    pid_t pid = playos_supervisor_spawn_game(s, game_id, manifest_path);
    if (pid <= 0)
        return -1;

    /* Notify the shell asynchronously that a game started. */
    char started_json[384];
    snprintf(started_json, sizeof(started_json),
             "\"game_id\":\"%s\",\"pid\":%d,\"launch_token\":\"%s\"",
             game_id, s->game_pid, s->launch_token);
    playos_ipc_emit_to_shell(s, PLAYOS_IPC_TYPE_GAME_STARTED, started_json);

    return pid;
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

/* Enter the recovery UI from the running session (S14-T6).
 *
 * Unlike playos_enter_recovery() below (legacy: halts with a console banner),
 * this keeps the compositor alive and restarts the shell in recovery mode so
 * the recovery menu can render. Used by the non-blocking late button watch;
 * the cmdline and data-missing paths set recovery_mode before the shell is
 * first spawned instead. Idempotent. */
void
playos_supervisor_enter_recovery_ui(struct playos_init_state *s,
                                    const char *reason)
{
    if (s->recovery_mode)
        return;

    playos_log_write(s, "init", "ENTERING RECOVERY UI: %s", reason);
    s->recovery_mode = 1;
    s->boot_stage = BOOT_STAGE_RECOVERY;
    playos_boot_stage_write(BOOT_STAGE_RECOVERY);

    /* A game should not be running this early in boot; stop it defensively. */
    if (s->game_pid > 0)
        kill(s->game_pid, SIGTERM);

    pid_t shell_pid = s->shell_pid;
    pid_t overlay_pid = s->overlay_pid;

    /* Drop the live shell + overlay. Zeroing the pids makes their pending
     * SIGCHLD reaps take the "unknown child" path, so the restart policies
     * never respawn them and only our recovery shell comes back. */
    playos_supervisor_stop_shell_and_overlay(s);
    wait_child_exit(s, shell_pid, 1000);
    wait_child_exit(s, overlay_pid, 1000);

    if (s->compositor_state == COMPOSITOR_RUNNING) {
        playos_supervisor_spawn_shell(s); /* PLAYOS_RECOVERY=1 */
        return;
    }

    /* The compositor is down - most likely because graphics is what broke, which
     * is precisely when recovery has to work (S14 F3). Start it again in
     * software mode (recovery_mode is set, so spawn_compositor exports
     * PLAYOS_RENDERER=pixman) and then the recovery shell. */
    playos_log_write(s, "sup",
                     "recovery UI: compositor not running — starting it in "
                     "software mode");
    if (playos_supervisor_spawn_compositor(s) == 0) {
        usleep(500000);
        playos_supervisor_spawn_shell(s); /* PLAYOS_RECOVERY=1 */
    } else {
        playos_log_write(s, "sup",
                         "recovery UI: software compositor start failed");
    }
}

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

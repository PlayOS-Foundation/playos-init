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
        /* -B keeps dhcpcd in the foreground so init supervises the real PID.
         *
         * Wi-Fi must never get in the way of the wired link:
         *   -m 1000            give Wi-Fi's routes a high metric, so if both
         *                      interfaces end up on the same subnet the wired
         *                      route stays preferred and replies still leave by
         *                      the dock (a Wi-Fi route + an AP that isolates
         *                      clients is enough to make the device unreachable
         *                      even though it is perfectly online);
         *   -Z en* / -Z eth*   dhcpcd may not touch a wired NIC at all — it is
         *                      the developer's link to the device.
         * Patterns are passed literally: execl does not glob. */
        /* dhcpcd manages every NIC: the wireless interface for normal use, and
         * the wired dock port, which is the developer's link to the device and
         * must work without Wi-Fi. Wired is kept less preferred than Wi-Fi by
         * the metric in /etc/dhcpcd.conf, so bringing up the dock never steals
         * the default route. (An earlier version passed the wireless interface
         * plus -Z en*/-Z eth*, which meant the wired port never got a lease at
         * all - the opposite of the intent recorded in that comment.) */
        execl("/sbin/dhcpcd", "dhcpcd", "-B", "-q", "-m", "1000",
              (char *)NULL);
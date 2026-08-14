/*
 * playos-init/src/audio_debug.c — Best-effort audio subsystem diagnostics
 *
 * Writes a snapshot of the ALSA topology and the kernel audio log to
 * /data/log/audio-debug.log so on-device audio problems can be diagnosed
 * without a shell on the production image.
 *
 * This is deliberately defensive: every operation is best-effort and must
 * never abort or otherwise affect PID 1's boot sequence.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

#include "playos-init/init.h"

#define AUDIO_DEBUG_PATH "/data/log/audio-debug.log"
#define MAX_DUMP_BYTES   (512 * 1024)

static int dbg_fd = -1;
static size_t dbg_total = 0;

/* ── Output helpers ──────────────────────────────────────────────── */

static void dbg_write(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    int n;

    if (dbg_fd < 0 || dbg_total >= MAX_DUMP_BYTES)
        return;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0)
        return;
    if ((size_t)n > sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;

    ssize_t w = write(dbg_fd, buf, (size_t)n);
    (void)w;
    dbg_total += (size_t)n;
}

/* Copy a readable file into the dump (best-effort, bounded). */
static void dbg_copy_file(const char *path)
{
    int fd;
    char buf[4096];
    ssize_t n;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        dbg_write("--- %s: %s\n", path, strerror(errno));
        return;
    }

    dbg_write("=== %s ===\n", path);
    while (dbg_total < MAX_DUMP_BYTES &&
           (n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(dbg_fd, buf, (size_t)n);
        (void)w;
        dbg_total += (size_t)n;
    }
    close(fd);
    dbg_write("\n");
}

/* ── ALSA topology dump ──────────────────────────────────────────── */

static void dbg_dump_alsa(void)
{
    static const char *files[] = {
        "/proc/asound/cards",
        "/proc/asound/pcm",
        "/proc/asound/version",
        "/proc/asound/modules",
        NULL
    };
    DIR *dir;
    struct dirent *de;

    for (int i = 0; files[i] != NULL; i++)
        dbg_copy_file(files[i]);

    /* Per-card details: id, hwdep, codec#*, pcm* */
    dir = opendir("/proc/asound");
    if (dir != NULL) {
        while ((de = readdir(dir)) != NULL) {
            char path[320];
            if (strncmp(de->d_name, "card", 4) != 0)
                continue;

            snprintf(path, sizeof(path), "/proc/asound/%s/id", de->d_name);
            dbg_copy_file(path);

            snprintf(path, sizeof(path), "/proc/asound/%s/codec#0", de->d_name);
            dbg_copy_file(path);

            snprintf(path, sizeof(path), "/proc/asound/%s/codec#1", de->d_name);
            dbg_copy_file(path);

            snprintf(path, sizeof(path), "/proc/asound/%s/codec#2", de->d_name);
            dbg_copy_file(path);
        }
        closedir(dir);
    }
}

/* ── Directory listing dump ──────────────────────────────────────── */

/* List a directory's entry names, resolving symlink targets. Used for the
 * I2C topology so we can see whether the DesignWare controller registered an
 * adapter and whether the CS35L41 (CSC3551) amps enumerated. */
static void dbg_list_dir(const char *path, const char *heading)
{
    DIR *dir = opendir(path);
    struct dirent *de;

    if (dir == NULL) {
        dbg_write("--- %s: %s\n", path, strerror(errno));
        return;
    }

    dbg_write("=== %s ===\n", heading);
    while ((de = readdir(dir)) != NULL) {
        char link[320], target[320];
        ssize_t tl;

        if (de->d_name[0] == '.')
            continue;

        snprintf(link, sizeof(link), "%s/%s", path, de->d_name);
        tl = readlink(link, target, sizeof(target) - 1);
        if (tl >= 0) {
            target[tl] = '\0';
            dbg_write("%s -> %s\n", de->d_name, target);
        } else {
            dbg_write("%s\n", de->d_name);
        }
    }
    closedir(dir);
    dbg_write("\n");
}

/* ── I2C / ACPI amp-enumeration dump ─────────────────────────────── */

/* Read a small sysfs attribute into buf, newline-stripped. Returns length,
 * or -1 on error. */
static int dbg_read_attr(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0)
        return -1;
    n = read(fd, buf, size - 1);
    close(fd);
    if (n <= 0)
        return -1;

    buf[n] = '\0';
    char *nl = strchr(buf, '\n');
    if (nl)
        *nl = '\0';
    return (int)strlen(buf);
}

/* The CS35L41 amps are ACPI I2C devices (HID "CSC3551") on a DesignWare
 * controller. If they never bind, this shows whether the adapter is present,
 * which ACPI controller node each amp lives under (its `path`), and whether
 * the ACPI node has been bound to a physical I2C client (`physical_node`). */
static void dbg_dump_i2c(void)
{
    dbg_list_dir("/sys/class/i2c-adapter", "I2C adapters (/sys/class/i2c-adapter)");
    dbg_list_dir("/sys/bus/i2c/devices", "I2C devices (/sys/bus/i2c/devices)");
    dbg_list_dir("/sys/bus/platform/devices", "Platform devices (/sys/bus/platform/devices)");
    dbg_list_dir("/sys/bus/acpi/devices", "ACPI devices (/sys/bus/acpi/devices)");

    DIR *dir = opendir("/sys/bus/acpi/devices");
    struct dirent *de;

    dbg_write("=== ACPI CSC3551 devices ===\n");
    if (dir == NULL) {
        dbg_write("--- /sys/bus/acpi/devices: %s\n", strerror(errno));
        return;
    }

    int found = 0;
    while ((de = readdir(dir)) != NULL) {
        if (strncmp(de->d_name, "CSC3551", 7) != 0)
            continue;
        found++;

        char path[320], val[320];
        const char *attrs[] = { "hid", "uid", "status", "path" };
        const char *names[] = { "hid", "uid", "status", "path" };

        dbg_write("%s:", de->d_name);
        for (size_t i = 0; i < sizeof(attrs) / sizeof(attrs[0]); i++) {
            snprintf(path, sizeof(path),
                     "/sys/bus/acpi/devices/%s/%s", de->d_name, attrs[i]);
            if (dbg_read_attr(path, val, sizeof(val)) >= 0)
                dbg_write(" %s=%s", names[i], val);
        }

        /* physical_node symlink target shows the I2C client when bound. */
        snprintf(path, sizeof(path),
                 "/sys/bus/acpi/devices/%s/physical_node", de->d_name);
        char target[320];
        ssize_t tl = readlink(path, target, sizeof(target) - 1);
        if (tl >= 0) {
            target[tl] = '\0';
            dbg_write(" physical_node=%s", target);
        }
        dbg_write("\n");
    }
    if (!found)
        dbg_write("(none)\n");
    closedir(dir);
    dbg_write("\n");
}

/* ── Kernel log dump ─────────────────────────────────────────────── */

static void dbg_dump_kmsg(void)
{
    char buf[8192];
    ssize_t n;
    int fd;

    /* /proc/kmsg yields the unread kernel ring buffer (read-once). In the
     * production image klogd is not running, so this contains the full boot
     * trace. We dump it unfiltered: the previous audio-only filter hid the
     * DesignWare I2C / AMD PSP-CCP / deferred-probe lines that are needed to
     * see why the CS35L41 amps don't bind. */
    fd = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        dbg_write("--- /proc/kmsg: %s\n", strerror(errno));
        return;
    }

    dbg_write("=== kernel log (full) ===\n");
    while (dbg_total < MAX_DUMP_BYTES &&
           (n = read(fd, buf, sizeof(buf))) > 0) {
        ssize_t w = write(dbg_fd, buf, (size_t)n);
        (void)w;
        dbg_total += (size_t)n;
    }
    close(fd);
    dbg_write("\n");
}

/* ── Public entry point ──────────────────────────────────────────── */

void playos_audio_debug_dump(void)
{
    dbg_fd = open(AUDIO_DEBUG_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dbg_fd < 0)
        return; /* not fatal — diagnostics are optional */

    dbg_total = 0;

    dbg_write("playos audio diagnostics\n");
    dbg_write("=======================\n\n");

    dbg_dump_alsa();
    dbg_dump_i2c();
    dbg_dump_kmsg();

    fsync(dbg_fd);
    close(dbg_fd);
    dbg_fd = -1;
}

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
#define MAX_DUMP_BYTES   (256 * 1024)

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

/* ── Kernel audio log dump ───────────────────────────────────────── */

static void dbg_dump_kmsg(void)
{
    char buf[4096];
    ssize_t n;
    int fd;

    /* /proc/kmsg yields the unread kernel ring buffer (read-once).
     * In the production image klogd is not running, so this contains the
     * full boot trace including the snd_hda / realtek / cs35l41 probe. */
    fd = open("/proc/kmsg", O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        dbg_write("--- /proc/kmsg: %s\n", strerror(errno));
        return;
    }

    dbg_write("=== kernel audio log (filtered) ===\n");
    while (dbg_total < MAX_DUMP_BYTES &&
           (n = read(fd, buf, sizeof(buf))) > 0) {
        /* kmsg records are '<prio>,seq,ts,flags;text' — keep only lines
         * mentioning the audio stack. */
        char *line = buf;
        char *end;
        buf[n] = '\0';
        while ((end = strchr(line, '\n')) != NULL && dbg_total < MAX_DUMP_BYTES) {
            *end = '\0';
            if (strstr(line, "snd") || strstr(line, "hda") ||
                strstr(line, "HDA") || strstr(line, "alc") ||
                strstr(line, "ALC") || strstr(line, "cs35l41") ||
                strstr(line, "CS35L41") || strstr(line, "azx") ||
                strstr(line, "soundwire") || strstr(line, "sof") ||
                strstr(line, "codec")) {
                dbg_write("%s\n", line);
            }
            line = end + 1;
        }
    }
    close(fd);
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
    dbg_dump_kmsg();

    fsync(dbg_fd);
    close(dbg_fd);
    dbg_fd = -1;
}

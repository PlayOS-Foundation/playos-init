/*
 * playos-init/src/logging.c — Bounded ring-buffer logging
 *
 * Logs are written to a ring buffer in memory and flushed to
 * /run/playos/log/init.log. If the log file cannot be opened,
 * fall back to stderr/console. Must never crash PID 1.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <sys/types.h>

#include "playos-init/init.h"

/* ── Timestamp helper ────────────────────────────────────────────── */

static void get_timestamp(char *buf, size_t size)
{
    struct timespec ts;
    /* Use CLOCK_MONOTONIC for uptime-based timestamps (no RTC dependency) */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        snprintf(buf, size, "[%10lu]", (unsigned long)time(NULL));
        return;
    }
    snprintf(buf, size, "[%10lu.%03lu]",
             (unsigned long)ts.tv_sec,
             (unsigned long)(ts.tv_nsec / 1000000));
}

/* ── Ring buffer append ──────────────────────────────────────────── */

static void ring_append(struct playos_init_state *s, const char *line)
{
    int len = (int)strlen(line);
    int pos = s->log_write_pos;

    for (int i = 0; i < len; i++) {
        s->log_ring[pos] = line[i];
        pos++;
        if (pos >= PLAYOS_LOG_RING_SIZE) {
            pos = 0;
            s->log_wrapped = 1;
        }
    }

    /* Ensure newline */
    if (len > 0 && line[len - 1] != '\n') {
        s->log_ring[pos] = '\n';
        pos++;
        if (pos >= PLAYOS_LOG_RING_SIZE) {
            pos = 0;
            s->log_wrapped = 1;
        }
    }

    s->log_write_pos = pos;
}

/* ── Initialize logging ──────────────────────────────────────────── */

void playos_log_init(struct playos_init_state *s)
{
    /* Create log directory */
    mkdir("/run/playos", 0755);
    mkdir("/run/playos/log", 0755);

    /* Try to open the log file */
    s->log_fd = open("/run/playos/log/init.log",
                     O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (s->log_fd < 0) {
        /* Fall back to stderr */
        dprintf(STDERR_FILENO,
                "playos-init: cannot open init.log: %s (using stderr)\n",
                strerror(errno));
    }

    memset(s->log_ring, 0, sizeof(s->log_ring));
    s->log_write_pos = 0;
    s->log_wrapped = 0;
}

/* ── Log write ───────────────────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
{
    char ts[32];
    char line[512];
    int line_len;

    get_timestamp(ts, sizeof(ts));

    /* Build the log prefix: [timestamp] [TAG] */
    line_len = snprintf(line, sizeof(line), "%s [%-5s] ", ts, tag);

    /* Append the formatted message */
    va_list ap;
    va_start(ap, fmt);
    line_len += vsnprintf(line + line_len, sizeof(line) - line_len, fmt, ap);
    va_end(ap);

    /* Append newline if there's room */
    if (line_len < (int)sizeof(line) - 1) {
        line[line_len] = '\n';
        line[line_len + 1] = '\0';
    }

    /* Write to ring buffer */
    ring_append(s, line);

    /* Write to log file (best-effort) */
    if (s->log_fd >= 0) {
        /* Ignore errors — logging must not crash PID 1 */
        ssize_t written = write(s->log_fd, line, strlen(line));
        (void)written;
    }

    /*
     * Also write to stderr/console during early boot
     * so it's visible on the serial console.
     */
    dprintf(STDERR_FILENO, "%s", line);
}

/* ── Fatal log — writes then halts ───────────────────────────────── */

void playos_log_fatal(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
{
    char ts[32];
    char line[512];

    get_timestamp(ts, sizeof(ts));
    int line_len = snprintf(line, sizeof(line), "%s [FATAL] [%s] ", ts, tag);

    va_list ap;
    va_start(ap, fmt);
    line_len += vsnprintf(line + line_len, sizeof(line) - line_len, fmt, ap);
    va_end(ap);

    if (line_len < (int)sizeof(line) - 1) {
        line[line_len] = '\n';
        line[line_len + 1] = '\0';
    }

    /* Write to all available outputs */
    ring_append(s, line);
    if (s->log_fd >= 0) {
        write(s->log_fd, line, strlen(line));
    }
    dprintf(STDERR_FILENO, "%s", line);

    /* Sync and halt */
    sync();
    reboot(RB_HALT_SYSTEM);
}

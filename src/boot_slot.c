/*
 * playos-init/src/boot_slot.c — A/B boot slot accounting (Sprint 11)
 *
 * boot.json is a tiny hand-maintained JSON object. Parsing and writing are
 * done inline (no JSON library) to keep PID 1 dependency-free. The format is
 * deliberately forgiving on read: a missing or malformed file falls back to
 * a safe default and never halts boot.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>

#include "playos-init/boot_slot.h"

/* ── Defaults ─────────────────────────────────────────────────────── */

static void default_state(struct boot_slot_state *st)
{
    memset(st, 0, sizeof(*st));
    st->v = 1;
    st->active_slot = 'a';
    snprintf(st->slot_a.version, sizeof(st->slot_a.version), "0.1.0");
    st->slot_a.boot_count = 0;
    snprintf(st->slot_a.health, sizeof(st->slot_a.health), "good");
    st->slot_b.version[0] = '\0';
    st->slot_b.boot_count = 0;
    snprintf(st->slot_b.health, sizeof(st->slot_b.health), "empty");
}

/* ── Minimal JSON helpers ─────────────────────────────────────────── */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    return p;
}

static int parse_string_field(const char *json, const char *key,
                              char *out, size_t outsz)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;

    const char *p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p = strchr(p + 1, '"');
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
    return 0;
}

static int parse_int_field(const char *json, const char *key, int *out)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;

    const char *p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p, ':');
    if (!p)
        return -1;
    p = skip_ws(p + 1);

    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p)
        return -1;

    *out = (int)v;
    return 0;
}

/* Extract the `{...}` object following `"name":` and parse it. */
static int parse_slot(const char *json, const char *name,
                      struct boot_slot_info *info)
{
    char needle[64];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", name);
    if (n < 0 || (size_t)n >= sizeof(needle))
        return -1;

    const char *p = strstr(json, needle);
    if (!p)
        return -1;
    p = strchr(p, '{');
    if (!p)
        return -1;

    const char *end = p;
    int depth = 0;
    while (*end) {
        if (*end == '{')
            depth++;
        else if (*end == '}') {
            depth--;
            if (depth == 0)
                break;
        }
        end++;
    }
    if (depth != 0 || *end != '}')
        return -1;

    size_t len = (size_t)(end - p) + 1;
    if (len >= 256)
        return -1;

    char buf[256];
    memcpy(buf, p, len);
    buf[len] = '\0';

    if (parse_string_field(buf, "version", info->version,
                           sizeof(info->version)) != 0)
        return -1;
    if (parse_int_field(buf, "boot_count", &info->boot_count) != 0)
        return -1;
    if (parse_string_field(buf, "health", info->health,
                           sizeof(info->health)) != 0)
        return -1;
    return 0;
}

static struct boot_slot_info *active_info(struct boot_slot_state *st)
{
    return (st->active_slot == 'b') ? &st->slot_b : &st->slot_a;
}

/* ── Public API ───────────────────────────────────────────────────── */

int boot_slot_read(const char *path, struct boot_slot_state *out)
{
    default_state(out);

    if (!path)
        return -1;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "playos-init: boot.json open failed: %s\n",
                strerror(errno));
        return -1;
    }

    char buf[2048];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        dprintf(STDERR_FILENO, "playos-init: boot.json read failed\n");
        return -1;
    }
    buf[n] = '\0';

    int v = 0;
    if (parse_int_field(buf, "v", &v) != 0 || v != 1) {
        dprintf(STDERR_FILENO, "playos-init: boot.json has bad schema version\n");
        return -1;
    }

    char active[2];
    if (parse_string_field(buf, "active_slot", active, sizeof(active)) != 0 ||
        (active[0] != 'a' && active[0] != 'b')) {
        dprintf(STDERR_FILENO, "playos-init: boot.json has bad active_slot\n");
        return -1;
    }

    if (parse_slot(buf, "slot_a", &out->slot_a) != 0 ||
        parse_slot(buf, "slot_b", &out->slot_b) != 0) {
        dprintf(STDERR_FILENO, "playos-init: boot.json slot parse failed\n");
        return -1;
    }

    out->v = v;
    out->active_slot = active[0];
    return 0;
}

int boot_slot_write(const char *path, const struct boot_slot_state *st)
{
    if (!path || !st)
        return -1;

    char body[1024];
    int n = snprintf(body, sizeof(body),
        "{\n"
        "  \"v\":%d,\n"
        "  \"active_slot\":\"%c\",\n"
        "  \"slot_a\":{\"version\":\"%s\",\"boot_count\":%d,\"health\":\"%s\"},\n"
        "  \"slot_b\":{\"version\":\"%s\",\"boot_count\":%d,\"health\":\"%s\"}\n"
        "}\n",
        st->v, st->active_slot,
        st->slot_a.version, st->slot_a.boot_count, st->slot_a.health,
        st->slot_b.version, st->slot_b.boot_count, st->slot_b.health);
    if (n < 0 || (size_t)n >= sizeof(body))
        return -1;

    char tmp[PATH_MAX];
    n = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    if (n < 0 || (size_t)n >= sizeof(tmp))
        return -1;

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "playos-init: boot.json.tmp open failed: %s\n",
                strerror(errno));
        return -1;
    }

    size_t len = strlen(body);
    ssize_t w = write(fd, body, len);
    int rc = 0;
    if (w != (ssize_t)len) {
        dprintf(STDERR_FILENO, "playos-init: boot.json write failed: %s\n",
                strerror(errno));
        rc = -1;
    }
    if (rc == 0 && fsync(fd) != 0)
        rc = -1;
    close(fd);

    if (rc == 0 && rename(tmp, path) != 0) {
        dprintf(STDERR_FILENO, "playos-init: boot.json rename failed: %s\n",
                strerror(errno));
        rc = -1;
    }

    if (rc != 0)
        unlink(tmp);

    return rc;
}

int boot_slot_increment(const char *path, struct boot_slot_state *st)
{
    if (boot_slot_read(path, st) != 0) {
        dprintf(STDERR_FILENO,
                "playos-init: boot.json unreadable — skipping boot count\n");
        return 0;
    }

    struct boot_slot_info *active = active_info(st);
    active->boot_count++;

    if (boot_slot_write(path, st) != 0) {
        dprintf(STDERR_FILENO,
                "playos-init: failed to persist boot count\n");
        return 0;
    }

    return (active->boot_count >= 3 && strcmp(active->health, "good") != 0);
}

int boot_slot_mark_good(const char *path, struct boot_slot_state *st)
{
    if (boot_slot_read(path, st) != 0)
        return -1;

    struct boot_slot_info *active = active_info(st);
    snprintf(active->health, sizeof(active->health), "good");
    active->boot_count = 0;

    return boot_slot_write(path, st);
}

int boot_slot_rollback(const char *path, struct boot_slot_state *st)
{
    struct boot_slot_info *old = active_info(st);
    snprintf(old->health, sizeof(old->health), "bad");

    char new_slot = (st->active_slot == 'a') ? 'b' : 'a';
    st->active_slot = new_slot;

    struct boot_slot_info *newactive = active_info(st);
    snprintf(newactive->health, sizeof(newactive->health), "pending");
    newactive->boot_count = 0;
    /* Keep newactive->version — it describes the image we are rolling into. */

    return boot_slot_write(path, st);
}

void playos_boot_mark_good_once(struct playos_init_state *s)
{
    if (!s || s->boot_slot_marked_good)
        return;

    s->boot_slot_marked_good = 1;

    if (!s->efi_mounted)
        return;

    struct boot_slot_state bs;
    if (boot_slot_mark_good(PLAYOS_BOOT_JSON_PATH, &bs) != 0)
        dprintf(STDERR_FILENO,
                "playos-init: failed to mark boot slot good\n");
}

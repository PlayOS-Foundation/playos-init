/*
 * playos-init/src/thermal.c — Thermal monitoring and EPP profile control
 *
 * Sprint 9: 1 Hz thermal monitor, performance-profile (EPP) writer,
 * thermal state machine, and graceful over-temperature handling.
 *
 * Runs entirely on the PID 1 supervision loop: playos_thermal_tick() is
 * called once per second from main.c. No worker threads, matching the
 * existing single-threaded init design.
 *
 * All /sys reads and writes are best-effort: on hosts (or kernels) without
 * the expected sysfs nodes the helpers return -1 and callers keep going.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>

#include "playos-init/init.h"
#include "playos-init/thermal.h"
#include "playos-init/shutdown.h"
#include "playos-init/ipc_handler.h"

/* IPC framing/type constants (bundled from playos-runtime) */
#include "ipc.h"

/* ── External dependencies ───────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Monotonic clock ─────────────────────────────────────────────── */

static long long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

/* ── Sysfs helpers ───────────────────────────────────────────────── */

/* Read the first integer from `path`. Returns -1 on any error. */
static int read_int_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    long v = -1;
    if (fscanf(f, "%ld", &v) != 1)
        v = -1;
    fclose(f);
    return (int)v;
}

/* Read a small string, strip the trailing newline. Returns -1 on error. */
static int read_str_file(const char *path, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return -1;

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    if (fgets(out, (int)outsz, f) == NULL) {
        fclose(f);
        return -1;
    }
    fclose(f);

    size_t len = strlen(out);
    while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r'))
        out[--len] = '\0';
    return (int)len;
}

/*
 * Find a thermal zone by its `type` string and return its `temp` value
 * in degrees Celsius. Returns -1 if no matching zone exists.
 */
static int read_thermal_zone_by_type(const char *want)
{
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir)
        return -1;

    struct dirent *ent;
    int result = -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "thermal_zone", 12) != 0)
            continue;

        char type_path[384];
        char type[64] = {0};
        snprintf(type_path, sizeof(type_path),
                 "/sys/class/thermal/%s/type", ent->d_name);
        if (read_str_file(type_path, type, sizeof(type)) < 0)
            continue;

        if (strcmp(type, want) != 0)
            continue;

        char temp_path[384];
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/thermal/%s/temp", ent->d_name);
        int millic = read_int_file(temp_path);
        if (millic > 0)
            result = millic / 1000;
        break;
    }

    closedir(dir);
    return result;
}

/*
 * Read the AMD GPU junction temperature (hwmon `amdgpu` / `temp1_input`).
 * Returns degrees Celsius, or -1 if unavailable.
 */
static int read_hwmon_gpu_temp(void)
{
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir)
        return -1;

    struct dirent *ent;
    int result = -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        char name_path[384];
        char name[64] = {0};
        snprintf(name_path, sizeof(name_path),
                 "/sys/class/hwmon/%s/name", ent->d_name);
        if (read_str_file(name_path, name, sizeof(name)) < 0)
            continue;

        if (strcmp(name, "amdgpu") != 0)
            continue;

        char temp_path[384];
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/hwmon/%s/temp1_input", ent->d_name);
        int millic = read_int_file(temp_path);
        if (millic > 0)
            result = millic / 1000;
        break;
    }

    closedir(dir);
    return result;
}

/*
 * Read the AMD CPU package temperature from the k10temp hwmon node.
 * The Ally exposes CPU temp via k10temp/temp1_input rather than a named
 * thermal zone, so fall back to it when thermal-zone lookups fail.
 * Returns degrees Celsius, or -1 if unavailable.
 */
static int read_hwmon_cpu_temp(void)
{
    DIR *dir = opendir("/sys/class/hwmon");
    if (!dir)
        return -1;

    struct dirent *ent;
    int result = -1;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hwmon", 5) != 0)
            continue;

        char name_path[384];
        char name[64] = {0};
        snprintf(name_path, sizeof(name_path),
                 "/sys/class/hwmon/%s/name", ent->d_name);
        if (read_str_file(name_path, name, sizeof(name)) < 0)
            continue;

        if (strcmp(name, "k10temp") != 0)
            continue;

        char temp_path[384];
        snprintf(temp_path, sizeof(temp_path),
                 "/sys/class/hwmon/%s/temp1_input", ent->d_name);
        int millic = read_int_file(temp_path);
        if (millic > 0)
            result = millic / 1000;
        break;
    }

    closedir(dir);
    return result;
}

/* ── EPP writer ──────────────────────────────────────────────────── */

static const char *epp_wire_name(int profile)
{
    switch (profile) {
    case PLAYOS_PERF_PROFILE_POWER_SAVE:
        return "power";
    case PLAYOS_PERF_PROFILE_PERFORMANCE:
        return "performance";
    case PLAYOS_PERF_PROFILE_BALANCED:
    default:
        return "balance_performance";
    }
}

/*
 * Write the requested EPP string to every online CPU's cpufreq policy.
 * Returns 0 if at least one CPU accepted the write, -1 if none did.
 */
static int write_epp_profile(int profile)
{
    const char *epp = epp_wire_name(profile);
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (!dir)
        return -1;

    struct dirent *ent;
    int wrote_any = 0;

    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, "cpu", 3) != 0)
            continue;
        if (name[3] < '0' || name[3] > '9')
            continue;

        char path[384];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/%s/cpufreq/energy_performance_preference",
                 name);

        FILE *f = fopen(path, "w");
        if (!f)
            continue;

        if (fputs(epp, f) >= 0)
            wrote_any = 1;
        fclose(f);
    }

    closedir(dir);
    return wrote_any ? 0 : -1;
}

/*
 * Read the current EPP value from cpu0 and map it back to a profile enum.
 * Unrecognised/missing values default to BALANCED.
 */
static int read_current_epp_profile(void)
{
    char path[384] =
        "/sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference";
    char value[64] = {0};
    if (read_str_file(path, value, sizeof(value)) < 0)
        return PLAYOS_PERF_PROFILE_BALANCED;

    if (strcmp(value, "power") == 0)
        return PLAYOS_PERF_PROFILE_POWER_SAVE;
    if (strcmp(value, "performance") == 0)
        return PLAYOS_PERF_PROFILE_PERFORMANCE;
    return PLAYOS_PERF_PROFILE_BALANCED;
}

/* ── Minimal JSON int parser ─────────────────────────────────────── */

/*
 * Parse a top-level integer field of the form "key":123.
 * Returns 0 and stores the value in *out on success, -1 if absent.
 */
static int json_int_field(const char *json, const char *key, int *out)
{
    if (!json || !key || !out)
        return -1;

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
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p)
        return -1;

    *out = (int)v;
    return 0;
}

/* ── Thermal thresholds ──────────────────────────────────────────── */

static int warm_c      = 75;
static int hot_c       = 85;
static int critical_c  = 95;

/* ── Public API ──────────────────────────────────────────────────── */

void playos_thermal_init(struct playos_init_state *s)
{
    if (!s)
        return;

    s->thermal_initialized = 0;
    s->thermal_state = PLAYOS_THERMAL_STATE_NORMAL;
    s->thermal_critical_since_ms = 0;

    /* Load optional thresholds from the persistent config overlay.
     * Any failure falls back to the compiled-in defaults above. */
    FILE *f = fopen("/data/config/thermal.json", "r");
    if (f) {
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        (void)json_int_field(buf, "warm_c", &warm_c);
        (void)json_int_field(buf, "hot_c", &hot_c);
        (void)json_int_field(buf, "critical_c", &critical_c);

        /* Sanity: keep the machine usable even with a bad config. */
        if (warm_c <= 0)
            warm_c = 75;
        if (hot_c <= warm_c)
            hot_c = warm_c + 5;
        if (critical_c <= hot_c)
            critical_c = hot_c + 5;
    }

    /* Apply the safe Balanced default at boot instead of inheriting the
     * kernel's default EPP (which is "performance" under Active/EPP). */
    if (access("/sys", F_OK) == 0) {
        if (write_epp_profile(PLAYOS_PERF_PROFILE_BALANCED) == 0)
            s->active_profile = PLAYOS_PERF_PROFILE_BALANCED;
        else
            s->active_profile = read_current_epp_profile();
    }

    s->thermal_initialized = 1;

    playos_log_write(s, "thermal",
                     "initialized (warm=%dC hot=%dC critical=%dC profile=%d)",
                     warm_c, hot_c, critical_c, s->active_profile);
}

void playos_thermal_tick(struct playos_init_state *s)
{
    if (!s || !s->thermal_initialized)
        return;

    /* Host / non-sysfs kernels have no meaningful thermal nodes. */
    if (access("/sys", F_OK) != 0)
        return;

    int cpu_temp = read_thermal_zone_by_type("x86_pkg_temp");
    if (cpu_temp < 0)
        cpu_temp = read_thermal_zone_by_type("cpu_thermal");
    if (cpu_temp < 0)
        cpu_temp = read_hwmon_cpu_temp();
    int gpu_temp = read_hwmon_gpu_temp();

    /* Prefer the highest available reading — that is what actually
     * governs throttling/shutdown on the Ally. */
    int temp = cpu_temp;
    if (gpu_temp > temp)
        temp = gpu_temp;

    if (temp < 0) {
        /* No readable sensor; nothing useful to do this tick. */
        return;
    }

    int new_state;
    if (temp < warm_c)
        new_state = PLAYOS_THERMAL_STATE_NORMAL;
    else if (temp < hot_c)
        new_state = PLAYOS_THERMAL_STATE_WARM;
    else if (temp < critical_c)
        new_state = PLAYOS_THERMAL_STATE_HOT;
    else
        new_state = PLAYOS_THERMAL_STATE_CRITICAL;

    if (new_state != s->thermal_state) {
        int old_state = s->thermal_state;
        s->thermal_state = new_state;

        if (new_state == PLAYOS_THERMAL_STATE_CRITICAL) {
            s->thermal_critical_since_ms = monotonic_ms();
        } else if (old_state == PLAYOS_THERMAL_STATE_CRITICAL) {
            s->thermal_critical_since_ms = 0;
        }

        playos_log_write(s, "thermal",
                         "state %d -> %d (cpu=%dC gpu=%dC)",
                         old_state, new_state, cpu_temp, gpu_temp);

        char extra[64];
        snprintf(extra, sizeof(extra), "\"state\":%d", new_state);
        playos_ipc_emit_to_shell(s, PLAYOS_IPC_TYPE_THERMAL_STATE_CHANGED,
                                 extra);
    }

    /* CRITICAL: force power-save immediately. */
    if (s->thermal_state == PLAYOS_THERMAL_STATE_CRITICAL) {
        if (s->active_profile != PLAYOS_PERF_PROFILE_POWER_SAVE)
            playos_thermal_request_profile(s, PLAYOS_PERF_PROFILE_POWER_SAVE);

        /* 10 s at CRITICAL without recovering -> orderly shutdown. */
        if (s->thermal_critical_since_ms > 0
            && (monotonic_ms() - s->thermal_critical_since_ms) >= 10000LL) {
            playos_log_write(s, "thermal",
                             "critical temperature persisted 10s — shutting down");
            playos_shutdown(s, 0);
        }
        return;
    }

    /* HOT: back off an explicit performance request. */
    if (s->thermal_state == PLAYOS_THERMAL_STATE_HOT
        && s->active_profile == PLAYOS_PERF_PROFILE_PERFORMANCE) {
        playos_thermal_request_profile(s, PLAYOS_PERF_PROFILE_BALANCED);
    }
}

int playos_thermal_request_profile(struct playos_init_state *s, int profile)
{
    if (!s)
        return PLAYOS_THERMAL_REQ_INVALID;

    if (profile < PLAYOS_PERF_PROFILE_BALANCED
        || profile > PLAYOS_PERF_PROFILE_PERFORMANCE) {
        playos_log_write(s, "thermal", "rejecting invalid profile %d", profile);
        return PLAYOS_THERMAL_REQ_INVALID;
    }

    /* Thermal protection wins over user preference. */
    if (profile == PLAYOS_PERF_PROFILE_PERFORMANCE
        && s->thermal_state >= PLAYOS_THERMAL_STATE_HOT) {
        playos_log_write(s, "thermal",
                         "denied performance profile while HOT/CRITICAL");
        return PLAYOS_THERMAL_REQ_DENIED;
    }

    if (access("/sys", F_OK) == 0 && write_epp_profile(profile) != 0) {
        playos_log_write(s, "thermal", "EPP write failed for profile %d",
                         profile);
        return PLAYOS_THERMAL_REQ_EPP_FAIL;
    }

    s->active_profile = profile;
    playos_log_write(s, "thermal", "active performance profile -> %d", profile);

    char extra[64];
    snprintf(extra, sizeof(extra), "\"profile\":%d", profile);
    playos_ipc_emit_to_shell(s, PLAYOS_IPC_TYPE_PERF_PROFILE_CHANGED, extra);
    return PLAYOS_THERMAL_REQ_OK;
}

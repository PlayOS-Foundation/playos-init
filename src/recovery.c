/*
 * recovery.c — recovery entry helpers (S14-T6)
 *
 * Detects a held-button recovery request by reading evdev key state directly
 * from /dev/input during early boot. Triggers:
 *
 *   - Gamepad START + SELECT held 5 s   (primary trigger on ROG Ally — the
 *                             firmware never sees the internal controller)
 *   - Volume Up held 5 s
 *   - Volume Down held 5 s   (on ROG Ally the firmware captures Vol-Down at
 *                             power-on and enters BIOS, so this only works on
 *                             devices whose firmware does not intercept it)
 *
 * Normal boots are not delayed by the instant check; the late watch adds a
 * bounded listen window so a hold that starts slightly after boot begins is
 * still caught.
 */
#define _DEFAULT_SOURCE 1
#include "playos-init/recovery.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define RECOVERY_HOLD_MS 5000    /* default (volume triggers) */
#define RECOVERY_GAMEPAD_HOLD_MS 2000 /* START+SELECT is unambiguous at boot */
#define RECOVERY_POLL_MS 100

static const unsigned int trigger_gamepad[] = { BTN_SELECT, BTN_START };
static const unsigned int trigger_volume_up[] = { KEY_VOLUMEUP };
static const unsigned int trigger_volume_down[] = { KEY_VOLUMEDOWN };

struct trigger {
    const unsigned int *keys;
    int count;
    int hold_ms;
    const char *name;
};

static const struct trigger triggers[] = {
    { trigger_gamepad,     2, RECOVERY_GAMEPAD_HOLD_MS, "start+select" },
    { trigger_volume_up,   1, RECOVERY_HOLD_MS,         "volume up" },
    { trigger_volume_down, 1, RECOVERY_HOLD_MS,         "volume down" },
};

static int
evdev_supports_keys(int fd, const unsigned int *keys, int count)
{
    unsigned char bits[(KEY_MAX / 8) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return 0;
    for (int i = 0; i < count; i++) {
        if (!((bits[keys[i] / 8] >> (keys[i] % 8)) & 1))
            return 0;
    }
    return 1;
}

static int
evdev_keys_down(int fd, const unsigned int *keys, int count)
{
    unsigned char kbd[(KEY_MAX / 8) + 1];
    memset(kbd, 0, sizeof(kbd));
    if (ioctl(fd, EVIOCGKEY(sizeof(kbd)), kbd) < 0)
        return 0;
    for (int i = 0; i < count; i++) {
        if (!((kbd[keys[i] / 8] >> (keys[i] % 8)) & 1))
            return 0;
    }
    return 1;
}

/* Scan /dev/input for a device that has a recovery trigger currently held.
 * On success returns the open fd and fills *out_tr; caller closes fd. */
static int
find_held_trigger(int *out_fd, const struct trigger **out_tr)
{
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return -1;

    int found_fd = -1;
    const struct trigger *found = NULL;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int f = open(path, O_RDONLY | O_NONBLOCK);
        if (f < 0)
            continue;

        for (size_t t = 0; t < sizeof(triggers) / sizeof(triggers[0]); t++) {
            const struct trigger *tr = &triggers[t];
            if (evdev_supports_keys(f, tr->keys, tr->count) &&
                evdev_keys_down(f, tr->keys, tr->count)) {
                found_fd = f;
                found = tr;
                break;
            }
        }
        if (found_fd >= 0)
            break;
        close(f);
    }
    closedir(dir);

    if (found_fd < 0 || found == NULL)
        return -1;
    *out_fd = found_fd;
    *out_tr = found;
    return 0;
}

/* Require the trigger to stay held for its hold_ms duration. */
static int
confirm_hold(int fd, const struct trigger *tr)
{
    int steps = tr->hold_ms / RECOVERY_POLL_MS;
    for (int i = 0; i < steps; i++) {
        usleep(RECOVERY_POLL_MS * 1000);
        if (!evdev_keys_down(fd, tr->keys, tr->count))
            return 0;
    }
    return 1;
}

int
playos_recovery_button_held(void)
{
    int fd = -1;
    const struct trigger *tr = NULL;
    if (find_held_trigger(&fd, &tr) != 0)
        return 0;
    int held = confirm_hold(fd, tr);
    close(fd);
    return held;
}

int
playos_recovery_button_watch(int listen_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long deadline_ms = (long long)ts.tv_sec * 1000 +
                            ts.tv_nsec / 1000000 + listen_ms;

    while (1) {
        int fd = -1;
        const struct trigger *tr = NULL;
        if (find_held_trigger(&fd, &tr) == 0) {
            int held = confirm_hold(fd, tr);
            close(fd);
            if (held)
                return 1;
            /* Released early — keep watching for a fresh press. */
        }
        clock_gettime(CLOCK_MONOTONIC, &ts);
        long long now_ms = (long long)ts.tv_sec * 1000 +
                           ts.tv_nsec / 1000000;
        if (now_ms >= deadline_ms)
            return 0;
        usleep(RECOVERY_POLL_MS * 1000);
    }
}

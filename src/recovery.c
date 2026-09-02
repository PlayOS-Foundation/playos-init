/*
 * recovery.c — recovery entry helpers (S14-T6)
 *
 * Detects a held-button recovery request by reading evdev key state directly
 * from /dev/input during early boot. Triggers:
 *
 *   - Volume Up held 5 s
 *   - Volume Down held 5 s   (on ROG Ally the firmware captures Vol-Down at
 *                             power-on and enters BIOS, so this only works on
 *                             devices whose firmware does not intercept it)
 *   - Gamepad START + SELECT held 5 s   (primary trigger on ROG Ally — the
 *                             firmware never sees the internal controller)
 *
 * Normal boots are not delayed: if no trigger is already held when init
 * reaches this check, we return immediately.
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
#include <unistd.h>

#define RECOVERY_HOLD_MS 5000
#define RECOVERY_POLL_STEPS 50 /* 50 x 100ms = 5s */

static const unsigned int trigger_volume_up[] = { KEY_VOLUMEUP };
static const unsigned int trigger_volume_down[] = { KEY_VOLUMEDOWN };
static const unsigned int trigger_gamepad[] = { BTN_SELECT, BTN_START };

struct trigger {
    const unsigned int *keys;
    int count;
    const char *name;
};

static const struct trigger triggers[] = {
    { trigger_volume_up,   1, "volume up" },
    { trigger_volume_down, 1, "volume down" },
    { trigger_gamepad,     2, "start+select" },
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

int
playos_recovery_button_held(void)
{
    /* Fast pre-check: a trigger must already be held at boot. This keeps
     * normal boots free of any added delay. */
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return 0;

    int fd = -1;
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
                fd = f;
                found = tr;
                break;
            }
        }
        if (fd >= 0)
            break;
        close(f);
    }
    closedir(dir);

    if (fd < 0 || found == NULL)
        return 0;

    /* Hold window: require the same trigger to stay pressed for 5 s. */
    int held = 1;
    for (int i = 0; i < RECOVERY_POLL_STEPS; i++) {
        usleep(RECOVERY_HOLD_MS / RECOVERY_POLL_STEPS * 1000);
        if (!evdev_keys_down(fd, found->keys, found->count)) {
            held = 0;
            break;
        }
    }
    close(fd);
    return held;
}

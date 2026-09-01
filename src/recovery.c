/*
 * recovery.c — recovery entry helpers (S14-T6)
 *
 * Detects the "hold Volume Down for 5 seconds at boot" recovery request by
 * reading evdev key state directly from /dev/input. Normal boots are not
 * delayed: if volume-down is not already held when init reaches this check,
 * we return immediately.
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

static int
evdev_supports_key(int fd, unsigned int key)
{
    unsigned char bits[(KEY_MAX / 8) + 1];
    memset(bits, 0, sizeof(bits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0)
        return 0;
    return (bits[key / 8] >> (key % 8)) & 1;
}

static int
evdev_key_down(int fd, unsigned int key)
{
    unsigned char keys[(KEY_MAX / 8) + 1];
    memset(keys, 0, sizeof(keys));
    if (ioctl(fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
        return 0;
    return (keys[key / 8] >> (key % 8)) & 1;
}

int
playos_recovery_button_held(void)
{
    /* Fast pre-check: volume-down must already be held at boot. This keeps
     * normal boots free of any added delay. */
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return 0;

    int fd = -1;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL) {
        if (strncmp(e->d_name, "event", 5) != 0)
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        int f = open(path, O_RDONLY | O_NONBLOCK);
        if (f < 0)
            continue;
        if (evdev_supports_key(f, KEY_VOLUMEDOWN) &&
            evdev_key_down(f, KEY_VOLUMEDOWN)) {
            fd = f;
            break;
        }
        close(f);
    }
    closedir(dir);

    if (fd < 0)
        return 0;

    /* Hold window: require volume-down to stay pressed for the full 5s. */
    int held = 1;
    for (int i = 0; i < RECOVERY_POLL_STEPS; i++) {
        usleep(RECOVERY_HOLD_MS / RECOVERY_POLL_STEPS * 1000);
        if (!evdev_key_down(fd, KEY_VOLUMEDOWN)) {
            held = 0;
            break;
        }
    }
    close(fd);
    return held;
}

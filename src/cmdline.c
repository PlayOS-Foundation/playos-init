/*
 * playos-init/src/cmdline.c — kernel command-line parsing (pure helpers)
 *
 * Kept separate from mount.c so the parsers can be unit-tested on the host
 * without dragging in the mount/init dependencies. The runtime wrappers that
 * read /proc/cmdline live in mount.c next to the existing flag helpers.
 */
#include <stddef.h>
#include <string.h>

#include "playos-init/cmdline.h"

#define PLAYOS_CMDLINE_AUTOSTART_KEY "playos.autostart="

/* A /proc/cmdline token is whitespace-delimited. */
static int cmdline_is_sep(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int playos_cmdline_autostart(const char *cmdline, char *out, size_t outsz)
{
    const char *key = PLAYOS_CMDLINE_AUTOSTART_KEY;
    size_t key_len = sizeof(PLAYOS_CMDLINE_AUTOSTART_KEY) - 1;

    if (!cmdline || !out || outsz == 0)
        return 0;

    out[0] = '\0';

    for (const char *p = cmdline; (p = strstr(p, key)) != NULL; p += key_len) {
        /* Only a whole token counts: not the tail of a longer one such as
         * `foo=playos.autostart=x`. */
        if (p != cmdline && !cmdline_is_sep(p[-1]))
            continue;

        const char *val = p + key_len;
        size_t len = strcspn(val, " \t\r\n");

        if (len == 0)
            return -1; /* `playos.autostart=` with no game id */
        if (len >= outsz)
            return -1; /* does not fit */

        memcpy(out, val, len);
        out[len] = '\0';
        return (int)len;
    }

    return 0;
}

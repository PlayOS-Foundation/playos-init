/*
 * playos-init/cmdline.h — kernel command-line parsing (pure helpers)
 */
#ifndef PLAYOS_INIT_CMDLINE_H
#define PLAYOS_INIT_CMDLINE_H

#include <stddef.h>

/*
 * Extract the value of `playos.autostart=<game-id>` from a command-line
 * string (S15-T7).
 *
 * Pure — it never touches /proc — so it is host-testable. The token must be
 * whitespace-delimited and the key must match exactly. On success the value
 * is copied into `out`, NUL-terminated, and its length (> 0) is returned.
 * Returns 0 when the token is absent, or -1 when it is present but the value
 * is empty or does not fit in `outsz` (including the NUL terminator).
 */
int playos_cmdline_autostart(const char *cmdline, char *out, size_t outsz);

#endif /* PLAYOS_INIT_CMDLINE_H */

/*
 * playos-init/src/init.c — State initialization
 *
 * Separate from main.c so tests can link against it without
 * pulling in the PID 1 main() entry point.
 */
#include <string.h>
#include <time.h>
#include <sys/types.h>

#include "playos-init/init.h"

void playos_init_state_init(struct playos_init_state *s)
{
    memset(s, 0, sizeof(*s));
    s->boot_stage = BOOT_STAGE_START;
    s->boot_time = time(NULL);
    s->compositor_pid = 0;
    s->compositor_state = COMPOSITOR_NOT_STARTED;
    s->game_pid = 0;
    s->game_state = GAME_NONE;
    s->control_sock_fd = -1;
    s->compositor_sock_fd = -1;
    s->log_fd = -1;
    s->log_fd_persistent = -1;
    s->compositor_restarts.window_start = time(NULL);
    s->recovery_mode = 0;
}

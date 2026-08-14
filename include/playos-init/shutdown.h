/*
 * playos-init/shutdown.h — Orderly system shutdown
 */
#ifndef PLAYOS_SHUTDOWN_H
#define PLAYOS_SHUTDOWN_H

#include "init.h"

/* Perform an orderly shutdown: notify the game via lifecycle pipe, SIGTERM
 * the game and compositor, sync filesystems, then reboot(2).
 * restart=1 for reboot, restart=0 for power-off. Never returns. */
void playos_shutdown(struct playos_init_state *s, int restart);

#endif /* PLAYOS_SHUTDOWN_H */

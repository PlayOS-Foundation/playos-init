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

/* Deliver SUSPEND to the active game, attempt S3 (mem) suspend, then deliver
 * RESUME after wake. Best-effort — never fatal, and never returns the system
 * to a stuck state: if suspend is unavailable the game still gets RESUME. */
void playos_suspend(struct playos_init_state *s);

#endif /* PLAYOS_SHUTDOWN_H */

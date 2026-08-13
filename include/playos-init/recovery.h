/*
 * playos-init/recovery.h — Recovery mode subsystem
 */
#ifndef PLAYOS_RECOVERY_H
#define PLAYOS_RECOVERY_H

#include "init.h"

/* Enter recovery mode with a reason string. This halts normal operation. */
void playos_recovery_enter(struct playos_init_state *state, const char *reason);

/* Display recovery diagnostic. Blocks until shutdown/reboot. */
void playos_recovery_loop(struct playos_init_state *state);

#endif /* PLAYOS_RECOVERY_H */

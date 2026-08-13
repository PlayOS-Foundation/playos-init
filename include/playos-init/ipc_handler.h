/*
 * playos-init/include/playos-init/ipc_handler.h — IPC server internals
 */
#ifndef PLAYOS_INIT_IPC_HANDLER_H
#define PLAYOS_INIT_IPC_HANDLER_H

#include "playos-init/init.h"

/* Start the IPC server — creates /run/playos/control.sock */
int playos_ipc_server_start(struct playos_init_state *s);

/* Process incoming connections (non-blocking, call from main loop) */
void playos_ipc_server_poll(struct playos_init_state *s);

#endif /* PLAYOS_INIT_IPC_HANDLER_H */

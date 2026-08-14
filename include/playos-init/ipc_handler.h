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

/* Compositor control socket (Sprint 7) */
int playos_compositor_server_start(struct playos_init_state *s);
void playos_compositor_server_poll(struct playos_init_state *s);
int playos_compositor_send(struct playos_init_state *s, const char *type,
                           const char *extra_json);

/* Emit an async event to the persistent shell listener (Sprint 7) */
void playos_ipc_emit_to_shell(struct playos_init_state *s, const char *type,
                              const char *extra_json);

#endif /* PLAYOS_INIT_IPC_HANDLER_H */

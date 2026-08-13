/*
 * playos-init/supervisor.h — Process supervision subsystem
 */
#ifndef PLAYOS_SUPERVISOR_H
#define PLAYOS_SUPERVISOR_H

#include "init.h"

/* Spawn the compositor process. Returns 0 on success, -1 on error. */
int playos_supervisor_spawn_compositor(struct playos_init_state *state);

/* Handle compositor exit. Restarts if within limits, enters recovery otherwise. */
void playos_supervisor_compositor_exited(struct playos_init_state *state,
                                          int exit_code, int signal);

/* Spawn a game process. Returns PID on success, -1 on error. */
pid_t playos_supervisor_spawn_game(struct playos_init_state *state,
                                    const char *game_id,
                                    const char *manifest_path);

/* Terminate a running game. force=0 for SIGTERM, force=1 for immediate SIGKILL. */
int playos_supervisor_terminate_game(struct playos_init_state *state, int force);

/* Handle game exit. Updates state and emits appropriate IPC events. */
void playos_supervisor_game_exited(struct playos_init_state *state,
                                    int exit_code, int signal);

/* Reap all zombie children. Call from SIGCHLD handler or event loop. */
void playos_supervisor_reap_children(struct playos_init_state *state);

/* Register SIGCHLD handler */
int playos_supervisor_init_signal_handler(void);

/* Launch the hardware-accelerated test client for visual diagnostics */
void playos_supervisor_spawn_test_client(struct playos_init_state *state);

/* Launch the PlayOS shell as a Wayland client (Sprint 5) */
void playos_supervisor_spawn_shell(struct playos_init_state *state);

/* Handle shell exit. Restarts if within limits. */
void playos_supervisor_shell_exited(struct playos_init_state *state,
                                     int exit_code, int signal);

/* Enter recovery mode */
void playos_enter_recovery(struct playos_init_state *state, const char *reason);

#endif /* PLAYOS_SUPERVISOR_H */

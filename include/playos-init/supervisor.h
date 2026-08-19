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

/* Background the game (overlay shown): deliver PLAYOS_LIFECYCLE_BACKGROUND
 * and arm the non-cooperative SIGSTOP timer (Sprint 7). */
void playos_supervisor_game_background(struct playos_init_state *state);

/* Foreground the game (overlay dismissed): SIGCONT if stopped, then
 * deliver PLAYOS_LIFECYCLE_FOREGROUND (Sprint 7). */
void playos_supervisor_game_foreground(struct playos_init_state *state);

/* Non-cooperative SIGSTOP fallback tick — call from the main loop. Sends
 * SIGSTOP if a backgrounded game hasn't paused within the timeout. */
void playos_supervisor_lifecycle_tick(struct playos_init_state *state);

/* Reap all zombie children. Call from SIGCHLD handler or event loop. */
void playos_supervisor_reap_children(struct playos_init_state *state);

/* Register SIGCHLD handler */
int playos_supervisor_init_signal_handler(void);

/* Launch the PlayOS shell as a Wayland client (Sprint 5) */
void playos_supervisor_spawn_shell(struct playos_init_state *state);

/* Handle shell exit. Restarts if within limits. */
void playos_supervisor_shell_exited(struct playos_init_state *state,
                                     int exit_code, int signal);

/* Generate a random launch token for a game launch (Sprint 7) */
int playos_supervisor_generate_launch_token(struct playos_init_state *s);

/* Launch the PlayOS overlay as a Wayland client (Sprint 7) */
void playos_supervisor_spawn_overlay(struct playos_init_state *s);

/* Handle overlay exit. Restarts if within limits. */
void playos_supervisor_overlay_exited(struct playos_init_state *s,
                                      int exit_code, int signal);

/* Launch the PlayOS installer as a Wayland client (Sprint 10) */
void playos_supervisor_spawn_installer(struct playos_init_state *s);

/* Handle installer exit. Restarts if within limits. */
void playos_supervisor_installer_exited(struct playos_init_state *s,
                                        int exit_code, int signal);

/* Launch the developer SSH bring-up daemon (Sprint 11.6) */
void playos_supervisor_spawn_ssh(struct playos_init_state *s);

/* Handle SSH bring-up exit. Restarts if within limits. */
void playos_supervisor_ssh_exited(struct playos_init_state *s,
                                  int exit_code, int signal);

/* Enter recovery mode */
void playos_enter_recovery(struct playos_init_state *state, const char *reason);

#endif /* PLAYOS_SUPERVISOR_H */

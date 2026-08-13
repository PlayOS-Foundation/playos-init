/*
 * playos-init/init.h — Central state container for PID 1
 *
 * playos-init owns a single global mutable state struct. All subsystems
 * receive a pointer to this struct. No other global mutable variables.
 */
#ifndef PLAYOS_INIT_H
#define PLAYOS_INIT_H

#include <stdint.h>
#include <sys/types.h>

/* ── Constants ───────────────────────────────────────────────────── */

#define PLAYOS_COMPOSITOR_MAX_RESTARTS  3
#define PLAYOS_COMPOSITOR_WINDOW_S      60
#define PLAYOS_COMPOSITOR_RESTART_DELAY_MS 500
#define PLAYOS_SHELL_MAX_RESTARTS       5
#define PLAYOS_SHELL_WINDOW_S           30
#define PLAYOS_SHELL_RESTART_DELAY_MS   250
#define PLAYOS_GAME_EXIT_TIMEOUT_MS     2000
#define PLAYOS_LOG_RING_SIZE            (64 * 1024)

#define PLAYOS_SOCK_CONTROL    "/run/playos/control.sock"
#define PLAYOS_SOCK_COMPOSITOR "/run/playos/compositor.sock"

/* ── Boot stages ─────────────────────────────────────────────────── */

enum playos_boot_stage {
    BOOT_STAGE_START          = 0,
    BOOT_STAGE_MOUNTS         = 1,
    BOOT_STAGE_DATA_DISCOVERY = 2,
    BOOT_STAGE_DATA_MOUNTED   = 3,
    BOOT_STAGE_IPC_READY      = 4,
    BOOT_STAGE_COMPOSITOR     = 5,
    BOOT_STAGE_READY          = 6,
    BOOT_STAGE_RECOVERY       = 99
};

/* ── Compositor state ────────────────────────────────────────────── */

enum playos_compositor_state {
    COMPOSITOR_NOT_STARTED = 0,
    COMPOSITOR_STARTING    = 1,
    COMPOSITOR_RUNNING     = 2,
    COMPOSITOR_EXITED      = 3,
};

/* ── Game state ──────────────────────────────────────────────────── */

enum playos_game_state {
    GAME_NONE      = 0,
    GAME_STARTING  = 1,
    GAME_RUNNING   = 2,
    GAME_STOPPING  = 3,
};

/* ── Restart tracking ────────────────────────────────────────────── */

struct playos_restart_info {
    int    count;          /* restarts within current window */
    time_t window_start;   /* when the current window began */
    int    last_exit_code; /* exit code from last exit */
    int    last_signal;    /* signal that killed it, or 0 */
};

/* ── Central state ───────────────────────────────────────────────── */

struct playos_init_state {
    /* Boot */
    enum playos_boot_stage boot_stage;
    time_t                 boot_time;   /* monotonic start time */

    /* Compositor supervision */
    pid_t                     compositor_pid;
    enum playos_compositor_state compositor_state;
    struct playos_restart_info compositor_restarts;

	/* Shell supervision */
	pid_t                     shell_pid;
	struct playos_restart_info shell_restarts;

    /* Game supervision */
    pid_t               game_pid;
    enum playos_game_state game_state;
    char                game_id[256];
    char                launch_token[64];

    /* IPC */
    int control_sock_fd;
    int compositor_sock_fd;

    /* Logging */
    int  log_fd;
    char log_ring[PLAYOS_LOG_RING_SIZE];
    int  log_write_pos;
    int  log_wrapped;

    /* Recovery */
    int recovery_mode;
};

/* ── Global state accessor ───────────────────────────────────────── */

extern struct playos_init_state g_state;

/* ── Initialization ──────────────────────────────────────────────── */

void playos_init_state_init(struct playos_init_state *s);

#endif /* PLAYOS_INIT_H */

/*
 * playos-game-stub.c — Minimal game stub for lifecycle testing (Sprint 1)
 *
 * This is a placeholder game process that:
 *   1. Reads its game ID from PLAYOS_GAME_ID env var
 *   2. Reads lifecycle events from PLAYOS_LIFECYCLE_FD
 *   3. Prints status changes
 *   4. Exits cleanly on TERMINATE or SIGTERM
 *
 * Compile: gcc -std=c99 -static -o playos-game-stub playos-game-stub.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <time.h>

#define PLAYOS_LIFECYCLE_FOREGROUND 0x01
#define PLAYOS_LIFECYCLE_BACKGROUND 0x02
#define PLAYOS_LIFECYCLE_SUSPEND    0x03
#define PLAYOS_LIFECYCLE_RESUME     0x04
#define PLAYOS_LIFECYCLE_TERMINATE  0x05

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

static const char *event_name(uint8_t event)
{
    switch (event) {
        case PLAYOS_LIFECYCLE_FOREGROUND: return "FOREGROUND";
        case PLAYOS_LIFECYCLE_BACKGROUND: return "BACKGROUND";
        case PLAYOS_LIFECYCLE_SUSPEND:    return "SUSPEND";
        case PLAYOS_LIFECYCLE_RESUME:     return "RESUME";
        case PLAYOS_LIFECYCLE_TERMINATE:  return "TERMINATE";
        default:                          return "UNKNOWN";
    }
}

int main(void)
{
    const char *game_id = getenv("PLAYOS_GAME_ID");
    const char *lifecycle_fd_str = getenv("PLAYOS_LIFECYCLE_FD");
    const char *launch_token = getenv("PLAYOS_LAUNCH_TOKEN");

    fprintf(stderr, "playos-game-stub: starting\n");
    fprintf(stderr, "  GAME_ID: %s\n", game_id ? game_id : "(not set)");
    fprintf(stderr, "  LIFECYCLE_FD: %s\n", lifecycle_fd_str ? lifecycle_fd_str : "(not set)");
    fprintf(stderr, "  LAUNCH_TOKEN: %s\n", launch_token ? launch_token : "(not set)");

    /* Set up signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    /* Read lifecycle events from the fd if available */
    int lifecycle_fd = -1;
    if (lifecycle_fd_str) {
        lifecycle_fd = atoi(lifecycle_fd_str);
        /* Make non-blocking so we can poll in the main loop */
        int flags = fcntl(lifecycle_fd, F_GETFL, 0);
        if (flags >= 0)
            fcntl(lifecycle_fd, F_SETFL, flags | O_NONBLOCK);
    }

    fprintf(stderr, "playos-game-stub: running (PID %d), waiting for events...\n",
            getpid());

    /* Main loop: read lifecycle events, respond to signals */
    while (running) {
        /* Check for lifecycle events */
        if (lifecycle_fd >= 0) {
            uint8_t event;
            ssize_t n = read(lifecycle_fd, &event, 1);
            if (n == 1) {
                fprintf(stderr, "playos-game-stub: received %s (0x%02x)\n",
                        event_name(event), event);
                if (event == PLAYOS_LIFECYCLE_TERMINATE) {
                    fprintf(stderr, "playos-game-stub: terminating by request\n");
                    running = 0;
                    break;
                }
            }
            /* EAGAIN means no data (non-blocking) — that's fine */
        }

        /* Small sleep to avoid busy-waiting */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; /* 100ms */
        nanosleep(&ts, NULL);
    }

    fprintf(stderr, "playos-game-stub: exiting cleanly\n");
    return 0;
}

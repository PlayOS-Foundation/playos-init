/*
 * playos-compositor-placeholder.c — Minimal compositor stub for Sprint 1
 *
 * This is a placeholder process that sleeps until SIGTERM.
 * It simulates a running compositor for supervision testing.
 * Replaced by the real playos-compositor in Sprint 2.
 *
 * Compile: gcc -std=c99 -static -o playos-compositor-placeholder playos-compositor-placeholder.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t running = 1;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

int main(void)
{
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    fprintf(stderr, "playos-compositor-placeholder: running (PID %d)\n", getpid());

    while (running) {
        /* Check every second if we should exit */
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
    }

    fprintf(stderr, "playos-compositor-placeholder: exiting\n");
    return 0;
}

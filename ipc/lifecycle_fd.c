/*
 * PlayOS Runtime — Lifecycle fd helpers
 *
 * Provides a pipe-based one-directional channel for delivering
 * lifecycle events from playos-init to game processes.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ipc.h"

#include <unistd.h>

int playos_lifecycle_create(int *read_fd, int *write_fd)
{
    int fds[2];

    if (pipe(fds) < 0)
        return -1;

    *read_fd  = fds[0];
    *write_fd = fds[1];
    return 0;
}

int playos_lifecycle_send_event(int write_fd, uint8_t event)
{
    ssize_t rc;

    rc = write(write_fd, &event, 1);
    if (rc == 1)
        return 0;
    if (rc < 0)
        return -1;

    /* Short write — shouldn't happen for a single byte to a pipe */
    return -1;
}

/*
 * PlayOS Runtime IPC — Client-side socket helpers
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE
#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int playos_ipc_client_connect(const char *path)
{
    struct sockaddr_un addr;
    int fd;

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        close(fd);
        return -1;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (const struct sockaddr *)&addr,
                (socklen_t)sizeof(addr)) < 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }

    return fd;
}

int playos_ipc_client_send(int fd, const struct playos_ipc_message *msg)
{
    return playos_ipc_frame_write(fd, msg);
}

int playos_ipc_client_recv(int fd, struct playos_ipc_message *out, size_t max)
{
    struct playos_ipc_frame *frame = (struct playos_ipc_frame *)malloc(max);
    if (!frame) {
        errno = ENOMEM;
        return -1;
    }

    int total = playos_ipc_frame_read(fd, frame, max);
    if (total <= 0) {
        free(frame);
        return total;
    }

    if (playos_ipc_frame_validate(frame) != 0) {
        free(frame);
        errno = EBADMSG;
        return -1;
    }

    if (playos_ipc_message_parse(frame->body, frame->length, out) != 0) {
        free(frame);
        return -1;
    }

    free(frame);
    return total;
}

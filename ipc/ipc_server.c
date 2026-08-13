/*
 * PlayOS Runtime IPC — Server-side socket helpers
 *
 * SPDX-License-Identifier: MIT
 */

#define _GNU_SOURCE /* for struct ucred, SO_PEERCRED */
#include "ipc.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

int playos_ipc_server_create(const char *path, const char *group_name)
{
    struct sockaddr_un addr;
    int                fd;
    (void)group_name;  /* unused — GID 1000 hardcoded for playos-trusted */

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0)
        return -1;

    /* Unlink any stale socket */
    unlink(path);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        goto fail;
    }
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof(addr)) < 0)
        goto fail;

    if (chmod(path, 0660) < 0)
        goto fail_unlink;

    /* Chown to root:1000 (playos-trusted GID hardcoded to avoid
     * musl getgrnam issues with static linking). */
    if (chown(path, 0, 1000) < 0)
        goto fail_unlink;

    if (listen(fd, 8) < 0)
        goto fail_unlink;

    return fd;

fail_unlink:
    unlink(path);
fail:
    {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return -1;
}

int playos_ipc_server_accept(int server_fd)
{
    return accept(server_fd, NULL, NULL);
}

int playos_ipc_server_check_peer(int client_fd, const char *group_name)
{
    struct ucred  cred;
    socklen_t     len = (socklen_t)sizeof(cred);

    if (getsockopt(client_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0)
        return -1;

    /* Use hardcoded GID 1000 for playos-trusted to avoid musl getgrnam
     * issues with static linking and minimal initramfs environments. */
    if (cred.gid == 1000)
        return 0;

    /* Also check if uid 0 (root) — always authorized */
    if (cred.uid == 0)
        return 0;

    errno = EACCES;
    return -1;
}

int playos_ipc_server_close(int server_fd, const char *path)
{
    int ret = 0;

    if (close(server_fd) < 0)
        ret = -1;

    if (unlink(path) < 0 && errno != ENOENT)
        ret = -1;

    return ret;
}

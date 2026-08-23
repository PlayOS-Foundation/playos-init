/*
 * apply_client.c — minimal IPC client to send ApplyUpdate (Sprint 11.5 T5).
 *
 * Usage: apply-client <bundle_path>
 * Connects to /run/playos/control.sock, sends ApplyUpdate with the given
 * path, and prints the synchronous ack/error. Async UpdateComplete/
 * UpdateError go to the shell listener, so the definitive result is read
 * from /data/log/init.log on the device.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "ipc.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <bundle_path>\n", argv[0]);
        return 2;
    }
    const char *path = argv[1];

    int fd = playos_ipc_client_connect("/run/playos/control.sock");
    if (fd < 0) {
        fprintf(stderr, "FAIL: connect: %s\n", strerror(errno));
        return 2;
    }

    char extra[640];
    snprintf(extra, sizeof(extra), "\"path\":\"%s\"", path);

    struct playos_ipc_message msg;
    if (playos_ipc_message_from_type(PLAYOS_IPC_PROTOCOL_VERSION,
                                     PLAYOS_IPC_TYPE_APPLY_UPDATE,
                                     extra, &msg) != 0) {
        fprintf(stderr, "FAIL: message_from_type\n");
        return 2;
    }

    if (playos_ipc_client_send(fd, &msg) != 0) {
        fprintf(stderr, "FAIL: send: %s\n", strerror(errno));
        playos_ipc_message_free(&msg);
        return 2;
    }
    playos_ipc_message_free(&msg);
    printf("sent ApplyUpdate path=%s\n", path);

    /* The server replies with ApplyUpdateAck (accepted) or
     * ApplyUpdateError (rejected) on this same connection. */
    struct playos_ipc_message resp;
    memset(&resp, 0, sizeof(resp));
    size_t max = sizeof(struct playos_ipc_frame) + PLAYOS_IPC_MAX_BODY;
    struct playos_ipc_frame *frame = malloc(max);
    if (!frame) {
        fprintf(stderr, "FAIL: malloc\n");
        return 2;
    }
    int total = playos_ipc_frame_read(fd, frame, max);
    if (total <= 0) {
        fprintf(stderr, "FAIL: frame_read: %s (total=%d)\n",
                strerror(errno), total);
        free(frame);
        return 2;
    }
    if (playos_ipc_message_parse(frame->body, frame->length, &resp) != 0) {
        fprintf(stderr, "FAIL: parse\n");
        free(frame);
        return 2;
    }
    printf("response type=%s json=%s\n", resp.type, resp.json_raw);

    int rc = 0;
    if (strcmp(resp.type, PLAYOS_IPC_TYPE_APPLY_UPDATE_ACK) == 0) {
        printf("APPLY ACCEPTED\n");
        rc = 0;
    } else if (strcmp(resp.type, PLAYOS_IPC_TYPE_APPLY_UPDATE_ERROR) == 0) {
        printf("APPLY REJECTED\n");
        rc = 3;
    } else {
        printf("UNEXPECTED RESPONSE\n");
        rc = 4;
    }

    playos_ipc_message_free(&resp);
    free(frame);
    close(fd);
    return rc;
}

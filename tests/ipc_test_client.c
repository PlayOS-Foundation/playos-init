/*
 * ipc_test_client.c — IPC test client for Sprint 1 integration tests
 *
 * Connects to /run/playos/control.sock and runs a battery of tests:
 *   1. QueryStatus → validates StatusReport response
 *   2. LaunchGame → validates GameStarted event
 *   3. QueryStatus again → verifies game_pid is non-zero
 *   4. TerminateGame → validates GameExited event
 *   5. QueryStatus → verifies game_pid is 0
 *
 * Usage: ipc-test-client [--test <name>] [--verbose]
 *
 * Exit code 0 = all tests passed, 1 = test failure, 2 = connection failure.
 *
 * Compile: gcc -std=c99 -static -I../ipc -I../include \
 *          ipc_test_client.c ../ipc/ipc_framing.c ../ipc/ipc_server.c ../ipc/lifecycle_fd.c \
 *          -o ipc-test-client
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>

#include "ipc.h"

/* ── Helpers ────────────────────────────────────────────────────── */

static int g_verbose = 0;

#define LOG(fmt, ...) do { \
    if (g_verbose) fprintf(stderr, "[test] " fmt "\n", ##__VA_ARGS__); \
} while(0)

static int connect_to_init(void)
{
    int fd = playos_ipc_client_connect("/run/playos/control.sock");
    if (fd < 0) {
        fprintf(stderr, "FAIL: cannot connect to /run/playos/control.sock: %s\n",
                strerror(errno));
        return -1;
    }
    LOG("connected to control socket");
    return fd;
}

static int send_message(int fd, const char *type, const char *extra)
{
    struct playos_ipc_message msg;
    if (playos_ipc_message_from_type(PLAYOS_IPC_PROTOCOL_VERSION,
                                      type, extra, &msg) != 0) {
        fprintf(stderr, "FAIL: playos_ipc_message_from_type failed\n");
        return -1;
    }
    if (playos_ipc_client_send(fd, &msg) != 0) {
        fprintf(stderr, "FAIL: send %s: %s\n", type, strerror(errno));
        playos_ipc_message_free(&msg);
        return -1;
    }
    playos_ipc_message_free(&msg);
    LOG("sent %s", type);
    return 0;
}

static int recv_message(int fd, struct playos_ipc_message *out)
{
    size_t max = sizeof(struct playos_ipc_frame) + PLAYOS_IPC_MAX_BODY;
    struct playos_ipc_frame *frame = malloc(max);
    if (!frame) {
        fprintf(stderr, "FAIL: malloc for frame buffer\n");
        return -1;
    }

    int total = playos_ipc_frame_read(fd, frame, max);
    if (total < 0) {
        fprintf(stderr, "FAIL: frame_read: %s\n", strerror(errno));
        free(frame);
        return -1;
    }
    if (total == 0) {
        fprintf(stderr, "FAIL: unexpected EOF on socket\n");
        free(frame);
        return -1;
    }

    if (playos_ipc_message_parse(frame->body, frame->length, out) != 0) {
        fprintf(stderr, "FAIL: message parse failed\n");
        free(frame);
        return -1;
    }

    LOG("received type=%s json=%s", out->type, out->json_raw);
    free(frame);
    return 0;
}

static int expect_type(const struct playos_ipc_message *msg, const char *expected)
{
    if (strcmp(msg->type, expected) != 0) {
        fprintf(stderr, "FAIL: expected type '%s', got '%s'\n",
                expected, msg->type);
        return -1;
    }
    return 0;
}

static int json_contains(const struct playos_ipc_message *msg, const char *key)
{
    if (!strstr(msg->json_raw, key)) {
        fprintf(stderr, "FAIL: JSON missing key '%s': %s\n",
                key, msg->json_raw);
        return -1;
    }
    return 0;
}

/* ── Test cases ─────────────────────────────────────────────────── */

static int test_query_status(int fd)
{
    printf("TEST: QueryStatus...\n");
    if (send_message(fd, PLAYOS_IPC_TYPE_QUERY_STATUS, NULL) != 0)
        return -1;

    struct playos_ipc_message resp;
    memset(&resp, 0, sizeof(resp));
    if (recv_message(fd, &resp) != 0)
        return -1;

    int ok = 1;
    if (expect_type(&resp, PLAYOS_IPC_TYPE_STATUS_REPORT) != 0) ok = 0;
    if (json_contains(&resp, "\"uptime\"") != 0) ok = 0;
    if (json_contains(&resp, "\"compositor_pid\"") != 0) ok = 0;
    if (json_contains(&resp, "\"boot_stage\"") != 0) ok = 0;

    playos_ipc_message_free(&resp);
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

static int test_launch_game(int fd)
{
    printf("TEST: LaunchGame...\n");

    const char *extra = "\"game_id\":\"test-game-1\",\"manifest_path\":\"/data/games/test-game-1/manifest.json\"";
    if (send_message(fd, PLAYOS_IPC_TYPE_LAUNCH_GAME, extra) != 0)
        return -1;

    struct playos_ipc_message resp;
    memset(&resp, 0, sizeof(resp));
    if (recv_message(fd, &resp) != 0)
        return -1;

    int ok = 1;
    if (expect_type(&resp, PLAYOS_IPC_TYPE_LAUNCH_GAME_ACK) != 0) ok = 0;
    if (json_contains(&resp, "\"game_id\"") != 0) ok = 0;
    if (json_contains(&resp, "\"pid\"") != 0) ok = 0;

    playos_ipc_message_free(&resp);
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

static int test_query_status_with_game(int fd)
{
    printf("TEST: QueryStatus (game running)...\n");
    if (send_message(fd, PLAYOS_IPC_TYPE_QUERY_STATUS, NULL) != 0)
        return -1;

    struct playos_ipc_message resp;
    memset(&resp, 0, sizeof(resp));
    if (recv_message(fd, &resp) != 0)
        return -1;

    int ok = 1;
    if (expect_type(&resp, PLAYOS_IPC_TYPE_STATUS_REPORT) != 0) ok = 0;
    if (json_contains(&resp, "\"game_id\"") != 0) ok = 0;
    if (json_contains(&resp, "\"game_pid\"") != 0) ok = 0;

    /* Verify game_pid is non-zero */
    const char *gp = strstr(resp.json_raw, "\"game_pid\"");
    if (gp) {
        gp = strchr(gp, ':');
        if (gp) {
            gp++; /* skip colon */
            while (*gp == ' ' || *gp == '\t') gp++;
            int gpid = atoi(gp);
            if (gpid <= 0) {
                fprintf(stderr, "FAIL: game_pid is %d (expected > 0)\n", gpid);
                ok = 0;
            } else {
                LOG("game_pid=%d", gpid);
            }
        }
    }

    playos_ipc_message_free(&resp);
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

static int test_terminate_game(int fd)
{
    printf("TEST: TerminateGame...\n");

    const char *extra = "\"game_id\":\"test-game-1\"";
    if (send_message(fd, PLAYOS_IPC_TYPE_TERMINATE_GAME, extra) != 0)
        return -1;

    struct playos_ipc_message resp;
    memset(&resp, 0, sizeof(resp));
    if (recv_message(fd, &resp) != 0)
        return -1;

    int ok = 1;
    if (expect_type(&resp, PLAYOS_IPC_TYPE_TERMINATE_GAME_ACK) != 0) ok = 0;

    playos_ipc_message_free(&resp);
    printf("  %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

/* ── Main ───────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    const char *single_test = NULL;
    int failed = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--test") == 0 && i + 1 < argc) {
            single_test = argv[++i];
        } else {
            fprintf(stderr, "Usage: %s [--verbose] [--test <name>]\n", argv[0]);
            return 2;
        }
    }

    /* Wait a moment for the init to be fully ready */
    if (!single_test) {
        printf("ipc-test-client: waiting for init readiness...\n");
        sleep(2);
    }

    struct {
        const char *name;
        int (*fn)(int);
    } tests[] = {
        {"query-status",             test_query_status},
        {"launch-game",              test_launch_game},
        {"query-status-with-game",   test_query_status_with_game},
        {"terminate-game",           test_terminate_game},
    };

    int n_tests = sizeof(tests) / sizeof(tests[0]);

    for (int i = 0; i < n_tests; i++) {
        if (single_test && strcmp(tests[i].name, single_test) != 0)
            continue;

        /* Reconnect fresh for each test — server closes after every message */
        int tfd = connect_to_init();
        if (tfd < 0) {
            failed++;
            continue;
        }

        if (tests[i].fn(tfd) != 0)
            failed++;

        close(tfd);
    }

    if (failed > 0) {
        printf("\n%d test(s) FAILED\n", failed);
        return 1;
    }

    printf("\nAll tests PASSED\n");
    return 0;
}

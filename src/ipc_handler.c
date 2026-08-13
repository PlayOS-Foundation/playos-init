/*
 * playos-init/src/ipc_handler.c — IPC server for playos-init
 *
 * Creates /run/playos/control.sock and handles incoming IPC messages:
 *   - QueryStatus  → StatusReport
 *   - Shutdown     → orderly shutdown
 *   - Reboot       → orderly reboot
 *
 * Sprint 1 minimal implementation — game lifecycle added in S1-T6.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>
#include <dirent.h>
#include <limits.h>

#include "playos-init/init.h"

/* ── External dependencies ───────────────────────────────────────── */

/* IPC framing from playos-runtime (bundled source) */
#include "ipc.h"

extern void playos_log_write(struct playos_init_state *s, const char *tag,
                             const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Internal helpers ────────────────────────────────────────────── */

/*
 * Build a simple JSON string on the stack.
 * Sprint 1: hand-rolled for the 3 message types we need.
 * Will be replaced by a proper JSON builder in Sprint 2.
 */
static int build_status_json(char *buf, size_t size,
                             const struct playos_init_state *s)
{
    return snprintf(buf, size,
        "{\"v\":%d,\"type\":\"%s\","
        "\"uptime\":%ld,"
        "\"compositor_pid\":%d,"
        "\"game_pid\":%d,"
        "\"game_id\":\"%s\","
        "\"boot_stage\":%d,"
        "\"recovery\":%d}",
        PLAYOS_IPC_PROTOCOL_VERSION,
        PLAYOS_IPC_TYPE_STATUS_REPORT,
        (long)(time(NULL) - s->boot_time),
        s->compositor_pid,
        s->game_pid,
        s->game_id ? s->game_id : "",
        (int)s->boot_stage,
        s->recovery_mode);
}

/*
 * Parse a top-level JSON boolean field of the form "field":true.
 * Returns 0 and stores the value in *out on success, or -1 if the
 * field is absent/malformed. The caller treats -1 as false (defensive).
 */
static int json_get_bool(const char *json, const char *field, int *out)
{
    if (!json || !field || !out)
        return -1;

    char key[64];
    int n = snprintf(key, sizeof(key), "\"%s\"", field);
    if (n < 0 || (size_t)n >= sizeof(key))
        return -1;

    const char *p = strstr(json, key);
    if (!p)
        return -1;

    p = strchr(p, ':');
    if (!p)
        return -1;
    p++;

    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;

    if (strncmp(p, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = 0;
        return 0;
    }

    return -1;
}

/*
 * Recursively delete a directory tree and recreate the top-level
 * directory. Skips "." and "..". Returns 0 on success, -1 on error.
 */
static int rmtree(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char child[PATH_MAX];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            closedir(dir);
            return -1;
        }

        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (rmtree(child) != 0) {
                closedir(dir);
                return -1;
            }
        } else {
            if (unlink(child) != 0 && errno != ENOENT) {
                closedir(dir);
                return -1;
            }
        }
    }
    closedir(dir);

    if (rmdir(path) != 0 && errno != ENOENT)
        return -1;

    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

/*
 * Handle an incoming IPC message from a client.
 * Returns 0 to keep the connection open, -1 to close it.
 */
static int handle_message(struct playos_init_state *s, int client_fd,
                          const char *raw_body, size_t body_len)
{
    struct playos_ipc_message msg;
    memset(&msg, 0, sizeof(msg));

    if (playos_ipc_message_parse(raw_body, body_len, &msg) != 0) {
        playos_log_write(s, "ipc", "parse error from client fd=%d", client_fd);
        struct playos_ipc_message err;
        memset(&err, 0, sizeof(err));
        /* Use a static error message */
        char err_json[256];
        int err_len = snprintf(err_json, sizeof(err_json),
            "{\"v\":%d,\"type\":\"%s\",\"reason\":\"parse error\"}",
            PLAYOS_IPC_PROTOCOL_VERSION, PLAYOS_IPC_TYPE_PROTOCOL_ERROR);
        /* Write raw framed message as single SOCK_SEQPACKET datagram */
        struct playos_ipc_frame *err_frame = malloc(sizeof(*err_frame) + (size_t)err_len);
        if (err_frame) {
            err_frame->magic = PLAYOS_IPC_MAGIC;
            err_frame->length = (uint32_t)err_len;
            memcpy(err_frame->body, err_json, (size_t)err_len);
            (void)!write(client_fd, err_frame, sizeof(*err_frame) + (size_t)err_len);
            free(err_frame);
        }
        return -1;
    }

    playos_log_write(s, "ipc", "received type=%s from fd=%d",
                     msg.type ? msg.type : "(null)", client_fd);

    if (msg.type == NULL) {
        playos_ipc_message_free(&msg);
        return -1;
    }

    /* ── QueryStatus ─────────────────────────────────────────── */
    if (strcmp(msg.type, PLAYOS_IPC_TYPE_QUERY_STATUS) == 0) {
        char status_json[512];
        int status_len = build_status_json(status_json, sizeof(status_json), s);

        if (status_len < 0 || (size_t)status_len >= sizeof(status_json)) {
            playos_ipc_message_free(&msg);
            return -1;
        }

        struct playos_ipc_frame *frame;
        size_t frame_size = sizeof(*frame) + (size_t)status_len;
        frame = malloc(frame_size);
        if (!frame) {
            playos_ipc_message_free(&msg);
            return -1;
        }

        frame->magic = PLAYOS_IPC_MAGIC;
        frame->length = (uint32_t)status_len;
        memcpy(frame->body, status_json, (size_t)status_len);

        ssize_t wrote = write(client_fd, frame, frame_size);
        free(frame);

        playos_ipc_message_free(&msg);
        return (wrote == (ssize_t)frame_size) ? 0 : -1;
    }

    /* ── Shutdown ────────────────────────────────────────────── */
    if (strcmp(msg.type, PLAYOS_IPC_TYPE_SHUTDOWN) == 0) {
        playos_log_write(s, "ipc", "shutdown requested via IPC");
        playos_ipc_message_free(&msg);
        /* Signal main loop to shut down */
        s->recovery_mode = 1;
        return 0;
    }

    /* ── Reboot ──────────────────────────────────────────────── */
    if (strcmp(msg.type, PLAYOS_IPC_TYPE_REBOOT) == 0) {
        playos_log_write(s, "ipc", "reboot requested via IPC");

        /* Send acknowledgment */
        char ack_json[128];
        int ack_len = snprintf(ack_json, sizeof(ack_json),
            "{\"v\":%d,\"type\":\"%s\"}",
            PLAYOS_IPC_PROTOCOL_VERSION, PLAYOS_IPC_TYPE_SHUTDOWN);
        struct playos_ipc_frame *ack_frame = malloc(sizeof(*ack_frame) + (size_t)ack_len);
        if (ack_frame) {
            ack_frame->magic = PLAYOS_IPC_MAGIC;
            ack_frame->length = (uint32_t)ack_len;
            memcpy(ack_frame->body, ack_json, (size_t)ack_len);
            (void)!write(client_fd, ack_frame, sizeof(*ack_frame) + (size_t)ack_len);
            free(ack_frame);
        }

        playos_ipc_message_free(&msg);

        /* Orderly reboot */
        sync();
        extern void playos_shutdown(int reboot_after);
        playos_shutdown(1);
        return -1;
    }

    /* ── LaunchGame ─────────────────────────────────────────── */
    if (strcmp(msg.type, PLAYOS_IPC_TYPE_LAUNCH_GAME) == 0) {
        /* Extract game_id and manifest_path from JSON body.
         * Sprint 1 minimal parser: find "game_id":"..." and "manifest_path":"..."
         * using simple string scanning. */
        char game_id[128] = {0};
        char manifest_path[256] = {0};

        const char *p = strstr(msg.json_raw, "\"game_id\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p = strchr(p, '"');
                if (p) {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len < sizeof(game_id)) {
                            memcpy(game_id, p, len);
                        }
                    }
                }
            }
        }

        p = strstr(msg.json_raw, "\"manifest_path\"");
        if (p) {
            p = strchr(p, ':');
            if (p) {
                p = strchr(p, '"');
                if (p) {
                    p++;
                    char *end = strchr(p, '"');
                    if (end) {
                        size_t len = (size_t)(end - p);
                        if (len < sizeof(manifest_path)) {
                            memcpy(manifest_path, p, len);
                        }
                    }
                }
            }
        }

        if (game_id[0] == '\0') {
            playos_log_write(s, "ipc", "LaunchGame: missing game_id");
            playos_ipc_message_free(&msg);
            return -1;
        }

        if (s->game_pid != 0) {
            /* A game is already running */
            char err_json[256];
            int err_len = snprintf(err_json, sizeof(err_json),
                "{\"v\":%d,\"type\":\"%s\","
                "\"reason\":\"game already running\","
                "\"game_id\":\"%s\"}",
                PLAYOS_IPC_PROTOCOL_VERSION,
                PLAYOS_IPC_TYPE_LAUNCH_GAME_ERROR,
                game_id);
            struct playos_ipc_frame *err_frame = malloc(sizeof(*err_frame) + (size_t)err_len);
            if (err_frame) {
                err_frame->magic = PLAYOS_IPC_MAGIC;
                err_frame->length = (uint32_t)err_len;
                memcpy(err_frame->body, err_json, (size_t)err_len);
                (void)!write(client_fd, err_frame, sizeof(*err_frame) + (size_t)err_len);
                free(err_frame);
            }
            playos_log_write(s, "ipc", "LaunchGame rejected: game already running");
            playos_ipc_message_free(&msg);
            return 0;
        }

        playos_log_write(s, "ipc", "LaunchGame: id=%s path=%s",
                         game_id,
                         manifest_path[0] ? manifest_path : "(none)");

        /* Spawn the game process */
        extern int playos_supervisor_spawn_game(struct playos_init_state *s,
                                                const char *game_id,
                                                const char *manifest_path);
        extern int playos_supervisor_terminate_game(struct playos_init_state *s,
                                                     int force);
        if (playos_supervisor_spawn_game(s, game_id, manifest_path) > 0) {
            /* Game started — send acknowledgment as single SOCK_SEQPACKET frame */
            char ack_json[384];
            int ack_len = snprintf(ack_json, sizeof(ack_json),
                "{\"v\":%d,\"type\":\"%s\","
                "\"game_id\":\"%s\","
                "\"pid\":%d,"
                "\"launch_token\":\"sprint-1-test-token\"}",
                PLAYOS_IPC_PROTOCOL_VERSION,
                PLAYOS_IPC_TYPE_LAUNCH_GAME_ACK,
                game_id,
                s->game_pid);
            struct playos_ipc_frame *ack_frame = malloc(sizeof(*ack_frame) + (size_t)ack_len);
            if (ack_frame) {
                ack_frame->magic = PLAYOS_IPC_MAGIC;
                ack_frame->length = (uint32_t)ack_len;
                memcpy(ack_frame->body, ack_json, (size_t)ack_len);
                (void)!write(client_fd, ack_frame, sizeof(*ack_frame) + (size_t)ack_len);
                free(ack_frame);
            }
        } else {
            char err_json[256];
            int err_len = snprintf(err_json, sizeof(err_json),
                "{\"v\":%d,\"type\":\"%s\","
                "\"reason\":\"spawn failed\","
                "\"game_id\":\"%s\"}",
                PLAYOS_IPC_PROTOCOL_VERSION,
                PLAYOS_IPC_TYPE_LAUNCH_GAME_ERROR,
                game_id);
            struct playos_ipc_frame *err_frame = malloc(sizeof(*err_frame) + (size_t)err_len);
            if (err_frame) {
                err_frame->magic = PLAYOS_IPC_MAGIC;
                err_frame->length = (uint32_t)err_len;
                memcpy(err_frame->body, err_json, (size_t)err_len);
                (void)!write(client_fd, err_frame, sizeof(*err_frame) + (size_t)err_len);
                free(err_frame);
            }
        }

        playos_ipc_message_free(&msg);
        return 0;
    }

    /* ── TerminateGame ──────────────────────────────────────── */
    if (strcmp(msg.type, PLAYOS_IPC_TYPE_TERMINATE_GAME) == 0) {
        if (s->game_pid == 0) {
            playos_log_write(s, "ipc", "TerminateGame: no game running");
            playos_ipc_message_free(&msg);
            return -1;
        }

        playos_log_write(s, "ipc", "TerminateGame: pid=%d", s->game_pid);

        /* SIGTERM the game, then 2s grace, then SIGKILL */
        extern int playos_supervisor_terminate_game(struct playos_init_state *s,
                                                     int force);
        playos_supervisor_terminate_game(s, 0);

        /* Send acknowledgment as single SOCK_SEQPACKET frame */
        char ack_json[256];
        int ack_len = snprintf(ack_json, sizeof(ack_json),
            "{\"v\":%d,\"type\":\"%s\"}",
            PLAYOS_IPC_PROTOCOL_VERSION,
            PLAYOS_IPC_TYPE_TERMINATE_GAME_ACK);
        struct playos_ipc_frame *ack_frame = malloc(sizeof(*ack_frame) + (size_t)ack_len);
        if (ack_frame) {
            ack_frame->magic = PLAYOS_IPC_MAGIC;
            ack_frame->length = (uint32_t)ack_len;
            memcpy(ack_frame->body, ack_json, (size_t)ack_len);
            (void)!write(client_fd, ack_frame, sizeof(*ack_frame) + (size_t)ack_len);
            free(ack_frame);
        }

        playos_ipc_message_free(&msg);
        return 0;
    }

    /* ── ShellReady (Sprint 5) ────────────────────────────────── */
    if (strcmp(msg.type, PLAYOS_IPC_TYPE_SHELL_READY) == 0) {
        playos_log_write(s, "ipc", "shell ready notification received");
        playos_ipc_message_free(&msg);
        return 0;
    }

    /* ── FactoryReset (Sprint 6) ──────────────────────────────── */
    if (strcmp(msg.type, PLAYOS_IPC_TYPE_FACTORY_RESET) == 0) {
        int erase_games = 0;
        int erase_saves = 0;
        int erase_cache = 0;
        int erase_config = 0;
        int erase_logs = 0;

        /* Absent or malformed booleans default to false (defensive). */
        (void)json_get_bool(msg.json_raw, "erase_games", &erase_games);
        (void)json_get_bool(msg.json_raw, "erase_saves", &erase_saves);
        (void)json_get_bool(msg.json_raw, "erase_cache", &erase_cache);
        (void)json_get_bool(msg.json_raw, "erase_config", &erase_config);
        (void)json_get_bool(msg.json_raw, "erase_logs", &erase_logs);

        if (s->game_pid != 0) {
            playos_log_write(s, "ipc", "FactoryReset rejected: game running");

            char err_json[256];
            int err_len = snprintf(err_json, sizeof(err_json),
                "{\"v\":%d,\"type\":\"%s\",\"reason\":\"game_running\"}",
                PLAYOS_IPC_PROTOCOL_VERSION,
                PLAYOS_IPC_TYPE_FACTORY_RESET_ERROR);
            struct playos_ipc_frame *err_frame =
                malloc(sizeof(*err_frame) + (size_t)err_len);
            if (err_frame) {
                err_frame->magic = PLAYOS_IPC_MAGIC;
                err_frame->length = (uint32_t)err_len;
                memcpy(err_frame->body, err_json, (size_t)err_len);
                (void)!write(client_fd, err_frame,
                             sizeof(*err_frame) + (size_t)err_len);
                free(err_frame);
            }

            playos_ipc_message_free(&msg);
            return 0;
        }

        /* Sprint 6 scope: erase_cache and erase_config only. */
        if (erase_cache) {
            playos_log_write(s, "ipc", "FactoryReset: erasing /data/cache");
            if (rmtree("/data/cache") != 0) {
                playos_log_write(s, "ipc",
                                 "FactoryReset: failed to reset /data/cache");
            }
        }
        if (erase_config) {
            playos_log_write(s, "ipc", "FactoryReset: erasing /data/config");
            if (rmtree("/data/config") != 0) {
                playos_log_write(s, "ipc",
                                 "FactoryReset: failed to reset /data/config");
            }
        }

        /* erase_games/erase_saves/erase_logs are deferred to Sprint 10;
         * report them explicitly rather than acting on them. */
        char deferred_json[128] = "";
        if (erase_games || erase_saves || erase_logs) {
            char items[96] = "";
            size_t off = 0;
            if (erase_games)
                off += (size_t)snprintf(items + off, sizeof(items) - off,
                                        "%s\"games\"", off ? "," : "");
            if (erase_saves)
                off += (size_t)snprintf(items + off, sizeof(items) - off,
                                        "%s\"saves\"", off ? "," : "");
            if (erase_logs)
                off += (size_t)snprintf(items + off, sizeof(items) - off,
                                        "%s\"logs\"", off ? "," : "");
            snprintf(deferred_json, sizeof(deferred_json),
                     ",\"deferred\":[%s]", items);
        }

        char done_json[512];
        int done_len = snprintf(done_json, sizeof(done_json),
            "{\"v\":%d,\"type\":\"%s\"%s}",
            PLAYOS_IPC_PROTOCOL_VERSION,
            PLAYOS_IPC_TYPE_FACTORY_RESET_COMPLETE,
            deferred_json);
        if (done_len < 0 || (size_t)done_len >= sizeof(done_json)) {
            playos_ipc_message_free(&msg);
            return -1;
        }

        struct playos_ipc_frame *done_frame =
            malloc(sizeof(*done_frame) + (size_t)done_len);
        if (done_frame) {
            done_frame->magic = PLAYOS_IPC_MAGIC;
            done_frame->length = (uint32_t)done_len;
            memcpy(done_frame->body, done_json, (size_t)done_len);
            (void)!write(client_fd, done_frame,
                         sizeof(*done_frame) + (size_t)done_len);
            free(done_frame);
        }

        playos_log_write(s, "ipc", "FactoryReset complete");
        playos_ipc_message_free(&msg);
        return 0;
    }

    /* ── Unknown type ────────────────────────────────────────── */
    playos_log_write(s, "ipc", "unknown message type: %s", msg.type);
    playos_ipc_message_free(&msg);
    return -1;
}

/* ── IPC server lifecycle ────────────────────────────────────────── */

int playos_ipc_server_start(struct playos_init_state *s)
{
    const char *sock_path = "/run/playos/control.sock";

    /* Ensure directory exists */
    mkdir("/run/playos", 0755);

    /* Remove stale socket */
    unlink(sock_path);

    int server_fd = playos_ipc_server_create(sock_path, "playos-trusted");
    if (server_fd < 0) {
        playos_log_write(s, "ipc",
                         "WARN: could not create control socket at %s: %s",
                         sock_path, strerror(errno));
        return -1;
    }

    /* Set non-blocking for poll-based accept */
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(server_fd, F_SETFL, flags | O_NONBLOCK);
    }

    s->control_sock_fd = server_fd;
    playos_log_write(s, "ipc", "IPC server listening on %s", sock_path);
    return 0;
}

/*
 * Process incoming IPC connections and messages.
 * Called from the main supervision loop (non-blocking via poll).
 */
void playos_ipc_server_poll(struct playos_init_state *s)
{
    if (s->control_sock_fd < 0)
        return;

    struct pollfd pfd;
    pfd.fd = s->control_sock_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 0); /* 0 timeout = pure poll */
    if (ret <= 0)
        return;

    int client_fd = accept(s->control_sock_fd, NULL, NULL);
    if (client_fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            playos_log_write(s, "ipc", "accept error: %s", strerror(errno));
        }
        return;
    }

    /* Verify peer is in playos-trusted group (GID 1000 hardcoded) */
    int peer_ok = playos_ipc_server_check_peer(client_fd, "playos-trusted");
    if (peer_ok != 0) {
        playos_log_write(s, "ipc", "rejected unauthorized client fd=%d",
                         client_fd);
        close(client_fd);
        return;
    }

    /* Read and handle one message, then close.
     * Use heap allocation — stack buffer of 64KB may overflow
     * the limited stack of a statically-linked PID 1. */
    size_t buf_size = sizeof(struct playos_ipc_frame) + PLAYOS_IPC_MAX_BODY;
    struct playos_ipc_frame *frame = malloc(buf_size);
    if (!frame) {
        playos_log_write(s, "ipc", "malloc failed for frame buffer");
        close(client_fd);
        return;
    }

    ssize_t n = recv(client_fd, frame, buf_size, 0);
    if (n >= (ssize_t)sizeof(struct playos_ipc_frame)) {
        if (frame->magic == PLAYOS_IPC_MAGIC
            && frame->length <= PLAYOS_IPC_MAX_BODY
            && (size_t)n >= sizeof(struct playos_ipc_frame) + frame->length) {
            handle_message(s, client_fd, frame->body,
                           (size_t)frame->length);
        }
    }

    free(frame);
    close(client_fd);
}

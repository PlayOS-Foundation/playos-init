/*
 * PlayOS Runtime IPC — Public Header
 *
 * Defines the wire-format framing, message types, lifecycle events,
 * and all public API declarations for the playos-ipc static library.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PLAYOS_IPC_H
#define PLAYOS_IPC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Framing constants ─────────────────────────────────────── */

/** 4-byte magic sentinel at the start of every frame: "PLOS" */
#define PLAYOS_IPC_MAGIC 0x504C4F53U

/** Maximum allowed JSON body size in bytes (64 KiB). */
#define PLAYOS_IPC_MAX_BODY 65536U

/** Current protocol version — inserted as "v" in every message. */
#define PLAYOS_IPC_PROTOCOL_VERSION 1

/* ── Message type strings ──────────────────────────────────── */

#define PLAYOS_IPC_TYPE_QUERY_STATUS        "QueryStatus"
#define PLAYOS_IPC_TYPE_LAUNCH_GAME         "LaunchGame"
#define PLAYOS_IPC_TYPE_TERMINATE_GAME      "TerminateGame"
#define PLAYOS_IPC_TYPE_SHUTDOWN            "Shutdown"
#define PLAYOS_IPC_TYPE_REBOOT              "Reboot"
#define PLAYOS_IPC_TYPE_STATUS_REPORT       "StatusReport"
#define PLAYOS_IPC_TYPE_GAME_STARTED        "GameStarted"
#define PLAYOS_IPC_TYPE_GAME_EXITED         "GameExited"
#define PLAYOS_IPC_TYPE_GAME_CRASHED        "GameCrashed"
#define PLAYOS_IPC_TYPE_ERROR               "Error"
#define PLAYOS_IPC_TYPE_PROTOCOL_ERROR      "ProtocolError"
#define PLAYOS_IPC_TYPE_LAUNCH_GAME_ACK     "LaunchGameAck"
#define PLAYOS_IPC_TYPE_LAUNCH_GAME_ERROR   "LaunchGameError"
#define PLAYOS_IPC_TYPE_TERMINATE_GAME_ACK  "TerminateGameAck"
#define PLAYOS_IPC_TYPE_SHELL_READY        "ShellReady"

/* ── Lifecycle event constants ─────────────────────────────── */

/** Normal foreground operation. */
#define PLAYOS_LIFECYCLE_FOREGROUND 0x01U
/** Game moved to background (e.g. overlay shown). */
#define PLAYOS_LIFECYCLE_BACKGROUND 0x02U
/** Game suspended (e.g. system sleep / GPU lost). */
#define PLAYOS_LIFECYCLE_SUSPEND    0x03U
/** Game resumed from suspend. */
#define PLAYOS_LIFECYCLE_RESUME     0x04U
/** Game must save state and exit. */
#define PLAYOS_LIFECYCLE_TERMINATE  0x05U

/* ── Wire-format structs ───────────────────────────────────── */

/**
 * On-wire binary frame.
 *
 * Followed immediately by `length` bytes of UTF-8 JSON body
 * (no trailing NUL).
 */
struct playos_ipc_frame {
    uint32_t magic;   /**< Must equal PLAYOS_IPC_MAGIC */
    uint32_t length;  /**< Byte count of `body` (little-endian) */
    char     body[];  /**< Flexible array — JSON follows */
};

/**
 * Parsed, in-memory representation of an IPC message.
 *
 * Callers must call playos_ipc_message_free() when done.
 */
struct playos_ipc_message {
    int         version;   /**< "v" field from JSON */
    const char *type;      /**< "type" field from JSON (borrowed pointer) */
    char       *json_raw;  /**< Owned copy of the full JSON body */
    size_t      json_len;  /**< Byte length of json_raw (excl. NUL) */
};

/* ── Framing API ───────────────────────────────────────────── */

/**
 * Read one framed message from `fd` into caller-allocated `out`.
 *
 * @param fd   File descriptor to read from.
 * @param out  Pre-allocated buffer large enough for header + max body.
 * @param max  Total size of `out` buffer.
 * @return     Total bytes placed in `out` (header + body) on success,
 *             0 on clean EOF before any data, -1 on error.
 */
int playos_ipc_frame_read(int fd, struct playos_ipc_frame *out, size_t max);

/**
 * Write a parsed message to `fd` as a framed binary message.
 *
 * @param fd   File descriptor to write to.
 * @param msg  Message to serialize and send.
 * @return     0 on success, -1 on error.
 */
int playos_ipc_frame_write(int fd, const struct playos_ipc_message *msg);

/**
 * Validate a received frame.
 *
 * Checks magic == PLAYOS_IPC_MAGIC and length <= PLAYOS_IPC_MAX_BODY.
 *
 * @param frame  Frame to validate.
 * @return       0 if valid, -1 otherwise.
 */
int playos_ipc_frame_validate(const struct playos_ipc_frame *frame);

/**
 * Free all heap allocations inside a parsed message.
 *
 * Safe to call with a zero-filled message or after a failed parse.
 */
void playos_ipc_message_free(struct playos_ipc_message *msg);

/* ── Message parsing / building ────────────────────────────── */

/**
 * Parse a raw JSON body into a playos_ipc_message.
 *
 * Only extracts "v" (int) and "type" (string).  The full JSON body
 * is copied into msg->json_raw for later use, and the type string
 * is separately allocated.
 *
 * The caller must free the message with playos_ipc_message_free().
 *
 * @param raw  Raw JSON body bytes (not NUL-terminated).
 * @param len  Length of `raw` in bytes.
 * @param out  Output message struct (caller-allocated).
 * @return     0 on success, -1 on parse failure.
 */
int playos_ipc_message_parse(const char *raw, size_t len,
                             struct playos_ipc_message *out);

/**
 * Build a message from a version + type string.
 *
 * Produces JSON: {"v": version, "type": TYPE}
 * If `extra_json` is non-NULL, its content is spliced in:
 *   {"v": version, "type": TYPE, EXTRA}
 *
 * The caller owns the returned message and must free it with
 * playos_ipc_message_free().
 *
 * @param version    Protocol version (usually PLAYOS_IPC_PROTOCOL_VERSION).
 * @param type       Message type string (e.g. PLAYOS_IPC_TYPE_LAUNCH_GAME).
 * @param extra_json Optional extra fields as JSON fragment (without
 *                   surrounding braces), e.g. `"game_id":"foo"`.
 *                   Pass NULL for no extra fields.
 * @param out        Output message struct (caller-allocated).
 * @return           0 on success, -1 on allocation failure.
 */
int playos_ipc_message_from_type(int version, const char *type,
                                 const char *extra_json,
                                 struct playos_ipc_message *out);

/* ── Server-side socket helpers ────────────────────────────── */

/**
 * Create a listening SOCK_SEQPACKET Unix socket.
 *
 * Binds to `path`, sets mode 0660, and chowns to root:group_name.
 *
 * @param path       Filesystem path for the socket (e.g. /run/playos/control.sock).
 * @param group_name Group name to own the socket.
 * @return           Listening fd on success, -1 on error.
 */
int playos_ipc_server_create(const char *path, const char *group_name);

/**
 * Accept one client connection on a server socket.
 *
 * @param server_fd  Listening socket fd.
 * @return           Client fd on success, -1 on error.
 */
int playos_ipc_server_accept(int server_fd);

/**
 * Verify that the peer on `client_fd` belongs to `group_name`.
 *
 * Uses SO_PEERCRED and getgrnam().
 *
 * @param client_fd  Connected client fd.
 * @param group_name Expected UNIX group name.
 * @return           0 if authorized, -1 if not (or on error).
 */
int playos_ipc_server_check_peer(int client_fd, const char *group_name);

/**
 * Close a server socket and unlink its path.
 *
 * @param server_fd  Listening socket fd.
 * @param path       Path to unlink.
 * @return           0 on success, -1 on error.
 */
int playos_ipc_server_close(int server_fd, const char *path);

/* ── Client-side socket helpers ────────────────────────────── */

/**
 * Connect to a SOCK_SEQPACKET Unix socket.
 *
 * @param path  Socket path to connect to.
 * @return      Connected fd on success, -1 on error.
 */
int playos_ipc_client_connect(const char *path);

/**
 * Send a framed message over a client connection.
 *
 * Convenience wrapper around playos_ipc_frame_write().
 *
 * @param fd   Connected socket fd.
 * @param msg  Message to send.
 * @return     0 on success, -1 on error.
 */
int playos_ipc_client_send(int fd, const struct playos_ipc_message *msg);

/**
 * Receive and parse a framed message from a client connection.
 *
 * @param fd   Connected socket fd.
 * @param out  Output message struct (caller-allocated).
 * @param max  Maximum total frame size (header + body).
 * @return     Total frame bytes read on success, 0 on clean EOF, -1 on error.
 */
int playos_ipc_client_recv(int fd, struct playos_ipc_message *out, size_t max);

/* ── Lifecycle fd helpers ──────────────────────────────────── */

/**
 * Create a pipe pair for lifecycle event delivery.
 *
 * The write end stays with playos-init; the read end is passed
 * to the game process via PLAYOS_LIFECYCLE_FD.
 *
 * @param read_fd   [out] Read end of the pipe.
 * @param write_fd  [out] Write end of the pipe.
 * @return          0 on success, -1 on error.
 */
int playos_lifecycle_create(int *read_fd, int *write_fd);

/**
 * Send a single-byte lifecycle event.
 *
 * @param write_fd  Write end of the lifecycle pipe.
 * @param event     One of PLAYOS_LIFECYCLE_* constants.
 * @return          0 on success, -1 on error.
 */
int playos_lifecycle_send_event(int write_fd, uint8_t event);

#ifdef __cplusplus
}
#endif

#endif /* PLAYOS_IPC_H */

/*
 * PlayOS Runtime IPC — Framing layer
 *
 * Implements the binary framing protocol: magic + length + JSON body.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

/* ── Internal helpers ──────────────────────────────────────── */

/** Decode a little-endian uint32 from raw bytes. */
static uint32_t le32_decode(const unsigned char *p)
{
    return ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/** Encode a uint32 into raw bytes as little-endian. */
static void le32_encode(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ── Public framing functions ──────────────────────────────── */

int playos_ipc_frame_read(int fd, struct playos_ipc_frame *out, size_t max)
{
    ssize_t  n;
    uint32_t body_len;

    /* Minimum frame: 8-byte header */
    if (max < 8) {
        errno = EOVERFLOW;
        return -1;
    }

    /*
     * Single read() to consume the entire SOCK_SEQPACKET message.
     * For pipes this also works — read() returns whatever is available.
     */
    n = read(fd, out, max);
    if (n == 0)
        return 0; /* clean EOF */
    if (n < 0) {
        if (errno == EINTR) {
            /*
             * Interrupted before any data — retry once.
             * After a partial read on SOCK_SEQPACKET the rest
             * is lost, so we cannot retry after partial data.
             */
            n = read(fd, out, max);
            if (n == 0)
                return 0;
            if (n < 0)
                return -1;
        } else {
            return -1;
        }
    }

    /* Must have at least a full header */
    if (n < 8) {
        errno = EBADMSG;
        return -1;
    }

    out->magic  = le32_decode((unsigned char *)out);
    out->length = le32_decode((unsigned char *)out + 4);

    /* Validate magic */
    if (out->magic != PLAYOS_IPC_MAGIC) {
        errno = EBADMSG;
        return -1;
    }

    body_len = out->length;

    /* Check that we received the complete frame */
    if ((size_t)n < 8 + body_len) {
        errno = EBADMSG;
        return -1;
    }

    return (int)n;
}

int playos_ipc_frame_write(int fd, const struct playos_ipc_message *msg)
{
    unsigned char header[8];
    struct iovec  iov[2];
    ssize_t       total;

    if (msg->json_len > PLAYOS_IPC_MAX_BODY) {
        errno = EOVERFLOW;
        return -1;
    }

    le32_encode(header, PLAYOS_IPC_MAGIC);
    le32_encode(header + 4, (uint32_t)msg->json_len);

    iov[0].iov_base = header;
    iov[0].iov_len  = 8;
    iov[1].iov_base = (void *)msg->json_raw;
    iov[1].iov_len  = msg->json_len;

    total = (ssize_t)(8 + msg->json_len);

    while (total > 0) {
        ssize_t rc = writev(fd, iov, 2);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        /* Advance iov past written bytes */
        while (rc > 0 && iov[0].iov_len > 0) {
            if ((size_t)rc >= iov[0].iov_len) {
                rc -= (ssize_t)iov[0].iov_len;
                iov[0].iov_base = (char *)iov[0].iov_base + iov[0].iov_len;
                iov[0].iov_len  = 0;
            } else {
                iov[0].iov_base = (char *)iov[0].iov_base + rc;
                iov[0].iov_len  -= (size_t)rc;
                rc = 0;
            }
        }
        while (rc > 0) {
            if ((size_t)rc >= iov[1].iov_len) {
                rc -= (ssize_t)iov[1].iov_len;
                iov[1].iov_base = (char *)iov[1].iov_base + iov[1].iov_len;
                iov[1].iov_len  = 0;
            } else {
                iov[1].iov_base = (char *)iov[1].iov_base + rc;
                iov[1].iov_len  -= (size_t)rc;
                rc = 0;
            }
        }
        total = (ssize_t)(iov[0].iov_len + iov[1].iov_len);
    }

    return 0;
}

int playos_ipc_frame_validate(const struct playos_ipc_frame *frame)
{
    if (frame->magic != PLAYOS_IPC_MAGIC)
        return -1;
    if (frame->length > PLAYOS_IPC_MAX_BODY)
        return -1;
    return 0;
}

void playos_ipc_message_free(struct playos_ipc_message *msg)
{
    if (!msg)
        return;
    free(msg->json_raw);
    free((void *)msg->type); /* type may be separately allocated by parse */
    msg->json_raw  = NULL;
    msg->json_len  = 0;
    msg->type      = NULL;
    msg->version   = 0;
}

/* ── Minimal JSON parser for "v" and "type" ────────────────── */

/**
 * Skip whitespace (space, tab, newline, carriage-return).
 * Returns pointer to first non-whitespace char (or end of buffer).
 */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/**
 * Find a JSON integer value after the key "v": in raw JSON.
 *
 * Expects:  "v" : <integer>
 * Returns 0 on success, -1 if not found.
 */
static int find_json_v(const char *raw, size_t len, int *out)
{
    const char *end = raw + len;
    const char *p   = raw;

    while (p + 3 < end) {
        p = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!p)
            return -1;

        /* Check for "v" */
        if ((size_t)(end - p) >= 5 && p[1] == 'v' && p[2] == '"') {
            p += 3; /* skip past "v" */
            p = skip_ws(p, end);
            if (p >= end || *p != ':')
                continue;
            p++; /* skip ':' */
            p = skip_ws(p, end);
            if (p >= end)
                return -1;

            /* Parse integer (may be negative) */
            int sign = 1;
            if (*p == '-') {
                sign = -1;
                p++;
                p = skip_ws(p, end);
            }
            if (p >= end || (*p < '0' || *p > '9'))
                return -1;

            int value = 0;
            while (p < end && *p >= '0' && *p <= '9') {
                value = value * 10 + ((int)(*p - '0'));
                p++;
            }
            *out = value * sign;
            return 0;
        }
        p++; /* skip the opening quote, keep searching */
    }
    return -1;
}

/**
 * Find a JSON string value after the key "type": in raw JSON.
 *
 * Expects:  "type" : "STRING"
 *
 * On success, sets *value_start and *value_end to the byte offsets
 * of the string value (excluding surrounding quotes) within `raw`,
 * and copies the NUL-terminated value into `buf` of size `buf_sz`.
 *
 * Returns 0 on success, -1 if not found.
 */
static int find_json_type(const char *raw, size_t len,
                          char *buf, size_t buf_sz,
                          size_t *value_start, size_t *value_len)
{
    const char *end = raw + len;
    const char *p   = raw;

    while (p + 6 < end) {
        p = (const char *)memchr(p, '"', (size_t)(end - p));
        if (!p)
            return -1;

        /* Check for "type" */
        if ((size_t)(end - p) >= 8
            && p[1] == 't' && p[2] == 'y' && p[3] == 'p'
            && p[4] == 'e' && p[5] == '"') {
            p += 6; /* skip past "type" */
            p = skip_ws(p, end);
            if (p >= end || *p != ':')
                continue;
            p++; /* skip ':' */
            p = skip_ws(p, end);
            if (p >= end || *p != '"')
                return -1;
            p++; /* skip opening quote of value */

            /* Record the value start offset */
            if (value_start)
                *value_start = (size_t)(p - raw);

            const char *start = p;
            while (p < end && *p != '"') {
                /* Skip escaped characters */
                if (*p == '\\' && p + 1 < end)
                    p++;
                p++;
            }
            if (p >= end)
                return -1;

            size_t vlen = (size_t)(p - start);
            if (value_len)
                *value_len = vlen;

            if (vlen >= buf_sz)
                vlen = buf_sz - 1;

            memcpy(buf, start, vlen);
            buf[vlen] = '\0';
            return 0;
        }
        p++;
    }
    return -1;
}

/* ── Public message parse / build ──────────────────────────── */

int playos_ipc_message_parse(const char *raw, size_t len,
                             struct playos_ipc_message *out)
{
    char   type_buf[256];
    size_t type_len = 0;

    memset(out, 0, sizeof(*out));

    /* Copy the full JSON body */
    out->json_raw = (char *)malloc(len + 1);
    if (!out->json_raw)
        return -1;

    memcpy(out->json_raw, raw, len);
    out->json_raw[len] = '\0';
    out->json_len = len;

    /* Extract "v" (from the original raw, before any modification) */
    if (find_json_v(raw, len, &out->version) != 0) {
        free(out->json_raw);
        out->json_raw = NULL;
        out->json_len = 0;
        return -1;
    }

    /* Extract "type" — get the NUL-terminated copy and its length */
    if (find_json_type(raw, len, type_buf, sizeof(type_buf),
                       NULL, &type_len) != 0) {
        free(out->json_raw);
        out->json_raw = NULL;
        out->json_len = 0;
        return -1;
    }

    /* Allocate a separate copy for the type string */
    {
        char *type_copy = (char *)malloc(type_len + 1);
        if (!type_copy) {
            free(out->json_raw);
            out->json_raw = NULL;
            out->json_len = 0;
            return -1;
        }
        memcpy(type_copy, type_buf, type_len);
        type_copy[type_len] = '\0';
        out->type = type_copy;
    }

    return 0;
}

int playos_ipc_message_from_type(int version, const char *type,
                                 const char *extra_json,
                                 struct playos_ipc_message *out)
{
    size_t extra_len;
    size_t total_len;
    char  *buf;
    char  *type_copy;
    int    n;

    memset(out, 0, sizeof(*out));

    extra_len = extra_json ? strlen(extra_json) : 0;

    /*
     * Format: {"v":%d, "type":"%s"%s}
     *                    ^^ optional ", EXTRA"
     */
    total_len = 64 + strlen(type) + extra_len; /* generous over-estimate */

    buf = (char *)malloc(total_len + 1);
    if (!buf)
        return -1;

    if (extra_json && extra_len > 0)
        n = snprintf(buf, total_len + 1,
                     "{\"v\":%d,\"type\":\"%s\",%s}",
                     version, type, extra_json);
    else
        n = snprintf(buf, total_len + 1,
                     "{\"v\":%d,\"type\":\"%s\"}",
                     version, type);

    if (n < 0 || (size_t)n > total_len) {
        free(buf);
        return -1;
    }

    /* Allocate a copy of the type string for consistent ownership */
    {
        size_t type_len = strlen(type);
        type_copy = (char *)malloc(type_len + 1);
        if (!type_copy) {
            free(buf);
            return -1;
        }
        memcpy(type_copy, type, type_len);
        type_copy[type_len] = '\0';
    }

    out->json_raw = buf;
    out->json_len = (size_t)n;
    out->version  = version;
    out->type     = type_copy;

    return 0;
}

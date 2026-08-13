/*
 * playos-init/src/mount.c — Filesystem mounting and data partition discovery
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>

#include "playos-init/init.h"
#include "playos-init/mount.h"

/* ── GPT helpers ─────────────────────────────────────────────────── */

/** Decode little-endian uint32 from raw bytes. */
static inline uint32_t le32_dec(const unsigned char *p)
{
    return ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/** Decode little-endian uint64 from raw bytes. */
static inline uint64_t le64_dec(const unsigned char *p)
{
    return ((uint64_t)p[0])
         | ((uint64_t)p[1] << 8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

/*
 * PlayOS data partition type GUID in GPT mixed-endian binary form.
 *
 * UUID: 4B9A8721-1AB3-40E2-9F0C-8B3D4E5F6071
 *   data1 (LE): 0x4B9A8721  →  21 87 9A 4B
 *   data2 (LE): 0x1AB3      →  B3 1A
 *   data3 (LE): 0x40E2      →  E2 40
 *   data4 (BE): 9F0C-8B3D-4E5F6071  →  9F 0C 8B 3D 4E 5F 60 71
 */
static const unsigned char PLAYOS_DATA_TYPE_GUID[16] = {
    0x21, 0x87, 0x9A, 0x4B,
    0xB3, 0x1A,
    0xE2, 0x40,
    0x9F, 0x0C, 0x8B, 0x3D, 0x4E, 0x5F, 0x60, 0x71
};

/* ── External logging ────────────────────────────────────────────── */

void playos_log_write(struct playos_init_state *s, const char *tag,
                      const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* ── Storage version marker (Sprint 6) ──────────────────────────── */

#define PLAYOS_STORAGE_VERSION_MARKER "/data/.playos-storage-version"
#define PLAYOS_STORAGE_VERSION_CURRENT "1"

/* ── Mount virtual filesystems ───────────────────────────────────── */

int playos_mount_virtual(void)
{
    /* /dev — device filesystem */
    if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) != 0) {
        dprintf(STDERR_FILENO, "playos-init: mount /dev failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* /proc — process information */
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        dprintf(STDERR_FILENO, "playos-init: mount /proc failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* /sys — kernel and device information */
    if (mount("sysfs", "/sys", "sysfs", 0, NULL) != 0) {
        dprintf(STDERR_FILENO, "playos-init: mount /sys failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* /run — runtime data, tmpfs */
    if (mount("tmpfs", "/run", "tmpfs", 0, "mode=0755") != 0) {
        dprintf(STDERR_FILENO, "playos-init: mount /run failed: %s\n",
                strerror(errno));
        return -1;
    }

    /* /dev/shm — POSIX shared memory, needed by wlroots shm_open() */
    mkdir("/dev/shm", 0755);
    if (mount("tmpfs", "/dev/shm", "tmpfs", 0, "mode=1777") != 0) {
        dprintf(STDERR_FILENO, "playos-init: mount /dev/shm failed: %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}

/* ── Data partition discovery ────────────────────────────────────── */

/*
 * Search for the data partition in order:
 *   1. Partition with label "playos-data" via /dev/disk/by-label/
 *   2. Direct scan of common block devices (for systems without udev)
 *   3. GPT partition type GUID (reserved for future use)
 *   4. UUID from kernel command line: playos.data_uuid=<uuid>
 */

/*
 * Read ext4 volume label from superblock at offset 0x400.
 * The label is at offset 0x78 within the superblock (0x478 total),
 * 16 bytes, null-terminated.
 */
static int read_ext4_label(const char *device, char *label, size_t label_size)
{
    int fd = open(device, O_RDONLY);
    if (fd < 0) return -1;

    /* ext4 superblock starts at byte 1024 (0x400).
     * Magic (0xEF53) is at superblock+0x38 = 0x438.
     * Volume label (16 bytes) is at superblock+0x78 = 0x478. */

    /* Check magic at offset 0x438 */
    if (lseek(fd, 0x438, SEEK_SET) != 0x438) {
        close(fd);
        return -1;
    }
    unsigned char magic_buf[2];
    if (read(fd, magic_buf, 2) != 2) {
        close(fd);
        return -1;
    }
    if (magic_buf[0] != 0x53 || magic_buf[1] != 0xEF) {
        close(fd);
        return -1;
    }

    /* Read label at offset 0x478 */
    if (lseek(fd, 0x478, SEEK_SET) != 0x478) {
        close(fd);
        return -1;
    }
    ssize_t n = read(fd, label, label_size - 1);
    close(fd);
    if (n <= 0) return -1;

    label[n] = '\0';
    return 0;
}

/* Check whether a partition's parent disk is removable (e.g. USB stick).
 * /dev/sda3 -> sda, /dev/nvme0n1p3 -> nvme0n1, /dev/mmcblk0p3 -> mmcblk0 */
static int device_is_removable(const char *dev_path)
{
    const char *base = strrchr(dev_path, '/');
    base = base ? base + 1 : dev_path;

    char disk[64];
    snprintf(disk, sizeof(disk), "%s", base);
    size_t len = strlen(disk);

    /* nvme/mmc style: strip trailing "pN"; sd/vd style: strip digits */
    while (len > 0 && disk[len - 1] >= '0' && disk[len - 1] <= '9')
        disk[--len] = '\0';
    if (len > 0 && disk[len - 1] == 'p' &&
        (strstr(disk, "nvme") || strstr(disk, "mmcblk")))
        disk[--len] = '\0';
    if (len == 0)
        return 0;

    char sys_path[128];
    snprintf(sys_path, sizeof(sys_path), "/sys/block/%s/removable", disk);
    FILE *f = fopen(sys_path, "r");
    if (!f)
        return 0;
    int c = fgetc(f);
    fclose(f);
    return c == '1';
}

static int find_data_partition(char *device_path, size_t path_size)
{
    /* When several playos-data partitions exist (e.g. a USB stick and an
     * internal install), prefer the one on removable media — that is the
     * device we booted from. Otherwise the LAST match wins: USB storage
     * enumerates after built-in NVMe, so the stick tends to come last. */
    char fallback_path[128] = {0};
    int have_fallback = 0;

    /* Try up to 10 times with increasing delays (100ms → 1000ms)
     * because block device detection may be asynchronous even with
     * built-in virtio-blk. Total max wait: ~5s. */
    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) {
            usleep(attempt * 100000); /* 100ms, 200ms, 300ms... */
        }

        /* Strategy 1: Label "playos-data" via udev symlinks */
        const char *label_path = "/dev/disk/by-label/playos-data";
        if (access(label_path, F_OK) == 0) {
            ssize_t len = readlink(label_path, device_path, path_size - 1);
            if (len > 0) {
                device_path[len] = '\0';
                if (device_is_removable(device_path)) {
                    dprintf(STDERR_FILENO,
                            "playos-init: data partition by label (removable): %s\n",
                            device_path);
                    return 0;
                }
                /* remember last non-removable match as fallback */
                snprintf(fallback_path, sizeof(fallback_path), "%s", device_path);
                have_fallback = 1;
            }
        }

        /* Strategy 2: Direct scan of common block devices (no udev) */
        const char *candidates[] = {
            "/dev/vda", "/dev/vdb", "/dev/sda", "/dev/sdb",
            "/dev/vda1", "/dev/sda1",
            NULL
        };
        for (const char **c = candidates; *c; c++) {
            if (access(*c, F_OK) != 0) continue;
            char label[32] = {0};
            if (read_ext4_label(*c, label, sizeof(label)) == 0) {
                if (strcmp(label, "playos-data") == 0) {
                    if (device_is_removable(*c)) {
                        snprintf(device_path, path_size, "%s", *c);
                        dprintf(STDERR_FILENO,
                                "playos-init: data partition by scan (removable): %s\n",
                                device_path);
                        return 0;
                    }
                    /* remember last non-removable match as fallback */
                    snprintf(fallback_path, sizeof(fallback_path), "%s", *c);
                    have_fallback = 1;
                }
            }
        }

        /* Strategy 3: Scan devices from /proc/partitions */
        FILE *parts = fopen("/proc/partitions", "r");
        if (parts) {
            char line[256];
            /* Skip header lines */
            fgets(line, sizeof(line), parts);
            fgets(line, sizeof(line), parts);
            while (fgets(line, sizeof(line), parts)) {
                char name[64] = {0};
                /* Format: major minor #blocks name */
                if (sscanf(line, "%*d %*d %*d %63s", name) == 1) {
                    char dev_path[128];
                    snprintf(dev_path, sizeof(dev_path), "/dev/%s", name);
                    if (access(dev_path, F_OK) != 0) continue;
                    char label[32] = {0};
                    if (read_ext4_label(dev_path, label, sizeof(label)) == 0) {
                        if (strcmp(label, "playos-data") == 0) {
                            if (device_is_removable(dev_path)) {
                                snprintf(device_path, path_size, "%s", dev_path);
                                fclose(parts);
                                dprintf(STDERR_FILENO,
                                        "playos-init: data partition by proc scan (removable): %s\n",
                                        device_path);
                                return 0;
                            }
                            /* remember last non-removable match as fallback */
                            snprintf(fallback_path, sizeof(fallback_path),
                                     "%s", dev_path);
                            have_fallback = 1;
                        }
                    }
                }
            }
            fclose(parts);
        }
    }

    /* No removable playos-data found — accept the last one seen */
    if (have_fallback) {
        snprintf(device_path, path_size, "%s", fallback_path);
        dprintf(STDERR_FILENO,
                "playos-init: data partition (last-found fallback): %s\n",
                device_path);
        return 0;
    }

    /* Strategy 4: GPT partition type GUID */
    {
        FILE *gpt_parts = fopen("/proc/partitions", "r");
        if (gpt_parts) {
            char gpt_line[256];
            fgets(gpt_line, sizeof(gpt_line), gpt_parts); /* skip hdr */
            fgets(gpt_line, sizeof(gpt_line), gpt_parts); /* skip hdr */
            while (fgets(gpt_line, sizeof(gpt_line), gpt_parts)) {
                char gpt_name[64] = {0};
                if (sscanf(gpt_line, "%*d %*d %*d %63s", gpt_name) != 1)
                    continue;

                char gpt_dev[128];
                snprintf(gpt_dev, sizeof(gpt_dev), "/dev/%s", gpt_name);

                int gfd = open(gpt_dev, O_RDONLY);
                if (gfd < 0) continue;

                /* Read GPT header at LBA 1 (byte 512) */
                unsigned char hdr[512];
                if (lseek(gfd, 512, SEEK_SET) != 512
                    || read(gfd, hdr, 512) != 512) {
                    close(gfd);
                    continue;
                }

                /* Validate GPT signature "EFI PART" */
                if (memcmp(hdr, "EFI PART", 8) != 0) {
                    close(gfd);
                    continue;
                }

                uint64_t entry_lba  = le64_dec(hdr + 72);
                uint32_t entry_cnt  = le32_dec(hdr + 80);
                uint32_t entry_sz   = le32_dec(hdr + 84);

                if (entry_cnt == 0 || entry_sz < 128
                    || entry_cnt > 256) {
                    close(gfd);
                    continue;
                }

                size_t  arr_sz = (size_t)entry_cnt * entry_sz;
                off_t   arr_of = (off_t)entry_lba * 512;
                unsigned char *entries = (unsigned char *)malloc(arr_sz);
                if (!entries) { close(gfd); continue; }

                if (lseek(gfd, arr_of, SEEK_SET) != arr_of
                    || read(gfd, entries, arr_sz) != (ssize_t)arr_sz) {
                    free(entries);
                    close(gfd);
                    continue;
                }
                close(gfd);

                int part_num = -1;
                for (uint32_t i = 0; i < entry_cnt; i++) {
                    unsigned char *e = entries + (size_t)i * entry_sz;
                    if (memcmp(e, PLAYOS_DATA_TYPE_GUID, 16) == 0) {
                        part_num = (int)(i + 1); /* 1-based */
                        break;
                    }
                }
                free(entries);

                if (part_num > 0) {
                    /* Build partition device path.
                     * NVMe (/dev/nvme0n1p1) and MMC (/dev/mmcblk0p1)
                     * use 'p' separator.  SCSI/SATA/VirtIO use simple
                     * suffix: /dev/sda1, /dev/vda1. */
                    if (strncmp(gpt_name, "nvme", 4) == 0
                        || strstr(gpt_name, "mmcblk")) {
                        snprintf(device_path, path_size,
                                 "/dev/%sp%d", gpt_name, part_num);
                    } else {
                        snprintf(device_path, path_size,
                                 "/dev/%s%d", gpt_name, part_num);
                    }
                    fclose(gpt_parts);
                    dprintf(STDERR_FILENO,
                            "playos-init: data partition by GPT GUID:"
                            " %s\n", device_path);
                    return 0;
                }
            }
            fclose(gpt_parts);
        }
    }

    /* Strategy 5: Kernel command line */
    FILE *cmdline = fopen("/proc/cmdline", "r");
    if (cmdline) {
        char buf[4096] = {0};
        if (fgets(buf, sizeof(buf), cmdline)) {
            char *p = strstr(buf, "playos.data_uuid=");
            if (p) {
                p += strlen("playos.data_uuid=");
                char *end = strchrnul(p, ' ');
                int uuid_len = (int)(end - p);
                if (uuid_len > 0 && uuid_len < 64) {
                    snprintf(device_path, path_size,
                             "/dev/disk/by-uuid/%.*s", uuid_len, p);
                    if (access(device_path, F_OK) == 0) {
                        fclose(cmdline);
                        dprintf(STDERR_FILENO,
                                "playos-init: data partition by UUID: %s\n",
                                device_path);
                        return 0;
                    }
                }
            }
        }
        fclose(cmdline);
    }

    return -1;
}

/* ── Boot marker (diagnostic) ────────────────────────────────────── */

/* Write playos-boot.txt to EVERY playos-data partition found, naming
 * the device init actually mounted as /data. Makes it possible to tell
 * from any stick after the fact whether init ran and which device it
 * chose as the data partition. */
static void write_data_markers(const char *chosen_dev)
{
    FILE *parts = fopen("/proc/partitions", "r");
    if (!parts)
        return;

    char line[256];
    fgets(line, sizeof(line), parts); /* header */
    fgets(line, sizeof(line), parts); /* blank  */

    while (fgets(line, sizeof(line), parts)) {
        char name[64] = {0};
        if (sscanf(line, "%*d %*d %*d %63s", name) != 1)
            continue;

        char dev[128];
        snprintf(dev, sizeof(dev), "/dev/%s", name);
        if (access(dev, F_OK) != 0)
            continue;

        char label[32] = {0};
        if (read_ext4_label(dev, label, sizeof(label)) != 0 ||
            strcmp(label, "playos-data") != 0)
            continue;

        /* The chosen device is already mounted at /data; mount the
         * others briefly to drop the marker. */
        const char *target = "/data";
        int mounted_here   = 0;
        if (strcmp(dev, chosen_dev) != 0) {
            target = "/tmp/dmark";
            mkdir(target, 0755);
            if (mount(dev, target, "ext4", 0, NULL) != 0)
                continue;
            mounted_here = 1;
        }

        char mpath[160];
        snprintf(mpath, sizeof(mpath), "%s/playos-boot.txt", target);
        int fd = open(mpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            dprintf(fd,
                    "playos-init boot marker\n"
                    "this_device=%s\n"
                    "chosen_data=%s\n"
                    "unix_time=%ld\n",
                    dev, chosen_dev, (long)time(NULL));
            close(fd);
            sync();
        }

        if (mounted_here)
            umount(target);
    }

    fclose(parts);
}

/* Log every block device the kernel knows about so a failed data-partition
 * discovery produces a diagnostic trail rather than a silent halt. */
static void log_available_block_devices(void)
{
    FILE *parts = fopen("/proc/partitions", "r");
    if (!parts) {
        dprintf(STDERR_FILENO,
                "playos-init: cannot read /proc/partitions for diagnostics\n");
        return;
    }

    char line[256];
    dprintf(STDERR_FILENO, "playos-init: available block devices:\n");

    fgets(line, sizeof(line), parts); /* header */
    fgets(line, sizeof(line), parts); /* blank  */
    while (fgets(line, sizeof(line), parts)) {
        char name[64] = {0};
        if (sscanf(line, "%*d %*d %*d %63s", name) == 1)
            dprintf(STDERR_FILENO, "  /dev/%s\n", name);
    }

    fclose(parts);
}

int playos_mount_data(struct playos_init_state *state)
{
    (void)state;

    char device_path[256] = {0};

    if (find_data_partition(device_path, sizeof(device_path)) != 0) {
        dprintf(STDERR_FILENO,
                "playos-init: data partition not found "
                "(label=playos-data, block-device scan, /proc/partitions, "
                "GPT GUID, cmdline playos.data_uuid=)\n");
        log_available_block_devices();
        return -1;
    }

    /* Create mount point */
    mkdir("/data", 0755);

    /* Mount the data partition (ext4 assumed, read-write) */
    if (mount(device_path, "/data", "ext4", 0, NULL) != 0) {
        /* Try common filesystems */
        if (mount(device_path, "/data", "vfat", 0, NULL) != 0) {
            if (mount(device_path, "/data", "auto", 0, NULL) != 0) {
                dprintf(STDERR_FILENO,
                        "playos-init: mount /data failed: %s\n",
                        strerror(errno));
                return -1;
            }
        }
    }

    dprintf(STDERR_FILENO, "playos-init: /data mounted from %s\n", device_path);

    /* Drop a marker on every playos-data partition so the choice is
     * visible from any of them after the fact */
    write_data_markers(device_path);

    return 0;
}

/* ── First-boot directories ──────────────────────────────────────── */

/*
 * Validate (or create) the /data storage version marker.
 *
 * Returns:
 *   1  — first boot: the marker was absent and has now been created
 *   0  — marker exists and matches the current schema version
 *  -1  — marker could not be read or created
 *
 * On a version mismatch we warn but still return 0 so boot continues.
 */
static int playos_storage_version_validate(void)
{
    int fd = open(PLAYOS_STORAGE_VERSION_MARKER, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            /* First boot — stamp the current schema version. */
            fd = open(PLAYOS_STORAGE_VERSION_MARKER,
                      O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) {
                dprintf(STDERR_FILENO,
                        "playos-init: cannot create storage version marker: %s\n",
                        strerror(errno));
                return -1;
            }
            dprintf(fd, "%s\n", PLAYOS_STORAGE_VERSION_CURRENT);
            close(fd);
            dprintf(STDERR_FILENO,
                    "playos-init: storage version marker created (%s)\n",
                    PLAYOS_STORAGE_VERSION_CURRENT);
            return 1;
        }

        dprintf(STDERR_FILENO,
                "playos-init: cannot read storage version marker: %s\n",
                strerror(errno));
        return -1;
    }

    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n < 0) {
        dprintf(STDERR_FILENO,
                "playos-init: storage version marker read error: %s\n",
                strerror(errno));
        return -1;
    }
    buf[n] = '\0';

    /* Trim trailing newline/whitespace before comparing. */
    char *end = buf + n;
    while (end > buf &&
           (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
        *--end = '\0';

    if (strcmp(buf, PLAYOS_STORAGE_VERSION_CURRENT) != 0) {
        dprintf(STDERR_FILENO,
                "playos-init: WARN storage version mismatch: found '%s', "
                "expected '%s' — continuing\n",
                buf, PLAYOS_STORAGE_VERSION_CURRENT);
    } else {
        dprintf(STDERR_FILENO,
                "playos-init: storage version marker validated (%s)\n",
                PLAYOS_STORAGE_VERSION_CURRENT);
    }

    return 0;
}

/* ── Shipped game seeding (Sprint 6) ─────────────────────────────── */

/*
 * Recursively copy a single regular file, preserving the executable bit.
 */
static int playos_copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -1;

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) {
        close(in);
        return -1;
    }

    char buf[4096];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                rc = -1;
                break;
            }
            off += w;
        }
        if (rc != 0)
            break;
    }
    if (n < 0)
        rc = -1;

    close(out);
    close(in);
    return rc;
}

/*
 * Recursively copy a directory tree from the read-only rootfs into the
 * writable /data partition. Entries keep the source mode; only regular
 * files and directories are handled (the shipped games contain only these).
 */
static int playos_copy_tree(const char *src, const char *dst)
{
    struct stat st;
    if (stat(src, &st) != 0)
        return -1;

    if (S_ISREG(st.st_mode))
        return playos_copy_file(src, dst, st.st_mode & 0777);

    if (!S_ISDIR(st.st_mode))
        return 0;

    if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST)
        return -1;

    DIR *d = opendir(src);
    if (!d)
        return -1;

    int rc = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char src_path[PATH_MAX];
        char dst_path[PATH_MAX];
        snprintf(src_path, sizeof(src_path), "%s/%s", src, de->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, de->d_name);

        if (playos_copy_tree(src_path, dst_path) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    return rc;
}

/*
 * Seed the shipped sample games from the read-only rootfs into the data
 * partition on first boot. The library scans /data/games, so the titles
 * appear only after this copy. A missing seed source is non-fatal: an
 * image built without the package simply boots with an empty library.
 */
static void playos_seed_shipped_games(void)
{
    const char *seed_root = "/usr/share/playos/games";
    DIR *d = opendir(seed_root);
    if (!d) {
        dprintf(STDERR_FILENO,
                "playos-init: no shipped games at %s to seed\n", seed_root);
        return;
    }

    struct dirent *de;
    int seeded = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        char src[PATH_MAX];
        char dst[PATH_MAX];
        snprintf(src, sizeof(src), "%s/%s", seed_root, de->d_name);
        snprintf(dst, sizeof(dst), "/data/games/%s", de->d_name);

        if (playos_copy_tree(src, dst) == 0) {
            dprintf(STDERR_FILENO, "playos-init: seeded game %s\n",
                    de->d_name);
            seeded++;
        } else {
            dprintf(STDERR_FILENO, "playos-init: failed to seed game %s\n",
                    de->d_name);
        }
    }
    closedir(d);

    if (seeded > 0)
        sync();
}

int playos_data_create_dirs(void)
{
    /* Final MVP /data layout — /data/log is the shipped singular spelling. */
    const char *dirs[] = {
        "/data/games",
        "/data/saves",
        "/data/cache",
        "/data/log",
        "/data/system",
        "/data/profiles",
        "/data/resources",
        "/data/downloads",
        "/data/updates",
        "/data/screenshots",
        "/data/config",
        NULL
    };

    for (const char **d = dirs; *d; d++) {
        if (mkdir(*d, 0755) != 0 && errno != EEXIST) {
            dprintf(STDERR_FILENO,
                    "playos-init: mkdir %s failed: %s\n",
                    *d, strerror(errno));
            return -1;
        }
    }

    /* Validate the schema version on every mount; a fresh marker (return 1)
     * means first boot, so seed the shipped games into the new /data. */
    int first_boot = playos_storage_version_validate();
    if (first_boot < 0) {
        dprintf(STDERR_FILENO,
                "playos-init: storage provisioning incomplete "
                "(version marker unavailable)\n");
        return -1;
    }

    if (first_boot == 1)
        playos_seed_shipped_games();

    return 0;
}

/* ── Boot stage marker ───────────────────────────────────────────── */

int playos_boot_stage_write(enum playos_boot_stage stage)
{
    /* Ensure /run/playos exists */
    mkdir("/run/playos", 0755);

    int fd = open("/run/playos/boot-stage",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return -1;

    const char *stages[] = {
        [BOOT_STAGE_START]          = "start",
        [BOOT_STAGE_MOUNTS]         = "mounts",
        [BOOT_STAGE_DATA_DISCOVERY] = "data_discovery",
        [BOOT_STAGE_DATA_MOUNTED]   = "data_mounted",
        [BOOT_STAGE_IPC_READY]      = "ipc_ready",
        [BOOT_STAGE_COMPOSITOR]     = "compositor",
        [BOOT_STAGE_READY]          = "ready",
        [BOOT_STAGE_RECOVERY]       = "recovery",
    };

    const char *name = (stage < sizeof(stages)/sizeof(stages[0]) && stages[stage])
                       ? stages[stage] : "unknown";

    dprintf(fd, "%s\n", name);
    close(fd);
    return 0;
}

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
#include <sys/wait.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>

#include "playos-init/init.h"
#include "playos-init/mount.h"
#include "playos-init/boot_slot.h"

extern char **environ;

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

/** Decode little-endian uint16 from raw bytes. */
static inline uint16_t le16_dec(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
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

    /* /tmp — writable scratch space. The active slot is a read-only squashfs,
     * so /tmp must be a tmpfs for tools that assume a writable temp dir (the
     * stock udhcpc default.script calls mktemp, which defaults to /tmp and
     * fails with EROFS otherwise). mode=1777 matches the conventional sticky
     * bit; /var/tmp is a symlink to ../tmp so it is covered too. */
    mkdir("/tmp", 0755);
    if (mount("tmpfs", "/tmp", "tmpfs", 0, "mode=1777") != 0) {
        dprintf(STDERR_FILENO, "playos-init: mount /tmp failed: %s\n",
                strerror(errno));
        return -1;
    }

    return 0;
}

/* ── udev startup (Sprint 12) ────────────────────────────────────── */

/* Run a command to completion and return its exit status, or -1 if it could
 * not be spawned or was killed by a signal. Used only for best-effort udev
 * bring-up, so failures are logged but never abort boot. */
static int run_cmd(const char *path, char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;

    if (pid == 0) {
        execv(path, argv);
        dprintf(STDERR_FILENO, "playos-init: exec %s failed: %s\n",
                path, strerror(errno));
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return -1;
}

int playos_udev_start(struct playos_init_state *s)
{
    /* Best-effort: an image built without eudev simply skips device
     * permission management. Games then depend only on devtmpfs defaults,
     * which is exactly the pre-udev behaviour we are replacing. */
    if (access("/sbin/udevd", X_OK) != 0) {
        playos_log_write(s, "udev",
                         "udevd not present — skipping device permission setup");
        return 0;
    }

    /* udevd's control socket and database live under /run/udev. */
    mkdir("/run/udev", 0755);

    pid_t pid = fork();
    if (pid < 0) {
        playos_log_write(s, "udev", "fork failed: %s", strerror(errno));
        return -1;
    }

    if (pid == 0) {
        /* Stay in the foreground as a supervised child of PID 1 (no
         * --daemon), so udevd outlives boot and keeps reacting to device
         * hotplug for the whole session. */
        execl("/sbin/udevd", "udevd", NULL);
        dprintf(STDERR_FILENO, "playos-init: exec udevd failed: %s\n",
                strerror(errno));
        _exit(127);
    }
    playos_log_write(s, "udev", "udevd started (PID %d)", pid);

    /* Re-run the rule engine over already-enumerated devices, then wait for
     * the queue to drain so every /dev node carries its final owner/group/
     * mode (render, audio, input, ...) before the compositor and games start.
     * Both are best-effort: a missing udevadm or a slow queue must never
     * block boot. */
    if (access("/usr/bin/udevadm", X_OK) != 0) {
        playos_log_write(s, "udev",
                         "udevadm not present — skipping trigger/settle");
        return 0;
    }

    char *const trigger_argv[] = {
        "/usr/bin/udevadm", "trigger", "--action=add", NULL
    };
    int rc = run_cmd(trigger_argv[0], trigger_argv);
    if (rc != 0)
        playos_log_write(s, "udev", "WARN: udevadm trigger exited %d", rc);

    char *const settle_argv[] = {
        "/usr/bin/udevadm", "settle", NULL
    };
    rc = run_cmd(settle_argv[0], settle_argv);
    if (rc != 0)
        playos_log_write(s, "udev", "WARN: udevadm settle exited %d", rc);

    return 0;
}

/* ── Data partition discovery ────────────────────────────────────── */

/*
 * Search for the data partition. Highest-priority first:
 *   0. The playos-data partition on the disk that / is mounted from
 *   1. Partition with label "playos-data" via /dev/disk/by-label/
 *   2. Direct scan of common block devices (for systems without udev)
 *   3. /proc/partitions scan for label "playos-data"
 *   4. GPT partition type GUID (reserved for future use)
 *   5. UUID from kernel command line: playos.data_uuid=<uuid>
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

/* Extract the whole-disk name from a partition device name.
 *   nvme0n1p5 -> nvme0n1   mmcblk0p3 -> mmcblk0   sda3 -> sda
 * Whole disks (nvme0n1, sda) pass through unchanged. */
static void partition_parent_disk(const char *name, char *disk, size_t size)
{
    size_t len = strlen(name);

    /* nvme/mmcblk partition nodes use a 'p' separator before the number. */
    if (strncmp(name, "nvme", 4) == 0 || strncmp(name, "mmcblk", 6) == 0) {
        size_t cut = len;
        const char *p = strrchr(name, 'p');
        if (p && p > name) {
            const char *q = p + 1;
            if (*q >= '1' && *q <= '9') {
                int digits = 1;
                for (const char *r = q; *r; r++) {
                    if (*r < '0' || *r > '9') { digits = 0; break; }
                }
                if (digits)
                    cut = (size_t)(p - name);
            }
        }
        if (cut >= size) cut = size - 1;
        memcpy(disk, name, cut);
        disk[cut] = '\0';
        return;
    }

    /* sd/vd/virtio style: strip trailing digits. */
    size_t cut = len;
    while (cut > 0 && name[cut - 1] >= '0' && name[cut - 1] <= '9')
        cut--;
    if (cut >= size) cut = size - 1;
    memcpy(disk, name, cut);
    disk[cut] = '\0';
}

/* Trailing partition number from a device name (nvme0n1p5 -> 5, sda3 -> 3),
 * or -1 when the name carries no numeric partition suffix. */
static int partition_number(const char *name)
{
    size_t len = strlen(name);
    size_t i = len;
    while (i > 0 && name[i - 1] >= '0' && name[i - 1] <= '9')
        i--;
    if (i == len || i == 0)
        return -1;
    return atoi(name + i);
}

/* Check whether a partition's parent disk is removable (e.g. USB stick). */
static int device_is_removable(const char *dev_path)
{
    const char *base = strrchr(dev_path, '/');
    base = base ? base + 1 : dev_path;

    char disk[64];
    partition_parent_disk(base, disk, sizeof(disk));
    if (disk[0] == '\0')
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

/* Resolve the whole disk backing the root filesystem from /proc/mounts.
 * After the slot pivot, / is the squashfs slot (e.g. /dev/nvme0n1p2); before
 * the pivot, or for a live/installer initramfs boot, / has no /dev/ backing
 * device. Fills `disk` with the parent disk name and returns 0, else -1. */
static int get_root_disk(char *disk, size_t size)
{
    FILE *f = fopen("/proc/mounts", "r");
    if (!f)
        return -1;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char src[128], dst[256], fstype[64];
        if (sscanf(line, "%127s %255s %63s", src, dst, fstype) != 3)
            continue;
        if (strcmp(dst, "/") != 0)
            continue;

        if (strncmp(src, "/dev/", 5) != 0) {
            disk[0] = '\0';
            fclose(f);
            return -1;
        }

        partition_parent_disk(src + 5, disk, size);
        fclose(f);
        return (disk[0] != '\0') ? 0 : -1;
    }

    fclose(f);
    return -1;
}

/* Find the playos-data partition that lives on `root_disk`. When more than
 * one candidate exists (a stale slot-B superblock left by an older layout),
 * prefer the highest partition number — data is always the last partition. */
static int find_data_on_disk(const char *root_disk, char *device_path,
                             size_t path_size)
{
    FILE *parts = fopen("/proc/partitions", "r");
    if (!parts)
        return -1;

    char line[256];
    fgets(line, sizeof(line), parts); /* header */
    fgets(line, sizeof(line), parts); /* blank  */

    char best_path[128] = {0};
    int  best_partno   = -1;

    while (fgets(line, sizeof(line), parts)) {
        char name[64] = {0};
        if (sscanf(line, "%*d %*d %*d %63s", name) != 1)
            continue;

        char disk[64];
        partition_parent_disk(name, disk, sizeof(disk));
        if (strcmp(disk, root_disk) != 0)
            continue;
        if (strcmp(disk, name) == 0)   /* whole disk, not a partition */
            continue;

        char dev_path[128];
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", name);
        if (access(dev_path, F_OK) != 0)
            continue;

        char label[32] = {0};
        if (read_ext4_label(dev_path, label, sizeof(label)) != 0 ||
            strcmp(label, "playos-data") != 0)
            continue;

        int partno = partition_number(name);
        if (partno > best_partno) {
            best_partno = partno;
            snprintf(best_path, sizeof(best_path), "%s", dev_path);
        }
    }

    fclose(parts);

    if (best_partno < 0)
        return -1;

    snprintf(device_path, path_size, "%s", best_path);
    return 0;
}

/* Return non-zero when a candidate data partition should be preferred.
 *
 * Both normal and installer boots prefer a genuinely removable device — the
 * USB stick / boot medium — over a fixed internal disk. The sysfs
 * `removable` flag is the exact same signal the installer uses
 * (playos_disk_enumerate skips removable != 0 when choosing install
 * targets), so keeping the two consistent means a leftover playos-data
 * partition on an internal SATA/NVMe/eMMC disk can never hijack /data.
 *
 * `require_removable` does not change the preference test: it only governs
 * whether find_data_partition() may later fall back to a fixed disk
 * (normal boot) or must fail closed (installer boot). */
static int data_partition_is_preferred(const char *dev_path,
                                       int require_removable)
{
    if (device_is_removable(dev_path))
        return 1;

    /* In installer mode a non-removable candidate is a leftover playos-data
     * partition on the internal install target. Name it loudly so a future
     * EBUSY-style failure is self-explanatory instead of silent. */
    if (require_removable)
        dprintf(STDERR_FILENO,
                "playos-init: installer mode: ignoring playos-data on "
                "internal/install-target disk %s (not the boot medium)\n",
                dev_path);

    return 0;
}

static int find_data_partition(char *device_path, size_t path_size,
                               int require_removable)
{
    /* Resolution order:
     *   0. the playos-data partition on the disk we booted from (installed
     *      systems must never be hijacked by a still-attached installer USB)
     *   1. a removable playos-data partition (the installer/live-USB case)
     *   2. any remaining playos-data partition as a last-resort fallback */
    char fallback_path[128] = {0};
    int have_fallback = 0;

    char root_disk[64] = {0};
    int have_root_disk = (get_root_disk(root_disk, sizeof(root_disk)) == 0);

    /* Try up to 10 times with increasing delays (100ms → 1000ms)
     * because block device detection may be asynchronous even with
     * built-in virtio-blk. Total max wait: ~5s. */
    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) {
            usleep(attempt * 100000); /* 100ms, 200ms, 300ms... */
        }

        /* Strategy 0: data partition on the disk we booted from. */
        if (!require_removable && have_root_disk &&
            find_data_on_disk(root_disk, device_path, path_size) == 0) {
            dprintf(STDERR_FILENO,
                    "playos-init: data partition on boot disk (%s): %s\n",
                    root_disk, device_path);
            return 0;
        }

        /* Strategy 1: Label "playos-data" via udev symlinks */
        const char *label_path = "/dev/disk/by-label/playos-data";
        if (access(label_path, F_OK) == 0) {
            ssize_t len = readlink(label_path, device_path, path_size - 1);
            if (len > 0) {
                device_path[len] = '\0';
                /* eudev creates these by-label symlinks with relative targets
                 * ("../../sda3"); a raw relative path is useless to mount()
                 * resolved from PID1's CWD ("/"). Resolve to an absolute path
                 * so the installer can actually mount the USB data partition. */
                if (device_path[0] != '/') {
                    char resolved[PATH_MAX];
                    if (realpath(label_path, resolved))
                        snprintf(device_path, path_size, "%s", resolved);
                }
                if (data_partition_is_preferred(device_path, require_removable)) {
                    dprintf(STDERR_FILENO,
                            "playos-init: data partition by label (removable): %s\n",
                            device_path);
                    return 0;
                }
                /* remember last non-removable match as fallback */
                if (!require_removable) {
                    snprintf(fallback_path, sizeof(fallback_path), "%s", device_path);
                    have_fallback = 1;
                }
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
                    if (data_partition_is_preferred(*c, require_removable)) {
                        snprintf(device_path, path_size, "%s", *c);
                        dprintf(STDERR_FILENO,
                                "playos-init: data partition by scan (removable): %s\n",
                                device_path);
                        return 0;
                    }
                    /* remember last non-removable match as fallback */
                    if (!require_removable) {
                        snprintf(fallback_path, sizeof(fallback_path), "%s", *c);
                        have_fallback = 1;
                    }
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
                            if (data_partition_is_preferred(dev_path, require_removable)) {
                                snprintf(device_path, path_size, "%s", dev_path);
                                fclose(parts);
                                dprintf(STDERR_FILENO,
                                        "playos-init: data partition by proc scan (removable): %s\n",
                                        device_path);
                                return 0;
                            }
                            /* remember last non-removable match as fallback */
                            if (!require_removable) {
                                snprintf(fallback_path, sizeof(fallback_path),
                                         "%s", dev_path);
                                have_fallback = 1;
                            }
                        }
                    }
                }
            }
            fclose(parts);
        }
    }

    /* No removable playos-data found — accept the last one seen */
    if (!require_removable && have_fallback) {
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
                    if (!data_partition_is_preferred(device_path, require_removable)) {
                        /* internal target in installer mode — keep scanning */
                        continue;
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
                    if (access(device_path, F_OK) == 0 &&
                        data_partition_is_preferred(device_path, require_removable)) {
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

/* ── Kernel command line flags (Sprint 10 installer trigger) ─────── */

/* Read /proc/cmdline and test whether `flag` is one of its
 * whitespace-delimited tokens. A token matches only when it is
 * byte-for-byte equal to `flag` (so "playos.mode=install" never matches
 * a longer "playos.mode=installer" token). */
int playos_cmdline_has_flag(const char *flag)
{
    FILE *cmdline = fopen("/proc/cmdline", "r");
    if (!cmdline)
        return 0;

    char buf[4096] = {0};
    int found = 0;

    if (fgets(buf, sizeof(buf), cmdline)) {
        char *save = NULL;
        char *tok = strtok_r(buf, " \t\r\n", &save);
        while (tok != NULL) {
            if (strcmp(tok, flag) == 0) {
                found = 1;
                break;
            }
            tok = strtok_r(NULL, " \t\r\n", &save);
        }
    }

    fclose(cmdline);
    return found;
}

int playos_install_mode_requested(void)
{
    return playos_cmdline_has_flag("playos.mode=install");
}

int playos_auto_install_requested(void)
{
    return playos_cmdline_has_flag("playos.install.auto");
}

/* ── A/B boot slot partition lookup (Sprint 11) ───────────────────── */

/*
 * Scan /proc/partitions and, for each /dev/<name> device, read its GPT
 * header at LBA 1 and walk the partition-entry array looking for a
 * partition whose UTF-16LE name label matches `label`. On success fills
 * `device_path` with the /dev/<name><sep><N> node and returns 0; returns
 * -1 if no such partition is found.
 */
int playos_find_partition_by_label(const char *label, char *device_path,
                                   size_t path_size)
{
    if (!label || !device_path || path_size == 0)
        return -1;

    FILE *parts = fopen("/proc/partitions", "r");
    if (!parts)
        return -1;

    char line[256];
    if (!fgets(line, sizeof(line), parts)) {   /* header line */
        fclose(parts);
        return -1;
    }
    if (!fgets(line, sizeof(line), parts)) {   /* blank line   */
        fclose(parts);
        return -1;
    }

    int found = 0;

    while (fgets(line, sizeof(line), parts)) {
        char name[64] = {0};
        if (sscanf(line, "%*d %*d %*d %63s", name) != 1)
            continue;

        char dev[128];
        snprintf(dev, sizeof(dev), "/dev/%s", name);

        int fd = open(dev, O_RDONLY);
        if (fd < 0)
            continue;

        unsigned char hdr[512];
        if (pread(fd, hdr, sizeof(hdr), 512) != (ssize_t)sizeof(hdr)) {
            close(fd);
            continue;
        }

        if (memcmp(hdr, "EFI PART", 8) != 0) {
            close(fd);
            continue;
        }

        uint64_t entry_lba = le64_dec(hdr + 72);
        uint32_t entry_cnt = le32_dec(hdr + 80);
        uint32_t entry_sz  = le32_dec(hdr + 84);
        if (entry_cnt == 0 || entry_cnt > 256 || entry_sz < 128) {
            close(fd);
            continue;
        }

        size_t table_bytes = (size_t)entry_cnt * entry_sz;
        unsigned char *table = malloc(table_bytes);
        if (!table) {
            close(fd);
            continue;
        }

        if (pread(fd, table, table_bytes, (off_t)entry_lba * 512)
                == (ssize_t)table_bytes) {
            for (uint32_t i = 0; i < entry_cnt; i++) {
                const unsigned char *e = table + (size_t)i * entry_sz;

                /* Skip entries with an all-zero type GUID (unused). */
                int empty = 1;
                for (int b = 0; b < 16; b++) {
                    if (e[b] != 0) {
                        empty = 0;
                        break;
                    }
                }
                if (empty)
                    continue;

                /* Partition name: 36 UTF-16LE code units at offset 56.
                 * Stop at NUL or any non-ASCII code unit (labels we match
                 * are plain ASCII). */
                char pname[37];
                int n = 0;
                for (int u = 0; u < 36 && n < 36; u++) {
                    uint16_t cu = le16_dec(e + 56 + u * 2);
                    if (cu == 0 || cu > 0x7F)
                        break;
                    pname[n++] = (char)cu;
                }
                pname[n] = '\0';

                if (strcmp(pname, label) == 0) {
                    const char *sep =
                        (strncmp(name, "nvme", 4) == 0 ||
                         strncmp(name, "mmcblk", 6) == 0) ? "p" : "";
                    snprintf(device_path, path_size, "/dev/%s%s%u",
                             name, sep, i + 1);
                    found = 1;
                    break;
                }
            }
        }

        free(table);
        close(fd);

        if (found)
            break;
    }

    fclose(parts);
    return found ? 0 : -1;
}

/* ── Pivot into active A/B rootfs slot (Sprint 11.5) ─────────────── */

static int
playos_read_int_file(const char *path, int *out)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    int r = fscanf(f, "%d", out) == 1 ? 0 : -1;
    fclose(f);
    return r;
}

/* S13.7: scan removable whole disks for an ESP carrying the live-USB marker.
 * init's ESP discovery may mount the internal NVMe's ESP first, so a USB boot
 * on a machine with a previous install would otherwise never see the marker
 * on the USB ESP and would wrongly pivot into the NVMe slot. */
static int
playos_removable_esp_has_live_marker(void)
{
    DIR *dir = opendir("/sys/block");
    if (!dir)
        return 0;

    mkdir("/mnt/esp-check", 0755);
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(dir)) != NULL) {
        const char *disk = e->d_name;
        if (disk[0] == '.')
            continue;
        if (strncmp(disk, "loop", 4) == 0 || strncmp(disk, "ram", 3) == 0 ||
            strncmp(disk, "zram", 4) == 0 || strncmp(disk, "dm-", 3) == 0)
            continue;

        char rb[192];
        snprintf(rb, sizeof(rb), "/sys/block/%s/removable", disk);
        int removable = 0;
        if (playos_read_int_file(rb, &removable) != 0 || removable == 0)
            continue;

        char dpath[192];
        snprintf(dpath, sizeof(dpath), "/sys/block/%s", disk);
        DIR *pd = opendir(dpath);
        if (!pd)
            continue;
        struct dirent *pe;
        while (!found && (pe = readdir(pd)) != NULL) {
            const char *part = pe->d_name;
            if (strncmp(part, disk, strlen(disk)) != 0)
                continue; /* only partition entries */
            char dev[160];
            snprintf(dev, sizeof(dev), "/dev/%s", part);
            if (mount(dev, "/mnt/esp-check", "vfat", MS_RDONLY, NULL) != 0)
                continue;
            found = access("/mnt/esp-check/EFI/playos/live-usb", F_OK) == 0;
            umount("/mnt/esp-check");
        }
        closedir(pd);
    }
    closedir(dir);
    rmdir("/mnt/esp-check");
    return found;
}

/*
 * Sprint 11.5: hand control from the initramfs to the real read-only
 * rootfs slot. Installed internal disks carry the active slot as a raw
 * squashfs filesystem (playos-a / playos-b). The initramfs is the
 * early-boot shim: it does ESP/boot.json accounting, then pivots into the
 * selected squashfs and execs its /init.
 *
 * Uses the busybox switch_root idiom (mount --move + chroot) rather than
 * pivot_root: the new root is read-only, so we cannot create pivot_root's
 * put_old directory inside it.
 *
 * Returns 0 on success, which never actually returns because the new init
 * is exec'd. Returns non-zero when the pivot must be skipped (live USB, no
 * raw squashfs slot, or a mount failure), so the caller keeps booting from
 * the initramfs exactly as before.
 */
int playos_pivot_to_active_slot(struct playos_init_state *s)
{
    /* Installer boots always run from the removable boot medium's
     * initramfs. A previously-installed internal slot (squashfs playos-a/b)
     * may already be present on the target NVMe; pivoting into it would
     * hide /usr/bin/playos-installer (which lives only in the installer
     * initramfs) and break the installer with exec ENOENT. */
    if (s->install_mode) {
        playos_log_write(s, "init",
                         "installer mode: staying in initramfs "
                         "(skip pivot to installed slot)");
        return 1;
    }

    /* Live-USB marker (S13.7): the image gen scripts stamp the removable
     * ESP with EFI/playos/live-usb. A machine with a previously-installed
     * internal PlayOS also carries a raw-squashfs playos-a slot; without
     * this check a USB boot would pivot into that NVMe slot and silently
     * boot the installed system instead of the live session. */
    if (s->efi_mounted && access("/EFI/playos/live-usb", F_OK) == 0) {
        playos_log_write(s, "init",
                         "live USB marker present — staying in initramfs "
                         "(skip pivot to installed slot)");
        return 1;
    }

    /* The internal NVMe ESP may have been mounted instead of the USB's.
     * Scan removable disks' ESPs too so a USB boot is still recognized. */
    if (playos_removable_esp_has_live_marker()) {
        playos_log_write(s, "init",
                         "live USB marker found on removable ESP — staying "
                         "in initramfs (skip pivot to installed slot)");
        return 1;
    }

    /* If / is already squashfs, we are the exec'd init inside the real
     * root and there is nothing left to pivot. */
    FILE *mounts = fopen("/proc/mounts", "r");
    if (mounts) {
        char line[512];
        while (fgets(line, sizeof(line), mounts)) {
            char src[128], dst[256], fstype[64];
            if (sscanf(line, "%127s %255s %63s", src, dst, fstype) == 3 &&
                strcmp(dst, "/") == 0) {
                if (strcmp(fstype, "squashfs") == 0) {
                    fclose(mounts);
                    return 1;
                }
                break;
            }
        }
        fclose(mounts);
    }

    /* Select the active slot from boot.json (safe default: slot a). */
    struct boot_slot_state bs;
    boot_slot_read(PLAYOS_BOOT_JSON_PATH, &bs);
    const char *label = (bs.active_slot == 'b') ? "playos-b" : "playos-a";

    char dev[128] = {0};
    if (playos_find_partition_by_label(label, dev, sizeof(dev)) != 0) {
        playos_log_write(s, "init",
                         "no %s partition found — staying in initramfs",
                         label);
        return 1;
    }

    mkdir("/mnt/newroot", 0755);

    /* Raw slot: squashfs, read-only. Do NOT fall back to ext2/auto — the
     * live USB's playos-a is an ext2 carrier and must not be pivoted into. */
    if (mount(dev, "/mnt/newroot", "squashfs", MS_RDONLY, NULL) != 0) {
        playos_log_write(s, "init",
                         "active slot %s is not a raw squashfs (%s) — "
                         "staying in initramfs", label, strerror(errno));
        rmdir("/mnt/newroot");
        return 1;
    }

    playos_log_write(s, "init",
                     "pivoting to active slot %c (%s, read-only squashfs)",
                     bs.active_slot, dev);

    /* Release the ESP before switching roots so the exec'd second init can
     * re-mount it cleanly. Without this the device stays busy across the
     * pivot and the second init's mount fails with EBUSY, which both spams
     * warnings and leaves mark-good without a mounted ESP. */
    if (s->efi_mounted)
        umount("/EFI");

    if (chdir("/mnt/newroot") != 0 ||
        mount(".", "/", NULL, MS_MOVE, NULL) != 0 ||
        chroot(".") != 0 ||
        chdir("/") != 0) {
        playos_log_write(s, "init",
                         "pivot switch_root failed: %s — staying in initramfs",
                         strerror(errno));
        return 1;
    }

    char *const argv[] = { "/init", NULL };
    execve("/init", argv, environ);

    /* execve failed — unreachable in a healthy boot. */
    playos_log_write(s, "init", "exec /init failed: %s", strerror(errno));
    return 1;
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
    char device_path[256] = {0};

    if (find_data_partition(device_path, sizeof(device_path),
                            state->install_mode) != 0) {
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

    dprintf(STDERR_FILENO,
            "playos-init: /data mounted from %s (install_mode=%d)\n",
            device_path, state->install_mode);

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

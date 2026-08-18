/*
 * playos-init/mount.h — Filesystem mounting subsystem
 */
#ifndef PLAYOS_MOUNT_H
#define PLAYOS_MOUNT_H

#include "init.h"

/* Mount virtual filesystems: /dev, /proc, /sys, /run */
int playos_mount_virtual(void);

/* Discover and mount the data partition at /data */
int playos_mount_data(struct playos_init_state *state);

/* Create first-boot directories under /data if missing */
int playos_data_create_dirs(void);

/* Update boot stage marker file */
int playos_boot_stage_write(enum playos_boot_stage stage);

/* Kernel command line inspection (Sprint 10 installer trigger).
 * Returns 1 if `flag` appears as a whitespace-delimited token in
 * /proc/cmdline, 0 otherwise. */
int playos_cmdline_has_flag(const char *flag);

/* True when booted with playos.mode=install (installer USB). */
int playos_install_mode_requested(void);

/* Locate a GPT partition by its UTF-16LE name label (e.g. "ESP",
 * "playos-a"). Fills device_path with the /dev/ node and returns 0, or
 * -1 if no such partition is found. */
int playos_find_partition_by_label(const char *label, char *device_path,
                                   size_t path_size);

#endif /* PLAYOS_MOUNT_H */

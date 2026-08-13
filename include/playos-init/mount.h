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

#endif /* PLAYOS_MOUNT_H */

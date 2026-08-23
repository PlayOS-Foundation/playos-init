/*
 * playos-init/boot_slot.h — A/B boot slot accounting (Sprint 11)
 *
 * The bootloader records which rootfs slot is active plus per-slot health
 * in a small JSON file on the EFI System Partition. init mounts the ESP at
 * /EFI, advances the active slot's boot counter on every boot, and rolls
 * back to the other slot when a non-"good" slot fails too many times.
 */
#ifndef PLAYOS_BOOT_SLOT_H
#define PLAYOS_BOOT_SLOT_H

#include "init.h"

/* boot.json lives on the EFI System Partition, which init mounts at /EFI. */
#define PLAYOS_BOOT_JSON_PATH "/EFI/playos/boot.json"

/* Per-slot accounting recorded in boot.json. */
struct boot_slot_info {
    char version[64];
    int  boot_count;
    char health[16];
};

/* Full boot.json contents. */
struct boot_slot_state {
    int  v;                    /* schema version, currently 1 */
    char active_slot;          /* 'a' or 'b' */
    struct boot_slot_info slot_a;
    struct boot_slot_info slot_b;
};

/*
 * Read boot.json from `path` into `out`.
 *
 * Returns 0 on a valid read. Returns -1 when the file is missing, corrupt,
 * or unreadable — but `out` is still initialized to a safe default state
 * (v=1, active_slot='a', slot_a healthy, slot_b empty) so callers can keep
 * going without branching on every field.
 */
int boot_slot_read(const char *path, struct boot_slot_state *out);

/* Atomically write `st` to `path` (write tmp + fsync + rename). 0 / -1. */
int boot_slot_write(const char *path, const struct boot_slot_state *st);

/*
 * Increment the active slot's boot counter and persist it.
 *
 * Returns 1 when a rollback is required (boot_count >= 3 while health is
 * not "good"), otherwise 0. On a read/write error this logs and returns 0
 * so a transient ESP problem never hard-fails boot.
 */
int boot_slot_increment(const char *path, struct boot_slot_state *st);

/* Mark the active slot healthy and clear its boot counter. 0 / -1. */
int boot_slot_mark_good(const char *path, struct boot_slot_state *st);

/*
 * Roll back to the inactive slot: mark the previous active slot "bad",
 * switch `active_slot`, and put the new active slot into "pending" health
 * with a zeroed boot counter (its version is left untouched). 0 / -1.
 */
int boot_slot_rollback(const char *path, struct boot_slot_state *st);

/*
 * One-shot "this boot is healthy" gate. Called once the shell registers
 * and again as a ~60s fallback from the main loop; the state flag makes it
 * idempotent. No-op unless the ESP is mounted.
 */
void playos_boot_mark_good_once(struct playos_init_state *s);

/*
 * Return 1 when / is mounted read-only squashfs (i.e. init has already
 * pivoted into the active slot and this is the exec'd second init). Used to
 * skip boot-count accounting on the second init (the first init already
 * counted it) and to gate mark-good against fallback initramfs boots.
 */
int playos_root_is_squashfs(void);

#endif /* PLAYOS_BOOT_SLOT_H */

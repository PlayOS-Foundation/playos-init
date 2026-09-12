/*
 * boot_media.h — which medium did the firmware boot us from? (S14-T10)
 */
#ifndef PLAYOS_INIT_BOOT_MEDIA_H
#define PLAYOS_INIT_BOOT_MEDIA_H

#include <stddef.h>

/*
 * Pure parser for an EFI `Boot####` variable payload (attributes + device-path
 * length + UTF-16 description + device path).
 *
 * Returns 1 when the device path contains a USB messaging node, 0 when it does
 * not (NVMe/SATA/etc.), and -1 when the buffer is not a usable boot entry.
 * Exposed for host unit tests.
 */
int playos_boot_var_is_usb(const unsigned char *var, size_t len);

/*
 * Ask the firmware what it booted: reads BootCurrent and the matching Boot####
 * variable from efivarfs. Returns 1 for a USB boot, 0 for a fixed disk, and -1
 * when the answer is unavailable (no efivars, non-UEFI boot, malformed data).
 */
int playos_booted_from_usb(void);

#endif /* PLAYOS_INIT_BOOT_MEDIA_H */

/*
 * boot_media.c — which medium did the firmware boot us from? (S14-T10)
 *
 * Why this exists: after an install, the live USB and the internal disk carry
 * the *same* partition names and labels (ESP, playos-a, playos-b, playos-data),
 * so anything that resolves a partition by name is ambiguous. On the Ally a USB
 * boot resolved "ESP" to the internal NVMe's ESP (first match), saw no
 * EFI/playos/live-usb marker there, and pivoted into the *installed* slot — so
 * "boot from USB" silently booted the installed system and the installer never
 * appeared.
 *
 * The firmware's own record is unambiguous: `BootCurrent` names the `Boot####`
 * entry it used, and that entry's device path says whether the boot device is
 * USB (messaging node subtype 5 = USB, 0x10 = USB class) or a fixed disk
 * (NVMe 0x17, SATA 0x12, ...).
 */
#define _DEFAULT_SOURCE 1
#include "playos-init/boot_media.h"

#include <stdio.h>
#include <string.h>

#define EFI_GLOBAL_VAR_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define EFIVARS_DIR         "/sys/firmware/efi/efivars"
#define EFI_VAR_MAX         2048

static unsigned int
le16(const unsigned char *p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

int
playos_boot_var_is_usb(const unsigned char *var, size_t len)
{
    if (!var || len < 8)
        return -1;

    /* attributes(4) + file-path-list length(2) + description + device path */
    size_t fplen = le16(var + 4);
    size_t desc = 6;

    /* Description is UTF-16LE, NUL-terminated. */
    while (desc + 2 <= len) {
        if (var[desc] == 0 && var[desc + 1] == 0) {
            desc += 2;
            break;
        }
        desc += 2;
    }
    if (desc + 4 > len)
        return -1;

    size_t path = desc;
    size_t end = path + fplen;
    if (fplen == 0 || end > len)
        end = len;                      /* tolerate a bogus length */

    int seen_node = 0;
    int is_usb = 0;

    while (path + 4 <= end) {
        unsigned char type = var[path];
        unsigned char subtype = var[path + 1];
        size_t nlen = le16(var + path + 2);

        if (type == 0x7F)               /* end of device path */
            break;
        if (nlen < 4)
            break;

        seen_node = 1;
        if (type == 0x03 &&             /* messaging device path */
            (subtype == 0x05 ||         /* USB                       */
             subtype == 0x10))          /* USB class                 */
            is_usb = 1;

        path += nlen;
    }

    if (!seen_node)
        return -1;
    return is_usb;
}

/* Read an efivar verbatim (attribute prefix included). Returns bytes read. */
static long
efi_var_read(const char *name, unsigned char *out, size_t outsz)
{
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", EFIVARS_DIR, name);

    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;

    size_t n = fread(out, 1, outsz, f);
    fclose(f);
    return (long)n;
}

int
playos_booted_from_usb(void)
{
    static unsigned char current[16];
    static unsigned char entry[EFI_VAR_MAX];
    char var_name[80];

    /* BootCurrent: 4 attribute bytes + a little-endian u16 entry index. */
    snprintf(var_name, sizeof(var_name), "BootCurrent-%s", EFI_GLOBAL_VAR_GUID);
    long n = efi_var_read(var_name, current, sizeof(current));
    if (n < 6)
        return -1;

    unsigned int index = le16(current + 4);

    snprintf(var_name, sizeof(var_name), "Boot%04X-%s", index,
             EFI_GLOBAL_VAR_GUID);
    n = efi_var_read(var_name, entry, sizeof(entry));
    if (n < 8)
        return -1;

    return playos_boot_var_is_usb(entry, (size_t)n);
}

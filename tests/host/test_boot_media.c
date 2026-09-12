/*
 * test_boot_media.c — host tests for the firmware boot-medium parser (S14-T10)
 *
 * The parser reads an EFI Boot#### variable: 4 attribute bytes, a u16
 * device-path length, a UTF-16LE NUL-terminated description, then the device
 * path as a list of length-prefixed nodes. We only care whether a USB node is
 * present, because that is what makes the difference between "boot the live
 * medium" and "pivot into the installed slot".
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "playos-init/boot_media.h"

/* Build a Boot#### payload: attributes + length + UTF-16 description + path. */
static size_t
build_var(unsigned char *out, const char *desc, const unsigned char *nodes,
          size_t nodes_len)
{
    size_t n = 0;
    memset(out, 0, 4);
    n = 4;

    size_t desc_len = strlen(desc);
    out[n++] = (unsigned char)(nodes_len & 0xff);
    out[n++] = (unsigned char)((nodes_len >> 8) & 0xff);

    for (size_t i = 0; i < desc_len; i++) {
        out[n++] = (unsigned char)desc[i];
        out[n++] = 0;
    }
    out[n++] = 0;
    out[n++] = 0;

    memcpy(out + n, nodes, nodes_len);
    return n + nodes_len;
}

static void
test_usb_boot(void)
{
    /* USB messaging node (type 3, subtype 5) then end-of-path. */
    static const unsigned char nodes[] = {
        0x03, 0x05, 0x06, 0x00, 0x01, 0x01,   /* USB(1,1)          */
        0x7F, 0xFF, 0x04, 0x00                /* end of device path */
    };
    unsigned char var[256];
    size_t n = build_var(var, "UEFI:  USB, Partition 1", nodes, sizeof(nodes));
    assert(playos_boot_var_is_usb(var, n) == 1);
}

static void
test_usb_class_node(void)
{
    /* USB class node (type 3, subtype 0x10) also means a USB device. */
    static const unsigned char nodes[] = {
        0x03, 0x10, 0x06, 0x00, 0x00, 0x00,
        0x7F, 0xFF, 0x04, 0x00
    };
    unsigned char var[256];
    size_t n = build_var(var, "UEFI: USB", nodes, sizeof(nodes));
    assert(playos_boot_var_is_usb(var, n) == 1);
}

static void
test_fixed_disk_boot(void)
{
    /* NVMe namespace node (type 3, subtype 0x17) = internal disk. */
    static const unsigned char nodes[] = {
        0x03, 0x17, 0x10, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x04, 0x01, 0x2A, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00,
        0x7F, 0xFF, 0x04, 0x00
    };
    unsigned char var[256];
    size_t n = build_var(var, "PlayOS", nodes, sizeof(nodes));
    assert(playos_boot_var_is_usb(var, n) == 0);
}

static void
test_malformed(void)
{
    static const unsigned char nodes[] = { 0x7F, 0xFF, 0x04, 0x00 };
    unsigned char var[256];
    size_t n = build_var(var, "empty", nodes, sizeof(nodes));

    assert(playos_boot_var_is_usb(NULL, 100) == -1);
    assert(playos_boot_var_is_usb(var, 4) == -1);        /* too short */
    assert(playos_boot_var_is_usb(var, n) == -1);        /* no nodes at all */

    /* Zero-length node must not loop forever. */
    static const unsigned char bad[] = { 0x03, 0x05, 0x00, 0x00 };
    n = build_var(var, "bad", bad, sizeof(bad));
    assert(playos_boot_var_is_usb(var, n) == -1);
}

int
main(void)
{
    test_usb_boot();
    test_usb_class_node();
    test_fixed_disk_boot();
    test_malformed();
    printf("boot_media: all tests passed\n");
    return 0;
}

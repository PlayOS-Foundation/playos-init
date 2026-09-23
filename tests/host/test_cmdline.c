/*
 * test_cmdline.c — host tests for kernel command-line parsing (S15-T7)
 *
 * The parser is pure so it can be driven directly; the runtime wrapper that
 * reads /proc/cmdline lives in mount.c and is exercised on device/QEMU.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "playos-init/cmdline.h"

static void test_absent(void)
{
    char out[128];

    assert(playos_cmdline_autostart("", out, sizeof(out)) == 0);
    assert(playos_cmdline_autostart("console=ttyS0 quiet", out, sizeof(out)) == 0);

    /* The key includes '=', so a bare flag or a longer key is not a match. */
    assert(playos_cmdline_autostart("playos.autostart", out, sizeof(out)) == 0);
    assert(playos_cmdline_autostart("playos.autostartx=foo", out, sizeof(out)) == 0);

    /* Embedded in another token's value — not a token boundary. */
    assert(playos_cmdline_autostart("foo=playos.autostart=bar", out, sizeof(out)) == 0);
}

static void test_present(void)
{
    char out[128];
    int n = playos_cmdline_autostart(
        "console=ttyS0 playos.autostart=com.playos.sample-bunnymark quiet",
        out, sizeof(out));

    assert(n == (int)strlen("com.playos.sample-bunnymark"));
    assert(strcmp(out, "com.playos.sample-bunnymark") == 0);
}

static void test_boundaries(void)
{
    char out[128];

    /* First token in the line. */
    assert(playos_cmdline_autostart("playos.autostart=alpha", out, sizeof(out)) > 0);
    assert(strcmp(out, "alpha") == 0);

    /* /proc/cmdline normally ends in a newline; the value must stop there. */
    assert(playos_cmdline_autostart("a=1 playos.autostart=beta\n", out, sizeof(out)) > 0);
    assert(strcmp(out, "beta") == 0);

    /* Leading tab. */
    assert(playos_cmdline_autostart("\tplayos.autostart=gamma", out, sizeof(out)) > 0);
    assert(strcmp(out, "gamma") == 0);
}

static void test_malformed(void)
{
    char out[128];

    /* Present but empty. */
    assert(playos_cmdline_autostart("playos.autostart=", out, sizeof(out)) == -1);
    assert(playos_cmdline_autostart("x playos.autostart= y", out, sizeof(out)) == -1);

    /* Present but does not fit (no silent truncation). */
    assert(playos_cmdline_autostart("playos.autostart=abcdef", out, 4) == -1);
}

int main(void)
{
    test_absent();
    test_present();
    test_boundaries();
    test_malformed();
    printf("test_cmdline: all assertions passed\n");
    return 0;
}

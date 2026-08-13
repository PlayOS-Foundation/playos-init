/*
 * tests/host/test_init_state.c — Unit test for state initialization
 *
 * Compile (standalone):
 *   gcc -std=c99 -I../../include -o test_init_state test_init_state.c ../../src/main.c
 *   gcc -std=c99 -I../../include -c ../../src/main.c (to pull in playos_init_state_init)
 *
 * Or run via CMake test target.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "playos-init/init.h"

int main(void)
{
    struct playos_init_state s;
    memset(&s, 0xFF, sizeof(s)); /* Fill with garbage to ensure init resets */

    playos_init_state_init(&s);

    /* Verify initial state */
    assert(s.boot_stage == BOOT_STAGE_START);
    assert(s.boot_time > 0);
    assert(s.compositor_pid == 0);
    assert(s.compositor_state == COMPOSITOR_NOT_STARTED);
    assert(s.game_pid == 0);
    assert(s.game_state == GAME_NONE);
    assert(s.control_sock_fd == -1);
    assert(s.compositor_sock_fd == -1);
    assert(s.log_fd == -1);
    assert(s.recovery_mode == 0);
    assert(s.game_id[0] == '\0');
    assert(s.launch_token[0] == '\0');
    assert(s.compositor_restarts.count == 0);
    assert(s.compositor_restarts.window_start > 0);

    printf("PASS: playos_init_state_init sets correct defaults\n");

    /* Verify structure sizes are reasonable */
    assert(sizeof(struct playos_init_state) < 131072); /* < 128KB (includes 64KB ring buffer) */

    printf("PASS: state struct size check (%zu bytes)\n",
           sizeof(struct playos_init_state));

    printf("\nAll state init tests passed.\n");
    return 0;
}

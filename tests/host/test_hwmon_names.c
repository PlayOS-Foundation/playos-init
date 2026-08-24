/*
 * test_hwmon_names.c — host test for vendor-agnostic hwmon classification
 * (Sprint 13 T5)
 */
#include <assert.h>
#include <stdio.h>

#include "playos-init/hwmon_names.h"

static void
test_gpu_names(void)
{
    assert(playos__hwmon_name_is_gpu("amdgpu"));
    assert(playos__hwmon_name_is_gpu("i915"));
    assert(playos__hwmon_name_is_gpu("xe"));
    assert(!playos__hwmon_name_is_gpu("k10temp"));
    assert(!playos__hwmon_name_is_gpu("coretemp"));
    assert(!playos__hwmon_name_is_gpu("acpitz"));
    assert(!playos__hwmon_name_is_gpu(""));
    assert(!playos__hwmon_name_is_gpu(NULL));
}

static void
test_cpu_names(void)
{
    assert(playos__hwmon_name_is_cpu("k10temp"));
    assert(playos__hwmon_name_is_cpu("coretemp"));
    assert(!playos__hwmon_name_is_cpu("amdgpu"));
    assert(!playos__hwmon_name_is_cpu("i915"));
    assert(!playos__hwmon_name_is_cpu("xe"));
    assert(!playos__hwmon_name_is_cpu(NULL));
}

int
main(void)
{
    test_gpu_names();
    test_cpu_names();
    printf("PASS: hwmon name matchers (AMD + Intel)\n");
    return 0;
}

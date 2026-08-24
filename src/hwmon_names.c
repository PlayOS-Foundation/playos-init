/*
 * hwmon_names.c — vendor-agnostic hwmon sensor classification (Sprint 13 T5)
 */
#include <string.h>

#include "playos-init/hwmon_names.h"

int
playos__hwmon_name_is_gpu(const char *name)
{
    if (!name)
        return 0;
    return strcmp(name, "amdgpu") == 0 ||
           strcmp(name, "i915") == 0 ||
           strcmp(name, "xe") == 0;
}

int
playos__hwmon_name_is_cpu(const char *name)
{
    if (!name)
        return 0;
    return strcmp(name, "k10temp") == 0 ||
           strcmp(name, "coretemp") == 0;
}

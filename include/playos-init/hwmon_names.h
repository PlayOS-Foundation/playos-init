/*
 * hwmon_names.h — vendor-agnostic hwmon sensor classification
 *
 * Sprint 13 T5: hwmon `name` strings are vendor-specific. These matchers keep
 * the vendor knowledge in one place so the thermal monitor works on both AMD
 * (amdgpu/k10temp) and Intel (i915/xe/coretemp).
 */
#ifndef PLAYOS_INIT_HWMON_NAMES_H
#define PLAYOS_INIT_HWMON_NAMES_H

/* GPU temperature hwmon names: amdgpu (AMD), i915/xe (Intel). */
int playos__hwmon_name_is_gpu(const char *name);

/* CPU temperature hwmon names: k10temp (AMD), coretemp (Intel). */
int playos__hwmon_name_is_cpu(const char *name);

#endif /* PLAYOS_INIT_HWMON_NAMES_H */

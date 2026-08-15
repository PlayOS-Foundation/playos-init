/*
 * playos-init/thermal.h — Thermal monitoring and EPP profile control
 *
 * Sprint 9: 1 Hz thermal monitor, performance-profile (EPP) writer,
 * thermal state machine, and graceful over-temperature handling.
 */
#ifndef PLAYOS_THERMAL_H
#define PLAYOS_THERMAL_H

#include "init.h"

/* ── Performance profiles ─────────────────────────────────────────── */
/* Values also map onto PlayOSPerfProfile in playos-platform-api. */
#define PLAYOS_PERF_PROFILE_BALANCED    0
#define PLAYOS_PERF_PROFILE_POWER_SAVE  1
#define PLAYOS_PERF_PROFILE_PERFORMANCE 2

/* ── Thermal states ───────────────────────────────────────────────── */
#define PLAYOS_THERMAL_STATE_NORMAL    0
#define PLAYOS_THERMAL_STATE_WARM      1
#define PLAYOS_THERMAL_STATE_HOT       2
#define PLAYOS_THERMAL_STATE_CRITICAL  3

/* ── Return codes for playos_thermal_request_profile() ───────────── */
#define PLAYOS_THERMAL_REQ_OK         0   /* accepted and applied */
#define PLAYOS_THERMAL_REQ_INVALID   -1   /* profile out of range */
#define PLAYOS_THERMAL_REQ_DENIED    -2   /* thermal policy denied request */
#define PLAYOS_THERMAL_REQ_EPP_FAIL  -3   /* EPP sysfs write failed */

/**
 * Load thermal thresholds (thermal.json) and sync the current EPP profile
 * into the kernel. Call once during boot, after /sys and /data are ready.
 */
void playos_thermal_init(struct playos_init_state *s);

/**
 * Run one thermal monitor iteration. Intended to be called from the PID 1
 * supervision loop at 1 Hz. Reads temperatures, drives the thermal state
 * machine, writes EPP as needed, emits events, and triggers an orderly
 * shutdown if CRITICAL persists too long.
 */
void playos_thermal_tick(struct playos_init_state *s);

/**
 * Request a performance profile change.
 *
 * Validates the request against the current thermal state (PERFORMANCE is
 * denied while HOT or CRITICAL), writes EPP to every online CPU, updates
 * s->active_profile, and emits PerfProfileChanged to the shell listener.
 *
 * @return PLAYOS_THERMAL_REQ_OK if accepted, or a negative
 *         PLAYOS_THERMAL_REQ_* code describing the rejection reason.
 */
int playos_thermal_request_profile(struct playos_init_state *s, int profile);

#endif /* PLAYOS_THERMAL_H */

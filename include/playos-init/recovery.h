#ifndef PLAYOS_RECOVERY_H
#define PLAYOS_RECOVERY_H

/* True when the user is holding a recovery trigger (START+SELECT / volume up /
 * volume down) during early boot (S14-T6 recovery entry point). START+SELECT
 * confirms after 2s, the volume triggers after 5s. Returns 0 immediately when
 * no trigger is already held, so normal boots are not delayed. */
int playos_recovery_button_held(void);

/* Watch for a recovery trigger for `listen_ms` (polls every 100ms). Returns 1
 * as soon as a trigger has been held continuously for its hold duration.
 * Blocking — kept for callers that explicitly want a bounded listen window.
 * The normal boot path does NOT use this: it polls the non-blocking
 * playos_recovery_button_held() from the supervision loop so a boot is never
 * delayed (S14 review). */
int playos_recovery_button_watch(int listen_ms);

#endif /* PLAYOS_RECOVERY_H */

#ifndef PLAYOS_RECOVERY_H
#define PLAYOS_RECOVERY_H

/* True when the user held a recovery trigger (START+SELECT / volume up /
 * volume down) for 5 seconds during early boot (S14-T6 recovery entry point).
 * Returns 0 immediately when no trigger is already held, so normal boots are
 * not delayed. */
int playos_recovery_button_held(void);

/* Watch for a recovery trigger for `listen_ms` (polls every 100ms). Returns 1
 * as soon as a trigger has been held continuously for RECOVERY_HOLD_MS.
 * Intended for a late-boot check where input devices are already settled and
 * the user may start holding slightly after the boot begins. */
int playos_recovery_button_watch(int listen_ms);

#endif /* PLAYOS_RECOVERY_H */

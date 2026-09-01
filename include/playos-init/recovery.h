#ifndef PLAYOS_RECOVERY_H
#define PLAYOS_RECOVERY_H

/* True when the user held Volume Down for 5 seconds during early boot
 * (S14-T6 recovery entry point). Returns 0 immediately when the button is
 * not already held, so normal boots are not delayed. */
int playos_recovery_button_held(void);

#endif /* PLAYOS_RECOVERY_H */

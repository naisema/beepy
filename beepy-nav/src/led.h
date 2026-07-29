/* beepy-nav/src/led.h -- the keyboard LED, DESIGN.md 7.5.
 *
 * "At 500 m, 200 m and 50 m from a cue, flash the keyboard LED." It is the
 * only non-visual alert this hardware has; there is no buzzer.
 *
 * /sys/firmware/beepy/led{,_red,_green,_blue} are root-write-only as shipped
 * (DESIGN.md 3), so this module's contract is that it NEVER fails loudly: an
 * unwritable LED leaves the navigator visual-only, which is what it always was
 * before this existed. install/99-beepy-led.rules is the fix.
 *
 * Blinking is a state machine advanced by led_tick(), not a sleep: the frame
 * clock of 6.3 runs at 8 Hz and must not be blocked for a tenth of a second
 * to wink a light. Time is the caller's clock in seconds -- the ride clock --
 * so a replay blinks deterministically and a test can see it.
 *
 * Portable C: plain stdio against paths that simply do not exist off the
 * device, which is the same "not writable" case.
 */
#ifndef BEEPY_NAV_LED_H
#define BEEPY_NAV_LED_H

/* Probe once. `enabled` is the config's led_alerts: 0 disables the LED
 * without pretending it is missing. Returns 1 when alerts will be felt as
 * well as seen. */
int led_init(int enabled);

int led_available(void);

/* Queue `blinks` on/off pairs starting at time `t`. A no-op when the LED is
 * unavailable -- callers do not branch on it. */
void led_pulse(int blinks, double t);

/* Advance the blink state machine to time `t`. Cheap and idempotent; call it
 * once per frame. */
void led_tick(double t);

/* Dark, whatever the state machine thought. Every exit path calls this. */
void led_off(void);

#endif /* BEEPY_NAV_LED_H */

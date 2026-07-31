/* libbeepyfb/input.h -- evdev keyboards, bypassing the (paused) fbterm.
 *
 * The library delivers raw (keycode, value) events; mapping keycodes to app
 * actions is each application's business (gps-monitor keeps its keymap in
 * main.c, beepy-nav will carry a much larger one).
 *
 * Device-only: input.c needs linux/input.h and does not compile on the Mac.
 */
#ifndef BEEPYFB_INPUT_H
#define BEEPYFB_INPUT_H

#define MAX_EVDEV 4

/* Scan /dev/input/event0..31 for keyboards (KEY_Q + KEY_TAB capability) and
 * open up to MAX_EVDEV of them, optionally grabbing (EVIOCGRAB). */
void evdev_open(int grab);
void evdev_close(void);

/* The open fds, for the caller's poll() set. */
int evdev_count(void);
int evdev_fd(int i);

/* How many of the keyboards evdev_open(1) opened REFUSED the grab, because
 * something else holds them exclusively -- in practice another beepy-nav. Such a
 * reader receives no events at all while the holder runs, so a diagnostic must
 * say so rather than appear to work. The ride path ignores this: there, a lost
 * grab only means keys also reach the console. Zero after evdev_close(). */
int evdev_grab_failed(void);

/* Drain the next EV_KEY event from fd into code/value (raw keycode and
 * 0=release 1=press 2=repeat). Skips non-key events. Returns 1 when a key
 * event was read, 0 when fd has no complete event left. */
int evdev_next_key(int fd, int *code, int *value);

#endif /* BEEPYFB_INPUT_H */

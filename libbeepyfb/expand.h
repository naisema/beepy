/* libbeepyfb/expand.h -- 1bpp canvas -> XRGB8888, the panel's byte format.
 *
 * Split out of fbdev.c so it can be tested in the Mac lane: fbdev.c needs
 * linux/fb.h and does not compile there, while this is pure arithmetic with no
 * framebuffer in it. fbdev.h includes this, so nothing that used to get
 * expand() from fbdev.h has to change.
 *
 * There are three places in this tree that turn a 1bpp canvas into the
 * device's XRGB words -- expand(), canvas_dump() (dump.c) and beepy-nav's
 * fbplay.c -- and until libbeepyfb/tests/test_expand.c none of them was
 * compared against any other. Every golden in goldens/ goes through
 * canvas_dump(); the panel goes through expand(). They must agree.
 */
#ifndef BEEPYFB_EXPAND_H
#define BEEPYFB_EXPAND_H

#include <stddef.h>

#include "canvas.h"

/* Expand rows 0..h-1, w pixels wide, into frame at the given stride.
 *
 * The panel's two words match the device's own shutdownimage.fb:
 * ink (a set bit) = 00 00 00 ff, paper = ff ff ff ff.
 *
 * Unlike canvas_dump(), this does NOT clip to c->w/c->h and does not pad a
 * short canvas -- the caller is the panel, whose canvas is always full size.
 * The two agree only for a full-size canvas, which is what test_expand pins.
 */
void expand(const canvas_t *c, unsigned char *frame, size_t line_len, int h, int w);

#endif /* BEEPYFB_EXPAND_H */

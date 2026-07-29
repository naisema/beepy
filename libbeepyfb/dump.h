/* libbeepyfb/dump.h -- write a canvas as the panel's raw frame file.
 *
 * The same 384000-byte XRGB image fb_present() puts on /dev/fb1, but via
 * stdio and with no linux/fb.h in sight, so the design gate and the goldens
 * work in the host lane too. fbdev.c stays device-only.
 */
#ifndef BEEPYFB_DUMP_H
#define BEEPYFB_DUMP_H

#include "canvas.h"

/* 0 on success, -1 with errno set. */
int canvas_dump(const canvas_t *c, const char *path);

#endif /* BEEPYFB_DUMP_H */

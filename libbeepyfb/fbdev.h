/* libbeepyfb/fbdev.h -- full-frame writes to /dev/fb1, fbterm handover.
 *
 * Device-only: fbdev.c needs linux/fb.h and does not compile on the Mac.
 */
#ifndef BEEPYFB_FBDEV_H
#define BEEPYFB_FBDEV_H

#include <stddef.h>
#include <sys/types.h>

#include "canvas.h"
/* expand() lives in expand.c now (it is portable; this file is not), but is
 * still reached through this header so callers did not have to change. */
#include "expand.h"

typedef struct {
    int fd;
    int w, h;
    size_t line_len, frame_sz;
    unsigned char *frame;
    pid_t paused[8];
    int npaused;
} fb_t;

int fb_open(fb_t *f, const char *path);
void fb_take(fb_t *f);
void fb_release(fb_t *f);
int fb_present(fb_t *f, const canvas_t *c);
int fb_dump(const canvas_t *c, const char *path);
int write_all(int fd, const void *buf, size_t n);

#endif /* BEEPYFB_FBDEV_H */

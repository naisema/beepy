/* libbeepyfb/fbdev.c -- full-frame writes to /dev/fb1, fbterm handover.
 *
 * Split out of gps-monitor.c (M1). The things worth knowing here:
 *
 *  - Frames are written whole (384000 bytes) rather than mmapped, matching the
 *    device's own shutdownimage mechanism, which is known to work here.
 *  - The panel console is fbterm (userspace), not kernel fbcon, so it is
 *    SIGSTOPped while we own the display and SIGCONTed on every exit path.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#include "fbdev.h"

int fb_open(fb_t *f, const char *path)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo fx;

    memset(f, 0, sizeof *f);
    f->fd = open(path, O_RDWR);
    if (f->fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(f->fd, FBIOGET_FSCREENINFO, &fx) < 0) {
        fprintf(stderr, "%s: FBIOGET_*SCREENINFO: %s\n", path, strerror(errno));
        close(f->fd);
        return -1;
    }
    if (v.bits_per_pixel != 32) {
        fprintf(stderr, "%s: %u bpp, this build expects 32\n", path, v.bits_per_pixel);
        close(f->fd);
        return -1;
    }
    f->w = (int)v.xres;
    f->h = (int)v.yres;
    f->line_len = fx.line_length;
    f->frame_sz = f->line_len * (size_t)f->h;
    f->frame = malloc(f->frame_sz);
    if (!f->frame) {
        close(f->fd);
        return -1;
    }
    return 0;
}

/* fbterm is a userspace terminal; pause it or it repaints over our frames. */
void fb_take(fb_t *f)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) && f->npaused < (int)(sizeof f->paused / sizeof f->paused[0])) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char p[288], comm[64] = "";
        FILE *fp;
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        if (!(fp = fopen(p, "r")))
            continue;
        if (fgets(comm, sizeof comm, fp)) {
            char *nl = strchr(comm, '\n');
            if (nl)
                *nl = 0;
            if (!strcmp(comm, "fbterm")) {
                pid_t pid = (pid_t)atoi(e->d_name);
                if (kill(pid, SIGSTOP) == 0)
                    f->paused[f->npaused++] = pid;
            }
        }
        fclose(fp);
    }
    closedir(d);
}

void fb_release(fb_t *f)
{
    for (int i = 0; i < f->npaused; i++)
        kill(f->paused[i], SIGCONT);
    f->npaused = 0;
    /* fbterm only repaints when tty content changes, so nudge tmux. */
    int rc = system("tmux list-clients -F '#{client_name}' 2>/dev/null | "
                    "while read -r c; do tmux refresh-client -t \"$c\" 2>/dev/null; done");
    (void)rc; /* best effort: the panel repaints on the next tty write anyway */
}

/* expand() moved to expand.c (M0) so it can be compiled -- and compared
 * against canvas_dump() -- on the Mac. fbdev.h includes expand.h. */

int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int pwrite_all(int fd, const void *buf, size_t n, off_t off)
{
    const char *p = buf;
    while (n) {
        ssize_t w = pwrite(fd, p, n, off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
        off += w;
    }
    return 0;
}

/* Present rows y0..y1 only.
 *
 * The panel is line addressed and the driver's damage rect passes the row
 * range through untouched, so a shorter write is a proportionally shorter SPI
 * transfer -- the whole reason this exists. Measured: 420000 B/s, i.e.
 * panel_fps(rows) = 420000 / (52*rows + 2), so 240 rows is 33.1 fps and 120
 * rows is 68 (beepy-vid/DESIGN.md 0.1).
 *
 * row_span_clamp() widens a single row to two, because a write of exactly one
 * line length produces no panel update whatsoever.
 */
int fb_present_rows(fb_t *f, const canvas_t *c, int y0, int y1)
{
    off_t off;
    size_t n;

    if (!row_span_clamp(f->h, &y0, &y1))
        return 0; /* nothing to present is not a failure */
    expand_rows(c, f->frame, f->line_len, y0, y1, f->w);
    off = (off_t)y0 * (off_t)f->line_len;
    n = (size_t)(y1 - y0 + 1) * f->line_len;
    return pwrite_all(f->fd, f->frame + off, n, off);
}

int fb_present(fb_t *f, const canvas_t *c)
{
    return fb_present_rows(f, c, 0, f->h - 1);
}

/* Same frame, to a file -- lets rendering be verified without the panel. */
int fb_dump(const canvas_t *c, const char *path)
{
    size_t line_len = (size_t)SCR_W * 4;
    unsigned char *frame = malloc(line_len * SCR_H);
    int fd, rc;
    if (!frame)
        return -1;
    expand(c, frame, line_len, SCR_H, SCR_W);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(frame);
        return -1;
    }
    rc = write_all(fd, frame, line_len * SCR_H);
    close(fd);
    free(frame);
    return rc;
}

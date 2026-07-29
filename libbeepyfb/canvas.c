/* libbeepyfb/canvas.c -- 1bpp backbuffer and drawing primitives.
 *
 * Split out of gps-monitor.c (M1). Only pure black and pure white are
 * emitted, with any halftone produced as an explicit checkerboard, so
 * nothing depends on what the sharp_drm driver would otherwise dither.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"

canvas_t *canvas_new(int w, int h)
{
    canvas_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->w = w;
    c->h = h;
    c->stride = (w + 7) / 8;
    c->bits = calloc((size_t)c->stride * h, 1);
    if (!c->bits) {
        free(c);
        return NULL;
    }
    return c;
}

void canvas_clear(canvas_t *c, int ink)
{
    memset(c->bits, ink ? 0xFF : 0x00, (size_t)c->stride * c->h);
}

void px(canvas_t *c, int x, int y, int ink)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    unsigned char *b = &c->bits[(size_t)y * c->stride + x / 8];
    unsigned char m = (unsigned char)(0x80u >> (x % 8));
    if (ink)
        *b |= m;
    else
        *b &= (unsigned char)~m;
}

void hline(canvas_t *c, int x, int y, int w, int ink)
{
    for (int i = 0; i < w; i++)
        px(c, x + i, y, ink);
}

void vline(canvas_t *c, int x, int y, int h, int ink)
{
    for (int i = 0; i < h; i++)
        px(c, x, y + i, ink);
}

void fillrect(canvas_t *c, int x, int y, int w, int h, int ink)
{
    for (int j = 0; j < h; j++)
        hline(c, x, y + j, w, ink);
}

void rect(canvas_t *c, int x, int y, int w, int h, int ink)
{
    if (w <= 0 || h <= 0)
        return;
    hline(c, x, y, w, ink);
    hline(c, x, y + h - 1, w, ink);
    vline(c, x, y, h, ink);
    vline(c, x + w - 1, y, h, ink);
}

/* 50% ordered dither: the only halftone a 1-bit panel can honestly show. */
void checker(canvas_t *c, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (((x + i) + (y + j)) % 2 == 0)
                px(c, x + i, y + j, INK);
}

void circle(canvas_t *c, int cx, int cy, int r, int ink)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px(c, cx + x, cy + y, ink); px(c, cx + y, cy + x, ink);
        px(c, cx - y, cy + x, ink); px(c, cx - x, cy + y, ink);
        px(c, cx - x, cy - y, ink); px(c, cx - y, cy - x, ink);
        px(c, cx + y, cy - x, ink); px(c, cx + x, cy - y, ink);
        y++;
        if (err < 0)
            err += 2 * y + 1;
        else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void disc(canvas_t *c, int cx, int cy, int r, int ink)
{
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r)
                px(c, cx + i, cy + j, ink);
}

/* Grid rings are drawn as spaced dots so they stay visually behind markers. */
void dots_circle(canvas_t *c, int cx, int cy, int r, int step)
{
    if (r <= 0)
        return;
    int n = (int)(2.0 * M_PI * r / step);
    if (n < 12)
        n = 12;
    for (int i = 0; i < n; i++) {
        double a = 2.0 * M_PI * i / n;
        px(c, cx + (int)lround(r * sin(a)), cy - (int)lround(r * cos(a)), INK);
    }
}

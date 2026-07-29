/* libbeepyfb/cover.h -- analytic-coverage renderer (beepy-nav DESIGN.md 5.3).
 *
 * One 400x240 8-bit buffer, 0 = ink .. 255 = paper. Primitives composite
 * in painter's order: ink with dst = dst*(1-a), paper with
 * dst = dst + (255-dst)*a, a = coverage. cov_resolve() thresholds at 128
 * into a 1bpp canvas, exactly as mockup.py's resolve() does.
 *
 * Coverage is computed by scan-converting each primitive at 4x with
 * Pillow's own rasterization rules (see cover.c), because the design gate
 * byte-compares these frames against the PIL-rendered mockups.
 *
 * Portable C, libc+libm only.
 */
#ifndef BEEPYFB_COVER_H
#define BEEPYFB_COVER_H

#include "canvas.h"

typedef struct {
    unsigned char v[SCR_H][SCR_W]; /* 0 = ink .. 255 = paper */
    int batch_ink;                 /* pending same-ink batch; -1 = none */
    int bx0, by0, bx1, by1;        /* batch dirty bbox, 4x subpixels */
} cov_t;

#define COV_INK 1
#define COV_PAPER 0

void cov_begin(cov_t *c);
void cov_resolve(cov_t *c, canvas_t *out);

/* mockup.py Canvas.rect(): axis-aligned, exact SSxSS blocks. */
void cov_fill_rect(cov_t *c, double x0, double y0, double x1, double y1,
                   int ink);
/* PIL rectangle outline (chequered-flag frame), width in 1x pixels. */
void cov_rect_outline(cov_t *c, double x0, double y0, double x1, double y1,
                      double width, int ink);
/* mockup.py Canvas.line(): one thick segment, min width 2 (pass width/2). */
void cov_stroke(cov_t *c, double x0, double y0, double x1, double y1,
                double halfwidth, int ink);
void cov_disc(cov_t *c, double cx, double cy, double r, int ink);
void cov_ring(cov_t *c, double cx, double cy, double r, double width,
              int ink);
/* mockup.py stroke(): segments + round caps + joint discs where the turn
 * exceeds 15 degrees. The segs form takes clip_poly() output (x0 y0 x1 y1
 * per segment) and re-caps where clipping split the line. */
void cov_polyline(cov_t *c, const double *xy, int npts, double width,
                  int ink);
void cov_stroke_segs(cov_t *c, const double *segs, int nsegs, double width,
                     int ink);
/* Filled polygon (arrow heads, chevrons); xy = x,y pairs. */
void cov_poly(cov_t *c, const double *xy, int npts, int ink);
/* PIL-style arc band: angles in degrees, clockwise from 3 o'clock, the
 * band extending inward from radius r; width in 1x pixels. */
void cov_arc(cov_t *c, double cx, double cy, double r, double a0, double a1,
             double width, int ink);
/* mockup.py Canvas.hairline(): aliased 1x line expanded to exact 4x4
 * blocks -- full ink, no coverage (dashes, streets, chrome). */
void cov_hairline(cov_t *c, double x0, double y0, double x1, double y1,
                  double w, int ink);
/* 5x7 font at integer scale, exact blocks (wraps font.c's glyph data). */
void cov_text(cov_t *c, int x, int y, const char *s, int scale, int ink);
int cov_text_w(const char *s, int scale);

/* Exact 1:1 bitmap blit: h rows of (w+7)/8 bytes, MSB first, 1 = paint. */
void cov_blit_bits(cov_t *c, int x, int y, int w, int h,
                   const unsigned char *rows, int ink);

#ifdef NUMERALS_H
/* Glyph form, available once the caller has included
 * beepy-nav/src/numerals.h (which owns glyph_t). */
static inline void
cov_blit_glyph(cov_t *c, int x, int y, const glyph_t *g, int ink)
{
    cov_blit_bits(c, x + g->dx, y + g->dy, g->w, g->h, g->rows, ink);
}
#endif

#endif /* BEEPYFB_COVER_H */

/* beepy-nav/src/arrows.c -- see arrows.h.
 *
 * Every coordinate here is transcribed from mockup.py's arrow(); the
 * fractions of s are the design, not derived numbers, so they are written
 * out literally rather than folded into named constants.
 */
#include <string.h>

#include "arrows.h"
#include "draw.h"

const char *const ARROW_TAGS[ARROW_N] = {
    "straight", "slight-left",  "left",        "sharp-left", "slight-right",
    "right",    "sharp-right",  "uturn",       "dest"};

const char *const ARROW_CAPTIONS[ARROW_N] = {
    "STR", "SLTL", "LEFT", "SHPL", "SLTR", "RGHT", "SHPR", "UTRN", "END"};

int
arrow_kind(const char *tag)
{
    int i;
    for (i = 0; i < ARROW_N; i++)
        if (!strcmp(ARROW_TAGS[i], tag))
            return i;
    return -1;
}

/* Geometry shared by every kind: stem half-width, head half-width, head
 * length. The floors keep the 16 px glyph legible. */
typedef struct {
    double hw, hh, hl;
    int ink;
} geom_t;

/* Tip triangle: (dx, dy) is the unit direction it points. */
static void
head(cov_t *c, const geom_t *g, double tx, double ty, double dx, double dy)
{
    double px = -dy, py = dx;
    double bx = tx - dx * g->hl, by = ty - dy * g->hl;
    double p[6];
    p[0] = tx;
    p[1] = ty;
    p[2] = bx + px * g->hh;
    p[3] = by + py * g->hh;
    p[4] = bx - px * g->hh;
    p[5] = by - py * g->hh;
    poly1x(c, p, 3, g->ink);
}

/* One stem segment; `cap` rounds off the far end so an elbow stays solid. */
static void
bar(cov_t *c, const geom_t *g, double ax, double ay, double bx, double by,
    int cap)
{
    cov_stroke(c, ax, ay, bx, by, g->hw, g->ink); /* mockup width = hw * 2 */
    if (cap)
        cov_disc(c, bx, by, g->hw, g->ink);
}

void
arrow_draw(cov_t *c, double x0, double y0, double s, int kind, int ink)
{
    double cx = x0 + s / 2.0;
    geom_t g;
    double sgn, ty, dx, dy, tipx, tipy, x_tip, r, top;

    g.hw = s * 0.10 > 0.6 ? s * 0.10 : 0.6;
    g.hh = s * 0.26 > 2.4 ? s * 0.26 : 2.4;
    g.hl = s * 0.30 > 3.5 ? s * 0.30 : 3.5;
    g.ink = ink;

    switch (kind) {
    case ARROW_STRAIGHT:
        bar(c, &g, cx, y0 + s, cx, y0 + g.hl, 0);
        head(c, &g, cx, y0 + s * 0.02, 0, -1);
        break;

    case ARROW_LEFT:
    case ARROW_RIGHT:
        sgn = kind == ARROW_LEFT ? -1.0 : 1.0;
        ty = y0 + s * 0.36;
        bar(c, &g, cx, y0 + s, cx, ty, 1);
        x_tip = cx + sgn * s * 0.44;
        bar(c, &g, cx, ty, x_tip - sgn * g.hl, ty, 0);
        head(c, &g, x_tip, ty, sgn, 0);
        break;

    case ARROW_SLIGHT_LEFT:
    case ARROW_SLIGHT_RIGHT:
        sgn = kind == ARROW_SLIGHT_LEFT ? -1.0 : 1.0;
        ty = y0 + s * 0.56;
        dx = sgn * 0.55;
        dy = -0.84;
        tipx = cx + sgn * s * 0.36;
        tipy = y0 + s * 0.04;
        bar(c, &g, cx, y0 + s, cx, ty, 1);
        bar(c, &g, cx, ty, tipx - dx * g.hl, tipy - dy * g.hl, 0);
        head(c, &g, tipx, tipy, dx, dy);
        break;

    case ARROW_SHARP_LEFT:
    case ARROW_SHARP_RIGHT:
        /* Climb, then strike back down and outward at 135 degrees. A head
         * pointing below its own corner is the one cue "sharp" needs, and
         * the only version that still reads at 16 px. */
        sgn = kind == ARROW_SHARP_LEFT ? -1.0 : 1.0;
        ty = y0 + s * 0.28;
        dx = sgn * 0.7;
        dy = 0.7;
        tipx = cx + sgn * s * 0.44;
        tipy = y0 + s * 0.74;
        bar(c, &g, cx, y0 + s, cx, ty, 1);
        bar(c, &g, cx, ty, tipx - dx * g.hl, tipy - dy * g.hl, 0);
        head(c, &g, tipx, tipy, dx, dy);
        break;

    case ARROW_UTURN:
        r = s * 0.23;
        top = y0 + s * 0.30;
        cov_arc(c, cx, top, r, 180, 360, g.hw * 2, ink);
        bar(c, &g, cx + r, top, cx + r, y0 + s, 0);
        bar(c, &g, cx - r, top, cx - r, y0 + s * 0.50 - g.hl, 0);
        head(c, &g, cx - r, y0 + s * 0.62, 0, 1);
        break;

    case ARROW_DEST: { /* chequered flag */
        double fx = cx - s * 0.20, fy = y0 + s * 0.08;
        double fw = s * 0.46, fh = s * 0.34;
        double cw = fw / 4.0, chh = fh / 3.0;
        int row, col;
        bar(c, &g, fx, fy, fx, y0 + s, 0);
        for (row = 0; row < 3; row++)
            for (col = 0; col < 4; col++)
                if ((row + col) % 2 == 0)
                    cov_fill_rect(c, fx + col * cw, fy + row * chh,
                                  fx + (col + 1) * cw - 1,
                                  fy + (row + 1) * chh - 1, ink);
        cov_rect_outline(c, fx, fy, fx + fw, fy + fh, 1.0, ink);
        break;
    }
    default:
        break;
    }
}

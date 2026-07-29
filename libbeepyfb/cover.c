/* libbeepyfb/cover.c -- analytic-coverage renderer (beepy-nav DESIGN.md 5.3,
 * decision D3).
 *
 * The public model is the one the design specifies: a single 400x240 8-bit
 * buffer (0 = ink .. 255 = paper), primitives composited in painter's order
 * (ink: dst = dst*(1-a); paper: dst = dst + (255-dst)*a), resolved with a
 * 50% threshold. No full-frame 4x supersample buffer is rendered or
 * resolved per frame.
 *
 * How a primitive's coverage `a` is obtained matters more than the design
 * anticipated: the M2 gate byte-compares these frames against mockup.py,
 * which rasterizes through Pillow at 4x and box-downsamples. A smooth
 * signed-distance ramp agrees with that only to within a pixel here and
 * there -- systematically off along every axis-aligned stroke edge whose
 * coverage lands on exactly 1/2. So coverage is computed by scan-converting
 * the primitive at 4x with Pillow's own integer rasterization rules,
 * transcribed from Pillow 12.1.1 src/libImaging/Draw.c (PIL license):
 * polygon_generic() for thick strokes and polygons, the quarter-arc state
 * machine for ellipses, the clip-tree for arc bands, Bresenham for
 * hairlines. Coordinates are truncated to the 4x grid exactly as
 * _imaging.c's (int) casts do.
 *
 * Consecutive primitives of the SAME colour accumulate in a shared 4x
 * bitmask (192 KB, bounding-box managed) and composite on colour change,
 * so unions inside one shape -- polyline joins, an arrow's stem + head --
 * match Pillow bit for bit. The box filter itself is Pillow's separable
 * resize: per output pixel, each 4-subpixel row averages with
 * round-half-up, then the four row values average the same way. Only
 * partial edges of different-colour shapes go through the a-blend
 * approximation, which is where the gate's small edges-only budget goes.
 */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "font.h"
#include "cover.h"

#define SS 4
#define SW (SCR_W * SS) /* 1600 */
#define SH (SCR_H * SS) /*  960 */

/* ------------------------------------------------- 4x accumulator bitmask */

static unsigned char ACC[SH][SW / 8];

/* Pillow Draw.c rounding: around zero, so ROUND(f) == -ROUND(-f). */
#define PROUND_UP(f) ((int)((f) >= 0.0 ? floor((f) + 0.5F) : -floor(fabs(f) + 0.5F)))
#define PROUND_DOWN(f) ((int)((f) >= 0.0 ? ceil((f) - 0.5F) : -ceil(fabs(f) - 0.5F)))

static void
bbox_add(cov_t *c, int sx0, int sy0, int sx1, int sy1)
{
    if (sx0 < 0)
        sx0 = 0;
    if (sy0 < 0)
        sy0 = 0;
    if (sx1 >= SW)
        sx1 = SW - 1;
    if (sy1 >= SH)
        sy1 = SH - 1;
    if (sx0 > sx1 || sy0 > sy1)
        return;
    if (c->batch_ink < 0)
        return; /* defensive; primitives always open a batch first */
    if (sx0 < c->bx0)
        c->bx0 = sx0;
    if (sy0 < c->by0)
        c->by0 = sy0;
    if (sx1 > c->bx1)
        c->bx1 = sx1;
    if (sy1 > c->by1)
        c->by1 = sy1;
}

/* Emitter: where rasterized spans land. The 4x emitter plots subpixels;
 * the block emitter plots 1x pixels as solid 4x4 blocks (the mockup's
 * aliased hairline layer, blown up NEAREST). Both carry the Pillow image
 * size their coordinate system uses, for Pillow-identical clipping. */
typedef struct {
    cov_t *c;
    int xsize, ysize;
    void (*point)(cov_t *c, int x, int y);
    void (*hline)(cov_t *c, int x0, int y, int x1);
} emit_t;

static void
acc_span(cov_t *c, int x0, int y, int x1)
{
    /* x0..x1 inclusive on row y, already clipped; subpixel coords. */
    unsigned char *row = ACC[y];
    int b0 = x0 >> 3, b1 = x1 >> 3;
    if (b0 == b1) {
        row[b0] |= (unsigned char)((0xFFu >> (x0 & 7)) &
                                   (0xFFu << (7 - (x1 & 7))));
    } else {
        row[b0] |= (unsigned char)(0xFFu >> (x0 & 7));
        if (b1 > b0 + 1)
            memset(row + b0 + 1, 0xFF, (size_t)(b1 - b0 - 1));
        row[b1] |= (unsigned char)(0xFFu << (7 - (x1 & 7)));
    }
    bbox_add(c, x0, y, x1, y);
}

static void
e4_point(cov_t *c, int x, int y)
{
    if (x >= 0 && x < SW && y >= 0 && y < SH)
        acc_span(c, x, y, x);
}

static void
e4_hline(cov_t *c, int x0, int y, int x1)
{
    /* Pillow hline8 clipping semantics. */
    if (y < 0 || y >= SH)
        return;
    if (x0 < 0)
        x0 = 0;
    else if (x0 >= SW)
        return;
    if (x1 < 0)
        return;
    else if (x1 >= SW)
        x1 = SW - 1;
    if (x0 <= x1)
        acc_span(c, x0, y, x1);
}

static void
blk_point(cov_t *c, int x, int y)
{
    int sy;
    if (x < 0 || x >= SCR_W || y < 0 || y >= SCR_H)
        return;
    for (sy = y * SS; sy < y * SS + SS; sy++)
        acc_span(c, x * SS, sy, x * SS + SS - 1);
}

static void
blk_hline(cov_t *c, int x0, int y, int x1)
{
    int sy;
    if (y < 0 || y >= SCR_H)
        return;
    if (x0 < 0)
        x0 = 0;
    else if (x0 >= SCR_W)
        return;
    if (x1 < 0)
        return;
    else if (x1 >= SCR_W)
        x1 = SCR_W - 1;
    if (x0 > x1)
        return;
    for (sy = y * SS; sy < y * SS + SS; sy++)
        acc_span(c, x0 * SS, sy, x1 * SS + SS - 1);
}

static const emit_t EM4 = {NULL, SW, SH, e4_point, e4_hline};
static const emit_t EMBLK = {NULL, SCR_W, SCR_H, blk_point, blk_hline};

/* ------------------------------------------------ batch commit / composite
 *
 * Pillow's BOX downsample of a binary 4x image, per output pixel: each of
 * the four subpixel rows averages its four samples with round-half-up
 * (horizontal pass), then the four row values average the same way
 * (vertical pass). ROWV[] is that first pass for 0..4 ink samples.
 *
 * Round-half-UP is toward white, and this buffer stores 0 = ink, so which
 * way a half lands depends on the batch's colour. For an ink batch ROWV is
 * literally Pillow's row value and (sum + 2) >> 2 its column average. For a
 * paper batch the buffer holds the complement, and the complement of a
 * half-up average is a half-DOWN one: ROWVP[2] is 127 rather than 128, and
 * the column average rounds with + 1. Without the second table every
 * axis-aligned paper edge whose coverage lands on exactly 1/2 -- the whole
 * length of the panel arrow's stem, for one -- resolved a pixel darker than
 * the mockup.
 */
static const unsigned char ROWV[5] = {255, 191, 128, 64, 0};
static const unsigned char ROWVP[5] = {255, 191, 127, 64, 0};
static const unsigned char POPC[16] = {0, 1, 1, 2, 1, 2, 2, 3,
                                       1, 2, 2, 3, 2, 3, 3, 4};

static void
cov_commit(cov_t *c)
{
    const unsigned char *rowv;
    int x, y, px0, py0, px1, py1, rnd;
    if (c->batch_ink < 0)
        return;
    if (c->bx0 > c->bx1 || c->by0 > c->by1) { /* empty batch */
        c->batch_ink = -1;
        return;
    }
    px0 = c->bx0 / SS;
    py0 = c->by0 / SS;
    px1 = c->bx1 / SS;
    py1 = c->by1 / SS;
    rowv = c->batch_ink == COV_INK ? ROWV : ROWVP;
    rnd = c->batch_ink == COV_INK ? 2 : 1;
    for (y = py0; y <= py1; y++) {
        const unsigned char *r0 = ACC[y * SS];
        const unsigned char *r1 = ACC[y * SS + 1];
        const unsigned char *r2 = ACC[y * SS + 2];
        const unsigned char *r3 = ACC[y * SS + 3];
        for (x = px0; x <= px1; x++) {
            int bi = x >> 1, sh = (x & 1) ? 0 : 4, v;
            int n0 = (r0[bi] >> sh) & 0xF, n1 = (r1[bi] >> sh) & 0xF;
            int n2 = (r2[bi] >> sh) & 0xF, n3 = (r3[bi] >> sh) & 0xF;
            unsigned char *d;
            if (!(n0 | n1 | n2 | n3))
                continue;
            v = (rowv[POPC[n0]] + rowv[POPC[n1]] + rowv[POPC[n2]] +
                 rowv[POPC[n3]] + rnd) >>
                2;
            d = &c->v[y][x];
            if (c->batch_ink == COV_INK)
                *d = (unsigned char)((*d * v + 127) / 255);
            else
                *d = (unsigned char)(255 - ((255 - *d) * v + 127) / 255);
        }
    }
    for (y = c->by0; y <= c->by1; y++)
        memset(&ACC[y][c->bx0 >> 3], 0, (size_t)((c->bx1 >> 3) - (c->bx0 >> 3) + 1));
    c->batch_ink = -1;
}

static void
batch_want(cov_t *c, int ink)
{
    if (c->batch_ink == ink)
        return;
    cov_commit(c);
    c->batch_ink = ink;
    c->bx0 = SW;
    c->by0 = SH;
    c->bx1 = -1;
    c->by1 = -1;
}

void
cov_begin(cov_t *c)
{
    if (c->batch_ink >= 0 && c->bx1 >= 0) { /* abandoned batch: just clear */
        int y;
        for (y = c->by0; y <= c->by1; y++)
            memset(&ACC[y][c->bx0 >> 3], 0,
                   (size_t)((c->bx1 >> 3) - (c->bx0 >> 3) + 1));
    }
    memset(c->v, 255, sizeof(c->v));
    c->batch_ink = -1;
}

/* ---------------------------------------------- Pillow polygon rasterizer */

typedef struct {
    int d;
    int x0, y0;
    int xmin, ymin, xmax, ymax;
    float dx;
} pedge_t;

static void
add_edge(pedge_t *e, int x0, int y0, int x1, int y1)
{
    if (x0 <= x1) {
        e->xmin = x0;
        e->xmax = x1;
    } else {
        e->xmin = x1;
        e->xmax = x0;
    }
    if (y0 <= y1) {
        e->ymin = y0;
        e->ymax = y1;
    } else {
        e->ymin = y1;
        e->ymax = y0;
    }
    if (y0 == y1) {
        e->d = 0;
        e->dx = 0.0f;
    } else {
        e->dx = ((float)(x1 - x0)) / (float)(y1 - y0);
        e->d = (y0 == e->ymin) ? 1 : -1;
    }
    e->x0 = x0;
    e->y0 = y0;
}

#define MAXEDGE 16

static void
polygon_generic(const emit_t *em, cov_t *c, int n, pedge_t *e)
{
    pedge_t *edge_table[MAXEDGE];
    float xx[2 * MAXEDGE];
    int edge_count = 0;
    int ymin = em->ysize - 1;
    int ymax = 0;
    int i, j, k;
    float adjacent_line_x, adjacent_line_x_other_edge;

    if (n <= 0)
        return;
    for (i = 0; i < n; i++) {
        if (ymin > e[i].ymin)
            ymin = e[i].ymin;
        if (ymax < e[i].ymax)
            ymax = e[i].ymax;
        if (e[i].ymin == e[i].ymax) {
            em->hline(c, e[i].xmin, e[i].ymin, e[i].xmax);
            continue;
        }
        edge_table[edge_count++] = e + i;
    }
    if (ymin < 0)
        ymin = 0;
    if (ymax > em->ysize)
        ymax = em->ysize;

    for (; ymin <= ymax; ymin++) {
        j = 0;
        for (i = 0; i < edge_count; i++) {
            pedge_t *current = edge_table[i];
            if (ymin >= current->ymin && ymin <= current->ymax) {
                xx[j++] = (float)(ymin - current->y0) * current->dx +
                          (float)current->x0;
                if (ymin == current->ymax && ymin < ymax) {
                    /* needed to draw consistent polygons */
                    xx[j] = xx[j - 1];
                    j++;
                } else if ((ymin == current->ymin || ymin == current->ymax) &&
                           current->dx != 0) {
                    /* connect discontiguous corners */
                    for (k = 0; k < i; k++) {
                        pedge_t *other_edge = edge_table[k];
                        if ((ymin != other_edge->ymin &&
                             ymin != other_edge->ymax) ||
                            other_edge->dx == 0)
                            continue;
                        if (roundf(xx[j - 1]) ==
                            roundf((float)(ymin - other_edge->y0) *
                                       other_edge->dx +
                                   (float)other_edge->x0)) {
                            int offset = (ymin == current->ymax) ? -1 : 1;
                            adjacent_line_x =
                                (float)(ymin + offset - current->y0) *
                                    current->dx +
                                (float)current->x0;
                            if (ymin + offset >= other_edge->ymin &&
                                ymin + offset <= other_edge->ymax) {
                                adjacent_line_x_other_edge =
                                    (float)(ymin + offset - other_edge->y0) *
                                        other_edge->dx +
                                    (float)other_edge->x0;
                                if (xx[j - 1] > adjacent_line_x + 1 &&
                                    xx[j - 1] >
                                        adjacent_line_x_other_edge + 1) {
                                    xx[j - 1] =
                                        roundf(fmaxf(
                                            adjacent_line_x,
                                            adjacent_line_x_other_edge)) +
                                        1;
                                } else if (xx[j - 1] < adjacent_line_x - 1 &&
                                           xx[j - 1] <
                                               adjacent_line_x_other_edge -
                                                   1) {
                                    xx[j - 1] =
                                        roundf(fminf(
                                            adjacent_line_x,
                                            adjacent_line_x_other_edge)) -
                                        1;
                                }
                                break;
                            }
                        }
                    }
                }
            }
        }
        /* insertion sort (Pillow qsorts; j is tiny here) */
        for (i = 1; i < j; i++) {
            float t = xx[i];
            for (k = i; k > 0 && xx[k - 1] > t; k--)
                xx[k] = xx[k - 1];
            xx[k] = t;
        }
        for (i = 1; i < j; i += 2)
            em->hline(c, PROUND_UP(xx[i - 1]), ymin, PROUND_DOWN(xx[i]));
    }
}

/* Pillow line8: Bresenham, endpoint excluded (draw_lines adds it back for
 * user-level 1px lines; rectangle outlines call this bare). */
static void
pil_line(const emit_t *em, cov_t *c, int x0, int y0, int x1, int y1)
{
    int i, n, e, dx, dy, xs, ys;
    dx = x1 - x0;
    if (dx < 0) {
        dx = -dx;
        xs = -1;
    } else
        xs = 1;
    dy = y1 - y0;
    if (dy < 0) {
        dy = -dy;
        ys = -1;
    } else
        ys = 1;
    if (dx == 0) {
        for (i = 0; i < dy; i++) {
            em->point(c, x0, y0);
            y0 += ys;
        }
    } else if (dy == 0) {
        for (i = 0; i < dx; i++) {
            em->point(c, x0, y0);
            x0 += xs;
        }
    } else if (dx > dy) {
        n = dx;
        dy += dy;
        e = dy - dx;
        dx += dx;
        for (i = 0; i < n; i++) {
            em->point(c, x0, y0);
            if (e >= 0) {
                y0 += ys;
                e -= dx;
            }
            e += dy;
            x0 += xs;
        }
    } else {
        n = dy;
        dx += dx;
        e = dx - dy;
        dy += dy;
        for (i = 0; i < n; i++) {
            em->point(c, x0, y0);
            if (e >= 0) {
                x0 += xs;
                e -= dy;
            }
            e += dx;
            y0 += ys;
        }
    }
}

/* Pillow ImagingDrawWideLine. */
static void
pil_wide_line(const emit_t *em, cov_t *c, int x0, int y0, int x1, int y1,
              int width)
{
    int dx, dy;
    double big_hypotenuse, small_hypotenuse, ratio_max, ratio_min;
    int dxmin, dxmax, dymin, dymax;
    pedge_t e[4];

    dx = x1 - x0;
    dy = y1 - y0;
    if (dx == 0 && dy == 0) {
        em->point(c, x0, y0);
        return;
    }
    big_hypotenuse = hypot(dx, dy);
    small_hypotenuse = (width - 1) / 2.0;
    ratio_max = PROUND_UP(small_hypotenuse) / big_hypotenuse;
    ratio_min = PROUND_DOWN(small_hypotenuse) / big_hypotenuse;
    dxmin = PROUND_DOWN(ratio_min * dy);
    dxmax = PROUND_DOWN(ratio_max * dy);
    dymin = PROUND_DOWN(ratio_min * dx);
    dymax = PROUND_DOWN(ratio_max * dx);

    add_edge(e + 0, x0 - dxmin, y0 + dymax, x1 - dxmin, y1 + dymax);
    add_edge(e + 1, x1 - dxmin, y1 + dymax, x1 + dxmax, y1 - dymin);
    add_edge(e + 2, x1 + dxmax, y1 - dymin, x0 + dxmax, y0 - dymin);
    add_edge(e + 3, x0 + dxmax, y0 - dymin, x0 - dxmin, y0 + dymax);
    polygon_generic(em, c, 4, e);
}

/* User-level line: coordinates truncated like _imaging.c's (int) casts,
 * width already the integer Pillow receives. */
static void
pil_user_line(const emit_t *em, cov_t *c, double fx0, double fy0, double fx1,
              double fy1, int width)
{
    int x0 = (int)fx0, y0 = (int)fy0, x1 = (int)fx1, y1 = (int)fy1;
    if (width <= 1) {
        pil_line(em, c, x0, y0, x1, y1);
        em->point(c, x1, y1); /* draw_lines draws the last point */
    } else {
        pil_wide_line(em, c, x0, y0, x1, y1, width);
    }
}

/* --------------------------------------------- Pillow ellipse rasterizer */

typedef struct {
    int32_t a, b, cx, cy, ex, ey;
    int64_t a2, b2, a2b2;
    int8_t finished;
} quarter_state;

static void
quarter_init(quarter_state *s, int32_t a, int32_t b)
{
    if (a < 0 || b < 0) {
        s->finished = 1;
    } else {
        s->a = a;
        s->b = b;
        s->cx = a;
        s->cy = b % 2;
        s->ex = a % 2;
        s->ey = b;
        s->a2 = (int64_t)a * a;
        s->b2 = (int64_t)b * b;
        s->a2b2 = s->a2 * s->b2;
        s->finished = 0;
    }
}

static int64_t
quarter_delta(quarter_state *s, int64_t x, int64_t y)
{
    return llabs(s->a2 * y * y + s->b2 * x * x - s->a2b2);
}

static int8_t
quarter_next(quarter_state *s, int32_t *ret_x, int32_t *ret_y)
{
    if (s->finished)
        return -1;
    *ret_x = s->cx;
    *ret_y = s->cy;
    if (s->cx == s->ex && s->cy == s->ey) {
        s->finished = 1;
    } else {
        int32_t nx = s->cx;
        int32_t ny = s->cy + 2;
        int64_t ndelta = quarter_delta(s, nx, ny);
        if (nx > 1) {
            int64_t newdelta = quarter_delta(s, s->cx - 2, s->cy + 2);
            if (ndelta > newdelta) {
                nx = s->cx - 2;
                ny = s->cy + 2;
                ndelta = newdelta;
            }
            newdelta = quarter_delta(s, s->cx - 2, s->cy);
            if (ndelta > newdelta) {
                nx = s->cx - 2;
                ny = s->cy;
            }
        }
        s->cx = nx;
        s->cy = ny;
    }
    return 0;
}

typedef struct {
    quarter_state st_o, st_i;
    int32_t py, pl, pr;
    int32_t cy[4], cl[4], cr[4];
    int8_t bufcnt;
    int8_t finished;
    int8_t leftmost;
} ellipse_state;

static void
ellipse_init(ellipse_state *s, int32_t a, int32_t b, int32_t w)
{
    s->bufcnt = 0;
    s->leftmost = (int8_t)(a % 2);
    quarter_init(&s->st_o, a, b);
    if (w < 1 || quarter_next(&s->st_o, &s->pr, &s->py) == -1) {
        s->finished = 1;
    } else {
        s->finished = 0;
        quarter_init(&s->st_i, a - 2 * (w - 1), b - 2 * (w - 1));
        s->pl = s->leftmost;
    }
}

static int8_t
ellipse_next(ellipse_state *s, int32_t *ret_x0, int32_t *ret_y,
             int32_t *ret_x1)
{
    if (s->bufcnt == 0) {
        int32_t y, l, r, cx = 0, cy = 0;
        int8_t next_ret;
        if (s->finished)
            return -1;
        y = s->py;
        l = s->pl;
        r = s->pr;
        while ((next_ret = quarter_next(&s->st_o, &cx, &cy)) != -1 && cy <= y)
            ;
        if (next_ret == -1)
            s->finished = 1;
        else {
            s->pr = cx;
            s->py = cy;
        }
        while ((next_ret = quarter_next(&s->st_i, &cx, &cy)) != -1 && cy <= y)
            l = cx;
        s->pl = (next_ret == -1) ? s->leftmost : cx;

        if ((l > 0 || l < r) && y > 0) {
            s->cl[s->bufcnt] = (l == 0) ? 2 : l;
            s->cy[s->bufcnt] = y;
            s->cr[s->bufcnt] = r;
            ++s->bufcnt;
        }
        if (y > 0) {
            s->cl[s->bufcnt] = -r;
            s->cy[s->bufcnt] = y;
            s->cr[s->bufcnt] = -l;
            ++s->bufcnt;
        }
        if (l > 0 || l < r) {
            s->cl[s->bufcnt] = (l == 0) ? 2 : l;
            s->cy[s->bufcnt] = -y;
            s->cr[s->bufcnt] = r;
            ++s->bufcnt;
        }
        s->cl[s->bufcnt] = -r;
        s->cy[s->bufcnt] = -y;
        s->cr[s->bufcnt] = -l;
        ++s->bufcnt;
    }
    --s->bufcnt;
    *ret_x0 = s->cl[s->bufcnt];
    *ret_y = s->cy[s->bufcnt];
    *ret_x1 = s->cr[s->bufcnt];
    return 0;
}

/* Ellipse in a truncated 4x bbox; fill or outline width, per ellipseNew. */
static void
pil_ellipse(const emit_t *em, cov_t *c, int x0, int y0, int x1, int y1,
            int fill, int width)
{
    ellipse_state st;
    int32_t X0, Y, X1;
    int a = x1 - x0;
    int b = y1 - y0;
    if (a < 0 || b < 0)
        return;
    if (fill)
        width = a + b;
    ellipse_init(&st, a, b, width);
    while (ellipse_next(&st, &X0, &Y, &X1) != -1)
        em->hline(c, x0 + (X0 + a) / 2, y0 + (Y + b) / 2, x0 + (X1 + a) / 2);
}

/* ------------------------------------------------- Pillow arc (clip tree) */

typedef enum { CT_AND, CT_OR, CT_CLIP } ctype_t;

typedef struct cnode {
    ctype_t type;
    double a, b, c;
    struct cnode *l, *r;
} cnode_t;

typedef struct ev {
    int32_t x;
    int8_t type;
    struct ev *next;
} ev_t;

typedef struct {
    ellipse_state st;
    cnode_t *root;
    cnode_t nodes[7];
    int32_t node_count;
    ev_t *head;
    int32_t y;
} clip_ellipse_state;

static void
clip_tree_transpose(cnode_t *root)
{
    if (root != NULL) {
        if (root->type == CT_CLIP) {
            double t = root->a;
            root->a = root->b;
            root->b = t;
        }
        clip_tree_transpose(root->l);
        clip_tree_transpose(root->r);
    }
}

static int
clip_tree_do_clip(cnode_t *root, int32_t x0, int32_t y, int32_t x1,
                  ev_t **ret)
{
    if (root == NULL) {
        ev_t *start = malloc(sizeof(ev_t));
        ev_t *end = malloc(sizeof(ev_t));
        if (!start || !end) {
            free(start);
            free(end);
            return -1;
        }
        start->x = x0;
        start->type = 1;
        start->next = end;
        end->x = x1;
        end->type = -1;
        end->next = NULL;
        *ret = start;
        return 0;
    }
    if (root->type == CT_CLIP) {
        double eps = 1e-9;
        double A = root->a;
        double B = root->b;
        double C = root->c;
        if (fabs(A) < eps) {
            if (B * y + C < -eps) {
                x0 = 1;
                x1 = 0;
            }
        } else {
            double ix = -(B * y + C) / A;
            if (A * x0 + B * y + C < eps)
                x0 = (int32_t)lround(fmax(x0, ix));
            if (A * x1 + B * y + C < eps)
                x1 = (int32_t)lround(fmin(x1, ix));
        }
        if (x0 <= x1) {
            ev_t *start = malloc(sizeof(ev_t));
            ev_t *end = malloc(sizeof(ev_t));
            if (!start || !end) {
                free(start);
                free(end);
                return -1;
            }
            start->x = x0;
            start->type = 1;
            start->next = end;
            end->x = x1;
            end->type = -1;
            end->next = NULL;
            *ret = start;
        } else {
            *ret = NULL;
        }
        return 0;
    }
    if (root->type == CT_OR || root->type == CT_AND) {
        ev_t *l1, *l2, *tail = NULL;
        int32_t k1 = 0, k2 = 0;
        if (clip_tree_do_clip(root->l, x0, y, x1, &l1) < 0)
            return -1;
        if (clip_tree_do_clip(root->r, x0, y, x1, &l2) < 0) {
            while (l1) {
                l2 = l1->next;
                free(l1);
                l1 = l2;
            }
            return -1;
        }
        *ret = NULL;
        while (l1 != NULL || l2 != NULL) {
            ev_t *t;
            if (l2 == NULL ||
                (l1 != NULL &&
                 (l1->x < l2->x || (l1->x == l2->x && l1->type > l2->type)))) {
                t = l1;
                k1 += t->type;
                l1 = l1->next;
            } else {
                t = l2;
                k2 += t->type;
                l2 = l2->next;
            }
            t->next = NULL;
            if ((root->type == CT_OR &&
                 ((t->type == 1 && (tail == NULL || tail->type == -1)) ||
                  (t->type == -1 && k1 == 0 && k2 == 0))) ||
                (root->type == CT_AND &&
                 ((t->type == 1 && (tail == NULL || tail->type == -1) &&
                   k1 > 0 && k2 > 0) ||
                  (t->type == -1 && tail != NULL && tail->type == 1 &&
                   (k1 == 0 || k2 == 0))))) {
                if (tail == NULL)
                    *ret = t;
                else
                    tail->next = t;
                tail = t;
            } else {
                free(t);
            }
        }
        return 0;
    }
    *ret = NULL;
    return 0;
}

/* Resulting angles satisfy 0 <= al < 360, al <= ar <= al + 360. */
static void
normalize_angles(float *al, float *ar)
{
    if (*ar - *al >= 360) {
        *al = 0;
        *ar = 360;
    } else {
        *al = (float)fmod(*al < 0 ? 360 - fmod(-*al, 360) : *al, 360);
        *ar = *al + (float)fmod(*ar < *al ? 360 - fmod(*al - *ar, 360)
                                          : *ar - *al,
                                360);
    }
}

static void
arc_init(clip_ellipse_state *s, int32_t a, int32_t b, int32_t w, float al,
         float ar)
{
    if (a < b) {
        /* transpose the coordinate system */
        arc_init(s, b, a, w, 90 - ar, 90 - al);
        ellipse_init(&s->st, a, b, w);
        clip_tree_transpose(s->root);
    } else {
        ellipse_init(&s->st, a, b, w);
        s->head = NULL;
        s->node_count = 0;
        normalize_angles(&al, &ar);
        if (ar == al + 360) {
            s->root = NULL;
        } else {
            cnode_t *lc = s->nodes + s->node_count++;
            cnode_t *rc = s->nodes + s->node_count++;
            lc->l = lc->r = rc->l = rc->r = NULL;
            lc->type = rc->type = CT_CLIP;
            lc->a = -a * sin(al * M_PI / 180.0);
            lc->b = b * cos(al * M_PI / 180.0);
            lc->c = (double)(a * a - b * b) * sin(al * M_PI / 90.0) / 2.0;
            rc->a = a * sin(ar * M_PI / 180.0);
            rc->b = -b * cos(ar * M_PI / 180.0);
            rc->c = (double)(b * b - a * a) * sin(ar * M_PI / 90.0) / 2.0;
            if (fmod(al, 180) == 0 || fmod(ar, 180) == 0) {
                s->root = s->nodes + s->node_count++;
                s->root->l = lc;
                s->root->r = rc;
                s->root->type = (ar - al < 180) ? CT_AND : CT_OR;
            } else if (((int)(al / 180) + (int)(ar / 180)) % 2 == 1) {
                s->root = s->nodes + s->node_count++;
                s->root->l = s->nodes + s->node_count++;
                s->root->l->l = s->nodes + s->node_count++;
                s->root->l->r = lc;
                s->root->r = s->nodes + s->node_count++;
                s->root->r->l = s->nodes + s->node_count++;
                s->root->r->r = rc;
                s->root->type = CT_OR;
                s->root->l->type = CT_AND;
                s->root->r->type = CT_AND;
                s->root->l->l->type = CT_CLIP;
                s->root->r->l->type = CT_CLIP;
                s->root->l->l->l = s->root->l->l->r = NULL;
                s->root->r->l->l = s->root->r->l->r = NULL;
                s->root->l->l->a = s->root->l->l->c = 0;
                s->root->r->l->a = s->root->r->l->c = 0;
                s->root->l->l->b = ((int)(al / 180) % 2 == 0) ? 1 : -1;
                s->root->r->l->b = ((int)(ar / 180) % 2 == 0) ? 1 : -1;
            } else {
                s->root = s->nodes + s->node_count++;
                s->root->l = s->nodes + s->node_count++;
                s->root->r = s->nodes + s->node_count++;
                s->root->type = s->root->l->type =
                    (ar - al < 180) ? CT_AND : CT_OR;
                s->root->l->l = lc;
                s->root->l->r = rc;
                s->root->r->type = CT_CLIP;
                s->root->r->l = s->root->r->r = NULL;
                s->root->r->a = s->root->r->c = 0;
                s->root->r->b = (ar < 180 || ar > 540) ? 1 : -1;
            }
        }
    }
}

static void
clip_ellipse_free(clip_ellipse_state *s)
{
    while (s->head != NULL) {
        ev_t *t = s->head;
        s->head = s->head->next;
        free(t);
    }
}

static int8_t
clip_ellipse_next(clip_ellipse_state *s, int32_t *ret_x0, int32_t *ret_y,
                  int32_t *ret_x1)
{
    int32_t x0, y, x1;
    while (s->head == NULL && ellipse_next(&s->st, &x0, &y, &x1) >= 0) {
        if (clip_tree_do_clip(s->root, x0, y, x1, &s->head) < 0)
            return -2;
        s->y = y;
    }
    if (s->head != NULL) {
        ev_t *t = s->head;
        *ret_y = s->y;
        s->head = s->head->next;
        *ret_x0 = t->x;
        free(t);
        t = s->head;
        s->head = s->head->next;
        *ret_x1 = t->x;
        free(t);
        return 0;
    }
    return -1;
}

static void
pil_arc(const emit_t *em, cov_t *c, int x0, int y0, int x1, int y1,
        float start, float end, int width)
{
    clip_ellipse_state st;
    int32_t X0, Y, X1;
    int a = x1 - x0;
    int b = y1 - y0;
    if (a < 0 || b < 0)
        return;
    arc_init(&st, a, b, width, start, end);
    while (clip_ellipse_next(&st, &X0, &Y, &X1) >= 0)
        em->hline(c, x0 + (X0 + a) / 2, y0 + (Y + b) / 2, x0 + (X1 + a) / 2);
    clip_ellipse_free(&st);
}

/* ---------------------------------------------------------- public API
 *
 * Every function mirrors one mockup.py Canvas method, including its
 * argument conditioning (width clamps, Python round() = rint half-even,
 * coordinate truncation at 4x).
 */

void
cov_stroke(cov_t *c, double x0, double y0, double x1, double y1,
           double halfwidth, int ink)
{
    double w = fmax(2.0, halfwidth * 2.0);
    int wsub = (int)rint(w * SS);
    if (wsub < 1)
        wsub = 1;
    batch_want(c, ink);
    pil_user_line(&EM4, c, x0 * SS, y0 * SS, x1 * SS, y1 * SS, wsub);
}

void
cov_disc(cov_t *c, double cx, double cy, double r, int ink)
{
    batch_want(c, ink);
    pil_ellipse(&EM4, c, (int)((cx - r) * SS), (int)((cy - r) * SS),
                (int)((cx + r) * SS), (int)((cy + r) * SS), 1, 0);
}

void
cov_ring(cov_t *c, double cx, double cy, double r, double width, int ink)
{
    int wsub = (int)rint(width * SS);
    if (wsub < 1)
        wsub = 1;
    batch_want(c, ink);
    pil_ellipse(&EM4, c, (int)((cx - r) * SS), (int)((cy - r) * SS),
                (int)((cx + r) * SS), (int)((cy + r) * SS), 0, wsub);
}

void
cov_arc(cov_t *c, double cx, double cy, double r, double a0, double a1,
        double width, int ink)
{
    int wsub = (int)rint(width * SS);
    if (wsub < 1)
        wsub = 1;
    batch_want(c, ink);
    pil_arc(&EM4, c, (int)((cx - r) * SS), (int)((cy - r) * SS),
            (int)((cx + r) * SS), (int)((cy + r) * SS), (float)a0, (float)a1,
            wsub);
}

void
cov_poly(cov_t *c, const double *xy, int npts, int ink)
{
    /* ImagingDrawPolygon(fill): truncate vertices, merge consecutive
     * colinear horizontal edges, auto-close, scan fill. */
    int ixy[2 * MAXEDGE];
    pedge_t e[MAXEDGE];
    int i, n = 0;
    if (npts < 2 || npts > MAXEDGE)
        return;
    for (i = 0; i < npts; i++) {
        ixy[2 * i] = (int)xy[2 * i];
        ixy[2 * i + 1] = (int)xy[2 * i + 1];
    }
    batch_want(c, ink);
    for (i = 0; i < npts - 1; i++) {
        int x0 = ixy[i * 2], y0 = ixy[i * 2 + 1];
        int x1 = ixy[i * 2 + 2], y1 = ixy[i * 2 + 3];
        if (y0 == y1 && i != 0 && y0 == ixy[i * 2 - 1]) {
            pedge_t *last_e = &e[n - 1];
            if (x1 > x0 && x0 > ixy[i * 2 - 2]) {
                last_e->xmax = x1;
                continue;
            } else if (x1 < x0 && x0 < ixy[i * 2 - 2]) {
                last_e->xmin = x1;
                continue;
            }
        }
        add_edge(&e[n++], x0, y0, x1, y1);
    }
    if (ixy[i * 2] != ixy[0] || ixy[i * 2 + 1] != ixy[1])
        add_edge(&e[n++], ixy[i * 2], ixy[i * 2 + 1], ixy[0], ixy[1]);
    polygon_generic(&EM4, c, n, e);
}

void
cov_stroke_segs(cov_t *c, const double *s, int nsegs, double width, int ink)
{
    /* mockup.py stroke(): thick polyline with round joins and caps. */
    double r = width / 2.0;
    int i;
    if (nsegs <= 0)
        return;
    for (i = 0; i < nsegs; i++)
        cov_stroke(c, s[4 * i], s[4 * i + 1], s[4 * i + 2], s[4 * i + 3],
                   width / 2.0, ink);
    cov_disc(c, s[0], s[1], r, ink);
    cov_disc(c, s[4 * (nsegs - 1) + 2], s[4 * (nsegs - 1) + 3], r, ink);
    for (i = 0; i + 1 < nsegs; i++) {
        double ax = s[4 * i], ay = s[4 * i + 1];
        double bx = s[4 * i + 2], by = s[4 * i + 3];
        double px = s[4 * i + 4], py = s[4 * i + 5];
        double ex = s[4 * i + 6], ey = s[4 * i + 7];
        double v1x, v1y, v2x, v2y, l1, l2, cosang;
        if (bx != px || by != py) { /* clipping split the line */
            cov_disc(c, bx, by, r, ink);
            cov_disc(c, px, py, r, ink);
            continue;
        }
        v1x = bx - ax;
        v1y = by - ay;
        v2x = ex - px;
        v2y = ey - py;
        l1 = hypot(v1x, v1y);
        l2 = hypot(v2x, v2y);
        if (l1 < 1e-6 || l2 < 1e-6)
            continue;
        cosang = (v1x * v2x + v1y * v2y) / (l1 * l2);
        if (cosang > 1.0)
            cosang = 1.0;
        else if (cosang < -1.0)
            cosang = -1.0;
        if (acos(cosang) * (180.0 / M_PI) > 15.0)
            cov_disc(c, bx, by, r, ink); /* only real joins */
    }
}

void
cov_polyline(cov_t *c, const double *xy, int npts, double width, int ink)
{
    double segs[4 * (MAXEDGE * 4)];
    int i, n = npts - 1;
    if (n <= 0)
        return;
    if (n > MAXEDGE * 4)
        n = MAXEDGE * 4;
    for (i = 0; i < n; i++) {
        segs[4 * i] = xy[2 * i];
        segs[4 * i + 1] = xy[2 * i + 1];
        segs[4 * i + 2] = xy[2 * i + 2];
        segs[4 * i + 3] = xy[2 * i + 3];
    }
    cov_stroke_segs(c, segs, n, width, ink);
}

void
cov_fill_rect(cov_t *c, double x0, double y0, double x1, double y1, int ink)
{
    /* mockup rect(): PIL rectangle [x0*SS, y0*SS, (x1+1)*SS-1, (y1+1)*SS-1],
     * coordinates truncated, fill inclusive. */
    int sx0 = (int)(x0 * SS), sy0 = (int)(y0 * SS);
    int sx1 = (int)((x1 + 1.0) * SS - 1.0), sy1 = (int)((y1 + 1.0) * SS - 1.0);
    int y;
    batch_want(c, ink);
    if (sy0 > sy1) {
        y = sy0;
        sy0 = sy1;
        sy1 = y;
    }
    if (sy0 < 0)
        sy0 = 0;
    else if (sy0 >= SH)
        return;
    if (sy1 < 0)
        return;
    else if (sy1 > SH)
        sy1 = SH;
    for (y = sy0; y <= sy1; y++)
        EM4.hline(c, sx0, y, sx1);
}

void
cov_rect_outline(cov_t *c, double x0, double y0, double x1, double y1,
                 double width, int ink)
{
    /* PIL rectangle outline: width rings drawn inward from the edge. */
    int sx0 = (int)(x0 * SS), sy0 = (int)(y0 * SS);
    int sx1 = (int)(x1 * SS), sy1 = (int)(y1 * SS);
    int w = (int)rint(width * SS), i, t;
    if (w < 1)
        w = 1;
    batch_want(c, ink);
    if (sy0 > sy1) {
        t = sy0;
        sy0 = sy1;
        sy1 = t;
    }
    for (i = 0; i < w; i++) {
        EM4.hline(c, sx0, sy0 + i, sx1);
        EM4.hline(c, sx0, sy1 - i, sx1);
        pil_line(&EM4, c, sx1 - i, sy0 + w, sx1 - i, sy1 - w + 1);
        pil_line(&EM4, c, sx0 + i, sy0 + w, sx0 + i, sy1 - w + 1);
    }
}

void
cov_hairline(cov_t *c, double x0, double y0, double x1, double y1, double w,
             int ink)
{
    /* mockup hairline(): endpoints Python-rounded (half-even), drawn 1x
     * aliased, then expanded to solid 4x4 blocks. */
    int wi = (int)rint(w);
    if (wi < 1)
        wi = 1;
    batch_want(c, ink);
    pil_user_line(&EMBLK, c, rint(x0), rint(y0), rint(x1), rint(y1), wi);
}

void
cov_text(cov_t *c, int x, int y, const char *s, int scale, int ink)
{
    /* mockup text(): each glyph pixel an exact scale x scale block. */
    for (; *s; s++) {
        const unsigned char *g = glyph(*s);
        int r, col;
        for (r = 0; r < GLYPH_H; r++)
            for (col = 0; col < GLYPH_W; col++)
                if (g[r] & (0x10 >> col))
                    cov_fill_rect(c, x + col * scale, y + r * scale,
                                  x + col * scale + scale - 1,
                                  y + r * scale + scale - 1, ink);
        x += CELL_W * scale;
    }
}

int
cov_text_w(const char *s, int scale)
{
    return text_w(s, scale);
}

void
cov_blit_bits(cov_t *c, int x, int y, int w, int h,
              const unsigned char *rows, int ink)
{
    int stride = (w + 7) / 8, ry, gx, run;
    for (ry = 0; ry < h; ry++) {
        run = -1;
        for (gx = 0; gx <= w; gx++) {
            int on = gx < w &&
                     (rows[ry * stride + gx / 8] & (0x80 >> (gx % 8)));
            if (on && run < 0)
                run = gx;
            else if (!on && run >= 0) {
                cov_fill_rect(c, x + run, y + ry, x + gx - 1, y + ry, ink);
                run = -1;
            }
        }
    }
}

void
cov_resolve(cov_t *c, canvas_t *out)
{
    int x, y;
    cov_commit(c);
    for (y = 0; y < SCR_H && y < out->h; y++) {
        unsigned char *row = &out->bits[(size_t)y * out->stride];
        memset(row, 0, (size_t)out->stride);
        for (x = 0; x < SCR_W && x < out->w; x++)
            if (c->v[y][x] < 128)
                row[x / 8] |= (unsigned char)(0x80u >> (x % 8));
    }
}

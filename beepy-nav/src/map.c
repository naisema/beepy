/* beepy-nav/src/map.c -- see map.h. */
#include <math.h>

#include "map.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

/* The two finest rungs MAGNIFY the pack's finest tiles rather than being cut
 * (DESIGN.md 6.1, 6.5): 0.75 is 1.5 m/px doubled and 0.375 is it quadrupled, so
 * one pack pixel becomes a 2x2 or 4x4 block. They exist because the scale bar
 * cannot say 50 M or 25 M above 1.0 and 0.5 m/px respectively -- the bar must
 * land in 50..100 px -- and a rider asking to see a junction wants those.
 *
 * MANUAL ONLY. map_auto_zoom() skips them: magnification adds no information, so
 * choosing it FOR a rider as a cue approaches would trade a sharp map for a
 * blocky one without being asked. Z and X reach them; A does not. */
const double MAP_ZOOMS[MAP_NZOOM] = {0.375, 0.75,
                                     1.5, 2.5, 4,  6,   10,  15,
                                     25,  40,  60, 100, 150, 250};
/* MAP_AUTO_FIRST lives in map.h, so the tests assert against the same number. */

void
map_project(const double *en, int n, double org_e, double org_n, double mpp,
            double cx, double cy, double theta, double *out_xy)
{
    double ct = cos(theta), st = sin(theta);
    int i;
    for (i = 0; i < n; i++) {
        double e = en[2 * i] - org_e, nn = en[2 * i + 1] - org_n;
        out_xy[2 * i] = cx + (e * ct - nn * st) / mpp;
        out_xy[2 * i + 1] = cy - (e * st + nn * ct) / mpp;
    }
}

int
map_round_corners(const double *en, int n, double radius, double min_deg,
                  double *out, int max_out)
{
    int i, m = 0;

#define PUSH(X, Y)                                                            \
    do {                                                                      \
        if (m >= max_out)                                                     \
            return m;                                                         \
        out[2 * m] = (X);                                                     \
        out[2 * m + 1] = (Y);                                                 \
        m++;                                                                  \
    } while (0)

    if (n < 3) {
        for (i = 0; i < n; i++)
            PUSH(en[2 * i], en[2 * i + 1]);
        return m;
    }
    PUSH(en[0], en[1]);
    for (i = 1; i < n - 1; i++) {
        double px = en[2 * i - 2], py = en[2 * i - 1];
        double ccx = en[2 * i], ccy = en[2 * i + 1];
        double nx = en[2 * i + 2], ny = en[2 * i + 3];
        double v1x = px - ccx, v1y = py - ccy;
        double v2x = nx - ccx, v2y = ny - ccy;
        double l1 = hypot(v1x, v1y), l2 = hypot(v2x, v2y);
        double cosang, turn, r, ax, ay, bx, by;
        int steps, s;

        if (l1 < 1e-6 || l2 < 1e-6)
            continue;
        cosang = (v1x * v2x + v1y * v2y) / (l1 * l2);
        if (cosang > 1.0)
            cosang = 1.0;
        else if (cosang < -1.0)
            cosang = -1.0;
        turn = 180.0 - acos(cosang) * (180.0 / M_PI);
        if (turn < min_deg) {
            PUSH(ccx, ccy);
            continue;
        }
        r = radius;
        if (l1 / 2.0 < r)
            r = l1 / 2.0;
        if (l2 / 2.0 < r)
            r = l2 / 2.0;
        ax = ccx + v1x / l1 * r;
        ay = ccy + v1y / l1 * r;
        bx = ccx + v2x / l2 * r;
        by = ccy + v2y / l2 * r;
        steps = (int)(turn / 10.0);
        if (steps < 3)
            steps = 3;
        for (s = 0; s <= steps; s++) {
            double t = (double)s / steps, u = 1.0 - t;
            PUSH(u * u * ax + 2 * u * t * ccx + t * t * bx,
                 u * u * ay + 2 * u * t * ccy + t * t * by);
        }
    }
    PUSH(en[2 * n - 2], en[2 * n - 1]);
    return m;
#undef PUSH
}

static int
outcode(double x, double y, double x0b, double y0b, double x1b, double y1b)
{
    return (x < x0b) | ((x > x1b) << 1) | ((y < y0b) << 2) | ((y > y1b) << 3);
}

int
map_clip_segs(const double *xy, int npts, double x0b, double y0b, double x1b,
              double y1b, double *out_segs, int max)
{
    int i, n = 0;
    for (i = 0; i + 1 < npts; i++) {
        double ax = xy[2 * i], ay = xy[2 * i + 1];
        double bx = xy[2 * i + 2], by = xy[2 * i + 3];
        int ca = outcode(ax, ay, x0b, y0b, x1b, y1b);
        int cb = outcode(bx, by, x0b, y0b, x1b, y1b);
        for (;;) {
            int cc;
            double x, y;
            if (!(ca | cb)) {
                if (n >= max)
                    return n;
                out_segs[4 * n] = ax;
                out_segs[4 * n + 1] = ay;
                out_segs[4 * n + 2] = bx;
                out_segs[4 * n + 3] = by;
                n++;
                break;
            }
            if (ca & cb)
                break;
            cc = ca ? ca : cb;
            if (cc & 8) {
                x = ax + (bx - ax) * (y1b - ay) / (by - ay);
                y = y1b;
            } else if (cc & 4) {
                x = ax + (bx - ax) * (y0b - ay) / (by - ay);
                y = y0b;
            } else if (cc & 2) {
                x = x1b;
                y = ay + (by - ay) * (x1b - ax) / (bx - ax);
            } else {
                x = x0b;
                y = ay + (by - ay) * (x0b - ax) / (bx - ax);
            }
            if (cc == ca) {
                ax = x;
                ay = y;
                ca = outcode(x, y, x0b, y0b, x1b, y1b);
            } else {
                bx = x;
                by = y;
                cb = outcode(x, y, x0b, y0b, x1b, y1b);
            }
        }
    }
    return n;
}

double
map_auto_zoom(double cue_dist_m, double ahead_px)
{
    int i;
    /* From the finest CUT rung, never from the magnified ones -- see MAP_ZOOMS. */
    for (i = MAP_AUTO_FIRST; i < MAP_NZOOM; i++)
        if (cue_dist_m / MAP_ZOOMS[i] <= 0.8 * ahead_px)
            return MAP_ZOOMS[i];
    return MAP_ZOOMS[MAP_NZOOM - 1];
}

double
map_zoom_step(double mpp, int dir)
{
    int i, best = 0;
    double bd = fabs(mpp - MAP_ZOOMS[0]);
    for (i = 1; i < MAP_NZOOM; i++) {
        double d = fabs(mpp - MAP_ZOOMS[i]);
        if (d < bd) {
            bd = d;
            best = i;
        }
    }
    if (dir > 0)
        best++;
    else if (dir < 0)
        best--;
    if (best < 0)
        best = 0;
    if (best >= MAP_NZOOM)
        best = MAP_NZOOM - 1;
    return MAP_ZOOMS[best];
}

/* Shared by the two scale ladders: first rung whose bar lands in 50..100 px,
 * falling back to the last one. (Python's for/else, which is what mockup.py
 * wrote and what the metric ladder has always done.) */
static void
scale_pick(const int *ladder, int n, double unit_per_px, int *out_v,
           int *out_px)
{
    int i, v = ladder[0];
    double px = v / unit_per_px;
    for (i = 0; i < n; i++) {
        v = ladder[i];
        px = v / unit_per_px;
        if (px >= 50.0 && px <= 100.0)
            break;
    }
    *out_v = v;
    *out_px = (int)px;
}

void
map_scale_pick(double mpp, int *out_m, int *out_px)
{
    static const int LADDER[] = {25,   50,   100,  200,   500,
                                 1000, 2000, 5000, 10000, 20000};
    scale_pick(LADDER, (int)(sizeof LADDER / sizeof LADDER[0]), mpp, out_m,
               out_px);
}

void
map_scale_pick_ft(double mpp, int *out_ft, int *out_px)
{
    /* 200 FT is here for DESIGN.md 6.1's magnified rungs. The bar must land in
     * 50..100 px, and at 0.75 m/px (2.46 ft/px) 100 FT is 41 px and 250 FT is
     * 102 -- so without a rung between them scale_pick() ran off the end of the
     * ladder and returned TEN MILES. The metric ladder needed nothing: it
     * already had 50 M for exactly this span. */
    static const int LADDER[] = {100,  200,   250,   500,   1000,  2640,
                                 5280, 10560, 26400, 52800, 105600};
    scale_pick(LADDER, (int)(sizeof LADDER / sizeof LADDER[0]),
               mpp * MAP_FT_PER_M, out_ft, out_px);
}

double
map_cue_distance(const double *en, int npts, double pe, double pn, int pos_i,
                 int turn_i)
{
    double total = 0.0, prev_e = pe, prev_n = pn;
    int i;
    for (i = pos_i + 1; i <= turn_i && i < npts; i++) {
        total += hypot(en[2 * i] - prev_e, en[2 * i + 1] - prev_n);
        prev_e = en[2 * i];
        prev_n = en[2 * i + 1];
    }
    return total;
}

/* beepy-nav/src/map.c -- see map.h. */
#include <math.h>

#include "map.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

const double MAP_ZOOMS[MAP_NZOOM] = {1.5, 2.5, 4,  6,   10,  15,
                                     25,  40,  60, 100, 150, 250};

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
    for (i = 0; i < MAP_NZOOM; i++)
        if (cue_dist_m / MAP_ZOOMS[i] <= 0.8 * ahead_px)
            return MAP_ZOOMS[i];
    return MAP_ZOOMS[MAP_NZOOM - 1];
}

void
map_scale_pick(double mpp, int *out_m, int *out_px)
{
    static const int LADDER[] = {25,   50,   100,  200,   500,
                                 1000, 2000, 5000, 10000, 20000};
    int i, m = LADDER[0];
    double px = m / mpp;
    /* Python's for/else: if nothing lands in the window the loop leaves the
     * last rung's values in place. */
    for (i = 0; i < (int)(sizeof LADDER / sizeof LADDER[0]); i++) {
        m = LADDER[i];
        px = m / mpp;
        if (px >= 50.0 && px <= 100.0)
            break;
    }
    *out_m = m;
    *out_px = (int)px;
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

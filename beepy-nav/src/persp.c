/* beepy-nav/src/persp.c -- see persp.h. */
#include <math.h>

#include "persp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void
persp_init(persp_t *c, double pitch_deg, double focal, double nadir_mpp,
           double heading_rad, double vw, double vh, double chevron_frac)
{
    double v_r, den, t_r;

    if (pitch_deg > 90.0) pitch_deg = 90.0;
    if (pitch_deg < 1.0) pitch_deg = 1.0;
    if (!(focal > 0.0)) focal = 1.0;
    if (!(nadir_mpp > 0.0)) nadir_mpp = 1.0;

    c->pitch = pitch_deg * (M_PI / 180.0);
    c->focal = focal;
    /* h/focal IS the metres per pixel at nadir. Deriving the height from the
     * scale rather than the other way round is what lets the 2D ladder's
     * m/px values (6.1) drive the 3D view unchanged. */
    c->h = nadir_mpp * focal;
    c->sp = sin(c->pitch);
    c->cp = cos(c->pitch);
    c->cx = vw / 2.0;
    c->cy = vh / 2.0;
    c->fwd_e = sin(heading_rad);
    c->fwd_n = cos(heading_rad);
    c->rgt_e = cos(heading_rad);
    c->rgt_n = -sin(heading_rad);

    /* Solve `back` so the rider lands at chevron_frac down the viewport for
     * WHATEVER the pitch is. Without this, tilting also zooms -- the rider
     * slides up the screen as the pitch drops -- and two pitches could not be
     * compared without confounding the two effects. */
    v_r = c->cy - chevron_frac * vh;
    den = c->focal * c->sp - v_r * c->cp;
    if (fabs(den) < 1e-12) {
        c->back = 0.0;
    } else {
        t_r = c->h / den;
        c->back = t_r * (v_r * c->sp + c->focal * c->cp);
    }

    /* At pitch 90 the ground plane and the image plane are parallel: every
     * pixel has a ground point and there is no horizon at all. Guarding on cp
     * rather than on the angle keeps the two branches consistent with the
     * arithmetic below, which divides by it. */
    if (c->cp > 1e-12) {
        c->has_horizon = 1;
        c->horizon_v = c->focal * c->sp / c->cp;
    } else {
        c->has_horizon = 0;
        c->horizon_v = 0.0;
    }
}

void
persp_to_ground(const persp_t *c, double de, double dn, double *fwd,
                double *lat)
{
    if (fwd) *fwd = de * c->fwd_e + dn * c->fwd_n;
    if (lat) *lat = de * c->rgt_e + dn * c->rgt_n;
}

int
persp_ground(const persp_t *c, double x, double y, double *fwd, double *lat)
{
    double u = x - c->cx;
    double v = c->cy - y;
    double den = c->focal * c->sp - v * c->cp;
    double t;

    /* den <= 0 is the ray at or above the horizon: it never meets the ground,
     * or meets it behind the camera. Returning 0 rather than a clamped point
     * matters -- a clamp would draw the sky as if it were tarmac. */
    if (!(den > 1e-12))
        return 0;
    t = c->h / den;
    if (fwd) *fwd = -c->back + t * (v * c->sp + c->focal * c->cp);
    if (lat) *lat = t * u;
    return 1;
}

int
persp_screen(const persp_t *c, double fwd, double lat, double *x, double *y,
             double *t)
{
    double Y = fwd + c->back;         /* forward distance from the CAMERA */
    double den = -(Y * c->cp + c->h * c->sp);
    double v, tt;

    if (fabs(den) < 1e-12)
        return 0;
    v = (c->h * c->focal * c->cp - Y * c->focal * c->sp) / den;
    /* Beyond the horizon there is no ground, and the algebra above would
     * happily return a point above it with a negative t. */
    if (c->has_horizon && v >= c->horizon_v - 1e-9)
        return 0;
    tt = c->focal * c->sp - v * c->cp;
    if (!(tt > 1e-12))
        return 0;
    tt = c->h / tt;
    if (!(tt > 0.0))
        return 0;
    if (x) *x = c->cx + lat / tt;
    if (y) *y = c->cy - v;
    if (t) *t = tt;
    return 1;
}

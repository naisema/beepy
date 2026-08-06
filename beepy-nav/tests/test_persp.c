/* beepy-nav/tests/test_persp.c -- the tilted camera.
 *
 * The assertion that earns its keep is t_pitch90_is_2d: if pitch 90 is not
 * bit-for-bit the top-down affine, then 2D and 3D are two projections wearing
 * one name, and every later claim about "one code path with one knob" is false.
 * Everything else here is the arithmetic that claim needs to be safe: an inverse
 * that really inverts, a horizon that refuses rather than clamps, and a chevron
 * that does not slide up the screen when the pitch changes.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "persp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define VW 270.0
#define VH 240.0

static int fails;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

static void
checkf(double got, double want, double tol, const char *what)
{
    if (!(fabs(got - want) <= tol)) {
        printf("FAIL %s: got %.9f want %.9f (tol %g)\n", what, got, want, tol);
        fails++;
    }
}

/* THE INVARIANT. At pitch 90 the projection must be the plain top-down affine
 * at `mpp` metres per pixel: forward d metres is d/mpp pixels UP, right d metres
 * is d/mpp pixels RIGHT, and nothing else happens. No perspective term may
 * survive -- in particular the scale must not vary with distance, which is the
 * one thing that would make a 3D renderer disagree with the 2D map it claims to
 * generalise. Tested at 1e-9, which is far tighter than a pixel: a genuine
 * perspective leak shows up in the third decimal, not the ninth. */
static void
t_pitch90_is_2d(void)
{
    persp_t c;
    const double mpp = 4.0, focal = 120.0;
    double x, y, t, x2, y2, t2;
    int i;

    persp_init(&c, 90.0, focal, mpp, 0.0, VW, VH, 0.72);
    check(!c.has_horizon, "at pitch 90 there is no horizon");

    check(persp_screen(&c, 0.0, 0.0, &x, &y, &t), "the rider projects");
    checkf(y, 0.72 * VH, 1e-9, "the rider sits at chevron_frac");
    checkf(x, VW / 2.0, 1e-9, "and on the centre line");
    checkf(t, mpp, 1e-12, "t is exactly the metres per pixel");

    check(persp_screen(&c, 100.0, 0.0, &x2, &y2, &t2), "100 m ahead projects");
    checkf(y - y2, 100.0 / mpp, 1e-9, "100 m ahead is 100/mpp px up");
    checkf(x2, x, 1e-9, "and does not move sideways");
    checkf(t2, mpp, 1e-12, "and the scale has NOT changed with distance");

    check(persp_screen(&c, 0.0, 100.0, &x2, &y2, &t2), "100 m right projects");
    checkf(x2 - x, 100.0 / mpp, 1e-9, "100 m right is 100/mpp px right");
    checkf(y2, y, 1e-9, "and does not move vertically");

    /* Linearity across the whole viewport: a perspective term would show as a
     * scale that drifts with distance. Ten samples, one tolerance. */
    for (i = 1; i <= 10; i++) {
        double d = i * 90.0;
        check(persp_screen(&c, d, 0.0, &x2, &y2, &t2), "a far point projects");
        checkf(y - y2, d / mpp, 1e-9, "the forward scale stays affine");
        checkf(t2, mpp, 1e-12, "and t stays constant everywhere");
    }
}

/* The inverse must actually invert, at a tilt where the perspective term is
 * doing real work. Round-tripping through the ground catches a sign error or a
 * dropped `back` that a one-way test would not. */
static void
t_round_trip(void)
{
    persp_t c;
    double xs[] = { 10.0, 135.0, 260.0 };
    double ys[] = { 40.0, 120.0, 200.0, 239.0 };
    int i, j;

    persp_init(&c, 70.0, 120.0, 4.0, 0.0, VW, VH, 0.72);
    check(c.has_horizon, "at pitch 70 there is a horizon");

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 4; j++) {
            double fwd, lat, x2, y2, t;
            if (!persp_ground(&c, xs[i], ys[j], &fwd, &lat)) {
                check(0, "a pixel inside the viewport has a ground point");
                continue;
            }
            check(persp_screen(&c, fwd, lat, &x2, &y2, &t),
                  "and that ground point projects back");
            checkf(x2, xs[i], 1e-6, "screen -> ground -> screen keeps x");
            checkf(y2, ys[j], 1e-6, "screen -> ground -> screen keeps y");
            check(t > 0.0, "and t is positive");
        }
    }
}

/* A tilted view must SHOW perspective: nearer ground has to be coarser in
 * metres per pixel than farther ground, and by a lot. This is the pair to
 * t_pitch90_is_2d -- together they say the knob does something and that one end
 * of it is exactly 2D. */
static void
t_tilt_compresses(void)
{
    persp_t c;
    double near_t, far_t, fwd_n, fwd_f, lat;

    persp_init(&c, 70.0, 120.0, 4.0, 0.0, VW, VH, 0.72);
    check(persp_ground(&c, VW / 2, VH - 1, &fwd_n, &lat), "the bottom row hits ground");
    check(persp_ground(&c, VW / 2, 0.0, &fwd_f, &lat), "the top row hits ground");
    check(fwd_f > fwd_n, "the top of the screen is farther away than the bottom");

    persp_screen(&c, fwd_n, 0.0, NULL, NULL, &near_t);
    persp_screen(&c, fwd_f, 0.0, NULL, NULL, &far_t);
    check(far_t > near_t * 2.0,
          "and a pixel up there covers more than twice the ground");
}

/* The horizon refuses. A ray at or above it never meets the ground ahead, and
 * the caller must be told so rather than handed a clamped point -- drawing the
 * sky as tarmac is exactly the "smeared horizon" failure the prototypes had. */
static void
t_horizon_refuses(void)
{
    persp_t c;
    double fwd, lat, x, y, t;
    double y_h;

    persp_init(&c, 50.0, 120.0, 4.0, 0.0, VW, VH, 0.72);
    check(c.has_horizon, "pitch 50 has a horizon");
    y_h = c.cy - c.horizon_v;

    /* Just below the horizon: ground exists. Just above: it must not. */
    if (y_h + 2.0 < VH) {
        check(persp_ground(&c, VW / 2, y_h + 2.0, &fwd, &lat),
              "two pixels below the horizon there is ground");
    }
    check(!persp_ground(&c, VW / 2, y_h, &fwd, &lat),
          "at the horizon there is none");
    check(!persp_ground(&c, VW / 2, y_h - 1.0, &fwd, &lat),
          "and above it there is none");

    /* From the other direction, and NOT what a first draft of this test
     * asserted. A point a thousand km ahead is NOT refused, and should not be:
     * on an infinite flat plane every finite distance lies strictly below the
     * horizon, so projecting it is the correct answer. Measured here it lands
     * 1.4e-4 px under the horizon line. What makes it harmless is not the
     * camera but `t` -- the scale there is astronomical, so a 14 m motorway is
     * far thinner than a pixel and the width cull drops it. Deciding what is
     * too far away is the renderer's job; the camera's job is to be right.
     *
     * The distinction matters because a camera that refused far points would
     * also refuse legitimately visible ones near the horizon at low pitch. */
    check(persp_screen(&c, 1.0e9, 0.0, &x, &y, &t),
          "a point a thousand km ahead still projects -- it is finite");
    check(y > c.cy - c.horizon_v - 1.0 && y < c.cy - c.horizon_v + 1.0,
          "and lands within a pixel of the horizon line");
    check(t > 1.0e6, "with a scale that makes any road sub-pixel");
    /* A point behind the camera likewise. `back` is where the camera is, so
     * anything more than that behind the rider is behind the lens. */
    check(!persp_screen(&c, -(c.back + 1.0e6), 0.0, &x, &y, &t),
          "and so is a point behind the camera");
}

/* Changing the pitch must not move the rider, or "compare these tilts" would
 * be comparing tilts AND zooms. This is why persp_init solves for `back`. */
static void
t_chevron_is_stable(void)
{
    double pitches[] = { 90.0, 75.0, 70.0, 60.0, 50.0, 35.0 };
    int i;
    for (i = 0; i < 6; i++) {
        persp_t c;
        double x, y, t;
        persp_init(&c, pitches[i], 120.0, 4.0, 0.0, VW, VH, 0.72);
        check(persp_screen(&c, 0.0, 0.0, &x, &y, &t), "the rider projects");
        checkf(y, 0.72 * VH, 1e-6, "the rider stays put across pitches");
        checkf(x, VW / 2.0, 1e-6, "and stays centred");
    }
}

/* Heading rotates the ground and nothing else. At heading 90 (due east) a point
 * to the east must be AHEAD, not to the right. */
static void
t_heading(void)
{
    persp_t c;
    double fwd, lat;

    persp_init(&c, 70.0, 120.0, 4.0, M_PI / 2.0, VW, VH, 0.72);
    persp_to_ground(&c, 100.0, 0.0, &fwd, &lat);   /* 100 m east */
    checkf(fwd, 100.0, 1e-9, "riding east, a point east is ahead");
    checkf(lat, 0.0, 1e-9, "and not to the side");
    persp_to_ground(&c, 0.0, 100.0, &fwd, &lat);   /* 100 m north */
    checkf(fwd, 0.0, 1e-9, "a point north is not ahead");
    checkf(lat, -100.0, 1e-9, "it is 100 m to the LEFT");
}

/* Nonsense in, something usable out: these are clamped rather than rejected
 * because the caller is a draw loop with a config value in its hand, and a
 * camera that returns nothing draws a blank page with no explanation. */
static void
t_clamps(void)
{
    persp_t c;
    persp_init(&c, 140.0, 120.0, 4.0, 0.0, VW, VH, 0.72);
    checkf(c.pitch, M_PI / 2.0, 1e-12, "a pitch over 90 clamps to 90");
    persp_init(&c, -10.0, 120.0, 4.0, 0.0, VW, VH, 0.72);
    check(c.pitch > 0.0, "a negative pitch clamps to something positive");
    persp_init(&c, 70.0, 0.0, 4.0, 0.0, VW, VH, 0.72);
    check(c.focal > 0.0, "a zero focal clamps");
    persp_init(&c, 70.0, 120.0, 0.0, 0.0, VW, VH, 0.72);
    check(c.h > 0.0, "a zero scale clamps");
}

int
main(void)
{
    t_pitch90_is_2d();
    t_round_trip();
    t_tilt_compresses();
    t_horizon_refuses();
    t_chevron_is_stable();
    t_heading();
    t_clamps();
    if (fails) {
        printf("test_persp: %d FAILURES\n", fails);
        return 1;
    }
    printf("test_persp: OK\n");
    return 0;
}

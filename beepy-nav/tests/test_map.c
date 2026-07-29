/* beepy-nav/tests/test_map.c -- host unit tests for the map maths.
 *
 * map.c is the one beepy-nav module with no pixels in it, so it is the one
 * that can be checked by assertion rather than by frame comparison. The
 * expected values are computed here from first principles (or read off
 * mockup.py's own arithmetic), never from a previous run of map.c.
 *
 *     make test-unit
 */
#include <math.h>
#include <stdio.h>

#include "map.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

static int failures;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void
close_to(double got, double want, double tol, const char *what)
{
    if (!(fabs(got - want) <= tol)) {
        printf("FAIL %s: got %.9g, want %.9g\n", what, got, want);
        failures++;
    }
}

/* ------------------------------------------------------------- projection */

static void
test_project_identity(void)
{
    /* theta = 0: east is +x, north is -y, scaled by 1/mpp about (cx, cy). */
    const double en[] = {40, -150, 40, 260, 190, 260};
    double xy[6];
    map_project(en, 3, 40, -150, 4.0, 265, 172.8, 0.0, xy);
    close_to(xy[0], 265.0, 1e-9, "project origin x");
    close_to(xy[1], 172.8, 1e-9, "project origin y");
    close_to(xy[2], 265.0, 1e-9, "project 410m north x");
    close_to(xy[3], 172.8 - 410.0 / 4.0, 1e-9, "project 410m north y");
    close_to(xy[4], 265.0 + 150.0 / 4.0, 1e-9, "project 150m east x");
    close_to(xy[5], 172.8 - 410.0 / 4.0, 1e-9, "project 150m east y");
}

static void
test_project_roundtrip(void)
{
    /* Rotating by theta and unprojecting by -theta must return the world
     * point, at every theta -- the sign convention that page_nav depends on
     * (course-up passes theta = +heading). */
    static const double en[] = {123.5, -47.25, -900.0, 610.0, 0.0, 0.0};
    const double mpp = 6.0, cx = 265.0, cy = 172.8;
    int k, i;
    for (k = 0; k < 8; k++) {
        double th = k * M_PI / 4.0 + 0.13;
        double xy[6], back[6];
        map_project(en, 3, 12.0, -5.0, mpp, cx, cy, th, xy);
        for (i = 0; i < 3; i++) {
            /* inverse of project(): undo the centre, scale and rotation */
            double dx = (xy[2 * i] - cx) * mpp;
            double dy = -(xy[2 * i + 1] - cy) * mpp;
            back[2 * i] = 12.0 + dx * cos(th) + dy * sin(th);
            back[2 * i + 1] = -5.0 - dx * sin(th) + dy * cos(th);
        }
        for (i = 0; i < 6; i++)
            close_to(back[i], en[i], 1e-9, "project round trip");
    }
    /* A point due north of the origin sits straight up the screen on a
     * course-up map, whatever the heading. */
    for (k = 0; k < 8; k++) {
        double th = k * M_PI / 4.0;
        double ahead[2], xy[2];
        ahead[0] = 100.0 * sin(th); /* 100 m along the heading */
        ahead[1] = 100.0 * cos(th);
        map_project(ahead, 1, 0, 0, 5.0, cx, cy, th, xy);
        close_to(xy[0], cx, 1e-9, "course-up keeps the cue on the centreline");
        close_to(xy[1], cy - 20.0, 1e-9, "course-up puts the cue ahead");
    }
}

/* --------------------------------------------------------- corner rounding */

static void
test_round_corners(void)
{
    /* A 9.086 degree bend is under the 12 degree floor, so the vertex
     * survives untouched -- this is mockup.py's own ridden-track case. */
    static const double gentle[] = {0, -900, 0, -400, 40, -150};
    /* A right angle is well over it, and gets an arc. */
    static const double square[] = {0, 0, 0, 400, 400, 400};
    double out[256];
    int n, i;
    double r;

    n = map_round_corners(gentle, 3, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                          out, 128);
    check(n == 3, "gentle bend keeps its vertex");
    close_to(out[2], 0.0, 1e-9, "gentle bend vertex e");
    close_to(out[3], -400.0, 1e-9, "gentle bend vertex n");

    n = map_round_corners(square, 3, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                          out, 128);
    /* 90 degrees -> steps = 9 -> 10 arc points, plus both endpoints. */
    check(n == 12, "right angle is replaced by a 10-point arc");
    close_to(out[0], 0.0, 1e-9, "arc keeps the first point");
    close_to(out[2 * n - 2], 400.0, 1e-9, "arc keeps the last point e");
    close_to(out[2 * n - 1], 400.0, 1e-9, "arc keeps the last point n");
    /* The arc starts and ends 25 m back from the corner along each leg. */
    close_to(out[2], 0.0, 1e-9, "arc entry e");
    close_to(out[3], 400.0 - MAP_CORNER_RADIUS, 1e-9, "arc entry n");
    close_to(out[2 * n - 4], MAP_CORNER_RADIUS, 1e-9, "arc exit e");
    close_to(out[2 * n - 3], 400.0, 1e-9, "arc exit n");
    /* Every arc point stays inside the corner: a quadratic Bezier lies in
     * the convex hull of its three control points. */
    for (i = 1; i < n - 1; i++)
        check(out[2 * i] >= -1e-9 && out[2 * i] <= MAP_CORNER_RADIUS + 1e-9 &&
                  out[2 * i + 1] >= 400.0 - MAP_CORNER_RADIUS - 1e-9 &&
                  out[2 * i + 1] <= 400.0 + 1e-9,
              "arc point inside the corner hull");

    /* Radius clamps to half the shorter adjacent segment. */
    {
        static const double tight[] = {0, 0, 0, 20, 400, 20};
        n = map_round_corners(tight, 3, MAP_CORNER_RADIUS,
                              MAP_CORNER_MIN_DEG, out, 128);
        r = 20.0 / 2.0;
        close_to(out[3], 20.0 - r, 1e-9, "clamped arc entry n");
        close_to(out[2 * n - 4], r, 1e-9, "clamped arc exit e");
    }

    /* Degenerate: a repeated vertex is dropped, not divided by zero. */
    {
        static const double dup[] = {0, -900, 0, -400, 40, -150, 40, -150};
        n = map_round_corners(dup, 4, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                              out, 128);
        check(n == 3, "repeated vertex collapses");
        close_to(out[4], 40.0, 1e-9, "collapsed tail e");
    }

    /* Fewer than three points passes straight through. */
    n = map_round_corners(gentle, 2, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                          out, 128);
    check(n == 2, "two points pass through");

    /* max_out is honoured. */
    n = map_round_corners(square, 3, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                          out, 5);
    check(n == 5, "max_out caps the output");
}

/* ---------------------------------------------------------------- clipping */

static void
test_clip(void)
{
    /* mockup.py's own case: the ridden track leaves the bottom of the map. */
    static const double xy[] = {255, 360.3, 255, 235.3, 265, 172.8};
    double segs[64];
    int n = map_clip_segs(xy, 3, 130, 0, 399, 239, segs, 16);
    check(n == 2, "one segment clipped at the bottom edge, one whole");
    close_to(segs[0], 255.0, 1e-9, "clipped seg enters at x");
    close_to(segs[1], 239.0, 1e-9, "clipped seg enters at the box edge");
    close_to(segs[2], 255.0, 1e-9, "clipped seg ends at x");
    close_to(segs[3], 235.3, 1e-9, "clipped seg ends inside");
    close_to(segs[4], 255.0, 1e-9, "second seg starts where the first ended");
    close_to(segs[5], 235.3, 1e-9, "second seg y");

    /* Wholly outside on the same side: rejected without an intersection. */
    {
        static const double out[] = {0, 0, 100, 50};
        n = map_clip_segs(out, 2, 130, 0, 399, 239, segs, 16);
        check(n == 0, "segment left of the panel is dropped");
    }
    /* Crossing the whole box: two intersections, one segment. */
    {
        static const double thru[] = {0, 120, 399, 120};
        n = map_clip_segs(thru, 2, 130, 0, 399, 239, segs, 16);
        check(n == 1, "crossing segment survives once");
        close_to(segs[0], 130.0, 1e-9, "entry clipped to the left edge");
        close_to(segs[2], 399.0, 1e-9, "exit at the right edge");
    }
    /* A polyline that leaves and re-enters yields two segments whose ends
     * do not meet -- which is what tells cov_stroke_segs to re-cap. */
    {
        static const double zig[] = {200, 100, 200, -50, 300, -50, 300, 100};
        n = map_clip_segs(zig, 4, 130, 0, 399, 239, segs, 16);
        check(n == 2, "excursion above the box splits the line");
        check(segs[2] != segs[4] || segs[3] != segs[5],
              "split is visible as a gap between segments");
    }
}

/* ------------------------------------------------------- zoom + scale bar */

static void
test_auto_zoom(void)
{
    const double ahead = 172.8; /* H * 0.72, the NAV page's fix line */
    /* mockup.py's NAV page: 410 m of cue distance -> 4 m/px. */
    close_to(map_auto_zoom(410.0, ahead), 4.0, 1e-12, "nav page zoom");
    /* Exactly on a rung boundary the rung is accepted (<=, not <). */
    close_to(map_auto_zoom(0.8 * ahead * 1.5, ahead), 1.5, 1e-12,
             "boundary takes the finer rung");
    close_to(map_auto_zoom(0.8 * ahead * 1.5 + 1e-6, ahead), 2.5, 1e-12,
             "just past the boundary steps up");
    close_to(map_auto_zoom(0.0, ahead), MAP_ZOOMS[0], 1e-12,
             "zero distance takes the finest rung");
    close_to(map_auto_zoom(1e9, ahead), MAP_ZOOMS[MAP_NZOOM - 1], 1e-12,
             "beyond the ladder saturates");
    /* Monotone: more distance never zooms in. */
    {
        double d, prev = 0.0;
        for (d = 0.0; d < 60000.0; d += 37.0) {
            double z = map_auto_zoom(d, ahead);
            check(z >= prev - 1e-12, "auto zoom is monotone");
            prev = z;
        }
    }
}

static void
test_scale_pick(void)
{
    int m, px;
    map_scale_pick(4.0, &m, &px); /* the NAV page */
    check(m == 200 && px == 50, "scale bar at 4 m/px is 200 M / 50 px");
    map_scale_pick(1.5, &m, &px);
    check(m == 100 && px == 66, "scale bar at 1.5 m/px is 100 M / 66 px");
    map_scale_pick(250.0, &m, &px);
    check(m == 20000 && px == 80, "scale bar at 250 m/px is 20 KM / 80 px");
    /* Every ladder rung produces a bar in the window. */
    {
        int i;
        for (i = 0; i < MAP_NZOOM; i++) {
            map_scale_pick(MAP_ZOOMS[i], &m, &px);
            check(px >= 50 && px <= 100, "scale bar length stays in 50..100");
        }
    }
}

static void
test_cue_distance(void)
{
    static const double route[] = {0, -900, 0, -400, 40, -150, 40, 260};
    close_to(map_cue_distance(route, 4, 40, -150, 2, 3), 410.0, 1e-9,
             "cue distance from the fix to the bend");
    close_to(map_cue_distance(route, 4, 40, -150, 2, 2), 0.0, 1e-9,
             "cue distance to the segment start is zero");
}

int
main(void)
{
    test_project_identity();
    test_project_roundtrip();
    test_round_corners();
    test_clip();
    test_auto_zoom();
    test_scale_pick();
    test_cue_distance();
    if (failures) {
        printf("test_map: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_map: PASS\n");
    return 0;
}

/* beepy-nav/tests/test_route.c -- snapping, cue derivation, progress, latch.
 *
 * DESIGN.md 7.2-7.4. Every expectation here is computed from the geometry
 * the test builds, or read off the design's own thresholds -- never off a
 * previous run of route.c.
 *
 * The geometry is synthetic on purpose: a corner with a KNOWN angle is the
 * only way to check that 7.4's bands are being applied to the angle the
 * design means, and a real GPX cannot supply one.
 *
 *     make test-unit
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpx.h"
#include "route.h"

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
eq_int(long got, long want, const char *what)
{
    if (got != want) {
        printf("FAIL %s: got %ld, want %ld\n", what, got, want);
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

/* ------------------------------------------------------- geometry helpers */

#define LAT0 13.7375
#define LON0 100.561

/* Build a route from metre offsets, so the tests can talk in metres and the
 * code under test still sees the lat/lon it will see in the field. */
static void
build(route_t *r, const double *en, int n)
{
    static pt_t buf[40000];
    double mlat = 1.0 / GEO_M_PER_DEG_LAT;
    double mlon = 1.0 / (GEO_M_PER_DEG_LON * cos(LAT0 * M_PI / 180.0));
    int i;
    route_init(r);
    for (i = 0; i < n; i++) {
        buf[i].lat = LAT0 + en[2 * i + 1] * mlat;
        buf[i].lon = LON0 + en[2 * i] * mlon;
        buf[i].ele = 0;
    }
    r->pt = buf;
    r->npt = n;
    check(route_prepare(r) == 0, "build: prepare");
}

/* build() lends a static array, so only the derived parts may be released. */
static void
teardown(route_t *r)
{
    r->pt = NULL;
    route_free(r);
}

/* A straight line of `n` points every `step` metres along bearing `brg`. */
static int
line(double *en, int at, double e0, double n0, double brg_deg, double step,
     int n)
{
    double b = brg_deg * M_PI / 180.0;
    int i;
    for (i = 0; i < n; i++) {
        en[2 * (at + i)] = e0 + sin(b) * step * i;
        en[2 * (at + i) + 1] = n0 + cos(b) * step * i;
    }
    return at + n;
}

/* ---------------------------------------------------------------- prepare */

static void
test_prepare(void)
{
    /* 100 m east, then 100 m north: a right-angle L of known length. */
    static const double en[] = {0, 0, 100, 0, 100, 100};
    route_t r;
    build(&r, en, 3);
    close_to(r.cum[0], 0.0, 1e-9, "cum[0] is zero");
    close_to(r.cum[1], 100.0, 0.05, "cum[1] = 100 m");
    close_to(r.cum[2], 200.0, 0.05, "cum[2] = 200 m");
    close_to(r.total_m, 200.0, 0.05, "total = 200 m");
    close_to(r.min_e, 0.0, 0.05, "bbox min e");
    close_to(r.max_e, 100.0, 0.05, "bbox max e");
    close_to(r.min_n, 0.0, 0.05, "bbox min n");
    close_to(r.max_n, 100.0, 0.05, "bbox max n");
    teardown(&r);
}

/* route_rebase(): the same route, on somebody else's tangent origin.
 *
 * This is what lets a reroute replace the live route without moving the world
 * frame every dead-reckoning coordinate is already in (7.11), so what has to be
 * true is that the route is the SAME route afterwards. Checked three ways: the
 * distances survive, the cue list still points at the same junctions with
 * distances that agree with the new cum[], and a snap gives the same answer in
 * the new frame that it gave in the old one.
 *
 * The origin used here is 5 km away, which is further than a reroute moves the
 * reference in practice -- the new route starts where the rider is, and the
 * frame stays where the old route started. */
static void
test_rebase(void)
{
    /* 2 km north with a right-angle turn 1 km along, so there is a real cue to
     * carry across rather than only a destination. */
    static double en[2 * 201];
    route_t r;
    navctx_t ctx;
    nav_t nv, nv2;
    double total0, along0, ref_lat, ref_lon;
    int n, i, ncue0, idx0;

    /* The corner is index 100; the second leg starts one step PAST it, as
     * test_cues_corner() does -- restarting it on the corner itself would
     * repeat a point and leave a zero-length segment, which is a fixture bug
     * that "cum[] is still increasing" below caught on the first run. */
    n = line(en, 0, 0, 0, 0, 10, 101);         /* 1 km north     */
    n = line(en, n, 10, 1000, 90, 10, 100);    /* then 1 km east */
    build(&r, en, n);
    route_cues_derive(&r);
    route_cues_finish(&r);
    total0 = r.total_m;
    ncue0 = r.ncue;
    check(ncue0 >= 2, "rebase: the corner and the destination are both cues");
    idx0 = r.cue[0].idx;
    along0 = r.cue[0].along_m;

    nav_init(&ctx);
    nav_reset(&nv);
    route_snap(&r, &ctx, 5.0, 500.0, 0, &nv); /* 5 m off, 500 m along */

    /* 5 km south-west of the route's own start, and NOT the route's first point:
     * the whole claim is that the origin is arbitrary. */
    ref_lat = r.pt[0].lat - 5000.0 / GEO_M_PER_DEG_LAT;
    ref_lon = r.pt[0].lon - 5000.0 / GEO_M_PER_DEG_LON;
    check(route_rebase(&r, ref_lat, ref_lon) == 0, "rebase: succeeds");
    check(r.lat0 == ref_lat && r.lon0 == ref_lon,
          "rebase: the route now says which origin it is on");
    check(fabs(r.en[0]) > 4000.0 && fabs(r.en[1]) > 4000.0,
          "rebase: en[0] is no longer the origin, which is the point");

    /* 0.1% of 2 km is 2 m, and 6.1 quotes 0.1% at 50 km -- so this bound is
     * two orders of magnitude looser than the geometry needs and would still
     * catch a projection that had been dropped or applied twice. */
    close_to(r.total_m, total0, 0.5, "rebase: the route is the same length");
    for (i = 1; i < r.npt; i++)
        if (!(r.cum[i] > r.cum[i - 1])) {
            check(0, "rebase: cum[] is still increasing");
            break;
        }
    eq_int(r.ncue, ncue0, "rebase: the cue list survives");
    eq_int(r.cue[0].idx, idx0, "rebase: on the same vertex");
    close_to(r.cue[0].along_m, along0, 0.5, "rebase: at the same distance");
    /* The assertion that would have caught keeping the OLD along_m: the cue's
     * distance must agree with the cum[] the route now carries, not with the one
     * it was measured on. */
    close_to(r.cue[0].along_m, r.cum[r.cue[0].idx], 1e-9,
             "rebase: and it agrees with the new cum[]");

    /* The same rider, in the new frame: 5 m off, 500 m along, expressed as
     * metres from the new origin. Same segment, same distances. */
    nav_init(&ctx);
    nav_reset(&nv2);
    route_snap(&r, &ctx, r.en[0] + 5.0, r.en[1] + 500.0, 0, &nv2);
    eq_int(nv2.seg, nv.seg, "rebase: a snap lands on the same segment");
    close_to(nv2.along, nv.along, 0.5, "rebase: at the same distance along");
    close_to(nv2.off_m, nv.off_m, 0.05, "rebase: and the same distance off");
    teardown(&r);
}

/* ------------------------------------------------------------------- snap */

static void
test_snap_basic(void)
{
    /* 1 km due north, a point every 10 m: 101 points. */
    static double en[2 * 101];
    route_t r;
    navctx_t ctx;
    nav_t nv;
    int n = line(en, 0, 0, 0, 0, 10, 101);
    build(&r, en, n);
    nav_init(&ctx);
    nav_reset(&nv);

    /* 30 m off to the east, 255 m along. Mid-segment on purpose: a point
     * abeam a VERTEX is equidistant from the two segments meeting there and
     * either answer is right, which makes it a poor thing to assert. */
    route_snap(&r, &ctx, 30.0, 255.0, 1000, &nv);
    eq_int(nv.seg, 25, "snap segment");
    close_to(nv.off_m, 30.0, 0.05, "snap cross-track");
    close_to(nv.along, 255.0, 0.1, "snap along");
    close_to(nv.snap_e, 0.0, 0.05, "snap foot e");
    close_to(nv.snap_n, 255.0, 0.1, "snap foot n");

    /* Off the end: clamped to the last vertex, never extrapolated. */
    route_snap(&r, &ctx, 0.0, 1200.0, 1001, &nv);
    eq_int(nv.seg, 99, "snap past the end clamps to the last segment");
    close_to(nv.along, 1000.0, 0.2, "snap past the end clamps along");
    close_to(nv.off_m, 200.0, 0.5, "snap past the end measures the gap");
    teardown(&r);
}

/* DESIGN.md 7.2's window: +/-100 points, widening on the first fix and after
 * 30 s lost. Both branches must be taken, and the widened one must find a
 * match the narrow one provably cannot. */
static void
test_snap_window(void)
{
    /* 6 km north at 10 m: 601 points, so a 100-point window is 1 km and a
     * jump of 3 km is far outside it. */
    static double en[2 * 601];
    route_t r;
    navctx_t ctx;
    nav_t nv;
    int n = line(en, 0, 0, 0, 0, 10, 601);
    build(&r, en, n);
    nav_init(&ctx);
    nav_reset(&nv);

    route_snap(&r, &ctx, 0.0, 105.0, 1000, &nv);
    eq_int(nv.seg, 10, "window: first fix");
    eq_int(ctx.full_scans, 1, "window: the first fix is a full scan");

    /* One second later, teleport 3 km up the route. Inside the window the
     * nearest reachable segment is the far edge of it -- point 110 -- and
     * that is what a correctly WINDOWED search must return. */
    route_snap(&r, &ctx, 0.0, 3105.0, 1001, &nv);
    eq_int(nv.seg, 110, "window: a jump outside the window is clamped to it");
    eq_int(ctx.full_scans, 1, "window: no widening while fixes keep arriving");

    /* Now lose the fix for longer than 30 s and try again: the window widens
     * to the whole route and finds the true nearest segment. */
    route_snap(&r, &ctx, 0.0, 3105.0, 1001 + ROUTE_LOST_S + 1, &nv);
    eq_int(nv.seg, 310, "window: widens after 30 s lost");
    eq_int(ctx.full_scans, 2, "window: the widening was a full scan");
    close_to(nv.along, 3105.0, 0.5, "window: widened along");
    teardown(&r);
}

/* --------------------------------------------------------------- classify */

static void
test_classify(void)
{
    /* 7.4's bands, at both signs and on both sides of every boundary. */
    eq_int(route_classify(0.0), CUE_STRAIGHT, "classify 0");
    eq_int(route_classify(29.9), CUE_STRAIGHT, "classify 29.9");
    eq_int(route_classify(-29.9), CUE_STRAIGHT, "classify -29.9");
    eq_int(route_classify(30.1), CUE_SLIGHT_RIGHT, "classify 30.1");
    eq_int(route_classify(-30.1), CUE_SLIGHT_LEFT, "classify -30.1");
    eq_int(route_classify(49.9), CUE_SLIGHT_RIGHT, "classify 49.9");
    eq_int(route_classify(50.1), CUE_RIGHT, "classify 50.1");
    eq_int(route_classify(-50.1), CUE_LEFT, "classify -50.1");
    eq_int(route_classify(114.9), CUE_RIGHT, "classify 114.9");
    eq_int(route_classify(115.1), CUE_SHARP_RIGHT, "classify 115.1");
    eq_int(route_classify(-115.1), CUE_SHARP_LEFT, "classify -115.1");
    eq_int(route_classify(159.9), CUE_SHARP_RIGHT, "classify 159.9");
    eq_int(route_classify(160.1), CUE_UTURN, "classify 160.1");
    eq_int(route_classify(-170.0), CUE_UTURN, "classify -170");
    close_to(route_wrap_deg(350.0), -10.0, 1e-9, "wrap 350");
    close_to(route_wrap_deg(-350.0), 10.0, 1e-9, "wrap -350");
    close_to(route_wrap_deg(180.0), 180.0, 1e-9, "wrap 180");
}

/* One corner of a known angle, derived. The bearing arms are +/-25 m, so
 * each straight leg is 200 m -- long enough that the arms see only one leg
 * each and the measured angle is the built one. */
static void
one_corner(double turn_deg, int want_kind, const char *what)
{
    static double en[2 * 401];
    route_t r;
    int n, i, hits = 0, kind = -1;
    double worst = 0.0;

    /* 200 m due north into the corner at index 40, then 200 m out along the
     * new bearing. Legs far longer than the 25 m arms, so what the deriver
     * measures at the corner is the angle this test built. */
    n = line(en, 0, 0.0, 0.0, 0.0, 5.0, 41);
    n = line(en, 41, en[80] + sin(turn_deg * M_PI / 180.0) * 5.0,
             en[81] + cos(turn_deg * M_PI / 180.0) * 5.0, turn_deg, 5.0, 40);
    build(&r, en, n);
    check(route_cues_derive(&r) >= 0, "derive returns");
    for (i = 0; i < r.ncue; i++)
        if (fabs(r.cue[i].along_m - 200.0) < 40.0) {
            hits++;
            kind = r.cue[i].kind;
            worst = r.cue[i].theta_deg;
        }
    /* Exactly one cue at the corner: 7.4's 20 m merge is what collapses the
     * run of 10 m samples that all see the same bend. */
    eq_int(hits, 1, what);
    eq_int(kind, want_kind, what);
    close_to(worst, turn_deg, 6.0, what);
    eq_int(r.ncue, 1, "derive: a single corner yields a single cue");
    teardown(&r);
}

static void
test_derive(void)
{
    one_corner(90.0, CUE_RIGHT, "derive 90 right");
    one_corner(-90.0, CUE_LEFT, "derive 90 left");
    one_corner(40.0, CUE_SLIGHT_RIGHT, "derive 40 slight right");
    one_corner(-40.0, CUE_SLIGHT_LEFT, "derive 40 slight left");
    one_corner(140.0, CUE_SHARP_RIGHT, "derive 140 sharp right");
    one_corner(-140.0, CUE_SHARP_LEFT, "derive 140 sharp left");
    one_corner(175.0, CUE_UTURN, "derive 175 u-turn");
}

static void
test_derive_straight(void)
{
    /* A 2 km straight has no cues at all -- the classifier must not
     * manufacture one out of floating-point noise. */
    static double en[2 * 201];
    route_t r;
    int n = line(en, 0, 0, 0, 22.5, 10, 201);
    build(&r, en, n);
    eq_int(route_cues_derive(&r), 0, "derive: a straight has no cues");
    /* ...but route_cues_finish() still gives it a destination. */
    eq_int(route_cues_finish(&r), 1, "finish: appends the destination");
    eq_int(r.cue[0].kind, CUE_DEST, "finish: it is a CUE_DEST");
    eq_int(r.cue[0].idx, n - 1, "finish: at the last point");
    close_to(r.cue[0].along_m, r.total_m, 0.5, "finish: at the full length");
    teardown(&r);
}

/* --------------------------------------------------------------- progress */

static void
test_progress(void)
{
    static double en[2 * 101];
    route_t r;
    navctx_t ctx;
    nav_t nv;
    int n = line(en, 0, 0, 0, 0, 10, 101), i;
    double prev_pct = -1, prev_togo = 1e18;

    build(&r, en, n);
    nav_init(&ctx);
    nav_reset(&nv);
    /* Ride the whole kilometre at 10 m/s, one fix a second. */
    for (i = 0; i <= 100; i++) {
        route_snap(&r, &ctx, 0.0, i * 10.0, 1000 + i, &nv);
        route_progress(&r, &ctx, 1000 + i, &nv);
        check(nv.pct >= prev_pct - 1e-9, "progress: pct never decreases");
        check(nv.togo_m <= prev_togo + 1e-9, "progress: togo never increases");
        prev_pct = nv.pct;
        prev_togo = nv.togo_m;
    }
    close_to(nv.pct, 100.0, 0.01, "progress: ends at 100%");
    close_to(nv.togo_m, 0.0, 0.5, "progress: ends with nothing to go");

    /* Half way at a steady 10 m/s, the ETA is the remaining 500 m / 10. */
    nav_init(&ctx);
    nav_reset(&nv);
    for (i = 0; i <= 50; i++) {
        route_snap(&r, &ctx, 0.0, i * 10.0, 2000 + i, &nv);
        route_progress(&r, &ctx, 2000 + i, &nv);
    }
    close_to(nv.pct, 50.0, 0.2, "progress: 50% at the half way point");
    close_to(nv.eta_s, 50.0, 2.0, "progress: ETA from the rolling speed");

    /* A traffic light must NOT erase the ETA: 7.2 averages over ten minutes
     * precisely so a stop of seconds does not. Seventy seconds standing
     * still, and the estimate is still there and still sane. */
    for (i = 51; i <= 120; i++) {
        route_snap(&r, &ctx, 0.0, 500.0, 2000 + i, &nv);
        route_progress(&r, &ctx, 2000 + i, &nv);
    }
    check(nv.eta_s > 0.0, "progress: a 70 s stop keeps the ETA");

    /* Standing still for longer than the whole window is different: there is
     * then no evidence of any speed at all, and no ETA is better than a
     * wrong one or an infinite one. */
    for (i = 121; i <= 900; i++) {
        route_snap(&r, &ctx, 0.0, 500.0, 2000 + i, &nv);
        route_progress(&r, &ctx, 2000 + i, &nv);
    }
    check(nv.eta_s < 0.0, "progress: a full window of standing still drops it");
    teardown(&r);
}

/* ------------------------------------------------------------------ latch */

static void
test_latch(void)
{
    static double en[2 * 101];
    route_t r;
    navctx_t ctx;
    nav_t nv;
    int n = line(en, 0, 0, 0, 0, 10, 101), i;

    build(&r, en, n);
    nav_init(&ctx);
    nav_reset(&nv);

    /* Two fixes beyond 40 m are not enough; the third sets it (7.3). */
    for (i = 0; i < 2; i++) {
        route_snap(&r, &ctx, 45.0, 500.0, 1000 + i, &nv);
        eq_int(route_offroute_update(&ctx, &nv), 0,
               "latch: two fixes over 40 m do not set it");
    }
    route_snap(&r, &ctx, 45.0, 500.0, 1002, &nv);
    eq_int(route_offroute_update(&ctx, &nv), 1,
           "latch: the third consecutive fix sets it");

    /* Coming back to 30 m does NOT clear it -- the clear threshold is 25. */
    route_snap(&r, &ctx, 30.0, 500.0, 1003, &nv);
    eq_int(route_offroute_update(&ctx, &nv), 1,
           "latch: 30 m does not clear it");
    route_snap(&r, &ctx, 24.0, 500.0, 1004, &nv);
    eq_int(route_offroute_update(&ctx, &nv), 0, "latch: below 25 m clears it");

    /* The case the asymmetry exists for: a fix wandering between 30 and 38 m
     * on a wide road, for a long time. It must never fire. */
    nav_init(&ctx);
    for (i = 0; i < 500; i++) {
        route_snap(&r, &ctx, 30.0 + 8.0 * (i & 1), 500.0, 2000 + i, &nv);
        if (route_offroute_update(&ctx, &nv)) {
            printf("FAIL latch: 30/38 m oscillation fired at fix %d\n", i);
            failures++;
            break;
        }
    }

    /* And a run interrupted before it completes must restart the count. */
    nav_init(&ctx);
    route_snap(&r, &ctx, 45.0, 500.0, 3000, &nv);
    route_offroute_update(&ctx, &nv);
    route_snap(&r, &ctx, 45.0, 500.0, 3001, &nv);
    route_offroute_update(&ctx, &nv);
    route_snap(&r, &ctx, 10.0, 500.0, 3002, &nv); /* back on, run broken */
    route_offroute_update(&ctx, &nv);
    route_snap(&r, &ctx, 45.0, 500.0, 3003, &nv);
    route_offroute_update(&ctx, &nv);
    route_snap(&r, &ctx, 45.0, 500.0, 3004, &nv);
    eq_int(route_offroute_update(&ctx, &nv), 0,
           "latch: an interrupted run restarts the count");
    teardown(&r);
}

/* ------------------------------------------------------------ cue advance */

static void
test_cue_ahead(void)
{
    static double en[2 * 101];
    route_t r;
    navctx_t ctx;
    nav_t nv;
    int n = line(en, 0, 0, 0, 0, 10, 101), i, prev = -1;

    build(&r, en, n);
    /* Three hand-placed cues plus the destination finish appends. */
    r.cue = calloc(3, sizeof *r.cue);
    r.ncue = 3;
    r.cue[0].idx = 20;
    r.cue[0].kind = CUE_RIGHT;
    r.cue[1].idx = 50;
    r.cue[1].kind = CUE_LEFT;
    r.cue[2].idx = 80;
    r.cue[2].kind = CUE_SLIGHT_RIGHT;
    eq_int(route_cues_finish(&r), 4, "cue_ahead: destination appended");

    nav_init(&ctx);
    nav_reset(&nv);
    for (i = 0; i <= 100; i++) {
        route_snap(&r, &ctx, 0.0, i * 10.0, 1000 + i, &nv);
        route_cue_ahead(&r, &nv);
        check(nv.cue_i >= prev, "cue_ahead: the index never regresses");
        check(nv.cue_m >= -0.001, "cue_ahead: the distance is never negative");
        prev = nv.cue_i;
    }
    eq_int(nv.cue_i, 3, "cue_ahead: ends on the destination");
    close_to(nv.cue_m, 0.0, 0.5, "cue_ahead: arrives at it");
    teardown(&r);
}

/* --------------------------------------------------- against a real loader */

static void
test_loaded_route(void)
{
    /* The komoot fixture has no cues in it, so route_load() must derive
     * them: 300 m north, a square right turn, 300 m east. */
    route_t r;
    char err[256];
    if (route_load("beepy-nav/tests/gpx/komoot-style.gpx", &r, err,
                   sizeof err)) {
        printf("FAIL loaded: %s\n", err);
        failures++;
        return;
    }
    eq_int(r.ncue, 2, "loaded: one derived turn plus the destination");
    eq_int(r.cue[0].kind, CUE_RIGHT, "loaded: the corner is a right turn");
    close_to(r.cue[0].along_m, 300.0, 15.0, "loaded: at the 300 m mark");
    eq_int(r.cue[1].kind, CUE_DEST, "loaded: destination last");
    route_free(&r);

    /* The rwgps fixture HAS cues, so nothing may be derived: DESIGN.md 7.4
     * makes the file the preferred source. */
    if (route_load("beepy-nav/tests/gpx/rwgps-style.gpx", &r, err,
                   sizeof err)) {
        printf("FAIL loaded rwgps: %s\n", err);
        failures++;
        return;
    }
    eq_int(r.ncue, 4, "loaded: the file's own cues, none added");
    eq_int(r.cue[3].kind, CUE_DEST, "loaded: the file's own destination kept");
    close_to(r.cue[1].along_m, 300.0, 1.0, "loaded: file cue has a distance");
    route_free(&r);
}

/* DESIGN.md 1.1.1 -- the ladder, and the latch that makes it flicker-proof. */
static void
test_countdown(void)
{
    int shown = 0, shown_cue = -99, i;
    struct {
        double m;
        int q;
    } ladder[] = {
        {5834, 5800}, {5799.9, 5700}, {1000, 1000}, {999, 950}, {410, 400},
        {200, 200},   {199, 190},     {194, 190},
        {12, 10},     {10, 10},       {9, CUE_FLOOR}, {0, CUE_FLOOR},
    };

    for (i = 0; i < (int)(sizeof ladder / sizeof ladder[0]); i++)
        eq_int(cue_quantise(ladder[i].m), ladder[i].q, "cue_quantise");

    /* nav_init(), not a hand-set sentinel: a zeroed shown_cue collides with
     * the real cue 0 and pins the display at 0 M for a whole ride. That is
     * exactly the bug this test missed the first time by initialising the
     * latch itself. */
    {
        navctx_t c0;
        nav_t n0;
        nav_init(&c0);
        nav_reset(&n0);
        eq_int(cue_latch(400.0, 0, &c0.shown_q, &c0.shown_cue), 400,
               "first fix on cue 0 shows the real distance");
    }

    /* Jitter on a step boundary must not move the display. 850 +/- 4 m
     * straddles the 850 rung; the shown value may only fall. */
    (void)cue_latch(854.0, 3, &shown, &shown_cue);
    eq_int(shown, 850, "latch first value");
    (void)cue_latch(846.0, 3, &shown, &shown_cue);
    eq_int(shown, 800, "846 m steps down to 800");
    (void)cue_latch(853.0, 3, &shown, &shown_cue); /* jitter backwards */
    eq_int(shown, 800, "jitter must not raise the countdown");

    /* A new cue resets it -- upward is legal exactly then. */
    (void)cue_latch(1500.0, 4, &shown, &shown_cue);
    eq_int(shown, 1500, "new cue resets the latch");

    /* The bottom rung holds: at 8 m the arrow is the instruction, and the
     * number must not tick to zero or bounce back up on jitter. */
    (void)cue_latch(8.0, 4, &shown, &shown_cue);
    eq_int(shown, CUE_FLOOR, "the last few metres hold at the bottom rung");
    (void)cue_latch(12.0, 4, &shown, &shown_cue);
    eq_int(shown, CUE_FLOOR, "and jitter does not raise it again");

    /* Monotonicity over a whole approach: never rises, ends at NOW. */
    shown_cue = -99;
    (void)cue_latch(1000.0, 7, &shown, &shown_cue);
    for (i = 1000; i >= 0; i--) {
        int prev = shown;
        double noisy = i + ((i % 7) - 3); /* +/-3 m of jitter */
        (void)cue_latch(noisy < 0.0 ? 0.0 : noisy, 7, &shown, &shown_cue);
        check(shown <= prev, "countdown never rises while the cue holds");
    }
    eq_int(shown, CUE_FLOOR, "approach ends at the bottom rung");
}

/* DESIGN.md 1.1.1's second table: "imperial keeps the ladder's SHAPE, not its
 * numbers". Every expectation below is the shape restated -- 0.1 mi steps
 * beyond a mile, 100 ft from 500 ft out, 50 ft inside that, holding at 50 --
 * computed here from the definition, never read off a previous run. */
static void
test_countdown_imperial(void)
{
    int shown = 0, shown_cue = -99, i;
    struct {
        double m; /* metres in, as everything inside the program is */
        int q;    /* feet out */
    } ladder[] = {
        /* 1609.344 m is a mile exactly: the first rung of the mile ladder. */
        {1609.344, 5280},
        /* 2.35 mi floors to a whole tenth: 10560 + 3 * 528 = 12144. The
         * inputs here avoid exact rung boundaries on purpose -- the ladder
         * floors a round-tripped conversion, so 500 ft expressed in metres
         * and converted back is 499.999... and lands a rung below. That is
         * inherent to flooring, not a defect, and a rider never notices a
         * boundary crossed a hundredth of a foot early. */
        {1609.344 * 2.35, 12144},
        /* Just under a mile: 100 ft steps, 5279 ft -> 5200. */
        {5279 / GEO_FT_PER_M, 5200},
        /* 500 ft is the rung where the 100 ft steps start. */
        {501 / GEO_FT_PER_M, 500},
        {549 / GEO_FT_PER_M, 500},
        /* Below it, 50 ft steps. */
        {499 / GEO_FT_PER_M, 450},
        {101 / GEO_FT_PER_M, 100},
        {51 / GEO_FT_PER_M, CUE_FLOOR_FT},
        /* And the bottom rung holds, as the metric one does at 10 m. */
        {49 / GEO_FT_PER_M, CUE_FLOOR_FT},
        {0, CUE_FLOOR_FT},
        {-5, CUE_FLOOR_FT},
    };

    for (i = 0; i < (int)(sizeof ladder / sizeof ladder[0]); i++)
        eq_int(cue_quantise_u(ladder[i].m, UNITS_IMPERIAL), ladder[i].q,
               "cue_quantise_u imperial");

    /* Never a number nobody reads: under a mile every rung is a multiple of
     * 50 ft, and at or above one it is a whole tenth of a mile. */
    for (i = 0; i < 30000; i++) {
        int q = cue_quantise_u(i / 10.0, UNITS_IMPERIAL);
        if (q < GEO_FT_PER_MILE)
            check(q % 50 == 0, "under a mile, a multiple of 50 ft");
        else
            check(q % (GEO_FT_PER_MILE / 10) == 0,
                  "above a mile, a whole tenth of a mile");
    }

    /* Monotone non-increasing over a whole approach, exactly as metric is. */
    (void)cue_latch_u(3000.0, 7, &shown, &shown_cue, UNITS_IMPERIAL);
    for (i = 3000; i >= 0; i--) {
        int prev = shown;
        double noisy = i + ((i % 7) - 3);
        (void)cue_latch_u(noisy < 0.0 ? 0.0 : noisy, 7, &shown, &shown_cue,
                          UNITS_IMPERIAL);
        check(shown <= prev, "imperial countdown never rises");
    }
    eq_int(shown, CUE_FLOOR_FT, "imperial approach ends at the bottom rung");
}

/* A units change mid-ride resets the latch: 400 metres is not a value 1312
 * feet may only decrease from (1.1.1). */
static void
test_units_toggle(void)
{
    navctx_t c;
    nav_t nv;
    route_t r;
    static pt_t pts[3];
    static cue_t cues[1];

    /* Two cues 400 m apart on a straight north line, so cue_m is exact. */
    route_init(&r);
    pts[0].lat = 13.0;
    pts[0].lon = 100.0;
    pts[0].ele = 0;
    pts[1].lat = 13.0 + 400.0 / GEO_M_PER_DEG_LAT;
    pts[1].lon = 100.0;
    pts[1].ele = 0;
    pts[2].lat = 13.0 + 800.0 / GEO_M_PER_DEG_LAT;
    pts[2].lon = 100.0;
    pts[2].ele = 0;
    r.pt = pts;
    r.npt = 3;
    cues[0].idx = 2;
    cues[0].kind = CUE_DEST;
    r.cue = cues;
    r.ncue = 1;
    check(route_prepare(&r) == 0, "toggle fixture prepares");
    route_cues_finish(&r);

    nav_init(&c);
    nav_reset(&nv);
    nv.cue_i = 0;
    nv.cue_m = 400.0;
    nv.along = 400.0;
    eq_int(c.units, UNITS_METRIC, "metric is the default");

    route_countdown_refresh(&r, &c, &nv);
    eq_int(nv.cue_q, 400, "400 m reads 400");

    /* Without the reset the latch would refuse 1312 ft as an increase and the
     * panel would sit at 400 -- reading FT -- for the rest of the ride. */
    nav_set_units(&c, UNITS_IMPERIAL);
    route_countdown_refresh(&r, &c, &nv);
    eq_int(nv.cue_q, cue_quantise_u(400.0, UNITS_IMPERIAL),
           "the toggle reseeds the latch on the imperial ladder");
    check(nv.cue_q > 1000, "and the value really is feet, not metres");

    /* And back. The metric ladder must not inherit the feet value either. */
    nav_set_units(&c, UNITS_METRIC);
    route_countdown_refresh(&r, &c, &nv);
    eq_int(nv.cue_q, 400, "and back to metres");

    /* Idempotent: setting the units it already has must not reset a latch
     * that is mid-countdown. */
    nv.cue_m = 380.0;
    route_countdown_refresh(&r, &c, &nv);
    eq_int(nv.cue_q, 350, "the countdown steps down");
    nav_set_units(&c, UNITS_METRIC);
    nv.cue_m = 400.0;
    route_countdown_refresh(&r, &c, &nv);
    eq_int(nv.cue_q, 350, "a no-op toggle leaves the latch alone");

    /* Only pt/cum/en were allocated by route_prepare(); pt and cue are ours. */
    r.pt = NULL;
    r.cue = NULL;
    r.npt = r.ncue = 0;
    route_free(&r);
}

int
main(void)
{
    test_prepare();
    test_rebase();
    test_snap_basic();
    test_snap_window();
    test_classify();
    test_derive();
    test_derive_straight();
    test_progress();
    test_latch();
    test_countdown();
    test_countdown_imperial();
    test_units_toggle();
    test_cue_ahead();
    test_loaded_route();
    printf("test_route: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/* beepy-nav/src/route.h -- the loaded route and the per-fix navigation state.
 *
 * DESIGN.md 8 fixes the shape of route_t and nav_t; 7.2-7.4 fix the maths.
 * Three things here are additions the design implies but does not spell out,
 * and they are marked where they appear:
 *
 *   route_t.en    the route in local-tangent metres. 7.2 snaps "onto route
 *                 segments" and 7.4 takes bearings over 25 m, both of which
 *                 are metre operations; re-projecting lat/lon per fix would
 *                 be 20 000 trig calls a second for a value that never
 *                 changes. Built once by route_prepare().
 *   navctx_t      nav_t is "recomputed each fix" (8), so the state that must
 *                 SURVIVE a fix -- the window hint, the off-route run length,
 *                 the 10-minute speed ring -- cannot live in it.
 *   cue_t.along_m the cue's distance along the route. 8 lists only "index,
 *                 kind, name", but the panel needs a distance and deriving it
 *                 from the index every frame is a loop over cum[].
 *
 * Portable C: libc + libm, no pixel headers. That is deliberate -- it is what
 * lets tests/test_route.c link route.c on its own.
 */
#ifndef BEEPY_NAV_ROUTE_H
#define BEEPY_NAV_ROUTE_H

#include <stddef.h>
#include <time.h>

/* Cue kinds. The numbering is arrows.h's ARROW_* order on purpose: a cue kind
 * is passed straight to arrow_draw(). route.c and gpx.c stay free of the
 * pixel headers, so view_nav.c carries the compile-time check that the two
 * enumerations still agree. */
enum {
    CUE_STRAIGHT = 0,
    CUE_SLIGHT_LEFT,
    CUE_LEFT,
    CUE_SHARP_LEFT,
    CUE_SLIGHT_RIGHT,
    CUE_RIGHT,
    CUE_SHARP_RIGHT,
    CUE_UTURN,
    CUE_DEST,
    CUE_N
};

/* DESIGN.md 7.1: 20 000 points, 16 bytes each. */
#define ROUTE_MAXPT 20000
#define ROUTE_MAXCUE 4096
#define CUE_NAME 48
#define ROUTE_NAME 64

/* DESIGN.md 7.2/7.3/7.4, every threshold in one place. */
#define ROUTE_SNAP_WINDOW 100 /* +/- points around the last match      */
#define ROUTE_LOST_S 30       /* after this, widen to a full scan      */
#define ROUTE_OFF_SET_M 40.0  /* off route beyond this...              */
#define ROUTE_OFF_FIXES 3     /* ...for this many consecutive fixes    */
#define ROUTE_OFF_CLEAR_M 25.0 /* back on below this                   */
#define CUE_RESAMPLE_M 10.0   /* derive: resample step                 */
#define CUE_BEARING_M 25.0    /* derive: bearing arm each side         */
#define CUE_MERGE_M 20.0      /* derive: merge window                  */
#define CUE_MIN_DEG 30.0      /* derive: below this, no cue            */
#define ETA_WINDOW_S 600.0    /* rolling average speed over 10 minutes */

/* DESIGN.md 6.1: local tangent plane. */
#define GEO_M_PER_DEG_LAT 110540.0
#define GEO_M_PER_DEG_LON 111320.0

typedef struct {
    double lat, lon, ele;
} pt_t;

typedef struct {
    int idx;          /* the route vertex the cue sits on           */
    int kind;         /* CUE_*                                      */
    double along_m;   /* distance along the route to it             */
    double theta_deg; /* signed turn, + = right; 0 for file cues    */
    char name[CUE_NAME];
} cue_t;

typedef struct {
    pt_t *pt;
    int npt;
    double *cum; /* npt cumulative metres, cum[0] = 0     */
    double *en;  /* npt e,n pairs from (lat0, lon0)       */
    cue_t *cue;
    int ncue;
    char name[ROUTE_NAME];
    double total_m;
    double lat0, lon0;                 /* projection reference   */
    double min_e, min_n, max_e, max_n; /* bbox, metres           */
    int prepared;
    int decimated; /* raw point count before the 20 000 cap */
} route_t;

/* DESIGN.md 8, plus the three values the OVERVIEW strip and the trace need
 * and that are pure functions of the rest (pct, eta_s, snap_e/snap_n). */
typedef struct {
    int seg;      /* snap: segment index, -1 = never matched */
    double along; /* metres along the route                  */
    double off_m; /* perpendicular distance to the route     */
    int off;      /* the latch (7.3), 0/1                    */
    int cue_i;    /* the announced cue                       */
    double cue_m; /* distance to it along the route          */
    double togo_m;
    double pct;
    double eta_s; /* seconds remaining, < 0 = unknown        */
    time_t eta;   /* wall clock, 0 = unknown                 */
    double snap_e, snap_n;
    int cue_q;    /* cue_m quantised + latched (1.1.1), or CUE_NOW */
    int then_q;   /* same, for the distance to the cue after it     */
} nav_t;

/* Everything that must survive a fix. */
#define NAV_SPEED_RING 1024

typedef struct {
    int last_seg;   /* window hint; -1 forces a full scan  */
    int full_scans; /* diagnostics: how often we widened   */
    time_t last_fix_t;
    int have_fix;
    int off_run; /* consecutive fixes beyond 40 m       */
    int off;     /* the latch itself                    */

    /* Rolling 10-minute average speed, as (t, along) samples. A ring rather
     * than a running sum: samples leave the window by age, not by count. */
    double ring_t[NAV_SPEED_RING], ring_s[NAV_SPEED_RING];
    int ring_head, ring_n;

    double t0, s0; /* trip start, for the first-10-minutes fallback */
    int have_t0;

    /* Countdown latch (1.1.1): the shown value only ever decreases while
     * approaching one cue, so GPS jitter on a step boundary cannot flicker it.
     * Reset when the announced cue changes. */
    int shown_q, shown_cue;
} navctx_t;

/* --------------------------------------------------- countdown (1.1.1) */

/* Distance the rider is shown, not the distance measured: floored onto a
 * ladder that coarsens with distance -- 100 m steps beyond a kilometre, 50 m
 * from 200 m out, 10 m inside that, and CUE_NOW for the last few metres.
 * Coarse where there is nothing to do yet, fine where you are about to act. */
#define CUE_NOW (-1)

int cue_quantise(double metres);

/* Quantise and latch against jitter: monotone non-increasing while `cue_i`
 * stays put, reset when it changes. `shown`/`shown_cue` are the caller's
 * latch state. Returns the value to display. */
int cue_latch(double metres, int cue_i, int *shown, int *shown_cue);

/* ------------------------------------------------------------------ setup */

void route_init(route_t *r);
void route_free(route_t *r);

/* cum[], en[], the bbox and total_m. Idempotent. 0 on success. */
int route_prepare(route_t *r);

/* DESIGN.md 7.4, the derived path: resample at 10 m, bearings over +/-25 m,
 * classify, merge within 20 m keeping the largest |theta|. Replaces any
 * existing cue list. Returns the cue count, or -1 on allocation failure. */
int route_cues_derive(route_t *r);

/* along_m for cues that came from the file (which know only an index), then
 * a CUE_DEST at the finish if the list does not already end in one. */
int route_cues_finish(route_t *r);

/* gpx_load + prepare + derive-when-absent + finish, which is what every
 * caller outside the tests actually wants. 0 on success, -1 with a message
 * in err (which always names a line number when the file is at fault). */
int route_load(const char *path, route_t *r, char *err, size_t errsz);

/* ------------------------------------------------------------ per-fix work */

void nav_init(navctx_t *ctx);
void nav_reset(nav_t *nv);

/* DESIGN.md 7.2: project onto the segments in a +/-100-point window around
 * the last match, widening to the whole route on the first fix or after 30 s
 * without one. Fills seg/along/off_m/snap_e/snap_n and advances the window
 * hint; the latch is route_offroute_update()'s job. */
void route_snap(const route_t *r, navctx_t *ctx, double e, double n,
                time_t now, nav_t *nv);

/* DESIGN.md 7.3, as a latch: >40 m for 3 consecutive fixes sets it, <25 m
 * clears it. Returns the latched state and writes it to nv->off. */
int route_offroute_update(navctx_t *ctx, nav_t *nv);

/* The next cue at or ahead of nv->along, and the distance to it. */
void route_cue_ahead(const route_t *r, nav_t *nv);

/* pct, togo_m and the ETA. `now` seconds and `along` metres feed the rolling
 * average; pass the same clock as route_snap(). */
void route_progress(const route_t *r, navctx_t *ctx, double now, nav_t *nv);

/* ------------------------------------------------------------- small tools */

/* Local-tangent projection about (lat0, lon0), DESIGN.md 6.1. */
void geo_project(double lat0, double lon0, double lat, double lon, double *e,
                 double *n);

/* Signed smallest difference b - a, degrees, in (-180, 180]. */
double route_wrap_deg(double d);

/* CUE_* for a signed turn in degrees (7.4's bands). CUE_STRAIGHT below 30. */
int route_classify(double theta_deg);

#endif /* BEEPY_NAV_ROUTE_H */

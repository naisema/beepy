/* beepy-nav/src/route.c -- see route.h. DESIGN.md 7.2 - 7.4. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpx.h"
#include "route.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

#define DEG (180.0 / M_PI)

/* ------------------------------------------------------------------ setup */

void
route_init(route_t *r)
{
    memset(r, 0, sizeof *r);
}

void
route_free(route_t *r)
{
    free(r->pt);
    free(r->cum);
    free(r->en);
    free(r->cue);
    memset(r, 0, sizeof *r);
}

void
geo_project(double lat0, double lon0, double lat, double lon, double *e,
            double *n)
{
    *e = (lon - lon0) * GEO_M_PER_DEG_LON * cos(lat0 * (M_PI / 180.0));
    *n = (lat - lat0) * GEO_M_PER_DEG_LAT;
}

int
route_prepare(route_t *r)
{
    int i;
    if (r->npt < 2)
        return -1;
    free(r->cum);
    free(r->en);
    r->cum = malloc((size_t)r->npt * sizeof *r->cum);
    r->en = malloc((size_t)r->npt * 2 * sizeof *r->en);
    if (!r->cum || !r->en) {
        free(r->cum);
        free(r->en);
        r->cum = NULL;
        r->en = NULL;
        return -1;
    }
    /* The reference is the first point rather than the centroid: DESIGN.md
     * 6.1 re-references every 10 km and quotes <0.1% error inside 50 km, and
     * a route long enough to care is re-referenced by the live code, not
     * here. Using the start keeps en[0] exactly (0, 0), which several tests
     * and the snap window's arithmetic read more easily. */
    r->lat0 = r->pt[0].lat;
    r->lon0 = r->pt[0].lon;
    for (i = 0; i < r->npt; i++)
        geo_project(r->lat0, r->lon0, r->pt[i].lat, r->pt[i].lon,
                    &r->en[2 * i], &r->en[2 * i + 1]);
    r->cum[0] = 0.0;
    r->min_e = r->max_e = r->en[0];
    r->min_n = r->max_n = r->en[1];
    for (i = 1; i < r->npt; i++) {
        r->cum[i] = r->cum[i - 1] + hypot(r->en[2 * i] - r->en[2 * i - 2],
                                          r->en[2 * i + 1] - r->en[2 * i - 1]);
        if (r->en[2 * i] < r->min_e)
            r->min_e = r->en[2 * i];
        if (r->en[2 * i] > r->max_e)
            r->max_e = r->en[2 * i];
        if (r->en[2 * i + 1] < r->min_n)
            r->min_n = r->en[2 * i + 1];
        if (r->en[2 * i + 1] > r->max_n)
            r->max_n = r->en[2 * i + 1];
    }
    r->total_m = r->cum[r->npt - 1];
    r->prepared = 1;
    return 0;
}

/* ------------------------------------------------------------------- cues */

double
route_wrap_deg(double d)
{
    while (d > 180.0)
        d -= 360.0;
    while (d <= -180.0)
        d += 360.0;
    return d;
}

int
route_classify(double theta_deg)
{
    double a = fabs(theta_deg);
    int right = theta_deg > 0;
    if (a < CUE_MIN_DEG)
        return CUE_STRAIGHT;
    if (a < 50.0)
        return right ? CUE_SLIGHT_RIGHT : CUE_SLIGHT_LEFT;
    if (a < 115.0)
        return right ? CUE_RIGHT : CUE_LEFT;
    if (a <= 160.0)
        return right ? CUE_SHARP_RIGHT : CUE_SHARP_LEFT;
    return CUE_UTURN;
}

/* Point at distance d along the route, by binary search on cum[]. */
static void
at_along(const route_t *r, double d, double *e, double *n)
{
    int lo = 0, hi = r->npt - 1, i;
    double seg, t;
    if (d <= 0.0) {
        *e = r->en[0];
        *n = r->en[1];
        return;
    }
    if (d >= r->total_m) {
        *e = r->en[2 * (r->npt - 1)];
        *n = r->en[2 * (r->npt - 1) + 1];
        return;
    }
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (r->cum[mid] <= d)
            lo = mid;
        else
            hi = mid;
    }
    i = lo;
    seg = r->cum[i + 1] - r->cum[i];
    t = seg > 1e-9 ? (d - r->cum[i]) / seg : 0.0;
    *e = r->en[2 * i] + (r->en[2 * i + 2] - r->en[2 * i]) * t;
    *n = r->en[2 * i + 1] + (r->en[2 * i + 3] - r->en[2 * i + 1]) * t;
}

/* Compass bearing in degrees, clockwise from north -- so a positive turn is
 * a right turn, which is what route_classify() assumes. */
static double
bearing_deg(double e0, double n0, double e1, double n1)
{
    return atan2(e1 - e0, n1 - n0) * DEG;
}

/* Nearest route vertex to a distance along it. */
static int
vertex_at(const route_t *r, double d)
{
    int lo = 0, hi = r->npt - 1;
    while (hi - lo > 1) {
        int mid = (lo + hi) / 2;
        if (r->cum[mid] <= d)
            lo = mid;
        else
            hi = mid;
    }
    return (d - r->cum[lo]) <= (r->cum[hi] - d) ? lo : hi;
}

int
route_cues_derive(route_t *r)
{
    int nsamp, i, out = 0;
    cue_t *cue;
    if (!r->prepared && route_prepare(r))
        return -1;
    if (r->total_m < 2 * CUE_BEARING_M)
        return 0;

    nsamp = (int)(r->total_m / CUE_RESAMPLE_M) + 1;
    if (nsamp > ROUTE_MAXCUE * 64)
        nsamp = ROUTE_MAXCUE * 64;
    cue = malloc((size_t)ROUTE_MAXCUE * sizeof *cue);
    if (!cue)
        return -1;

    /* DESIGN.md 7.4: resample at 10 m, take the bearing over the 25 m before
     * and the 25 m after each sample, classify the signed difference. The
     * arms are taken in ALONG-ROUTE distance rather than over a fixed number
     * of vertices, so a dense GPX and a sparse one classify the same corner
     * the same way. */
    for (i = 0; i < nsamp; i++) {
        double d = i * CUE_RESAMPLE_M;
        double ae, an, be, bn, ce, cn, theta;
        int kind;
        if (d < CUE_BEARING_M || d > r->total_m - CUE_BEARING_M)
            continue;
        at_along(r, d - CUE_BEARING_M, &ae, &an);
        at_along(r, d, &be, &bn);
        at_along(r, d + CUE_BEARING_M, &ce, &cn);
        theta = route_wrap_deg(bearing_deg(be, bn, ce, cn) -
                               bearing_deg(ae, an, be, bn));
        kind = route_classify(theta);
        if (kind == CUE_STRAIGHT)
            continue;
        /* Merge within 20 m, keeping the largest |theta|. A single junction
         * lights up several consecutive 10 m samples; without the merge a
         * right turn becomes three right turns. */
        if (out > 0 && d - cue[out - 1].along_m <= CUE_MERGE_M) {
            if (fabs(theta) > fabs(cue[out - 1].theta_deg)) {
                cue[out - 1].along_m = d;
                cue[out - 1].theta_deg = theta;
                cue[out - 1].kind = kind;
            }
            continue;
        }
        if (out >= ROUTE_MAXCUE)
            break;
        memset(&cue[out], 0, sizeof cue[out]);
        cue[out].along_m = d;
        cue[out].theta_deg = theta;
        cue[out].kind = kind;
        out++;
    }
    for (i = 0; i < out; i++)
        cue[i].idx = vertex_at(r, cue[i].along_m);

    free(r->cue);
    r->cue = cue;
    r->ncue = out;
    return out;
}

int
route_cues_finish(route_t *r)
{
    int i;
    if (!r->prepared && route_prepare(r))
        return -1;
    /* File cues know an index; derived ones know a distance. Fill in
     * whichever is missing so everything downstream can use along_m. */
    for (i = 0; i < r->ncue; i++) {
        if (r->cue[i].along_m <= 0.0 && r->cue[i].idx > 0 &&
            r->cue[i].idx < r->npt)
            r->cue[i].along_m = r->cum[r->cue[i].idx];
        if (r->cue[i].idx <= 0 && r->cue[i].along_m > 0.0)
            r->cue[i].idx = vertex_at(r, r->cue[i].along_m);
    }
    /* The destination is a cue: the panel needs something to count down to
     * on the last leg, and the OVERVIEW's flag needs a cue to sit on. */
    if (r->ncue == 0 || r->cue[r->ncue - 1].kind != CUE_DEST) {
        cue_t *nc = realloc(r->cue, (size_t)(r->ncue + 1) * sizeof *nc);
        if (!nc)
            return -1;
        r->cue = nc;
        memset(&r->cue[r->ncue], 0, sizeof r->cue[r->ncue]);
        r->cue[r->ncue].idx = r->npt - 1;
        r->cue[r->ncue].kind = CUE_DEST;
        r->cue[r->ncue].along_m = r->total_m;
        r->ncue++;
    }
    return r->ncue;
}

int
route_load(const char *path, route_t *r, char *err, size_t errsz)
{
    if (gpx_load(path, r, err, errsz))
        return -1;
    if (route_prepare(r)) {
        route_free(r);
        if (err && errsz)
            snprintf(err, errsz, "%s: route is too short to follow", path);
        return -1;
    }
    /* DESIGN.md 7.4: the file is the preferred source; derivation is the
     * fallback for a bare <trk>, which is the common case for a shared ride. */
    if (r->ncue == 0)
        route_cues_derive(r);
    route_cues_finish(r);
    return 0;
}

/* ------------------------------------------------------------------- snap */

void
nav_init(navctx_t *ctx)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->last_seg = -1;
    /* NOT 0: cue 0 is a real cue, and a zeroed shown_cue makes the latch
     * believe it is already counting down to it from a shown value of 0 --
     * which, being the minimum, it can then never leave. The panel read
     * "0 M" for an entire ride before this line existed. */
    ctx->shown_cue = -1;
}

void
nav_set_units(navctx_t *ctx, int units)
{
    if (ctx->units == units)
        return;
    ctx->units = units;
    /* The latch is monotone within one ladder and meaningless across two: a
     * shown 400 (metres) would forbid 1312 (feet) forever. -1 is "no cue yet",
     * which re-seeds it on the next evaluation. */
    ctx->shown_cue = -1;
}

void
nav_reset(nav_t *nv)
{
    memset(nv, 0, sizeof *nv);
    nv->seg = -1;
    nv->cue_i = -1;
    nv->eta_s = -1.0;
}

/* Perpendicular distance from (e, n) to segment i, and the fraction along. */
static double
seg_dist(const route_t *r, int i, double e, double n, double *t_out)
{
    double ax = r->en[2 * i], ay = r->en[2 * i + 1];
    double bx = r->en[2 * i + 2], by = r->en[2 * i + 3];
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy, t;
    if (len2 < 1e-12)
        t = 0.0;
    else {
        t = ((e - ax) * dx + (n - ay) * dy) / len2;
        if (t < 0.0)
            t = 0.0;
        else if (t > 1.0)
            t = 1.0;
    }
    *t_out = t;
    return hypot(e - (ax + dx * t), n - (ay + dy * t));
}

void
route_snap(const route_t *r, navctx_t *ctx, double e, double n, time_t now,
           nav_t *nv)
{
    int lo = 0, hi = r->npt - 2, i, best = -1;
    double bestd = 0.0, bestt = 0.0;
    int widen;

    if (r->npt < 2 || !r->prepared) {
        nv->seg = -1;
        return;
    }
    /* DESIGN.md 7.2: a +/-100-point window around the last match, because a
     * full scan every fix would be 20 000 segment projections a second for
     * no benefit. It widens on the first fix, and after 30 s without one --
     * long enough that the bike could be anywhere. */
    widen = ctx->last_seg < 0 || !ctx->have_fix ||
            (now - ctx->last_fix_t) > ROUTE_LOST_S;
    if (!widen) {
        lo = ctx->last_seg - ROUTE_SNAP_WINDOW;
        hi = ctx->last_seg + ROUTE_SNAP_WINDOW;
        if (lo < 0)
            lo = 0;
        if (hi > r->npt - 2)
            hi = r->npt - 2;
    } else {
        ctx->full_scans++;
    }
    for (i = lo; i <= hi; i++) {
        double t, d = seg_dist(r, i, e, n, &t);
        if (best < 0 || d < bestd) {
            best = i;
            bestd = d;
            bestt = t;
        }
    }
    if (best < 0) {
        nv->seg = -1;
        return;
    }
    nv->seg = best;
    nv->off_m = bestd;
    nv->along = r->cum[best] + (r->cum[best + 1] - r->cum[best]) * bestt;
    nv->snap_e = r->en[2 * best] + (r->en[2 * best + 2] - r->en[2 * best]) * bestt;
    nv->snap_n =
        r->en[2 * best + 1] + (r->en[2 * best + 3] - r->en[2 * best + 1]) * bestt;
    ctx->last_seg = best;
    ctx->last_fix_t = now;
    ctx->have_fix = 1;
}

int
route_offroute_update(navctx_t *ctx, nav_t *nv)
{
    /* DESIGN.md 7.3, as a latch. The asymmetry is the whole point: a fix
     * oscillating between 30 m and 38 m on a wide road never reaches 40, so
     * it can neither set the latch nor -- once set by a genuine excursion --
     * clear it by luck. */
    if (nv->off_m > ROUTE_OFF_SET_M) {
        if (ctx->off_run < ROUTE_OFF_FIXES)
            ctx->off_run++;
        if (ctx->off_run >= ROUTE_OFF_FIXES)
            ctx->off = 1;
    } else {
        ctx->off_run = 0;
        if (nv->off_m < ROUTE_OFF_CLEAR_M)
            ctx->off = 0;
    }
    nv->off = ctx->off;
    return ctx->off;
}

void
route_cue_ahead(const route_t *r, nav_t *nv)
{
    int i;
    nv->cue_i = -1;
    nv->cue_m = 0.0;
    for (i = 0; i < r->ncue; i++) {
        /* "Ahead" with a metre of slack: standing exactly on a cue must
         * advance to the next one, or the panel would announce a turn you
         * are already making forever. */
        if (r->cue[i].along_m > nv->along + 1.0) {
            nv->cue_i = i;
            nv->cue_m = r->cue[i].along_m - nv->along;
            return;
        }
    }
    if (r->ncue > 0) {
        nv->cue_i = r->ncue - 1;
        nv->cue_m = r->cue[r->ncue - 1].along_m - nv->along;
        if (nv->cue_m < 0.0)
            nv->cue_m = 0.0;
    }
}

/* -------------------------------------------------- countdown (1.1.1) */

int
cue_quantise_u(double metres, int units)
{
    int m;

    if (units == UNITS_IMPERIAL) {
        int ft;
        if (!(metres > 0.0))
            return CUE_FLOOR_FT;
        ft = (int)(metres * GEO_FT_PER_M); /* floor, as the metric rung does */
        /* The same three-rung shape as below, in the units a rider who thinks
         * in feet actually reads: tenths of a mile beyond a mile (528 ft), a
         * hundred feet from five hundred out, fifty inside that. */
        if (ft >= GEO_FT_PER_MILE)
            return ft - ft % (GEO_FT_PER_MILE / 10);
        if (ft >= 500)
            return ft - ft % 100;
        if (ft >= CUE_FLOOR_FT)
            return ft - ft % 50;
        return CUE_FLOOR_FT;
    }

    if (!(metres > 0.0))
        return CUE_FLOOR;
    m = (int)metres; /* floor: never overstate what is left */
    if (m >= 1000)
        return m - m % 100;
    if (m >= 200)
        return m - m % 50;
    if (m >= CUE_FLOOR)
        return m - m % 10;
    /* The last few metres hold at the bottom rung rather than counting to
     * zero or switching to a word: at this range the arrow is the
     * instruction, and a changing number is only noise. */
    return CUE_FLOOR;
}

int
cue_quantise(double metres)
{
    return cue_quantise_u(metres, UNITS_METRIC);
}

int
cue_latch_u(double metres, int cue_i, int *shown, int *shown_cue, int units)
{
    int q = cue_quantise_u(metres, units);

    if (cue_i != *shown_cue) {
        *shown_cue = cue_i;
        *shown = q;
        return q;
    }
    /* Monotone non-increasing while the cue holds: jitter can only be
     * ignored, never shown. */
    if (q < *shown)
        *shown = q;
    return *shown;
}

int
cue_latch(double metres, int cue_i, int *shown, int *shown_cue)
{
    return cue_latch_u(metres, cue_i, shown, shown_cue, UNITS_METRIC);
}

/* --------------------------------------------------------------- progress */

void
route_progress(const route_t *r, navctx_t *ctx, double now, nav_t *nv)
{
    int i, oldest;
    double v = -1.0;

    nv->togo_m = r->total_m - nv->along;
    if (nv->togo_m < 0.0)
        nv->togo_m = 0.0;
    nv->pct = r->total_m > 0.0 ? 100.0 * nv->along / r->total_m : 0.0;
    if (nv->pct < 0.0)
        nv->pct = 0.0;
    if (nv->pct > 100.0)
        nv->pct = 100.0;

    if (!ctx->have_t0) {
        ctx->t0 = now;
        ctx->s0 = nv->along;
        ctx->have_t0 = 1;
    }
    ctx->ring_t[ctx->ring_head] = now;
    ctx->ring_s[ctx->ring_head] = nv->along;
    ctx->ring_head = (ctx->ring_head + 1) % NAV_SPEED_RING;
    if (ctx->ring_n < NAV_SPEED_RING)
        ctx->ring_n++;

    /* DESIGN.md 7.2: TO GO / rolling average over the last 10 minutes,
     * falling back to the trip average in the first 10 minutes. The oldest
     * sample still inside the window is the denominator, so the average is
     * over real elapsed time and a dropout shortens the window rather than
     * inventing distance. */
    oldest = -1;
    for (i = 0; i < ctx->ring_n; i++) {
        int k = (ctx->ring_head - 1 - i + 2 * NAV_SPEED_RING) % NAV_SPEED_RING;
        if (now - ctx->ring_t[k] > ETA_WINDOW_S)
            break;
        oldest = k;
    }
    if (oldest >= 0 && now - ctx->ring_t[oldest] >= 30.0)
        v = (nv->along - ctx->ring_s[oldest]) / (now - ctx->ring_t[oldest]);
    else if (now - ctx->t0 >= 30.0)
        v = (nv->along - ctx->s0) / (now - ctx->t0);

    if (v > 0.1) {
        nv->eta_s = nv->togo_m / v;
        nv->eta = (time_t)(now + nv->eta_s);
    } else {
        /* Stopped, or not enough history: no ETA rather than a wrong one. */
        nv->eta_s = -1.0;
        nv->eta = 0;
    }

    route_countdown_refresh(r, ctx, nv);
}

void
route_countdown_refresh(const route_t *r, navctx_t *ctx, nav_t *nv)
{
    /* What the panel shows: quantised and latched, never the raw metres. */
    nv->cue_q = cue_latch_u(nv->cue_m, nv->cue_i, &ctx->shown_q,
                            &ctx->shown_cue, ctx->units);
    /* The THEN gap is cue-to-cue, so it does not count down and needs no
     * latch -- only the same ladder, so the two rows read alike. */
    if (nv->cue_i >= 0 && nv->cue_i + 1 < r->ncue)
        nv->then_q = cue_quantise_u(r->cue[nv->cue_i + 1].along_m
                                        - r->cue[nv->cue_i].along_m,
                                    ctx->units);
    else
        nv->then_q = ctx->units == UNITS_IMPERIAL ? CUE_FLOOR_FT : CUE_FLOOR;
}

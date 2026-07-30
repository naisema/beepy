/* beepy-nav/src/view_overview.c -- the OVERVIEW page (DESIGN.md 1.2).
 *
 * A transcription of mockup.py's page_overview(), constant for constant. The
 * design gate byte-compares this frame against the mockup's own, so every
 * literal below is copied rather than re-derived -- including the ones that
 * look arbitrary (the 2.6/1.8 cue dots, the 5.5/4.5 start ring, the 14 px
 * gap that separates TO GO from the digits).
 *
 * It answers the one question the NAV page cannot: where does this go, and
 * how much is left. Marking every cue as a dot is what makes "how many turns
 * are coming" readable from the shape alone.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "arrows.h"
#include "draw.h"
#include "map.h"
#include "route.h"
#include "seg.h"
#include "tile.h"
#include "view.h"

#define W SCR_W
#define H SCR_H

/* page_overview()'s frame. */
#define OV_STRIP 42                    /* inverted bottom strip           */
#define OV_MY0 24                      /* title band: the map never runs  */
#define OV_MY1 (H - OV_STRIP - 1)      /* under it                        */
#define OV_PAD 16

/* A whole route rather than the stretch ahead, so this needs more room than
 * view_nav.c's scratch. At the fitted zoom 4096 points is ten to the pixel;
 * anything denser is decimated on the way in, which is invisible and keeps
 * the buffers bounded. */
#define OV_MAXPTS 4096
static double o_world[2 * OV_MAXPTS];
static double o_path[2 * OV_MAXPTS];
static double o_segs[4 * OV_MAXPTS];
static double o_raw[2 * OV_MAXPTS]; /* the raw vertices, projected */

/* Right-aligned 5x7 text: mockup.py's rtext(). */
static void
rtext(cov_t *c, int x_right, int y, const char *s, int scale, int ink)
{
    cov_text(c, x_right - cov_text_w(s, scale), y, s, scale, ink);
}

/* Every k'th vertex, both ends kept. Only bites on routes denser than the
 * page can show. */
static int
thin(const double *pts, int npts, double *out, int max)
{
    int i, m = 0, step;
    if (npts <= max) {
        for (i = 0; i < npts; i++) {
            out[2 * i] = pts[2 * i];
            out[2 * i + 1] = pts[2 * i + 1];
        }
        return npts;
    }
    step = (npts + max - 2) / (max - 1);
    for (i = 0; i < npts; i += step, m++) {
        out[2 * m] = pts[2 * i];
        out[2 * m + 1] = pts[2 * i + 1];
    }
    out[2 * m] = pts[2 * (npts - 1)];
    out[2 * m + 1] = pts[2 * (npts - 1) + 1];
    return m + 1;
}

void
view_overview(cov_t *c, const overview_t *o)
{
    double min_e, max_e, min_n, max_n, org_e, org_n, mpp, cx, cy;
    double sx, sy, pos_e, pos_n, px, py, xy[2], en[2];
    char buf[96];
    /* The two numbers arrive pre-formatted -- the caller is the one that knows
     * the units setting -- so all this page decides is what to call them. */
    const char *unit = o->units == UNITS_IMPERIAL ? "MI" : "KM";
    int n, i, k, ns, nseg, pos_i, wd;

    n = thin(o->pts, o->npts, o_raw, OV_MAXPTS);
    /* pos_i indexes the caller's vertices; after thinning it indexes ours.
     * Scaling it is exact when nothing was thinned, which is the demo and
     * every route short enough to matter. */
    pos_i = o->npts > 1 ? (int)((long)o->pos_i * (n - 1) / (o->npts - 1)) : 0;
    if (pos_i > n - 2)
        pos_i = n - 2;
    if (pos_i < 0)
        pos_i = 0;

    min_e = max_e = o_raw[0];
    min_n = max_n = o_raw[1];
    for (i = 1; i < n; i++) {
        if (o_raw[2 * i] < min_e)
            min_e = o_raw[2 * i];
        if (o_raw[2 * i] > max_e)
            max_e = o_raw[2 * i];
        if (o_raw[2 * i + 1] < min_n)
            min_n = o_raw[2 * i + 1];
        if (o_raw[2 * i + 1] > max_n)
            max_n = o_raw[2 * i + 1];
    }
    org_e = (min_e + max_e) / 2.0;
    org_n = (min_n + max_n) / 2.0;
    /* Fit: the coarser of the two axes wins, so the whole route is inside a
     * 16 px margin. The height budget is measured from the title band down,
     * never from the top of the screen -- that is what keeps the route out
     * from under the name. */
    mpp = (max_e - min_e) / (W - 2.0 * OV_PAD);
    sy = (max_n - min_n) / (double)(OV_MY1 - OV_MY0 - 2 * OV_PAD);
    if (sy > mpp)
        mpp = sy;
    if (!(mpp > 0.0))
        mpp = MAP_ZOOMS[0];
    cx = W / 2.0;
    cy = (OV_MY0 + OV_MY1) / 2.0;

    pos_e = o_raw[2 * pos_i] +
            (o_raw[2 * pos_i + 2] - o_raw[2 * pos_i]) * o->pos_f;
    pos_n = o_raw[2 * pos_i + 1] +
            (o_raw[2 * pos_i + 3] - o_raw[2 * pos_i + 1]) * o->pos_f;

    /* Ridden: everything behind the fix, dashed and thin, through the crisp
     * hairline layer. */
    for (k = 0; k <= pos_i && k < OV_MAXPTS - 1; k++) {
        o_world[2 * k] = o_raw[2 * k];
        o_world[2 * k + 1] = o_raw[2 * k + 1];
    }
    o_world[2 * k] = pos_e;
    o_world[2 * k + 1] = pos_n;
    k++;
    ns = map_round_corners(o_world, k, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                           o_path, OV_MAXPTS);
    map_project(o_path, ns, org_e, org_n, mpp, cx, cy, 0.0, o_path);
    nseg = map_clip_segs(o_path, ns, 0, OV_MY0, W - 1, OV_MY1, o_segs,
                         OV_MAXPTS);
    mark_dashed(c, o_segs, nseg, 5, 5, 1, COV_INK);

    /* Remaining: cased, but a 3 px core rather than the NAV page's 6 -- at
     * this zoom the whole route is on screen and a fat line would be a
     * sausage. */
    o_world[0] = pos_e;
    o_world[1] = pos_n;
    k = 1;
    for (i = pos_i + 1; i < n && k < OV_MAXPTS; i++, k++) {
        o_world[2 * k] = o_raw[2 * i];
        o_world[2 * k + 1] = o_raw[2 * i + 1];
    }
    ns = map_round_corners(o_world, k, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                           o_path, OV_MAXPTS);
    map_project(o_path, ns, org_e, org_n, mpp, cx, cy, 0.0, o_path);
    nseg = map_clip_segs(o_path, ns, 0, OV_MY0, W - 1, OV_MY1, o_segs,
                         OV_MAXPTS);
    mark_cased_route(c, o_segs, nseg, 8, 4.5);

    map_project(o_raw, n, org_e, org_n, mpp, cx, cy, 0.0, o_raw);

    /* Every cue as a dot: paper first, then ink, so a dot sitting on the
     * route still reads as a dot and not as a bulge. */
    for (i = 0; i < o->ncue_dots; i++) {
        int v = o->cue_idx[i];
        if (o->npts > 1)
            v = (int)((long)v * (n - 1) / (o->npts - 1));
        if (v < 0 || v >= n)
            continue;
        cov_disc(c, o_raw[2 * v], o_raw[2 * v + 1], 2.6, COV_PAPER);
        cov_disc(c, o_raw[2 * v], o_raw[2 * v + 1], 1.8, COV_INK);
    }

    sx = o_raw[0];
    sy = o_raw[1];
    cov_disc(c, sx, sy, 5.5, COV_PAPER);
    cov_ring(c, sx, sy, 4.5, 2.0, COV_INK);
    arrow_draw(c, o_raw[2 * (n - 1)] - 11, o_raw[2 * (n - 1) + 1] - 22, 22,
               ARROW_DEST, COV_INK);

    en[0] = pos_e;
    en[1] = pos_n;
    map_project(en, 1, org_e, org_n, mpp, cx, cy, 0.0, xy);
    px = xy[0];
    py = xy[1];
    mark_position(c, px, py, 9, 0.0);

    /* The badge normally sits at y 27, where its needle's paper halo reaches
     * up to y 5 and into the title band -- harmless while the band is one
     * line of text starting at x 6. The attribution of 6.5 lands in rows 1..7
     * at the other end of that band, so when it is there the badge drops
     * seven pixels and the needle clears it. Moving the badge is the cheap
     * side of this collision: the alternative was thirty pixels off the
     * title, and on a north-up page the needle is the one mark on the screen
     * that says nothing a rider does not already know. */
    mark_compass(c, W - 21, o->osm ? 34 : 27, 0.0, 11);
    mark_scale_bar(c, 7, OV_MY1 - 5, mpp, o->units);

    /* Name and total length share the title line. That is what frees the
     * strip's second line, which is what lets everything in the strip run at
     * scale 2 -- the panel's readable floor.
     *
     * And when a basemap is loaded they share it with the attribution, which
     * DESIGN.md 6.5 puts here and nowhere else on the screen: the NAV page has
     * no square inch to give and this line is the one piece of chrome a rider
     * reads at a standstill. It goes at scale 1 and right-aligned -- 30
     * characters of legal text is not a headline.
     *
     * y = 1, not 4: the compass badge is centred at (W-21, 27) with a paper
     * halo of radius 12.6, so it reaches up to y = 14 and left to x = 366.
     * Sitting the attribution's 7 px of ink in rows 1..7 clears it entirely
     * and lets the line run to x = 394 -- the alternative, keeping y = 4 and
     * stopping short of the badge, costs thirty pixels of title to buy two
     * pixels of vertical alignment nobody can see.
     *
     * That leaves 394 - 180 - 8 - 6 = 200 px for the title, which the demo's
     * own name overruns, so the rule is written out rather than assumed: the
     * LENGTH is kept whole and the NAME loses characters. The number is the
     * part that changes; the name is the part you already know. With no
     * basemap not a pixel of this happens and the frozen overview golden is
     * untouched. */
    if (o->osm) {
        int aw = cov_text_w(TILES_ATTRIB, 1);
        int budget = (W - 6 - aw - 8) - 6;
        int keep = strlen(o->name);
        rtext(c, W - 6, 1, TILES_ATTRIB, 1, COV_INK);
        for (;;) {
            snprintf(buf, sizeof buf, "%.*s  %s%s", keep, o->name, o->total,
                     unit);
            if (keep <= 1 || cov_text_w(buf, 2) <= budget)
                break;
            keep--;
        }
        /* Trailing spaces left by a cut mid-word read as a gap, not a cut. */
        while (keep > 1 && o->name[keep - 1] == ' ')
            keep--;
        snprintf(buf, sizeof buf, "%.*s  %s%s", keep, o->name, o->total, unit);
    } else {
        snprintf(buf, sizeof buf, "%s  %s%s", o->name, o->total, unit);
    }
    cov_text(c, 6, 4, buf, 2, COV_INK);

    cov_fill_rect(c, 0, H - OV_STRIP, W - 1, H - 1, COV_INK);
    snprintf(buf, sizeof buf, "%d%% DONE", o->done);
    cov_text(c, 6, H - OV_STRIP + 5, buf, 2, COV_PAPER);
    snprintf(buf, sizeof buf, "%d/%d CUES  ETA %s", o->cue_i, o->ncues,
             o->eta);
    cov_text(c, 6, H - OV_STRIP + 23, buf, 2, COV_PAPER);
    wd = num_draw(c, W - 8, H - OV_STRIP + 5, o->togo, NUM_22, NUM_RT,
                  COV_PAPER);
    rtext(c, W - 14 - wd, H - OV_STRIP + 5, "TO GO", 2, COV_PAPER);
    rtext(c, W - 14 - wd, H - OV_STRIP + 23, unit, 2, COV_PAPER);
}

/* ------------------------------------------------------------ the demo */

/* mockup.py's ROUTE_M, POS_I/POS_F and the cue set page_overview() dots. */
static const double DEMO_ROUTE[] = {
    0,    -900, 0,    -400, 40,   -150, 40,   260,  190, 260,
    190,  620,  430,  700,  560,  980,  430,  1180, 120, 1260,
    -220, 1210, -430, 1040, -470, 780,  -330, 560,  -120, 520};
#define DEMO_NPTS ((int)(sizeof DEMO_ROUTE / sizeof DEMO_ROUTE[0] / 2))
static const int DEMO_CUES[] = {3, 4, 6, 8, 10, 12};

void
view_overview_demo(cov_t *c, int osm)
{
    overview_t o;
    o.pts = DEMO_ROUTE;
    o.npts = DEMO_NPTS;
    o.cue_idx = DEMO_CUES;
    o.ncue_dots = (int)(sizeof DEMO_CUES / sizeof DEMO_CUES[0]);
    o.pos_i = 2;
    o.pos_f = 0.0;
    o.name = "SUKHUMVIT LOOP";
    o.total = "31.0";
    o.togo = "12.6";
    o.eta = "10:42";
    o.done = 59;
    o.cue_i = 3;
    o.ncues = 11;
    o.units = UNITS_METRIC; /* the frozen design state; see view_nav_demo() */
    o.osm = osm;
    view_overview(c, &o);
}

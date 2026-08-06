/* beepy-nav/src/view_confirm.c -- the CONFIRM page (DESIGN.md 1.4).
 *
 * A transcription of mockup.py's render_confirm(), constant for constant. It is
 * the OVERVIEW page's cartography fitted to the PROPOSED route -- so the marks
 * are the shared ones from view_nav.c and only the framing and the strip differ:
 *
 *   my0 26 rather than 24 and pad 20 rather than 16, because there is no
 *   attribution or badge collision to dodge here and a proposal reads better
 *   with air around it;
 *   no dashed ridden track, because none of it has been ridden;
 *   the strip carries the one decision instead of five progress figures.
 *
 * Nothing on this page is below scale 2 (the panel's readable floor), which the
 * strip's four half-lines are laid out for: two rows of 18 px inside a 42 px
 * strip, and no fifth value competing for the space.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "arrows.h"
#include "draw.h"
#include "map.h"
#include "route.h"
#include "seg.h"
#include "view.h"

#define W SCR_W
#define H SCR_H

/* render_confirm()'s frame. */
#define CF_STRIP 42
#define CF_MY0 26
#define CF_MY1 (H - CF_STRIP - 1)
#define CF_PAD 20
/* DESIGN.md 1.4: "ETA assumes 17 km/h city riding until there is ride history
 * to do better." A whole ride log now exists (7.6) and could supply an average,
 * but a proposal made before the wheel has turned has no history OF THIS RIDE,
 * and using last week's average would make the same screen quote two different
 * numbers for the same route. One honest constant, stated. */
#define CF_EST_KMH 17.0
/* 33 characters at scale 2 is 396 px, which is the whole width -- the title is
 * cut, not shrunk, for the reason 6.5 gives on the OVERVIEW line: the number is
 * the part that changes and the name is the part you already know. */
#define CF_TITLE_CHARS 33

#define CF_MAXPTS 4096
static double f_world[2 * CF_MAXPTS];
static double f_path[2 * CF_MAXPTS];
static double f_segs[4 * CF_MAXPTS];
static double f_raw[2 * CF_MAXPTS];

static void
rtext(cov_t *c, int x_right, int y, const char *s, int scale, int ink)
{
    cov_text(c, x_right - cov_text_w(s, scale), y, s, scale, ink);
}

/* Every k'th vertex, both ends kept -- view_overview.c's thin(), repeated
 * rather than shared because both files are transcriptions of their own mockup
 * function and a shared helper would be a third place to keep in step. */
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

/* "827M" / "1.2KM", and the imperial pair on the same rule -- DESIGN.md 1.4 has
 * no imperial mockup, so this follows the metric layout rather than inventing
 * one. The metric threshold is the mockup's own 950 m (one decimal only once
 * "1.0KM" would be honest); the imperial one is 0.95 mi for the same reason. */
static void
fmt_total(char *buf, size_t n, double metres, int units)
{
    if (units == UNITS_IMPERIAL) {
        double ft = metres * GEO_FT_PER_M;
        if (ft < 0.95 * GEO_FT_PER_MILE)
            snprintf(buf, n, "%.0fFT", ft);
        else
            snprintf(buf, n, "%.1fMI", ft / GEO_FT_PER_MILE);
        return;
    }
    if (metres >= 950.0)
        snprintf(buf, n, "%.1fKM", metres / 1000.0);
    else
        snprintf(buf, n, "%.0fM", metres);
}

void
view_confirm(cov_t *c, const confirm_t *cf)
{
    double min_e, max_e, min_n, max_n, org_e, org_n, mpp, cx, cy, sy;
    double total = 0.0;
    /* dtxt holds two unrelated things: the fitted title (CF_TITLE_CHARS of it)
     * and a formatted distance. It was 24 bytes, which is ample for the second
     * and ten short of the first -- so a long destination was cut at 23
     * characters rather than at the 33 this page says it cuts at, and gcc 10's
     * -Wformat-truncation is what said so. No frozen frame moves: the mockup's
     * "TO SOI SUKHUMVIT 23" is 19 characters and fits either limit. */
    char buf[96], dtxt[CF_TITLE_CHARS + 1];
    int n, i, ns, nseg, mins;
    /* titlechars only ever SHRINKS from CF_TITLE_CHARS, so dtxt's sizing above
     * still bounds it and the %.*s cannot outrun the buffer. */
    const char *tolltxt;
    int titlechars;

    n = thin(cf->pts, cf->npts, f_raw, CF_MAXPTS);
    min_e = max_e = f_raw[0];
    min_n = max_n = f_raw[1];
    for (i = 1; i < n; i++) {
        if (f_raw[2 * i] < min_e)
            min_e = f_raw[2 * i];
        if (f_raw[2 * i] > max_e)
            max_e = f_raw[2 * i];
        if (f_raw[2 * i + 1] < min_n)
            min_n = f_raw[2 * i + 1];
        if (f_raw[2 * i + 1] > max_n)
            max_n = f_raw[2 * i + 1];
    }
    org_e = (min_e + max_e) / 2.0;
    org_n = (min_n + max_n) / 2.0;
    mpp = (max_e - min_e) / (W - 2.0 * CF_PAD);
    sy = (max_n - min_n) / (double)(CF_MY1 - CF_MY0 - 2 * CF_PAD);
    if (sy > mpp)
        mpp = sy;
    /* max(..., 1.0): a route 40 m long would otherwise be fitted at 0.1 m/px
     * and drawn as a wandering line across the whole screen. The mockup floors
     * it at one metre per pixel for the same reason. */
    if (!(mpp > 1.0))
        mpp = 1.0;
    cx = W / 2.0;
    cy = (CF_MY0 + CF_MY1) / 2.0;

    /* The whole route is ahead, so all of it is the cased "remaining" style --
     * there is no dashed track on this page because nothing has been ridden. */
    for (i = 0; i < n; i++) {
        f_world[2 * i] = f_raw[2 * i];
        f_world[2 * i + 1] = f_raw[2 * i + 1];
    }
    ns = map_round_corners(f_world, n, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                           f_path, CF_MAXPTS);
    map_project(f_path, ns, org_e, org_n, mpp, cx, cy, 0.0, f_path);
    nseg = map_clip_segs(f_path, ns, 0, CF_MY0, W - 1, CF_MY1, f_segs,
                         CF_MAXPTS);
    mark_cased_route(c, f_segs, nseg, 8, 4.5);

    map_project(f_raw, n, org_e, org_n, mpp, cx, cy, 0.0, f_raw);

    /* One dot per turn, paper then ink so a dot on the route still reads as a
     * dot. The destination is not among them: it gets the flag. */
    for (i = 0; i < cf->ncues; i++) {
        int v = cf->cue_idx[i];
        if (cf->npts > 1)
            v = (int)((long)v * (n - 1) / (cf->npts - 1));
        if (v < 0 || v >= n)
            continue;
        cov_disc(c, f_raw[2 * v], f_raw[2 * v + 1], 2.6, COV_PAPER);
        cov_disc(c, f_raw[2 * v], f_raw[2 * v + 1], 1.8, COV_INK);
    }

    /* Start marker and destination flag. The marker is the position marker at
     * r 11, not the OVERVIEW's ring-and-disc start mark: on this page the rider
     * IS at the start, and the chevron says which way they are facing. It points
     * straight up because a proposal made at a standstill has no course. */
    mark_position(c, f_raw[0], f_raw[1], 11, 0.0);
    arrow_draw(c, f_raw[2 * (n - 1)] - 11, f_raw[2 * (n - 1) + 1] - 22, 22,
               ARROW_DEST, COV_INK);

    mark_compass(c, W - 21, CF_MY0 + 4 + 21, 0.0, 11);
    mark_scale_bar(c, 7, CF_MY1 - 5, mpp, cf->units);

    /* The toll badge, and what it costs the title.
     *
     * Only a router that ANSWERED gets to put anything here. An offline route
     * cannot know -- the pack has no toll bit at all -- and OSRM does not say,
     * so both draw nothing rather than an absence the rider would read as NO.
     * That is also why NO TOLL is spelled out when it IS known: a blank right
     * margin has to keep meaning "not told", or the one case worth trusting
     * becomes indistinguishable from the three that cannot be. */
    tolltxt = cf->toll == ROUTE_TOLL_YES   ? "T TOLL"
              : cf->toll == ROUTE_TOLL_NO  ? "T NO TOLL"
                                           : "T TOLL?";
    /* Two characters of gap, taken off the title rather than overlaid on it.
     * The title is cut and not shrunk (CF_TITLE_CHARS), so it is the thing
     * with room to give -- and the destination is the part the rider already
     * knows, which is the argument 6.5 makes for cutting it in the first
     * place. */
    titlechars = CF_TITLE_CHARS;
    if (tolltxt)
        titlechars -= (int)strlen(tolltxt) + 2;
    snprintf(buf, sizeof buf, "TO %s", cf->dest ? cf->dest : "DESTINATION");
    snprintf(dtxt, sizeof dtxt, "%.*s", titlechars, buf);
    cov_text(c, 6, 5, dtxt, 2, COV_INK);
    if (tolltxt)
        rtext(c, W - 6, 5, tolltxt, 2, COV_INK);

    /* The length is measured off the geometry this page just drew rather than
     * taken from the caller, so the number and the picture cannot disagree. */
    for (i = 1; i < cf->npts; i++)
        total += hypot(cf->pts[2 * i] - cf->pts[2 * i - 2],
                       cf->pts[2 * i + 1] - cf->pts[2 * i - 1]);
    /* rint(), not +0.5: mockup.py rounds with Python's round(), which is
     * half-to-even, and rint() under the default FE_TONEAREST is the same rule.
     * The two differ on exactly one input in a million and this page is
     * byte-compared against that mockup. */
    mins = (int)rint(total / 1000.0 / CF_EST_KMH * 60.0);
    if (mins < 1)
        mins = 1; /* "EST 0 MIN" is not an estimate */

    cov_fill_rect(c, 0, H - CF_STRIP, W - 1, H - 1, COV_INK);
    fmt_total(dtxt, sizeof dtxt, total, cf->units);
    snprintf(buf, sizeof buf, "%s  EST %d MIN", dtxt, mins);
    cov_text(c, 6, H - CF_STRIP + 5, buf, 2, COV_PAPER);
    /* The transient of 7.5 takes the turn count, which is the figure on this
     * strip that can most afford a second and a half away: it is the one the
     * rider has already read by the time they are reaching for a key. */
    if (cf->note && *cf->note)
        snprintf(buf, sizeof buf, "%s", cf->note);
    else
        snprintf(buf, sizeof buf, "%d TURN%s", cf->ncues,
                 cf->ncues == 1 ? "" : "S");
    cov_text(c, 6, H - CF_STRIP + 23, buf, 2, COV_PAPER);
    rtext(c, W - 6, H - CF_STRIP + 5, "ENTER = GO", 2, COV_PAPER);
    /* The mode and the key that changes it, sharing row two's right column with
     * the cancel key. Right-aligned, so it sits clear of the turn count on the
     * left and entirely outside the x < 130 region the design gate holds
     * byte-exact -- but mockup.py draws the identical string, so the gate is
     * exact here too rather than merely within budget. */
    snprintf(buf, sizeof buf, "M %s  Q CANCEL",
             cf->mode == NAV_MODE_CAR ? "CAR" : "BIKE");
    rtext(c, W - 6, H - CF_STRIP + 23, buf, 2, COV_PAPER);
}

/* ------------------------------------------------------------ the demo */

/* mockup.py's page_confirm(): render_confirm(load_osm()[1]) -- the same Asok
 * route view_nav_tiles_demo() rides, proposed rather than under way. The
 * geometry is view_nav.c's ASOK_ROUTE (written by tests/gpx/gen_asok.py from
 * osm-asok.json's own carriageways) and the cues are the mockup's own turn_i
 * and pin_i; the destination cue is not in the list because it gets the flag.
 *
 * The array is duplicated rather than shared with view_nav.c because that one
 * is static to its own file and exporting it would make an 18-point demo
 * fixture part of the program's interface. Both are generated by the same
 * script from the same extract, and the design gate compares both frames. */
static const double CF_ROUTE[] = {
    0.0000,   0.0000,   1.7194,   35.7818,  8.6727,   76.5821,
    19.9623,  114.0441, 38.6377,  151.4619, 67.7809,  197.2697,
    86.2184,  253.8109, 125.6239, 289.9685, 142.8935, 337.3570,
    151.5121, 372.1329, 156.6811, 402.5425, 163.0072, 457.2819,
    208.5008, 421.3011, 266.2897, 378.5111, 299.4448, 354.3470,
    308.1608, 414.2486, 316.6171, 475.8858, 320.8778, 511.3470};
#define CF_NPTS ((int)(sizeof CF_ROUTE / sizeof CF_ROUTE[0] / 2))
static const int CF_CUES[] = {11, 14};

void
view_confirm_demo(cov_t *c)
{
    confirm_t cf;
    cf.pts = CF_ROUTE;
    cf.npts = CF_NPTS;
    cf.cue_idx = CF_CUES;
    cf.ncues = (int)(sizeof CF_CUES / sizeof CF_CUES[0]);
    cf.dest = "SOI SUKHUMVIT 23";
    cf.units = UNITS_METRIC; /* the frozen design state; see view_nav_demo() */
    /* BIKE, because that is the default and this is a bicycle navigator -- and
     * explicitly, because the strip reads it. */
    cf.mode = NAV_MODE_BIKE;
    cf.note = NULL;
    /* UNKNOWN, and explicitly for the same reason mode is explicit: the page
     * reads it. This is also the value that keeps the frozen frame frozen --
     * the badge draws nothing and takes no width, so mockup.py needs no
     * counterpart and the design gate stays byte-exact on this page. A demo
     * left uninitialised here would flap the gate on stack contents. */
    cf.toll = ROUTE_TOLL_UNKNOWN;
    view_confirm(c, &cf);
}

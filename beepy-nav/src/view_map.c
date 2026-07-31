/* beepy-nav/src/view_map.c -- the MAP page (DESIGN.md 1.5).
 *
 * A transcription of mockup.py's page_map() / page_map_wait() / map_strip(),
 * constant for constant. The design gate byte-compares these frames against the
 * mockup's own, so the literals below are the design: they are copied, not
 * re-derived.
 *
 * This is the page the program opens on when there is no route: where you are,
 * and nothing else. It reuses every mark of the NAV map -- mark_position,
 * mark_compass, mark_speed_badge, mark_scale_bar, mark_dashed and the tile blit
 * -- because it IS the same cartography, and a second copy of any of them would
 * be a second thing to keep in step with the mockup. Only the frame differs:
 *
 *   FULL WIDTH. There is no turn panel, because with no route there is no next
 *   turn, and an inverted empty third of the screen would be worse than none.
 *
 *   THE FIX SITS AT THE CENTRE, not at NAV's 0.72 of the height. That bias
 *   exists to give two thirds of the map to the road ahead of a route; with no
 *   route there is no ahead, and a where-am-I screen wants equal ground on
 *   every side of the marker.
 *
 *   THE STRIP CARRIES THE POSITION AND THE KEYS instead of five progress
 *   figures there is no route to have.
 *
 * Portable C, libc + libm. Nothing here touches the panel, so it renders in the
 * host lane and is in the design gate.
 */
#include <math.h>
#include <stdio.h>

#include "draw.h"
#include "map.h"
#include "seg.h"
#include "tile.h"
#include "view.h"

#define W SCR_W
#define H SCR_H

/* mockup.py's page_map() frame. */
#define MAPP_STRIP 42      /* with a position: coordinates + hints */
#define MAPP_STRIP_WAIT 24 /* without one: the hints alone         */
#define MAPP_Y1 (H - MAPP_STRIP - 1)
/* 21 px from the edge is where the NAV map puts the compass, but that page's
 * map starts at x 130 with a panel to its left. Here the needle's 21.6 px reach
 * would leave the frame whenever the rotation points it at the margin, so the
 * badge sits four pixels further in and nothing else moves. */
#define MAPP_COMPASS_X 25
#define MAPP_HINTS "F FIND   R ROUTES   Q QUIT"

/* The breadcrumb plus the position the page joins to it. MAP_TRACK_MAX (view.h)
 * is the cap the caller records to, so this is exactly one point of headroom and
 * not a second, independently chosen limit. */
#define MAPP_MAXPTS (MAP_TRACK_MAX + 1)
static double m_world[2 * MAPP_MAXPTS];
static double m_segs[4 * MAPP_MAXPTS];

/* The inverted bottom strip. Two scale-2 rows when there is a position -- where
 * you are, then the keys -- and the hint row alone when there is not.
 *
 * The hint sits at the same y either way (H - 19): the strip GROWS a row when
 * there is something to put in it, so the one line a rider navigates by never
 * moves. Nothing here is below scale 2, which is the measured floor of this
 * panel and the reason the route picker's own hint was enlarged.
 *
 * This row is also where the page advertises its keymap, and that is doing more
 * work than it looks: Tab is not bound here (there is no OVERVIEW without a
 * route) and neither are the letters the ride pages use, so a key that is not on
 * this line is not a dead key -- it is a key the page never claimed. */
static void
map_strip(cov_t *c, const livemap_t *m)
{
    int strip = m->have_pos ? MAPP_STRIP : MAPP_STRIP_WAIT;

    cov_fill_rect(c, 0, H - strip, W - 1, H - 1, COV_INK);
    if (m->have_pos) {
        char buf[48];
        /* A transient confirmation displaces the coordinates for about a second
         * and a half, by exactly the argument view_turn_panel() makes for
         * arrival: they change slowly, and they are not what the rider pressed
         * a key about. Five decimals is 1.1 m of latitude -- enough to read a
         * position off the screen and type it somewhere, and 18 characters at
         * scale 2 is 216 px of the 400. */
        if (m->note)
            snprintf(buf, sizeof buf, "%s", m->note);
        else
            snprintf(buf, sizeof buf, "%.5f %.5f", m->lat, m->lon);
        cov_text(c, 6, H - strip + 5, buf, 2, COV_PAPER);
        /* DESIGN.md 1.1.2's treatment, in the one row this page has for it. The
         * strip is solid ink, so a bar of paper is again the only emphasis
         * available -- and it sits BESIDE the coordinates rather than over them,
         * because a frozen position is still the last thing known and worth
         * reading. That is the difference from the turn panel, where the row it
         * takes had a competing value in it. */
        if (m->nofix) {
            int wd = cov_text_w("NO FIX", 2);
            cov_fill_rect(c, W - 12 - wd, H - strip + 3, W - 4,
                          H - strip + 20, COV_PAPER);
            cov_text(c, W - 8 - wd, H - strip + 5, "NO FIX", 2, COV_INK);
        }
    }
    cov_text(c, 6, H - 19, MAPP_HINTS, 2, COV_PAPER);
}

/* Before the first fix. Nothing of the map is drawn -- no marker, no compass,
 * no scale bar, no streets -- because there is nothing to centre one on, and a
 * map drawn about a guessed position is the one thing a navigator must never
 * put on a screen. An empty frame that says what it is waiting for is not a
 * lesser page; it is the only honest one. */
static void
map_wait(cov_t *c, const livemap_t *m)
{
    static const char WAIT[] = "WAITING FOR FIX";
    char buf[32];
    double cy = (H - MAPP_STRIP_WAIT) / 2.0;

    /* DESIGN.md 1.4.6: with a saved place to centre on, the streets are drawn
     * even though there is no fix -- indoors a receiver may never get one, and
     * an rider who knows the map around home can at least see where the
     * program thinks home is.
     *
     * NO MARKER, and that is the line this page will not cross. Everything
     * else on the waiting screen exists to refuse to claim a position, and a
     * chevron over these streets would be exactly the lie that argument was
     * about. The map is a picture of somewhere the rider named; the WAITING
     * FOR FIX above it still says nobody knows where they are. */
    if (m->tiles && m->have_home) {
        tileview_t tv;
        tv.mpp = m->mpp_manual > 0.0 ? m->mpp_manual : MAP_MPP_DEFAULT;
        tv.cx = W / 2.0;
        tv.cy = cy;
        tv.theta = 0.0; /* north-up: there is no course to be up */
        tv.org_e = m->home_e;
        tv.org_n = m->home_n;
        tv.x0 = 0;
        tv.y0 = 0;
        tv.x1 = W - 1;
        tv.y1 = H - MAPP_STRIP_WAIT - 1;
        tiles_blit(c, m->tiles, &tv);
    }

    /* Over a drawn map the message needs a floor to stand on: ink text on a
     * street grid is ink on ink, and the first render of this state was
     * genuinely unreadable. A PAPER bar, which is 1.1's own treatment for the
     * one thing on a page that must not be mistaken for content -- and it
     * doubles as the separation between "a picture of somewhere" and "a
     * sentence about what is not known". Sized to the wider line so it is the
     * same bar whether or not a satellite count has appeared yet.
     *
     * Not drawn without a map under it: on the empty waiting screen a white
     * bar on white is invisible, and every frozen golden of that state stays
     * byte-identical because this whole block is skipped. */
    if (m->tiles && m->have_home) {
        int tw = cov_text_w(WAIT, 3);
        cov_fill_rect(c, (W - tw) / 2.0 - 8, cy - 32, (W + tw) / 2.0 + 8,
                      cy + 30, COV_PAPER);
    }
    cov_text(c, (W - cov_text_w(WAIT, 3)) / 2, (int)(cy - 24), WAIT, 3,
             COV_INK);
    /* The satellite count only when the receiver is talking at all: a count of
     * zero is news, and no count at all is different news. The line above does
     * not move when it appears -- a message that jumps as the receiver warms up
     * reads as a glitch rather than as progress. */
    if (m->sats >= 0) {
        if (m->sats == 1)
            snprintf(buf, sizeof buf, "1 SATELLITE");
        else
            snprintf(buf, sizeof buf, "%d SATELLITES", m->sats);
        cov_text(c, (W - cov_text_w(buf, 2)) / 2, (int)(cy + 10), buf, 2,
                 COV_INK);
    }
    map_strip(c, m);
}

/* --------------------------------------------- saved places (1.4.6) ------
 *
 * Every shape here is drawn TWICE -- once fat in paper, then in ink -- which
 * is the route casing's trick from 6.5 and the only reason a thin outline
 * survives a street grid on a 1-bit screen. Without it these read as noise
 * over anything denser than a dual carriageway; the mockup that proved this
 * was drawn over the rider's own neighbourhood at 6 m/px.
 */
#define SV_HALO 1

/* Each shape draws its own PAPER INTERIOR first and its ink outline second, so
 * it reads as an object sitting on the map rather than as a wireframe with a
 * street grid showing through it. The first attempt outlined only, over the
 * rider's own neighbourhood, and the roads ran straight through the roof.
 *
 * `fill` is what distinguishes the two passes: the halo pass wants the whole
 * shape in paper, the real pass wants a paper body and an ink edge. */
static void
sv_house(cov_t *c, double x, double y, int ink, int fill)
{
    double roof[8];

    cov_fill_rect(c, x - 7, y - 2, x + 7, y + 8, fill ? ink : COV_PAPER);
    roof[0] = x - 9; roof[1] = y - 1;
    roof[2] = x;     roof[3] = y - 10;
    roof[4] = x + 9; roof[5] = y - 1;
    roof[6] = x - 9; roof[7] = y - 1;
    cov_poly(c, roof, 4, fill ? ink : COV_PAPER);
    if (fill)
        return;
    cov_polyline(c, roof, 4, 1.0, ink);
    cov_rect_outline(c, x - 7, y - 2, x + 7, y + 8, 1.0, ink);
}

static void
sv_case(cov_t *c, double x, double y, int ink, int fill)
{
    cov_fill_rect(c, x - 9, y - 3, x + 9, y + 8, fill ? ink : COV_PAPER);
    cov_fill_rect(c, x - 4, y - 7, x + 4, y - 3, fill ? ink : COV_PAPER);
    if (fill)
        return;
    cov_rect_outline(c, x - 9, y - 3, x + 9, y + 8, 1.0, ink);
    cov_rect_outline(c, x - 4, y - 7, x + 4, y - 3, 1.0, ink);
    /* The clasp: one stroke, and the only thing that stops a briefcase at this
     * size reading as a plain box. */
    cov_fill_rect(c, x - 2, y + 1, x + 2, y + 3, ink);
}

/* Anything the caller did not recognise. A filled diamond and NO letter: a
 * character in a ring is mush at this size -- that is what killed the first
 * candidate design -- and it is not needed, because every mark carries its
 * name on a bar underneath. The shape only has to say "a place you saved",
 * and which one is written directly below it. */
static void
sv_other(cov_t *c, double x, double y, int ink, int fill)
{
    double d[10];

    d[0] = x;     d[1] = y - 9;
    d[2] = x + 8; d[3] = y;
    d[4] = x;     d[5] = y + 9;
    d[6] = x - 8; d[7] = y;
    d[8] = x;     d[9] = y - 9;
    cov_poly(c, d, 5, fill ? ink : COV_PAPER);
    if (!fill)
        cov_polyline(c, d, 5, 1.0, ink);
}

static void
sv_shape(cov_t *c, const savedmark_t *sm, double x, double y, int ink,
         int fill)
{
    if (sm->kind == SAVED_HOME)
        sv_house(c, x, y, ink, fill);
    else if (sm->kind == SAVED_WORK)
        sv_case(c, x, y, ink, fill);
    else
        sv_other(c, x, y, ink, fill);
}

static void
map_saved(cov_t *c, const livemap_t *m, double mpp, double cx, double cy,
          double theta)
{
    double ct = cos(theta), st = sin(theta);
    int i, k;

    if (!m->saved)
        return;
    for (i = 0; i < m->nsaved && i < SAVED_MARK_MAX; i++) {
        const savedmark_t *sm = &m->saved[i];
        double de = sm->e - m->pos_e, dn = sm->n - m->pos_n;
        double x = cx + (de * ct - dn * st) / mpp;
        double y = cy - (de * st + dn * ct) / mpp;
        int nw;
        double half;

        /* Off the map: nothing. No edge tag, no arrow -- a mark is where the
         * place is, and a symbol pinned to the frame would be a different
         * claim in the same visual language. Zoom out and it appears. */
        if (x < 10 || x > W - 10 || y < 14 || y > MAPP_Y1 - 24)
            continue;

        for (k = 0; k < 9; k++)
            sv_shape(c, sm, x + (k % 3) - 1, y + (k / 3) - 1, COV_PAPER, 1);
        sv_shape(c, sm, x, y, COV_INK, 0);
        /* The stem, so the picture has a point: the icon sits above the spot
         * and this is the line that says which spot. */
        cov_stroke(c, x, y + 7, x, y + 12, 1.0, COV_PAPER);
        cov_stroke(c, x, y + 8, x, y + 12, 1.0, COV_INK);

        /* The name on a paper bar, for the reason 1.4.6 gives: with three
         * saved places the shapes alone stop being an answer. */
        if (!sm->name || !*sm->name)
            continue;
        nw = cov_text_w(sm->name, 1);
        half = nw / 2.0;
        cov_fill_rect(c, x - half - 2, y + 13, x + half + 1, y + 21,
                      COV_PAPER);
        cov_text(c, (int)(x - half), (int)(y + 15), sm->name, 1, COV_INK);
    }
}

void
view_map(cov_t *c, const livemap_t *m)
{
    double cx = W / 2.0;
    /* The centre of the map, which is what a where-am-I screen wants: the NAV
     * page's 0.72 exists to give the road ahead of a route two thirds of the
     * height, and with no route there is no ahead. */
    double cy = (H - MAPP_STRIP) / 2.0;
    double mpp = m->mpp_manual > 0.0 ? m->mpp_manual : MAP_MPP_DEFAULT;
    /* The direction the track ran INTO the fix. view_nav_map() takes its
     * fallback rotation from the route ahead of the fix; there is no ahead
     * here, so it comes from the leg just travelled -- which is what a
     * receiver's smoothed course reports anyway, and is the value the static
     * demo state and mockup.py rely on, neither having a receiver. */
    double geoh = 0.0, theta;
    int n = m->ntrack, ns, nseg, i;

    if (!m->have_pos) {
        map_wait(c, m);
        return;
    }
    if (n > MAP_TRACK_MAX)
        n = MAP_TRACK_MAX;
    if (n > 0) {
        double de = m->pos_e - m->track[2 * (n - 1)];
        double dn = m->pos_n - m->track[2 * (n - 1) + 1];
        if (fabs(de) + fabs(dn) > 1e-9)
            geoh = atan2(de, dn);
    }
    theta = m->course_up ? (m->have_heading ? m->heading : geoh) : 0.0;

    /* The basemap goes under everything, exactly as it does on the NAV page.
     * With no pack this is a single NULL test and the frame is unchanged. */
    if (m->tiles) {
        tileview_t tv;
        tv.mpp = mpp;
        tv.cx = cx;
        tv.cy = cy;
        tv.theta = theta;
        tv.org_e = m->pos_e;
        tv.org_n = m->pos_n;
        tv.x0 = 0;
        tv.y0 = 0;
        tv.x1 = W - 1;
        tv.y1 = MAPP_Y1;
        tiles_blit(c, m->tiles, &tv);
    }

    /* The breadcrumb: the track travelled this session, dashed exactly as the
     * NAV page dashes its ridden track, with the position joined on the end so
     * the dashes reach the marker.
     *
     * NOT corner-rounded, which the NAV page's track is: a GPX is a chain of
     * survey chords and reads as a polygon drawn raw, while a GPS trace is
     * already a dense wander and rounding it would invent bends the rider did
     * not take. */
    for (i = 0; i < n; i++) {
        m_world[2 * i] = m->track[2 * i];
        m_world[2 * i + 1] = m->track[2 * i + 1];
    }
    m_world[2 * n] = m->pos_e;
    m_world[2 * n + 1] = m->pos_n;
    ns = n + 1;
    if (ns > 1) {
        map_project(m_world, ns, m->pos_e, m->pos_n, mpp, cx, cy, theta,
                    m_world);
        nseg = map_clip_segs(m_world, ns, 0, 0, W - 1, MAPP_Y1, m_segs,
                             MAPP_MAXPTS);
        mark_dashed(c, m_segs, nseg, 5, 5, 1, COV_INK);
    }

    /* Saved places (DESIGN.md 1.4.6), UNDER the position marker and over
     * everything else. Under, because the one mark a rider must never lose is
     * the one that says where they are; a house drawn on top of the chevron
     * would be a picture obscuring a fact. */
    map_saved(c, m, mpp, cx, cy, theta);

    mark_position(c, cx, cy, 13, m->residual);
    mark_compass(c, MAPP_COMPASS_X, 27, theta, 11);
    mark_speed_badge(c, W - 33, 33, m->spd_kmh, 27, m->units);
    mark_scale_bar(c, 7, H - MAPP_STRIP - 6, mpp, m->units);
    map_strip(c, m);
}

/* ------------------------------------------------------------ the demo */

/* mockup.py's page_map(): ROUTE_M's first four points read as a track already
 * travelled rather than as a route to follow, with the fix at the fifth. There
 * is no route on this page, and a synthetic trace is the only honest fixture
 * for one -- the geometry turns 90 degrees into the fix, so the frozen frame
 * exercises a rotation the NAV goldens (a few degrees off north, where +h and
 * -h are indistinguishable) never could.
 *
 * The array is duplicated rather than shared with view_nav.c for the reason
 * view_confirm.c gives about its own: that one is static to its file, and
 * exporting a demo fixture would make it part of the program's interface. Both
 * are transcriptions of mockup.py's ROUTE_M, which the design gate compares. */
static const double MAP_DEMO_TRACK[] = {0, -900, 0, -400, 40, -150, 40, 260};
#define MAP_DEMO_NTRACK 4
#define MAP_DEMO_POS_E 190.0
#define MAP_DEMO_POS_N 260.0
/* The device's own desk (13.885 N, 100.378 E), so the coordinate row on the
 * golden is the string a rider sitting there would actually read. */
#define MAP_DEMO_LAT 13.88510
#define MAP_DEMO_LON 100.37850
#define MAP_DEMO_SATS 9

static void
map_demo_state(livemap_t *m)
{
    m->have_pos = 1;
    m->lat = MAP_DEMO_LAT;
    m->lon = MAP_DEMO_LON;
    m->pos_e = MAP_DEMO_POS_E;
    m->pos_n = MAP_DEMO_POS_N;
    m->track = MAP_DEMO_TRACK;
    m->ntrack = MAP_DEMO_NTRACK;
    m->spd_kmh = 24; /* the sample state's speed, as on the NAV page */
    m->course_up = 1;
    /* The frozen design states are metric and on the default rung, and must
     * stay so: they are what the design gate compares against mockup.py, which
     * has no units toggle and no keys. */
    m->units = UNITS_METRIC;
    m->mpp_manual = 0.0;
    /* A static state has no receiver and therefore no smoothed heading: the
     * rotation comes from the track's last leg and the chevron sits straight
     * up, which is exactly what mockup.py's page_map() draws. */
    m->heading = 0.0;
    m->have_heading = 0;
    m->residual = 0.0;
    m->tiles = NULL;
    m->nofix = 0;
    m->note = NULL;
    m->sats = MAP_DEMO_SATS;
    /* Explicit, and the reason is a segfault: livemap_t is a caller's local,
     * every demo state is built by this function, and a saved-place COUNT left
     * as whatever was on the stack sends map_saved() walking a wild pointer.
     * It crashed --page map-nofix the moment the field existed. */
    m->saved = NULL;
    m->nsaved = 0;
    m->have_home = 0;
    m->home_e = m->home_n = 0.0;
}

void
view_map_demo(cov_t *c, int nofix)
{
    livemap_t m;
    map_demo_state(&m);
    m.nofix = nofix;
    /* The frozen design states have no basemap, exactly as view_nav_demo()'s do
     * -- and this is not a detail: the demo path never calls
     * tiles_bind_route(), so a pack picked up from the config would be asked
     * about ROUTE_M's synthetic coordinates in its OWN frame and would happily
     * draw the streets that happen to be there. On the device, where basemap is
     * set in beepy-nav.conf, that is precisely what it did, and the golden moved.
     * view_map_tiles_demo() below is the state that WANTS a pack. */
    m.tiles = NULL;
    view_map(c, &m);
}

void
view_map_wait_demo(cov_t *c)
{
    livemap_t m;
    map_demo_state(&m);
    /* Everything above is still set, and none of it is drawn. That is the
     * point of the state: a page with a position it is not allowed to use. */
    m.have_pos = 0;
    m.tiles = NULL;
    m.have_home = 0;
    view_map(c, &m);
}

/* The same waiting state with a saved place to centre on (DESIGN.md 1.4.6).
 *
 * A state of its own rather than a flag, for view_map_wait_demo()'s reason
 * turned around: that golden's whole claim is that NO map is drawn, and this
 * one's is that a map IS -- and still no marker. Neither can stand for the
 * other, and a single frame could only assert one of them.
 *
 * The centre is the pack's own origin, so the streets it draws are the ones
 * mktiles.py cut around and the frame is reproducible from the fixture alone.
 */
void
view_map_wait_home_demo(cov_t *c, struct tiles *t)
{
    livemap_t m;
    map_demo_state(&m);
    m.have_pos = 0;
    m.tiles = t;
    m.have_home = 1;
    m.home_e = 0.0;
    m.home_n = 0.0;
    view_map(c, &m);
}

/* ------------------------------------------------- the basemap state (6.5)
 *
 * Real geometry, and the reason this is a separate state rather than a flag on
 * the one above: the MAP page is the first page whose world frame is a PACK'S
 * OWN reference instead of a route's first point (DESIGN.md 1.5), and the only
 * way to see that the translation is right is to put a position in that frame
 * over the pack cut for it.
 *
 * The first five points of the Asok route beepy-nav/tests/tiles/asok.tiles is
 * cut around -- read, again, as a breadcrumb and not as a route -- so the trace
 * runs north up Ratchadaphisek with the actual carriageways beneath it. It is in
 * the pack's frame by construction: tests/gpx/gen_asok.py wrote the GPX whose
 * first point mktiles.py referenced the pack to, so the offset is exactly zero.
 *
 * mockup.py has no reference for this state, exactly as it has none for
 * view_nav_tiles_demo(): the layer is a raster pack and the mockup draws
 * vectors. Its evidence is a golden of its own and T-MAP-BASEMAP. */
static const double MAP_TILES_TRACK[] = {
    0.0000,  0.0000,   1.7194,  35.7818,  8.6727, 76.5821,
    19.9623, 114.0441, 38.6377, 151.4619};
#define MAP_TILES_NTRACK 5
#define MAP_TILES_POS_E 67.7809
#define MAP_TILES_POS_N 197.2697
/* The same position in degrees, so the strip's row is the position the map is
 * drawn about rather than a second, unrelated fixture. Derived from the pack's
 * reference (13.73231, 100.55983) through geo_project()'s own constants:
 * lat = lat0 + n/110540, lon = lon0 + e/(111320 cos lat0). */
#define MAP_TILES_LAT 13.73409
#define MAP_TILES_LON 100.56046

void
view_map_tiles_demo(cov_t *c, struct tiles *t)
{
    livemap_t m;
    map_demo_state(&m);
    m.track = MAP_TILES_TRACK;
    m.ntrack = MAP_TILES_NTRACK;
    m.pos_e = MAP_TILES_POS_E;
    m.pos_n = MAP_TILES_POS_N;
    m.lat = MAP_TILES_LAT;
    m.lon = MAP_TILES_LON;
    m.tiles = t;
    view_map(c, &m);
}

/* The saved-place state (DESIGN.md 1.4.6). The basemap state's geometry, with
 * two places put at KNOWN offsets from the fix -- one of each recognised kind,
 * far enough apart to prove the marks do not collide and near enough to be on
 * screen at the default rung. A third at 4 km is deliberately OFF the map: the
 * golden's job is partly to show that it draws nothing.
 *
 * Offsets and not coordinates, because a golden that depended on where the
 * rider's real home is would be a golden nobody else could regenerate. */
static savedmark_t MAP_SAVED[] = {
    {"HOME", MAP_TILES_POS_E - 150.0, MAP_TILES_POS_N + 120.0, SAVED_HOME},
    {"WORK", MAP_TILES_POS_E + 130.0, MAP_TILES_POS_N - 90.0, SAVED_WORK},
    {"GYM", MAP_TILES_POS_E + 40.0, MAP_TILES_POS_N + 210.0, SAVED_OTHER},
    {"FAR", MAP_TILES_POS_E + 4000.0, MAP_TILES_POS_N, SAVED_HOME},
};

void
view_map_saved_demo(cov_t *c, struct tiles *t)
{
    livemap_t m;
    map_demo_state(&m);
    m.track = MAP_TILES_TRACK;
    m.ntrack = MAP_TILES_NTRACK;
    m.pos_e = MAP_TILES_POS_E;
    m.pos_n = MAP_TILES_POS_N;
    m.have_pos = 1;
    m.tiles = t;
    m.saved = MAP_SAVED;
    m.nsaved = (int)(sizeof MAP_SAVED / sizeof MAP_SAVED[0]);
    view_map(c, &m);
}


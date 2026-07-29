/* beepy-nav/src/view_nav.c -- the NAV page and the cue glyph sheet.
 *
 * A transcription of mockup.py's page_nav() + turn_panel() and the marks
 * they use (mark_position, mark_pin, mark_compass, speed_badge,
 * mark_scale_bar, mark_dashed, mark_cased_route). The M2 design gate
 * byte-compares the panel half of these frames against the mockup's own
 * PNGs, so the literals below are the design: they are copied, not
 * re-derived.
 */
#include <math.h>
#include <stdio.h>

#include "arrows.h"
#include "draw.h"
#include "map.h"
#include "seg.h"
#include "view.h"

#define W SCR_W
#define H SCR_H

/* Route geometry is short but corner rounding multiplies it; one static
 * scratch set keeps it off the stack and out of malloc. Rendering is
 * single-threaded by construction (one page per frame).
 *
 * 4096, not the 1024 M2 needed: a live route arrives as a windowed slice of
 * up to 600 vertices (nav.c), and map_round_corners() can turn each sharp
 * one into a dozen arc points. The cap is graceful -- it truncates the drawn
 * route rather than overrunning -- but a truncated route is a lie about
 * where the road goes, so the buffer is sized past the worst case. */
#define MAXPTS 4096
static double g_world[2 * MAXPTS];
static double g_scr[2 * MAXPTS];
static double g_segs[4 * MAXPTS];

/* ------------------------------------------------------------------ marks
 *
 * The six mark_* routines are shared with view_overview.c and declared in
 * view.h. They live here because this is where they were transcribed from
 * mockup.py, and because the OVERVIEW page is the same cartography at a
 * different zoom -- two copies would be two things to keep in step with the
 * mockup. speed_badge() stays private: only the NAV map has one. */

/* White disc, black ring, solid black chevron -- which is what the
 * reference resolves to at threshold. The white fill doubles as a halo: it
 * cuts the black route so the marker never merges into it. `ang` is the
 * residual course (raw course minus map rotation), so mid-turn the chevron
 * stays true while a smoothed course-up map catches up. */
void
mark_position(cov_t *c, double x, double y, double r, double ang)
{
    double k = r * 0.62, ca = cos(ang), sa = sin(ang), p[8], shape[4][2];
    int i;
    cov_disc(c, x, y, r + 2.2, COV_PAPER);
    cov_disc(c, x, y, r, COV_INK);
    cov_disc(c, x, y, r - 2.2, COV_PAPER);
    /* Built from k in the same order of operations as mockup.py -- folding
     * 0.62 * 0.85 into a literal shifts the last ulp, and a truncated 4x
     * coordinate can land a pixel elsewhere for it. */
    shape[0][0] = 0;
    shape[0][1] = k;
    shape[1][0] = k * 0.85;
    shape[1][1] = -k * 0.85;
    shape[2][0] = 0;
    shape[2][1] = -k * 0.32;
    shape[3][0] = -k * 0.85;
    shape[3][1] = -k * 0.85;
    for (i = 0; i < 4; i++) {
        double px = shape[i][0], py = shape[i][1];
        p[2 * i] = x + px * ca + py * sa;
        p[2 * i + 1] = y + px * sa - py * ca;
    }
    poly1x(c, p, 4, COV_INK);
}

/* Teardrop with a hole, tip at (x, y). */
void
mark_pin(cov_t *c, double x, double y, double r)
{
    double p[6];
    cov_disc(c, x, y - r, r + 1.6, COV_PAPER);
    p[0] = x - (r + 1.6) * 0.76;
    p[1] = y - r * 0.7;
    p[2] = x + (r + 1.6) * 0.76;
    p[3] = y - r * 0.7;
    p[4] = x;
    p[5] = y + 2.4;
    poly1x(c, p, 3, COV_PAPER);
    cov_disc(c, x, y - r, r, COV_INK);
    p[0] = x - r * 0.70;
    p[1] = y - r * 0.70;
    p[2] = x + r * 0.70;
    p[3] = y - r * 0.70;
    p[4] = x;
    p[5] = y + 1;
    poly1x(c, p, 3, COV_INK);
    cov_disc(c, x, y - r, r * 0.36, COV_PAPER);
}

static void
needle(cov_t *c, double x, double y, double nx, double ny, double px,
       double py, double b, double t, double hwidth, int ink)
{
    double p[6];
    p[0] = x + nx * t;
    p[1] = y + ny * t;
    p[2] = x + nx * b + px * hwidth;
    p[3] = y + ny * b + py * hwidth;
    p[4] = x + nx * b - px * hwidth;
    p[5] = y + ny * b - py * hwidth;
    poly1x(c, p, 3, ink);
}

/* Filled disc with a reversed N, plus a needle on the rim pointing to world
 * north -- on a course-up map the badge alone would be a lie. */
void
mark_compass(cov_t *c, double x, double y, double theta, double r)
{
    double nx = -sin(theta), ny = -cos(theta);
    double base = r + 2.5, tip = r + 9;
    double px = -ny, py = nx;
    cov_disc(c, x, y, r + 1.6, COV_PAPER);
    /* Detached from the badge by a 1 px white gap: fused to the rim it read
     * as a droplet, not a pointer. */
    needle(c, x, y, nx, ny, px, py, base - 1.4, tip + 1.6, 5.4, COV_PAPER);
    needle(c, x, y, nx, ny, px, py, base, tip, 4.0, COV_INK);
    cov_disc(c, x, y, r, COV_INK);
    cov_text(c, (int)rint(x - 6), (int)rint(y - 7), "N", 2, COV_PAPER);
}

/* Same ring construction as the position marker, so the two read as one
 * family of round instruments. */
static void
speed_badge(cov_t *c, double x, double y, int kmh, double r)
{
    char s[16]; /* "%d" of an int is up to 11 chars + NUL, whatever the km/h */
    int cap;
    cov_disc(c, x, y, r + 2.2, COV_PAPER);
    cov_disc(c, x, y, r, COV_INK);
    cov_disc(c, x, y, r - 2.2, COV_PAPER);
    snprintf(s, sizeof s, "%d", kmh < 0 ? 0 : kmh);
    cap = num_fit(s, 26, (int)(2 * (r - 7)));
    num_draw(c, x, y - 19, s, num_set_for_cap(cap), NUM_CT, COV_INK);
    cov_text(c, (int)rint(x - 12), (int)rint(y + 11), "KM/H", 1, COV_INK);
}

void
mark_scale_bar(cov_t *c, double x, double y, double mpp)
{
    char lbl[24]; /* the ladder tops out at "20KM", but %d could be anything */
    int m, px;
    map_scale_pick(mpp, &m, &px);
    cov_fill_rect(c, x - 2, y - 22, x + px + 2, y + 1, COV_PAPER);
    cov_fill_rect(c, x, y, x + px, y + 1, COV_INK); /* pixel-aligned: solid */
    cov_fill_rect(c, x, y - 5, x + 1, y, COV_INK);
    cov_fill_rect(c, x + px - 1, y - 5, x + px, y, COV_INK);
    if (m < 1000)
        snprintf(lbl, sizeof lbl, "%dM", m);
    else
        snprintf(lbl, sizeof lbl, "%dKM", m / 1000);
    cov_text(c, (int)rint(x + 2), (int)rint(y - 22), lbl, 2, COV_INK);
}

/* Ridden track. Thin, so it goes through the crisp hairline layer: a 1 px
 * diagonal has no room to anti-alias and would resolve to a broken line. */
void
mark_dashed(cov_t *c, const double *segs, int nsegs, double on, double off,
            double width, int ink)
{
    double carry = 0.0;
    int i;
    for (i = 0; i < nsegs; i++) {
        double x0 = segs[4 * i], y0 = segs[4 * i + 1];
        double x1 = segs[4 * i + 2], y1 = segs[4 * i + 3];
        double seg = hypot(x1 - x0, y1 - y0), pos = 0.0;
        if (seg < 0.01)
            continue;
        while (pos < seg) {
            double end = pos + (carry < on ? on : off);
            if (end > seg)
                end = seg;
            if (carry < on)
                cov_hairline(c, x0 + (x1 - x0) * pos / seg,
                             y0 + (y1 - y0) * pos / seg,
                             x0 + (x1 - x0) * end / seg,
                             y0 + (y1 - y0) * end / seg, width, ink);
            carry = fmod(carry + (end - pos), on + off);
            pos = end;
        }
    }
}

/* White casing under a black core: the route stays legible over streets. */
void
mark_cased_route(cov_t *c, const double *segs, int nsegs, double outer,
                 double inner)
{
    cov_stroke_segs(c, segs, nsegs, outer, COV_PAPER);
    cov_stroke_segs(c, segs, nsegs, inner, COV_INK);
}

/* ------------------------------------------------------------- turn panel */

/* Format an already-quantised distance (DESIGN.md 1.1.1). `m` is what the
 * rider is shown, not what was measured -- the ladder and the anti-jitter
 * latch live in route.c, so this only chooses the unit. */
static void
fmt_dist(int m, char *val, size_t n, const char **unit)
{
    if (m < 1000) {
        snprintf(val, n, "%d", m);
        *unit = "M";
    } else {
        snprintf(val, n, "%.1f", m / 1000.0);
        *unit = "KM";
    }
}

/* One centred scale-2 row of the panel's lower stack. */
static void
panel_row(cov_t *c, int y, const char *s)
{
    if (!s || !*s)
        return;
    cov_text(c, (PANEL_W - cov_text_w(s, 2)) / 2, y, s, 2, COV_PAPER);
}

void
view_turn_panel(cov_t *c, const panel_t *p)
{
    char buf[24];
    const char *unit;
    int cap;

    cov_fill_rect(c, 0, 0, PANEL_W - 1, H - 1, COV_INK);

    if (p->off) {
        /* Off route the junction distance is meaningless, so it is withheld
         * rather than frozen: a stale "410 M" beside a route you are not on
         * is the worst thing this panel could say. No helper text either --
         * the dotted tie-line on the map already points back. */
        label_draw(c, PANEL_W / 2.0, 10, "OFF", NUM_CT, COV_PAPER);
        label_draw(c, PANEL_W / 2.0, 44, "ROUTE", NUM_CT, COV_PAPER);
        snprintf(buf, sizeof buf, "%d", p->off);
        cap = num_fit(buf, 60, PANEL_W - 10);
        num_draw(c, PANEL_W / 2.0, 96, buf, num_set_for_cap(cap), NUM_CT,
                 COV_PAPER);
        label_draw(c, PANEL_W / 2.0, 96 + cap + 10, "M AWAY", NUM_CT,
                   COV_PAPER);
    } else {
        arrow_draw(c, (PANEL_W - 76) / 2.0, 6, 76, p->kind, COV_PAPER);
        fmt_dist(p->turn_m, buf, sizeof buf, &unit);
        /* PANEL_W - 8, not - 10: integer advances round a 3-digit NUM54
         * string to 119 px, and a 118 px limit would demote it to the 22 px
         * set by that single pixel. The margin is still 4+ px a side. */
        cap = num_fit(buf, 64, PANEL_W - 8);
        num_draw(c, PANEL_W / 2.0, 92, buf, num_set_for_cap(cap), NUM_CT,
                 COV_PAPER);
        num_draw(c, PANEL_W / 2.0, 92 + cap + 8, unit, UNITS_22, NUM_CT,
                 COV_PAPER);
    }

    /* Below the rule, three centred rows: how long is left, how far is left,
     * and when you arrive. This displaced the next-cue preview, which was a
     * 20 px glyph and a distance. The cue after the announced one is still
     * marked -- the teardrop on the map is exactly that pin (1.1) -- so what
     * went is the preview, not the information.
     *
     * Three rows and not the one "bottom line" the arrangement asks for,
     * because both values cannot share a line at a readable size: the budget
     * is ten characters (12 px each against a 128 px panel) and
     * "12.6KM 1:42PM" is thirteen. Stacked, each gets its full precision and
     * its label. 16 px cells at 192/208/224 exactly fill the space to the
     * bottom edge; the glyphs are 14 px of ink, so the rows stay separated. */
    cov_fill_rect(c, 6, H - 51, PANEL_W - 7, H - 51, COV_PAPER);

    panel_row(c, 192, p->remain);
    if (p->togo_m >= 0.0) {
        if (p->togo_m < 1000.0)
            snprintf(buf, sizeof buf, "%dM",
                     (int)p->togo_m - (int)p->togo_m % 50);
        else
            snprintf(buf, sizeof buf, "%.1fKM",
                     floor(p->togo_m / 100.0) / 10.0);
        panel_row(c, 208, buf);
        panel_row(c, 224, p->eta);
    } else {
        /* No route: nothing to count down to, so the pair that has always
         * been true of the device goes back in. */
        if (p->batt >= 0) {
            snprintf(buf, sizeof buf, "%d%%", p->batt);
            panel_row(c, 208, buf);
        }
        panel_row(c, 224, p->clock);
    }
}

/* -------------------------------------------------------------- the map */

void
view_nav_map(cov_t *c, const navmap_t *m)
{
    const double *pts = m->pts;
    int pos_i = m->pos_i, n = m->npts;
    double ae = pts[2 * pos_i], an = pts[2 * pos_i + 1];
    double be = pts[2 * pos_i + 2], bn = pts[2 * pos_i + 3];
    double on_e = ae + (be - ae) * m->pos_f;
    double on_n = an + (bn - an) * m->pos_f;
    double heading = atan2(be - on_e, bn - on_n);
    /* +heading, not -: map_project() rotates the world by +theta, so putting
     * the course at the top of the screen takes theta = heading. */
    double theta = m->course_up ? heading : 0.0;
    double pos_e = on_e, pos_n = on_n;
    double cx = MAP_X + (W - MAP_X) / 2.0;
    double cy = H * 0.72; /* the fix sits low: two thirds is road ahead */
    double mpp;
    int i, k, ns, nseg;

    if (m->off != 0.0) {
        pos_e += cos(heading) * m->off;
        pos_n += -sin(heading) * m->off;
    }
    mpp = map_auto_zoom(map_cue_distance(pts, n, on_e, on_n, pos_i, m->turn_i),
                        cy);

    /* Ridden track: everything behind the fix, plus the fix itself. */
    k = 0;
    for (i = 0; i <= pos_i && k < MAXPTS; i++, k++) {
        g_world[2 * k] = pts[2 * i];
        g_world[2 * k + 1] = pts[2 * i + 1];
    }
    if (m->off != 0.0 && k < MAXPTS) { /* a kink where the detour starts */
        g_world[2 * k] = pts[2 * pos_i] + (on_e - pts[2 * pos_i]) * 0.55;
        g_world[2 * k + 1] = pts[2 * pos_i + 1] + (on_n - pts[2 * pos_i + 1]) * 0.55;
        k++;
    }
    if (k < MAXPTS) {
        g_world[2 * k] = pos_e;
        g_world[2 * k + 1] = pos_n;
        k++;
    }
    ns = map_round_corners(g_world, k, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                           g_scr, MAXPTS);
    map_project(g_scr, ns, pos_e, pos_n, mpp, cx, cy, theta, g_scr);
    nseg = map_clip_segs(g_scr, ns, MAP_X, 0, W - 1, H - 1, g_segs, MAXPTS);
    mark_dashed(c, g_segs, nseg, 5, 5, 1, COV_INK);

    /* The route ahead, cased so it stays legible over the track. */
    g_world[0] = on_e;
    g_world[1] = on_n;
    k = 1;
    for (i = pos_i + 1; i < n && k < MAXPTS; i++, k++) {
        g_world[2 * k] = pts[2 * i];
        g_world[2 * k + 1] = pts[2 * i + 1];
    }
    ns = map_round_corners(g_world, k, MAP_CORNER_RADIUS, MAP_CORNER_MIN_DEG,
                           g_scr, MAXPTS);
    map_project(g_scr, ns, pos_e, pos_n, mpp, cx, cy, theta, g_scr);
    nseg = map_clip_segs(g_scr, ns, MAP_X, 0, W - 1, H - 1, g_segs, MAXPTS);
    mark_cased_route(c, g_segs, nseg, 10, 6);

    if (m->off != 0.0) {
        /* Dotted tie-line back to the route: it says which way, without
         * pretending to know a way back. */
        double ox, oy, en[2], xy[2];
        int t;
        en[0] = on_e;
        en[1] = on_n;
        map_project(en, 1, pos_e, pos_n, mpp, cx, cy, theta, xy);
        ox = xy[0];
        oy = xy[1];
        for (t = 0; t < 100; t += 7)
            cov_disc(c, cx + (ox - cx) * t / 100.0, cy + (oy - cy) * t / 100.0,
                     1.5, COV_INK);
    } else if (m->pin_i > 0 && m->pin_i < n) {
        /* The pin marks the cue AFTER the announced one: the announced
         * junction is already the bend plus the whole left panel. */
        double en[2], xy[2];
        en[0] = pts[2 * m->pin_i];
        en[1] = pts[2 * m->pin_i + 1];
        map_project(en, 1, pos_e, pos_n, mpp, cx, cy, theta, xy);
        if (MAP_X + 10 < xy[0] && xy[0] < W - 10 && 24 < xy[1] &&
            xy[1] < H - 10)
            mark_pin(c, xy[0], xy[1], 10);
    }

    mark_position(c, cx, cy, 13, 0.0);
    mark_compass(c, MAP_X + 21, 27, theta, 11);
    speed_badge(c, W - 33, 33, m->spd_kmh, 27);
    mark_scale_bar(c, MAP_X + 7, H - 8, mpp);
}

void
view_nav(cov_t *c, const navmap_t *m, const panel_t *p)
{
    view_nav_map(c, m);
    view_turn_panel(c, p);
    cov_fill_rect(c, PANEL_W, 0, PANEL_W, H - 1, COV_INK); /* the divider */
    /* The 1 px gutter between the divider and the map (DESIGN.md 1.1 gives
     * the map x 130-399), cleared explicitly.
     *
     * The map's geometry is clipped to x >= MAP_X, but the CASING is 10 px
     * wide, so a route running down the left edge lays its outer stroke five
     * pixels further left than any clipped coordinate. The panel covers
     * 0-127 and the divider 128; column 129 is the one place that spill can
     * still be seen. No mockup has a route reaching that edge, which is why
     * this never showed -- view_cliptest() is the page that makes it show,
     * and this is the line that answers it. Every existing golden already
     * has this column white, so clearing it costs nothing. */
    cov_fill_rect(c, PANEL_W + 1, 0, PANEL_W + 1, H - 1, COV_PAPER);
}

/* ------------------------------------------------------------ the demo */

/* The stretch ahead of the fix is the reference's composition: straight on
 * for 410 m, a right turn, 150 m east, then north again. */
static const double DEMO_ROUTE[] = {
    0,    -900, 0,    -400, 40,   -150, 40,   260,  190, 260,
    190,  620,  430,  700,  560,  980,  430,  1180, 120, 1260,
    -220, 1210, -430, 1040, -470, 780,  -330, 560,  -120, 520};
#define DEMO_NPTS ((int)(sizeof DEMO_ROUTE / sizeof DEMO_ROUTE[0] / 2))

void
view_nav_demo(cov_t *c, int off)
{
    navmap_t m;
    panel_t p;

    m.pts = DEMO_ROUTE;
    m.npts = DEMO_NPTS;
    m.pos_i = 2;
    m.pos_f = 0.0;
    m.turn_i = 3;
    m.pin_i = 4;
    m.off = off;
    m.spd_kmh = 24;
    m.course_up = 1;

    p.off = off;
    /* The demo carries the raw metres the design's sample state names, and
     * quantises them here exactly as the live path does -- so the reference
     * frames show what a rider would actually see (1.1.1): 410 -> "400". */
    p.turn_m = cue_quantise(410);
    p.kind = ARROW_RIGHT;
    p.remain = "44 MIN";  /* 12.6 km at the design's 17 km/h city average */
    p.eta = "10:42 ETA";
    p.togo_m = 12600; /* the design's sample state: 1.2's strip says 12.6 KM */
    p.batt = 86;
    p.clock = "09:40";

    view_nav(c, &m, &p);
}

/* ----------------------------------------------------------- clip test */

/* DESIGN.md 10: "render at a zoom where the route leaves the map on all four
 * sides; assert no ink lands in x < 130". The turn panel is the only part of
 * this screen that is never allowed to be painted over, and a clipping bug is
 * the one thing that would destroy it.
 *
 * Auto-zoom lands on 6 m/px because the announced cue is 800 m ahead: that is
 * the coarsest rung keeping it inside 0.8 * 172.8 px, which is 829 m there.
 * The visible world is then e in [-810, 804] and n in [-397, 1037], and every
 * leg below is placed to CROSS one of those bounds rather than merely to sit
 * outside it -- a segment wholly outside is rejected by the bbox test and
 * proves nothing about the clipper.
 *
 *   track  (0,-3000) -> the fix          crosses the BOTTOM
 *   3 -> 4 (0,800) -> (2000,800)         crosses the RIGHT
 *   5 -> 6 (2000,500) -> (-2000,500)     crosses RIGHT then LEFT
 *   8 -> 9 (0,2500) -> (0,-100)          crosses the TOP coming back down
 */
static const double CLIP_ROUTE[] = {
    0,     -3000, 0,     -1000, 0,     0,    0, 800, 2000, 800,
    2000,  500,   -2000, 500,   -2000, 2500, 0, 2500, 0,   -100};
#define CLIP_NPTS ((int)(sizeof CLIP_ROUTE / sizeof CLIP_ROUTE[0] / 2))

static void
clip_panel(panel_t *p)
{
    p->off = 0;
    p->turn_m = cue_quantise(800);
    p->kind = ARROW_LEFT;
    p->remain = "44 MIN";
    p->eta = "10:42 ETA";
    p->togo_m = 12600;
    p->batt = 86;
    p->clock = "09:40";
}

void
view_cliptest(cov_t *c)
{
    navmap_t m;
    panel_t p;
    m.pts = CLIP_ROUTE;
    m.npts = CLIP_NPTS;
    m.pos_i = 2;
    m.pos_f = 0.0;
    m.turn_i = 3;
    m.pin_i = 4;
    m.off = 0;
    m.spd_kmh = 31;
    m.course_up = 1;
    clip_panel(&p);
    view_nav(c, &m, &p);
}

/* The same panel with NO map behind it. The test is the comparison: every
 * pixel of x < 130 must be identical between the two, which says the map put
 * nothing there that the panel did not. Asserting the region is merely blank
 * would be weaker -- it would also pass if the map had erased the panel. */
void
view_cliptest_panel(cov_t *c)
{
    navmap_t m;
    panel_t p;
    static const double NONE[] = {0, 0, 0, 1};
    m.pts = NONE;
    m.npts = 2;
    m.pos_i = 0;
    m.pos_f = 0.0;
    m.turn_i = 1;
    m.pin_i = -1;
    m.off = 0;
    m.spd_kmh = 31;
    m.course_up = 1;
    clip_panel(&p);
    view_nav(c, &m, &p);
}

/* ------------------------------------------------------- cue glyph sheet */

void
view_arrows(cov_t *c)
{
    static const char *const CAP = "76 / 40 / 24 / 16 PX";
    int i, x = 4;

    cov_fill_rect(c, 0, 0, W - 1, 17, COV_INK);
    cov_text(c, 4, 5, "CUE GLYPH SET", 1, COV_PAPER);
    cov_text(c, W - 4 - cov_text_w(CAP, 1), 5, CAP, 1, COV_PAPER);
    for (i = 0; i < ARROW_N; i++) {
        arrow_draw(c, x, 24, 40, i, COV_INK);
        arrow_draw(c, x + 8, 70, 24, i, COV_INK);
        arrow_draw(c, x + 12, 100, 16, i, COV_INK);
        cov_text(c, x, 120, ARROW_CAPTIONS[i], 1, COV_INK);
        x += 44;
    }
    cov_fill_rect(c, 0, 133, W - 1, 133, COV_INK);
    arrow_draw(c, 4, 140, 76, ARROW_RIGHT, COV_INK);
    /* Full size means the NAV page's size: the 54 px set. The unit drops to
     * y 208 to keep clear of the taller digits. */
    num_draw(c, 88, 146, "410", NUM_54, NUM_LT, COV_INK);
    num_draw(c, 88, 208, "M", UNITS_22, NUM_LT, COV_INK);
    cov_text(c, 208, 150, "PANEL ARROW AND", 2, COV_INK);
    cov_text(c, 208, 172, "DISTANCE AT FULL", 2, COV_INK);
    cov_text(c, 208, 194, "SIZE, AS THE NAV", 2, COV_INK);
    cov_text(c, 208, 216, "PAGE DRAWS THEM", 2, COV_INK);
}

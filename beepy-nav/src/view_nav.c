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
#include "tile.h"
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
speed_badge(cov_t *c, double x, double y, int kmh, double r, int units)
{
    char s[16]; /* "%d" of an int is up to 11 chars + NUL, whatever the km/h */
    /* Centred by the same arithmetic the metric literal encoded: cov_text_w
     * is strlen * 6 * scale, so "KM/H" is 24 px and half of it is the 12 the
     * mockup hard-coded. Writing it out keeps "MPH" centred too, instead of
     * needing a second magic number beside the first. */
    const char *lbl = units == UNITS_IMPERIAL ? "MPH" : "KM/H";
    int cap, v = kmh < 0 ? 0 : kmh;
    if (units == UNITS_IMPERIAL)
        v = (int)(v * (1000.0 * GEO_FT_PER_M / GEO_FT_PER_MILE) + 0.5);
    cov_disc(c, x, y, r + 2.2, COV_PAPER);
    cov_disc(c, x, y, r, COV_INK);
    cov_disc(c, x, y, r - 2.2, COV_PAPER);
    snprintf(s, sizeof s, "%d", v);
    cap = num_fit(s, 26, (int)(2 * (r - 7)));
    num_draw(c, x, y - 19, s, num_set_for_cap(cap), NUM_CT, COV_INK);
    cov_text(c, (int)rint(x) - cov_text_w(lbl, 1) / 2, (int)rint(y + 11), lbl,
             1, COV_INK);
}

void
mark_scale_bar(cov_t *c, double x, double y, double mpp, int units)
{
    char lbl[24]; /* the ladder tops out at "20KM", but %d could be anything */
    int m, px;

    if (units == UNITS_IMPERIAL) {
        /* `m` is feet here. Under a mile it is printed as feet; at or above
         * one, as miles -- whole where the rung is a whole mile, a tenth
         * where it is the half-mile rung. Same shape as the metric M/KM
         * switch, which is the only rule imperial has to be consistent with:
         * there is no imperial mockup to transcribe. */
        map_scale_pick_ft(mpp, &m, &px);
        if (m < (int)MAP_FT_PER_MILE)
            snprintf(lbl, sizeof lbl, "%dFT", m);
        else if (m % (int)MAP_FT_PER_MILE == 0)
            snprintf(lbl, sizeof lbl, "%dMI", m / (int)MAP_FT_PER_MILE);
        else
            snprintf(lbl, sizeof lbl, "%.1fMI", m / MAP_FT_PER_MILE);
    } else {
        map_scale_pick(mpp, &m, &px);
        if (m < 1000)
            snprintf(lbl, sizeof lbl, "%dM", m);
        else
            snprintf(lbl, sizeof lbl, "%dKM", m / 1000);
    }
    cov_fill_rect(c, x - 2, y - 22, x + px + 2, y + 1, COV_PAPER);
    cov_fill_rect(c, x, y, x + px, y + 1, COV_INK); /* pixel-aligned: solid */
    cov_fill_rect(c, x, y - 5, x + 1, y, COV_INK);
    cov_fill_rect(c, x + px - 1, y - 5, x + px, y, COV_INK);
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
 * latch live in route.c, so this only chooses the unit. Under UNITS_IMPERIAL
 * it arrives in FEET, because that is the ladder cue_quantise_u() counted on.
 *
 * Four-digit feet (the 100 ft rungs between 500 ft and a mile) are wider than
 * the 54 px numerals can be at 128 px, so num_fit() demotes them to the 22 px
 * set. That is not an imperial quirk: four-digit kilometres -- a cue more than
 * 10 km out -- demote in exactly the same way and always have. Imperial
 * inherits the metric layout rule rather than inventing one. */
static void
fmt_dist(int m, int units, char *val, size_t n, const char **unit)
{
    if (units == UNITS_IMPERIAL) {
        if (m < GEO_FT_PER_MILE) {
            snprintf(val, n, "%d", m);
            *unit = "FT";
        } else {
            snprintf(val, n, "%.1f", m / (double)GEO_FT_PER_MILE);
            *unit = "MI";
        }
        return;
    }
    if (m < 1000) {
        snprintf(val, n, "%d", m);
        *unit = "M";
    } else {
        snprintf(val, n, "%.1f", m / 1000.0);
        *unit = "KM";
    }
}

/* The whole-route row, which is a different number with a different job: it
 * is glanced at, not acted on, so it is coarser than the countdown and does
 * not latch. Metric floors to 50 m below a kilometre and to 0.1 km above;
 * imperial floors to 100 ft below a mile and to 0.1 mi above -- the same two
 * rungs' worth of precision, in the numbers a rider who thinks in miles
 * reads. */
static void
fmt_togo(char *buf, size_t n, double metres, int units)
{
    if (units == UNITS_IMPERIAL) {
        double ft = metres * GEO_FT_PER_M;
        if (ft < GEO_FT_PER_MILE)
            snprintf(buf, n, "%dFT", (int)ft - (int)ft % 100);
        else
            snprintf(buf, n, "%.1fMI",
                     floor(ft / (GEO_FT_PER_MILE / 10.0)) / 10.0);
        return;
    }
    if (metres < 1000.0)
        snprintf(buf, n, "%dM", (int)metres - (int)metres % 50);
    else
        snprintf(buf, n, "%.1fKM", floor(metres / 100.0) / 10.0);
}

/* How far below the digits' top the unit line goes.
 *
 * mockup.py stacks it at the cap it ASKED num_fit() for, and where the string
 * fits at that size the two are the same thing. Where it does not -- four-digit
 * feet, or a cue more than 10 km out, both of which num_fit() demotes to the
 * 22 px set -- the asked-for cap is 30 px taller than the ink that got drawn,
 * and the unit floats away from the number it belongs to. Then, and only then,
 * the stack closes up to the size actually used.
 *
 * The condition is exact rather than approximate: num_width() is the same for
 * every cap in a set, so num_fit() either returns cap0 or falls straight
 * through to the set below it. No mockup renders a demoted string, so every
 * frozen frame takes the first branch and is untouched. */
static int
stack_cap(int cap, int cap0)
{
    return cap == cap0 ? cap : num_cap(num_set_for_cap(cap));
}

/* One centred scale-2 row of the panel's lower stack. */
static void
panel_row(cov_t *c, int y, const char *s)
{
    if (!s || !*s)
        return;
    cov_text(c, (PANEL_W - cov_text_w(s, 2)) / 2, y, s, 2, COV_PAPER);
}

/* DESIGN.md 1.1: "NO FIX replaces the bottom row, inverted, when the fix is
 * lost". A paper bar with ink text, filling the 16 px cell the arrival row
 * occupies -- the panel is solid ink, so a bar of paper is the one treatment
 * on this screen that cannot be read as ordinary content, and with two pixel
 * words (4) it is the only emphasis there is. A row of paper text in the same
 * place would look exactly like the ETA it replaced.
 *
 * It is drawn in the arrival row's cell rather than added below it because
 * the three rows already fill the panel to the bottom edge (1.1); there is no
 * spare line, and arrival is the row this panel can most afford to lose -- the
 * same argument 7.5 makes for the transient. It outranks that transient:
 * "ALERTS OFF" is a preference the rider expressed two seconds ago, and NO FIX
 * is a fault they do not yet know about. */
static void
panel_nofix(cov_t *c)
{
    cov_fill_rect(c, 0, H - 16, PANEL_W - 1, H - 1, COV_PAPER);
    cov_text(c, (PANEL_W - cov_text_w("NO FIX", 2)) / 2, H - 16, "NO FIX", 2,
             COV_INK);
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
        /* Metres in -- everything inside the program is -- feet on the
         * panel when the rider asked for feet. Showing "85 M AWAY" under an
         * otherwise imperial display would be the one number on the screen
         * in the wrong unit, which is worse than either choice made
         * consistently. */
        snprintf(buf, sizeof buf, "%d",
                 p->units == UNITS_IMPERIAL
                     ? (int)(p->off * GEO_FT_PER_M + 0.5)
                     : p->off);
        cap = num_fit(buf, 60, PANEL_W - 10);
        num_draw(c, PANEL_W / 2.0, 96, buf, num_set_for_cap(cap), NUM_CT,
                 COV_PAPER);
        label_draw(c, PANEL_W / 2.0, 96 + stack_cap(cap, 60) + 10,
                   p->units == UNITS_IMPERIAL ? "FT AWAY" : "M AWAY", NUM_CT,
                   COV_PAPER);
    } else {
        arrow_draw(c, (PANEL_W - 76) / 2.0, 6, 76, p->kind, COV_PAPER);
        fmt_dist(p->turn_m, p->units, buf, sizeof buf, &unit);
        /* PANEL_W - 8, not - 10: integer advances round a 3-digit NUM54
         * string to 119 px, and a 118 px limit would demote it to the 22 px
         * set by that single pixel. The margin is still 4+ px a side. */
        cap = num_fit(buf, 64, PANEL_W - 8);
        num_draw(c, PANEL_W / 2.0, 92, buf, num_set_for_cap(cap), NUM_CT,
                 COV_PAPER);
        num_draw(c, PANEL_W / 2.0, 92 + stack_cap(cap, 64) + 8, unit, UNITS_22,
                 NUM_CT, COV_PAPER);
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

    /* The bottom row is where a transient confirmation goes. It displaces
     * arrival, which is the row that can most afford a second and a half
     * away: it changes slowly, it is a prediction rather than a fact, and it
     * is the one of the three a rider is least likely to be reading at the
     * exact moment they pressed a key. Nothing permanent is added -- panel
     * space is the scarce resource, and a setting the rider chose two seconds
     * ago does not need continuous display. */
    panel_row(c, 192, p->remain);
    if (p->togo_m >= 0.0) {
        fmt_togo(buf, sizeof buf, p->togo_m, p->units);
        panel_row(c, 208, buf);
    } else if (p->batt >= 0) {
        /* No route: nothing to count down to, so the pair that has always
         * been true of the device goes back in. */
        snprintf(buf, sizeof buf, "%d%%", p->batt);
        panel_row(c, 208, buf);
    }
    /* The bottom row, in order of precedence: a lost fix, then a transient
     * confirmation, then the value the row belongs to. */
    if (p->nofix)
        panel_nofix(c);
    else
        panel_row(c, 224,
                  p->note ? p->note : (p->togo_m >= 0.0 ? p->eta : p->clock));
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
    /* The direction the ROUTE runs at the fix. It is the axis the off-route
     * offset is measured against, and it is the fallback map rotation for a
     * caller with no smoothed heading to give (the demo pages, and mockup.py,
     * which has no receiver). */
    double geoh = atan2(be - on_e, bn - on_n);
    /* +theta, not -: map_project() rotates the world by +theta, so putting
     * the course at the top of the screen takes theta = heading. */
    double theta =
        m->course_up ? (m->have_heading ? m->heading : geoh) : 0.0;
    double pos_e = on_e, pos_n = on_n;
    double cx = MAP_X + (W - MAP_X) / 2.0;
    double cy = H * 0.72; /* the fix sits low: two thirds is road ahead */
    double mpp;
    int i, k, ns, nseg;

    if (m->off != 0.0) {
        pos_e += cos(geoh) * m->off;
        pos_n += -sin(geoh) * m->off;
    }
    /* Auto unless a key has said otherwise (DESIGN.md 6.1). Manual zoom is a
     * rider decision and outranks the cue-fitting rule entirely -- including
     * its promise that the junction stays on screen, which is exactly what
     * somebody pressing Z to see the wider picture is choosing to give up. */
    mpp = m->mpp_manual > 0.0
              ? m->mpp_manual
              : map_auto_zoom(
                    map_cue_distance(pts, n, on_e, on_n, pos_i, m->turn_i),
                    cy);

    /* The basemap goes UNDER everything, which is the whole reason the route
     * is cased (DESIGN.md 1.1): streets run beneath it and the white outer
     * stroke is what keeps the black core readable crossing them. With no
     * pack this is a single NULL test and the frame is unchanged -- the five
     * original nav goldens are the standing proof of that. */
    if (m->tiles) {
        tileview_t tv;
        tv.mpp = mpp;
        tv.cx = cx;
        tv.cy = cy;
        tv.theta = theta;
        tv.org_e = pos_e;
        tv.org_n = pos_n;
        tv.x0 = MAP_X;
        tv.y0 = 0;
        tv.x1 = W - 1;
        tv.y1 = H - 1;
        tiles_blit(c, m->tiles, &tv);
    }

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

    mark_position(c, cx, cy, 13, m->residual);
    mark_compass(c, MAP_X + 21, 27, theta, 11);
    speed_badge(c, W - 33, 33, m->spd_kmh, 27, m->units);
    mark_scale_bar(c, MAP_X + 7, H - 8, mpp, m->units);
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
view_nav_demo(cov_t *c, int off, int nofix)
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
    /* A static state has no receiver and therefore no lag to show: the map
     * rotation comes from the route and the chevron sits straight up, which
     * is exactly what mockup.py's page_nav() draws. */
    m.heading = 0.0;
    m.have_heading = 0;
    m.residual = 0.0;
    /* The frozen design states are metric and auto-zoom, and must stay so:
     * they are what the design gate compares against mockup.py, which has no
     * units toggle and no keys. */
    m.units = UNITS_METRIC;
    m.mpp_manual = 0.0;
    m.tiles = NULL; /* the frozen design states have no basemap */

    p.off = off;
    p.units = UNITS_METRIC;
    p.note = NULL;
    /* The frozen NO FIX state draws the same map and the same countdown as
     * the turn page: that is the point of it. A lost fix does not blank the
     * screen, it stops the numbers moving and says so on one row -- and the
     * only way to see that the rest is unchanged is to render the pair. */
    p.nofix = nofix;
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

/* ------------------------------------------------- the basemap demo (6.5)
 *
 * Real geometry, not the synthetic staircase: this is the Asok / Sukhumvit
 * route mockup.py's load_osm() builds out of osm-asok.json's own carriageways
 * -- north up Ratchadaphisek, right at the junction, left into Soi Sukhumvit
 * 23 -- which is the composition nav-turn-osm.png shows and the route
 * beepy-nav/tests/tiles/asok.tiles is cut around. It is in the ROUTE frame,
 * referenced to its own first point exactly as route.c would project it, so
 * the pack's (lat0, lon0) lands on the origin and the two frames coincide
 * with no offset at all.
 *
 * Written by `python3 tests/gpx/gen_asok.py --c`; the GPX beside it is the
 * same 18 points and is what tools/mktiles.py cuts the corridor along.
 */
static const double ASOK_ROUTE[] = {
    0.0000,   0.0000,   1.7194,   35.7818,  8.6727,   76.5821,
    19.9623,  114.0441, 38.6377,  151.4619, 67.7809,  197.2697,
    86.2184,  253.8109, 125.6239, 289.9685, 142.8935, 337.3570,
    151.5121, 372.1329, 156.6811, 402.5425, 163.0072, 457.2819,
    208.5008, 421.3011, 266.2897, 378.5111, 299.4448, 354.3470,
    308.1608, 414.2486, 316.6171, 475.8858, 320.8778, 511.3470};
#define ASOK_NPTS ((int)(sizeof ASOK_ROUTE / sizeof ASOK_ROUTE[0] / 2))

void
view_nav_tiles_demo(cov_t *c, struct tiles *t)
{
    navmap_t m;
    panel_t p;

    m.pts = ASOK_ROUTE;
    m.npts = ASOK_NPTS;
    /* Two vertices in, so there is a ridden track for the streets to run
     * under and the layer order is actually visible. The cue is then 420 m
     * away, which puts auto-zoom on the 4 m/px rung -- a rung the fixture
     * pack carries, chosen rather than forced so the pack and the zoom ladder
     * have to agree the way they will on a ride. */
    m.pos_i = 2;
    m.pos_f = 0.0;
    m.turn_i = 11; /* the Sukhumvit junction */
    m.pin_i = 14;  /* the mouth of the soi, which is the cue after it */
    m.off = 0;
    m.spd_kmh = 24;
    m.course_up = 1;
    m.heading = 0.0;
    m.have_heading = 0;
    m.residual = 0.0;
    m.units = UNITS_METRIC;
    m.mpp_manual = 0.0;
    m.tiles = t;

    p.off = 0;
    p.units = UNITS_METRIC;
    p.note = NULL;
    p.nofix = 0;
    p.turn_m = cue_quantise(420);
    p.kind = ARROW_RIGHT;
    p.remain = "3 MIN";
    p.eta = "10:42 ETA";
    p.togo_m = 827;
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
    p->units = UNITS_METRIC;
    p->note = NULL;
    p->nofix = 0;
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
    m.heading = 0.0;
    m.have_heading = 0;
    m.residual = 0.0;
    m.units = UNITS_METRIC;
    m.mpp_manual = 0.0;
    m.tiles = NULL;
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
    m.heading = 0.0;
    m.have_heading = 0;
    m.residual = 0.0;
    m.units = UNITS_METRIC;
    m.mpp_manual = 0.0;
    m.tiles = NULL;
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

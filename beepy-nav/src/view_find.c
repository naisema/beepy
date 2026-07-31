/* beepy-nav/src/view_find.c -- the FIND page (DESIGN.md 1.4).
 *
 * A transcription of mockup.py's render_search(), constant for constant: the
 * inverted title bar to y 25, FIND at (6, 5), the hit count right-aligned at
 * W-6, the 24 px query at (8, 34) with a 12x22 block cursor, four rows 34 px
 * apart from y 74, and the hint on the last line. The design gate byte-compares
 * this frame against the mockup's own, so every literal below is copied rather
 * than re-derived.
 *
 * NOTHING ON THIS PAGE IS BELOW SCALE 2, including the hint. That floor is not
 * a preference: scale-1 text was measured unreadable on the physical panel, and
 * it is why the route picker's own hint was enlarged (chooser.c). The one line
 * on this page that would have been tempting to shrink is the hint, because it
 * is the longest -- 30 characters at scale 2 is 360 px of the 400, which fits,
 * so there was never a reason.
 *
 * Three states have no mockup reference and are argued where they are drawn: a
 * query that matches nothing, imperial units, and the online request of 7.10 in
 * the title bar.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "search.h"
#include "seg.h"
#include "view.h"

#define W SCR_W
#define H SCR_H

/* render_search()'s frame. */
#define FIND_TITLE_H 26   /* the inverted bar is rows 0..25            */
#define FIND_QY 34        /* the query's cap-top                       */
#define FIND_CUR_X 14     /* the cursor sits 14 px past the query's x  */
#define FIND_CUR_W 12
#define FIND_CUR_Y0 36
#define FIND_CUR_Y1 58
#define FIND_ROW0 74
#define FIND_ROW_H 34
/* The inverted selection band: y-4 .. y+22 around the row's text top. */
#define FIND_SEL_UP 4
#define FIND_SEL_DN 22
/* nm[:19] -- 19 characters at scale 2 is 228 px, which clears the widest
 * distance the right column can hold ("1.2KM NE", 96 px) with room between. */
#define FIND_NAME_CHARS 19

/* Right-aligned 5x7 text: mockup.py's rtext(). */
static void
rtext(cov_t *c, int x_right, int y, const char *s, int scale, int ink)
{
    cov_text(c, x_right - cov_text_w(s, scale), y, s, scale, ink);
}

/* "464M" / "1.2KM", and the imperial pair on the same rule.
 *
 * DESIGN.md 1.4 has no imperial mockup, so this follows the metric layout rule
 * rather than inventing one: metres until the kilometre figure would round to
 * 1.0 (950 m), then one decimal -- so feet until the mile figure would round to
 * 1.0 (0.95 mi = 5016 ft), then one decimal. Same shape, round numbers in the
 * rider's own units, exactly as 1.1.1 argues for the countdown ladder. */
static void
fmt_hit_dist(char *buf, size_t n, double metres, int units)
{
    if (units == UNITS_IMPERIAL) {
        double ft = metres * GEO_FT_PER_M;
        if (ft < 0.95 * GEO_FT_PER_MILE)
            snprintf(buf, n, "%.0fFT", ft);
        else
            snprintf(buf, n, "%.1fMI", ft / GEO_FT_PER_MILE);
        return;
    }
    if (metres < 1000.0)
        snprintf(buf, n, "%.0fM", metres);
    else
        snprintf(buf, n, "%.1fKM", metres / 1000.0);
}

/* The row-sized twin of view_map.c's marks: the same three pictures at the
 * size a 16 px row allows. Outline only -- there is no street grid to hide
 * here, so the paper interior that the map versions need would only cut a hole
 * in an inverted row. */
static void
find_icon(cov_t *c, double x, double y, int kind, int ink)
{
    double p[10];

    if (kind == SAVED_HOME) {
        p[0] = x;     p[1] = y;
        p[2] = x + 7; p[3] = y + 6;
        p[4] = x - 7; p[5] = y + 6;
        p[6] = x;     p[7] = y;
        cov_polyline(c, p, 4, 1.0, ink);
        cov_rect_outline(c, x - 5, y + 6, x + 5, y + 13, 1.0, ink);
    } else if (kind == SAVED_WORK) {
        cov_rect_outline(c, x - 7, y + 3, x + 7, y + 13, 1.0, ink);
        cov_rect_outline(c, x - 3, y, x + 3, y + 3, 1.0, ink);
    } else {
        p[0] = x;     p[1] = y + 1;
        p[2] = x + 6; p[3] = y + 7;
        p[4] = x;     p[5] = y + 13;
        p[6] = x - 6; p[7] = y + 7;
        p[8] = x;     p[9] = y + 1;
        cov_polyline(c, p, 5, 1.0, ink);
    }
}

void
view_find(cov_t *c, const find_t *f)
{
    char buf[96];
    int i, y, qw;

    cov_fill_rect(c, 0, 0, W - 1, FIND_TITLE_H - 1, COV_INK);
    cov_text(c, 6, 5, "FIND", 2, COV_PAPER);
    /* What the page is showing, which before a key is pressed is the saved
     * list and not a search. "0 HITS" over two visible rows would be the page
     * calling itself a failure.
     *
     * An online request outranks both (DESIGN.md 7.10). The count answers "what
     * did typing find"; FETCHING and its four failures answer "what came of
     * pressing ENTER on one of them", which is the more recent question and the
     * one the rider is waiting on. Nothing else about the page changes -- the
     * rows stay, the query stays, and the rider can go on typing while the
     * child process works. */
    if (f->net && *f->net)
        snprintf(buf, sizeof buf, "%s", f->net);
    else if (f->query && !*f->query && f->nsaved > 0)
        snprintf(buf, sizeof buf, "%d SAVED", f->nsaved);
    else
        snprintf(buf, sizeof buf, "%d HIT%s", f->nhits,
                 f->nhits == 1 ? "" : "S");
    rtext(c, W - 6, 5, buf, 2, COV_PAPER);

    /* The query, in the 24 px table of DESIGN.md 5.2's mechanism (see
     * tools/gen_query.py). An empty query draws nothing and leaves the cursor
     * at the left margin, which is the state the page opens in. */
    num_draw(c, 8, FIND_QY, f->query ? f->query : "", NUM_QUERY24, NUM_LT,
             COV_INK);
    qw = num_advance(f->query ? f->query : "", NUM_QUERY24);
    if (qw < 0)
        qw = 0;
    cov_fill_rect(c, FIND_CUR_X + qw, FIND_CUR_Y0,
                  FIND_CUR_X + qw + FIND_CUR_W, FIND_CUR_Y1, COV_INK);

    y = FIND_ROW0;
    for (i = 0; i < f->nshown && i < FIND_ROWS; i++) {
        int on = i == f->sel;
        int ink = on ? COV_PAPER : COV_INK;
        if (on)
            cov_fill_rect(c, 0, y - FIND_SEL_UP, W - 1, y + FIND_SEL_DN,
                          COV_INK);
        /* The rider's own places carry the SAME picture the MAP page draws for
         * them (DESIGN.md 1.4.6), so one thing looks like one thing in both
         * places. Rows that are pack hits get no icon and no indent -- the
         * absence is the distinction, and it costs the name no characters.
         *
         * Drawn in the row's own ink, which inverts with the selection: a
         * house outlined in paper on an ink row is the same house. */
        if (i < f->nsaved) {
            int kind = f->savedkind ? f->savedkind[i] : SAVED_OTHER;
            find_icon(c, 9, y + 1, kind, ink);
            snprintf(buf, sizeof buf, "%.*s", FIND_NAME_CHARS - 2,
                     f->hit[i].name ? f->hit[i].name : "");
            cov_text(c, 30, y, buf, 2, ink);
        } else {
            snprintf(buf, sizeof buf, "%.*s", FIND_NAME_CHARS,
                     f->hit[i].name ? f->hit[i].name : "");
            cov_text(c, 8, y, buf, 2, ink);
        }
        {
            char d[24];
            fmt_hit_dist(d, sizeof d, f->hit[i].dist_m, f->units);
            snprintf(buf, sizeof buf, "%s %s", d,
                     search_compass8(f->hit[i].bearing));
        }
        rtext(c, W - 8, y, buf, 2, ink);
        y += FIND_ROW_H;
    }

    /* No mockup reference: the mockup only ever renders a query that matches.
     * A rider who types a street that is not in the pack must not be left
     * reading a blank screen and concluding the street does not exist --
     * DESIGN.md 1.4's "nothing is searchable that is not in the pack" is a
     * property of the PACK, and this is where the page admits it. The dropped
     * count is the other half: a Bangkok pack has names with no ASCII form at
     * all, and silently missing them is the failure this line prevents.
     *
     * Only once something has been typed. Before that there is nothing to
     * explain, and the mockup's opening frame is exactly the blank one. */
    if (f->nhits == 0 && f->query && *f->query) {
        cov_text(c, 8, FIND_ROW0, "NOT IN THIS PACK", 2, COV_INK);
        if (f->ndropped > 0) {
            snprintf(buf, sizeof buf, "%d NAMES ARE NOT", f->ndropped);
            cov_text(c, 8, FIND_ROW0 + FIND_ROW_H, buf, 2, COV_INK);
            cov_text(c, 8, FIND_ROW0 + 2 * FIND_ROW_H, "IN THE 5X7 ALPHABET",
                     2, COV_INK);
        }
    }

    cov_text(c, 8, H - 20, "TYPE TO FILTER   ENTER = ROUTE", 2, COV_INK);
}

/* ------------------------------------------------------------ the demo */

/* mockup.py's page_search(): the query it types, and the position it types it
 * from -- the Asok route's first point, which is the pack's own reference, so
 * in the pack's frame it is exactly the origin (see tools/mkpack.py's --route).
 * That is not a coincidence to be relied on quietly: gen_asok.py wrote the GPX
 * whose first point mkpack.py referenced the fixture to, so the two agree by
 * construction and this page needs no fix to render. */
#define DEMO_QUERY "SOI 23"
/* A query with no match in the Asok pack. "ZZ" and not something plausible:
 * a plausible one might start matching if the fixture pack were ever rebuilt
 * from a wider extract, and a test whose expected answer can drift is not a
 * test. */
#define DEMO_QUERY_NONE "ZZQX"

void
view_find_demo(cov_t *c, roads_t *g, int zero)
{
    place_t hit[FIND_ROWS];
    find_t f;
    const char *q = zero ? DEMO_QUERY_NONE : DEMO_QUERY;

    memset(hit, 0, sizeof hit);
    f.query = q;
    f.hit = hit;
    f.sel = 0;
    f.units = UNITS_METRIC; /* the frozen design state; see view_nav_demo() */
    f.ndropped = roads_ndropped(g);
    /* Explicitly none, and not left to whatever was on the stack: find_t is a
     * local here, nsaved indexes savedkind, and an uninitialised count is the
     * kind of bug that renders correctly until the day it does not. */
    f.nsaved = 0;
    f.savedkind = NULL;
    /* No online request, explicitly: `net` replaces the title bar's count when
     * it is set, so a stack-garbage pointer here would corrupt a golden -- and
     * the one above it is the state that taught this file to initialise every
     * field of a local find_t rather than the ones it happens to care about. */
    f.net = NULL;
    f.nhits = search_places(g, q, 0.0, 0.0, hit, FIND_ROWS);
    f.nshown = f.nhits < FIND_ROWS ? f.nhits : FIND_ROWS;
    view_find(c, &f);
}

/* 7.10's offer, frozen: the page after a TOO FAR, with the armed ENTER on it.
 *
 * A state of its own for view_find_saved_demo()'s reason, one floor up. The whole
 * change is a string in the title bar, and a string is exactly the thing that
 * rots silently -- reworded, truncated by the right-aligned draw, or overrunning
 * the FIND label to its left. None of those three would fail any behavioural
 * test in this repo, because every one of them asserts on stderr and on g_mode.
 * The frame is the only assertion that can see the bar the RIDER reads.
 *
 * Deliberately the SAME query and pack as view_find_demo(), so the diff between
 * this golden and nav-find.fb is the title bar and nothing else. */
void
view_find_toofar_demo(cov_t *c, roads_t *g)
{
    place_t hit[FIND_ROWS];
    find_t f;

    memset(hit, 0, sizeof hit);
    f.query = DEMO_QUERY;
    f.hit = hit;
    f.sel = 0;
    f.units = UNITS_METRIC;
    f.ndropped = roads_ndropped(g);
    f.nsaved = 0;
    f.savedkind = NULL;
    /* The one field this state is about. It must be the SAME literal nav.c arms
     * the offer with; two copies of a sentence are two things to reword. */
    f.net = FIND_NET_TOOFAR_CAR;
    f.nhits = search_places(g, DEMO_QUERY, 0.0, 0.0, hit, FIND_ROWS);
    f.nshown = f.nhits < FIND_ROWS ? f.nhits : FIND_ROWS;
    view_find(c, &f);
}

/* The saved-place state (DESIGN.md 1.4.6): the page as it opens, BEFORE a key
 * is pressed. A state of its own and not a flag on the one above, because the
 * one above types a query -- and a golden of the typed page with places
 * configured is byte-identical to the golden without them, since none of them
 * match "SOI 23". That is exactly the mistake this state exists to correct: the
 * first version of this golden froze nothing at all and no test noticed,
 * because every assertion on it was a "the frames differ" comparison against a
 * frame that also had no places in it.
 *
 * Three places, one of each kind, so the golden carries all three pictures and
 * the file order (HOME, WORK, GYM -- not alphabetical) at once. */
static const savedmark_t FIND_DEMO_SAVED[] = {
    {"HOME", 0.0, 0.0, SAVED_HOME},
    {"WORK", 1200.0, -800.0, SAVED_WORK},
    {"GYM", -400.0, 600.0, SAVED_OTHER},
};

void
view_find_saved_demo(cov_t *c, roads_t *g)
{
    place_t hit[FIND_ROWS];
    int kind[FIND_ROWS];
    find_t f;
    int i, n = (int)(sizeof FIND_DEMO_SAVED / sizeof FIND_DEMO_SAVED[0]);

    memset(hit, 0, sizeof hit);
    for (i = 0; i < n && i < FIND_ROWS; i++) {
        hit[i].name = FIND_DEMO_SAVED[i].name;
        hit[i].e = FIND_DEMO_SAVED[i].e;
        hit[i].n = FIND_DEMO_SAVED[i].n;
        hit[i].dist_m = hypot(hit[i].e, hit[i].n);
        hit[i].bearing = atan2(hit[i].e, hit[i].n);
        hit[i].place = -1;
        kind[i] = FIND_DEMO_SAVED[i].kind;
    }
    f.query = "";
    f.hit = hit;
    f.sel = 0;
    f.units = UNITS_METRIC;
    f.ndropped = roads_ndropped(g);
    f.nsaved = n;
    f.savedkind = kind;
    f.net = NULL;
    f.nhits = 0;
    f.nshown = n;
    view_find(c, &f);
}

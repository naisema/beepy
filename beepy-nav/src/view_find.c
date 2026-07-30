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
 * Two states have no mockup reference and are argued where they are drawn: a
 * query that matches nothing, and imperial units.
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

void
view_find(cov_t *c, const find_t *f)
{
    char buf[96];
    int i, y, qw;

    cov_fill_rect(c, 0, 0, W - 1, FIND_TITLE_H - 1, COV_INK);
    cov_text(c, 6, 5, "FIND", 2, COV_PAPER);
    /* What the page is showing, which before a key is pressed is the saved
     * list and not a search. "0 HITS" over two visible rows would be the page
     * calling itself a failure. */
    if (f->query && !*f->query && f->nsaved > 0)
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
        /* A star on the rider's own places, so a list that mixes them with
         * pack hits -- which is what typing does -- still says which is which.
         * It costs two characters of the name, and the names people save are
         * short by construction: they chose them. */
        snprintf(buf, sizeof buf, "%s%.*s", i < f->nsaved ? "* " : "",
                 i < f->nsaved ? FIND_NAME_CHARS - 2 : FIND_NAME_CHARS,
                 f->hit[i].name ? f->hit[i].name : "");
        cov_text(c, 8, y, buf, 2, ink);
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
    f.nhits = search_places(g, q, 0.0, 0.0, hit, FIND_ROWS);
    f.nshown = f.nhits < FIND_ROWS ? f.nhits : FIND_ROWS;
    view_find(c, &f);
}

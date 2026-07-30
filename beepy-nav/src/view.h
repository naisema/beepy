/* beepy-nav/src/view.h -- the beepy-nav pages.
 *
 * Layout follows DESIGN.md 1.1 and the reference screenshot; every constant
 * in view_nav.c is transcribed from mockup.py's page_nav()/turn_panel(),
 * which the M2 design gate byte-compares against.
 */
#ifndef BEEPY_NAV_VIEW_H
#define BEEPY_NAV_VIEW_H

#include "libbeepyfb/cover.h"
#include "route.h"  /* the countdown ladder (1.1.1) */
#include "search.h" /* place_t, for the FIND page's rows (1.4) */

#define PANEL_W 128       /* turn panel: 32% of the width */
#define MAP_X (PANEL_W + 2)

/* The inverted left panel. */
typedef struct {
    int off;            /* metres off route; 0 = on route */
    int turn_m;         /* quantised distance to the announced cue (1.1.1) --
                     * what to show, not what was measured */
    int kind;           /* its arrow kind */
    const char *remain; /* time left on the route, "44 MIN" / "1H 23M" */
    const char *eta;    /* arrival, "ETA 1:42PM"                       */
    double togo_m;      /* whole-route remaining; < 0 = no route loaded */
    int batt;           /* only shown when there is no route            */
    const char *clock;  /* likewise                                     */
    /* UNITS_METRIC / UNITS_IMPERIAL (route.h). It picks the labels and the
     * whole-route row's arithmetic; `turn_m` arrives ALREADY on the right
     * ladder -- metres or feet -- because the quantiser is what knows which
     * rungs exist (DESIGN.md 1.1.1). */
    int units;
    /* A transient confirmation, or NULL. It takes the bottom row for about a
     * second and a half and then gives it back -- see view_turn_panel(). */
    const char *note;
    /* DESIGN.md 1.1: "GPS state earns panel space only when it is a problem".
     * Set once the receiver has gone NOFIX_S seconds without a valid fix, and
     * the bottom row becomes an INVERTED "NO FIX" -- outranking both arrival
     * and the transient. Everything on this panel derived from position is
     * frozen while it is set; the panel says so rather than pretending. */
    int nofix;
} panel_t;

/* The map on the right. Route geometry is metres east/north of any origin;
 * the fix sits a fraction pos_f along segment pos_i -> pos_i+1. */
typedef struct {
    const double *pts;
    int npts;
    int pos_i;
    double pos_f;
    int turn_i; /* the announced cue */
    int pin_i;  /* the cue AFTER it -- what the teardrop marks */
    double off; /* metres perpendicular off the route; 0 = on it */
    int spd_kmh; /* always km/h: the badge converts, nothing upstream does */
    int course_up;
    int units;

    /* Metres per pixel, or <= 0 for auto zoom (DESIGN.md 6.1: "Z/X switch to
     * manual; A returns to auto"). The value is a rung of MAP_ZOOMS -- the
     * keys step the ladder, they do not scale freely -- but nothing here
     * requires that, and the demo pages leave it 0. */
    double mpp_manual;

    /* Map rotation, radians clockwise from north: the smoothed heading of
     * DESIGN.md 6.1. `have_heading` 0 means "derive it from the route
     * direction ahead of the fix", which is what the static demo pages and
     * mockup.py both do -- a mockup has no receiver to smooth. */
    double heading;
    int have_heading;

    /* The optional OSM raster basemap (DESIGN.md 6.5), or NULL for none --
     * which is what every frozen design state passes, and what makes the
     * five original nav goldens the proof that the layer is optional. It is
     * an opaque handle rather than a flag so the page stays a pure function
     * of its arguments: nothing in view_nav.c reaches for global state. */
    struct tiles *tiles;

    /* The chevron's angle RELATIVE to that rotation: raw course over ground
     * minus the map rotation (DESIGN.md 1.1). Mid-turn the smoothed map lags
     * the bike by up to a second, and a chevron drawn straight up would point
     * off the road you are demonstrably on. North-up, where the rotation is
     * zero, this is simply the course. */
    double residual;
} navmap_t;

/* The OVERVIEW page: the whole route, north-up, fitted below the title band
 * (DESIGN.md 1.2). Geometry is world metres, exactly as navmap_t's. The
 * strip's five values arrive pre-formatted -- the page is a transcription of
 * mockup.py's page_overview() and formatting decisions belong with the
 * caller that knows the units setting. */
typedef struct {
    const double *pts;
    int npts;
    const int *cue_idx; /* one vertex index per cue dot */
    int ncue_dots;
    int pos_i;
    double pos_f;
    const char *name;  /* "SUKHUMVIT LOOP"        */
    const char *total; /* "31.0", km or miles    */
    const char *togo;  /* "12.6", km or miles    */
    const char *eta;   /* "10:42"                */
    int units;         /* which of the two, for the labels and the bar */
    int done;          /* per cent          */
    int cue_i, ncues;  /* "3/11 CUES"       */
    /* DESIGN.md 6.5: the attribution rides the title line whenever a basemap
     * is loaded, and nowhere at all when one is not -- which is why this is a
     * flag on the page rather than a permanent part of the chrome. */
    int osm;
} overview_t;

/* The FIND page (DESIGN.md 1.4): a query being typed and the nearest matches.
 * Four rows is what the mockup shows and what the layout holds -- 34 px a row
 * from y 74, at the scale-2 floor.
 *
 * `nhits` is the TOTAL match count, not `nshown`: 1.4 calls the title bar's
 * number a live hit count, and a count that stops at the size of the visible
 * list is not one. search_places() returns the two separately for this reason.
 */
#define FIND_ROWS 4
#define FIND_QUERY_MAX 40
typedef struct {
    const char *query;   /* what has been typed, uppercase */
    const place_t *hit;  /* nshown of them, nearest first */
    int nshown;
    int nhits;
    int sel;             /* which row is inverted; N/P move it */
    int units;
    /* Names the pack could not index for having no ASCII form
     * (roads_ndropped()). DESIGN.md 1.4: "nothing is searchable that is not in
     * the pack, and the hit count says so honestly" -- so when a query finds
     * nothing, the page says how much of the pack it cannot show rather than
     * letting the rider conclude the place does not exist. */
    int ndropped;
} find_t;

/* The CONFIRM page (DESIGN.md 1.4): the OVERVIEW page's cartography fitted to
 * the PROPOSED route, with the strip carrying the one decision. Geometry is
 * world metres, as everywhere else; the length, the estimate and the turn count
 * are all derived here from `pts` and `ncues`, so the page cannot disagree with
 * the route it is drawing. */
typedef struct {
    const double *pts;
    int npts;
    const int *cue_idx; /* one vertex index per NON-destination cue */
    int ncues;          /* how many -- also the "N TURNS" figure */
    const char *dest;   /* uppercase; the title reads "TO <dest>" */
    int units;
} confirm_t;

void view_turn_panel(cov_t *c, const panel_t *p);
void view_nav_map(cov_t *c, const navmap_t *m);
void view_nav(cov_t *c, const navmap_t *m, const panel_t *p);
void view_overview(cov_t *c, const overview_t *o);
void view_find(cov_t *c, const find_t *f);
void view_confirm(cov_t *c, const confirm_t *cf);

/* Map marks shared by the two pages; defined in view_nav.c, where they were
 * transcribed from mockup.py. The OVERVIEW page is the same cartography at a
 * different zoom, and a second copy would be a second thing to keep in step
 * with the mockup. */
void mark_position(cov_t *c, double x, double y, double r, double ang);
void mark_pin(cov_t *c, double x, double y, double r);
void mark_compass(cov_t *c, double x, double y, double theta, double r);
void mark_scale_bar(cov_t *c, double x, double y, double mpp, int units);
void mark_dashed(cov_t *c, const double *segs, int nsegs, double on,
                 double off, double width, int ink);
void mark_cased_route(cov_t *c, const double *segs, int nsegs, double outer,
                      double inner);

/* The nine cue glyphs at four sizes -- a reference sheet, not a page. */
void view_arrows(cov_t *c);

/* The static demo state page_nav() renders: off = 0 for the turn page,
 * metres off-route for the OFF ROUTE variant, nofix for the NO FIX one. */
void view_nav_demo(cov_t *c, int off, int nofix);

/* The basemap state (DESIGN.md 6.5): the real Asok / Sukhumvit route from
 * osm-asok.json, over a tile pack cut around it. `t` may be NULL, and then
 * this is the same page with the tile layer absent -- which is the pair the
 * "a missing pack changes nothing" test compares. */
void view_nav_tiles_demo(cov_t *c, struct tiles *t);

/* The static demo state page_overview() renders. `osm` adds the attribution
 * line of DESIGN.md 6.5, which is what a loaded basemap does to this page. */
void view_overview_demo(cov_t *c, int osm);

/* mockup.py's page_search(): "SOI 23" from the Asok route's first point, over
 * the road pack `g` -- so this page is the search itself and not a picture of
 * one. `g` NULL renders the empty-pack state, which is the frame nothing should
 * ever reach (F says so on the panel instead) and is therefore worth having.
 * `zero` types a query that matches nothing, which is the honest-coverage state
 * DESIGN.md 1.4 asks for and which the mockup has no reference for. */
void view_find_demo(cov_t *c, roads_t *g, int zero);

/* mockup.py's page_confirm(): the same Asok route the basemap demo rides,
 * proposed rather than under way. */
void view_confirm_demo(cov_t *c);

/* The clipping test of DESIGN.md 10: the NAV map at a zoom where the route
 * leaves the view on all four sides. Nothing but the panel may put ink in
 * x < 130, and this is the page that makes a failure to clip visible. */
void view_cliptest(cov_t *c);
/* The same page's panel with an empty map behind it: the two frames must
 * agree pixel for pixel over x < 130. */
void view_cliptest_panel(cov_t *c);

#endif /* BEEPY_NAV_VIEW_H */

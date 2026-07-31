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
    /* DESIGN.md 7.11: `reroute = ask` has fired and is waiting for ENTER. It
     * takes BOTH lower rows -- the question on one and the key on the other,
     * because a question a rider cannot see the answer to is not a question --
     * and it takes them from the whole-route distance and the arrival time,
     * which are the two figures on this panel least worth having while the
     * rider is 200 m off the route they describe.
     *
     * It outranks the transient and is outranked by NO FIX. A transient is a
     * confirmation of something the rider just did and they can miss it; this
     * one is waiting on them. NO FIX wins because with no position there is
     * nothing to reroute from, and the answer would fail anyway. */
    int ask_reroute;
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

/* A saved place drawn on the MAP page (DESIGN.md 1.4.6). Geometry is world
 * metres like everything else; `kind` is decided by the CALLER, so the page
 * draws what it is told and no view file matches on a name. That split is the
 * point: 1.4.6 refuses to give a magic name any behaviour, and choosing a
 * picture is the one place where recognising HOME is harmless -- an
 * unrecognised name falls back to its initial and nothing else changes. */
enum { SAVED_OTHER, SAVED_HOME, SAVED_WORK };
typedef struct {
    const char *name; /* uppercase, as the config stored it */
    double e, n;      /* world metres, the caller's frame   */
    int kind;
} savedmark_t;
#define SAVED_MARK_MAX 8

/* The MAP page (DESIGN.md 1.5): where you are, with no route loaded. Full
 * width -- there is no turn panel, because with no route there is no next turn
 * and an inverted empty third of the screen would be worse than none.
 *
 * The cartography is navmap_t's, mark for mark, and only the framing differs:
 * the fix sits at the CENTRE of the map rather than at 0.72 of the height, and
 * an inverted two-row strip along the bottom carries the position and the keys.
 *
 * Geometry is world metres east/north of any origin, as everywhere else. The
 * caller decides that origin: with no route there is no first point to
 * reference to, so it is the basemap pack's own reference (tile.h) -- which is
 * what makes the streets line up with a position the pack never heard about.
 */
typedef struct {
    /* 0 = the waiting state: no map is drawn at all, because there is nothing
     * to centre one on. Set as soon as there has ever been a fix, and stays
     * set through a loss -- `nofix` is what says the position is stale. */
    int have_pos;
    double lat, lon;     /* the strip's first row, to five decimals   */
    double pos_e, pos_n; /* the same position, in world metres        */

    /* The breadcrumb: the track travelled this session, world metres, oldest
     * first, NOT including the position above -- the page joins the two, so a
     * caller recording fixes never has to keep a moving point in the array. */
    const double *track;
    int ntrack;

    int spd_kmh; /* always km/h: the badge converts, nothing upstream does */
    int course_up;
    int units;

    /* Metres per pixel, or <= 0 for MAP_MPP_DEFAULT. There is no auto zoom on
     * this page: auto-zoom fits the next cue (DESIGN.md 6.1) and with no route
     * there is no cue to fit, so the ladder has a default rung instead. */
    double mpp_manual;

    /* As navmap_t's: the smoothed rotation, and the chevron's angle relative
     * to it. `have_heading` 0 falls back to the last leg of the drawn track,
     * which is what the static demo state and mockup.py both rely on. */
    double heading;
    int have_heading;
    double residual;

    struct tiles *tiles; /* the optional basemap, or NULL */

    /* DESIGN.md 1.1.2, in the one row this page has for it: the last known
     * position stays drawn and an inverted NO FIX appears beside it. */
    int nofix;
    /* A transient confirmation, or NULL -- it displaces the coordinates for
     * about a second and a half, exactly as the panel's displaces arrival. */
    const char *note;
    /* Waiting state only: satellites seen, or < 0 when the receiver has not
     * said anything at all yet. */
    int sats;
    /* Waiting state only: the first saved place (DESIGN.md 1.4.6), in world
     * metres, to centre the basemap on before there has ever been a fix. 0
     * leaves the waiting screen exactly as it was -- which is what every
     * frozen golden and every rider with no saved places gets. */
    int have_home;
    double home_e, home_n;
    /* Where the map is looking, in SCREEN PIXELS away from the rider (1.5.1).
     * 0,0 is the ordinary state -- the rider at the centre, the map following
     * them. The arrow keys move it and `C` puts it back.
     *
     * PIXELS and not world metres, because every layer of this page already
     * projects about one centre: shifting that centre pans the tiles, the
     * breadcrumb, the saved marks and the position marker together, and there
     * is nothing to keep in step. The cost is that zooming while panned changes
     * which ground the offset covers -- which is the right trade for a rider who
     * swiped to look at something, because "up" stays up. */
    double pan_x, pan_y;
    /* Panning while MOVING has to be acknowledged (1.5.1): the map stops
     * following the rider and does not start again by itself, so the hint row
     * asks first and this is the question waiting. The pan does not happen until
     * it is answered. */
    int ask_pan;
    /* The saved places to draw (1.4.6), or none. MAP only: the NAV page
     * already carries a route, a ridden track, cue dots and a turn pin, and a
     * sixth kind of mark on the busiest screen buys nothing a rider is looking
     * for mid-corner. */
    const savedmark_t *saved;
    int nsaved;
} livemap_t;

/* The breadcrumb's cap, and what happens when it fills: see view_map.c. It is
 * here rather than in nav.c because the page's own scratch buffers are sized
 * from it, and two numbers that must agree should be one number. */
#define MAP_TRACK_MAX 2048
/* The MAP page's zoom when the rider has not chosen one. A rung of MAP_ZOOMS,
 * so a basemap pack carrying that rung draws at it. */
#define MAP_MPP_DEFAULT 6.0

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
    /* How many of the rows above are the rider's saved places (DESIGN.md
     * 1.4.6) rather than pack hits. They come first, and with no query typed
     * they are all there is -- which is what the title bar counts, because
     * "0 HITS" over a list of two would read as a failure. */
    int nsaved;
    /* One SAVED_* per saved row, so a row shows the same picture the map
     * shows for the same place. NULL falls back to the plain star, which is
     * what the pages that do not care pass. */
    const int *savedkind;
    /* What the title bar says INSTEAD of the hit count while an online request
     * is in flight or after one failed: FETCHING, NO SIGNAL, TIMED OUT,
     * NO ROUTE, NO ROUTER, NO FIX (DESIGN.md 7.10). NULL is the count.
     *
     * The TITLE BAR and not a transient, and that was a decision rather than a
     * convenience: 7.5's transient is cheaper and consistent, and it is gone in
     * 1.5 s -- before a rider who glanced down has read it. A failure a rider
     * misses is a failure they will attribute to the program being broken. It
     * sits where the count sat because the count is the thing it is the answer
     * to: the rider pressed ENTER on a row and this is what came of it. */
    const char *net;
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
    /* DESIGN.md 7.7's travel mode, NAV_MODE_BIKE or NAV_MODE_CAR, shown on the
     * strip beside the key that toggles it.
     *
     * It is on THIS page because this is where a rider commits to a route the
     * mode shaped -- bike and car do not return a refinement of the same line,
     * they return 43.8 km and 56.4 km over different roads (7.7's measurement).
     * And it is shown with its KEY rather than as a bare label, because a key the
     * page does not advertise is one the rider has no way to know about (2), and
     * a label with no way to act on it would only raise the question. */
    int mode;
    /* A transient confirmation, or NULL -- the mechanism of 7.5, on the row that
     * carries the turn count. Toggling the mode rebuilds the route, so the
     * figures on the left change on their own; this says which of the two
     * profiles produced them, once, for the rider who pressed the key. */
    const char *note;
} confirm_t;

/* The QUIT page (DESIGN.md 1.6): the one modal question in the program.
 *
 * It carries figures rather than only a question because the thing being
 * confirmed is a LOSS -- the ride ends here -- and a rider deciding whether to
 * accept that is entitled to know what they are ending. All three are facts the
 * loop already holds, not estimates: metres actually travelled this session,
 * seconds actually on the clock, and whether the bytes are on the card. */
typedef struct {
    int riding;        /* a route is loaded: the question mentions the ride */
    double ridden_m;   /* the session odometer, metres */
    double elapsed_s;  /* the ride clock */
    int logging;       /* a ride log is open, so the ride survives the exit */
    int units;
} quit_t;

void view_turn_panel(cov_t *c, const panel_t *p);
void view_nav_map(cov_t *c, const navmap_t *m);
void view_nav(cov_t *c, const navmap_t *m, const panel_t *p);
void view_map(cov_t *c, const livemap_t *m);
void view_overview(cov_t *c, const overview_t *o);
void view_find(cov_t *c, const find_t *f);
void view_confirm(cov_t *c, const confirm_t *cf);
void view_quit(cov_t *c, const quit_t *q);

/* Map marks shared by the pages that draw a map; defined in view_nav.c, where
 * they were transcribed from mockup.py. OVERVIEW, CONFIRM and MAP are the same
 * cartography at different zooms and in different frames, and a second copy
 * would be a second thing to keep in step with the mockup. */
void mark_position(cov_t *c, double x, double y, double r, double ang);
void mark_pin(cov_t *c, double x, double y, double r);
void mark_compass(cov_t *c, double x, double y, double theta, double r);
/* The round speed instrument. Private to view_nav.c until the MAP page needed
 * it: two pages have one now, and there is still exactly one of it. */
void mark_speed_badge(cov_t *c, double x, double y, int kmh, double r,
                      int units);
void mark_scale_bar(cov_t *c, double x, double y, double mpp, int units);
void mark_dashed(cov_t *c, const double *segs, int nsegs, double on,
                 double off, double width, int ink);
void mark_cased_route(cov_t *c, const double *segs, int nsegs, double outer,
                      double inner);

/* The nine cue glyphs at four sizes -- a reference sheet, not a page. */
void view_arrows(cov_t *c);

/* The static demo state page_nav() renders: off = 0 for the turn page,
 * metres off-route for the OFF ROUTE variant, nofix for the NO FIX one. */
void view_nav_demo(cov_t *c, int off, int nofix, int ask);

/* The basemap state (DESIGN.md 6.5): the real Asok / Sukhumvit route from
 * osm-asok.json, over a tile pack cut around it. `t` may be NULL, and then
 * this is the same page with the tile layer absent -- which is the pair the
 * "a missing pack changes nothing" test compares. */
void view_nav_tiles_demo(cov_t *c, struct tiles *t);

/* The static states mockup.py's page_map() and page_map_wait() render: the MAP
 * page with a position (`nofix` 1 for the state where it has gone stale), and
 * the page before the first fix has ever arrived. Neither takes a basemap: they
 * are frozen design states, and view_nav_demo()'s argument for passing NULL
 * applies twice as hard here (see view_map.c). */
void view_map_demo(cov_t *c, int nofix);
void view_map_wait_demo(cov_t *c);

/* The basemap state (DESIGN.md 6.5) on the MAP page: a position in the tile
 * pack's OWN reference frame, which is the frame this page introduced. `t` may
 * be NULL, and then this is the same page with the tile layer absent -- which is
 * the pair T-MAP-BASEMAP compares. */
void view_map_tiles_demo(cov_t *c, struct tiles *t);

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
void view_find_saved_demo(cov_t *c, roads_t *g);
/* What the FIND title bar reads when a TOO FAR is waiting for ENTER to retry it
 * as a car (DESIGN.md 7.10). Here rather than in nav.c because BOTH nav.c (which
 * arms it) and view_find.c (which freezes it into a golden) must show the rider
 * the same twenty characters, and a golden that agrees with a second copy of the
 * sentence proves nothing about the first. */
#define FIND_NET_TOOFAR_CAR "TOO FAR  ENTER = CAR"
void view_find_toofar_demo(cov_t *c, roads_t *g);

/* mockup.py's page_confirm(): the same Asok route the basemap demo rides,
 * proposed rather than under way. */
void view_confirm_demo(cov_t *c);
void view_quit_demo(cov_t *c, int riding);
void view_map_wait_home_demo(cov_t *c, struct tiles *t);
void view_map_saved_demo(cov_t *c, struct tiles *t);
/* DESIGN.md 1.5.1: `asking` picks the question, 0 the held map. */
void view_map_pan_demo(cov_t *c, int asking);

/* The clipping test of DESIGN.md 10: the NAV map at a zoom where the route
 * leaves the view on all four sides. Nothing but the panel may put ink in
 * x < 130, and this is the page that makes a failure to clip visible. */
void view_cliptest(cov_t *c);
/* The same page's panel with an empty map behind it: the two frames must
 * agree pixel for pixel over x < 130. */
void view_cliptest_panel(cov_t *c);

#endif /* BEEPY_NAV_VIEW_H */

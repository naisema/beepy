/* beepy-nav/src/view.h -- the beepy-nav pages.
 *
 * Layout follows DESIGN.md 1.1 and the reference screenshot; every constant
 * in view_nav.c is transcribed from mockup.py's page_nav()/turn_panel(),
 * which the M2 design gate byte-compares against.
 */
#ifndef BEEPY_NAV_VIEW_H
#define BEEPY_NAV_VIEW_H

#include "libbeepyfb/cover.h"
#include "route.h" /* the countdown ladder (1.1.1) */

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
    int spd_kmh;
    int course_up;

    /* Map rotation, radians clockwise from north: the smoothed heading of
     * DESIGN.md 6.1. `have_heading` 0 means "derive it from the route
     * direction ahead of the fix", which is what the static demo pages and
     * mockup.py both do -- a mockup has no receiver to smooth. */
    double heading;
    int have_heading;

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
    const char *name;  /* "SUKHUMVIT LOOP"  */
    const char *total; /* "31.0", km        */
    const char *togo;  /* "12.6", km        */
    const char *eta;   /* "10:42"           */
    int done;          /* per cent          */
    int cue_i, ncues;  /* "3/11 CUES"       */
} overview_t;

void view_turn_panel(cov_t *c, const panel_t *p);
void view_nav_map(cov_t *c, const navmap_t *m);
void view_nav(cov_t *c, const navmap_t *m, const panel_t *p);
void view_overview(cov_t *c, const overview_t *o);

/* Map marks shared by the two pages; defined in view_nav.c, where they were
 * transcribed from mockup.py. The OVERVIEW page is the same cartography at a
 * different zoom, and a second copy would be a second thing to keep in step
 * with the mockup. */
void mark_position(cov_t *c, double x, double y, double r, double ang);
void mark_pin(cov_t *c, double x, double y, double r);
void mark_compass(cov_t *c, double x, double y, double theta, double r);
void mark_scale_bar(cov_t *c, double x, double y, double mpp);
void mark_dashed(cov_t *c, const double *segs, int nsegs, double on,
                 double off, double width, int ink);
void mark_cased_route(cov_t *c, const double *segs, int nsegs, double outer,
                      double inner);

/* The nine cue glyphs at four sizes -- a reference sheet, not a page. */
void view_arrows(cov_t *c);

/* The static demo state page_nav() renders: off = 0 for the turn page,
 * metres off-route for the OFF ROUTE variant. */
void view_nav_demo(cov_t *c, int off);

/* The static demo state page_overview() renders. */
void view_overview_demo(cov_t *c);

/* The clipping test of DESIGN.md 10: the NAV map at a zoom where the route
 * leaves the view on all four sides. Nothing but the panel may put ink in
 * x < 130, and this is the page that makes a failure to clip visible. */
void view_cliptest(cov_t *c);
/* The same page's panel with an empty map behind it: the two frames must
 * agree pixel for pixel over x < 130. */
void view_cliptest_panel(cov_t *c);

#endif /* BEEPY_NAV_VIEW_H */

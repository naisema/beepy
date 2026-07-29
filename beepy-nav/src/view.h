/* beepy-nav/src/view.h -- the beepy-nav pages.
 *
 * Layout follows DESIGN.md 1.1 and the reference screenshot; every constant
 * in view_nav.c is transcribed from mockup.py's page_nav()/turn_panel(),
 * which the M2 design gate byte-compares against.
 */
#ifndef BEEPY_NAV_VIEW_H
#define BEEPY_NAV_VIEW_H

#include "libbeepyfb/cover.h"

#define PANEL_W 128       /* turn panel: 32% of the width */
#define MAP_X (PANEL_W + 2)

/* The inverted left panel. */
typedef struct {
    int off;            /* metres off route; 0 = on route */
    int turn_m;         /* distance to the announced cue */
    int kind;           /* its arrow kind */
    int then_kind;      /* the cue after it */
    const char *then_d; /* "150M" */
    int batt;
    const char *clock;
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
} navmap_t;

void view_turn_panel(cov_t *c, const panel_t *p);
void view_nav_map(cov_t *c, const navmap_t *m);
void view_nav(cov_t *c, const navmap_t *m, const panel_t *p);

/* The nine cue glyphs at four sizes -- a reference sheet, not a page. */
void view_arrows(cov_t *c);

/* The static demo state page_nav() renders: off = 0 for the turn page,
 * metres off-route for the OFF ROUTE variant. */
void view_nav_demo(cov_t *c, int off);

#endif /* BEEPY_NAV_VIEW_H */

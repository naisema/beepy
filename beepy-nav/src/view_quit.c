/* beepy-nav/src/view_quit.c -- the QUIT page (DESIGN.md 1.6).
 *
 * The only modal page in the program: nothing behind it is reachable until it
 * is answered. Every other page is somewhere you can be -- NAV, OVERVIEW, MAP,
 * FIND -- and Tab or Esc leaves. This one has two exits and both are decisions.
 *
 * It exists because Q used to end the program on one keypress. On a desk that
 * is fine. On a bike, with a keyboard you are hitting by feel and gloves in
 * between, it is a ride ending because a thumb landed one key left of P -- and
 * the ride does not come back, because the loop that was averaging your speed
 * and holding your route is gone.
 *
 * WHY A PAGE AND NOT A SECOND KEYPRESS. A two-press confirm costs no screen and
 * would have reused 7.5's transient, but it answers the wrong question. "Press
 * Q again" tells a rider what to DO; it does not tell them what they are about
 * to LOSE, and the loss is the whole reason the confirmation exists. A page can
 * carry the three facts that make the decision -- how far, how long, and whether
 * the ride is already on the card -- and those turn "am I sure?" into a
 * question with an answer.
 *
 * The layout is CONFIRM's, deliberately: an inverted title bar, content, and
 * the same 42 px inverted strip carrying the two choices in the same corners.
 * A rider who has answered CONFIRM once already knows where to look and which
 * key is which, and ENTER means "go through with it" on both.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "draw.h"
#include "route.h"
#include "view.h"

#define W SCR_W
#define H SCR_H

/* CONFIRM's frame, shared for the reason the header gives. */
#define QT_TITLE 26
#define QT_STRIP 42

/* The content rows. Three facts and a question, at scale 2 -- the panel's
 * readable floor, and what every other decision on this device is set in. */
#define QT_ASK 46
#define QT_ROW0 96
#define QT_ROW1 122
#define QT_ROW2 156
#define QT_LEFT 10

static void
rtext(cov_t *c, int x_right, int y, const char *s, int scale, int ink)
{
    cov_text(c, x_right - cov_text_w(s, scale), y, s, scale, ink);
}

/* view_confirm.c's fmt_total(), repeated rather than shared for the reason
 * that file gives about thin(): each page is a transcription of its own
 * mockup, and a shared helper would make one page's rounding rule the other's
 * problem. Same thresholds, so the two agree by construction. */
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

/* "1H 02M" past the hour, "14 MIN" below it -- the panel's own clock format
 * (view_nav.c's fmt_remain), repeated rather than shared because that one is
 * static to a file the design gate byte-compares and this page has no claim on
 * it. Seconds are never shown: this is a ride, not a stopwatch. */
static void
fmt_elapsed(char *out, size_t n, double secs)
{
    int m = (int)(secs / 60.0 + 0.5);

    if (m < 1)
        snprintf(out, n, "0 MIN");
    else if (m < 60)
        snprintf(out, n, "%d MIN", m);
    else
        snprintf(out, n, "%dH %02dM", m / 60, m % 60);
}

void
view_quit(cov_t *c, const quit_t *q)
{
    char buf[64], dtxt[64];

    cov_fill_rect(c, 0, 0, W - 1, QT_TITLE - 1, COV_INK);
    cov_text(c, 6, 5, "QUIT", 2, COV_PAPER);

    /* Two questions, because there are two things to be leaving. With a route
     * the ride is the loss and the page says so; on the MAP page there is no
     * ride to end and claiming otherwise would be the kind of small lie that
     * teaches a rider to stop reading the screen. */
    cov_text(c, QT_LEFT, QT_ASK,
             q->riding ? "END THIS RIDE AND EXIT?" : "EXIT NAVIGATOR?", 2,
             COV_INK);

    /* Distance travelled, not distance along a route: the odometer is the
     * session's, so it is a true answer on the MAP page as well, where there is
     * no route to be a fraction of. */
    fmt_total(dtxt, sizeof dtxt, q->ridden_m, q->units);
    snprintf(buf, sizeof buf, "%s RIDDEN", dtxt);
    cov_text(c, QT_LEFT, QT_ROW0, buf, 2, COV_INK);

    fmt_elapsed(dtxt, sizeof dtxt, q->elapsed_s);
    snprintf(buf, sizeof buf, "%s ELAPSED", dtxt);
    cov_text(c, QT_LEFT, QT_ROW1, buf, 2, COV_INK);

    /* The one that changes the decision. A rider who knows the ride is already
     * written can quit and look at it later; a rider who is told nothing has to
     * assume the worst, and assuming the worst means not quitting -- which is
     * how a navigator ends up left running until the battery decides. */
    cov_text(c, QT_LEFT, QT_ROW2,
             q->logging ? "RIDE LOG SAVED" : "NOT LOGGING", 2, COV_INK);

    cov_fill_rect(c, 0, H - QT_STRIP, W - 1, H - 1, COV_INK);
    cov_text(c, 6, H - QT_STRIP + 5, "ENTER = QUIT", 2, COV_PAPER);
    rtext(c, W - 6, H - QT_STRIP + 5, "Q = CANCEL", 2, COV_PAPER);
    /* Second row of the strip: the way out that is not an exit. E is the key
     * that ends a route and keeps the program, and this is the one place it is
     * worth spelling out -- a rider who opened this page wanted to stop
     * something, and stopping the RIDE is more often what they meant. */
    if (q->riding)
        cov_text(c, 6, H - QT_STRIP + 23, "E = END ROUTE, KEEP RIDING", 2,
                 COV_PAPER);
    else
        cov_text(c, 6, H - QT_STRIP + 23, "TAB = BACK TO THE MAP", 2,
                 COV_PAPER);
}

/* ------------------------------------------------------------ the demo */

void
view_quit_demo(cov_t *c, int riding)
{
    quit_t q;

    /* The frozen design state: a ride far enough in to be worth not losing.
     * Metric for the reason view_nav_demo() gives -- the goldens are one
     * frozen state and the units toggle is asserted elsewhere. */
    q.riding = riding;
    q.ridden_m = 12400.0;
    q.elapsed_s = 3720.0; /* 1H 02M */
    q.logging = 1;
    q.units = UNITS_METRIC;
    view_quit(c, &q);
}

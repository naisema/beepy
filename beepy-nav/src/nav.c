/* beepy-nav -- route navigator for the Beepy's 400x240 Sharp panel.
 *
 *   beepy-nav --route RIDE.gpx [-d /dev/ttyACM0]
 *   beepy-nav --route RIDE.gpx --replay ride.nmea [--headless] [--trace T]
 *   beepy-nav --demo --page nav|nav-off|overview|arrows|cliptest --dump F
 *
 * M2's static frame dumper is still here unchanged, because the goldens
 * depend on it byte for byte. What M3 adds is everything around it: a GPX
 * route, an NMEA source (a port or a replay file), the snap/cue/progress
 * maths per fix, two live pages and the keys that switch them.
 *
 * PORTABILITY. Everything except the panel, evdev and the serial port
 * compiles on the Mac, which is what lets a replay be run and asserted
 * without a device round trip. Those three are Linux-only and sit behind
 * NAV_DEVICE; --replay is plain stdio and works in both lanes.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__)
#define NAV_DEVICE 1
#include <linux/input.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include "libbeepyfb/fbdev.h"
#include "libbeepyfb/input.h"
#include "libnmea/serial.h"
#endif

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/dump.h"
#include "libnmea/gps.h"
#include "libnmea/nmea.h"

#include "arrows.h"
#include "chooser.h"
#include "config.h"
#include "fix.h"
#include "led.h"
#include "map.h"
#include "route.h"
#include "view.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

/* route.h numbers the cue kinds in arrows.h's order so a cue can be passed
 * straight to arrow_draw(). This is the only file that includes both, so
 * this is where the claim is checked. */
/* The casts are for gcc's -Wenum-compare: these are two distinct anonymous
 * enumerations, and comparing them is exactly the point. */
_Static_assert((int)CUE_STRAIGHT == (int)ARROW_STRAIGHT &&
                   (int)CUE_LEFT == (int)ARROW_LEFT &&
                   (int)CUE_RIGHT == (int)ARROW_RIGHT &&
                   (int)CUE_UTURN == (int)ARROW_UTURN &&
                   (int)CUE_DEST == (int)ARROW_DEST &&
                   (int)CUE_N == (int)ARROW_N,
               "cue_t.kind is passed straight to arrow_draw()");

static const char USAGE[] =
    "usage: beepy-nav --route FILE.gpx [-d DEV] [--replay F.nmea]\n"
    "       beepy-nav --demo --page PAGE [--dump FILE] [--bench N]\n"
    "\n"
    "  --route FILE  the GPX to follow; omitted, the routes in ~/routes\n"
    "                (or $BEEPY_ROUTES) are offered on the panel\n"
    "  -d DEV        NMEA serial port (default /dev/ttyACM0)\n"
    "  --replay F    read NMEA from a file instead, paced to the clock the\n"
    "                sentences carry; exits cleanly at end of file\n"
    "  --pace        force that pacing, --no-pace defeat it (default: pace\n"
    "                only when there is a panel to watch)\n"
    "  --headless    render but present nothing -- a replay on a machine\n"
    "                with no panel, which is how the assertions run\n"
    "  --trace FILE  one TSV row per fix; see tools/assert_trace.py\n"
    "  --trace-frames F  one TSV row per rendered FRAME: the dead-reckoning\n"
    "                state, so the 8 Hz extrapolation can be asserted\n"
    "  --fps N       frames per second while moving (default 8; stopped is\n"
    "                always 1 Hz -- DESIGN.md 6.3)\n"
    "  --stats       render ms and frames drawn/skipped, per second and a\n"
    "                p95 summary at exit\n"
    "  --dump-at S:F write the frame at replay second S to file F (up to 4;\n"
    "                this is how a moving page gets into fbshow --verify)\n"
    "  --print       dump nav_t as text, per fix\n"
    "  --north-up    start north-up instead of course-up\n"
    "  --imperial    feet and miles; --metric is the default\n"
    "  --config F    read F instead of ~/.config/beepy-nav.conf; every\n"
    "                setting in it is overridden by the flag for it\n"
    "  --rate-5hz    ask the receiver for 200 ms fixes at startup;\n"
    "                --no-rate-5hz is the default (see DESIGN.md 6.3)\n"
    "  --demo        render a static design state instead of navigating\n"
    "  --page P      nav, nav-off, overview, arrows, chooser, cliptest,\n"
    "                cliptest-panel -- and with\n"
    "                --route it picks the page the ride opens on\n"
    "  --dump FILE   write the frame as 384000 raw XRGB bytes\n"
    "  --bench N     time N draw+resolve cycles and print ms/frame\n"
    "\n"
    "keys: Tab page   Q quit   H hold   O course-up/north-up\n"
    "      Z/X zoom out/in   A auto zoom   U metric/imperial\n";

enum {
    PAGE_NAV,
    PAGE_NAV_OFF,
    PAGE_OVERVIEW,
    PAGE_ARROWS,
    PAGE_CLIPTEST,
    PAGE_CLIPTEST_PANEL,
    PAGE_CHOOSER
};
enum { LIVE_NAV, LIVE_OVERVIEW };

/* The coverage buffer is 96 KB; keep it out of the stack. */
static cov_t COV;

/* ------------------------------------------------------------------ demo */

static void
render_demo(cov_t *cov, canvas_t *cv, int page, const char *routes)
{
    cov_begin(cov);
    switch (page) {
    case PAGE_NAV_OFF:
        view_nav_demo(cov, 85);
        break;
    case PAGE_OVERVIEW:
        view_overview_demo(cov);
        break;
    case PAGE_ARROWS:
        view_arrows(cov);
        break;
    case PAGE_CLIPTEST:
        view_cliptest(cov);
        break;
    case PAGE_CLIPTEST_PANEL:
        view_cliptest_panel(cov);
        break;
    case PAGE_CHOOSER: {
        /* Dumpable so the list can be looked at without a device -- the
         * whole reason view_chooser() is portable and separate. */
        chooser_t ch;
        chooser_scan(&ch, routes);
        view_chooser(cov, &ch);
        break;
    }
    default:
        view_nav_demo(cov, 0);
        break;
    }
    cov_resolve(cov, cv);
}

/* ------------------------------------------------------------------ live */

/* The NAV map draws a stretch, not a route: at every rung of the ladder the
 * visible world is a few km across, and handing the page 20 000 vertices
 * would spend the frame projecting points well past the horizon. */
#define NAVWIN_MAX 600
#define NAVWIN_BACK_M 1500.0
#define NAVWIN_FWD_M 12000.0

/* Frame captures during a replay. Two pages and a moving map cannot be
 * caught by --demo, and DESIGN.md 10's fbshow --verify needs a file to
 * compare the panel against, so the interesting seconds are named up front. */
#define MAX_DUMPS 4
typedef struct {
    double at;
    const char *path;
    int done;
} dumpat_t;
static dumpat_t g_dumps[MAX_DUMPS];
static int g_ndumps;

/* --- dead reckoning, DESIGN.md 6.3 -------------------------------------- */

#define DR_FPS 8.0        /* while moving                                  */
#define DR_STOPPED_FPS 1.0/* "and 1 Hz when stopped"                       */
#define DR_MOVING_KMH 3.0 /* the same threshold the heading freeze uses     */
#define DR_SNAP_M 5.0     /* beyond this a fix is a correction, not drift   */
#define DR_EASE 3         /* frames a correction inside 5 m is spread over  */
#define DR_MAX_EXTRAP 2.0 /* seconds: after this the position simply holds  */
#define DR_MAX_CATCHUP 2.0/* seconds of missed frames worth redrawing       */

/* --- cue alerts, DESIGN.md 7.5 ------------------------------------------ */

/* "At 500 m, 200 m and 50 m from a cue, flash the keyboard LED." Nearer means
 * more urgent, so the count of blinks is the rung index plus one: one wink at
 * half a kilometre, three at fifty metres, and no need to look down to tell
 * them apart. */
#define NAV_ALERTS 3
static const double ALERT_M[NAV_ALERTS] = {500.0, 200.0, 50.0};

/* DESIGN.md 6.1: the EWMA is a time constant, not a per-frame alpha --
 * sim.py's 0.3 at 6 fps read to first order. See the section for the
 * arithmetic and for why 0.55 rather than the exact-equivalent 0.47. */
#define HEADING_TAU 0.55

typedef struct {
    route_t rt;
    navctx_t ctx;
    nav_t nv;
    fix_t fx;
    gps_t gps;

    /* What the pages actually draw: the per-fix state with the position (and
     * only the position) moved on by dead reckoning. The latches, the ETA
     * ring and the countdown belong to the fix that set them, so they are
     * copied across rather than recomputed eight times a second. */
    navctx_t rctx;
    nav_t rnv;

    double heading;    /* smoothed map rotation, radians clockwise from N */
    double raw_course; /* what the chevron gets                           */
    int have_heading;

    /* The extrapolation base: the last fix, and the correction being eased
     * across after the one before it. */
    double fix_e, fix_n, fix_course, fix_speed, fix_t, fix_mono;
    double err_e, err_n, err_course; /* prediction minus fix               */
    double fix_err;                  /* its magnitude, metres              */
    double ease_w;                   /* the weight applied this frame      */
    int ease_left;
    int have_dr;

    double dr_e, dr_n; /* the position being drawn */

    double fps;        /* the moving frame rate; --fps */
    double render_t;   /* ride seconds of the last frame drawn */
    int have_render;
    long frames_since_fix;

    /* Which rungs of ALERT_M[] this cue has already used, and which fired on
     * the fix just processed (for the frame trace). */
    int alert_cue, alert_done, alert_fired;

    int page, hold, quit, course_up;

    int rate_5hz; /* ask the receiver for 5 Hz at startup (DESIGN.md 6.3) */

    /* Metres per pixel while the rider has taken the NAV map off auto zoom;
     * 0 is auto. Not in navctx_t: it is a view decision, and nothing in the
     * route maths may depend on what the map happens to be showing. */
    double mpp_manual;
    int have_pos;
    double e, n; /* the fix, in route metres */

    long epochs;
    double t0; /* first epoch, seconds since midnight */
    double t;  /* this epoch, seconds since t0        */

    /* The slice of the route the NAV map is given. */
    double win_en[2 * NAVWIN_MAX];
    int win_n, win_pos_i, win_turn_i, win_pin_i;
    double win_pos_f;

    int cue_idx[ROUTE_MAXCUE];
} app_t;

static app_t APP;

static double
wrap_pi(double a)
{
    while (a > M_PI)
        a -= 2 * M_PI;
    while (a < -M_PI)
        a += 2 * M_PI;
    return a;
}

/* DESIGN.md 6.1: circular EWMA over a 0.55 s time constant, FROZEN below
 * 3 km/h. Without the freeze the map spins at every traffic light -- a
 * stationary receiver's reported course is pure noise -- and that is the
 * single most common complaint about course-up displays.
 *
 * `dt` is the interval since the previous FRAME, not since the previous fix:
 * at 8 Hz a per-frame constant would smooth eight times harder than the same
 * number did at 1 Hz. Expressing it as a time constant is what makes the
 * rotation look the same at either rate, and through a dropout. */
static void
heading_update(app_t *a, double raw, double kmh, double dt)
{
    if (!a->have_heading) {
        a->heading = raw;
        a->raw_course = raw;
        a->have_heading = 1;
        return;
    }
    if (kmh >= DR_MOVING_KMH) {
        double alpha = dt > 0.0 ? 1.0 - exp(-dt / HEADING_TAU) : 0.0;
        a->raw_course = raw;
        a->heading += wrap_pi(raw - a->heading) * alpha;
    } else {
        /* Frozen. The chevron follows the map rather than the noise, so the
         * whole instrument holds still instead of twitching at the lights. */
        a->raw_course = a->heading;
    }
}

/* --------------------------------------------------- dead reckoning (6.3) */

/* Where the last fix says we are at ride-second `t`, before any correction is
 * eased in: "pos(t) = last_fix + bearing(course) . speed . (t - t_fix)".
 *
 * Clamped at DR_MAX_EXTRAP. A receiver that has gone quiet has not stopped
 * the bike, but two seconds is as far as one second's course and speed can
 * honestly be projected -- past that the position holds where it is, and the
 * frames stop differing, so the frame skip of 6.4 makes them free. */
static void
dr_predict(const app_t *a, double t, double *pe, double *pn)
{
    double dt = t - a->fix_t;
    if (dt < 0.0)
        dt = 0.0;
    if (dt > DR_MAX_EXTRAP)
        dt = DR_MAX_EXTRAP;
    *pe = a->fix_e + sin(a->fix_course) * a->fix_speed * dt;
    *pn = a->fix_n + cos(a->fix_course) * a->fix_speed * dt;
}

/* A new fix, against what we had been drawing. Within 5 m the difference is
 * drift and gets eased across so the marker does not twitch; beyond 5 m it is
 * a genuine correction and is taken whole, because hiding it would be lying
 * about where the bike is. */
static void
dr_on_fix(app_t *a, double e, double n, double course_deg, double kmh,
          double t)
{
    double course = course_deg * (M_PI / 180.0);
    if (a->have_dr) {
        double pe, pn;
        dr_predict(a, t, &pe, &pn);
        a->fix_err = hypot(pe - e, pn - n);
        if (a->fix_err <= DR_SNAP_M) {
            a->err_e = pe - e;
            a->err_n = pn - n;
            a->err_course = wrap_pi(a->fix_course - course);
            a->ease_left = DR_EASE;
        } else {
            a->err_e = a->err_n = a->err_course = 0.0;
            a->ease_left = 0;
        }
    } else {
        a->fix_err = 0.0;
        a->err_e = a->err_n = a->err_course = 0.0;
        a->ease_left = 0;
        a->dr_e = e;
        a->dr_n = n;
    }
    a->fix_e = e;
    a->fix_n = n;
    a->fix_course = course;
    a->fix_speed = kmh / 3.6;
    a->fix_t = t;
    a->have_dr = 1;
    a->frames_since_fix = 0;
}

/* The position and course to draw at ride-second `t`. The correction decays
 * 3/4, 2/4, 1/4 over the DR_EASE frames after a fix and is gone on the
 * fourth, so a small correction is spread over exactly three frames. */
static void
dr_advance(app_t *a, double t, double dt)
{
    double pe, pn, w = 0.0;
    if (!a->have_dr)
        return;
    dr_predict(a, t, &pe, &pn);
    if (a->ease_left > 0) {
        w = a->ease_left / (double)(DR_EASE + 1);
        a->ease_left--;
    }
    a->ease_w = w;
    a->dr_e = pe + a->err_e * w;
    a->dr_n = pn + a->err_n * w;
    /* Course is interpolated the same way, so a course-up map rotates
     * continuously instead of in 1 Hz steps (6.3). */
    heading_update(a, wrap_pi(a->fix_course + a->err_course * w),
                   a->fix_speed * 3.6, dt);
    a->frames_since_fix++;
}

/* DESIGN.md 7.5. Each rung fires once per cue -- `alert_done` is cleared when
 * the announced cue changes and never otherwise, so jitter across a boundary
 * cannot re-fire one.
 *
 * Nothing fires while the off-route latch is set. Off route the announced cue
 * is not the junction you are riding towards, so a flash would be an
 * instruction to turn where there is no turn; 7.3 takes the panel over for the
 * same reason. The rungs are not consumed either, so they still fire once the
 * route is regained.
 *
 * Returns the bitmask that fired, for the trace. */
static int
alerts_update(app_t *a)
{
    int k, fired = 0, worst = -1;

    if (a->nv.cue_i != a->alert_cue) {
        a->alert_cue = a->nv.cue_i;
        a->alert_done = 0;
    }
    if (a->nv.off || a->nv.cue_i < 0)
        return 0;
    for (k = 0; k < NAV_ALERTS; k++) {
        if ((a->alert_done & (1 << k)) || a->nv.cue_m > ALERT_M[k])
            continue;
        a->alert_done |= 1 << k;
        fired |= 1 << k;
        worst = k;
    }
    if (worst >= 0)
        led_pulse(worst + 1, a->t);
    return fired;
}

/* Everything the pages read, at the dead-reckoned position rather than at the
 * last fix. route_snap() runs against a SEPARATE context so a render frame
 * can never touch the off-route latch, the countdown latch or the ETA ring --
 * those are decisions of the fix that made them (DESIGN.md 8: nav_t is
 * "recomputed each fix"), and re-deciding them eight times a second would
 * make the 3-fix off-route rule of 7.3 fire in three eighths of a second. */
static void
render_state(app_t *a)
{
    const route_t *r = &a->rt;

    a->rnv = a->nv;
    if (!a->have_pos || !a->have_dr || a->nv.seg < 0)
        return;
    /* Anchored on the last real match, never on the previous frame's: the
     * window hint must not be able to walk off down the route on its own. */
    a->rctx.last_seg = a->ctx.last_seg;
    a->rctx.have_fix = 1;
    a->rctx.last_fix_t = 0;
    route_snap(r, &a->rctx, a->dr_e, a->dr_n, (time_t)0, &a->rnv);
    a->rnv.off = a->nv.off; /* the latch is the fix's, not this frame's */
    /* The announced cue is the fix's too, so the map's bend and the panel's
     * arrow can never disagree; only the distance to it moves. */
    if (a->rnv.cue_i >= 0 && a->rnv.cue_i < r->ncue) {
        a->rnv.cue_m = r->cue[a->rnv.cue_i].along_m - a->rnv.along;
        if (a->rnv.cue_m < 0.0)
            a->rnv.cue_m = 0.0;
    }
    a->rnv.togo_m = r->total_m - a->rnv.along;
    if (a->rnv.togo_m < 0.0)
        a->rnv.togo_m = 0.0;
    a->rnv.pct = r->total_m > 0.0 ? 100.0 * a->rnv.along / r->total_m : 0.0;
    if (a->rnv.pct > 100.0)
        a->rnv.pct = 100.0;
}

/* The vertices within reach of the fix, thinned to fit. Sets win_*. */
static void
build_window(app_t *a)
{
    const route_t *r = &a->rt;
    int seg = a->rnv.seg, lo, hi, step, i, m = 0, j;
    double a0, a1;

    if (seg < 0)
        seg = 0;
    for (lo = seg; lo > 0 && r->cum[seg] - r->cum[lo] < NAVWIN_BACK_M; lo--)
        ;
    for (hi = seg + 1;
         hi < r->npt - 1 && r->cum[hi] - r->cum[seg] < NAVWIN_FWD_M; hi++)
        ;
    step = (hi - lo) / (NAVWIN_MAX - 1) + 1;
    for (i = lo; i <= hi && m < NAVWIN_MAX - 1; i += step, m++) {
        a->win_en[2 * m] = r->en[2 * i];
        a->win_en[2 * m + 1] = r->en[2 * i + 1];
    }
    a->win_en[2 * m] = r->en[2 * hi];
    a->win_en[2 * m + 1] = r->en[2 * hi + 1];
    a->win_n = m + 1;

    /* Where the fix sits in the thinned array. Interpolating on cumulative
     * distance rather than on the index is what keeps the marker on the line
     * when step > 1 and two window vertices are far apart. */
    j = (seg - lo) / step;
    if (j > a->win_n - 2)
        j = a->win_n - 2;
    if (j < 0)
        j = 0;
    a0 = r->cum[lo + j * step];
    a1 = (lo + (j + 1) * step <= hi) ? r->cum[lo + (j + 1) * step] : r->cum[hi];
    a->win_pos_i = j;
    a->win_pos_f = a1 > a0 ? (a->rnv.along - a0) / (a1 - a0) : 0.0;
    if (a->win_pos_f < 0.0)
        a->win_pos_f = 0.0;
    if (a->win_pos_f > 1.0)
        a->win_pos_f = 1.0;

    a->win_turn_i = a->win_pin_i = -1;
    if (a->rnv.cue_i >= 0) {
        int v = r->cue[a->rnv.cue_i].idx;
        if (v >= lo && v <= hi)
            a->win_turn_i = (v - lo + step / 2) / step;
        if (a->rnv.cue_i + 1 < r->ncue) {
            v = r->cue[a->rnv.cue_i + 1].idx;
            if (v >= lo && v <= hi)
                a->win_pin_i = (v - lo + step / 2) / step;
        }
    }
    if (a->win_turn_i > a->win_n - 1)
        a->win_turn_i = a->win_n - 1;
    if (a->win_pin_i > a->win_n - 1)
        a->win_pin_i = a->win_n - 1;
    /* The announced cue has to be ahead of the marker, or map_cue_distance()
     * measures backwards and auto-zoom clamps to the tightest rung. */
    if (a->win_turn_i <= a->win_pos_i)
        a->win_turn_i = a->win_pos_i + 1;
}

/* The OVERVIEW strip's two big numbers. Kilometres or miles -- the strip
 * prints the unit beside them, so the digits carry no label of their own. */
static void
fmt_big(char *buf, size_t n, double metres, int units)
{
    double per = units == UNITS_IMPERIAL
                     ? GEO_FT_PER_MILE / GEO_FT_PER_M
                     : 1000.0;
    snprintf(buf, n, "%.1f", metres / per);
}

/* Time left on the route, centred under the rule. Minutes while there are
 * fewer than sixty of them, then hours and minutes -- "97 MIN" is a number
 * you have to convert, and this is meant to be read at a glance. */
static void
fmt_remaining(char *buf, size_t n, double eta_s)
{
    int mins;
    if (eta_s < 0.0) {
        /* Stopped, or too little history to average: no figure rather than a
         * wrong one, and a dash says which. */
        snprintf(buf, n, "-- MIN");
        return;
    }
    mins = (int)(eta_s / 60.0 + 0.5);
    if (mins < 60)
        snprintf(buf, n, "%d MIN", mins);
    else
        snprintf(buf, n, "%dH %02dM", mins / 60, mins % 60);
}

/* Arrival, 12-hour, value first and the label trailing. No meridiem: on a
 * ride you know whether it is morning, and "12:42 ETA" is nine characters
 * against a ten-character panel -- it always fits, with no degradation rule
 * to reason about. */
static void
fmt_eta(char *buf, size_t n, time_t eta)
{
    struct tm tm;
    int h12;
    if (eta == 0) {
        snprintf(buf, n, "-- ETA");
        return;
    }
    localtime_r(&eta, &tm);
    h12 = tm.tm_hour % 12;
    if (h12 == 0)
        h12 = 12;
    snprintf(buf, n, "%d:%02d ETA", h12, tm.tm_min);
}

static void
fmt_clock(char *buf, size_t n, time_t t)
{
    struct tm tmv;
    if (t <= 0) {
        snprintf(buf, n, "--:--");
        return;
    }
    tmv = *localtime(&t);
    snprintf(buf, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

/* /sys/firmware/beepy/battery_percent, or -1 when it is not there -- the
 * panel then prints the clock alone rather than a confident "0%". */
static int
read_battery(void)
{
    FILE *f = fopen("/sys/firmware/beepy/battery_percent", "r");
    int v = -1;
    if (!f)
        return -1;
    if (fscanf(f, "%d", &v) != 1)
        v = -1;
    fclose(f);
    return v;
}

static void
render_live(app_t *a, cov_t *cov, canvas_t *cv)
{
    const route_t *r = &a->rt;
    char clock[8], togo[16], total[16], remain[16], etabuf[24];

    cov_begin(cov);
    if (a->page == LIVE_OVERVIEW) {
        overview_t o;
        int i;
        for (i = 0; i < r->ncue && i < ROUTE_MAXCUE; i++)
            a->cue_idx[i] = r->cue[i].idx;
        fmt_big(togo, sizeof togo, a->rnv.togo_m, a->ctx.units);
        fmt_big(total, sizeof total, r->total_m, a->ctx.units);
        fmt_clock(clock, sizeof clock, a->nv.eta);
        o.pts = r->en;
        o.npts = r->npt;
        o.cue_idx = a->cue_idx;
        o.ncue_dots = i;
        o.pos_i = a->rnv.seg < 0 ? 0 : a->rnv.seg;
        o.pos_f = 0.0;
        if (a->rnv.seg >= 0 && r->cum[a->rnv.seg + 1] > r->cum[a->rnv.seg])
            o.pos_f = (a->rnv.along - r->cum[a->rnv.seg]) /
                      (r->cum[a->rnv.seg + 1] - r->cum[a->rnv.seg]);
        o.name = r->name;
        o.total = total;
        o.togo = togo;
        o.eta = clock;
        o.done = (int)(a->rnv.pct + 0.5);
        o.cue_i = a->rnv.cue_i < 0 ? 0 : a->rnv.cue_i;
        o.ncues = r->ncue;
        o.units = a->ctx.units;
        view_overview(cov, &o);
    } else {
        navmap_t m;
        panel_t p;
        build_window(a);
        m.pts = a->win_en;
        m.npts = a->win_n;
        m.pos_i = a->win_pos_i;
        m.pos_f = a->win_pos_f;
        m.turn_i = a->win_turn_i;
        m.pin_i = a->win_pin_i;
        /* The marker leaves the line only once the latch has fired: one
         * noisy fix must not throw it into the verge (DESIGN.md 7.3). */
        m.off = a->rnv.off ? a->rnv.off_m : 0.0;
        m.spd_kmh = (int)(a->fx.speed_kmh + 0.5);
        m.course_up = a->course_up;
        m.units = a->ctx.units;
        m.mpp_manual = a->mpp_manual;
        /* DESIGN.md 6.1: the map turns with the SMOOTHED heading, and 1.1:
         * the chevron gets what is left over, so it keeps pointing along the
         * road while the rotation catches up. North-up (theta 0) leaves the
         * chevron carrying the whole course, which is what makes a north-up
         * map readable at all. */
        m.heading = a->heading;
        m.have_heading = a->have_heading;
        m.residual =
            a->have_heading
                ? wrap_pi(a->raw_course - (a->course_up ? a->heading : 0.0))
                : 0.0;

        fmt_clock(clock, sizeof clock, time(NULL));
        remain[0] = etabuf[0] = '\0';
        /* The PANEL's numbers stay on the fix's clock: the countdown is
         * latched and quantised precisely so it changes slowly (1.1.1), and
         * the metres-off figure would flicker if it were re-rounded eight
         * times a second. Only the map moves at the frame rate. */
        p.off = a->nv.off ? (int)(a->nv.off_m + 0.5) : 0;
        p.turn_m = a->nv.cue_q; /* quantised + latched, not raw metres */
        p.units = a->ctx.units;
        p.kind = a->rnv.cue_i >= 0 ? r->cue[a->rnv.cue_i].kind : CUE_DEST;
        fmt_remaining(remain, sizeof remain, a->nv.eta_s);
        fmt_eta(etabuf, sizeof etabuf, a->nv.eta);
        p.remain = remain;
        p.eta = etabuf;
        p.togo_m = a->nv.seg >= 0 ? a->nv.togo_m : -1.0;
        p.batt = read_battery();
        p.clock = clock;
        view_nav(cov, &m, &p);
    }
    cov_resolve(cov, cv);
}

/* ----------------------------------------------------------------- trace */

static FILE *g_trace;

static void
trace_open(const char *path)
{
    g_trace = fopen(path, "w");
    if (!g_trace) {
        perror(path);
        exit(1);
    }
    fputs("#t\tlat\tlon\tseg\talong_m\toff_m\toff_latched\tcue_i\tcue_m\t"
          "togo_m\tpct\teta_s\theading_deg\tzoom_mpp\tpresented\t"
          "course_deg\tresidual_deg\n",
          g_trace);
}

static void
trace_row(const app_t *a, double zoom_mpp, int presented)
{
    if (!g_trace)
        return;
    fprintf(g_trace,
            "%.3f\t%.7f\t%.7f\t%d\t%.2f\t%.2f\t%d\t%d\t%.2f\t%.2f\t%.3f\t"
            "%.1f\t%.2f\t%.2f\t%d\t%.2f\t%.2f\n",
            a->t, a->fx.lat, a->fx.lon, a->nv.seg, a->nv.along, a->nv.off_m,
            a->nv.off, a->nv.cue_i, a->nv.cue_m, a->nv.togo_m, a->nv.pct,
            a->nv.eta_s, a->heading * (180.0 / M_PI), zoom_mpp, presented,
            a->raw_course * (180.0 / M_PI),
            wrap_pi(a->raw_course - (a->course_up ? a->heading : 0.0)) *
                (180.0 / M_PI));
}

/* One row per rendered FRAME rather than per fix -- the DESIGN.md 6.3 maths
 * happens eight times a second and a per-fix trace cannot see any of it. Every
 * quantity the extrapolation is built from is here, so tools/assert_trace.py
 * can recompute the closed form and check the drawn position against it. */
static FILE *g_ftrace;

static void
ftrace_open(const char *path)
{
    g_ftrace = fopen(path, "w");
    if (!g_ftrace) {
        perror(path);
        exit(1);
    }
    fputs("#t\tisfix\tsince_fix\tbase_e\tbase_n\tbase_crs\tbase_spd\tdt\t"
          "err_e\terr_n\tease_w\tdr_e\tdr_n\tcourse_deg\theading_deg\t"
          "presented\tms\tled\toff_latched\tcue_i\tcue_m\n",
          g_ftrace);
}

static void
ftrace_row(const app_t *a, double t, int isfix, int presented, double ms)
{
    double dt = t - a->fix_t;
    if (!g_ftrace)
        return;
    if (dt < 0.0)
        dt = 0.0;
    if (dt > DR_MAX_EXTRAP)
        dt = DR_MAX_EXTRAP;
    fprintf(g_ftrace,
            "%.4f\t%d\t%ld\t%.4f\t%.4f\t%.6f\t%.4f\t%.4f\t%.4f\t%.4f\t%.4f\t"
            "%.4f\t%.4f\t%.3f\t%.3f\t%d\t%.3f\t%d\t%d\t%d\t%.2f\n",
            t, isfix, a->frames_since_fix, a->fix_e, a->fix_n,
            a->fix_course * (180.0 / M_PI), a->fix_speed, dt, a->err_e,
            a->err_n, a->ease_w, a->dr_e, a->dr_n,
            a->raw_course * (180.0 / M_PI), a->heading * (180.0 / M_PI),
            presented, ms, isfix ? a->alert_fired : 0, a->nv.off,
            a->rnv.cue_i, a->rnv.cue_m);
}

/* --------------------------------------------------------- the fix cycle */

/* The ride-clock second this epoch belongs to, WITHOUT consuming it: the
 * frames covering the gap since the last fix have to be drawn from the old
 * extrapolation base before that base is replaced. */
static double
epoch_seconds(const app_t *a)
{
    double secs = fix_utc_seconds(a->gps.utc), t;
    if (a->epochs == 0)
        return 0.0;
    if (secs < 0.0)
        return (double)a->epochs;
    t = secs - a->t0;
    if (t < -43200.0) /* midnight, once a ride */
        t += 86400.0;
    return t;
}

/* One completed NMEA epoch: everything the two pages consume, recomputed in
 * DESIGN.md 7's order -- snap, latch, cue, progress. Returns 1 when there was
 * a position to work from. */
static int
on_epoch(app_t *a, time_t now)
{
    double secs;
    if (!fix_from_gps(&a->gps, now, &a->fx))
        return 0; /* no position this epoch; nothing to recompute */

    a->epochs++;
    secs = fix_utc_seconds(a->fx.utc);
    if (secs >= 0.0) {
        if (a->epochs == 1)
            a->t0 = secs;
        a->t = secs - a->t0;
        if (a->t < -43200.0) /* midnight, once a ride */
            a->t += 86400.0;
    } else {
        a->t = (double)(a->epochs - 1);
    }

    geo_project(a->rt.lat0, a->rt.lon0, a->fx.lat, a->fx.lon, &a->e, &a->n);
    a->have_pos = 1;
    /* The ride clock, not the wall clock. DESIGN.md 7.2's "after 30 s lost"
     * is about how long the RECEIVER has been quiet, which the sentences
     * themselves say; wall time would also make the rule untestable, because
     * an unpaced replay of an hour's riding takes a second. */
    route_snap(&a->rt, &a->ctx, a->e, a->n, (time_t)a->t, &a->nv);
    route_offroute_update(&a->ctx, &a->nv);
    route_cue_ahead(&a->rt, &a->nv);
    route_progress(&a->rt, &a->ctx, a->t, &a->nv);
    dr_on_fix(a, a->e, a->n, a->fx.course, a->fx.speed_kmh, a->t);
    a->alert_fired = alerts_update(a);
    return 1;
}

/* The rung the NAV map will land on, for the trace. Recomputed here rather
 * than reached out of view_nav.c, which keeps the page a pure function of
 * its arguments -- and lets the trace assert the DESIGN.md 6.1 ladder from
 * outside the renderer, which is where a regression would show. */
static double
live_zoom(const app_t *a)
{
    return a->mpp_manual > 0.0 ? a->mpp_manual
                               : map_auto_zoom(a->nv.cue_m, SCR_H * 0.72);
}

/* -------------------------------------------------------- the frame clock
 *
 * DESIGN.md 6.3 puts the display on its own clock: 8 Hz while moving, 1 Hz
 * when stopped, with the position extrapolated between fixes. Everything the
 * loop needs to draw one of those frames lives here so that the replay branch
 * and the serial branch drive the same code -- the alternative is two frame
 * schedulers that disagree, and only one of them testable.
 */

#define STAT_BINS 1000 /* 0.1 ms each, so 100 ms and an overflow bin */

typedef struct {
    canvas_t *cv, *prev;
    int have_fb;
#ifdef NAV_DEVICE
    fb_t *fb;
#endif
    int paced;
    double pace0; /* monotonic seconds at ride t = 0 */
    int have_pace0;

    int stats;
    long frames, presents;
    double ms_sum, ms_max;
    int hist[STAT_BINS + 1];
    double sec_at; /* ride second the current stats bucket started */
    long sec_frames, sec_presents;
    double sec_ms;
} render_ctx_t;

static render_ctx_t RC;

static double
mono_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void
sleep_s(double s)
{
    struct timespec ts;
    if (s <= 0.0)
        return;
    if (s > 5.0)
        s = 5.0; /* a dropout must not stall the replay for its full length */
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

/* Hold a paced replay to the clock the sentences carry. Pacing per FRAME
 * rather than per epoch is what makes a paced replay look like the ride: the
 * old per-epoch sleep would have drawn eight frames and then waited a whole
 * second with the map frozen. */
static void
pace_to(double t)
{
    if (!RC.paced)
        return;
    if (!RC.have_pace0) {
        RC.pace0 = mono_now() - t;
        RC.have_pace0 = 1;
        return;
    }
    sleep_s(RC.pace0 + t - mono_now());
}

static void
stats_flush(const app_t *a, int final)
{
    long skipped = RC.sec_frames - RC.sec_presents;
    if (!RC.stats)
        return;
    if (RC.sec_frames > 0)
        fprintf(stderr, "stats t=%6.1f  drawn %3ld  presented %3ld  "
                        "skipped %3ld  render %.2f ms avg\n",
                a->render_t, RC.sec_frames, RC.sec_presents, skipped,
                RC.sec_ms / (double)RC.sec_frames);
    RC.sec_frames = RC.sec_presents = 0;
    RC.sec_ms = 0.0;
    if (!final)
        return;
    if (RC.frames > 0) {
        long want = (long)(0.95 * (double)RC.frames), seen = 0;
        double p95 = 0.0;
        int i;
        for (i = 0; i <= STAT_BINS; i++) {
            seen += RC.hist[i];
            if (seen >= want) {
                p95 = (i + 1) * 0.1;
                break;
            }
        }
        fprintf(stderr,
                "stats: %ld frames, %ld presented, %ld skipped (%.0f%%), "
                "render %.2f ms mean, %.2f ms p95, %.2f ms max\n",
                RC.frames, RC.presents, RC.frames - RC.presents,
                100.0 * (double)(RC.frames - RC.presents) / (double)RC.frames,
                RC.ms_sum / (double)RC.frames, p95, RC.ms_max);
    }
}

/* Draw one frame at ride-second `t`, present it only if it differs from the
 * last one presented, and account for it. Returns 1 when it was presented. */
static int
frame_at(app_t *a, double t)
{
    double dt = a->have_render ? t - a->render_t : 0.0, ms;
    struct timespec c0, c1;
    int presented, bin, d;

    pace_to(t);
    dr_advance(a, t, dt);
    render_state(a);

    clock_gettime(CLOCK_MONOTONIC, &c0);
    render_live(a, &COV, RC.cv);
    clock_gettime(CLOCK_MONOTONIC, &c1);
    ms = (double)(c1.tv_sec - c0.tv_sec) * 1e3 +
         (double)(c1.tv_nsec - c0.tv_nsec) / 1e6;

    /* DESIGN.md 6.4: a frame identical to the last is not sent. That is what
     * makes a high nominal rate affordable -- a stationary display costs
     * nothing regardless of it. */
    presented = memcmp(RC.cv->bits, RC.prev->bits,
                       (size_t)RC.cv->stride * RC.cv->h) != 0;
    if (presented) {
        memcpy(RC.prev->bits, RC.cv->bits, (size_t)RC.cv->stride * RC.cv->h);
#ifdef NAV_DEVICE
        if (RC.have_fb)
            fb_present(RC.fb, RC.cv);
#endif
    }

    RC.frames++;
    RC.presents += presented;
    RC.ms_sum += ms;
    if (ms > RC.ms_max)
        RC.ms_max = ms;
    bin = (int)(ms * 10.0);
    if (bin < 0)
        bin = 0;
    if (bin > STAT_BINS)
        bin = STAT_BINS;
    RC.hist[bin]++;
    RC.sec_frames++;
    RC.sec_presents += presented;
    RC.sec_ms += ms;

    a->render_t = t;
    a->have_render = 1;
    /* frames_since_fix is 1 only on the frame drawn immediately after a fix,
     * which is that fix's own frame. */
    ftrace_row(a, t, a->frames_since_fix == 1, presented, ms);
    if (a->frames_since_fix == 1)
        a->alert_fired = 0;
    led_tick(t);
    if (RC.stats && t - RC.sec_at >= 1.0) {
        stats_flush(a, 0);
        RC.sec_at = t;
    }
    for (d = 0; d < g_ndumps; d++)
        if (!g_dumps[d].done && t >= g_dumps[d].at) {
            g_dumps[d].done = 1;
            if (canvas_dump(RC.cv, g_dumps[d].path) < 0)
                perror(g_dumps[d].path);
            else
                fprintf(stderr, "wrote %s at t=%.2f\n", g_dumps[d].path, t);
        }
    return presented;
}

/* Every frame the display owes between the last one drawn and ride-second
 * `until`, exclusive -- the extrapolated ones. Called with the OLD fix still
 * in place, which is the whole point: those frames belong to it. */
static void
frames_before(app_t *a, double until)
{
    double dt;
    if (a->hold || !a->have_render || !a->have_dr)
        return;
    dt = 1.0 / (a->fix_speed * 3.6 >= DR_MOVING_KMH ? a->fps : DR_STOPPED_FPS);
    /* A long silence is not worth animating frame by frame: draw the last
     * couple of seconds and let the rest go. Without this a 40 s dropout
     * costs 320 identical renders. */
    if (until - a->render_t > DR_MAX_CATCHUP)
        a->render_t = until - DR_MAX_CATCHUP;
    while (a->render_t + dt < until - 1e-9)
        frame_at(a, a->render_t + dt);
}

/* ------------------------------------------------- the receiver's own rate
 *
 * DESIGN.md 6.3: "Optionally raise the receiver to 5 Hz with UBX-CFG-RATE
 * (measRate = 200 ms), which shortens the extrapolation from 1 s to 200 ms.
 * That is a menu-free one-shot message at startup, gated behind a config
 * flag, and it is a refinement rather than a dependency -- dead reckoning is
 * what actually buys the smoothness."
 *
 * Off by default, and the default is the honest one. The measured environment
 * of section 3 is a u-blox 7 at 9600 baud emitting RMC, VTG, GGA, GSA and GSV
 * -- call it 450 bytes an epoch. 9600 8N1 carries 960 bytes a second. One
 * epoch per second fits with room to spare; FIVE need about 2 250 B/s, which
 * is more than twice what the line can carry, so the receiver will simply not
 * deliver whole epochs. Making 5 Hz genuinely useful needs the port taken to
 * 38400 with UBX-CFG-PRT, or GSV and GSA turned off with UBX-CFG-MSG -- and
 * the second of those would silently break gps-monitor's sky page, which
 * shares the receiver and whose whole subject is GSV. Neither belongs in this
 * commit. What is here is the message the design named, an honest ACK check,
 * and a --trace whose fix cadence shows exactly what the receiver did with it.
 *
 * Device-only: there is no port in the host lane to configure.
 */
#ifdef NAV_DEVICE

#define UBX_5HZ_MS 200

/* Fletcher-8 over class, id, length and payload -- everything between the
 * 0xB5 0x62 sync and the checksum itself. */
static void
ubx_cksum(const unsigned char *b, size_t n, unsigned char *out)
{
    unsigned char a = 0, c = 0;
    size_t i;
    for (i = 0; i < n; i++) {
        a = (unsigned char)(a + b[i]);
        c = (unsigned char)(c + a);
    }
    out[0] = a;
    out[1] = c;
}

/* Wait for UBX-ACK-ACK or UBX-ACK-NAK naming (cls, id), up to `ms`.
 *
 * The bytes eaten here are NMEA that never reaches the parser, which is why
 * this runs once, before the loop: two seconds of sentences at startup is
 * nothing, and the alternative -- threading a UBX matcher through the main
 * read path -- is a permanent complication for a one-shot message.
 *
 * Returns 1 for ACK, 0 for NAK, -1 for silence. */
static int
ubx_await_ack(int fd, unsigned char cls, unsigned char id, int ms)
{
    /* B5 62 05 ack 02 00 cls id: eight bytes, matched as a sliding window. */
    unsigned char w[8];
    double deadline = mono_now() + ms / 1000.0;
    int have = 0;

    for (;;) {
        struct pollfd pfd;
        unsigned char b;
        double left = deadline - mono_now();
        if (left <= 0.0)
            return -1;
        pfd.fd = fd;
        pfd.events = POLLIN;
        if (poll(&pfd, 1, (int)(left * 1000.0) + 1) <= 0)
            return -1;
        if (read(fd, &b, 1) != 1)
            return -1;
        if (have == (int)sizeof w) {
            memmove(w, w + 1, sizeof w - 1);
            have--;
        }
        w[have++] = b;
        if (have == (int)sizeof w && w[0] == 0xB5 && w[1] == 0x62 &&
            w[2] == 0x05 && (w[3] == 0x01 || w[3] == 0x00) && w[4] == 0x02 &&
            w[5] == 0x00 && w[6] == cls && w[7] == id)
            return w[3] == 0x01;
    }
}

/* UBX-CFG-RATE: measRate ms, navRate 1 (a solution every measurement),
 * timeRef 1 (GPS time). Returns what ubx_await_ack() did. */
static int
ubx_set_rate(int fd, int meas_ms)
{
    unsigned char body[10], msg[14];
    ssize_t n;

    body[0] = 0x06; /* class CFG */
    body[1] = 0x08; /* id    RATE */
    body[2] = 0x06; /* payload length, little endian */
    body[3] = 0x00;
    body[4] = (unsigned char)(meas_ms & 0xFF);
    body[5] = (unsigned char)((meas_ms >> 8) & 0xFF);
    body[6] = 0x01; /* navRate */
    body[7] = 0x00;
    body[8] = 0x01; /* timeRef: GPS time */
    body[9] = 0x00;

    msg[0] = 0xB5;
    msg[1] = 0x62;
    memcpy(msg + 2, body, sizeof body);
    ubx_cksum(body, sizeof body, msg + 12);

    n = write(fd, msg, sizeof msg);
    if (n != (ssize_t)sizeof msg) {
        perror("beepy-nav: UBX-CFG-RATE");
        return -1;
    }
    return ubx_await_ack(fd, 0x06, 0x08, 2000);
}

/* On the wire there is no ride clock to read ahead of, so it is the last
 * fix's second plus however long the monotonic clock says has passed. */
static double
live_ride_now(const app_t *a)
{
    return a->fix_t + (mono_now() - a->fix_mono);
}

static void
live_frames(app_t *a)
{
    if (!a->have_dr || a->hold)
        return;
    frames_before(a, live_ride_now(a));
}

/* How long poll() may sleep without missing the next frame. */
static int
live_poll_ms(const app_t *a)
{
    double dt, left;
    if (!a->have_dr || a->hold || !a->have_render)
        return 1000;
    dt = 1.0 / (a->fix_speed * 3.6 >= DR_MOVING_KMH ? a->fps : DR_STOPPED_FPS);
    left = (a->render_t + dt) - live_ride_now(a);
    if (left <= 0.001)
        return 1;
    if (left > 1.0)
        return 1000;
    return (int)(left * 1000.0);
}
#endif

/* ------------------------------------------------------------------ keys
 *
 * Device-only, and not for want of trying to keep it portable: DESIGN.md 2
 * has the keys coming from /dev/input/event0 with EVIOCGRAB, because fbterm
 * is SIGSTOPped for as long as the panel is owned. The host lane runs
 * headless replays, which have nobody to press anything. */
#ifdef NAV_DEVICE

/* A key changes what the panel says, and the answer has to arrive with the
 * press. Waiting for the next tick of the frame clock would mean up to a
 * whole second at the 1 Hz stopped rate -- press U, watch nothing happen,
 * press it again -- which is how an instrument comes to feel broken.
 *
 * Deliberately NOT frame_at(): this is not a frame of the ride clock. The
 * ease of 6.3 is spread over three frames OF MOTION and a rider pressing a
 * key must not consume one of them, the stats must not count a frame the
 * display did not owe, and the traces stay one row per real frame. What it
 * shares with frame_at() is the only part that matters here -- the 6.4 skip,
 * so a key that changes nothing visible still costs no SPI. */
static void
key_repaint(app_t *a)
{
    size_t nb;
    if (a->hold || !a->have_render || !RC.cv)
        return;
    render_state(a);
    render_live(a, &COV, RC.cv);
    nb = (size_t)RC.cv->stride * RC.cv->h;
    if (memcmp(RC.cv->bits, RC.prev->bits, nb) == 0)
        return;
    memcpy(RC.prev->bits, RC.cv->bits, nb);
    if (RC.have_fb)
        fb_present(RC.fb, RC.cv);
}

static int
handle_key(app_t *a, int ch)
{
    switch (ch) {
    case '\t':
        a->page = a->page == LIVE_NAV ? LIVE_OVERVIEW : LIVE_NAV;
        break;
    case 'q':
    case 'Q':
        a->quit = 1;
        return 1; /* no repaint: the next thing that happens is the exit */
    case 'h':
    case 'H':
        a->hold = !a->hold;
        break;
    case 'o':
    case 'O':
        a->course_up = !a->course_up;
        break;
    /* DESIGN.md 6.1: "Z/X switch to manual; A returns to auto". Stepping
     * from live_zoom() rather than from mpp_manual is what makes the first
     * press continuous -- the map moves ONE rung from what it was showing,
     * instead of jumping to wherever a stale manual value was left. */
    case 'z':
    case 'Z':
        a->mpp_manual = map_zoom_step(live_zoom(a), +1);
        break;
    case 'x':
    case 'X':
        a->mpp_manual = map_zoom_step(live_zoom(a), -1);
        break;
    case 'a':
    case 'A':
        a->mpp_manual = 0.0;
        break;
    /* 1.1.1: the ladder changes under the countdown, so the latch has to be
     * reset -- nav_set_units() does that -- and then re-evaluated, or the
     * panel would keep the old ladder's number until the next fix. */
    case 'u':
    case 'U':
        nav_set_units(&a->ctx, a->ctx.units == UNITS_METRIC ? UNITS_IMPERIAL
                                                            : UNITS_METRIC);
        route_countdown_refresh(&a->rt, &a->ctx, &a->nv);
        break;
    default:
        return 0;
    }
    key_repaint(a);
    return 1;
}

static int
keycode_to_char(int code)
{
    switch (code) {
    case KEY_TAB:
        return '\t';
    case KEY_Q:
        return 'q';
    case KEY_H:
        return 'h';
    case KEY_O:
        return 'o';
    case KEY_Z:
        return 'z';
    case KEY_X:
        return 'x';
    case KEY_A:
        return 'a';
    case KEY_U:
        return 'u';
    default:
        return 0;
    }
}

static fb_t *g_fb;
static struct termios g_tty_saved;
static int g_tty_raw;

static void
tty_raw(void)
{
    struct termios t;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_tty_saved) < 0)
        return;
    t = g_tty_saved;
    t.c_lflag &= (unsigned)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
        g_tty_raw = 1;
}

/* Every exit path leaves fbterm in S+, never T (DESIGN.md 10). */
static void
cleanup(void)
{
    led_off();
    if (g_tty_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);
    evdev_close();
    if (g_fb)
        fb_release(g_fb);
}
#endif

/* Set when the route chooser has already opened the panel and grabbed the
 * keyboard, so run_live() inherits them instead of taking them again. */
#ifdef NAV_DEVICE
static fb_t *g_preopened;
#endif

#ifdef NAV_DEVICE
/* Non-blocking sweep of every key source. The replay branch of the loop is
 * driven by the FILE, not by poll(), so without this a replay on the device
 * ignores the keyboard completely -- no page switch and, worse, no way to
 * quit but a signal from another terminal. Timeout 0: this is a peek
 * between fixes, not a wait. */
static void
drain_keys(app_t *a)
{
    struct pollfd pfd[1 + MAX_EVDEV];
    int np = 0, i;
    if (g_tty_raw) {
        pfd[np].fd = STDIN_FILENO;
        pfd[np].events = POLLIN;
        np++;
    }
    for (i = 0; i < evdev_count(); i++) {
        pfd[np].fd = evdev_fd(i);
        pfd[np].events = POLLIN;
        np++;
    }
    if (np == 0 || poll(pfd, (unsigned)np, 0) <= 0)
        return;
    for (i = 0; i < np; i++) {
        int code, value;
        char k;
        if (!(pfd[i].revents & POLLIN))
            continue;
        if (g_tty_raw && pfd[i].fd == STDIN_FILENO) {
            while (read(STDIN_FILENO, &k, 1) == 1)
                handle_key(a, k);
            continue;
        }
        while (evdev_next_key(pfd[i].fd, &code, &value))
            if (value == 1)
                handle_key(a, keycode_to_char(code));
    }
}
#endif

static volatile sig_atomic_t g_signal_quit;

static void
on_signal(int s)
{
    (void)s;
    g_signal_quit = 1;
}

/* --------------------------------------------------------------- chooser */

#ifdef NAV_DEVICE
/* DESIGN.md 2: run with no route and it lists the .gpx files in
 * /home/beepy/routes and waits for a selection -- "a startup chooser, not a
 * page".
 *
 * It has to be drawn on the panel and driven by evdev, because by the time
 * this runs fbterm is SIGSTOPped and stdin is going nowhere. That is the
 * whole reason this cannot be twenty lines of fgets().
 *
 * Returns 0 with the path in `out`, or -1 when the rider quit. */
/* One keycode. Returns 1 when the chooser is finished, setting *rc to 0 for
 * a selection and leaving it at -1 for a quit. Shared by the evdev and the
 * stdin paths so the two cannot drift apart. */
static int
chooser_key(chooser_t *ch, int code, char *out, size_t outsz, int *rc)
{
    switch (code) {
    case KEY_N:
    case KEY_DOWN:
        chooser_move(ch, 1);
        return 0;
    case KEY_P:
    case KEY_UP:
        chooser_move(ch, -1);
        return 0;
    case KEY_ENTER:
    case KEY_KPENTER:
        if (ch->n <= 0)
            return 0; /* nothing to load; the page already says why */
        snprintf(out, outsz, "%s", ch->path[ch->sel]);
        *rc = 0;
        return 1;
    case KEY_Q:
    case KEY_ESC:
        return 1;
    default:
        return 0;
    }
}

static int
chooser_run(char *out, size_t outsz, fb_t *fb, const char *dir)
{
    chooser_t ch;
    canvas_t *cv = canvas_new(SCR_W, SCR_H);
    int done = 0, rc = -1, dirty = 1;

    if (!cv)
        return -1;
    chooser_scan(&ch, dir);

    while (!done && !g_signal_quit) {
        struct pollfd pfd[1 + MAX_EVDEV];
        int np = 0, i;
        if (dirty) {
            cov_begin(&COV);
            view_chooser(&COV, &ch);
            cov_resolve(&COV, cv);
            fb_present(fb, cv);
            dirty = 0;
        }
        /* evdev is the real input (DESIGN.md 2), and stdin is the same
         * debugging affordance the main loop has: a chooser that only a
         * physical thumb can drive cannot be verified over ssh, and the one
         * thing it must never do is hang holding the panel. */
        if (g_tty_raw) {
            pfd[np].fd = STDIN_FILENO;
            pfd[np].events = POLLIN;
            np++;
        }
        for (i = 0; i < evdev_count(); i++) {
            pfd[np].fd = evdev_fd(i);
            pfd[np].events = POLLIN;
            np++;
        }
        if (np == 0) {
            /* No keyboard at all: the list is on screen but unusable, and a
             * poll with no fds would spin. Say so rather than hang with a
             * picture of a menu. */
            fputs("beepy-nav: no keyboard for the route chooser\n", stderr);
            break;
        }
        if (poll(pfd, (unsigned)np, 500) <= 0)
            continue;
        for (i = 0; i < np; i++) {
            int code, value;
            if (!(pfd[i].revents & POLLIN))
                continue;
            if (g_tty_raw && pfd[i].fd == STDIN_FILENO) {
                char k;
                while (read(STDIN_FILENO, &k, 1) == 1) {
                    if (k == 'n' || k == 'N')
                        code = KEY_N;
                    else if (k == 'p' || k == 'P')
                        code = KEY_P;
                    else if (k == '\n' || k == '\r')
                        code = KEY_ENTER;
                    else if (k == 'q' || k == 'Q')
                        code = KEY_Q;
                    else
                        continue;
                    if (chooser_key(&ch, code, out, outsz, &rc))
                        done = 1;
                    dirty = 1;
                }
                continue;
            }
            while (evdev_next_key(pfd[i].fd, &code, &value)) {
                if (value != 1)
                    continue;
                if (chooser_key(&ch, code, out, outsz, &rc))
                    done = 1;
                dirty = 1;
            }
        }
    }
    free(cv->bits);
    free(cv);
    return rc;
}
#endif /* NAV_DEVICE */

/* ------------------------------------------------------------------ loop */

static int
run_live(app_t *a, const char *devpath, const char *replaypath, int headless,
         int pace_opt, int do_print)
{
    canvas_t *cv, *prev;
    FILE *replay = NULL;
    char line[256];
    size_t ll = 0;
#ifdef NAV_DEVICE
    fb_t fb;
    port_t port;
    int have_fb = 0, have_port = 0;
#endif

    cv = canvas_new(SCR_W, SCR_H);
    prev = canvas_new(SCR_W, SCR_H);
    if (!cv || !prev) {
        fputs("beepy-nav: out of memory\n", stderr);
        return 1;
    }
    /* Neither all-ink nor all-paper, so the first real frame always differs
     * and is always presented. */
    memset(prev->bits, 0xAA, (size_t)prev->stride * prev->h);
    RC.cv = cv;
    RC.prev = prev;

    if (replaypath) {
        replay = fopen(replaypath, "rb");
        if (!replay) {
            perror(replaypath);
            return 1;
        }
        /* Paced against the clock the sentences carry, so the panel moves at
         * the speed the ride did. Off by default under --headless: the
         * replay assertions would otherwise take as long as the ride. */
        RC.paced = pace_opt >= 0 ? pace_opt : !headless;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
#ifdef NAV_DEVICE
    if (g_preopened) {
        /* Inherited from the route chooser, panel and grab and all. */
        fb = *g_preopened;
        have_fb = 1;
        tty_raw();
    } else if (!headless) {
        if (fb_open(&fb, "/dev/fb1") < 0)
            return 1;
        g_fb = &fb;
        have_fb = 1;
        atexit(cleanup);
        tty_raw();
        evdev_open(1);
        fb_take(&fb);
    }
    RC.fb = &fb;
    RC.have_fb = have_fb;
    if (!replaypath) {
        memset(&port, 0, sizeof port);
        snprintf(port.path, sizeof port.path, "%s", devpath);
        port.baud = 9600;
        port.fd = -1;
        if (port_open(&port) < 0) {
            fprintf(stderr, "%s: %s\n", devpath, strerror(errno));
            return 1;
        }
        have_port = 1;
        if (a->rate_5hz) {
            int ack = ubx_set_rate(port.fd, UBX_5HZ_MS);
            /* Said out loud either way. A silent "5 Hz requested" that the
             * receiver ignored is worse than 1 Hz, because the smoothness
             * budget would then have been spent on an assumption. */
            fprintf(stderr, "beepy-nav: UBX-CFG-RATE %d ms: %s\n", UBX_5HZ_MS,
                    ack == 1   ? "ACK"
                    : ack == 0 ? "NAK -- the receiver refused it"
                               : "no answer in 2 s -- assume 1 Hz");
        }
    }
#else
    (void)devpath;
    if (!replaypath) {
        fputs("beepy-nav: no serial port in the host lane; use --replay\n",
              stderr);
        return 2;
    }
    if (!headless) {
        fputs("beepy-nav: no panel in the host lane; use --headless\n",
              stderr);
        return 2;
    }
#endif

    while (!a->quit && !g_signal_quit) {
        int got_epoch, got_fix;

        /* --- one line of NMEA ---------------------------------------- */
        if (replay) {
            int c = fgetc(replay);
            if (c == EOF)
                break; /* end of the ride: exit cleanly */
            if (c != '\n' && c != '\r') {
                if (ll + 1 < sizeof line)
                    line[ll++] = (char)c;
                continue;
            }
            if (!ll)
                continue;
        } else {
#ifdef NAV_DEVICE
            struct pollfd pfd[1 + MAX_EVDEV];
            int np = 0, i;
            pfd[np].fd = port.fd;
            pfd[np].events = POLLIN;
            np++;
            if (g_tty_raw) {
                pfd[np].fd = STDIN_FILENO;
                pfd[np].events = POLLIN;
                np++;
            }
            for (i = 0; i < evdev_count(); i++) {
                pfd[np].fd = evdev_fd(i);
                pfd[np].events = POLLIN;
                np++;
            }
            /* Wake in time for the next frame rather than in time for the
             * next sentence: at 8 Hz the display owes seven frames between
             * one GGA and the next, and a 1000 ms timeout would sleep
             * straight through all of them. */
            if (poll(pfd, (unsigned)np, live_poll_ms(a)) <= 0) {
                live_frames(a);
                continue;
            }
            for (i = 1; i < np; i++) {
                int code, value;
                char k;
                if (!(pfd[i].revents & POLLIN))
                    continue;
                if (g_tty_raw && pfd[i].fd == STDIN_FILENO) {
                    while (read(STDIN_FILENO, &k, 1) == 1)
                        handle_key(a, k);
                    continue;
                }
                while (evdev_next_key(pfd[i].fd, &code, &value))
                    if (value == 1)
                        handle_key(a, keycode_to_char(code));
            }
            live_frames(a);
            if (!(pfd[0].revents & POLLIN))
                continue;
            for (;;) {
                char b;
                if (read(port.fd, &b, 1) != 1)
                    break;
                if (b == '\n' || b == '\r') {
                    if (ll)
                        break;
                    continue;
                }
                if (ll + 1 < sizeof line)
                    line[ll++] = b;
            }
            if (!ll)
                continue;
#else
            break;
#endif
        }
        line[ll] = '\0';

        /* GGA closes a u-blox epoch: the receiver emits RMC, VTG, GGA in
         * that order (DESIGN.md 3), so by the time GGA lands, the position,
         * the course and the speed are all from this second. Triggering on
         * RMC instead would pair this second's position with last second's
         * VTG, and the map would rotate one fix late. */
        got_epoch = ll > 6 && !memcmp(line + 3, "GGA", 3);
        nmea_line(&a->gps, line, ll);
        ll = 0;
        if (!got_epoch)
            continue;

#ifdef NAV_DEVICE
        drain_keys(a);
#endif

        /* The frames the display owes for the interval that just elapsed,
         * drawn from the OLD fix -- they belong to it, and drawing them after
         * on_epoch() has moved the extrapolation base would teleport the map
         * a second forward and then walk it back. */
        if (replay)
            frames_before(a, epoch_seconds(a));

        /* The panel is redrawn either way -- the clock ticks and NO FIX has
         * to be able to appear -- but the per-fix trace is one row per FIX, as
         * its header promises, so a dropout leaves a gap in t rather than a
         * run of duplicated rows. */
        got_fix = on_epoch(a, time(NULL));
        if (got_fix)
            a->fix_mono = mono_now();
        if (do_print && got_fix)
            printf("t=%.0f %.6f,%.6f seg=%d along=%.0f off=%.1f%s cue=%d "
                   "in %.0fm togo=%.0f %.1f%% eta=%.0fs hdg=%.0f\n",
                   a->t, a->fx.lat, a->fx.lon, a->nv.seg, a->nv.along,
                   a->nv.off_m, a->nv.off ? " OFF" : "", a->nv.cue_i,
                   a->nv.cue_m, a->nv.togo_m, a->nv.pct, a->nv.eta_s,
                   a->heading * (180.0 / M_PI));

        if (a->hold) {
            pace_to(a->t);
            led_tick(a->t);
            if (got_fix)
                trace_row(a, live_zoom(a), 0);
        } else {
            int presented = frame_at(a, a->t);
            if (got_fix)
                trace_row(a, live_zoom(a), presented);
        }
    }
    stats_flush(a, 1);

    led_off();
    if (g_trace)
        fclose(g_trace);
    if (g_ftrace)
        fclose(g_ftrace);
    if (replay)
        fclose(replay);
#ifdef NAV_DEVICE
    if (have_port)
        port_close(&port);
    if (have_fb)
        fb_release(&fb);
    g_fb = NULL;
#endif
    fprintf(stderr, "beepy-nav: %ld fixes, %.1f%% done, %d full scans\n",
            a->epochs, a->nv.pct, a->ctx.full_scans);
    return 0;
}

/* ------------------------------------------------------------------ main */

int
main(int argc, char **argv)
{
    const char *dumppath = NULL, *routepath = NULL, *replaypath = NULL;
    const char *tracepath = NULL, *devpath = "/dev/ttyACM0";
    const char *ftracepath = NULL;
    int page = PAGE_NAV, bench = 0, demo = 0, headless = 0, do_print = 0;
    int pace_opt = -1, i;
    double fps = DR_FPS;
    char err[320];
    char cfgpath[CFG_PATH_MAX], routes[CFG_PATH_MAX];
    navcfg_t cfg;
    canvas_t *cv;
#ifdef NAV_DEVICE
    char chosen[512];
    fb_t chooser_fb;
#endif

    /* The config file first, so the flags below can override it. --config
     * itself has to be found before the file is read, which is what this
     * pre-pass is for; it is cheaper than deferring every other flag. */
    cfg_defaults(&cfg);
    cfgpath[0] = '\0';
    for (i = 1; i < argc - 1; i++)
        if (!strcmp(argv[i], "--config"))
            snprintf(cfgpath, sizeof cfgpath, "%s", argv[++i]);
    if (cfgpath[0]) {
        cfg_load(&cfg, cfgpath, 1);
    } else {
        cfg_default_path(cfgpath, sizeof cfgpath);
        cfg_load(&cfg, cfgpath, 0);
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--demo"))
            demo = 1;
        else if (!strcmp(a, "--page") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "nav"))
                page = PAGE_NAV;
            else if (!strcmp(p, "nav-off"))
                page = PAGE_NAV_OFF;
            else if (!strcmp(p, "overview"))
                page = PAGE_OVERVIEW;
            else if (!strcmp(p, "arrows"))
                page = PAGE_ARROWS;
            else if (!strcmp(p, "cliptest"))
                page = PAGE_CLIPTEST;
            else if (!strcmp(p, "cliptest-panel"))
                page = PAGE_CLIPTEST_PANEL;
            else if (!strcmp(p, "chooser"))
                page = PAGE_CHOOSER;
            else {
                fprintf(stderr, "unknown page: %s\n%s", p, USAGE);
                return 2;
            }
        } else if (!strcmp(a, "--dump") && i + 1 < argc)
            dumppath = argv[++i];
        else if (!strcmp(a, "--bench") && i + 1 < argc)
            bench = atoi(argv[++i]);
        else if (!strcmp(a, "--route") && i + 1 < argc)
            routepath = argv[++i];
        else if (!strcmp(a, "--replay") && i + 1 < argc)
            replaypath = argv[++i];
        else if (!strcmp(a, "--trace") && i + 1 < argc)
            tracepath = argv[++i];
        else if (!strcmp(a, "--trace-frames") && i + 1 < argc)
            ftracepath = argv[++i];
        else if (!strcmp(a, "--stats"))
            RC.stats = 1;
        else if (!strcmp(a, "--fps") && i + 1 < argc) {
            fps = atof(argv[++i]);
            if (!(fps > 0.0) || fps > 60.0) {
                fprintf(stderr, "bad --fps: %s\n%s", argv[i], USAGE);
                return 2;
            }
        }
        else if (!strcmp(a, "--dump-at") && i + 1 < argc) {
            const char *v = argv[++i], *colon = strchr(v, ':');
            if (!colon || g_ndumps >= MAX_DUMPS) {
                fprintf(stderr, "bad --dump-at: %s\n%s", v, USAGE);
                return 2;
            }
            g_dumps[g_ndumps].at = atof(v);
            g_dumps[g_ndumps].path = colon + 1;
            g_ndumps++;
        }
        else if (!strcmp(a, "-d") && i + 1 < argc)
            devpath = argv[++i];
        else if (!strcmp(a, "--headless"))
            headless = 1;
        else if (!strcmp(a, "--print"))
            do_print = 1;
        else if (!strcmp(a, "--pace"))
            pace_opt = 1;
        else if (!strcmp(a, "--no-pace"))
            pace_opt = 0;
        else if (!strcmp(a, "--north-up"))
            cfg.north_up = 1;
        else if (!strcmp(a, "--imperial"))
            cfg.units = UNITS_IMPERIAL;
        else if (!strcmp(a, "--metric"))
            cfg.units = UNITS_METRIC;
        else if (!strcmp(a, "--rate-5hz"))
            cfg.rate_5hz = 1;
        else if (!strcmp(a, "--no-rate-5hz"))
            cfg.rate_5hz = 0;
        else if (!strcmp(a, "--config") && i + 1 < argc)
            i++; /* already read, above */
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(USAGE, stdout);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n%s", a, USAGE);
            return 2;
        }
    }

    /* The config file's routes_dir, or the way it has always been decided --
     * $BEEPY_ROUTES, then ~/routes. Resolved here rather than inside
     * chooser_default_dir() so that the environment keeps working for anyone
     * already using it, and so the --demo chooser page lists the same
     * directory the real one would. */
    if (cfg.routes_dir[0])
        snprintf(routes, sizeof routes, "%s", cfg.routes_dir);
    else
        chooser_default_dir(routes, sizeof routes);

    /* ---------------------------------------------------- static pages */
    if (demo) {
        if (!dumppath && bench <= 0) {
            fputs("beepy-nav: nothing to do (--dump or --bench)\n", stderr);
            fputs(USAGE, stderr);
            return 2;
        }
        cv = canvas_new(SCR_W, SCR_H);
        if (!cv) {
            fputs("beepy-nav: out of memory\n", stderr);
            return 1;
        }
        if (bench > 0) {
            struct timespec t0, t1;
            double ms;
            render_demo(&COV, cv, page, routes); /* warm the caches */
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0; i < bench; i++)
                render_demo(&COV, cv, page, routes);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                  (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) /
                 bench;
            printf("bench: %d frames, %.3f ms/frame\n", bench, ms);
        } else {
            render_demo(&COV, cv, page, routes);
        }
        if (dumppath) {
            if (canvas_dump(cv, dumppath) < 0) {
                perror(dumppath);
                return 1;
            }
            fprintf(stderr, "wrote %s\n", dumppath);
        }
        return 0;
    }

    /* ------------------------------------------------------------ live */
    gps_init(&APP.gps);
    fix_init(&APP.fx);
    nav_init(&APP.ctx);
    nav_reset(&APP.nv);
    APP.page = page == PAGE_OVERVIEW ? LIVE_OVERVIEW : LIVE_NAV;
    APP.course_up = !cfg.north_up;
    APP.fps = fps;
    APP.rate_5hz = cfg.rate_5hz;
    APP.alert_cue = -1;
    nav_init(&APP.rctx);
    nav_set_units(&APP.ctx, cfg.units);
    led_init(cfg.led_alerts);

    if (!routepath) {
#ifdef NAV_DEVICE
        /* Never under --headless or --demo: neither has a panel to draw the
         * list on nor a key to answer with, and a program that silently
         * waits for a keypress that cannot arrive is worse than an error. */
        if (headless) {
            fputs("beepy-nav: --headless needs an explicit --route\n", stderr);
            return 2;
        }
        if (fb_open(&chooser_fb, "/dev/fb1") < 0)
            return 1;
        g_fb = &chooser_fb;
        atexit(cleanup);
        tty_raw();
        evdev_open(1);
        fb_take(&chooser_fb);
        if (chooser_run(chosen, sizeof chosen, &chooser_fb, routes) != 0) {
            fb_release(&chooser_fb);
            g_fb = NULL;
            return 0; /* the rider quit the chooser; not an error */
        }
        /* The panel and the grab carry straight into the ride: releasing
         * and re-taking them would flash fbterm between the list and the
         * first frame. */
        g_preopened = &chooser_fb;
        routepath = chosen;
#else
        fputs("beepy-nav: no route (--route FILE.gpx)\n", stderr);
        fputs(USAGE, stderr);
        return 2;
#endif
    }
    if (route_load(routepath, &APP.rt, err, sizeof err)) {
        fprintf(stderr, "beepy-nav: %s\n", err);
        return 1;
    }
    fprintf(stderr, "beepy-nav: %s -- %d points, %.2f km, %d cues\n",
            APP.rt.name, APP.rt.npt, APP.rt.total_m / 1000.0, APP.rt.ncue);

    if (tracepath)
        trace_open(tracepath);
    if (ftracepath)
        ftrace_open(ftracepath);
    return run_live(&APP, devpath, replaypath, headless, pace_opt, do_print);
}

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
#include "ridelog.h"
#include "route.h"
#include "tile.h"
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
    "  --key S:C     press key C at replay second S (up to 8) -- how the\n"
    "                keymap is driven in a headless test\n"
    "  --print       dump nav_t as text, per fix\n"
    "  --north-up    start north-up instead of course-up\n"
    "  --imperial    feet and miles; --metric is the default\n"
    "  --config F    read F instead of ~/.config/beepy-nav.conf; every\n"
    "                setting in it is overridden by the flag for it\n"
    "  --rate-5hz    ask the receiver for 200 ms fixes at startup;\n"
    "                --no-rate-5hz is the default (see DESIGN.md 6.3)\n"
    "  --basemap F   an OSM tile pack under the map (DESIGN.md 6.5), built\n"
    "                by tools/mktiles.py; --no-basemap defeats the config\n"
    "  --no-log      do not record the ride. On a real port the raw NMEA and\n"
    "                the per-fix trace go to ~/rides/YYYYMMDD-HHMMSS.{nmea,tsv}\n"
    "                unless this says otherwise (rides_dir in the config)\n"
    "  --demo        render a static design state instead of navigating\n"
    "  --page P      nav, nav-off, nav-nofix, nav-tiles, overview,\n"
    "                overview-tiles, arrows, chooser, cliptest,\n"
    "                cliptest-panel -- and with\n"
    "                --route it picks the page the ride opens on\n"
    "  --dump FILE   write the frame as 384000 raw XRGB bytes\n"
    "  --bench N     time N draw+resolve cycles and print ms/frame\n"
    "\n"
    "keys: Tab page   R change route   Q quit   H hold\n"
    "      O course-up/north-up   Z/X zoom out/in   A auto zoom\n"
    "      U metric/imperial   L cue alerts on/off\n";

enum {
    PAGE_NAV,
    PAGE_NAV_OFF,
    PAGE_NAV_NOFIX,
    PAGE_NAV_TILES,
    PAGE_OVERVIEW,
    PAGE_OVERVIEW_TILES,
    PAGE_ARROWS,
    PAGE_CLIPTEST,
    PAGE_CLIPTEST_PANEL,
    PAGE_CHOOSER
};
enum { LIVE_NAV, LIVE_OVERVIEW };

/* run_live()'s "the rider wants a different route", distinct from any exit
 * status: main() loops back to the picker rather than returning it. Outside
 * the NAV_DEVICE guard because main() tests it in both lanes. */
#define RUN_PICK_AGAIN 100

/* The coverage buffer is 96 KB; keep it out of the stack. */
static cov_t COV;

/* The basemap pack (DESIGN.md 6.5), or NULL -- which is every case in which
 * there is no basemap: none configured, the file missing, the file not a
 * pack. It lives at file scope rather than in app_t because R zeroes app_t
 * to start the next route, and a pack outlives a route: only the frame it is
 * bound to changes, which tiles_bind_route() does after each route_load(). */
static tiles_t *g_tiles;

/* ------------------------------------------------------------------ demo */

static void
render_demo(cov_t *cov, canvas_t *cv, int page, const char *routes,
            tiles_t *tiles)
{
    cov_begin(cov);
    switch (page) {
    case PAGE_NAV_OFF:
        view_nav_demo(cov, 85, 0);
        break;
    case PAGE_NAV_NOFIX:
        view_nav_demo(cov, 0, 1);
        break;
    /* The basemap state. With no --basemap this is the SAME page with the
     * tile layer absent, which is exactly the comparison that says the layer
     * is optional -- see the Makefile's `check`. */
    case PAGE_NAV_TILES:
        view_nav_tiles_demo(cov, tiles);
        break;
    case PAGE_OVERVIEW:
        view_overview_demo(cov, 0);
        break;
    case PAGE_OVERVIEW_TILES:
        view_overview_demo(cov, 1);
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
        view_nav_demo(cov, 0, 0);
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
#define MAX_DUMPS 8
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

/* --- NO FIX, DESIGN.md 1.1 ----------------------------------------------
 *
 * "GPS state earns panel space only when it is a problem -- NO FIX replaces
 * the bottom row, inverted, when the fix is lost."
 *
 * Measured in SECONDS and not in missed epochs, so one rule covers the
 * receiver's measured 1 Hz (3), the optional 5 Hz of 6.3, and a dropout that
 * stops the sentences altogether rather than merely voiding them.
 *
 * FOUR seconds, from that 1 Hz cadence. One dropped epoch leaves a 2 s hole
 * between good fixes and two leave 3 s; neither is news, and a warning that
 * blinks on every dropped sentence is a warning a rider learns to ignore --
 * which would cost exactly the trust the row exists to earn. Four fires on
 * the third consecutive miss. Below three the rule is unsafe at 1 Hz; above
 * five the panel is lying for longer than the dead reckoning it is covering
 * for, which is 2 s (DR_MAX_EXTRAP). Four sits between the two bounds with a
 * second of margin on each side.
 *
 * DR_MAX_EXTRAP is the other half of the same decision and is already the
 * code's: the extrapolation of 6.3 clamps at 2 s, so by the time this fires
 * the drawn position has been frozen for two seconds and cannot have run on
 * down a road the bike may not be on. Dead reckoning is honest for a second
 * or two and no longer. */
#define NOFIX_S 4.0

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
    /* R: leave this route and go back to the picker, without leaving the
     * program. Distinct from quit, because the panel, the evdev grab and the
     * process all survive it -- only the route does not. */
    int pick_again;
    int can_pick; /* a picker exists to go back to */

    int rate_5hz; /* ask the receiver for 5 Hz at startup (DESIGN.md 6.3) */

    /* Cue alerts, as a SESSION state. The config's led_alerts supplies the
     * starting value and L overrides it for this ride only -- nothing here
     * ever writes the config back, because the file is a default and the key
     * is a decision about the next ten minutes. */
    int alerts;

    /* The transient confirmation of that decision, and when it expires, both
     * on the ride clock (so a replay shows it for the same 1.5 s a ride
     * would, and a test can see it). */
    const char *note;
    double note_until;
    double frame_t; /* the ride second being drawn, for the expiry test */

    /* Metres per pixel while the rider has taken the NAV map off auto zoom;
     * 0 is auto. Not in navctx_t: it is a view decision, and nothing in the
     * route maths may depend on what the map happens to be showing. */
    double mpp_manual;
    int have_pos;
    double e, n; /* the fix, in route metres */

    /* NO FIX (1.1). last_fix_t is on the ride clock, which is why that clock
     * has to keep running through a gap -- see clock_advance(). */
    double last_fix_t;
    int nofix;

    long epochs;    /* epochs carrying a usable position: FIXES  */
    long epoch_seq; /* epochs seen at all, fix or not            */
    int have_t0;
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
 * A MUTE is the opposite case and is handled the opposite way: the rungs are
 * still consumed, they simply do not ring. Off route the alerts are wrong and
 * owed later; muted they are right and refused now. Keeping the ladder moving
 * under a mute is what stops L, pressed a hundred metres from a junction, from
 * firing 500 and 200 as a burst for a junction already half taken.
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
    if (!a->alerts)
        return 0; /* consumed above, refused here */
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
        o.osm = g_tiles != NULL;
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
        m.tiles = g_tiles;
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
        p.note = a->note && a->frame_t < a->note_until ? a->note : NULL;
        /* DESIGN.md 1.1. Everything above is already frozen by construction
         * -- the countdown and the metres-off are the fix's (1.1.1), and the
         * marker's extrapolation clamps at DR_MAX_EXTRAP -- so this row is
         * the only thing that has to change, and its whole job is to say that
         * the rest has stopped. */
        p.nofix = a->nofix;
        view_nav(cov, &m, &p);
    }
    cov_resolve(cov, cv);
}

/* ----------------------------------------------------------------- trace */

static FILE *g_trace;

/* The ride log (DESIGN.md 7.6) carries the SAME columns, because the point of
 * it is that a log lifted off the device is a --trace by another name: the
 * assertions of section 10 read it unchanged, and tools/ride2fixture.sh can
 * check the replay against what the device actually computed. Two formats
 * would be two things to keep in step. */
static void
trace_header(FILE *f)
{
    if (!f)
        return;
    fputs("#t\tlat\tlon\tseg\talong_m\toff_m\toff_latched\tcue_i\tcue_m\t"
          "togo_m\tpct\teta_s\theading_deg\tzoom_mpp\tpresented\t"
          "course_deg\tresidual_deg\n",
          f);
}

static void
trace_write(FILE *f, const app_t *a, double zoom_mpp, int presented)
{
    if (!f)
        return;
    fprintf(f,
            "%.3f\t%.7f\t%.7f\t%d\t%.2f\t%.2f\t%d\t%d\t%.2f\t%.2f\t%.3f\t"
            "%.1f\t%.2f\t%.2f\t%d\t%.2f\t%.2f\n",
            a->t, a->fx.lat, a->fx.lon, a->nv.seg, a->nv.along, a->nv.off_m,
            a->nv.off, a->nv.cue_i, a->nv.cue_m, a->nv.togo_m, a->nv.pct,
            a->nv.eta_s, a->heading * (180.0 / M_PI), zoom_mpp, presented,
            a->raw_course * (180.0 / M_PI),
            wrap_pi(a->raw_course - (a->course_up ? a->heading : 0.0)) *
                (180.0 / M_PI));
}

static ridelog_t RIDELOG;

static void
trace_open(const char *path)
{
    g_trace = fopen(path, "w");
    if (!g_trace) {
        perror(path);
        exit(1);
    }
    trace_header(g_trace);
}

static void
trace_row(const app_t *a, double zoom_mpp, int presented)
{
    trace_write(g_trace, a, zoom_mpp, presented);
    trace_write(RIDELOG.tsv, a, zoom_mpp, presented);
}

/* One row per rendered FRAME rather than per fix -- the DESIGN.md 6.3 maths
 * happens eight times a second and a per-fix trace cannot see any of it. Every
 * quantity the extrapolation is built from is here, so tools/assert_trace.py
 * can recompute the closed form and check the drawn position against it.
 *
 * fix_err -- the distance between where the last frame said we were and where
 * the new fix says we are -- is the one value that cannot be recovered from
 * the others, because the prediction it was measured against belonged to the
 * PREVIOUS extrapolation base, which this row has already replaced. It is
 * also the whole of 6.3's decision: at or under DR_SNAP_M the correction is
 * eased over three frames, over it the fix is taken whole. Without the column
 * a trace cannot tell an eased correction from one that never happened. */
static FILE *g_ftrace;

static void
ftrace_open(const char *path)
{
    g_ftrace = fopen(path, "w");
    if (!g_ftrace) {
        perror(path);
        exit(1);
    }
    /* cue_q and nofix are the DESIGN.md 1.1 pair: the number the PANEL prints
     * (quantised and latched, not the raw metres of cue_m) and whether the
     * bottom row has been taken over. A per-fix trace can see neither -- the
     * gap they describe is precisely the stretch with no fix rows in it. */
    fputs("#t\tisfix\tsince_fix\tbase_e\tbase_n\tbase_crs\tbase_spd\tdt\t"
          "err_e\terr_n\tfix_err\tease_w\tdr_e\tdr_n\tcourse_deg\t"
          "heading_deg\tpresented\tms\tled\toff_latched\tcue_i\tcue_m\t"
          "cue_q\tnofix\n",
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
            "%.4f\t%d\t%ld\t%.4f\t%.4f\t%.6f\t%.4f\t%.4f\t%.4f\t%.4f\t"
            "%.4f\t%.4f\t%.4f\t%.4f\t%.3f\t%.3f\t%d\t%.3f\t%d\t%d\t"
            "%d\t%.2f\t%d\t%d\n",
            t, isfix, a->frames_since_fix, a->fix_e, a->fix_n,
            a->fix_course * (180.0 / M_PI), a->fix_speed, dt, a->err_e,
            a->err_n, a->fix_err, a->ease_w, a->dr_e, a->dr_n,
            a->raw_course * (180.0 / M_PI), a->heading * (180.0 / M_PI),
            presented, ms, isfix ? a->alert_fired : 0, a->nv.off,
            a->rnv.cue_i, a->rnv.cue_m, a->nv.cue_q, a->nofix);
}

/* --------------------------------------------------------- the fix cycle */

/* The ride-clock second this epoch belongs to, WITHOUT consuming it: the
 * frames covering the gap since the last fix have to be drawn from the old
 * extrapolation base before that base is replaced. */
static double
epoch_seconds(const app_t *a)
{
    double secs = fix_utc_seconds(a->gps.utc), t;
    if (secs < 0.0 || !a->have_t0)
        return (double)a->epoch_seq;
    t = secs - a->t0;
    if (t < -43200.0) /* midnight, once a ride */
        t += 86400.0;
    return t;
}

/* The ride clock, advanced once per EPOCH -- with a position or without one.
 *
 * That distinction is the whole of NO FIX (1.1). The warning is a duration and
 * a clock that stops the moment the fix does can never measure it: before this
 * existed, `t` was frozen for the length of every gap, so "four seconds
 * without a fix" was four seconds that never elapsed. Two other rules were
 * quietly dead for the same reason -- 7.2's "after 30 s lost, widen to a full
 * scan" could not trigger, because the only clock it reads had stopped, and
 * the frames drawn during a gap were all stamped with the last fix's second.
 *
 * A receiver with no almanac yet emits GGA with an EMPTY time field, which is
 * exactly the indoor cold-start case, so the fallback counts epochs instead.
 * At the measured 1 Hz that is the same clock to within the drift of a device
 * that is not moving. */
static void
clock_advance(app_t *a)
{
    double secs = fix_utc_seconds(a->gps.utc);
    if (secs >= 0.0) {
        if (!a->have_t0) {
            /* The first epoch carrying a time sets the origin, and sets it
             * so the clock does not jump: `a->t` may already have been
             * counting epochs through a cold start, and stitching the two
             * here is cheaper than a second clock. On a warm receiver this
             * is the first epoch and the subtraction is of zero. */
            a->t0 = secs - a->t;
            a->have_t0 = 1;
        }
        a->t = secs - a->t0;
        if (a->t < -43200.0) /* midnight, once a ride */
            a->t += 86400.0;
    } else {
        a->t = (double)a->epoch_seq;
    }
    a->epoch_seq++;
}

/* One completed NMEA epoch: everything the two pages consume, recomputed in
 * DESIGN.md 7's order -- snap, latch, cue, progress. Returns 1 when there was
 * a position to work from. */
static int
on_epoch(app_t *a, time_t now)
{
    int got = fix_from_gps(&a->gps, now, &a->fx);

    /* The clock first, and unconditionally: an epoch that carries no position
     * still carries the second it happened in, and NO FIX is measured in
     * those seconds. */
    clock_advance(a);
    if (!got)
        return 0; /* no position this epoch; nothing else to recompute */

    a->epochs++;
    /* DESIGN.md 1.1, recovery. The countdown latch is monotone only "while
     * approaching a given cue", and the gap just ended broke that: the world
     * moved unobserved and the frozen value is not a floor the re-snapped
     * distance may only decrease from. Re-armed BEFORE route_progress(), so
     * this fix's countdown is the first value of the new sequence rather than
     * the last of the old one. */
    if (a->nofix) {
        nav_relatch(&a->ctx);
        a->nofix = 0;
    }
    a->last_fix_t = a->t;

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
    a->frame_t = t;
    /* DESIGN.md 1.1, evaluated on the FRAME clock and not on the fix clock:
     * a fix is the one event that cannot happen during the gap this measures,
     * so a rule that only ran on fixes could never fire. Cleared in
     * on_epoch(), which is where recovery is decided. */
    a->nofix = t - a->last_fix_t >= NOFIX_S;
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
        /* These bytes never reach the parser, so without this the log would
         * have a hole in it exactly where the receiver was being configured
         * -- which is the one part of a startup a bug report would want. */
        ridelog_raw(&RIDELOG, (const char *)&b, 1);
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
    /* A transient owes the bottom row back on time. At the 1 Hz stopped rate
     * the next scheduled frame can be most of a second after it expires, and
     * "ALERTS OFF" lingering for two and a half seconds reads as a stuck
     * panel rather than a confirmation. */
    if (a->note) {
        double until = a->note_until - live_ride_now(a);
        if (until > 0.0 && until < left)
            left = until;
    }
    if (left <= 0.001)
        return 1;
    if (left > 1.0)
        return 1000;
    return (int)(left * 1000.0);
}
#endif

/* ------------------------------------------------------------------ keys
 *
 * The SOURCES are device-only -- DESIGN.md 2 has them coming from
 * /dev/input/event0 with EVIOCGRAB, because fbterm is SIGSTOPped for as long
 * as the panel is owned -- but what a key DOES is portable, and it is kept
 * that way deliberately. --key schedules presses against the ride clock, so
 * the whole keymap runs in a headless replay on the Mac and every one of its
 * effects is assertable there. A keymap that only a physical thumb can reach
 * is a keymap with no tests.
 *
 * A key changes what the panel says, and the answer has to arrive with the
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
#ifdef NAV_DEVICE
    if (RC.have_fb)
        fb_present(RC.fb, RC.cv);
#endif
}

/* 1.5 s: long enough to read four syllables at handlebar distance, short
 * enough that the row it borrowed is back before the rider next wants it. */
#define NOTE_S 1.5

static void
note_show(app_t *a, const char *s)
{
    a->note = s;
    a->note_until = a->frame_t + NOTE_S;
}

static int
handle_key(app_t *a, int ch)
{
    switch (ch) {
    case '\t':
        a->page = a->page == LIVE_NAV ? LIVE_OVERVIEW : LIVE_NAV;
        break;
    case 'r':
    case 'R':
        /* Only where there is a picker to go back to: a --route on the command
         * line or a replay has no list behind it, and a key that silently does
         * nothing is worse than one that is not bound. */
        if (a->can_pick) {
            a->pick_again = 1;
            a->quit = 1;
        }
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
    /* DESIGN.md 7.5. The confirmation is not decoration: muting is the one
     * setting on this device whose effect is INVISIBLE in the direction that
     * matters. A silenced LED looks exactly like a route with no junction
     * nearby, so without a word on the screen a rider cannot tell "I turned
     * the alerts off" from "the alerts are broken" -- and would find out
     * which at the next missed turn. */
    case 'l':
    case 'L':
        a->alerts = !a->alerts;
        led_enable(a->alerts);
        note_show(a, a->alerts ? "ALERTS ON" : "ALERTS OFF");
        break;
    default:
        return 0;
    }
    key_repaint(a);
    return 1;
}

/* Presses scheduled against the ride clock (--key SEC:CHAR). The mechanism a
 * replay test uses to press a key, and the reason handle_key() above is not
 * behind NAV_DEVICE. */
#define MAX_KEYS 8
typedef struct {
    double at;
    int ch;
    int done;
} keyat_t;
static keyat_t g_keys[MAX_KEYS];
static int g_nkeys;

static void
keys_due(app_t *a, double t)
{
    int i;
    for (i = 0; i < g_nkeys; i++)
        if (!g_keys[i].done && t >= g_keys[i].at) {
            g_keys[i].done = 1;
            /* So a transient started by a scheduled key expires 1.5 s after
             * the press and not 1.5 s after whatever frame happened last. */
            a->frame_t = t;
            handle_key(a, g_keys[i].ch);
        }
}

#ifdef NAV_DEVICE

static int
keycode_to_char(int code)
{
    switch (code) {
    case KEY_TAB:
        return '\t';
    case KEY_R:
        return 'r';
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
    case KEY_L:
        return 'l';
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
    tiles_close(g_tiles);
    g_tiles = NULL;
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
/* The panel has exactly ONE owner, at file scope, because it outlives every
 * function that draws on it: the picker hands it to the ride, R hands it back,
 * and cleanup() may release it from a signal at any moment in between. An
 * earlier version kept it in run_live()'s frame and pointed g_fb at that --
 * which dangles the instant the ride returns to the picker, so a Ctrl-C landing
 * in the list would have released a dead stack slot. */
static fb_t g_panel;
static int g_panel_open;
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
         int pace_opt, int do_print, const char *ridesdir)
{
    canvas_t *cv, *prev;
    FILE *replay = NULL;
    char line[256];
    size_t ll = 0;
#ifdef NAV_DEVICE
    port_t port;
    int have_port = 0;
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
    if (g_panel_open) {
        /* Inherited from the route picker, panel and grab and all. */
        tty_raw();
    } else if (!headless) {
        if (fb_open(&g_panel, "/dev/fb1") < 0)
            return 1;
        g_panel_open = 1;
        g_fb = &g_panel;
        atexit(cleanup);
        tty_raw();
        evdev_open(1);
        fb_take(&g_panel);
    }
    RC.fb = &g_panel;
    RC.have_fb = g_panel_open;
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
        /* DESIGN.md 7.6. On a real port and following a real route, always --
         * this is the only lane where a failure has no other record. A replay
         * is skipped because it already HAS a log, and copying it into
         * ~/rides on every test run would be noise. Failure to open one is a
         * warning and nothing more: ridelog.c holds the line that logging may
         * never take the navigator down. */
        if (ridesdir)
            ridelog_open(&RIDELOG, ridesdir, time(NULL));
        trace_header(RIDELOG.tsv);
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
    /* There is no serial port in the host lane, so there is nothing to record
     * verbatim -- a replay already IS the log this module would have made. */
    (void)ridesdir;
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
                /* DESIGN.md 7.6: verbatim, and BEFORE the parser. Line
                 * endings, bad checksums, half a sentence cut off by a USB
                 * reset -- all of it goes down exactly as it arrived, because
                 * the malformed input is the input worth having. */
                ridelog_raw(&RIDELOG, &b, 1);
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

        keys_due(a, a->t);

        /* Once per epoch: flush what the kernel can keep across a SIGKILL,
         * and fsync on DESIGN.md 7.6's 30 s budget. After on_epoch(), so the
         * ride clock the fsync cadence reads has already advanced -- and
         * before the frame, so a crash during a render still leaves the
         * sentences that caused it on disk. */
        ridelog_tick(&RIDELOG, a->t);

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
    ridelog_close(&RIDELOG);
    if (g_trace)
        fclose(g_trace);
    if (g_ftrace)
        fclose(g_ftrace);
    if (replay)
        fclose(replay);
#ifdef NAV_DEVICE
    if (have_port)
        port_close(&port);
    /* On R the panel and the grab stay exactly where they are -- one owner,
     * handed back to the picker untouched. Releasing and re-taking would flash
     * fbterm between the two screens, and re-taking can fail. */
    if (!a->pick_again && g_panel_open) {
        fb_release(&g_panel);
        g_panel_open = 0;
        g_fb = NULL;
    }
#endif
    fprintf(stderr, "beepy-nav: %ld fixes, %.1f%% done, %d full scans\n",
            a->epochs, a->nv.pct, a->ctx.full_scans);
    return a->pick_again ? RUN_PICK_AGAIN : 0;
}

/* ------------------------------------------------------------------ main */

int
main(int argc, char **argv)
{
    const char *dumppath = NULL, *routepath = NULL, *replaypath = NULL;
    const char *tracepath = NULL, *devpath = "/dev/ttyACM0";
    const char *ftracepath = NULL;
    int page = PAGE_NAV, bench = 0, demo = 0, headless = 0, do_print = 0;
    int pace_opt = -1, i, no_log = 0;
    double fps = DR_FPS;
    char err[320];
    char cfgpath[CFG_PATH_MAX], routes[CFG_PATH_MAX];
    char rides[RIDELOG_PATH_MAX];
    navcfg_t cfg;
    canvas_t *cv;
#ifdef NAV_DEVICE
    char chosen[512];
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
            else if (!strcmp(p, "nav-nofix"))
                page = PAGE_NAV_NOFIX;
            else if (!strcmp(p, "nav-tiles"))
                page = PAGE_NAV_TILES;
            else if (!strcmp(p, "overview"))
                page = PAGE_OVERVIEW;
            else if (!strcmp(p, "overview-tiles"))
                page = PAGE_OVERVIEW_TILES;
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
        else if (!strcmp(a, "--key") && i + 1 < argc) {
            const char *v = argv[++i], *colon = strchr(v, ':');
            if (!colon || !colon[1] || g_nkeys >= MAX_KEYS) {
                fprintf(stderr, "bad --key: %s\n%s", v, USAGE);
                return 2;
            }
            g_keys[g_nkeys].at = atof(v);
            g_keys[g_nkeys].ch = (unsigned char)colon[1];
            g_nkeys++;
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
        else if (!strcmp(a, "--no-log"))
            no_log = 1;
        else if (!strcmp(a, "--basemap") && i + 1 < argc)
            snprintf(cfg.basemap, sizeof cfg.basemap, "%s", argv[++i]);
        else if (!strcmp(a, "--no-basemap"))
            cfg.basemap[0] = '\0';
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
    /* Same shape for the ride log (DESIGN.md 7.6): the config's rides_dir, or
     * ~/rides. Resolved here so --no-log is a single NULL further down rather
     * than a flag threaded through run_live(). */
    if (cfg.rides_dir[0])
        snprintf(rides, sizeof rides, "%s", cfg.rides_dir);
    else
        ridelog_default_dir(rides, sizeof rides);

    /* The basemap (DESIGN.md 6.5). ONE line on stderr if it cannot be had,
     * and then the map draws exactly as it did before packs existed -- a
     * navigator must not refuse to navigate because a decoration is missing,
     * and the rider finding out at the roadside that the streets are gone is
     * better served by a line they can read afterwards than by an exit. */
    if (cfg.basemap[0]) {
        char why[96];
        g_tiles = tiles_open(cfg.basemap, why, (int)sizeof why);
        if (!g_tiles)
            fprintf(stderr, "beepy-nav: %s: %s; no basemap\n", cfg.basemap,
                    why);
        else
            fprintf(stderr,
                    "beepy-nav: basemap %s -- %d tiles, %d zooms, "
                    "reference %.5f,%.5f\n",
                    cfg.basemap, tiles_count(g_tiles), tiles_nzoom(g_tiles),
                    tiles_ref_lat(g_tiles), tiles_ref_lon(g_tiles));
    }

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
            render_demo(&COV, cv, page, routes, g_tiles); /* warm the caches */
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0; i < bench; i++)
                render_demo(&COV, cv, page, routes, g_tiles);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                  (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) /
                 bench;
            printf("bench: %d frames, %.3f ms/frame\n", bench, ms);
        } else {
            render_demo(&COV, cv, page, routes, g_tiles);
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
    /* The config sets where the ride starts; L moves it from there, and
     * nothing writes the file back. */
    APP.alerts = cfg.led_alerts;
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
        if (fb_open(&g_panel, "/dev/fb1") < 0)
            return 1;
        g_panel_open = 1;
        g_fb = &g_panel;
        atexit(cleanup);
        tty_raw();
        evdev_open(1);
        fb_take(&g_panel);
        if (chooser_run(chosen, sizeof chosen, &g_panel, routes) != 0) {
            fb_release(&g_panel);
            g_panel_open = 0;
            g_fb = NULL;
            return 0; /* the rider quit the picker; not an error */
        }
        /* The panel and the grab carry straight into the ride: releasing
         * and re-taking them would flash fbterm between the list and the
         * first frame. */
        routepath = chosen;
        APP.can_pick = 1; /* R has somewhere to go back to */
#else
        fputs("beepy-nav: no route (--route FILE.gpx)\n", stderr);
        fputs(USAGE, stderr);
        return 2;
#endif
    }
    /* Ride, and come back here if the rider pressed R. The picker keeps the
     * panel and the grab across the hand-off in both directions, so changing
     * route never drops to fbterm and never re-opens the framebuffer. */
    for (;;) {
        int rc;

        if (route_load(routepath, &APP.rt, err, sizeof err)) {
            fprintf(stderr, "beepy-nav: %s\n", err);
            return 1;
        }
        fprintf(stderr, "beepy-nav: %s -- %d points, %.2f km, %d cues\n",
                APP.rt.name, APP.rt.npt, APP.rt.total_m / 1000.0, APP.rt.ncue);
        /* The route decides the world frame (its first point), so the pack
         * has to be told which frame it is being asked about -- again after
         * every R, because the next route has a different origin. */
        tiles_bind_route(g_tiles, APP.rt.lat0, APP.rt.lon0);

        if (tracepath)
            trace_open(tracepath);
        if (ftracepath)
            ftrace_open(ftracepath);

        rc = run_live(&APP, devpath, replaypath, headless, pace_opt, do_print,
                      no_log ? NULL : rides);
        if (rc != RUN_PICK_AGAIN)
            return rc;

#ifdef NAV_DEVICE
        /* A different route is a different ride: the old one's state cannot
         * carry over. route_free() and a zeroed app_t are the whole reset --
         * the snap window, the off-route run, the speed ring, the countdown
         * latch and the ride clock all live in there. The ride log closed
         * with the ride, and the next one opens its own. */
        route_free(&APP.rt);
        memset(&APP, 0, sizeof APP);
        APP.can_pick = 1;
        if (chooser_run(chosen, sizeof chosen, &g_panel, routes) != 0) {
            fb_release(&g_panel);
            g_panel_open = 0;
            g_fb = NULL;
            return 0; /* quit from the picker: the rider is done */
        }
        routepath = chosen;
#else
        return 0;
#endif
    }
}

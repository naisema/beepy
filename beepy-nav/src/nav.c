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
#include "fix.h"
#include "map.h"
#include "route.h"
#include "view.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

/* route.h numbers the cue kinds in arrows.h's order so a cue can be passed
 * straight to arrow_draw(). This is the only file that includes both, so
 * this is where the claim is checked. */
_Static_assert(CUE_STRAIGHT == ARROW_STRAIGHT && CUE_LEFT == ARROW_LEFT &&
                   CUE_RIGHT == ARROW_RIGHT && CUE_UTURN == ARROW_UTURN &&
                   CUE_DEST == ARROW_DEST && CUE_N == ARROW_N,
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
    "  --dump-at S:F write the frame at replay second S to file F (up to 4;\n"
    "                this is how a moving page gets into fbshow --verify)\n"
    "  --print       dump nav_t as text, per fix\n"
    "  --north-up    start north-up instead of course-up\n"
    "  --demo        render a static design state instead of navigating\n"
    "  --page P      nav, nav-off, overview, arrows, chooser, cliptest,\n"
    "                cliptest-panel -- and with\n"
    "                --route it picks the page the ride opens on\n"
    "  --dump FILE   write the frame as 384000 raw XRGB bytes\n"
    "  --bench N     time N draw+resolve cycles and print ms/frame\n"
    "\n"
    "keys: Tab page   Q quit   H hold   O course-up/north-up\n";

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
render_demo(cov_t *cov, canvas_t *cv, int page)
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
        char dir[192];
        chooser_default_dir(dir, sizeof dir);
        chooser_scan(&ch, dir);
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

typedef struct {
    route_t rt;
    navctx_t ctx;
    nav_t nv;
    fix_t fx;
    gps_t gps;

    double heading;    /* smoothed map rotation, radians clockwise from N */
    double raw_course; /* what the chevron gets                           */
    int have_heading;

    int page, hold, quit, course_up;
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

/* DESIGN.md 6.1: circular EWMA, alpha 0.25, FROZEN below 3 km/h. Without the
 * freeze the map spins at every traffic light -- a stationary receiver's
 * reported course is pure noise -- and that is the single most common
 * complaint about course-up displays. */
static void
heading_update(app_t *a, double course_deg, double kmh)
{
    double raw = course_deg * (M_PI / 180.0);
    if (!a->have_heading) {
        a->heading = raw;
        a->raw_course = raw;
        a->have_heading = 1;
        return;
    }
    if (kmh >= 3.0) {
        a->raw_course = raw;
        a->heading += wrap_pi(raw - a->heading) * 0.25;
    } else {
        /* Frozen. The chevron follows the map rather than the noise, so the
         * whole instrument holds still instead of twitching at the lights. */
        a->raw_course = a->heading;
    }
}

/* The vertices within reach of the fix, thinned to fit. Sets win_*. */
static void
build_window(app_t *a)
{
    const route_t *r = &a->rt;
    int seg = a->nv.seg, lo, hi, step, i, m = 0, j;
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
    a->win_pos_f = a1 > a0 ? (a->nv.along - a0) / (a1 - a0) : 0.0;
    if (a->win_pos_f < 0.0)
        a->win_pos_f = 0.0;
    if (a->win_pos_f > 1.0)
        a->win_pos_f = 1.0;

    a->win_turn_i = a->win_pin_i = -1;
    if (a->nv.cue_i >= 0) {
        int v = r->cue[a->nv.cue_i].idx;
        if (v >= lo && v <= hi)
            a->win_turn_i = (v - lo + step / 2) / step;
        if (a->nv.cue_i + 1 < r->ncue) {
            v = r->cue[a->nv.cue_i + 1].idx;
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

static void
fmt_km(char *buf, size_t n, double metres)
{
    snprintf(buf, n, "%.1f", metres / 1000.0);
}

static void
fmt_short(char *buf, size_t n, double metres)
{
    if (metres < 1000.0)
        snprintf(buf, n, "%dM", (int)(metres + 0.5));
    else
        snprintf(buf, n, "%.1fKM", metres / 1000.0);
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
    char clock[8], togo[16], total[16], then_d[16];

    cov_begin(cov);
    if (a->page == LIVE_OVERVIEW) {
        overview_t o;
        int i;
        for (i = 0; i < r->ncue && i < ROUTE_MAXCUE; i++)
            a->cue_idx[i] = r->cue[i].idx;
        fmt_km(togo, sizeof togo, a->nv.togo_m);
        fmt_km(total, sizeof total, r->total_m);
        fmt_clock(clock, sizeof clock, a->nv.eta);
        o.pts = r->en;
        o.npts = r->npt;
        o.cue_idx = a->cue_idx;
        o.ncue_dots = i;
        o.pos_i = a->nv.seg < 0 ? 0 : a->nv.seg;
        o.pos_f = 0.0;
        if (a->nv.seg >= 0 && r->cum[a->nv.seg + 1] > r->cum[a->nv.seg])
            o.pos_f = (a->nv.along - r->cum[a->nv.seg]) /
                      (r->cum[a->nv.seg + 1] - r->cum[a->nv.seg]);
        o.name = r->name;
        o.total = total;
        o.togo = togo;
        o.eta = clock;
        o.done = (int)(a->nv.pct + 0.5);
        o.cue_i = a->nv.cue_i < 0 ? 0 : a->nv.cue_i;
        o.ncues = r->ncue;
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
        m.off = a->nv.off ? a->nv.off_m : 0.0;
        m.spd_kmh = (int)(a->fx.speed_kmh + 0.5);
        m.course_up = a->course_up;

        fmt_clock(clock, sizeof clock, time(NULL));
        then_d[0] = '\0';
        p.off = a->nv.off ? (int)(a->nv.off_m + 0.5) : 0;
        p.turn_m = (int)(a->nv.cue_m + 0.5);
        p.kind = a->nv.cue_i >= 0 ? r->cue[a->nv.cue_i].kind : CUE_DEST;
        p.then_kind = CUE_DEST;
        if (a->nv.cue_i >= 0 && a->nv.cue_i + 1 < r->ncue) {
            p.then_kind = r->cue[a->nv.cue_i + 1].kind;
            fmt_short(then_d, sizeof then_d,
                      r->cue[a->nv.cue_i + 1].along_m -
                          r->cue[a->nv.cue_i].along_m);
        }
        p.then_d = then_d;
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
          "togo_m\tpct\teta_s\theading_deg\tzoom_mpp\tpresented\n",
          g_trace);
}

static void
trace_row(const app_t *a, double zoom_mpp, int presented)
{
    if (!g_trace)
        return;
    fprintf(g_trace,
            "%.3f\t%.7f\t%.7f\t%d\t%.2f\t%.2f\t%d\t%d\t%.2f\t%.2f\t%.3f\t"
            "%.1f\t%.2f\t%.2f\t%d\n",
            a->t, a->fx.lat, a->fx.lon, a->nv.seg, a->nv.along, a->nv.off_m,
            a->nv.off, a->nv.cue_i, a->nv.cue_m, a->nv.togo_m, a->nv.pct,
            a->nv.eta_s, a->heading * (180.0 / M_PI), zoom_mpp, presented);
}

/* --------------------------------------------------------- the fix cycle */

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
    heading_update(a, a->fx.course, a->fx.speed_kmh);
    return 1;
}

/* The rung the NAV map will land on, for the trace. Recomputed here rather
 * than reached out of view_nav.c, which keeps the page a pure function of
 * its arguments -- and lets the trace assert the DESIGN.md 6.1 ladder from
 * outside the renderer, which is where a regression would show. */
static double
live_zoom(const app_t *a)
{
    return map_auto_zoom(a->nv.cue_m, SCR_H * 0.72);
}

/* ------------------------------------------------------------------ keys
 *
 * Device-only, and not for want of trying to keep it portable: DESIGN.md 2
 * has the keys coming from /dev/input/event0 with EVIOCGRAB, because fbterm
 * is SIGSTOPped for as long as the panel is owned. The host lane runs
 * headless replays, which have nobody to press anything. */
#ifdef NAV_DEVICE

static int
handle_key(app_t *a, int ch)
{
    switch (ch) {
    case '\t':
        a->page = a->page == LIVE_NAV ? LIVE_OVERVIEW : LIVE_NAV;
        return 1;
    case 'q':
    case 'Q':
        a->quit = 1;
        return 1;
    case 'h':
    case 'H':
        a->hold = !a->hold;
        return 1;
    case 'o':
    case 'O':
        a->course_up = !a->course_up;
        return 1;
    default:
        return 0;
    }
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

static volatile sig_atomic_t g_signal_quit;

static void
on_signal(int s)
{
    (void)s;
    g_signal_quit = 1;
}

/* --------------------------------------------------------------- chooser */

#ifdef NAV_DEVICE
/* DESIGN.md 2: "Run with no route, it lists /home/beepy/routes/*.gpx and
 * waits for a selection -- a startup chooser, not a page."
 *
 * It has to be drawn on the panel and driven by evdev, because by the time
 * this runs fbterm is SIGSTOPped and stdin is going nowhere. That is the
 * whole reason this cannot be twenty lines of fgets().
 *
 * Returns 0 with the path in `out`, or -1 when the rider quit. */
static int
chooser_run(char *out, size_t outsz, fb_t *fb)
{
    chooser_t ch;
    canvas_t *cv = canvas_new(SCR_W, SCR_H);
    char dir[192];
    int done = 0, rc = -1, dirty = 1;

    if (!cv)
        return -1;
    chooser_default_dir(dir, sizeof dir);
    chooser_scan(&ch, dir);

    while (!done && !g_signal_quit) {
        struct pollfd pfd[MAX_EVDEV];
        int np = 0, i;
        if (dirty) {
            cov_begin(&COV);
            view_chooser(&COV, &ch);
            cov_resolve(&COV, cv);
            fb_present(fb, cv);
            dirty = 0;
        }
        for (i = 0; i < evdev_count(); i++) {
            pfd[np].fd = evdev_fd(i);
            pfd[np].events = POLLIN;
            np++;
        }
        if (np == 0) {
            /* No keyboard: the list is on screen but unusable, and looping
             * on a poll with no fds would spin. Say so and give up rather
             * than hang with a picture of a menu. */
            fputs("beepy-nav: no evdev keyboard for the route chooser\n",
                  stderr);
            break;
        }
        if (poll(pfd, (unsigned)np, 500) <= 0)
            continue;
        for (i = 0; i < np; i++) {
            int code, value;
            if (!(pfd[i].revents & POLLIN))
                continue;
            while (evdev_next_key(pfd[i].fd, &code, &value)) {
                if (value != 1)
                    continue;
                switch (code) {
                case KEY_N:
                case KEY_DOWN:
                    chooser_move(&ch, 1);
                    dirty = 1;
                    break;
                case KEY_P:
                case KEY_UP:
                    chooser_move(&ch, -1);
                    dirty = 1;
                    break;
                case KEY_ENTER:
                case KEY_KPENTER:
                    if (ch.n > 0) {
                        snprintf(out, outsz, "%s", ch.path[ch.sel]);
                        rc = 0;
                        done = 1;
                    }
                    break;
                case KEY_Q:
                case KEY_ESC:
                    done = 1;
                    break;
                default:
                    break;
                }
            }
        }
    }
    free(cv->bits);
    free(cv);
    return rc;
}
#endif /* NAV_DEVICE */

/* ---------------------------------------------------------------- source */

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

/* ------------------------------------------------------------------ loop */

static int
run_live(app_t *a, const char *devpath, const char *replaypath, int headless,
         int pace_opt, int do_print)
{
    canvas_t *cv, *prev;
    FILE *replay = NULL;
    int paced = 0;
    double prev_utc = -1.0;
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

    if (replaypath) {
        replay = fopen(replaypath, "rb");
        if (!replay) {
            perror(replaypath);
            return 1;
        }
        /* Paced against the clock the sentences carry, so the panel moves at
         * the speed the ride did. Off by default under --headless: the
         * replay assertions would otherwise take as long as the ride. */
        paced = pace_opt >= 0 ? pace_opt : !headless;
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
            if (poll(pfd, (unsigned)np, 1000) <= 0)
                continue;
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

        if (paced) {
            double u = fix_utc_seconds(a->gps.utc);
            if (u >= 0.0 && prev_utc >= 0.0) {
                double d = u - prev_utc;
                if (d < 0.0)
                    d += 86400.0;
                sleep_s(d);
            }
            if (u >= 0.0)
                prev_utc = u;
        }

        /* The panel is redrawn either way -- the clock ticks and NO FIX has
         * to be able to appear -- but the trace is one row per FIX, as its
         * header promises, so a dropout leaves a gap in t rather than a run
         * of duplicated rows. */
        got_fix = on_epoch(a, time(NULL));
        if (do_print && got_fix)
            printf("t=%.0f %.6f,%.6f seg=%d along=%.0f off=%.1f%s cue=%d "
                   "in %.0fm togo=%.0f %.1f%% eta=%.0fs hdg=%.0f\n",
                   a->t, a->fx.lat, a->fx.lon, a->nv.seg, a->nv.along,
                   a->nv.off_m, a->nv.off ? " OFF" : "", a->nv.cue_i,
                   a->nv.cue_m, a->nv.togo_m, a->nv.pct, a->nv.eta_s,
                   a->heading * (180.0 / M_PI));

        if (a->hold) {
            if (got_fix)
                trace_row(a, live_zoom(a), 0);
        } else {
            int presented;
            render_live(a, &COV, cv);
            /* DESIGN.md 6.4: a frame identical to the last is not sent. That
             * is what makes a high nominal rate affordable -- a stationary
             * display costs nothing regardless of it. */
            presented =
                memcmp(cv->bits, prev->bits, (size_t)cv->stride * cv->h) != 0;
            if (presented) {
                memcpy(prev->bits, cv->bits, (size_t)cv->stride * cv->h);
#ifdef NAV_DEVICE
                if (have_fb)
                    fb_present(&fb, cv);
#endif
            }
            if (got_fix)
                trace_row(a, live_zoom(a), presented);
            for (int d = 0; d < g_ndumps; d++)
                if (!g_dumps[d].done && a->t >= g_dumps[d].at) {
                    g_dumps[d].done = 1;
                    if (canvas_dump(cv, g_dumps[d].path) < 0)
                        perror(g_dumps[d].path);
                    else
                        fprintf(stderr, "wrote %s at t=%.0f\n",
                                g_dumps[d].path, a->t);
                }
        }
    }

    if (g_trace)
        fclose(g_trace);
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
    int page = PAGE_NAV, bench = 0, demo = 0, headless = 0, do_print = 0;
    int pace_opt = -1, north_up = 0, i;
    char err[320];
    canvas_t *cv;
#ifdef NAV_DEVICE
    char chosen[512];
    fb_t chooser_fb;
#endif

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
            north_up = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(USAGE, stdout);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n%s", a, USAGE);
            return 2;
        }
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
            render_demo(&COV, cv, page); /* warm the caches; not counted */
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0; i < bench; i++)
                render_demo(&COV, cv, page);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                  (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) /
                 bench;
            printf("bench: %d frames, %.3f ms/frame\n", bench, ms);
        } else {
            render_demo(&COV, cv, page);
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
    APP.course_up = !north_up;

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
        evdev_open(1);
        fb_take(&chooser_fb);
        if (chooser_run(chosen, sizeof chosen, &chooser_fb) != 0) {
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
    return run_live(&APP, devpath, replaypath, headless, pace_opt, do_print);
}

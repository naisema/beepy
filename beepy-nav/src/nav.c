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
#include "netfetch.h"
#include "netroute.h"
#include "config.h"
#include "fix.h"
#include "led.h"
#include "map.h"
#include "ridelog.h"
#include "route.h"
#include "router.h"
#include "search.h"
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
    "usage: beepy-nav [--route FILE.gpx] [-d DEV] [--replay F.nmea]\n"
    "       beepy-nav --demo --page PAGE [--dump FILE] [--bench N]\n"
    "\n"
    "  --route FILE  the GPX to follow; omitted, the program opens on the\n"
    "                MAP page -- where you are, with no route -- and R\n"
    "                offers the routes in ~/routes (or $BEEPY_ROUTES)\n"
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
    "  --roads F     the road/name pack F searches and routes over (1.4),\n"
    "                built by tools/mkpack.py; --no-roads defeats the config\n"
    "  --no-log      do not record the ride. On a real port the raw NMEA and\n"
    "                the per-fix trace go to ~/rides/YYYYMMDD-HHMMSS.{nmea,tsv}\n"
    "                unless this says otherwise (rides_dir in the config)\n"
    "  --demo        render a static design state instead of navigating\n"
    "  --page P      nav, nav-off, nav-nofix, nav-ask, nav-tiles, map,\n"
    "                map-nofix,\n"
    "                map-wait, map-pan, map-pan-ask, map-tiles, overview,\n"
    "                overview-tiles, arrows,\n"
    "                chooser, find, find-none, confirm, cliptest,\n"
    "                cliptest-panel -- and with --route it picks the page\n"
    "                the ride opens on\n"
    "  --dump FILE   write the frame as 384000 raw XRGB bytes\n"
    "  --bench N     time N draw+resolve cycles and print ms/frame\n"
    "\n"
    "keys: Tab page   F find a destination   R change route   E end route\n"
"      Q quit (asks first)   H hold\n"
    "      O course-up/north-up   Z/X zoom out/in   A auto zoom\n"
    "      U metric/imperial   L cue alerts on/off\n"
    "map:  F find   R routes   Q quit (asks first) -- and O, Z/X/A, U as\n"
"      above; Tab is not\n"
    "      bound, because with no route there is no OVERVIEW to switch to\n"
    "find: type to filter   Down/Up select   Enter route   Esc (or Backspace\n"
    "      on an empty query) cancel;  confirm: Enter go   Q cancel\n";

enum {
    PAGE_NAV,
    PAGE_NAV_OFF,
    PAGE_NAV_NOFIX,
    PAGE_NAV_ASK,
    PAGE_NAV_TILES,
    PAGE_MAP,
    PAGE_MAP_NOFIX,
    PAGE_MAP_WAIT,
    PAGE_MAP_PAN,
    PAGE_MAP_PAN_ASK,
    PAGE_MAP_TILES,
    PAGE_OVERVIEW,
    PAGE_OVERVIEW_TILES,
    PAGE_ARROWS,
    PAGE_FIND,
    PAGE_FIND_NONE,
    PAGE_CONFIRM,
    PAGE_QUIT,
    PAGE_QUIT_MAP,
    PAGE_MAP_WAIT_HOME,
    PAGE_MAP_SAVED,
    PAGE_FIND_SAVED,
    PAGE_CLIPTEST,
    PAGE_CLIPTEST_PANEL,
    PAGE_CHOOSER
};
/* The live pages. FIND and CONFIRM are pages and not a modal sub-loop, which is
 * the whole reason they are testable: the frame clock keeps running behind them,
 * --key drives them in a headless replay exactly as it drives L, and the ride
 * that resumes after a cancel never noticed they were there.
 *
 * LIVE_MAP (DESIGN.md 1.5) is the page there is no route behind: the program
 * opens on it, and it is the one page that never coexists with a loaded route.
 * It is LAST so LIVE_NAV stays zero -- a zeroed app_t is a ride on the NAV
 * page, which is what main()'s reset across R counts on. */
enum { LIVE_NAV, LIVE_OVERVIEW, LIVE_FIND, LIVE_CONFIRM, LIVE_MAP,
       LIVE_QUIT };

/* run_live()'s "the rider wants a different route", distinct from any exit
 * status: main() loops back to the picker rather than returning it. Outside
 * the NAV_DEVICE guard because main() tests it in both lanes. */
#define RUN_PICK_AGAIN 100
/* And "the rider chose a destination on CONFIRM": main() installs the route the
 * router built and goes round the same loop, so a routed destination and a GPX
 * reach run_live() by the identical path (DESIGN.md 1.4). */
#define RUN_FIND_ROUTE 101
/* E: end the route and keep the program. The loop treats it exactly as it
 * treats a cancelled picker -- routepath cleared, round again -- which is
 * why it needs no new machinery, only a name. */
#define RUN_END_ROUTE 102

/* The coverage buffer is 96 KB; keep it out of the stack. */
static cov_t COV;

/* The basemap pack (DESIGN.md 6.5), or NULL -- which is every case in which
 * there is no basemap: none configured, the file missing, the file not a
 * pack. It lives at file scope rather than in app_t because R zeroes app_t
 * to start the next route, and a pack outlives a route: only the frame it is
 * bound to changes, which tiles_bind_route() does after each route_load(). */
static tiles_t *g_tiles;

/* The road/name pack (DESIGN.md 1.4), or NULL -- none configured, missing, or
 * not a pack. At file scope for the same reason g_tiles is: it outlives every
 * route, and unlike the tile pack it is not even bound to one (search.h). */
static roads_t *g_roads;

/* The route CONFIRM approved, waiting for main() to install it. A route_t and
 * not a path, because there is no file: router_to() built it, route_prepare()
 * and route_cues_derive() have already run on it, and main() takes ownership by
 * struct copy. */
static route_t g_found;
static int g_have_found;

/* --- the world frame (DESIGN.md 1.5, 6.1) --------------------------------
 *
 * Every position inside the program is metres east/north of ONE reference, and
 * with a route loaded that reference is the route's first point (route.c). The
 * MAP page has no route, so the reference has to come from somewhere else, and
 * the order below is not arbitrary:
 *
 *   1. the BASEMAP pack's own reference. A tile pack is pre-rendered in its own
 *      tangent frame (6.5), and tiles_bind_route() exists to translate between
 *      that frame and the caller's -- so using the pack's reference makes the
 *      translation exactly zero and the streets land where they were drawn.
 *   2. the ROAD pack's, so that the map and FIND's distances are measured from
 *      the same origin. That pack keeps its own frame by design (1.4.1).
 *   3. the first fix, when there is neither pack. Then the frame is whatever
 *      the rider is standing on, which is all a bare map needs.
 *
 * At file scope because it outlives every route: R zeroes app_t, and the frame
 * a breadcrumb was recorded against must not be zeroed with it. */
static double g_ref_lat, g_ref_lon;
static int g_have_ref;

/* --- the breadcrumb (DESIGN.md 1.5) --------------------------------------
 *
 * The track travelled this SESSION, which is a different thing from the ridden
 * track the NAV page draws: that one is the part of the route already covered
 * and dies with the route. This one is where the rider has actually been, and it
 * is kept across R and across a routed destination for the same reason g_tiles
 * is -- it outlives the route, so it lives outside app_t.
 *
 * In LAT/LON, not in world metres, and that is the load-bearing decision: the
 * frame changes when a route loads (its first point becomes the origin), so a
 * breadcrumb in metres would silently bend at every route change. Degrees cross
 * that boundary unharmed, exactly as 1.4.1 argues for the road pack, and the
 * cost is one projection per point per frame -- 2048 of them is a few thousand
 * multiplies against a 125 ms budget.
 *
 * MIN_M, because a receiver at a standstill jitters by metres: without a floor
 * ten minutes at a traffic light would fill the buffer with a scribble the size
 * of the marker. Five metres is under one pixel at every rung coarser than
 * 5 m/px, so nothing visible is lost.
 *
 * WHEN IT FILLS every second point is dropped, in place, and recording carries
 * on. So the whole session survives at coarsening resolution rather than its
 * beginning being forgotten: the older a stretch is, the sparser it is drawn,
 * which is the right trade for a mark whose job is "roughly where have I been".
 * At the 5 m floor the first halving is 10 km of riding away. */
#define CRUMB_MAX MAP_TRACK_MAX
#define CRUMB_MIN_M 5.0
/* The session odometer: metres actually travelled, for the QUIT page (1.6).
 *
 * At file scope with the breadcrumb, and for its reason: it is a fact about
 * the SESSION, not about the route, so R and E must not zero it.
 *
 * Gated on the same standstill threshold the heading freeze uses rather than a
 * new constant. A receiver sitting still still reports a metre or two of
 * multipath every second, which is 5 km over an afternoon in a car park -- and
 * an odometer that counts a parked bike is worse than no odometer, because the
 * QUIT page offers it as a reason to keep riding. */
static double g_odo_m;
static double g_odo_lat, g_odo_lon;
static int g_have_odo;

/* The ride log (DESIGN.md 7.6). Up here with the other file-scope state
 * because the QUIT page reads it: "RIDE LOG SAVED" is the fact that decides
 * whether ending the ride loses anything, and the page is rendered well above
 * the point the log used to be declared. */
/* The rider's saved places (DESIGN.md 1.4.6). At file scope beside the packs
 * and for the same reason: they outlive every route, R zeroes app_t, and the
 * FIND page wants them on a page opened long after main() handed control to
 * the ride loop. Copied out of navcfg_t rather than pointed at, because that
 * lives on main()'s stack. */
static cfgplace_t g_place[CFG_PLACES_MAX];
static int g_nplace;
/* The same places projected for the MAP page, rebuilt each frame because the
 * world frame can change under them (a route loads, R starts another). At file
 * scope only so the page can be handed a pointer that outlives this call. */
static savedmark_t g_savedmark[SAVED_MARK_MAX];
/* The travel mode (DESIGN.md 7.7). At file scope with the packs: it outlives
 * every route, and both the router and the pages want it. */
static int g_mode = NAV_MODE_BIKE;

/* The travel mode as the log spells it. On every line that reports a built
 * route, because 7.7's two profiles return 43.8 km and 56.4 km over different
 * roads -- a route length with no profile beside it is half a fact, and it is the
 * half that makes the mode toggle assertable rather than merely visible. */
static const char *
mode_name(void)
{
    return g_mode == NAV_MODE_CAR ? "car" : "bike";
}
/* Online routing (DESIGN.md 7.8). One fetch at a time, at file scope with the
 * packs because it outlives a route and because the signal handler that owns
 * the panel must be able to kill the child on the way out -- an orphaned curl
 * holding a socket open is a small thing, but it is a small thing that
 * survives the program. */
static netfetch_t g_fetch;
static char g_router_url[CFG_PATH_MAX];
static char g_router_type[16];
static char g_fetch_cmd[CFG_PATH_MAX];
/* Test seam: start a fetch at this replay second, so N1's frame-timing claim
 * can be measured before anything is wired to FIND. < 0 is off. */
static double g_fetch_at = -1.0;
static int g_fetch_fired;

/* What the fetch in flight is FOR (DESIGN.md 7.10).
 *
 * `live` is what separates a route request from --fetch-at's probe, and it is
 * not bookkeeping: the probe's fetcher answers `SLOWBYTES`, and a poll loop that
 * fed every arriving fetch to the parser would turn N1's frame-timing
 * measurement into a parse failure on the panel. The probe measures the loop;
 * this measures nothing and asks for a route.
 *
 * The destination is remembered here rather than read back off the FIND page,
 * because the two can differ by the time the bytes land: 1.4 s is long enough
 * for a rider to go on typing, and the route that arrives is the one they
 * ASKED for, not the one their thumb has since selected. */
static struct {
    int live;
    /* A REROUTE rather than a FIND, which decides what happens to the route
     * when it lands: a FIND proposes it on CONFIRM and waits for a thumb, a
     * reroute has already been agreed to and goes straight under the rider. */
    int reroute;
    char name[ROUTE_NAME];
    double lat, lon; /* the destination, degrees */
} g_req;

/* Rerouting policy (DESIGN.md 7.11): REROUTE_OFF / REROUTE_ASK / REROUTE_AUTO.
 * At file scope with the mode and the packs, because it outlives every route. */
static int g_reroute = REROUTE_ASK;

static ridelog_t RIDELOG;

static double g_crumb[2 * CRUMB_MAX]; /* lat, lon pairs, oldest first */
static int g_ncrumb;
static double g_crumb_en[2 * CRUMB_MAX]; /* the same, projected, per frame */

static void
crumb_add(double lat, double lon)
{
    int i;
    if (g_ncrumb > 0) {
        double e, n;
        geo_project(g_crumb[2 * (g_ncrumb - 1)], g_crumb[2 * (g_ncrumb - 1) + 1],
                    lat, lon, &e, &n);
        if (hypot(e, n) < CRUMB_MIN_M)
            return;
    }
    if (g_ncrumb >= CRUMB_MAX) {
        for (i = 0; 2 * i < g_ncrumb; i++) {
            g_crumb[2 * i] = g_crumb[4 * i];
            g_crumb[2 * i + 1] = g_crumb[4 * i + 1];
        }
        g_ncrumb = i;
    }
    g_crumb[2 * g_ncrumb] = lat;
    g_crumb[2 * g_ncrumb + 1] = lon;
    g_ncrumb++;
}

/* The breadcrumb in the current world frame, for the page to draw. Recomputed
 * every frame rather than cached, because the frame itself can change (a route
 * loads, R starts another) and a cache keyed on nothing would be the bug this
 * whole lat/lon representation exists to prevent. */
static const double *
crumb_world(int *n)
{
    int i;
    for (i = 0; i < g_ncrumb; i++)
        geo_project(g_ref_lat, g_ref_lon, g_crumb[2 * i], g_crumb[2 * i + 1],
                    &g_crumb_en[2 * i], &g_crumb_en[2 * i + 1]);
    *n = g_ncrumb;
    return g_crumb_en;
}

/* ------------------------------------------------------------------ demo */

static void
render_demo(cov_t *cov, canvas_t *cv, int page, const char *routes,
            tiles_t *tiles, roads_t *roads)
{
    cov_begin(cov);
    switch (page) {
    case PAGE_NAV_OFF:
        view_nav_demo(cov, 85, 0, 0);
        break;
    case PAGE_NAV_NOFIX:
        view_nav_demo(cov, 0, 1, 0);
        break;
    /* DESIGN.md 7.11's question, frozen. OFF ROUTE as well, and not as
     * decoration: the trigger is 200 m, so a rider who is being asked this is
     * always off route, and a golden of the prompt over a normal panel would
     * freeze a frame the program cannot produce. 250 m is past the trigger and
     * reads as three digits, which is what the row has to hold. */
    case PAGE_NAV_ASK:
        view_nav_demo(cov, 250, 0, 1);
        break;
    /* The basemap state. With no --basemap this is the SAME page with the
     * tile layer absent, which is exactly the comparison that says the layer
     * is optional -- see the Makefile's `check`. */
    case PAGE_NAV_TILES:
        view_nav_tiles_demo(cov, tiles);
        break;
    /* The MAP page of DESIGN.md 1.5, in its four states: a position, a position
     * that has gone stale, no position yet, and a position over a basemap. The
     * fourth is separate rather than a --basemap on the first for exactly the
     * reason nav-tiles is separate from nav -- a frozen design state must not
     * pick up a pack from the config -- and it carries geometry in the PACK's
     * own reference frame, which is the frame this page introduced. */
    case PAGE_MAP:
        view_map_demo(cov, 0);
        break;
    case PAGE_MAP_NOFIX:
        view_map_demo(cov, 1);
        break;
    case PAGE_MAP_PAN:
        view_map_pan_demo(cov, 0);
        break;
    case PAGE_MAP_PAN_ASK:
        view_map_pan_demo(cov, 1);
        break;
    case PAGE_MAP_WAIT:
        view_map_wait_demo(cov);
        break;
    case PAGE_MAP_TILES:
        view_map_tiles_demo(cov, tiles);
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
    /* The FIND page IS the search: it runs search_places() against the pack it
     * is handed, so the frozen frame is evidence about the index and not just
     * about the layout. With no --roads it renders the empty-pack state, which
     * is the pair that says "the search is what put the row there". */
    case PAGE_FIND:
        view_find_demo(cov, roads, 0);
        break;
    case PAGE_FIND_NONE:
        view_find_demo(cov, roads, 1);
        break;
    case PAGE_CONFIRM:
        view_confirm_demo(cov);
        break;
    /* Both states, because the two differ in the question AND in the way out
     * the strip offers, and a golden of one would say nothing about the other.
     * 1.6's whole claim is that the page tells the truth about what is being
     * left, and there are two truths. */
    case PAGE_QUIT:
        view_quit_demo(cov, 1);
        break;
    case PAGE_QUIT_MAP:
        view_quit_demo(cov, 0);
        break;
    case PAGE_MAP_WAIT_HOME:
        view_map_wait_home_demo(cov, g_tiles);
        break;
    case PAGE_MAP_SAVED:
        view_map_saved_demo(cov, g_tiles);
        break;
    case PAGE_FIND_SAVED:
        view_find_saved_demo(cov, roads);
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
        view_nav_demo(cov, 0, 0, 0);
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
/* Screen pixels per arrow press (1.5.1). 24 is a tenth of the map's height:
 * enough that one press is visibly a move, small enough to aim with -- and the
 * Beepy's trackpad sends a stream of presses per swipe, so a bigger step would
 * make a flick unrecoverable. */
#define PAN_STEP 24.0
/* Five screens in each direction. There is no navigational reason to be further
 * out than this, `C` is always one key away, and a bound means a fumbled swipe
 * cannot leave the rider looking at empty space with no idea which way home is. */
#define PAN_MAX 1000.0
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

    /* 0 on the MAP page of DESIGN.md 1.5, and only there: `rt` is then an empty
     * route_t and every rule of section 7 is skipped rather than run against
     * nothing. It is a flag and not a test of rt.npt because "there is no route"
     * is a statement about the program's mode -- which page it opens on, which
     * keys are bound -- and not about how many points an array happens to hold. */
    int have_route;

    /* --- FIND and CONFIRM (DESIGN.md 1.4) ------------------------------- */

    int page_back;              /* the page F was pressed on               */
    char query[FIND_QUERY_MAX]; /* what has been typed, uppercase          */
    int qn;
    place_t hit[FIND_ROWS];
    int nhits;  /* the TOTAL, which is what the title bar shows */
    int nsaved; /* how many of the rows above are saved places (1.4.6)  */
    int savedkind[FIND_ROWS]; /* SAVED_* per saved row, for its icon    */
    int nshown; /* how many of them are on screen               */
    int sel;
    /* What CONFIRM is drawing: a fully prepared route_t, so the page it is
     * shown on and the ride it becomes are the same object. Freed on cancel and
     * on quit; handed to main() by struct copy on GO. */
    route_t proposed;
    int have_proposed;
    int cf_idx[ROUTE_MAXCUE];
    int find_go; /* CONFIRM's ENTER: leave this route for the new one */
    /* What the FIND title bar says INSTEAD of the hit count, while an online
     * request is in flight or after one failed (DESIGN.md 7.10). A pointer to a
     * literal and not a buffer: there are five of them, they are chosen where
     * the thing happens, and a `char[]` would invite a caller to compose a
     * sentence too long for the bar. NULL is the ordinary count. */
    const char *net;

    /* --- the MAP pan (DESIGN.md 1.5.1) ---------------------------------- */

    /* Screen pixels away from the rider; 0,0 is following them. It does NOT
     * decay and nothing recentres it but `C` -- a map the rider deliberately
     * moved is a map they are reading, and snapping it back under them would
     * throw away the one thing they asked for. */
    double pan_x, pan_y;
    /* A pan was asked for while MOVING and is waiting for ENTER (1.5.1). The
     * step that triggered it is held here, so accepting applies the swipe the
     * rider actually made rather than nothing. */
    int pan_ask;
    double pan_want_x, pan_want_y;

    /* --- rerouting (DESIGN.md 7.11) ------------------------------------- */

    /* The ride second the deviation first passed REROUTE_OFF_M, or < 0 for "on
     * route". This is the whole of "SUSTAINED": one wild fix sets it and the
     * next good one clears it, and only a deviation that survives
     * REROUTE_OFF_S of ride clock ever reaches the trigger. */
    double off_since;
    double last_try;  /* ride second of the last attempt; < 0 = none yet   */
    int tries;        /* attempts since the deviation last cleared         */
    /* The panel prompt of `reroute = ask` is up and ENTER will answer it. Armed
     * by the trigger, cleared by the answer, by Esc, and by getting back on
     * route -- a question about a situation that has resolved itself must not
     * still be on the screen. */
    int reroute_ask;

    /* R: leave this route and go back to the picker, without leaving the
     * program. Distinct from quit, because the panel, the evdev grab and the
     * process all survive it -- only the route does not. */
    int pick_again;
    int end_route;  /* E: leave this route, stay in the program */
    int quit_from;  /* the page QUIT was opened over, to cancel back to */
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

/* The three forward declarations in this file, and they earn themselves for one
 * reason: all three are reached from the fix and frame loops, and all three are
 * written further down with the ROUTING they belong to. Moving either group to
 * meet the other would put the parser and the router next to the frame clock, or
 * the frame clock next to the search.
 *
 *   net_arrived()    bytes landed; make a route of them        (7.9, 7.10)
 *   reroute_check()  one fix's worth of 7.11's arithmetic
 *   route_install()  put a route under a running ride          (7.11)
 */
static void net_arrived(app_t *a);
static void reroute_check(app_t *a);
static void route_install(app_t *a, route_t *nr);

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
    /* have_route as well as seg: there is nothing to re-snap against on the MAP
     * page, and route_snap() on an empty route_t would index a segment that
     * does not exist. The seg test alone would be enough today (nav_reset()
     * leaves it -1 and nothing else sets it), but the invariant worth stating is
     * the mode, not the sentinel. */
    if (!a->have_route || !a->have_pos || !a->have_dr || a->nv.seg < 0)
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
    /* DESIGN.md 1.6, before everything: it is the only MODAL page, so while it
     * is up nothing behind it is drawn or reachable. Putting it first is what
     * makes that true of the renderer as well as of the key handler. */
    if (a->page == LIVE_QUIT) {
        quit_t q;
        q.riding = a->have_route;
        q.ridden_m = g_odo_m;
        q.elapsed_s = a->t;
        q.logging = RIDELOG.tsv != NULL;
        q.units = a->ctx.units;
        view_quit(cov, &q);
        cov_resolve(cov, cv);
        return;
    }
    /* DESIGN.md 1.5. First of the rest, because it is the one page that does
     * not read the route at all -- and the one page that can be on screen when
     * there is no route to read. */
    if (a->page == LIVE_MAP) {
        livemap_t lm;
        int i2;
        lm.have_pos = a->have_pos && a->have_dr;
        /* The last known position, which is exactly what 1.1.2 asks to keep
         * drawn through a loss: fix_t is not cleared by an epoch with no fix in
         * it, and the marker itself froze two seconds ago (DR_MAX_EXTRAP). */
        lm.lat = a->fx.lat;
        lm.lon = a->fx.lon;
        lm.pos_e = a->dr_e;
        lm.pos_n = a->dr_n;
        lm.track = crumb_world(&lm.ntrack);
        lm.spd_kmh = (int)(a->fx.speed_kmh + 0.5);
        lm.course_up = a->course_up;
        lm.units = a->ctx.units;
        lm.mpp_manual = a->mpp_manual;
        lm.heading = a->heading;
        lm.have_heading = a->have_heading;
        lm.residual =
            a->have_heading
                ? wrap_pi(a->raw_course - (a->course_up ? a->heading : 0.0))
                : 0.0;
        lm.tiles = g_tiles;
        lm.nofix = a->nofix;
        lm.note = a->note && a->frame_t < a->note_until ? a->note : NULL;
        /* "The satellite count if the receiver is talking at all" -- epoch_seq
         * counts epochs seen, fix or not, so this is exactly that condition. */
        lm.sats = a->epoch_seq > 0 ? a->fx.sats : -1;
        /* The first saved place, for the waiting screen (1.4.6). First and not
         * "the one called HOME": the file's order is the rider's order, and a
         * program that searched for a magic name would silently do nothing for
         * anyone who called theirs FLAT or MUM. */
        /* The saved places to draw (1.4.6). Projected here rather than in the
         * page, because this is where the world frame is known -- and the
         * PICTURE is chosen here too, so no view file matches on a name. The
         * split matters: 1.4.6 gives a magic name no behaviour, and this is
         * the one place where recognising HOME is harmless, because an
         * unrecognised name simply gets its initial. */
        lm.pan_x = a->pan_x;
        lm.pan_y = a->pan_y;
        lm.ask_pan = a->pan_ask;
        lm.nsaved = 0;
        lm.saved = g_savedmark;
        if (g_have_ref) {
            for (i2 = 0; i2 < g_nplace && i2 < SAVED_MARK_MAX; i2++) {
                savedmark_t *sm = &g_savedmark[lm.nsaved];
                geo_project(g_ref_lat, g_ref_lon, g_place[i2].lat,
                            g_place[i2].lon, &sm->e, &sm->n);
                sm->name = g_place[i2].name;
                sm->kind = !strcmp(g_place[i2].name, "HOME")   ? SAVED_HOME
                           : !strcmp(g_place[i2].name, "WORK") ? SAVED_WORK
                                                               : SAVED_OTHER;
                lm.nsaved++;
            }
        }
        lm.have_home = 0;
        if (g_nplace > 0 && g_have_ref) {
            geo_project(g_ref_lat, g_ref_lon, g_place[0].lat, g_place[0].lon,
                        &lm.home_e, &lm.home_n);
            lm.have_home = 1;
        }
        view_map(cov, &lm);
        cov_resolve(cov, cv);
        return;
    }
    if (a->page == LIVE_FIND) {
        find_t f;
        f.query = a->query;
        f.hit = a->hit;
        f.nshown = a->nshown;
        f.nhits = a->nhits;
        f.sel = a->sel;
        f.units = a->ctx.units;
        f.ndropped = roads_ndropped(g_roads);
        f.nsaved = a->nsaved;
        f.savedkind = a->savedkind;
        f.net = a->net;
        view_find(cov, &f);
        cov_resolve(cov, cv);
        return;
    }
    if (a->page == LIVE_CONFIRM && a->have_proposed) {
        const route_t *pr = &a->proposed;
        confirm_t cf;
        int i, k = 0;
        /* Every cue but the destination gets a dot; the destination gets the
         * flag. route_cues_finish() guarantees the last one is CUE_DEST. */
        for (i = 0; i < pr->ncue && k < ROUTE_MAXCUE; i++)
            if (pr->cue[i].kind != CUE_DEST)
                a->cf_idx[k++] = pr->cue[i].idx;
        cf.pts = pr->en;
        cf.npts = pr->npt;
        cf.cue_idx = a->cf_idx;
        cf.ncues = k;
        cf.dest = pr->name;
        cf.units = a->ctx.units;
        cf.mode = g_mode;
        cf.note = a->note && a->frame_t < a->note_until ? a->note : NULL;
        view_confirm(cov, &cf);
        cov_resolve(cov, cv);
        return;
    }
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
        p.ask_reroute = a->reroute_ask;
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
     * gap they describe is precisely the stretch with no fix rows in it.
     *
     * `crumb` is 1.5's breadcrumb length. It is here rather than derivable
     * because the 5 m floor and the halving are decisions this trace is the only
     * outside view of: a column that merely counted fixes would tell nothing
     * about either. Last, so every assertion written against the columns before
     * it still reads the same file. */
    fputs("#t\tisfix\tsince_fix\tbase_e\tbase_n\tbase_crs\tbase_spd\tdt\t"
          "err_e\terr_n\tfix_err\tease_w\tdr_e\tdr_n\tcourse_deg\t"
          "heading_deg\tpresented\tms\tled\toff_latched\tcue_i\tcue_m\t"
          "cue_q\tnofix\tcrumb\n",
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
            "%d\t%.2f\t%d\t%d\t%d\n",
            t, isfix, a->frames_since_fix, a->fix_e, a->fix_n,
            a->fix_course * (180.0 / M_PI), a->fix_speed, dt, a->err_e,
            a->err_n, a->fix_err, a->ease_w, a->dr_e, a->dr_n,
            a->raw_course * (180.0 / M_PI), a->heading * (180.0 / M_PI),
            presented, ms, isfix ? a->alert_fired : 0, a->nv.off,
            a->rnv.cue_i, a->rnv.cue_m, a->nv.cue_q, a->nofix, g_ncrumb);
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

    /* No route AND no pack: the frame is wherever the rider turned out to be
     * (DESIGN.md 1.5). Bound here rather than in main() because this is the
     * first moment the answer exists, and the tile pack is told so that a
     * basemap acquired later still lines up. */
    if (!g_have_ref) {
        g_ref_lat = a->fx.lat;
        g_ref_lon = a->fx.lon;
        g_have_ref = 1;
        tiles_bind_route(g_tiles, g_ref_lat, g_ref_lon);
    }
    geo_project(g_ref_lat, g_ref_lon, a->fx.lat, a->fx.lon, &a->e, &a->n);
    a->have_pos = 1;
    /* The breadcrumb (1.5) is recorded whether or not there is a route, and in
     * degrees, so it is the one thing here that does not depend on the frame
     * above. It is the session's track, not the route's. */
    crumb_add(a->fx.lat, a->fx.lon);
    /* The odometer, from the SAME fix and before any decimation: the crumb
     * drops points closer together than CRUMB_MIN_M, and summing it would
     * understate a ride by however much that threw away. Gated on the
     * standstill threshold the heading freeze already uses rather than a new
     * constant -- a receiver sitting still reports a metre or two of multipath
     * a second, which is kilometres over an afternoon, and an odometer that
     * counts a parked bike is worse than none: the QUIT page offers the figure
     * as a reason to keep riding. */
    if (g_have_odo && a->fx.speed_kmh >= DR_MOVING_KMH) {
        double oe, on;
        geo_project(g_odo_lat, g_odo_lon, a->fx.lat, a->fx.lon, &oe, &on);
        g_odo_m += hypot(oe, on);
    }
    g_odo_lat = a->fx.lat;
    g_odo_lon = a->fx.lon;
    g_have_odo = 1;
    /* Section 7's whole machinery, and only where there is a route to run it
     * against: the MAP page skips it rather than snapping to an empty array. */
    if (a->have_route) {
        /* The ride clock, not the wall clock. DESIGN.md 7.2's "after 30 s lost"
         * is about how long the RECEIVER has been quiet, which the sentences
         * themselves say; wall time would also make the rule untestable, because
         * an unpaced replay of an hour's riding takes a second. */
        route_snap(&a->rt, &a->ctx, a->e, a->n, (time_t)a->t, &a->nv);
        route_offroute_update(&a->ctx, &a->nv);
        route_cue_ahead(&a->rt, &a->nv);
        route_progress(&a->rt, &a->ctx, a->t, &a->nv);
        /* DESIGN.md 7.11, and on the FIX rather than the frame: it is a rule
         * about how far off the route the rider MEASURABLY is, and between fixes
         * there is no new measurement -- only dead reckoning, which would let
         * the trigger fire on an extrapolation. */
        reroute_check(a);
    }
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
    if (a->mpp_manual > 0.0)
        return a->mpp_manual;
    /* Auto-zoom fits the next CUE (DESIGN.md 6.1), and the MAP page has no
     * route and therefore no cue to fit: its ladder has a default rung instead,
     * which is what Z and X step away from and A comes back to. */
    if (!a->have_route)
        return MAP_MPP_DEFAULT;
    return map_auto_zoom(a->nv.cue_m, SCR_H * 0.72);
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
    /* The fetch, reaped between frames and never waited on (DESIGN.md 7.8).
     * This is the whole non-blocking claim in two lines: a child that has not
     * finished costs one waitpid(WNOHANG) per frame, and one that has finished
     * costs a read of a file nothing is writing to any more. */
    if (g_fetch.state == NF_RUNNING) {
        int st = netfetch_poll(&g_fetch);
        if (st == NF_READY) {
            fprintf(stderr, "beepy-nav: fetched %ld bytes\n", g_fetch.len);
            /* A route request, or --fetch-at's probe? The probe's answer is
             * nine bytes of SLOWBYTES and has nothing to do with routing. */
            if (g_req.live)
                net_arrived(a);
        } else if (st == NF_FAILED) {
            fprintf(stderr, "beepy-nav: fetch failed -- %s\n", g_fetch.why);
            if (g_req.live) {
                g_req.live = 0;
                /* The deadline and the network are different things to a rider:
                 * one says wait somewhere with signal, the other says the
                 * server is having a bad day. netfetch tells them apart so this
                 * does not have to read its sentence. */
                a->net = g_fetch.timedout ? "TIMED OUT" : "NO SIGNAL";
            }
        }
    }
    if (g_fetch_at >= 0.0 && !g_fetch_fired && t >= g_fetch_at) {
        g_fetch_fired = 1;
        if (netfetch_start(&g_fetch, g_fetch_cmd, g_router_url, "{}") != 0)
            fprintf(stderr, "beepy-nav: fetch not started -- %s\n",
                    g_fetch.why);
        else
            fprintf(stderr, "beepy-nav: fetch started at t=%.2f\n", t);
    }
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

/* Move the view, clamped. Nothing else touches pan_x/pan_y, so the clamp cannot
 * be bypassed by a second caller forgetting it. */
static void
pan_by(app_t *a, double dx, double dy)
{
    a->pan_x += dx;
    a->pan_y += dy;
    if (a->pan_x > PAN_MAX)
        a->pan_x = PAN_MAX;
    if (a->pan_x < -PAN_MAX)
        a->pan_x = -PAN_MAX;
    if (a->pan_y > PAN_MAX)
        a->pan_y = PAN_MAX;
    if (a->pan_y < -PAN_MAX)
        a->pan_y = -PAN_MAX;
}

static void
note_show(app_t *a, const char *s)
{
    a->note = s;
    a->note_until = a->frame_t + NOTE_S;
}

/* --------------------------------------------------- FIND (DESIGN.md 1.4)
 *
 * Two keycodes that are not characters, so the arrow keys can move the
 * selection without stealing a letter from the query. Above 255 deliberately:
 * every value handle_key() gets from a keyboard or from stdin is a byte, so
 * these cannot collide with one.
 */
#define NAVKEY_DOWN 0x100
#define NAVKEY_UP 0x101
/* LEFT and RIGHT exist for the MAP pan of 1.5.1 and for nothing else. They were
 * unmapped everywhere until then -- which is also why the Beepy's trackpad,
 * whose driver runs it in arrow mode (touch_as = keys), could scroll the FIND
 * list with up and down but do nothing at all sideways. */
#define NAVKEY_LEFT 0x102
#define NAVKEY_RIGHT 0x103

/* Where the search measures from. The fix if there has ever been one, and
 * otherwise the pack's own reference -- which is the middle of the corridor it
 * was cut for, and the only position that exists before the receiver has said
 * anything. A cold navigator on a roadside is exactly when a rider wants to
 * type a destination, so refusing to search without a fix would break the
 * feature at the one moment it matters. The distances are then honest about a
 * position nobody claimed; nothing else on the page depends on them. */
static void
find_origin(const app_t *a, double *e, double *n)
{
    if (a->epochs > 0)
        roads_project(g_roads, a->fx.lat, a->fx.lon, e, n);
    else
        *e = *n = 0.0;
}

/* One saved place (1.4.6) rendered as an ordinary search hit.
 *
 * A place_t is exactly what a saved place already is -- a name, a coordinate,
 * and a distance and bearing from wherever the rider is -- so nothing
 * downstream needs to know which list a row came from. The FIND page draws it
 * the same, N/P select it the same, and ENTER hands router_to() the same three
 * fields. That is why favourites cost no new page and no new key.
 *
 * `place` is -1: it indexes the pack's own table and a saved place is not in
 * it. Nothing reads that field for routing; it is set for the same reason the
 * pack sets it, so a debugger can tell the two apart. */
static void
saved_as_hit(const app_t *a, const cfgplace_t *cp, double from_e, double from_n,
             place_t *out)
{
    double e, n;

    /* Through the pack's own projection when there is one, so a saved place
     * and a searched one are measured in the same frame -- otherwise HOME and
     * the street outside it would disagree about how far away they are. With
     * no pack there is nothing to route over anyway, and geo_project() from
     * the world reference keeps the distance honest for the display. */
    if (g_roads)
        roads_project(g_roads, cp->lat, cp->lon, &e, &n);
    else
        geo_project(g_ref_lat, g_ref_lon, cp->lat, cp->lon, &e, &n);
    out->name = cp->name;
    out->e = e;
    out->n = n;
    out->lat = cp->lat;
    out->lon = cp->lon;
    out->dist_m = hypot(e - from_e, n - from_n);
    out->bearing = atan2(e - from_e, n - from_n);
    out->place = -1;
    (void)a;
}

/* Re-run the search. Called on every keystroke that changes the query, which is
 * what makes this type-to-filter and not type-then-search: the whole cost is
 * one pass over the name table (DESIGN.md 1.4). */
static void
find_update(app_t *a)
{
    double e, n;
    int i, ns = 0;

    find_origin(a, &e, &n);
    /* Saved places first, and above the pack's own hits rather than mixed into
     * their ranking. They are the rider's list: two of them at the top of an
     * empty page is the whole feature, and burying HOME at rank 9 of 29 546
     * because a soi happens to be nearer would be a search that technically
     * worked. An EMPTY query matches nothing in the pack (search.h) but shows
     * every saved place -- which is what fills a page that used to open blank.
     */
    for (i = 0; i < g_nplace && ns < FIND_ROWS; i++) {
        if (a->qn > 0 && !search_name_matches(g_place[i].name, a->query))
            continue;
        saved_as_hit(a, &g_place[i], e, n, &a->hit[ns]);
        a->savedkind[ns] = !strcmp(g_place[i].name, "HOME")   ? SAVED_HOME
                           : !strcmp(g_place[i].name, "WORK") ? SAVED_WORK
                                                              : SAVED_OTHER;
        ns++;
    }
    a->nsaved = ns;
    a->nhits = search_places(g_roads, a->query, e, n, a->hit + ns,
                             FIND_ROWS - ns);
    /* The title counts what the page is showing. With no query typed there is
     * nothing to have hit, so it counts the saved list instead and says so --
     * "2 SAVED" rather than a "0 HITS" that would read as a failure. */
    a->nshown = ns + (a->nhits < FIND_ROWS - ns ? a->nhits : FIND_ROWS - ns);
    if (a->sel >= a->nshown)
        a->sel = a->nshown > 0 ? a->nshown - 1 : 0;
}

static void
find_open(app_t *a)
{
    a->page_back = a->page;
    a->page = LIVE_FIND;
    a->qn = 0;
    a->query[0] = '\0';
    a->sel = 0;
    a->net = NULL;
    find_update(a);
}

static void
find_drop_proposed(app_t *a)
{
    if (a->have_proposed)
        route_free(&a->proposed);
    a->have_proposed = 0;
}

/* ------------------------------------------------------- online (7.10) */

/* Abandon whatever is in flight. Called when the rider leaves FIND, when a
 * second request starts, and from the exit path -- an orphaned curl holding a
 * socket open outlives the program, and so does the temp file it was writing
 * to. */
static void
net_abandon(app_t *a)
{
    if (g_req.live || g_fetch.state == NF_RUNNING)
        netfetch_cancel(&g_fetch);
    g_req.live = 0;
    if (a)
        a->net = NULL;
}

/* atexit(), so it also covers the SIGINT/SIGTERM path -- on_signal() only sets
 * a flag and the loop returns through main(). */
static void
fetch_atexit(void)
{
    net_abandon(NULL);
}

/* The request, which is the one thing N2 deliberately did not build.
 *
 * Valhalla takes a JSON POST body, so it goes in the file netfetch puts at
 * $BEEPY_BODY. OSRM takes its coordinates in the PATH, so the whole URL is
 * composed here and reaches the fetcher as $BEEPY_URL. Neither is interpolated
 * into the shell line -- the environment is the whole reason netfetch takes
 * them separately (7.8).
 *
 * `mode` steers Valhalla's costing and CANNOT steer OSRM's, and that asymmetry
 * is deliberate rather than an omission. An OSRM profile is part of its URL
 * (`/route/v1/<profile>/`) and no two deployments agree on what it is called --
 * the public demo answers to `bike`, `cycling`, `driving` and `foot` with the
 * identical car route, which is the measurement that stopped this program
 * shipping a default URL at all. So the profile stays in the rider's
 * `router_url`, where they can see it, and a `mode = car` that an OSRM URL will
 * not honour says so on stderr rather than silently meaning nothing. */
static void
net_request(double slat, double slon, double dlat, double dlon, char *url,
            int nurl, char *body, int nbody)
{
    const int car = g_mode == NAV_MODE_CAR;

    if (netroute_type(g_router_type) == NETROUTE_OSRM) {
        /* %.6f: OSRM's own coordinate precision, and about 11 cm -- further
         * decimals would be noise from a GPS with a 3 m CEP. */
        snprintf(url, (size_t)nurl,
                 "%s/%.6f,%.6f;%.6f,%.6f?overview=full&steps=true"
                 "&geometries=polyline",
                 g_router_url, slon, slat, dlon, dlat);
        body[0] = '\0';
        if (car)
            fprintf(stderr, "beepy-nav: mode = car does not reach an OSRM "
                            "server -- its profile is part of router_url\n");
        return;
    }
    snprintf(url, (size_t)nurl, "%s", g_router_url);
    /* directions_options.units is asked for explicitly because netroute.c
     * believes whatever `trip.units` comes back saying, and a server whose
     * default is miles would otherwise be checked against a distance in
     * kilometres. Asking removes the question. */
    snprintf(body, (size_t)nbody,
             "{\"locations\":[{\"lat\":%.6f,\"lon\":%.6f},"
             "{\"lat\":%.6f,\"lon\":%.6f}],\"costing\":\"%s\","
             "\"directions_options\":{\"units\":\"kilometers\"}}",
             slat, slon, dlat, dlon, car ? "auto" : "bicycle");
}

/* Ask the online router for what the pack could not answer. The FIND page stays
 * up and the title bar says FETCHING; the rider can go on typing, which is the
 * entire point of N1's child process. */
static void
net_start(app_t *a, const place_t *h, int reroute)
{
    char url[CFG_PATH_MAX + 128], body[512];

    if (!g_router_url[0]) {
        /* The default state of this program, and therefore the one a rider meets
         * first: there is no router, so there was never anything that could have
         * answered. Both halves are said -- the panel because the rider is
         * waiting on it, stderr because whoever is reading a log over SSH is
         * the person who can fix it. */
        fprintf(stderr, "beepy-nav: no router_url configured, so there is "
                        "nothing to ask\n");
        a->net = "NO ROUTER";
        return;
    }
    if (a->epochs < 1) {
        /* An online router needs two coordinates and we have one. Offline never
         * hits this because FIND's origin falls back to the pack reference,
         * which is fine for a distance and not for a route. */
        fprintf(stderr, "beepy-nav: no fix yet, so there is no start to ask "
                        "about\n");
        a->net = "NO FIX";
        return;
    }
    net_abandon(a);
    net_request(a->fx.lat, a->fx.lon, h->lat, h->lon, url, (int)sizeof url,
                body, (int)sizeof body);
    if (netfetch_start(&g_fetch, g_fetch_cmd, url, body[0] ? body : NULL) != 0) {
        fprintf(stderr, "beepy-nav: fetch not started -- %s\n", g_fetch.why);
        a->net = "NO SIGNAL";
        return;
    }
    g_req.live = 1;
    /* Set HERE and nowhere else: this is the line where the request becomes real,
     * and a `reroute` flag left standing after a FAILED start would make the next
     * FIND install its route without a CONFIRM page. That is the bug this
     * parameter exists to make impossible rather than to remember not to write. */
    g_req.reroute = reroute;
    g_req.lat = h->lat;
    g_req.lon = h->lon;
    snprintf(g_req.name, sizeof g_req.name, "%s", h->name ? h->name : "");
    a->net = "FETCHING";
    fprintf(stderr, "beepy-nav: asking %s for a route to %s\n", g_router_type,
            g_req.name);
}

/* The bytes landed. Parse them into a proposal and show CONFIRM -- the same two
 * lines the offline router reaches, so CONFIRM and everything past it cannot
 * tell which router produced what it is drawing. */
static void
net_arrived(app_t *a)
{
    char why[NETROUTE_WHY];
    int type = netroute_type(g_router_type);
    int code = NR_OK;

    g_req.live = 0;
    find_drop_proposed(a);
    why[0] = '\0';
    if (netroute_parse(g_fetch.buf, type < 0 ? NETROUTE_VALHALLA : type,
                       g_req.name, &a->proposed, why, (int)sizeof why, &code)) {
        fprintf(stderr, "beepy-nav: %s\n", why);
        /* THREE answers, because three is what a rider can act on differently --
         * and this used to be one, which told them the wrong thing. A 203 km
         * destination refused by a server that caps a bicycle at 200 km reported
         * NO ROUTE, i.e. "there is no way there", when the truth was "not from
         * this server": ride to it, or point router_url somewhere without the
         * cap. TOO FAR is the difference between those two.
         *
         * The rest still collapse, and that part of the original argument
         * stands: a captive portal and a malformed body are both BAD REPLY,
         * because a rider cannot do anything different about them and the
         * sentence that tells them apart is on stderr for whoever debugs it. */
        a->net = code == NR_TOOFAR    ? "TOO FAR"
                 : code == NR_REFUSED ? "NO ROUTE"
                                      : "BAD REPLY";
        return;
    }
    a->net = NULL;
    fprintf(stderr, "beepy-nav: routed to %s -- %d points, %.2f km, %d cues"
                    ", %s (online)\n",
            a->proposed.name, a->proposed.npt, a->proposed.total_m / 1000.0,
            a->proposed.ncue, mode_name());
    if (g_req.reroute) {
        /* Already agreed to -- by the config in `auto`, by ENTER in `ask` -- so
         * there is nothing left to confirm. It goes under the rider. */
        g_req.reroute = 0;
        route_install(a, &a->proposed);
        a->have_proposed = 0;
        return;
    }
    a->have_proposed = 1;
    a->page = LIVE_CONFIRM;
}

/* Route to the selected hit and show CONFIRM. An unreachable destination is a
 * message on the FIND page's transient row and nothing else -- never a crash,
 * and never a half-built route (router_to() leaves `out` empty on failure).
 *
 * OFFLINE FIRST, ALWAYS (DESIGN.md 7.10). Inside the pack the answer costs
 * 0.1 ms and no connection; online costs 1.4 s and needs one. So the pack is
 * asked first and the network only where the pack cannot honestly answer --
 * which is two of the eight RC_* codes and not a judgement about the sentence
 * router_to() wrote. */
static void
find_route_selected(app_t *a)
{
    static char why[96];
    double e, n;
    int code = RC_OK;
    const place_t *h;
    route_t nr;

    if (a->sel < 0 || a->sel >= a->nshown)
        return;
    if (g_req.live)
        return; /* one request at a time; ENTER during a fetch is not a queue */
    h = &a->hit[a->sel];
    find_origin(a, &e, &n);
    a->net = NULL;
    /* Into a LOCAL, and the old proposal is not dropped until this one exists.
     * A failed attempt must leave the rider looking at the route they already
     * had -- which is netroute_parse()'s rule from 7.9 applied one level up, and
     * it is what makes 7.7's mode toggle on CONFIRM safe: a bicycle refused up a
     * motorway-only spur puts the mode back and the route is still there. */
    route_init(&nr);
    if (router_to(g_roads, e, n, h->e, h->n, g_mode, h->name, &nr, why,
                  (int)sizeof why, &code)) {
        fprintf(stderr, "beepy-nav: %s\n", why);
        if (code == RC_OFFMAP || code == RC_UNREACHABLE) {
            net_start(a, h, 0);
            return;
        }
        note_show(a, "NO ROUTE");
        return;
    }
    find_drop_proposed(a);
    a->proposed = nr;
    a->have_proposed = 1;
    a->page = LIVE_CONFIRM;
    fprintf(stderr,
            "beepy-nav: routed to %s -- %d points, %.2f km, %d cues, %s\n",
            a->proposed.name, a->proposed.npt, a->proposed.total_m / 1000.0,
            a->proposed.ncue, mode_name());
}

/* ---------------------------------------------------- rerouting (7.11)
 *
 * The first thing in this program that acts on its own while the rider is
 * moving, which is why every number below is a brake rather than a feature.
 */

/* NOT the 40 m off-route latch of 7.3. That latch fires for GPS wobble in a
 * city canyon and for a deliberate stop at a shop twenty metres off the line,
 * and rerouting on either would be both irritating and expensive. 200 m is a
 * distance a bicycle only reaches by having gone somewhere else. */
#define REROUTE_OFF_M 200.0
/* And sustained, on the RIDE clock rather than a fix count, because the same
 * ten seconds is ten fixes at 1 Hz and fifty at the 5 Hz of 6.3 -- a count would
 * quietly mean something different on a receiver that had been sped up. Ten
 * seconds at city speed is seventy metres of extra riding, which against a 200 m
 * deviation is noise; ten fixes agreeing is not multipath. */
#define REROUTE_OFF_S 10.0
/* One attempt a minute. A rider parked 300 m off route must not re-fetch until
 * the battery dies, and this is the first of the two brakes that stops it. */
#define REROUTE_MIN_S 60.0
/* The second brake, and it counts attempts since the deviation last CLEARED
 * rather than since the ride began.
 *
 * Resetting on a successful install would be the obvious rule and it is the
 * wrong one: a router that snaps the rider to a road they are not on returns a
 * route they are still 300 m off, so every attempt would "succeed", reset the
 * count, and the loop the cap exists to stop would run through the success path.
 * Resetting on "back on route" is the condition that actually means the
 * situation resolved -- and it is also the answer to what happens when an online
 * route starts 500 m away, which needs no refusal of its own because this stops
 * it after five. */
#define REROUTE_MAX 5

/* Put a route under the rider, in place, mid-ride.
 *
 * NOT the RUN_FIND_ROUTE hand-off that CONFIRM's ENTER uses. That leaves
 * run_live() and comes back, which re-opens the serial port, re-runs the UBX
 * rate exchange of 6.3 and starts a new ride log -- several seconds with no
 * position, at the exact moment the rider is off route and moving. Acceptable
 * for FIND, where the rider is stopped and choosing; not acceptable here.
 *
 * So the route is swapped under a running ride, and the price is paid in one
 * line: route_rebase() brings it into the tangent frame the ride is ALREADY in
 * (route.h says why that is cheaper than moving the frame). Everything else
 * that survives -- the breadcrumb, the odometer, the ride log, the port, the
 * panel -- survives because it was never in app_t or was never in the frame. */
static void
route_install(app_t *a, route_t *nr)
{
    int units = a->ctx.units;

    if (g_have_ref)
        route_rebase(nr, g_ref_lat, g_ref_lon);
    route_free(&a->rt);
    a->rt = *nr;
    memset(nr, 0, sizeof *nr); /* ownership moved; the caller must not free it */
    a->have_route = 1;

    /* Every value section 7 keeps between fixes was about the route that is
     * gone: the snap window hint, the off-route latch, the ten-minute speed
     * ring, the countdown latch. A window hint pointing at segment 900 of a
     * route with 40 segments is not a stale optimisation, it is a wrong answer.
     */
    nav_init(&a->ctx);
    nav_init(&a->rctx);
    nav_set_units(&a->ctx, units); /* nav_init() zeroes it back to metric */
    nav_reset(&a->nv);
    a->alert_cue = -1; /* 7.5's rungs are indices into the old cue list */

    /* Re-snap on the spot rather than waiting for the next fix, so no frame is
     * ever drawn with a new route and last second's position on it. */
    if (a->have_pos) {
        route_snap(&a->rt, &a->ctx, a->e, a->n, (time_t)a->t, &a->nv);
        route_offroute_update(&a->ctx, &a->nv);
        route_cue_ahead(&a->rt, &a->nv);
        route_progress(&a->rt, &a->ctx, a->t, &a->nv);
    }
    a->reroute_ask = 0;
    a->off_since = -1.0;
    a->tries = 0;
    /* The re-snapped deviation is on this line for one reason: it is the number
     * that says whether route_rebase() did its job. A new route goes under the
     * rider, so it starts where they are and this reads a few metres. Skip the
     * rebase and route_snap() compares a world-frame position against a
     * route-frame polyline, and it reads the distance between the two origins --
     * tens of kilometres. There is no third possibility, which is what makes one
     * grep enough. */
    fprintf(stderr,
            "beepy-nav: rerouted to %s -- %d points, %.2f km, %d cues, "
            "%.0f m off, %s\n",
            a->rt.name, a->rt.npt, a->rt.total_m / 1000.0, a->rt.ncue,
            a->have_pos ? a->nv.off_m : -1.0, mode_name());
}

/* Where a reroute is going: the end of the route the rider is on.
 *
 * The DESTINATION and not the next cue, which is the question the plan left
 * open and the geometry answers -- off route by 200 m, the next cue is very
 * often behind you, and a route to it would turn the rider round to reach a
 * junction they no longer need. It is also all a GPX can offer: a file names a
 * path and an endpoint, and once the rider is 200 m off the path, the endpoint
 * is the only part of it they still want. That a scenic GPX becomes the shortest
 * way to its own finish is a real loss, and the honest alternative -- rejoining
 * the line at the nearest point ahead -- is a routing problem this pack cannot
 * pose. `off` exists for riders who would rather keep the line. */
static void
reroute_target(const app_t *a, double *lat, double *lon)
{
    *lat = a->rt.pt[a->rt.npt - 1].lat;
    *lon = a->rt.pt[a->rt.npt - 1].lon;
}

/* Build the replacement and put it under the rider, or start a fetch that will.
 * Offline first, exactly as FIND is (7.10): the pack answers in 0.1 ms and needs
 * no connection, and a rider who has drifted 200 m off a route inside their own
 * pack is the commonest case by far. */
static void
reroute_go(app_t *a)
{
    static char why[96];
    double dlat, dlon, de, dn, se, sn;
    int code = RC_OK;
    route_t nr;

    a->last_try = a->t;
    a->tries++;
    a->reroute_ask = 0;
    /* ONE line per attempt, before anything can fail, so the log answers "how
     * many times did it try" by counting rather than by inference. That is the
     * question T-REROUTE asks -- both brakes are claims about a COUNT -- and the
     * lines below it are about how each one turned out. */
    fprintf(stderr, "beepy-nav: reroute %d of %d: %.0f m off route for %.0f s\n",
            a->tries, REROUTE_MAX, a->nv.off_m,
            a->off_since >= 0.0 ? a->t - a->off_since : 0.0);
    reroute_target(a, &dlat, &dlon);
    if (!g_roads) {
        /* No pack and no router is not a reroute at all. Said once per attempt
         * rather than once per fix, which is what the rate limit above buys. */
        fprintf(stderr, "beepy-nav: %s\n",
                g_router_url[0] ? "no road pack; asking the router"
                                : "no road pack and no router_url: cannot "
                                  "reroute");
        if (!g_router_url[0])
            return;
    } else {
        roads_project(g_roads, a->fx.lat, a->fx.lon, &se, &sn);
        roads_project(g_roads, dlat, dlon, &de, &dn);
        route_init(&nr);
        if (router_to(g_roads, se, sn, de, dn, g_mode, a->rt.name, &nr, why,
                      (int)sizeof why, &code) == 0) {
            route_install(a, &nr);
            return;
        }
        fprintf(stderr, "beepy-nav: reroute: %s\n", why);
        if (code != RC_OFFMAP && code != RC_UNREACHABLE)
            return;
    }
    /* Same two codes as 7.10, and the same reason: those are the only failures a
     * wider map could fix. */
    {
        place_t h;
        memset(&h, 0, sizeof h);
        h.name = a->rt.name;
        h.lat = dlat;
        h.lon = dlon;
        net_start(a, &h, 1);
        /* The FIND page is not open, so 7.10's title bar cannot carry this.
         * A transient can: `auto` is a thing the program is doing on its own,
         * and 1.5 s of the bottom row is the smallest honest way to say so. */
        if (a->net && strcmp(a->net, "FETCHING") != 0)
            note_show(a, a->net);
        a->net = NULL;
    }
}

/* Once per fix. Everything here is arithmetic on off_m and the ride clock; the
 * only thing it can start is one attempt a minute, five to an episode. */
static void
reroute_check(app_t *a)
{
    if (g_reroute == REROUTE_OFF || !a->have_route || a->rt.npt < 2)
        return;
    if (a->nv.seg < 0 || a->nofix)
        return; /* nothing has been measured, so nothing is known */

    if (a->nv.off_m <= REROUTE_OFF_M) {
        /* Back on route -- or never off it. This is the one place the episode
         * ends: the cap resets here and nowhere else. */
        a->off_since = -1.0;
        a->tries = 0;
        a->reroute_ask = 0;
        return;
    }
    if (a->off_since < 0.0) {
        a->off_since = a->t;
        return;
    }
    if (a->t - a->off_since < REROUTE_OFF_S)
        return; /* off, but not yet SUSTAINED */
    if (a->tries >= REROUTE_MAX || g_req.live)
        return;
    if (a->last_try >= 0.0 && a->t - a->last_try < REROUTE_MIN_S)
        return; /* one a minute, however long the rider stays out here */

    if (g_reroute == REROUTE_ASK) {
        if (!a->reroute_ask) {
            /* Armed, and NOTHING ELSE: ask mode spends no battery and no data
             * until the rider says so, which is the whole difference between it
             * and auto. The attempt is counted when ENTER is pressed, so the
             * cap counts requests rather than questions. */
            a->reroute_ask = 1;
            fprintf(stderr, "beepy-nav: %.0f m off route -- ENTER to "
                            "reroute\n", a->nv.off_m);
        }
        return;
    }
    reroute_go(a);
}

/* The FIND page owns every key. That is not an oversight: the whole surface of
 * this page is a text field, so Q types a Q and H types an H, and a page that
 * quietly kept a global meaning for one of the twenty-six letters would eat a
 * character out of half the street names in Bangkok.
 *
 * Which leaves cancelling. DESIGN.md 1.4's table says N/P move the selection,
 * and that cannot be true here for the same reason -- so the arrows do, and
 * Esc backs out. Backspace on an EMPTY query backs out too, because the Beepy's
 * Esc needs the symbol layer and a rider must never be able to reach a page
 * they cannot leave with the keys already under their thumbs. */
static int
find_key(app_t *a, int ch)
{
    switch (ch) {
    case '\n':
    case '\r':
        find_route_selected(a);
        break;
    case 27: /* Esc */
        /* Leaving the page abandons the request. A rider who backed out of FIND
         * is not waiting for anything, and a CONFIRM page appearing over the
         * NAV page 1.4 seconds later would be the program answering a question
         * they had withdrawn. */
        net_abandon(a);
        a->page = a->page_back;
        break;
    case '\b':
    case 127: /* DEL, which is what a terminal sends for Backspace */
        if (a->qn == 0) {
            net_abandon(a);
            a->page = a->page_back;
            break;
        }
        a->query[--a->qn] = '\0';
        /* Typing clears the STATUS but not the request: the count is about to
         * be true again, and the fetch is still the rider's own. FETCHING is
         * left alone for the same reason -- net_abandon() is the only thing that
         * stops a fetch, and a keystroke is not it. */
        if (!g_req.live)
            a->net = NULL;
        find_update(a);
        break;
    case NAVKEY_DOWN:
        if (a->nshown > 0)
            a->sel = (a->sel + 1) % a->nshown;
        break;
    case NAVKEY_UP:
        if (a->nshown > 0)
            a->sel = (a->sel + a->nshown - 1) % a->nshown;
        break;
    default:
        /* A-Z, 0-9 and space, and nothing else: those are the glyphs
         * tools/gen_query.py prepared, and a character with no glyph would
         * blank the whole query line rather than appear as a box. Lower case is
         * folded up because the pack's names are uppercase and so is the font.
         *
         * Digits need the Beepy's symbol layer (DESIGN.md 2 -- "the digit row
         * needs the Alt modifier"), which is why the keymap accepts them from
         * evdev as well: the overlay sends KEY_1..KEY_0 and this is the one
         * page where a digit is worth the modifier. */
        if (ch >= 'a' && ch <= 'z')
            ch = ch - 'a' + 'A';
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
              ch == ' '))
            return 0;
        if (a->qn + 1 >= FIND_QUERY_MAX)
            return 0; /* the field is full; a silent no-op beats a wrap */
        a->query[a->qn++] = (char)ch;
        a->query[a->qn] = '\0';
        if (!g_req.live)
            a->net = NULL;
        find_update(a);
        break;
    }
    key_repaint(a);
    return 1;
}

/* CONFIRM has exactly the three keys its own strip advertises. */
static int
confirm_key(app_t *a, int ch)
{
    switch (ch) {
    /* DESIGN.md 7.7's travel mode, on the page where changing it has a visible
     * consequence: the route is rebuilt for the same destination, so the
     * distance, the estimate, the turn count and the drawn line all change. That
     * is why the toggle is here and not only in the config file -- 43.8 km by
     * bicycle and 56.4 km by car are not a refinement of the same line. */
    case 'm':
    case 'M': {
        int was = g_mode;
        if (!a->have_proposed || g_req.live)
            return 0; /* nothing to rebuild, or one already in flight */
        g_mode = g_mode == NAV_MODE_CAR ? NAV_MODE_BIKE : NAV_MODE_CAR;
        find_route_selected(a);
        if (!a->have_proposed) {
            /* Cannot happen: find_route_selected() only drops the proposal once
             * a replacement exists. Belt and braces, because the alternative is
             * a CONFIRM page with no route on it. */
            g_mode = was;
            note_show(a, "NO ROUTE");
        } else if (g_req.live) {
            /* The pack could not answer and the router was asked. The strip
             * already reads the new mode; the route on it is still the old one
             * for about 1.4 s, and this row says something is happening rather
             * than leaving the page apparently inert. */
            note_show(a, "FETCHING");
        } else {
            note_show(a, g_mode == NAV_MODE_CAR ? "CAR" : "BIKE");
        }
        break;
    }
    case '\n':
    case '\r':
        if (!a->have_proposed)
            return 0;
        /* Hand the route to main() and leave this one. Not a new program state:
         * the same RUN_* mechanism R uses, so the panel, the evdev grab and the
         * process all survive, and the ride that starts is started by exactly
         * the code that starts a GPX ride. */
        g_found = a->proposed;
        g_have_found = 1;
        a->have_proposed = 0; /* ownership moved; do not free it here */
        a->find_go = 1;
        a->quit = 1;
        return 1; /* no repaint: the next frame belongs to the new route */
    case 'q':
    case 'Q':
    case 27: /* Esc, for a thumb that has just used it on the page before */
        find_drop_proposed(a);
        a->page = LIVE_FIND;
        break;
    default:
        return 0;
    }
    key_repaint(a);
    return 1;
}

/* End the route and keep the program: back to the MAP page with the position
 * still live. The mechanism is the one R and a cancelled picker already use --
 * leave run_live(), clear the route, come round again -- so nothing here has to
 * unwind a ride in place. Everything that must survive (the breadcrumb, the
 * odometer, the packs, the panel) already lives outside app_t. */
static void
end_route(app_t *a)
{
    a->end_route = 1;
    a->quit = 1;
}

/* QUIT (DESIGN.md 1.6): the two decisions its strip advertises, plus the third
 * way out for a rider who wanted to stop the RIDE rather than the program. */
static int
quit_key(app_t *a, int ch)
{
    switch (ch) {
    case '\n':
    case '\r':
        a->quit = 1;
        return 1; /* no repaint: the next thing that happens is the exit */
    case 'e':
    case 'E':
        /* Only when there is one. On MAP the strip offers Tab instead, so this
         * cannot be reached by a rider following the screen -- but a thumb is
         * not following the screen, and a key that silently ends nothing is
         * better than one that ends the program by falling through. */
        if (!a->have_route)
            return 0;
        end_route(a);
        return 1;
    case 'q':
    case 'Q':
    case '\t':
    case 27: /* Esc, the same cancel CONFIRM takes */
        a->page = a->quit_from;
        break;
    default:
        /* Everything else is swallowed. It is a modal page: a stray O or Z
         * changing the map behind an unanswered question would be the bug that
         * makes a rider distrust the answer. */
        return 1;
    }
    key_repaint(a);
    return 1;
}

static int
handle_key(app_t *a, int ch)
{
    if (a->page == LIVE_QUIT)
        return quit_key(a, ch);
    if (a->page == LIVE_FIND)
        return find_key(a, ch);
    if (a->page == LIVE_CONFIRM)
        return confirm_key(a, ch);
    /* The MAP pan of 1.5.1. Arrows, and only on MAP -- the NAV page's map is a
     * third of the screen with a route in it and auto-zooms to the next cue, so
     * there is nothing there a pan would improve.
     *
     * PANNING WHILE MOVING IS A DECISION, not a nudge: the map stops following
     * the rider and nothing puts it back but `C`. So at speed the first swipe
     * asks instead of moving, and the swipe it asks about is remembered so that
     * ENTER applies the pan the rider actually made. Stopped, it just pans --
     * there is nothing to be unaware of when the map and the bike are both
     * still. */
    if (a->page == LIVE_MAP &&
        (ch == NAVKEY_UP || ch == NAVKEY_DOWN || ch == NAVKEY_LEFT ||
         ch == NAVKEY_RIGHT)) {
        double dx = ch == NAVKEY_LEFT ? PAN_STEP : ch == NAVKEY_RIGHT ? -PAN_STEP : 0.0;
        double dy = ch == NAVKEY_UP ? PAN_STEP : ch == NAVKEY_DOWN ? -PAN_STEP : 0.0;
        if (a->fx.speed_kmh >= DR_MOVING_KMH && !a->pan_x && !a->pan_y &&
            !a->pan_ask) {
            a->pan_ask = 1;
            a->pan_want_x = dx;
            a->pan_want_y = dy;
            fprintf(stderr, "beepy-nav: map pan while moving -- ENTER to hold "
                            "the map, C to centre it\n");
        } else if (!a->pan_ask) {
            pan_by(a, dx, dy);
        }
        key_repaint(a);
        return 1;
    }
    if (a->page == LIVE_MAP && a->pan_ask && (ch == '\n' || ch == '\r')) {
        a->pan_ask = 0;
        pan_by(a, a->pan_want_x, a->pan_want_y);
        key_repaint(a);
        return 1;
    }
    /* C centres it, from either state, and is the ONLY thing that does. Bound
     * even when the map is already centred, because a rider who has lost track
     * of where the view is should not have to work out whether the key applies
     * before pressing it. */
    if (a->page == LIVE_MAP && (ch == 'c' || ch == 'C')) {
        a->pan_ask = 0;
        a->pan_x = a->pan_y = 0.0;
        key_repaint(a);
        return 1;
    }

    /* ENTER answers the reroute prompt of 7.11, and answers nothing else: it is
     * unbound on all three ride pages, which is what left it free to become the
     * one key this question needs. Esc declines -- the prompt does not simply
     * expire, because a rider who has not looked down yet has not declined.
     *
     * The pan question above cannot collide with it: that one is MAP only and
     * this one needs a route, and MAP is the page with no route.
     *
     * Both are checked BEFORE the switch, so a prompt cannot be answered by
     * accident with a key that already means something else. */
    if (a->reroute_ask && (ch == '\n' || ch == '\r')) {
        reroute_go(a);
        key_repaint(a);
        return 1;
    }
    if (a->reroute_ask && ch == 27) {
        /* Declined. It stays declined until the deviation clears and comes back,
         * or REROUTE_MIN_S passes -- the rate limit is what stops "no" from
         * being asked again on the next fix. */
        a->reroute_ask = 0;
        a->last_try = a->t;
        a->tries++;
        note_show(a, "NO REROUTE");
        key_repaint(a);
        return 1;
    }
    switch (ch) {
    case '\t':
        /* DESIGN.md 1.5: not bound on MAP. Tab's promise is "switch page (there
         * are only two)", and both of those are pages OF a ride -- OVERVIEW
         * without a route would have to draw a route it does not have. It is
         * left unbound rather than made a no-op with a message, because this
         * page's own strip lists the keys it has (F / R / Q): a key that is not
         * on that line was never claimed, which is the honest form of "does
         * nothing" and the one case §2's "a dead key is indistinguishable from
         * a broken program" does not apply to. */
        if (a->page == LIVE_MAP)
            return 0;
        a->page = a->page == LIVE_NAV ? LIVE_OVERVIEW : LIVE_NAV;
        break;
    case 'r':
    case 'R':
        /* Only where there is a picker to go back to: a --route on the command
         * line or a replay has no list behind it, and a key that silently does
         * nothing is worse than one that is not bound. From MAP it is the way
         * a route gets loaded at all (1.5). */
        if (a->can_pick) {
            a->pick_again = 1;
            a->quit = 1;
        }
        break;
    case 'q':
    case 'Q':
        /* DESIGN.md 1.6. Q used to end the program on one press, which on a
         * bike is a ride lost to a thumb landing one key over. It opens the
         * question instead, and the question knows which page it came from. */
        a->quit_from = a->page;
        a->page = LIVE_QUIT;
        break;
    case 'e':
    case 'E':
        /* End the route, keep the navigator. Without this the only ways out of
         * a ride were R, which demands you pick another one, and Q, which ends
         * the program -- and neither is "I have arrived, I am going to ride
         * home now". */
        if (!a->have_route) {
            note_show(a, "NO ROUTE");
            break;
        }
        end_route(a);
        return 1;
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
        if (a->have_route)
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
    /* The same key as CONFIRM's, and 7.5's argument for L applies to it word for
     * word: the effect is invisible in the direction that matters. Here there is
     * no route to rebuild -- the one being ridden was already built -- so what it
     * changes is the NEXT one: a reroute (7.11) or the next thing F finds. The
     * transient is the only way a rider can tell they pressed it. */
    case 'm':
    case 'M':
        g_mode = g_mode == NAV_MODE_CAR ? NAV_MODE_BIKE : NAV_MODE_CAR;
        note_show(a, g_mode == NAV_MODE_CAR ? "CAR" : "BIKE");
        break;
    /* DESIGN.md 2 promises F opens FIND "from any page", and 1.4 says nothing
     * is searchable that is not in the pack. With no pack there is nothing to
     * type into, and the one thing this key must not be is silent: a dead key
     * is indistinguishable from a broken program. The transient row says which
     * of the two it is, once, and the ride carries on underneath.
     *
     * "Any page" now includes MAP, which is the whole point of 1.5's flow --
     * open the program, see where you are, search a destination, go. The MAP
     * strip has its own transient row for exactly this message. */
    case 'f':
    case 'F':
        if (!g_roads) {
            note_show(a, "NO ROAD PACK");
            break;
        }
        find_open(a);
        break;
    default:
        return 0;
    }
    key_repaint(a);
    return 1;
}

/* --key SEC:NAME for the presses that are not characters. Without these the
 * FIND flow could only be driven by a physical thumb, and DESIGN.md 2 is
 * explicit that this is not a debugging affordance to be removed later: it is
 * the only way any of the keymap is testable. */
static int
keyname_to_ch(const char *s)
{
    static const struct {
        const char *name;
        int ch;
    } NAMES[] = {{"enter", '\n'}, {"esc", 27},        {"bs", '\b'},
                 {"space", ' '},  {"tab", '\t'},      {"down", NAVKEY_DOWN},
                 {"up", NAVKEY_UP},   {"left", NAVKEY_LEFT},
                 {"right", NAVKEY_RIGHT}};
    size_t i;
    if (!s || !*s)
        return -1;
    if (!s[1])
        return (unsigned char)s[0];
    for (i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (!strcmp(s, NAMES[i].name))
            return NAMES[i].ch;
    return -1;
}

/* Presses scheduled against the ride clock (--key SEC:CHAR). The mechanism a
 * replay test uses to press a key, and the reason handle_key() above is not
 * behind NAV_DEVICE. */
#define MAX_KEYS 16
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

/* Every letter, every digit, space and the four editing keys.
 *
 * Before FIND existed this mapped the ten bound keys and nothing else, and that
 * was the right size: a key with no meaning should not become a character. The
 * FIND page changes the premise -- its whole surface is a text field, so the
 * WHOLE alphabet has to arrive, and what a key means is decided by handle_key()
 * against the current page rather than here. Letters with no meaning on the NAV
 * page still fall through handle_key()'s default and cost nothing.
 *
 * The table is written out rather than derived from KEY_A..KEY_Z, because Linux
 * numbers those in QWERTY row order and not alphabetically -- KEY_A is 30 and
 * KEY_B is 48. A loop over that range would map half the alphabet wrong. */
static int
keycode_to_char(int code)
{
    switch (code) {
    case KEY_A: return 'a';
    case KEY_B: return 'b';
    case KEY_C: return 'c';
    case KEY_D: return 'd';
    case KEY_E: return 'e';
    case KEY_F: return 'f';
    case KEY_G: return 'g';
    case KEY_H: return 'h';
    case KEY_I: return 'i';
    case KEY_J: return 'j';
    case KEY_K: return 'k';
    case KEY_L: return 'l';
    case KEY_M: return 'm';
    case KEY_N: return 'n';
    case KEY_O: return 'o';
    case KEY_P: return 'p';
    case KEY_Q: return 'q';
    case KEY_R: return 'r';
    case KEY_S: return 's';
    case KEY_T: return 't';
    case KEY_U: return 'u';
    case KEY_V: return 'v';
    case KEY_W: return 'w';
    case KEY_X: return 'x';
    case KEY_Y: return 'y';
    case KEY_Z: return 'z';
    /* The digit row needs the Beepy's symbol layer (DESIGN.md 2), which sends
     * these keycodes; the FIND page is the one place a digit is worth the
     * modifier, and "SOI 23" is why. */
    case KEY_0: return '0';
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_3: return '3';
    case KEY_4: return '4';
    case KEY_5: return '5';
    case KEY_6: return '6';
    case KEY_7: return '7';
    case KEY_8: return '8';
    case KEY_9: return '9';
    case KEY_SPACE: return ' ';
    case KEY_BACKSPACE: return '\b';
    case KEY_ENTER:
    case KEY_KPENTER: return '\n';
    case KEY_ESC: return 27;
    case KEY_DOWN: return NAVKEY_DOWN;
    case KEY_UP: return NAVKEY_UP;
    case KEY_LEFT: return NAVKEY_LEFT;
    case KEY_RIGHT: return NAVKEY_RIGHT;
    case KEY_TAB: return '\t';
    default:
        return 0;
    }
}

/* stdin, byte at a time, with just enough of a state machine to turn the two
 * arrow keys' CSI sequences into single presses.
 *
 * Not a terminal library: without it "\x1b[B" arrives on the FIND page as Esc
 * (which cancels), '[' (which is not a glyph) and 'B' (which is typed) -- so a
 * rider pressing Down over ssh would leave the page and type a letter into
 * nothing. Three bytes of state buys the whole keymap a second input source,
 * and DESIGN.md 2 is explicit that the ssh source is not optional. */
static void
stdin_keys(app_t *a)
{
    static int esc; /* 0 = idle, 1 = saw Esc, 2 = saw Esc [ */
    char k;
    while (read(STDIN_FILENO, &k, 1) == 1) {
        if (esc == 1 && k == '[') {
            esc = 2;
            continue;
        }
        if (esc == 2) {
            esc = 0;
            if (k == 'A')
                handle_key(a, NAVKEY_UP);
            else if (k == 'B')
                handle_key(a, NAVKEY_DOWN);
            else if (k == 'D')
                handle_key(a, NAVKEY_LEFT);
            else if (k == 'C')
                handle_key(a, NAVKEY_RIGHT);
            /* Home, End and the rest: swallowed rather than typed. */
            continue;
        }
        if (esc == 1) {
            /* Esc followed by anything else really was an Esc, and then this. */
            esc = 0;
            handle_key(a, 27);
        }
        if (k == 27) {
            esc = 1;
            continue;
        }
        handle_key(a, (unsigned char)k);
    }
    /* Esc still pending at the end of the drain was a real Esc: an arrow key's
     * three bytes are written by the terminal in one go and read in one go, so
     * "Esc alone in this batch" is the classic disambiguation and the reason
     * this does not need a timeout. A "[" still pending is left for the next
     * call, which is the one case where the sequence really did split. */
    if (esc == 1) {
        esc = 0;
        handle_key(a, 27);
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
    roads_close(g_roads);
    g_roads = NULL;
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
        if (!(pfd[i].revents & POLLIN))
            continue;
        if (g_tty_raw && pfd[i].fd == STDIN_FILENO) {
            stdin_keys(a);
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
    /* In BOTH lanes, and not inside cleanup() with the panel, because the thing
     * being cleaned up is not device-specific: a fetch still running at exit
     * leaves an orphaned child holding a socket and a temp file in /tmp, which
     * on this device is RAM. 7.8 promised the temp file was unlinked on every
     * exit path; until FIND could start a fetch that outlives a page, nothing in
     * this file had ever called netfetch_cancel() and the promise was only true
     * because every fetch happened to finish. */
    atexit(fetch_atexit);
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
    /* One frame before the first sentence arrives.
     *
     * Every other frame on this panel is owed to the frame clock of 6.3, and
     * that clock does not start until there is a fix to extrapolate from -- so
     * with a receiver that is cold, unplugged, or simply half a second from its
     * first GGA, the panel would go on showing whatever fbterm left on it. On a
     * --route ride that was survivable: somebody who passed a route is watching
     * for a route. On the MAP page of 1.5 it is the entire first impression, and
     * "open the program and see where you are" cannot begin with a stale
     * terminal. This is the same frame the first fixless epoch would have drawn a
     * second later -- WAITING FOR FIX, and its satellite count once there is
     * one -- drawn now instead.
     *
     * Not frame_at(): this is not a frame of the ride clock. It owes no trace
     * row, it must not advance the dead reckoning, and there is no ride second
     * to stamp it with yet. It does take the 6.4 skip's bookkeeping, so the real
     * first frame is presented only if it differs. */
    if (RC.have_fb) {
        a->frame_t = 0.0;
        render_state(a);
        render_live(a, &COV, RC.cv);
        memcpy(RC.prev->bits, RC.cv->bits, (size_t)RC.cv->stride * RC.cv->h);
        fb_present(RC.fb, RC.cv);
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
                if (!(pfd[i].revents & POLLIN))
                    continue;
                if (g_tty_raw && pfd[i].fd == STDIN_FILENO) {
                    stdin_keys(a);
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

    /* A proposal the rider neither accepted nor cancelled -- Ctrl-C on the
     * CONFIRM page -- is still holding a route_t's four allocations. */
    find_drop_proposed(a);

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
     * fbterm between the two screens, and re-taking can fail. The same is true
     * of a GO from CONFIRM, which is the same hand-off to a different caller. */
    if (!a->pick_again && !a->find_go && !a->end_route && g_panel_open) {
        fb_release(&g_panel);
        g_panel_open = 0;
        g_fb = NULL;
    }
#endif
    fprintf(stderr, "beepy-nav: %ld fixes, %.1f%% done, %d full scans\n",
            a->epochs, a->nv.pct, a->ctx.full_scans);
    if (a->find_go)
        return RUN_FIND_ROUTE;
    if (a->end_route)
        return RUN_END_ROUTE;
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
    g_reroute = cfg.reroute;
    snprintf(g_router_url, sizeof g_router_url, "%s", cfg.router_url);
    snprintf(g_router_type, sizeof g_router_type, "%s", cfg.router_type);
    snprintf(g_fetch_cmd, sizeof g_fetch_cmd, "%s", cfg.fetch_cmd);
    memcpy(g_place, cfg.place, sizeof g_place);
    g_nplace = cfg.nplace;
    g_mode = cfg.mode;

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
            else if (!strcmp(p, "nav-ask"))
                page = PAGE_NAV_ASK;
            else if (!strcmp(p, "nav-tiles"))
                page = PAGE_NAV_TILES;
            else if (!strcmp(p, "map"))
                page = PAGE_MAP;
            else if (!strcmp(p, "map-pan"))
                page = PAGE_MAP_PAN;
            else if (!strcmp(p, "map-pan-ask"))
                page = PAGE_MAP_PAN_ASK;
            else if (!strcmp(p, "map-nofix"))
                page = PAGE_MAP_NOFIX;
            else if (!strcmp(p, "map-wait"))
                page = PAGE_MAP_WAIT;
            else if (!strcmp(p, "map-tiles"))
                page = PAGE_MAP_TILES;
            else if (!strcmp(p, "overview"))
                page = PAGE_OVERVIEW;
            else if (!strcmp(p, "overview-tiles"))
                page = PAGE_OVERVIEW_TILES;
            else if (!strcmp(p, "arrows"))
                page = PAGE_ARROWS;
            else if (!strcmp(p, "find"))
                page = PAGE_FIND;
            else if (!strcmp(p, "find-none"))
                page = PAGE_FIND_NONE;
            else if (!strcmp(p, "confirm"))
                page = PAGE_CONFIRM;
            else if (!strcmp(p, "quit"))
                page = PAGE_QUIT;
            else if (!strcmp(p, "quit-map"))
                page = PAGE_QUIT_MAP;
            else if (!strcmp(p, "map-wait-home"))
                page = PAGE_MAP_WAIT_HOME;
            else if (!strcmp(p, "map-saved"))
                page = PAGE_MAP_SAVED;
            else if (!strcmp(p, "find-saved"))
                page = PAGE_FIND_SAVED;
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
        else if (!strcmp(a, "--fetch-at") && i + 1 < argc) {
            /* Test seam (DESIGN.md 7.8): start a fetch at this replay second.
             * It exists so N1's claim -- that a fetch in flight costs the 8 Hz
             * loop nothing -- can be MEASURED before anything is wired to
             * FIND. The same reason --dump-at and --key exist. */
            g_fetch_at = atof(argv[++i]);
        }
        else if (!strcmp(a, "--fps") && i + 1 < argc) {
            fps = atof(argv[++i]);
            if (!(fps > 0.0) || fps > 60.0) {
                fprintf(stderr, "bad --fps: %s\n%s", argv[i], USAGE);
                return 2;
            }
        }
        else if (!strcmp(a, "--key") && i + 1 < argc) {
            const char *v = argv[++i], *colon = strchr(v, ':');
            int ch = colon ? keyname_to_ch(colon + 1) : -1;
            if (!colon || ch < 0 || g_nkeys >= MAX_KEYS) {
                fprintf(stderr, "bad --key: %s\n%s", v, USAGE);
                return 2;
            }
            g_keys[g_nkeys].at = atof(v);
            g_keys[g_nkeys].ch = ch;
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
        else if (!strcmp(a, "--roads") && i + 1 < argc)
            snprintf(cfg.roads, sizeof cfg.roads, "%s", argv[++i]);
        else if (!strcmp(a, "--no-roads"))
            cfg.roads[0] = '\0';
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

    /* The road pack (DESIGN.md 1.4), on exactly the same terms: one line either
     * way, and a navigator that still navigates without it. The difference is
     * what its absence costs -- a basemap that is missing is a decoration that
     * is missing, and a road pack that is missing means F has nothing to search,
     * which is why the key says so rather than doing nothing. The dropped-name
     * count is printed because it is the honest measure of "nothing is
     * searchable that is not in the pack". */
    if (cfg.roads[0]) {
        char why[96];
        g_roads = roads_open(cfg.roads, why, (int)sizeof why);
        if (!g_roads)
            fprintf(stderr, "beepy-nav: %s: %s; no search or routing\n",
                    cfg.roads, why);
        else
            fprintf(stderr,
                    "beepy-nav: roads %s -- %d nodes, %d edges, %d names "
                    "(%d dropped, no ASCII form), oneway %s, "
                    "reference %.5f,%.5f\n",
                    cfg.roads, roads_nnode(g_roads), roads_nedge(g_roads),
                    roads_nplace(g_roads), roads_ndropped(g_roads),
                    roads_honours_oneway(g_roads) ? "honoured" : "IGNORED",
                    roads_ref_lat(g_roads), roads_ref_lon(g_roads));
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
            render_demo(&COV, cv, page, routes, g_tiles, g_roads); /* warm the caches */
            clock_gettime(CLOCK_MONOTONIC, &t0);
            for (i = 0; i < bench; i++)
                render_demo(&COV, cv, page, routes, g_tiles, g_roads);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
                  (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) /
                 bench;
            printf("bench: %d frames, %.3f ms/frame\n", bench, ms);
        } else {
            render_demo(&COV, cv, page, routes, g_tiles, g_roads);
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
    /* --page picks the page a ride opens on; with no --route the program opens
     * on MAP regardless, because that is what "no route" means (DESIGN.md 1.5)
     * and there is no ride to open a page of. */
    APP.page = page == PAGE_OVERVIEW ? LIVE_OVERVIEW : LIVE_NAV;
    APP.course_up = !cfg.north_up;
    APP.fps = fps;
    APP.rate_5hz = cfg.rate_5hz;
    /* The config sets where the ride starts; L moves it from there, and
     * nothing writes the file back. */
    APP.alerts = cfg.led_alerts;
    APP.alert_cue = -1;
    /* < 0 is "never", and 0 is a real ride second: a zeroed app_t
     * would read as "the deviation started at t=0" and "an attempt
     * was made at t=0", which are two different wrong answers. */
    APP.off_since = -1.0;
    APP.last_try = -1.0;
    nav_init(&APP.rctx);
    nav_set_units(&APP.ctx, cfg.units);
    led_init(cfg.led_alerts);

    /* DESIGN.md 1.5: with no --route the program opens on MAP, not on the route
     * picker. That is the whole change in the startup flow -- the picker is
     * still there, one R away, and R is a page change rather than an exit now.
     *
     * The picker used to run HERE, before the loop, which is why the loop below
     * could assume a route existed. It runs only after an R now, inside the
     * loop, and nothing before the loop opens the panel: run_live() does it on
     * the first frame exactly as it does for a --route ride. */
#ifdef NAV_DEVICE
    /* R has somewhere to go from MAP. Not from a --route ride, where DESIGN.md
     * 2's argument still holds: that started from a command line and has no list
     * behind it, and a key that silently does nothing is worse than one that is
     * not bound. */
    if (!routepath && !headless)
        APP.can_pick = 1;
#endif
    /* Ride, and come back here if the rider pressed R. The picker keeps the
     * panel and the grab across the hand-off in both directions, so changing
     * route never drops to fbterm and never re-opens the framebuffer. */
    for (;;) {
        int rc;

        /* Either a file, or a destination the router built, or nothing at all.
         * The first two are indistinguishable from here on: route_load() and
         * router_to() both leave a prepared route_t with cues, and this loop,
         * run_live(), the ride pages and the ride log see exactly one kind of
         * route (DESIGN.md 1.4: "a GPX IS one, and everything downstream is
         * identical"). The third is the MAP page, and `have_route` is what the
         * rest of the program tests instead of an array length. */
        if (g_have_found) {
            APP.rt = g_found;
            memset(&g_found, 0, sizeof g_found);
            g_have_found = 0;
            APP.have_route = 1;
        } else if (routepath) {
            if (route_load(routepath, &APP.rt, err, sizeof err)) {
                fprintf(stderr, "beepy-nav: %s\n", err);
                return 1;
            }
            APP.have_route = 1;
        } else {
            /* route_init() leaves an empty, valid route_t, so nothing below has
             * to test a pointer -- only `have_route`, which says what mode the
             * program is in rather than what an array happens to hold. */
            route_init(&APP.rt);
            APP.have_route = 0;
            APP.page = LIVE_MAP;
        }
        if (APP.have_route)
            fprintf(stderr, "beepy-nav: %s -- %d points, %.2f km, %d cues\n",
                    APP.rt.name, APP.rt.npt, APP.rt.total_m / 1000.0,
                    APP.rt.ncue);
        /* The world frame, and the pack's translation into it. With a route it
         * is the route's first point, exactly as it has always been -- again
         * after every R, because the next route has a different origin. With no
         * route it is a pack's own reference, which is the case tiles_bind_route()
         * was written for and never had until now: the offset is then zero and
         * the streets land where they were rendered. With neither, on_epoch()
         * binds the first fix. */
        if (APP.have_route) {
            g_ref_lat = APP.rt.lat0;
            g_ref_lon = APP.rt.lon0;
            g_have_ref = 1;
        } else if (g_tiles) {
            g_ref_lat = tiles_ref_lat(g_tiles);
            g_ref_lon = tiles_ref_lon(g_tiles);
            g_have_ref = 1;
        } else if (g_roads) {
            g_ref_lat = roads_ref_lat(g_roads);
            g_ref_lon = roads_ref_lon(g_roads);
            g_have_ref = 1;
        } else {
            g_have_ref = 0;
        }
        if (g_have_ref)
            tiles_bind_route(g_tiles, g_ref_lat, g_ref_lon);

        if (tracepath)
            trace_open(tracepath);
        if (ftracepath)
            ftrace_open(ftracepath);

        rc = run_live(&APP, devpath, replaypath, headless, pace_opt, do_print,
                      no_log ? NULL : rides);
        if (rc != RUN_PICK_AGAIN && rc != RUN_FIND_ROUTE &&
            rc != RUN_END_ROUTE)
            return rc;

        /* A different route is a different ride: the old one's state cannot
         * carry over. route_free() and a zeroed app_t are the whole reset --
         * the snap window, the off-route run, the speed ring, the countdown
         * latch and the ride clock all live in there. The ride log closed
         * with the ride, and the next one opens its own.
         *
         * Everything the ride is ABOUT to need has already been moved out:
         * g_found holds the routed destination, and the pack, the panel and the
         * breadcrumb are at file scope precisely because they outlive this
         * memset -- the breadcrumb is the SESSION's track, and a rider who
         * changes route has not been anywhere else. */
        route_free(&APP.rt);
        {
            /* The rider's SESSION decisions survive the reset; the route's do
             * not. 7.5 argues this for the mute -- a key is a decision about
             * the next ten minutes -- and it is just as true of the units and
             * the map orientation: a new destination must not silently put the
             * panel back into kilometres. `can_pick` survives because whether
             * there is a picker behind this ride is a fact about how the
             * program was started, not about which route is loaded. */
            int units = APP.ctx.units, alerts = APP.alerts;
            int course_up = APP.course_up, can_pick = APP.can_pick;
            double fps_keep = APP.fps;
            memset(&APP, 0, sizeof APP);
            nav_init(&APP.ctx);
            nav_init(&APP.rctx);
            /* A zeroed nav_t is not a reset one: nav_reset() sets seg, cue_i
             * and eta_s to their "nothing known yet" values, and zero means
             * "on segment 0, approaching cue 0, arriving now". It was
             * survivable while every pass through this loop loaded a route
             * that immediately re-snapped; with the MAP page it is not, because
             * seg 0 of an empty route is a segment that does not exist. */
            nav_reset(&APP.nv);
            nav_set_units(&APP.ctx, units);
            APP.alerts = alerts;
            APP.course_up = course_up;
            APP.can_pick = can_pick;
            APP.fps = fps_keep > 0.0 ? fps_keep : DR_FPS;
            APP.alert_cue = -1;
            APP.off_since = -1.0;
            APP.last_try = -1.0;
        }
        if (rc == RUN_FIND_ROUTE) {
            /* The destination the router built supersedes whatever file was
             * loaded before it, or this loop would reload that file on the next
             * R-free pass through. */
            routepath = NULL;
            continue;
        }
        if (rc == RUN_END_ROUTE) {
            /* E (DESIGN.md 1.6). Byte for byte what a cancelled picker does,
             * and for the same reason: clearing routepath drops this pass into
             * the no-route branch above, which is the MAP page. The rider keeps
             * the program, the position, the breadcrumb and the odometer, and
             * loses only the route they asked to be rid of. */
            routepath = NULL;
            continue;
        }
#ifdef NAV_DEVICE
        APP.can_pick = 1;
        if (chooser_run(chosen, sizeof chosen, &g_panel, routes) != 0) {
            /* Quit from the picker. Back to MAP rather than out of the program:
             * R is a page change now (1.5), and a rider who opens the list and
             * changes their mind has not asked to be dropped to a shell. Q on
             * MAP is how the program ends, and the strip says so. */
            routepath = NULL;
            continue;
        }
        routepath = chosen;
#else
        return 0;
#endif
    }
}

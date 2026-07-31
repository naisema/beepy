/* beepy-nav/src/config.h -- ~/.config/beepy-nav.conf.
 *
 * DESIGN.md 2: "There is no menu." The route is a command-line argument and
 * everything else lives in one flat file, read once at startup. That is the
 * whole scope of this module -- five settings, no sections, no types beyond
 * "flag", "word" and "path".
 *
 * THE FILE IS NEVER FATAL. A rider whose navigator refuses to start because
 * of a typo in a preferences file, at the roadside, with the only text editor
 * being whatever is on the SD card, has been failed by the program and not by
 * the typo. Every malformed line, unknown key and unreadable value warns to
 * stderr with its line number and is then ignored; the setting keeps its
 * default. An absent file is not even a warning -- it is the ordinary case.
 *
 * Portable C: stdio and string handling, nothing device-specific.
 */
#ifndef BEEPY_NAV_CONFIG_H
#define BEEPY_NAV_CONFIG_H

#include <stddef.h>

#define CFG_PATH_MAX 256
/* How many saved places the file may name. Eight because the FIND page shows
 * four rows at a time and two screens of them is already more than anyone
 * navigates to by memory -- past that, type the name. */
#define CFG_PLACES_MAX 8
#define CFG_PLACE_NAME 20

/* A place worth keeping: DESIGN.md 1.4.6. Name is uppercased on the way in,
 * because everything searchable on this device is uppercase and a saved place
 * sits in the same list as the pack's own names. */
typedef struct {
    char name[CFG_PLACE_NAME];
    double lat, lon;
} cfgplace_t;

typedef struct {
    int units;      /* UNITS_METRIC (default) / UNITS_IMPERIAL   */
    int north_up;   /* 0 = course-up, the default                */
    int rate_5hz;   /* 1 = ask the receiver for 5 Hz (DESIGN 6.3)*/
    int led_alerts; /* 1 = flash the keyboard LED, the default   */
    /* Empty means "decide the usual way" -- $BEEPY_ROUTES, then ~/routes --
     * which keeps the environment variable working for anyone who was already
     * using it, and keeps this file from having to name a default it would
     * then have to keep in step with chooser.c. */
    char routes_dir[CFG_PATH_MAX];
    /* Where the ride log goes (DESIGN.md 7.6). Empty means ~/rides, decided
     * by ridelog_default_dir() for the same reason routes_dir is: a default
     * named twice is a default that drifts. */
    char rides_dir[CFG_PATH_MAX];
    /* The OSM raster basemap pack (DESIGN.md 6.5), built on the Mac by
     * tools/mktiles.py. Empty means no basemap, which is the default and the
     * only setting here whose absence is a whole feature not happening --
     * so unlike the two directories above there is no default path to guess:
     * a pack is cut for a particular corridor and nothing can invent one. */
    char basemap[CFG_PATH_MAX];
    /* The road/name pack FIND searches and routes over (DESIGN.md 1.4), built
     * on the Mac by tools/mkpack.py. Empty means no pack, and then F says so
     * on the panel and does nothing else -- the same shape as basemap above,
     * and for the same reason: nothing can invent an extract. */
    char roads[CFG_PATH_MAX];
    /* Saved places (DESIGN.md 1.4.6): `place = HOME 13.8851,100.3785`, one per
     * line, kept IN FILE ORDER rather than sorted. The order is the rider's --
     * whatever they put first is what the FIND page offers first, and sorting
     * would quietly overrule that. The first one is also the map's centre
     * before there has ever been a fix (1.5), which is why "home first" is
     * worth documenting rather than enforcing. */
    cfgplace_t place[CFG_PLACES_MAX];
    int nplace;
    /* Travel mode (DESIGN.md 7.7): NAV_MODE_BIKE (the default -- this is a
     * bicycle navigator) or NAV_MODE_CAR. It picks the online costing and,
     * offline, which road classes the router will use. */
    int mode;
    /* Online routing (DESIGN.md 7.8). `router_url` empty means offline only,
     * and there is NO DEFAULT -- a safety argument, not a licensing one: the
     * public OSRM demo server is car-only and ignores the profile in the path,
     * so defaulting to it would route a bicycle onto an expressway. */
    char router_url[CFG_PATH_MAX];
    char router_type[16]; /* "valhalla" (default) or "osrm" */
    /* How the bytes are fetched. A command and not a compiled-in curl call,
     * because that is what lets a test substitute `cat FIXTURE` and keeps the
     * gate off the network structurally rather than by promise. */
    char fetch_cmd[CFG_PATH_MAX];
} navcfg_t;

void cfg_defaults(navcfg_t *c);

/* $HOME/.config/beepy-nav.conf, or /home/beepy/... when HOME is unset. */
void cfg_default_path(char *buf, size_t n);

/* Reads `path` over `c`, which the caller has already filled with defaults.
 * Returns 0 when the file was read, -1 when it was not there. `loud` makes an
 * absent file a warning too -- what an explicit --config wants, and what the
 * default path must not do. Complaints go to stderr either way; the return
 * value is information, not an error to act on. */
int cfg_load(navcfg_t *c, const char *path, int loud);

#endif /* BEEPY_NAV_CONFIG_H */

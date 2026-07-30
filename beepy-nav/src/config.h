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

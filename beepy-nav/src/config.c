/* beepy-nav/src/config.c -- see config.h. */
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h> /* mkdir, for the one directory cfg_place_add() may make */

#include "config.h"
#include "route.h" /* UNITS_METRIC / UNITS_IMPERIAL */
#include "search.h" /* NAV_MODE_BIKE / NAV_MODE_CAR */

void
cfg_defaults(navcfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->units = UNITS_METRIC;
    c->north_up = 0;   /* course-up: DESIGN.md 1.1 */
    c->rate_5hz = 0;   /* 6.3 calls it "a refinement rather than a dependency" */
    c->led_alerts = 1; /* 7.5; led.c already copes with an unwritable LED */
    c->nplace = 0;
    c->mode = NAV_MODE_BIKE;
    /* 1, and NOT what the memset gives -- see config.h. This is the one default
     * in this file that changes an answer the program used to give. */
    c->major_roads = 1;
    c->view3d = 0;
    c->reroute = REROUTE_ASK;
    c->router_url[0] = '\0';
    snprintf(c->router_type, sizeof c->router_type, "valhalla");
    snprintf(c->fetch_cmd, CFG_PATH_MAX, "%s",
             "curl -s -m 10 --max-filesize 1000000 "
             "-H 'Content-Type: application/json' "
             "--data-binary @\"$BEEPY_BODY\" \"$BEEPY_URL\"");

    c->routes_dir[0] = '\0';
    c->rides_dir[0] = '\0';  /* 7.6; ridelog_default_dir() decides */
    c->basemap[0] = '\0';    /* 6.5; no pack, and no path to guess  */
    c->roads[0] = '\0';      /* 1.4; likewise -- F says so when it is absent */
}

void
cfg_default_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.config/beepy-nav.conf",
             home && *home ? home : "/home/beepy");
}

void
cfg_places_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.config/beepy-nav.places",
             home && *home ? home : "/home/beepy");
}

int
cfg_place_add(const char *path, const char *name, double lat, double lon)
{
    FILE *f;

    if (!path || !*path || !name || !*name)
        return -1;
    f = fopen(path, "a");
    if (!f) {
        /* One retry after making the directory, and no more than that. A fresh
         * card has no ~/.config until something writes there, and a favourite
         * that could never be saved on a new device because a directory was
         * missing would be a feature that looks broken on first contact. Any
         * other errno -- a read-only card, a full one -- is the rider's to fix,
         * and the panel says NOT SAVED. */
        char dir[CFG_PATH_MAX];
        char *slash;
        snprintf(dir, sizeof dir, "%s", path);
        slash = strrchr(dir, '/');
        /* ONE level, which is all this needs: the parent of ~/.config is the
         * home directory and it exists by construction. EEXIST is the ordinary
         * case and not an error -- ridelog_open()'s rule, for its reason. */
        if (slash && slash != dir) {
            *slash = '\0';
            if (mkdir(dir, 0755) == 0 || errno == EEXIST)
                f = fopen(path, "a");
        }
    }
    if (!f) {
        fprintf(stderr, "beepy-nav: cannot write %s -- %s\n", path,
                strerror(errno));
        return -1;
    }
    fprintf(f, "place = %s %.5f,%.5f\n", name, lat, lon);
    /* Both checked, because a full card fails at the flush and not at the
     * write, and a favourite reported saved that is not on the card is worse
     * than one refused. */
    if (fflush(f) != 0 || ferror(f)) {
        fprintf(stderr, "beepy-nav: cannot write %s -- %s\n", path,
                strerror(errno));
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "beepy-nav: cannot close %s -- %s\n", path,
                strerror(errno));
        return -1;
    }
    return 0;
}

/* In place, both ends. Returns the new start. */
static char *
trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t')
        s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n'))
        e--;
    *e = '\0';
    return s;
}

/* One number, scanned RIGHT to LEFT out of [start, e). Returns where it begins,
 * or NULL if there is not one there. Leading whitespace before `e` is skipped, so
 * the caller can hand this the position just past the previous field. */
static char *
num_left(char *start, char *e)
{
    char *p = e;

    while (p > start && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    e = p;
    while (p > start && ((p[-1] >= '0' && p[-1] <= '9') || p[-1] == '.'))
        p--;
    if (p == e)
        return NULL; /* no digits at all: not a number */
    if (p > start && (p[-1] == '-' || p[-1] == '+'))
        p--;
    return p;
}

/* Split `NAME LAT,LON` by finding the COORDINATE FROM THE RIGHT, and so allow a
 * name with spaces in it. Returns where the coordinate starts and cuts the name
 * off in place; NULL when either half is missing.
 *
 * Taking the name as the first field was the obvious reading and it was wrong in
 * a way nothing could see. `place = PLACE 1 13.72936,100.56100` -- which is
 * exactly what 1.4.8's default name writes -- parsed as a place called PLACE at
 * 1.00000, 13.72936: a point in the Atlantic off Guinea. Both halves are legal
 * numbers, so the +-90 guard below could not catch it, and the rider would have
 * been given a favourite that read back as somewhere else entirely.
 *
 * A character-class walk from the right does not work either, and the reason is
 * worth recording: a name ending in a digit ("GYM 2") is indistinguishable from
 * part of the coordinate that way, and " 2 13.88,100.37" parses happily as
 * 2, 13.88. So this counts NUMBERS -- exactly two, with an optional comma
 * between them -- which is the only rule that can tell the two apart. */
static char *
place_split(char *val)
{
    char *lat, *lon, *p;

    lon = num_left(val, val + strlen(val));
    if (!lon)
        return NULL;
    p = lon;
    while (p > val && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    if (p > val && p[-1] == ',')
        p--; /* `LAT , LON` and `LAT,LON` reach the same place */
    lat = num_left(val, p);
    if (!lat || lat == val)
        return NULL; /* no latitude, or nothing left to be the name */
    p = lat;
    while (p > val && (p[-1] == ' ' || p[-1] == '\t'))
        p--;
    if (p == val)
        return NULL;
    *p = '\0'; /* the name ends here */
    return lat;
}

/* ASCII, in place. strcasecmp is POSIX and this file is meant to compile
 * anywhere the rest of the navigator does, so the three lines are written
 * out. Only the enumerated values get this -- routes_dir is a path, and on
 * the device's f2fs and this Mac's APFS a path's case is its own business. */
static void
lower(char *s)
{
    for (; *s; s++)
        if (*s >= 'A' && *s <= 'Z')
            *s = (char)(*s - 'A' + 'a');
}

/* 0/1, and the words people actually type. -1 for anything else. */
static int
parse_flag(const char *v)
{
    static const char *const YES[] = {"1", "yes", "true", "on"};
    static const char *const NO[] = {"0", "no", "false", "off"};
    size_t i;
    for (i = 0; i < sizeof YES / sizeof YES[0]; i++)
        if (!strcmp(v, YES[i]))
            return 1;
    for (i = 0; i < sizeof NO / sizeof NO[0]; i++)
        if (!strcmp(v, NO[i]))
            return 0;
    return -1;
}

static void
complain(const char *path, int line, const char *what, const char *detail)
{
    fprintf(stderr, "beepy-nav: %s:%d: %s%s%s%s\n", path, line, what,
            detail ? " '" : "", detail ? detail : "", detail ? "'" : "");
}

int
cfg_load(navcfg_t *c, const char *path, int loud)
{
    char line[512];
    FILE *f = fopen(path, "r");
    int lineno = 0;

    if (!f) {
        /* The ordinary case for the default path: no file, all defaults, not
         * a word about it. An explicit --config is different -- being told
         * silently nothing happened is the worst outcome of a typo'd path. */
        if (loud)
            fprintf(stderr, "beepy-nav: %s: not read; using defaults\n", path);
        return -1;
    }

    while (fgets(line, sizeof line, f)) {
        char *s, *eq, *key, *val;
        int b;

        lineno++;
        s = trim(line);
        /* A comment is a whole line. `#` inside a value is a `#`: a path may
         * legitimately contain one, and there are five keys here -- not
         * enough surface to be worth a quoting rule nobody would remember. */
        if (!*s || *s == '#')
            continue;
        eq = strchr(s, '=');
        if (!eq) {
            complain(path, lineno, "not key=value:", s);
            continue;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);
        if (!*key) {
            complain(path, lineno, "empty key", NULL);
            continue;
        }

        lower(key);
        if (!strcmp(key, "units")) {
            lower(val);
            if (!strcmp(val, "metric"))
                c->units = UNITS_METRIC;
            else if (!strcmp(val, "imperial"))
                c->units = UNITS_IMPERIAL;
            else
                complain(path, lineno, "units must be metric or imperial, not",
                         val);
        } else if (!strcmp(key, "mode")) {
            if (!strcmp(val, "bike") || !strcmp(val, "bicycle"))
                c->mode = NAV_MODE_BIKE;
            else if (!strcmp(val, "car") || !strcmp(val, "driving"))
                c->mode = NAV_MODE_CAR;
            else
                complain(path, lineno, "mode must be bike or car, not", val);
        } else if (!strcmp(key, "prefer")) {
            if (!strcmp(val, "pack") || !strcmp(val, "offline"))
                c->prefer = CFG_PREFER_PACK;
            else if (!strcmp(val, "online") || !strcmp(val, "net"))
                c->prefer = CFG_PREFER_ONLINE;
            else
                complain(path, lineno, "prefer must be pack or online, not",
                         val);
        } else if (!strcmp(key, "view3d")) {
            lower(val);
            b = parse_flag(val);
            if (b < 0)
                complain(path, lineno, "not 0 or 1:", val);
            else
                c->view3d = b;
        } else if (!strcmp(key, "major_roads")) {
            lower(val);
            b = parse_flag(val);
            if (b < 0)
                complain(path, lineno, "not 0 or 1:", val);
            else
                c->major_roads = b;
        } else if (!strcmp(key, "tolls")) {
            if (!strcmp(val, "avoid") || !strcmp(val, "no"))
                c->tolls = CFG_TOLLS_AVOID;
            else if (!strcmp(val, "allow") || !strcmp(val, "yes"))
                c->tolls = CFG_TOLLS_ALLOW;
            else
                complain(path, lineno, "tolls must be avoid or allow, not",
                         val);
        } else if (!strcmp(key, "place")) {
            /* `place = HOME 13.8851,100.3785`. Repeated, and accumulating
             * rather than overwriting -- the one key in this file where a
             * second line adds instead of replacing, because a list of
             * favourites is what it is for.
             *
             * Never fatal, like everything else here: a place that will not
             * parse warns with its line number and is dropped, and the rest of
             * the list still loads. A rider with a typo in WORK still gets
             * HOME. */
            char nm[CFG_PLACE_NAME];
            double la, lo;
            /* From the RIGHT, so `MY OFFICE 13.7,100.5` works and 1.4.8's own
             * `PLACE 1` default does not silently become a place in the
             * Atlantic. place_split() cuts the name off in place and hands back
             * the coordinate; see it for what the first version of this did. */
            char *sp = place_split(val);
            size_t nl = sp ? strlen(val) : 0;
            if (!sp || !nl) {
                complain(path, lineno, "wants NAME LAT,LON, not", val);
            } else if (nl >= (size_t)CFG_PLACE_NAME) {
                complain(path, lineno, "place name is too long", val);
            } else if (c->nplace >= CFG_PLACES_MAX) {
                complain(path, lineno, "too many places, ignoring", val);
            } else {
                memcpy(nm, val, nl);
                nm[nl] = '\0';
                if (sscanf(sp, "%lf , %lf", &la, &lo) != 2 &&
                    sscanf(sp, "%lf %lf", &la, &lo) != 2) {
                    complain(path, lineno, "is not LAT,LON", sp);
                } else if (la < -90.0 || la > 90.0 || lo < -180.0 ||
                           lo > 180.0) {
                    /* A swapped pair is the mistake this catches: 100,13 in
                     * Thailand is a latitude that does not exist, and without
                     * this it would silently become a place in the ocean. */
                    complain(path, lineno, "is not a lat,lon on Earth", sp);
                } else {
                    size_t i;
                    for (i = 0; i < nl; i++)
                        nm[i] = (char)toupper((unsigned char)nm[i]);
                    snprintf(c->place[c->nplace].name, CFG_PLACE_NAME, "%s",
                             nm);
                    c->place[c->nplace].lat = la;
                    c->place[c->nplace].lon = lo;
                    c->nplace++;
                }
            }
        } else if (!strcmp(key, "reroute")) {
            lower(val);
            if (!strcmp(val, "off") || !strcmp(val, "no"))
                c->reroute = REROUTE_OFF;
            else if (!strcmp(val, "ask"))
                c->reroute = REROUTE_ASK;
            else if (!strcmp(val, "auto"))
                c->reroute = REROUTE_AUTO;
            else
                complain(path, lineno,
                         "reroute must be ask, auto or off, not", val);
        } else if (!strcmp(key, "router_type")) {
            if (strcmp(val, "valhalla") && strcmp(val, "osrm"))
                complain(path, lineno,
                         "router_type must be valhalla or osrm, not", val);
            else
                snprintf(c->router_type, sizeof c->router_type, "%s", val);
        } else if (!strcmp(key, "router_url") ||
                   !strcmp(key, "fetch_cmd")) {
            char *dst = !strcmp(key, "fetch_cmd") ? c->fetch_cmd
                                                  : c->router_url;
            if (strlen(val) >= (size_t)CFG_PATH_MAX)
                complain(path, lineno, "is too long", key);
            else
                snprintf(dst, CFG_PATH_MAX, "%s", val);
        } else if (!strcmp(key, "routes_dir") || !strcmp(key, "rides_dir") ||
                   !strcmp(key, "basemap") || !strcmp(key, "roads")) {
            /* Four paths, one rule. Not lowercased: on the device's f2fs and
             * on a Mac's APFS a path's case is its own business. */
            char *dst = !strcmp(key, "rides_dir")  ? c->rides_dir
                        : !strcmp(key, "basemap")  ? c->basemap
                        : !strcmp(key, "roads")    ? c->roads
                                                   : c->routes_dir;
            if (!*val)
                complain(path, lineno, "is empty", key);
            else if (strlen(val) >= (size_t)CFG_PATH_MAX)
                complain(path, lineno, "is too long", key);
            else
                snprintf(dst, CFG_PATH_MAX, "%s", val);
        } else if (!strcmp(key, "north_up") || !strcmp(key, "rate_5hz") ||
                   !strcmp(key, "led_alerts")) {
            lower(val);
            b = parse_flag(val);
            if (b < 0) {
                complain(path, lineno, "not 0 or 1:", val);
            } else if (!strcmp(key, "north_up")) {
                c->north_up = b;
            } else if (!strcmp(key, "rate_5hz")) {
                c->rate_5hz = b;
            } else {
                c->led_alerts = b;
            }
        } else {
            complain(path, lineno, "unknown key", key);
        }
    }
    fclose(f);
    return 0;
}

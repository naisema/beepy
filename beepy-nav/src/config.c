/* beepy-nav/src/config.c -- see config.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "route.h" /* UNITS_METRIC / UNITS_IMPERIAL */

void
cfg_defaults(navcfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->units = UNITS_METRIC;
    c->north_up = 0;   /* course-up: DESIGN.md 1.1 */
    c->rate_5hz = 0;   /* 6.3 calls it "a refinement rather than a dependency" */
    c->led_alerts = 1; /* 7.5; led.c already copes with an unwritable LED */
    c->routes_dir[0] = '\0';
}

void
cfg_default_path(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/.config/beepy-nav.conf",
             home && *home ? home : "/home/beepy");
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
        } else if (!strcmp(key, "routes_dir")) {
            if (!*val)
                complain(path, lineno, "routes_dir is empty", NULL);
            else if (strlen(val) >= sizeof c->routes_dir)
                complain(path, lineno, "routes_dir is too long", NULL);
            else
                snprintf(c->routes_dir, sizeof c->routes_dir, "%s", val);
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

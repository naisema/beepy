/* beepy-nav/src/fix.h -- the live position, DESIGN.md 8.
 *
 * libnmea owns gps_t, which carries everything a signal monitor wants:
 * satellite arrays, per-constellation staging, GSA PRN lists. beepy-nav
 * consumes almost none of that -- the two pages need a position, a course, a
 * speed and enough of the fix state to know when to stop trusting them. So
 * fix_t is derived from gps_t rather than replacing it, and the adapter is
 * the one place that knows about both.
 *
 * Portable: gps.h is pure C, and nothing here touches the serial port.
 */
#ifndef BEEPY_NAV_FIX_H
#define BEEPY_NAV_FIX_H

#include <time.h>

#include "libnmea/gps.h"

typedef struct {
    double lat, lon;
    double speed_kmh, course; /* course over ground, degrees true */
    int fix, sats;            /* GGA quality, GGA satellites used */
    double hdop;
    char utc[9]; /* "HH:MM:SS" */
    time_t last_fix;
} fix_t;

void fix_init(fix_t *f);

/* Fill f from the current gps_t. Returns 1 when the epoch carries a usable
 * position -- quality above zero and finite coordinates -- and 0 otherwise,
 * in which case f keeps the last good values and last_fix is not advanced.
 * The pages need that distinction: a stale position drawn as if it were
 * current is the one thing a navigator must never do. */
int fix_from_gps(const gps_t *g, time_t now, fix_t *f);

/* "HH:MM:SS" -> seconds since midnight, or -1 when it is not a time. Used to
 * pace a replay against the clock the receiver was running on. */
double fix_utc_seconds(const char *utc);

#endif /* BEEPY_NAV_FIX_H */

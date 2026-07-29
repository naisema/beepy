/* beepy-nav/src/fix.c -- see fix.h. */
#include <ctype.h>
#include <math.h>
#include <string.h>

#include "fix.h"

void
fix_init(fix_t *f)
{
    memset(f, 0, sizeof *f);
    f->lat = f->lon = 0.0;
    f->speed_kmh = 0.0;
    f->course = 0.0;
    f->hdop = 0.0;
}

int
fix_from_gps(const gps_t *g, time_t now, fix_t *f)
{
    int usable = g->quality > 0 && !isnan(g->lat) && !isnan(g->lon);

    /* Counts and quality are copied either way: "NO FIX" on the panel is
     * exactly the case where the receiver state is the interesting part. */
    f->fix = g->quality;
    f->sats = g->sats_used;
    f->hdop = isnan(g->hdop) ? 0.0 : g->hdop;
    memcpy(f->utc, g->utc, sizeof f->utc);
    f->utc[sizeof f->utc - 1] = '\0';
    if (!usable)
        return 0;

    f->lat = g->lat;
    f->lon = g->lon;
    /* VTG is preferred upstream and already merged into these two fields;
     * an empty VTG leaves RMC's knots*1.852 in place (libnmea/nmea.c). NaN
     * means neither sentence supplied it, and zero is the honest stand-in --
     * a NaN course would poison the heading EWMA for the rest of the ride. */
    f->speed_kmh = isnan(g->speed_kmh) ? 0.0 : g->speed_kmh;
    f->course = isnan(g->course_deg) ? f->course : g->course_deg;
    f->last_fix = now;
    return 1;
}

double
fix_utc_seconds(const char *utc)
{
    int h, m, s;
    if (!utc || strlen(utc) < 8)
        return -1.0;
    if (!isdigit((unsigned char)utc[0]) || utc[2] != ':' || utc[5] != ':')
        return -1.0;
    h = (utc[0] - '0') * 10 + (utc[1] - '0');
    m = (utc[3] - '0') * 10 + (utc[4] - '0');
    s = (utc[6] - '0') * 10 + (utc[7] - '0');
    return h * 3600.0 + m * 60.0 + s;
}

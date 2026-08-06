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

/* Days from 1970-01-01 to a proleptic-Gregorian y-m-d, by the standard
 * shift-the-era method: move March to the start of the year so the leap day
 * lands at the end, and count whole 400-year eras. No loops, no tables, no
 * timezone, and correct either side of 2100 -- where "every fourth year" is
 * wrong and a hand-rolled leap rule usually is too. */
static long
days_from_civil(int y, int m, int d)
{
    long era;
    unsigned yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = (unsigned)(y - era * 400);              /* year of era, 0..399   */
    doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  /* day of era, 0..146096 */
    return era * 146097L + (long)doe - 719468L;
}

time_t
fix_utc_epoch(const char *date, const char *utc)
{
    int y, mo, d;
    double secs = fix_utc_seconds(utc);

    if (secs < 0.0 || !date || strlen(date) < 10)
        return 0;
    if (date[4] != '-' || date[7] != '-')
        return 0;
    if (!isdigit((unsigned char)date[0]) || !isdigit((unsigned char)date[5]) ||
        !isdigit((unsigned char)date[8]))
        return 0;
    y = (date[0] - '0') * 1000 + (date[1] - '0') * 100 + (date[2] - '0') * 10 +
        (date[3] - '0');
    mo = (date[5] - '0') * 10 + (date[6] - '0');
    d = (date[8] - '0') * 10 + (date[9] - '0');
    /* Refused rather than clamped: a receiver mid-lock can emit a partial or
     * absurd RMC, and an ETA computed from month 0 would be a confident time in
     * the wrong year. 0 means "no clock", and the caller falls back. */
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || y < 1970 || y > 2999)
        return 0;
    return (time_t)(days_from_civil(y, mo, d) * 86400L + (long)secs);
}

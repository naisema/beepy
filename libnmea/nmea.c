/* libnmea/nmea.c -- NMEA 0183 sentence framing and parsing into gps_t.
 *
 * Split out of gps-monitor.c (M1).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nmea.h"

int csum_ok(const char *l, size_t n)
{
    unsigned char x = 0;
    size_t i = 1;
    if (n < 4 || l[0] != '$')
        return 0;
    for (; i < n && l[i] != '*'; i++)
        x ^= (unsigned char)l[i];
    if (i + 2 >= n + 1 || l[i] != '*')
        return 0;
    char hex[3] = {l[i + 1], l[i + 2], 0};
    return (unsigned)strtoul(hex, NULL, 16) == x;
}

int split(char *l, char *f[], int maxf)
{
    int n = 0;
    char *p = l;
    char *star = strchr(l, '*');
    if (star)
        *star = 0;
    f[n++] = p;
    while ((p = strchr(p, ',')) && n < maxf) {
        *p++ = 0;
        f[n++] = p;
    }
    return n;
}

/* NMEA leaves fields empty rather than zero, so absent must not read as 0. */
double fnum(char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || !*f[i])
        return NAN;
    return atof(f[i]);
}

int fint(char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || !*f[i])
        return -1;
    return atoi(f[i]);
}

/* ddmm.mmmm / dddmm.mmmm -> signed degrees. */
double coord(char *f[], int nf, int vi, int hi)
{
    double v = fnum(f, nf, vi);
    if (isnan(v))
        return NAN;
    double deg = floor(v / 100.0);
    double d = deg + (v - deg * 100.0) / 60.0;
    if (hi < nf && f[hi] && (*f[hi] == 'S' || *f[hi] == 'W'))
        d = -d;
    return d;
}

void set_utc(gps_t *g, char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || strlen(f[i]) < 6)
        return;
    snprintf(g->utc, sizeof g->utc, "%c%c:%c%c:%c%c",
             f[i][0], f[i][1], f[i][2], f[i][3], f[i][4], f[i][5]);
}

void set_date(gps_t *g, char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || strlen(f[i]) < 6)
        return;
    snprintf(g->date, sizeof g->date, "20%c%c-%c%c-%c%c",
             f[i][4], f[i][5], f[i][2], f[i][3], f[i][0], f[i][1]);
}

void nmea_apply(gps_t *g, char *f[], int nf)
{
    const char *tag = f[0];
    char sys;
    if (nf < 2 || strlen(tag) < 6)
        return;
    switch (tag[2]) {
    case 'P': sys = 'G'; break;
    case 'L': sys = 'R'; break;
    case 'A': sys = 'E'; break;
    case 'B': sys = 'B'; break;
    case 'N': sys = 'N'; break; /* combined */
    default:  sys = '?'; break;
    }
    const char *type = tag + 3;

    if (!strncmp(type, "GGA", 3)) {
        set_utc(g, f, nf, 1);
        g->lat = coord(f, nf, 2, 3);
        g->lon = coord(f, nf, 4, 5);
        g->quality = fint(f, nf, 6);
        g->sats_used = fint(f, nf, 7);
        g->hdop = fnum(f, nf, 8);
        g->alt_m = fnum(f, nf, 9);
    } else if (!strncmp(type, "RMC", 3)) {
        set_utc(g, f, nf, 1);
        if (nf > 3) {
            double la = coord(f, nf, 3, 4), lo = coord(f, nf, 5, 6);
            if (!isnan(la))
                g->lat = la;
            if (!isnan(lo))
                g->lon = lo;
        }
        double kn = fnum(f, nf, 7);
        g->speed_kmh = isnan(kn) ? NAN : kn * 1.852;
        g->course_deg = fnum(f, nf, 8);
        set_date(g, f, nf, 9);
    } else if (!strncmp(type, "GSA", 3)) {
        int si = sys_idx(sys == 'N' ? '?' : sys);
        int m = fint(f, nf, 2);
        if (m > 0)
            g->mode = m;
        g->gsa_n[si] = 0;
        for (int i = 3; i <= 14 && i < nf; i++) {
            int prn = fint(f, nf, i);
            if (prn > 0 && g->gsa_n[si] < 12)
                g->gsa_prn[si][g->gsa_n[si]++] = prn;
        }
        double p = fnum(f, nf, 15), h = fnum(f, nf, 16), v = fnum(f, nf, 17);
        if (!isnan(p)) g->pdop = p;
        if (!isnan(h)) g->hdop = h;
        if (!isnan(v)) g->vdop = v;
        gps_apply_used(g);
    } else if (!strncmp(type, "GSV", 3)) {
        int total = fint(f, nf, 1), msg = fint(f, nf, 2);
        char psys = (sys == 'N' || sys == '?') ? 0 : sys;
        int si;
        if (total < 1 || msg < 1)
            return;
        /* A GN-talker GSV has no constellation of its own; attribute per PRN. */
        si = sys_idx(psys ? psys : 'G');
        if (msg == 1)
            g->stage[si].n = 0;
        for (int i = 4; i + 3 <= nf && i + 3 <= 20; i += 4) {
            int prn = fint(f, nf, i);
            if (prn <= 0)
                continue;
            sat_t s;
            s.sys = psys ? psys : sys_from_prn(prn);
            s.prn = prn;
            s.elev = fint(f, nf, i + 1);
            s.azim = fint(f, nf, i + 2);
            s.snr = fint(f, nf, i + 3);
            s.used = 0;
            if (g->stage[si].n < MAX_SATS)
                g->stage[si].s[g->stage[si].n++] = s;
        }
        if (msg == total)
            gps_publish(g, psys ? psys : 'G');
    } else {
        g->unknown++;
    }
}

void nmea_line(gps_t *g, char *l, size_t n)
{
    char *f[32];
    g->lines++;
    if (!csum_ok(l, n)) {
        g->bad_crc++;
        return;
    }
    int nf = split(l, f, 32);
    nmea_apply(g, f, nf);
    g->last_data = time(NULL);
}

void nmea_feed(nmea_rx_t *rx, gps_t *g, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            if (rx->len && !rx->overflow) {
                rx->buf[rx->len] = 0;
                nmea_line(g, rx->buf, rx->len);
            }
            rx->len = 0;
            rx->overflow = 0;
            continue;
        }
        if (rx->len + 1 >= sizeof rx->buf) {
            /* Discard to the next terminator rather than truncate into a
             * sentence that would parse as something else. */
            rx->overflow = 1;
            rx->len = 0;
            continue;
        }
        rx->buf[rx->len++] = c;
    }
}

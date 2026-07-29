/* libnmea/gps.c -- GNSS receiver state: fix, DOP, satellite sets.
 *
 * Split out of gps-monitor.c (M1).
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gps.h"

void gps_init(gps_t *g)
{
    memset(g, 0, sizeof *g);
    g->lat = g->lon = NAN;
    g->alt_m = g->speed_kmh = g->course_deg = NAN;
    g->hdop = g->pdop = g->vdop = NAN;
    strcpy(g->utc, "--:--:--");
    strcpy(g->date, "----------");
}

int sys_idx(char s)
{
    switch (s) {
    case 'G': return 0;
    case 'R': return 1;
    case 'E': return 2;
    case 'B': return 3;
    default:  return 4;
    }
}

/* GPS 1-32, SBAS 33-64, GLONASS 65-96 -- used to attribute GN-talker PRNs. */
char sys_from_prn(int prn)
{
    if (prn >= 65 && prn <= 96)
        return 'R';
    if (prn >= 1 && prn <= 64)
        return 'G';
    return '?';
}

void gps_apply_used(gps_t *g)
{
    for (int i = 0; i < g->live.n; i++)
        g->live.s[i].used = 0;
    for (int si = 0; si < NSYS; si++)
        for (int k = 0; k < g->gsa_n[si]; k++)
            for (int i = 0; i < g->live.n; i++)
                if (g->live.s[i].prn == g->gsa_prn[si][k] &&
                    (si == 4 || sys_idx(g->live.s[i].sys) == si))
                    g->live.s[i].used = 1;
}

/* Replace every satellite of one constellation with the freshly staged cycle. */
void gps_publish(gps_t *g, char sys)
{
    satset_t out;
    int si = sys_idx(sys);
    out.n = 0;
    for (int i = 0; i < g->live.n; i++)
        if (g->live.s[i].sys != sys && out.n < MAX_SATS)
            out.s[out.n++] = g->live.s[i];
    for (int i = 0; i < g->stage[si].n && out.n < MAX_SATS; i++)
        out.s[out.n++] = g->stage[si].s[i];
    g->live = out;
    gps_apply_used(g);
}

int gps_order(const gps_t *g, int by_snr, int *idx, int max)
{
    int n = g->live.n < max ? g->live.n : max;
    for (int i = 0; i < n; i++)
        idx[i] = i;
    for (int i = 1; i < n; i++) { /* insertion sort, n is tiny */
        int k = idx[i], j = i - 1;
        while (j >= 0) {
            int a = idx[j], worse;
            if (by_snr)
                worse = g->live.s[a].snr < g->live.s[k].snr;
            else
                worse = g->live.s[a].prn > g->live.s[k].prn;
            if (!worse)
                break;
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = k;
    }
    return n;
}

int gps_count(const gps_t *g, char sys, int *used)
{
    int n = 0;
    *used = 0;
    for (int i = 0; i < g->live.n; i++)
        if (g->live.s[i].sys == sys) {
            n++;
            if (g->live.s[i].used)
                (*used)++;
        }
    return n;
}

/* ------------------------------------------------------------------ demo */

void load_demo(gps_t *g)
{
    static const struct { char s; int prn, el, az, snr, used; } d[] = {
        {'G', 2, 67, 210, 44, 1}, {'G', 5, 54, 118, 41, 1},
        {'G', 7, 41, 302, 38, 1}, {'G', 13, 33, 95, 35, 1},
        {'G', 21, 28, 201, 33, 1}, {'G', 19, 36, 240, 31, 1},
        {'G', 24, 22, 155, 29, 0}, {'R', 66, 61, 340, 28, 1},
        {'R', 73, 44, 55, 26, 1},  {'R', 75, 19, 28, 22, 1},
        {'G', 30, 12, 266, 19, 0}, {'R', 81, 15, 70, 15, 0},
        {'R', 84, 9, 300, 11, 0},  {'G', 11, 8, 190, 8, 0},
    };
    g->live.n = 0;
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        sat_t s = {d[i].s, d[i].prn, d[i].el, d[i].az, d[i].snr, d[i].used};
        g->live.s[g->live.n++] = s;
    }
    g->mode = 3;
    g->quality = 1;
    g->sats_used = 9;
    g->hdop = 0.9;
    g->pdop = 1.4;
    g->vdop = 1.1;
    g->lat = 13.75632;
    g->lon = 100.50184;
    g->alt_m = 18.4;
    g->speed_kmh = 0.31;
    g->course_deg = 142.3;
    strcpy(g->utc, "12:34:56");
    strcpy(g->date, "2026-07-29");
    g->connected = 1;
    g->last_data = time(NULL);
}

void print_state(const gps_t *g)
{
    printf("mode=%d quality=%d used=%d inview=%d\n", g->mode, g->quality,
           g->sats_used, g->live.n);
    printf("lat=%.6f lon=%.6f alt=%.1f spd=%.2f crs=%.1f\n", g->lat, g->lon,
           g->alt_m, g->speed_kmh, g->course_deg);
    printf("dop p=%.1f h=%.1f v=%.1f  utc=%s date=%s\n", g->pdop, g->hdop,
           g->vdop, g->utc, g->date);
    printf("lines=%lu bad_crc=%lu unknown=%lu\n", g->lines, g->bad_crc, g->unknown);
    for (int i = 0; i < g->live.n; i++) {
        const sat_t *s = &g->live.s[i];
        printf("  %c%02d el=%3d az=%3d snr=%3d %s\n", s->sys, s->prn, s->elev,
               s->azim, s->snr, s->used ? "USED" : "-");
    }
}

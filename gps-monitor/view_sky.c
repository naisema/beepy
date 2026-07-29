/* gps-monitor/view_sky.c -- polar sky view page.
 *
 * Split out of gps-monitor.c (M1). Includes the occupancy-grid label
 * placement and the elevation/azimuth -> screen projection.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gpsmon.h"

#define OCC_CELL 4
#define OCC_W (SCR_W / OCC_CELL)
#define OCC_H (SCR_H / OCC_CELL)

static unsigned char g_occ[OCC_W * OCC_H];

static void occ_reset(void) { memset(g_occ, 0, sizeof g_occ); }

static void occ_seed(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j += OCC_CELL)
        for (int i = x; i < x + w; i += OCC_CELL) {
            int cx = i / OCC_CELL, cy = j / OCC_CELL;
            if (cx >= 0 && cy >= 0 && cx < OCC_W && cy < OCC_H)
                g_occ[cy * OCC_W + cx] = 1;
        }
}

static int occ_claim(int x, int y, int w, int h, int bx0, int by0, int bx1, int by1)
{
    if (x < bx0 || y < by0 || x + w > bx1 || y + h > by1)
        return 0;
    for (int j = y; j < y + h; j += OCC_CELL)
        for (int i = x; i < x + w; i += OCC_CELL) {
            int cx = i / OCC_CELL, cy = j / OCC_CELL;
            if (cx < 0 || cy < 0 || cx >= OCC_W || cy >= OCC_H)
                return 0;
            if (g_occ[cy * OCC_W + cx])
                return 0;
        }
    occ_seed(x, y, w, h);
    return 1;
}

static void sky_pos(int cx, int cy, int r, int el, int az, int *x, int *y)
{
    double k = (1.0 - (el < 0 ? 0 : el) / 90.0) * r;
    double a = az * M_PI / 180.0;
    *x = cx + (int)lround(k * sin(a));
    *y = cy - (int)lround(k * cos(a));
}

void view_sky(canvas_t *c, const gps_t *g, const view_t *v)
{
    int idx[MAX_SATS];
    int n = gps_order(g, 1, idx, MAX_SATS);
    int sel = sel_index(g, v, idx, n);

    int colw = 10 * CW + 4;
    int skx1 = c->w - colw;
    int y0 = STATUS_H + 2;
    int cx = skx1 / 2, cy = (y0 + c->h) / 2;
    int pad = CW + 6;
    int r = ((skx1 < c->h - y0 ? skx1 : c->h - y0) / 2) - pad;
    int mr = 6;

    status_bar(c, g, "SKY");
    vline(c, skx1 - 2, y0, c->h - y0, INK);

    if (v->grid < 2) {
        dots_circle(c, cx, cy, r, 9);
        if (v->grid == 0) {
            dots_circle(c, cx, cy, r * 2 / 3, 9);
            dots_circle(c, cx, cy, r / 3, 9);
        }
    }
    hline(c, cx - 4, cy, 9, INK);
    vline(c, cx, cy - 4, 9, INK);

    occ_reset();
    occ_seed(skx1 - 4, y0, colw + 4, c->h - y0);
    occ_seed(0, 0, c->w, STATUS_H + 2);

    struct { const char *t; int x, y; } card[4] = {
        {"N", cx - CW / 2, cy - r - CH},
        {"S", cx - CW / 2, cy + r + 2},
        {"W", cx - r - CW - 4, cy - CH / 2},
        {"E", cx + r + 4, cy - CH / 2},
    };
    for (int i = 0; i < 4; i++) {
        draw_text(c, card[i].x, card[i].y, card[i].t, SC, INK);
        occ_seed(card[i].x, card[i].y, CW, CH);
    }

    /* A satellite being acquired reports empty elevation/azimuth. Plotting that
     * as 0/0 would place it on the horizon due north, which is a plausible
     * looking lie -- so it is omitted from the plot and counted instead. */
    int sx[MAX_SATS], sy[MAX_SATS], haspos[MAX_SATS], nopos = 0;
    for (int i = 0; i < n; i++) {
        const sat_t *s = &g->live.s[idx[i]];
        haspos[i] = (s->elev >= 0 && s->azim >= 0);
        if (!haspos[i]) {
            nopos++;
            sx[i] = sy[i] = -1000;
            continue;
        }
        sky_pos(cx, cy, r, s->elev, s->azim, &sx[i], &sy[i]);
        occ_seed(sx[i] - mr - 1, sy[i] - mr - 1, 2 * mr + 3, 2 * mr + 3);
    }

    int hidden = 0;
    for (int k = -1; k < n; k++) {
        int i = (k < 0) ? sel : k;            /* selected first, then by SNR */
        if (i < 0 || (k >= 0 && i == sel))
            continue;
        const sat_t *s = &g->live.s[idx[i]];
        int x = sx[i], y = sy[i];
        if (!haspos[i])
            continue;

        if (s->snr <= 0)
            fillrect(c, x - 1, y - 1, 3, 3, INK);
        else if (s->used)
            disc(c, x, y, mr, INK);
        else {
            disc(c, x, y, mr, PAPER);
            circle(c, x, y, mr, INK);
        }
        if (i == sel)
            circle(c, x, y, mr + 3, INK);

        char full[8], num[8];
        snprintf(full, sizeof full, "%c%02d", s->sys, s->prn % 100);
        snprintf(num, sizeof num, "%02d", s->prn % 100);
        const char *tags[2] = {full, num};
        int placed = 0;
        for (int t = 0; t < 2 && !placed; t++) {
            int tw = text_w(tags[t], SC), gap = mr + 2;
            int cand[4][2] = {{x + gap, y - CH / 2},
                              {x - tw - gap, y - CH / 2},
                              {x - tw / 2, y - gap - CH},
                              {x - tw / 2, y + gap}};
            for (int a = 0; a < 4; a++)
                if (occ_claim(cand[a][0], cand[a][1], tw, CH, 0, y0, skx1 - 4, c->h)) {
                    draw_text(c, cand[a][0], cand[a][1], tags[t], SC, INK);
                    placed = 1;
                    break;
                }
        }
        if (!placed)
            hidden++;
    }

    /* Stats column: content rows first, blank spacers dropped when short. */
    char rows[16][32];
    int nr = 0, spacer[16];
    long age = g->last_data ? (long)(time(NULL) - g->last_data) : -1;
    int gu, gn, ru, rn;

#define ROW(sp, ...) do { if (nr < 16) { snprintf(rows[nr], 32, __VA_ARGS__); \
                          spacer[nr] = (sp); nr++; } } while (0)
    ROW(0, "SELECTED");
    if (sel >= 0) {
        const sat_t *s = &g->live.s[idx[sel]];
        ROW(0, " %c%02d %dDB", s->sys, s->prn % 100, s->snr < 0 ? 0 : s->snr);
        ROW(0, "EL%d AZ%d", s->elev < 0 ? 0 : s->elev, s->azim < 0 ? 0 : s->azim);
        ROW(0, " %s", s->used ? "IN FIX" : "UNUSED");
    } else {
        ROW(0, " NONE");
    }
    ROW(1, "%s", "");
    if (!isnan(g->pdop)) ROW(0, "DOP P%.1f", g->pdop);
    if (!isnan(g->hdop)) ROW(0, " H%.1f", g->hdop);
    if (!isnan(g->vdop)) ROW(0, " V%.1f", g->vdop);
    ROW(1, "%s", "");
    gn = gps_count(g, 'G', &gu);
    rn = gps_count(g, 'R', &ru);
    if (gn) ROW(0, "GPS %d/%d", gu, gn);
    if (rn) ROW(0, "GLO %d/%d", ru, rn);
    ROW(1, "%s", "");
    if (nopos)
        ROW(0, "%d NO POS", nopos);
    if (hidden)
        ROW(0, "+%dHID", hidden);
    ROW(0, "AGE %lds", age < 0 ? 0 : age);
    ROW(0, "1=BARS");
#undef ROW

    int maxr = (c->h - (y0 + 2)) / CH;
    while (nr > maxr) {
        int cut = -1;
        for (int i = nr - 1; i >= 0; i--)
            if (spacer[i]) { cut = i; break; }
        if (cut < 0)
            break;
        for (int i = cut; i < nr - 1; i++) {
            memcpy(rows[i], rows[i + 1], 32);
            spacer[i] = spacer[i + 1];
        }
        nr--;
    }
    for (int i = 0; i < nr && i < maxr; i++)
        draw_text(c, skx1 + 2, y0 + 2 + i * CH, rows[i], SC, INK);
}

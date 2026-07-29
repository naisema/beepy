/* gps-monitor/view_bars.c -- vertical SNR bargraph page (+ shared view bits).
 *
 * Split out of gps-monitor.c (M1).
 *
 * Text is drawn at scale 2 (12x16 cells); that is why bar labels stack "02"
 * over "G" instead of writing "G02", which would need 36px in a 26px slot.
 */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <time.h>

#include "gpsmon.h"

/* ------------------------------------------------------------- view state */

int sel_index(const gps_t *g, const view_t *v, const int *idx, int n)
{
    for (int i = 0; i < n; i++)
        if (g->live.s[idx[i]].sys == v->sel_sys && g->live.s[idx[i]].prn == v->sel_prn)
            return i;
    return n ? 0 : -1;
}

static int bar_px(int snr, int span)
{
    if (snr <= SNR_MIN)
        return 0;
    if (snr >= SNR_MAX)
        return span;
    int h = (int)lround((double)(snr - SNR_MIN) / (SNR_MAX - SNR_MIN) * span);
    return h < 2 ? 2 : h; /* anything above the floor must be visible */
}

static void fill_bar(canvas_t *c, const view_t *v, int x, int y, int w, int h, int used)
{
    if (used) {
        if (v->ascii) {
            rect(c, x, y, w, h, INK);
            for (int j = y + 1; j < y + h - 1; j += 2)
                hline(c, x + 1, j, w - 2, INK);
        } else {
            fillrect(c, x, y, w, h, INK);
        }
    } else {
        rect(c, x, y, w, h, INK);
        checker(c, x + 1, y + 1, w - 2, h - 2);
    }
}

void status_bar(canvas_t *c, const gps_t *g, const char *page)
{
    char s[64];
    const char *mode = g->mode == 3 ? "3D" : g->mode == 2 ? "2D" : "NO";
    int inview = g->live.n;
    fillrect(c, 0, 0, c->w, STATUS_H, INK);
    if (isnan(g->hdop))
        snprintf(s, sizeof s, "%s %s %d/%dSAT", page, mode,
                 g->sats_used < 0 ? 0 : g->sats_used, inview);
    else
        snprintf(s, sizeof s, "%s %s %d/%dSAT H%.1f", page, mode,
                 g->sats_used < 0 ? 0 : g->sats_used, inview, g->hdop);
    draw_text(c, 3, 1, s, SC, PAPER);
    draw_text(c, c->w - text_w(g->utc, SC) - 3, 1, g->utc, SC, PAPER);
}

/* --------------------------------------------------------------- bars page */

void view_bars(canvas_t *c, const gps_t *g, const view_t *v)
{
    int idx[MAX_SATS];
    int n = gps_order(g, v->by_snr, idx, MAX_SATS);
    int sel = sel_index(g, v, idx, n);

    int foot_y = c->h - 2 * CH;
    int base_y = foot_y - 4 - 2 * CH;   /* two stacked label rows */
    int top_y = STATUS_H + 2 + CH;      /* room for the SNR value above a bar */
    int span = base_y - top_y;
    int ax_x = text_w("50", SC) + 2;

    status_bar(c, g, "BARS");

    for (int db = SNR_MAX; db >= SNR_MIN; db -= 10) {
        char lb[8];
        int y = base_y - bar_px(db, span);
        snprintf(lb, sizeof lb, "%d", db);
        draw_text(c, 0, y - CH / 2, lb, SC, INK);
        for (int x = ax_x + 2; x < c->w; x += 6)
            px(c, x, y, INK);
    }
    vline(c, ax_x, top_y, span + 1, INK);
    hline(c, ax_x, base_y, c->w - ax_x, INK);

    /* A slot only as wide as its label leaves the labels touching, so cap the
     * number of bars at what can be labelled with a readable gap. gps_order()
     * sorted by SNR, so the ones dropped are the weakest, and the footer says
     * how many. */
    int avail = c->w - (ax_x + 6);
    int need = text_w("00", SC) + 6;
    int shown = n;
    if (n > 0 && avail / n < need) {
        shown = avail / need;
        if (shown < 1)
            shown = 1;
    }
    int slot = shown > 0 ? avail / shown : avail;
    if (slot > 34)
        slot = 34;
    int bw = slot - 6;
    if (bw < 3)
        bw = 3;

    for (int i = 0; i < shown; i++) {
        const sat_t *s = &g->live.s[idx[i]];
        int bx = ax_x + 6 + i * slot;
        int cx = bx + bw / 2;
        char lb[16]; /* [8] trips gcc10's -Wformat-truncation on "%d", snr */
        if (bx + bw > c->w)
            break;

        int h = bar_px(s->snr, span);
        if (h == 0) {
            /* Tracked but at/below the noise floor: keep the slot and labels,
             * draw a baseline stub so it is visibly present. */
            hline(c, bx, base_y - 1, bw, INK);
        } else {
            fill_bar(c, v, bx, base_y - h, bw, h, s->used);
        }
        if (s->snr > 0) {
            int ny = base_y - h - CH;
            if (ny < STATUS_H + 1)
                ny = STATUS_H + 1;
            snprintf(lb, sizeof lb, "%d", s->snr);
            draw_ctext(c, cx, ny, lb, SC, INK);
        }
        if (i == sel)
            rect(c, bx - 3, base_y - h - 2, bw + 6, h + 4, INK);

        snprintf(lb, sizeof lb, "%02d", s->prn % 100);
        draw_ctext(c, cx, base_y + 3, lb, SC, INK);
        lb[0] = s->sys;
        lb[1] = 0;
        draw_ctext(c, cx, base_y + 3 + CH, lb, SC, INK);
    }

    hline(c, 0, foot_y - 2, c->w, INK);

    char l1[64], l2[80];
    if (isnan(g->lat) || isnan(g->lon))
        snprintf(l1, sizeof l1, "NO FIX  %lu LINES CRC%lu", g->lines, g->bad_crc);
    else if (isnan(g->alt_m))
        snprintf(l1, sizeof l1, "%.5f%c %.5f%c", fabs(g->lat), g->lat < 0 ? 'S' : 'N',
                 fabs(g->lon), g->lon < 0 ? 'W' : 'E');
    else
        snprintf(l1, sizeof l1, "%.5f%c %.5f%c ALT%.0fM", fabs(g->lat),
                 g->lat < 0 ? 'S' : 'N', fabs(g->lon), g->lon < 0 ? 'W' : 'E',
                 g->alt_m);
    draw_text(c, 2, foot_y, l1, SC, INK);

    long age = g->last_data ? (long)(time(NULL) - g->last_data) : -1;
    if (sel >= 0) {
        const sat_t *s = &g->live.s[idx[sel]];
        char more[16] = "";
        if (shown < n)
            snprintf(more, sizeof more, " +%d", n - shown);
        snprintf(l2, sizeof l2, "%c%02d EL%d AZ%d AGE%lds%s 2=SKY", s->sys,
                 s->prn % 100, s->elev < 0 ? 0 : s->elev, s->azim < 0 ? 0 : s->azim,
                 age < 0 ? 0 : age, more);
    } else {
        snprintf(l2, sizeof l2, "%s AGE%lds", g->connected ? "NO SATS" : "NO DEVICE",
                 age < 0 ? 0 : age);
    }
    draw_text(c, 2, foot_y + CH, l2, SC, INK);
}

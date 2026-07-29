/* gps-monitor/gpsmon.h -- app-internal: view state and page renderers. */
#ifndef GPSMON_H
#define GPSMON_H

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/font.h"
#include "libnmea/gps.h"

#define SC 2                    /* body text scale */
#define CW (CELL_W * SC)        /* 12 */
#define CH (CELL_H * SC)        /* 16 */

#define STATUS_H (CH + 2)

typedef enum { PAGE_BARS = 0, PAGE_SKY = 1 } page_t;

typedef struct {
    page_t page;
    int by_snr;
    char sel_sys;
    int sel_prn;
    int hold, grid, ascii;
} view_t;

int sel_index(const gps_t *g, const view_t *v, const int *idx, int n);
void status_bar(canvas_t *c, const gps_t *g, const char *page);
void view_bars(canvas_t *c, const gps_t *g, const view_t *v);
void view_sky(canvas_t *c, const gps_t *g, const view_t *v);

#endif /* GPSMON_H */

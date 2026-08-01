/* beepy-vid/src/view_pages.h -- the pages that are not the film.
 *
 * LIBRARY, END and HELP. Geometry from beepy-vid/DESIGN.md 6.4, and it closes
 * to the pixel: 24 title + 7 rows x 22 + 36 detail + 26 footer = 240 exactly,
 * (240 - 24 - 36 - 26) / 22 = 7.0 with no remainder. The constants match
 * beepy-nav/src/chooser.c so the two list pages look like the same program.
 *
 * Every string here is upper case and avoids ! " # $ & ' ; @ -- font.c is
 * ASCII 32..90 and glyph() maps anything outside to a SPACE, so a stray
 * exclamation mark does not draw a box, it silently vanishes.
 */
#ifndef BEEPYVID_VIEW_PAGES_H
#define BEEPYVID_VIEW_PAGES_H

#include "libbeepyfb/canvas.h"

#define LIB_TITLE_H 24
#define LIB_ROW_H 22
#define LIB_DETAIL_H 36
#define LIB_FOOT_H 26
#define LIB_ROWS ((SCR_H - LIB_TITLE_H - LIB_DETAIL_H - LIB_FOOT_H) / LIB_ROW_H)

#define LIB_NAME_MAX 64

typedef struct {
    char name[LIB_NAME_MAX]; /* upper-cased, extension stripped */
    double seconds;
    int fps_num, fps_den;
    int dither;      /* 0 bayer8, 1 bayer4 */
    long bytes;
    int resume_pct;  /* -1 when never opened: zero and never-opened differ */
    int readable;
} lib_item_t;

typedef struct {
    const char *dir;
    lib_item_t *items;
    int n, sel, top;
} lib_t;

/* The picker. With n == 0 it draws the empty state, which names the directory
 * it searched -- chooser.c:116-125's argument: a page with nothing to say and
 * a frozen one look the same, so it has to say something. */
void view_library(canvas_t *c, const lib_t *l);

/* Finished. This page exists because a memory LCD holds its last image with
 * no power, so a player that simply stops on the final frame is
 * byte-identical to one that segfaulted -- and the fbterm underneath may or
 * may not have been resumed. */
void view_end(canvas_t *c, const char *title, double seconds, int idx, int of);

/* The calibration screen. A2DP always makes audio LAG -- the sink buffers
 * 150-250 ms before playing -- and that is the fortunate direction, because
 * tolerance is asymmetric: audio arriving late is what distance does in the
 * physical world. So the correction delays the VIDEO, and negative offsets
 * are refused rather than offered.
 *
 * The instrument is a flash and a click: once a second a disc fills for
 * exactly one frame while a click is queued at the same presentation time,
 * and the viewer nudges the offset until they coincide. On one bit a one-frame
 * all-or-nothing change is the sharpest temporal event the panel can make.
 *
 * reported is what the sink claims, shown so a viewer can see whether their
 * trim is a small correction or a large one; it is a lower bound and not the
 * answer (audio.h explains why).
 */
void view_sync(canvas_t *c, const char *sink, int offset_ms, int reported_ms,
               int flash);

/* The whole keymap. beepy-nav gets away with a three-key hint row; this app
 * has sixteen bindings and no spare row over video. */
void view_help(canvas_t *c);

#endif /* BEEPYVID_VIEW_PAGES_H */

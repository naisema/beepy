/* beepy-vid/src/view_play.h -- the playback status band.
 *
 * beepy-vid/DESIGN.md 2 cuts the panel once, at y = 225, into a 400x225 stage
 * and a 400x15 band. 400 * 9/16 is exactly 225.0, so fitting 16:9 at full
 * width leaves precisely 15 rows and the band costs no video pixels at all.
 *
 * The band is ink ground with paper text, not the reverse: a permanent white
 * bar under a mostly dark film is a light source you look past for ninety
 * minutes.
 *
 * There is no overlay ON the stage in M4, and on one bit there never will be
 * an inverted one. A Bayer midtone is 50% ink by construction, so inverting a
 * rectangle of it yields the same local mean and the same apparent tone -- the
 * dither phase shifts and nothing else. A 5x7 glyph is four dither cells wide
 * and the eye cannot resolve a phase shift over four cells as a letter. Solid
 * fill is the only legible operation, which is why the band is reserved rather
 * than drawn over the picture (DESIGN.md 6.1).
 *
 * view_play draws into a canvas_t with canvas.h and font.h primitives and
 * never through cov_t: cov_resolve() (cover.c:1203) memsets every destination
 * row before thresholding, which would erase the decoded video underneath.
 * Every other page in this project composes a whole frame from nothing; this
 * one composes over a bitmap it did not draw.
 */
#ifndef BEEPYVID_VIEW_PLAY_H
#define BEEPYVID_VIEW_PLAY_H

#include "libbeepyfb/canvas.h"

#define STAGE_H 225 /* 400 * 9/16, exactly */
#define BAND_Y0 STAGE_H
#define BAND_H (SCR_H - BAND_Y0) /* 15 */

/* The transient grows UPWARD from the band into the bottom 40 stage rows, so
 * band and transient read as one thing swelling rather than as a second
 * object appearing. Scale 3, because it answers "what did I just press" from
 * across the room, where 18x24 makes a word a shape rather than a string.
 * 40 of 225 rows is 17.8% of the stage, for 2.5 s. */
#define TRANSIENT_H 40
#define TRANSIENT_Y0 (BAND_Y0 - TRANSIENT_H)

/* The paused panel is free in a way the transient is not: a paused frame
 * conveys no motion, so covering 28% of it costs nothing. That is what
 * removes a mode -- an earlier draft had a separate pinned-OSD state, and
 * pausing already produces exactly the condition where a large persistent
 * panel is free. 1+3+16+3+12+3+16+2+8 = 64, closing exactly. */
#define PAUSED_H 64
#define PAUSED_Y0 (BAND_Y0 - PAUSED_H)

typedef struct {
    const char *title;
    double t, total; /* seconds */
    int paused;
    int ended;
    int waiting; /* starved: the audio clock has nothing to give */
    int nopack;  /* no pack open -- say so rather than show a blank stage */
    const char *transient; /* NULL, or scale-3 text over the bottom 40 rows */
} osd_t;

/* Draw the band over rows BAND_Y0..239. Leaves the stage untouched. */
void view_play_band(canvas_t *c, const osd_t *o);

/* The whole page: matte the stage where the pack's image rect does not reach,
 * then the band. The caller has already put the decoded frame in the canvas. */
void view_play(canvas_t *c, const osd_t *o);

#endif /* BEEPYVID_VIEW_PLAY_H */

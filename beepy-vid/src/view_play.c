/* beepy-vid/src/view_play.c -- see view_play.h. */
#include <stdio.h>
#include <string.h>

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/font.h"

#include "view_play.h"

/* Band interior, to the pixel:
 *   y225      1  paper rule, full width -- reads against any video pixel above
 *   y226..233 8  scale-1 text row, paper on ink
 *   y234..239 6  progress bar
 * 1 + 8 + 6 = 15. Nothing is left over.
 */
#define RULE_Y BAND_Y0
#define TEXT_Y (BAND_Y0 + 1)
#define BAR_Y (BAND_Y0 + 9)
#define BAR_H 6
#define BAR_X0 6
#define BAR_X1 (SCR_W - 7) /* inclusive; 6 px margin each side */

static void
clock_str(char *out, size_t n, double s)
{
    long v = (long)(s < 0 ? 0 : s);
    long h = v / 3600, m = (v % 3600) / 60, sec = v % 60;
    if (h > 0)
        snprintf(out, n, "%ld:%02ld:%02ld", h, m, sec);
    else
        snprintf(out, n, "%ld:%02ld", m, sec);
}

void
view_play_band(canvas_t *c, const osd_t *o)
{
    char left[48], el[16], tot[16];
    const char *state;
    int right_w;
    char right[24];

    fillrect(c, 0, BAND_Y0, SCR_W, BAND_H, INK);
    hline(c, 0, RULE_Y, SCR_W, PAPER);

    /* One character of state, because the band is a glance target and a word
     * costs 4-7 of the 64 columns a scale-1 row has. Every glyph here exists
     * in FONT[] (font.c is ASCII 32..90). */
    if (o->nopack)
        state = "-";
    else if (o->ended)
        state = "E";
    else if (o->waiting)
        state = "W";
    else if (o->paused)
        state = "=";
    else
        state = ">";

    clock_str(el, sizeof el, o->t);
    clock_str(tot, sizeof tot, o->total);
    if (o->nopack)
        snprintf(left, sizeof left, "- NO PACK");
    else
        snprintf(left, sizeof left, "%s %s / %s", state, el, tot);
    draw_text(c, BAR_X0, TEXT_Y, left, 1, PAPER);

    /* The title sits at the right end and is clipped, not scrolled: a moving
     * string in a permanent band is the sort of thing you stop seeing and
     * start being annoyed by. */
    if (o->title && *o->title) {
        size_t max = 18;
        snprintf(right, sizeof right, "%.*s", (int)max, o->title);
        right_w = text_w(right, 1);
        if (BAR_X0 + text_w(left, 1) + 12 < SCR_W - 6 - right_w)
            draw_text(c, SCR_W - 6 - right_w, TEXT_Y, right, 1, PAPER);
    }

    /* Progress bar. 1 px paper outline, interior filled to the elapsed
     * fraction. No playhead marker: on one bit the boundary between the fill
     * and the trough IS the playhead, at 1 px precision, and a marker would
     * have to be drawn in the only other value available -- a notch inside the
     * fill, which reads as damage. */
    rect(c, BAR_X0, BAR_Y, BAR_X1 - BAR_X0 + 1, BAR_H, PAPER);
    if (o->total > 0 && !o->nopack) {
        int inner = (BAR_X1 - BAR_X0 + 1) - 2;
        double f = o->t / o->total;
        int w;
        if (f < 0)
            f = 0;
        if (f > 1)
            f = 1;
        w = (int)(inner * f + 0.5);
        if (w > 0)
            fillrect(c, BAR_X0 + 1, BAR_Y + 1, w, BAR_H - 2, PAPER);
    }
}

void
view_play(canvas_t *c, const osd_t *o)
{
    if (o->nopack) {
        /* An empty stage and a crashed player look identical on a memory LCD,
         * which holds its last image with no power. Say which this is. */
        fillrect(c, 0, 0, SCR_W, STAGE_H, INK);
        draw_ctext(c, SCR_W / 2, 96, "NO PACK", 3, PAPER);
        draw_ctext(c, SCR_W / 2, 132, "BEEPY-VID FILM.VID", 2, PAPER);
    }
    view_play_band(c, o);
}

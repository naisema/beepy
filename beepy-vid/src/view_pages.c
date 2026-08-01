/* beepy-vid/src/view_pages.c -- see view_pages.h. */
#include <stdio.h>
#include <string.h>

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/font.h"

#include "view_pages.h"

static void
hms(char *out, size_t n, double s)
{
    long v = (long)(s < 0 ? 0 : s);
    if (v >= 3600)
        snprintf(out, n, "%ldH%02ldM", v / 3600, (v % 3600) / 60);
    else if (v >= 60)
        snprintf(out, n, "%ldMIN", v / 60);
    else
        snprintf(out, n, "%ldSEC", v);
}

/* An inverted strip with paper text, the convention every page in this
 * project uses for a title or a footer (chooser.c:108). */
static void
strip(canvas_t *c, int y, int h, const char *left, const char *right, int scale)
{
    fillrect(c, 0, y, SCR_W, h, INK);
    if (left)
        draw_text(c, 6, y + (h - GLYPH_H * scale) / 2, left, scale, PAPER);
    if (right)
        draw_text(c, SCR_W - 6 - text_w(right, scale),
                  y + (h - GLYPH_H * scale) / 2, right, scale, PAPER);
}

void
view_library(canvas_t *c, const lib_t *l)
{
    char buf[80], b2[40];
    int y;

    canvas_clear(c, PAPER);

    if (l->n <= 0) {
        strip(c, 0, LIB_TITLE_H, "VIDEOS", "NONE", 2);
        draw_text(c, 6, 44, "NO .VID PACKS IN", 2, INK);
        /* Scale 1 for the path, and this is the one departure from the
         * scale-2 floor: 33 characters of scale 2 will not hold a path, and
         * this line is read once, standing still. chooser.c:120 makes the
         * same exception for the same reason. */
        draw_text(c, 6, 68, l->dir ? l->dir : "(NO DIRECTORY)", 1, INK);
        draw_text(c, 6, 92, "MAKE ONE ON THE MAC WITH", 2, INK);
        draw_text(c, 6, 114, "TOOLS/MKVID.PY FILM.MP4", 2, INK);
        strip(c, SCR_H - LIB_FOOT_H, LIB_FOOT_H, "Q QUIT", NULL, 2);
        return;
    }

    snprintf(buf, sizeof buf, "%d/%d", l->sel + 1, l->n);
    strip(c, 0, LIB_TITLE_H, "VIDEOS", buf, 2);

    y = LIB_TITLE_H;
    for (int i = 0; i < LIB_ROWS; i++) {
        int idx = l->top + i;
        int sel = (idx == l->sel);
        if (idx >= l->n)
            break;
        if (sel)
            fillrect(c, 0, y, SCR_W, LIB_ROW_H, INK);
        draw_text(c, 6, y + 3, l->items[idx].name, 2, sel ? PAPER : INK);
        if (!l->items[idx].readable)
            draw_text(c, SCR_W - 6 - text_w("BAD", 2), y + 3, "BAD", 2,
                      sel ? PAPER : INK);
        y += LIB_ROW_H;
    }

    /* The detail strip costs one list row and earns it. Two of its fields
     * cannot be recovered any other way and both decide whether pressing
     * ENTER will work: the transcode settings, because dithering discards
     * tone irreversibly so "this looks bad" is nearly always a re-transcode
     * and not a player bug; and the resume position, where zero and
     * never-opened are different facts and are shown differently. */
    {
        const lib_item_t *it = &l->items[l->sel];
        int dy = SCR_H - LIB_FOOT_H - LIB_DETAIL_H;
        fillrect(c, 0, dy, SCR_W, LIB_DETAIL_H, INK);
        hms(b2, sizeof b2, it->seconds);
        snprintf(buf, sizeof buf, "%s  %dFPS  %s", b2,
                 it->fps_den ? (it->fps_num + it->fps_den / 2) / it->fps_den : 0,
                 it->dither == 1 ? "BAYER4" : "BAYER8");
        draw_text(c, 6, dy + 2, buf, 2, PAPER);
        if (it->resume_pct >= 0)
            snprintf(buf, sizeof buf, "%ldMB  RESUME %d%%",
                     it->bytes / 1000000, it->resume_pct);
        else
            snprintf(buf, sizeof buf, "%ldMB", it->bytes / 1000000);
        draw_text(c, 6, dy + 19, buf, 2, PAPER);
    }

    strip(c, SCR_H - LIB_FOOT_H, LIB_FOOT_H,
          "N/P MOVE  ENTER PLAY  Q QUIT", NULL, 2);
}

void
view_end(canvas_t *c, const char *title, double seconds, int idx, int of)
{
    char buf[80], b2[40];

    canvas_clear(c, PAPER);
    strip(c, 0, LIB_TITLE_H, "END", title, 2);

    hms(b2, sizeof b2, seconds);
    draw_ctext(c, SCR_W / 2, 72, b2, 3, INK);
    snprintf(buf, sizeof buf, "%d OF %d IN THIS FOLDER", idx, of);
    draw_ctext(c, SCR_W / 2, 116, buf, 2, INK);

    strip(c, SCR_H - LIB_FOOT_H, LIB_FOOT_H,
          "ENTER REPLAY  N NEXT  ESC LIST", NULL, 2);
}

void
view_sync(canvas_t *c, const char *sink, int offset_ms, int reported_ms,
          int flash)
{
    char buf[80];

    canvas_clear(c, PAPER);
    strip(c, 0, LIB_TITLE_H, "AUDIO SYNC", sink, 2);

    /* A localised disc rather than a full-screen invert, so the number stays
     * readable while the calibration runs. */
    if (flash)
        disc(c, SCR_W / 2, 82, 30, INK);
    else
        circle(c, SCR_W / 2, 82, 30, INK);

    snprintf(buf, sizeof buf, "%d MS", offset_ms);
    draw_ctext(c, SCR_W / 2, 124, buf, 3, INK);
    if (reported_ms >= 0)
        snprintf(buf, sizeof buf, "SINK REPORTS %d MS", reported_ms);
    else
        snprintf(buf, sizeof buf, "SINK REPORTS NOTHING");
    draw_ctext(c, SCR_W / 2, 158, buf, 2, INK);
    draw_ctext(c, SCR_W / 2, 178, "SAVED FOR THIS DEVICE", 2, INK);

    strip(c, SCR_H - LIB_FOOT_H, LIB_FOOT_H,
          "ARROWS TRIM  ENTER SAVE  Q BACK", NULL, 2);
}

void
view_help(canvas_t *c)
{
    static const char *const K[][2] = {
        { "SPACE", "PLAY / PAUSE" },
        { "LEFT RIGHT", "SEEK 10 SEC" },
        { "DOWN UP", "SEEK 60 SEC" },
        { "Z X", "FRAME STEP, PAUSED" },
        { "N P", "NEXT / PREV PACK" },
        { "F", "FIT OR FILL" },
        { "I", "PACK INFO" },
        { "ESC", "LIBRARY" },
        { "Q", "QUIT" },
    };
    int y = LIB_TITLE_H + 4;

    canvas_clear(c, PAPER);
    strip(c, 0, LIB_TITLE_H, "KEYS", NULL, 2);
    for (unsigned i = 0; i < sizeof K / sizeof K[0]; i++) {
        draw_text(c, 6, y, K[i][0], 2, INK);
        draw_text(c, 150, y, K[i][1], 2, INK);
        y += 19;
    }
    strip(c, SCR_H - LIB_FOOT_H, LIB_FOOT_H, "ANY KEY BACK", NULL, 2);
}

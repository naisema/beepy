/* beepy-nav/src/view_save.c -- the SAVE page (DESIGN.md 1.4.8).
 *
 * A name being typed, over the coordinate it is about to be written against.
 * §1.4.6 gave the rider eight favourites and no way to make one; this is the
 * page that makes one, and `S` on the MAP page is the only thing that opens it.
 *
 * WHY A PAGE AND NOT A KEYPRESS, when view_quit.c argued the other way round for
 * Q. There the page existed to state a LOSS, and only a page could carry the
 * three facts that decided it. Here it exists to carry a NAME -- and a name is
 * the whole value of a favourite. HOME is worth eight keys, PLACE 3 is worth
 * none, and a save with no page can only ever produce the second. The field
 * opens pre-filled, so the rider who does not care still pays only two presses.
 *
 * EVERY CONSTANT IS BORROWED, and that is the design rather than laziness:
 *
 *   FIND's field   the name sits at FIND's query y, in FIND's 24 px table,
 *                  behind FIND's 12x22 block cursor, out of FIND's alphabet.
 *                  Nothing about typing on this device is learned twice.
 *   QUIT's frame   which is CONFIRM's -- a 26 px inverted title bar, content,
 *                  and a 42 px inverted strip carrying ENTER and the way out in
 *                  the same two corners a rider has already answered twice.
 *
 * So this page made no layout decisions of its own, and the only thing it can
 * get wrong is a string -- which is exactly why it is a golden and a design-gate
 * page rather than a behavioural test alone.
 */
#include <stdio.h>

#include "draw.h"
#include "seg.h"
#include "view.h"

#define W SCR_W
#define H SCR_H

/* CONFIRM's frame, by way of view_quit.c. */
#define SV_TITLE 26
#define SV_STRIP 42

/* view_find.c's query field, constant for constant: the cap-top, the cursor's
 * 14 px offset from the field's x, and its 12x22 block. Copied and not shared
 * for the reason view_quit.c gives about fmt_total() -- each page is a
 * transcription of its own mockup frame, and a shared helper would make one
 * page's cursor the other page's problem. They agree because they are the same
 * four numbers. */
#define SV_QY 34
#define SV_CUR_X 14
#define SV_CUR_W 12
#define SV_CUR_Y0 36
#define SV_CUR_Y1 58

/* The coordinate, at scale 3 and not the strip's scale 2: it is the SUBJECT of
 * this page rather than a status row, and 18 characters at scale 3 is 324 px of
 * the 400. Left-aligned with the field above it, because a centred line under a
 * left-aligned one reads as two unrelated things.
 *
 * At FIND's own first-row y, which puts it 16 px under the field -- the two
 * pages then agree on where content starts, and the first render of this page
 * proved why it matters: 96 left a hole in the middle of the screen and the
 * coordinate read as unrelated to the name above it. A form is top-anchored,
 * and the air belongs above the strip where there is nothing to say. */
#define SV_POS_Y 74
#define SV_LEFT 8

static void
rtext(cov_t *c, int x_right, int y, const char *s, int scale, int ink)
{
    cov_text(c, x_right - cov_text_w(s, scale), y, s, scale, ink);
}

void
view_save(cov_t *c, const save_t *s)
{
    char buf[64];
    const char *nm = s->name ? s->name : "";
    int qw;

    cov_fill_rect(c, 0, 0, W - 1, SV_TITLE - 1, COV_INK);
    cov_text(c, 6, 5, "SAVE PLACE", 2, COV_PAPER);
    /* How full the list is, where FIND puts its hit count. The CEILING is worth
     * the width because the eighth save is the one that runs into it: a rider
     * who has watched the number climb is not surprised by 8 PLACES MAX, and one
     * who has not would read the refusal as a fault. */
    snprintf(buf, sizeof buf, "%d OF %d", s->nplace, s->max);
    rtext(c, W - 6, 5, buf, 2, COV_PAPER);

    /* The name, in the 24 px table of DESIGN.md 5.2's mechanism -- the same call
     * view_find.c makes for its query, so the two fields are the same field.
     *
     * TWO STATES, and the difference is the point of the field:
     *
     *   fresh   the untouched default, drawn as a SELECTION: a bar of ink with
     *           the name in paper, and NO cursor. The first character typed
     *           takes the whole field, and inverted-means-replaceable is the one
     *           convention a 1-bit panel has for saying so in advance.
     *   edited  ordinary text with the 12x22 block cursor after it, which is
     *           FIND's field exactly.
     *
     * The bar is sized to the glyphs' own advance and not to a fixed width, so a
     * one-character name is not a banner. */
    qw = num_advance(nm, NUM_QUERY24);
    if (qw < 0)
        qw = 0;
    if (s->fresh) {
        cov_fill_rect(c, SV_LEFT - 4, SV_CUR_Y0 - 4, SV_LEFT + qw + 4,
                      SV_CUR_Y1 + 4, COV_INK);
        num_draw(c, SV_LEFT, SV_QY, nm, NUM_QUERY24, NUM_LT, COV_PAPER);
    } else {
        num_draw(c, SV_LEFT, SV_QY, nm, NUM_QUERY24, NUM_LT, COV_INK);
        cov_fill_rect(c, SV_CUR_X + qw, SV_CUR_Y0, SV_CUR_X + qw + SV_CUR_W,
                      SV_CUR_Y1, COV_INK);
    }

    /* Where. The SAME five decimals and the same single space the MAP strip uses
     * (view_map.c), because this is the row the rider just read on that strip
     * and a page that reformatted it would be asking them to check two numbers
     * against each other. Five decimals is 1.1 m of latitude, and it is also
     * exactly what cfg_place_add() writes to the file -- screen, page and file
     * all say one thing. */
    snprintf(buf, sizeof buf, "%.5f %.5f", s->lat, s->lon);
    cov_text(c, SV_LEFT, SV_POS_Y, buf, 3, COV_INK);

    cov_fill_rect(c, 0, H - SV_STRIP, W - 1, H - 1, COV_INK);
    /* QUIT's two corners and QUIT's phrasing: ENTER goes through with it on
     * every page in this program that asks anything, and the cancel sits right.
     * Esc and not Q, because Q types a Q here -- the whole surface is a text
     * field, which is view_find.c's argument for owning all twenty-six letters. */
    cov_text(c, 6, H - SV_STRIP + 5, "ENTER = SAVE", 2, COV_PAPER);
    rtext(c, W - 6, H - SV_STRIP + 5, "ESC = CANCEL", 2, COV_PAPER);
    /* The second row: the instruction, or the refusal that displaced it.
     *
     * A refusal belongs HERE and not on a transient over the MAP page, and the
     * reason is that both of them are answered by editing the field: NAME IN USE
     * wants a different name and NOT SAVED wants another try, so the page has to
     * still be up when the rider reads why. A transient over a page they had
     * already been returned to would tell them the truth in a place where they
     * could no longer act on it.
     *
     * BS = CANCEL is not advertised, and that is deliberate: backspace on an
     * empty field backing out is a SAFETY NET for the rider whose Esc needs the
     * symbol layer (1.4.4), not a second documented way out. Two cancels on one
     * strip would spend the row that is carrying the refusals. */
    if (s->note && *s->note)
        snprintf(buf, sizeof buf, "%s", s->note);
    else
        snprintf(buf, sizeof buf, "TYPE A NAME");
    cov_text(c, 6, H - SV_STRIP + 23, buf, 2, COV_PAPER);
}

/* ------------------------------------------------------------ the demo */

/* The device's own desk, which is view_map.c's MAP_DEMO_LAT/LON to the digit --
 * so the coordinate on this golden is the coordinate on nav-map.fb, and a build
 * that formatted one of the two differently could not pass both frames. It is
 * not shared as a constant for the reason each view file gives about its own
 * fixtures: exporting one would make a demo state part of the program's
 * interface, and mockup.py holds the third copy that the design gate compares
 * all of them against.
 *
 * TWO STATES, because the field has two and neither golden can stand for the
 * other -- view_map.c's argument for its own pair of waiting frames:
 *
 *   opened   the default, SELECTED: an ink bar, the name in paper, no cursor.
 *            This is the frame every rider meets, and the inversion is the whole
 *            promise that typing will not append to it.
 *   typed    a name over it, with the block cursor -- and the cursor's position
 *            after four glyphs is the one thing on this page that is computed
 *            rather than placed, so it is worth freezing on its own.
 *
 * "PLACE 4" and not "PLACE 1", with three places used: that is what a rider who
 * had saved PLACE 1 through 3 would actually be offered, so the field and the
 * count on the title bar tell one story rather than two. */
#define SV_DEMO_DEFAULT "PLACE 4"
#define SV_DEMO_NAME "CAFE"
#define SV_DEMO_LAT 13.88510
#define SV_DEMO_LON 100.37850
/* Three of eight used, so the count is a real fraction rather than 0 OF 8 or
 * 8 OF 8 -- both of which are the boundary cases, and neither of which shows
 * that the two numbers come from different places. */
#define SV_DEMO_NPLACE 3

void
view_save_demo(cov_t *c, int typed)
{
    save_t s;

    s.name = typed ? SV_DEMO_NAME : SV_DEMO_DEFAULT;
    s.fresh = !typed;
    s.lat = SV_DEMO_LAT;
    s.lon = SV_DEMO_LON;
    s.nplace = SV_DEMO_NPLACE;
    s.max = SAVED_MARK_MAX;
    /* Explicitly NULL, and this is the field that taught view_find.c's demos to
     * initialise every member of a stack struct: `note` displaces the strip's
     * second row when it is set, so stack garbage here would put a sentence on a
     * frozen frame and move it between runs. */
    s.note = NULL;
    view_save(c, &s);
}

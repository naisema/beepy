/* beepy-nav/src/arrows.h -- the nine cue glyphs.
 *
 * Filled polygons proportioned as fractions of the box, so one routine
 * serves the 76 px panel arrow and the 16 px inline one. A straight-line
 * port of mockup.py's arrow(); see nav-arrows.png for the sheet.
 */
#ifndef BEEPY_NAV_ARROWS_H
#define BEEPY_NAV_ARROWS_H

#include "libbeepyfb/cover.h"

enum {
    ARROW_STRAIGHT = 0,
    ARROW_SLIGHT_LEFT,
    ARROW_LEFT,
    ARROW_SHARP_LEFT,
    ARROW_SLIGHT_RIGHT,
    ARROW_RIGHT,
    ARROW_SHARP_RIGHT,
    ARROW_UTURN,
    ARROW_DEST,
    ARROW_N
};

/* "straight", "slight-left", ... -- mockup.py's ARROWS, same order. */
extern const char *const ARROW_TAGS[ARROW_N];
/* "STR", "SLTL", ... -- the 5x7 captions on the glyph sheet. */
extern const char *const ARROW_CAPTIONS[ARROW_N];

int arrow_kind(const char *tag); /* -1 when unknown */

/* x0,y0 = top-left of the s x s box. */
void arrow_draw(cov_t *c, double x0, double y0, double s, int kind, int ink);

#endif /* BEEPY_NAV_ARROWS_H */

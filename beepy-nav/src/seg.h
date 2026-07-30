/* beepy-nav/src/seg.h -- typeface string drawing over the generated tables.
 *
 * The distance numerals are pre-rendered 1-bit bitmaps (tools/gen_tables.py
 * -> numerals.h, tools/gen_labels.py -> labels.h), blitted with integer pen
 * advances. This is the C half of mockup.py's num(): the same tables, the
 * same advances and the same set selection, so mockup and panel are
 * identical by construction rather than by resemblance (DESIGN.md 5.2).
 *
 * Portable C, libc only. numerals.h/labels.h are included by seg.c alone.
 */
#ifndef BEEPY_NAV_SEG_H
#define BEEPY_NAV_SEG_H

#include "libbeepyfb/cover.h"

/* Glyph sets. Only two digit sizes exist, so a cap between them is a
 * request for the nearer prepared size, not a new rasterization.
 *
 * NUM_QUERY24 is the FIND page's 24 px query (DESIGN.md 1.4): the same
 * mechanism as the digits, extended to A-Z and space because a destination is
 * typed and not measured. It is never selected by cap -- num_set_for_cap()
 * still answers with a DIGIT set, so nothing that asks for a number can be
 * handed letters -- and the one page that wants it names it. */
enum { NUM_54 = 0, NUM_22, UNITS_22, NUM_LABEL, NUM_QUERY24 };

/* Anchor: x is the left edge, the centre, or the right edge of the ink box. */
enum { NUM_LT = 0, NUM_CT, NUM_RT };

/* Below this cap mockup.py's num() falls back to the 5x7 bitmap font; the
 * fit loops stop here rather than shrinking forever. */
#define NUM_MIN_CAP 16

/* NUM_54 at cap >= 54, NUM_22 below -- mockup.py _table_layout(). */
int num_set_for_cap(int cap);

/* The cap a set is actually prepared at: 54, 22, or 22 for the unit words.
 * num_fit() answers with the cap it was ASKED for, which is the number
 * mockup.py stacks the next line at; a caller that needs to know how tall the
 * ink really is -- because the string got demoted and the two no longer agree
 * -- asks here. Returns 0 for NUM_LABEL, whose glyphs each have their own. */
int num_cap(int set);

/* Whole-string LABELS, then whole-string UNITS22, then the digit set the
 * cap selects: mockup.py's lookup order, for callers that have a cap but
 * not a set. Returns -1 when nothing in the tables can set the string. */
int num_lookup_set(const char *s, int cap);

/* Ink width in pixels (pen advances minus the last advance, plus the last
 * glyph's ink width), or -1 when the set cannot render the string. */
int num_width(const char *s, int set);

/* Total PEN advance, which is where the next glyph would start rather than
 * where the ink stops. The two differ by the last glyph's right side bearing,
 * and for a trailing space they differ by a whole space: "SOI " is ink-narrower
 * than "SOI", so a caret placed at the ink edge walks BACKWARDS over the I when
 * a rider types a space. The FIND page's block cursor uses this instead
 * (DESIGN.md 1.4). 0 for an empty string, -1 when a glyph is missing. */
int num_advance(const char *s, int set);

/* Draw at x,y = the ink box's top-left/top-centre/top-right. Coordinates
 * are Python-rounded (half to even) before blitting, because a fractional
 * origin splits every glyph block across two pixels. Returns the width. */
int num_draw(cov_t *c, double x, double y, const char *s, int set, int anchor,
             int ink);

/* mockup.py's `while num_size(...)[0] > maxw: cap -= 2` loop, which is how
 * the panel picks its size -- and it returns the CAP, not just the set,
 * because the caller stacks the unit line at y + cap + n. Only two widths
 * exist, so in practice this answers cap0 or the first cap below 54. */
int num_fit(const char *s, int cap0, int maxw);

/* labels.h whole-word bitmaps (OFF / ROUTE / M AWAY). */
int label_width(const char *s);
int label_draw(cov_t *c, double x, double y, const char *s, int anchor,
               int ink);

#endif /* BEEPY_NAV_SEG_H */

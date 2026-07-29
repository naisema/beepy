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
 * request for the nearer prepared size, not a new rasterization. */
enum { NUM_54 = 0, NUM_22, UNITS_22, NUM_LABEL };

/* Anchor: x is the left edge, the centre, or the right edge of the ink box. */
enum { NUM_LT = 0, NUM_CT, NUM_RT };

/* Below this cap mockup.py's num() falls back to the 5x7 bitmap font; the
 * fit loops stop here rather than shrinking forever. */
#define NUM_MIN_CAP 16

/* NUM_54 at cap >= 54, NUM_22 below -- mockup.py _table_layout(). */
int num_set_for_cap(int cap);

/* Whole-string LABELS, then whole-string UNITS22, then the digit set the
 * cap selects: mockup.py's lookup order, for callers that have a cap but
 * not a set. Returns -1 when nothing in the tables can set the string. */
int num_lookup_set(const char *s, int cap);

/* Ink width in pixels (pen advances minus the last advance, plus the last
 * glyph's ink width), or -1 when the set cannot render the string. */
int num_width(const char *s, int set);

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

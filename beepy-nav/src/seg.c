/* beepy-nav/src/seg.c -- see seg.h.
 *
 * numerals.h and labels.h are included before seg.h on purpose: cover.h
 * only defines cov_blit_glyph() once NUMERALS_H has introduced glyph_t.
 */
#include <math.h>
#include <string.h>

#include "numerals.h"
#include "labels.h"

#include "seg.h"

static const glyph_t *
find(const glyph_t *tab, int n, const char *s, int len)
{
    int i;
    for (i = 0; i < n; i++)
        if ((int)strlen(tab[i].tag) == len && !memcmp(tab[i].tag, s, (size_t)len))
            return &tab[i];
    return NULL;
}

static const glyph_t *
digit_tab(int set, int *n)
{
    if (set == NUM_54) {
        *n = NUM54_N;
        return NUM54;
    }
    *n = NUM22_N;
    return NUM22;
}

int
num_set_for_cap(int cap)
{
    return cap >= NUM54_CAP ? NUM_54 : NUM_22;
}

int
num_lookup_set(const char *s, int cap)
{
    int len = (int)strlen(s);
    if (find(LABELS, LABELS_N, s, len))
        return NUM_LABEL;
    if (find(UNITS22, UNITS22_N, s, len))
        return UNITS_22;
    if (num_width(s, num_set_for_cap(cap)) >= 0)
        return num_set_for_cap(cap);
    return -1;
}

int
num_width(const char *s, int set)
{
    const glyph_t *tab, *g, *last = NULL;
    int n, pen = 0;

    if (!s || !*s)
        return -1;
    if (set == NUM_LABEL) {
        g = find(LABELS, LABELS_N, s, (int)strlen(s));
        return g ? g->w : -1;
    }
    if (set == UNITS_22) {
        g = find(UNITS22, UNITS22_N, s, (int)strlen(s));
        return g ? g->w : -1;
    }
    tab = digit_tab(set, &n);
    for (; *s; s++) {
        g = find(tab, n, s, 1);
        if (!g)
            return -1;
        pen += g->adv;
        last = g;
    }
    return pen - last->adv + last->w;
}

int
num_draw(cov_t *c, double x, double y, const char *s, int set, int anchor,
         int ink)
{
    const glyph_t *tab, *g;
    int n, w = num_width(s, set), bx, by, pen = 0;

    if (w < 0)
        return 0;
    /* Python round(): half to even, which is what rint() does under the
     * default FE_TONEAREST. mockup.py rounds the anchored origin, not the
     * pen, so a centred string can sit half a pixel left of true centre. */
    bx = (int)rint(anchor == NUM_LT ? x
                   : anchor == NUM_CT ? x - w / 2.0
                                      : x - w);
    by = (int)rint(y);

    if (set == NUM_LABEL || set == UNITS_22) {
        g = set == NUM_LABEL ? find(LABELS, LABELS_N, s, (int)strlen(s))
                             : find(UNITS22, UNITS22_N, s, (int)strlen(s));
        cov_blit_glyph(c, bx, by, g, ink);
        return w;
    }
    tab = digit_tab(set, &n);
    for (; *s; s++) {
        g = find(tab, n, s, 1);
        cov_blit_glyph(c, bx + pen, by, g, ink);
        pen += g->adv;
    }
    return w;
}

int
num_fit(const char *s, int cap0, int maxw)
{
    int cap = cap0, w;
    while (cap > NUM_MIN_CAP) {
        w = num_width(s, num_set_for_cap(cap));
        if (w < 0 || w <= maxw)
            break;
        cap -= 2;
    }
    return cap;
}

int
label_width(const char *s)
{
    return num_width(s, NUM_LABEL);
}

int
label_draw(cov_t *c, double x, double y, const char *s, int anchor, int ink)
{
    return num_draw(c, x, y, s, NUM_LABEL, anchor, ink);
}

/* beepy-nav/src/draw.h -- one shim over cover.h's polygon entry point.
 *
 * cov_poly() is the odd one out in cover.h: it takes coordinates already on
 * the 4x subpixel grid, because it mirrors PIL's ImageDraw.polygon() as
 * called through mockup.py's Canvas._s() scaling. Every other primitive
 * takes 1x screen units. Rather than scatter "* 4" through the page code,
 * everything here goes through poly1x().
 */
#ifndef BEEPY_NAV_DRAW_H
#define BEEPY_NAV_DRAW_H

#include "libbeepyfb/cover.h"

#define NAV_SS 4
#define NAV_POLY_MAX 8

static inline void
poly1x(cov_t *c, const double *xy, int npts, int ink)
{
    double s[2 * NAV_POLY_MAX];
    int i;
    if (npts < 2 || npts > NAV_POLY_MAX)
        return;
    for (i = 0; i < 2 * npts; i++)
        s[i] = xy[i] * (double)NAV_SS;
    cov_poly(c, s, npts, ink);
}

#endif /* BEEPY_NAV_DRAW_H */

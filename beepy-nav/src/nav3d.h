/* beepy-nav/src/nav3d.h -- the 3D nav map plane (DESIGN.md 6.6).
 *
 * Draws the road network and the route as ground RIBBONS through persp.c's
 * tilted camera, into the NAV page's map viewport. It draws the map plane and
 * nothing else: the turn panel, compass, speed badge and scale bar are
 * view_nav.c's and are untouched, because the product owner asked for the
 * existing page kept.
 *
 * WHY THE VECTOR GRAPH AND NOT THE RASTER BASEMAP. Sampling pre-rendered tiles
 * in perspective destroys the one property 6.5's blit rests on -- "a straight
 * line stays connected ... never dotted" -- because perspective minifies
 * ANISOTROPICALLY. At 100 m out on a 40 m camera the vertical ground step per
 * screen row is 2.5x the horizontal step per column, so nearest-neighbour
 * sampling of a 1 px road skips rows. Choosing a coarser rung to compensate
 * turns the horizon into a solid bar instead, and there is no rung between
 * those two failures. tools/nav3d_proto.py holds the measurement and the two
 * pictures. A line primitive is connected by construction, so vector it is.
 *
 * IT RENDERS INTO ITS OWN BIT BUFFER and lands with one cov_blit_bits(), which
 * is exactly what tiles_blit() does and for the same reason: streets must not go
 * near the coverage path. mockup.py's Canvas comment records what a 1-2 px
 * street looks like anti-aliased, and the word it uses is "hairy".
 */
#ifndef BEEPY_NAV_NAV3D_H
#define BEEPY_NAV_NAV3D_H

#include "libbeepyfb/cover.h"
#include "roadgrid.h"
#include "search.h"

/* What the page has to hand. `grid` may be NULL, in which case nothing draws --
 * the caller builds it lazily on the first 3D frame and shows a note while it
 * does, because that build is about 950 ms on the device (roadgrid.h). */
typedef struct {
    const roads_t *roads;
    const roadgraph_t *g;
    const roadgrid_t *grid;
} nav3d_ctx_t;

/* Every knob the view has, so the caller holds no magic numbers and the design
 * gate can drive the same values into mockup.py. */
typedef struct {
    double pitch_deg;     /* 70 -- the product owner's choice from NAV3D-GMAP */
    double focal;         /* 120 px                                          */
    double nadir_mpp;     /* metres per pixel at nadir; the 2D ladder's value */
    double route_cap_px;  /* 20 -- the route's widest allowed SCREEN width    */
    double min_seg_px;    /* 3.0 -- shorter than this contributes a dot       */
    double min_w_px;      /* 0.8 -- thinner than a pixel is noise            */
    double chevron_frac;  /* 0.72 down the viewport                          */
} nav3d_cfg_t;

void nav3d_defaults(nav3d_cfg_t *cfg, double nadir_mpp);

/* A rectangle to leave as paper before the caller draws its own overlay there.
 * Without these the compass and badge sit on a dense oblique street grid and
 * are legible only by luck; the 2D view needs none because a raster basemap at
 * 4 m/px is sparser. Kept in the 3D buffer rather than added to view_nav.c so
 * the 2D page -- and every golden of it -- is untouched. */
typedef struct {
    double cx, cy, r;     /* a disc, when r > 0                              */
    double x0, y0, x1, y1;/* otherwise a rectangle                           */
} nav3d_knockout_t;

/* Draw into `c` at viewport (x0, y0) of size (vw, vh).
 *
 * `route_en` is nroute (e, n) pairs in the ROADS pack's frame -- the caller
 * projects them, because it is the one that knows whether a route came from a
 * GPX, the offline router or the network.
 *
 * Returns the number of ribbons drawn, or 0 if there was nothing to draw
 * (no grid, no graph, or a camera that put the whole viewport above the
 * horizon). A 0 return means the caller should say so, not that it should
 * silently show an empty page. */
int nav3d_draw(cov_t *c, const nav3d_ctx_t *ctx, const nav3d_cfg_t *cfg,
               double lat, double lon, double heading_rad,
               const double *route_en, int nroute,
               const nav3d_knockout_t *ko, int nko,
               int x0, int y0, int vw, int vh);

#endif /* BEEPY_NAV_NAV3D_H */

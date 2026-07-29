/* beepy-nav/src/map.h -- map maths: projection, corner rounding, clipping,
 * zoom ladder and scale bar.
 *
 * Pure and portable: no cov_t, no canvas, libc + libm only. Everything here
 * is a straight port of the corresponding function in mockup.py, and it is
 * the piece M3's live map and overview page build on -- so the coordinate
 * conventions are worth stating once:
 *
 *   world  metres east/north of an arbitrary origin, stored as e,n pairs
 *          in a flat double[]. North is +n.
 *   screen pixels, y down, stored as x,y pairs in a flat double[].
 *
 * map_project() rotates the world by +theta, so a course-up map passes
 * theta = heading (NOT -heading; the ride simulation is what settled the
 * sign -- see mockup.py page_nav).
 */
#ifndef BEEPY_NAV_MAP_H
#define BEEPY_NAV_MAP_H

/* The zoom ladder, metres per pixel. */
#define MAP_NZOOM 12
extern const double MAP_ZOOMS[MAP_NZOOM];

/* Defaults for map_round_corners(): 25 m radius, ignore bends under 12 deg. */
#define MAP_CORNER_RADIUS 25.0
#define MAP_CORNER_MIN_DEG 12.0

/* n world points -> n screen points. In-place (out_xy == en) is fine: each
 * point is read whole before it is written. */
void map_project(const double *en, int n, double org_e, double org_n,
                 double mpp, double cx, double cy, double theta,
                 double *out_xy);

/* Replace sharp vertices with quadratic arcs of `radius` metres, in world
 * space so a junction keeps the same real radius at every zoom. A GPX is a
 * chain of straight chords; drawn raw the route reads as a polygon. The
 * radius is clamped to half the shorter adjacent segment. Returns the
 * number of points written, capped at max_out. */
int map_round_corners(const double *en, int n, double radius, double min_deg,
                      double *out, int max_out);

/* Cohen-Sutherland per segment; without it the map paints over the panel.
 * Returns the number of surviving segments, each written as four doubles
 * (x0 y0 x1 y1) -- the form cov_stroke_segs() takes, which also needs the
 * split-detection that a flat point list would lose. */
int map_clip_segs(const double *xy, int npts, double x0, double y0, double x1,
                  double y1, double *out_segs, int max);

/* Coarsest ladder step that still keeps the cue inside 80% of the space
 * ahead of the fix. */
double map_auto_zoom(double cue_dist_m, double ahead_px);

/* 1-2-5 scale bar: the round distance whose bar lands in 50..100 px. */
void map_scale_pick(double mpp, int *out_m, int *out_px);

/* Along-route distance from a position on segment pos_i to vertex turn_i. */
double map_cue_distance(const double *en, int npts, double pe, double pn,
                        int pos_i, int turn_i);

#endif /* BEEPY_NAV_MAP_H */

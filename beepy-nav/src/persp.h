/* beepy-nav/src/persp.h -- the tilted camera behind the 3D nav view.
 *
 * A pinhole with three knobs: PITCH (90 degrees looks straight down, smaller
 * tilts toward the horizon), FOCAL in pixels, and the metres-per-pixel the view
 * would have at nadir. Heading rotates the ground under it, exactly as the
 * course-up 2D map already does.
 *
 * THE INVARIANT THE DESIGN RESTS ON: at pitch 90 this reduces EXACTLY to the
 * top-down affine the 2D map uses -- a point `d` metres ahead lands d/mpp pixels
 * up, and d metres to the right lands d/mpp pixels right, with no perspective
 * term surviving. So 2D and 3D are one projection with one knob rather than two
 * renderers that would drift apart, and `t_pitch90_is_2d` asserts it rather than
 * trusting it.
 *
 * The first two prototypes (tools/nav3d_proto.py, tools/nav3d_ribbon.py) used a
 * Mode-7 formulation -- horizontal camera axis, horizon on screen -- and could
 * not express the gentle oblique the product owner actually asked for, nor
 * degenerate to 2D at any setting. Their "tilt" knob was really a camera height.
 * That is why this is a real pitched camera and not the cheaper thing.
 *
 * Everything is in the caller's ground frame: metres, x east-ish along the
 * rider's right, y along the rider's heading. persp_ground() and persp_screen()
 * are inverses where both are defined.
 */
#ifndef BEEPY_NAV_PERSP_H
#define BEEPY_NAV_PERSP_H

typedef struct {
    double pitch;        /* radians; M_PI/2 is straight down                */
    double focal;        /* pixels                                          */
    double h;            /* camera height, metres -- derived: nadir_mpp*focal */
    double sp, cp;       /* sin/cos of pitch, computed once                 */
    double cx, cy;       /* the viewport centre, in screen pixels           */
    double back;         /* how far behind the rider the camera sits, metres */
    double fwd_e, fwd_n; /* the heading's unit vector in the pack's frame   */
    double rgt_e, rgt_n; /* and its right-hand normal                       */
    int has_horizon;     /* 0 when pitch is 90 and there is no horizon      */
    double horizon_v;    /* the v (up-from-centre) the horizon sits at      */
} persp_t;

/* Set up a camera. `chevron_frac` places the RIDER at that fraction down the
 * viewport (0.72 puts it where the reference screenshot has it), and `back` is
 * solved for so that holds at every pitch -- otherwise changing the tilt would
 * silently change the zoom too and the two would be impossible to judge apart.
 *
 * `pitch_deg` is clamped to (0, 90]. `vw`/`vh` are the viewport size in pixels,
 * which on the NAV page is 270x240 and not the full screen: PANEL_W takes the
 * left third for the turn panel. */
void persp_init(persp_t *c, double pitch_deg, double focal, double nadir_mpp,
                double heading_rad, double vw, double vh, double chevron_frac);

/* Screen pixel -> ground, in metres relative to the rider: *fwd along the
 * heading, *lat to the rider's right. Returns 0 for a pixel at or above the
 * horizon, where no ground point exists. */
int persp_ground(const persp_t *c, double x, double y, double *fwd,
                 double *lat);

/* Ground -> screen. Also yields *t, the ray parameter, which is what a metre
 * measures in pixels at that point: a ground width w spans w/t pixels. Callers
 * use it for the sub-pixel width cull and for the per-vertex ribbon taper.
 * Returns 0 for a point at or beyond the horizon, or behind the camera. */
int persp_screen(const persp_t *c, double fwd, double lat, double *x,
                 double *y, double *t);

/* The pack-frame point for a ground offset, so callers can go straight from a
 * road node's (e, n) to the camera frame without repeating the rotation. */
void persp_to_ground(const persp_t *c, double de, double dn, double *fwd,
                     double *lat);

#endif /* BEEPY_NAV_PERSP_H */

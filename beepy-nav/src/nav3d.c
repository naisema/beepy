/* beepy-nav/src/nav3d.c -- see nav3d.h. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "nav3d.h"
#include "persp.h"

#define VW_MAX 400
#define VH_MAX 240
#define STRIDE_MAX ((VW_MAX + 7) / 8)

/* The same metres mktiles.py gives the 2D casings (OSM_WIDTH_M), indexed by
 * ROADCLASS_*. Sharing the numbers is the point: the 3D view must not disagree
 * with the 2D basemap about how wide Sukhumvit is, and a second table would
 * drift the first time either was edited. Class 0 is "highway tagged but not
 * classified", which mktiles.py gives OSM_WIDTH_DEFAULT_M. */
static const double CLASS_W_M[ROADCLASS_N] = {
    5.0,   /* 0 unknown        */
    14.0,  /* 1 motorway       */
    12.0,  /* 2 trunk          */
    11.0,  /* 3 primary        */
    9.0,   /* 4 secondary      */
    7.0,   /* 5 tertiary       */
    6.0,   /* 6 unclassified   */
    5.0,   /* 7 residential    */
    4.0    /* 8 living_street  */
};

/* How far out each class is worth drawing at all, metres. A residential lane
 * 2 km away is one row of ink and a thousand of them are a smear; a motorway at
 * 2 km is the thing you are navigating by. This is a coarse first filter -- the
 * sub-pixel width cull below is what actually scales with pitch. */
static const double CLASS_CULL_M[ROADCLASS_N] = {
    900.0, 4000.0, 3000.0, 2000.0, 1400.0, 900.0, 600.0, 450.0, 350.0
};

/* The route's ground width. Wider than a residential street (5 m) so it reads
 * as the road you are on. */
#define ROUTE_W_M 9.0

void
nav3d_defaults(nav3d_cfg_t *cfg, double nadir_mpp)
{
    cfg->pitch_deg = 70.0;
    cfg->focal = 120.0;
    cfg->nadir_mpp = nadir_mpp > 0.0 ? nadir_mpp : 4.0;
    cfg->route_cap_px = 20.0;
    cfg->min_seg_px = 3.0;
    cfg->min_w_px = 0.8;
    cfg->chevron_frac = 0.72;
}

/* ------------------------------------------------------------ the bit buffer */

/* Flat, and the stride is derived from the WIDTH rather than from a maximum,
 * because cov_blit_bits() computes (w+7)/8 for itself. A buffer with a fixed
 * 50-byte pitch handed to a blit expecting 34 reads a different row every
 * scanline -- it does not crash, it draws a sheared map. */
static unsigned char BUF[VH_MAX * STRIDE_MAX];
static int BUF_W, BUF_H, BUF_STRIDE;

static void
buf_begin(int w, int h)
{
    BUF_W = w > VW_MAX ? VW_MAX : w;
    BUF_H = h > VH_MAX ? VH_MAX : h;
    BUF_STRIDE = (BUF_W + 7) / 8;
    memset(BUF, 0, (size_t)BUF_H * (size_t)BUF_STRIDE);
}

static void
px_set(int x, int y, int on)
{
    unsigned char *b;
    if (x < 0 || y < 0 || x >= BUF_W || y >= BUF_H)
        return;
    b = &BUF[(size_t)y * (size_t)BUF_STRIDE + (size_t)(x >> 3)];
    if (on)
        *b |= (unsigned char)(0x80 >> (x & 7));
    else
        *b &= (unsigned char)~(0x80 >> (x & 7));
}

/* Quantise a screen coordinate to 1/256 px before anything rounds it.
 *
 * This is 6.5's fixed-point argument, arrived at the same way and for the same
 * reason. Everything upstream of here is doubles -- sin, cos, a division per
 * vertex -- and a coordinate that lands within an ulp of a .5 boundary rounds
 * one way in clang and the other in Python, which is how the first C-versus-
 * mockup comparison came out five pixels apart: four of them a 2x2 block
 * displaced by exactly one row.
 *
 * At 1/256 px the two implementations would have to disagree by 2e-3 px before
 * the rounding could differ, against the 1e-13 an ulp actually buys. It does
 * not make the renderer more accurate -- it makes it AGREE, which is what a
 * byte-compared golden needs. */
static double
q256(double v)
{
    return floor(v * 256.0 + 0.5) / 256.0;
}

/* Bresenham. Aliased and connected, which is the whole reason this file exists
 * rather than calling cov_stroke(). */
static void
line(double fx0, double fy0, double fx1, double fy1, int on)
{
    int x0 = (int)floor(q256(fx0) + 0.5), y0 = (int)floor(q256(fy0) + 0.5);
    int x1 = (int)floor(q256(fx1) + 0.5), y1 = (int)floor(q256(fy1) + 0.5);
    int dx = abs(x1 - x0), dy = abs(y1 - y0);
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    /* A projected road can land far outside the viewport; without a bound a
     * near-horizon segment would iterate for millions of steps. The clamp is on
     * the ITERATION and not the endpoints, so the visible part still draws. */
    long guard = (long)dx + (long)dy + 4;
    if (guard > 4L * (VW_MAX + VH_MAX))
        guard = 4L * (VW_MAX + VH_MAX);
    for (;;) {
        px_set(x0, y0, on);
        if ((x0 == x1 && y0 == y1) || guard-- <= 0)
            return;
        {
            int e2 = 2 * err;
            if (e2 > -dy) { err -= dy; x0 += sx; }
            if (e2 <  dx) { err += dx; y0 += sy; }
        }
    }
}

/* Scanline fill of a convex quad. A ground rectangle under a projective map is
 * convex, so left/right spans per row are enough -- no edge table needed. */
static void
quad_fill(const double *xy, int on)
{
    double q[8];
    double ymin, ymax;
    int i, y;
    for (i = 0; i < 8; i++)
        q[i] = q256(xy[i]);
    xy = q;
    ymin = ymax = xy[1];
    for (i = 1; i < 4; i++) {
        if (xy[2 * i + 1] < ymin) ymin = xy[2 * i + 1];
        if (xy[2 * i + 1] > ymax) ymax = xy[2 * i + 1];
    }
    if (ymin < 0.0) ymin = 0.0;
    if (ymax > (double)(BUF_H - 1)) ymax = (double)(BUF_H - 1);
    for (y = (int)floor(ymin + 0.5); y <= (int)floor(ymax + 0.5); y++) {
        double yc = (double)y;
        double xl = 1e300, xr = -1e300;
        int hit = 0, x, xa, xb;
        for (i = 0; i < 4; i++) {
            int j = (i + 1) & 3;
            double ya = xy[2 * i + 1], yb = xy[2 * j + 1];
            double xa2 = xy[2 * i], xb2 = xy[2 * j];
            double t, xx;
            if (ya == yb)
                continue;
            if (!((yc >= ya && yc < yb) || (yc >= yb && yc < ya)))
                continue;
            t = (yc - ya) / (yb - ya);
            xx = xa2 + t * (xb2 - xa2);
            if (xx < xl) xl = xx;
            if (xx > xr) xr = xx;
            hit++;
        }
        if (hit < 2)
            continue;
        /* The SPAN ends are quantised too, and not just the quad's corners.
         * xl and xr come out of an interpolation, so quantising the inputs does
         * not bound the output: this is where the last three differing pixels
         * lived -- a vertical run of a fill boundary that C rounded one column
         * further than Python did. */
        xa = (int)floor(q256(xl) + 0.5);
        xb = (int)floor(q256(xr) + 0.5);
        if (xa < 0) xa = 0;
        if (xb > BUF_W - 1) xb = BUF_W - 1;
        for (x = xa; x <= xb; x++)
            px_set(x, y, on);
    }
}

/* ------------------------------------------------------------------ ribbons */

typedef struct {
    double fa, la, fb, lb;   /* ground, relative to the rider */
    double tfar, tnear;
    int cls;
    int na, nb;              /* the edge's own node indices -- the tie-break */
} rib_t;

#define RIB_MAX 8192
static rib_t RIB[RIB_MAX];

/* Far to near, and a TOTAL order.
 *
 * The tie-break is not decoration. qsort is not stable, this project
 * byte-compares goldens between clang and gcc, and two ribbons at equal depth
 * painted in opposite sequence differ by a pixel wherever they overlap.
 *
 * It breaks on the edge's own NODE INDICES and deliberately not on the order it
 * happened to be gathered in. Those indices are a property of the pack, so any
 * two implementations that read the same pack agree on them without agreeing on
 * anything else -- which is what lets mockup.py gather in whatever order suits
 * Python and still paint the identical sequence. Tie-breaking on gather order
 * would have forced the mockup to reimplement roadgrid's ring walk AND its
 * radix sort just to reproduce a draw order, and the design gate would have
 * been testing that reimplementation rather than the renderer. */
static int
rib_cmp(const void *A, const void *B)
{
    const rib_t *a = (const rib_t *)A, *b = (const rib_t *)B;
    if (a->tfar > b->tfar) return -1;
    if (a->tfar < b->tfar) return 1;
    if (a->na != b->na) return a->na < b->na ? -1 : 1;
    if (a->nb != b->nb) return a->nb < b->nb ? -1 : 1;
    return 0;
}

/* One ribbon: a ground quad of width w, filled then edged.
 *
 * The half-width is capped PER VERTEX rather than once per segment. Capping per
 * segment from its nearer end makes consecutive segments clamp to different
 * widths, and the ribbon edge comes out notched -- visible in the first
 * NAV3D-GOOGLE.png. Per vertex, the quad simply tapers. */
/* mode: RIB_EDGE draws the two long edges only (a road too thin to have an
 * interior), RIB_ROAD fills paper and edges it (a road that does), RIB_ROUTE
 * fills solid ink (the guidance line). */
#define RIB_EDGE  0
#define RIB_ROAD  1
#define RIB_ROUTE 2

static int
ribbon(const persp_t *cam, double fa, double la, double fb, double lb,
       double w, double cap_px, int mode)
{
    double df = fb - fa, dl = lb - la;
    double L = sqrt(df * df + dl * dl);
    double pf, pl, q[8];
    double ex[2][2], ey[2][2];
    int e;

    if (!(L > 1e-9))
        return 0;
    pf = -dl / L;
    pl =  df / L;
    for (e = 0; e < 2; e++) {
        double f0 = e ? fb : fa, l0 = e ? lb : la;
        double t, hw = w / 2.0;
        if (!persp_screen(cam, f0, l0, NULL, NULL, &t))
            return 0;
        if (cap_px > 0.0 && hw > cap_px / 2.0 * t)
            hw = cap_px / 2.0 * t;
        if (!persp_screen(cam, f0 + pf * hw, l0 + pl * hw,
                          &ex[e][0], &ey[e][0], NULL))
            return 0;
        if (!persp_screen(cam, f0 - pf * hw, l0 - pl * hw,
                          &ex[e][1], &ey[e][1], NULL))
            return 0;
    }
    q[0] = ex[0][0]; q[1] = ey[0][0];
    q[2] = ex[1][0]; q[3] = ey[1][0];
    q[4] = ex[1][1]; q[5] = ey[1][1];
    q[6] = ex[0][1]; q[7] = ey[0][1];
    if (mode == RIB_ROUTE) {
        quad_fill(q, 1);                  /* solid ink */
        return 1;
    }
    if (mode == RIB_ROAD)
        quad_fill(q, 0);                  /* paper: a near road occludes a far */
    line(q[0], q[1], q[2], q[3], 1);
    line(q[4], q[5], q[6], q[7], 1);
    return 1;
}

/* Every undirected road out of one node that survives the culls, appended to
 * RIB. Returns the new count. Split out of the draw loop so the ring walk above
 * stays readable. */
static int
gather(const nav3d_ctx_t *ctx, const persp_t *cam, double e0, double n0,
       int nd, const nav3d_cfg_t *cfg, int nrib)
{
    unsigned int j;
    double fa, la;

    if (nd < 0 || nd >= ctx->g->nnode)
        return nrib;
    persp_to_ground(cam, ctx->g->en[2 * nd] - e0, ctx->g->en[2 * nd + 1] - n0,
                    &fa, &la);
    for (j = ctx->g->adj[nd]; j < ctx->g->adj[nd + 1] && nrib < RIB_MAX; j++) {
        int to = (int)ctx->g->edge[j].to;
        int cls = (int)ROADEDGE_CLASS(ctx->g->edge[j].flags);
        double fb, lb, w, ta, tb, da, db, d;
        /* Each undirected road once. The pack is directed, so both halves are
         * present and drawing both would double the fill work for no ink. */
        if (to <= nd || to < 0 || to >= ctx->g->nnode)
            continue;
        if (cls < 0 || cls >= ROADCLASS_N)
            cls = 0;
        persp_to_ground(cam, ctx->g->en[2 * to] - e0,
                        ctx->g->en[2 * to + 1] - n0, &fb, &lb);
        da = sqrt(fa * fa + la * la);
        db = sqrt(fb * fb + lb * lb);
        d = da < db ? da : db;
        if (d > CLASS_CULL_M[cls])
            continue;
        /* The gather radius as a CIRCLE, matching mockup.py's collect(). The
         * class cull above already implies it for every class except the widest,
         * which is exactly where the ring square's corners would otherwise
         * smuggle roads in. */
        if (d > CLASS_CULL_M[ROADCLASS_MOTORWAY])
            continue;
        if (!persp_screen(cam, fa, la, NULL, NULL, &ta))
            continue;
        if (!persp_screen(cam, fb, lb, NULL, NULL, &tb))
            continue;
        w = CLASS_W_M[cls];
        /* THE CULL THAT SCALES WITH PITCH. A ribbon thinner than a pixel is
         * noise: measured at pitch 50 this rule alone takes the ink in the top
         * 40 rows from 3009 to 395, which is the smeared horizon going away. A
         * distance table cannot do it -- the top screen row is 905 m out at
         * pitch 75 and 5347 m at pitch 50, so the far field outgrows any fixed
         * number. */
        if (w / (ta > tb ? ta : tb) < cfg->min_w_px)
            continue;
        RIB[nrib].fa = fa; RIB[nrib].la = la;
        RIB[nrib].fb = fb; RIB[nrib].lb = lb;
        RIB[nrib].tfar = ta > tb ? ta : tb;
        RIB[nrib].tnear = ta < tb ? ta : tb;
        RIB[nrib].cls = cls;
        RIB[nrib].na = nd;
        RIB[nrib].nb = to;
        nrib++;
    }
    return nrib;
}

int
nav3d_draw(cov_t *c, const nav3d_ctx_t *ctx, const nav3d_cfg_t *cfg,
           double lat, double lon, double heading_rad,
           const double *route_en, int nroute,
           const nav3d_knockout_t *ko, int nko,
           int x0, int y0, int vw, int vh)
{
    persp_t cam;
    double e0, n0, far_m;
    int nrib = 0, drawn = 0, i;

    if (!c || !ctx || !cfg || !ctx->g || !ctx->grid || !ctx->roads)
        return 0;
    if (vw <= 0 || vh <= 0)
        return 0;

    persp_init(&cam, cfg->pitch_deg, cfg->focal, cfg->nadir_mpp, heading_rad,
               (double)vw, (double)vh, cfg->chevron_frac);
    roads_project(ctx->roads, lat, lon, &e0, &n0);

    /* Cells outward from the rider's own, so a cap drops the FARTHEST material
     * rather than whatever the scan reached last. The first version of this
     * asked roadgrid_nodes() for one 4.2 km square, which in Bangkok is about
     * 42 000 nodes against a 40 000 buffer -- and the truncation was in cell
     * row order, south to north, so it silently discarded a band that could
     * include the ground under the rider. The frames came out sparse for no
     * visible reason. Rings cannot do that: whatever is dropped is farther away
     * than everything kept, which is what the culls remove anyway. */
    far_m = CLASS_CULL_M[ROADCLASS_MOTORWAY];
    {
        /* Rings out to the widest class cull, and the CIRCLE is what defines
         * membership -- the ring walk is only how cells are visited. Without
         * the radial test below, a 17-ring square reaches 8.9 km across its
         * diagonal against a 4 km cull, and the corners would put roads on
         * screen that mockup.py's circle never gathers. */
        int rmax = (int)ceil(far_m / roadgrid_cell_m(ctx->grid)) + 1;
        int cx0, cy0, ring;
        roadgrid_cell_at(ctx->grid, e0, n0, &cx0, &cy0);
        buf_begin(vw, vh);
        for (ring = 0; ring <= rmax && nrib < RIB_MAX; ring++) {
            int gx, gy;
            for (gy = cy0 - ring; gy <= cy0 + ring && nrib < RIB_MAX; gy++) {
                for (gx = cx0 - ring; gx <= cx0 + ring && nrib < RIB_MAX; gx++) {
                    int ncnt, q;
                    const unsigned int *cell;
                    /* Only the ring's boundary; the interior was done already. */
                    if (ring > 0 && gx != cx0 - ring && gx != cx0 + ring &&
                        gy != cy0 - ring && gy != cy0 + ring)
                        continue;
                    cell = roadgrid_cell(ctx->grid, gx, gy, &ncnt);
                    if (!cell)
                        continue;
                    for (q = 0; q < ncnt && nrib < RIB_MAX; q++)
                        nrib = gather(ctx, &cam, e0, n0, (int)cell[q], cfg,
                                      nrib);
                }
            }
        }
    }

    qsort(RIB, (size_t)nrib, sizeof RIB[0], rib_cmp);

    for (i = 0; i < nrib; i++) {
        double xa, ya, xb, yb, w = CLASS_W_M[RIB[i].cls];
        if (!persp_screen(&cam, RIB[i].fa, RIB[i].la, &xa, &ya, NULL))
            continue;
        if (!persp_screen(&cam, RIB[i].fb, RIB[i].lb, &xb, &yb, NULL))
            continue;
        /* Euclidean and NOT |dx|+|dy|. Manhattan is never smaller, so it
         * keeps segments a hypot test drops -- on a diagonal by up to 41% --
         * and mockup.py measures the true length. The design gate byte-compares
         * the two, so a metric that merely "looks similar" is a failed gate. */
        if (sqrt((xb - xa) * (xb - xa) + (yb - ya) * (yb - ya)) <
            cfg->min_seg_px)
            continue;
        /* Below about two pixels of screen width there is no interior left to
         * show, so the casing collapses to a single stroke. Filling it anyway
         * would erase the road either side of it. */
        /* Below about two pixels of screen width there is no interior left to
         * show, so the casing collapses to a single stroke. Filling it anyway
         * would erase the road either side of it. */
        drawn += ribbon(&cam, RIB[i].fa, RIB[i].la, RIB[i].fb, RIB[i].lb, w,
                        0.0,
                        (w / RIB[i].tnear) >= 2.2 ? RIB_ROAD : RIB_EDGE);
    }

    /* The route on top, solid: the reference draws it as one unmissable
     * coloured ribbon and solid ink is the only equivalent one bit affords.
     * ROUTE_W_M is a little wider than a residential street so it reads as the
     * road you are ON rather than one more road; route_cap_px is what stops it
     * from swallowing the near field, where 9 m subtends a third of a 270 px
     * viewport. */
    for (i = 0; i + 1 < nroute; i++) {
        double fa, la, fb, lb;
        persp_to_ground(&cam, route_en[2 * i] - e0, route_en[2 * i + 1] - n0,
                        &fa, &la);
        persp_to_ground(&cam, route_en[2 * i + 2] - e0,
                        route_en[2 * i + 3] - n0, &fb, &lb);
        drawn += ribbon(&cam, fa, la, fb, lb, ROUTE_W_M, cfg->route_cap_px,
                        RIB_ROUTE);
    }

    /* Knockouts, before the caller's overlays land on top. */
    for (i = 0; i < nko; i++) {
        if (ko[i].r > 0.0) {
            int yy;
            for (yy = (int)floor(ko[i].cy - ko[i].r);
                 yy <= (int)ceil(ko[i].cy + ko[i].r); yy++) {
                double dy = (double)yy - ko[i].cy;
                double dx2 = ko[i].r * ko[i].r - dy * dy;
                int xx, xa, xb;
                if (dx2 < 0.0)
                    continue;
                xa = (int)floor(ko[i].cx - sqrt(dx2));
                xb = (int)ceil(ko[i].cx + sqrt(dx2));
                for (xx = xa; xx <= xb; xx++)
                    px_set(xx, yy, 0);
            }
        } else {
            int xx, yy;
            for (yy = (int)floor(ko[i].y0); yy <= (int)ceil(ko[i].y1); yy++)
                for (xx = (int)floor(ko[i].x0); xx <= (int)ceil(ko[i].x1); xx++)
                    px_set(xx, yy, 0);
        }
    }

    /* One blit, the aliased path, exactly as tiles_blit() does it. */
    cov_blit_bits(c, x0, y0, BUF_W, BUF_H, BUF, COV_INK);
    return drawn;
}

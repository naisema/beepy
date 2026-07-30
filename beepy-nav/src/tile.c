/* beepy-nav/src/tile.c -- see tile.h. The reader for tools/mktiles.py's pack.
 *
 * Three things in here are decisions rather than transcription, and each is
 * argued where it is made:
 *
 *   - the basemap is sampled by INVERSE nearest-neighbour, one source bit per
 *     destination pixel, because the map is course-up and a pre-rendered tile
 *     is not (see tiles_blit);
 *   - it lands in the frame through cov_blit_bits(), which is the aliased
 *     block path -- the same one the pre-rendered numerals use and the same
 *     one cov_hairline() ends in. Streets must not go anywhere near the
 *     coverage path: mockup.py's Canvas comment records what a 1 px street
 *     looks like anti-aliased, which is "hairy rather than thin";
 *   - the arithmetic is integer. Everything else in this program is doubles,
 *     but the goldens are byte-compared between clang on the Mac and gcc
 *     10.2.1 on the device, and a one-ulp difference in cos() would move a
 *     sample across a pixel boundary. Fixed at 1/65536 of a pack pixel it
 *     would take a difference eleven orders of magnitude larger to do that.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libbeepyfb/cover.h"
#include "tile.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

#define MAGIC "BNAVTILE"
#define TILE_VERSION 1
#define HDR_BYTES 64
#define ZOOM_ENTRY 32
#define FLAG_MSB_INK 1u

/* 256x256 at 1 bit. A pack with bigger tiles is refused rather than
 * accommodated: the LRU below is statically sized on this. */
#define TILE_MAX_BYTES 8192

/* The blit walks the destination, so the tiles it touches are the ones a
 * rotated 270x240 rectangle covers. That rectangle's bounding box is at most
 * sqrt(270^2 + 240^2) = 362 px on a side, and a 362 px span can straddle
 * three 256 px tiles -- so the worst case is 3x3 = 9 tiles in ONE frame, and
 * an eight-slot cache would evict a tile it is about to need again on every
 * single frame. Twelve slots is 96 KB, the same order as the coverage buffer,
 * and leaves the 3x3 with room to keep the previous frame's overlap. */
#define TILE_LRU 12

/* 16 fractional bits. A pack pixel is 1.5 m at the finest rung, so the sample
 * grid resolves to 23 microns; the point is not precision but repeatability. */
#define FXB 16
#define FX (1 << FXB)

typedef struct {
    double mpp;
    int32_t tx0, ty0;
    uint32_t nx, ny;
    uint32_t *idx; /* nx*ny file offsets; 0 = blank */
} zoom_t;

typedef struct {
    int zi, tx, ty;      /* what is in `data`; zi < 0 = empty slot */
    unsigned long stamp; /* LRU clock */
    unsigned char data[TILE_MAX_BYTES];
} slot_t;

struct tiles {
    FILE *f;
    long fsize;
    int tw, th;              /* tile pixels                     */
    int tw_shift, th_shift;  /* both are powers of two          */
    int stride;              /* bytes per tile row              */
    int tbytes;              /* stride * th                     */
    double lat0, lon0;
    double klat, klon;
    double corridor_m;
    int nzoom;
    uint32_t ntiles;
    zoom_t *z;
    double off_e, off_n; /* the pack origin, in the CALLER's frame */
    unsigned long clock;
    slot_t lru[TILE_LRU];
};

/* The destination bitmap the whole blit accumulates into, so the frame takes
 * ONE cov_blit_bits() and the coverage buffer sees one batch of ink instead
 * of 65 000 single-pixel writes. 12 KB, static for the same reason cov_t is:
 * the Pi Zero's stack is not where a frame buffer goes. */
static unsigned char DST[SCR_H][(SCR_W + 7) / 8];

/* ------------------------------------------------------- little-endian I/O
 *
 * Read byte at a time rather than casting into the buffer: the pack is
 * little-endian by definition and the reader must not silently depend on the
 * host agreeing, nor on an aligned load being legal at an arbitrary offset. */

static uint32_t
rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t
rd_u16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t
rd_i32(const unsigned char *p)
{
    return (int32_t)rd_u32(p);
}

/* IEEE 754 binary64, little-endian byte order -- which is what every target
 * this program has ever been built for uses, and the one assumption in the
 * format that a byte-at-a-time read cannot remove. */
static double
rd_f64(const unsigned char *p)
{
    uint64_t u = 0;
    double d;
    int i;
    for (i = 7; i >= 0; i--)
        u = (u << 8) | p[i];
    memcpy(&d, &u, sizeof d);
    return d;
}

static int
log2_exact(int v)
{
    int s = 0;
    if (v <= 0 || (v & (v - 1)) != 0)
        return -1;
    while ((1 << s) < v)
        s++;
    return s;
}

static void
fail(char *why, int nwhy, const char *msg)
{
    if (why && nwhy > 0)
        snprintf(why, (size_t)nwhy, "%s", msg);
}

/* ------------------------------------------------------------ open / close */

tiles_t *
tiles_open(const char *path, char *why, int nwhy)
{
    unsigned char hdr[HDR_BYTES];
    unsigned char *ztab = NULL;
    tiles_t *t;
    int i, j;

    fail(why, nwhy, "");
    if (!path || !*path) {
        fail(why, nwhy, "no path");
        return NULL;
    }
    t = (tiles_t *)calloc(1, sizeof *t);
    if (!t) {
        fail(why, nwhy, "out of memory");
        return NULL;
    }
    t->f = fopen(path, "rb");
    if (!t->f) {
        fail(why, nwhy, "cannot open");
        free(t);
        return NULL;
    }
    if (fseek(t->f, 0, SEEK_END) != 0 || (t->fsize = ftell(t->f)) < HDR_BYTES) {
        fail(why, nwhy, "too short to be a tile pack");
        goto bad;
    }
    rewind(t->f);
    if (fread(hdr, 1, sizeof hdr, t->f) != sizeof hdr) {
        fail(why, nwhy, "truncated header");
        goto bad;
    }
    if (memcmp(hdr, MAGIC, 8) != 0) {
        fail(why, nwhy, "not a beepy-nav tile pack");
        goto bad;
    }
    if (rd_u16(hdr + 8) != TILE_VERSION) {
        fail(why, nwhy, "unsupported pack version");
        goto bad;
    }
    if (rd_u16(hdr + 10) != HDR_BYTES) {
        fail(why, nwhy, "unexpected header size");
        goto bad;
    }
    t->tw = rd_u16(hdr + 12);
    t->th = rd_u16(hdr + 14);
    t->nzoom = rd_u16(hdr + 16);
    t->tw_shift = log2_exact(t->tw);
    t->th_shift = log2_exact(t->th);
    if (t->tw_shift < 3 || t->th_shift < 0) {
        fail(why, nwhy, "tile size is not a power of two");
        goto bad;
    }
    t->stride = t->tw / 8;
    t->tbytes = t->stride * t->th;
    if (t->tbytes <= 0 || t->tbytes > TILE_MAX_BYTES) {
        fail(why, nwhy, "tiles are larger than 256x256");
        goto bad;
    }
    if (!(rd_u16(hdr + 18) & FLAG_MSB_INK)) {
        fail(why, nwhy, "unknown bit order");
        goto bad;
    }
    if (t->nzoom < 1 || t->nzoom > 64) {
        fail(why, nwhy, "implausible zoom count");
        goto bad;
    }
    t->lat0 = rd_f64(hdr + 20);
    t->lon0 = rd_f64(hdr + 28);
    t->klat = rd_f64(hdr + 36);
    t->klon = rd_f64(hdr + 44);
    t->corridor_m = rd_f64(hdr + 52);
    t->ntiles = rd_u32(hdr + 60);
    if (!(t->klat > 0.0) || !(t->klon > 0.0)) {
        fail(why, nwhy, "bad projection constants");
        goto bad;
    }

    ztab = (unsigned char *)malloc((size_t)ZOOM_ENTRY * (size_t)t->nzoom);
    t->z = (zoom_t *)calloc((size_t)t->nzoom, sizeof *t->z);
    if (!ztab || !t->z) {
        fail(why, nwhy, "out of memory");
        goto bad;
    }
    if (fread(ztab, ZOOM_ENTRY, (size_t)t->nzoom, t->f) != (size_t)t->nzoom) {
        fail(why, nwhy, "truncated zoom table");
        goto bad;
    }
    for (i = 0; i < t->nzoom; i++) {
        const unsigned char *e = ztab + ZOOM_ENTRY * i;
        zoom_t *z = &t->z[i];
        uint32_t off, n;
        z->mpp = rd_f64(e);
        z->tx0 = rd_i32(e + 8);
        z->ty0 = rd_i32(e + 12);
        z->nx = rd_u32(e + 16);
        z->ny = rd_u32(e + 20);
        off = rd_u32(e + 24);
        if (!(z->mpp > 0.0) || z->nx == 0 || z->ny == 0 ||
            z->nx > 0x10000u || z->ny > 0x10000u ||
            (uint64_t)z->nx * z->ny > 0x100000u) {
            fail(why, nwhy, "implausible zoom grid");
            goto bad;
        }
        n = z->nx * z->ny;
        if ((long)off < HDR_BYTES ||
            (long)off + (long)n * 4 > t->fsize) {
            fail(why, nwhy, "zoom index outside the file");
            goto bad;
        }
        z->idx = (uint32_t *)malloc((size_t)n * 4);
        if (!z->idx) {
            fail(why, nwhy, "out of memory");
            goto bad;
        }
        if (fseek(t->f, (long)off, SEEK_SET) != 0) {
            fail(why, nwhy, "cannot seek to a zoom index");
            goto bad;
        }
        for (j = 0; j < (int)n; j++) {
            unsigned char b[4];
            uint32_t v;
            if (fread(b, 1, 4, t->f) != 4) {
                fail(why, nwhy, "truncated zoom index");
                goto bad;
            }
            v = rd_u32(b);
            /* Validated here, once, so the per-pixel path can trust it. */
            if (v != 0 &&
                ((long)v < HDR_BYTES || (long)v + t->tbytes > t->fsize)) {
                fail(why, nwhy, "tile offset outside the file");
                goto bad;
            }
            z->idx[j] = v;
        }
    }
    free(ztab);
    for (i = 0; i < TILE_LRU; i++)
        t->lru[i].zi = -1;
    /* Until told otherwise the caller's frame IS the pack's. */
    t->off_e = t->off_n = 0.0;
    return t;

bad:
    free(ztab);
    tiles_close(t);
    return NULL;
}

void
tiles_close(tiles_t *t)
{
    int i;
    if (!t)
        return;
    if (t->z) {
        for (i = 0; i < t->nzoom; i++)
            free(t->z[i].idx);
        free(t->z);
    }
    if (t->f)
        fclose(t->f);
    free(t);
}

void
tiles_bind_route(tiles_t *t, double lat0, double lon0)
{
    if (!t)
        return;
    /* The pack's reference, expressed in the caller's frame -- geo_project()
     * from route.c, repeated rather than included so this file stays
     * linkable on its own (test_tile.c does exactly that). */
    t->off_e = (t->lon0 - lon0) * t->klon * cos(lat0 * (M_PI / 180.0));
    t->off_n = (t->lat0 - lat0) * t->klat;
}

double
tiles_ref_lat(const tiles_t *t)
{
    return t ? t->lat0 : 0.0;
}
double
tiles_ref_lon(const tiles_t *t)
{
    return t ? t->lon0 : 0.0;
}
int
tiles_nzoom(const tiles_t *t)
{
    return t ? t->nzoom : 0;
}
double
tiles_zoom(const tiles_t *t, int i)
{
    return (t && i >= 0 && i < t->nzoom) ? t->z[i].mpp : 0.0;
}
int
tiles_count(const tiles_t *t)
{
    return t ? (int)t->ntiles : 0;
}

/* ------------------------------------------------------------------- cache */

static const unsigned char *
tile_bits(tiles_t *t, int zi, int tx, int ty)
{
    const zoom_t *z = &t->z[zi];
    uint32_t off;
    int i, victim = 0;
    unsigned long oldest;

    if (tx < z->tx0 || ty < z->ty0 || tx >= z->tx0 + (int)z->nx ||
        ty >= z->ty0 + (int)z->ny)
        return NULL;
    off = z->idx[(size_t)(ty - z->ty0) * z->nx + (size_t)(tx - z->tx0)];
    if (off == 0)
        return NULL; /* a blank tile costs an index slot, not 8 KB */

    for (i = 0; i < TILE_LRU; i++)
        if (t->lru[i].zi == zi && t->lru[i].tx == tx && t->lru[i].ty == ty) {
            t->lru[i].stamp = ++t->clock;
            return t->lru[i].data;
        }
    oldest = t->lru[0].stamp;
    for (i = 1; i < TILE_LRU; i++) {
        if (t->lru[i].zi < 0) {
            victim = i;
            break;
        }
        if (t->lru[i].stamp < oldest) {
            oldest = t->lru[i].stamp;
            victim = i;
        }
    }
    if (fseek(t->f, (long)off, SEEK_SET) != 0 ||
        fread(t->lru[victim].data, 1, (size_t)t->tbytes, t->f) !=
            (size_t)t->tbytes) {
        /* A read that fails mid-ride is not worth a diagnostic every frame:
         * the slot is dropped and the tile simply does not draw. */
        t->lru[victim].zi = -1;
        return NULL;
    }
    t->lru[victim].zi = zi;
    t->lru[victim].tx = tx;
    t->lru[victim].ty = ty;
    t->lru[victim].stamp = ++t->clock;
    return t->lru[victim].data;
}

/* -------------------------------------------------------------------- blit */

int
tiles_blit(cov_t *c, tiles_t *t, const tileview_t *v)
{
    double ct, st, p0, q0, u0, v0;
    int64_t px_row, py_row, dpx_dx, dpy_dx, dpx_dy, dpy_dy;
    int zi = -1, x, y, x0, y0, x1, y1, i;
    int cur_tx = 0, cur_ty = 0, have_cur = 0, drew = 0, ntiles = 0;
    const unsigned char *cur = NULL;
    struct {
        int tx, ty;
    } seen[TILE_LRU * 2];

    if (!c || !t || !v)
        return 0;
    /* Exact rung only. A pack that does not carry this zoom draws nothing,
     * which is the same code path -- and the same frame -- as no pack at all.
     * Resampling a coarser rung up would be a blurred street grid, and the
     * one thing a raster basemap has to be on this panel is crisp. */
    for (i = 0; i < t->nzoom; i++)
        if (fabs(t->z[i].mpp - v->mpp) <= 1e-9 * t->z[i].mpp) {
            zi = i;
            break;
        }
    if (zi < 0)
        return 0;

    x0 = v->x0 < 0 ? 0 : v->x0;
    y0 = v->y0 < 0 ? 0 : v->y0;
    x1 = v->x1 >= SCR_W ? SCR_W - 1 : v->x1;
    y1 = v->y1 >= SCR_H ? SCR_H - 1 : v->y1;
    if (x0 > x1 || y0 > y1)
        return 0;

    ct = cos(v->theta);
    st = sin(v->theta);
    /* The view centre, in pack pixels. */
    p0 = (v->org_e - t->off_e) / t->z[zi].mpp;
    q0 = -(v->org_n - t->off_n) / t->z[zi].mpp;
    /* A view a continent away from the pack is not an error, but it must not
     * be allowed to overflow the fixed-point grid either. 2^31 pack pixels is
     * 3 200 km at the finest rung; beyond that there is nothing to draw. */
    if (!(fabs(p0) < 2147483648.0) || !(fabs(q0) < 2147483648.0))
        return 0;

    /* Sample at pixel CENTRES: destination pixel (x, y) stands for the square
     * [x, x+1) x [y, y+1), and floor() of its centre is the pack pixel whose
     * own square contains it. */
    u0 = (x0 + 0.5) - v->cx;
    v0 = v->cy - (y0 + 0.5);
    px_row = (int64_t)llround((p0 + u0 * ct + v0 * st) * (double)FX);
    py_row = (int64_t)llround((q0 + u0 * st - v0 * ct) * (double)FX);
    dpx_dx = (int64_t)llround(ct * (double)FX);
    dpy_dx = (int64_t)llround(st * (double)FX);
    dpx_dy = -dpy_dx;
    dpy_dy = dpx_dx;

    memset(DST, 0, sizeof DST);

    for (y = y0; y <= y1; y++) {
        int64_t px = px_row, py = py_row;
        unsigned char *drow = DST[y];
        for (x = x0; x <= x1; x++, px += dpx_dx, py += dpy_dx) {
            /* Arithmetic shift: floor, including for negative pack pixels,
             * which is exactly the grid convention mktiles.py wrote with. */
            int ipx = (int)(px >> FXB), ipy = (int)(py >> FXB);
            int tx = ipx >> t->tw_shift, ty = ipy >> t->th_shift;
            int col, row;
            if (!have_cur || tx != cur_tx || ty != cur_ty) {
                cur = tile_bits(t, zi, tx, ty);
                cur_tx = tx;
                cur_ty = ty;
                have_cur = 1;
                if (cur) {
                    for (i = 0; i < ntiles; i++)
                        if (seen[i].tx == tx && seen[i].ty == ty)
                            break;
                    if (i == ntiles && ntiles < (int)(sizeof seen / sizeof seen[0])) {
                        seen[ntiles].tx = tx;
                        seen[ntiles].ty = ty;
                        ntiles++;
                    }
                }
            }
            if (!cur)
                continue;
            col = ipx - (tx << t->tw_shift);
            row = ipy - (ty << t->th_shift);
            if (cur[row * t->stride + (col >> 3)] & (0x80 >> (col & 7))) {
                drow[x >> 3] |= (unsigned char)(0x80 >> (x & 7));
                drew = 1;
            }
        }
        px_row += dpx_dy;
        py_row += dpy_dy;
    }

    if (!drew)
        return 0;
    /* One blit, through the exact-block path: full ink, no coverage, no
     * fringe. Runs of set bits become cov_fill_rect() spans, which is the
     * same primitive the pre-rendered numerals land through. */
    cov_blit_bits(c, 0, 0, SCR_W, SCR_H, &DST[0][0], COV_INK);
    return ntiles;
}

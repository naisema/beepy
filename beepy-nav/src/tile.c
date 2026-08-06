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
/* Both of these must precede every system header, and both are load-bearing on
 * the target rather than on a Mac.
 *
 * _FILE_OFFSET_BITS 64 makes off_t eight bytes. The device is armv7l with a
 * 32-bit userland, where off_t defaults to four, and fopen() on a file larger
 * than 2 GiB then fails outright with EOVERFLOW -- errno 75, "Value too large
 * for defined data type" -- before a byte is read. The four-region 0.375 m/px
 * basemap is 2.5 GB and reported "cannot open; no basemap" on the device while
 * opening perfectly on the Mac, where off_t was already 64-bit and this define
 * changes nothing. It lives here rather than in CFLAGS because CFLAGS is `?=`
 * and an environment that overrides it would silently take a correct reader and
 * cap it at 2 GiB again.
 *
 * _POSIX_C_SOURCE because -std=c11 sets __STRICT_ANSI__, under which glibc
 * exposes ISO C only -- and fseeko/ftello are POSIX. They are what this file
 * needs once a seek target can exceed LONG_MAX; plain fseek takes a long and
 * would seek a 2.4 GB offset to a negative one. Same convention as netfetch.c. */
#define _FILE_OFFSET_BITS 64
#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>            /* off_t -- see the defines above */

#include "libbeepyfb/cover.h"
#include "tile.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

#define MAGIC "BNAVTILE"
#define TILE_VERSION 1
/* Sparse index; same header and zoom table, see zoom_t. */
#define TILE_VERSION_SPARSE 2
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
    /* DENSE (pack v1): nx*ny file offsets, 0 = blank.
     * SPARSE (v2): `nsparse` pairs of (key, offset) sorted by key, where key is
     * the same (row * nx + column) the dense index uses as its subscript. The
     * encoding is a property of the whole pack, so this is 0 for a v1 pack and
     * the tile count for a v2 one -- see DESIGN.md 6.5 for why v2 exists at all
     * (105 MB of index for four regions at 0.375 m/px, against 1.5 MB). */
    uint32_t *idx;
    uint32_t nsparse;
} zoom_t;

typedef struct {
    int zi, tx, ty;      /* what is in `data`; zi < 0 = empty slot */
    unsigned long stamp; /* LRU clock */
    unsigned char data[TILE_MAX_BYTES];
} slot_t;

struct tiles {
    FILE *f;
    /* off_t and NOT long, and every seek below is fseeko for the same reason.
     * The target is armv7l with a 32-bit userland, where long is four bytes and
     * LONG_MAX is 2 147 483 647 -- so a pack larger than 2 GiB cannot have its
     * size held here, let alone be seeked within, and a tile at byte
     * 2 400 000 000 would be sought at a negative offset. The four-region
     * 0.375 m/px basemap is 2.5 GB, and the format's u32 offsets reach 4 GiB,
     * so the reader has to as well. The _FILE_OFFSET_BITS define at the top of
     * this file is what makes off_t eight bytes; without it this declaration
     * compiles and still overflows. */
    off_t fsize;
    int version;
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
    /* Caller metres east -> PACK metres east. Both frames measure northing the
     * same way (klat has no latitude term) but easting is scaled by
     * cos(reference latitude), so two packs referenced at different latitudes
     * disagree about how many metres a degree of longitude is. 1.0 until
     * tiles_bind_route() says otherwise. See it for what that cost. */
    double sx;
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
    if (fseeko(t->f, 0, SEEK_END) != 0 ||
        (t->fsize = ftello(t->f)) < HDR_BYTES) {
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
    t->version = rd_u16(hdr + 8);
    if (t->version != TILE_VERSION && t->version != TILE_VERSION_SPARSE) {
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
        /* The cell count bounds the malloc below, and NOTHING ELSE: the real
         * guard against a corrupt header is the file-size check immediately
         * after, because a bogus nx/ny large enough to matter names an index the
         * file is far too small to hold.
         *
         * It was 0x100000 -- one mebicell, 4 MB of index -- and that turned out
         * to refuse a legitimate pack. DESIGN.md 6.5's union puts several cities
         * in one rung, and the bounding box then spans the empty ground between
         * them: four cuts across 12 degrees of latitude need 546 x 3468 =
         * 1 893 528 cells at 1.5 m/px, or 7.6 MB. The pack was correct and the
         * reader said "implausible zoom grid".
         *
         * 0x400000 -- four mebicells, 16 MB per rung -- is the same kind of
         * number with room in it: four times what the widest real pack needs, so
         * a corrupt header still cannot ask for an unbounded allocation, and a
         * rider who wants a fifth city does not meet this line. Measured on the
         * pack that provoked it: 12.1 MB of index across all twelve rungs. */
        if (!(z->mpp > 0.0) || z->nx == 0 || z->ny == 0 ||
            z->nx > 0x10000u || z->ny > 0x10000u) {
            fail(why, nwhy, "implausible zoom grid");
            goto bad;
        }
        /* The cell cap applies to the DENSE encoding ONLY, because it is the
         * dense index that allocates per cell. A sparse rung's grid is
         * legitimately enormous -- 2012 x 13716 for four Thai regions at
         * 0.375 m/px, 27.6 million cells -- and it allocates per TILE, so the
         * bound belongs on the count. 4 mebitiles is 33 MB of index, four times
         * what the widest real pack needs (182 724 tiles, 1.4 MB).
         *
         * Getting this wrong is what made the first v2 pack unreadable: the cap
         * was raised for the dense case and then applied to both, so a correct
         * 2.5 GB pack came back "implausible zoom grid". */
        if (t->version == TILE_VERSION &&
            (uint64_t)z->nx * z->ny > 0x400000u) {
            fail(why, nwhy, "implausible zoom grid");
            goto bad;
        }
        /* v1 stores one u32 per cell; v2 stores two per TILE -- and the count
         * lives in the zoom entry's last word, which was always zero in v1. */
        if (t->version == TILE_VERSION_SPARSE) {
            z->nsparse = rd_u32(e + 28);
            /* Bounded by what the grid could hold, so a corrupt count cannot ask
             * for an unbounded allocation; the file-size check below is the real
             * guard, exactly as for the dense case. */
            if ((uint64_t)z->nsparse > (uint64_t)z->nx * z->ny ||
                z->nsparse > 0x400000u) {
                fail(why, nwhy, "sparse index longer than its grid");
                goto bad;
            }
            n = z->nsparse * 2;
        } else {
            z->nsparse = 0;
            n = z->nx * z->ny;
        }
        if ((off_t)off < HDR_BYTES ||
            (off_t)off + (off_t)n * 4 > t->fsize) {
            fail(why, nwhy, "zoom index outside the file");
            goto bad;
        }
        z->idx = (uint32_t *)malloc((size_t)n * 4);
        if (!z->idx) {
            fail(why, nwhy, "out of memory");
            goto bad;
        }
        if (fseeko(t->f, (off_t)off, SEEK_SET) != 0) {
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
            /* Validated here, once, so the per-pixel path can trust it. In a
             * sparse index the EVEN words are keys, not offsets, and a key is
             * bounded by the grid rather than by the file. */
            if (t->version == TILE_VERSION_SPARSE && (j % 2) == 0) {
                if ((uint64_t)v >= (uint64_t)z->nx * z->ny) {
                    fail(why, nwhy, "sparse index key outside the grid");
                    goto bad;
                }
                z->idx[j] = v;
                continue;
            }
            if (v != 0 &&
                ((off_t)v < HDR_BYTES || (off_t)v + t->tbytes > t->fsize)) {
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
    t->sx = 1.0;
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
    double ck, cc;

    if (!t)
        return;
    /* The pack's reference, expressed in the caller's frame -- geo_project()
     * from route.c, repeated rather than included so this file stays
     * linkable on its own (test_tile.c does exactly that). */
    t->off_e = (t->lon0 - lon0) * t->klon * cos(lat0 * (M_PI / 180.0));
    t->off_n = (t->lat0 - lat0) * t->klat;

    /* AND THE SCALE, which this used to leave out (DESIGN.md 6.5).
     *
     * A translation is not enough. Subtracting off_e leaves the distance from
     * the pack's origin measured in the CALLER's metres, and the tiles were cut
     * in the PACK's -- and a metre of longitude is 111320*cos(lat0), so the two
     * disagree by cos(pack ref) / cos(caller ref).
     *
     * It is small and it is multiplied by a large number. A country-framed
     * basemap (13.05 N) under a route referenced in Bangkok (13.885 N) differ by
     * 0.35%, which over the 1.01 degrees from the pack's reference to where the
     * riding happens is 382 METRES -- 64 px at 6 m/px, and 254 px at the finest
     * rung, where it puts the streets off the side of the screen. The symptom is
     * a route that does not lie on any road, and it went unnoticed because every
     * frozen frame in the gate uses a pack cut FOR its route: asok.tiles is
     * referenced to asok.gpx's first point, so cos cancels and the bug is
     * invisible in exactly the place it would have been caught.
     *
     * What remains after this is a 0.35% difference in SCALE between the drawn
     * route and the drawn streets, because the tiles' own pixels were rendered
     * at the pack's metre. Over a 400 px screen that is 1.2 px, and correcting it
     * would mean resampling a raster basemap -- which 6.5 refuses on the grounds
     * that a blurred street grid is worse than a sharp one. */
    ck = cos(t->lat0 * (M_PI / 180.0));
    cc = cos(lat0 * (M_PI / 180.0));
    t->sx = (fabs(cc) > 1e-12) ? ck / cc : 1.0;
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
    if (z->nsparse) {
        /* BINARY SEARCH the keys (DESIGN.md 6.5). The writer sorts by
         * (row, column), which is the same order this key is computed in, so the
         * array is ascending by construction.
         *
         * O(log n) against the dense index's O(1), and it does not matter: this
         * runs when the tile UNDER THE CURSOR CHANGES, which is at most nine
         * times a frame (the LRU above absorbs the rest), and log2(200 000) is
         * eighteen comparisons. The dense index it replaces cost 105 MB of
         * resident memory on a 426 MB device. */
        uint32_t key = (uint32_t)(ty - z->ty0) * z->nx +
                       (uint32_t)(tx - z->tx0);
        uint32_t lo = 0, hi = z->nsparse;
        off = 0;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            uint32_t k = z->idx[2 * mid];
            if (k == key) {
                off = z->idx[2 * mid + 1];
                break;
            }
            if (k < key)
                lo = mid + 1;
            else
                hi = mid;
        }
    } else {
        off = z->idx[(size_t)(ty - z->ty0) * z->nx + (size_t)(tx - z->tx0)];
    }
    if (off == 0)
        return NULL; /* a blank tile is simply absent from a sparse index */

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
    if (fseeko(t->f, (off_t)off, SEEK_SET) != 0 ||
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
    double ct, st, p0, q0, u0, v0, zoomscale = 1.0;
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
    /* No rung at this scale. MAGNIFY the finest one the pack has, if the view is
     * finer than it (DESIGN.md 6.5).
     *
     * The comment above used to refuse this outright, on the grounds that
     * "resampling a coarser rung up would be a blurred street grid". That is true
     * of a filtered resample and false of the one this blit already performs: the
     * sampler is inverse NEAREST-NEIGHBOUR, one source bit per destination pixel,
     * so halving the step turns each pack pixel into a clean 2x2 block. Blocky,
     * not blurred -- and every other mark on the page (the route, the chevron, the
     * text, the scale bar) is a vector drawn at full resolution over it.
     *
     * It buys the two rungs the scale bar cannot otherwise reach: 50 M needs
     * 0.5-1.0 m/px and 25 M needs 0.25-0.5, against a finest cut rung of 1.5.
     *
     * ONLY FINER, never coarser. Stepping MORE than one pack pixel per screen
     * pixel would drop source bits and alias a street grid into moire, which is a
     * real reason to refuse -- and the pack carries proper coarse rungs for that.
     * Capped at 8x so a wild mpp cannot turn one tile into a whole screen of
     * blocks; nothing on the ladder asks for more than 4x. */
    if (zi < 0) {
        int fine = -1;
        for (i = 0; i < t->nzoom; i++)
            if (fine < 0 || t->z[i].mpp < t->z[fine].mpp)
                fine = i;
        if (fine >= 0 && v->mpp > 0.0 && v->mpp < t->z[fine].mpp &&
            t->z[fine].mpp <= 8.0 * v->mpp)
            zi = fine;
    }
    if (zi < 0)
        return 0;
    /* Pack pixels per screen pixel: 1 for a cut rung, 1/2 or 1/4 when magnifying. */
    zoomscale = v->mpp / t->z[zi].mpp;

    x0 = v->x0 < 0 ? 0 : v->x0;
    y0 = v->y0 < 0 ? 0 : v->y0;
    x1 = v->x1 >= SCR_W ? SCR_W - 1 : v->x1;
    y1 = v->y1 >= SCR_H ? SCR_H - 1 : v->y1;
    if (x0 > x1 || y0 > y1)
        return 0;

    ct = cos(v->theta);
    st = sin(v->theta);
    /* The view centre, in pack pixels. */
    p0 = (v->org_e - t->off_e) * t->sx / t->z[zi].mpp;
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
    /* zoomscale carries the magnification: a screen pixel steps that many pack
     * pixels. It is exactly 1, 1/2 or 1/4 on the ladder, so the fixed-point step
     * is exact and the blocks land on pixel boundaries rather than shimmering. */
    px_row = (int64_t)llround((p0 + (u0 * ct + v0 * st) * zoomscale) *
                              (double)FX);
    py_row = (int64_t)llround((q0 + (u0 * st - v0 * ct) * zoomscale) *
                              (double)FX);
    dpx_dx = (int64_t)llround(ct * zoomscale * (double)FX);
    dpy_dx = (int64_t)llround(st * zoomscale * (double)FX);
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

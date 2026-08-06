/* beepy-nav/tests/test_tile.c -- host unit tests for the basemap tile layer.
 *
 * tile.c is the one M5 module whose behaviour is mostly NOT visible in a
 * frozen frame: the interesting cases are the ones that draw nothing on
 * purpose -- a missing file, a corrupt file, a zoom the pack does not carry,
 * a view outside the corridor -- and a golden that shows no basemap cannot
 * distinguish "correctly declined" from "silently broken". So this builds
 * packs byte by byte, in C, with no Pillow anywhere near it (which is also
 * what lets it run on the device inside `make check`), and asserts on the
 * coverage buffer directly.
 *
 * Every expected pixel below is derived from the format in tile.h and
 * mktiles.py, not read off a previous run.
 *
 *     make test-unit
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libbeepyfb/cover.h"
#include "tile.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int failures;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* ------------------------------------------------------- pack construction */

#define TW 256
#define TH 256
#define TSTRIDE (TW / 8)
#define TBYTES (TSTRIDE * TH)
#define HDR 64
#define ZENT 32

static void
put_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void
put_u32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void
put_i32(unsigned char *p, long v)
{
    put_u32(p, (unsigned long)v);
}

/* Little-endian IEEE 754, spelled out for the same reason tile.c reads it
 * that way: the test must not agree with the reader by accident. */
static void
put_f64(unsigned char *p, double d)
{
    unsigned char tmp[8];
    int i;
    memcpy(tmp, &d, 8);
    for (i = 0; i < 8; i++)
        p[i] = tmp[i]; /* both lanes are little-endian; see tile.c */
}

/* A one-zoom pack with a 1x1 grid holding one tile, whose bits the caller
 * paints. Returns the path (a fixed name in the working directory, removed by
 * the caller) or NULL. */
static const char *
write_pack(const char *path, double mpp, double lat0, double lon0,
           int tx0, int ty0, const unsigned char *tile)
{
    unsigned char hdr[HDR], zent[ZENT], idx[4];
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "BNAVTILE", 8);
    put_u16(hdr + 8, 1);   /* version      */
    put_u16(hdr + 10, HDR);
    put_u16(hdr + 12, TW);
    put_u16(hdr + 14, TH);
    put_u16(hdr + 16, 1);  /* nzoom        */
    put_u16(hdr + 18, 1);  /* MSB-first    */
    put_f64(hdr + 20, lat0);
    put_f64(hdr + 28, lon0);
    put_f64(hdr + 36, 110540.0);
    put_f64(hdr + 44, 111320.0);
    put_f64(hdr + 52, 500.0);
    put_u32(hdr + 60, tile ? 1 : 0);
    fwrite(hdr, 1, sizeof hdr, f);

    memset(zent, 0, sizeof zent);
    put_f64(zent, mpp);
    put_i32(zent + 8, tx0);
    put_i32(zent + 12, ty0);
    put_u32(zent + 16, 1); /* nx */
    put_u32(zent + 20, 1); /* ny */
    put_u32(zent + 24, HDR + ZENT);
    fwrite(zent, 1, sizeof zent, f);

    put_u32(idx, tile ? (unsigned long)(HDR + ZENT + 4) : 0UL);
    fwrite(idx, 1, 4, f);
    if (tile)
        fwrite(tile, 1, TBYTES, f);
    fclose(f);
    return path;
}

static void
set_px(unsigned char *tile, int col, int row)
{
    tile[row * TSTRIDE + col / 8] |= (unsigned char)(0x80 >> (col % 8));
}

/* ------------------------------------------------------------------ probes */

static int
ink_at(const cov_t *c, int x, int y)
{
    return c->v[y][x] < 128;
}

static int
ink_count(const cov_t *c)
{
    int x, y, n = 0;
    for (y = 0; y < SCR_H; y++)
        for (x = 0; x < SCR_W; x++)
            if (c->v[y][x] < 128)
                n++;
    return n;
}

static void
view_at(tileview_t *v, double mpp, double theta, double org_e, double org_n)
{
    v->mpp = mpp;
    v->cx = 200.0;
    v->cy = 120.0;
    v->theta = theta;
    v->org_e = org_e;
    v->org_n = org_n;
    v->x0 = 0;
    v->y0 = 0;
    v->x1 = SCR_W - 1;
    v->y1 = SCR_H - 1;
}

/* One rendered frame. cov_resolve() is what commits the pending same-ink
 * batch into c->v, so the probes above only mean anything after it -- hence
 * the throwaway canvas, which is otherwise unread. */
static cov_t COV;
static canvas_t *CV;

static int
blit(tiles_t *t, const tileview_t *v)
{
    int n;
    cov_begin(&COV);
    n = tiles_blit(&COV, t, v);
    cov_resolve(&COV, CV);
    return n;
}

/* ------------------------------------------------------------------- tests */

/* DESIGN.md 6.5: a pack bound to a caller's frame must draw the same GROUND in
 * the same place, whatever reference that caller happens to use. This is the
 * assertion the gate did not have, and its absence cost a rider a route that did
 * not lie on any road.
 *
 * tiles_bind_route() translated the pack origin into the caller's frame and
 * stopped there. Subtracting that offset leaves the distance from the pack's
 * origin in the CALLER's metres, while the tiles were cut in the PACK's -- and a
 * metre of longitude is 111320*cos(lat0), so the two disagree by
 * cos(pack ref)/cos(caller ref). A country-framed basemap under a Bangkok route
 * came out 382 m adrift: 64 px at 6 m/px.
 *
 * WHY EVERY EXISTING FRAME MISSED IT. The goldens use asok.tiles, which
 * mktiles.py cut with --route asok.gpx -- so the pack's reference IS the route's
 * first point, cos cancels, and the bug is exactly invisible in the one place it
 * would have been caught.
 *
 * AND WHY THE FIRST VERSION OF *THIS* TEST MISSED IT TOO, which is the part
 * worth keeping: it lit a pixel 40 m from the pack's reference. The error is a
 * SCALE, so it grows with distance from that reference -- 40 m of it is 1 m,
 * a quarter of a pixel, and the test passed with the fix reverted. The lit pixel
 * is now a degree of longitude out, where the same scale error is 643 px and puts
 * it off the screen entirely. A test of a proportional error has to be run far
 * from the origin or it is testing nothing. */
static void
t_frame_scale(void)
{
    static unsigned char tile[TBYTES];
    tiles_t *t;
    tileview_t v;
    const char *p = "test-tile-f.tiles";
    /* The pack's frame, and a caller five degrees north -- 2.4% of cos. At the
     * same latitude this test cannot fail, which is why they differ. */
    const double plat = 13.0, plon = 100.0;
    const double clat = 18.0, clon = 98.0;
    /* A 1x1 grid placed 105 tiles east, so the lit pixel is 107 560 m -- about a
     * degree of longitude -- from the pack's reference. */
    const int tx0 = 105;
    double glat, glon, ge, gn;
    int i, gotx[2], goty[2], lit[2];

    memset(tile, 0, sizeof tile);
    set_px(tile, 10, 20);
    check(write_pack(p, 4.0, plat, plon, tx0, 0, tile) != NULL, "write pack f");
    t = tiles_open(p, NULL, 0);
    check(t != NULL, "open pack f");
    if (!t)
        return;

    /* The lit pixel's ground position, back through the PACK's own projection. */
    ge = (tx0 * TW + 10) * 4.0;
    gn = -20 * 4.0;
    glon = plon + ge / (111320.0 * cos(plat * (M_PI / 180.0)));
    glat = plat + gn / 110540.0;

    for (i = 0; i < 2; i++) {
        double lat0 = i ? clat : plat, lon0 = i ? clon : plon;
        double ce = (glon - lon0) * 111320.0 * cos(lat0 * (M_PI / 180.0));
        double cn = (glat - lat0) * 110540.0;
        int x, y;
        gotx[i] = goty[i] = -1;
        tiles_bind_route(t, lat0, lon0);
        /* Centred on that ground, so the pixel must land dead centre in BOTH. */
        view_at(&v, 4.0, 0.0, ce, cn);
        blit(t, &v);
        lit[i] = ink_count(&COV);
        for (y = 0; y < SCR_H && gotx[i] < 0; y++)
            for (x = 0; x < SCR_W; x++)
                if (ink_at(&COV, x, y)) {
                    gotx[i] = x;
                    goty[i] = y;
                    break;
                }
    }
    check(lit[0] == 1, "the pack's own frame lights exactly one pixel");
    /* This is the one that failed before the scale was carried: 643 px adrift
     * put the pixel off the screen, so nothing was drawn at all. */
    check(lit[1] == 1, "and so does a frame referenced 5 degrees away");
    check(gotx[0] == gotx[1] && goty[0] == goty[1],
          "the same ground lands on the same pixel in both frames");
    tiles_close(t);
    remove(p);
}

/* Pack v2's SPARSE index (DESIGN.md 6.5): the same tiles, addressed by a sorted
 * (key, offset) table instead of one u32 per grid cell.
 *
 * It exists because a merged pack uses one grid per rung spanning every region in
 * it, and the dense index is 4 bytes per cell whether a tile is there or not:
 * four Thai regions at 0.375 m/px share a 2013 x 13717 grid, which is 105 MB
 * resident on a device with 426 MB. The same tiles sparse are 1.2 MB.
 *
 * The pack here is written by hand so the reader is tested against the FORMAT and
 * not against tools/mergetiles.py agreeing with it. Three claims:
 *   - a key that is present resolves to its tile;
 *   - a key that is absent draws nothing, rather than finding a neighbour (the
 *     failure a binary search gets wrong when the miss case returns `lo`);
 *   - a count longer than the grid is refused, because it bounds a malloc.
 */
static const char *
write_sparse(const char *path, double mpp, int tx0, int ty0, int nx, int ny,
             const unsigned char *tile, uint32_t key, uint32_t count)
{
    unsigned char hdr[HDR], zent[ZENT], pair[8];
    FILE *f = fopen(path, "wb");
    if (!f)
        return NULL;
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, "BNAVTILE", 8);
    put_u16(hdr + 8, 2);            /* the sparse version */
    put_u16(hdr + 10, HDR);
    put_u16(hdr + 12, TW);
    put_u16(hdr + 14, TH);
    put_u16(hdr + 16, 1);
    put_u16(hdr + 18, 1);
    put_f64(hdr + 20, 13.0);
    put_f64(hdr + 28, 100.0);
    put_f64(hdr + 36, 110540.0);
    put_f64(hdr + 44, 111320.0);
    put_f64(hdr + 52, 500.0);
    put_u32(hdr + 60, 1);
    fwrite(hdr, 1, sizeof hdr, f);

    memset(zent, 0, sizeof zent);
    put_f64(zent, mpp);
    put_i32(zent + 8, tx0);
    put_i32(zent + 12, ty0);
    put_u32(zent + 16, (unsigned long)nx);
    put_u32(zent + 20, (unsigned long)ny);
    put_u32(zent + 24, HDR + ZENT);      /* index follows the zoom table */
    put_u32(zent + 28, count);           /* v1's spare word is v2's count */
    fwrite(zent, 1, sizeof zent, f);

    put_u32(pair, key);
    put_u32(pair + 4, HDR + ZENT + 8);   /* the one tile follows the index */
    fwrite(pair, 1, 8, f);
    fwrite(tile, 1, TBYTES, f);
    fclose(f);
    return path;
}

static void
t_sparse_index(void)
{
    static unsigned char tile[TBYTES];
    tiles_t *t;
    tileview_t v;
    const char *p = "test-tile-s.tiles";
    /* A 4x4 grid at the origin; put the tile at grid (2,1), so the key is
     * 1 * 4 + 2 = 6 and neither coordinate is zero. */
    const int nx = 4, ny = 4, gx = 2, gy = 1;
    const uint32_t key = (uint32_t)(gy * nx + gx);

    memset(tile, 0, sizeof tile);
    set_px(tile, 0, 0);                 /* one pixel, at the tile's own corner */
    check(write_sparse(p, 4.0, 0, 0, nx, ny, tile, key, 1) != NULL,
          "write a sparse pack");
    t = tiles_open(p, NULL, 0);
    check(t != NULL, "a v2 pack opens");
    if (!t)
        return;

    /* Tile (2,1) covers pack pixels x 512..767, y 256..511. Its pixel (0,0) is
     * pack pixel (512, 256), which at 4 m/px is e = 2048, n = -1024. Centre the
     * view there and it lands dead centre. */
    view_at(&v, 4.0, 0.0, 512.0 * 4.0, -256.0 * 4.0);
    check(blit(t, &v) == 1, "the sparse key resolves to its tile");
    check(ink_count(&COV) == 1 && ink_at(&COV, 200, 120),
          "and the tile's own pixel lands where the projection says");

    /* A tile the index does NOT hold. Grid (0,0) is key 0, which sorts before
     * the only key present -- the case a binary search returns `lo` for, and
     * would read the wrong offset if the miss were not checked. */
    view_at(&v, 4.0, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 0 && ink_count(&COV) == 0,
          "a key absent from the index draws nothing");
    tiles_close(t);

    /* A count longer than the grid could hold is refused: it sizes a malloc. */
    check(write_sparse(p, 4.0, 0, 0, nx, ny, tile, key, 999) != NULL,
          "write a sparse pack with an absurd count");
    check(tiles_open(p, NULL, 0) == NULL,
          "a sparse count longer than its grid is refused");

    /* The case a 4x4 grid can never reach, and the one that mattered: a grid
     * whose CELL count is far past the dense cap, holding one tile.
     *
     * 2048 x 16384 is 33.5 million cells -- eight times the 4-mebicell dense
     * limit -- and is the shape a real four-region pack takes at 0.375 m/px
     * (2012 x 13716). Dense, that index would be 134 MB and rightly refused;
     * sparse it is eight bytes. The first v2 pack ever built was rejected with
     * "implausible zoom grid" because the cap was applied to both encodings,
     * and the test above passed throughout: a 4x4 grid is 16 cells, so it
     * cannot tell a per-cell bound from a per-tile one. This can.
     *
     * The tile goes at grid (1000, 9000) -- key 18 433 000, well past what a
     * 32-bit cell index would overflow at, and past the dense cap in both
     * axes' product. */
    {
        const int bx = 2048, by = 16384, hx = 1000, hy = 9000;
        const uint32_t bkey = (uint32_t)hy * bx + hx;
        check(write_sparse(p, 4.0, 0, 0, bx, by, tile, bkey, 1) != NULL,
              "write a sparse pack on a grid past the dense cap");
        t = tiles_open(p, NULL, 0);
        check(t != NULL, "a huge-grid sparse pack opens -- the cap is per tile");
        if (t) {
            view_at(&v, 4.0, 0.0, (double)hx * TW * 4.0,
                    -(double)hy * TH * 4.0);
            check(blit(t, &v) == 1 && ink_at(&COV, 200, 120),
                  "and its one tile still resolves 18 million cells in");
            tiles_close(t);
        }
    }
    remove(p);
}

/* DESIGN.md 6.5's magnification: a view FINER than the pack's finest rung uses
 * that rung and turns each pack pixel into a block, because the sampler is
 * nearest-neighbour and halving its step is exact.
 *
 * The assertion is a count and a position, which is what separates "magnified"
 * from "blurred" and from "wrong": at 2x one lit pack pixel must be exactly FOUR
 * lit screen pixels forming a 2x2 square, and at 4x exactly SIXTEEN. A filtered
 * resample would light more than that with grey in between; a blit that ignored
 * the magnification would light one.
 *
 * And the direction is asserted too: a view COARSER than the only rung must still
 * draw nothing, because stepping more than one pack pixel per screen pixel drops
 * source bits and aliases a street grid. */
static void
t_magnify(void)
{
    static unsigned char tile[TBYTES];
    tiles_t *t;
    tileview_t v;
    const char *p = "test-tile-m.tiles";

    memset(tile, 0, sizeof tile);
    set_px(tile, 10, 20);
    check(write_pack(p, 4.0, 13.0, 100.0, 0, 0, tile) != NULL, "write pack m");
    t = tiles_open(p, NULL, 0);
    check(t != NULL, "open pack m");
    if (!t)
        return;

    /* The cut rung itself: one pack pixel, one screen pixel, as t_single_pixel
     * already pins -- repeated here as the baseline the ratios are against. */
    view_at(&v, 4.0, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 1 && ink_count(&COV) == 1, "at the rung: one pixel");

    /* 2x. The pack pixel spans e in [40,44), n in (-84,-80]; at 2 m/px that is
     * 20 px right of the centre and 40 px below it -- twice the 10/20 the cut
     * rung puts it at, because the metres have not moved and the pixels are half
     * the size. */
    view_at(&v, 2.0, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 1, "2x magnified: the tile still contributes");
    check(ink_count(&COV) == 4, "and the pack pixel is a 2x2 block");
    check(ink_at(&COV, 220, 160) && ink_at(&COV, 221, 160) &&
              ink_at(&COV, 220, 161) && ink_at(&COV, 221, 161),
          "the block sits where the projection puts it");

    /* 4x: sixteen, and still square. */
    view_at(&v, 1.0, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 1, "4x magnified: the tile still contributes");
    check(ink_count(&COV) == 16, "and the pack pixel is a 4x4 block");

    /* Coarser than the only rung: refused, not minified. */
    view_at(&v, 8.0, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 0 && ink_count(&COV) == 0,
          "a view coarser than the rung draws nothing rather than aliasing");
    /* And past the 8x cap. */
    view_at(&v, 0.25, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 0, "and magnification stops at 8x");

    tiles_close(t);
    remove(p);
}

/* A single lit pack pixel must land on exactly the screen pixel the
 * projection of tile.h names, and on no other. */
static void
t_single_pixel(void)
{
    static unsigned char tile[TBYTES];
    tiles_t *t;
    tileview_t v;
    const char *p = "test-tile-a.tiles";
    int n;

    memset(tile, 0, sizeof tile);
    /* Tile (0,0) holds pack pixels 0..255. Light (10, 20). At 4 m/px that is
     * the world square e in [40,44), n in (-84,-80]. */
    set_px(tile, 10, 20);
    check(write_pack(p, 4.0, 13.0, 100.0, 0, 0, tile) != NULL, "write pack a");
    t = tiles_open(p, NULL, 0);
    check(t != NULL, "open pack a");
    if (!t)
        return;

    /* Put the view centre on the world origin, north up. Pack pixel (10, 20)
     * is then at screen (cx + 10, cy + 20) -- px is e/mpp and py is -n/mpp,
     * which is already screen-y-down. */
    view_at(&v, 4.0, 0.0, 0.0, 0.0);
    n = blit(t, &v);
    check(n == 1, "one tile contributed");
    check(ink_at(&COV, 210, 140), "the lit pack pixel lands at (210,140)");
    check(ink_count(&COV) == 1, "and nothing else is inked");

    /* Pan half a tile east: the same pack pixel must move left by 128 px. */
    view_at(&v, 4.0, 0.0, 128.0 * 4.0, 0.0);
    blit(t, &v);
    check(ink_at(&COV, 82, 140), "panning 128 px east moves it to (82,140)");
    check(ink_count(&COV) == 1, "still exactly one pixel");

    /* A quarter turn: map_project rotates the world by +theta, so a point
     * due east of centre swings to due north of it. 10 px east and 20 px
     * south of the centre becomes 20 px east and 10 px north. */
    view_at(&v, 4.0, M_PI / 2.0, 0.0, 0.0);
    blit(t, &v);
    check(ink_at(&COV, 220, 109), "rotated a quarter turn it lands at (220,109)");
    check(ink_count(&COV) == 1, "rotation neither drops nor duplicates it");

    tiles_close(t);
    remove(p);
}

/* A straight source line stays a CONNECTED line under rotation. This is the
 * property that makes inverse nearest-neighbour acceptable for 1 px streets
 * at all: for every destination column the sampled band is at least one row
 * high, so the rotated line is stair-stepped like Bresenham's rather than
 * dotted. If it were not true the basemap would read as gravel. */
static void
t_rotated_line_is_connected(void)
{
    static unsigned char tile[TBYTES];
    tiles_t *t;
    tileview_t v;
    const char *p = "test-tile-b.tiles";
    int i, y, x, gaps = 0, cols = 0;

    memset(tile, 0, sizeof tile);
    for (i = 0; i < 256; i++)
        set_px(tile, i, 128); /* one horizontal pack row, the full tile wide */
    check(write_pack(p, 4.0, 13.0, 100.0, 0, 0, tile) != NULL, "write pack b");
    t = tiles_open(p, NULL, 0);
    check(t != NULL, "open pack b");
    if (!t)
        return;

    /* 30 degrees, centred on the middle of the line. */
    view_at(&v, 4.0, 30.0 * M_PI / 180.0, 128.0 * 4.0, -128.0 * 4.0);
    blit(t, &v);
    for (x = 0; x < SCR_W; x++) {
        int hit = 0;
        for (y = 0; y < SCR_H; y++)
            if (ink_at(&COV, x, y))
                hit = 1;
        if (hit)
            cols++;
    }
    /* The line crosses the full width at 30 degrees, so every column in the
     * span must carry at least one pixel. Count the interior columns that do
     * not. */
    for (x = 1; x < SCR_W - 1; x++) {
        int here = 0, before = 0, after = 0;
        for (y = 0; y < SCR_H; y++) {
            if (ink_at(&COV, x, y))
                here = 1;
            if (ink_at(&COV, x - 1, y))
                before = 1;
            if (ink_at(&COV, x + 1, y))
                after = 1;
        }
        if (!here && before && after)
            gaps++;
    }
    /* 256 pack pixels of line at 30 degrees span 256*cos30 = 222 columns. */
    check(cols > 200, "the rotated line spans most of the screen");
    check(gaps == 0, "and has no gap in any column");
    tiles_close(t);
    remove(p);
}

/* Everything that must draw NOTHING. Each of these is a frame that has to be
 * byte-identical to the frame with no pack at all -- which is the whole claim
 * DESIGN.md 6.5 makes for the layer being optional. */
static void
t_declines(void)
{
    static unsigned char tile[TBYTES];
    tiles_t *t;
    tileview_t v;
    const char *p = "test-tile-c.tiles";
    char why[96];

    memset(tile, 0, sizeof tile);
    set_px(tile, 10, 20);
    write_pack(p, 4.0, 13.0, 100.0, 0, 0, tile);
    t = tiles_open(p, NULL, 0);
    check(t != NULL, "open pack c");
    if (!t)
        return;

    /* A zoom the pack does not carry. */
    view_at(&v, 6.0, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 0, "a rung the pack lacks draws nothing");
    check(ink_count(&COV) == 0, "and leaves the buffer clean");

    /* A rung that is close but not equal: no resampling, ever. */
    view_at(&v, 4.0001, 0.0, 0.0, 0.0);
    check(blit(t, &v) == 0, "a near miss on the rung is still a miss");

    /* Ten kilometres away -- outside the grid, inside the arithmetic. */
    view_at(&v, 4.0, 0.0, 10000.0, 10000.0);
    check(blit(t, &v) == 0, "a view outside the coverage draws nothing");
    check(ink_count(&COV) == 0, "and leaves the buffer clean");

    /* Half the earth away: the fixed-point guard, not the grid test. */
    view_at(&v, 4.0, 0.0, 2.0e10, 0.0);
    check(blit(t, &v) == 0, "an absurd view is refused rather than wrapped");

    /* A NULL pack is the ordinary no-basemap case and must be silent. */
    view_at(&v, 4.0, 0.0, 0.0, 0.0);
    check(blit(NULL, &v) == 0, "no pack draws nothing");
    tiles_close(t);
    remove(p);

    check(tiles_open("test-tile-does-not-exist", why, (int)sizeof why) == NULL,
          "a missing file does not open");
    check(why[0] != '\0', "and says why");
}

/* A file that is the right length and the wrong content must be refused, not
 * read: this reader runs on a ride, from an SD card, on a pack a rider
 * copied over themselves. */
static void
t_corrupt(void)
{
    static unsigned char tile[TBYTES];
    const char *p = "test-tile-d.tiles";
    unsigned char bad[8];
    FILE *f;
    char why[96];

    memset(tile, 0, sizeof tile);
    set_px(tile, 1, 1);

    write_pack(p, 4.0, 13.0, 100.0, 0, 0, tile);
    f = fopen(p, "r+b");
    check(f != NULL, "reopen pack d");
    if (f) {
        memcpy(bad, "BNAVTIL?", 8);
        fwrite(bad, 1, 8, f);
        fclose(f);
    }
    check(tiles_open(p, why, (int)sizeof why) == NULL, "bad magic is refused");

    /* A truncated pack: the index promises a tile the file does not hold. */
    write_pack(p, 4.0, 13.0, 100.0, 0, 0, tile);
    f = fopen(p, "r+b");
    if (f) {
        long n = HDR + ZENT + 4 + TBYTES / 2;
        fclose(f);
        f = fopen("test-tile-e.tiles", "wb");
        if (f) {
            FILE *g = fopen(p, "rb");
            long i;
            for (i = 0; i < n && g; i++) {
                int ch = fgetc(g);
                if (ch == EOF)
                    break;
                fputc(ch, f);
            }
            if (g)
                fclose(g);
            fclose(f);
        }
    }
    check(tiles_open("test-tile-e.tiles", why, (int)sizeof why) == NULL,
          "a truncated pack is refused");
    remove(p);
    remove("test-tile-e.tiles");

    /* A zero-length file, which is what an interrupted copy leaves. */
    f = fopen(p, "wb");
    if (f)
        fclose(f);
    check(tiles_open(p, why, (int)sizeof why) == NULL, "an empty file is refused");
    remove(p);
}

/* Binding to a route frame is a pure translation, and binding to the pack's
 * OWN reference must be exactly the identity -- that is the case that has to
 * be exact, because it is the case every pack built for its own route hits. */
static void
t_bind(void)
{
    static unsigned char tile[TBYTES];
    tiles_t *t;
    tileview_t v;
    const char *p = "test-tile-f.tiles";
    double lat0 = 13.7323109, lon0 = 100.5598316, dn = 100.0;

    memset(tile, 0, sizeof tile);
    set_px(tile, 10, 20);
    write_pack(p, 4.0, lat0, lon0, 0, 0, tile);
    t = tiles_open(p, NULL, 0);
    check(t != NULL, "open pack f");
    if (!t)
        return;

    view_at(&v, 4.0, 0.0, 0.0, 0.0);
    blit(t, &v);
    check(ink_at(&COV, 210, 140), "unbound, the pixel is where it was");
    tiles_bind_route(t, lat0, lon0);
    blit(t, &v);
    check(ink_at(&COV, 210, 140),
          "binding to the pack's own reference is the identity");
    check(ink_count(&COV) == 1, "exactly");

    /* A route referenced 100 m NORTH of the pack: the pack's origin is then
     * at n = -100 in the route frame, so everything in it draws 100 m -- 25
     * px at 4 m/px -- further south on the screen. */
    tiles_bind_route(t, lat0 + dn / 110540.0, lon0);
    blit(t, &v);
    check(ink_at(&COV, 210, 165), "a route 100 m north shifts the pack 25 px down");
    tiles_close(t);
    remove(p);
}

int
main(void)
{
    CV = canvas_new(SCR_W, SCR_H);
    if (!CV) {
        printf("test_tile: out of memory\n");
        return 1;
    }
    t_single_pixel();
    t_rotated_line_is_connected();
    t_declines();
    t_corrupt();
    t_bind();
    t_frame_scale();
    t_magnify();
    t_sparse_index();
    if (failures) {
        printf("test_tile: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_tile: OK\n");
    return 0;
}

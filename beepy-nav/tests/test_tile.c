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
    if (failures) {
        printf("test_tile: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_tile: OK\n");
    return 0;
}

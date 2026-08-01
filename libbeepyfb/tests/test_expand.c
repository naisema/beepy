/* libbeepyfb/tests/test_expand.c -- the panel's two XRGB writers must agree.
 *
 * This tree has three independent 1bpp -> XRGB8888 implementations:
 *
 *   expand()       fbdev.c -> expand.c    the panel path
 *   canvas_dump()  dump.c                 every golden in goldens/
 *   fbplay.c's LUT beepy-nav/fbplay.c     a standalone tool
 *
 * Nothing compared any of them until this file. That matters because the
 * goldens are produced by canvas_dump() and the panel is driven by expand(),
 * so a change to expand() moves what the device displays with no gate at all
 * -- and beepy-vid/DESIGN.md 5.2 wants to replace expand()'s inner loop with a
 * lookup table. This test exists so that change is safe.
 *
 * Two things it deliberately does NOT assume:
 *
 *  - It does not compare only against canvas_dump(). Two implementations that
 *    drift together would pass. The literal byte values are asserted from the
 *    device's own shutdownimage.fb format, independently of both.
 *  - It uses a full-size 400x240 canvas throughout, because that is the only
 *    shape the two agree on: canvas_dump() clips to c->w/c->h and pads a short
 *    canvas with paper, while expand() reads w*h unconditionally. Asserting
 *    equivalence on a short canvas would be asserting a coincidence.
 *
 *     make test-unit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/dump.h"
#include "libbeepyfb/expand.h"

#define LINE_LEN ((size_t)SCR_W * 4)
#define FRAME_SZ (LINE_LEN * SCR_H)
#define REF_PATH "out-expand-ref.fb"

static int failures;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* ---------------------------------------------------------- the contract */

/* The device's own bytes: ink = 00 00 00 ff, paper = ff ff ff ff. Asserted
 * literally so that neither implementation can redefine "black" unilaterally,
 * and so a big-endian port fails here rather than inverting the panel. */
static void
check_pixel(const unsigned char *frame, int x, int y, int ink, const char *what)
{
    const unsigned char *p = frame + (size_t)y * LINE_LEN + (size_t)x * 4;
    unsigned char w0 = ink ? 0x00 : 0xFF;
    if (p[0] != w0 || p[1] != w0 || p[2] != w0 || p[3] != 0xFF) {
        printf("FAIL %s: (%d,%d) is %02x %02x %02x %02x, want %02x %02x %02x ff\n",
               what, x, y, p[0], p[1], p[2], p[3], w0, w0, w0);
        failures++;
    }
}

/* --------------------------------------------------------- equivalence */

static unsigned char *
run_expand(const canvas_t *c)
{
    unsigned char *f = malloc(FRAME_SZ);
    if (!f) {
        printf("FAIL out of memory\n");
        exit(1);
    }
    memset(f, 0xA5, FRAME_SZ); /* poison: every byte must be written */
    expand(c, f, LINE_LEN, SCR_H, SCR_W);
    return f;
}

static unsigned char *
run_dump(const canvas_t *c)
{
    unsigned char *f = malloc(FRAME_SZ);
    FILE *fp;
    if (!f) {
        printf("FAIL out of memory\n");
        exit(1);
    }
    if (canvas_dump(c, REF_PATH) != 0) {
        printf("FAIL canvas_dump wrote nothing\n");
        failures++;
        memset(f, 0, FRAME_SZ);
        return f;
    }
    fp = fopen(REF_PATH, "rb");
    if (!fp || fread(f, 1, FRAME_SZ, fp) != FRAME_SZ) {
        printf("FAIL could not read back %s\n", REF_PATH);
        failures++;
        memset(f, 0, FRAME_SZ);
    }
    if (fp)
        fclose(fp);
    return f;
}

/* Compare, and on a mismatch say WHICH pixel -- a byte offset alone is not
 * actionable when the frame is 384000 bytes. */
static void
same(const canvas_t *c, const char *what)
{
    unsigned char *a = run_expand(c);
    unsigned char *b = run_dump(c);
    for (size_t i = 0; i < FRAME_SZ; i++) {
        if (a[i] != b[i]) {
            size_t y = i / LINE_LEN, x = (i % LINE_LEN) / 4;
            printf("FAIL %s: first difference at byte %zu = pixel (%zu,%zu) "
                   "byte %zu: expand %02x, canvas_dump %02x\n",
                   what, i, x, y, i % 4, a[i], b[i]);
            failures++;
            break;
        }
    }
    free(a);
    free(b);
}

/* ------------------------------------------------------------- patterns */

static canvas_t *
fresh(int ink)
{
    canvas_t *c = canvas_new(SCR_W, SCR_H);
    if (!c) {
        printf("FAIL canvas_new\n");
        exit(1);
    }
    canvas_clear(c, ink);
    return c;
}

int
main(void)
{
    canvas_t *c;
    unsigned char *f;

    /* 1. The literal byte contract, both ways round. */
    c = fresh(PAPER);
    f = run_expand(c);
    check_pixel(f, 0, 0, 0, "all-paper (0,0)");
    check_pixel(f, SCR_W - 1, SCR_H - 1, 0, "all-paper (399,239)");
    free(f);
    same(c, "all paper");
    free(c);

    c = fresh(INK);
    f = run_expand(c);
    check_pixel(f, 0, 0, 1, "all-ink (0,0)");
    check_pixel(f, SCR_W - 1, SCR_H - 1, 1, "all-ink (399,239)");
    free(f);
    same(c, "all ink");
    free(c);

    /* 2. Bit order inside a byte. MSB is the leftmost pixel (canvas.h:14), so
     *    a single pixel at x=n must light exactly x=n and nothing else. This
     *    is the assertion that catches an MSB/LSB flip, which is invisible in
     *    a photograph and inverts fine detail. */
    for (int n = 0; n < 8; n++) {
        char what[64];
        c = fresh(PAPER);
        px(c, n, 5, INK);
        f = run_expand(c);
        snprintf(what, sizeof what, "single pixel x=%d is ink", n);
        check_pixel(f, n, 5, 1, what);
        for (int o = 0; o < 8; o++) {
            if (o == n)
                continue;
            snprintf(what, sizeof what, "single pixel x=%d leaves x=%d paper", n, o);
            check_pixel(f, o, 5, 0, what);
        }
        free(f);
        snprintf(what, sizeof what, "single pixel x=%d", n);
        same(c, what);
        free(c);
    }

    /* 3. The four corners -- catches a stride or off-by-one at either end. */
    c = fresh(PAPER);
    px(c, 0, 0, INK);
    px(c, SCR_W - 1, 0, INK);
    px(c, 0, SCR_H - 1, INK);
    px(c, SCR_W - 1, SCR_H - 1, INK);
    f = run_expand(c);
    check_pixel(f, 0, 0, 1, "corner tl");
    check_pixel(f, SCR_W - 1, 0, 1, "corner tr");
    check_pixel(f, 0, SCR_H - 1, 1, "corner bl");
    check_pixel(f, SCR_W - 1, SCR_H - 1, 1, "corner br");
    check_pixel(f, 1, 0, 0, "corner tl neighbour still paper");
    free(f);
    same(c, "four corners");
    free(c);

    /* 4. A 1px checkerboard: worst case for bit packing, and the pattern that
     *    would expose a whole-byte shortcut. */
    c = fresh(PAPER);
    for (int y = 0; y < SCR_H; y++)
        for (int x = 0; x < SCR_W; x++)
            if ((x + y) & 1)
                px(c, x, y, INK);
    same(c, "1px checkerboard");
    free(c);

    /* 5. Mixed geometry, so the comparison is not only over regular patterns. */
    c = fresh(PAPER);
    fillrect(c, 10, 10, 137, 61, INK);
    rect(c, 200, 30, 91, 45, INK);
    disc(c, 320, 170, 37, INK);
    hline(c, 0, 239, SCR_W, INK);
    vline(c, 399, 0, SCR_H, INK);
    checker(c, 40, 120, 83, 57);
    same(c, "mixed geometry");
    free(c);

    /* 6. Anti-vacuity. Without this, an expand() that wrote a constant frame
     *    -- or a canvas_dump() that did -- passes every assertion above.
     *    Two different canvases must not produce the same bytes. */
    {
        canvas_t *a = fresh(PAPER);
        canvas_t *b = fresh(PAPER);
        unsigned char *fa, *fb;
        px(b, 200, 120, INK); /* one pixel of difference, in the middle */
        fa = run_expand(a);
        fb = run_expand(b);
        check(memcmp(fa, fb, FRAME_SZ) != 0,
              "expand distinguishes canvases differing by one pixel");
        free(fa);
        free(fb);
        fa = run_dump(a);
        fb = run_dump(b);
        check(memcmp(fa, fb, FRAME_SZ) != 0,
              "canvas_dump distinguishes canvases differing by one pixel");
        free(fa);
        free(fb);
        free(a);
        free(b);
    }

    remove(REF_PATH);

    if (failures) {
        printf("test_expand: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_expand: PASS\n");
    return 0;
}

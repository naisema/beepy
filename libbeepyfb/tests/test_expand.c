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

/* The pre-M1 implementation, kept here as the reference the lookup table must
 * reproduce. Written with explicit bytes rather than a uint32 store so that
 * the reference itself is endianness-neutral; the original used a uint32 and
 * that difference is the latent bug the table also fixed. */
static void
ref_expand(const canvas_t *c, unsigned char *frame, size_t line_len, int h, int w)
{
    for (int y = 0; y < h; y++) {
        unsigned char *row = frame + (size_t)y * line_len;
        const unsigned char *src = &c->bits[(size_t)y * c->stride];
        for (int x = 0; x < w; x++) {
            int ink = src[x / 8] & (0x80u >> (x % 8));
            row[4 * x] = row[4 * x + 1] = row[4 * x + 2] = ink ? 0x00 : 0xFF;
            row[4 * x + 3] = 0xFF;
        }
    }
}

/* The table must reproduce the loop it replaced, byte for byte. Without this
 * the only guard on the optimisation is canvas_dump(), and a table that was
 * wrong in the same way as a rewritten canvas_dump() would pass. */
static void
same_as_ref(const canvas_t *c, const char *what)
{
    unsigned char *a = run_expand(c);
    unsigned char *b = malloc(FRAME_SZ);
    if (!b) {
        printf("FAIL out of memory\n");
        exit(1);
    }
    memset(b, 0x5A, FRAME_SZ);
    ref_expand(c, b, LINE_LEN, SCR_H, SCR_W);
    for (size_t i = 0; i < FRAME_SZ; i++) {
        if (a[i] != b[i]) {
            size_t y = i / LINE_LEN, x = (i % LINE_LEN) / 4;
            printf("FAIL %s (LUT vs reference): pixel (%zu,%zu) byte %zu: "
                   "lut %02x, reference %02x\n", what, x, y, i % 4, a[i], b[i]);
            failures++;
            break;
        }
    }
    free(a);
    free(b);
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
    /* Every pattern is checked against BOTH the golden writer and the loop the
     * lookup table replaced, so neither can drift alone. */
    same_as_ref(c, what);
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

    /* 7. expand_rows() must touch exactly the rows asked for and no others.
     *    The poison byte is what proves the "no others" half -- without it a
     *    row range that quietly expanded to the whole frame would pass. */
    {
        canvas_t *full = fresh(INK);
        unsigned char *f2 = malloc(FRAME_SZ);
        const int Y0 = 100, Y1 = 139; /* the 40-row transient band of DESIGN 6.2 */
        int clean = 1;
        if (!f2) {
            printf("FAIL out of memory\n");
            exit(1);
        }
        memset(f2, 0xA5, FRAME_SZ);
        expand_rows(full, f2, LINE_LEN, Y0, Y1, SCR_W);
        for (int y = 0; y < SCR_H && clean; y++) {
            for (size_t b = 0; b < LINE_LEN; b++) {
                unsigned char got = f2[(size_t)y * LINE_LEN + b];
                int inside = (y >= Y0 && y <= Y1);
                unsigned char want = inside ? ((b % 4) == 3 ? 0xFF : 0x00) : 0xA5;
                if (got != want) {
                    printf("FAIL expand_rows row %d byte %zu: got %02x want %02x "
                           "(%s the range)\n", y, b, got, want,
                           inside ? "inside" : "outside");
                    failures++;
                    clean = 0;
                    break;
                }
            }
        }
        /* And the same rows must match a full expand of the same canvas. */
        {
            unsigned char *fw = run_expand(full);
            check(memcmp(fw + (size_t)Y0 * LINE_LEN, f2 + (size_t)Y0 * LINE_LEN,
                         (size_t)(Y1 - Y0 + 1) * LINE_LEN) == 0,
                  "expand_rows band equals the same band of a full expand");
            free(fw);
        }
        free(f2);
        free(full);
    }

    /* 8. row_span_clamp(): the single-row rule is not tidiness. A write of
     *    exactly one line length yields a zero-height damage rect and the
     *    driver performs no update at all, so a one-row present would be
     *    silently dropped -- measured, beepy-vid/DESIGN.md 0.3. */
    {
        int y0, y1;
        y0 = 10; y1 = 20;
        check(row_span_clamp(240, &y0, &y1) == 1 && y0 == 10 && y1 == 20,
              "clamp leaves an ordinary range alone");
        y0 = 50; y1 = 50;
        check(row_span_clamp(240, &y0, &y1) == 1 && y0 == 50 && y1 == 51,
              "clamp widens a single row DOWNWARDS, keeping the asked-for row first");
        y0 = 239; y1 = 239;
        check(row_span_clamp(240, &y0, &y1) == 1 && y0 == 238 && y1 == 239,
              "clamp widens the LAST row downwards, not off the panel");
        y0 = -5; y1 = 300;
        check(row_span_clamp(240, &y0, &y1) == 1 && y0 == 0 && y1 == 239,
              "clamp trims an out-of-range span to the panel");
        y0 = 100; y1 = 40;
        check(row_span_clamp(240, &y0, &y1) == 0,
              "an inverted range presents nothing");
        y0 = 0; y1 = 0;
        check(row_span_clamp(1, &y0, &y1) == 0,
              "a one-row panel cannot be presented at all");
    }

    remove(REF_PATH);

    if (failures) {
        printf("test_expand: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_expand: PASS\n");
    return 0;
}

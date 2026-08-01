/* libbeepyfb/tests/test_present_rows.c -- does fb_present_rows() land the band where it claims?
 *
 * Nothing else can answer this. Every golden goes through canvas_dump(), so a
 * pwrite() at the wrong offset would paint the band in the wrong place on the
 * panel and `make check` would still be byte-perfect. Here the framebuffer is
 * read back and compared against what expand_rows() should have produced.
 */
#define _XOPEN_SOURCE 500  /* pread */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/fbdev.h"
#include "libbeepyfb/expand.h"

static int failures;

static void ck(int ok, const char *what)
{
    printf("%-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

int main(void)
{
    fb_t f;
    canvas_t *all_paper, *all_ink;
    unsigned char *back, *want;
    const int Y0 = 100, Y1 = 139;

    if (fb_open(&f, "/dev/fb1") < 0)
        return 1;
    fb_take(&f);

    all_paper = canvas_new(SCR_W, SCR_H);
    all_ink = canvas_new(SCR_W, SCR_H);
    canvas_clear(all_paper, PAPER);
    canvas_clear(all_ink, INK);
    back = malloc(f.frame_sz);
    want = malloc(f.frame_sz);

    /* 1. Whole frame paper, then an ink band via fb_present_rows(). */
    if (fb_present(&f, all_paper) != 0) { printf("fb_present failed\n"); return 1; }
    if (fb_present_rows(&f, all_ink, Y0, Y1) != 0) { printf("rows failed\n"); return 1; }

    if (pread(f.fd, back, f.frame_sz, 0) != (ssize_t)f.frame_sz) {
        printf("readback failed\n");
        return 1;
    }

    /* Build what it should be: paper everywhere, ink in Y0..Y1. */
    expand(all_paper, want, f.line_len, f.h, f.w);
    expand_rows(all_ink, want, f.line_len, Y0, Y1, f.w);
    ck(memcmp(back, want, f.frame_sz) == 0, "band lands at exactly rows 100..139");

    /* Say where, if not. */
    if (memcmp(back, want, f.frame_sz) != 0) {
        for (int y = 0; y < f.h; y++) {
            if (memcmp(back + (size_t)y * f.line_len,
                       want + (size_t)y * f.line_len, f.line_len) != 0) {
                printf("    first wrong row: %d\n", y);
                break;
            }
        }
    }

    /* 2. Rows outside the band must be untouched by the partial present. */
    {
        int clean = 1;
        for (int y = 0; y < f.h; y++) {
            if (y >= Y0 && y <= Y1)
                continue;
            const unsigned char *p = back + (size_t)y * f.line_len;
            for (int x = 0; x < f.w; x++)
                if (p[4 * x] != 0xFF) { clean = 0; break; }
            if (!clean) { printf("    row %d was disturbed\n", y); break; }
        }
        ck(clean, "rows outside the band are untouched");
    }

    /* 3. A single-row request must still update the panel (it is widened). */
    if (fb_present(&f, all_paper) != 0) return 1;
    ck(fb_present_rows(&f, all_ink, 77, 77) == 0, "single-row present returns ok");
    if (pread(f.fd, back, f.frame_sz, 0) == (ssize_t)f.frame_sz) {
        const unsigned char *r77 = back + (size_t)77 * f.line_len;
        const unsigned char *r78 = back + (size_t)78 * f.line_len;
        ck(r77[0] == 0x00, "row 77 became ink");
        ck(r78[0] == 0x00, "row 78 also written (widened, so the driver updates)");
    }

    /* 4. An empty range must be a no-op, not a crash or a full repaint. */
    if (fb_present(&f, all_paper) != 0) return 1;
    ck(fb_present_rows(&f, all_ink, 90, 40) == 0, "inverted range returns ok");
    if (pread(f.fd, back, f.frame_sz, 0) == (ssize_t)f.frame_sz)
        ck(back[(size_t)90 * f.line_len] == 0xFF, "inverted range wrote nothing");

    /* Leave the panel as we found it. */
    fb_present(&f, all_paper);
    fb_release(&f);
    printf(failures ? "test_present_rows: %d FAILURES\n" : "test_present_rows: PASS\n", failures);
    return failures ? 1 : 0;
}

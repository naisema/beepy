/* beepy-vid/tests/test_codec.c -- the four .vid encodings, as arithmetic.
 *
 * codec.c takes a payload and a plane and touches nothing else, so all of this
 * runs on the Mac with no file, no device and no pack. The refusals matter as
 * much as the round trips: a .vid is a file some tool wrote onto an SD card,
 * and a malformed one must be rejected rather than read outside the plane.
 *
 *     make test-unit
 */
#include <stdio.h>
#include <string.h>

#include "codec.h"

#define STRIDE 8
#define HEIGHT 8
#define PLANE (STRIDE * HEIGHT)

static int failures;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* A plane with a recognisable value per byte, so a misplaced write shows up as
 * the wrong number rather than as a plausible one. */
static void
fill(unsigned char *p)
{
    for (int i = 0; i < PLANE; i++)
        p[i] = (unsigned char)(0x10 + i);
}

static int
untouched_outside(const unsigned char *got, int y0, int y1)
{
    unsigned char ref[PLANE];
    fill(ref);
    for (int y = 0; y < HEIGHT; y++) {
        if (y >= y0 && y <= y1)
            continue;
        if (memcmp(got + y * STRIDE, ref + y * STRIDE, STRIDE) != 0)
            return 0;
    }
    return 1;
}

static void
test_raw(void)
{
    unsigned char plane[PLANE], pay[STRIDE * 3];
    memset(pay, 0xAB, sizeof pay);

    fill(plane);
    check(codec_raw(plane, STRIDE, HEIGHT, 2, 4, pay, sizeof pay) == 0,
          "raw: a correctly sized band is accepted");
    for (int y = 2; y <= 4; y++)
        for (int x = 0; x < STRIDE; x++)
            check(plane[y * STRIDE + x] == 0xAB, "raw: band written");
    check(untouched_outside(plane, 2, 4), "raw: rows outside the band untouched");

    fill(plane);
    check(codec_raw(plane, STRIDE, HEIGHT, 2, 4, pay, sizeof pay - 1) == -1,
          "raw: a payload one byte short is refused");
    check(codec_raw(plane, STRIDE, HEIGHT, 2, 4, pay, sizeof pay + 1) == -1,
          "raw: a payload one byte long is refused");
    check(codec_raw(plane, STRIDE, HEIGHT, 6, HEIGHT, pay, STRIDE * 3) == -1,
          "raw: y1 == height is one row too far and is refused");
    check(codec_raw(plane, STRIDE, HEIGHT, 4, 2, pay, sizeof pay) == -1,
          "raw: an inverted row span is refused");
    check(codec_raw(plane, STRIDE, HEIGHT, -1, 2, pay, sizeof pay) == -1,
          "raw: a negative y0 is refused");
    check(untouched_outside(plane, -1, -1),
          "raw: nothing was written by any refused call");
}

static void
test_xor_band(void)
{
    unsigned char plane[PLANE], pay[STRIDE * 2], before[PLANE];
    memset(pay, 0xFF, sizeof pay);

    fill(plane);
    fill(before);
    check(codec_xor_band(plane, STRIDE, HEIGHT, 1, 2, pay, sizeof pay) == 0,
          "xor_band: accepted");
    for (int y = 1; y <= 2; y++)
        for (int x = 0; x < STRIDE; x++)
            check(plane[y * STRIDE + x] == (before[y * STRIDE + x] ^ 0xFF),
                  "xor_band: bytes XORed");
    check(untouched_outside(plane, 1, 2), "xor_band: outside untouched");

    /* XOR is its own inverse -- applying twice must restore exactly. */
    check(codec_xor_band(plane, STRIDE, HEIGHT, 1, 2, pay, sizeof pay) == 0,
          "xor_band: second application accepted");
    check(memcmp(plane, before, PLANE) == 0,
          "xor_band: applying the same delta twice restores the plane");

    check(codec_xor_band(plane, STRIDE, HEIGHT, 1, 2, pay, sizeof pay - 1) == -1,
          "xor_band: wrong size refused");
}

/* Build a span payload: nrow, then {y, x0, n, bytes...}. */
static size_t
span(unsigned char *out, int nrow, const int *y, const int *x0,
     const int *cnt, const unsigned char *vals)
{
    size_t at = 2;
    out[0] = (unsigned char)(nrow & 0xFF);
    out[1] = (unsigned char)(nrow >> 8);
    for (int r = 0; r < nrow; r++) {
        out[at++] = (unsigned char)y[r];
        out[at++] = (unsigned char)x0[r];
        out[at++] = (unsigned char)cnt[r];
        for (int i = 0; i < cnt[r]; i++)
            out[at++] = vals[r];
    }
    return at;
}

static void
test_spans(void)
{
    unsigned char plane[PLANE], before[PLANE], pay[64];
    int y[3], x0[3], cnt[3];
    unsigned char vals[3] = { 0x0F, 0xF0, 0x33 };
    size_t n;

    /* Two disjoint spans on the SAME row -- the encoder splits a row when the
     * gap exceeds the 3-byte entry overhead, so this is a normal payload and
     * not an edge case. */
    y[0] = 3; x0[0] = 0; cnt[0] = 2;
    y[1] = 3; x0[1] = 6; cnt[1] = 2;
    y[2] = 5; x0[2] = 1; cnt[2] = 1;
    n = span(pay, 3, y, x0, cnt, vals);

    fill(plane);
    fill(before);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n) == 0,
          "spans: accepted");
    check(plane[3 * STRIDE + 0] == (before[3 * STRIDE + 0] ^ 0x0F) &&
          plane[3 * STRIDE + 1] == (before[3 * STRIDE + 1] ^ 0x0F),
          "spans: first span on row 3 applied");
    check(plane[3 * STRIDE + 6] == (before[3 * STRIDE + 6] ^ 0xF0) &&
          plane[3 * STRIDE + 7] == (before[3 * STRIDE + 7] ^ 0xF0),
          "spans: second span on the same row applied");
    check(plane[3 * STRIDE + 3] == before[3 * STRIDE + 3],
          "spans: the gap between two spans on one row is untouched");
    check(plane[5 * STRIDE + 1] == (before[5 * STRIDE + 1] ^ 0x33),
          "spans: span on row 5 applied");
    {   /* every row except 3 and 5 must be exactly as it was */
        int clean = 1;
        for (int r = 0; r < HEIGHT; r++) {
            if (r == 3 || r == 5)
                continue;
            if (memcmp(plane + r * STRIDE, before + r * STRIDE, STRIDE) != 0)
                clean = 0;
        }
        check(clean, "spans: rows with no span are untouched");
    }

    /* nrow == 0 is a legal empty delta: two bytes, nothing applied. */
    fill(plane);
    pay[0] = pay[1] = 0;
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, 2) == 0,
          "spans: an empty payload is legal");
    check(memcmp(plane, before, PLANE) == 0,
          "spans: an empty payload changes nothing");

    /* Refusals. Each one is a way a corrupt pack could read out of bounds. */
    fill(plane);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, 1) == -1,
          "spans: a payload shorter than the count is refused");

    y[0] = 3; x0[0] = 0; cnt[0] = 0;
    n = span(pay, 1, y, x0, cnt, vals);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n) == -1,
          "spans: a zero-length span is refused");

    y[0] = HEIGHT; x0[0] = 0; cnt[0] = 1;
    n = span(pay, 1, y, x0, cnt, vals);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n) == -1,
          "spans: a row past the bottom of the plane is refused");

    y[0] = 0; x0[0] = STRIDE - 1; cnt[0] = 2;
    n = span(pay, 1, y, x0, cnt, vals);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n) == -1,
          "spans: a span running off the right of the row is refused");

    y[0] = 0; x0[0] = STRIDE; cnt[0] = 1;
    n = span(pay, 1, y, x0, cnt, vals);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n) == -1,
          "spans: x0 at the stride is refused");

    y[0] = 0; x0[0] = 0; cnt[0] = 4;
    n = span(pay, 1, y, x0, cnt, vals);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n - 2) == -1,
          "spans: a truncated span payload is refused");

    y[0] = 0; x0[0] = 0; cnt[0] = 2;
    n = span(pay, 1, y, x0, cnt, vals);
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n + 3) == -1,
          "spans: trailing bytes after the last span are refused");

    /* nrow says two, only one is present. */
    y[0] = 0; x0[0] = 0; cnt[0] = 1;
    n = span(pay, 1, y, x0, cnt, vals);
    pay[0] = 2;
    check(codec_xor_spans(plane, STRIDE, HEIGHT, pay, n) == -1,
          "spans: a count larger than the entries present is refused");

    check(memcmp(plane, before, PLANE) == 0,
          "spans: no refused payload modified the plane before failing");
}

int
main(void)
{
    test_raw();
    test_xor_band();
    test_spans();
    if (failures) {
        printf("test_codec: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_codec: PASS\n");
    return 0;
}

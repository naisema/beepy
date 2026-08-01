/* beepy-vid/src/codec.c -- see codec.h. */
#include <string.h>

#include "libbeepyfb/le.h"

#include "codec.h"

/* Rows y0..y1 must lie inside the plane and be in order. y1 is inclusive, so
 * y1 == height is one row too far -- the off-by-one this checks for is the
 * whole reason the bound is written out rather than inlined three times. */
static int
band_ok(int stride, int height, int y0, int y1, size_t n)
{
    if (stride <= 0 || height <= 0)
        return 0;
    if (y0 < 0 || y1 < y0 || y1 >= height)
        return 0;
    return n == (size_t)(y1 - y0 + 1) * (size_t)stride;
}

int
codec_raw(unsigned char *plane, int stride, int height,
          int y0, int y1, const unsigned char *p, size_t n)
{
    if (!band_ok(stride, height, y0, y1, n))
        return -1;
    memcpy(plane + (size_t)y0 * stride, p, n);
    return 0;
}

int
codec_xor_band(unsigned char *plane, int stride, int height,
               int y0, int y1, const unsigned char *p, size_t n)
{
    unsigned char *dst;
    if (!band_ok(stride, height, y0, y1, n))
        return -1;
    dst = plane + (size_t)y0 * stride;
    for (size_t i = 0; i < n; i++)
        dst[i] ^= p[i];
    return 0;
}

/* Walk the payload without touching the plane. Returns 0 if every entry is
 * in bounds and the payload is exactly consumed. */
static int
spans_valid(int stride, int height, const unsigned char *p, size_t n)
{
    size_t at = 2;
    unsigned nrow;

    if (stride <= 0 || height <= 0 || n < 2)
        return -1;
    nrow = rd_u16(p);
    for (unsigned r = 0; r < nrow; r++) {
        unsigned y, x0, cnt;
        if (at + 3 > n) /* header of this entry does not fit */
            return -1;
        y = p[at];
        x0 = p[at + 1];
        cnt = p[at + 2];
        at += 3;
        if (cnt == 0)
            return -1; /* would encode nothing and cost three bytes */
        if (y >= (unsigned)height)
            return -1;
        if (x0 >= (unsigned)stride || x0 + cnt > (unsigned)stride)
            return -1;
        if (at + cnt > n) /* payload of this entry does not fit */
            return -1;
        at += cnt;
    }
    /* Trailing bytes mean the writer and the reader disagree about the format,
     * which is exactly the case a version field cannot catch. Refuse. */
    return at == n ? 0 : -1;
}

int
codec_xor_spans(unsigned char *plane, int stride, int height,
                const unsigned char *p, size_t n)
{
    size_t at = 2;
    unsigned nrow;

    /* Validated in full before a single byte is applied, so a refused payload
     * leaves the plane exactly as it was. Applying as we go would corrupt the
     * XOR reference on the way to returning -1, and the reference IS the
     * previous frame -- the damage would outlive the bad frame and show up
     * later as a decode that cannot be explained by the frame it came from. */
    if (spans_valid(stride, height, p, n) != 0)
        return -1;

    nrow = rd_u16(p);
    for (unsigned r = 0; r < nrow; r++) {
        unsigned y = p[at], x0 = p[at + 1], cnt = p[at + 2];
        unsigned char *dst = plane + (size_t)y * stride + x0;
        at += 3;
        for (unsigned i = 0; i < cnt; i++)
            dst[i] ^= p[at + i];
        at += cnt;
    }
    return 0;
}

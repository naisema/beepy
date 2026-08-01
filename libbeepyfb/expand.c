/* libbeepyfb/expand.c -- see expand.h.
 *
 * Split out of fbdev.c in M0 so it could be compared against canvas_dump() on
 * the Mac; the inner loop became a lookup table in M1.
 *
 * The table is beepy-nav/fbplay.c's idea (fbplay.c:26,44-46): one 256 x 32
 * byte entry per source byte, so eight pixels become one 32-byte memcpy
 * instead of eight shift-mask-store rounds. Measured on the device at 225
 * rows: 0.750 ms -> 0.163 ms, 4.59x. At 24 fps that returns 14 ms of CPU per
 * second, which matters because the panel's own SPI transfer already occupies
 * 67% of the bus at that rate.
 *
 * The table also removes a latent bug rather than merely going faster. The
 * old loop stored a uint32, so on a big-endian host it would have emitted
 * ff 00 00 00 where canvas_dump() -- which writes the four bytes explicitly
 * and documents itself as "identical on any endianness" (dump.c:6) -- emits
 * 00 00 00 ff. The table is built byte by byte, so the two now agree on any
 * host. test_expand asserts the literal byte order either way.
 */
#include <string.h>

#include "expand.h"

/* Eight pixels of XRGB per source byte. MSB is the leftmost pixel
 * (canvas.h:14), matching canvas_dump()'s 0x80u >> (x % 8). */
static unsigned char lut[256][32];
static int lut_ready;

static void
lut_init(void)
{
    for (int v = 0; v < 256; v++) {
        for (int b = 0; b < 8; b++) {
            unsigned char *p = &lut[v][b * 4];
            int ink = (v >> (7 - b)) & 1;
            p[0] = p[1] = p[2] = ink ? 0x00 : 0xFF;
            p[3] = 0xFF;
        }
    }
    lut_ready = 1;
}

void expand(const canvas_t *c, unsigned char *frame, size_t line_len, int h, int w)
{
    expand_rows(c, frame, line_len, 0, h - 1, w);
}

void expand_rows(const canvas_t *c, unsigned char *frame, size_t line_len,
                 int y0, int y1, int w)
{
    int full = w / 8; /* whole source bytes, 50 of them at SCR_W */
    if (!lut_ready)
        lut_init();
    for (int y = y0; y <= y1; y++) {
        unsigned char *dst = frame + (size_t)y * line_len;
        const unsigned char *src = &c->bits[(size_t)y * c->stride];
        for (int i = 0; i < full; i++, dst += 32)
            memcpy(dst, lut[src[i]], 32);
        /* A width that is not a multiple of 8 finishes a pixel at a time.
         * SCR_W is 400 so this never runs on the panel, but expand() takes a
         * width and must not quietly corrupt a caller that passes an odd one. */
        for (int x = full * 8; x < w; x++, dst += 4) {
            int ink = src[x / 8] & (0x80u >> (x % 8));
            dst[0] = dst[1] = dst[2] = ink ? 0x00 : 0xFF;
            dst[3] = 0xFF;
        }
    }
}

int row_span_clamp(int h, int *y0, int *y1)
{
    if (h <= 0)
        return 0;
    if (*y0 < 0)
        *y0 = 0;
    if (*y1 > h - 1)
        *y1 = h - 1;
    if (*y1 < *y0)
        return 0;
    if (*y1 == *y0) { /* one row would be dropped by the driver -- widen it */
        if (*y1 + 1 <= h - 1)
            (*y1)++;
        else if (*y0 > 0)
            (*y0)--;
        else
            return 0; /* a one-row panel cannot be presented at all */
    }
    return 1;
}

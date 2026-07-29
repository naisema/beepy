/* libbeepyfb/dump.c -- see dump.h.
 *
 * Byte format is the device's own: black = 00 00 00 ff, white = ff ff ff ff
 * (XRGB8888 little-endian with the unused byte set), 400 * 240 * 4 bytes,
 * matching /dev/fb1's 1600-byte stride. Written out as explicit bytes
 * rather than as uint32, so the file is identical on any endianness.
 */
#include <stdio.h>

#include "dump.h"

int
canvas_dump(const canvas_t *c, const char *path)
{
    unsigned char row[SCR_W * 4];
    FILE *f = fopen(path, "wb");
    int x, y;

    if (!f)
        return -1;
    for (y = 0; y < SCR_H; y++) {
        /* A short canvas pads with paper rather than reading past its end. */
        const unsigned char *src =
            y < c->h ? &c->bits[(size_t)y * c->stride] : NULL;
        for (x = 0; x < SCR_W; x++) {
            int ink = src && x < c->w && (src[x / 8] & (0x80u >> (x % 8)));
            row[4 * x] = ink ? 0x00 : 0xFF;
            row[4 * x + 1] = ink ? 0x00 : 0xFF;
            row[4 * x + 2] = ink ? 0x00 : 0xFF;
            row[4 * x + 3] = 0xFF;
        }
        if (fwrite(row, 1, sizeof row, f) != sizeof row) {
            fclose(f);
            return -1;
        }
    }
    return fclose(f) == 0 ? 0 : -1;
}

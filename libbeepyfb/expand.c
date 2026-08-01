/* libbeepyfb/expand.c -- see expand.h.
 *
 * Moved here from fbdev.c verbatim (M0). The only reason it is its own file is
 * that fbdev.c cannot be compiled on the Mac, and this needs to be, so that
 * test_expand runs in the fast lane instead of costing a 25-minute `make
 * check` per iteration. No behaviour change: this milestone exists to pin the
 * current output before M1 replaces the inner loop with a lookup table.
 *
 * A note on endianness, because the sibling implementation makes a promise
 * this one does not: dump.c writes the four bytes explicitly and documents
 * itself as "identical on any endianness" (dump.c:6). This stores a uint32, so
 * on a big-endian host it would emit ff 00 00 00 where dump.c emits
 * 00 00 00 ff. Both targets here are little-endian -- armv7l and arm64 -- so
 * the divergence is latent rather than live, and test_expand asserts the
 * literal byte order so that a port would fail loudly here rather than
 * silently invert the panel.
 */
#include <stdint.h>

#include "expand.h"

void expand(const canvas_t *c, unsigned char *frame, size_t line_len, int h, int w)
{
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(frame + (size_t)y * line_len);
        const unsigned char *src = &c->bits[(size_t)y * c->stride];
        for (int x = 0; x < w; x++)
            row[x] = (src[x / 8] & (0x80u >> (x % 8))) ? 0xFF000000u : 0xFFFFFFFFu;
    }
}

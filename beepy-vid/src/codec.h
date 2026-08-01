/* beepy-vid/src/codec.h -- the four .vid frame encodings, as pure arithmetic.
 *
 * Separate from pack.c so it can be unit tested with no file at all: every
 * function here takes a payload and a plane and touches nothing else. The
 * plane is exactly canvas_t's layout (canvas.h:12-15) -- row major, stride
 * bytes per row, MSB leftmost, a set bit is INK -- so a decoded frame can be
 * memcpy'd straight into cv->bits and presented.
 *
 * Every function validates its payload and returns -1 rather than reading or
 * writing outside the plane. A .vid pack is a file on an SD card that some
 * tool on a Mac wrote; it is not trusted input.
 */
#ifndef BEEPYVID_CODEC_H
#define BEEPYVID_CODEC_H

#include <stddef.h>

/* Index entry mode byte. See beepy-vid/DESIGN.md 3.4. */
#define CODEC_RAW 0
#define CODEC_DEFLATE 1
#define CODEC_XOR_SPANS 2
#define CODEC_XOR_DEFLATE 3

/* Overwrite rows y0..y1 with the payload, which must be exactly
 * (y1-y0+1)*stride bytes. */
int codec_raw(unsigned char *plane, int stride, int height,
              int y0, int y1, const unsigned char *p, size_t n);

/* XOR the payload into rows y0..y1. Same length rule as codec_raw. */
int codec_xor_band(unsigned char *plane, int stride, int height,
                   int y0, int y1, const unsigned char *p, size_t n);

/* Apply per-row byte spans:
 *
 *     u16 nrow
 *     nrow x { u8 y, u8 x0, u8 n, u8 v[n] }   applied as
 *                                             plane[y*stride + x0 + i] ^= v[i]
 *
 * A row may appear more than once with disjoint spans -- the encoder splits a
 * row when the gap between dirty bytes exceeds the 3-byte entry overhead. n==0
 * is illegal, because it would encode nothing while costing three bytes and
 * would let a malformed pack spin.
 */
int codec_xor_spans(unsigned char *plane, int stride, int height,
                    const unsigned char *p, size_t n);

#endif /* BEEPYVID_CODEC_H */

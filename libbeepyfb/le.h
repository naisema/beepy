/* libbeepyfb/le.h -- little-endian field reads for the pack formats.
 *
 * The argument is beepy-nav/src/tile.c:94-98's, written once here instead of
 * once per pack reader: a pack is little-endian by definition and the reader
 * must not silently depend on the host agreeing, nor on an aligned load being
 * legal at an arbitrary offset. Reading byte at a time costs nothing measurable
 * against an SD read and is correct everywhere.
 *
 * Header-only static inline, so this adds no object to FB_OBJS.
 *
 * tile.c and search.c still carry their own copies. Unifying them means
 * touching beepy-nav's pack readers, which is a change with goldens behind it
 * and no reason to make it in the same commit as a new format.
 */
#ifndef BEEPYFB_LE_H
#define BEEPYFB_LE_H

#include <stdint.h>

static inline uint16_t
rd_u16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t
rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t
rd_i32(const unsigned char *p)
{
    return (int32_t)rd_u32(p);
}

#endif /* BEEPYFB_LE_H */

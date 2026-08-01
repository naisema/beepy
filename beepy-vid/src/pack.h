/* beepy-vid/src/pack.h -- the .vid container reader.
 *
 * Format: beepy-vid/DESIGN.md 3. Little-endian, 64-byte header, a 3-entry
 * section table at 64, sections from 88: INDEX (12 bytes per frame), FRAMES (a
 * payload arena) and AUDIO (contiguous interleaved S16LE, possibly empty).
 *
 * Deliberately linkable on its own, for the reason Makefile:1444-1449 gives
 * about test_tile: a test that can build a pack byte by byte in C does not
 * have to shell out to the Python packer, so the reader is gated without the
 * writer being involved.
 *
 * Everything here validates. A pack is a file some tool on a Mac wrote onto an
 * SD card; a truncated or hostile one must be refused at open, not discovered
 * at frame 4800. The one refusal that is NOT deferred to first use is
 * PACKF_DEFLATE: a reader built without zlib says so when the pack is opened.
 */
#ifndef BEEPYVID_PACK_H
#define BEEPYVID_PACK_H

#include <stdint.h>
#include <stdio.h>
#include <stddef.h>

#define VID_MAGIC "BEEPYVID"
#define VID_VERSION 1
#define VID_HEADER_BYTES 64
#define VID_NSECT 3
#define VID_INDEX_ENTRY 12

/* header flags */
#define PACKF_INK_MSB 0x0001 /* rows MSB-first, set bit = ink. Always set. */
#define PACKF_DEFLATE 0x0002 /* some frame uses mode 1 or 3 */
#define PACKF_AUDIO 0x0004

/* index entry flags */
#define FRAMEF_KEY 0x01

typedef struct {
    uint32_t off;  /* absolute file offset of the payload */
    uint32_t size; /* payload bytes; 0 means "same as the previous frame" */
    uint8_t mode;  /* CODEC_* */
    uint8_t flags; /* FRAMEF_* */
    uint8_t y0, y1; /* rows this frame may change, y1 inclusive */
} vidx_t;

typedef struct {
    FILE *fp;
    long file_bytes;

    uint16_t version, flags, width, height;
    uint32_t nframe, fps_num, fps_den, gop;
    uint32_t audio_rate;
    uint16_t audio_ch, audio_bits;
    uint16_t img_x0, img_y0, img_w, img_h;
    uint16_t hysteresis, dither;

    uint32_t idx_off, frames_off, frames_bytes, audio_off, audio_bytes;

    int stride;      /* (width + 7) / 8 */
    size_t plane_bytes; /* stride * height */

    vidx_t *idx;
    unsigned char *buf;  /* payload scratch */
    size_t buf_cap;
    unsigned char *flat; /* inflate scratch */
    size_t flat_cap;

    char err[160];
} pack_t;

/* 0 on success; -1 with p->err set. p is safe to pack_close() either way. */
int pack_open(pack_t *p, const char *path);
void pack_close(pack_t *p);

/* Decode frame i into plane, which must be pack_plane_bytes() long.
 *
 * Delta frames are applied in place, so the caller must hold one plane and
 * feed it forward -- the reference frame IS the plane. Returns 1 when the
 * plane changed, 0 when the frame is byte-identical to its predecessor (size
 * 0, which costs 12 bytes and no SPI), -1 on a malformed frame. */
int pack_frame(pack_t *p, uint32_t i, unsigned char *plane);

/* Nearest keyframe at or before i, or -1 if the chain is unenterable -- which
 * is a corrupt pack, since frame 0 must be a keyframe. */
long pack_keyframe_before(const pack_t *p, uint32_t i);

/* Audio bytes at an absolute offset into the AUDIO section. Returns bytes
 * read, which is short at the end of the track, or -1. */
long pack_audio(pack_t *p, uint32_t byte_off, unsigned char *buf, size_t n);

static inline size_t
pack_plane_bytes(const pack_t *p)
{
    return p->plane_bytes;
}

#endif /* BEEPYVID_PACK_H */

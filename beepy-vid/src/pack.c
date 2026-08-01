/* beepy-vid/src/pack.c -- see pack.h. */
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "libbeepyfb/le.h"

#include "codec.h"
#include "pack.h"

static int
fail(pack_t *p, const char *msg)
{
    snprintf(p->err, sizeof p->err, "%s", msg);
    return -1;
}

static int
failf(pack_t *p, const char *fmt, unsigned long a, unsigned long b)
{
    snprintf(p->err, sizeof p->err, fmt, a, b);
    return -1;
}

static int
read_at(pack_t *p, long off, void *dst, size_t n)
{
    if (off < 0 || (unsigned long)off + n > (unsigned long)p->file_bytes)
        return -1;
    if (fseek(p->fp, off, SEEK_SET) != 0)
        return -1;
    return fread(dst, 1, n, p->fp) == n ? 0 : -1;
}

static int
grow(unsigned char **buf, size_t *cap, size_t need)
{
    unsigned char *nb;
    if (*cap >= need)
        return 0;
    nb = realloc(*buf, need);
    if (!nb)
        return -1;
    *buf = nb;
    *cap = need;
    return 0;
}

int
pack_open(pack_t *p, const char *path)
{
    unsigned char hdr[VID_HEADER_BYTES];
    unsigned char sect[VID_NSECT * 8];
    unsigned char *raw = NULL;
    uint16_t header_bytes, nsect;
    uint32_t idx_count;

    memset(p, 0, sizeof *p);
    p->fp = fopen(path, "rb");
    if (!p->fp)
        return fail(p, "cannot open");
    if (fseek(p->fp, 0, SEEK_END) != 0)
        return fail(p, "not seekable");
    p->file_bytes = ftell(p->fp);
    if (p->file_bytes < VID_HEADER_BYTES + VID_NSECT * 8)
        return fail(p, "too short to be a pack");

    if (read_at(p, 0, hdr, sizeof hdr) != 0)
        return fail(p, "cannot read header");
    if (memcmp(hdr, VID_MAGIC, 8) != 0)
        return fail(p, "not a .vid pack (bad magic)");

    p->version = rd_u16(hdr + 8);
    if (p->version != VID_VERSION)
        return failf(p, "pack version %lu, this build reads %lu",
                     p->version, (unsigned long)VID_VERSION);

    header_bytes = rd_u16(hdr + 10);
    p->flags = rd_u16(hdr + 12);
    nsect = rd_u16(hdr + 14);
    if (header_bytes != VID_HEADER_BYTES)
        return failf(p, "header_bytes %lu, expected %lu",
                     header_bytes, (unsigned long)VID_HEADER_BYTES);
    if (nsect != VID_NSECT)
        return failf(p, "nsect %lu, expected %lu", nsect, (unsigned long)VID_NSECT);
    if (!(p->flags & PACKF_INK_MSB))
        return fail(p, "pack does not declare MSB-first ink rows");

    p->width = rd_u16(hdr + 16);
    p->height = rd_u16(hdr + 18);
    p->nframe = rd_u32(hdr + 20);
    p->fps_num = rd_u32(hdr + 24);
    p->fps_den = rd_u32(hdr + 28);
    p->gop = rd_u32(hdr + 32);
    p->audio_rate = rd_u32(hdr + 36);
    p->audio_ch = rd_u16(hdr + 40);
    p->audio_bits = rd_u16(hdr + 42);
    /* 44: audio_bytes, informational -- the section table is the authority */
    p->img_x0 = rd_u16(hdr + 48);
    p->img_y0 = rd_u16(hdr + 50);
    p->img_w = rd_u16(hdr + 52);
    p->img_h = rd_u16(hdr + 54);
    p->hysteresis = rd_u16(hdr + 56);
    p->dither = rd_u16(hdr + 58);

    if (p->width == 0 || p->height == 0 || p->width > 4096 || p->height > 4096)
        return failf(p, "implausible geometry %lux%lu", p->width, p->height);
    if (p->fps_num == 0 || p->fps_den == 0)
        return fail(p, "frame rate is zero or has a zero denominator");
    if (p->nframe == 0)
        return fail(p, "pack contains no frames");
    /* The image rect must fit inside the plane, or the OSD would letterbox
     * against numbers that do not describe this pack. */
    if ((uint32_t)p->img_x0 + p->img_w > p->width ||
        (uint32_t)p->img_y0 + p->img_h > p->height)
        return fail(p, "image rect does not fit the frame");
    if (p->audio_bits != 0 && p->audio_bits != 16)
        return fail(p, "only 16-bit audio is defined in v1");

    p->stride = (p->width + 7) / 8;
    p->plane_bytes = (size_t)p->stride * p->height;

    if (read_at(p, VID_HEADER_BYTES, sect, sizeof sect) != 0)
        return fail(p, "cannot read section table");
    p->idx_off = rd_u32(sect + 0);
    idx_count = rd_u32(sect + 4);
    p->frames_off = rd_u32(sect + 8);
    p->frames_bytes = rd_u32(sect + 12);
    p->audio_off = rd_u32(sect + 16);
    p->audio_bytes = rd_u32(sect + 20);

    if (idx_count != p->nframe)
        return failf(p, "index holds %lu entries, header says %lu frames",
                     idx_count, p->nframe);
    if ((unsigned long)p->idx_off + (unsigned long)idx_count * VID_INDEX_ENTRY >
        (unsigned long)p->file_bytes)
        return fail(p, "index runs past the end of the file");
    if ((unsigned long)p->frames_off + p->frames_bytes > (unsigned long)p->file_bytes)
        return fail(p, "frame arena runs past the end of the file");
    if ((unsigned long)p->audio_off + p->audio_bytes > (unsigned long)p->file_bytes)
        return fail(p, "audio section runs past the end of the file");
    if ((p->flags & PACKF_AUDIO) && p->audio_bytes == 0)
        return fail(p, "pack declares audio and carries none");

    raw = malloc((size_t)idx_count * VID_INDEX_ENTRY);
    p->idx = calloc(idx_count, sizeof *p->idx);
    if (!raw || !p->idx) {
        free(raw);
        return fail(p, "out of memory");
    }
    if (read_at(p, p->idx_off, raw, (size_t)idx_count * VID_INDEX_ENTRY) != 0) {
        free(raw);
        return fail(p, "index is truncated");
    }
    for (uint32_t i = 0; i < idx_count; i++) {
        const unsigned char *e = raw + (size_t)i * VID_INDEX_ENTRY;
        vidx_t *v = &p->idx[i];
        v->off = rd_u32(e + 0);
        v->size = rd_u32(e + 4);
        v->mode = e[8];
        v->flags = e[9];
        v->y0 = e[10];
        v->y1 = e[11];
        if (v->mode > CODEC_XOR_DEFLATE) {
            free(raw);
            return failf(p, "frame %lu uses unknown mode %lu", i, v->mode);
        }
        if (v->size) {
            if ((unsigned long)v->off + v->size > (unsigned long)p->file_bytes) {
                free(raw);
                return failf(p, "frame %lu runs past the end of the file", i, 0);
            }
            /* y1 is inclusive; y1 == height is one row too far. A y1 < y0 with
             * a non-zero size is the encoder saying "nothing changed" while
             * shipping bytes, which is a contradiction, not a nuance. */
            if (v->y1 >= p->height || v->y1 < v->y0) {
                free(raw);
                return failf(p, "frame %lu has row span %lu..", i, v->y0);
            }
        }
        if (i == 0 && !(v->flags & FRAMEF_KEY)) {
            free(raw);
            return fail(p, "frame 0 is not a keyframe, so the chain cannot be entered");
        }
        if ((v->mode == CODEC_DEFLATE || v->mode == CODEC_XOR_DEFLATE) &&
            !(p->flags & PACKF_DEFLATE)) {
            free(raw);
            return failf(p, "frame %lu is deflated but the header does not say so",
                         i, 0);
        }
    }
    free(raw);

    /* gop is a promise the seek path relies on: never walk back further than
     * this looking for a keyframe. Check it once here rather than discovering
     * it during a seek, when the only symptom is a long pause. */
    if (p->gop == 0)
        return fail(p, "gop is zero, so no seek is bounded");
    {
        uint32_t since = 0;
        for (uint32_t i = 0; i < p->nframe; i++) {
            if (p->idx[i].flags & FRAMEF_KEY)
                since = 0;
            else if (++since >= p->gop)
                return failf(p, "no keyframe within gop (%lu) at frame %lu",
                             p->gop, i);
        }
    }
    return 0;
}

void
pack_close(pack_t *p)
{
    if (p->fp)
        fclose(p->fp);
    free(p->idx);
    free(p->buf);
    free(p->flat);
    memset(p, 0, sizeof *p);
}

/* Raw deflate (windowBits -15): the index already carries the size, so a zlib
 * wrapper would duplicate a check a whole-file hash does better. */
static int
inflate_raw(pack_t *p, const unsigned char *src, size_t n, size_t want)
{
    z_stream z;
    int rc;

    if (grow(&p->flat, &p->flat_cap, want ? want : 1) != 0)
        return fail(p, "out of memory inflating");
    memset(&z, 0, sizeof z);
    if (inflateInit2(&z, -15) != Z_OK)
        return fail(p, "inflateInit2 failed");
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)n;
    z.next_out = p->flat;
    z.avail_out = (uInt)want;
    rc = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (rc != Z_STREAM_END || z.total_out != want)
        return fail(p, "deflated frame did not inflate to its declared size");
    return 0;
}

int
pack_frame(pack_t *p, uint32_t i, unsigned char *plane)
{
    const vidx_t *v;
    size_t band;

    if (i >= p->nframe)
        return fail(p, "frame index out of range");
    v = &p->idx[i];
    if (v->size == 0)
        return 0; /* identical to the previous frame: nothing to do at all */

    if (grow(&p->buf, &p->buf_cap, v->size) != 0)
        return fail(p, "out of memory reading a frame");
    if (read_at(p, (long)v->off, p->buf, v->size) != 0)
        return fail(p, "frame payload is truncated");

    band = (size_t)(v->y1 - v->y0 + 1) * (size_t)p->stride;

    switch (v->mode) {
    case CODEC_RAW:
        if (codec_raw(plane, p->stride, p->height, v->y0, v->y1,
                      p->buf, v->size) != 0)
            return fail(p, "raw frame is not the size its row span implies");
        return 1;
    case CODEC_XOR_SPANS:
        if (codec_xor_spans(plane, p->stride, p->height, p->buf, v->size) != 0)
            return fail(p, "span frame is malformed");
        return 1;
    case CODEC_DEFLATE:
        if (inflate_raw(p, p->buf, v->size, band) != 0)
            return -1;
        if (codec_raw(plane, p->stride, p->height, v->y0, v->y1,
                      p->flat, band) != 0)
            return fail(p, "inflated frame does not fit its row span");
        return 1;
    case CODEC_XOR_DEFLATE:
        if (inflate_raw(p, p->buf, v->size, band) != 0)
            return -1;
        if (codec_xor_band(plane, p->stride, p->height, v->y0, v->y1,
                           p->flat, band) != 0)
            return fail(p, "inflated delta does not fit its row span");
        return 1;
    default:
        return fail(p, "unknown frame mode");
    }
}

long
pack_keyframe_before(const pack_t *p, uint32_t i)
{
    if (i >= p->nframe)
        return -1;
    for (;;) {
        if (p->idx[i].flags & FRAMEF_KEY)
            return (long)i;
        if (i == 0)
            return -1;
        i--;
    }
}

long
pack_audio(pack_t *p, uint32_t byte_off, unsigned char *buf, size_t n)
{
    size_t avail;
    if (byte_off > p->audio_bytes)
        return -1;
    avail = p->audio_bytes - byte_off;
    if (n > avail)
        n = avail;
    if (n == 0)
        return 0;
    if (read_at(p, (long)p->audio_off + byte_off, buf, n) != 0)
        return -1;
    return (long)n;
}

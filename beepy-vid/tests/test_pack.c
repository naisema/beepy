/* beepy-vid/tests/test_pack.c -- the .vid reader, gated without the writer.
 *
 * The reader is deliberately linkable on its own, which is what lets this
 * build packs byte by byte in C instead of shelling out to tools/mkvid.py --
 * the same argument Makefile:1444-1449 makes for test_tile. It means the
 * format is gated in M2, before the packer exists.
 *
 * Method: assemble one known-good pack, then for each refusal copy it and
 * corrupt exactly one field. That way every refusal test is one byte away from
 * a pack that demonstrably opens, so a test cannot pass because the pack was
 * broken in some other way.
 *
 *     make test-unit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "codec.h"
#include "pack.h"

#define W 64
#define H 8
#define STRIDE (W / 8)
#define PLANE (STRIDE * H)
#define NFRAME 5
#define TMP "out-test-pack.vid"

static int failures;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void
wr16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void
wr32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* raw deflate, windowBits -15, matching what the reader inflates */
static size_t
deflate_raw(const unsigned char *src, size_t n, unsigned char *dst, size_t cap)
{
    z_stream z;
    memset(&z, 0, sizeof z);
    if (deflateInit2(&z, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return 0;
    z.next_in = (Bytef *)src;
    z.avail_in = (uInt)n;
    z.next_out = dst;
    z.avail_out = (uInt)cap;
    if (deflate(&z, Z_FINISH) != Z_STREAM_END) {
        deflateEnd(&z);
        return 0;
    }
    deflateEnd(&z);
    return z.total_out;
}

/* ------------------------------------------------------- the good pack */

/* Frame plan, chosen so that every mode and the size==0 case are exercised:
 *   0 RAW keyframe, whole plane
 *   1 XOR_SPANS delta
 *   2 size 0 (identical to frame 1)
 *   3 XOR_DEFLATE delta over rows 2..5
 *   4 DEFLATE keyframe, whole plane
 */
static unsigned char good[4096];
static size_t good_len;
static unsigned char want[NFRAME][PLANE]; /* what each frame should decode to */

#define IDX_AT(i) (88 + (i) * 12)

static void
build_good(void)
{
    unsigned char key0[PLANE], key4[PLANE], band[STRIDE * 4], spanpay[32];
    unsigned char dfl4[512], dfl3[512];
    size_t dfl4n, dfl3n;
    unsigned char *frames;
    size_t at, foff[NFRAME], fsize[NFRAME];
    unsigned char mode[NFRAME] = { CODEC_RAW, CODEC_XOR_SPANS, 0,
                                   CODEC_XOR_DEFLATE, CODEC_DEFLATE };
    unsigned char flg[NFRAME] = { FRAMEF_KEY, 0, 0, 0, FRAMEF_KEY };
    unsigned char y0[NFRAME] = { 0, 0, 255, 2, 0 };
    unsigned char y1[NFRAME] = { H - 1, H - 1, 0, 5, H - 1 };

    for (int i = 0; i < PLANE; i++)
        key0[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < PLANE; i++)
        key4[i] = (unsigned char)(0xC3 ^ i);

    /* frame 1: two spans */
    spanpay[0] = 2; spanpay[1] = 0;
    spanpay[2] = 1; spanpay[3] = 0; spanpay[4] = 2; spanpay[5] = 0xAA; spanpay[6] = 0xBB;
    spanpay[7] = 6; spanpay[8] = 3; spanpay[9] = 1; spanpay[10] = 0x5C;

    /* frame 3: a delta band over rows 2..5 */
    for (size_t i = 0; i < sizeof band; i++)
        band[i] = (unsigned char)(i * 3 + 9);

    dfl3n = deflate_raw(band, sizeof band, dfl3, sizeof dfl3);
    dfl4n = deflate_raw(key4, sizeof key4, dfl4, sizeof dfl4);

    /* Expected decodes, computed here independently of pack.c. */
    memcpy(want[0], key0, PLANE);
    memcpy(want[1], want[0], PLANE);
    want[1][1 * STRIDE + 0] ^= 0xAA;
    want[1][1 * STRIDE + 1] ^= 0xBB;
    want[1][6 * STRIDE + 3] ^= 0x5C;
    memcpy(want[2], want[1], PLANE); /* size 0 */
    memcpy(want[3], want[2], PLANE);
    for (size_t i = 0; i < sizeof band; i++)
        want[3][2 * STRIDE + i] ^= band[i];
    memcpy(want[4], key4, PLANE);

    memset(good, 0, sizeof good);
    memcpy(good, VID_MAGIC, 8);
    wr16(good + 8, VID_VERSION);
    wr16(good + 10, VID_HEADER_BYTES);
    wr16(good + 12, PACKF_INK_MSB | PACKF_DEFLATE);
    wr16(good + 14, VID_NSECT);
    wr16(good + 16, W);
    wr16(good + 18, H);
    wr32(good + 20, NFRAME);
    wr32(good + 24, 24000);
    wr32(good + 28, 1001);
    wr32(good + 32, 4); /* gop */
    wr32(good + 36, 0); /* no audio */
    wr16(good + 40, 0);
    wr16(good + 42, 0);
    wr32(good + 44, 0);
    wr16(good + 48, 0);  /* img rect = whole plane */
    wr16(good + 50, 0);
    wr16(good + 52, W);
    wr16(good + 54, H);
    wr16(good + 56, 8);  /* hysteresis */
    wr16(good + 58, 1);  /* dither: bayer4 */

    frames = good + IDX_AT(NFRAME);
    at = 0;
#define PUT(i, src, n) do { foff[i] = IDX_AT(NFRAME) + at; fsize[i] = (n); \
                            memcpy(frames + at, (src), (n)); at += (n); } while (0)
    PUT(0, key0, PLANE);
    PUT(1, spanpay, 11);
    foff[2] = 0; fsize[2] = 0;
    PUT(3, dfl3, dfl3n);
    PUT(4, dfl4, dfl4n);
#undef PUT

    for (int i = 0; i < NFRAME; i++) {
        unsigned char *e = good + IDX_AT(i);
        wr32(e + 0, (unsigned long)foff[i]);
        wr32(e + 4, (unsigned long)fsize[i]);
        e[8] = mode[i];
        e[9] = flg[i];
        e[10] = y0[i];
        e[11] = y1[i];
    }

    wr32(good + 64, IDX_AT(0));          /* INDEX off   */
    wr32(good + 68, NFRAME);             /* INDEX count */
    wr32(good + 72, IDX_AT(NFRAME));     /* FRAMES off  */
    wr32(good + 76, (unsigned long)at);  /* FRAMES bytes */
    wr32(good + 80, IDX_AT(NFRAME) + (unsigned long)at); /* AUDIO off */
    wr32(good + 84, 0);                  /* AUDIO bytes */

    good_len = IDX_AT(NFRAME) + at;
}

static int
write_tmp(const unsigned char *b, size_t n)
{
    FILE *f = fopen(TMP, "wb");
    if (!f)
        return -1;
    if (fwrite(b, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

/* Corrupt one field of the good pack and require pack_open() to refuse. */
static void
reject(size_t at, const unsigned char *bytes, size_t n, const char *what)
{
    unsigned char *copy = malloc(good_len);
    pack_t p;
    memcpy(copy, good, good_len);
    memcpy(copy + at, bytes, n);
    if (write_tmp(copy, good_len) != 0) {
        printf("FAIL cannot write %s\n", TMP);
        failures++;
        free(copy);
        return;
    }
    if (pack_open(&p, TMP) == 0) {
        printf("FAIL %s: pack_open ACCEPTED it\n", what);
        failures++;
    }
    pack_close(&p);
    free(copy);
}

static void
reject16(size_t at, unsigned v, const char *what)
{
    unsigned char b[2];
    wr16(b, v);
    reject(at, b, 2, what);
}

static void
reject32(size_t at, unsigned long v, const char *what)
{
    unsigned char b[4];
    wr32(b, v);
    reject(at, b, 4, what);
}

int
main(void)
{
    pack_t p;
    unsigned char plane[PLANE];

    build_good();

    /* ------------------------------------------------ the good pack opens */
    if (write_tmp(good, good_len) != 0) {
        printf("FAIL cannot write %s\n", TMP);
        return 1;
    }
    if (pack_open(&p, TMP) != 0) {
        printf("FAIL the good pack did not open: %s\n", p.err);
        pack_close(&p);
        return 1;
    }
    check(p.width == W && p.height == H, "header: geometry read back");
    check(p.nframe == NFRAME, "header: frame count read back");
    check(p.fps_num == 24000 && p.fps_den == 1001,
          "header: frame rate is a ratio, not a float");
    check(p.stride == STRIDE, "header: stride derived from width");
    check(pack_plane_bytes(&p) == PLANE, "header: plane size derived");
    check(p.hysteresis == 8 && p.dither == 1,
          "header: provenance fields read back");

    /* ------------------------------------------------------ decode, in order */
    memset(plane, 0, sizeof plane);
    for (uint32_t i = 0; i < NFRAME; i++) {
        char what[64];
        int rc = pack_frame(&p, i, plane);
        snprintf(what, sizeof what, "frame %u decodes", i);
        check(rc >= 0, what);
        snprintf(what, sizeof what, "frame %u matches the expected plane", i);
        check(memcmp(plane, want[i], PLANE) == 0, what);
    }

    /* size == 0 must report "nothing changed" rather than "I decoded it". */
    memset(plane, 0, sizeof plane);
    pack_frame(&p, 0, plane);
    pack_frame(&p, 1, plane);
    check(pack_frame(&p, 2, plane) == 0,
          "a size-0 frame returns 0 (identical to its predecessor)");
    check(memcmp(plane, want[2], PLANE) == 0,
          "a size-0 frame leaves the plane alone");

    /* keyframes */
    check(pack_keyframe_before(&p, 0) == 0, "keyframe search: frame 0 is itself");
    check(pack_keyframe_before(&p, 3) == 0, "keyframe search: walks back to 0");
    check(pack_keyframe_before(&p, 4) == 4, "keyframe search: finds frame 4");
    check(pack_keyframe_before(&p, NFRAME) == -1,
          "keyframe search: out-of-range index refused");

    check(pack_frame(&p, NFRAME, plane) == -1, "frame index out of range refused");
    check(pack_audio(&p, 0, plane, 4) == 0, "a pack with no audio reads 0 bytes");
    pack_close(&p);

    /* --------------------------------------------------------- refusals */
    reject(0, (const unsigned char *)"BEEPYVIE", 8, "bad magic");
    reject16(8, 2, "wrong version");
    reject16(10, 32, "wrong header_bytes");
    reject16(14, 2, "wrong nsect");
    reject16(12, PACKF_DEFLATE, "MSB-ink flag not declared");
    reject16(16, 0, "zero width");
    reject16(18, 0, "zero height");
    reject32(20, 0, "zero frames");
    reject32(24, 0, "zero fps numerator");
    reject32(28, 0, "zero fps denominator");
    reject32(32, 0, "zero gop leaves seeks unbounded");
    reject16(52, W + 8, "image rect wider than the frame");
    reject16(54, H + 1, "image rect taller than the frame");
    reject16(42, 24, "audio bit depth other than 16");
    reject32(68, NFRAME + 1, "index count disagrees with the header");
    reject32(64, 0xFFFFFF, "index runs past the end of the file");
    reject32(76, 0xFFFFFF, "frame arena runs past the end of the file");
    reject32(84, 0xFFFFFF, "audio section runs past the end of the file");
    reject16(12, PACKF_INK_MSB | PACKF_DEFLATE | PACKF_AUDIO,
             "declares audio but carries none");
    reject32(IDX_AT(0) + 0, 0xFFFFFF, "a frame payload past the end of the file");
    /* Frame 0 not being a keyframe must be refused BY THAT RULE, not as a
     * side effect of the gop walk. With gop 4 and frames 1..3 non-key,
     * clearing frame 0's flag makes a run of four and the gop check fires
     * first -- so the test would pass with the frame-0 rule deleted. Widening
     * gop to 8 in the same copy removes the mask and leaves only one reason
     * to refuse. */
    {
        unsigned char *copy = malloc(good_len);
        pack_t q;
        memcpy(copy, good, good_len);
        wr32(copy + 32, 8);            /* gop = 8: no run exceeds it */
        copy[IDX_AT(0) + 9] = 0;       /* frame 0 is no longer a keyframe */
        write_tmp(copy, good_len);
        check(pack_open(&q, TMP) != 0,
              "frame 0 is not a keyframe (with gop widened so it cannot mask)");
        pack_close(&q);
        /* And the control: the same widened gop on its own must still open,
         * or the test above proves nothing about the frame-0 rule. */
        memcpy(copy, good, good_len);
        wr32(copy + 32, 8);
        write_tmp(copy, good_len);
        check(pack_open(&q, TMP) == 0, "widening gop alone leaves a valid pack");
        pack_close(&q);
        free(copy);
    }
    reject(IDX_AT(0) + 8, (const unsigned char *)"\x09", 1, "unknown frame mode");
    reject(IDX_AT(1) + 11, (const unsigned char *)"\x08", 1,
           "row span ends at y1 == height");
    /* Frame 3 spans rows 2..5, so y0 = 6 puts the start past the end. Frame 1
     * would not do: it spans 0..7, and y0 = 7 is a legal single row. */
    reject(IDX_AT(3) + 10, (const unsigned char *)"\x06", 1,
           "row span is inverted (y0 > y1)");
    reject16(12, PACKF_INK_MSB, "a deflated frame with no deflate flag");
    /* Frames 1,2,3 are non-key and gop is 4; make frame 4 non-key too and the
     * chain exceeds gop, which the seek path relies on never happening. */
    reject(IDX_AT(4) + 9, (const unsigned char *)"\0", 1,
           "a run of non-key frames longer than gop");

    /* A truncated file: same bytes, fewer of them. */
    {
        pack_t q;
        write_tmp(good, good_len - 4);
        check(pack_open(&q, TMP) != 0, "a truncated pack is refused");
        pack_close(&q);
        write_tmp(good, 20);
        check(pack_open(&q, TMP) != 0, "a file too short for a header is refused");
        pack_close(&q);
    }

    remove(TMP);
    if (failures) {
        printf("test_pack: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_pack: PASS\n");
    return 0;
}

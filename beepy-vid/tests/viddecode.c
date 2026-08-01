/* beepy-vid/tests/viddecode.c -- decode a .vid pack to raw 1bpp planes.
 *
 * The point is to close the loop between the two halves of the pipeline:
 * tools/mkvid.py writes the pack in Python, pack.c reads it in C, and until
 * something decodes a real pack with the real reader the two are only believed
 * to agree. `mkvid.py --verify` compares this output against its own dither of
 * the same gray8, so a disagreement anywhere -- span coder, deflate framing,
 * row spans, the size==0 rule -- shows up as a named frame.
 *
 *     viddecode PACK OUT.planes      # every frame, plane_bytes each
 *     viddecode PACK --hash          # one crc32 per frame, for a manifest
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "pack.h"

int
main(int argc, char **argv)
{
    pack_t p;
    unsigned char *plane;
    FILE *out = NULL;
    int hash = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: viddecode PACK (OUT.planes | --hash)\n");
        return 2;
    }
    hash = !strcmp(argv[2], "--hash");

    if (pack_open(&p, argv[1]) != 0) {
        fprintf(stderr, "viddecode: %s: %s\n", argv[1], p.err);
        pack_close(&p);
        return 1;
    }
    plane = calloc(1, pack_plane_bytes(&p));
    if (!plane) {
        pack_close(&p);
        return 1;
    }
    if (!hash && !(out = fopen(argv[2], "wb"))) {
        perror(argv[2]);
        pack_close(&p);
        return 1;
    }

    for (uint32_t i = 0; i < p.nframe; i++) {
        if (pack_frame(&p, i, plane) < 0) {
            fprintf(stderr, "viddecode: frame %u: %s\n", i, p.err);
            return 1;
        }
        if (hash)
            printf("%u %08lx\n", i,
                   (unsigned long)crc32(0, plane, (uInt)pack_plane_bytes(&p)));
        else if (fwrite(plane, 1, pack_plane_bytes(&p), out) !=
                 pack_plane_bytes(&p)) {
            perror("write");
            return 1;
        }
    }
    if (out)
        fclose(out);
    free(plane);
    if (!hash)
        printf("viddecode: %u frames, %zu bytes each\n",
               p.nframe, pack_plane_bytes(&p));
    pack_close(&p);
    return 0;
}

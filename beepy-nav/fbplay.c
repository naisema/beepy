/*
 * fbplay -- play packed 1-bit frames on the Beepy panel at a fixed rate.
 *
 * Frame format: 12000 bytes = 400x240 px, MSB first, 1 = white -- exactly
 * PIL's mode "1" tobytes(). Each byte expands through a 256-entry LUT into
 * 32 bytes of the panel's XRGB words (black 00 00 00 ff, white ff ff ff ff,
 * the two words taken from the device's own shutdownimage.fb).
 *
 * fbterm pause/restore is the caller's job -- run this through fbanim, which
 * wraps it in the same SIGSTOP/SIGCONT dance as fbshow.
 *
 *     cc -O2 -o fbplay fbplay.c && ./fbplay sim-anim.bin 6
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define W 400
#define H 240
#define PACKED (W * H / 8)
#define FRAME (W * H * 4)

static unsigned char lut[256][32];

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: fbplay FILE FPS\n");
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    double fps = atof(argv[2]);
    if (fps <= 0 || fps > 40) { fprintf(stderr, "bad fps\n"); return 2; }

    int fb = open("/dev/fb1", O_WRONLY);
    if (fb < 0) { perror("/dev/fb1"); return 1; }

    static const unsigned char black[4] = { 0x00, 0x00, 0x00, 0xff };
    static const unsigned char white[4] = { 0xff, 0xff, 0xff, 0xff };
    for (int v = 0; v < 256; v++)
        for (int b = 0; b < 8; b++)
            memcpy(&lut[v][b * 4], (v >> (7 - b)) & 1 ? white : black, 4);

    static unsigned char in[PACKED], out[FRAME];
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    long n = 0;

    while (fread(in, 1, PACKED, f) == PACKED) {
        unsigned char *dst = out;
        for (int i = 0; i < PACKED; i++, dst += 32)
            memcpy(dst, lut[in[i]], 32);

        if (lseek(fb, 0, SEEK_SET) != 0 ||
            write(fb, out, FRAME) != FRAME) {
            perror("write fb");
            return 1;
        }

        n++;                                   /* sleep until frame n's time */
        double due = n / fps;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (now.tv_sec - t0.tv_sec) +
                         (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (due > elapsed) {
            struct timespec ts;
            ts.tv_sec = (time_t)(due - elapsed);
            ts.tv_nsec = (long)(((due - elapsed) - ts.tv_sec) * 1e9);
            nanosleep(&ts, NULL);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "fbplay: %ld frames in %.1f s\n", n,
            (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9);
    return 0;
}

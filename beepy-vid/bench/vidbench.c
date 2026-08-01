/* vidbench -- retire the estimates beepy-vid/DESIGN.md rests on.
 *
 * Measures, on the real panel with fbterm stopped:
 *   ceiling ROWS        unpaced max panel rate for a row band
 *   naive   ROWS FPS    nanosleep pacing: submitted vs landed
 *   bp      ROWS FPS    sysfs back-pressure pacing: submitted vs landed
 *   expand              per-pixel bit test vs 256x32 LUT
 *
 * Ground truth is the kernel's own SPI message/byte counters, not wall time.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define W 400
#define H 240
#define STRIDE (W / 8)          /* 50 */
#define LINELEN 1600            /* measured: fb1 stride */

static const char *SPI_MSG = "/sys/class/spi_master/spi0/spi0.0/statistics/messages";
static const char *SPI_BYT = "/sys/class/spi_master/spi0/spi0.0/statistics/bytes";

static long counter(const char *path)
{
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strtol(buf, NULL, 10);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void sleep_until(double due)
{
    double d = due - now();
    if (d <= 0) return;
    struct timespec ts = { (time_t)d, (long)((d - (time_t)d) * 1e9) };
    nanosleep(&ts, NULL);
}

static unsigned char lut[256][32];

static void lut_init(void)
{
    static const unsigned char blk[4] = { 0x00, 0x00, 0x00, 0xff };
    static const unsigned char wht[4] = { 0xff, 0xff, 0xff, 0xff };
    for (int v = 0; v < 256; v++)
        for (int b = 0; b < 8; b++)
            memcpy(&lut[v][b * 4], (v >> (7 - b)) & 1 ? blk : wht, 4);
}

/* the current libbeepyfb expand(), verbatim in shape */
static void expand_bits(const unsigned char *bits, unsigned char *frame, int rows)
{
    for (int y = 0; y < rows; y++) {
        uint32_t *row = (uint32_t *)(frame + (size_t)y * LINELEN);
        const unsigned char *src = &bits[(size_t)y * STRIDE];
        for (int x = 0; x < W; x++)
            row[x] = (src[x / 8] & (0x80u >> (x % 8))) ? 0xFF000000u : 0xFFFFFFFFu;
    }
}

static void expand_lut(const unsigned char *bits, unsigned char *frame, int rows)
{
    for (int y = 0; y < rows; y++) {
        unsigned char *dst = frame + (size_t)y * LINELEN;
        const unsigned char *src = &bits[(size_t)y * STRIDE];
        for (int i = 0; i < STRIDE; i++, dst += 32)
            memcpy(dst, lut[src[i]], 32);
    }
}

static unsigned char plane[H * STRIDE];
static unsigned char frame[H * LINELEN];

/* a band that differs every frame, so nothing can be skipped */
static void fill(int k, int rows)
{
    for (int y = 0; y < rows; y++)
        for (int i = 0; i < STRIDE; i++)
            plane[y * STRIDE + i] = (unsigned char)(y * 7 + i * 3 + k * 11);
}

static int band_write(int fd, int rows)
{
    size_t n = (size_t)rows * LINELEN;
    if (lseek(fd, 0, SEEK_SET) != 0) return -1;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, frame + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: vidbench MODE [args]\n"); return 2; }
    lut_init();

    if (!strcmp(argv[1], "expand")) {
        int reps = 2000;
        fill(1, H);
        double t0 = now();
        for (int i = 0; i < reps; i++) expand_bits(plane, frame, 225);
        double t1 = now();
        for (int i = 0; i < reps; i++) expand_lut(plane, frame, 225);
        double t2 = now();
        printf("expand 225 rows: per-pixel %.3f ms   LUT %.3f ms   speedup %.2fx\n",
               (t1 - t0) * 1000 / reps, (t2 - t1) * 1000 / reps,
               (t1 - t0) / (t2 - t1));
        return 0;
    }

    int rows = argc > 2 ? atoi(argv[2]) : 240;
    if (rows < 2 || rows > H) { fprintf(stderr, "rows 2..240\n"); return 2; }

    int fd = open("/dev/fb1", O_RDWR);
    if (fd < 0) { perror("/dev/fb1"); return 1; }

    if (!strcmp(argv[1], "ceiling")) {
        int n = argc > 3 ? atoi(argv[3]) : 200;
        long m0 = counter(SPI_MSG), b0 = counter(SPI_BYT);
        double t0 = now();
        for (int i = 0; i < n; i++) {
            fill(i, rows);
            expand_lut(plane, frame, rows);
            if (band_write(fd, rows) < 0) { perror("write"); return 1; }
        }
        /* let the workqueue drain */
        double drain = now() + 0.5;
        sleep_until(drain);
        double t1 = now();
        long m1 = counter(SPI_MSG), b1 = counter(SPI_BYT);
        double el = t1 - t0;
        printf("rows %3d  submitted %d in %.3fs  spi_msgs %ld  spi_bytes %ld"
               "  -> panel %.1f fps  %.0f B/update  %.0f B/s\n",
               rows, n, el, m1 - m0, b1 - b0,
               (m1 - m0) / el, (double)(b1 - b0) / (m1 - m0 ? m1 - m0 : 1),
               (b1 - b0) / el);
        close(fd);
        return 0;
    }

    double fps = argc > 3 ? atof(argv[3]) : 24.0;
    int secs = argc > 4 ? atoi(argv[4]) : 6;
    int n = (int)(fps * secs);
    int bp = !strcmp(argv[1], "bp");

    long m0 = counter(SPI_MSG);
    double t0 = now();
    long last = m0;
    int stalls = 0;
    for (int i = 0; i < n; i++) {
        fill(i, rows);
        expand_lut(plane, frame, rows);
        if (bp) {
            /* wait for the previous transfer to actually complete */
            double giveup = now() + 0.25;
            while (counter(SPI_MSG) == last && now() < giveup)
                ;
            if (counter(SPI_MSG) == last) stalls++;
            last = counter(SPI_MSG);
        }
        if (band_write(fd, rows) < 0) { perror("write"); return 1; }
        sleep_until(t0 + (i + 1) / fps);
    }
    double drain = now() + 0.5;
    sleep_until(drain);
    double t1 = now();
    long m1 = counter(SPI_MSG);
    double el = t1 - t0;
    printf("%-5s rows %3d  target %.1f fps  submitted %d  landed %ld"
           "  (%.1f%%)  effective %.1f fps  stalls %d\n",
           bp ? "bp" : "naive", rows, fps, n, m1 - m0,
           100.0 * (m1 - m0) / n, (m1 - m0) / el, stalls);
    close(fd);
    return 0;
}

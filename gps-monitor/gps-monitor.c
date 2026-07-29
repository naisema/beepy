/*
 * gps-monitor -- GNSS signal monitor for the Beepy's 400x240 Sharp panel.
 *
 * Reads NMEA from a u-blox receiver on /dev/ttyACM0, draws two full-screen
 * pages (vertical SNR bargraph, polar sky view) into a 1bpp backbuffer, and
 * presents them as full XRGB frames written to /dev/fb1.
 *
 * Design notes live in DESIGN.md / STRUCTURE.md. The things worth knowing here:
 *
 *  - The panel is 1 bit. Only pure black and pure white are emitted, with any
 *    halftone produced as an explicit checkerboard, so nothing depends on what
 *    the sharp_drm driver would otherwise dither.
 *  - Frames are written whole (384000 bytes) rather than mmapped, matching the
 *    device's own shutdownimage mechanism, which is known to work here.
 *  - The panel console is fbterm (userspace), not kernel fbcon, so it is
 *    SIGSTOPped while we own the display and SIGCONTed on every exit path.
 *  - Text comes from an embedded 5x7 font drawn at integer scale. Scale 2
 *    (12x16 cells) is the body size; that is why bar labels stack "02" over "G"
 *    instead of writing "G02", which would need 36px in a 26px slot.
 *
 * Build: cc -O2 -Wall -Wextra -std=c11 -o gps-monitor gps-monitor.c -lm
 */

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

/* ------------------------------------------------------------------ config */

#define SCR_W 400
#define SCR_H 240

#define SNR_MIN 10
#define SNR_MAX 50

#define MAX_SATS 64
#define NSYS 5 /* G R E B other */

#define GLYPH_W 5
#define GLYPH_H 7
#define CELL_W 6 /* glyph + 1px spacing */
#define CELL_H 8

#define SC 2                    /* body text scale */
#define CW (CELL_W * SC)        /* 12 */
#define CH (CELL_H * SC)        /* 16 */

#define INK 1
#define PAPER 0

/* ------------------------------------------------------------------- font
 *
 * ASCII 32..90 (space through 'Z'), 5 bits per row, bit 4 = leftmost pixel.
 * Uppercase only -- which is why every string drawn on screen is upper case.
 */
static const unsigned char FONT[59][GLYPH_H] = {
    [0]  = {0, 0, 0, 0, 0, 0, 0},                          /* space */
    [11] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},           /* + */
    [12] = {0,0,0,0,0x06,0x06,0x04},                       /* , */
    [13] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},           /* - */
    [14] = {0,0,0,0,0,0x06,0x06},                          /* . */
    [15] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10},           /* / */
    [16] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E},           /* 0 */
    [17] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},           /* 1 */
    [18] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F},           /* 2 */
    [19] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},           /* 3 */
    [20] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02},           /* 4 */
    [21] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},           /* 5 */
    [22] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E},           /* 6 */
    [23] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},           /* 7 */
    [24] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E},           /* 8 */
    [25] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},           /* 9 */
    [26] = {0x00,0x06,0x06,0x00,0x06,0x06,0x00},           /* : */
    [29] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},           /* = */
    [33] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},           /* A */
    [34] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},           /* B */
    [35] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},           /* C */
    [36] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C},           /* D */
    [37] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},           /* E */
    [38] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},           /* F */
    [39] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},           /* G */
    [40] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},           /* H */
    [41] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x1F},           /* I */
    [42] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},           /* J */
    [43] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11},           /* K */
    [44] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},           /* L */
    [45] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},           /* M */
    [46] = {0x11,0x11,0x19,0x15,0x13,0x11,0x11},           /* N */
    [47] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},           /* O */
    [48] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},           /* P */
    [49] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},           /* Q */
    [50] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},           /* R */
    [51] = {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},           /* S */
    [52] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},           /* T */
    [53] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},           /* U */
    [54] = {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},           /* V */
    [55] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11},           /* W */
    [56] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},           /* X */
    [57] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},           /* Y */
    [58] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F},           /* Z */
};

static const unsigned char *glyph(int c)
{
    c = toupper((unsigned char)c);
    if (c < 32 || c > 90)
        c = 32;
    return FONT[c - 32];
}

static int text_w(const char *s, int scale) { return (int)strlen(s) * CELL_W * scale; }

/* ----------------------------------------------------------------- canvas */

typedef struct {
    int w, h, stride;
    unsigned char *bits; /* 1bpp, MSB leftmost */
} canvas_t;

static canvas_t *canvas_new(int w, int h)
{
    canvas_t *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->w = w;
    c->h = h;
    c->stride = (w + 7) / 8;
    c->bits = calloc((size_t)c->stride * h, 1);
    if (!c->bits) {
        free(c);
        return NULL;
    }
    return c;
}

static void canvas_clear(canvas_t *c, int ink)
{
    memset(c->bits, ink ? 0xFF : 0x00, (size_t)c->stride * c->h);
}

static void px(canvas_t *c, int x, int y, int ink)
{
    if (x < 0 || y < 0 || x >= c->w || y >= c->h)
        return;
    unsigned char *b = &c->bits[(size_t)y * c->stride + x / 8];
    unsigned char m = (unsigned char)(0x80u >> (x % 8));
    if (ink)
        *b |= m;
    else
        *b &= (unsigned char)~m;
}

static void hline(canvas_t *c, int x, int y, int w, int ink)
{
    for (int i = 0; i < w; i++)
        px(c, x + i, y, ink);
}

static void vline(canvas_t *c, int x, int y, int h, int ink)
{
    for (int i = 0; i < h; i++)
        px(c, x, y + i, ink);
}

static void fillrect(canvas_t *c, int x, int y, int w, int h, int ink)
{
    for (int j = 0; j < h; j++)
        hline(c, x, y + j, w, ink);
}

static void rect(canvas_t *c, int x, int y, int w, int h, int ink)
{
    if (w <= 0 || h <= 0)
        return;
    hline(c, x, y, w, ink);
    hline(c, x, y + h - 1, w, ink);
    vline(c, x, y, h, ink);
    vline(c, x + w - 1, y, h, ink);
}

/* 50% ordered dither: the only halftone a 1-bit panel can honestly show. */
static void checker(canvas_t *c, int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++)
            if (((x + i) + (y + j)) % 2 == 0)
                px(c, x + i, y + j, INK);
}

static void circle(canvas_t *c, int cx, int cy, int r, int ink)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px(c, cx + x, cy + y, ink); px(c, cx + y, cy + x, ink);
        px(c, cx - y, cy + x, ink); px(c, cx - x, cy + y, ink);
        px(c, cx - x, cy - y, ink); px(c, cx - y, cy - x, ink);
        px(c, cx + y, cy - x, ink); px(c, cx + x, cy - y, ink);
        y++;
        if (err < 0)
            err += 2 * y + 1;
        else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void disc(canvas_t *c, int cx, int cy, int r, int ink)
{
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++)
            if (i * i + j * j <= r * r)
                px(c, cx + i, cy + j, ink);
}

/* Grid rings are drawn as spaced dots so they stay visually behind markers. */
static void dots_circle(canvas_t *c, int cx, int cy, int r, int step)
{
    if (r <= 0)
        return;
    int n = (int)(2.0 * M_PI * r / step);
    if (n < 12)
        n = 12;
    for (int i = 0; i < n; i++) {
        double a = 2.0 * M_PI * i / n;
        px(c, cx + (int)lround(r * sin(a)), cy - (int)lround(r * cos(a)), INK);
    }
}

static void draw_text(canvas_t *c, int x, int y, const char *s, int scale, int ink)
{
    for (; *s; s++) {
        const unsigned char *g = glyph(*s);
        for (int r = 0; r < GLYPH_H; r++)
            for (int col = 0; col < GLYPH_W; col++)
                if (g[r] & (0x10 >> col))
                    fillrect(c, x + col * scale, y + r * scale, scale, scale, ink);
        x += CELL_W * scale;
    }
}

static void draw_ctext(canvas_t *c, int cx, int y, const char *s, int scale, int ink)
{
    draw_text(c, cx - text_w(s, scale) / 2, y, s, scale, ink);
}

/* ------------------------------------------------------------------ fbdev */

typedef struct {
    int fd;
    int w, h;
    size_t line_len, frame_sz;
    unsigned char *frame;
    pid_t paused[8];
    int npaused;
} fb_t;

static int fb_open(fb_t *f, const char *path)
{
    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo fx;

    memset(f, 0, sizeof *f);
    f->fd = open(path, O_RDWR);
    if (f->fd < 0) {
        fprintf(stderr, "open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(f->fd, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(f->fd, FBIOGET_FSCREENINFO, &fx) < 0) {
        fprintf(stderr, "%s: FBIOGET_*SCREENINFO: %s\n", path, strerror(errno));
        close(f->fd);
        return -1;
    }
    if (v.bits_per_pixel != 32) {
        fprintf(stderr, "%s: %u bpp, this build expects 32\n", path, v.bits_per_pixel);
        close(f->fd);
        return -1;
    }
    f->w = (int)v.xres;
    f->h = (int)v.yres;
    f->line_len = fx.line_length;
    f->frame_sz = f->line_len * (size_t)f->h;
    f->frame = malloc(f->frame_sz);
    if (!f->frame) {
        close(f->fd);
        return -1;
    }
    return 0;
}

/* fbterm is a userspace terminal; pause it or it repaints over our frames. */
static void fb_take(fb_t *f)
{
    DIR *d = opendir("/proc");
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) && f->npaused < (int)(sizeof f->paused / sizeof f->paused[0])) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        char p[288], comm[64] = "";
        FILE *fp;
        snprintf(p, sizeof p, "/proc/%s/comm", e->d_name);
        if (!(fp = fopen(p, "r")))
            continue;
        if (fgets(comm, sizeof comm, fp)) {
            char *nl = strchr(comm, '\n');
            if (nl)
                *nl = 0;
            if (!strcmp(comm, "fbterm")) {
                pid_t pid = (pid_t)atoi(e->d_name);
                if (kill(pid, SIGSTOP) == 0)
                    f->paused[f->npaused++] = pid;
            }
        }
        fclose(fp);
    }
    closedir(d);
}

static void fb_release(fb_t *f)
{
    for (int i = 0; i < f->npaused; i++)
        kill(f->paused[i], SIGCONT);
    f->npaused = 0;
    /* fbterm only repaints when tty content changes, so nudge tmux. */
    int rc = system("tmux list-clients -F '#{client_name}' 2>/dev/null | "
                    "while read -r c; do tmux refresh-client -t \"$c\" 2>/dev/null; done");
    (void)rc; /* best effort: the panel repaints on the next tty write anyway */
}

/* 1bpp -> XRGB8888. The two words match the device's own shutdownimage.fb:
 * black = 00 00 00 ff, white = ff ff ff ff. */
static void expand(const canvas_t *c, unsigned char *frame, size_t line_len, int h, int w)
{
    for (int y = 0; y < h; y++) {
        uint32_t *row = (uint32_t *)(frame + (size_t)y * line_len);
        const unsigned char *src = &c->bits[(size_t)y * c->stride];
        for (int x = 0; x < w; x++)
            row[x] = (src[x / 8] & (0x80u >> (x % 8))) ? 0xFF000000u : 0xFFFFFFFFu;
    }
}

static int write_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int fb_present(fb_t *f, const canvas_t *c)
{
    expand(c, f->frame, f->line_len, f->h, f->w);
    if (lseek(f->fd, 0, SEEK_SET) < 0)
        return -1;
    return write_all(f->fd, f->frame, f->frame_sz);
}

/* Same frame, to a file -- lets rendering be verified without the panel. */
static int fb_dump(const canvas_t *c, const char *path)
{
    size_t line_len = (size_t)SCR_W * 4;
    unsigned char *frame = malloc(line_len * SCR_H);
    int fd, rc;
    if (!frame)
        return -1;
    expand(c, frame, line_len, SCR_H, SCR_W);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        free(frame);
        return -1;
    }
    rc = write_all(fd, frame, line_len * SCR_H);
    close(fd);
    free(frame);
    return rc;
}

/* -------------------------------------------------------------- gps state */

typedef struct {
    char sys; /* G R E B */
    int prn, elev, azim, snr, used;
} sat_t;

typedef struct {
    sat_t s[MAX_SATS];
    int n;
} satset_t;

typedef struct {
    int quality, mode, sats_used;
    double hdop, pdop, vdop;
    double lat, lon, alt_m, speed_kmh, course_deg;
    char utc[9], date[11];

    satset_t live;
    satset_t stage[NSYS];

    /* GSA can arrive before the GSV cycle that creates the satellites, so the
     * PRN lists are stored and re-applied whenever either changes. */
    int gsa_prn[NSYS][12], gsa_n[NSYS];

    unsigned long lines, bad_crc, unknown;
    time_t last_data;
    int connected;
} gps_t;

static void gps_init(gps_t *g)
{
    memset(g, 0, sizeof *g);
    g->lat = g->lon = NAN;
    g->alt_m = g->speed_kmh = g->course_deg = NAN;
    g->hdop = g->pdop = g->vdop = NAN;
    strcpy(g->utc, "--:--:--");
    strcpy(g->date, "----------");
}

static int sys_idx(char s)
{
    switch (s) {
    case 'G': return 0;
    case 'R': return 1;
    case 'E': return 2;
    case 'B': return 3;
    default:  return 4;
    }
}

/* GPS 1-32, SBAS 33-64, GLONASS 65-96 -- used to attribute GN-talker PRNs. */
static char sys_from_prn(int prn)
{
    if (prn >= 65 && prn <= 96)
        return 'R';
    if (prn >= 1 && prn <= 64)
        return 'G';
    return '?';
}

static void gps_apply_used(gps_t *g)
{
    for (int i = 0; i < g->live.n; i++)
        g->live.s[i].used = 0;
    for (int si = 0; si < NSYS; si++)
        for (int k = 0; k < g->gsa_n[si]; k++)
            for (int i = 0; i < g->live.n; i++)
                if (g->live.s[i].prn == g->gsa_prn[si][k] &&
                    (si == 4 || sys_idx(g->live.s[i].sys) == si))
                    g->live.s[i].used = 1;
}

/* Replace every satellite of one constellation with the freshly staged cycle. */
static void gps_publish(gps_t *g, char sys)
{
    satset_t out;
    int si = sys_idx(sys);
    out.n = 0;
    for (int i = 0; i < g->live.n; i++)
        if (g->live.s[i].sys != sys && out.n < MAX_SATS)
            out.s[out.n++] = g->live.s[i];
    for (int i = 0; i < g->stage[si].n && out.n < MAX_SATS; i++)
        out.s[out.n++] = g->stage[si].s[i];
    g->live = out;
    gps_apply_used(g);
}

static int gps_order(const gps_t *g, int by_snr, int *idx, int max)
{
    int n = g->live.n < max ? g->live.n : max;
    for (int i = 0; i < n; i++)
        idx[i] = i;
    for (int i = 1; i < n; i++) { /* insertion sort, n is tiny */
        int k = idx[i], j = i - 1;
        while (j >= 0) {
            int a = idx[j], worse;
            if (by_snr)
                worse = g->live.s[a].snr < g->live.s[k].snr;
            else
                worse = g->live.s[a].prn > g->live.s[k].prn;
            if (!worse)
                break;
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = k;
    }
    return n;
}

static int gps_count(const gps_t *g, char sys, int *used)
{
    int n = 0;
    *used = 0;
    for (int i = 0; i < g->live.n; i++)
        if (g->live.s[i].sys == sys) {
            n++;
            if (g->live.s[i].used)
                (*used)++;
        }
    return n;
}

/* ----------------------------------------------------------- nmea parsing */

typedef struct {
    char buf[160];
    size_t len;
    int overflow;
} nmea_rx_t;

static int csum_ok(const char *l, size_t n)
{
    unsigned char x = 0;
    size_t i = 1;
    if (n < 4 || l[0] != '$')
        return 0;
    for (; i < n && l[i] != '*'; i++)
        x ^= (unsigned char)l[i];
    if (i + 2 >= n + 1 || l[i] != '*')
        return 0;
    char hex[3] = {l[i + 1], l[i + 2], 0};
    return (unsigned)strtoul(hex, NULL, 16) == x;
}

static int split(char *l, char *f[], int maxf)
{
    int n = 0;
    char *p = l;
    char *star = strchr(l, '*');
    if (star)
        *star = 0;
    f[n++] = p;
    while ((p = strchr(p, ',')) && n < maxf) {
        *p++ = 0;
        f[n++] = p;
    }
    return n;
}

/* NMEA leaves fields empty rather than zero, so absent must not read as 0. */
static double fnum(char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || !*f[i])
        return NAN;
    return atof(f[i]);
}

static int fint(char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || !*f[i])
        return -1;
    return atoi(f[i]);
}

/* ddmm.mmmm / dddmm.mmmm -> signed degrees. */
static double coord(char *f[], int nf, int vi, int hi)
{
    double v = fnum(f, nf, vi);
    if (isnan(v))
        return NAN;
    double deg = floor(v / 100.0);
    double d = deg + (v - deg * 100.0) / 60.0;
    if (hi < nf && f[hi] && (*f[hi] == 'S' || *f[hi] == 'W'))
        d = -d;
    return d;
}

static void set_utc(gps_t *g, char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || strlen(f[i]) < 6)
        return;
    snprintf(g->utc, sizeof g->utc, "%c%c:%c%c:%c%c",
             f[i][0], f[i][1], f[i][2], f[i][3], f[i][4], f[i][5]);
}

static void set_date(gps_t *g, char *f[], int nf, int i)
{
    if (i >= nf || !f[i] || strlen(f[i]) < 6)
        return;
    snprintf(g->date, sizeof g->date, "20%c%c-%c%c-%c%c",
             f[i][4], f[i][5], f[i][2], f[i][3], f[i][0], f[i][1]);
}

static void nmea_apply(gps_t *g, char *f[], int nf)
{
    const char *tag = f[0];
    char sys;
    if (nf < 2 || strlen(tag) < 6)
        return;
    switch (tag[2]) {
    case 'P': sys = 'G'; break;
    case 'L': sys = 'R'; break;
    case 'A': sys = 'E'; break;
    case 'B': sys = 'B'; break;
    case 'N': sys = 'N'; break; /* combined */
    default:  sys = '?'; break;
    }
    const char *type = tag + 3;

    if (!strncmp(type, "GGA", 3)) {
        set_utc(g, f, nf, 1);
        g->lat = coord(f, nf, 2, 3);
        g->lon = coord(f, nf, 4, 5);
        g->quality = fint(f, nf, 6);
        g->sats_used = fint(f, nf, 7);
        g->hdop = fnum(f, nf, 8);
        g->alt_m = fnum(f, nf, 9);
    } else if (!strncmp(type, "RMC", 3)) {
        set_utc(g, f, nf, 1);
        if (nf > 3) {
            double la = coord(f, nf, 3, 4), lo = coord(f, nf, 5, 6);
            if (!isnan(la))
                g->lat = la;
            if (!isnan(lo))
                g->lon = lo;
        }
        double kn = fnum(f, nf, 7);
        g->speed_kmh = isnan(kn) ? NAN : kn * 1.852;
        g->course_deg = fnum(f, nf, 8);
        set_date(g, f, nf, 9);
    } else if (!strncmp(type, "GSA", 3)) {
        int si = sys_idx(sys == 'N' ? '?' : sys);
        int m = fint(f, nf, 2);
        if (m > 0)
            g->mode = m;
        g->gsa_n[si] = 0;
        for (int i = 3; i <= 14 && i < nf; i++) {
            int prn = fint(f, nf, i);
            if (prn > 0 && g->gsa_n[si] < 12)
                g->gsa_prn[si][g->gsa_n[si]++] = prn;
        }
        double p = fnum(f, nf, 15), h = fnum(f, nf, 16), v = fnum(f, nf, 17);
        if (!isnan(p)) g->pdop = p;
        if (!isnan(h)) g->hdop = h;
        if (!isnan(v)) g->vdop = v;
        gps_apply_used(g);
    } else if (!strncmp(type, "GSV", 3)) {
        int total = fint(f, nf, 1), msg = fint(f, nf, 2);
        char psys = (sys == 'N' || sys == '?') ? 0 : sys;
        int si;
        if (total < 1 || msg < 1)
            return;
        /* A GN-talker GSV has no constellation of its own; attribute per PRN. */
        si = sys_idx(psys ? psys : 'G');
        if (msg == 1)
            g->stage[si].n = 0;
        for (int i = 4; i + 3 <= nf && i + 3 <= 20; i += 4) {
            int prn = fint(f, nf, i);
            if (prn <= 0)
                continue;
            sat_t s;
            s.sys = psys ? psys : sys_from_prn(prn);
            s.prn = prn;
            s.elev = fint(f, nf, i + 1);
            s.azim = fint(f, nf, i + 2);
            s.snr = fint(f, nf, i + 3);
            s.used = 0;
            if (g->stage[si].n < MAX_SATS)
                g->stage[si].s[g->stage[si].n++] = s;
        }
        if (msg == total)
            gps_publish(g, psys ? psys : 'G');
    } else {
        g->unknown++;
    }
}

static void nmea_line(gps_t *g, char *l, size_t n)
{
    char *f[32];
    g->lines++;
    if (!csum_ok(l, n)) {
        g->bad_crc++;
        return;
    }
    int nf = split(l, f, 32);
    nmea_apply(g, f, nf);
    g->last_data = time(NULL);
}

static void nmea_feed(nmea_rx_t *rx, gps_t *g, const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            if (rx->len && !rx->overflow) {
                rx->buf[rx->len] = 0;
                nmea_line(g, rx->buf, rx->len);
            }
            rx->len = 0;
            rx->overflow = 0;
            continue;
        }
        if (rx->len + 1 >= sizeof rx->buf) {
            /* Discard to the next terminator rather than truncate into a
             * sentence that would parse as something else. */
            rx->overflow = 1;
            rx->len = 0;
            continue;
        }
        rx->buf[rx->len++] = c;
    }
}

/* ------------------------------------------------------------------- port */

typedef struct {
    int fd;
    char path[128];
    int baud, replay;
    time_t retry_at;
} port_t;

static speed_t baud_of(int b)
{
    switch (b) {
    case 4800:   return B4800;
    case 9600:   return B9600;
    case 19200:  return B19200;
    case 38400:  return B38400;
    case 57600:  return B57600;
    case 115200: return B115200;
    default:     return B9600;
    }
}

static int port_open(port_t *p)
{
    p->fd = open(p->path, p->replay ? O_RDONLY : (O_RDWR | O_NOCTTY | O_NONBLOCK));
    if (p->fd < 0)
        return -1;
    if (!p->replay && isatty(p->fd)) {
        struct termios t;
        if (tcgetattr(p->fd, &t) == 0) {
            cfmakeraw(&t);
            t.c_cflag |= CLOCAL | CREAD | CS8;
            t.c_cflag &= (unsigned)~CRTSCTS;
            t.c_cc[VMIN] = 0;
            t.c_cc[VTIME] = 0;
            cfsetispeed(&t, baud_of(p->baud));
            cfsetospeed(&t, baud_of(p->baud));
            tcsetattr(p->fd, TCSANOW, &t);
        }
    }
    return 0;
}

static void port_close(port_t *p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
}

/* ------------------------------------------------------------- view state */

typedef enum { PAGE_BARS = 0, PAGE_SKY = 1 } page_t;

typedef struct {
    page_t page;
    int by_snr;
    char sel_sys;
    int sel_prn;
    int hold, grid, ascii;
} view_t;

static int sel_index(const gps_t *g, const view_t *v, const int *idx, int n)
{
    for (int i = 0; i < n; i++)
        if (g->live.s[idx[i]].sys == v->sel_sys && g->live.s[idx[i]].prn == v->sel_prn)
            return i;
    return n ? 0 : -1;
}

static int bar_px(int snr, int span)
{
    if (snr <= SNR_MIN)
        return 0;
    if (snr >= SNR_MAX)
        return span;
    int h = (int)lround((double)(snr - SNR_MIN) / (SNR_MAX - SNR_MIN) * span);
    return h < 2 ? 2 : h; /* anything above the floor must be visible */
}

static void fill_bar(canvas_t *c, const view_t *v, int x, int y, int w, int h, int used)
{
    if (used) {
        if (v->ascii) {
            rect(c, x, y, w, h, INK);
            for (int j = y + 1; j < y + h - 1; j += 2)
                hline(c, x + 1, j, w - 2, INK);
        } else {
            fillrect(c, x, y, w, h, INK);
        }
    } else {
        rect(c, x, y, w, h, INK);
        checker(c, x + 1, y + 1, w - 2, h - 2);
    }
}

#define STATUS_H (CH + 2)

static void status_bar(canvas_t *c, const gps_t *g, const char *page)
{
    char s[64];
    const char *mode = g->mode == 3 ? "3D" : g->mode == 2 ? "2D" : "NO";
    int inview = g->live.n;
    fillrect(c, 0, 0, c->w, STATUS_H, INK);
    if (isnan(g->hdop))
        snprintf(s, sizeof s, "%s %s %d/%dSAT", page, mode,
                 g->sats_used < 0 ? 0 : g->sats_used, inview);
    else
        snprintf(s, sizeof s, "%s %s %d/%dSAT H%.1f", page, mode,
                 g->sats_used < 0 ? 0 : g->sats_used, inview, g->hdop);
    draw_text(c, 3, 1, s, SC, PAPER);
    draw_text(c, c->w - text_w(g->utc, SC) - 3, 1, g->utc, SC, PAPER);
}

/* --------------------------------------------------------------- bars page */

static void view_bars(canvas_t *c, const gps_t *g, const view_t *v)
{
    int idx[MAX_SATS];
    int n = gps_order(g, v->by_snr, idx, MAX_SATS);
    int sel = sel_index(g, v, idx, n);

    int foot_y = c->h - 2 * CH;
    int base_y = foot_y - 4 - 2 * CH;   /* two stacked label rows */
    int top_y = STATUS_H + 2 + CH;      /* room for the SNR value above a bar */
    int span = base_y - top_y;
    int ax_x = text_w("50", SC) + 2;

    status_bar(c, g, "BARS");

    for (int db = SNR_MAX; db >= SNR_MIN; db -= 10) {
        char lb[8];
        int y = base_y - bar_px(db, span);
        snprintf(lb, sizeof lb, "%d", db);
        draw_text(c, 0, y - CH / 2, lb, SC, INK);
        for (int x = ax_x + 2; x < c->w; x += 6)
            px(c, x, y, INK);
    }
    vline(c, ax_x, top_y, span + 1, INK);
    hline(c, ax_x, base_y, c->w - ax_x, INK);

    /* A slot only as wide as its label leaves the labels touching, so cap the
     * number of bars at what can be labelled with a readable gap. gps_order()
     * sorted by SNR, so the ones dropped are the weakest, and the footer says
     * how many. */
    int avail = c->w - (ax_x + 6);
    int need = text_w("00", SC) + 6;
    int shown = n;
    if (n > 0 && avail / n < need) {
        shown = avail / need;
        if (shown < 1)
            shown = 1;
    }
    int slot = shown > 0 ? avail / shown : avail;
    if (slot > 34)
        slot = 34;
    int bw = slot - 6;
    if (bw < 3)
        bw = 3;

    for (int i = 0; i < shown; i++) {
        const sat_t *s = &g->live.s[idx[i]];
        int bx = ax_x + 6 + i * slot;
        int cx = bx + bw / 2;
        char lb[8];
        if (bx + bw > c->w)
            break;

        int h = bar_px(s->snr, span);
        if (h == 0) {
            /* Tracked but at/below the noise floor: keep the slot and labels,
             * draw a baseline stub so it is visibly present. */
            hline(c, bx, base_y - 1, bw, INK);
        } else {
            fill_bar(c, v, bx, base_y - h, bw, h, s->used);
        }
        if (s->snr > 0) {
            int ny = base_y - h - CH;
            if (ny < STATUS_H + 1)
                ny = STATUS_H + 1;
            snprintf(lb, sizeof lb, "%d", s->snr);
            draw_ctext(c, cx, ny, lb, SC, INK);
        }
        if (i == sel)
            rect(c, bx - 3, base_y - h - 2, bw + 6, h + 4, INK);

        snprintf(lb, sizeof lb, "%02d", s->prn % 100);
        draw_ctext(c, cx, base_y + 3, lb, SC, INK);
        lb[0] = s->sys;
        lb[1] = 0;
        draw_ctext(c, cx, base_y + 3 + CH, lb, SC, INK);
    }

    hline(c, 0, foot_y - 2, c->w, INK);

    char l1[64], l2[80];
    if (isnan(g->lat) || isnan(g->lon))
        snprintf(l1, sizeof l1, "NO FIX  %lu LINES CRC%lu", g->lines, g->bad_crc);
    else if (isnan(g->alt_m))
        snprintf(l1, sizeof l1, "%.5f%c %.5f%c", fabs(g->lat), g->lat < 0 ? 'S' : 'N',
                 fabs(g->lon), g->lon < 0 ? 'W' : 'E');
    else
        snprintf(l1, sizeof l1, "%.5f%c %.5f%c ALT%.0fM", fabs(g->lat),
                 g->lat < 0 ? 'S' : 'N', fabs(g->lon), g->lon < 0 ? 'W' : 'E',
                 g->alt_m);
    draw_text(c, 2, foot_y, l1, SC, INK);

    long age = g->last_data ? (long)(time(NULL) - g->last_data) : -1;
    if (sel >= 0) {
        const sat_t *s = &g->live.s[idx[sel]];
        char more[16] = "";
        if (shown < n)
            snprintf(more, sizeof more, " +%d", n - shown);
        snprintf(l2, sizeof l2, "%c%02d EL%d AZ%d AGE%lds%s 2=SKY", s->sys,
                 s->prn % 100, s->elev < 0 ? 0 : s->elev, s->azim < 0 ? 0 : s->azim,
                 age < 0 ? 0 : age, more);
    } else {
        snprintf(l2, sizeof l2, "%s AGE%lds", g->connected ? "NO SATS" : "NO DEVICE",
                 age < 0 ? 0 : age);
    }
    draw_text(c, 2, foot_y + CH, l2, SC, INK);
}

/* ---------------------------------------------------------------- sky page */

#define OCC_CELL 4
#define OCC_W (SCR_W / OCC_CELL)
#define OCC_H (SCR_H / OCC_CELL)

static unsigned char g_occ[OCC_W * OCC_H];

static void occ_reset(void) { memset(g_occ, 0, sizeof g_occ); }

static void occ_seed(int x, int y, int w, int h)
{
    for (int j = y; j < y + h; j += OCC_CELL)
        for (int i = x; i < x + w; i += OCC_CELL) {
            int cx = i / OCC_CELL, cy = j / OCC_CELL;
            if (cx >= 0 && cy >= 0 && cx < OCC_W && cy < OCC_H)
                g_occ[cy * OCC_W + cx] = 1;
        }
}

static int occ_claim(int x, int y, int w, int h, int bx0, int by0, int bx1, int by1)
{
    if (x < bx0 || y < by0 || x + w > bx1 || y + h > by1)
        return 0;
    for (int j = y; j < y + h; j += OCC_CELL)
        for (int i = x; i < x + w; i += OCC_CELL) {
            int cx = i / OCC_CELL, cy = j / OCC_CELL;
            if (cx < 0 || cy < 0 || cx >= OCC_W || cy >= OCC_H)
                return 0;
            if (g_occ[cy * OCC_W + cx])
                return 0;
        }
    occ_seed(x, y, w, h);
    return 1;
}

static void sky_pos(int cx, int cy, int r, int el, int az, int *x, int *y)
{
    double k = (1.0 - (el < 0 ? 0 : el) / 90.0) * r;
    double a = az * M_PI / 180.0;
    *x = cx + (int)lround(k * sin(a));
    *y = cy - (int)lround(k * cos(a));
}

static void view_sky(canvas_t *c, const gps_t *g, const view_t *v)
{
    int idx[MAX_SATS];
    int n = gps_order(g, 1, idx, MAX_SATS);
    int sel = sel_index(g, v, idx, n);

    int colw = 10 * CW + 4;
    int skx1 = c->w - colw;
    int y0 = STATUS_H + 2;
    int cx = skx1 / 2, cy = (y0 + c->h) / 2;
    int pad = CW + 6;
    int r = ((skx1 < c->h - y0 ? skx1 : c->h - y0) / 2) - pad;
    int mr = 6;

    status_bar(c, g, "SKY");
    vline(c, skx1 - 2, y0, c->h - y0, INK);

    if (v->grid < 2) {
        dots_circle(c, cx, cy, r, 9);
        if (v->grid == 0) {
            dots_circle(c, cx, cy, r * 2 / 3, 9);
            dots_circle(c, cx, cy, r / 3, 9);
        }
    }
    hline(c, cx - 4, cy, 9, INK);
    vline(c, cx, cy - 4, 9, INK);

    occ_reset();
    occ_seed(skx1 - 4, y0, colw + 4, c->h - y0);
    occ_seed(0, 0, c->w, STATUS_H + 2);

    struct { const char *t; int x, y; } card[4] = {
        {"N", cx - CW / 2, cy - r - CH},
        {"S", cx - CW / 2, cy + r + 2},
        {"W", cx - r - CW - 4, cy - CH / 2},
        {"E", cx + r + 4, cy - CH / 2},
    };
    for (int i = 0; i < 4; i++) {
        draw_text(c, card[i].x, card[i].y, card[i].t, SC, INK);
        occ_seed(card[i].x, card[i].y, CW, CH);
    }

    /* A satellite being acquired reports empty elevation/azimuth. Plotting that
     * as 0/0 would place it on the horizon due north, which is a plausible
     * looking lie -- so it is omitted from the plot and counted instead. */
    int sx[MAX_SATS], sy[MAX_SATS], haspos[MAX_SATS], nopos = 0;
    for (int i = 0; i < n; i++) {
        const sat_t *s = &g->live.s[idx[i]];
        haspos[i] = (s->elev >= 0 && s->azim >= 0);
        if (!haspos[i]) {
            nopos++;
            sx[i] = sy[i] = -1000;
            continue;
        }
        sky_pos(cx, cy, r, s->elev, s->azim, &sx[i], &sy[i]);
        occ_seed(sx[i] - mr - 1, sy[i] - mr - 1, 2 * mr + 3, 2 * mr + 3);
    }

    int hidden = 0;
    for (int k = -1; k < n; k++) {
        int i = (k < 0) ? sel : k;            /* selected first, then by SNR */
        if (i < 0 || (k >= 0 && i == sel))
            continue;
        const sat_t *s = &g->live.s[idx[i]];
        int x = sx[i], y = sy[i];
        if (!haspos[i])
            continue;

        if (s->snr <= 0)
            fillrect(c, x - 1, y - 1, 3, 3, INK);
        else if (s->used)
            disc(c, x, y, mr, INK);
        else {
            disc(c, x, y, mr, PAPER);
            circle(c, x, y, mr, INK);
        }
        if (i == sel)
            circle(c, x, y, mr + 3, INK);

        char full[8], num[8];
        snprintf(full, sizeof full, "%c%02d", s->sys, s->prn % 100);
        snprintf(num, sizeof num, "%02d", s->prn % 100);
        const char *tags[2] = {full, num};
        int placed = 0;
        for (int t = 0; t < 2 && !placed; t++) {
            int tw = text_w(tags[t], SC), gap = mr + 2;
            int cand[4][2] = {{x + gap, y - CH / 2},
                              {x - tw - gap, y - CH / 2},
                              {x - tw / 2, y - gap - CH},
                              {x - tw / 2, y + gap}};
            for (int a = 0; a < 4; a++)
                if (occ_claim(cand[a][0], cand[a][1], tw, CH, 0, y0, skx1 - 4, c->h)) {
                    draw_text(c, cand[a][0], cand[a][1], tags[t], SC, INK);
                    placed = 1;
                    break;
                }
        }
        if (!placed)
            hidden++;
    }

    /* Stats column: content rows first, blank spacers dropped when short. */
    char rows[16][32];
    int nr = 0, spacer[16];
    long age = g->last_data ? (long)(time(NULL) - g->last_data) : -1;
    int gu, gn, ru, rn;

#define ROW(sp, ...) do { if (nr < 16) { snprintf(rows[nr], 32, __VA_ARGS__); \
                          spacer[nr] = (sp); nr++; } } while (0)
    ROW(0, "SELECTED");
    if (sel >= 0) {
        const sat_t *s = &g->live.s[idx[sel]];
        ROW(0, " %c%02d %dDB", s->sys, s->prn % 100, s->snr < 0 ? 0 : s->snr);
        ROW(0, "EL%d AZ%d", s->elev < 0 ? 0 : s->elev, s->azim < 0 ? 0 : s->azim);
        ROW(0, " %s", s->used ? "IN FIX" : "UNUSED");
    } else {
        ROW(0, " NONE");
    }
    ROW(1, "%s", "");
    if (!isnan(g->pdop)) ROW(0, "DOP P%.1f", g->pdop);
    if (!isnan(g->hdop)) ROW(0, " H%.1f", g->hdop);
    if (!isnan(g->vdop)) ROW(0, " V%.1f", g->vdop);
    ROW(1, "%s", "");
    gn = gps_count(g, 'G', &gu);
    rn = gps_count(g, 'R', &ru);
    if (gn) ROW(0, "GPS %d/%d", gu, gn);
    if (rn) ROW(0, "GLO %d/%d", ru, rn);
    ROW(1, "%s", "");
    if (nopos)
        ROW(0, "%d NO POS", nopos);
    if (hidden)
        ROW(0, "+%dHID", hidden);
    ROW(0, "AGE %lds", age < 0 ? 0 : age);
    ROW(0, "1=BARS");
#undef ROW

    int maxr = (c->h - (y0 + 2)) / CH;
    while (nr > maxr) {
        int cut = -1;
        for (int i = nr - 1; i >= 0; i--)
            if (spacer[i]) { cut = i; break; }
        if (cut < 0)
            break;
        for (int i = cut; i < nr - 1; i++) {
            memcpy(rows[i], rows[i + 1], 32);
            spacer[i] = spacer[i + 1];
        }
        nr--;
    }
    for (int i = 0; i < nr && i < maxr; i++)
        draw_text(c, skx1 + 2, y0 + 2 + i * CH, rows[i], SC, INK);
}

/* ------------------------------------------------------------------ demo */

static void load_demo(gps_t *g)
{
    static const struct { char s; int prn, el, az, snr, used; } d[] = {
        {'G', 2, 67, 210, 44, 1}, {'G', 5, 54, 118, 41, 1},
        {'G', 7, 41, 302, 38, 1}, {'G', 13, 33, 95, 35, 1},
        {'G', 21, 28, 201, 33, 1}, {'G', 19, 36, 240, 31, 1},
        {'G', 24, 22, 155, 29, 0}, {'R', 66, 61, 340, 28, 1},
        {'R', 73, 44, 55, 26, 1},  {'R', 75, 19, 28, 22, 1},
        {'G', 30, 12, 266, 19, 0}, {'R', 81, 15, 70, 15, 0},
        {'R', 84, 9, 300, 11, 0},  {'G', 11, 8, 190, 8, 0},
    };
    g->live.n = 0;
    for (size_t i = 0; i < sizeof d / sizeof d[0]; i++) {
        sat_t s = {d[i].s, d[i].prn, d[i].el, d[i].az, d[i].snr, d[i].used};
        g->live.s[g->live.n++] = s;
    }
    g->mode = 3;
    g->quality = 1;
    g->sats_used = 9;
    g->hdop = 0.9;
    g->pdop = 1.4;
    g->vdop = 1.1;
    g->lat = 13.75632;
    g->lon = 100.50184;
    g->alt_m = 18.4;
    g->speed_kmh = 0.31;
    g->course_deg = 142.3;
    strcpy(g->utc, "12:34:56");
    strcpy(g->date, "2026-07-29");
    g->connected = 1;
    g->last_data = time(NULL);
}

static void print_state(const gps_t *g)
{
    printf("mode=%d quality=%d used=%d inview=%d\n", g->mode, g->quality,
           g->sats_used, g->live.n);
    printf("lat=%.6f lon=%.6f alt=%.1f spd=%.2f crs=%.1f\n", g->lat, g->lon,
           g->alt_m, g->speed_kmh, g->course_deg);
    printf("dop p=%.1f h=%.1f v=%.1f  utc=%s date=%s\n", g->pdop, g->hdop,
           g->vdop, g->utc, g->date);
    printf("lines=%lu bad_crc=%lu unknown=%lu\n", g->lines, g->bad_crc, g->unknown);
    for (int i = 0; i < g->live.n; i++) {
        const sat_t *s = &g->live.s[i];
        printf("  %c%02d el=%3d az=%3d snr=%3d %s\n", s->sys, s->prn, s->elev,
               s->azim, s->snr, s->used ? "USED" : "-");
    }
}

/* ------------------------------------------------------------------ evdev
 *
 * On the Beepy's own console the keyboard cannot be read from stdin: fbterm is
 * what feeds keystrokes to the pty, and it is SIGSTOPped while we own the
 * panel. Reading /dev/input/event* bypasses it entirely. The beepy user is in
 * group "input", so this needs no privileges.
 */

#define MAX_EVDEV 4
static int g_ev[MAX_EVDEV];
static int g_evn;

static int bit_set(const unsigned long *b, int n)
{
    return (b[n / (8 * sizeof(unsigned long))] >>
            (n % (8 * sizeof(unsigned long)))) & 1UL;
}

static void evdev_open(int grab)
{
    for (int i = 0; i < 32 && g_evn < MAX_EVDEV; i++) {
        char path[32];
        unsigned long bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
                           (8 * sizeof(unsigned long))];
        int fd;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0)
            continue;
        memset(bits, 0, sizeof bits);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0 ||
            !bit_set(bits, KEY_Q) || !bit_set(bits, KEY_TAB)) {
            close(fd); /* not a keyboard */
            continue;
        }
        /* Grabbing stops the keys we consume from also queueing up on the
         * console, which would otherwise spill into the shell on exit. The
         * kernel releases the grab when the fd closes, including on a crash. */
        if (grab)
            ioctl(fd, EVIOCGRAB, 1);
        g_ev[g_evn++] = fd;
    }
}

static void evdev_close(void)
{
    for (int i = 0; i < g_evn; i++) {
        ioctl(g_ev[i], EVIOCGRAB, 0);
        close(g_ev[i]);
    }
    g_evn = 0;
}

/* Letter aliases exist because the Beepy's digits need the Alt/symbol
 * modifier, which makes "1"/"2" awkward on the physical keyboard. */
static int keycode_to_char(int code)
{
    switch (code) {
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_TAB: return '\t';
    case KEY_B: return 'b';
    case KEY_K: return 'k';
    case KEY_V: return 'v';
    case KEY_N: return 'n';
    case KEY_P: return 'p';
    case KEY_S: return 's';
    case KEY_G: return 'g';
    case KEY_H: return 'h';
    case KEY_R: return 'r';
    case KEY_A: return 'a';
    case KEY_Q: return 'q';
    case KEY_ESC: return 'q';
    default: return 0;
    }
}

/* ------------------------------------------------------------------- main */

static volatile sig_atomic_t g_quit;

static int is_evdev(int fd)
{
    for (int i = 0; i < g_evn; i++)
        if (g_ev[i] == fd)
            return 1;
    return 0;
}

static void handle_key(int k, gps_t *gps, view_t *view, port_t *port)
{
    int idx[MAX_SATS];
    int n = gps_order(gps, view->by_snr, idx, MAX_SATS);
    int cur = sel_index(gps, view, idx, n);

    switch (k) {
    case 'q': case 'Q': case 3: g_quit = 1; break;
    case '1': case 'b': case 'B': view->page = PAGE_BARS; break;
    case '2': case 'k': case 'K': view->page = PAGE_SKY; break;
    case '\t': case 'v': case 'V':
        view->page = view->page == PAGE_BARS ? PAGE_SKY : PAGE_BARS;
        break;
    case 's': case 'S': view->by_snr = !view->by_snr; break;
    case 'g': case 'G': view->grid = (view->grid + 1) % 3; break;
    case 'h': case 'H': view->hold = !view->hold; break;
    case 'a': case 'A': view->ascii = !view->ascii; break;
    case 'r': case 'R':
        port_close(port);
        gps->connected = 0;
        port->retry_at = 0;
        break;
    case 'n': case 'N':
        if (n) {
            cur = (cur + 1) % n;
            view->sel_sys = gps->live.s[idx[cur]].sys;
            view->sel_prn = gps->live.s[idx[cur]].prn;
        }
        break;
    case 'p': case 'P':
        if (n) {
            cur = (cur + n - 1) % n;
            view->sel_sys = gps->live.s[idx[cur]].sys;
            view->sel_prn = gps->live.s[idx[cur]].prn;
        }
        break;
    default: break;
    }
}
static fb_t *g_fb;
static struct termios g_tty_saved;
static int g_tty_raw;

static void on_signal(int s)
{
    (void)s;
    g_quit = 1;
}

static void cleanup(void)
{
    if (g_tty_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);
    evdev_close();
    if (g_fb)
        fb_release(g_fb);
}

static void tty_raw(void)
{
    struct termios t;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_tty_saved) < 0)
        return;
    t = g_tty_saved;
    t.c_lflag &= (unsigned)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
        g_tty_raw = 1;
}

static void usage(void)
{
    fputs("usage: gps-monitor [options]\n"
          "  -d DEV        serial device (default /dev/ttyACM0)\n"
          "  -b BAUD       line speed (default 9600)\n"
          "  --replay F    read NMEA from a file instead of a device\n"
          "  --log F       append received sentences to F\n"
          "  --demo        built-in synthetic satellite set, no receiver\n"
          "  --print       dump parsed state as text and exit\n"
          "  --dump F      render one frame to F (384000 raw bytes) and exit\n"
          "  --page bars|sky   initial page (default bars)\n"
          "  --cycle N     auto-switch pages every N seconds\n"
          "  --seconds N   exit after N seconds\n"
          "  --fbdev PATH  framebuffer (default /dev/fb1)\n"
          "  --ascii       hatch fills instead of solid, for panel comparison\n"
          "  --no-evdev    do not read /dev/input/event* (stdin keys only)\n"
          "  --no-grab     read evdev without grabbing it\n"
          "keys: 1 or B bars   2 or K sky   TAB or V toggle\n"
          "      n/p select  s sort  g grid  h hold  r reopen  q quit\n"
          "      letter keys avoid the Alt modifier the digits need\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/ttyACM0", *fbpath = "/dev/fb1";
    const char *replay = NULL, *logpath = NULL, *dumppath = NULL;
    int baud = 9600, demo = 0, do_print = 0, cycle = 0, seconds = 0;
    int use_evdev = 1, grab = 1;
    gps_t gps;
    view_t view = {PAGE_BARS, 1, 'G', 2, 0, 0, 0};
    nmea_rx_t rx = {{0}, 0, 0};
    port_t port = {-1, "", 9600, 0, 0};
    canvas_t *cv;
    FILE *logf = NULL;
    fb_t fb;
    time_t start, last_draw = 0, last_cycle;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(a, "-b") && i + 1 < argc) baud = atoi(argv[++i]);
        else if (!strcmp(a, "--replay") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(a, "--log") && i + 1 < argc) logpath = argv[++i];
        else if (!strcmp(a, "--dump") && i + 1 < argc) dumppath = argv[++i];
        else if (!strcmp(a, "--fbdev") && i + 1 < argc) fbpath = argv[++i];
        else if (!strcmp(a, "--cycle") && i + 1 < argc) cycle = atoi(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(a, "--page") && i + 1 < argc)
            view.page = !strcmp(argv[++i], "sky") ? PAGE_SKY : PAGE_BARS;
        else if (!strcmp(a, "--demo")) demo = 1;
        else if (!strcmp(a, "--print")) do_print = 1;
        else if (!strcmp(a, "--ascii")) view.ascii = 1;
        else if (!strcmp(a, "--no-evdev")) use_evdev = 0;
        else if (!strcmp(a, "--no-grab")) grab = 0;
        else usage();
    }

    gps_init(&gps);
    if (demo)
        load_demo(&gps);

    if (!(cv = canvas_new(SCR_W, SCR_H))) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (logpath && !(logf = fopen(logpath, "a")))
        fprintf(stderr, "warning: cannot open %s: %s\n", logpath, strerror(errno));

    snprintf(port.path, sizeof port.path, "%s", replay ? replay : dev);
    port.baud = baud;
    port.replay = replay != NULL;

    if (!demo) {
        if (port_open(&port) == 0)
            gps.connected = 1;
        else if (replay || dumppath || do_print) {
            fprintf(stderr, "cannot open %s: %s\n", port.path, strerror(errno));
            return 1;
        }
    }

    /* --print / --dump consume the whole source, render once, and exit: this is
     * how rendering is verified without the panel or a receiver. */
    if (do_print || dumppath) {
        if (!demo && port.fd >= 0) {
            /* A serial port never reaches EOF, so collect for a bounded window
             * instead of reading until zero -- otherwise a live receiver yields
             * only whatever happened to be buffered. A replay file does EOF, so
             * it is drained normally. */
            time_t deadline = time(NULL) + (seconds > 0 ? seconds : 3);
            char buf[512];
            ssize_t k;
            for (;;) {
                if (port.replay) {
                    if ((k = read(port.fd, buf, sizeof buf)) <= 0)
                        break;
                    nmea_feed(&rx, &gps, buf, (size_t)k);
                    continue;
                }
                if (time(NULL) >= deadline)
                    break;
                struct pollfd pf = {port.fd, POLLIN, 0};
                if (poll(&pf, 1, 500) > 0 && (pf.revents & POLLIN)) {
                    k = read(port.fd, buf, sizeof buf);
                    if (k > 0)
                        nmea_feed(&rx, &gps, buf, (size_t)k);
                }
            }
        }
        if (do_print)
            print_state(&gps);
        if (dumppath) {
            canvas_clear(cv, PAPER);
            if (view.page == PAGE_SKY)
                view_sky(cv, &gps, &view);
            else
                view_bars(cv, &gps, &view);
            if (fb_dump(cv, dumppath) < 0) {
                fprintf(stderr, "dump %s: %s\n", dumppath, strerror(errno));
                return 1;
            }
            fprintf(stderr, "wrote %s\n", dumppath);
        }
        return 0;
    }

    if (fb_open(&fb, fbpath) < 0)
        return 1;
    if (fb.w != SCR_W || fb.h != SCR_H)
        fprintf(stderr, "warning: panel is %dx%d, layout assumes %dx%d\n",
                fb.w, fb.h, SCR_W, SCR_H);
    g_fb = &fb;
    atexit(cleanup);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    tty_raw();
    if (use_evdev)
        evdev_open(grab);
    fb_take(&fb);

    start = last_cycle = time(NULL);

    while (!g_quit) {
        struct pollfd pfd[2 + MAX_EVDEV];
        int np = 0, ptimeout = 500;
        if (port.fd >= 0) {
            pfd[np].fd = port.fd;
            pfd[np].events = POLLIN;
            np++;
        }
        /* Only watch stdin when it is a terminal: with stdin closed or a pipe
         * (a non-interactive ssh run) poll would report POLLHUP forever and
         * spin the loop. */
        if (g_tty_raw) {
            pfd[np].fd = STDIN_FILENO;
            pfd[np].events = POLLIN;
            np++;
        }
        for (int i = 0; i < g_evn; i++) {
            pfd[np].fd = g_ev[i];
            pfd[np].events = POLLIN;
            np++;
        }

        if (poll(pfd, (unsigned)np, ptimeout) > 0) {
            for (int i = 0; i < np; i++) {
                if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                if (g_tty_raw && pfd[i].fd == STDIN_FILENO) {
                    char k;
                    while (read(STDIN_FILENO, &k, 1) == 1)
                        handle_key(k, &gps, &view, &port);
                } else if (is_evdev(pfd[i].fd)) {
                    struct input_event ev;
                    while (read(pfd[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev)
                        if (ev.type == EV_KEY && ev.value == 1) {
                            int k = keycode_to_char(ev.code);
                            if (k)
                                handle_key(k, &gps, &view, &port);
                        }
                } else {
                    char buf[512];
                    ssize_t k = read(port.fd, buf, sizeof buf);
                    if (k > 0) {
                        nmea_feed(&rx, &gps, buf, (size_t)k);
                        if (logf) {
                            fwrite(buf, 1, (size_t)k, logf);
                            fflush(logf);
                        }
                    } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                        /* Receiver vanished (this one flaps): drop the fd and
                         * retry, keeping the last known state on screen. */
                        port_close(&port);
                        gps.connected = 0;
                        port.retry_at = time(NULL) + 1;
                    }
                }
            }
        }

        time_t now = time(NULL);
        /* Must exclude demo: otherwise this reopen path opens the real device
         * on the first iteration and live NMEA overwrites the demo data. */
        if (!demo && port.fd < 0 && !port.replay && now >= port.retry_at) {
            if (port_open(&port) == 0) {
                gps.connected = 1;
                rx.len = 0;
            } else {
                port.retry_at = now + 1;
            }
        }
        if (cycle > 0 && now - last_cycle >= cycle) {
            view.page = view.page == PAGE_BARS ? PAGE_SKY : PAGE_BARS;
            last_cycle = now;
        }
        if (seconds > 0 && now - start >= seconds)
            break;

        if (!view.hold || last_draw == 0) {
            canvas_clear(cv, PAPER);
            if (view.page == PAGE_SKY)
                view_sky(cv, &gps, &view);
            else
                view_bars(cv, &gps, &view);
            fb_present(&fb, cv);
            last_draw = now;
        }
    }

    if (logf)
        fclose(logf);
    port_close(&port);
    return 0;
}

# gps-monitor — code structure

Implementation plan for the design in `DESIGN.md`. **XL font scale, two full-screen
pages (bars, sky), framebuffer front-end.**

Status: **implemented, built and verified on the device.** Single file
`gps-monitor.c` (~1180 lines), installed at `/usr/local/bin/gps-monitor`.
Built one big file, no ncurses, as requested -- the module layout in section 2
below describes the sections within that file rather than separate sources.

---

## 1. The font decision, and what it forces

The mockups use TrueType at 13–17 px. C has no TrueType, so text comes from an
**embedded 5×7 bitmap font in a 6×8 cell, drawn at integer scale**. That quantises
text to two sizes — scale 1 (6×8) and scale 2 (12×16) — and scale 2 is the XL body
size. Consequences that change the layout versus the PNG mockups:

| At scale 2 | Width | Fits? |
|---|---|---|
| Char advance | 12 px | — |
| Status `BARS 3D 9/14SAT HDOP0.9` + `12:34:56Z` | 276 + 108 = 384 px | yes, 16 px spare |
| Bar label `G02` horizontal | 36 px | **no** — slot is 26 px |
| Bar label `02` over `G` stacked | 24 px | yes |
| Axis label `50` | 24 px → 30 px gutter | yes |
| Footer line | 33 chars max | see below |

So the bars page **stacks** its labels, which is what the regenerated mockup now
shows. Two footer strings must be shortened to fit 33 characters:

```
SEL G02 EL67 AZ210  AGE0S CRC0  2=SKY   (37) -> SEL G02 EL67 AZ210 AGE0S 2=SKY  (30)
DOP P1.4 H0.9                    (13 ch = 156 px column) -> DOP P1.4 / H0.9 V1.1
```

Sky radius becomes 88 px (height-limited, not width-limited), so narrowing the
stats column buys nothing and the shorter DOP lines are free.

Glyph set: `0-9 A-Z` plus `. : / + - = ° space` — about 45 glyphs × 7 bytes ≈ 315
bytes of table. No lowercase, which is why all on-screen text is upper case.

---

## 2. File layout

```
gps-monitor/
  Makefile
  src/
    gps.h          types, constants, satellite ordering
    nmea.h  nmea.c framing, checksum, field split, GGA/RMC/GSA/GSV
    serial.h serial.c  port open/configure/read/reopen, --replay file source
    font.h  font.c  5x7 glyph table + text metrics
    canvas.h canvas.c  1bpp backbuffer + drawing primitives
    fbdev.h fbdev.c    /dev/fb1, 1bpp -> XRGB expand, fbterm pause/resume
    view.h         page/selection state, shared render helpers
    view_bars.c    bars page
    view_sky.c     sky page: projection + label placement
    input.h input.c    raw tty key reader
    main.c         args, poll loop, dispatch, signals, cleanup
  tools/
    mknmea.sh      synthetic NMEA generator (to write)
    mockup.py      design mockups (done)
    png2fb.py      PNG -> raw frame (done)
    fbshow         panel preview with readback verify (done)
  DESIGN.md  STRUCTURE.md  README.md
```

Modules rather than one file: `canvas` and `font` know nothing about GPS, `nmea`
knows nothing about drawing, and the two views are independently reviewable. It
also means phase 1 below is testable before any NMEA code exists.

---

## 3. Types

```c
/* gps.h */
#define MAX_SATS 64
#define NSYS      5              /* G R E B ? */
#define SNR_MIN  10
#define SNR_MAX  50

typedef struct {
    char sys;                    /* 'G' GPS 'R' GLONASS 'E' Galileo 'B' BeiDou */
    int  prn;
    int  elev, azim;             /* degrees, -1 = unknown */
    int  snr;                    /* dBHz, -1 = no signal reported */
    int  used;                   /* listed in a GSA PRN set */
} sat_t;

typedef struct { sat_t s[MAX_SATS]; int n; } satset_t;

typedef struct {
    int    quality;              /* GGA fld 6 */
    int    mode;                 /* GSA fld 2: 1 none 2 = 2D 3 = 3D */
    int    sats_used;
    double hdop, pdop, vdop;
    double lat, lon;             /* signed degrees, NAN = unknown */
    double alt_m, speed_kmh, course_deg;
    char   utc[9];               /* "HH:MM:SS" */
    char   date[11];             /* "YYYY-MM-DD" */

    satset_t live;               /* what gets drawn */
    satset_t stage[NSYS];        /* per-constellation GSV accumulation */

    unsigned long lines, bad_crc, unknown;
    time_t last_data;
    int    connected;
} gps_t;

void gps_init(gps_t *g);
int  gps_order(const gps_t *g, int by_snr, int *idx, int max);  /* -> count */
int  gps_count(const gps_t *g, char sys, int *used);            /* per-system */
```

```c
/* view.h */
typedef enum { PAGE_BARS = 0, PAGE_SKY } page_t;

typedef struct {
    page_t page;
    int    by_snr;               /* sort order */
    int    sel;                  /* selected satellite index into live */
    int    hold;                 /* freeze display */
    int    grid;                 /* sky grid density 0..2 */
    int    ascii;                /* '#' fill instead of solid, for comparison */
} view_t;

void view_bars_draw(canvas_t *c, const gps_t *g, const view_t *v);
void view_sky_draw (canvas_t *c, const gps_t *g, const view_t *v);
```

---

## 4. Key prototypes

```c
/* canvas.h -- 1bpp, renderer-agnostic, no GPS knowledge */
typedef struct { int w, h, stride; unsigned char *bits; } canvas_t;

canvas_t *canvas_new(int w, int h);
void canvas_clear (canvas_t *, int ink);
void canvas_px    (canvas_t *, int x, int y, int ink);
void canvas_hline (canvas_t *, int x, int y, int w, int ink);
void canvas_vline (canvas_t *, int x, int y, int h, int ink);
void canvas_rect  (canvas_t *, int x, int y, int w, int h, int ink);   /* outline */
void canvas_fill  (canvas_t *, int x, int y, int w, int h, int ink);
void canvas_checker(canvas_t *, int x, int y, int w, int h);           /* 50% dither */
void canvas_circle (canvas_t *, int cx, int cy, int r, int ink);       /* midpoint */
void canvas_disc   (canvas_t *, int cx, int cy, int r, int ink);
void canvas_dots   (canvas_t *, int cx, int cy, int r, int step);      /* grid ring */
void canvas_text   (canvas_t *, int x,  int y, const char *, int scale, int ink);
void canvas_ctext  (canvas_t *, int cx, int y, const char *, int scale, int ink);
```

```c
/* fbdev.h */
typedef struct {
    int    fd;
    int    w, h;
    size_t frame_sz;             /* 384000 */
    unsigned char *frame;        /* XRGB scratch, reused every present */
    pid_t  paused[8];            /* fbterm pids we SIGSTOPped */
    int    npaused;
} fb_t;

int  fb_open   (fb_t *, const char *path);       /* validates 400x240x32 */
void fb_take   (fb_t *);                         /* SIGSTOP fbterm */
void fb_release(fb_t *);                         /* SIGCONT + tmux refresh */
int  fb_present(fb_t *, const canvas_t *);       /* expand + write(384000) */
int  fb_dump   (const canvas_t *, const char *path);  /* --dump, no device */
```

```c
/* nmea.h */
typedef struct { char buf[160]; size_t len; int overflow; } nmea_rx_t;

void nmea_rx_init(nmea_rx_t *);
void nmea_feed   (nmea_rx_t *, const char *data, size_t n, gps_t *);
int  nmea_csum_ok(const char *line, size_t len);
int  nmea_split  (char *line, char *f[], int maxf);   /* in place, -> nfields */
void nmea_apply  (gps_t *, char *f[], int nf);        /* dispatch talker+type */
```

```c
/* serial.h */
typedef struct {
    int  fd;
    char path[128];
    int  baud;
    int  replay;                 /* reading a file, pace to wall clock */
    int  retry_at;               /* uptime seconds of next reopen attempt */
} port_t;

int  port_open (port_t *, const char *path, int baud, int replay);
int  port_read (port_t *, char *buf, size_t n);   /* 0 = none, -1 = fatal */
void port_close(port_t *);
```

---

## 5. Main loop

```
parse args
gps_init, port_open, fb_open, fb_take, input_raw
atexit(cleanup); SIGINT/SIGTERM/SIGHUP -> g_quit

loop until g_quit:
    poll({port.fd, tty_fd}, 500 ms)
      port readable -> read()  -> nmea_feed()      /* updates gps_t */
      port EOF/EIO  -> port_close(); connected = 0 /* retry every 500 ms */
      tty readable  -> keys -> view_t / g_quit
      timeout       -> fall through (clock + age keep ticking)
    if !hold or dirty:
        canvas_clear
        view_bars_draw | view_sky_draw
        fb_present

cleanup: fb_release, input_restore
```

Keys: `1` bars, `2` sky, Tab toggle, `n`/`p` select, `s` sort, `g` grid, `h` hold,
`r` reopen port, `q` quit.

Cleanup must be crash-safe: a SIGSTOPped `fbterm` with a stale frame looks like a
dead device (§13.2 of `DESIGN.md`).

---

## 6. Build order, each phase verifiable on its own

**No `libncurses-dev` needed for phases 1–4** — the framebuffer front-end is pure
libc. That is why this ordering starts with pixels, not parsing: it unblocks
coding immediately.

| Phase | Code | How it is verified |
|---|---|---|
| 1 | `font`, `canvas`, `fbdev`, `--selftest` | Render a fixed scene, `--dump` it, convert on the Mac, diff against the mockup PNG. Proves the pixel path with no GPS involved. |
| 2 | `nmea`, `serial`, `--replay`, `--print` | `mknmea.sh` emits a known stream; `--print` dumps parsed state as text. Check DOPs, lat/lon conversion, `used` flags, GSV reassembly, CRC counter, the overlong-line discard. |
| 3 | `view_bars`, `view_sky` | Replay + `--dump` + `fbshow --verify` on the panel. Compare against `fb-bars.png` / `fb-sky.png`. |
| 4 | `main` loop, keys, reopen | Live receiver once it stays attached; unplug it mid-run to exercise the reopen path. |
| 5 | optional ncurses TUI | Needs `sudo apt-get install libncurses-dev`. Separate view files; the pixel canvas cannot be shared with a character grid. |

CLI across phases:

```
gps-monitor [-d DEV] [-b BAUD] [--replay F] [--log F] [--dump F]
            [--print] [--selftest] [--ascii] [--fb PATH]
```

```
Makefile:  cc -O2 -Wall -Wextra -std=c11 -o gps-monitor src/*.c
```

Built natively on the Beepy (gcc 10.2.1), installed to `/usr/local/bin`.

Estimate ≈ 1000 lines: font 330 (mostly table), canvas 180, fbdev 120, nmea 220,
serial 80, views 260, main 140.


---

## 7. Implementation notes (post-build)

Built with `cc -O2 -Wall -Wextra -std=c11 -lm`, zero warnings, on the device's
own gcc 10.2.1.

### Verified

| Check | Result |
|---|---|
| `make check` -- both pages render to 384000-byte frames | pass |
| Live panel frame vs `--dump` reference, static rows 0-199 | **0 of 320000 bytes differ** |
| Whole frame | 180 bytes differ -- the ticking AGE counter, as expected |
| Live NMEA parse, 8 s window | 51 sentences, **0 CRC errors**, UTC + date correct |
| Unparsed sentence types counted, not mis-parsed | 22 counted as unknown |
| Console restored after every exit | `fbterm` back to `S+`, never left `T` |

The byte-exact match between what the renderer produces and what is sitting in
`/dev/fb1` is the real end-to-end proof: it covers the 1bpp canvas, the XRGB
expansion, and the full-frame write in one comparison.

### Bugs found by testing, and fixed

1. **`--demo` was not isolated from the receiver.** The port-reopen path in the
   main loop ran regardless of demo mode, so a demo run opened `/dev/ttyACM0` on
   its first iteration and live NMEA replaced the demo satellites. This first
   looked like a phantom second process writing to the panel.
2. **`--print` / `--dump` read a serial port to EOF, which never comes.** On a
   live device they captured only whatever happened to be buffered. Now they
   collect for a bounded window (`--seconds`, default 3); a replay file still
   drains to EOF.
3. **Unknown elevation/azimuth was plotted as 0/0**, placing a satellite being
   acquired on the horizon due north, on top of the `N` label -- a plausible
   looking lie. Such satellites are now omitted from the plot and reported as
   `n NO POS` in the stats column.
4. **Bar labels touched.** 14 satellites gave a 26 px slot for a 24 px label.
   The bars page now caps itself at what can be labelled with a readable gap
   (12 satellites at 30 px) and reports the remainder as `+n` in the footer.
5. **stdin was polled when not a terminal**, spinning the loop on a
   non-interactive run. Now only polled when stdin is a tty.

### On-device keys: fixed with evdev

`fbterm` is SIGSTOPped while the program owns the panel, and `fbterm` is what
feeds keystrokes to the pty -- so stdin is dead on the device's own console. The
program therefore reads `/dev/input/event*` directly, which bypasses `fbterm`
entirely. `beepy-kbd` is `event0`; the `beepy` user is in group `input`, so no
privileges are needed.

Devices are filtered to real keyboards (must report `KEY_Q` and `KEY_TAB`, which
excludes the `vc4-hdmi` IR receiver on `event1`), and grabbed with `EVIOCGRAB` so
the keys we consume do not also queue on the console and spill into the shell on
exit. The kernel releases a grab when the fd closes, including on a crash.
`--no-evdev` and `--no-grab` disable either behaviour.

**Letter aliases matter here:** the Beepy's digit row needs the Alt/symbol
modifier, which makes `1`/`2` awkward, so `B` = bars, `K` = sky, `V` = toggle
were added alongside `1`/`2`/Tab.

Verified: `/proc/PID/fd` shows `event0` open, `fuser` shows the grab held during
the run and released after exit.

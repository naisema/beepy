# gps-monitor — design

Vertical-bargraph GNSS signal monitor for the Beepy. ncurses, C, single file.

Status: **design for review. No code written, nothing installed on the device.**

---

## 1. Target environment (measured, not assumed)

| Property | Value | How it was established |
|---|---|---|
| Receiver | u-blox 7 GPS/GNSS, USB `1546:01a7` | `dmesg`, `lsusb` |
| Driver | `cdc_acm`, built into the kernel | `dmesg`: `cdc_acm 1-1:1.0: ttyACM0` |
| Device node | `/dev/ttyACM0` | same |
| OS | Raspberry Pi OS (bullseye, armhf) | `gcc (Raspbian 10.2.1-6+rpi1)` |
| Kernel | `6.1.21-v7+` | `uname -r` |
| Compiler | native gcc 10.2.1, make, ld | test compile succeeded |
| Console | `TERM=linux`, **50 cols × 15 rows** | `stty size < /dev/tty1` → `15 50` |
| Panel | `/dev/fb1`, 400×240, 1 bit | `/sys/class/graphics/fb1/virtual_size` |
| Font | `VGA8x16` | `fbcon=font:VGA8x16` in `/boot/cmdline.txt` |
| ncurses runtime | `libncurses.so.6`, `libncursesw.so.6` | present |
| ncurses headers | **absent** | `ncurses.h: No such file or directory` |

### Two open facts that affect delivery

1. **`libncurses-dev` must be installed** (`6.2+20201114-2+deb11u2`, in the Raspbian
   bullseye repo, no extra deps, 53 GB free). Without headers nothing can compile
   against ncurses. Reversible via `apt remove`.
2. **The receiver is currently not attached.** `dmesg` shows six
   enumerate→disconnect cycles about one second apart, then nothing; `lsusb` shows
   only the root hub. The driver binds correctly every time, so this reads as USB
   power or a marginal OTG adapter, not software. Section 9 explains how the design
   is testable anyway.

---

## 2. Design constraints

- **15 rows is the hard budget.** Every row is allocated in §3; there is no slack.
- **Monochrome.** No color. The only attribute used is `A_REVERSE`; `A_BOLD` and
  `A_DIM` are unreliable on a 1-bit panel.
- **No glyph assumptions.** Bars are `A_REVERSE` spaces, which render as solid
  black rectangles regardless of console font. No CP437 or Unicode block
  characters, so nothing depends on console UTF-8 mode. `--ascii` switches to `#`
  if reverse video looks wrong on the panel.
- **Plain `ncurses`, not `ncursesw`.** Nothing here needs wide characters.
- **libc + libncurses only.** No gpsd, no external NMEA library.

---

## 3. Screen layout

50 × 15, every row spoken for:

```
3D FIX 9/14sat HDOP0.9 12:34:56Z ttyACM0 age0s   <- 1  status
13.75632N 100.50184E  alt 18m  0.3km/h crs142    <- 2  position
50|                                              <- 3  ┐
  |     ██                                       <- 4  │
40|  ██ ██                                       <- 5  │
  |  ██ ██ ██                                    <- 6  │ bar area
30|  ██ ██ ██ ██                                 <- 7  │ H = 8 rows
  |  ██ ██ ██ ██ ██                              <- 8  │
20|  ██ ██ ██ ██ ██ ██ ::  ::                    <- 9  │
  |  ██ ██ ██ ██ ██ ██ :: :: :: ::               <- 10 ┘
10+---------------------------------------------  <- 11 axis (floor = 10 dB)
    G  G  G  G  R  R  G  R  G  G                 <- 12 constellation
    0  0  0  1  6  7  2  8  0  1                 <- 13 PRN tens
    2  5  7  3  6  5  4  1  9  1                 <- 14 PRN units
q quit h hold s sort w width  crc 0              <- 15 help + health
```

Row budget: `2 info + H bars + 1 axis + 3 labels + 1 help = 15`, so `H = rows - 7`.

### Column allocation

- Gutter: 3 columns. Bar rows show a right-aligned 2-digit dB label plus `|`;
  unlabelled rows show `  |`. The axis row's gutter is `10+`, which is how the
  floor of the scale is communicated without spending a row on it.
- Plot area: columns 3..49, i.e. `PLOTW = cols - 3` = 47.
- Slot per satellite: `SLOT = BARW + 1` (one column of gap).
  `BARW = 2` → SLOT 3 → `floor(47/3)` = **15 satellites**.
  `BARW = 1` → SLOT 2 → `floor(47/2)` = **23 satellites**.

### Adaptive sizing

Chosen automatically, overridable with `w`:

1. `BARW = 2` if all visible satellites fit.
2. Otherwise `BARW = 1`.
3. If they still don't fit, keep the strongest `N` and render `+n` in the help row.
   Truncation is always shown — never silent.

`KEY_RESIZE` recomputes everything. Degradation ladder for short terminals (a tmux
pane is smaller than raw tty1):

| Rows available | What is dropped |
|---|---|
| ≥ 15 | nothing (H = rows − 7) |
| 12–14 | position row (H = rows − 6) |
| 10–11 | position row + constellation label row (H = rows − 5) |
| < 10 | bars + help only, H ≥ 3 |
| < 6 or < 20 cols | single message: `terminal too small` |

---

## 4. Bar scaling

Signal-to-noise from GSV is in dBHz, useful range roughly 10–50.

```
SNR_MIN 10    SNR_MAX 50    step = (SNR_MAX - SNR_MIN) / H     /* 5 dB at H=8 */

filled(snr) = 0                                    if snr <= SNR_MIN or snr < 0
              H                                    if snr >= SNR_MAX
              max(1, lround((snr-SNR_MIN)/step))   otherwise
```

The `max(1, …)` matters: a satellite at 12 dBHz would otherwise round to zero rows
and look identical to one with no signal at all. Anything above the floor gets at
least one cell.

Y-axis labels are printed on rows whose top-of-row level is a multiple of 10 —
which is exactly why the scale reads 50/40/30/20 and the axis gutter reads 10.

### Fill style carries the fix status

- **Solid** (`A_REVERSE` space) — satellite is used in the position fix, per GSA.
- **Dotted** (`:`) — tracked with signal, but *not* used in the fix.

This is the most informative thing on the screen: it distinguishes "good signal"
from "actually contributing to your position", which is what you want when a fix
is poor despite strong bars.

A satellite in view with no signal still gets its labels printed with an empty
column, so you can see it is being tracked.

---

## 5. Data model

```c
#define MAX_SATS 64
#define NSYS      5          /* G R E B ? */

typedef struct {
    char sys;                /* 'G' GPS, 'R' GLONASS, 'E' Galileo, 'B' BeiDou */
    int  prn;
    int  elev, azim;         /* degrees, -1 unknown */
    int  snr;                /* dBHz, -1 = no signal reported */
    int  used;               /* listed in a GSA PRN set */
} sat_t;

typedef struct { sat_t s[MAX_SATS]; int n; } satset_t;

typedef struct {
    int    quality;          /* GGA fld 6: 0 none, 1 GPS, 2 DGPS, ...        */
    int    mode;             /* GSA fld 2: 1 none, 2 = 2D, 3 = 3D            */
    int    sats_used;
    double hdop, pdop, vdop;
    double lat, lon;         /* signed degrees, NAN when unknown             */
    double alt_m, speed_kmh, course_deg;
    char   utc[9];           /* "HH:MM:SS"                                   */
    char   date[11];         /* "YYYY-MM-DD"                                 */

    satset_t live;           /* what is drawn                                */
    satset_t stage[NSYS];    /* per-constellation GSV accumulation           */

    unsigned long lines, bad_crc;
    time_t last_data;        /* drives the age field                         */
    int    connected;
} gps_t;
```

Two satellite sets, not one. GSV arrives as *N* sentences per cycle; drawing
directly into `live` would show half-updated sets. Accumulation happens in
`stage[sys]` and is published to `live` only when a cycle completes (§6).

---

## 6. NMEA parsing

### Framing and validation

- Read into a buffer, split on `\r\n` (tolerating bare `\n`).
- Lines are capped at 128 bytes (NMEA's limit is 82; the slack absorbs
  non-conforming vendor sentences). An overlong line is discarded up to the next
  terminator rather than truncated into a bogus sentence.
- Checksum: XOR of every byte between `$` and `*`, compared against the two hex
  digits. Failures increment `bad_crc`, which is displayed. On a device with a
  marginal USB connection a rising CRC count is a genuinely useful diagnostic, so
  it earns its place on screen.
- Talker ID is characters 1–2 (`GP`, `GL`, `GA`, `GB`, `GN`), sentence type 3–5.
  `GN` means combined.
- Empty fields are normal in NMEA and must not be parsed as zero — every field
  accessor distinguishes "absent" from "0".

### Sentences consumed

| Sentence | Fields used | Feeds |
|---|---|---|
| `GGA` | UTC, lat/NS, lon/EW, quality, sats used, HDOP, altitude | header, position |
| `RMC` | status, lat, lon, speed (knots), course, date | position, speed, date |
| `GSA` | mode (2D/3D), PRN list ×12, PDOP/HDOP/VDOP | `used` flags, DOPs |
| `GSV` | total msgs, msg no., sats in view, then ≤4×(PRN, elev, azim, SNR) | bars |

Unrecognised sentences are counted and ignored.

Conversions: NMEA latitude is `ddmm.mmmm` and longitude `dddmm.mmmm` — degrees are
*not* the leading two/three digits in a fixed position for both, so the split is
computed per field, then `deg + min/60`, negated for S/W. Speed comes in knots from
RMC and is multiplied by 1.852 for km/h.

### GSV reassembly

```
on GSV(sys, msg_no, total):
    if msg_no == 1:  stage[sys].n = 0        /* start a fresh cycle */
    append the (≤4) satellites in this sentence to stage[sys]
    if msg_no == total:
        replace all sys satellites in live with stage[sys]   /* atomic publish */
```

Per-constellation staging is required because a u-blox 7 interleaves `GPGSV` and
`GLGSV`; a single shared buffer would let one constellation truncate the other.

### GSA "used" attribution

A `GSA` sentence lists up to 12 PRNs but, with a `GP`/`GL` talker, no explicit
constellation. Attribution rules:

- Talker `GP`/`GL`/`GA`/`GB` → match PRNs within that constellation.
- Talker `GN` → match by the standard PRN ranges: 1–32 GPS, 33–64 SBAS,
  65–96 GLONASS.

This is a documented heuristic, not a guarantee; it is the same approach gpsd
takes for NMEA 4.0 receivers. `used` flags are cleared for a constellation when its
GSA arrives, so stale flags cannot persist.

---

## 7. Program structure

Single file, `gps-monitor.c`, ~550 lines, in labelled sections:

```
term    tui_init, tui_fini, layout_compute
serial  port_open, port_close, port_read
nmea    csum_ok, field, feed_line, parse_{gga,rmc,gsa,gsv}
render  draw, draw_status, draw_position, draw_bars, draw_labels,
        draw_help, draw_waiting
main    args, poll loop, keys
```

### Main loop

```
open port (or replay file)
loop:
    poll(fd, 500 ms)
      readable -> read, feed assembler, feed_line per sentence
      EOF/EIO  -> close; connected = 0; retry open every 500 ms
      timeout  -> fall through (keeps clock and age ticking)
    drain keys
    redraw at 2 Hz unless held
```

`poll` with a timeout rather than blocking reads is what lets the age counter and
clock advance while no data arrives — important when the receiver keeps dropping.

### Serial setup

`open(O_RDWR|O_NOCTTY|O_NONBLOCK)`, then raw termios: clear `ICANON ECHO ISIG
IEXTEN`, clear `IXON ICRNL INLCR IGNCR`, clear `OPOST`, `CS8`, `VMIN=0`, `VTIME=0`.
Baud is set from `-b` (default 9600) and is a no-op for CDC-ACM, but keeps the
program usable with a UART-attached receiver.

### Terminal restore

`endwin()` is registered with `atexit`, and `SIGINT`/`SIGTERM`/`SIGHUP` set a quit
flag handled in the loop. A yanked cable or a `kill` must not leave the console in
raw mode — recovering that on a 50×15 panel with no scrollback is unpleasant.

### Disconnected state

Bars freeze, last known values stay visible, `age` climbs, and the status row
switches to `WAITING /dev/ttyACM0 retry 3` so the flapping is legible rather than
looking like a hung program.

---

## 8. Interface

```
gps-monitor [-d DEV] [-b BAUD] [--replay FILE] [--log FILE] [--ascii] [-h]

  -d DEV       serial device            (default /dev/ttyACM0)
  -b BAUD      line speed               (default 9600)
  --replay F   read NMEA from a file instead of a device, paced in real time
  --log F      append every valid sentence received to F
  --ascii      '#' instead of reverse video, for panel comparison
```

| Key | Action |
|---|---|
| `q` | quit |
| `h` | hold / unhold the display |
| `s` | sort: SNR descending (default) ↔ PRN order |
| `w` | cycle bar width 2 → 1 → auto |
| `r` | force close and reopen the port |

Default sort is SNR descending so the graph reads like a spectrum analyser. PRN
order is there because a satellite that holds a fixed column is easier to watch
over time.

---

## 9. How this gets verified

Because the receiver is currently unplugged, testing does not depend on it:

1. **Synthetic stream.** A small generator writes an NMEA file with a known
   satellite set — varying SNR, some sats in the GSA list and some not, an
   interleaved GPGSV/GLGSV cycle, plus deliberately corrupt sentences to exercise
   the CRC counter and one overlong line to exercise the discard path.
2. **`--replay`** feeds that file through the identical parse and render path used
   for live data.
3. **Real 50×15 verification.** Run it inside tmux on the device at exactly 50×15,
   then `tmux capture-pane -p` to pull back a text snapshot of the rendered screen.
   That confirms the true geometry rather than my arithmetic.
4. **Live** once the receiver stays attached: `--log` captures a session, which
   then becomes a regression fixture for `--replay`.

Correctness checks worth stating: bar heights against hand-computed `filled()`
values, `used` flags against the GSA PRN list, and the full 50-column line widths
against the layout table in §3.

---

## 10. Deliverables and scope

Files, all under `gps-monitor/`:

- `gps-monitor.c` — the program
- `Makefile` — `cc -O2 -Wall -Wextra -o gps-monitor gps-monitor.c -lncurses`
- `mknmea.sh` — synthetic stream generator for `--replay`
- `README.md` — build, install, keys

Built natively on the Beepy, installed to `/usr/local/bin/gps-monitor`.

**Explicitly out of scope:** no Buildroot package, no `beepy_drivers/` package, no
changes to `br_defconfig` or `zero2w_defconfig`. This is a standalone program for
the Raspberry Pi OS install that is actually running on the device.

**Requires one action on your side:** approval to `sudo apt-get install
libncurses-dev` on the Beepy, or the program cannot be compiled at all.

---

## 11. Layout variants

All variants share §5–§7 unchanged — the same parser, the same `gps_t`, the same
poll loop. Only the render layer differs, so each is a self-contained
`draw_layout_x()` of roughly 40–60 lines. Type A is the baseline from §3.

Row budgets are stated as formulas because `H` must follow terminal height.

### Type A — Spectrum (baseline, §3)

`H = rows - 7` (=8) · 15 sats · 5 dB/row

Sorted by SNR, three label rows, two info rows. The generalist: satellites and
fix data visible together, sorted so the shape of the graph is meaningful.

### Type B — Constellation-grouped

```
3D 9/14sat HDOP0.9 12:34:56Z 13.756N 100.502E
50|                    |
  |    ██              |
40| ██ ██              |
  | ██ ██ ██           | ██
30| ██ ██ ██ ██        | ██ ██
  | ██ ██ ██ ██ ██     | ██ ██
20| ██ ██ ██ ██ ██ ::  | ██ ██ ::
  | ██ ██ ██ ██ ██ ::  | ██ :: ::
10+--------------------+---------------------
   0  0  0  1  2  3      6  7  8
   2  5  7  3  4  0      6  5  1
  GPS 6/9 used 5         GLO 3/5 used 3
q quit h hold w width l layout crc 0
```

`H = rows - 6` (=9) · 15 sats · 4.4 dB/row

- One status row: fix, sats, HDOP, time and position compressed onto a single line.
- A vertical divider splits the plot into per-constellation groups, so the
  constellation label row from Type A is no longer needed — grouping carries that
  information. That buys one extra bar row over Type A.
- Per-group summary row: in view / used, per constellation.
- Plot width is `47 - 1` divider `= 46`, allocated proportionally to satellites per
  constellation with a minimum of 3 slots per group; a group that overflows
  truncates independently and shows its own `+n`.

**Optimises for:** comparing GPS against GLONASS performance — whether one
constellation is being tracked but excluded from the fix.
**Costs:** position data is abbreviated to fit one line; group widths shift as
satellite counts change, so bars move around more than in Type A.

### Type C — Bars plus side panel

```
50|                        FIX  3D
  |  ██                    SAT  9/14
40|  ██ ██                 HDOP 0.9
  |  ██ ██ ██              PDOP 1.4
30|  ██ ██ ██ ██           VDOP 1.1
  |  ██ ██ ██ ██ ██        ----------
20|  ██ ██ ██ ██ ██        13.75632N
  |  ██ ██ ██ ██ ██ ::     100.50184E
  |  ██ ██ ██ ██ ██ ::     alt   18m
10+----------------------  spd 0.3kmh
    G  G  G  G  R  R       crs 142
    0  0  0  1  6  7       12:34:56Z
    2  5  7  3  6  5       age 0s
q quit h hold s sort w width  crc 0
```

`H = rows - 5` (=10) · 7 sats at width 2, 11 at width 1 · 4 dB/row

- No header rows at all: every scalar lives in the right-hand panel, which is
  22 columns wide with a 1-column gap.
- Plot area shrinks to `50 - 3 - 23 = 23` columns, which is the real cost.
- Taller bars *and* more numbers than any other variant, simultaneously.

**Optimises for:** a stationary "is my fix good" panel — full DOP set, full
position precision, and signal strength in one view with nothing hidden.
**Costs:** roughly half the satellites of Type A. Poor for a receiver tracking 20+.

### Type D — Paged, maximum resolution

Page 1 — bars only, grouped, no scalars:

```
50|                     |
  |                     |
45|     ██              |
  |  ██ ██              |
40|  ██ ██              | ██
  |  ██ ██ ██           | ██
35|  ██ ██ ██           | ██ ██
  |  ██ ██ ██ ██        | ██ ██
30|  ██ ██ ██ ██        | ██ ██
  |  ██ ██ ██ ██ ██     | ██ :: ::
  |  ██ ██ ██ ██ ██ ::  | ██ :: ::
10+---------------------+--------------------
   0  0  0  1  2  3       6  7  8
   2  5  7  3  4  0       6  5  1
1 bars 2 fix 3 sky  q h w   crc 0
```

`H = rows - 4` (=11) · 15 sats · **3.6 dB/row** — the finest of any variant

Page 2 — fix detail, full width:

```
FIX     3D  (quality 1, GPS)
SATS    9 used / 14 in view
DOP     P 1.4   H 0.9   V 1.1
LAT     13.756321 N
LON     100.501847 E
ALT     18.4 m MSL
SPEED   0.31 km/h    COURSE 142.3
UTC     12:34:56   2026-07-29
GPS     6 view / 5 used
GLO     5 view / 3 used
PORT    /dev/ttyACM0 @ 9600  connected
DATA    age 0s  lines 8412  crc err 0
1 bars 2 fix 3 sky  q quit
```

Page 3 — sky view:

```
                  N
         .    G05    G02    .
    G13       +---+---      R66
              |   |
  W    G21    + 90 +    G07     E
              |   |
    G24       +---+---      R75
         .    G30    R81    .
                  S
 solid = used in fix   dotted = tracked
 outer ring 0 deg  inner 30/60  + zenith
1 bars 2 fix 3 sky  q quit
```

Pages selected with `1`/`2`/`3`, or cycled with Tab.

Cell aspect ratio is 8×16 px, so the sky plot uses a 2:1 column-to-row scale to
draw a visually round hemisphere: radius 6 rows / 12 columns, centred, with
azimuth 0° at top and elevation as radial distance inward.

**Optimises for:** the best possible bar resolution, plus a genuine az/el view for
diagnosing obstructions — the thing none of the single-page layouts can offer.
**Costs:** three renderers instead of one (roughly double the render code of any
other variant), and no single screen shows both signal and position.

### Type E — Bars with elevation marker

```
3D FIX 9/14sat HDOP0.9 12:34:56Z ttyACM0 age0s
13.75632N 100.50184E  alt 18m  0.3km/h crs142
50|                                            
  |     ══                                     
40|  ██ ██                                     
  |  ██ ██ ══   --                             
30|  ══ ██ ██ ██                               
  |  ██ ██ ██ ══ ██                            
20|  ██ ██ ██ ██ ██ ══ ::                      
  |  ██ ██ ██ ██ ██ :: ══ ::                   
10+---------------------------------------------
    G  G  G  G  R  R  G  R                     
    0  0  0  1  6  7  2  8                     
    2  5  7  3  6  5  4  1                     
q quit h hold s sort e elev  crc 0             
```

`H = rows - 7` (=8) · 15 sats · 5 dB/row · same geometry as Type A

Dual encoding on one bar: **height is SNR, and the `══` band marks elevation**
(0–90° mapped onto the same 8 rows). Where the marker sits above the top of the
bar it is drawn as a plain `--` floating in empty space.

That floating case is the whole point: a satellite high in the sky with a weak
signal is the signature of an obstruction, reflection or antenna problem, and it is
immediately visible as a detached marker. A high bar with a low marker is a
satellite near the horizon coming in unexpectedly strong — usually multipath.

**Optimises for:** diagnosing *why* signals are poor, not just that they are.
**Costs:** the densest display of the five; two meanings per column takes a moment
to learn, and on a 1-bit panel `══` versus `██` is a subtler distinction than
solid versus dotted.

### Comparison

| | bar rows | dB/row | sats | scalars on screen | extra code |
|---|---|---|---|---|---|
| A spectrum | 8 | 5.0 | 15 | 10 fields | baseline |
| B grouped | 9 | 4.4 | 15 | 6 fields + per-group | ~40 lines |
| C side panel | 10 | 4.0 | 7–11 | 14 fields | ~50 lines |
| D paged | 11 | 3.6 | 15 | all, on page 2 | ~140 lines |
| E elev marker | 8 | 5.0 | 15 | 10 fields + elevation | ~35 lines |

### Recommendation

Render layouts as a dispatch table rather than picking one:

```c
static const struct layout {
    const char *name;
    void (*draw)(const gps_t *, const geom_t *);
} layouts[] = {
    { "spectrum", draw_a }, { "grouped", draw_b },
    { "panel",    draw_c }, { "elev",    draw_e },
};
```

Selected with `-L spectrum|grouped|panel|elev` and cycled at runtime with `l`.
A, B, C and E together cost about the same as D alone, and you can decide which
one you actually like while looking at the real panel instead of these mockups.
D's paged model is the one genuine fork — its pages 2 and 3 can be added later
without disturbing the others, since they read the same `gps_t`.

---

## 12. CHOSEN DESIGN — Type A bars + sky view

Decided: two pages, nothing else. Types B, C and E are not built. Both pages read
the same `gps_t`; §5–§7 are unchanged.

### 12.1 Page model

| Key | Page |
|---|---|
| `1` | bars (Type A, §3) |
| `2` | sky |
| Tab | toggle between the two |

The current page is a single enum. Each page owns its own status row and its own
help row, so no layout state leaks between them. Keys common to both (`q`, `h`,
`r`) are handled before page dispatch; page-specific keys (`s`, `w` on bars;
`n`, `p`, `g` on sky) after.

### 12.2 Page 1 — bars

Exactly §3, unchanged: `H = rows - 7` (=8 at 15 rows), 15 satellites, 5 dB/row,
solid = used in fix, dotted = tracked but unused. Help row gains the page hint:

```
1 bars 2 sky  q h s w   crc 0
```

### 12.3 Page 2 — sky view

```
SEL G02 el67 az210 44dB used   3D 9/14sat
                        N
                 .             .
               .            o R75.
              .                   .
             .    █ G07█ R66 R81 o .
            .                 G13   .
            W o G30     +       █   E
            .         █ G02 █ G05   .
             .                     .
              .                   .
               .     █ G21  o G24.
                 .    o G11    .
                        S
1 bars 2 sky  n/p sel  g grid  q quit
```

Row budget: `1 status + 13 plot + 1 help = 15`, so plot height `PH = rows - 2`,
forced odd so an exact centre row exists.

#### Projection

Zenith at centre, horizon at the rim, azimuth 0° at top increasing clockwise:

```
r      = 1 - elev/90                    /* 0 at zenith, 1 at horizon */
col    = cx + lround(RC * r * sin(az))
row    = cy - lround(RR * r * cos(az))
```

`RR = (PH-1)/2` = 6 rows. **`RC = 2 * RR` = 12 columns**, not the full available
width. The console cell is 8×16 px, so a 2:1 column-to-row ratio is what makes the
hemisphere render round. Stretching it to fill all 50 columns would distort
perceived bearing, and judging bearing is the entire point of a sky plot — so the
spare side columns are deliberately left for labels, which are the scarce resource
here.

#### Grid

Dotted `.` outlines, `+` at zenith, and `N`/`S`/`E`/`W` written into the rim cells
at the four cardinal points. Three densities, cycled with `g`:

1. rim only (default)
2. rim + 30° + 60° elevation rings
3. no grid

The default is rim-only because on a 1-bit panel the denser grids compete with the
satellite markers. Which density actually reads best is a genuine unknown that only
the real display can answer — hence the toggle rather than a guess.

#### Markers

| Glyph | Meaning |
|---|---|
| solid block (`A_REVERSE` space) | used in the fix |
| `o` | tracked, has SNR, not used in the fix |
| `-` | in view, no SNR reported |

`.` therefore only ever means grid — no glyph is overloaded. `--ascii` swaps the
solid block for `#`.

#### Label placement

Labels are 3 characters (`G02`). With 14 satellites in a 25×13 ellipse, collisions
are the norm, so placement is explicit rather than best-effort-and-hope:

```
occupancy = bitmap of the plot area, pre-seeded with grid and marker cells
order     = selected satellite first, then by SNR descending
for each satellite:
    for each candidate in [right(col+2..col+4), left(col-4..col-2),
                           above(row-1), below(row+1)]:
        if candidate is inside the plot and all 3 cells are free:
            place label, mark cells occupied, break
    else: no label this frame  /* marker still drawn */
```

Labels overwrite grid dots — identity beats decoration. Sorting by SNR means that
when space runs out it is the weakest satellites that lose their labels, and the
selected satellite is placed first so it is *always* labelled.

Two satellites landing on the same cell draw the stronger one; the count of hidden
markers appears in the help row as `+n hidden`. Never silent.

#### Selection

`n` / `p` step through satellites in SNR order. The selected satellite is always
labelled, and the status row shows its full detail — the one place elevation,
azimuth and SNR appear together as numbers:

```
SEL G02 el67 az210 44dB used   3D 9/14sat
```

This is how the sky page compensates for having no room for per-satellite numerics.

#### Degradation

| Terminal | Behaviour |
|---|---|
| ≥ 15 rows, ≥ 30 cols | `PH = rows-2` (odd), `RR = (PH-1)/2`, `RC = 2*RR` |
| fewer rows | `RR` shrinks with the terminal; grid rings drop to rim-only |
| < 30 cols | `RC = (cols-2)/2`, accepting an oval rather than clipping |
| < 9 rows or < 20 cols | `sky needs 9x20` message |

### 12.4 Deliverables

```
gps-monitor/
  gps-monitor.c    single file: parser + both pages
  Makefile         cc -O2 -Wall -Wextra -o gps-monitor gps-monitor.c -lncurses
  mknmea.sh        synthetic NMEA generator, with elevation/azimuth spread
  README.md        build, install, keys
```

Installed to `/usr/local/bin/gps-monitor`. Size estimate: ~700 lines, of which the
sky page is ~110 (projection, grid, label placement, selection).

`mknmea.sh` must now emit GSV sentences with a realistic elevation and azimuth
spread, not just SNR — the sky page cannot be verified without them. Verification
is otherwise as §9: `--replay` for the full render path, then
`tmux capture-pane -p` at exactly 50×15 to check both pages against the row and
column budgets above.

Still out of scope: no Buildroot package, no `br_defconfig` or `zero2w_defconfig`
changes. Still blocked on `sudo apt-get install libncurses-dev`.

---

## 13. Graphical mode — direct framebuffer

The panel is a real framebuffer, so the 50×15 character grid is not the ceiling.
Mockups: `fb-combined.png`, `fb-sky.png` (and `-3x` versions for viewing),
generated by `mockup.py` at exact native geometry in 1-bit.

### 13.1 Measured framebuffer facts

| Property | Value |
|---|---|
| Device | `/dev/fb1`, `sharp_drmdrmfb` (DRM `card0-SPI-1`) |
| Visible | 400 × 240 |
| Depth | **32 bpp**, `rgba 8/16,8/8,8/0,0/0` → XRGB8888 |
| Stride | 1600 bytes |
| Frame | **384000 bytes** |
| fbcon | `vtcon1`, `bind=1` (bindable) |

384000 bytes is exactly the size of `root_overlay/opt/shutdownimage.fb`, which the
existing `S00shutdownimage` script displays with a plain
`dd of=/dev/fb1 bs=384000 count=1`. So **full-frame `write()` to `/dev/fb1` is
already proven on this hardware** — no mmap, no DRM ioctls, no damage/dirty
handling to get wrong. At 2 Hz that is 750 KB/s, which is nothing.

The panel itself is 1 bit. Rather than trust whatever thresholding or dithering the
driver applies, the renderer emits **only 0x00000000 and 0x00FFFFFF**, with any
halftone produced as an explicit checkerboard. What is designed is then exactly
what appears.

Pixels are square (400×240 buffer on a 400×240 panel), so circles are circles —
the 2:1 aspect correction the text-mode sky view needed (§12.3) is not required
here.

### 13.2 Owning the display

**Correction from measurement:** the console on this panel is **`fbterm`**, a
*userspace* framebuffer terminal (`fbterm -- tmux -u`, pid 475, running as
`beepy`) — not the kernel's `fbcon`. Unbinding `/sys/class/vtconsole/vtcon1`
therefore does nothing useful; `fbterm` keeps drawing, and tmux's status bar
refresh repaints over any frame within seconds.

What works instead is pausing it:

```
start:  kill -STOP $(pgrep -x fbterm)
exit:   kill -CONT $(pgrep -x fbterm)   + ask tmux clients to refresh
```

`fbterm` only repaints when the tty content changes, so `SIGCONT` alone leaves the
frame on screen; the restore path also issues `tmux refresh-client` for every
attached client. Restore is wired to `atexit` plus `SIGINT`/`SIGTERM`/`SIGHUP`,
because a stopped `fbterm` with a stale frame is indistinguishable from a dead
device. Recovery of last resort is `kill -CONT $(pgrep -x fbterm)`.

`/dev/fb1` is writable by group `video`, which `beepy` is in, so no privilege
escalation is needed for the framebuffer itself. Keys are read from `/dev/tty` in
raw mode, which keeps working while `fbterm` is stopped — though note that when
run *on* the device the panel shows the frame, not the shell, so any interaction
is necessarily blind.

This is all implemented and verified by `fbshow` (§13.7).

### 13.3 Rendering pipeline

```
draw into 1bpp backbuffer (400x240 = 12000 bytes)
      |  primitives: hline vline rect fill checker circle text
      v
expand to XRGB frame (384000 bytes)  -- 1 bit -> 0x000000 / 0xFFFFFF
      v
write(fb, frame, 384000) at 2 Hz
```

A 1bpp backbuffer keeps drawing cheap and makes the expand step the only place
pixel format is known. Two embedded bitmap fonts, **5×7** (labels, status) and
**3×5** (axis, PRN digits) — the mockups approximate these with thresholded Menlo,
so real glyph shapes will differ slightly and be cleaner (Menlo's slashed zero and
its `V`/`W` at 9 px are mockup artefacts, not the design).

Circles use the midpoint algorithm; the grid circles are drawn as spaced dots so
they stay visually behind the markers.

### 13.4 Page 1 — bars, full screen — `fb-bars.png`

**Decided: the two views are separate pages, each using the whole 400×240.** The
side-by-side combined layout is kept only as `fb-combined.png` for comparison.

Giving bars the full width buys two things the combined layout could not fit: the
**SNR value printed above every bar**, and **horizontal PRN labels** (`G02` on one
line) instead of stacked digits.

| Region | Pixels |
|---|---|
| Status bar (inverted), page tag `BARS` | y 0–14, full width |
| dB axis labels | x 0–17 |
| Axis / baseline | x 20 vertical, baseline y 197 |
| Bars | x 26 onward, **slot 26 px, bar 20 px**, 14 slots |
| SNR value per bar | centred, 12 px above each bar top |
| PRN label per bar | centred, y 199–208 |
| Footer, 2 lines | y 213–239 |

Bar scale spans 10 → 50 dBHz over 163 px = **4.1 px/dB**, against 5 dB per whole
character row in text mode.

Slot width adapts: `slot = min(30, plotw/n)`, and when `slot < 22` the label falls
back to stacked digits plus a constellation row, exactly as the 50×15 design does.
So a receiver tracking 20+ satellites degrades gracefully rather than clipping.

Encodings, consistent across both pages:

| Sky | Bars | Meaning |
|---|---|---|
| filled disc r4 | solid fill | used in the fix |
| hollow circle r4 | checkerboard fill + outline | tracked, has SNR, not used |
| 3×3 dot | baseline stub only | in view, no SNR |
| halo ring r7 | bracket + SNR printed above | selected satellite |

### 13.5 Page 2 — sky, full screen — `fb-sky.png`

| Region | Pixels |
|---|---|
| Status bar (inverted), page tag `SKY` | y 0–14, full width |
| Sky panel | x 0–280, y 17–239, centre (140,128), **r 97** |
| Divider | x 282 |
| Stats column | x 287–399 (≈13 chars at 9 px) |

Stats column carries what the plot cannot: selected satellite (PRN, SNR, elevation,
azimuth, in-fix), the full DOP set, per-constellation used/in-view, position and
altitude, data age, and the page/key hints.

Label placement uses the §12.3 occupancy algorithm at 4 px cell granularity,
seeded with marker and cardinal-letter cells, ordered selected-first then
strongest-first, trying right → left → above → below. Unplaceable labels are
counted and reported as `+nHID` — never silently dropped.

**Separating the views is what makes labelling work.** At r 81 in the combined
layout, R81 could not be placed and the display honestly reported `+1HID`. At r 97
with the full width available, **all 14 satellites label successfully and nothing
is hidden.** That is the concrete payoff for splitting the pages.

### 13.6 Page switching

`1` = bars, `2` = sky, Tab toggles. The status bar carries a page tag (`BARS` /
`SKY`) so the current view is never ambiguous, and each page's footer names the
key for the other one.

### 13.7 Previewing designs on the real panel — built and working

`fbshow` (installed at `/usr/local/bin/fbshow` on the device) displays a raw
384000-byte frame and restores the console afterwards, implementing the `fbterm`
pause/resume of §13.2. `--verify` reads `/dev/fb1` back and compares it byte for
byte, which is how these mockups were confirmed to actually reach the panel rather
than merely being written without error:

```
fbshow --verify fb-bars.fb 10     # 10 s, then restore
fbshow fb-sky.fb 0                # hold until Enter
```

`png2fb.py` converts a 400×240 1-bit PNG into that frame format, using the two
pixel words taken from the device's own known-good `shutdownimage.fb`.

`mockup.py` runs on the Beepy as well as the Mac (Python 3.9 + PIL 8.1 are
installed there; the font falls back Menlo → DejaVuSansMono → Courier), so the
whole design loop — edit layout constants, render, view on the panel — can happen
on the device with no host involved. Device and Mac renders differ by ~0.2% of
pixels, purely from DejaVu vs Menlo glyph shapes.

### 13.8 Relationship to the ncurses design

The parser and state (§5–§6) are shared; only the renderer differs. One binary,
two front-ends:

```
gps-monitor [--fb | --tui] [-d DEV] [--replay F] [--log F] ...
```

Default: `--fb` when `/dev/fb1` is writable and `$SSH_CONNECTION` is unset,
otherwise `--tui`. That matters in practice — the framebuffer is useless over SSH,
and the ncurses version from §12 remains the right front-end when you are working
from the Mac. Keys are identical in both (`q h g n p` and TAB).

### 13.9 Revised deliverables

```
gps-monitor/
  gps-monitor.c    parser + state + both renderers
  font5x7.h        embedded bitmap fonts (5x7 and 3x5)
  Makefile
  mknmea.sh        synthetic NMEA with elevation/azimuth spread
  mockup.py        this design's PNG mockups
  DESIGN.md
  README.md
```

Estimate ~1100 lines: ~420 shared parser/state, ~250 ncurses renderer, ~300
framebuffer renderer and primitives, ~130 font data.

Verification adds one step to §9: render a frame to a file instead of `/dev/fb1`
(`--dump frame.raw`), convert to PNG on the Mac, and compare against these
mockups. That checks the real renderer's geometry against the design without
needing the panel or the receiver.

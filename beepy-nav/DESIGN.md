# beepy-nav — design

A two-page GPS route navigator for the Beepy, built on the parser and
framebuffer renderer already proven by `gps-monitor`.

Status: **implemented through DESIGN phase 3 and running on the device.**
`mockup.py` remains the pixel reference — `make design-gate` byte-compares the
C renderer against it, and both are frozen into `goldens/nav-*.fb`, which
`make check` verifies on the device.

```
NAV        turn arrow, distance to the junction, and a live map
OVERVIEW   the whole route, fitted, with progress
```

Plus one pre-ride flow — **FIND → CONFIRM** — that produces the route the two
pages then follow: type a destination on the Beepy's own keyboard, see the
routed overview, press ENTER to go.

That is the entire program. It follows a route and shows you where you are on
it. It is not a cycle computer: **no data-field pages, no elevation profile, no
menu page, no ride recording, no satellite page.** An earlier draft had all of
those and they are gone — see §14 for what was removed and what that bought.

The layout of the NAV page follows `Screenshot 2569-07-29 at 16.54.16.png` in
this directory: an inverted panel across the left third holding the turn arrow
over the distance over the unit, with the map filling the rest.

---

## 1. Pages

### 1.1 NAV — `nav-turn.png`, `nav-turn-nomap.png`, `nav-turn-off.png`

| Region | Pixels |
|---|---|
| Turn panel, inverted | x 0–127 (32% of the width) |
| Divider | x 128 |
| Map | x 130–399, full height |

Turn panel, top to bottom:

| Element | Pixels |
|---|---|
| Turn arrow, white | 76×76, centred, y 6 |
| Distance, bold typeface, white | up to 64 px cap height, centred, y 92 |
| Unit (`M` / `KM` / `FT` / `MI`) | 24 px cap height, centred, below the digits |
| Rule | y 189 |
| Time remaining, centred | scale 2, y 192 |
| Whole-route distance remaining, centred | scale 2, y 208 |
| Arrival, centred | scale 2, y 224 |

Nothing on the panel is smaller than scale 2 (16 px) — scale-1 text was
unreadable on the physical panel at handlebar distance. That size is also what
fixes the layout: the budget is **ten characters** at 12 px each against a
128 px panel, and `12.6KM 10:42 ETA` is far past it. So the three values are
stacked rather than paired on one line, each keeping its full precision. The
16 px cells at 192/208/224 fill the space to the bottom edge exactly; glyphs
are 14 px of ink, so the rows stay separated without gaps.

The battery per cent and the clock **held the bottom line and lost it**. While
riding, how much ride is left is the question you actually have, and the big
number above answers only "how far to the next junction". Battery and clock
return only when no route is loaded, when there is nothing to count down.

Formats: minutes below an hour then `1H 23M`, because `97 MIN` is a number you
have to convert; arrival as 12-hour with the value first and the label
trailing (`10:42 ETA`), with no meridiem — on a ride you know whether it is
morning, and nine characters always fit with no degradation rule to reason
about. Both read `--` when there is too little history to average, rather than
a guess.

**The next-cue preview is gone from the panel** — it was a 20 px glyph and a
distance in this space. The cue after the announced one is still marked: the
teardrop pin on the map is exactly that (below). What went is the preview, not
the information.

**GPS state earns panel space only when it is a problem** — `NO FIX` replaces
the bottom row, inverted, when the fix is lost; a healthy receiver is not
information.

Map region:

| Element | Drawn as |
|---|---|
| Route ahead | 6 px black core inside a 10 px white casing |
| Track already ridden | 1 px dashed, 5 on / 5 off |
| Cue after the announced one | teardrop pin with a punched hole |
| Position | white disc, black ring, solid black chevron, at `(map centre, 0.72 × height)` — the chevron is rotated by the **residual course angle** (raw course minus smoothed map rotation), so mid-turn it stays pointed along the road while the course-up rotation catches up |
| North | black disc with a reversed `N`, plus a needle on the rim pointing to world north |
| Current speed | number in a white circle, black ring, top-right of the map — ~24 px digits with `KM/H` beneath; same ring construction as the position marker so the round instruments read as a family |
| Scale | 1-2-5 bar, bottom-left, scale-2 label |
| Basemap (optional, §6) | streets at 1 px, arterials at 2 px |

### 1.1.1 The countdown must change slowly — quantisation and latching

The distance to the cue is the one number a rider looks at while moving, so how
*often* it changes matters as much as what it says. Rounding to 10 m — the
first draft — changes the display every **1.3 s** at 28 km/h. That is motion in
the corner of the eye, on the screen, exactly when attention belongs on the
road.

Displayed distance is therefore quantised, **floored** (never overstating how
far is left, so you prepare early rather than late):

| Distance to cue | Step | Reads | Changes every |
|---|---|---|---|
| ≥ 1000 m | 100 m | `5.8 KM` → `5.7 KM` → `5.6 KM` | 12.8 s @ 28 km/h |
| 200–999 m | 50 m | `900 M` → `850 M` → `800 M` | 6.4 s @ 28 km/h |
| 10–199 m | 10 m | `200 M` → `190 M` → `180 M` | 2.8 s @ 13 km/h |
| < 10 m | — | holds at `10 M` | — |

The step tightens as the junction approaches, and that is the whole idea:
**coarse where there is nothing to do yet, fine where you are about to act.**
Far out, a number that ticks every second is pure distraction — nothing changes
in your riding between 5.8 and 5.7 km. Inside 200 m you are choosing a lane and
braking, the 10 m steps are information, and you are slower anyway, so they
arrive no faster than the 50 m steps did at cruise.

The last few metres **hold at the bottom rung** rather than counting to zero.
An earlier draft printed `NOW` there; a word swapping in for the digits is a
change at the worst possible moment, and by then the arrow is the instruction.
Worked examples: 5834 → `5.8 KM`, 1000 → `1.0 KM`, 999 → `950 M`,
410 → `400 M`, 205 → `200 M`, 194 → `190 M`, 12 → `10 M`, 9 → `10 M`.

**Imperial keeps the ladder's shape, not its numbers** (`U` toggles):
0.1 mi steps beyond a mile, 100 ft from 500 ft out, 50 ft inside that, holding
at 50 ft. Converting the metric rungs would give a 328 ft step, which is not a
figure anybody reads at a glance. A units change mid-ride **resets the latch** —
400 metres is not a value 1312 feet may only decrease from.

**Quantisation alone is not enough — the value must also latch.** GPS jitter of
±4 m sitting on a boundary would flicker `850`/`800` several times a second,
which is worse than the fast-but-smooth countdown it replaces. So the shown
value only ever *decreases* while approaching a given cue:

```
on each fix:
    q = floor_to_step(distance_to_cue)
    if cue_index changed:   shown = q          /* new cue, start over  */
    else if q < shown:      shown = q          /* monotone decrease    */
    /* q > shown is ignored: jitter, not progress */
```

The result is a strictly monotone non-increasing countdown per cue, with no
flicker possible by construction. Moving genuinely away from the route is not
this mechanism's problem — the off-route latch (§7.3) takes the panel over.

Everything inside the program stays in metres; units reach no further than the
ladder and the strings.

**The latch is initialised to a sentinel, not zero.** Cue 0 is a real cue, and
a zeroed "which cue am I on" makes the latch believe it is already counting
down to it from a shown value of 0 — which, being the minimum, it can never
leave. The panel read `0 M` for an entire ride before that was fixed, and the
unit test missed it by initialising the latch by hand instead of through
`nav_init()`.

**The pin does not mark the announced turn.** The announced junction is already
communicated twice — the whole left panel, and the visible bend in the route —
so a pin there would only cover the bend. The teardrop marks the **cue after
it**, which is what supports the panel's `THEN` line and is exactly where the
reference screenshot puts its pin. The compass needle rotates with the map;
on a course-up display a bare `N` badge would be a lie.

The position marker is a **white disc with a black ring and a solid black
chevron** — thresholding the reference confirms that polarity (an earlier draft
of this design had it inverted).

**The white casing around the route is the load-bearing detail.** With a basemap
underneath, a plain black route line disappears the moment it runs along a black
street. The casing clears a gap and the core fills it, which is the standard
cartographic answer and the only one available with two colours. Compare
`nav-turn.png` (basemap) against `nav-turn-nomap.png` (no basemap): identical
code, and the casing costs nothing when there is nothing under it.

The fix sits at 72% of the height so roughly two thirds of the map is the road
ahead. The map is course-up by default, north-up on a keypress.

**Off route** (`nav-turn-off.png`) changes the panel, not just the map: the
junction distance is replaced by `OFF ROUTE / 85 / M AWAY`. A stale `410 M` next to a route you are not on is the worst thing
this panel could say, so it is withheld rather than frozen. On the map the
marker genuinely leaves the line, with a dotted tie-line to the nearest point
on the route.

An earlier draft put `FOLLOW DOTS BACK` where the cue preview used to be. It is
not drawn: at scale 2 — the panel's floor since §5.1 — sixteen characters need
192 px against a 128 px panel, and the instruction is redundant, because the
tie-line already points at the route and the panel already says how far.

### 1.2 OVERVIEW — `nav-overview.png`

The whole route, north-up, fitted to the screen with a 14 px margin.

| Element | Drawn as |
|---|---|
| Route remaining | cased line, 3 px core |
| Route ridden | dashed |
| Start | small ring |
| Finish | chequered flag |
| Every cue | 4 px dot |
| Position | filled disc with a reversed chevron |
| Title band, y 0–23 | route name + total length, scale 2 — the map is fitted below it, never under it |
| Bottom strip, inverted, 42 px | all scale 2: `59% DONE` / `3/11 CUES ETA 10:42`, and `TO GO 12.6 KM` with 32 px digits |

This answers the one question the NAV page cannot answer at all — where does
this go, and how much is left. Marking every cue as a dot makes "how many turns
are coming" readable from the shape.

`Z` / `X` are **not** wired to this page. They zoom the NAV map; here the fit
is the whole point, and a page whose one job is "the whole route at once"
gains nothing from a rider being able to lose part of it. Zooming the fitted
view would need a pan as well — a fitted map that is bigger than its frame has
a centre to choose — and that is two keys this device does not have spare.

### 1.3 Cue glyphs — `nav-arrows.png`

Nine glyphs: straight, slight/normal/sharp × left/right, U-turn, destination.
Filled polygons in an `s × s` box with proportions as fractions of `s`, so one
routine draws the 78 px panel arrow and the 16 px `THEN` arrow.

The sheet renders all nine at 40/24/16 px. The 16 px row is the real test.
Sharp turns **climb and then strike back down and outward at 135°** so the head
ends up below its own corner; an over-the-top curl read as a crown once the
stroke was thick enough to see at 16 px, and bend angle alone is invisible at
that size.

### 1.4 Pre-ride: FIND and CONFIRM — `nav-search.png`, `nav-confirm.png`

The Beepy's one hardware luxury is a full QWERTY keyboard, so destination entry
is **type-to-filter** — no on-screen keyboard, no cursor chasing:

| FIND screen | |
|---|---|
| Title bar, inverted | `FIND` · live hit count |
| Query, 24 px bold + block cursor | what has been typed so far |
| Up to 4 matches, nearest first | name · distance + 8-way bearing (`464M NE`) |
| Selected match | inverted row; `N`/`P` move, ENTER routes |

The search index is the street names already in the offline pack (`name:en`
falling back to `name`), matched token-AND, ranked by distance from the current
fix. POIs and addresses can join the index when packed; **nothing is searchable
that is not in the pack**, and the hit count says so honestly.

**Routing is on-device Dijkstra over the pack's road graph** — ways joined on
exact shared coordinates. Measured on the Asok extract: 2,803 nodes built in
3 ms, shortest path in 0.1 ms, and the 824 m result matched the hand-built
demo route within 3 m. Even at Pi Zero speeds a corridor pack routes
instantly; a whole-city pack (~10⁵–10⁶ nodes) is Dijkstra-in-tens-of-ms, still
fine without any A*/contraction cleverness. Two honest limits: the mockup
router ignores `oneway` tags (production must not), and beyond pack coverage
the fallback is straight-line bearing navigation, labelled as such.

CONFIRM is the OVERVIEW page's cartography fitted to the *proposed* route —
start marker, cue dots, destination flag — with the strip carrying the one
decision: `827M · EST 3 MIN · 2 TURNS` against `ENTER = GO / Q = CANCEL`. ETA
assumes 17 km/h city riding until there is ride history to do better.

A GPX file remains the other entry: FIND builds a route, a GPX *is* one, and
everything downstream is identical.

---

## 2. Keys

During a ride — NAV and OVERVIEW, and every one of them live in a `--replay`
session too, because a replay that cannot be paged or quit from the device is
not a rehearsal of anything:

| Key | Action |
|---|---|
| `Tab` | switch page (there are only two) |
| `Z` / `X` | zoom the NAV map out / in — one rung of §6.1's ladder, and switches it to manual |
| `A` | return the NAV map to auto zoom |
| `O` | course-up ↔ north-up |
| `U` | units: metric ↔ imperial |
| `H` | hold — freeze the display |
| `Q` | quit |

In the startup route chooser, which is a different program state and not a
page:

| Key | Action |
|---|---|
| `N` / `P`, `↓` / `↑` | move the selection |
| `Enter` | load the route and start riding |
| `Q`, `Esc` | quit without loading one |

A key repaints immediately rather than waiting for the next tick of §6.3's
frame clock: at the 1 Hz stopped rate that would be a whole second between the
press and the answer, which is how an instrument comes to feel broken. The
repaint is not a frame of the ride clock — it does not advance the dead
reckoning, and it is not counted — but it does go through the §6.4 skip, so a
key that changes nothing visible still costs no SPI.

`F`, and the `FIND` page of §1.4 that it would open, are **not built**. §1.4 is
a design for a later milestone; nothing in the shipped program reads the key.

Letters, not digits: the Beepy's digit row needs the Alt/symbol modifier, which
`gps-monitor` already found unusable one-handed. Keys come from `/dev/input/event0`
with `EVIOCGRAB`, because `fbterm` is SIGSTOPped while the panel is owned —
with `stdin` in raw mode as a second source, so the whole keymap can be driven
over ssh. That is not a debugging affordance to be removed later: it is the
only way any of this is testable without a physical thumb.

**There is no menu.** The route is a command-line argument:

```
beepy-nav --route ROUTE.gpx [-d DEV] [--north-up] [--imperial]
          [--replay F.nmea] [--config FILE]
```

Run with no `--route`, it lists `$BEEPY_ROUTES` or `~/routes` (`routes_dir` in
the config file) and waits for a selection — a startup chooser, not a page.
Everything else lives in `~/.config/beepy-nav.conf`, which is read once at
startup.

### 2.1 `~/.config/beepy-nav.conf`

Flat `key = value`, one per line; a line whose first non-blank character is `#`
is a comment. Whitespace around both sides is ignored, keys and the enumerated
values are case-insensitive, and a `#` inside a value is a `#` — a path may
legitimately contain one, and five keys is not enough surface to justify a
quoting rule nobody would remember.

| Key | Values | Default | Effect |
|---|---|---|---|
| `units` | `metric` / `imperial` | `metric` | starting units; `U` still toggles |
| `north_up` | `0` / `1` | `0` | start north-up instead of course-up |
| `rate_5hz` | `0` / `1` | `0` | ask the receiver for 5 Hz at startup (§6.3) |
| `routes_dir` | a path | `$BEEPY_ROUTES`, else `~/routes` | where the chooser looks |
| `led_alerts` | `0` / `1` | `1` | flash the keyboard LED at cues (§7.5) |

Flags also accept `yes`/`no`, `true`/`false`, `on`/`off`. **A command-line flag
always wins**, because it is the more specific statement of intent: the file
says what you usually want and the flag says what you want this time.
`--config FILE` reads somewhere else, which is what makes the parser testable.

**The file is never fatal.** A rider whose navigator refuses to start because
of a typo in a preferences file — at the roadside, with the only text editor
being whatever is on the SD card — has been failed by the program, not by the
typo. Every malformed line, unknown key and unreadable value warns to stderr
with its line number and is then ignored, and the setting keeps its default.
An absent file is not even a warning; that is the ordinary case. An absent
*explicitly named* file is one, because being told nothing at all is the worst
possible outcome of a mistyped `--config`.

---

## 3. Target environment (measured 2026-07-29, not assumed)

| Property | Value | How established |
|---|---|---|
| Receiver | u-blox 7, USB `1546:01a7`, `/dev/ttyACM0` | `lsusb`, `dmesg` |
| **Receiver streams** | RMC, VTG, GGA, GSA, GSV at 1 Hz | `dd if=/dev/ttyACM0` |
| Constellations seen | **GPS only** — `$GPGSV` only, 10 sats in view | same capture |
| `VTG` present | course-over-ground true + speed in km/h, as fields | same capture |
| Panel | `/dev/fb1`, 400×240, 32bpp XRGB, frame 384000 B | `gps-monitor` §13.1 |
| Console | `fbterm`, must be SIGSTOPped to own the panel | `gps-monitor` §13.2 |
| Keys | `/dev/input/event0`, `beepy` is in group `input` | `id` |
| Battery | `/sys/firmware/beepy/battery_percent` → `86`, readable | `cat` as `beepy` |
| LED | `/sys/firmware/beepy/led{,_red,_green,_blue}` — **`--w--w---- root root`** | `ls -l` |
| Storage | 53 GB free on `/` | `df -h` |
| RAM | 426 MB total, 319 MB available | `free -m` |
| Toolchain | native gcc 10.2.1 | `gps-monitor` was built with it |

Two consequences:

1. **`VTG` must be parsed.** `gps-monitor` derives speed from RMC knots × 1.852
   and has no course at all. A navigator leans on course constantly — map
   rotation, off-route direction — so this is a required 15 lines.
2. **The LED needs root.** Flashing the keyboard LED at a cue is the only
   non-visual alert this hardware has; there is no buzzer. It needs a udev rule
   granting group `input` write access, or a small setuid helper. Nothing else
   in this design needs privileges.

---

## 4. Constraints

- **Monochrome, 1 bit.** Two pixel words only. Any halftone is an explicit
  checkerboard. This is why route-vs-track and ridden-vs-ahead are encoded as
  *line style*, never as a shade — and why the route needs a casing (§1.1).
- **Read in ~0.4 s, one-handed, while moving.** That brief is what makes the
  distance-to-junction the largest thing on the screen and pushes everything
  else to scale 1.
- **libc only.** No expat, no zlib, despite both being installed with headers.
  Zero dependencies is what would let this be packaged for the Buildroot image
  later — the whole purpose of the repo next door. GPX parsing is a hand-written
  scanner (§7.1).
- **Panel ownership is already solved.** `fbterm` pause/resume, evdev grab and
  crash-safe restore are implemented and verified in `gps-monitor`; this program
  reuses them unchanged.

---

## 5. Typography

### 5.1 Labels: the existing 5×7 font

5×7 glyphs in a 6×8 cell at integer scale, exactly as `gps-monitor` embeds it.
Advance is `6 × scale`: 66 chars per 400 px line at scale 1, **33 at scale 2**,
22 at scale 3. Every string in the mockups respects that budget, and
`mockup.py` **parses the glyph table straight out of `../gps-monitor/gps-monitor.c`**,
so a mockup cannot disagree with the device about glyph shape or string width.

**Four glyphs must be added to `FONT[]` before this ships:** `%` `>` `<` `*`
(indices 5, 30, 28, 10). `mockup.py` carries them in an `EXTRA` table; they need
transcribing into the C source. That is a real prerequisite, not a nicety.

### 5.2 Distances: a real typeface, pre-rendered

The reference sets its distance in a **bold grotesque**, not a segment display,
and matching that is most of what makes the panel look right. An earlier draft
drew 7-segment digits from rectangles; they are legible but unmistakably a
cheaper object.

So: digits come from a real face (Arial Bold / Helvetica, DejaVu Sans Bold as the
fallback), rendered at 4× and resolved with the rest of the frame (§5.3). In C
there is no font engine — `mockup.py` doubles as the generator, emitting the
glyphs as **1-bit bitmap tables**:

| Set | Cap height | Cost |
|---|---|---|
| `0-9 . -` panel distance | 54 px | ~40×54 bits × 12 ≈ 3.3 KB |
| `0-9 . -` off-route + overview | 22 px | ~16×22 bits × 12 ≈ 0.6 KB |
| `M KM FT MI` units | 22 px | ~0.4 KB |

Under 5 KB of table against 319 MB free, and the glyphs on the device are then
bit-identical to the mockup by construction rather than by resemblance.

Sizes are picked to fit: `while width(value, cap) > 112: cap -= 2`. `410` gets
the full 54 px; a four-digit `1250` steps down rather than overflowing. Only the
54 px and 22 px sets are generated, so the step is between two prepared sizes.

**The size floor matters.** Below about 16 px cap height a proportional face
resolves to mush — the fringe becomes as wide as the stem, and `TO GO KM ETA` at
cap 10 came out as grey soup in one render. Everything smaller uses the 5×7
bitmap font of §5.1, drawn on integer pixel boundaries. Crisp beats smooth once a
glyph is only a few pixels tall.

### 5.3 Smoothness — `nav-smooth.png`

This section replaces an earlier claim of mine that anti-aliasing was impossible
here. That was wrong in an important way, and the reference image is what settled
it.

**What the reference actually is.** Measured: 487×295, **239 distinct grey
levels**, two plateaus at 22 (ink) and 224 (paper), and **8.5% of pixels between
40 and 215**. Those mid-tones sit on edges. It is a two-tone design that is
anti-aliased, and nothing else.

**What the panel can take.** `sharp-drm` has a `mono_cutoff` parameter, default
**32**, documented as "consider all pixels with one of R, G, B below this
threshold to be black, otherwise white" — a per-pixel threshold, no dithering
anywhere in the path. So grey cannot be *emitted*: a 50%-grey edge pixel has all
channels at 128 and snaps to white.

But that only rules out sending grey down the wire. Anti-aliasing performed here
and resolved to black and white before the write reaches the panel intact,
because the driver's threshold never sees a grey value. The distinction I missed
first time round is between the tone we compute and the tone we transmit.

**The pipeline.** Everything is drawn into an 8-bit coverage canvas at 4×, box-
downsampled to 400×240, and resolved with a plain **50% threshold**.

**Dithering was tried and rejected, on evidence.** Downsampling the reference to
400×240 and resolving it both ways is decisive: the threshold reproduces it
cleanly, while an 8×8 Bayer matrix speckles the black panel with white dots,
lays a grey pattern over the whole map, and frays the stems of the numerals. The
reason is simple in hindsight — 4× supersampling has *already* put the ink in the
right pixel, so there is no residual tone worth diffusing and the matrix only
adds noise. `page_smooth()` regenerates the comparison; `resolve(dither=True)`
reproduces the rejected version.

**Thin features bypass the coverage canvas entirely.** Anti-aliasing helps large
smooth shapes — the arrow, the numerals, the discs, the 4 px route — and actively
harms 1–2 px lines. A 1 px diagonal has no room: its coverage spreads over two
pixels at roughly half each, and the first render of the basemap came out as
*broken dotted lines*. Streets, the ridden-track dashes and the scale bar are
therefore drawn at 1× into a separate aliased layer, then blown up NEAREST into
the 4× canvas — each 1× pixel becomes a solid 4×4 block that box-downsamples back
to exactly itself. They are flushed **before** the route, so the route's white
casing still cuts through them.

**Geometry smoothing, independent of all of the above:**

- **Corner rounding in world metres.** A GPX is a chain of straight chords with
  hard vertices; drawn raw the route reads as a polygon. Every vertex turning
  more than 12° becomes a quadratic arc of radius
  `min(25 m, half the shorter adjacent segment)` — computed in metres before
  projection, so a junction keeps the same real radius at every zoom, and a dense
  route with points every 10 m is untouched by the clamp.
- **Round joins and caps.** A bare segment list notches the outside of every
  vertex. A disc fills it — but a disc at *every* vertex makes a rounded curve
  lumpy, because arc points are a pixel or two apart and each disc lands on a
  slightly different integer footprint. So: discs at the two caps and at joins
  turning more than 15°. The lumpy version is what the first attempt produced.

**What this costs in C.** No 4× buffer: a full-frame supersample would be
1600×960 and 1.5 M samples per frame. Instead, coverage is computed analytically
into one 400×240 8-bit buffer (96 KB) and thresholded:

| Primitive | Coverage |
|---|---|
| Thick strokes, discs | signed distance: `clamp(halfwidth + 0.5 - dist)`, evaluated only inside the bounding box |
| Arrow polygons | 4×4 sub-samples, and only inside the glyph box |
| Numerals, labels | pre-rendered bitmaps — no runtime coverage at all |
| Streets, dashes, chrome | written straight to the 1-bit output, aliased |

The two heavy cases are eliminated rather than optimised: text is a table lookup
and hairlines skip the pipeline. What remains is per-pixel work over the few
bounding boxes the route and markers occupy.

---

## 6. Map rendering

### 6.1 Projection, rotation, zoom

Local tangent plane, re-referenced every 10 km — under 0.1% error within 50 km,
and one `cos` per reference change instead of a haversine per fix:

```
x = (lon - lon0) · 111320 · cos(lat0)      y = (lat - lat0) · 110540
e' = e·cosθ - n·sinθ      n' = e·sinθ + n·cosθ
sx = cx + e'/mpp          sy = cy - n'/mpp
```

Zoom is a fixed ladder in metres per pixel:

```
1.5  2.5  4  6  10  15  25  40  60  100  150  250
```

Auto-zoom picks the coarsest level that still leaves the next cue within 80% of
the visible road ahead, so the junction is always on screen and the view opens
out on long straights. `Z`/`X` switch to manual; `A` returns to auto. The scale
bar picks whichever of 25/50/100/200/500 m, 1/2/5/10/20 km lands between 50 and
100 px, so it is always a round number.

Heading for course-up comes from VTG, smoothed with a circular EWMA and
**frozen below 3 km/h**. Without the freeze the map spins at every traffic
light, which is the single most common complaint about course-up modes.

**The smoothing constant is a time constant, not a per-frame α.** An earlier
draft of this section said α = 0.25, and left unsaid what it was 0.25 *per* —
which was survivable only while the display redrew once per fix. §6.3 renders
at 8 Hz, so a fixed per-frame α would smooth eight times harder than the same
number did at 1 Hz, and the map would lag a corner by seconds.

The reference is `sim.py`, which is the only version of this smoothing that has
been watched moving: α = 0.3 per frame at 6 fps. Read as a time constant that
is

```
tau = -dt / ln(1 - alpha) = -(1/6) / ln(0.7) = 0.47 s     (exact)
tau =  dt / alpha         =  (1/6) / 0.3     = 0.56 s     (first order)
```

**τ = 0.55 s is adopted**, the first-order reading, rounded. The two differ by
15%, which is a tenth of a second of lag on a rotation the rider is not
measuring, and the gentler value is the safer one on a display whose whole
complaint is jitter. Per frame:

```
alpha = 1 - exp(-dt / 0.55)
```

At 8 Hz that is α ≈ 0.203 per frame and 0.84 per second, against the old
0.25 per second — the corner is taken three times faster, and it was the
sluggishness of 0.25 that made the residual chevron of §1.1 necessary in the
first place. Making α depend on dt is also what keeps course-up looking the
same at 8 Hz, at the 1 Hz stopped rate, and through a dropout.

### 6.2 Clipping and simplification

Cohen–Sutherland per segment. Without it the route paints straight through the
turn panel — the first thing that goes wrong when a map is dropped into a region
that is not the whole screen. It is implemented in `mockup.py` and its output is
visible in every NAV mockup.

Douglas–Peucker at 1 px tolerance per zoom level, cached and recomputed only on
zoom change. Segments are bbox-rejected against the view before clipping.

### 6.3 Smooth motion — dead reckoning at 8 Hz

At 1 Hz and 30 km/h the fix advances **8.3 m per update**. At 6 m/px that is a
1.4-pixel jump — fine. At the 1.5 m/px zoom it is a **5-pixel jump once a
second**, and the map visibly stutters. Redrawing the same stale position faster
does not help; the position itself has to move.

Between fixes, extrapolate from the last one along the reported course:

```
pos(t) = last_fix + bearing(course) · speed · (t - t_fix)
```

Render at **8 Hz while moving**, 1 Hz when stopped. When a fix arrives, compare
it with the extrapolated position: within 5 m, ease across it over three frames;
beyond 5 m, snap — that is a genuine correction and hiding it would be lying
about where you are. Course is interpolated the same way, so a course-up map
rotates continuously rather than in 1 Hz steps.

Optionally raise the receiver to 5 Hz with `UBX-CFG-RATE` (`measRate = 200 ms`),
which shortens the extrapolation from 1 s to 200 ms. That is a menu-free
one-shot message at startup, gated behind a config flag, and it is a refinement
rather than a dependency — dead reckoning is what actually buys the smoothness.

### 6.4 Cost, and why 8 Hz fits

Measured from the live device tree: the panel's `spi-max-frequency` is
**4 MHz**. A full-screen update is 240 lines × ~52 bytes ≈ 12.5 KB ≈ 100 kbit,
so **25 ms of SPI time, a ~40 Hz hardware ceiling**. At 8 Hz that is 200 ms of
SPI per second — 20% of the bus, with the DRM damage tracking likely sending
less.

On our side of the write: expand the 12 000-byte 1-bit backbuffer to the
384 000-byte XRGB frame and `write()` it. At 8 Hz that is 3.2 MB/s of
memory traffic against a Cortex-A53 with multi-GB/s bandwidth. 20 000 points ×
(rotate + project + reject) at 8 Hz is a few million float ops per second.
Neither is close to a limit.

The frame is skipped entirely when the 1-bit backbuffer `memcmp`s equal to the
previous one, so a stationary display costs nothing regardless of the nominal
rate. **This is what makes 8 Hz affordable rather than merely possible:** the
high rate only applies while something is actually changing.

### 6.5 Basemap — OSM, never Google — `nav-turn-osm.png`

**Google Maps is ruled out on licensing, not technology.** Google's ToS forbids
caching tiles offline, extracting the data, and re-rendering in another style —
and this device needs all three (it navigates offline, and no online map style
survives a 1-bit threshold). The same applies to Apple/HERE/Mapbox raster
services.

**OpenStreetMap is the source.** The data is ODbL — offline use and re-rendering
are the norm — and we draw it in our own cartography, which we need anyway.
Attribution ("© OpenStreetMap contributors") goes in the README and on the
OVERVIEW title line when tiles are present.

`nav-turn-osm.png` is rendered from **real data**: an Overpass extract of every
road around the Asok / Sukhumvit junction in Bangkok (`osm-asok.json`, 557
ways), with the route built from the actual carriageway geometry — north up
Ratchadaphisek/Asok, right at the junction, pin on the first soi past it. Two
things this proved beyond what the synthetic grid could:

- **Real density works.** ~200 ways in view at 15 m/px, drawn as 1 px
  hairlines with 2 px arterials, reads as a city rather than as clutter, and
  the cased route stays legible crossing Sukhumvit's dual carriageway.
- **Naming is not geometry.** The approach road south of the junction is named
  "Ratchadaphisek Road", not "Asok Montri" — the first chain-building attempt
  found zero points and produced a 0 m turn distance. The production pipeline
  never touches names (the GPX carries the route), but any tooling that does
  should expect this.

Production shape (phase 4): a Mac-side tool cuts an extract along the route
corridor (±2 km), classifies highways into two weights, and pre-renders 1-bit
tiles per zoom level of the ladder in §6.1; the device blits tiles under the
track/route/marker layers. 256×256 at 1 bit is 8 KB per tile against 53 GB
free. The layering already exists — `nav-turn-nomap.png` is the same renderer
with the tile layer absent.

---

## 7. Routes

### 7.1 Loading GPX

Routes live in `/home/beepy/routes/*.gpx`. A hand-written scanner, not an XML
parser: find `<trkpt` or `<rtept`, pull the `lat`/`lon` attributes in either
order, then `<ele>`, `<name>`, `<cmt>`, `<desc>`, `<sym>`, `<type>` up to the
closing tag. Decode `&amp; &lt; &gt; &quot; &#nn;` in text fields only.

`libexpat` is installed with headers, so this is a choice: the GPX that matters
is machine-generated and flat, the scanner is ~120 lines and directly testable,
and zero dependencies keeps a Buildroot package possible. If a real file defeats
the scanner, expat is a drop-in replacement for that one function.

Points are decimated on load to a 20 000 cap (16 bytes each → 320 KB against
319 MB available). A malformed file fails with a message naming the line, rather
than half-loading.

### 7.2 Snapping and progress

Nearest point by projection onto route *segments*, searched in a window of ±100
points around the last match. A full scan every fix would be 20 000 segment
projections per second for no benefit. On load, or after 30 s lost, the window
widens to the whole route once.

`% DONE` and `TO GO` come from a cumulative-distance array built at load time.
ETA is `TO GO ÷ rolling average speed over the last 10 minutes`, falling back to
the trip average in the first 10 minutes.

### 7.3 Off route

Off when the perpendicular distance exceeds **40 m for 3 consecutive fixes**;
back on below **25 m**. The asymmetry stops the panel flickering on a wide road.
**There is no rerouting** — no routing engine and no map data. The display says
how far off you are and which way the route lies, and that is all it can
honestly do.

### 7.4 Cues

Preferred source is the file: `<rtept>` with `<sym>`/`<type>`/`<cmt>`, which is
what Komoot and RideWithGPS emit. When a route has no cues — a bare `<trk>`,
the common case for a shared ride — they are **derived**:

```
resample the route at 10 m
bearing_in  = bearing over the 25 m before the point
bearing_out = bearing over the 25 m after
theta       = signed difference
|theta| < 30      no cue
30..50            slight        50..115   turn
115..160          sharp         >160      u-turn
merge cues within 20 m, keeping the largest |theta|
```

Derived cues carry no street name, so the panel shows the arrow and the distance
with no name line rather than inventing text.

### 7.5 Alerts

At 500 m, 200 m and 50 m from a cue, flash the keyboard LED (§3, needs the udev
rule). No page switching is needed — the NAV page is already showing the turn.
This is a quiet benefit of collapsing to two pages: the auto-switch behaviour an
earlier draft needed, along with its "return to the previous page" rule and its
configuration toggle, simply does not exist.

---

## 8. Data model

Only what the two pages consume:

```c
typedef struct {                 /* live, from NMEA */
    double lat, lon;             /* GGA/RMC                     */
    double speed_kmh, course;    /* VTG                         */
    int    fix, sats;            /* GGA quality, GGA sats used  */
    double hdop;
    char   utc[9];
    time_t last_fix;
} fix_t;

typedef struct {                 /* loaded once from GPX        */
    pt_t  *pt;    int npt;       /* lat/lon/ele, decimated      */
    double *cum;                 /* cumulative metres           */
    cue_t *cue;   int ncue;      /* index, kind, name           */
    char   name[64];
    double total_m;
} route_t;

typedef struct {                 /* recomputed each fix         */
    int    seg;    double along; /* snap result                 */
    double off_m;  int off;      /* cross-track, latched        */
    int    cue_i;  double cue_m; /* next cue and distance to it */
    double togo_m; time_t eta;
} nav_t;
```

No trip counters, no ascent, no timers. `gps_t`'s satellite arrays from
`gps-monitor` are not needed either — only the count, for the status line.

---

## 9. Code structure

`gps-monitor.c` is 1596 lines, of which roughly 800 are generic: font, canvas,
framebuffer, evdev input, NMEA parsing, serial. Copying that would guarantee two
divergent copies, so split it into the module layout its own `STRUCTURE.md` §2
originally specified and then collapsed:

```
libbeepyfb/    font.c canvas.c fbdev.c input.c     panel, glyphs, keys
libnmea/       nmea.c serial.c gps.c               sentences -> fix_t
gps-monitor/   view_bars.c view_sky.c main.c       unchanged behaviour
beepy-nav/     seg.c arrows.c gpx.c route.c map.c
               view_nav.c view_overview.c nav.c
```

**The split is verifiable, which is the reason to trust it:** `gps-monitor
--demo --page bars --dump` before and after must produce byte-identical
384 000-byte frames. `make check` already does the dump; adding `cmp` against a
stored reference turns the refactor into a mechanical change with a pass/fail
gate.

New code estimate: seg digits 80, arrows 140, gpx 180, route 260, map 240,
view_nav 200, view_overview 160, nav main 180 ≈ **1450 lines**.

---

## 10. Verification

| What | How |
|---|---|
| Module split | byte-identical `gps-monitor` dumps before and after |
| Renderer geometry | `--dump` both pages from demo data, convert on the Mac, compare against these mockups |
| Renderer on the real panel | `fbshow --verify` byte-compares `/dev/fb1` against the dump — the trick that found 0 of 320 000 bytes differing for `gps-monitor` |
| Mockup ↔ device font agreement | `mockup.py` parses `FONT[]` from the C source; a glyph change cannot desync |
| Parser | `--replay` a captured NMEA log; `--print` dumps `fix_t` as text |
| Route maths | `--replay` a GPX ride against its own route: off-route stays 0, `% DONE` climbs monotonically to 100, `TO GO` decreases monotonically |
| Off route | replay the same ride with a synthetic 100 m detour spliced in; assert the latch fires once and clears once |
| Cue classifier | hand-label the junctions of one real GPX, assert the derived set matches |
| Clipping | render at a zoom where the route leaves the map on all four sides; assert no ink lands in x < 130 |
| Console restore | every exit path leaves `fbterm` in `S+`, never `T` |

The clipping test earns its place: the turn panel is the only part of this
screen that is never allowed to be painted over, and it is the one thing a map
bug would destroy.

---

## 11. Phases

| Phase | Content | Verifiable by |
|---|---|---|
| 0 | Module split, `%<>*` glyphs, VTG parsing | byte-identical `gps-monitor` dumps |
| 1 | Coverage canvas + threshold, generated numeral tables, cue glyphs, panel layout, static NAV page | dumps vs `nav-arrows.png`, `nav-smooth.png` |
| 2 | GPX load, snap, cues, corner rounding, live NAV + OVERVIEW, no basemap | replay against a real GPX |
| 3 | Off route, alerts, units, zoom and orientation keys, 8 Hz dead reckoning | detour replay; frame timing under `--replay` |
| 4 | Optional raster basemap and its build tooling | visual, on the panel |
| 5 | FIND + on-device router (Dijkstra, oneway-aware) + CONFIRM | routed path vs a reference route; oneway violations = 0 |

Phase 2 is the first genuinely useful build: it navigates.

---

## 12. Deliverables

```
beepy-nav/
  DESIGN.md          this file
  mockup.py          the mockups, at native geometry          (done)
  nav-turn.png       NAV, synthetic stand-in basemap          (done)
  nav-turn-osm.png   NAV over real OSM data (Asok, Bangkok)   (done)
  osm-asok.json      the Overpass extract behind it           (done)
  nav-turn-nomap.png NAV, route only -- what phase 2 ships    (done)
  nav-turn-off.png   NAV, off route                           (done)
  nav-overview.png   OVERVIEW                                 (done)
  nav-search.png     FIND screen, real matches                (done)
  nav-confirm.png    routed overview + GO/CANCEL              (done)
  nav-arrows.png     cue glyph set at three sizes             (done)
  nav-smooth.png     threshold vs Bayer dither, before/after  (done)
  nav-smooth-hard.png / -dither.png   the two full frames  (done)
  Makefile
  src/               per §9
  README.md          build, install, keys, config format
```

Installed to `/usr/local/bin/beepy-nav` on the Raspberry Pi OS install that is
actually on the device — same as `gps-monitor`.

**Out of scope:** no Buildroot package, no `br_defconfig` or `zero2w_defconfig`
change, no rerouting, no cloud sync, and none of the cycle-computer features
listed in §14.

---

## 13. What is needed from you

1. **One root action:** a udev rule (or setuid helper) giving group `input`
   write access to `/sys/firmware/beepy/led*`, or cue alerts are visual only.
2. **A real GPX** you would actually ride. Synthetic routes will not expose the
   cases that break cue derivation.
3. **Metric or imperial as the default** — the reference screenshot is in feet;
   the config default is currently metric.

Open questions only the panel can answer:

- Is 128 px the right panel width? It is 32%, from the screenshot. A narrower
  panel gives the map more room but caps the digits below 62 px.
- Do the 16 px `THEN` arrows survive on the real display, or is 24 px the floor?
- Do the anti-aliased numerals hold up at 1× on a reflective LCD in daylight?
  Thresholded supersampling is clearly better on a monitor; a reflective panel
  in sun has lower effective contrast, where a slightly heavier, fully-aliased
  glyph might read further.
- Is 8 Hz worth the power, or is 4 Hz with dead reckoning indistinguishable?
- Course-up with a frozen-below-3 km/h heading, or is north-up simply better on
  a screen this size?

---

## 14. What was removed, and what it bought

The previous draft was a Lezyne Mega XL clone: five pages, ten configurable data
fields, elevation profile, menu, ride recording to GPX, satellite page. All of
it is gone.

| Removed | Consequence |
|---|---|
| Data-field pages and the grid engine | −220 lines, and no field catalogue, config format or auto-fit-per-cell logic |
| Elevation page | GPS-altitude filtering, ascent hysteresis, grade windowing and VAM all disappear — §8.3 of the old draft was the most error-prone maths in the program, and it was only feeding a page nobody asked for |
| Menu page | No modal input handling, no submenu stack; settings become a config file and four keys |
| Ride recording | No `.trk` journal, no crash-recovery conversion, no `fsync` policy, no ODO persistence |
| Satellite page | No satellite arrays, no GSV reassembly, no sky projection or label placement in `nav` |
| Trip counters | No distance-accumulation gating, no auto-pause, no moving-vs-elapsed timers |

Roughly **650 lines and one whole class of bug** — silently wrong accumulated
totals — leave with them. What remains is the part that was always the point:
follow the route, show the next turn.

If any of it is wanted later, `gps-monitor` still has the satellite pages. The
rest is not recoverable — this workspace is not under version control and the
previous draft of this file and its six mockups were overwritten. The removed
designs are summarised in the table above and nowhere else.

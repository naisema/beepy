# beepy-vid — design

A video player for the Beepy: a 400×240 one-bit Sharp memory LCD on a Pi Zero
2 W. Video is pre-transcoded on the Mac into a purpose-built pack; the device
streams pre-dithered frames and does no decoding, scaling or dithering.

This document is a specification and an argument. Every constant below is
either measured — with the measurement named — or flagged as a guess with the
experiment that would retire it. It follows `beepy-nav/DESIGN.md`, and where
it contradicts that document it says so explicitly rather than quietly
diverging.

**Status: proposed. Nothing here is implemented.**

---

## 0. The one number this design is built on

The panel is **not refresh-rate limited**. It is SPI-bandwidth limited, and
the cost of a frame is proportional to **the number of rows written**:

```
panel_fps(rows) ≈ 420000 / (52 × rows + 2)
```

Measured on `beepy.local` with `fbterm` SIGSTOPped, by counting SPI messages
in `/sys/class/spi_master/spi0/spi0.0/statistics/messages` — a direct count
from the kernel, not an inference:

| rows written | measured fps | bytes/update | effective B/s |
|---|---|---|---|
| 60  | **135.4** | 3122  | 422,700 |
| 120 | **68.0**  | 6241  | 424,388 |
| 168 | **48.0**  | 8737  | 419,376 |
| 240 (full) | **33.1** | 12481 | 413,121 |

The fit holds within 2% across a 4× range at 84% utilisation of the 4 MHz bus.

### 0.1 This corrects `beepy-nav/DESIGN.md` §6.4

`beepy-nav/DESIGN.md:1504-1507` states:

> A full-screen update is 240 lines × ~52 bytes ≈ 12.5 KB ≈ 100 kbit, so
> **25 ms of SPI time, a ~40 Hz hardware ceiling**.

The arithmetic is right and the conclusion is wrong. The wire format is
`240 × (2 + 400/8) + 2 = 12482` bytes — a two-byte line tag, 50 data bytes and
a two-byte trailer — and the measured bytes/update at 240 rows is **12481**,
exact. But 12482 bytes at 4 MHz is 24.96 ms while the measured period is
**30.2 ms**. The missing ~5.3 ms is the kernel's own
`drm_fb_xrgb8888_to_gray8()` and `sharp_memory_gray8_to_mono_tagged()` over
96,000 pixels, which the 40 Hz figure does not account for.

**The real full-screen ceiling is 33.1 fps, not 40.** `beepy-nav/fbplay.c:37`
rejects `fps > 40` on the same wrong number. Neither is a bug in nav — nav
runs at 8 Hz and has 4× headroom either way — but the record should be
corrected in its own commit, per the standing rule that a measurement which
contradicts something written down gets fixed in writing.

### 0.2 Therefore: storage is not the constraint, SPI time is

Measured SD sequential read through ext4 on a real 200 MB file: **22.0 MB/s**
(23.5 MB/s raw at `bs=1M`, 20.4 MB/s at a realistic `bs=64k`). Free space on
`/`: **52 GB**. Demand at 24 fps is 270 kB/s — **1.2% of the card**.

So compression saves bytes nobody is short of. The valuable field in the pack
format is not a compression mode; it is **the two bytes saying which rows
changed**, because a Sharp panel is line-addressed and fewer lines is less SPI.
That reframing drives §2 and §3.

### 0.3 And a partial write really does cost less

The critical question — whether `sharp_drm` honours a sub-range write or
flushes all 240 lines regardless — was measured, not assumed:

| bytes written | rows actually transferred |
|---|---|
| 120 rows (192000 B) | 119 |
| 239 rows | 239 |
| 240 rows | 239–240 |
| **1 row (1600 B)** | **0 — no update at all** |

`sharp_memory_fb_dirty()` clips `x1=0, x2=width` always but passes `y1/y2`
through untouched: **damage is row-banded — full width, arbitrary row range.**
The one-row case is a real edge — a write of exactly one line length yields a
zero-height damage rect and is silently dropped — and the player must never
emit one.

---

## 1. Target environment (measured 2026-08-01, not assumed)

| Property | Value | How |
|---|---|---|
| Panel | `/dev/fb1`, 400×240, **32 bpp XRGB**, stride 1600, frame 384000 B | `fbset` |
| Driver | `sharp_drm` 1.5, source on device at `/usr/src/sharp-drm-1.5/src/` | `/sys/module/sharp_drm/version` |
| Refresh mechanism | **damage-driven, no kthread, no fixed period**; `spi_sync_transfer()` in the caller's context | `drm_iface.c` |
| VCOM timer | 1000 ms GPIO toggle — DC balance, **not** a refresh | `vcom_timer_callback()` |
| SPI clock | 4,000,000 Hz (`cdiv=100` from `core_freq=400`, exact) | device tree |
| CPU | Pi Zero 2 W, **2 of 4 cores online**, `arm_freq=1000` | `nproc`, `vcgencmd` |
| RAM | 426 MB total, ~334 MB available, 257 MB in buff/cache | `/proc/meminfo` |
| SD read | 22.0 MB/s through ext4; card `SR64G`, 50 MHz 4-bit | `dd`, `/sys/kernel/debug/mmc0/ios` |
| Free disk | 52 GB on `/` | `df -h` |
| zlib | present on device (`1.2.11`); **absent on the Mac** | `ls /usr/include` |
| Audio | **only `vc4hdmi`, unplugged.** No speaker exists | `/proc/asound/cards` |
| Bluetooth | **disabled** — `dtoverlay=disable-bt`, no HCI adapter | `/boot/config.txt:83` |
| PulseAudio | user daemon **already running**, pid 17551, `bluez5-discover` loaded | `systemctl --user` |
| Keyboard | one real device: `beepy-kbd` on `event0` | `/proc/bus/input/devices` |
| ffmpeg | **not installed on the Mac**; not on the device either | `which ffmpeg` |

---

## 2. Geometry: the panel is exactly 16:9 plus a status band

```
400 × 9/16 = 225.0
```

Exact — not 224.7 rounded. Fitting 16:9 at full width leaves **precisely 15
rows**, and 16:9 is the aspect of essentially everything anyone will
transcode. So:

> The panel is cut once, at y = 225, into a 400 × 225 **stage** and a
> 400 × 15 **band**. Video is fitted inside the stage; whatever the stage does
> not use is matte; the band is permanent and video never enters it.

One rule, no aspect-ratio special cases. Fit table:

| source | fit rule | video rect | left over |
|---|---|---|---|
| **16:9** | full width | 400 × 225 | 15 px band |
| 4:3 | fit height | 300 × 225 | band + two 50 px pillars |
| 1.85:1 | full width | 400 × 216 | band + 9 px matte |
| 2.39:1 | full width | 400 × 167 | band + 58 px matte |

The matte is a feature, not waste: it is where rendered subtitles go (§6.4).

**Rejected: splitting the 15 px into two 7–8 px bands.** A 7 px band cannot
hold an 8 px glyph cell at all. One 15 px band holds a text row *and* a
progress bar; two slivers hold neither.

**Rejected: a 24 px band matching the house `TITLE_H`.** It would force video
to 400 × 216 — cropping 9 of 225 rows, or pillarboxing and wasting 16 px of
width. 15 px is free and 24 px is not.

### 2.1 Frame rate: match the source, and never resample unevenly

At 225 rows the ceiling is **35.9 fps**. Utilisation:

| target | 225 rows | 240 rows |
|---|---|---|
| 24 fps | 67% | 71% |
| 30 fps | 84% | 89% |

**A 24 fps source is transcoded at 24 fps.** This resolves a conflict in the
team's reports: an earlier reading of the 40 Hz figure suggested decimating to
12–15 fps and picking an *exact integer* ratio to avoid 3:2 pulldown judder.
The judder argument is correct and important — uneven frame durations read
worse than fewer frames — but the measurement makes the trade unnecessary.
**24 fps native is affordable, so there is no pulldown and no judder.** The
rule becomes:

- Source rate fits under ~75% of the row-adjusted ceiling → **use it natively**.
- It does not → **exact integer decimation only** (30→15, 60→20), never 3:2.

30 fps at 84% is possible but has only 16% headroom and depends on the pacing
in §4.3; it is available via `--fps 30` and is not the default.

Raw cost at 24 fps, 400×225 (11250 B/frame): **162 MB per 10 minutes**,
270 kB/s. Both are non-issues (§0.2).

---

## 3. The pack format — `.vid`

Little-endian throughout, fixed-width fields, no implicit padding. The header
reuses the shape `tools/mkpack.py:60-63` argues for — *"two packs read by the
same program should not differ in the eight fields they have in common"* — so
`magic`/`version`/`header_bytes`/`flags`/`nsect` sit at 0/8/10/12/14 exactly as
in `BNAVTILE` and `BNAVROAD`.

### 3.1 Bit polarity — the trap

There are already **two** 1bpp→XRGB paths in this repo and they use
**opposite** conventions:

- `libbeepyfb/fbdev.c:108` — `expand()` maps a **set bit → black**
  (`canvas.h:9`, `INK 1`).
- `beepy-nav/fbplay.c:4` — declares its format as *"MSB first, **1 = white**"*.

A pack built to fbplay's convention plays back as a photographic negative, and
the failure looks like a working player. **`.vid` uses `canvas_t` polarity:
MSB leftmost, set bit = INK = black, stride 50.** That is what lets a payload
`memcpy` straight into `cv->bits` and `fb_present()` work unchanged.

### 3.2 HEADER — 64 bytes at offset 0

| off | size | type | field |
|---|---|---|---|
| 0 | 8 | `char[8]` | magic, `"BEEPYVID"` (no NUL) |
| 8 | 2 | `u16` | version, 1 |
| 10 | 2 | `u16` | header_bytes, 64 |
| 12 | 2 | `u16` | flags — bit 0 rows MSB-first/ink; bit 1 pack uses deflate (refuse at open, not at frame 4800); bit 2 audio present |
| 14 | 2 | `u16` | nsect, 3 |
| 16 | 2 | `u16` | width, 400 |
| 18 | 2 | `u16` | height, 240 |
| 20 | 4 | `u32` | nframe |
| 24 | 4 | `u32` | fps_num |
| 28 | 4 | `u32` | fps_den — **a ratio, never a float**; `24000/1001` is a real rate |
| 32 | 4 | `u32` | gop — max frames between keyframes |
| 36 | 4 | `u32` | audio_rate, Hz; 0 = none |
| 40 | 2 | `u16` | audio_ch |
| 42 | 2 | `u16` | audio_bits, 16 (S16LE, the only v1 value) |
| 44 | 4 | `u32` | audio_bytes (informational; authority is the section) |
| 48 | 2 | `u16` | img_x0 |
| 50 | 2 | `u16` | img_y0 |
| 52 | 2 | `u16` | img_w |
| 54 | 2 | `u16` | img_h |
| 56 | 2 | `u16` | hysteresis — provenance only; the device never dithers |
| 58 | 2 | `u16` | dither — 0 Bayer 8×8, 1 Bayer 4×4 |
| 60 | 4 | `u32` | reserved, 0 |

`img_*` exists so the OSD can sit in the matte and `--info` can say what was
cropped.

### 3.3 SECTION TABLE — 3 × 8 bytes at offset 64

`u32 off`, `u32 count`, byte-identical in shape to `mkpack.py:81-84`.

| # | section | count means |
|---|---|---|
| 0 | INDEX | frames |
| 1 | FRAMES | bytes (payload arena) |
| 2 | AUDIO | bytes; **0 = no audio** |

Sections begin at 88.

### 3.4 INDEX entry — 12 bytes, `nframe` of them

| off | size | type | field |
|---|---|---|---|
| 0 | 4 | `u32` | **off** — absolute file offset (one fewer addition in the hot loop) |
| 4 | 4 | `u32` | **size** — **`0` is legal: identical to the previous frame** |
| 8 | 1 | `u8` | **mode** — 0 RAW, 1 DEFLATE, 2 XOR_SPANS, 3 XOR_DEFLATE |
| 9 | 1 | `u8` | **flags** — bit 0 keyframe |
| 10 | 1 | `u8` | **y0** — first row this frame may change |
| 11 | 1 | `u8` | **y1** — last row, inclusive |

**Why the dirty rows live in the index and not the payload:** the player learns
a frame's cost — both its bytes and its SPI milliseconds — *before* reading
it. That is what lets the pacing loop drop a frame while dropping is still
cheap, and it is what feeds `fb_present_rows()` (§5.1).

`size == 0` promotes `nav.c:1752`'s runtime `memcmp` skip into a fact the
encoder measured once. A static title card costs 12 bytes and zero SPI.

Index size for a 100-minute film at 24 fps: 12 B × 144,000 = **1.7 MB**, read
whole at open. Against 334 MB available, a two-level index is not worth it.

**u32 offsets cap the file at 4 GiB** — about 4 hours at 24 fps with audio.
Stated, matching `mkpack.py`'s own u32 offsets; `mkvid.py` refuses to exceed it.

### 3.5 Frame payloads

- **mode 0 RAW** — `(y1-y0+1) × 50` bytes, rows `y0..y1` only. A keyframe is
  just the `y0=0, y1=239` case. One rule, no special cases. **A pack of only
  mode 0 needs no zlib at all** — the fallback the design keeps open.
- **mode 1 DEFLATE** — raw deflate (`windowBits=-15`, level 9) of the mode-0
  payload. Raw, not zlib-wrapped: the index has the size and a per-frame
  Adler-32 duplicates what a whole-file hash does better.
- **mode 2 XOR_SPANS** — per-row byte spans of the XOR against the previous
  plane: `u16 nrow`, then `{u8 y, u8 x0, u8 n, u8 v[n]}`, applied as
  `plane[y*50 + x0 + i] ^= v[i]`. Byte columns, not pixel columns. A row may
  appear more than once with disjoint ascending spans; the encoder splits when
  the gap between dirty bytes exceeds 3. Overhead 3 B/row + 2 B/frame.
- **mode 3 XOR_DEFLATE** — deflate of the full XOR band, for pans where the
  XOR is dense but carries the dither matrix's periodicity.

**Decode contract.** One video plane, decoded in place. `size == 0` → present
nothing. Modes 0/1 overwrite rows `y0..y1`. Modes 2/3 XOR into them.

Every multi-byte field is read byte-at-a-time, for the reason
`beepy-nav/src/tile.c:94-98` gives verbatim. There are no `f64` fields, so
tile.c's IEEE-754 caveat does not apply.

### 3.6 Audio — separate, contiguous, not muxed

Interleaved S16LE. The sample for time *t* is at
`AUDIO.off + floor(t × rate) × ch × 2`. Seek is exact arithmetic with no index
and no keyframe concept, and a pack with no audio is a zero-length section —
the *"absent costs exactly nothing"* shape `tile.h:8-15` argues for the
basemap.

**Default 24000 Hz stereo S16LE = 96,000 B/s**, chosen for an arithmetic
property: 96000 divides evenly by every plausible frame rate (4000 B/frame at
24 fps), so the audio offset for frame *k* is an exact integer and never
accumulates rounding over an hour.

### 3.7 Compression: what it is worth, honestly

**Measured 2026-08-01**, 72 frames at 400×225 (11250 B raw), three synthetic
archetypes, screen-anchored ordered dither, all four modes encoded per frame.
This replaces an earlier modelled table whose central claim — *"the XOR of a
pan is essentially incompressible"* — was **wrong by an order of magnitude**.
It predicted ~11,900 B for a panning delta; the measurement is **702 B**. The
model reasoned from the Shannon entropy of a 45%-dense XOR and never accounted
for hysteresis, which makes the XOR sparse in the first place. Caveat in
§3.7.1.

**Re-measured on real footage 2026-08-01** — a 1280×720 animated short and two
scenes from a 1920×1080 live-action music video, 8 s each at native rate,
extracted with the §4 ffmpeg invocation (`flags=area`, `sws_dither=none`,
bitexact). Bayer 4×4, hysteresis 8, bytes per frame:

| clip | RAW | DEFLATE | XOR_SPANS | **XOR_DEFLATE** | best-of-4 | saving | churn | dirty rows | MB/min @24 |
|---|---|---|---|---|---|---|---|---|---|
| animation | 6446 | 4080 | 1619 | **1042** | 958 | **91%** | 2.7% | 129 | 1.4 |
| live-action | 7770 | 2359 | 732 | **403** | 407 | **96%** | 0.3% | 155 | 0.6 |
| live-action 2 | 10562 | 1545 | 1792 | **949** | 960 | **91%** | 0.9% | 211 | 1.4 |

The synthetic clips this replaced gave 298 / 736 / 959 B — **the same order,
and in one case real footage compressed better.** See §3.7.1.

**A 90-minute film is 54–126 MB.** At 24 fps the pack costs 0.6–1.4 MB/min
against 52 GB free. Storage has stopped being a consideration of any kind.

**Three conclusions, each of which reverses an earlier position.**

1. **zlib is not optional; it is the entire win.** `XOR_DEFLATE` beats
   `XOR_SPANS` by 2–5×. The earlier plan — "ship modes 0 and 2, drop `-lz`" —
   would give up most of the compression. Modes 1 and 3 are load-bearing.
2. **Hysteresis is worth more than the matrix choice.** `h=8` cuts a pan from
   2192 to 736 B and bit churn from 2.8% to 0.5%. It exists to make the delta
   coder work, not to make the picture look better, and that is the design's
   one non-obvious coupling. **Default `--hysteresis 8`.**
3. **Bayer 4×4 beats 8×8 on size as well as on look**, by 5–20% at `h=8`. Taken
   with UX's argument that an 8 px cell reads as visible plaid across a 400 px
   panel, 4×4 is now the default on two independent grounds rather than one.

**Dirty rows help dialogue and nothing else.** Mean changed-row span: 140 of
225 for locked-off, but **224–225 for pans and cuts**. §0.2's framing stands
for a static camera and does not survive a moving one — a pan pays the full
SPI cost no matter how few bytes it took to store.

### 3.7.1 The synthetic caveat was itself wrong

This section previously warned that the synthetic figures were an **upper
bound** and that real photographic detail "will compress worse". **That was
wrong, and the reason is worth keeping.**

Real footage measured 958 / 407 / 960 B against synthetic's 298 / 736 / 959 —
the same order throughout, and the live-action scene compressed *better* than
the synthetic pan. The intuition behind the warning was that photographic
detail is less compressible than a sinusoid, which is true of the *source* and
irrelevant to the *pack*: **dithering to one bit at 400×225 discards almost all
of that detail before the encoder ever sees it.** What reaches the delta coder
is dominated by the dither structure, and with hysteresis the frame-to-frame
XOR is sparse regardless of how busy the source was. Source complexity barely
survives the 1-bit bottleneck.

The remaining caveat is narrower and honest: three 8-second clips are not a
corpus. Dark scenes, film grain and heavy noise are all under-represented, and
grain in particular defeats hysteresis by construction. Expect a bad clip to be
several times worse than these; expect the *ordering of the modes* to hold.

**One number is now in question rather than settled.** `h=16` beat `h=8` on
every clip (915 / 326 / 695 B), suggesting a higher default. It is not adopted,
because **these measurements score size and cannot see smearing**: hysteresis
works by making a pixel reluctant to change, which is exactly what trails a
moving edge. Choosing between 8 and 16 needs eyes on the panel, not another
table. `--hysteresis` stays at 8 until someone looks.

### 3.8 Keyframes and seeking

`gop` default **one second of frames** (24). Seek to *t*: index directly by
`i = floor(t × fps_num / fps_den)`, walk back ≤ `gop` entries to the nearest
keyframe, discard the rings, decode forward without presenting. Audio offset
is arithmetic. Cost: ≤23 XOR applies at 11250 B — sub-millisecond.

---

## 4. Playback

### 4.1 The clock: audio is master, unconditionally

`CLOCK_MONOTONIC` and a headset's DAC crystal drift apart; over a two-hour film
that is seconds of lip-sync. Video slaves to audio, always. With no audio
track the fallback is `mono_now()` and the arithmetic is unchanged.

```
played_pts = written_pts − sink_latency
target     = floor((played_pts + av_offset) × fps_num / fps_den)
```

- `target == last_shown` → **do nothing.** A memory LCD holds its image with
  no power and no bus traffic. Never resubmit an unchanged frame; it would burn
  a full SPI transfer for no visible change.
- `target == last + 1` → present.
- `target > last + 1` → **skip directly to `target`**, decoding intermediates
  without presenting (a delta frame is the next frame's premise and cannot be
  skipped), or seek to the next keyframe if more than `gop` behind.

Frame selection is integer arithmetic, never a float accumulator.

### 4.2 One `poll()` loop. No threads.

```c
struct pollfd pfd[1 + 1 + MAX_EVDEV];   /* audio pipe, pack fd, keys */
timeout = min(ms_until(next_frame), ms_until(ring_low), 100);
```

Same shape as `nav.c:3828-3866`. The case against threads is arithmetic:
per-frame cost is ~0.3 ms inflate + 20 µs XOR + ~0.4 ms table-driven expand +
~0.5 ms write ≈ **1.5–2 ms against a 41 ms period at 24 fps, about 4% duty**.
The 25 ms of SPI runs in a *kernel* worker, which is where the second core is
already going; a userspace thread would contend with the only thing that is
busy. And `grep -rn pthread` over this repo returns **zero matches** —
introducing the project's first thread to protect a 2 ms critical section
against a 41 ms deadline is not defensible.

The honest counter-argument is an SD read stall, and the answer is a buffer,
not a thread: a **512 kB video ring** and **256 kB audio ring**, refilled by
`pread()` whenever `poll()` returns early, plus
`posix_fadvise(POSIX_FADV_SEQUENTIAL)` at open. **Named fallback, not built
now:** if measurement shows stalls exceeding a frame period, the reader is the
one thing worth a thread — single producer, single consumer, one ring. That is
contained and does not touch the format.

**mmap is ruled out** for the pack (a cold fault is synchronous on the render
thread) and ruled out for the framebuffer (§5.2).

### 4.3 Pacing needs back-pressure

`write()` gives none — it returns into a shadow buffer and a workqueue
transfers later. Frames submitted while the previous transfer is still running
are **merged away**, silently.

**Measured 2026-08-01 at 400×225, fbterm stopped, landed frames counted from
the kernel's SPI message counter:**

| target | naive `nanosleep` | sysfs back-pressure |
|---|---|---|
| 20 fps | 100% | 100% |
| **24 fps** | **100%** | **100%** |
| 30 fps | **84.7%** | **100%** |
| 34 fps | 75.3% | **100%** |

So **24 fps needs no back-pressure at all** — the default is safe with a plain
paced loop — and 30 fps is fully achievable *with* it. The cliff is a phase
problem, not a bandwidth one, and it appears between 24 and 30.

The mechanism is polling
`/sys/class/spi_master/spi0/spi0.0/statistics/messages` — one small sysfs read
says the previous transfer completed. Ugly, exact, and it is how every number
in §0 was obtained.

**And never flood.** Submitting as fast as possible is not merely wasteful, it
is *counterproductive*: 150 frames written back-to-back at 400×225 produced
**4 panel updates**. Damage-merge discards the rest. A player that "renders as
fast as it can" would display a slideshow.

---

## 5. What `libbeepyfb` needs

### 5.1 `fb_present_rows()` — the high-leverage change

```c
int fb_present_rows(fb_t *f, const canvas_t *c, int y0, int y1);
```

`pwrite()` of `(y1-y0+1) × line_len` bytes at `y0 × line_len`.
`fb_present()` becomes `fb_present_rows(f, c, 0, f->h - 1)`.

It belongs in the shared library because it is a property of **the panel and
the fbdev write path**, which is what `fbdev.c` exists to own — and because
`beepy-nav` wants it too. Today `nav.c:1752-1758`'s skip is all-or-nothing: a
NAV frame where only the countdown digits changed writes all 384,000 bytes and
spends the full 25 ms. `DESIGN.md:1502-1508` computes that budget and then
leaves it on the table. One function pays both apps.

**Must refuse a single-row range** (§0.3): a one-line write produces no update
at all. Clamp to a minimum of two rows.

### 5.2 `expand()` — three writers, none compared

There are **three** independent 1bpp→XRGB implementations —
`canvas_dump()` (`dump.c:22-31`), `expand()` (`fbdev.c:103-111`), and
fbplay's LUT — and **nothing compares any of them to each other**. Every
existing golden goes through `canvas_dump()`, never `expand()`. So optimising
`expand()` for a video blit changes what `beepy-nav` and `gps-monitor` put on
the panel with **zero test coverage.**

This is the highest-risk shared change in the project. Before anything touches
either, add a unit test asserting `expand()` and `canvas_dump()` agree on one
canvas.

Then: replace the per-pixel bit test with `fbplay.c`'s 256×32 LUT and eight
`memcpy`s. Same output byte for byte, prior art in this tree, no API change,
no golden movement.

**Measured 2026-08-01 on the device, 225 rows, 2000 reps:
0.750 ms → 0.163 ms, a 4.59× speedup.** Better than the 1.5–3 ms → 0.4 ms
that was estimated. At 24 fps this returns 14 ms of CPU per second.

### 5.3 `libbeepyfb/le.h` and `libbeepyfb/tick.h`

`rd_u16`/`rd_u32`/`rd_i32` are `static` in `tile.c:100-132` and `pack.c` would
be the third copy. Header-only `static inline`, zero cost, no new object.

`mono_now()` / `sleep_s()` are `static` at `nav.c:1618-1638`, with a third
copy in `netfetch.c:38`. **Counter-argument on the record:** timing has nothing
to do with a framebuffer and putting it in `libbeepyfb` makes the library's
name slightly untrue. A fourth top-level directory for two functions is worse.
Put it there with an honest note in the header.

### 5.4 What is deliberately *not* added

- **`keycode_to_char()` stays per-app.** `input.h:4-5` states the rule and
  `nav.c:3175` and `gps-monitor/main.c:58-72` already diverge by design.
- **No shared config parser.** `config.h:37-93`'s whole claim is that *the set
  of keys is the documentation*; a generic callback parser loses that. Copy the
  shape — never fatal, every bad line warned with its number — and accept the
  duplication.
- **Nothing audio.** PulseAudio is not a framebuffer concern.
- **The OSD must not go through `cov_t`.** `cov_resolve()` (`cover.c:1203`)
  `memset`s every destination row before thresholding, which would erase the
  decoded frame underneath. `view_play.c` writes straight into a `canvas_t`
  with `canvas.h`/`font.h` primitives. This is the concrete respect in which a
  video player does not fit the existing page idiom, and it is better stated
  than discovered.

---

## 6. Screens

Character grid: the only font is 5×7 in a 6×8 cell, **uppercase ASCII 32–90
only** (`font.c:5`), and `glyph()` clamps anything outside to space — so `!`
`"` `#` `$` `&` `'` `;` `@` render as **blank**, not a missing-glyph box.

| scale | cell | cols | rows |
|---|---|---|---|
| 1 | 6×8 | 66 | 30 |
| 2 | 12×16 | 33 | 15 |
| 3 | 18×24 | 22 | 10 |

240 is divisible by 8, 16 and 24. Every vertical constant below is a multiple
of one of them.

### 6.1 There is no overlay on one bit

Three compositing operations exist on a 1bpp canvas and exactly one is legible
over arbitrary content:

| operation | over dithered video | verdict |
|---|---|---|
| solid fill | legible; destroys what is under it | the only one that works |
| **invert a rectangle** | **illegible** | ruled out |
| keyline | legible, but eats the counters of a 5×7 glyph | worse than solid fill |

**The invert arithmetic, because it is the tempting option.** A Bayer-dithered
midtone is 50% ink *by construction*. Inverting that rectangle yields a region
that is still 50% ink — same local mean, same spatial frequency, same apparent
tone. Only the dither *phase* changed. A 5×7 glyph is 4 dither cells wide;
the eye cannot resolve a phase shift over 4 cells as a letter. **There is no
contrast to read the text with.**

Invert works only where coverage is already near 0% or 100% — which over video
is exactly what you cannot predict. So it is not a treatment for video. It *is*
the correct treatment for a progress bar inside an already-opaque band, and
that is the only place this design uses it.

So the question is not how to make an overlay readable. It is **how many rows
of video to destroy, and for how long.**

### 6.2 PLAYBACK — three OSD states

**BAND (15 px, always).** Costs zero video pixels — it is outside the stage by
construction. Ink ground with paper text: a permanent *white* bar under a dark
film is a light source you look past for ninety minutes.

```
y225  ----------------------------------------------  1 px paper rule
y226  > 12:34 / 45:00              VOL 60  BT OK      8 px, scale 1
y234  [###################.......................]    6 px progress bar
```

`1 + 8 + 6 = 15`, nothing left over. The leading state glyph is one character,
all of which exist in `FONT[]`: `>` playing, `=` paused, `W` waiting on audio,
`E` ended.

**TRANSIENT (40 px, 2.5 s).** Fired by any key that changes something, growing
*upward* from the band so the two read as one thing swelling. Scale 3, because
it exists to answer "what did I just press" from across the room. Costs the
bottom 40 of 225 stage rows for 2.5 s.

**PAUSED PANEL (64 px, while paused only).** The insight that removes a mode:
**a paused frame conveys no motion, so covering 28% of it costs nothing.** An
earlier draft had a separate pinned-OSD mode bound to a key; pausing already
produces exactly the state where a large persistent panel is free, so the pin
is cut. Layout closes exactly:
`1 + 3 + 16 + 3 + 12 + 3 + 16 + 2 + 8 = 64`, with the full 64-character keymap
on the last scale-1 row.

### 6.3 The progress bar, and why seek needs a number

386 px of track:

| runtime | s/px | a 10 s seek moves the bar by |
|---|---|---|
| 3 min | 0.47 | 21 px |
| 34 min | 5.3 | **1.9 px** |
| 90 min | 14.0 | **0.7 px — nothing** |

On a feature-length film a seek press produces **no visible bar change at
all**, and a user watching a bar that does not move concludes the key is
broken. That is why the transient shows the *delta* numerically
(`-30S -> 12:04`) and why the delta is the larger element.

Chapter ticks are 1 px columns of **the opposite of whatever they land on** —
ink inside the paper fill, paper inside the trough. They self-invert as the
playhead crosses them and are always visible with no extra logic. The one
place one bit is easier than colour.

No playhead marker: on 1 bit the fill boundary *is* the playhead at 1 px
precision, and adding a marker means drawing a notch, which reads as damage.

### 6.4 LIBRARY, END, SYNC, HELP

**LIBRARY** — `TITLE_H 24` + 7 rows × `ROW_H 22` + 36 px detail + `FOOT_H 26`
= 240 exactly (`(240−24−36−26)/22 = 7.0`, no remainder). Constants match
`chooser.c:20-26`. The detail strip costs one list row and earns it: it shows
the transcode settings (`24FPS BAYER4`) and the audio sink. Dithering discards
tone irreversibly, so "this looks bad" is almost always a re-transcode and
almost never a player bug — a user who cannot see the settings files the bug
against the player. And if Bluetooth is not connected there is **no audio path
at all**; discovering that after pressing play, in silence, is the worst
ordering.

Empty state names the directory it searched, following `chooser.c:116-125`.

**END** exists because *"a frozen panel and a panel with nothing to say look
the same"* (`DESIGN.md:88-94`). A memory LCD holds its last image with no
power, so a player that simply stops on the final frame is byte-identical to
one that segfaulted — and the `fbterm` underneath may or may not have been
resumed. The END card is the only way the panel can distinguish those.

**SYNC** — §7.2. **HELP** — a full keymap page, because there are 16 bindings
and no room for a hint row over video.

### 6.5 Keys

No digits anywhere: they need the Beepy symbol layer
(`nav.c:3218-3258`, `DESIGN.md:1096-1097`), and nothing here wants one.

`SPACE` play/pause · `←`/`→` seek ∓10 s · `↓`/`↑` seek ∓60 s (magnitude by
axis, one mental model) · `C`/`V` volume (the `Z`/`X` adjacent-pair pattern
from `DESIGN.md:1038`) · `M` mute · `T` subtitles · `F` fit/fill · `Z`/`X`
frame step **while paused only**, so it cannot collide with seek · `N`/`P`
next/previous · `A` sync · `H` help · `ESC` library · `Q` quit.

**Seek coalescing.** `DESIGN.md:1057` notes the trackpad in arrow mode *is* the
arrow keys, so a swipe emits a burst at the kernel repeat rate. Without
coalescing one swipe is thirty seeks and thirty A2DP re-primes. Presses within
400 ms accumulate into one pending seek, with the running total shown live so
the user steers an accumulator rather than firing shots. **400 ms is a guess**
and should be set from the measured trackpad repeat interval.

**`Q` does not confirm, and that is a considered departure.**
`DESIGN.md:955-1027` argues at length that `Q` must open a confirmation page —
but the argument is specifically about **loss**, and the three facts that page
carries are all irrecoverable. Here nothing is lost: every exit writes the
resume position. §1.6's own test applied to this app returns the opposite
answer, and a confirmation with nothing to warn about is a keystroke tax.

### 6.6 Subtitles are rendered, never baked

A 12 px letter dithered is mush. **Burned-in subtitles are unreadable on this
panel, full stop.** Carry them as a text track and render with the 5×7 font at
scale 2, solid ink on solid paper, in the matte where one exists. This is what
the letterbox matte is for.

**Unresolved:** the font is uppercase-only and drops eight punctuation marks
(§6). Subtitles would render ALL CAPS with `!` silently vanishing. Whether
that is acceptable, or whether this warrants a second pre-rendered face, is
left open — commit `41f2b3e` suggests the project has been round a related
loop.

---

## 7. Audio

### 7.1 Bluetooth requires permanently reconfiguring the device

**Nothing in this section may be done without the owner's explicit consent.**
There is no speaker on this device and no analog output; `/proc/asound/cards`
lists only an unplugged `vc4hdmi`. **Without Bluetooth this is a silent-film
player.**

Enabling it, in order:

1. **Get consent.** The serial console goes away, a reboot is required.
2. `/boot/config.txt:83` — comment out `dtoverlay=disable-bt`. **Do not
   substitute `dtoverlay=miniuart-bt`**: mini-UART's clock is tied to the VPU
   core clock and A2DP over it is a known dropout source.
3. `/boot/cmdline.txt` — remove `console=serial0,115200`.
4. **`systemctl disable serial-getty@ttyAMA0`.** This is the #1 footgun: it is
   active right now, and after step 2 `ttyAMA0` belongs to the HCI link. A
   getty on it corrupts the link and BT fails in a way that looks like a
   firmware problem.
5. **Reboot** — unavoidable, it is a device-tree change.
6. Pair over SSH, not from the panel. `trust <MAC>` is what gives
   auto-reconnect.

Firmware is already present (`SYN43430B0.hcd`), `pi-bluetooth`, `bluez 5.55`
and `btuart` are installed, the PulseAudio user daemon is already running with
`module-bluez5-discover` loaded. **No PulseAudio configuration is needed at
all** — a `bluez_sink.*.a2dp_sink` appears automatically.

**7. Check `bluetoothd`'s plugin set — this device had it crippled.**
`/lib/systemd/system/bluetooth.service` was found locally modified (`dpkg -V`
reports `??5??????`) to run `bluetoothd --plugin=a2dp`, which loads *only* the
a2dp plugin and therefore disables two others that matter here:

- **`avrcp`** — without it there is no absolute volume control, so
  `pactl set-sink-volume` is software attenuation the speaker never sees, and
  §6.5's `C`/`V` keys could not change the speaker's actual volume. Symptom:
  `org.bluez.MediaTransport1` has **no `Volume` property** on any device, which
  reads as a limitation of the speaker and is not.
- **`policy`** — the plugin that auto-reconnects audio profiles for trusted
  devices.

Fixed on 2026-08-01 with a drop-in at
`/etc/systemd/system/bluetooth.service.d/10-restore-plugins.conf` — an empty
`ExecStart=` to clear the inherited value (it is otherwise additive) followed
by the plain binary. Verified: `MediaTransport1.Volume` returns `uint16 127`
and `MediaControl1`/`MediaPlayer1` appear. A drop-in rather than an edit to
`/lib`, so an apt upgrade of `bluez` cannot silently revert it.

Risks: losing the serial console removes the recovery path on a device whose
only other output is this panel — the escape hatch is that `/boot` is FAT and
editable from any PC, and that should be written down *before* step 2. The
CYW43436 shares one 2.4 GHz radio, so **disconnect WiFi during playback**.
`module-suspend-on-idle` is loaded, costing ~1–2 s to resume. And PA is a
per-user daemon in the tty1 autologin session, so a player launched over SSH
needs `XDG_RUNTIME_DIR=/run/user/1000` or it fails silently.

None of this transfers to the Buildroot image, which ships neither bluez nor
PulseAudio.

### 7.2 A2DP latency, and why there is a calibration screen

A2DP always makes audio **lag**, by 150–250 ms dominated by the speaker's own
buffer. This is the fortunate direction: human tolerance is strongly
asymmetric, because audio arriving late is what distance does in the physical
world. The correction is to **delay the video**, never to rush the audio — and
delaying video is nearly free here, 6 frames × 11250 B = 68 kB.

`pa_stream_get_latency()` reports PA's *own* pipeline latency. It does not know
the buffer inside the headset, which is the larger and more variable half. So
it is **a lower bound that typically understates by 50–150 ms** — enough to
seed a default, not enough to be right. Hence: seed from PA, default to
200 ms, **store the trim per sink MAC**, and offer live `+`/`-` keys.

The instrument is a **flash and a click**: once per second a 60 px disc fills
for exactly one frame while a 40 ms click is queued at the same PTS. The user
nudges until they coincide. On one bit a one-frame all-or-nothing state change
is the sharpest temporal event this display can produce.

**Honest caveat:** a full-frame SPI write takes ~30 ms, during which the panel
updates top-to-bottom, so the flash's leading edge is smeared across that
window. Well inside the ~45 ms detection threshold, but it is a hard floor on
precision — which is why the fine step is 10 ms and not 1 ms.

**Two operational rules the sync depends on:** re-prime on every discontinuity
(seek, pause, track change) or the offset is right at the start and drifts
after the first seek — the failure that reads as *"calibration doesn't work"*;
and seek is **audibly gapped by design**, because a quarter-second of silence
beats a quarter-second of the wrong audio.

### 7.3 `audio_cmd`, not linked libpulse

The audio sink is a **child process fed over a pipe**, configured as
`audio_cmd` in the config file — for exactly the reason `config.h:80-83` gives
for `fetch_cmd`: *"that is what lets a test substitute `cat FIXTURE` and keeps
the gate off the network structurally rather than by promise."* Here it keeps
`make check` off the sound card structurally: the gate sets
`audio_cmd = cat >/dev/null` and no assertion depends on a paired headset.

**This overrules the systems recommendation to link `libpulse-simple`.** That
would buy `pa_simple_get_latency()`, but §7.2 establishes the reported figure
is a lower bound that understates by more than the trim it would replace — so
it buys an estimate, not a measurement, at the cost of violating
`beepy-nav/DESIGN.md:4`'s **libc-only** rule and quietly killing the Buildroot
packaging goal that rule protects. The calibration screen is the source of
truth either way.

---

## 8. Testing

### 8.1 The Mac/device split

The precondition that makes the fast lane possible: **the decoder and the
scheduler are portable C; the only device-specific code is the framebuffer
write, the audio child, and evdev.** That is what makes `host/beepy-nav`
possible (`Makefile:1218-1219`) and it must hold here or every interesting test
costs a 25-minute round trip.

The device has `python3` (3.9.2) — `make check` already runs `tools/fbdiff.py`
there. What it lacks is Pillow. So a **pure-stdlib fixture generator runs in
both lanes**, and video fixtures need not be committed binaries.

### 8.2 Goldens for a moving image

Three separable claims, three artefacts:

**(a) The pack** — built twice, `cmp`ed, then `cmp`ed against a reference. Same
shape as `T-TILES-DETERMINISM` (`Makefile:1364-1370`). Proves `mkvid.py` is
reproducible; proves nothing about correctness (a pack of uniform grey is
perfectly deterministic).

**(b) The decode — a frame-hash manifest, committed as *text*.** One line per
frame: `idx  sha256-of-the-11250-byte-packed-frame`. A 9 MB binary golden is
unreviewable, and the standing rule is that regenerating a golden belongs in
its own commit *with the reason* — which you cannot write about a blob. A
manifest gives `git diff` that says "frames 300–420 moved, 121 of them".

**The manifest must be produced by the C decoder, not by `mkvid.py`.** If the
builder writes it the gate is `mkvid.py == mkvid.py`. The committed pack is the
builder's output; the manifest is the decoder's.

**And it needs an anti-vacuity assertion.** A decoder returning frame 0 forever
produces a beautifully stable manifest. `--distinct-frames N` asserts exactly N
distinct hashes for a clip where every frame differs by construction. Without
it the manifest is decoration.

**(c) Four to six named frames** frozen as `.fb`, for diagnosis.
`tools/fbdiff.py` already reads 384000-byte XRGB dumps, 12000-byte packed
canvases *and* PNGs (`fbdiff.py:41-53`) — the existing diff tool reads the
native video frame format today.

Regeneration guarded by `VID_OK=1`, matching `GOLDEN_OK`/`TILES_OK`/`ROADS_OK`.

### 8.3 Determinism: keep ffmpeg out of the golden path

**Split the pipeline at the gray8 boundary**, exactly as `pbf2osm.py` /
`osm-asok.json` / `mktiles.py` already does for maps — the upstream
version-sensitive step sits outside, the committed intermediate is small, and
the gate exercises the deterministic half.

```
IN.mp4 --ffmpeg--> clip.gray8 + clip.s16      } outside the gate
clip.gray8 + clip.s16 --mkvid.py--> clip.vid  } inside the gate
```

Letterbox, dither, delta-encode and pack are all pure integer Python and stay
*inside* the gate. Only resampling is outside.

Remaining sources and their kills:

| source | kill |
|---|---|
| ffmpeg version/SIMD | record `libavcodec` version + input SHA-256 in the header; gate asserts a match rather than silently re-encoding |
| filter threading | `-threads 1 -filter_threads 1`; `-sws_flags` pinned; `sws_dither=none` (we dither) |
| Python hash randomisation | `sorted()` at every set→list boundary, **and build once under `PYTHONHASHSEED=0` and once under `=1` and `cmp`** — the existing determinism gates run both builds in one environment and would not catch an unsorted set |
| float ties in the dither | **all dither arithmetic in integers.** `Makefile:586-594` and `design_gate.py:22-32` both exist because a coverage value landed at exactly 50% and the tie-break flipped. Integers mean **there is no tie**, which is what lets video goldens be `cmp`-exact where the nav design gate needs a 480 px tolerance |
| PIL | none in the golden path; `mockup.py:81-83` already records that a bare `convert("1")` Floyd–Steinbergs |
| zlib encoder output | **`--no-deflate` for committed fixtures** — stable in practice but not guaranteed by the API. A separate assertion checks a deflate pack *decodes* to identical frames |
| locale / mtime / paths | none in the header; ASCII-fold explicitly, per `Makefile:1510-1516` |

### 8.4 Fixtures — generated in both lanes, gitignored

Following `$(RDIR)/*.nmea` (`Makefile:349-352`), not `asok.tiles`. What gets
committed is the *manifest* and a handful of `.fb` frames.

| clip | detects |
|---|---|
| **count** — frame index as a binary bar | **ordering, drops, duplicates by index, not by hash.** Turns "tearing" into a number |
| **edge** — one hard edge moving 1 px/frame | tearing, ordering, **short writes to `/dev/fb1`** |
| **checker1** — static 1 px checkerboard | 1bpp bit order and packing, against a hand-computed `0xAA/0x55`; `--distinct-frames 1` |
| **checker1-shift** — the same, translating | **dither crawl.** The only thing that catches it — every still-frame golden passes a crawling dither |
| **gray** — flat fields at 17 luma levels | the dither matrix *exactly*: ink counts must land on exact multiples of 96000/64 = **1500** for Bayer 8 |
| **cut** — 20 white then 20 black | the `size == 0` skip must not advance the A/V clock by *frames written*. This is how lip-sync drifts |
| **odd** — 37 frames at 23.976 | end-of-stream off-by-one and PTS rounding accumulation |
| **long** — ~20 min, device lane, never committed | streaming vs mmap. Every short gate clip passes on a design that dies on the first real film |

### 8.5 Timing: injected stalls, never real load

The prior pain is documented — *"Every unpaced version of a fetch test in this
file has raced, four times in one day"* (`Makefile:978-983`). Three rules
transfer, and one has an exact video analogue that is the trap to avoid:
**`checker1` and `cut` are static, so under the `size == 0` skip they produce
no writes, and a frame-gap assertion on them measures the skip, not the frame
rate.** Assert gaps on `count`/`edge` only. That is the identical mistake, one
domain over.

**Do not create CPU load** — a hog on a 2-core Pi Zero is a flaky test and this
repo has paid for flaky timing four times. Make drops **deterministically
injectable**: `--stall-at 30:500` inserts a synthetic 500 ms stall before frame
30, so *"a 500 ms stall at frame 30 of a 24 fps clip drops exactly 12 frames"*
is arithmetic with one right answer. Same shape as the existing `--key N:x` and
`--dump-at N:file` affordances.

**A new assertion the repo lacks:** after a stall the player must not sprint
through its backlog, which needs a **minimum** gap. `assert_trace.py` has
`--max-frame-gap` and nothing on the other side, and a player that dumps its
backlog at once passes every existing check. Propose `--min-frame-gap`.

**No beepy-vid test may ever be paced**, and the device-lane additions must
total **under 3 minutes**. `make check` is already ~25 minutes; if it grows,
people stop running `make sync` and every risk here gets worse.

### 8.6 The `kill -CONT` net

`fb_take()` SIGSTOPs every `fbterm`; `fb_release()` SIGCONTs. `nav.c:3714-3715`
installs handlers for **SIGINT and SIGTERM only**. A missed CONT leaves the
device looking dead, and **a video player runs unattended for twenty minutes —
precisely when an SSH session drops.**

| path | testable | note |
|---|---|---|
| clean exit, SIGINT, SIGTERM | yes, automatable | assert `ps -o stat=` on fbterm is not `T` |
| **SIGHUP** | yes — **and it is missing today** | `fbanim` traps HUP; `nav.c` does not. **The shell wrapper is more careful than the C program** |
| SIGSEGV/SIGBUS/SIGABRT | yes, via `--crash-at 30:segv` | nothing in this repo tests any crash path, while `nav.c:3288` says the debt is owed *"on every exit path including a crash"* |
| **SIGKILL** | **structurally untestable as a program property** | reframe: assert fbterm is `T` after `kill -9`, and that the next clean run restores it |

The crash handler must `kill(pid, SIGCONT)` and `_exit()` — both
async-signal-safe — and must **not** call `fb_release()` as written, which ends
in `system()` for the tmux nudge (`fbdev.c:95-97`). Pids are captured at
startup into a `volatile sig_atomic_t` array, because `fb_take()` walks `/proc`
with `opendir`/`fopen`, none of which is legal in a handler.

**Three belts, in increasing robustness:**

1. Handle the fatal signals (above). Does nothing for SIGKILL.
2. **A wrapper script**, as `fbanim:17-23` already does:
   `trap 'kill -CONT $(pgrep -x fbterm)' EXIT INT TERM HUP`. The shell survives
   its child's SIGKILL. Cheap, proven in this tree, ship it from day one.
3. **DRM master** — while a userspace master holds `/dev/dri/card0` the kernel
   suppresses the in-kernel fbdev client and restores it when the fd closes,
   **including on SIGKILL, because the kernel does it**. No signalling, no
   watchdog. This is the strongest argument for the DRM path — stronger than
   the pacing one — and it is why §9 lists it as v2 rather than never.

`test-panel` runs **last** in `check` and self-traps: `make check` runs over
SSH, and a test that SIGSTOPs fbterm then deliberately crashes can wedge the
console of the machine being tested. Wedging at minute three costs the whole
25-minute run.

### 8.7 One lock, and it must be honoured

There is exactly one real keyboard: `beepy-kbd` on `event0`. If `beepy-nav` is
running it holds `EVIOCGRAB`, so `beepy-vid` would draw over nav's frames and
receive **zero keys** — unresponsive and unquittable from the panel.

**`beepy-vid` must call `evdev_grab_failed()` immediately after
`evdev_open(1)` and refuse to start.** That is exactly what `input.h:29-31`
already spells out; the ride path deliberately ignores it, and this app must
not. The grab is kernel-enforced and released on fd close *including on a
crash*, which is more than a lockfile can promise — so **acquire it before
touching the panel**. `nav.c:3735-3736` already orders this correctly; keep
that order.

### 8.8 Re-gating `libbeepyfb`

A change there is its own commit, and that commit runs `make host`,
`make design-gate`, `make test-tiles` on the Mac and `make check`, `make bench`
on the device. `design-gate`'s tolerances are `MAX_PX = 480` overall but
**`PANEL_MAX = 0`** (`design_gate.py:57-58`) — one wrong pixel in the turn
panel fails, deliberately. And `bench` matters most and is skipped most:
`DESIGN.md:1226` measures nav drawing at **17 ms/frame, over half its 8 Hz
budget**, so a `cover.c` change costing 3 ms is invisible to every correctness
gate and breaks nav.

---

## 9. Risks, ranked, and what to prototype

**Prototypes 1 and 2 have been run (2026-08-01). Both are retired**, and both
contradicted something written here — the pacing cliff is between 24 and 30 fps
rather than at 30 (§4.3), and the compression model was wrong by an order of
magnitude (§3.7). What remains:

1. **A/V drift over an hour with A2DP.** A wrong *offset* is recoverable by the
   live trim; only *drift* is fatal, and a 30-second test cannot see it. The
   test is a 10-minute clip with a flash+click every 30 s, filmed at 240 fps
   slow-mo — which quantises to 4.2 ms, and after calibrating out the phone's
   own fixed capture offset gives **±5–10 ms**, an order of magnitude inside
   the budget.
2. **`--hysteresis` 8 vs 16 cannot be settled by measurement.** 16 is smaller
   on every clip, but hysteresis smears moving edges and no byte count can see
   that. It needs a person looking at the panel (§3.7.1). Everything else about
   the codec is now measured on real footage.
3. **`expand()` divergence** — three XRGB writers, none compared (§5.2).
4. **The panel left dead after a crash or SSH HUP** (§8.6).
5. **Dither crawl on panned content** — only `checker1-shift` sees it.
6. **Streaming that isn't** — 334 MB free, and a 20-minute film is ~350 MB raw.
   Measured compression makes this far less likely than it looked, but mmap is
   still the obvious wrong implementation.
7. **Gamma is a guess.** `format=gray` gives luma still in the video transfer
   function, so dithering directly over-darkens midtones. Whether the panel
   wants a curve is unknown until someone photographs a 16-step ramp — which is
   what the `--demo --page ramp` golden is for.

**Resolved by prototype #2: Bayer 4×4, hysteresis 8.** The matrix question was
listed here as unresolved between UX (4×4, because an 8 px cell is 50 cells
across a 400 px panel and reads as plaid — which is what
`beepy-nav/DESIGN.md:1336-1343` complained about when it *rejected Bayer on
evidence*) and Dev (8×8, for 65 levels and LZ77 periodicity). Measurement
favours 4×4 on size too, by 5–20%, so both arguments now point the same way. It
remains a header field and a build flag.

**Also settled: error diffusion is disqualified on measurement, not
intuition.** Floyd–Steinberg churns **41.3%** of bits frame-to-frame on a
locked-off shot against Bayer's **1.0%** (0.3% with hysteresis) — a 40×
difference. That is the boiling artefact, quantified, and it is why the pack
format can assume temporal stability at all.

### 9.1 `maxcpus=2` — leave it alone

The reason was found, not guessed:
`beepy-buildroot/customization_scripts/max_2_cores.sh:5` —
`# Restrict CPU to 2 cores to limit peak power use`, exposed as a build input
defaulting to *off*. Someone turned it on for this device deliberately, and
then hand-applied it to a Raspbian `cmdline.txt`. That is a considered decision,
twice.

Gain for beepy-vid: **essentially zero** — the panel is SPI-bound, and more
cores cannot make a 4 MHz bus faster. Cost: higher peak current on a handheld
LiPo, which is what the owner was avoiding. And there is **no cheap
experiment**: `/sys/devices/system/cpu/cpu2/online` does not exist, because with
`maxcpus=2` the kernel never registers those CPUs. Changing it means editing
`cmdline.txt` and rebooting.

**Recommendation: do not change it.**

---

## 10. Build integration

```make
VIDPACK ?= beepy-vid/tests/vid/clip.vid
VIDGRAY ?= beepy-vid/tests/vid/clip.gray8    # committed, 400x225, 24 frames
VIDPCM  ?= beepy-vid/tests/vid/clip.s16      # committed, 1 s
VIDOPTS ?= --size 400x225 --fps 24 --gop 24 --hysteresis 8 --no-deflate
```

Fixtures are committed for the same reason the tile pack is: `check` runs on
the **device**, where there is no ffmpeg and no numpy.

New targets, mirroring the existing `test-<thing>` / `<thing>` + `_OK` pairs:
**`test-vid`** (Mac: determinism ×2 plus the `PYTHONHASHSEED` pair, roundtrip,
deflate-changes-bytes-never-pixels), **`test-vid-unit`** (device, folded into
`test-unit`, building packs byte by byte in C and asserting every refusal),
**`test-panel`** (device, last, self-trapping), **`vidpack`** (guarded by
`VID_OK=1`), and five `--demo --page` frames added to `check` and `goldens`
— including `! cmp -s goldens/vid-play.fb goldens/vid-paused.fb`, because a
build whose OSD drew nothing would pass both `cmp`s and freeze nothing at all.

**A real collision to avoid:** `Makefile:1330-1332`'s `host/%.o` pattern means
`beepy-vid/src/config.c` would fight `beepy-nav/src/config.c` for
`host/config.o`. Video objects are prefixed `host/vid-%.o`.

`make sync` needs no change — it rsyncs `./` with `--exclude-from=.gitignore`.
Real videos live in `~/videos` on the device and are never in the repo, exactly
like `~/packs/`.

**Prerequisite to write down:** `brew install ffmpeg` on the Mac, next to where
`tools/mkmaps.sh` names its own. It is not installed today.

---

## 11. What was removed

- **A pinned-OSD mode.** Pausing already produces the state where a large
  persistent panel is free (§6.2).
- **A quit confirmation.** Nothing is lost on exit (§6.5).
- **Threads, in v1.** 4% duty against a 41 ms deadline (§4.2). Named as a
  contained fallback, not built.
- **`libpulse`.** It buys a lower bound, not a measurement, at the cost of the
  libc-only rule (§7.3).
- **Buffering UI.** Read demand is 1.2% of the card; a spinner would imply a
  problem that cannot occur (§0.2).
- **A free-space meter.** 52 GB is 80 hours.
- **XOR + RLE.** Doubles the size of a pan (§3.7).
- **mmap of the framebuffer.** Capped at ~17 Hz by the `HZ/20` defio timer.

# beepy-vid — implementation plan

The specification is `beepy-vid/DESIGN.md`. This is the order to build it in,
what each step is allowed to break, and the gate that says it worked.

**Nothing here is started.** The design is measured end to end (panel, pacing,
`expand()`, codec on real footage) and the prototypes in `bench/` are committed.

## The ordering principle

Two rules decide the sequence, and both come from this repo's own scar tissue:

1. **A safety net goes in before the thing it protects.** `expand()` has three
   independent implementations and zero golden coverage (`DESIGN.md` §5.2);
   the test that pins its behaviour must exist *before* the commit that changes
   it, or the test is just a description of whatever the new code does.
2. **Push work into the Mac lane wherever it can go.** `make check` is ~25
   minutes on the device. Every module that can be portable C must be portable
   C, which is what makes `host/beepy-nav` useful and is stated at
   `Makefile:1218-1219`.

Milestones are sized to be one commit each, in the repo's style: a reason in
the message, and a gate that can genuinely fail.

---

## M0 — the `expand()` safety net (no behaviour change)

**Problem.** `expand()` (`fbdev.c:103`) and `canvas_dump()` (`dump.c:12`) both
turn a 1bpp canvas into XRGB8888 and **nothing compares them**. Every golden
in `goldens/` goes through `canvas_dump()`; the panel path goes through
`expand()`. They are only known to agree by inspection.

They also differ in two real ways found while reading:

- `canvas_dump()` clips to `c->w`/`c->h` and pads short canvases with paper.
  `expand()` reads `w`×`h` unconditionally. **They agree only for a full-size
  400×240 canvas** — which the test must therefore use, and say why.
- `canvas_dump()` writes explicit bytes and documents itself as *"identical on
  any endianness"* (`dump.c:6`). `expand()` stores `uint32`, so on a big-endian
  host it would emit `FF 00 00 00` where dump emits `00 00 00 FF`. Both targets
  are little-endian, so this is latent, not live — but the test would silently
  encode the assumption, so it gets a comment rather than a pretence.

**Decision this milestone forces: move `expand()` out of `fbdev.c`.**
`fbdev.c` needs `linux/fb.h` and does not build on the Mac (`fbdev.h:3`), so a
test linking it is device-only and costs 25 minutes per run. `expand()` itself
is pure arithmetic with no framebuffer in it. Moving it to a new portable
`libbeepyfb/expand.c` puts the test in the fast lane and adds `host/expand.o`
to `HOST_OBJS`. `fbdev.c` keeps `fb_present()` and includes the header.

- **Deliverable:** `libbeepyfb/expand.c` + declaration moved to a header;
  `libbeepyfb/tests/test_expand.c`; `UNIT_TESTS` and `FB_OBJS`/`HOST_OBJS`
  updated.
- **Assertions:** `expand()` and `canvas_dump()` produce byte-identical output
  for a full-size canvas across several patterns (all paper, all ink,
  checkerboard, single pixel at each corner, a text run). Plus the
  anti-vacuity pair the repo insists on: two *different* canvases must **not**
  produce identical output, or a stubbed `expand()` passes.
- **Gate:** `make host && make test-unit` on the Mac. `make check` on the
  device once, because `FB_OBJS` changed and the link must still work.
- **Risk:** low. No behaviour change; a file moves.
- **Stop-point:** if the two functions turn out *not* to agree, stop and report
  before changing either. That result would mean a golden somewhere is wrong,
  which is a much bigger conversation than this milestone.

## M1 — `fb_present_rows()` and the LUT `expand()`

Now the safety net exists, the two measured wins can land.

- **Deliverable:** `int fb_present_rows(fb_t *, const canvas_t *, int y0, int y1)`
  using `pwrite()`; `fb_present()` becomes the full-range case. `expand()`
  gains a row range and the 256×32 LUT (**measured 4.59× faster**,
  `DESIGN.md` §5.2).
- **Must refuse a single-row range.** A write of exactly one line length
  produces a zero-height damage rect and **no panel update at all** (§0.3).
  Clamp to two rows minimum and assert it.
- **Gate:** `make host`, `make design-gate`, `make test-tiles` on the Mac;
  `make check` **and `make bench`** on the device. `bench` is the one most
  likely to be skipped and most likely to matter — `DESIGN.md:1226` measures
  nav drawing at 17 ms/frame against its 8 Hz budget.
- **Risk: this is the highest-risk milestone in the project.** It changes what
  two shipping apps put on the panel. M0 exists entirely to make it safe.
- **Bonus, not the point:** nav gets the same saving — a countdown-digit frame
  becomes ~30 rows instead of 240, 4.5 ms instead of 30.2.

## M2 — pack reader and codec, in portable C

- **Deliverable:** `beepy-vid/src/pack.{h,c}`, `codec.{h,c}`, `libbeepyfb/le.h`.
  Reader validates the header, loads the index, decodes all four modes into a
  bare `unsigned char[11250]`.
- **Linkable on its own**, so tests build fixtures byte by byte in C rather
  than shelling out to Python — the argument `Makefile:1444-1449` already makes
  for `test_tile`.
- **Assertions (`test_pack`, `test_codec`):** every refusal — short file, bad
  magic, wrong version, truncated index, `off + size` past EOF, `y1 >= 240`, a
  non-key frame at index 0, `gop` exceeded with no keyframe. Round-trip each
  mode. `size == 0` presents nothing.
- **Gate:** Mac, `make test-unit`. No device needed.
- **Risk:** low, and it is the most testable code in the project.

## M3 — `tools/mkvid.py`

- **Deliverable:** the packer, split at the gray8 boundary so ffmpeg sits
  **outside** the gate (`DESIGN.md` §8.3), mirroring how `pbf2osm.py` /
  `osm-asok.json` / `mktiles.py` already work for maps.
- **Committed fixtures:** a small `clip.gray8` + `clip.s16`; the `.vid` pack
  built from them.
- **Gate — `test-vid`:** build twice and `cmp`; `cmp` against the committed
  pack; build under `PYTHONHASHSEED=0` and `=1` and `cmp` (the existing
  determinism gates run both builds in one environment and would miss an
  unsorted set); the deflate pack must differ in bytes and be identical in
  decoded pixels; `--info` round-trips the header.
- **Risk:** medium — determinism is fiddly. All dither arithmetic is integer,
  so there are no ties to break (§8.3), which is what lets these goldens be
  `cmp`-exact where the nav design gate needs a 480 px tolerance.

## M4 — the player skeleton, silent

First thing that puts video on the panel.

- **Deliverable:** `vid.c` — argv, config, `poll()` loop, integer frame
  selection, `CLOCK_MONOTONIC` clock, panel handover, signals, read-ahead
  rings. No audio, no OSD beyond the band, no library page.
- **Critical, and easy to omit:** call `evdev_grab_failed()` right after
  `evdev_open(1)` and **refuse to start** (§8.7). Without it, launching while
  `beepy-nav` runs gives a player that draws over nav and receives zero keys —
  unresponsive and unquittable from the panel.
- **Signals:** `SIGINT`/`SIGTERM`/**`SIGHUP`** plus the fatal set, async-signal-safe
  (`kill` + `_exit`, never `fb_release()` — it ends in `system()`). Ship the
  `fbanim`-style wrapper script from day one.
- **Gate:** `--demo --page play --at N --dump` goldens; the anti-vacuity
  `! cmp -s` between play and paused; `test-panel` device-only and **last** in
  `check`, self-trapping.
- **Milestone test:** play a real pack on the panel and watch it. This is the
  first point where the project is visibly real.

## M5 — pacing and drop behaviour

- **Deliverable:** sysfs back-pressure (needed only above 24 fps — measured,
  §4.3), drop-to-target, never-resubmit-unchanged, `--stall-at N:ms`,
  `--trace-frames`.
- **Gate:** injected-stall arithmetic, never real load — *"Every unpaced
  version of a fetch test in this file has raced, four times in one day"*
  (`Makefile:978`). Plus the new `--min-frame-gap` assertion: after a stall the
  player must not sprint through its backlog, which nothing in
  `assert_trace.py` can currently catch.
- **Trap to avoid:** assert frame gaps on `count`/`edge` fixtures only. Static
  fixtures produce no writes under the `size == 0` skip, so a gap assertion on
  them measures the skip, not the frame rate — the identical mistake to
  `Makefile:542-550`, one domain over.

## M6 — UI

Library, the three OSD states, END, HELP, resume position. Geometry is fully
specified and closes to the pixel (§2, §6), so this is mostly execution.

- **Gate:** goldens per page, plus the empty-state and `--no-pack` cases, and
  `T-VID-OPTIONAL` in the shape `T-BASEMAP-OPTIONAL` argues for — an explicit
  fixture, never "no flag", because the device's own config names a
  `videos_dir` and "no flag" would silently open whatever is in it
  (`Makefile:240`).

## M7 — audio and A/V sync

Last, because it is the only part that cannot be gated without hardware.

- **Deliverable:** `audio.c` (child process on a pipe, `audio_cmd`), the audio
  clock, the SYNC calibration screen, per-sink offset in the config.
- **Gate:** the scheduler against a **fake sink** with configurable latency —
  all of it clock arithmetic, runs on the Mac, and a sink deliberately
  consuming 1% slow must produce a trace where video follows it. `make check`
  sets `audio_cmd = cat >/dev/null` so the gate never touches a speaker.
- **Then measure for real:** flash-and-click at 240 fps slow-mo gives ±5–10 ms
  (§7.2). Both speakers now work, so this is unblocked.
- **Known per-device wrinkle:** the VOX-DC exposes no AVRCP absolute volume
  while the Spotless D1 does, so the `C`/`V` keys control the speaker on one
  and only attenuate in software on the other. The player must tolerate both
  rather than assume.

---

## Deferred, explicitly

- **Subtitles.** The font is uppercase-only and drops eight punctuation marks
  including `!` (§6.6). Rendering them is specified; whether ALL CAPS is
  acceptable is an open question and not worth blocking on.
- **DRM master.** Buys real back-pressure and SIGKILL-safe console restore
  (§8.6), but v1 does not need it and it is a large change.
- **Threads.** 4% duty against a 41 ms deadline (§4.2). Named as a contained
  fallback if SD stalls exceed a frame period; not built.
- **`--hysteresis` 8 vs 16.** 16 is smaller on every clip but no byte count can
  see edge smearing. Needs eyes on the panel (§3.7.1).

## What would make me stop and come back

- M0 finding that `expand()` and `canvas_dump()` **disagree** — that implicates
  a committed golden.
- M1 moving any golden. Under the standing rule a golden is never edited to
  make a test pass; a real move needs its own commit and a reason.
- `make bench` regressing nav's 17 ms/frame.
- Any measurement contradicting `DESIGN.md`. That gets corrected in writing, in
  the commit that found it — as already happened three times this session.

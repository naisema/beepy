# beepy

Software for the **Beepy** — a Raspberry Pi Zero 2 W with a 400×240 1-bit Sharp
memory LCD and a BlackBerry keyboard.

Two things live here, and they are unrelated beyond sharing that device:

| | |
|---|---|
| **[`beepy-nav/`](beepy-nav/README.md)** | a GPS bike navigator — turn-by-turn, an offline OpenStreetMap basemap, and on-device routing. The main project. |
| **[`gps-monitor/`](gps-monitor/DESIGN.md)** | the earlier, simpler app it grew out of: satellite bars and a sky view |
| `beepy-buildroot/` | a checkout of [michaelstepner/beepy-buildroot](https://github.com/michaelstepner/beepy-buildroot), vendored. **Its own git root, its own build.** Nothing above is part of that image. |

Everything here is C, runs on the Raspberry Pi OS install already on the device,
and is built by the top-level `Makefile`.

---

## beepy-nav

```
MAP        where you are, with no route loaded -- what it opens on
NAV        turn arrow, distance to the junction, and a live map
OVERVIEW   the whole route, fitted, with progress
FIND       type a destination; CONFIRM shows the route before you commit
QUIT       what you would be ending, before you end it
```

A bike computer that navigates offline. No phone, no data connection, no
account. You give it a GPX or type somewhere to go, and it draws the next turn
big enough to read at speed on a screen that is legible in direct sun and
invisible in the dark.

**What makes it interesting to work on** is the screen: 400×240 pixels, each
one black or white, no greys and no dithering in the driver. Everything is drawn
by an analytic coverage renderer and thresholded at 50%, which is what makes a
1-bit display look smooth rather than jagged — and every page is byte-compared
against a Python reference so it stays that way.

- **[`beepy-nav/README.md`](beepy-nav/README.md)** — how to ride with it: the
  screens, the keys, the settings, the map packs.
- **[`beepy-nav/DESIGN.md`](beepy-nav/DESIGN.md)** — the specification, and the
  reasoning behind every constant in it. Several of those numbers cost a
  debugging cycle; the file says which and why.

---

## Building

```sh
make host        # Mac: portable objects + host/beepy-nav, a replay binary
make sync        # rsync to beepy.local, build there, and run `make check`
```

**`make check` is the gate, and it runs on the device.** Anything touching
`/dev/fb1`, evdev or termios only compiles under Linux, so the Mac lane covers
the portable half and the device covers the rest. It byte-compares every page
against `goldens/` and runs the unit and replay suites — about 25 minutes on a
Pi Zero 2 W, which is normal rather than a hang.

```sh
make design-gate # Mac: the C pages vs beepy-nav/mockup.py, pixel for pixel
make test-tiles  # Mac: the basemap packs, built twice and compared
make test-roads  # Mac: the road/place packs
```

Map data is built on a Mac and copied over — the device carries no OSM data and
no renderer for it. One command does all of it:

```sh
tools/mkmaps.sh  # download, cut, render, merge, install
```

---

## Layout

```
beepy-nav/     the navigator: pages, routing, search, map packs   9,363 lines C
libbeepyfb/    panel drawing -- the coverage renderer and the 5x7 font   1,711
gps-monitor/   satellite bars and sky view                                764
libnmea/       NMEA sentences -> a fix                                    422
tools/         Mac-side: pack builders, fixture generators, gates   3,313 py/sh
goldens/       18 frozen framebuffer dumps -- what `make check` compares against
install/       a udev rule, so cue alerts can flash the keyboard LED
```

`CLAUDE.md` is the orientation file for coding agents, and is worth reading
first if you are one — it carries the standing rules (never edit a golden to
make a test pass; a test whose "absent" case reads the device config is not a
test) that are easier to state than to rediscover.

---

## Attribution and licence

Map data is **© OpenStreetMap contributors**, used under the ODbL. Tiles are
rendered from OSM extracts in this project's own cartography; nothing is cached
from a commercial tile service, which is a licensing constraint rather than a
technical one.

`beepy-buildroot/` is a third-party project vendored here and carries its own
licence.

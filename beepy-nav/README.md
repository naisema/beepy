# beepy-nav

A two-page GPS route navigator for the Beepy: follow a GPX route, show the
next turn. `DESIGN.md` is the specification and the reasoning; this file is
how to build it, install it and drive it.

```
NAV        turn arrow, distance to the junction, and a live map
OVERVIEW   the whole route, fitted, with progress
```

## Build

Two lanes, both from the repository root:

```sh
make sync           # rsync to beepy.local and run `make check` there
make host           # the portable objects, on a Mac, with a replay binary
make design-gate    # the C pages against mockup.py's own frames (needs Pillow)
```

`make check` is the gate: it byte-compares the `--demo` page dumps against
`goldens/`, runs the unit tests and runs the replay assertions.

## Install on the device

```sh
sudo install -m 755 beepy-nav/beepy-nav /usr/local/bin/beepy-nav
mkdir -p ~/routes && cp RIDE.gpx ~/routes/
```

**Cue alerts need one root action.** `/sys/firmware/beepy/led*` is
root-write-only as shipped, and flashing the keyboard LED at 500/200/50 m from
a turn is the only non-visual alert this hardware has — there is no buzzer.
Install the udev rule that hands write access to group `input`, which the
`beepy` user is already in:

```sh
sudo install -m 644 install/99-beepy-led.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=i2c --action=change
ls -l /sys/firmware/beepy/led*      # expect: --w--w---- root input
```

Without it beepy-nav degrades **silently** to visual-only: the panel still
counts the turn down, nothing errors, and one line on stderr at startup says
why the LED is quiet.

## Run

```sh
beepy-nav                              # choose from ~/routes/*.gpx on the panel
beepy-nav --route RIDE.gpx
beepy-nav --route RIDE.gpx -d /dev/ttyACM0
beepy-nav --route RIDE.gpx --replay ride.nmea      # a recorded ride
```

Useful for development: `--headless` (render, present nothing), `--trace F`
(one TSV row per fix), `--trace-frames F` (one row per rendered frame, with
the dead-reckoning state), `--stats` (render ms and frames drawn/skipped),
`--fps N`, `--dump-at S:F`, `--print`, `--demo --page P --dump F`.

## Keys

| Key | Action |
|---|---|
| `Tab` | switch page |
| `Z` / `X` | zoom out / in — switches the NAV map to manual zoom |
| `A` | return the NAV map to auto zoom |
| `O` | course-up ↔ north-up |
| `U` | units: metric ↔ imperial |
| `H` | hold — freeze the display |
| `Q` | quit |

Letters, not digits: the Beepy's digit row needs the Alt modifier, which is
unusable one-handed.

## Attribution

Any basemap data is © OpenStreetMap contributors (ODbL).

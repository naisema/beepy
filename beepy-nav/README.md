# beepy-nav

A two-page GPS route navigator for the Beepy: follow a GPX route, show the
next turn. `DESIGN.md` is the specification and the reasoning; this file is
how to build it, install it and drive it.

```
NAV        turn arrow, distance to the junction, and a live map
OVERVIEW   the whole route, fitted, with progress
```

When the receiver loses its fix, the NAV panel's bottom row becomes an
inverted `NO FIX` and everything derived from position stops moving rather
than pretending (`DESIGN.md` §1.1.2).

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
`--fps N`, `--dump-at S:F`, `--key S:C`, `--print`,
`--demo --page P --dump F`.

## The ride log

On a real port, every ride is recorded — the raw NMEA byte stream verbatim and
the per-fix trace beside it:

```
~/rides/YYYYMMDD-HHMMSS.nmea
~/rides/YYYYMMDD-HHMMSS.tsv
```

About 1.9 MB an hour, measured. `--no-log` turns it off; `rides_dir` in the
config file moves it. A ride will not start a new log below ~50 MB free, and
says so. Nothing here can stop the navigator: an unwritable directory or a full
disk is one line on stderr and the ride goes on.

This is not ride recording — there is no GPX writer and no trip data. It exists
so that a failure on a road can be replayed on a desk (`DESIGN.md` §7.6), which
is what this turns it into:

```sh
scp beepy.local:rides/20260730-113748.nmea .
tools/ride2fixture.sh 20260730-113748.nmea ROUTE.gpx my-failure
```

That copies the log into `beepy-nav/tests/rides/`, replays it, checks the
replay against what the device itself computed from the same bytes, and prints
the Makefile lines to paste into `test-replay`.

## Keys

| Key | Action |
|---|---|
| `Tab` | switch page |
| `Z` / `X` | zoom out / in — switches the NAV map to manual zoom |
| `A` | return the NAV map to auto zoom |
| `O` | course-up ↔ north-up |
| `U` | units: metric ↔ imperial |
| `L` | cue LED alerts on ↔ off, for this ride |
| `H` | hold — freeze the display |
| `Q` | quit |

Letters, not digits: the Beepy's digit row needs the Alt modifier, which is
unusable one-handed.

## Attribution

Any basemap data is © OpenStreetMap contributors (ODbL).

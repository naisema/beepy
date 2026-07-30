# beepy-nav

A two-page GPS route navigator for the Beepy: follow a GPX route, show the next
turn. `DESIGN.md` is the specification and the reasoning behind every choice
below; this file is how to get a route onto the device and ride it.

```
NAV        turn arrow, distance to the junction, and a live map
OVERVIEW   the whole route, fitted, with progress
```

---

## Quick start

Everything below assumes the program is installed (see **Install**) and a GPS
receiver is on `/dev/ttyACM0`.

```sh
# 1. put a route on the device
scp my-ride.gpx beepy.local:routes/

# 2. on the device, ride it
beepy-nav --route ~/routes/my-ride.gpx

# ...or run it with no arguments and pick from a list on the panel
beepy-nav
```

That is the whole workflow. The panel takes over the screen while it runs and
gives it back when you press `Q`.

### Where a GPX comes from

Any route planner exports one. **Komoot**, **RideWithGPS** and **Strava** all
have a *GPX export* in their route menus — all three formats are read, with or
without turn cues. If a route has no cues in it (a bare `<trk>`, which is what
most shared rides are), beepy-nav derives them from the shape of the road: it
resamples the line and calls anything past 30° a turn, classified slight /
turn / sharp / U-turn (`DESIGN.md` §7.4).

There is no route *planning* on the device — it follows a route, it does not
invent one. Searching for a destination and routing to it is designed and not
yet built (`DESIGN.md` §1.4).

---

## Reading the screen

### The NAV page

```
   ┌──────────┬──────────────────────────┐
   │    ↱     │  (N)              ( 24 ) │   compass · speed
   │          │                   KM/H   │
   │   400    │            ●             │   the cue after this one
   │    M     │            │             │
   │          │            ▲             │   you, pointing where you go
   │──────────│         ┄┄┄┘             │   ┄ where you have been
   │  44 MIN  │  ├─200M─┤                │
   │  12.6KM  │                          │
   │ 10:42 ETA│                          │
   └──────────┴──────────────────────────┘
```

**Left panel — the next turn, and nothing else.**

| Row | Means |
|---|---|
| Arrow | which way to turn at the next junction |
| Big number + unit | how far to *that junction* |
| `44 MIN` | time left on the whole route |
| `12.6KM` | distance left on the whole route |
| `10:42 ETA` | what time you arrive, 12-hour |

The big number **does not tick smoothly, on purpose.** It steps 100 m at a
time beyond a kilometre, 50 m from 200 m out, and 10 m inside that — coarse
where there is nothing to do yet, fine where you are about to act. It also
never counts *up*: GPS jitter on a step boundary would otherwise flicker it
several times a second (`DESIGN.md` §1.1.1). Inside 10 m it holds at `10 M`
and the arrow is the instruction.

**Right — the map, course-up.** The road ahead is up the screen and the map
rotates under you; the compass needle shows where north actually is. Your
marker is the circled chevron, sitting low so two thirds of the map is the
road in front. It leans into a corner while the map is still catching up, so
it always points where you are really going. The thick line with a white
outline is your route, the dashed line is where you have been, and the
teardrop pin is the turn *after* the one being announced.

### The OVERVIEW page — `Tab`

The whole route fitted to the screen, north-up, with every turn marked as a
dot, a flag at the finish, and a strip along the bottom: per cent done, cues
passed, total distance, distance and time remaining.

### When something is wrong

| The panel says | What happened |
|---|---|
| `OFF ROUTE / 85 / M AWAY` | you left the route. The map shows your marker off the line with a dotted trail back to the nearest point on it. There is **no rerouting** — no map data on the device to route with. |
| `NO FIX` *(inverted)* | the receiver has lost the sky for 4 seconds. Everything on the screen is now frozen and stale rather than pretending: the marker stops, the countdown stops. It clears itself when fixes return. |

---

## Keys

| Key | Action |
|---|---|
| `Tab` | switch page: NAV ↔ OVERVIEW |
| `R` | change route — back to the picker, without quitting |
| `Z` / `X` | zoom the map out / in (switches to manual zoom) |
| `A` | back to automatic zoom |
| `O` | course-up ↔ north-up |
| `U` | metric ↔ imperial |
| `L` | cue alerts on ↔ off, for this ride only |
| `H` | hold — freeze the display |
| `Q` | quit, and give the console back |

`R` only does something when there is a picker to go back to — that is, when
you started with a bare `beepy-nav` rather than `--route`. It closes the
current ride log and opens a new one for the next route, because a different
route is a different ride.

Letters and not digits, because the Beepy's digit row needs the Alt modifier
and that is unusable one-handed on a handlebar.

`L` confirms itself for a second and a half (`ALERTS ON` / `ALERTS OFF`) —
without that, muting is indistinguishable from a quiet stretch of route.

---

## Settings

`~/.config/beepy-nav.conf`, one `key=value` a line, `#` starts a comment.
Every setting has a command-line flag that overrides it. A missing file is
fine — these are the defaults:

```ini
units      = metric     # or imperial          (--imperial / --metric)
north_up   = 0          # 1 starts north-up    (--north-up)
led_alerts = 1          # 0 mutes cue alerts
rate_5hz   = 0          # see the note below   (--rate-5hz)
routes_dir = ~/routes   # what the panel chooser lists
rides_dir  = ~/rides    # where ride logs go
```

An unknown key or a malformed line is a warning naming the line number, never
a failure to start.

**`rate_5hz` is off deliberately.** The receiver will accept the request and
then fail to deliver: at 9600 baud its nine sentences an epoch need about
2250 B/s, against a 960 B/s line. Making it real needs a faster port or fewer
sentence types, and the latter would break `gps-monitor`'s satellite page — so
it is not done behind your back (`DESIGN.md` §6.3).

---

## Cue alerts need one root action

At 500 m, 200 m and 50 m from a turn the keyboard LED flashes. It is the only
non-visual alert this hardware has — there is no buzzer — and
`/sys/firmware/beepy/led*` is root-write-only as shipped. Hand write access to
group `input`, which the `beepy` user is already in:

```sh
sudo install -m 644 install/99-beepy-led.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --subsystem-match=i2c --action=change
ls -l /sys/firmware/beepy/led*      # expect: --w--w---- root input
```

Skip it and beepy-nav degrades to visual-only: the panel still counts the turn
down, nothing errors, and one line on stderr at startup says why the LED is
quiet.

---

## The ride log

On a real port every ride is recorded — the raw NMEA byte stream exactly as
the receiver sent it, and the per-fix trace beside it:

```
~/rides/YYYYMMDD-HHMMSS.nmea
~/rides/YYYYMMDD-HHMMSS.tsv
```

About **1.9 MB an hour**, measured. `--no-log` turns it off. A ride will not
start a new log below ~50 MB free and says so. Nothing here can take the
navigator down: an unwritable directory or a full disk is one line on stderr
and the ride goes on. Nothing prunes `~/rides` either — that is still your job.

This is **not ride recording**: there is no GPX writer and no trip statistics.
It exists so a failure on a road can be replayed on a desk, which is what
turns a bad ride into a permanent test:

```sh
scp beepy.local:rides/20260730-113748.nmea .
tools/ride2fixture.sh 20260730-113748.nmea ROUTE.gpx my-failure
```

That replays the log, checks the replay against what the device itself
computed from the same bytes, and prints the lines to paste into the test
suite.

---

## Install

```sh
make sync                                        # build on the device
ssh beepy.local
sudo install -m 755 beepy-src/beepy-nav/beepy-nav /usr/local/bin/
mkdir -p ~/routes ~/rides
```

If the panel is ever left showing a frozen frame — a dropped connection at the
wrong moment, or a `kill -9` — the console comes back with:

```sh
kill -CONT $(pgrep -x fbterm)
```

---

## Development

```sh
make host           # portable objects + a replay binary, on a Mac
make sync           # rsync to beepy.local, build there, run `make check`
make design-gate    # the C pages against mockup.py's frames (needs Pillow)
make host-replay    # the replay assertions, on a Mac
make test-frames NAV=host/beepy-nav
```

`make check` is the gate: it byte-compares the `--demo` page dumps against
`goldens/` and runs the unit and replay suites. `mockup.py` is the pixel
reference — the C renderer is held to it to the pixel, which is what stops the
display drifting from the design one small change at a time.

Flags that exist for testing: `--replay F`, `--headless`, `--trace F`,
`--trace-frames F`, `--stats`, `--fps N`, `--dump-at S:F`, `--key S:C`,
`--print`, `--pace` / `--no-pace`, `--demo --page P --dump F`, `--bench N`.

---

## Attribution

Any basemap data is © OpenStreetMap contributors (ODbL).

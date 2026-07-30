# beepy-nav

A GPS navigator for the Beepy: see where you are, follow a GPX route, show the
next turn. `DESIGN.md` is the specification and the reasoning behind every choice
below; this file is how to get it running and ride with it.

```
MAP        where you are, with no route loaded -- what it opens on
NAV        turn arrow, distance to the junction, and a live map
OVERVIEW   the whole route, fitted, with progress
FIND       type a destination; CONFIRM shows the route before you commit
```

---

## Quick start

Everything below assumes the program is installed (see **Install**) and a GPS
receiver is on `/dev/ttyACM0`.

```sh
# 1. run it with no arguments: it opens on the MAP page and shows you where
#    you are, with the streets under you if a basemap is configured
beepy-nav

# 2. from there:  R  picks a route from ~/routes
#                 F  types a destination and routes to it
#                 E  ends a route without leaving the program
#                 Q  quits (it asks first)

# ...or go straight to a route you already have
scp my-ride.gpx beepy.local:routes/
beepy-nav --route ~/routes/my-ride.gpx

# ...or with the packs named on the command line rather than in the config
beepy-nav --basemap ~/packs/home.tiles --roads ~/packs/home.roads
```

That is the whole workflow. The panel takes over the screen while it runs and
gives it back when you press `Q` and confirm.

Before the first fix the MAP page says `WAITING FOR FIX` and how many satellites
the receiver can see. A cold receiver indoors can sit there for minutes; near a
window it is usually seconds.

### Where a GPX comes from

Any route planner exports one. **Komoot**, **RideWithGPS** and **Strava** all
have a *GPX export* in their route menus — all three formats are read, with or
without turn cues. If a route has no cues in it (a bare `<trk>`, which is what
most shared rides are), beepy-nav derives them from the shape of the road: it
resamples the line and calls anything past 30° a turn, classified slight /
turn / sharp / U-turn (`DESIGN.md` §7.4).

A GPX is not the only way in any more: with a road pack loaded, `F` searches
street names *and places* — schools, stations, markets — and routes to one on
the device. See **Finding a destination**.

---

## Reading the screen

### The MAP page — what you get with no route

```
   ┌────────────────────────────────────────┐
   │ (N)                            ( 24 )  │   compass · speed
   │                                 KM/H   │
   │                                        │
   │                  ▲                     │   you, at the centre
   │                  └┄┄┄┄┄┄               │   ┄ where you have been
   │                                        │
   │ ├─500M─┤                               │
   ├────────────────────────────────────────┤
   │ 13.88510 100.37850                     │   where you are, in degrees
   │ F FIND   R ROUTES   Q QUIT             │
   └────────────────────────────────────────┘
```

The full width, because with no route there is no next turn and nothing to put a
panel there for. Same map as the NAV page — course-up, `O` for north-up, `Z`/`X`
to zoom, `A` back to the default 6 m/px — with two differences:

- **you sit at the centre**, not low down. The NAV page keeps you low so two
  thirds of the map is the road ahead; with no route there is no ahead.
- **the dashed line is the whole session**, not the part of a route you have
  covered. It survives changing route, and when it fills up (a few thousand
  points, about 10 km at its 5 m spacing) it thins itself rather than forgetting
  where you started.

If the fix is lost after you have had one, the last known position stays on
screen with an inverted `NO FIX` beside the coordinates — the same rule the NAV
panel follows, in the one row this page has for it.

`Tab` does nothing here: there is no OVERVIEW without a route. The hint row is
the page's whole keymap, so if a key is not on that line it does not do anything.

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
| `Tab` | switch page: NAV ↔ OVERVIEW (nothing on MAP — there is no route to overview) |
| `F` | find a destination — see below; says `NO ROAD PACK` if there is none |
| `R` | pick a route — from MAP, or to change route mid-ride |
| `Z` / `X` | zoom the map out / in (switches to manual zoom) |
| `A` | back to automatic zoom (on MAP: back to the 6 m/px default) |
| `O` | course-up ↔ north-up |
| `U` | metric ↔ imperial |
| `L` | cue alerts on ↔ off, for this ride only |
| `H` | hold — freeze the display |
| `E` | end the route — stop navigating, stay on the map, keep riding |
| `Q` | quit — it asks first, and shows what you would be ending |

### Stopping

There are two different things you can mean by "stop", so there are two keys.

**`E` ends the route.** You arrived, or you have had enough of it, and you want
to ride home without a navigator nagging you. The route goes away, the MAP page
comes back with your position still live, and the breadcrumb, the odometer and
the program all carry on. The ride log for that route is closed and saved.

**`Q` quits — and asks first.** It opens a page telling you what you would be
ending: how far you have ridden, how long, and whether the ride log is already
on the card. `Enter` goes through with it; `Q` again backs out and gives you
the exact screen you left. If you are on a route it also offers `E` there,
because "stop the ride" is more often what someone reaching for `Q` meant.

The confirmation exists because `Q` used to exit on one press, and on a bike
that is a ride lost to a thumb landing one key over.

`R` only does something when there is a picker to go back to — that is, when you
started with a bare `beepy-nav` rather than `--route`. Quitting the picker with
`Q` comes back to MAP rather than out of the program, so opening the list and
changing your mind costs nothing. Loading a route closes the current ride log and
opens a new one, because a different route is a different ride.

Letters and not digits, because the Beepy's digit row needs the Alt modifier
and that is unusable one-handed on a handlebar.

`L` confirms itself for a second and a half (`ALERTS ON` / `ALERTS OFF`) —
without that, muting is indistinguishable from a quiet stretch of route.

On the **FIND** page every letter is a letter, so none of the keys above apply
there:

| Key | Action |
|---|---|
| A–Z, 0–9, space | type into the query |
| `Backspace` | delete a character — and on an empty query, back out |
| `↓` / `↑` | move the selected match |
| `Enter` | route to it, and show CONFIRM |
| `Esc` | back out |

and on **CONFIRM** there are two, both printed on the screen: `Enter` to go and
`Q` to cancel back to FIND.

Digits *do* need the Beepy's Alt layer, which is the one place this program asks
for it — `SOI 23` is not typeable without them.

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
basemap    =            # empty: no streets    (--basemap F / --no-basemap)
roads      =            # empty: F does nothing (--roads F / --no-roads)
```

An unknown key or a malformed line is a warning naming the line number, never
a failure to start.

**`basemap` is empty by default and there is no default to guess.** A pack is
cut for one route's corridor, so nothing can invent one for you; see *Streets
under the route* below. If the file is missing or is not a pack you get one
line on stderr and the map draws the way it always did.

**`roads` is empty by default too**, for the same reason and with the same
behaviour: one line on stderr if it cannot be read, and then `F` says
`NO ROAD PACK` on the panel rather than doing nothing at all.

**`rate_5hz` is off deliberately.** The receiver will accept the request and
then fail to deliver: at 9600 baud its nine sentences an epoch need about
2250 B/s, against a 960 B/s line. Making it real needs a faster port or fewer
sentence types, and the latter would break `gps-monitor`'s satellite page — so
it is not done behind your back (`DESIGN.md` §6.3).

---

## Streets under the map

Optional, off by default, and it needs one command on a Mac. The device carries
no map data and no renderer for it — what it reads is a **tile pack**: 1-bit
256×256 tiles of OpenStreetMap street geometry, pre-rendered per zoom rung,
cut to a corridor along one route.

A pack works on the MAP page too, with no route in sight: put one in the config
(`basemap = /home/beepy/packs/home.tiles`) and a bare `beepy-nav` shows you the
streets around where you are standing — the pack's own reference point becomes
the world origin, so it lines up with a position it was never told about.

`mktiles.py` cuts three ways: `--route ride.gpx` for a corridor along a ride,
`--ref LAT,LON --radius M` for a disc around a place, and `--bbox` for a
rectangle. Only the selection differs; everything after it is the same.

```sh
# 1. get an extract from Overpass -- roads within a few km of the route
#    (overpass-turbo.eu; ask for `way[highway]` in a bbox, "geometry" output)

# 2. cut it to the route's corridor and render the tiles
python3 tools/mktiles.py --osm extract.json --route ride.gpx -o ride.tiles
python3 tools/mktiles.py --info ride.tiles      # what came out

# 3. put it on the device and point the navigator at it
scp ride.tiles beepy@beepy.local:~/maps/
beepy-nav --route ~/routes/ride.gpx --basemap ~/maps/ride.tiles
```

`--corridor M` sets the half-width (2 km by default). The zoom rungs follow
from it — a wider corridor gets coarser rungs, because a basemap that runs out
halfway across the screen looks broken; `--zooms 4,6,10` or `--zooms all`
overrides. A ±2 km corridor around a 4 km route is about **1.1 MB**, and a rung
the pack does not carry simply draws no streets, exactly as no pack does.

#### Updating the maps

OpenStreetMap changes daily and the packs are a snapshot, so refreshing them is
one command on the Mac:

```sh
tools/mkmaps.sh                 # download, rebuild everything, install
tools/mkmaps.sh --no-download   # reuse the .osm.pbf already downloaded
tools/mkmaps.sh --no-install    # build only, touch nothing on the device
```

About 25 minutes, and nearly all of it is the 310 MB download — the rendering
is seconds. It writes to `maps/`, verifies each pack's digest after the copy,
and only then moves it into place, so an interrupted transfer never leaves the
navigator pointed at half a file. **A running navigator holds its packs open;
restart it to see new data.**

The area it builds is set by four values at the top of the script —
`HOME_LL`, `HOME_RADIUS`, `COUNTRY`, and the two zoom lists. Change those to
move it somewhere else. They are in one place on purpose: the fine and coarse
builds must agree on a projection frame or the merge below refuses them, and
the script derives that frame from `COUNTRY` rather than trusting anyone to
retype it.

What comes out, and what each is for:

| file | built from | what it does |
|---|---|---|
| `thailand-nav.tiles` | both extracts | the streets drawn under the map, everywhere |
| `region.roads` | the region extract only | what `F` searches and what routes are computed over |

Note the asymmetry: the **basemap is nationwide, the routing is not**. You can
see roads 400 km away and cannot route to them, because the graph is cut from
the region extract. Widening `HOME_PAD` widens what is searchable, at about
10 MB of pack per 1 000 km² — measured on Greater Bangkok, so take it as an
upper bound; open country is far cheaper.

#### Detailed at home, coarse across a country — one pack

Only one basemap can be loaded at a time, so "every street near home" and
"major roads countrywide" have to end up in the same file. They cannot come
from the same build: every street within 20 km of home is 17,000 tiles at the
close rungs, and the same detail across Thailand would be about half a million.

So: build them separately and join them. Each zoom rung in a pack is its own
grid with its own extent, so one pack happily holds a 1.5 m/px rung covering a
city and a 60 m/px rung covering a country.

```sh
# a nationwide extract cannot come from Overpass -- start from a Geofabrik
# .osm.pbf and convert it to the JSON the tools read
python3 tools/pbf2osm.py thailand.osm.pbf --bbox 5.5,97.3,20.6,105.7 \
        --classes coarse --no-names -o th-coarse.json

# coarse: major roads, whole country, from 15 m/px out. The frame falls out
# of the bbox centre -- note it, because the fine build has to match it.
python3 tools/mktiles.py --osm th-coarse.json --bbox 5.5,97.3,20.6,105.7 \
        --zooms 15,25,40,60,100,150,250 -o coarse.tiles

# fine: every road within 20 km of home, up to 10 m/px -- but placed in the
# coarse build's frame, which is what --frame is for
python3 tools/mktiles.py --osm region.json --ref 13.8851,100.3785 --radius 20000 \
        --frame 13.05,101.50 --zooms 1.5,2.5,4,6,10 -o fine.tiles

python3 tools/mergetiles.py fine.tiles coarse.tiles -o thailand-nav.tiles
```

**Both inputs must share a projection reference.** Tiles are addressed relative
to the pack's own reference point, so packs cut against different ones describe
different ground; `mergetiles.py` refuses rather than produce a plausible wrong
pack, and `mktiles --frame` is how you avoid that. A zoom rung present in both
inputs is taken from the first, and it says so — which is why the two builds
above are given disjoint rungs.

That pair comes to **364 MB** for all twelve rungs: full street detail across
Greater Bangkok, and every motorway, trunk, primary and secondary road in
Thailand when you zoom out. Ordinary, and it fits many times over on the SD
card.

Streets go **under** everything: basemap, then the ridden track, then the cased
route, then the markers. That white casing round the route is doing its job
here — it is what keeps the black line readable crossing a dual carriageway.

Cost, measured: about 7 ms a frame, taking the render p95 from 11.5 ms to
19.1 ms against a 125 ms frame period.

---

## Finding a destination

`F` opens FIND: type, and names filter as you go, nearest first with a
distance and a bearing. `Enter` routes to the selected one on the device —
Dijkstra over the pack's road graph, `oneway` respected — and shows CONFIRM: the
proposed route drawn whole, with its length, an estimate and the number of
turns, against `ENTER = GO` and `Q = CANCEL`. Go, and it becomes an ordinary
ride: the same NAV page, the same cues, the same ride log. A route you searched
for and a GPX you downloaded are the same thing from that point on.

**What you can search for:** street names *and* destinations — schools,
stations, markets, shops, hospitals, parks, temples. Anything OpenStreetMap
tags as a place and gives a name to. Around Bangkok that is 11,213 streets and
25,318 destinations, 29,546 distinct names in all.

Three things about it that will save you a puzzled minute:

- **It is ASCII only.** The 5×7 font has no Thai glyphs, so a place named only
  in Thai cannot be shown and is not indexed. The pack counts them and says so
  (5,860 around Bangkok) rather than pretending to be complete. If you cannot
  find somewhere you know exists, this is usually why — try its English name.
- **Type less, not more.** Matching is token-AND on substrings, so every word
  you type must appear. `ASSUMPTION` finds the college; `ASSUMPTION COLLEGE
  THONBURI` finds it only if the pack holds that exact wording.
- **A hit is the middle of a place, not its gate.** A campus or a mall is
  reduced to its centre point, and the router then takes you to the nearest
  road to that centre. Expect to arrive at the perimeter.

It needs a **road pack**, built on a Mac from an OpenStreetMap extract. It is a
different pack from the basemap of *Streets under the map* — that one is
pictures of streets, this one is the graph and the names — and you can load
either, both, or neither.

```sh
# 1. an extract from Overpass: `way[highway]` in a bbox, "geometry" output.
#    A 15 km box over inner Bangkok is a 39 MB download.

# 2. build the pack. --route references it to that route's start; --ref LAT,LON
#    when there is no route to hand.
python3 tools/mkpack.py --osm extract.json --route ride.gpx -o city.roads
python3 tools/mkpack.py --info city.roads

# 3. put it on the device
scp city.roads beepy@beepy.local:~/maps/
beepy-nav --route ~/routes/ride.gpx --roads ~/maps/city.roads
```

Sizes, measured: the 557-way Asok test extract is **101 KB**; a 15 km box over
inner Bangkok — 25 484 roads, 104 815 graph nodes — is **3.8 MB**. Opening it
costs 32 ms once, a keystroke costs 7 ms, and a route across the whole city
takes 55 ms.

### What is searchable, and what is not

**Street names, and only the ones in the pack.** No POIs, no addresses, no
postcodes. The index is `name:en` where OSM has it, otherwise `name`.

**Names with no ASCII form are not indexed**, because the panel's font is
A–Z/0–9 and there is no Thai in it. The pack counts them and the screen admits
it: a query that finds nothing says `NOT IN THIS PACK` and how many names it
cannot show. For the Bangkok box that is 1 012 of 5 331 names — a quarter of the
city — so a soi signposted only in Thai is genuinely unreachable this way, and
you will be told so rather than left guessing.

**Still no rerouting once you are riding.** Going off route gets you
`OFF ROUTE / 85 / M AWAY` and a trail back to the line, exactly as before. The
router on the device makes rerouting *possible* for the first time; it is not
built, and nothing on the screen pretends it is. If you want a different route,
`F` and ask for one.

**`F` does not work from the route picker** — only from a ride. Load any GPX
first. That is a wart, not a design.

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
make test-tiles     # the basemap pack: built twice, compared; and merging
make test-roads     # the road packs: built twice, compared against git
make test-find NAV=host/beepy-nav   # F, a typed query, ENTER, CONFIRM, ENTER,
                    # a destination that is not on the graph, and QUIT / E
```

`tools/` is the Mac-side half of the project and none of it runs on the device:

| Tool | What it makes |
|---|---|
| `mkmaps.sh` | **all of it, in one command** — download, cut, render, merge, install |
| `pbf2osm.py` | a Geofabrik `.osm.pbf` → the Overpass JSON the pack tools read |
| `mktiles.py` | the raster basemap, by route corridor, radius or bounding box |
| `mergetiles.py` | joins tile packs that share a projection frame, so one file can be detailed at home and coarse across a country |
| `mkpack.py` | the road graph, street names and searchable places |
| `mknmea.py` | synthetic NMEA rides — the replay fixtures |
| `fbdiff.py`, `assert_trace.py`, `design_gate.py` | the gates |

`make check` is the gate: it byte-compares the `--demo` page dumps against
`goldens/` and runs the unit and replay suites. `mockup.py` is the pixel
reference — the C renderer is held to it to the pixel, which is what stops the
display drifting from the design one small change at a time.

Flags that exist for testing: `--replay F`, `--headless`, `--trace F`,
`--trace-frames F`, `--stats`, `--fps N`, `--dump-at S:F`, `--key S:C`,
`--print`, `--pace` / `--no-pace`, `--demo --page P --dump F`, `--bench N`.
`--key` also takes names for the presses that are not characters — `enter`,
`esc`, `bs`, `space`, `tab`, `down`, `up` — which is how the FIND page is driven
in a headless replay.

---

## Attribution

Basemap and road-pack data are **© OpenStreetMap contributors**, used under the
ODbL. The tiles are rendered from an OSM extract in this project's own
cartography, and the road graph, the street names and the searchable places are
derived from the same data;
nothing is cached from a commercial tile service, which is a licensing
constraint and not a technical one (`DESIGN.md` §6.5).

When a pack is loaded the navigator shows the attribution itself, on the
OVERVIEW page's title line — spelled `(C) OPENSTREETMAP CONTRIBUTORS`, because
the 5×7 panel font has no © glyph. If you distribute a pack you built, it
carries the same obligation.

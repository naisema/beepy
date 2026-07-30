# beepy-nav — design

A GPS route navigator for the Beepy — three pages, built on the parser and
framebuffer renderer already proven by `gps-monitor`.

Status: **implemented through DESIGN phase 6 and running on the device.**
`mockup.py` remains the pixel reference — `make design-gate` byte-compares the
C renderer against it, and both are frozen into `goldens/nav-*.fb`, which
`make check` verifies on the device.

```
MAP        where you are, with no route loaded -- what it opens on
NAV        turn arrow, distance to the junction, and a live map
OVERVIEW   the whole route, fitted, with progress
```

Plus one pre-ride flow — **FIND → CONFIRM** — that produces the route the two
ride pages then follow: type a destination on the Beepy's own keyboard, see the
routed overview, press ENTER to go. `F` reaches it from MAP, which is what makes
the whole flow one sitting: open, see where you are, search, go.

That is the entire program. It shows you where you are, and follows a route when
you have one. It is not a cycle computer: **no data-field pages, no elevation
profile, no menu page, no ride recording, no satellite page.** An earlier draft
had all of
those and they are gone — see §14 for what was removed and what that bought.
(One thing came back, deliberately and in a much smaller form: §7.6's raw NMEA
log, which records the receiver rather than the ride and exists so that a
failure on a road can be replayed on a desk.)

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
information. §1.1.2 is the whole of that rule.

### 1.1.2 NO FIX — when it fires, and what stops moving

A navigator that loses its fix and goes on drawing the same confident panel is
not merely unhelpful, it is *lying*, and it is lying at the exact moment the
rider most needs to know to look up. This is the one failure on this device
whose symptom is indistinguishable from working correctly: a frozen panel and a
panel with nothing to say look the same.

**Four seconds.** The receiver is measured at 1 Hz (§3), so:

| Missed epochs | Gap between good fixes | Verdict |
|---|---|---|
| 1 | 2 s | not news — one dropped sentence |
| 2 | 3 s | still not news |
| 3 | 4 s | **`NO FIX`** |

The rule is stated in seconds, not in missed epochs, so one constant covers
1 Hz, the optional 5 Hz of §6.3, and a receiver that stops talking altogether
rather than merely voiding its sentences. Below three seconds the warning
flickers on ordinary jitter, and a warning that cries wolf is worse than none —
it spends the trust the row exists to earn. Above five it is silent for longer
than the dead reckoning it is covering for. Four sits between the two bounds
with a second of margin either side.

**Inverted, and inverted for a reason.** A paper bar with ink text, in the
arrival row's own 16 px cell. The panel is solid ink, so paper is the one
treatment available here that cannot be read as ordinary content (§4: two pixel
words, no shades) — a row of paper text saying `NO FIX` would look exactly like
the ETA it replaced. There is no spare line to put it on: §1.1's three rows
already fill the panel to the bottom edge, and arrival is the row this panel can
most afford to lose, by the same argument §7.5 makes for its transient. It
**outranks** that transient: `ALERTS OFF` is a preference the rider expressed
two seconds ago; `NO FIX` is a fault they do not yet know about.

**What stale means.** Everything on the screen derived from position stops
pretending, and each of the three does it by a mechanism that already existed:

| Quantity | While the fix is gone | Because |
|---|---|---|
| Position marker | frozen after 2 s (`DR_MAX_EXTRAP`) | §6.3's extrapolation is honest for a second or two; past that a marker sliding on down the road is an invention. It is frozen a full second *before* the row appears |
| Countdown | frozen at its last latched value | §8 recomputes `nav_t` per **fix**, and there are none. The panel never counts down from dead reckoning |
| Map, heading, speed badge | hold their last value | The map is drawn about the frozen marker, and the EWMA converges to the last course and stops |

**One signal, not five.** No hollowed marker, no dashed route, no blanked
speed. Line style is already fully committed on this screen (§4: route vs
track, ahead vs ridden) and a second staleness encoding would be a second thing
to learn under load — and would collide with §7.3's off-route treatment of the
same marker. The inverted row is the qualifier: while it is up, *every* number
on the panel is the last one known, and that is a single fact to hold rather
than five.

**Recovery re-snaps rather than resumes.** When fixes return the row goes on the
fix that ends the gap, and the countdown latch is **re-armed** (`nav_relatch`).
§1.1.1's latch is monotone non-increasing "while approaching a given cue", and a
gap breaks that premise outright: the world moved unobserved, the rider may have
turned round, and the frozen value is not a floor the re-measured distance may
only decrease from. So the first fix back seeds a new sequence from the
re-snapped position. Nothing else is thrown away — the off-route run length, the
ETA ring and the snap window hint are all still the best information available,
and discarding them would buy a full route scan and a re-earned ETA for nothing.

**The ride clock has to keep running.** Three rules in this document are
measured in ride seconds and every one of them was dead before this section:
the four seconds above, §6.3's 2 s extrapolation clamp, and §7.2's "after 30 s
lost, widen to a full scan". The clock used to advance only on epochs that
carried a position, so during exactly the gap these rules describe, it stopped.
It now advances once per **epoch**, fix or not — and where a cold receiver emits
`GGA` with an empty time field, it counts epochs instead, which at 1 Hz is the
same clock.

**The OVERVIEW page is not covered.** Its strip carries position-derived
numbers too (`% DONE`, `TO GO`, `ETA`), and they go equally stale. It is left
alone deliberately for now: it is a page a rider switches to on purpose to see
the whole route, not the instrument they read at a junction, and giving it a
second inverted treatment is a design decision its own strip layout has not been
made for. This is a known gap, not an oversight.

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
| Selected match | inverted row; `↓`/`↑` move, ENTER routes |

The search index is the street names already in the offline pack (`name:en`
falling back to `name`), matched token-AND, ranked by distance from the current
fix. POIs and addresses can join the index when packed — they are not, as of M6,
and street names are all there is; **nothing is searchable that is not in the
pack**, and the hit count says so honestly (§1.4.2).

**Routing is on-device Dijkstra over the pack's road graph** — ways joined on
exact shared coordinates. Measured on the Asok extract: 2,803 nodes, and the
824 m result matched the hand-built demo route within 3.4 m — **but that route
is illegal, and §1.4.3 is the correction.** Even at Pi Zero speeds a corridor
pack routes instantly; a whole-city pack (~10⁵–10⁶ nodes) is
Dijkstra-in-tens-of-ms, still fine without any A*/contraction cleverness.
Beyond pack coverage the fallback would be straight-line bearing navigation,
labelled as such — that part is **still not built** (§1.4.6).

CONFIRM is the OVERVIEW page's cartography fitted to the *proposed* route —
start marker, cue dots, destination flag — with the strip carrying the one
decision: `827M · EST 3 MIN · 2 TURNS` against `ENTER = GO / Q = CANCEL`. ETA
assumes 17 km/h city riding until there is ride history to do better.

A GPX file remains the other entry: FIND builds a route, a GPX *is* one, and
everything downstream is identical. `router_to()` leaves a `route_t` that
`route_prepare()` and `route_cues_derive()` have already been over, and
`main()`'s route loop cannot tell it from a file — which is why nothing in §7
had to change.

#### 1.4.1 The pack — `BNAVROAD`

`tools/mkpack.py` cuts the graph and the name index on the Mac;
`beepy-nav/src/search.c` reads them on the device. The header deliberately
reuses §6.5's shape — magic, `u16` version, `u16 header_bytes`, then `lat0`,
`lon0` and the two metres-per-degree constants at the same offsets — because
two packs read by the same program should not differ in the eight fields they
have in common. Little-endian, fixed-width, no text.

A 64-byte header, then a six-entry section table of `(offset, count)`, then the
sections in that fixed order:

| Section | Record | Field |
|---|---|---|
| NODES | 8 B | `i32` lat×10⁷, `i32` lon×10⁷ |
| ADJ | 4 B | `u32` first edge of node *i*; node *i*'s edges are `ADJ[i]..ADJ[i+1]-1` |
| EDGES | 12 B | `u32` to, `u32` length in **millimetres**, `u16` flags, `u16` name |
| PLACES | 12 B | `u32` name offset, `u32` first point, `u32` point count |
| POINTS | 8 B | as NODES — the coordinates a name is searchable at |
| STRINGS | 1 B | NUL-terminated uppercase ASCII |

The complete field table lives at the top of `mkpack.py`, which is the only
thing that writes it. Five decisions in there are not obvious:

- **CSR, not an edge list.** The edge table is already sorted by source node
  and the source index is implied by `ADJ`, so a node expansion is one
  indirection rather than a search. Dijkstra is the only consumer and this is
  the shape it wants.
- **Directed, with a flag.** A `oneway` way contributes one edge per segment
  and no reverse; everything else contributes two. `ROADEDGE_ONEWAY` marks the
  survivors, so a test can tell "the pack honours `oneway`" from "the pack
  happens not to contain that edge" — which is exactly what T-ONEWAY needs.
- **Lengths are integer millimetres.** They are the numbers Dijkstra adds up,
  they are measured once on the Mac in the pack's own frame, and an integer is
  the only way "the same extract gives the same route" survives two compilers.
  `u32` mm reaches 4 295 km.
- **Every third vertex, not one representative point.** A name's distance is
  the distance to its *nearest* candidate, because ranking is by distance from
  the rider and the near end of a 2 km road is not a pack-time decision.
  `mockup.py` samples `[::3]`; the device does the same arithmetic over the
  same points, which is why `nav-search.png` reproduces byte for byte.
- **No route binding.** Unlike a tile pack (`tiles_bind_route()`), this one
  keeps its own tangent frame. There is no route when FIND runs — building one
  is what FIND is *for* — so everything crossing the boundary is lat/lon:
  `roads_project()` takes the fix in, `roads_unproject()` hands the path back.

Determinism is a requirement, not a nicety: `make test-roads` builds each
committed pack twice and compares against the copy in git, because a golden
hangs off one of them and T-ONEWAY's three hand-picked **node indices** are only
meaningful while the numbering is stable.

Asok (`osm-asok.json`, 557 ways): 2 803 nodes, 4 919 directed edges, 117
searchable names, 883 candidate points, **101 KB**. A 15 km box over inner
Bangkok (25 484 routable ways): 104 815 nodes, 192 085 edges, 4 319 names,
**3.8 MB**.

#### 1.4.2 Non-ASCII names, and being honest about them

The 5×7 font is A–Z/0–9 — there is no glyph for U+0E0B, and adding a Thai face
to a 400×240 1-bit panel is not a font problem, it is a shaping problem. So a
name is indexed only if it has an ASCII form (`name:en`, else `name` when that
is ASCII), and **the ones dropped are counted into the header**. The Asok
extract drops **3** (`ซอยทวีสุข`, `ซอยพร้อมจิต แยก 2`, `ซอยสุขุมวิท 49/16`);
the Bangkok box drops **1 012** — a quarter of its names, which is the honest
scale of the problem in this city.

That count is not a diagnostic, it is on the screen: a query with no match
prints `NOT IN THIS PACK` and, when the pack dropped anything, how many names it
cannot show. A rider who searches for a soi that is only signposted in Thai
gets told the pack cannot see it, rather than concluding it does not exist.

#### 1.4.3 `oneway`, and the 824 m that was a wrong-way route

The previous draft of this section listed "the mockup router ignores `oneway`
(production must not)" as an honest limit. Honouring it turned out to invalidate
this section's own measurement, and that is worth writing down rather than
quietly fixing:

- **The 824 m reference route rides 22 hops the wrong way up Ratchadaphisek
  Road**, which is `oneway=yes` southbound. The shortest *legal* route between
  the same two points is **864.5 m**. Both numbers are pinned in
  `tests/test_search.c`: the 824 m against a pack built with `--ignore-oneway`,
  which is the mockup's behaviour frozen, and the 864.5 m against the real one.
- **Nearest-node snapping then refuses to route at all.** The nearest node to
  the route's start is on the southbound carriageway; from there only 11 of
  2 803 nodes are reachable. The northbound carriageway is 20 m away.

So the endpoints attach to **every node within 25 m**, nearest first, capped at
32, and Dijkstra runs multi-source with the walk-on leg as each seed's initial
cost. With one node in range that *is* a nearest-node snap; 25 m is the distance
§7.3 already calls being on the road (`ROUTE_OFF_CLEAR_M`). The access legs are
real geometry — the route starts where the rider is, not at a graph node they
can already see — and they count toward the distance CONFIRM shows.

**T-ONEWAY** is 200 seeded random pairs plus those three hand-picked ones, with
every hop of every result checked against the pack's own adjacency: zero
violations, 142 of the 200 routable inside a corridor extract. The three
hand-picked pairs are each checked **both ways** — illegal on the
`--ignore-oneway` pack, legal on the real one — because a test that cannot fail
is worthless.

#### 1.4.4 Keys, and two places the table above was wrong

`F` opens FIND from either ride page. With no pack it says `NO ROAD PACK` on the
panel's transient row (§7.5's mechanism) and does nothing else: a dead key is
indistinguishable from a broken program.

It is the one transient that does not fit the panel at scale 2 — 144 px against
128 — so the row draws it at scale 1. `panel_row()` picks the largest scale that
fits rather than trusting the string, because a centred string wider than its
box does not truncate: it starts at a negative x and spills into the map on both
sides. That spill was real and shipped, and it was invisible for as long as
every test fixture had an empty map underneath it (`T-NOTE-FITS` is the
assertion that no longer allows that).

FIND is a **page**, not a modal sub-loop. The frame clock keeps running behind
it, `--key` drives it in a headless replay exactly as it drives `L`, and a
cancel gives the screen back frame-for-frame (`T-FIND-CANCEL`). That is what
makes any of it testable.

Two corrections to the table at the top of this section:

- **`N`/`P` cannot move the selection.** Every letter has to reach the query, so
  the arrows do it. `Esc` backs out, and so does `Backspace` on an empty query —
  because `Esc` needs the Beepy's symbol layer and a rider must never reach a
  page they cannot leave with the keys under their thumbs.
- **The hit count is the total, not the length of the list.** `search_places()`
  returns the two separately. The mockup's own count stopped at its limit, which
  would have read "5 HITS" for a query with ninety.

Digits need the symbol layer (§2), which is why the keymap accepts `KEY_0`…`KEY_9`
from evdev: `SOI 23` is the query in the screenshot and it is not typeable
without them.

The 24 px query is a **generated glyph table** (`tools/gen_query.py` →
`src/query24.h`, 40 glyphs at cap 24, 2.4 KB) rather than a live TrueType
render, by §5.2's argument and for a harder reason: the device has no
rasterizer. `mockup.py` blits the same table, so the page is byte-comparable and
is in the design gate. `nav-search.png` moved by 163 pixels when that landed —
integer tracking, and a block cursor that now sits at the pen rather than at the
ink edge (typing a space used to walk it backwards over the previous letter).

#### 1.4.5 Cost, measured on the device

| | Asok, 2 803 nodes | Bangkok, 104 815 nodes |
|---|---|---|
| open the pack | 1.1 ms | 32 ms |
| one keystroke → frame (search + draw + resolve) | 4.2 ms | **7.2 ms** |
| `search_places("SOI 23")` alone | 0.03 ms | 0.99 ms |
| one route, random pair | 1.1 ms | 55 ms |

A keystroke costs 7 ms at city scale against a 50 ms budget — a
type-to-filter field that lags a thumb is worse than a menu, and this one does
not. The 55 ms route is a one-off on ENTER, not a per-frame cost, and it is
still inside one 125 ms frame of §6.3; it is also a pathological pair, opposite
corners of a 15 km box. §1.4's "tens of ms" prediction was close.

#### 1.4.6 What is still missing

- **No rerouting.** Going off route still means §7.3's warning and nothing else.
  A router on the device makes rerouting possible for the first time; it is not
  built, and the panel does not pretend otherwise.
- **No straight-line fallback beyond pack coverage.** A destination outside the
  pack cannot be searched at all, so the case never arises — which is a
  restriction, not a solution.
- **No POIs and no addresses.** Street names only.
- **`F` does not work from the route picker**, which §2 calls "a different
  program state and not a page". This *was* the wart that forced a rider to load
  some GPX before they could search. §1.5 removes the force without removing the
  wart: the program now opens on MAP, which is a page, and `F` works there — so
  the picker is somewhere a rider goes on purpose rather than somewhere they are
  put on the way in. The key is still not bound inside it.

### 1.5 MAP — where you are, with no route — `nav-map.png`

**The product owner's words: "I want current location in map screen after open
beepy-nav program."** Until this page, the only way to see anything at all was
to pick a GPX first — so the very first thing anyone does with a GPS, look at
where they are, was the one thing this program could not do. That is not a
missing feature, it is a missing front door.

Full width, x 0–399. **There is no turn panel**, because with no route there is
no next turn: an inverted third of the screen holding nothing would be worse
than none, and the map wants the pixels.

| Element | Drawn as |
|---|---|
| Everything cartographic | exactly §1.1's marks — position, compass, speed badge, scale bar, dashed track, tile blit. Not a second copy: `mark_*` is one set of routines, and this page calls them |
| Position | at **(map centre, 0.5 × the map's height)** |
| Breadcrumb | the track travelled this session, 1 px dashed |
| Bottom strip, inverted, 42 px | scale 2: the position as decimal degrees (`13.88510 100.37850`), then `F FIND   R ROUTES   Q QUIT` |
| Before the first fix | no map at all: a centred `WAITING FOR FIX` at scale 3, the satellite count under it, and a 24 px strip carrying the hint row alone |

**Centred, not 0.72.** §1.1 puts the fix low on purpose — two thirds of the map
is then road *ahead* of the route. With no route there is no ahead, and a
where-am-I screen wants equal ground on every side of the marker. It is the
centre of the *map*, which ends where the strip starts (y 0–197), and not
0.5 × 240: the strip is opaque, so half of a height that includes it would put
the marker below the middle of everything the rider can see.

**Course-up by default, `O` toggles**, and below §6.1's 3 km/h the heading holds
— the same rule for the same reason, because the reason (a stationary receiver's
reported course is noise) has nothing to do with routes. `Z`/`X`/`A` and `U`
work too. What auto-zoom cannot do here is *fit the next cue*, since there is no
cue: the ladder gets a **default rung of 6 m/px** instead, which shows the
2.4 × 1.2 km around you, and `A` returns to it.

**The world frame is a pack's own reference.** Everywhere else the origin is the
route's first point, and `tiles_bind_route()` translates a tile pack into that
frame (§6.5). With no route there is no first point, so the order is: the
basemap pack's own reference — which makes that translation exactly zero and
the streets land where they were rendered — else the road pack's, so the map and
`F`'s distances are measured from one origin (§1.4.1), else the first fix, when
there is neither pack. This is the case `tiles_bind_route()` was written for and
had never been given.

**The breadcrumb is the session's, not the route's.** §1.1's dashed track is the
part of a route already covered and dies with the route; this is where the rider
has actually been, and it survives `R` and a routed destination. It is kept in
**lat/lon**, not in world metres, because the frame changes the moment a route
loads and a breadcrumb in metres would silently bend at every route change —
the same argument §1.4.1 makes for the road pack keeping its own frame.
Points nearer than **5 m** to the last are dropped, because a receiver at a
standstill jitters by metres and ten minutes at a traffic light would otherwise
fill the buffer with a scribble the size of the marker. **When it fills at 2 048
points, every second point is dropped and recording carries on:** the whole
session survives at coarsening resolution rather than its beginning being
forgotten, which is the right trade for a mark whose job is "roughly where have
I been". At the 5 m floor the first halving is 10 km of riding away. It is not
corner-rounded, which §1.1's track is — a GPX is a chain of survey chords and
reads as a polygon drawn raw, while a GPS trace is already a dense wander and
rounding it would invent bends the rider did not take.

**Before the first fix, no map.** There is nothing to centre one on, and a map
drawn about a guessed position is the one thing a navigator must never put on a
screen. So: no marker, no compass, no scale bar, no streets — a centred
`WAITING FOR FIX`, and the satellite count beneath it *only when the receiver is
talking at all* (a count of zero is news; no count is different news). The
message does not move when the count appears, because a line that jumps as the
receiver warms up reads as a glitch rather than as progress. The strip loses its
first row and keeps the hint at the same y (H − 19) — **the strip grows a row
when there is something to put in it**, so the one line a rider navigates by
never moves.

**A fix LOST after having had one is §1.1.2's case, not this one.** The last
known position stays drawn — it is still the last thing known, and the marker
froze two seconds ago at `DR_MAX_EXTRAP` — and an inverted `NO FIX` appears
*beside* the coordinates rather than over them. That is the one difference from
the turn panel's treatment: there, the row `NO FIX` takes had a competing value
in it; here the value it would displace is exactly the position the warning is
about, and covering it up would throw away the last thing worth reading.

**One frame is drawn before the first sentence arrives.** Every other frame is
owed to §6.3's clock, and that clock does not start until there is a fix to
extrapolate from — so with a receiver that is cold, unplugged, or half a second
from its first `GGA`, the panel went on showing whatever `fbterm` left on it. On
a `--route` ride that was survivable; here it is the entire first impression.

**`Tab` is not bound.** Its promise is "switch page (there are only two)", and
both of those are pages *of a ride* — OVERVIEW with no route would have to draw
a route it does not have. It is left unbound rather than made a no-op with a
message, and the strip is why: this page advertises its own keymap on the hint
row, so a key that is not on that line was never claimed. That is the honest
form of "does nothing", and the one case §2's "a dead key is indistinguishable
from a broken program" does not cover.

**`R` is a page change now, not an exit.** It opens the picker, and quitting the
picker comes *back* to MAP rather than dropping to a shell — a rider who opens
the list and changes their mind has not asked to leave. `Q` on MAP is how the
program ends, and the strip says so.

Verification is four goldens (`nav-map`, `nav-map-nofix`, `nav-map-wait`,
`nav-map-tiles`), the first three in the design gate against `mockup.py`'s own
frames, plus **T-MAP**: the same replay ride with no `--route` at all, asserting
that 5 505 frames render, that the breadcrumb grows past 300 points, that the
drawn position still matches §6.3's closed form, and that **nothing in the trace
is NaN** — which needed a new assertion, because every other one in
`assert_trace.py` is a comparison and every comparison against NaN is false.

---

## 2. Keys

During a ride — NAV and OVERVIEW, and every one of them live in a `--replay`
session too, because a replay that cannot be paged or quit from the device is
not a rehearsal of anything:

| Key | Action |
|---|---|
| `Tab` | switch page (there are only two) |
| `F` | open FIND (§1.4); with no road pack it says so and nothing else |
| `R` | change route — back to the picker, without leaving the program |
| `Z` / `X` | zoom the NAV map out / in — one rung of §6.1's ladder, and switches it to manual |
| `A` | return the NAV map to auto zoom |
| `O` | course-up ↔ north-up |
| `U` | units: metric ↔ imperial |
| `L` | cue LED alerts on ↔ off, for this ride (§7.5) |
| `H` | hold — freeze the display |
| `Q` | quit |

On the MAP page of §1.5, which is what the program is when there is no route.
Its own hint row lists the three that matter, and that row is load-bearing: a
key absent from it was never claimed by the page.

| Key | Action |
|---|---|
| `F` | open FIND — the whole point of the flow: open, see where you are, search, go |
| `R` | open the route picker; `Q` there comes back to MAP, it does not exit |
| `Q` | quit |
| `O`, `Z` / `X`, `A`, `U`, `H`, `L` | as above; `A` returns to the 6 m/px default rung rather than to a cue fit |
| `Tab` | **not bound** — there is no OVERVIEW without a route (§1.5) |

In the startup route chooser, which is a different program state and not a
page:

| Key | Action |
|---|---|
| `N` / `P`, `↓` / `↑` | move the selection |
| `Enter` | load the route and start riding |
| `Q`, `Esc` | back to MAP without loading one |

A key repaints immediately rather than waiting for the next tick of §6.3's
frame clock: at the 1 Hz stopped rate that would be a whole second between the
press and the answer, which is how an instrument comes to feel broken. The
repaint is not a frame of the ride clock — it does not advance the dead
reckoning, and it is not counted — but it does go through the §6.4 skip, so a
key that changes nothing visible still costs no SPI.

`F`, and the two pages of §1.4 behind it, are built as of M6. FIND and CONFIRM
are *pages* inside the ride loop, which is what makes them reachable from a
`--replay` session and therefore testable; their own keys are in §1.4.4,
including the two places §1.4's first draft had them wrong.

Letters, not digits: the Beepy's digit row needs the Alt/symbol modifier, which
`gps-monitor` already found unusable one-handed. Keys come from `/dev/input/event0`
with `EVIOCGRAB`, because `fbterm` is SIGSTOPped while the panel is owned —
with `stdin` in raw mode as a second source, so the whole keymap can be driven
over ssh. That is not a debugging affordance to be removed later: it is the
only way any of this is testable without a physical thumb.

**There is no menu.** The route is a command-line argument:

```
beepy-nav --route ROUTE.gpx [-d DEV] [--north-up] [--imperial]
          [--replay F.nmea] [--config FILE] [--key SEC:CHAR]
          [--basemap PACK.tiles] [--roads PACK.roads]
```

`--key` presses a key at a given replay second, either as a character or by
name (`enter`, `esc`, `bs`, `space`, `tab`, `down`, `up` — the presses that are
not characters, and without which the FIND page could only be driven by a
physical thumb). It exists because the keymap
is otherwise reachable only by a physical thumb, and a keymap with no tests is
a keymap that quietly rots: what a key *does* is portable C, and only where
the press comes *from* is device-specific. `make test-frames` uses it to drive
`L` mid-ride and assert what happens.

Run with no `--route`, it **opens on the MAP page of §1.5** — where you are,
with nothing loaded. `R` from there lists `$BEEPY_ROUTES` or `~/routes`
(`routes_dir` in the config file) and waits for a selection: still a chooser and
not a page, but no longer the front door. That is the one change §1.5 makes to
the startup flow; `--route FILE` and `--replay` are untouched, and a routed
destination from FIND → CONFIRM reaches the ride pages by the identical path it
always did. Everything else lives in `~/.config/beepy-nav.conf`, which is read
once at startup.

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
| `rides_dir` | a path | `~/rides` | where the ride log goes (§7.6); `--no-log` turns it off entirely |
| `basemap` | a path | none | the OSM raster pack under the map (§6.5); `--no-basemap` defeats it |
| `roads` | a path | none | the road/name pack FIND searches (§1.4); `--no-roads` defeats it |

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
| **Receiver streams** | RMC, VTG, GGA, GSA, **GLL**, GSV×4 — **nine sentences an epoch**, at 1 Hz, plus a `$GPTXT` banner at power-on | §3.1 |
| Constellations seen | **GPS only** — `$GPGSV` only, 13 sats in view | §3.1 |
| `VTG` present | speed in km/h as a field; **course over ground is EMPTY at a standstill** | §3.1 |
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

### 3.1 The first live run — measured 2026-07-30, indoors

The table above was assembled from a `dd` capture before any of this program
existed. This section is the navigator itself, three minutes against the real
port at 8 Hz with `--stats`, and it corrects the table in three places.

**A fix was acquired, indoors, and held.** GGA quality `1` on all 200 epochs,
4–6 satellites used of 13 in view, HDOP 1.5–1.8. The expected outcome was no
fix at all; that is not what happened, and the NO FIX row of §1.1.2 therefore
did **not** appear on the live port. It was exercised on the same serial code
path instead — see the end of this section.

| Measured | Value |
|---|---|
| Fix cadence | **exactly 1.000 s**, 200 fixes, no jitter and no dropped epoch |
| Sentence mix per epoch | RMC, VTG, GGA, GSA, GLL ×1 and GSV ×4 = **9**, plus 7 `$GPTXT` at power-on |
| Malformed lines | **0** in 92 KB |
| Byte rate | 514 B/s — **1.9 MB an hour**, against the 60 KB an hour §7.6 first assumed |
| Render | **17.4 ms mean, 18.2 ms p95, 20.5 ms max** on the Pi Zero 2 W |
| Frame rate | ~1.4 fps, not 8 — the receiver reports under 3 km/h at a standstill, so §6.3's stopped rate applies. Working as designed, and the first evidence that it does |
| Frames skipped | 29–55% (§6.4), the rest being real multipath wander |
| Cross-track wander | 0.6–15.6 m of it, indoors, never latching off route |

Three of those are corrections rather than confirmations:

- **`GLL` and `TXT` are in the stream and the original table missed them.**
  Neither is parsed and neither needs to be, but they are a third of the bytes
  and they are what made the log-size estimate wrong by thirty times.
- **`VTG`'s and `RMC`'s course fields are empty while stationary** — all 200
  epochs. No synthetic fixture has ever produced that, because `mknmea.py`
  always writes a course. `fix.c` already holds the last known value rather
  than taking a NaN, and §6.1's freeze below 3 km/h means nothing was going to
  use it anyway, but the case was untested until now and is now a fixture.
- **17 ms a frame is over half of §6.4's budget.** The bus arithmetic there is
  fine — 25 ms of SPI, a 40 Hz ceiling — but the *drawing* costs 17 ms on this
  CPU, so 8 Hz is 14% of one core rather than the rounding error the section
  implies. It fits, with less room than it sounded like.

**`T-LIVE`** replays 200 s of that capture, committed as
`beepy-nav/tests/rides/live-ublox.nmea` — the first fixture in this repo that
was not manufactured.

**NO FIX on the live code path.** The serial branch is the one path a replay
never exercises (`live_ride_now`, `live_frames`, `live_poll_ms` are all behind
`NAV_DEVICE`), so `nofix.nmea` was fed through a pty into it on the device:
last fix at t=199, `NO FIX` up at **t=203** — four seconds exactly — held to
t=230, cleared by the fix at t=231, and `cue_q` constant at 20 throughout. The
panel's bottom row went from 1 668 ink pixels to 300. Identical to the replay
lane's behaviour, on the code the replay lane cannot reach.

**Console restore, with the port open.** `fbterm` was `S+` before, and `S+`
after `Q` and after `SIGINT`, with the ride log closed cleanly and its last
line terminated in both cases.

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
one-shot message at startup, gated behind a config flag (`rate_5hz`), and it is
a refinement rather than a dependency — dead reckoning is what actually buys
the smoothness.

**And it will not fit down the wire as the receiver is currently configured.**
§3's measured environment is a u-blox 7 at **9600 baud** emitting RMC, VTG,
GGA, GSA and GSV — about 450 bytes an epoch. 9600 8N1 carries 960 bytes a
second: one epoch fits with room to spare, five need ~2 250 B/s, more than
twice the line rate. The receiver will `ACK` the message and then be unable to
deliver whole epochs.

Making 5 Hz genuinely useful therefore needs one of two things first, and
neither is free:

- **`UBX-CFG-PRT` to 38400** — then everything fits, but the port speed is now
  receiver state that `gps-monitor` and any other reader must agree with, and
  it survives until the receiver loses power.
- **`UBX-CFG-MSG` to silence GSV and GSA** — 5 Hz of RMC+VTG+GGA is ~1 000 B/s,
  still marginal, and it would silently break `gps-monitor`'s sky page, whose
  entire subject is GSV.

So the flag ships off, the ACK is reported out loud rather than assumed, and
`--trace`'s fix cadence is what says whether the receiver actually complied. A
silent "5 Hz requested" that the receiver ignored would be worse than 1 Hz,
because the smoothness budget would then have been spent on an assumption.

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

**The pack format, first.** `tools/mktiles.py` cuts the corridor and renders
the tiles on the Mac; `beepy-nav/src/tile.c` reads them on the device. The
format is written out here and in that tool's own header because it is an
on-disk contract between two machines and two languages — and because the
second pack this device will want (roads *and names*, for search and routing)
should not gratuitously differ from it.

#### The pack

One file, little-endian, fixed-width, no text: a 64-byte header (magic
`BNAVTILE`, version, tile size, `lat0`/`lon0` and the two metres-per-degree
constants of §6.1, the corridor width), a 32-byte entry per zoom rung (`mpp`,
the tile-grid origin `tx0`/`ty0`, `nx`/`ny`, the offset of that rung's index),
then one `u32` index per rung — `nx*ny` file offsets, `0` meaning the tile is
blank and cost nothing — then the tiles: 256 rows of 32 bytes, MSB first, bit
set = ink. 8 KB each, as this section always claimed. The complete field table
lives at the top of `tools/mktiles.py`, which is the only thing that writes it.

The address of a world point is
`px = floor(e/mpp)`, `py = floor(-n/mpp)` — the tangent-plane metres of §6.1
divided by the rung, y down, so a tile's row 0 is its north edge. The pack is
referenced to the **route's first point**, which is the same rule
`route_load()` uses, so a pack built for a route and that route share a
coordinate frame exactly; a pack used with a *different* route is corrected by
translation (`tiles_bind_route()`), which costs under a centimetre per
kilometre in scale.

Determinism is a requirement, not a nicety: the pack is a committed binary
fixture and a frozen golden hangs off it, so `make test-tiles` builds it twice
and compares sha256 against the committed copy. Every coordinate is floored to
an integer pack pixel **once, globally**, before any tile is drawn — which is
also what makes the seams invisible, since Bresenham is translation-invariant
under the integer tile offsets and the two halves of a street crossing a
boundary meet exactly.

#### Which rungs, and why not all twelve

The ladder has twelve rungs; a 2 km corridor gets **five** — 1.5, 2.5, 4, 6,
10 m/px. The rule is not taste: the NAV map is 270 px wide, so at a rung
coarser than `2·corridor/270` the screen is wider than anything the pack
contains and the streets would stop partway across the display. A basemap that
runs out mid-screen reads as a rendering fault, not as a map. Auto-zoom only
goes past 10 m/px when the next cue is more than 1.4 km away, which is the case
where there is least to look at anyway, and a rung the pack does not carry
draws nothing — the same code path, and the same frame, as no pack at all.
`--zooms` overrides this for anyone who wants the coarse end.

#### Two areas, one pack

A corridor is what a *route* needs. What a rider needs before choosing a route
is different: the MAP page opens on where you are (§1.5), which may be nowhere
near any GPX. So `mktiles.py` also cuts by area — `--ref LAT,LON --radius M`
for a disc around home, `--bbox LAT0,LON0,LAT1,LON1` for a country — and the
selection is the only thing that changes; everything downstream is identical.

Those two wants pull in opposite directions. Every residential street within
20 km of home is 17,000 tiles at the fine rungs; the same classes across
Thailand at 1.5 m/px would be roughly half a million, which is not a basemap,
it is a mistake. But major roads across Thailand at 15 m/px and coarser is
30,000 tiles, and *that* is what makes the map useful when you are 200 km from
home. The answer is not one build that compromises: it is **two builds and a
join**, because the zoom rungs are already independent grids — each carries its
own `mpp`, origin and extent — so nothing in the format objects to a pack whose
1.5 m/px rung covers a city and whose 60 m/px rung covers a country.

`tools/mergetiles.py` performs the join. It is a separate tool rather than a
flag because the inputs come from different extracts filtered to different road
classes, and no single invocation could sensibly want both.

**The one rule is a shared projection frame.** A tile is addressed as
`floor(e/mpp)` in the *pack's own* frame, so tiles cut against a different
reference name different ground, and a merge across references would produce a
pack that is well-formed and wrong. That is not recoverable after the fact, so
the tool refuses — and `mktiles --frame LAT,LON` exists to set the reference
independently of the region being cut, which is how the fine build is placed in
the coarse build's frame. A rung present in two inputs is taken from the first
and the choice is printed: rungs are whole grids, and interleaving two of them
would mean deciding per tile which map wins, which is a merge of cartography
and not of files.

What the join must not do is disturb the rungs it carries over, so that is what
`make test-tiles` asserts: the frozen `nav-tiles` golden, drawn from a pack that
now also holds two coarser rungs, comes back byte-identical; and a pack joined
to itself is itself, byte for byte, which is where an off-by-one in the index
rewrite would show.

#### Reading it on the device

`beepy-nav/src/tile.c`, wired to `basemap = PATH` in `~/.config/beepy-nav.conf`
or `--basemap FILE`, with `--no-basemap` to defeat it. Off by default, and
**optional in the strongest available sense: the five frozen `nav-*` goldens are
rendered with no pack and must still compare byte-identical.** A basemap that is
absent is therefore not merely unobtrusive, it provably changes nothing — and
the same is true of a pack that is missing, corrupt, at a zoom it does not
carry, or nowhere near the view. Every one of those is one line on stderr, at
most, and the frame the navigator drew before packs existed.

#### Rotation: inverse nearest-neighbour, and why it does not fringe

The map is course-up (§6.1) and a pre-rendered tile is not, so the blit walks
the **destination** and samples one source bit per screen pixel through the
inverse projection. Two things make that the right answer rather than a
compromise:

- **A straight line stays connected.** For a 1 px source line the sampled band
  is at least one pixel high in every destination column, so the rotated line
  is stair-stepped exactly as Bresenham's would be — never dotted. That is the
  property the whole approach rests on and it is asserted, not assumed
  (`tests/test_tile.c`, `t_rotated_line_is_connected`).
- **It lands through the aliased block path.** The bits go into the frame with
  one `cov_blit_bits()`, the same primitive the pre-rendered numerals use and
  the end of `cov_hairline()`'s road. Streets must not go near the coverage
  path: `mockup.py`'s `Canvas` comment records what a 1–2 px street looks like
  anti-aliased, which is "hairy rather than thin".

The arithmetic is **integer** — 16 fractional bits of a pack pixel — where
everything else in the program is doubles. The goldens are byte-compared
between clang on the Mac and gcc 10.2.1 on the device, and one ulp of
difference in `cos()` could move a sample across a pixel boundary; at 1/65536
of a pixel it would take a difference eleven orders of magnitude larger.
Confirmed empirically: `goldens/nav-tiles.fb` is byte-identical in both lanes.

The tile cache is an LRU of **twelve** 8 KB slots, not the four to eight a
"small cache" suggests, and the number is derived: a rotated 270×240 rectangle
has a bounding box of up to √(270²+240²) = 362 px a side, a 362 px span can
straddle three 256 px tiles, so one frame can touch 3×3 = 9 tiles. Eight slots
would evict a tile it is about to need again on every single frame.

#### Cost

Measured on the device, 200 renders of the NAV page with the Asok pack against
the same page without one: **9.5 ms → 16.7 ms** draw+resolve. Over a whole
replayed ride at 8 Hz (`--stats`, 5 505 frames): **10.2 ms mean / 11.5 ms p95
without tiles, 16.7 ms mean / 19.1 ms p95 with**, worst frame 31.0 ms. So the
layer costs about **7 ms a frame** and the p95 sits at two thirds of §6.4's
30 ms budget, against a 125 ms frame period. The one number that grazes the
budget is the max, and it is a single frame in five thousand.

One second-order effect worth recording: frame *skipping* nearly stops. Without
tiles 40 frames of that ride were dropped as unchanged; with tiles 15 were. A
basemap makes the display change on smaller movements, so §6.4's `memcmp` skip
buys less — which is the honest cost of the feature and not a bug in it.

#### Attribution

"© OpenStreetMap contributors" goes in the README and on the OVERVIEW **title
line** whenever a pack is loaded, and nowhere at all when one is not. The 5×7
font is uppercase ASCII with no U+00A9, so the screen spells it `(C)` — and the
two parentheses were added to `font.c` for this one string, because ` C ` with
blanks either side is not a copyright mark. Thirty characters at scale 1,
right-aligned, in rows 1–7 where they clear the compass badge (which drops
seven pixels on this page to make room). That leaves 200 px of title, which the
demo's own name overruns, so the rule is written down: the **length** is kept
whole and the **name** loses characters. The number is the part that changes;
the name is the part you already know.


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
rule). Nearer means more urgent, so the number of blinks is the rung index plus
one: one wink at half a kilometre, three at fifty metres, and no need to look
down to tell them apart. No page switching is needed — the NAV page is already
showing the turn. This is a quiet benefit of collapsing to two pages: the
auto-switch behaviour an earlier draft needed, along with its "return to the
previous page" rule and its configuration toggle, simply does not exist.

Nothing fires while the off-route latch is set: off route the announced cue is
not the junction ahead, so a flash would be an instruction to turn where there
is no turn. The rungs are **not** consumed either — they still fire once the
route is regained.

**`L` mutes the alerts for the ride** (`led_alerts` in the config file sets the
starting state; the key never writes the file back — the file is a default, the
key is a decision about the next ten minutes). A mute is the *opposite* case to
being off route and is handled the opposite way: the rungs go on being
**consumed**, they simply do not ring. Off route the alerts are wrong and owed
later; muted they are right and refused now. Keeping the ladder moving under a
mute is what stops `L`, pressed a hundred metres from a junction, from firing
500 and 200 as a burst for a turn already half taken.

**The toggle shows a transient confirmation** — `ALERTS ON` / `ALERTS OFF`,
centred, scale 2, in the bottom row for 1.5 s, then the row goes back to
arrival. It is not decoration. Muting is the one setting on this device whose
effect is invisible in the direction that matters: a silenced LED looks exactly
like a route with no junction nearby, so without a word on the screen a rider
cannot tell "I turned the alerts off" from "the alerts are broken", and would
find out which at the next missed turn.

Arrival is the row that can most afford a second and a half away — it changes
slowly, it is a prediction rather than a fact, and it is the one of the three a
rider is least likely to be reading at the exact moment they pressed a key.
**No permanent indicator is added.** Panel space is the scarce resource here,
and a setting the rider chose two seconds ago does not need continuous display.
The transient interacts correctly with §6.4's frame skip by construction — the
frames genuinely differ while it is up — and `live_poll_ms()` shortens the
poll so the row comes back on time even at the 1 Hz stopped rate, where the
next scheduled frame could otherwise be most of a second late and a lingering
`ALERTS OFF` would read as a stuck panel.

An unwritable LED (no udev rule) still accepts the toggle and still shows the
confirmation: the state is the rider's intent, and the one-line warning at
startup is where "there is no LED here" is said.

### 7.6 The ride log — evidence, not ride recording

§14 removed *ride recording* from this program and that removal stands: there
is no `.trk` journal, no GPX writer, no ODO persistence, no trip counters and
no accumulated totals — none of the silently-wrong-number class of bug that
left with them. What this section adds is a different thing wearing a similar
name. It records the **receiver**, not the ride.

The argument is one sentence long: **every bug in this program so far was found
by a replay, and a failure in the field currently leaves nothing to replay.**
The off-by-one in the countdown latch, the map rotating a fix late, the
window hint walking off down the route — each was a fixture and an assertion
within an hour of being seen, because there was a byte stream to point the
navigator at. On a road there is a rider's memory of a panel that looked wrong.

So, whenever the program is following a route on a real port:

```
~/rides/YYYYMMDD-HHMMSS.nmea   the raw byte stream, verbatim
~/rides/YYYYMMDD-HHMMSS.tsv    the per-fix trace -- --trace's own columns
```

Always on; `--no-log` turns it off and `rides_dir` moves it. A replay is not
logged, because a replay already *is* a log.

**Verbatim, before the parser.** The bytes go down exactly as they came off the
wire — line endings, bad checksums, empty fields, half a sentence cut short by
a USB reset, and anything that is not NMEA at all. Re-serialising the parsed
sentences would produce a clean file that reproduces none of the malformed
input worth studying, which is to say none of the input that causes bugs. The
UBX exchange of §6.3 is captured too: those bytes never reach the parser, and
without them the log would have a hole exactly where the receiver was being
configured.

**Two crash budgets, because SIGKILL and a power cut are different failures.**

| Cadence | Call | Protects against | Cost |
|---|---|---|---|
| every epoch | `fflush` | SIGKILL, a segfault, `kill -9` — the kernel keeps what it has been handed even when the process is gone | one `write(2)` a second |
| ≤ every 30 s | `fsync` | the battery falling out | ~15 KB at risk |

Thirty seconds is the budget §14 named while arguing this feature out of
existence, and at the measured 514 B/s (§3) it is 15 KB. Splitting the two is
what makes the flush affordable and the guarantee strong — the earlier draft of
this had only the 30 s `fsync`, which would have let a `kill -9` take up to a
4 KB stdio buffer, nine epochs, with it.

**A partial file is a valid replay input by construction.** The reader is
line-oriented and drops a final unterminated line, so the worst a kill can do is
lose the sentence that was mid-flight. Verified rather than asserted: §10.

**The log may never take the navigator down.** An unwritable directory, a full
disk, a write that fails mid-ride — each is one line on stderr, once, and then
the ride continues unlogged. This is the same argument §2.1 makes about the
config file, applied to the same rider at the same roadside.

**And it will not start below ~50 MB free.** Not a quota: at the measured
1.9 MB an hour (§3 — the first estimate of 60 KB counted three sentences an
epoch when the receiver sends nine) the guard is a little over a day of
continuous riding and it will never be what stops a ride. It exists because
the failure worth preventing is not "the log grew" but
"the log took the last of the disk and nobody was told". Refusing one more file
out loud is an acceptable outcome; silence on a full disk is not.

**`tools/ride2fixture.sh` is the point of all of it.** It copies a log into
`beepy-nav/tests/replay/`, replays it against the route to produce both traces,
and prints the Makefile lines to paste — a field failure becomes a regression
test in one command. It also does the check nothing else can: the `.tsv` written
*on the device* is compared against the one the replay produces from the same
bytes. The route maths is a pure function of the sentences, so those columns
must agree exactly, and a disagreement means the replay is not a rehearsal of
the ride and every assertion built on it would be measuring the wrong program.
The heading columns are excluded by design — §6.1's EWMA is over a time
constant and a live clock does not land on a replay's frame grid.

---

## 8. Data model

Only what the pages consume. The MAP page of §1.5 consumes a strict subset of
this — `fix_t`, the smoothed heading, and its own breadcrumb — and no `nav_t` at
all, because every field of `nav_t` is a statement about a route:

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
beepy-nav/     seg.c arrows.c gpx.c route.c map.c tile.c
               search.c router.c
               view_nav.c view_overview.c view_find.c view_confirm.c
               view_map.c nav.c
```

`view_map.c` (the §1.5 MAP page) owns no marks of its own: it calls
`view_nav.c`'s `mark_*`, including `mark_speed_badge()`, which was private until
this page needed the same instrument. Two pages have one now and there is still
exactly one of it — the alternative is two things to keep in step with
`mockup.py`, which is the argument the shared marks were extracted under.

`tile.c` (the §6.5 basemap reader) is deliberately linkable on its own: it needs
libbeepyfb for the blit and nothing at all from the navigator, which is what
lets `tests/test_tile.c` build pack fixtures byte by byte instead of shelling
out to a Pillow-dependent tool the device does not have.

`search.c` and `router.c` (§1.4) go further and need **no pixels at all** — libc
and libm, like `map.c` and `route.c`. That is what lets `tests/test_search.c`
link them against `route.c` alone, and it is why T-ONEWAY's 203 routes run
inside `make check` on the device rather than only on the Mac. `router.c` depends
on `search.c` for the graph and on `route.c` for the `route_t` it fills; nothing
depends on `router.c` but `nav.c`.

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
| Ride log survives a kill | `kill -9` the navigator mid-ride against a live port, then `--replay` the partial `.nmea` it left. The file must load, the fixes must be the ones that were on the wire, and the loss must be at most the sentence in flight — the flush cadence of §7.6, measured rather than argued |
| A field failure becomes a test | `tools/ride2fixture.sh LOG ROUTE NAME` turns a log into a `tests/rides/` case, and cross-checks the replayed per-fix trace against the `.tsv` the device itself wrote from the same bytes. They must agree exactly on every route-maths column |
| **T-LIVE** | 200 s of the real u-blox 7, recorded on the device (§3.1). It carries what no generated fixture does: an empty course field on every epoch, `GLL` and `TXT` sentences, and 15 m of indoor multipath the off-route latch must sit through. Asserted: the latch never fires, `off_m` stays under 40 m and exceeds 10 m somewhere, the heading never moves, the ETA stays unknown, and the drawn position still matches §6.3's closed form to 0.005 m |
| **T-BASEMAP-OPTIONAL** | the five `nav-*` goldens are rendered with **no** pack and must stay byte-identical, which is the whole claim §6.5 makes for the tile layer. Beside them, the same `nav-tiles` page with no pack and with an unreadable one must be the same frame, and neither may equal the frame with the pack — otherwise "draws nothing" would pass by drawing nothing ever. "No pack" is spelled `--no-basemap`, and that was a correction: the device's own `beepy-nav.conf` names a basemap, the demo path never calls `tiles_bind_route()`, and so the pack was being asked about the Asok state's metres *in its own frame* and drawing whatever happens to be there. The absent half of the pair had quietly stopped being absent. The three frozen MAP states hard-code `NULL` for the same reason |
| **T-MAP** | the §1.5 page from a headless replay with **no `--route` at all** — the mode nothing else in the suite ran. 5 505 frames render (before this page the loop assumed a route, and `route_snap()` on an empty one indexes a segment that is not there, so "it did not crash" is a real assertion), the breadcrumb grows past 300 points, the drawn position still matches §6.3's closed form, and two frames three minutes apart differ everywhere *except* the hint row — the map moved and the line the rider navigates by did not. Plus `--finite`, which is new: every other assertion here is a comparison and every comparison against NaN is false, so a column that had gone NaN would make `--max`, `--always` and `--monotone-*` all pass |
| **T-MAP-BASEMAP** | the basemap on the MAP page, whose world frame is the **pack's own reference** rather than a route's first point — the case `tiles_bind_route()` was written for and had never been given. A golden of its own, because a pair that merely said "the frames differ" would pass on streets drawn 200 m out; plus the absent/unreadable pair, plus the assertion that a pack reaches the map and never the `WAITING FOR FIX` frame, which has no position to draw one about |
| **T-ONEWAY** | 200 seeded random src/dst pairs plus three hand-picked ones over the real Asok graph, with **every hop of every result** checked against the pack's own directed adjacency: zero violations, and 142 of the 200 routable inside a corridor extract (asserted, so the test cannot pass by never routing). The three hand-picked pairs are the ones where the *undirected* shortest path provably rides a oneway backwards — including §1.4's own 824 m reference route, 22 hops the wrong way up Ratchadaphisek — and each is checked **both ways**: illegal on a pack built with `--ignore-oneway`, legal on the real one. Without that second pack the assertion could not fail |
| **T-ROADS-DETERMINISM** | `tools/mkpack.py` run twice on the same extract is byte-identical and equal to the committed `tests/roads/*.roads`, for the tile packs' reason and one more: T-ONEWAY's three pairs are **node indices**, and they mean nothing if the numbering moves |
| **T-ROADS-OPTIONAL** | the FIND page with no pack, and with an unreadable one, are the same frame — and neither is the frame with the pack. Unlike the basemap the difference here is the whole page, because a search with nothing to search cannot honestly show a hit; which is what makes the golden evidence about `search.c` and not only about the layout |
| **T-FIND** | the whole pre-ride flow from a headless replay: `F`, seven keystrokes, ENTER to route, ENTER to go. Asserted on the state machine rather than on pixels, because the frames move with the ride — the router ran on the device's own graph and found the place that was *typed*, `main()` installed the result by the same line that loads a GPX, and the three screens differ by at least 4 000 pixels each |
| **T-FIND-NOPACK** | `F` with no pack differs from an un-keyed run *only* inside the panel's bottom row, and is byte-identical two seconds later — the §7.5 transient mechanism, and the proof that the key is neither dead nor destructive |
| **T-NOTE-FITS** | the same claim as T-FIND-NOPACK with the **committed** pack named on the command line instead of whatever the config points at. That distinction is the whole test: the runs above inherit `beepy-nav.conf`, every config to date named a pack that did not reach this route, and under an empty map `NO ROAD PACK` spilling 8 px past the panel into the map was white on white. `tests/tiles/asok.tiles` does cover this ground and puts 154 px of street in the strip the overflow landed on. Found by merging a basemap that finally covered the ground the tests ride over — which is the third time a pair here has quietly stopped being a pair because one half read the device's config |
| **T-FIND-CANCEL** | two runs of one ride, one of which opened FIND and pressed `Esc`. Twenty seconds later they are the same frame: the page borrowed the screen and gave it back, which is the property that lets FIND be a page inside the ride loop rather than a modal detour |
| Search semantics | `tests/test_search.c` over a twelve-node hand-written pack (`tests/roads/names.json`), so every branch of the indexer has a case: `name:en` preferred, an ASCII `name` as the fallback, a Thai name dropped **and counted**, an unnamed way still routable, a footway not routable at all. Plus token-AND as substrings, order-independence, case folding, an empty query matching nothing, a miss matching nothing, ranking by distance to the nearest candidate point, and a hit count that is the total rather than the length of the list |
| Pack reader, the paths that refuse | one byte changed in the committed fixture at a time — magic, version, header size, coordinate scale, a section pointed inside the header — plus two truncations, and every accessor called on a `NULL` pack. Each must refuse with a message, because "no search" is a state the whole feature degrades to |
| **T-TILES-DETERMINISM** | `tools/mktiles.py` run twice on the same extract, route and options is byte-identical, and equal to the committed `tests/tiles/asok.tiles`. A pack is a binary fixture with a frozen golden hanging off it; without this, rebuilding one would silently move a frame |
| Tile reader, the paths that draw nothing | `tests/test_tile.c` builds packs byte by byte in C — no Pillow, so it runs inside `make check` on the device — and asserts the cases a golden cannot see: a zoom the pack lacks, a near-miss on the rung, a view 10 km outside the corridor, an absurd view, a missing file, bad magic, a truncated pack, an empty file. Also the two positive claims: a lit pack pixel lands on exactly the screen pixel §6.5's projection names, and a rotated straight line has **no gap in any column** |
| Console restore | every exit path leaves `fbterm` in `S+`, never `T` — verified on the device for `Q` and `SIGINT`. `SIGKILL` is not an exit path a program can hook, so it leaves `fbterm` in `T+`; the next clean run recovers it on its own (measured), and `kill -CONT $(pgrep -x fbterm)` is the manual answer |

The clipping test earns its place: the turn panel is the only part of this
screen that is never allowed to be painted over, and it is the one thing a map
bug would destroy.

Everything above is one row per **fix**, which is the wrong clock for half of
what this program now does. §6.1's smoothing, §6.3's extrapolation, §6.4's
frame skip and §7.5's alerts all happen *between* fixes, and a trace with one
row per fix cannot see any of them. `--trace-frames` writes one row per
rendered frame, carrying every quantity each of those four is built from, and
`make test-frames` asserts them:

| What | How |
|---|---|
| **T-DR** | the drawn position equals §6.3's closed form — `base + bearing(course)·speed·dt`, plus the correction being eased in — recomputed from the trace's own columns, to 5 mm over 10 000 frames |
| **T-DR** (ease) | a correction of 5 m or less is spread over exactly three frames, weights 3/4, 2/4, 1/4 and then gone; one beyond 5 m leaves no ease at all and the frame drawn *is* the fix |
| **T-HEAD-EWMA** | the heading trace is the circular EWMA of §6.1 with **τ = 0.55 s** — the constant this section adopted, asserted rather than assumed. τ = 0.47, the exact-equivalent reading rejected above, misses by 1.5°, so the test really does pin the choice |
| **T-HEAD-EWMA** (freeze) | below 3 km/h the heading does not move at all |
| **T-CUE-LED** | each of 500/200/50 m fires at most once per cue, only on a fix, and **never while the off-route latch is set**; ten of ten cues ring all three rungs on a clean ride, thirty rungs in total |
| **T-CUE-LED-MUTE** | `--key 120:l --key 250:l` mutes across cues 0–2. Nothing rings in the muted window, twenty-three rungs ring instead of thirty, and the cue whose approach straddles the un-mute rings **only its 50 m rung** — the 500 and 200 it passed while muted were consumed, not queued |
| **T-CUE-LED-MUTE** (panel) | the transient is byte-compared against the same frame from an un-keyed run: it differs *only* inside the bottom row, and two seconds later the two frames are byte-identical |
| **T-SKIP** | on a ride with the jitter turned off, 600 of 601 frames are skipped — the presents stop at the first identical frame and never resume |
| **T-NOFIX** | §1.1.2 over `nofix.nmea`, which is `ride.nmea` with thirty seconds of the fix voided and *nothing else changed*. The gaps are found in the trace rather than named by the Makefile: for each one, `NO FIX` does not appear inside 4 s, does appear within 5 s, is gone on the fix that ends the gap, and while it is up neither the countdown the panel prints (`cue_q`) nor the position it draws moves by so much as a micrometre — which is where "no phantom distance crosses the gap" is actually asserted |
| **T-NOFIX** (pixels) | three frames against the same three from the ride that never lost anything. At t=190 they are byte-identical, which is the control — without it a `NO FIX` that fired thirty seconds early would satisfy everything else. At t=215 the panel's bottom row differs by **1 556 of its 2 048 pixels**: that floor (`fbdiff --min-px`) is what makes it an assertion about *polarity* rather than about text, since a row swapped for another row of text moves a few hundred. At t=250 they are identical again everywhere outside the compass badge — recovery is clean, and the badge is the at-threshold tie described in the Makefile |

The `fix_err` column exists for T-DR alone: it is the one quantity that cannot
be recovered from the others, because the prediction it was measured against
belonged to the extrapolation base the row has already replaced. Without it a
trace cannot tell an eased correction from one that never happened.

T-SKIP needs a fixture that `stationary.nmea` deliberately is not. A real
receiver at a standstill jitters by a metre or three, which is what makes the
heading freeze worth having — and under it the map still shifts by a fraction
of a pixel, so 39% of frames are skipped rather than all of them and the claim
cannot be stated exactly. `still.nmea` (`--hold-jitter 0`) is unrealistic on
purpose.

---

## 11. Phases

| Phase | Content | Verifiable by |
|---|---|---|
| 0 | Module split, `%<>*` glyphs, VTG parsing | byte-identical `gps-monitor` dumps |
| 1 | Coverage canvas + threshold, generated numeral tables, cue glyphs, panel layout, static NAV page | dumps vs `nav-arrows.png`, `nav-smooth.png` |
| 2 | GPX load, snap, cues, corner rounding, live NAV + OVERVIEW, no basemap | replay against a real GPX |
| 3 | Off route, alerts, units, zoom and orientation keys, 8 Hz dead reckoning | detour replay; frame timing under `--replay` |
| 4 | Optional raster basemap and its build tooling | `fbshow --verify` on the panel at three rungs, and T-BASEMAP-OPTIONAL: the pre-basemap goldens are unchanged |
| 5 | FIND + on-device router (Dijkstra, oneway-aware) + CONFIRM | routed path vs a reference route; oneway violations = 0 |
| 6 | MAP: where you are, with no route (§1.5) | four goldens, three of them in the design gate, and T-MAP — a whole replay with no `--route` at all |

Phase 5 landed with one surprise worth carrying in this table: the reference
route it was to be checked against **was itself illegal**, so "routed path vs a
reference route" became two assertions rather than one (§1.4.3).

Phase 2 is the first genuinely useful build: it navigates. Phase 6 is the one
that makes it usable *before* you have decided where to go — which, in the
product owner's words, is "current location in map screen after open beepy-nav
program", and was the first thing anyone asked for that this program could not do.

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
  tests/roads/       the committed road packs and their extract (done)
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
| Ride recording | No `.trk` journal, no crash-recovery conversion, no ODO persistence. **§7.6 later brought back a raw NMEA log and only that** — a record of the receiver rather than of the ride, for replaying a field failure. None of the accumulated totals came back with it, and neither did the class of bug they carried |
| Satellite page | No satellite arrays, no GSV reassembly, no sky projection or label placement in `nav` |
| Trip counters | No distance-accumulation gating, no auto-pause, no moving-vs-elapsed timers |

Roughly **650 lines and one whole class of bug** — silently wrong accumulated
totals — leave with them. What remains is the part that was always the point:
follow the route, show the next turn.

If any of it is wanted later, `gps-monitor` still has the satellite pages. The
rest is not recoverable — this workspace is not under version control and the
previous draft of this file and its six mockups were overwritten. The removed
designs are summarised in the table above and nowhere else.

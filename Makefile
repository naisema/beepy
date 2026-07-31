# beepy — top-level build
#
# Two build lanes:
#   device (beepy.local, gcc 10.2.1)  — the real build; anything touching
#           /dev/fb1, evdev or termios only compiles here (linux/*.h).
#   host   (Mac, clang)               — fast dev loop for the portable
#           modules: font, canvas, nmea, gps (no fbdev/input/serial).
#
# The Mac entrypoint is `make sync`: rsync the tree to the device and run
# `make check` there. GATE: `check` byte-compares the --demo dumps against
# goldens/, and runs the map unit tests. Any diff is a regression; goldens
# regenerate only deliberately (GOLDEN_OK=1 make goldens).
#
# `make design-gate` is the other direction and Mac-only: it compares the
# beepy-nav pages against beepy-nav/mockup.py's own frames (needs Pillow).

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  ?= -lm
INC      = -I.

# The committed basemap fixture (DESIGN.md 6.5) and what rebuilds it. The
# pack is in git because `check` runs on the DEVICE, where there is no Pillow
# and no osm-asok.json; `make test-tiles` is the Mac-side check that it is
# still what mktiles.py produces, bit for bit.
TILEPACK   ?= beepy-nav/tests/tiles/asok.tiles
TILEROUTE  ?= beepy-nav/tests/gpx/asok.gpx
TILEOSM    ?= beepy-nav/osm-asok.json
TILEOPTS   ?= --corridor 1000 --zooms 2.5,4,6

# The committed road/name pack (DESIGN.md 1.4) and what rebuilds it. Committed
# for the same reason the tile pack is: `check` runs on the DEVICE, where there
# is no Overpass extract -- and here the fixture is not only a golden's input
# but the subject of T-ONEWAY, which needs BOTH forms of it. The second pack is
# built with --ignore-oneway on purpose: it is the mockup router's behaviour,
# frozen, and it is what makes T-ONEWAY a test that can fail.
ROADPACK   ?= beepy-nav/tests/roads/asok.roads
ROADOPEN   ?= beepy-nav/tests/roads/asok-nooneway.roads
ROADNAMES  ?= beepy-nav/tests/roads/names.roads
ROADOSM    ?= beepy-nav/osm-asok.json
ROADNAMESIN ?= beepy-nav/tests/roads/names.json
ROADPOIS   ?= beepy-nav/tests/roads/pois.json
ROADREF    ?= 13.740,100.560
# The saved-places fixture (DESIGN.md 1.4.6). A config file and not flags,
# because the feature IS a config file -- a test that set the places another
# way would not be testing the thing riders use.
SAVEDCONF  ?= beepy-nav/tests/saved.conf

DEVICE  ?= beepy@beepy.local
SSHKEY  ?= $(HOME)/.ssh/id_rsa
REMOTE_DIR ?= beepy-src

# ------------------------------------------------------------------ device
#
# libbeepyfb — Sharp-panel drawing + evdev input (canvas, font, fbdev, input)
# libnmea    — NMEA parsing, gps state, serial port
# gps-monitor — thin app: main loop, keymap, and the two page renderers

FB_OBJS   = libbeepyfb/canvas.o libbeepyfb/font.o libbeepyfb/cover.o \
            libbeepyfb/dump.o libbeepyfb/fbdev.o libbeepyfb/input.o
NMEA_OBJS = libnmea/nmea.o libnmea/gps.o libnmea/serial.o
GM_OBJS   = gps-monitor/main.o gps-monitor/view_bars.o gps-monitor/view_sky.o
NAV_OBJS  = beepy-nav/src/nav.o beepy-nav/src/view_nav.o beepy-nav/src/seg.o \
            beepy-nav/src/arrows.o beepy-nav/src/map.o beepy-nav/src/gpx.o \
            beepy-nav/src/route.o beepy-nav/src/view_overview.o \
            beepy-nav/src/fix.o beepy-nav/src/chooser.o beepy-nav/src/led.o \
            beepy-nav/src/config.o beepy-nav/src/ridelog.o \
            beepy-nav/src/tile.o beepy-nav/src/search.o \
            beepy-nav/src/router.o beepy-nav/src/view_find.o \
            beepy-nav/src/view_confirm.o beepy-nav/src/view_map.o \
            beepy-nav/src/view_quit.o
HDRS      = $(wildcard libbeepyfb/*.h libnmea/*.h gps-monitor/*.h beepy-nav/src/*.h)

all: gps-monitor/gps-monitor beepy-nav/beepy-nav

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(FB_OBJS) $(NMEA_OBJS) $(GM_OBJS) $(NAV_OBJS): $(HDRS)

libbeepyfb/libbeepyfb.a: $(FB_OBJS)
	ar rcs $@ $(FB_OBJS)

libnmea/libnmea.a: $(NMEA_OBJS)
	ar rcs $@ $(NMEA_OBJS)

gps-monitor/gps-monitor: $(GM_OBJS) libnmea/libnmea.a libbeepyfb/libbeepyfb.a
	$(CC) $(CFLAGS) -o $@ $(GM_OBJS) libnmea/libnmea.a libbeepyfb/libbeepyfb.a $(LDLIBS)

# libnmea joins the link in M3: the navigator has an NMEA source now.
beepy-nav/beepy-nav: $(NAV_OBJS) libnmea/libnmea.a libbeepyfb/libbeepyfb.a
	$(CC) $(CFLAGS) -o $@ $(NAV_OBJS) libnmea/libnmea.a \
		libbeepyfb/libbeepyfb.a $(LDLIBS)

check: gps-monitor/gps-monitor beepy-nav/beepy-nav test-unit test-replay
	./gps-monitor/gps-monitor --demo --page bars --dump out-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump out-sky.fb
	cmp goldens/gm-bars.fb out-bars.fb
	cmp goldens/gm-sky.fb  out-sky.fb
	./beepy-nav/beepy-nav --demo --page nav     --dump out-nav-turn.fb
	./beepy-nav/beepy-nav --demo --page nav-off --dump out-nav-off.fb
	./beepy-nav/beepy-nav --demo --page nav-nofix --dump out-nav-nofix.fb
	./beepy-nav/beepy-nav --demo --page arrows  --dump out-nav-arrows.fb
	./beepy-nav/beepy-nav --demo --page overview --dump out-nav-overview.fb
#	The MAP page of DESIGN.md 1.5, in its three states. Three and not one
#	because the two that are not the ordinary case are the ones a renderer can
#	get wrong while still "looking right": map-nofix is the only place on this
#	page where the strip's polarity flips, and map-wait is the frame that must
#	NOT contain a map -- a marker drawn about a position nobody has is exactly
#	the lie the page exists to refuse, and only a frozen frame can prove it is
#	absent.
	./beepy-nav/beepy-nav --demo --page map       --dump out-nav-map.fb
	./beepy-nav/beepy-nav --demo --page map-nofix --dump out-nav-map-nofix.fb
	./beepy-nav/beepy-nav --demo --page map-wait  --dump out-nav-map-wait.fb
#	And the fourth: a position in the tile pack's OWN reference frame, which is
#	the frame the MAP page introduced -- with no route there is no first point to
#	reference to, so the pack's own is what the world is measured from and
#	tiles_bind_route()'s offset is exactly zero. A pair that merely said "the
#	frames differ" would pass on streets drawn 200 m out; this is the golden that
#	says they land where they were rendered.
	./beepy-nav/beepy-nav --demo --page map-tiles \
		--basemap $(TILEPACK) --dump out-nav-map-tiles.fb
#	The basemap of DESIGN.md 6.5. The Asok pack is a committed fixture, not a
#	generated one: tools/mktiles.py needs Pillow and osm-asok.json, and the
#	device has neither -- 264 KB of tiles is the price of `check` meaning the
#	same thing in both lanes.
	./beepy-nav/beepy-nav --demo --page nav-tiles \
		--basemap $(TILEPACK) --dump out-nav-tiles.fb
	./beepy-nav/beepy-nav --demo --page overview-tiles \
		--dump out-nav-overview-tiles.fb
#	The FIND and CONFIRM pages of DESIGN.md 1.4. FIND takes the committed road
#	pack because the page IS the search: its row and its "464M NE" are computed
#	from the index, not drawn from a fixture, so this golden is evidence about
#	search.c as well as about the layout.
	./beepy-nav/beepy-nav --demo --page find \
		--roads $(ROADPACK) --dump out-nav-find.fb
	./beepy-nav/beepy-nav --demo --page find-none \
		--roads $(ROADPACK) --dump out-nav-find-none.fb
	./beepy-nav/beepy-nav --demo --page confirm --dump out-nav-confirm.fb
#	The QUIT page of DESIGN.md 1.6, in BOTH its states. Two frames and not one
#	because the question and the way out both change with whether a route is
#	loaded, and 1.6's whole claim is that the page tells the truth about what
#	is being left -- a golden of the riding state would say nothing about the
#	other truth.
#	DESIGN.md 1.4.6, and two states neither of which the other can stand for:
#	the FIND page before a key is pressed, which used to be four blank rows and
#	is now the rider's own list; and the waiting screen with somewhere to
#	centre on, whose claim is that a map IS drawn where nav-map-wait's is that
#	one is NOT -- and that neither draws a marker.
	./beepy-nav/beepy-nav --demo --page find-saved \
		--roads $(ROADPACK) --dump out-nav-find-saved.fb
	./beepy-nav/beepy-nav --demo --page map-saved \
		--basemap $(TILEPACK) --dump out-nav-map-saved.fb
	./beepy-nav/beepy-nav --demo --page map-wait-home \
		--basemap $(TILEPACK) --dump out-nav-map-wait-home.fb
	./beepy-nav/beepy-nav --demo --page quit     --dump out-nav-quit.fb
	./beepy-nav/beepy-nav --demo --page quit-map --dump out-nav-quit-map.fb
	cmp goldens/nav-turn.fb     out-nav-turn.fb
	cmp goldens/nav-off.fb      out-nav-off.fb
	cmp goldens/nav-nofix.fb    out-nav-nofix.fb
	cmp goldens/nav-arrows.fb   out-nav-arrows.fb
	cmp goldens/nav-overview.fb out-nav-overview.fb
	cmp goldens/nav-map.fb       out-nav-map.fb
	cmp goldens/nav-map-nofix.fb out-nav-map-nofix.fb
	cmp goldens/nav-map-wait.fb  out-nav-map-wait.fb
	cmp goldens/nav-map-tiles.fb out-nav-map-tiles.fb
	cmp goldens/nav-tiles.fb    out-nav-tiles.fb
	cmp goldens/nav-overview-tiles.fb out-nav-overview-tiles.fb
	cmp goldens/nav-find.fb     out-nav-find.fb
	cmp goldens/nav-find-none.fb out-nav-find-none.fb
	cmp goldens/nav-confirm.fb  out-nav-confirm.fb
	cmp goldens/nav-find-saved.fb out-nav-find-saved.fb
	cmp goldens/nav-map-saved.fb out-nav-map-saved.fb
#	The saved-place golden must not be the ordinary FIND golden. It was,
#	for one commit: the demo FIND page types a query, no saved place
#	matches it, and the frame came back byte-identical to nav-find --
#	freezing nothing, while a --min-px test on it passed happily. The
#	page has its own empty-query state now, and this is the assertion
#	that says so.
	! cmp -s goldens/nav-find-saved.fb goldens/nav-find.fb
	cmp goldens/nav-map-wait-home.fb out-nav-map-wait-home.fb
	cmp goldens/nav-quit.fb     out-nav-quit.fb
	cmp goldens/nav-quit-map.fb out-nav-quit-map.fb
#	The five goldens above nav-tiles are rendered with NO pack, and that is
#	the standing proof that the tile layer is optional: a basemap that is
#	absent must render exactly what rendered before basemaps existed. The two
#	comparisons below are the other half of it -- the same page with no pack
#	and with an unreadable one are the same frame, and neither is the frame
#	with the pack, so "draws nothing" cannot be passing by drawing nothing
#	ever.
#	--no-basemap and not "no flag", and this had to be learned the hard way: on
#	the DEVICE beepy-nav.conf names a basemap, so "no flag" meant the home pack,
#	and the demo path never calls tiles_bind_route() -- so that pack was asked
#	about the Asok state's coordinates in its OWN frame and drew the streets that
#	happen to be at those metres. The "absent" half of this pair was not absent
#	at all, and the assertion had quietly stopped being one. It went unseen
#	because the pack only recently appeared in that config, and `check` stops at
#	the first failure above this line.
	@echo "--- T-BASEMAP-OPTIONAL: absent and unreadable are the same frame"
	./beepy-nav/beepy-nav --demo --page nav-tiles --no-basemap \
		--dump out-nav-tiles-none.fb
	./beepy-nav/beepy-nav --demo --page nav-tiles \
		--basemap no-such-pack.tiles --dump out-nav-tiles-bad.fb
	cmp out-nav-tiles-none.fb out-nav-tiles-bad.fb
	! cmp -s out-nav-tiles.fb out-nav-tiles-none.fb
#	And the same argument for the MAP page. --no-basemap and not "no flag",
#	because unlike every other target here this one is affected by the DEVICE's
#	own beepy-nav.conf, which sets a basemap: the demo path never calls
#	tiles_bind_route(), so a configured pack is asked about this state's
#	coordinates in its own frame and draws whatever happens to be there. That is
#	not hypothetical -- it is what moved the golden the first time this ran on the
#	device, and it is why the three frozen MAP states hard-code NULL.
	@echo "--- T-MAP-BASEMAP: absent and unreadable are the same frame here too"
	./beepy-nav/beepy-nav --demo --page map-tiles --no-basemap \
		--dump out-nav-map-tiles-none.fb
	./beepy-nav/beepy-nav --demo --page map-tiles \
		--basemap no-such-pack.tiles --dump out-nav-map-tiles-bad.fb
	cmp out-nav-map-tiles-none.fb out-nav-map-tiles-bad.fb
	! cmp -s out-nav-map-tiles.fb out-nav-map-tiles-none.fb
#	...and a pack reaches the map but never the frame that has no position to
#	draw one about. A basemap blitted behind WAITING FOR FIX would be a map of
#	somewhere the rider has not been told they are: the same lie, quieter.
	./beepy-nav/beepy-nav --demo --page map-wait \
		--basemap $(TILEPACK) --dump out-nav-map-wait-tiles.fb
	cmp out-nav-map-wait.fb out-nav-map-wait-tiles.fb
#	The same argument for the road pack, and here it is stronger: the FIND page
#	with no pack is a DIFFERENT frame -- it has to be, because a search with
#	nothing to search cannot honestly show a hit -- so this pair says the row on
#	the golden was put there by the index and not by the renderer.
#
#	--no-roads for the reason T-BASEMAP-OPTIONAL now says --no-basemap: the device
#	config names a road pack too, and "no flag" would have searched Bangkok
#	instead of nothing.
	@echo "--- T-ROADS-OPTIONAL: no pack and an unreadable one are one frame"
	./beepy-nav/beepy-nav --demo --page find --no-roads \
		--dump out-nav-find-nopack.fb
	./beepy-nav/beepy-nav --demo --page find \
		--roads no-such-pack.roads --dump out-nav-find-badpack.fb
	cmp out-nav-find-nopack.fb out-nav-find-badpack.fb
	! cmp -s out-nav-find.fb out-nav-find-nopack.fb
	@echo "check: PASS - demo dumps byte-identical to goldens"

goldens: gps-monitor/gps-monitor beepy-nav/beepy-nav
ifndef GOLDEN_OK
	$(error refusing to regenerate goldens without GOLDEN_OK=1)
endif
	./gps-monitor/gps-monitor --demo --page bars --dump goldens/gm-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump goldens/gm-sky.fb
	./beepy-nav/beepy-nav --demo --page nav     --dump goldens/nav-turn.fb
	./beepy-nav/beepy-nav --demo --page nav-off --dump goldens/nav-off.fb
	./beepy-nav/beepy-nav --demo --page nav-nofix --dump goldens/nav-nofix.fb
	./beepy-nav/beepy-nav --demo --page arrows  --dump goldens/nav-arrows.fb
	./beepy-nav/beepy-nav --demo --page overview --dump goldens/nav-overview.fb
	./beepy-nav/beepy-nav --demo --page map \
		--dump goldens/nav-map.fb
	./beepy-nav/beepy-nav --demo --page map-nofix \
		--dump goldens/nav-map-nofix.fb
	./beepy-nav/beepy-nav --demo --page map-wait \
		--dump goldens/nav-map-wait.fb
	./beepy-nav/beepy-nav --demo --page map-tiles \
		--basemap $(TILEPACK) --dump goldens/nav-map-tiles.fb
	./beepy-nav/beepy-nav --demo --page nav-tiles \
		--basemap $(TILEPACK) --dump goldens/nav-tiles.fb
	./beepy-nav/beepy-nav --demo --page overview-tiles \
		--dump goldens/nav-overview-tiles.fb
	./beepy-nav/beepy-nav --demo --page find \
		--roads $(ROADPACK) --dump goldens/nav-find.fb
	./beepy-nav/beepy-nav --demo --page find-none \
		--roads $(ROADPACK) --dump goldens/nav-find-none.fb
	./beepy-nav/beepy-nav --demo --page confirm --dump goldens/nav-confirm.fb
	./beepy-nav/beepy-nav --demo --page find-saved \
		--roads $(ROADPACK) --dump goldens/nav-find-saved.fb
	./beepy-nav/beepy-nav --demo --page map-saved \
		--basemap $(TILEPACK) --dump goldens/nav-map-saved.fb
	./beepy-nav/beepy-nav --demo --page map-wait-home \
		--basemap $(TILEPACK) --dump goldens/nav-map-wait-home.fb
	./beepy-nav/beepy-nav --demo --page quit     --dump goldens/nav-quit.fb
	./beepy-nav/beepy-nav --demo --page quit-map --dump goldens/nav-quit-map.fb
	@echo "goldens regenerated - review the diff before committing"

# ----------------------------------------------------------- replay tests
#
# DESIGN.md 10 states most of the route maths as properties of a whole ride,
# not of a frame: "% DONE climbs monotonically to 100", "TO GO decreases",
# "the latch fires once and clears once". A golden frame cannot see any of
# that, so these replay a synthetic ride and assert over the trace.
#
# The rides are generated, not committed: half a megabyte of NMEA that
# tools/mknmea.py reproduces byte for byte from a seed is not worth carrying
# in git or rsyncing to the device on every sync.
#
# Runs in either lane. Device: `make test-replay`. Mac: `make host-replay`,
# which is the same thing pointed at the portable binary.

NAV       ?= ./beepy-nav/beepy-nav
# The four rides below assert per-FIX maths -- snap, latch, cue, progress --
# which DESIGN.md 8 recomputes once per fix and which is therefore independent
# of the display rate. Rendering each of them eight times over (6.3's frame
# clock) would add three minutes to every `make sync` and check nothing new,
# so they run one frame per fix. The 8 Hz path has its own tests below.
FPS1       = --fps 1
RDIR       = beepy-nav/tests/replay
# Recorded rides, as opposed to generated ones. Everything in $(RDIR) comes
# back byte for byte from mknmea.py and a seed, so it is gitignored and
# `clean` empties it; a recording comes back from nowhere and is committed.
# Which directory a fixture lives in is the whole distinction (DESIGN.md 7.6).
RIDES      = beepy-nav/tests/rides
RROUTE     = beepy-nav/tests/gpx/loop.gpx
MKNMEA     = python3 tools/mknmea.py --gpx $(RROUTE)
ASSERT     = python3 tools/assert_trace.py
REPLAYS    = $(RDIR)/ride.nmea $(RDIR)/stationary.nmea $(RDIR)/detour.nmea \
             $(RDIR)/rough.nmea $(RDIR)/still.nmea $(RDIR)/nofix.nmea
# The four rides above run one frame per fix; these three run again at the
# real 8 Hz, because 6.1's smoothing, 6.3's dead reckoning, 6.4's frame skip
# and 7.5's alerts all happen BETWEEN fixes and a one-frame-per-fix trace
# cannot see any of them. Three and not four: `detour` carries both an eased
# correction and a snapped one, so `rough` -- the most expensive of the four
# -- has nothing left to add here.
FPS8       = --fps 8

$(RDIR)/ride.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --speed 25 --brake --seed 1 -o $@

$(RDIR)/stationary.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --stationary 11@0 --duration 600 --seed 2 -o $@

$(RDIR)/detour.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --speed 25 --detour 100:900:300 --seed 3 -o $@

# ride.nmea with thirty seconds of the fix voided in the middle -- SAME seed,
# same speed, same brake points, so the trajectory is identical byte for byte
# and only the RMC status, the GGA quality, the satellite count and the HDOP
# change. That is what lets T-NOFIX compare frames against the ride that never
# lost anything: any difference between the two runs is the gap and nothing
# else. It is also what a receiver actually does under a bridge -- it keeps
# talking, it just stops claiming a position.
$(RDIR)/nofix.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --speed 25 --brake --seed 1 --nofix 200:230 -o $@

# A ride along the Asok route the road pack was cut around, so the FIND flow
# below searches and routes from a position that is actually IN the pack. Slow,
# because the whole route is 827 m and the flow needs thirty seconds of it.
$(RDIR)/asok.nmea: tools/mknmea.py $(TILEROUTE)
	python3 tools/mknmea.py --gpx $(TILEROUTE) --speed 12 --seed 11 -o $@

$(RDIR)/rough.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --speed 25 --noise 2 --dropout 100:140 --nofix 300:330 \
		--seed 7 -o $@

# Deliberately unrealistic: a receiver at a standstill jitters, which is why
# stationary.nmea does. This one does not, and that is the point -- DESIGN.md
# 6.4 claims a display that has stopped changing stops being sent, and under
# even a metre of jitter the map shifts by a fraction of a pixel and the claim
# cannot be stated exactly. It is also the only fixture where 6.1's heading
# FREEZE is what is under test rather than its smoothing.
$(RDIR)/still.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --stationary 11@0 --hold-jitter 0 --duration 600 --seed 2 -o $@

test-replay: $(NAV) $(REPLAYS)
	@echo "--- T-RIDE: a ride along its own route"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/ride.nmea --headless $(FPS1) \
		--trace $(RDIR)/ride.tsv
	$(ASSERT) $(RDIR)/ride.tsv --zero off_latched --monotone-up pct \
		--monotone-down togo_m --monotone-up cue_i \
		--always off_m "<=" 5 --final pct ">=" 99
	@echo "--- T-STATIONARY: ten minutes at a standstill"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/stationary.nmea --headless $(FPS1) \
		--trace $(RDIR)/stationary.tsv
	$(ASSERT) $(RDIR)/stationary.tsv --max along_m 5 --max off_m 5 \
		--constant heading_deg --zero off_latched
	@echo "--- T-DETOUR: a 100 m excursion, latched once and cleared once"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/detour.nmea --headless $(FPS1) \
		--trace $(RDIR)/detour.tsv
	$(ASSERT) $(RDIR)/detour.tsv --latch-count off_latched 1 \
		--final off_latched "==" 0 --monotone-up cue_i --final pct ">=" 99
	@echo "--- T-ROUGH: jitter, a 40 s dropout and 30 s with no fix"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/rough.nmea --headless $(FPS1) \
		--trace $(RDIR)/rough.tsv
	$(ASSERT) $(RDIR)/rough.tsv --monotone-up cue_i --final pct ">=" 99
	@echo "--- T-CLIP: a route leaving the map on all four sides"
	$(NAV) --demo --page cliptest       --dump $(RDIR)/clip.fb
	$(NAV) --demo --page cliptest-panel --dump $(RDIR)/clip-panel.fb
	python3 tools/fbdiff.py $(RDIR)/clip.fb $(RDIR)/clip-panel.fb \
		--mask 130,0,399,239 --max-px 0
#	Everything above this line is synthetic. This one is not: 200 seconds of
#	the actual u-blox 7 on /dev/ttyACM0, recorded by the ride log of 7.6 and
#	promoted with tools/ride2fixture.sh. It is worth its 100 KB because it
#	contains three things mknmea.py has never produced and would not have
#	thought to -- an EMPTY VTG/RMC course field (the receiver reports no
#	course over ground while stationary, so the heading never gets a value at
#	all), $GPGLL and $GPTXT sentences that DESIGN.md 3's measurement did not
#	list, and fifteen metres of real indoor multipath wander that the
#	off-route latch must sit through without firing.
	@echo "--- T-LIVE: 200 s of the real receiver, recorded on the device"
	$(NAV) --route beepy-nav/tests/gpx/live-ublox.gpx \
		--replay $(RIDES)/live-ublox.nmea --headless $(FPS8) \
		--trace $(RDIR)/live-ublox.tsv \
		--trace-frames $(RDIR)/live-ublox-frames.tsv
	$(ASSERT) $(RDIR)/live-ublox.tsv --zero off_latched --max off_m 40 \
		--max along_m 20 --constant course_deg --constant heading_deg \
		--always eta_s "<" 0 --any off_m ">" 10
	$(ASSERT) $(RDIR)/live-ublox-frames.tsv --dr-closed-form 0.005 \
		--head-ewma 0.55 0.01 --any base_spd "<" 0.8
	$(MAKE) test-frames NAV="$(NAV)"
	$(MAKE) test-find NAV="$(NAV)"
	@echo "test-replay: PASS"

# ------------------------------------------------- the 8 Hz frame clock
#
# One row per rendered FRAME (--trace-frames), which is the only place the
# maths of 6.1, 6.3, 6.4 and 7.5 is visible: all four happen between fixes.
# Every expected value is recomputed by tools/assert_trace.py from the columns
# the navigator wrote, from the closed form in the design -- never compared
# against a stored answer from a previous run.
test-frames: $(NAV) $(REPLAYS)
	@echo "--- T-DR: extrapolation, and a correction eased over three frames"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/ride.nmea --headless $(FPS8) \
		--dump-at 120.5:$(RDIR)/plain-a.fb \
		--dump-at 122.5:$(RDIR)/plain-b.fb \
		--dump-at 190:$(RDIR)/plain-pre.fb \
		--dump-at 215:$(RDIR)/plain-gap.fb \
		--dump-at 250:$(RDIR)/plain-post.fb \
		--trace-frames $(RDIR)/ride-frames.tsv
	$(ASSERT) $(RDIR)/ride-frames.tsv --dr-closed-form 0.005 --dr-ease
	@echo "--- T-DR-SNAP: and one beyond 5 m taken whole, on its own frame"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/detour.nmea --headless $(FPS8) \
		--trace-frames $(RDIR)/detour-frames.tsv
	$(ASSERT) $(RDIR)/detour-frames.tsv --dr-closed-form 0.005 --dr-ease
	@echo "--- T-HEAD-EWMA: tau = 0.55 s (DESIGN.md 6.1), and the freeze"
	$(ASSERT) $(RDIR)/ride-frames.tsv --head-ewma 0.55 0.01 \
		--any base_spd ">" 2.0
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--trace-frames $(RDIR)/still-frames.tsv
	$(ASSERT) $(RDIR)/still-frames.tsv --head-ewma 0.55 0.01 \
		--any base_spd "<" 0.8
	@echo "--- T-CUE-LED: 500/200/50 m, once each, never off route"
	$(ASSERT) $(RDIR)/ride-frames.tsv --cue-led 10 --led-rungs 30
	$(ASSERT) $(RDIR)/detour-frames.tsv --cue-led 9 --any off_latched "==" 1
	@echo "--- T-CUE-LED-MUTE: L off at 120 s, on at 250 s, no backlog"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/ride.nmea --headless $(FPS8) \
		--key 120:l --key 250:l \
		--dump-at 120.5:$(RDIR)/note-a.fb \
		--dump-at 122.5:$(RDIR)/note-b.fb \
		--trace-frames $(RDIR)/mute-frames.tsv
	$(ASSERT) $(RDIR)/mute-frames.tsv --led-quiet 120 250 --led-rungs 23 \
		--cue-led 7
	@echo "    ... and the transient takes the bottom row and gives it back"
	python3 tools/fbdiff.py $(RDIR)/note-a.fb $(RDIR)/plain-a.fb \
		--mask 0,216,127,239 --max-px 0
	python3 tools/fbdiff.py $(RDIR)/note-b.fb $(RDIR)/plain-b.fb --max-px 0
	@echo "--- T-SKIP: a display that stopped changing stops being sent"
	$(ASSERT) $(RDIR)/still-frames.tsv --settles presented
	@echo "--- T-NOFIX: thirty seconds with no fix (DESIGN.md 1.1)"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/nofix.nmea --headless $(FPS8) \
		--dump-at 190:$(RDIR)/nofix-pre.fb \
		--dump-at 215:$(RDIR)/nofix-gap.fb \
		--dump-at 250:$(RDIR)/nofix-post.fb \
		--trace-frames $(RDIR)/nofix-frames.tsv
	$(ASSERT) $(RDIR)/nofix-frames.tsv --nofix 4 --any nofix "==" 1 \
		--dr-closed-form 0.005
	@echo "    ... and the same ride WITHOUT the gap, frame against frame"
#	Before the gap the two runs are the same program on the same bytes, so
#	the frames are identical. This is the control: without it, a NO FIX that
#	appeared thirty seconds early would still satisfy everything below.
	python3 tools/fbdiff.py $(RDIR)/nofix-pre.fb $(RDIR)/plain-pre.fb \
		--max-px 0
#	Inside the gap the bottom row of the PANEL (x 0-127, y 224-239) has not
#	merely changed text -- its background has flipped. A row of paper text
#	swapped for another row of paper text differs by a few hundred pixels;
#	--min-px is what makes this an assertion about polarity.
	python3 tools/fbdiff.py $(RDIR)/nofix-gap.fb $(RDIR)/plain-gap.fb \
		--mask 0,0,399,223 --mask 128,224,399,239 --min-px 1200
#	Twenty seconds after the fixes return, the run that lost them draws the
#	same frame as the run that never did: the panel is back to the three-row
#	stack, the map has re-snapped, and the countdown latch re-armed on
#	recovery has landed on the same rung. Recovery is clean or this fails.
#
#	The compass badge is masked, and it is the only thing that is. The
#	smoothed heading of 6.1 is an infinite-impulse filter, so twenty seconds
#	(36 tau) after the gap the two runs differ by about 1e-13 degrees -- and
#	the needle's base edge sits exactly on a half-pixel boundary, where
#	coverage resolves at exactly 50 % and the threshold's own tie-break
#	flips. Twelve pixels of the needle's rim change and nothing else in the
#	frame does. That is the same at-threshold case tools/design_gate.py
#	exempts by name, measured rather than assumed: the assertion below is
#	--max-px 0 everywhere outside a 29x39 box.
	python3 tools/fbdiff.py $(RDIR)/nofix-post.fb $(RDIR)/plain-post.fb \
		--mask 138,4,166,42 --max-px 0
#	----------------------------------------------- the MAP page (DESIGN.md 1.5)
#
#	The same ride with NO --route at all, which is the mode this page exists for
#	and which nothing else in this Makefile has ever run. Three claims, and each
#	of them has a way to fail:
#
#	  * it renders every frame. Before 1.5 the loop assumed a route: route_snap()
#	    on an empty route_t indexes a segment that is not there, so "it did not
#	    crash" is a real assertion here and the frame count is the evidence.
#	  * the breadcrumb GROWS. --monotone-up is not enough on its own -- a
#	    breadcrumb that never recorded anything is monotone too -- so --any pins
#	    a floor, and it is derived rather than guessed: ride.nmea is 25 km/h over
#	    ~7.6 km, which is 300+ points at the 5 m spacing of CRUMB_MIN_M.
#	  * nothing is NaN. Every other assertion in this file is a comparison, and
#	    comparisons against NaN are false, so a page whose position came from an
#	    unset frame would pass all of them. --finite is the one that cannot.
	@echo "--- T-MAP: the same ride with no route, and a breadcrumb that grows"
	$(NAV) --replay $(RDIR)/ride.nmea --headless $(FPS8) \
		--dump-at 60:$(RDIR)/map-a.fb \
		--dump-at 240:$(RDIR)/map-b.fb \
		--trace $(RDIR)/map.tsv --trace-frames $(RDIR)/map-frames.tsv
	$(ASSERT) $(RDIR)/map-frames.tsv --finite --monotone-up crumb \
		--any crumb ">" 300 --zero off_latched --dr-closed-form 0.005
	$(ASSERT) $(RDIR)/map.tsv --finite --always seg "==" -1
#	Two frames three minutes apart, and they are different frames: the page is
#	drawing a position that MOVED, not a still picture of one. Without this the
#	assertions above would pass on a renderer that drew the first frame forever.
	! python3 tools/fbdiff.py $(RDIR)/map-a.fb $(RDIR)/map-b.fb --max-px 0
#	...and the HINT row is byte-identical in both, which is the other half of the
#	same claim: the map moved and the line the rider navigates by did not. Not
#	the whole strip -- its first row is the coordinates, and those had better
#	have changed.
	python3 tools/fbdiff.py $(RDIR)/map-a.fb $(RDIR)/map-b.fb \
		--mask 0,0,399,216 --max-px 0
	@echo "test-frames: PASS"

# ------------------------------------------------- FIND, CONFIRM and GO (1.4)
#
# The whole pre-ride flow, driven from a headless replay: F, seven keystrokes,
# ENTER to route, ENTER to go. DESIGN.md 2 is explicit that --key is not a
# debugging affordance to be removed later -- it is the only way any of the
# keymap is testable, and that goes double for a page whose entire surface is a
# text field.
#
# The assertions are deliberately about the STATE MACHINE rather than about
# pixels: the frames on this route move with the replay and cannot be frozen,
# but "typing changed the screen", "ENTER produced a CONFIRM that is a different
# screen again" and "the second ENTER started a ride on a route named after the
# place" are exactly the claims of 1.4 and each of them has a way to fail.
test-find: $(NAV) $(RDIR)/asok.nmea $(RDIR)/ride.nmea $(ROADPACK)
	@echo "--- T-FIND: F, a typed query, ENTER, CONFIRM, ENTER, riding"
	$(NAV) --route $(TILEROUTE) --replay $(RDIR)/asok.nmea --headless $(FPS8) \
		--roads $(ROADPACK) \
		--dump-at 9.5:$(RDIR)/find-nav.fb \
		--key 10:f --key 11:s --key 12:o --key 13:i \
		--key 14:space --key 15:2 --key 16:3 \
		--dump-at 17:$(RDIR)/find-typed.fb \
		--key 18:enter \
		--dump-at 19:$(RDIR)/find-confirm.fb \
		--key 20:enter \
		--dump-at 40:$(RDIR)/find-riding.fb \
		2> $(RDIR)/find.log
#	The router ran, on the device's own graph, and found the place that was
#	typed -- not a place the test named.
	grep -q "routed to SOI SUKHUMVIT 23" $(RDIR)/find.log
#	And main() installed it by the same path a GPX takes: this line is printed
#	by the route-loading loop and by nothing else.
	grep -q "beepy-nav: SOI SUKHUMVIT 23 -- " $(RDIR)/find.log
#	Three distinct screens. --min-px, because "the page changed" is the claim;
#	an equality here would pass on a FIND page that never opened.
	python3 tools/fbdiff.py $(RDIR)/find-nav.fb $(RDIR)/find-typed.fb \
		--min-px 4000
	python3 tools/fbdiff.py $(RDIR)/find-typed.fb $(RDIR)/find-confirm.fb \
		--min-px 4000
	python3 tools/fbdiff.py $(RDIR)/find-confirm.fb $(RDIR)/find-riding.fb \
		--min-px 4000
	@echo "--- T-FIND-NOPACK: F with no pack says so, and changes nothing else"
#	The transient of 7.5's mechanism, on the same bottom row and for the same
#	1.5 s. Masked exactly as T-CUE-LED-MUTE masks its own: the note is the only
#	difference at 120.5 s, and by 122.5 s there is no difference at all. A key
#	that did nothing would fail the first comparison; a key that changed the map
#	would fail it too.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/ride.nmea --headless $(FPS8) \
		--no-roads --key 120:f \
		--dump-at 120.5:$(RDIR)/nopack-a.fb \
		--dump-at 122.5:$(RDIR)/nopack-b.fb
	python3 tools/fbdiff.py $(RDIR)/nopack-a.fb $(RDIR)/plain-a.fb \
		--mask 0,216,127,239 --max-px 0
	! python3 tools/fbdiff.py $(RDIR)/nopack-a.fb $(RDIR)/plain-a.fb --max-px 0
	python3 tools/fbdiff.py $(RDIR)/nopack-b.fb $(RDIR)/plain-b.fb --max-px 0
	@echo "--- T-SAVED: F opens on the rider's own list, and routes to it"
#	DESIGN.md 1.4.6. Three claims, and the third is the one that makes the
#	feature worth its config key: the page that used to open blank now opens on
#	the saved list, typing turns it back into a search WITHOUT losing a saved
#	place that still matches, and ENTER routes to a coordinate that is in no
#	pack at all -- the same off-graph snap T-FIND-POI asserts, reached a
#	different way.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--config $(SAVEDCONF) --roads $(ROADPACK) \
		--key 10:f --dump-at 12:$(RDIR)/saved-list.fb \
		--key 14:w --key 15:o --key 16:r \
		--dump-at 18:$(RDIR)/saved-typed.fb \
		--key 20:enter --key 22:enter 2> $(RDIR)/saved.log
#	The empty page is not empty any more -- against the same page with no
#	places configured, which is the only comparison that isolates the feature
#	from the layout.
#	--config /dev/null, and NOT "no --config". Without it this run reads
#	~/.config/beepy-nav.conf, and the day the device's owner saves their own
#	HOME and WORK -- which is the day the feature starts being used -- the
#	control frame grows the very rows it exists to lack. That happened: this
#	assertion passed on the Mac and failed on the device with 460 differing
#	pixels, because the two frames were the same list at two distances.
#
#	It is the FOURTH time a pair here has quietly stopped being a pair by
#	reading the device's config, and the first three are why CLAUDE.md carries
#	the rule. An empty file is the only way to spell "no places" that cannot be
#	overruled by what is on the machine.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--config /dev/null --roads $(ROADPACK) --key 10:f \
		--dump-at 12:$(RDIR)/saved-none.fb 2>/dev/null
	python3 tools/fbdiff.py $(RDIR)/saved-list.fb $(RDIR)/saved-none.fb \
		--min-px 500
#	Typing keeps the saved place that matches and brings pack hits in beside
#	it, so the two frames differ -- a filter that dropped WORK would look
#	identical to one that never had it.
	python3 tools/fbdiff.py $(RDIR)/saved-typed.fb $(RDIR)/saved-list.fb \
		--min-px 500
#	The icons (1.4.6): a saved row draws the picture its kind names, and the
#	MAP page draws the same one at the same place. Asserted as "the frame
#	changes when places are configured" here and frozen exactly by
#	goldens/nav-find-saved.fb and nav-map-saved.fb, which is the split this
#	suite uses everywhere: a replay says the wiring is live, a golden says the
#	pixels are right.
	grep -q "routed to WORK" $(RDIR)/saved.log
	grep -q "beepy-nav: WORK -- " $(RDIR)/saved.log
#	The ORDER -- HOME above WORK, which is the file's order and not
#	alphabetical -- is frozen by goldens/nav-find-saved.fb and needs no
#	assertion of its own here. A `cmp` against a live replay frame would only
#	be comparing distances that legitimately differ.
	@echo "--- T-QUIT-CONFIRM: Q asks, and a cancel gives the frame back"
#	DESIGN.md 1.6. Q used to end the program on one press. The claim now is
#	three-part and each part can fail on its own: Q opens a page (so the frame
#	CHANGES), a second Q cancels it (so the program is still running to draw a
#	later frame at all), and what comes back is the frame that was there before
#	-- BYTE FOR BYTE, because a confirmation that perturbs the ride behind it
#	has charged the rider something for saying no.
#
#	On still.nmea and not ride.nmea, and that is the whole reason the stationary
#	fixture exists: byte-for-byte is only a meaningful claim where the frame is
#	not entitled to change on its own, and T-SKIP already proves this one
#	settles. On a moving ride the same assertion would be measuring the clock.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--dump-at 120:$(RDIR)/quit-before.fb \
		--key 121:q \
		--dump-at 122:$(RDIR)/quit-asked.fb \
		--key 123:q \
		--dump-at 125:$(RDIR)/quit-cancelled.fb 2> $(RDIR)/quit.log
	python3 tools/fbdiff.py $(RDIR)/quit-asked.fb $(RDIR)/quit-before.fb \
		--min-px 4000
	python3 tools/fbdiff.py $(RDIR)/quit-cancelled.fb $(RDIR)/quit-before.fb \
		--max-px 0
#	And it ran to the end of the fixture, which is what says Q did not quit: a
#	program that exited at 121 s could not have reached 601.
	grep -q "601 fixes" $(RDIR)/quit.log
	@echo "--- T-QUIT-GO: ENTER on the page is the exit Q used to be"
#	The pair. Without it "Q does not quit" would pass on a build where nothing
#	quits at all, and the page would be a trap rather than a question.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--key 121:q --key 123:enter 2> $(RDIR)/quit-go.log
	! grep -q "601 fixes" $(RDIR)/quit-go.log
	@echo "--- T-END-ROUTE: E leaves the route and keeps the program"
#	The other half of 1.6, and the one a rider reaches for after arriving. E
#	ends the ROUTE without ending the process: the first pass through run_live()
#	stops early, the loop takes its no-route branch, and a SECOND pass reports
#	its own fix count. Two "fixes," lines is that, and one is either a program
#	that quit or a key that did nothing.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--key 121:e --dump-at 200:$(RDIR)/ended.fb 2> $(RDIR)/end.log
	test `grep -c "fixes," $(RDIR)/end.log` -eq 2
#	What it lands on is the MAP page, not the NAV page holding an empty route.
#	MAP is full width -- there is no inverted turn panel -- so a frame that
#	still carried one would mean the route was freed and the page was not.
	python3 tools/fbdiff.py $(RDIR)/ended.fb $(RDIR)/quit-before.fb \
		--min-px 4000
	@echo "--- T-FIND-POI: routing to something that is not on the graph"
#	Every destination before this one WAS a graph node: a street's candidate
#	points are its own vertices, so the "snap to the nearest node" in router.c
#	had never been asked to travel. A POI is the first destination that lies off
#	the graph entirely -- a school in the middle of its campus -- and the claim
#	is that it still routes, by the same snap, with no special case anywhere.
#	tests/roads/pois.roads is the committed fixture; THE ACADEMY sits ~110 m off
#	NORTH ROAD and belongs to no way at all.
	$(NAV) --route $(TILEROUTE) --replay $(RDIR)/asok.nmea --headless $(FPS8) \
		--roads beepy-nav/tests/roads/pois.roads \
		--key 10:f --key 11:a --key 12:c --key 13:a --key 14:d \
		--key 16:enter --key 18:enter \
		--dump-at 20:$(RDIR)/find-poi.fb 2> $(RDIR)/find-poi.log
	grep -q "routed to THE ACADEMY" $(RDIR)/find-poi.log
#	And installed as the live route by the same line a GPX takes, which is what
#	makes it a destination rather than a map label.
	grep -q "beepy-nav: THE ACADEMY -- " $(RDIR)/find-poi.log
	@echo "--- T-NOTE-FITS: a note stays inside the panel, over a drawn map"
#	The pair above asserts "changes nothing outside the panel" -- but only as
#	strongly as the map under the note is interesting, and for a long time it
#	was not: these runs take whatever basemap the config names, and every
#	config to date named a pack that did not reach this route. Under an empty
#	map, "NO ROAD PACK" spilling 8 px past PANEL_W into it was white on white.
#	So the same claim again with the COMMITTED pack named explicitly, which
#	does cover this ground and puts 154 px of street in the strip the overflow
#	would have landed on. No config, and nothing to configure.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/ride.nmea --headless $(FPS8) \
		--basemap $(TILEPACK) --no-roads \
		--dump-at 120.5:$(RDIR)/notemap-plain.fb
	$(NAV) --route $(RROUTE) --replay $(RDIR)/ride.nmea --headless $(FPS8) \
		--basemap $(TILEPACK) --no-roads --key 120:f \
		--dump-at 120.5:$(RDIR)/notemap-note.fb
	python3 tools/fbdiff.py $(RDIR)/notemap-note.fb $(RDIR)/notemap-plain.fb \
		--mask 0,216,127,239 --max-px 0
	! python3 tools/fbdiff.py $(RDIR)/notemap-note.fb \
		$(RDIR)/notemap-plain.fb --max-px 0
	@echo "--- T-FIND-CANCEL: Esc leaves no trace of the page at all"
#	Two runs of the same ride, one of which opened FIND and backed out of it.
#	Twenty seconds later they are the same frame, so the page borrowed the
#	screen and gave it back -- the property that lets FIND be a page inside the
#	ride loop instead of a modal detour.
#
#	The typed key is Z on purpose. Z is the zoom-out key on the NAV page, so a
#	FIND page that failed to swallow it would leave the map one rung coarser --
#	1 486 pixels different at t=40, measured. Typing a letter with no ride
#	meaning would have made this pass whether the page owned the keyboard or
#	not, which is the difference between a test and a formality.
	$(NAV) --route $(TILEROUTE) --replay $(RDIR)/asok.nmea --headless $(FPS8) \
		--roads $(ROADPACK) --key 10:f --key 11:z --key 12:esc \
		--dump-at 40:$(RDIR)/cancel-post.fb
	$(NAV) --route $(TILEROUTE) --replay $(RDIR)/asok.nmea --headless $(FPS8) \
		--roads $(ROADPACK) --dump-at 40:$(RDIR)/cancel-plain.fb
	python3 tools/fbdiff.py $(RDIR)/cancel-post.fb $(RDIR)/cancel-plain.fb \
		--max-px 0
	@echo "test-find: PASS"

host-replay: host/beepy-nav $(REPLAYS)
	$(MAKE) test-replay NAV=host/beepy-nav

# 100 NAV renders, draw + resolve only (DESIGN.md 5.4 budgets 30 ms).
#
# Three lines, not one: DESIGN.md 6.5 quotes what the basemap costs, and a
# quoted number nobody can re-derive is a number that rots. The middle line is
# the control -- the same page, the same route, no pack -- which is what makes
# the difference between the first and third attributable to the tile layer
# rather than to the Asok route being a different shape from the demo one.
bench: beepy-nav/beepy-nav
	./beepy-nav/beepy-nav --demo --page nav --bench 100
	./beepy-nav/beepy-nav --demo --page nav-tiles --bench 100
	./beepy-nav/beepy-nav --demo --page nav-tiles --basemap $(TILEPACK) \
		--bench 100
#	KEYSTROKE -> FRAME for the FIND page (DESIGN.md 1.4). This is the whole cost
#	of one press: view_find_demo() runs search_places() over the pack and then
#	draws, which is exactly what find_key() does. The budget is 50 ms -- a
#	type-to-filter field that lags a fast thumb is worse than a menu -- and the
#	number to quote is the one from a CITY pack, so pass ROADPACK=... a big one.
	./beepy-nav/beepy-nav --demo --page find --roads $(ROADPACK) --bench 200

# -------------------------------------------------------------------- host
#
# Compile the portable objects on the Mac so parser/renderer edits get a
# warning pass without a device round-trip. fbdev/input/serial are device-only.

HOST_OBJS = host/canvas.o host/font.o host/cover.o host/dump.o \
            host/nmea.o host/gps.o \
            host/seg.o host/arrows.o host/map.o host/gpx.o host/route.o \
            host/view_nav.o host/view_overview.o host/fix.o host/chooser.o \
            host/led.o host/config.o host/ridelog.o host/tile.o \
            host/search.o host/router.o host/view_find.o \
            host/view_confirm.o host/view_map.o host/view_quit.o \
            host/nav.o

# beepy-nav is portable end to end (no fbdev, no evdev), so the Mac can link
# and run it -- which is what makes the M2 design gate a fast loop.
HOST_NAV = host/nav.o host/view_nav.o host/view_overview.o host/seg.o \
           host/arrows.o host/map.o host/gpx.o host/route.o host/fix.o \
           host/chooser.o host/led.o host/config.o host/ridelog.o \
           host/tile.o host/search.o host/router.o host/view_find.o \
           host/view_confirm.o host/view_map.o host/view_quit.o \
           host/nmea.o host/gps.o \
           host/canvas.o host/font.o host/cover.o host/dump.o

host: $(HOST_OBJS) host/beepy-nav
	@echo "host: portable objects compile clean"

host/beepy-nav: $(HOST_NAV)
	$(CC) $(CFLAGS) -o $@ $(HOST_NAV) $(LDLIBS)

# map.c, gpx.c and route.c are the beepy-nav modules with no pixels in them,
# so they are the ones that can be checked by assertion instead of by frame
# comparison. Runs in either lane; `check` runs it on the device.
UNIT_TESTS = beepy-nav/tests/test_map beepy-nav/tests/test_gpx \
             beepy-nav/tests/test_route beepy-nav/tests/test_tile \
             beepy-nav/tests/test_search

test-unit: $(UNIT_TESTS) beepy-nav/tests/gpx/oversize.gpx
	./beepy-nav/tests/test_map
	./beepy-nav/tests/test_gpx
	./beepy-nav/tests/test_route
	./beepy-nav/tests/test_tile
	./beepy-nav/tests/test_search

beepy-nav/tests/test_map: beepy-nav/tests/test_map.c beepy-nav/src/map.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_map.c beepy-nav/src/map.c $(LDLIBS)

beepy-nav/tests/test_gpx: beepy-nav/tests/test_gpx.c beepy-nav/src/gpx.c \
                          beepy-nav/src/route.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_gpx.c beepy-nav/src/gpx.c \
		beepy-nav/src/route.c $(LDLIBS)

beepy-nav/tests/test_route: beepy-nav/tests/test_route.c beepy-nav/src/gpx.c \
                            beepy-nav/src/route.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_route.c beepy-nav/src/gpx.c \
		beepy-nav/src/route.c $(LDLIBS)

# tile.c does draw, so this one needs the coverage renderer -- but not the
# navigator: the pack reader is deliberately linkable on its own, which is
# what lets the test build its fixtures byte by byte instead of shelling out
# to mktiles.py (Pillow, and therefore Mac-only, and therefore useless to a
# test that has to run inside `make check` on the device).
beepy-nav/tests/test_tile: beepy-nav/tests/test_tile.c beepy-nav/src/tile.c \
                           $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_tile.c beepy-nav/src/tile.c \
		libbeepyfb/cover.c libbeepyfb/canvas.c libbeepyfb/font.c $(LDLIBS)

# search.c and router.c have no pixels in them either, so like map/gpx/route
# they are checked by assertion. route.c joins the link because router_to()
# leaves a prepared route_t -- which is the point of it -- and gpx.c because
# route.c calls route_load(). No cover.c: nothing here draws.
beepy-nav/tests/test_search: beepy-nav/tests/test_search.c \
                             beepy-nav/src/search.c beepy-nav/src/router.c \
                             beepy-nav/src/route.c beepy-nav/src/gpx.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_search.c beepy-nav/src/search.c \
		beepy-nav/src/router.c beepy-nav/src/route.c \
		beepy-nav/src/gpx.c $(LDLIBS)

# 25 000 points is 1.2 MB -- bigger than the rest of the repo, and it would
# be rsynced to the device on every sync. Generated, not committed.
beepy-nav/tests/gpx/oversize.gpx: beepy-nav/tests/gpx/gen_oversize.py
	python3 $< $@

host/%.o: libbeepyfb/%.c $(HDRS)
	@mkdir -p host
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

host/%.o: libnmea/%.c $(HDRS)
	@mkdir -p host
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

host/%.o: beepy-nav/src/%.c $(HDRS)
	@mkdir -p host
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

tables:
	cd beepy-nav && python3 ../tools/gen_tables.py

# M2 acceptance: the C pages against mockup.py's own frames. Mac lane only
# (it renders the reference through Pillow); see tools/design_gate.py.
design-gate: host/beepy-nav
	python3 tools/design_gate.py --bin host/beepy-nav

# ------------------------------------------------------- the basemap fixture
#
# Mac lane only: mktiles.py wants Pillow and the 450 KB Overpass extract, and
# neither is on the device (osm-asok.json is excluded from `sync` by name).
#
# DETERMINISM is the point of the first half. The pack is a committed binary
# and the golden below it is a frozen frame, so "the same extract and the same
# route produce the same bytes" is not a nicety -- without it a rebuilt pack
# would silently move a golden. It is asserted the only way it can be: build
# twice into different files and compare, then compare against the committed
# fixture, which is the same claim across machines and across months.
test-tiles: host/beepy-nav $(TILEPACK)
	@echo "--- T-TILES-DETERMINISM: same inputs, same bytes, twice"
	python3 tools/mktiles.py --osm $(TILEOSM) --route $(TILEROUTE) \
		$(TILEOPTS) -o out-tiles-1.tiles --quiet
	python3 tools/mktiles.py --osm $(TILEOSM) --route $(TILEROUTE) \
		$(TILEOPTS) -o out-tiles-2.tiles --quiet
	cmp out-tiles-1.tiles out-tiles-2.tiles
	cmp out-tiles-1.tiles $(TILEPACK)
	@echo "--- T-TILES-FRAME: the committed golden comes back from the pack"
	./host/beepy-nav --demo --page nav-tiles \
		--basemap out-tiles-1.tiles --dump out-nav-tiles-rebuilt.fb
	cmp goldens/nav-tiles.fb out-nav-tiles-rebuilt.fb
#	tools/mergetiles.py joins packs cut over different areas so one basemap can
#	be detailed where you ride and coarse across a country. The claim that has
#	to hold is that joining changes NOTHING about the rungs it carries over: the
#	same golden, drawn from a pack that now also holds two coarser rungs, has to
#	come back byte-identical. If the tile offsets were rewritten wrongly the
#	frame moves, and no other assertion here would notice.
	@echo "--- T-TILES-MERGE: a joined pack draws the same golden"
	python3 tools/mktiles.py --osm $(TILEOSM) --route $(TILEROUTE) \
		--corridor 1000 --zooms 10,15 -o out-tiles-coarse.tiles --quiet
	python3 tools/mergetiles.py $(TILEPACK) out-tiles-coarse.tiles \
		-o out-tiles-merged-1.tiles --quiet
	python3 tools/mergetiles.py $(TILEPACK) out-tiles-coarse.tiles \
		-o out-tiles-merged-2.tiles --quiet
	cmp out-tiles-merged-1.tiles out-tiles-merged-2.tiles
	python3 tools/mktiles.py --info out-tiles-merged-1.tiles | \
		grep -q "5 zooms  41 tiles"
	./host/beepy-nav --demo --page nav-tiles \
		--basemap out-tiles-merged-1.tiles --dump out-nav-tiles-merged.fb
	cmp goldens/nav-tiles.fb out-nav-tiles-merged.fb
#	Joining a pack to itself is the degenerate case, and it is the one that
#	catches an off-by-one in the index rewrite: every rung is already present,
#	so the output must be the input, byte for byte.
	@echo "--- T-TILES-MERGE-IDEMPOTENT: a pack joined to itself is itself"
	python3 tools/mergetiles.py $(TILEPACK) $(TILEPACK) \
		-o out-tiles-merged-3.tiles --quiet
	cmp $(TILEPACK) out-tiles-merged-3.tiles
#	DESIGN.md 6.5: a tile is addressed as floor(e / mpp) in the PACK's frame,
#	so joining packs cut against different references would silently name
#	different ground. The tool has to refuse rather than produce a plausible
#	pack, and that refusal is a behaviour, so it is tested.
	@echo "--- T-TILES-MERGE-FRAME: a different reference is refused"
	python3 tools/mktiles.py --osm $(TILEOSM) --ref $(ROADREF) --radius 800 \
		--zooms 10 -o out-tiles-elsewhere.tiles --quiet
	! python3 tools/mergetiles.py $(TILEPACK) out-tiles-elsewhere.tiles \
		-o out-tiles-bad.tiles 2>/dev/null
	test ! -f out-tiles-bad.tiles
	rm -f out-tiles-1.tiles out-tiles-2.tiles out-tiles-coarse.tiles \
		out-tiles-merged-1.tiles out-tiles-merged-2.tiles \
		out-tiles-merged-3.tiles out-tiles-elsewhere.tiles \
		out-nav-tiles-merged.fb
	@echo "test-tiles: PASS"

# ---------------------------------------------------- the road pack fixture
#
# Mac lane only: mkpack.py wants the 450 KB Overpass extract, which is excluded
# from `sync` by name.
#
# DETERMINISM matters more here than it does for the tiles. The tile pack feeds
# one golden; these three feed a golden, every assertion in tests/test_search.c,
# and T-ONEWAY's three hand-picked NODE INDICES -- which are only meaningful
# while the node numbering is stable. So: build each of them twice, compare the
# two, then compare against the committed copy.
test-roads: $(ROADPACK) $(ROADOPEN) $(ROADNAMES)
	@echo "--- T-ROADS-DETERMINISM: same extract, same bytes, twice"
	python3 tools/mkpack.py --osm $(ROADOSM) --route $(TILEROUTE) \
		-o out-roads-1.roads --quiet
	python3 tools/mkpack.py --osm $(ROADOSM) --route $(TILEROUTE) \
		-o out-roads-2.roads --quiet
	cmp out-roads-1.roads out-roads-2.roads
	cmp out-roads-1.roads $(ROADPACK)
	python3 tools/mkpack.py --osm $(ROADOSM) --route $(TILEROUTE) \
		--ignore-oneway -o out-roads-3.roads --quiet
	cmp out-roads-3.roads $(ROADOPEN)
	! cmp -s out-roads-1.roads out-roads-3.roads
	python3 tools/mkpack.py --osm $(ROADNAMESIN) --ref $(ROADREF) \
		-o out-roads-4.roads --quiet
	cmp out-roads-4.roads $(ROADNAMES)
	rm -f out-roads-1.roads out-roads-2.roads out-roads-3.roads \
		out-roads-4.roads
#	The name index, from the outside: DESIGN.md 1.4 says only what is in the
#	pack is searchable and that the count of what was dropped is honest, so both
#	numbers are asserted here rather than left to the C tests alone.
	@echo "--- T-ROADS-NAMES: name:en, the ASCII fallback, and what was dropped"
	python3 tools/mkpack.py --info $(ROADPACK) | \
		grep -q "557 ways indexed, 3 names dropped"
	python3 tools/mkpack.py --info $(ROADNAMES) | \
		grep -q "5 ways indexed, 1 names dropped"
	python3 tools/mkpack.py --info $(ROADNAMES) | grep -q "'MAIN ROAD'"
	python3 tools/mkpack.py --info $(ROADNAMES) | grep -q "'SECOND STREET'"
#	Destinations -- the half of the index osm-asok.json cannot reach, because it
#	is a highway-only extract and every branch that reads a `node` is dead in
#	it. tests/roads/pois.json is one road and four things beside it, one per
#	case. Asserted through --info rather than by comparing a committed pack,
#	because what is being claimed here is WHICH NAMES ARE SEARCHABLE, and that
#	reads better as the names themselves than as a digest.
	@echo "--- T-ROADS-POIS: a school is somewhere to go, an unnamed thing is not"
	python3 tools/mkpack.py --osm $(ROADPOIS) --ref $(ROADREF) \
		-o out-roads-pois.roads --quiet
	python3 tools/mkpack.py --info out-roads-pois.roads | grep -q "'THE ACADEMY'"
	python3 tools/mkpack.py --info out-roads-pois.roads | \
		grep -q "'CENTRAL STATION'"
#	name:en beats a Thai name; a Thai-only name is dropped AND counted, and the
#	count is the header's, which is the number the device reports. Before
#	destinations existed that number covered streets only, and a POI dropped
#	for the same reason would have been invisible in it.
	python3 tools/mkpack.py --info out-roads-pois.roads | \
		grep -q "1 ways indexed, 1 names dropped"
#	amenity=parking with no name: a POI key is not enough. Nothing unnamed can
#	be searched for, so indexing it would cost bytes to no end.
	! python3 tools/mkpack.py --info out-roads-pois.roads | grep -q "'PARKING'"
#	A destination whose name is a street's merges into ONE place carrying both
#	sets of points -- 2 from the road's every-third-vertex, 1 from the shop.
	python3 tools/mkpack.py --info out-roads-pois.roads | \
		grep -q "'NORTH ROAD'  3 points"
#	--no-pois rebuilds the pack as it was, which is what makes the pair above a
#	pair: without it "destinations are indexed" could pass on a build that
#	indexes everything and always did.
	python3 tools/mkpack.py --osm $(ROADPOIS) --ref $(ROADREF) --no-pois \
		-o out-roads-nopois.roads --quiet
	! python3 tools/mkpack.py --info out-roads-nopois.roads | \
		grep -q "'THE ACADEMY'"
	python3 tools/mkpack.py --info out-roads-nopois.roads | \
		grep -q "'NORTH ROAD'  2 points"
	! cmp -s out-roads-pois.roads out-roads-nopois.roads
#	Determinism, for the reason every other pack here has it: same extract,
#	same bytes.
	python3 tools/mkpack.py --osm $(ROADPOIS) --ref $(ROADREF) \
		-o out-roads-pois-2.roads --quiet
	cmp out-roads-pois.roads out-roads-pois-2.roads
#	And the claim that costs nothing to make and everything to get wrong: an
#	extract with no `node` elements at all builds the SAME pack it always did.
#	osm-asok.json is that extract, and $(ROADPACK) is the committed answer.
	@echo "--- T-ROADS-POIS-INERT: a highway-only extract is untouched"
	python3 tools/mkpack.py --osm $(ROADOSM) --route $(TILEROUTE) \
		-o out-roads-inert.roads --quiet
	cmp out-roads-inert.roads $(ROADPACK)
	rm -f out-roads-pois.roads out-roads-pois-2.roads out-roads-nopois.roads \
		out-roads-inert.roads
	@echo "test-roads: PASS"

# Rebuilding the fixtures is deliberate, like regenerating a golden.
roads:
ifndef ROADS_OK
	$(error refusing to rebuild the committed packs without ROADS_OK=1)
endif
	python3 tools/mkpack.py --osm $(ROADOSM) --route $(TILEROUTE) \
		-o $(ROADPACK)
	python3 tools/mkpack.py --osm $(ROADOSM) --route $(TILEROUTE) \
		--ignore-oneway -o $(ROADOPEN)
	python3 tools/mkpack.py --osm $(ROADNAMESIN) --ref $(ROADREF) \
		-o $(ROADNAMES)

# Rebuilding the fixture is deliberate, like regenerating a golden.
tiles: 
ifndef TILES_OK
	$(error refusing to rebuild the committed pack without TILES_OK=1)
endif
	python3 tools/mktiles.py --osm $(TILEOSM) --route $(TILEROUTE) \
		$(TILEOPTS) -o $(TILEPACK)

# -------------------------------------------------------- Mac -> device
#
# --exclude-from=.gitignore is what keeps Mac build products out of the
# device tree: rsync preserves their mtimes, so a Mach-O beepy-nav or
# test_map lands looking newer than its sources and make happily runs it
# ("Syntax error: "(" unexpected" is the shell trying to execute an ELF's
# evil twin). The .gitignore patterns are anchored the same way rsync's are,
# so the one list serves both.
sync:
	rsync -az -e "ssh -i $(SSHKEY)" --exclude-from=.gitignore \
		--exclude .git --exclude beepy/ --exclude beepy-buildroot/ \
		--exclude '*.png' --exclude '*.tar.gz' --exclude sim-anim.bin \
		--exclude osm-asok.json --exclude host/ \
		./ $(DEVICE):$(REMOTE_DIR)/
	ssh -i $(SSHKEY) $(DEVICE) 'make -C $(REMOTE_DIR) check'

clean:
	rm -f gps-monitor/gps-monitor beepy-nav/beepy-nav \
		$(UNIT_TESTS) beepy-nav/tests/gpx/oversize.gpx out-*.fb out-*.png \
		$(RDIR)/*.nmea $(RDIR)/*.tsv $(RDIR)/*.fb $(RDIR)/*.log \
		out-tiles-*.tiles out-roads-*.roads out-*.roads \
		beepy-nav/tests/test_tile beepy-nav/tests/test_search \
		*.o */*.o */*/*.o *.a */*.a
	rm -rf host

.PHONY: all check goldens host test-unit test-replay test-frames test-find \
	host-replay tables design-gate test-tiles tiles test-roads roads \
	bench sync clean

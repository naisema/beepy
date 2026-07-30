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
ROADREF    ?= 13.740,100.560

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
            beepy-nav/src/router.o
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
#	The basemap of DESIGN.md 6.5. The Asok pack is a committed fixture, not a
#	generated one: tools/mktiles.py needs Pillow and osm-asok.json, and the
#	device has neither -- 264 KB of tiles is the price of `check` meaning the
#	same thing in both lanes.
	./beepy-nav/beepy-nav --demo --page nav-tiles \
		--basemap $(TILEPACK) --dump out-nav-tiles.fb
	./beepy-nav/beepy-nav --demo --page overview-tiles \
		--dump out-nav-overview-tiles.fb
	cmp goldens/nav-turn.fb     out-nav-turn.fb
	cmp goldens/nav-off.fb      out-nav-off.fb
	cmp goldens/nav-nofix.fb    out-nav-nofix.fb
	cmp goldens/nav-arrows.fb   out-nav-arrows.fb
	cmp goldens/nav-overview.fb out-nav-overview.fb
	cmp goldens/nav-tiles.fb    out-nav-tiles.fb
	cmp goldens/nav-overview-tiles.fb out-nav-overview-tiles.fb
#	The five goldens above nav-tiles are rendered with NO pack, and that is
#	the standing proof that the tile layer is optional: a basemap that is
#	absent must render exactly what rendered before basemaps existed. The two
#	comparisons below are the other half of it -- the same page with no pack
#	and with an unreadable one are the same frame, and neither is the frame
#	with the pack, so "draws nothing" cannot be passing by drawing nothing
#	ever.
	@echo "--- T-BASEMAP-OPTIONAL: absent and unreadable are the same frame"
	./beepy-nav/beepy-nav --demo --page nav-tiles --dump out-nav-tiles-none.fb
	./beepy-nav/beepy-nav --demo --page nav-tiles \
		--basemap no-such-pack.tiles --dump out-nav-tiles-bad.fb
	cmp out-nav-tiles-none.fb out-nav-tiles-bad.fb
	! cmp -s out-nav-tiles.fb out-nav-tiles-none.fb
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
	./beepy-nav/beepy-nav --demo --page nav-tiles \
		--basemap $(TILEPACK) --dump goldens/nav-tiles.fb
	./beepy-nav/beepy-nav --demo --page overview-tiles \
		--dump goldens/nav-overview-tiles.fb
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
	@echo "test-frames: PASS"

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

# -------------------------------------------------------------------- host
#
# Compile the portable objects on the Mac so parser/renderer edits get a
# warning pass without a device round-trip. fbdev/input/serial are device-only.

HOST_OBJS = host/canvas.o host/font.o host/cover.o host/dump.o \
            host/nmea.o host/gps.o \
            host/seg.o host/arrows.o host/map.o host/gpx.o host/route.o \
            host/view_nav.o host/view_overview.o host/fix.o host/chooser.o \
            host/led.o host/config.o host/ridelog.o host/tile.o \
            host/search.o host/router.o \
            host/nav.o

# beepy-nav is portable end to end (no fbdev, no evdev), so the Mac can link
# and run it -- which is what makes the M2 design gate a fast loop.
HOST_NAV = host/nav.o host/view_nav.o host/view_overview.o host/seg.o \
           host/arrows.o host/map.o host/gpx.o host/route.o host/fix.o \
           host/chooser.o host/led.o host/config.o host/ridelog.o \
           host/tile.o host/search.o host/router.o \
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
	rm -f out-tiles-1.tiles out-tiles-2.tiles
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
		$(UNIT_TESTS) beepy-nav/tests/gpx/oversize.gpx out-*.fb \
		$(RDIR)/*.nmea $(RDIR)/*.tsv $(RDIR)/*.fb $(RDIR)/*.log \
		out-tiles-*.tiles out-roads-*.roads out-*.roads \
		beepy-nav/tests/test_tile beepy-nav/tests/test_search \
		*.o */*.o */*/*.o *.a */*.a
	rm -rf host

.PHONY: all check goldens host test-unit test-replay test-frames \
	host-replay tables design-gate test-tiles tiles test-roads roads \
	bench sync clean

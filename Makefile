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
# The travel-mode fixture (DESIGN.md 7.7): a motorway and a residential
# dogleg between the same two points, so "the mode changed the route" is a
# length and not a flag. Read by tests/test_search.c, which runs on the device.
ROADMODES  ?= beepy-nav/tests/roads/modes.json
# The class-WEIGHTING fixture (DESIGN.md 7.7.2), which modes.json cannot be: a
# short residential zigzag against a LONGER primary, so the big road has to win
# on cost while losing on distance. Over modes.json the big road is already the
# shorter one and a weighting test there could not fail.
ROADBIAS   ?= beepy-nav/tests/roads/bias.json
ROADTOLLS  ?= beepy-nav/tests/roads/tolls.json
# DESIGN.md 7.8's fetch fixtures: a deliberately slow fetcher, and the tiny
# moving route the paced replay follows while it is in flight.
NETCONF    ?= beepy-nav/tests/net/slow.conf
SHORTGPX   ?= beepy-nav/tests/gpx/short.gpx
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

# beepy-vid fixtures. The gray8 and the PCM are GENERATED (tools/mkvidfix.py,
# pure stdlib, deterministic from a seed) and gitignored, following the .nmea
# replays at line 349 rather than the committed tile pack. The .vid itself IS
# committed: mkvid.py needs numpy, the device has none, and `check` will want
# this pack for the player's demo pages.
VIDFIX   = beepy-vid/tests/vid
VIDGRAY  = $(VIDFIX)/clip.gray8
VIDPCM   = $(VIDFIX)/clip.s16
VIDPACK  = $(VIDFIX)/clip.vid
VIDOPTS  = --size 400x225 --fps 24 --gop 24 --hysteresis 8 --dither bayer4 \
           --no-deflate

FB_OBJS   = libbeepyfb/canvas.o libbeepyfb/font.o libbeepyfb/cover.o \
            libbeepyfb/dump.o libbeepyfb/expand.o libbeepyfb/fbdev.o \
            libbeepyfb/input.o
NMEA_OBJS = libnmea/nmea.o libnmea/gps.o libnmea/serial.o
GM_OBJS   = gps-monitor/main.o gps-monitor/view_bars.o gps-monitor/view_sky.o
NAV_OBJS  = beepy-nav/src/nav.o beepy-nav/src/view_nav.o beepy-nav/src/seg.o \
            beepy-nav/src/arrows.o beepy-nav/src/map.o beepy-nav/src/gpx.o \
            beepy-nav/src/route.o beepy-nav/src/view_overview.o \
            beepy-nav/src/fix.o beepy-nav/src/chooser.o beepy-nav/src/led.o \
            beepy-nav/src/config.o beepy-nav/src/ridelog.o \
            beepy-nav/src/tile.o beepy-nav/src/search.o \
            beepy-nav/src/roadgrid.o beepy-nav/src/persp.o \
            beepy-nav/src/nav3d.o \
            beepy-nav/src/router.o beepy-nav/src/view_find.o \
            beepy-nav/src/view_confirm.o beepy-nav/src/view_map.o \
            beepy-nav/src/view_quit.o beepy-nav/src/view_save.o \
            beepy-nav/src/netfetch.o \
            beepy-nav/src/netroute.o
HDRS      = $(wildcard libbeepyfb/*.h libnmea/*.h gps-monitor/*.h \
                       beepy-nav/src/*.h beepy-vid/src/*.h)

VID_OBJS  = beepy-vid/src/vid.o beepy-vid/src/pack.o beepy-vid/src/codec.o \
            beepy-vid/src/view_play.o beepy-vid/src/view_pages.o beepy-vid/src/audio.o
VIDLIBS   = -lz

all: gps-monitor/gps-monitor beepy-nav/beepy-nav beepy-vid/beepy-vid

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(FB_OBJS) $(NMEA_OBJS) $(GM_OBJS) $(NAV_OBJS): $(HDRS)

beepy-vid/beepy-vid: $(VID_OBJS) libbeepyfb/libbeepyfb.a
	$(CC) $(CFLAGS) -o $@ $(VID_OBJS) libbeepyfb/libbeepyfb.a \
		$(LDLIBS) $(VIDLIBS)

$(VID_OBJS): $(HDRS)

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

check: gps-monitor/gps-monitor beepy-nav/beepy-nav beepy-vid/beepy-vid \
       test-unit test-replay
	./gps-monitor/gps-monitor --demo --page bars --dump out-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump out-sky.fb
	cmp goldens/gm-bars.fb out-bars.fb
	cmp goldens/gm-sky.fb  out-sky.fb
	./beepy-nav/beepy-nav --demo --page nav     --dump out-nav-turn.fb
	./beepy-nav/beepy-nav --demo --page nav-off --dump out-nav-off.fb
	./beepy-nav/beepy-nav --demo --page nav-nofix --dump out-nav-nofix.fb
#	DESIGN.md 7.11's question, over the OFF ROUTE panel it can only appear on.
	./beepy-nav/beepy-nav --demo --page nav-ask   --dump out-nav-ask.fb
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
#	DESIGN.md 1.5.1's two states: the map held off-centre, and the question a
#	swipe raises while moving.
	./beepy-nav/beepy-nav --demo --page map-pan     --dump out-nav-map-pan.fb
	./beepy-nav/beepy-nav --demo --page map-pan-ask --dump out-nav-map-pan-ask.fb
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
#	DESIGN.md 7.10's armed ENTER. The whole state is twenty characters in the
#	title bar, and a string is the thing no other assertion in this repo can
#	see: T-CAR-RETRY watches stderr and g_mode, so a reworded, truncated or
#	FIND-overrunning bar would pass it. This frame is the only test that reads
#	what the rider reads.
	./beepy-nav/beepy-nav --demo --page find-toofar \
		--roads $(ROADPACK) --dump out-nav-find-toofar.fb
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
#	The SAVE page of DESIGN.md 1.4.8, in both states of its field. Two frames
#	because the difference between them IS the feature: a default drawn as a
#	SELECTION that the first keystroke replaces, against an edited name with a
#	caret after it. A build that drew the second for both would look perfectly
#	reasonable and would have thrown away the only warning a rider gets before
#	their typing eats the default.
	./beepy-nav/beepy-nav --demo --page save --dump out-nav-save.fb
	./beepy-nav/beepy-nav --demo --page save-typed \
		--dump out-nav-save-typed.fb
	./beepy-nav/beepy-nav --demo --page quit     --dump out-nav-quit.fb
	./beepy-nav/beepy-nav --demo --page quit-map --dump out-nav-quit-map.fb
	cmp goldens/nav-turn.fb     out-nav-turn.fb
	cmp goldens/nav-off.fb      out-nav-off.fb
	cmp goldens/nav-nofix.fb    out-nav-nofix.fb
	cmp goldens/nav-ask.fb      out-nav-ask.fb
#	And it is not the OFF ROUTE panel with nothing added: without this, a build
#	where ask_reroute drew nothing at all would pass the cmp above and this one.
	! cmp -s goldens/nav-ask.fb goldens/nav-off.fb
	cmp goldens/nav-arrows.fb   out-nav-arrows.fb
	cmp goldens/nav-overview.fb out-nav-overview.fb
#	DESIGN.md 6.6's frozen 3D state. --view3d is required AFTER --demo, which
#	pins 2D: the goldens must not change because a rider set view3d in their
#	config, and this is the one page that asks for it explicitly.
	./beepy-nav/beepy-nav --demo --page nav-3d --view3d \
		--roads $(ROADPACK) --dump out-nav-3d.fb
	cmp goldens/nav-3d.fb        out-nav-3d.fb
#	And with no pack it falls back rather than drawing nothing -- the same
#	"the layer is optional" claim nav-tiles makes one level down.
	./beepy-nav/beepy-nav --demo --page nav-3d --view3d --dump out-nav-3d-nopack.fb
	! cmp -s goldens/nav-3d.fb out-nav-3d-nopack.fb
	cmp goldens/nav-map.fb       out-nav-map.fb
	cmp goldens/nav-map-nofix.fb out-nav-map-nofix.fb
	cmp goldens/nav-map-wait.fb  out-nav-map-wait.fb
	cmp goldens/nav-map-pan.fb     out-nav-map-pan.fb
	cmp goldens/nav-map-pan-ask.fb out-nav-map-pan-ask.fb
#	Neither is the centred page with nothing added, and they are not each other:
#	a build that drew no pan, or drew the pan but not the question, would pass
#	both cmps above and fail here.
	! cmp -s goldens/nav-map-pan.fb     goldens/nav-map.fb
	! cmp -s goldens/nav-map-pan-ask.fb goldens/nav-map.fb
	! cmp -s goldens/nav-map-pan.fb     goldens/nav-map-pan-ask.fb
	cmp goldens/nav-map-tiles.fb out-nav-map-tiles.fb
	cmp goldens/nav-tiles.fb    out-nav-tiles.fb
	cmp goldens/nav-overview-tiles.fb out-nav-overview-tiles.fb
	cmp goldens/nav-find.fb     out-nav-find.fb
	cmp goldens/nav-find-none.fb out-nav-find-none.fb
	cmp goldens/nav-find-toofar.fb out-nav-find-toofar.fb
#	nav-find-saved's lesson applied before it could be repeated: the TOO FAR
#	demo renders the SAME query and pack as nav-find deliberately, so if the
#	title bar ever stopped being drawn the frame would come back byte-identical
#	to nav-find and freeze nothing at all.
	! cmp -s goldens/nav-find-toofar.fb goldens/nav-find.fb
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
	cmp goldens/nav-save.fb       out-nav-save.fb
	cmp goldens/nav-save-typed.fb out-nav-save-typed.fb
#	And the two are not one frame, which is the whole reason there are two:
#	nav-find-saved's lesson (line 241) applied to a page whose two states differ
#	only in how one field is drawn.
	! cmp -s goldens/nav-save.fb goldens/nav-save-typed.fb
	cmp goldens/nav-quit.fb     out-nav-quit.fb
	cmp goldens/nav-quit-map.fb out-nav-quit-map.fb
#	beepy-vid's pages. The player is portable, so these frames are rendered
#	identically by host/beepy-vid on the Mac and by the device binary here --
#	which is itself the assertion that the two renderers have not diverged.
	./beepy-vid/beepy-vid --demo --page play --pack $(VIDPACK) --at 12 \
		--dump out-vid-play.fb
	./beepy-vid/beepy-vid --demo --page play --pack $(VIDPACK) --at 12 \
		--paused --dump out-vid-paused.fb
	./beepy-vid/beepy-vid --demo --page play --no-pack --dump out-vid-nopack.fb
	cmp goldens/vid-play.fb   out-vid-play.fb
	cmp goldens/vid-paused.fb out-vid-paused.fb
	cmp goldens/vid-nopack.fb out-vid-nopack.fb
#	Not the same frame with nothing added: a build whose band drew no state
#	would pass both cmps above and freeze nothing at all.
	! cmp -s goldens/vid-play.fb goldens/vid-paused.fb
#	T-VID-OPTIONAL, the shape T-BASEMAP-OPTIONAL argues for at line 240: an
#	unreadable pack and an absent one must be the SAME frame, and --no-pack is
#	an explicit fixture rather than the absence of a flag, because "no flag"
#	would one day read whatever a config file names.
	@echo "--- T-VID-OPTIONAL: absent and unreadable are the same frame"
	./beepy-vid/beepy-vid --demo --page play --pack no-such.vid \
		--dump out-vid-badpack.fb
	cmp out-vid-nopack.fb out-vid-badpack.fb
	! cmp -s out-vid-play.fb out-vid-nopack.fb
#	The pages that are not the film. --page library uses a FIXTURE compiled
#	into the binary, never a directory scan: a page that listed ~/videos
#	would photograph one device and move whenever a file was added there.
	./beepy-vid/beepy-vid --demo --page library --dump out-vid-library.fb
	./beepy-vid/beepy-vid --demo --page library-empty \
		--dump out-vid-library-empty.fb
	./beepy-vid/beepy-vid --demo --page end  --dump out-vid-end.fb
	./beepy-vid/beepy-vid --demo --page help --dump out-vid-help.fb
	./beepy-vid/beepy-vid --demo --page sync --dump out-vid-sync.fb
	./beepy-vid/beepy-vid --demo --page play --pack $(VIDPACK) --at 12 \
		--transient "-30S -> 0:04" --dump out-vid-transient.fb
	cmp goldens/vid-library.fb       out-vid-library.fb
	cmp goldens/vid-library-empty.fb out-vid-library-empty.fb
	cmp goldens/vid-end.fb           out-vid-end.fb
	cmp goldens/vid-help.fb          out-vid-help.fb
	cmp goldens/vid-sync.fb          out-vid-sync.fb
	cmp goldens/vid-transient.fb     out-vid-transient.fb
#	Each of these would pass as a frozen blank if the page drew nothing.
	! cmp -s goldens/vid-library.fb goldens/vid-library-empty.fb
	! cmp -s goldens/vid-end.fb     goldens/vid-help.fb
	! cmp -s out-vid-transient.fb   out-vid-play.fb
	! cmp -s out-vid-paused.fb      out-vid-transient.fb
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

goldens: gps-monitor/gps-monitor beepy-nav/beepy-nav beepy-vid/beepy-vid
ifndef GOLDEN_OK
	$(error refusing to regenerate goldens without GOLDEN_OK=1)
endif
	./gps-monitor/gps-monitor --demo --page bars --dump goldens/gm-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump goldens/gm-sky.fb
	./beepy-nav/beepy-nav --demo --page nav     --dump goldens/nav-turn.fb
	./beepy-nav/beepy-nav --demo --page nav-3d --view3d \
		--roads $(ROADPACK) --dump goldens/nav-3d.fb
	./beepy-nav/beepy-nav --demo --page nav-off --dump goldens/nav-off.fb
	./beepy-nav/beepy-nav --demo --page nav-nofix --dump goldens/nav-nofix.fb
	./beepy-nav/beepy-nav --demo --page nav-ask   --dump goldens/nav-ask.fb
	./beepy-nav/beepy-nav --demo --page arrows  --dump goldens/nav-arrows.fb
	./beepy-nav/beepy-nav --demo --page overview --dump goldens/nav-overview.fb
	./beepy-nav/beepy-nav --demo --page map \
		--dump goldens/nav-map.fb
	./beepy-nav/beepy-nav --demo --page map-nofix \
		--dump goldens/nav-map-nofix.fb
	./beepy-nav/beepy-nav --demo --page map-wait \
		--dump goldens/nav-map-wait.fb
	./beepy-nav/beepy-nav --demo --page map-pan     --dump goldens/nav-map-pan.fb
	./beepy-nav/beepy-nav --demo --page map-pan-ask \
		--dump goldens/nav-map-pan-ask.fb
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
	./beepy-nav/beepy-nav --demo --page find-toofar \
		--roads $(ROADPACK) --dump goldens/nav-find-toofar.fb
	./beepy-nav/beepy-nav --demo --page confirm --dump goldens/nav-confirm.fb
	./beepy-nav/beepy-nav --demo --page find-saved \
		--roads $(ROADPACK) --dump goldens/nav-find-saved.fb
	./beepy-nav/beepy-nav --demo --page map-saved \
		--basemap $(TILEPACK) --dump goldens/nav-map-saved.fb
	./beepy-nav/beepy-nav --demo --page map-wait-home \
		--basemap $(TILEPACK) --dump goldens/nav-map-wait-home.fb
	./beepy-nav/beepy-nav --demo --page save --dump goldens/nav-save.fb
	./beepy-nav/beepy-nav --demo --page save-typed \
		--dump goldens/nav-save-typed.fb
	./beepy-nav/beepy-nav --demo --page quit     --dump goldens/nav-quit.fb
	./beepy-nav/beepy-nav --demo --page quit-map --dump goldens/nav-quit-map.fb
	./beepy-vid/beepy-vid --demo --page play --pack $(VIDPACK) --at 12 \
		--dump goldens/vid-play.fb
	./beepy-vid/beepy-vid --demo --page play --pack $(VIDPACK) --at 12 \
		--paused --dump goldens/vid-paused.fb
	./beepy-vid/beepy-vid --demo --page play --no-pack \
		--dump goldens/vid-nopack.fb
	./beepy-vid/beepy-vid --demo --page library --dump goldens/vid-library.fb
	./beepy-vid/beepy-vid --demo --page library-empty \
		--dump goldens/vid-library-empty.fb
	./beepy-vid/beepy-vid --demo --page end  --dump goldens/vid-end.fb
	./beepy-vid/beepy-vid --demo --page help --dump goldens/vid-help.fb
	./beepy-vid/beepy-vid --demo --page sync --dump goldens/vid-sync.fb
	./beepy-vid/beepy-vid --demo --page play --pack $(VIDPACK) --at 12 \
		--transient "-30S -> 0:04" --dump goldens/vid-transient.fb
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
             $(RDIR)/rough.nmea $(RDIR)/still.nmea $(RDIR)/nofix.nmea \
             $(RDIR)/short.nmea
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

# DESIGN.md 7.11's two fixtures, and they exist as a PAIR because the two brakes
# on rerouting have to be told apart. Both are the same 300 m excursion off the
# same loop; only the speed differs, and the speed is what decides how long the
# rider spends past the 200 m trigger.
#
#   reroute.nmea       25 km/h, 105 s past 200 m -> the 60 s spacing allows 2,
#                      the cap allows 5. Exactly 2 can only be the spacing.
#   reroute-long.nmea   6 km/h, 436 s past 200 m -> the spacing allows 8, the cap
#                      allows 5. Exactly 5 can only be the cap.
#
# One fixture could not do it: at any single speed where the two bounds agree,
# a build with either brake missing still passes. The windows are measured, not
# assumed -- tools/assert_trace.py reads off_m out of the same trace.
$(RDIR)/reroute.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --speed 25 --detour 300:2000:3000 --seed 5 -o $@
$(RDIR)/reroute-long.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --speed 6 --detour 300:2000:3000 --seed 5 -o $@

# The same standstill, twenty-six seconds of it, for T-FIND-ONLINE's FETCHING and
# TIMED OUT frames (DESIGN.md 7.10). Three things about it are load-bearing.
#
# PACED, because netfetch measures its deadline on the WALL clock and a fetcher
# is only slow in real time. STATIONARY, because the claim is that nothing on the
# FIND page changes except the title bar -- a byte comparison, which a moving
# rider would break legitimately by changing every distance in the list.
#
# And TWENTY-SIX SECONDS, which is the deadline plus the fetch's start plus
# margin. Under pacing the ride clock can only LAG the wall clock, never lead it,
# so "the deadline has passed by ride second 23" follows from 23 > 11 + 10 with
# no timing assumption at all. The first version of this test used a 1.5 s
# fetcher and a dump at a fixed ride second, and passed or failed depending on
# how much disk I/O the run had done -- ride time lags further behind under load,
# so the arrival moved. A fetcher that NEVER answers has no such race.
$(RDIR)/still-net.nmea: tools/mknmea.py $(RROUTE)
	$(MKNMEA) --stationary 11@0 --hold-jitter 0 --duration 26 --seed 2 -o $@

# DESIGN.md 7.8's fixture, and the only one ridden in REAL TIME: T-NET-FETCH is
# paced, so twelve seconds of fixture costs twelve seconds of gate. Short for
# that reason, and MOVING because 6.3 drops a stopped ride to 1 Hz and there
# would be no 8 Hz left to measure. 33 km/h over 111 m is twelve seconds.
$(RDIR)/short.nmea: tools/mknmea.py $(SHORTGPX)
	python3 tools/mknmea.py --gpx $(SHORTGPX) --speed 33 --hz 1 -o $@

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
#	--config /dev/null, and it has to be here rather than only where the frames
#	are COMPARED. plain-a is the reference for T-CUE-LED-MUTE's note-a and for
#	T-FIND-NOPACK's nopack-a, and all three runs used to read
#	~/.config/beepy-nav.conf. They agreed -- not because the frames were
#	independent of the config, but because all three read the SAME config on
#	whichever machine ran the gate. Pinning one of the three and not the others
#	made the difference visible: 2981 px, which is the device's own basemap
#	drawn behind the map. So the family is pinned together, and each of these
#	frames is now a function of the fixtures alone.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/ride.nmea --headless $(FPS8) \
		--config /dev/null \
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
		--config /dev/null --key 120:l --key 250:l \
		--dump-at 120.5:$(RDIR)/note-a.fb \
		--dump-at 122.5:$(RDIR)/note-b.fb \
		--trace-frames $(RDIR)/mute-frames.tsv
	$(ASSERT) $(RDIR)/mute-frames.tsv --led-quiet 120 250 --led-rungs 23 \
		--cue-led 7
	@echo "    ... and the transient takes the bottom row and gives it back"
	python3 tools/fbdiff.py $(RDIR)/note-a.fb $(RDIR)/plain-a.fb \
		--mask 0,216,127,239 --max-px 0
	python3 tools/fbdiff.py $(RDIR)/note-b.fb $(RDIR)/plain-b.fb --max-px 0
	@echo "--- T-NET-FETCH: a fetch in flight costs the frame clock nothing"
#	DESIGN.md 7.8. A fetch takes about 1.4 s and the loop runs at 8 Hz, so a
#	blocking implementation leaves ONE 1400 ms hole and 96 perfect frames
#	either side -- which an average cannot see. Hence a maximum, and nothing
#	else.
#
#	--pace and a MOVING fixture, both load-bearing. Unpaced, the ride clock
#	outruns the wall clock and a gap in it means nothing; stationary, 6.3
#	drops the loop to 1 Hz on purpose and the first version of this test was
#	solemnly measuring the stop rate.
	$(NAV) --route $(SHORTGPX) --replay $(RDIR)/short.nmea --headless $(FPS8) \
		--pace --config $(NETCONF) --fetch-at 4 \
		--trace-frames $(RDIR)/fetch-frames.tsv 2> $(RDIR)/fetch.log
	grep -q "fetch started" $(RDIR)/fetch.log
#	And it finished. Without this the gap assertion would pass on a build
#	where the fetch never ran at all.
	grep -q "fetched 9 bytes" $(RDIR)/fetch.log
	$(ASSERT) $(RDIR)/fetch-frames.tsv --max-frame-gap 0.2
	@echo "--- T-SKIP: a display that stopped changing stops being sent"
	$(ASSERT) $(RDIR)/still-frames.tsv --settles presented
	@echo "--- T-NOFIX: thirty seconds with no fix (DESIGN.md 1.1)"
	$(NAV) --route $(RROUTE) --replay $(RDIR)/nofix.nmea --headless $(FPS8) \
		--config /dev/null \
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
test-find: $(NAV) $(RDIR)/asok.nmea $(RDIR)/ride.nmea $(ROADPACK) \
           $(RDIR)/still-net.nmea $(RDIR)/reroute.nmea \
           $(RDIR)/reroute-long.nmea $(RDIR)/detour.nmea
	@echo "--- T-FIND: F, a typed query, ENTER, CONFIRM, ENTER, riding"
#	--config /dev/null, for the reason T-SAVE states below and this test learned
#	the more expensive way. With no --config the run reads whatever
#	~/.config/beepy-nav.conf says, and on a device that has been ridden that
#	file says `prefer = online` -- so the assertion below stopped being about
#	the offline router and became about whichever routing server the owner last
#	configured. It failed on a device with no route to that server while the
#	code it tests was correct, and it would have PASSED on a broken offline
#	router as long as the network answered. Either way it was measuring the
#	config, not the program.
	$(NAV) --route $(TILEROUTE) --replay $(RDIR)/asok.nmea --headless $(FPS8) \
		--config /dev/null --no-view3d --roads $(ROADPACK) \
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
		--config /dev/null --no-roads --key 120:f \
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
	@echo "--- T-SAVE: S writes a place, and the program can then find it"
#	DESIGN.md 1.4.8. Everything before this could only READ favourites, so a
#	`place` line existed if a rider had carried a coordinate off the MAP strip as
#	far as a laptop. The claim here is the write: S opens a page, a typed name and
#	ENTER put one line in a file, and the place is in the live list before the
#	program exits.
#
#	--places INTO THE REPLAY DIRECTORY, and this is not a convenience. The rule
#	CLAUDE.md carries -- a test whose absent case reads the device config is not
#	a test -- applies with more force to a test that WRITES: a gate that appended
#	to ~/.config on every run would be editing the machine it is measuring, and
#	after four runs the device's own list would be full. The file is removed
#	first, so "one line" is a count and not a growth.
#
#	--config /dev/null, so the list starts EMPTY however many places the device's
#	owner has saved. Without it the ceiling assertion below would pass or fail
#	depending on whose Beepy `check` is running on. (`--config` also suppresses
#	the default places file, which is what makes the --places on the next line
#	the only one in play.)
	rm -f $(RDIR)/t-save.places $(RDIR)/t-save-none.places
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --roads $(ROADPACK) \
		--places $(RDIR)/t-save.places \
		--dump-at 19:$(RDIR)/save-map.fb \
		--key 20:s --dump-at 21:$(RDIR)/save-page.fb \
		--key 22:c --key 23:a --key 24:f --key 25:e \
		--dump-at 26:$(RDIR)/save-typed.fb \
		--key 27:enter --dump-at 29:$(RDIR)/save-done.fb \
		2> $(RDIR)/save.log
#	ONE line, and it is the name that was typed rather than the default that was
#	offered. `PLACE 1CAFE` is what this looked like before the default was drawn
#	and treated as a selection, and it passed a laxer grep.
	test `grep -c "^place = " $(RDIR)/t-save.places` -eq 1
	grep -q "^place = CAFE " $(RDIR)/t-save.places
#	The coordinate is the fix's, to the five decimals the MAP strip showed -- so
#	the file agrees with the screen the rider read it off. still.nmea sits at
#	13.72936, 100.56100.
	grep -q "^place = CAFE 13.72936,100.56100$$" $(RDIR)/t-save.places
	grep -q "beepy-nav: saved CAFE at 13.72936,100.56100" $(RDIR)/save.log
#	S opened a page: the frame changes, and it changes again as the name is
#	typed over the selected default. Two comparisons, because a build that
#	opened the page but ignored the keyboard would pass only the first.
	python3 tools/fbdiff.py $(RDIR)/save-page.fb $(RDIR)/save-map.fb --min-px 2000
	python3 tools/fbdiff.py $(RDIR)/save-typed.fb $(RDIR)/save-page.fb --min-px 200
#	ENTER went back to the MAP page, which is what says the page is not a trap.
#	Not byte-identical to the frame before S: the new place is now a mark on the
#	map and SAVED CAFE is on the strip's transient row.
	python3 tools/fbdiff.py $(RDIR)/save-done.fb $(RDIR)/save-typed.fb --min-px 2000
#	AND IT SURVIVES A RESTART, which is the end-to-end claim and the one thing a
#	favourite is for. A second process reads the file this one wrote and F opens
#	on a list with the place in it -- against a control run whose places file does
#	not exist, which is T-SAVED's own pairing and is there for T-SAVED's reason:
#	"the frame changed" is only evidence if the other frame could not have
#	contained the row. Both runs are otherwise identical, and /dev/null as the
#	config is what stops the device's own favourites from being in either.
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --roads $(ROADPACK) \
		--places $(RDIR)/t-save.places \
		--key 10:f --dump-at 12:$(RDIR)/save-find.fb 2>/dev/null
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --roads $(ROADPACK) \
		--places $(RDIR)/t-save-none.places \
		--key 10:f --dump-at 12:$(RDIR)/save-find-none.fb 2>/dev/null
	python3 tools/fbdiff.py $(RDIR)/save-find.fb $(RDIR)/save-find-none.fb \
		--min-px 200
#	And the control really did have no places file to read, so the pair above is
#	not two runs of the same state.
	test ! -f $(RDIR)/t-save-none.places
	@echo "--- T-SAVE-ROUNDTRIP: the name it offers survives being read back"
#	THE TWO-PRESS PATH, and the bug it is here for. `S ENTER` writes the offered
#	default, which contains a space -- and config.c used to take the name as the
#	FIRST field, so `place = PLACE 1 13.72936,100.56100` came back as a place
#	called PLACE at 1.00000, 13.72936: a point in the Atlantic off Guinea. Both
#	halves are legal numbers, so the +-90 guard could not see it, nothing warned,
#	and the rider would have had a favourite that read back as somewhere else.
#	place_split() now finds the coordinate from the right.
#
#	Asserted by ROUTING to it after a restart, which is the only assertion that
#	can tell the two apart: a place 10 000 km away is off the pack, the offline
#	router refuses it, and there is no "routed to" line at all.
	rm -f $(RDIR)/t-save-fast.places
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --places $(RDIR)/t-save-fast.places \
		--key 20:s --key 21:enter 2> $(RDIR)/save-fast.log
	grep -q "^place = PLACE 1 13.72936,100.56100$$" \
		$(RDIR)/t-save-fast.places
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --roads $(ROADPACK) \
		--places $(RDIR)/t-save-fast.places \
		--key 10:f --key 12:enter 2> $(RDIR)/save-fast2.log
	grep -q "routed to PLACE 1 --" $(RDIR)/save-fast2.log
#	A NAME WITH A SPACE IN IT, typed. The same claim from the other side: the
#	writer and the reader agree about where a multi-word name ends, so "CO OP" is
#	a name and not a name plus a number.
	rm -f $(RDIR)/t-save-word.places
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --places $(RDIR)/t-save-word.places \
		--key 20:s --key 21:c --key 22:o --key 23:space --key 24:o \
		--key 25:p --key 26:enter 2> $(RDIR)/save-word.log
	grep -q "^place = CO OP 13.72936,100.56100$$" $(RDIR)/t-save-word.places
	@echo "--- T-SAVE-REFUSE: the four ways S says no"
#	Each of them a transient AND a line on stderr (DESIGN.md 2's "a dead key is
#	indistinguishable from a broken program"), and each asserted on the line,
#	because a transient lives 1.5 s and a frame comparison cannot tell which of
#	four refusals it was looking at.
#
#	A DUPLICATE NAME. saved.conf already has HOME, so this types it and finds
#	the refusal -- and the file must not have grown, which is the assertion that
#	says the check happens BEFORE the append rather than after it.
	rm -f $(RDIR)/t-save-dup.places
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config $(SAVEDCONF) --places $(RDIR)/t-save-dup.places \
		--key 20:s --key 21:h --key 22:o --key 23:m --key 24:e \
		--key 25:enter 2> $(RDIR)/save-dup.log
	grep -q "HOME is already a saved place" $(RDIR)/save-dup.log
	! grep -q "beepy-nav: saved" $(RDIR)/save-dup.log
	test ! -f $(RDIR)/t-save-dup.places
#	NO PLACES FILE. --no-places means the program was told not to touch one, and
#	a save that lived only until exit would be worse than a refusal: the mark
#	would appear, the rider would trust it, and it would be gone next boot.
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config $(SAVEDCONF) --no-places --key 20:s \
		2> $(RDIR)/save-nofile.log
	grep -q "no places file" $(RDIR)/save-nofile.log
	! grep -q "beepy-nav: saved" $(RDIR)/save-nofile.log
#	NO FIX, in the harder of its two forms. nofix.nmea voids the fix from 200 s
#	to 230 s, and S is pressed inside that gap -- so there IS a last known
#	position, it is on the strip, and this asserts that S will not write it. That
#	is the case a rider meets (a bridge, a car park); "there has never been a fix"
#	is the same condition reached more easily, and it is not separately asserted
#	here because the fixture cannot be cold and warm at once.
	rm -f $(RDIR)/t-save-nofix.places
	$(NAV) --replay $(RDIR)/nofix.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --places $(RDIR)/t-save-nofix.places \
		--key 210:s --key 212:enter 2> $(RDIR)/save-nofix.log
	grep -q "the fix is lost, and the last one is stale" \
		$(RDIR)/save-nofix.log
	! grep -q "beepy-nav: saved" $(RDIR)/save-nofix.log
	test ! -f $(RDIR)/t-save-nofix.places
#	THE CEILING. Eight places is CFG_PLACES_MAX, and the ninth is refused rather
#	than rotated: dropping the rider's oldest favourite to make room for a coffee
#	shop is not a decision this program gets to make. The fixture is eight
#	`place` lines, so the refusal is reached without eight keypress rounds.
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config beepy-nav/tests/full.conf \
		--places $(RDIR)/t-save-full.places --key 20:s \
		2> $(RDIR)/save-full.log
	grep -q "8 places already saved" $(RDIR)/save-full.log
	test ! -f $(RDIR)/t-save-full.places
#	NAME NEEDED. Backspace on the selected default empties the field, and the
#	field is then empty for exactly one keypress -- so ENTER during it must not
#	write a nameless place. Reachable, which is why it is asserted rather than
#	commented as impossible.
	rm -f $(RDIR)/t-save-empty.places
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --places $(RDIR)/t-save-empty.places \
		--key 20:s --key 21:bs --key 22:enter 2> $(RDIR)/save-empty.log
	grep -q "a saved place needs a name" $(RDIR)/save-empty.log
	test ! -f $(RDIR)/t-save-empty.places
#	NOT SAVED. A path whose directory cannot be created -- a read-only card is
#	the real case, and this is the reachable stand-in for it. The errno text
#	differs between the Mac and the device (Read-only file system against
#	Permission denied), so only the sentence is matched.
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config /dev/null --places /nope-no-perm/x.places \
		--key 20:s --key 21:enter 2> $(RDIR)/save-ro.log
	grep -q "cannot write /nope-no-perm/x.places" $(RDIR)/save-ro.log
	! grep -q "beepy-nav: saved" $(RDIR)/save-ro.log
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
	@echo "--- T-MAP-PAN: the arrows move the map, and C brings it back"
#	DESIGN.md 1.5.1. The trackpad on this keyboard is in arrow mode
#	(touch_as = keys), so this is the same key path a thumb on the pad produces --
#	and up/down already reached the FIND list, which is why only left/right had to
#	be added to the keymap at all.
#
#	STATIONARY, because "C gives the frame back byte for byte" is the assertion
#	that says the pan is a pure view offset and nothing else, and a moving rider
#	changes the frame on their own. The same reason T-QUIT-CONFIRM uses this
#	fixture.
	$(NAV) --replay $(RDIR)/still.nmea --headless $(FPS8) --no-basemap \
		--config $(SAVEDCONF) \
		--dump-at 16:$(RDIR)/pan-centred.fb \
		--key 20:right --key 21:right --key 22:down \
		--dump-at 26:$(RDIR)/pan-held.fb \
		--key 30:c --dump-at 34:$(RDIR)/pan-back.fb 2> $(RDIR)/pan.log
#	The map moved...
	! python3 tools/fbdiff.py $(RDIR)/pan-centred.fb $(RDIR)/pan-held.fb \
		--max-px 0
#	...and C put it back EXACTLY. A pan that leaked into the projection, the
#	breadcrumb or the marker's own position would fail this and nothing else
#	would.
	python3 tools/fbdiff.py $(RDIR)/pan-centred.fb $(RDIR)/pan-back.fb --max-px 0
#	Stopped, a swipe just pans: no question, because there is nothing to be
#	unaware of when the map and the bike are both still.
	! grep -q "pan while moving" $(RDIR)/pan.log
#	MOVING, the first swipe ASKS and does not move the map -- then ENTER applies
#	the swipe that raised the question, which is why the step is remembered.
	$(NAV) --replay $(RDIR)/asok.nmea --headless $(FPS8) --no-basemap \
		--config $(SAVEDCONF) \
		--dump-at 20:$(RDIR)/panm-plain.fb \
		--key 22:right --dump-at 24:$(RDIR)/panm-ask.fb \
		--key 26:enter --dump-at 28:$(RDIR)/panm-held.fb 2> $(RDIR)/panm.log
	grep -q "map pan while moving" $(RDIR)/panm.log
	! python3 tools/fbdiff.py $(RDIR)/panm-plain.fb $(RDIR)/panm-ask.fb --max-px 0
	! python3 tools/fbdiff.py $(RDIR)/panm-ask.fb $(RDIR)/panm-held.fb --max-px 0
#	And the arrows do NOT pan the NAV page (1.5.1 is MAP only): with a route
#	loaded, the same presses change nothing at all.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--dump-at 16:$(RDIR)/pan-nav-a.fb \
		--key 20:right --key 21:down --key 22:left \
		--dump-at 26:$(RDIR)/pan-nav-b.fb 2>/dev/null
	python3 tools/fbdiff.py $(RDIR)/pan-nav-a.fb $(RDIR)/pan-nav-b.fb --max-px 0
	@echo "--- T-MODE-KEY: M on CONFIRM rebuilds the route, it does not relabel it"
#	DESIGN.md 7.7. That bike and car return DIFFERENT routes is already gated by
#	test_search.c on tests/roads/modes.roads -- 442 m up the motorway against
#	1,091 m around it. What is new here is the KEY, and the thing that can go
#	wrong with it is drawing "CAR" on the strip while the route on the screen is
#	still the bicycle's.
#	STATIONARY, and that is what makes the byte comparison below mean anything:
#	each press re-routes from where the rider IS, so on a moving fixture the three
#	routes legitimately start three places apart and no two frames can be equal.
#	The first version used asok.nmea and was measuring the bike rolling forward.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--roads $(ROADPACK) --config /dev/null \
		--key 10:f --key 11:s --key 12:o --key 13:i \
		--key 14:space --key 15:2 --key 16:3 --key 18:enter \
		--dump-at 19:$(RDIR)/mode-bike.fb \
		--key 20:m --dump-at 22:$(RDIR)/mode-car.fb \
		--key 24:m --dump-at 26:$(RDIR)/mode-back.fb \
		2> $(RDIR)/mode.log
#	THREE routes built for one destination, one per press, each naming the profile
#	that built it. A build where M only changed the strip would show one line.
	test `grep -c "routed to SOI SUKHUMVIT 23" $(RDIR)/mode.log` -eq 3
	grep -q "routed to SOI SUKHUMVIT 23 -- 51 points, 2.08 km, 10 cues, bike" \
		$(RDIR)/mode.log
	grep -q "routed to SOI SUKHUMVIT 23 -- 51 points, 2.08 km, 10 cues, car" \
		$(RDIR)/mode.log
#	The two frames differ -- the strip reads M BIKE then M CAR -- and the third is
#	back to the first BYTE FOR BYTE, which is the assertion that says the toggle
#	is a toggle and not a one-way door. It also says the rebuilt route is the same
#	route: this destination is reachable identically by both profiles in this pack,
#	so any difference in the drawn line would be the rebuild going wrong.
	! python3 tools/fbdiff.py $(RDIR)/mode-bike.fb $(RDIR)/mode-car.fb --max-px 0
	python3 tools/fbdiff.py $(RDIR)/mode-bike.fb $(RDIR)/mode-back.fb --max-px 0
	@echo "--- T-REROUTE: 200 m and sustained, once a minute, five to an episode"
#	DESIGN.md 7.11. Rerouting is the first thing in this program that ACTS on its
#	own while the rider is moving, so every assertion here is about a COUNT: how
#	many times it fired, and whether each brake is the one doing the stopping.
#
#	THE BRAKES ARE MEASURED WITH NOTHING ABLE TO INSTALL -- --no-roads and no
#	router_url -- and that is what makes them arithmetic. Let a reroute succeed
#	and the deviation clears, the episode ends, and the count becomes a fact about
#	the pack's geography instead of about the rate limit. The install is proved
#	separately, below.
#
#	Two fixtures, because one cannot tell the two brakes apart: at 25 km/h the
#	rider is past 200 m for 105 s, where the 60 s spacing allows 2 and the cap
#	allows 5; at 6 km/h for 436 s, where the spacing allows 8 and the cap allows 5.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/reroute.nmea --headless $(FPS1) \
		--no-roads --config beepy-nav/tests/net/reroute-auto.conf \
		2> $(RDIR)/reroute-auto.log
#	EXACTLY TWO, and only the 60 s spacing can produce that number: a build that
#	fired once per fix would give ninety-five, and the cap would have allowed five.
	test `grep -c "reroute [0-9] of" $(RDIR)/reroute-auto.log` -eq 2
	$(NAV) --route $(RROUTE) --replay $(RDIR)/reroute-long.nmea --headless \
		$(FPS1) --no-roads --config beepy-nav/tests/net/reroute-auto.conf \
		2> $(RDIR)/reroute-cap.log
#	EXACTLY FIVE, and only the cap can produce that number: this deviation lasts
#	long enough for the spacing alone to allow eight.
	test `grep -c "reroute [0-9] of" $(RDIR)/reroute-cap.log` -eq 5
#	NOT the 40 m off-route latch, which is the distinction 7.11 exists to draw.
#	T-DETOUR's own fixture is the control: a 100 m excursion that latches off
#	route, on the same route, with rerouting on auto -- and nothing fires.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/detour.nmea --headless $(FPS1) \
		--no-roads --config beepy-nav/tests/net/reroute-auto.conf \
		2> $(RDIR)/reroute-latch.log
	! grep -q "reroute" $(RDIR)/reroute-latch.log
#	`off` produces none, on the fixture that produces five under auto.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/reroute-long.nmea --headless \
		$(FPS1) --no-roads --config beepy-nav/tests/net/reroute-off.conf \
		2> $(RDIR)/reroute-off.log
	! grep -q "reroute" $(RDIR)/reroute-off.log
#	`ask` produces none WITHOUT the key -- which is the whole difference between
#	ask and auto: it spends nothing until asked. The prompt is armed, and that is
#	all that happens.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/reroute-long.nmea --headless \
		$(FPS1) --no-roads --config beepy-nav/tests/net/reroute-ask.conf \
		2> $(RDIR)/reroute-ask.log
	! grep -q "reroute [0-9] of" $(RDIR)/reroute-ask.log
	grep -q "off route -- ENTER to reroute" $(RDIR)/reroute-ask.log
#	And exactly one WITH it. One key, one attempt: a prompt that armed a repeating
#	timer would show more.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/reroute.nmea --headless $(FPS1) \
		--no-roads --config beepy-nav/tests/net/reroute-ask.conf \
		--key 460:enter 2> $(RDIR)/reroute-key.log
	test `grep -c "reroute [0-9] of" $(RDIR)/reroute-key.log` -eq 1
#	THE INSTALL, which is the other half and needs a pack that can answer. One
#	route replaces another under a running ride:
	$(NAV) --route $(RROUTE) --replay $(RDIR)/reroute.nmea --headless $(FPS1) \
		--roads $(ROADPACK) \
		--config beepy-nav/tests/net/reroute-auto.conf \
		--trace $(RDIR)/reroute-live.tsv 2> $(RDIR)/reroute-live.log
#	"0 m off" is the assertion that route_rebase() did its job, and it is exact
#	because there is no third possible answer: a new route starts where the rider
#	is, so a re-snap reads a few metres. Skip the rebase and route_snap() compares
#	a world-frame position against a route-frame polyline and reads the distance
#	between the two origins -- kilometres.
	grep -q "rerouted to Sukhumvit Loop -- 59 points, 1.67 km, 11 cues, 0 m off, bike" \
		$(RDIR)/reroute-live.log
#	ONE ride, not two. This is what says the route was swapped IN PLACE rather
#	than through CONFIRM's hand-off, which leaves run_live() and comes back --
#	re-opening the serial port, re-running 6.3's UBX exchange and starting a
#	second ride log, all while the rider is off route and moving. Two "fixes,"
#	lines would mean the ride had been restarted underneath them.
	test `grep -c "fixes," $(RDIR)/reroute-live.log` -eq 1
#	TWO attempts, and the pair is better evidence than one would have been: the
#	first is refused by the pack (the rider is in a component of this small
#	extract that cannot reach the finish) and the second succeeds. The log says
#	when -- "for 10 s" and "for 70 s", the age of the same unbroken deviation --
#	so the 60 s spacing is visible here in the lane where a reroute SUCCEEDS,
#	which is the lane the --no-roads runs above cannot reach at all.
	test `grep -c "reroute [0-9] of" $(RDIR)/reroute-live.log` -eq 2
	grep -q "reroute 1 of 5: 247 m off route for 10 s" $(RDIR)/reroute-live.log
	grep -q "reroute 2 of 5: 288 m off route for 70 s" $(RDIR)/reroute-live.log
#	And the ride went on navigating the REPLACEMENT. `pct` was reset to zero when
#	the route changed, so 62% at the end is a kilometre travelled and measured
#	along the new route -- which a route installed into the wrong frame could not
#	produce, because every fix would have landed kilometres off it. The rider does
#	NOT arrive: the fixture rides the original track home and the replacement is
#	the road route from the detour, so it ends 633 m short of its own finish. That
#	is the fixture being a replay and not a rider, and asserting arrival here would
#	be asserting something untrue.
	$(ASSERT) $(RDIR)/reroute-live.tsv --final pct ">=" 55
	test `wc -l < $(RDIR)/reroute-live.tsv` -eq 598
	@echo "--- T-FIND-ONLINE: the pack first, and the network only where it cannot"
#	DESIGN.md 7.10. Three runs, because the three claims fail independently.
#
#	The fixture is the rider in Asok with saved.conf's own two places: WORK is
#	inside the Asok pack and HOME is 24 km outside it. That is the whole
#	experiment and it needs no new geography -- ENTER on WORK must be answered by
#	the pack and never reach a fetcher, ENTER on HOME cannot be answered by the
#	pack at all. `fetch_cmd = cat` is the router, so nothing here touches the
#	network (7.8), and the answer it gives is N2's committed capture: 177 points,
#	4.95 km, 9 cues.
#
#	Run one: both, in that order, through one FIND page.
#
#	PACED, and that is not decoration. Unpaced, ride time races ahead of the wall
#	clock, so the ride second at which a fork+exec+cat lands depends on how fast
#	the machine renders -- 25 ride-seconds is 60 ms of wall on the Mac and four
#	seconds of it on a Pi Zero. The first version pressed the install ENTER at
#	ride 17 and the fetch had not arrived; at ride 40 it had, on this machine,
#	today. Paced, one ride second is one second and a `cat` has a whole one.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/online.conf \
		--key 10:f --key 11:down --key 12:enter --key 14:esc \
		--key 15:up --key 16:enter --key 20:enter \
		2> $(RDIR)/find-online.log
#	OFFLINE FIRST: WORK is in the pack, so it is routed by the pack. The absence
#	of "(online)" on that line is the assertion -- a build that went online for
#	everything would produce the route and pass every other check here.
	grep -q "routed to WORK -- 30 points, 0.80 km, 4 cues, bike$$" \
		$(RDIR)/find-online.log
#	HOME is not, and the reason is the one router.c refuses on rather than a
#	sentence this test matched: RC_OFFMAP is what sends it online.
	grep -q "24 km outside this map" $(RDIR)/find-online.log
	grep -q "asking valhalla for a route to HOME" $(RDIR)/find-online.log
#	And what came back is N2's fixture, parsed to the point, so the request, the
#	fetch, both adapters' worth of parsing and the install are one live path.
	grep -q "routed to HOME -- 177 points, 4.95 km, 9 cues, bike (online)" \
		$(RDIR)/find-online.log
#	Installed by the line a GPX takes, printed by the route-loading loop and by
#	nothing else -- which is the gate's own wording and the claim of 1.4 that
#	nothing downstream can tell where a route came from.
	grep -q "beepy-nav: HOME -- 177 points" $(RDIR)/find-online.log
#	Exactly one request for the two destinations. Without this, "offline first"
#	would pass on a build that asked the network for WORK as well and simply
#	preferred the pack's answer.
	test `grep -c "asking valhalla" $(RDIR)/find-online.log` -eq 1
	@echo "--- T-NET-USETOLLS: the car asks to avoid tolls, the bicycle does not"
#	ASSERTED ON THE REQUEST, because nothing downstream reflects it. A
#	costing_options block attached to the wrong costing, or malformed, would come
#	back as a perfectly good route -- the fixture answers the same bytes whatever
#	was asked -- so the only evidence is the body itself. These two confs keep it.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/body-car.conf \
		--key 10:f --key 12:enter 2> $(RDIR)/req-car.log
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/body-bike.conf \
		--key 10:f --key 12:enter 2> $(RDIR)/req-bike.log
#	Both must actually have asked, or the greps below run on a stale file from a
#	previous build -- or on no file at all, which `grep -q` would also fail on but
#	for the wrong reason.
	grep -q "asking valhalla for a route to HOME (car)" $(RDIR)/req-car.log
	grep -q "asking valhalla for a route to HOME (bike)" $(RDIR)/req-bike.log
	grep -q '"costing":"auto"' $(RDIR)/req-car.json
	grep -q '"costing_options":{"auto":{"use_tolls":0}}' $(RDIR)/req-car.json
#	And NOT on the bicycle: Valhalla's bicycle costing has no use_tolls, and a
#	stricter server may reject an unknown option outright. This is the assertion
#	that a future edit cannot quietly widen the flag to both profiles.
	grep -q '"costing":"bicycle"' $(RDIR)/req-bike.json
	! grep -q "use_tolls" $(RDIR)/req-bike.json
#	Still valid JSON after the insert -- the comma placement around an optional
#	object is exactly what a hand-built body gets wrong.
	python3 -c "import json,sys; json.load(open('$(RDIR)/req-car.json')); \
		json.load(open('$(RDIR)/req-bike.json')); print('  PASS  both bodies parse')"
	rm -f $(RDIR)/req-car.json $(RDIR)/req-bike.json \
		$(RDIR)/req-car.log $(RDIR)/req-bike.log
	@echo "--- T-NET-TOLL: has_toll reaches the CONFIRM title, and only there"
#	TWO RUNS DIFFERING IN ONE JSON LITERAL. online-toll.conf is online.conf with
#	a fetcher pointed at a sed of the same capture -- same 177 points, same
#	4.95 km, same 9 cues -- so every pixel that differs between these frames is
#	`"has_toll":true` arriving on the panel. A second real capture would have
#	moved the geometry too, and then a badge that never drew would still have
#	produced two different frames.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/online-toll.conf \
		--key 10:f --key 11:down --key 12:enter --key 14:esc \
		--key 15:up --key 16:enter --dump-at 18:$(RDIR)/confirm-toll.fb \
		--key 20:enter 2> $(RDIR)/find-toll.log
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/online.conf \
		--key 10:f --key 11:down --key 12:enter --key 14:esc \
		--key 15:up --key 16:enter --dump-at 18:$(RDIR)/confirm-notoll.fb \
		--key 20:enter 2> $(RDIR)/find-notoll.log
#	Both runs must have gone online at all, or two identical OFFLINE frames
#	would satisfy the mask check below and prove nothing.
	grep -q "routed to HOME -- 177 points" $(RDIR)/find-toll.log
	grep -q "routed to HOME -- 177 points" $(RDIR)/find-notoll.log
#	The flag changed the frame...
	! cmp -s $(RDIR)/confirm-toll.fb $(RDIR)/confirm-notoll.fb
#	...and changed NOTHING outside the title row. This is the half that makes
#	it a placement assertion rather than "something moved": the badge is drawn
#	in the title because DESIGN.md 7.7 built the strip for four half-lines and
#	said no fifth value competes for the space, and a badge that crept into the
#	strip would pass the line above and fail here.
	python3 tools/fbdiff.py $(RDIR)/confirm-toll.fb \
		$(RDIR)/confirm-notoll.fb --mask 0,0,399,25 --max-px 0
#	And an OFFLINE proposal says nothing at all -- the pack has no toll bit, so
#	a blank right margin has to keep meaning "not told". WORK is answered by the
#	pack, so the SAME two configs that produced different frames above must now
#	produce IDENTICAL ones: the fetcher is never reached, and a build that let a
#	reply's flag reach a route the pack built would differ here.
#
#	Comparing the two runs and not a run against itself. The first draft of this
#	assertion diffed confirm-offline.fb with confirm-offline.fb, which passes for
#	every possible build and tests nothing.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/online.conf \
		--key 10:f --key 11:down --key 12:enter \
		--dump-at 13:$(RDIR)/confirm-offa.fb 2> $(RDIR)/find-offa.log
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/online-toll.conf \
		--key 10:f --key 11:down --key 12:enter \
		--dump-at 13:$(RDIR)/confirm-offb.fb 2> $(RDIR)/find-offb.log
	grep -q "routed to WORK -- 30 points" $(RDIR)/find-offa.log
	grep -q "routed to WORK -- 30 points" $(RDIR)/find-offb.log
#	Neither run may have gone online, or this compares two fetches.
	test `grep -c "asking valhalla" $(RDIR)/find-offa.log` -eq 0
	test `grep -c "asking valhalla" $(RDIR)/find-offb.log` -eq 0
	cmp $(RDIR)/confirm-offa.fb $(RDIR)/confirm-offb.fb
	rm -f $(RDIR)/confirm-toll.fb $(RDIR)/confirm-notoll.fb \
		$(RDIR)/confirm-offa.fb $(RDIR)/confirm-offb.fb \
		$(RDIR)/find-toll.log $(RDIR)/find-notoll.log \
		$(RDIR)/find-offa.log $(RDIR)/find-offb.log
#	Run two: a fetcher that never answers, so FETCHING and then TIMED OUT are
#	both on the panel -- paced and stationary for the reasons still-net.nmea
#	gives. The dumps are 1 s apart because 6.3 renders a STOPPED ride at 1 Hz:
#	the first version put the baseline 0.5 s before the keypress and got the same
#	frame back, so "the bar changed" was being asserted against itself.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/online-hang.conf \
		--key 10:f --dump-at 11:$(RDIR)/net-before.fb \
		--key 12:enter --dump-at 13:$(RDIR)/net-fetching.fb \
		--dump-at 23:$(RDIR)/net-timeout.fb 2> $(RDIR)/find-hang.log
	grep -q "fetch failed -- timed out" $(RDIR)/find-hang.log
#	The title bar and NOTHING ELSE, both times. This is the whole of 7.10's
#	interface claim as a byte comparison: the rows stay, the query stays, the
#	hint stays, and the rider can go on typing while a child process works. A
#	page that had switched to a modal "please wait" would fail it; so would one
#	that had quietly dropped back to NAV.
	python3 tools/fbdiff.py $(RDIR)/net-before.fb $(RDIR)/net-fetching.fb \
		--mask 0,0,399,25 --max-px 0
	python3 tools/fbdiff.py $(RDIR)/net-before.fb $(RDIR)/net-timeout.fb \
		--mask 0,0,399,25 --max-px 0
#	And the bar DID change, twice and differently. Without these three the pair
#	above would pass on a build where pressing ENTER did nothing at all.
	! python3 tools/fbdiff.py $(RDIR)/net-before.fb $(RDIR)/net-fetching.fb \
		--max-px 0
	! python3 tools/fbdiff.py $(RDIR)/net-before.fb $(RDIR)/net-timeout.fb \
		--max-px 0
	! python3 tools/fbdiff.py $(RDIR)/net-fetching.fb $(RDIR)/net-timeout.fb \
		--max-px 0
#	Run three: no router_url, which is this program's DEFAULT and therefore the
#	state a rider meets first. It must fail on the page and say why, not fall
#	back to running fetch_cmd against an empty URL -- offline.conf carries a
#	working fetch_cmd precisely so that a build which did would produce a route
#	to a place 24 km outside its only map and pass.
	$(NAV) --route $(TILEROUTE) --replay $(RDIR)/asok.nmea --headless $(FPS8) \
		--roads $(ROADPACK) --config beepy-nav/tests/net/offline.conf \
		--key 10:f --dump-at 11:$(RDIR)/norouter-before.fb \
		--key 12:enter --dump-at 14:$(RDIR)/norouter.fb \
		2> $(RDIR)/find-norouter.log
	grep -q "24 km outside this map" $(RDIR)/find-norouter.log
	grep -q "no router_url configured" $(RDIR)/find-norouter.log
	! grep -q "routed to HOME" $(RDIR)/find-norouter.log
	python3 tools/fbdiff.py $(RDIR)/norouter-before.fb $(RDIR)/norouter.fb \
		--mask 0,0,399,25 --max-px 0
	! python3 tools/fbdiff.py $(RDIR)/norouter-before.fb $(RDIR)/norouter.fb \
		--max-px 0
	@echo "--- T-CAR-RETRY: TOO FAR offers a car, and ENTER asks for one"
#	DESIGN.md 7.10. One run, three claims, and the third is the one that made the
#	first two worth having.
#
#	Paced and stationary for still-net.nmea's reasons, unchanged: the fetcher is a
#	`cat` and lands inside a ride second, so ENTER at 12 has arrived by 14 and
#	ENTER at 16 by 18 -- both by arithmetic, because under pacing ride time can
#	only lag the wall clock. Every unpaced version of a fetch test in this file
#	has raced, four times in one day.
#
#	HOME is 26 km outside the Asok pack, so ENTER goes online; online-toofar.conf's
#	`cat` refuses it with the device's own 129-byte body, both times.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still-net.nmea --headless $(FPS8) \
		--pace --roads $(ROADPACK) \
		--config beepy-nav/tests/net/online-toofar.conf \
		--key 10:f --key 12:enter --dump-at 14:$(RDIR)/toofar-offer.fb \
		--key 16:enter --dump-at 18:$(RDIR)/toofar-car.fb \
		2> $(RDIR)/toofar.log
#	The refusal was recognised as a DISTANCE one, which is netroute.c's error_code
#	154 and not the sentence beside it.
	grep -q "max distance limit" $(RDIR)/toofar.log
#	TWO requests for one destination, and the MODE is the whole assertion: the
#	fetcher answers identically both times, so a build that ignored the offer and
#	re-sent the bicycle would produce a byte-identical second refusal and pass
#	every other check here. This pair is the only thing that can see the costing.
	grep -q "asking valhalla for a route to HOME (bike)" $(RDIR)/toofar.log
	grep -q "asking valhalla for a route to HOME (car)" $(RDIR)/toofar.log
	test `grep -c "asking valhalla" $(RDIR)/toofar.log` -eq 2
#	And the bar the RIDER reads is the bar in the golden -- byte for byte, from a
#	live armed request rather than from view_find_toofar_demo(). Everything below
#	the title bar is masked because the two frames are different searches; the
#	title bar is the same twenty characters in the same place, so it is comparable
#	and nothing else is. Without this, FIND_NET_TOOFAR_CAR could be drawn off the
#	right edge and both the golden and the greps above would still pass.
	python3 tools/fbdiff.py $(RDIR)/toofar-offer.fb \
		goldens/nav-find-toofar.fb --mask 0,26,399,239 --max-px 0
#	The offer is NOT re-armed once the car has been refused, because a car's own
#	cap is 5000 km and there is nothing left to offer. So the bar after the second
#	refusal reads a plain TOO FAR and must NOT match the golden's -- which is also
#	the assertion that the frame above was not simply the bar this build always
#	draws for a TOO FAR.
	! python3 tools/fbdiff.py $(RDIR)/toofar-car.fb \
		goldens/nav-find-toofar.fb --mask 0,26,399,239 --max-px 0
	@echo "--- T-BEEPY-DIGITS: the Alt digit layer reaches the FIND page"
#	DESIGN.md 2. This is the regression test for a bug that shipped with the FIND
#	page and survived every gate: keycode_to_char() mapped KEY_1..KEY_0, and
#	beepy-kbd does not send those. Alt+W is keycode 136, and
#	/usr/share/kbd/keymaps/beepy-kbd.map turns THAT into a digit -- a translation
#	belonging to the console, which this program reads evdev specifically to
#	bypass. So "SOI 23" could be typed over ssh and not on the bike.
#
#	NO TEST COULD HAVE CAUGHT IT, and that is the interesting part: every --key in
#	this file presses a CHARACTER, which enters below the keymap. A suite that
#	drives a program under its own keymap tests everything except the keymap.
#	--key kcNNN exists to close that, and this is the only test that uses it.
#
#	THE ASSERTION IS THAT THE TWO PATHS AGREE. Run one types 1 2 3 as characters,
#	run two sends keycodes 136 137 138, and the frames must be byte-identical --
#	no golden, because the character path is already the reference and a golden
#	would just be a third thing to keep in step.
#
#	Stationary, for T-MODE-KEY's reason: these frames are compared byte for byte
#	and a moving rider changes every distance in the list legitimately.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--roads $(ROADPACK) --config /dev/null \
		--key 10:f --key 11:s --key 12:o --key 13:i --key 14:space \
		--key 15:1 --key 16:2 --key 17:3 \
		--dump-at 19:$(RDIR)/digits-char.fb
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--roads $(ROADPACK) --config /dev/null \
		--key 10:f --key 11:s --key 12:o --key 13:i --key 14:space \
		--key 15:kc136 --key 16:kc137 --key 17:kc138 \
		--dump-at 19:$(RDIR)/digits-kc.fb
	cmp $(RDIR)/digits-char.fb $(RDIR)/digits-kc.fb
#	And NOT vacuously: the same page with the digits never pressed must differ.
#	Without this, a build where kc136 mapped to nothing would pass the cmp above
#	by drawing "SOI " in both runs.
	$(NAV) --route $(RROUTE) --replay $(RDIR)/still.nmea --headless $(FPS8) \
		--roads $(ROADPACK) --config /dev/null \
		--key 10:f --key 11:s --key 12:o --key 13:i --key 14:space \
		--dump-at 19:$(RDIR)/digits-none.fb
	! cmp -s $(RDIR)/digits-none.fb $(RDIR)/digits-kc.fb
#	What this canNOT prove: that the Beepy still SENDS 136. If the driver's keymap
#	changes, this passes while the bike breaks -- that half is hardware, and
#	`beepy-nav --print-keys` is the documented way to ask it.
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
            host/expand.o \
            host/nmea.o host/gps.o \
            host/seg.o host/arrows.o host/map.o host/gpx.o host/route.o \
            host/view_nav.o host/view_overview.o host/fix.o host/chooser.o \
            host/led.o host/config.o host/ridelog.o host/tile.o \
            host/roadgrid.o host/persp.o host/nav3d.o \
            host/search.o host/router.o host/view_find.o \
            host/view_confirm.o host/view_map.o host/view_quit.o \
            host/view_save.o \
           host/netfetch.o host/netroute.o \
            host/nav.o

# beepy-nav is portable end to end (no fbdev, no evdev), so the Mac can link
# and run it -- which is what makes the M2 design gate a fast loop.
HOST_NAV = host/nav.o host/view_nav.o host/view_overview.o host/seg.o \
           host/arrows.o host/map.o host/gpx.o host/route.o host/fix.o \
           host/chooser.o host/led.o host/config.o host/ridelog.o \
           host/tile.o host/search.o host/router.o host/view_find.o \
           host/roadgrid.o host/persp.o host/nav3d.o \
           host/view_confirm.o host/view_map.o host/view_quit.o \
           host/view_save.o \
           host/netfetch.o host/netroute.o \
           host/nmea.o host/gps.o \
           host/canvas.o host/font.o host/cover.o host/dump.o

HOST_VID = host/vid-vid.o host/vid-pack.o host/vid-codec.o \
           host/vid-view_play.o host/vid-view_pages.o host/vid-audio.o \
           host/canvas.o host/font.o host/cover.o host/dump.o host/expand.o

host/beepy-vid: $(HOST_VID)
	$(CC) $(CFLAGS) -o $@ $(HOST_VID) $(LDLIBS) $(VIDLIBS)

host: $(HOST_OBJS) $(HOST_VID) host/beepy-nav host/beepy-vid
	@echo "host: portable objects compile clean"

host/beepy-nav: $(HOST_NAV)
	$(CC) $(CFLAGS) -o $@ $(HOST_NAV) $(LDLIBS)

# map.c, gpx.c and route.c are the beepy-nav modules with no pixels in them,
# so they are the ones that can be checked by assertion instead of by frame
# comparison. Runs in either lane; `check` runs it on the device.
UNIT_TESTS = libbeepyfb/tests/test_expand \
             beepy-vid/tests/test_codec beepy-vid/tests/test_pack \
             beepy-nav/tests/test_map beepy-nav/tests/test_gpx \
             beepy-nav/tests/test_route beepy-nav/tests/test_tile \
             beepy-nav/tests/test_search \
             beepy-nav/tests/test_roadgrid beepy-nav/tests/test_persp \
             beepy-nav/tests/test_netroute \
             beepy-nav/tests/test_netfetch

test-unit: $(UNIT_TESTS) beepy-nav/tests/gpx/oversize.gpx
#	T-EXPAND. First, because it is the only test here that guards a function
#	the OTHER tests depend on: every golden compared below and in `check` is
#	written by canvas_dump(), while the panel is driven by expand(). Nothing
#	compared the two until this existed.
	./libbeepyfb/tests/test_expand
#	T-CODEC / T-PACK. The .vid reader is linkable on its own, so these build
#	packs byte by byte in C and gate the format before tools/mkvid.py exists.
	./beepy-vid/tests/test_codec
	./beepy-vid/tests/test_pack
	./beepy-nav/tests/test_map
	./beepy-nav/tests/test_gpx
	./beepy-nav/tests/test_route
	./beepy-nav/tests/test_tile
	./beepy-nav/tests/test_search
#	T-ROADGRID. The spatial index the 3D nav view queries every frame (6.6).
#	Node coordinates only, built by hand, so it needs no pack and runs here.
#	The assertion that matters is the 22-million-cell extent holding two nodes:
#	the sparse TILE index shipped with that bound written against the extent
#	instead of the occupancy, and a 4x4 fixture could not tell the difference.
	./beepy-nav/tests/test_roadgrid
#	T-PERSP. The tilted camera, and one assertion in it is load-bearing for the
#	whole 2D/3D design: at pitch 90 the projection must be EXACTLY the top-down
#	affine, to 1e-12. If it is not, then 2D and 3D are two projections sharing
#	a name and every later claim about one code path is false. Clamping the
#	pitch to 89.98 degrees -- invisible on the panel -- fails it four ways.
	./beepy-nav/tests/test_persp
#	T-NETROUTE / T-NETROUTE-BAD. Reads its fixtures by relative path, so it
#	runs from the repo root like the rest.
	./beepy-nav/tests/test_netroute
#	Last, because it is the only test here that spends real time -- ten
#	seconds of it, waiting for a deadline that is the sole defence against a
#	server which accepts a connection and then says nothing (DESIGN.md 7.8).
	./beepy-nav/tests/test_netfetch

# T-PRESENT-ROWS. Device only: it opens /dev/fb1, SIGSTOPs fbterm and reads the
# framebuffer back, so it cannot run in the Mac lane and is the ONLY thing that
# can catch a wrong pwrite() offset in fb_present_rows() -- every golden goes
# through canvas_dump(), so a band painted in the wrong place is invisible to
# `check`.
#
# Deliberately NOT a prerequisite of `check` yet. It takes the panel and must
# therefore run last and self-trap (beepy-vid/PLAN.md M4, and CLAUDE.md's rule
# that a missed SIGCONT leaves the device looking dead); wedging the console at
# minute three of a 25-minute gate costs the whole run. Run it by hand:
#
#     make test-panel        # on the device
#
test-panel: libbeepyfb/tests/test_present_rows
	@trap 'kill -CONT $$(pgrep -x fbterm) 2>/dev/null; true' EXIT INT TERM HUP; \
	./libbeepyfb/tests/test_present_rows

libbeepyfb/tests/test_present_rows: libbeepyfb/tests/test_present_rows.c \
                                    libbeepyfb/expand.c libbeepyfb/canvas.c \
                                    libbeepyfb/fbdev.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -o $@ \
		libbeepyfb/tests/test_present_rows.c libbeepyfb/expand.c \
		libbeepyfb/canvas.c libbeepyfb/fbdev.c $(LDLIBS)

beepy-vid/tests/test_codec: beepy-vid/tests/test_codec.c beepy-vid/src/codec.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-vid/src -o $@ \
		beepy-vid/tests/test_codec.c beepy-vid/src/codec.c $(LDLIBS)

beepy-vid/tests/test_pack: beepy-vid/tests/test_pack.c beepy-vid/src/pack.c \
                           beepy-vid/src/codec.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-vid/src -o $@ \
		beepy-vid/tests/test_pack.c beepy-vid/src/pack.c \
		beepy-vid/src/codec.c $(LDLIBS) -lz

# The panel's XRGB writers. Links canvas.c, dump.c and expand.c and nothing
# else -- expand.c was split out of fbdev.c precisely so this runs in the Mac
# lane, where fbdev.c (linux/fb.h) cannot be built.
libbeepyfb/tests/test_expand: libbeepyfb/tests/test_expand.c \
                              libbeepyfb/expand.c libbeepyfb/canvas.c \
                              libbeepyfb/dump.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -o $@ \
		libbeepyfb/tests/test_expand.c libbeepyfb/expand.c \
		libbeepyfb/canvas.c libbeepyfb/dump.c $(LDLIBS)

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
# roadgrid.c reads node coordinates and nothing else, so the test links it
# alone -- no pack, no search.c, no pixels. persp.c is pure arithmetic and
# links against libm only. Both are therefore runnable in either lane, which is
# what puts them inside `check` on the device.
beepy-nav/tests/test_roadgrid: beepy-nav/tests/test_roadgrid.c \
                              beepy-nav/src/roadgrid.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_roadgrid.c beepy-nav/src/roadgrid.c $(LDLIBS)

beepy-nav/tests/test_persp: beepy-nav/tests/test_persp.c \
                           beepy-nav/src/persp.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_persp.c beepy-nav/src/persp.c $(LDLIBS)

beepy-nav/tests/test_tile: beepy-nav/tests/test_tile.c beepy-nav/src/tile.c \
                           $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_tile.c beepy-nav/src/tile.c \
		libbeepyfb/cover.c libbeepyfb/canvas.c libbeepyfb/font.c $(LDLIBS)

# search.c and router.c have no pixels in them either, so like map/gpx/route
# they are checked by assertion. route.c joins the link because router_to()
# leaves a prepared route_t -- which is the point of it -- and gpx.c because
# route.c calls route_load(). No cover.c: nothing here draws.
# The fetcher (DESIGN.md 7.8). Links netfetch.c and nothing else -- it knows
# about bytes and child processes, not about routes -- which is what lets this
# run in `make check` on the device with no network anywhere near it.
beepy-nav/tests/test_netfetch: beepy-nav/tests/test_netfetch.c \
                               beepy-nav/src/netfetch.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_netfetch.c beepy-nav/src/netfetch.c

# The parsers (DESIGN.md 7.9). route.c joins the link because netroute_parse()
# leaves a PREPARED route_t -- the identical object route_load() and router_to()
# leave, which is the claim -- and gpx.c because route.c calls route_load().
# No netfetch.c: this module never sees a socket, only a string.
beepy-nav/tests/test_netroute: beepy-nav/tests/test_netroute.c \
                               beepy-nav/src/netroute.c \
                               beepy-nav/src/route.c beepy-nav/src/gpx.c \
                               $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-nav/src -o $@ \
		beepy-nav/tests/test_netroute.c beepy-nav/src/netroute.c \
		beepy-nav/src/route.c beepy-nav/src/gpx.c $(LDLIBS)

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

host/vid-%.o: beepy-vid/src/%.c $(HDRS)
	@mkdir -p host
	$(CC) $(CFLAGS) $(INC) -Ibeepy-vid/src -c -o $@ $<

host/%.o: beepy-nav/src/%.c $(HDRS)
	@mkdir -p host
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

tables:
	cd beepy-nav && python3 ../tools/gen_tables.py

# ------------------------------------------------- the router-reply fixtures
#
# Mac lane, and the outputs are COMMITTED -- unlike every other generated
# fixture here. `make check` runs on the device, and a gate whose bad-input
# cases only exist if python3 and this script are both on the Pi is a gate with
# a second thing to go wrong. The rule exists so that none of them is a file
# somebody made by hand once; regenerate and `git diff` should be empty.
netfix:
	python3 tools/mknetfix.py

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
$(VIDGRAY) $(VIDPCM):
	python3 tools/mkvidfix.py --gray8 $(VIDGRAY) --pcm $(VIDPCM) --quiet

beepy-vid/tests/viddecode: beepy-vid/tests/viddecode.c beepy-vid/src/pack.c \
                           beepy-vid/src/codec.c $(HDRS)
	$(CC) $(CFLAGS) $(INC) -Ibeepy-vid/src -o $@ \
		beepy-vid/tests/viddecode.c beepy-vid/src/pack.c \
		beepy-vid/src/codec.c $(LDLIBS) -lz

# Mac lane: mkvid.py needs numpy, which the device does not have.
test-vid: beepy-vid/tests/viddecode $(VIDGRAY) $(VIDPCM) $(VIDPACK)
	@echo "--- T-VID-DETERMINISM: same gray8, same bytes, twice"
	python3 tools/mkvid.py --gray8 $(VIDGRAY) --pcm $(VIDPCM) $(VIDOPTS) \
		-o out-vid-1.vid --quiet
	python3 tools/mkvid.py --gray8 $(VIDGRAY) --pcm $(VIDPCM) $(VIDOPTS) \
		-o out-vid-2.vid --quiet
	cmp out-vid-1.vid out-vid-2.vid
#	Two builds in ONE environment cannot see an unsorted set -- the existing
#	tile and road determinism gates have that hole. Vary the hash seed.
	PYTHONHASHSEED=0 python3 tools/mkvid.py --gray8 $(VIDGRAY) --pcm $(VIDPCM) \
		$(VIDOPTS) -o out-vid-h0.vid --quiet
	PYTHONHASHSEED=1 python3 tools/mkvid.py --gray8 $(VIDGRAY) --pcm $(VIDPCM) \
		$(VIDOPTS) -o out-vid-h1.vid --quiet
	cmp out-vid-h0.vid out-vid-h1.vid
	cmp out-vid-1.vid $(VIDPACK)
	@echo "--- T-VID-ROUNDTRIP: the C reader agrees with the Python dither"
#	Decodes EVERY frame with pack.c and compares against mkvid.py's own
#	dither of the same gray8. A per-frame golden would freeze frame 0 and say
#	nothing about frame 23, which is the only frame a broken XOR chain gets
#	wrong. --verify also refuses a fixture with fewer than two distinct
#	frames, so a decoder stuck on frame 0 cannot pass.
	./beepy-vid/tests/viddecode out-vid-1.vid out-vid-1.planes
	python3 tools/mkvid.py --gray8 $(VIDGRAY) $(VIDOPTS) -o /dev/null \
		--verify out-vid-1.planes
	@echo "--- T-VID-DEFLATE: compression changes bytes, never pixels"
	python3 tools/mkvid.py --gray8 $(VIDGRAY) --pcm $(VIDPCM) \
		$(filter-out --no-deflate,$(VIDOPTS)) -o out-vid-z.vid --quiet
	! cmp -s out-vid-z.vid out-vid-1.vid
	./beepy-vid/tests/viddecode out-vid-z.vid out-vid-z.planes
	cmp out-vid-1.planes out-vid-z.planes
	@echo "--- T-VID-INFO: the header round-trips through --info"
	python3 tools/mkvid.py --info $(VIDPACK) | grep -q "frames     24"
	python3 tools/mkvid.py --info $(VIDPACK) | grep -q "image rect 0,7 400x225"
	python3 tools/mkvid.py --info $(VIDPACK) | grep -q "hysteresis 8"
	@echo "--- T-VID-REFUSE: settings that cannot be reproduced are refused"
#	A float rate would round differently between builds, and a zero gop leaves
#	every seek unbounded. Both must fail loudly at build time rather than
#	produce a pack that only mostly works.
	! python3 tools/mkvid.py --gray8 $(VIDGRAY) --size 400x225 --fps 23.976 \
		-o out-vid-bad.vid --quiet 2>/dev/null
	! python3 tools/mkvid.py --gray8 $(VIDGRAY) --size 400x225 --gop 0 \
		-o out-vid-bad.vid --quiet 2>/dev/null
	test ! -f out-vid-bad.vid
	rm -f out-vid-1.vid out-vid-2.vid out-vid-h0.vid out-vid-h1.vid \
		out-vid-z.vid out-vid-1.planes out-vid-z.planes
	@echo "test-vid: PASS"

# Mac lane. The scheduler is portable, so pacing is asserted here rather than
# in the 25-minute device gate -- and NOT under real load: a CPU hog on a
# 2-core Pi Zero is a flaky test, and Makefile:978 counts four races from
# timing tests that raced. An injected stall has exactly one right answer.
#
# --fps 8 stretches the 24-frame fixture to 3 s so a stall fits inside it.
test-vidpace: host/beepy-vid $(VIDPACK)
	@echo "--- T-VID-PACE: an unstalled run drops nothing and holds cadence"
	./host/beepy-vid $(VIDPACK) --headless --fps 8 \
		--trace-frames out-vid-pace.tsv
#	0.100 against a 125 ms period, not 0.112. The sprint this must catch is
#	4 ms, so the margin is 25x either way, but the tighter threshold flaked
#	once in about ten runs on a loaded machine -- and a gate that fails
#	occasionally for no reason teaches people to re-run it, which is worse
#	than not having it.
	python3 tools/assert_trace.py out-vid-pace.tsv --finite \
		--dropped 0 --min-frame-gap 0.100 --max-frame-gap 0.200
	@echo "--- T-VID-STALL: 500 ms at 8 fps drops exactly 3, by arithmetic"
#	A stall of D seconds at F fps consumes floor(D*F) frame periods, one of
#	which belongs to the frame already on screen: 0.5 * 8 - 1 = 3. Stable
#	across six runs on this machine, and it is a count rather than a
#	duration, so it does not depend on how fast the machine is.
	./host/beepy-vid $(VIDPACK) --headless --fps 8 --stall-at 4:500 \
		--trace-frames out-vid-stall.tsv
	python3 tools/assert_trace.py out-vid-stall.tsv \
		--dropped 3 --min-frame-gap 0.100 --max-frame-gap 0.70
	@echo "--- T-VID-CONSERVED: every frame is either shown or counted dropped"
#	The machine-independent half of the same claim, and the one that would
#	survive a port: presented + dropped must equal the frame count exactly.
#	A player that quietly skipped a frame without counting it passes every
#	assertion above and fails this one.
	awk -F'\t' 'BEGIN{p=0} /^#/{next} $$4==1{p++} END{d=$$6+0; \
		if (p+d != 24) { printf "FAIL presented %d + dropped %d != 24\n", p, d; \
		exit 1 } printf "  PASS  presented %d + dropped %d == 24\n", p, d }' \
		out-vid-stall.tsv
	@echo "--- T-VID-NOSPRINT: --min-frame-gap catches what nothing else can"
#	A player that dumps its backlog after a stall shows every frame, drops
#	nothing, and leaves the stall as its only long gap -- so --max-frame-gap,
#	--dropped and --finite all pass it. Only a MINIMUM gap sees it. The
#	fixture is synthesised here because the player must never do this.
	python3 tools/mkpacefix.py --sprint out-vid-sprint.tsv
	python3 tools/assert_trace.py out-vid-sprint.tsv --finite \
		--dropped 0 --max-frame-gap 0.70
	! python3 tools/assert_trace.py out-vid-sprint.tsv --min-frame-gap 0.100 \
		> /dev/null 2>&1
	rm -f out-vid-pace.tsv out-vid-stall.tsv out-vid-sprint.tsv
	@echo "test-vidpace: PASS"

# Mac lane. A/V sync against a FAKE SINK, so the gate never touches a speaker
# and never needs one to be paired: audio_cmd is a command precisely so a test
# can substitute one (audio.h, and config.h:80-83 for the original argument).
#
# The fixture is generated, not committed: a clock test needs audio LONGER
# than the pipe buffer plus the sink buffer, or acceptance never blocks and
# the test measures nothing. 240 frames is 10 s and 960 KB.
VIDLONG = out-vid-long.vid

$(VIDLONG): host/beepy-vid
	python3 tools/mkvidfix.py --gray8 out-vid-long.gray8 \
		--pcm out-vid-long.s16 --frames 240 --quiet
	python3 tools/mkvid.py --gray8 out-vid-long.gray8 --size 400x225 \
		--pcm out-vid-long.s16 -o $@ --fps 24 --gop 24 --quiet

test-vidsync: host/beepy-vid $(VIDLONG)
	@echo "--- T-VID-AVCLOCK: video follows the sink, in both directions"
#	The clip is 10.00 s. A player scheduling from CLOCK_MONOTONIC takes 10 s
#	whatever the sink does; a player slaved to the audio takes 1/factor times
#	as long. That ratio is the entire assertion, and it is a ratio, so it does
#	not depend on how fast the machine is.
	@set -e; \
	 t1=$$(./host/beepy-vid $(VIDLONG) --headless --trace-frames out-vid-s1.tsv \
	   --audio-cmd "python3 tools/fakesink.py --rate 96000 --factor 1.0 --buffer 8000" \
	   >/dev/null 2>&1; tail -1 out-vid-s1.tsv | cut -f1); \
	 t2=$$(./host/beepy-vid $(VIDLONG) --headless --trace-frames out-vid-s2.tsv \
	   --audio-cmd "python3 tools/fakesink.py --rate 96000 --factor 0.8 --buffer 8000" \
	   >/dev/null 2>&1; tail -1 out-vid-s2.tsv | cut -f1); \
	 t3=$$(./host/beepy-vid $(VIDLONG) --headless --no-audio \
	   --trace-frames out-vid-s3.tsv >/dev/null 2>&1; \
	   tail -1 out-vid-s3.tsv | cut -f1); \
	 echo "    true rate $$t1 s   20% slow $$t2 s   no audio $$t3 s"; \
	 python3 -c "import sys; a,b,c=float('$$t1'),float('$$t2'),float('$$t3'); \
	   ok = b > a*1.15 and abs(c-10.0) < 0.5 and abs(a-10.0) < 1.5; \
	   print('  PASS  a 20%% slow sink stretches playback by %.0f%%' % (100*(b/a-1)) if ok \
	     else '  FAIL  slow=%.2f true=%.2f noaudio=%.2f' % (b,a,c)); \
	   sys.exit(0 if ok else 1)"
	@echo "--- T-VID-AVCONSERVED: nothing is lost when the clock is audio"
	awk -F'\t' 'BEGIN{p=0} /^#/{next} $$4==1{p++} END{d=$$6+0; \
		if (p+d != 240) { printf "FAIL presented %d + dropped %d != 240\n", p, d; \
		exit 1 } printf "  PASS  presented %d + dropped %d == 240\n", p, d }' \
		out-vid-s2.tsv
	@echo "--- T-VID-SILENT: --no-audio is the monotonic clock, unstretched"
	python3 tools/assert_trace.py out-vid-s3.tsv --finite --dropped 0
	rm -f out-vid-s1.tsv out-vid-s2.tsv out-vid-s3.tsv \
		out-vid-long.gray8 out-vid-long.s16 $(VIDLONG)
	@echo "test-vidsync: PASS"

# Volume is arithmetic on the samples, so it is assertable without a speaker:
# point audio_cmd at a FILE and the sink becomes evidence. This is the same
# property DESIGN.md 7.3 bought for the clock tests, spent again.
#
# --volume exists partly for this. Keys cannot be injected into a headless run,
# so a key-only volume control would be a feature the gate could not reach --
# and beepy-vid has been here before: a9105eb shipped a player whose device
# path had no audio at all because the only tested loop was the headless one.
test-vidvol: host/beepy-vid $(VIDLONG)
	@echo "--- T-VID-VOL-CLOCK: volume changes values, never byte counts"
#	The whole reason this is safe. audio.h derives the clock from bytes
#	ACCEPTED, so a volume control that resampled or requantised would silently
#	become a speed control. Four levels, one byte count.
	@set -e; \
	 for v in 10 8 5 0; do \
	   ./host/beepy-vid $(VIDLONG) --headless --volume $$v \
	     --audio-cmd "cat > out-vid-vol$$v.raw" >/dev/null 2>&1; \
	 done; \
	 n=$$(wc -c < out-vid-vol10.raw); \
	 for v in 8 5 0; do \
	   m=$$(wc -c < out-vid-vol$$v.raw); \
	   [ "$$n" = "$$m" ] || { echo "  FAIL level $$v wrote $$m, level 10 wrote $$n"; exit 1; }; \
	 done; \
	 echo "  PASS  all four levels wrote $$n bytes"
	@echo "--- T-VID-VOL-UNITY: the default is bit-identical passthrough"
#	Not "close enough". A player nobody touched the volume on must put the
#	pack's own bytes on the wire, or every existing assertion about what the
#	sink received is measuring the volume control instead.
	cmp out-vid-vol10.raw out-vid-long.s16
	@echo "--- T-VID-VOL-MUTE: level 0 is silence, not -60 dB of hiss"
#	cmp against /dev/zero over exactly the file's length, and not `tr -d '\0' |
#	read`: read returns failure at EOF whether or not it consumed anything, so
#	the obvious pipeline reports "all zeros" for a file that is nothing of the
#	sort. Checked against a file of \001\002\003, which it passed.
	@set -e; \
	 n=$$(wc -c < out-vid-vol0.raw); \
	 cmp -n $$n out-vid-vol0.raw /dev/zero; \
	 echo "  PASS  level 0 is $$n zero bytes"
	@echo "--- T-VID-VOL-DB: the ladder is the decibels it claims"
	@python3 -c "import sys, math, array; \
	  rms=lambda p:(lambda a:math.sqrt(sum(float(x)*x for x in a)/len(a)))(array.array('h',open(p,'rb').read())); \
	  ref=rms('out-vid-vol10.raw'); \
	  bad=[]; \
	  [bad.append((v,t,20*math.log10(rms('out-vid-vol%d.raw'%v)/ref))) \
	     for v,t in ((8,-6.0),(5,-17.0)) if abs(20*math.log10(rms('out-vid-vol%d.raw'%v)/ref)-t)>0.2]; \
	  print('  FAIL '+repr(bad)) if bad else print('  PASS  level 8 is -6 dB and level 5 is -17 dB'); \
	  sys.exit(1 if bad else 0)"
	rm -f out-vid-vol10.raw out-vid-vol8.raw out-vid-vol5.raw out-vid-vol0.raw \
		out-vid-long.gray8 out-vid-long.s16 $(VIDLONG)
	@echo "test-vidvol: PASS"

# Deliberate, like tiles: and it regenerates a committed file.
vidpack: $(VIDGRAY) $(VIDPCM)
ifndef VID_OK
	$(error refusing to rebuild the committed $(VIDPACK) without VID_OK=1)
endif
	python3 tools/mkvid.py --gray8 $(VIDGRAY) --pcm $(VIDPCM) $(VIDOPTS) \
		-o $(VIDPACK)

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
#	DESIGN.md 6.5: a rung present in two inputs is UNIONED, which is what puts
#	fine detail for several cities in one basemap. This pack pair is the smallest
#	case that can tell the union from the rule it replaced -- the same ground at
#	the same rung cut to two corridor widths, so one is a strict superset of the
#	other and the counts differ. Taking the rung from the FIRST input, which is
#	what this used to do, returns 6 tiles for narrow-then-wide; unioning returns
#	32 whichever way round they are given.
	@echo "--- T-TILES-MERGE-UNION: one rung from two packs keeps both areas"
	python3 tools/mktiles.py --osm $(TILEOSM) --route $(TILEROUTE) \
		--corridor 300 --zooms 2.5 -o out-tiles-narrow.tiles --quiet
	python3 tools/mktiles.py --osm $(TILEOSM) --route $(TILEROUTE) \
		--corridor 2000 --zooms 2.5 -o out-tiles-wide.tiles --quiet
	python3 tools/mktiles.py --info out-tiles-narrow.tiles | \
		grep -q "1 zooms  6 tiles"
	python3 tools/mktiles.py --info out-tiles-wide.tiles | \
		grep -q "1 zooms  32 tiles"
	python3 tools/mergetiles.py out-tiles-narrow.tiles out-tiles-wide.tiles \
		-o out-tiles-union.tiles --quiet
	python3 tools/mktiles.py --info out-tiles-union.tiles | \
		grep -q "1 zooms  32 tiles"
	python3 tools/mergetiles.py out-tiles-wide.tiles out-tiles-narrow.tiles \
		-o out-tiles-union2.tiles --quiet
	python3 tools/mktiles.py --info out-tiles-union2.tiles | \
		grep -q "1 zooms  32 tiles"
#	Joining a pack to itself is the degenerate case, and it is the one that
#	catches an off-by-one in the index rewrite: every rung is already present,
#	so the output must be the input, byte for byte. It is ALSO what covers the
#	union's collision path: every cell is present in both inputs, the first wins
#	every time, and byte-for-byte is the proof that nothing was added or moved.
	@echo "--- T-TILES-MERGE-IDEMPOTENT: a pack joined to itself is itself"
	python3 tools/mergetiles.py $(TILEPACK) $(TILEPACK) \
		-o out-tiles-merged-3.tiles --quiet
	cmp $(TILEPACK) out-tiles-merged-3.tiles
#	DESIGN.md 6.5's SPARSE index (pack v2). A merged pack uses one grid per rung
#	spanning every region in it, and a dense index is 4 bytes per CELL: two cuts
#	far enough apart overflow it, so mergetiles switches encodings. The claim that
#	has to hold is that the encoding changes NOTHING a reader can see.
#
#	--sparse forces the encoding. The alternative was two fixtures placed far
#	enough apart to overflow the dense ceiling by accident, which at 4 m/px means
#	2 000 km and makes the test rest on a coincidence in the geography.
	@echo "--- T-TILES-SPARSE: a sparse pack is the dense pack, exactly"
	python3 tools/mktiles.py --osm $(TILEOSM) --route $(TILEROUTE) \
		--corridor 500 --zooms 4 -o out-tiles-near.tiles --quiet
	python3 tools/mergetiles.py out-tiles-near.tiles --sparse \
		-o out-tiles-sparse.tiles --quiet
	python3 tools/mktiles.py --info out-tiles-sparse.tiles | grep -q "^.*: v2"
#	Same tiles, same frame: the merged sparse pack must draw the corridor exactly
#	as the pack it came from. A binary search that returned a neighbour on a miss,
#	or a key computed against the wrong grid width, fails here and nowhere else.
	./host/beepy-nav --demo --page nav-tiles \
		--basemap out-tiles-near.tiles --dump out-nav-near.fb
	./host/beepy-nav --demo --page nav-tiles \
		--basemap out-tiles-sparse.tiles --dump out-nav-sparse.fb
	cmp out-nav-near.fb out-nav-sparse.fb
	rm -f out-tiles-near.tiles out-tiles-sparse.tiles \
		out-nav-near.fb out-nav-sparse.fb
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
		out-tiles-narrow.tiles out-tiles-wide.tiles \
		out-tiles-union.tiles out-tiles-union2.tiles \
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
#	Pack v3 (DESIGN.md 1.4.7) widened EDGES.name to a u32; v2 (7.7) put the road
#	class in EDGES.flags. The version is asserted because a pack the device
#	cannot read is indistinguishable from one it can until it is opened. Asserted here as
#	"the packer wrote it" and in tests/test_search.c as "the router acts on
#	it" -- the split every pack feature here uses, because a byte written and
#	never read is not a feature.
	@echo "--- T-ROADS-CLASS: the pack records what kind of road each edge is"
	python3 tools/mkpack.py --osm $(ROADMODES) --ref $(ROADREF) \
		-o out-roads-modes.roads --quiet
	cmp out-roads-modes.roads beepy-nav/tests/roads/modes.roads
	python3 tools/mkpack.py --info beepy-nav/tests/roads/asok.roads | \
		grep -q "^beepy-nav/tests/roads/asok.roads: v4"
	rm -f out-roads-modes.roads
#	And the weighting fixture beside it, by the same rule: a committed pack is
#	only evidence for as long as it is still what the packer produces.
	python3 tools/mkpack.py --osm $(ROADBIAS) --ref $(ROADREF) \
		-o out-roads-bias.roads --quiet
	cmp out-roads-bias.roads beepy-nav/tests/roads/bias.roads
	rm -f out-roads-bias.roads
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
	@echo "--- T-ROADS-FOLD: an accent is not a shaping problem"
#	DESIGN.md 1.4.2 drops a name with no ASCII form because the 5x7 font cannot
#	shape Thai. An ACCENTED LATIN letter is a different thing entirely -- it folds
#	to the letter it is -- and a plain isascii() treated the two alike. Found in
#	the field: OSM spells nearly every PTT branch `Cafe' Amazon` with an acute, so
#	the one 3.4 km from the rider was invisible while the ones typed without the
#	accent were searchable 19 km away.
	python3 tools/mkpack.py --osm beepy-nav/tests/roads/fold.json \
		--ref $(ROADREF) -o out-roads-fold.roads --quiet
	python3 tools/mkpack.py --info out-roads-fold.roads | grep -q "'CAFE AMAZON'"
	python3 tools/mkpack.py --info out-roads-fold.roads | \
		grep -q "'BANG YAI COFFEE'"
#	And the Thai one still drops. Without this the fold could be "strip every
#	mark and hope", which would put unrenderable bytes on the panel -- the exact
#	failure 1.4.2 exists to prevent.
	python3 tools/mkpack.py --info out-roads-fold.roads | \
		grep -q "1 ways indexed, 1 names dropped"
	! python3 tools/mkpack.py --info out-roads-fold.roads | grep -q "WAT"
	rm -f out-roads-fold.roads
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
	python3 tools/mkpack.py --osm $(ROADPOIS) --ref $(ROADREF) \
		-o beepy-nav/tests/roads/pois.roads
	python3 tools/mkpack.py --osm $(ROADMODES) --ref $(ROADREF) \
		-o beepy-nav/tests/roads/modes.roads
	python3 tools/mkpack.py --osm $(ROADBIAS) --ref $(ROADREF) \
		-o beepy-nav/tests/roads/bias.roads
	python3 tools/mkpack.py --osm $(ROADTOLLS) --ref $(ROADREF) \
		-o beepy-nav/tests/roads/tolls.roads

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
		beepy-nav/tests/test_roadgrid beepy-nav/tests/test_persp \
		*.o */*.o */*/*.o *.a */*.a
	rm -rf host

.PHONY: all check goldens host test-unit test-replay test-frames test-find test-panel test-vid test-vidpace test-vidsync test-vidvol vidpack \
	host-replay tables design-gate test-tiles tiles test-roads roads \
	bench sync clean netfix

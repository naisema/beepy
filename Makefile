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
            beepy-nav/src/fix.o
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

check: gps-monitor/gps-monitor beepy-nav/beepy-nav test-unit
	./gps-monitor/gps-monitor --demo --page bars --dump out-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump out-sky.fb
	cmp goldens/gm-bars.fb out-bars.fb
	cmp goldens/gm-sky.fb  out-sky.fb
	./beepy-nav/beepy-nav --demo --page nav     --dump out-nav-turn.fb
	./beepy-nav/beepy-nav --demo --page nav-off --dump out-nav-off.fb
	./beepy-nav/beepy-nav --demo --page arrows  --dump out-nav-arrows.fb
	./beepy-nav/beepy-nav --demo --page overview --dump out-nav-overview.fb
	cmp goldens/nav-turn.fb     out-nav-turn.fb
	cmp goldens/nav-off.fb      out-nav-off.fb
	cmp goldens/nav-arrows.fb   out-nav-arrows.fb
	cmp goldens/nav-overview.fb out-nav-overview.fb
	@echo "check: PASS - demo dumps byte-identical to goldens"

goldens: gps-monitor/gps-monitor beepy-nav/beepy-nav
ifndef GOLDEN_OK
	$(error refusing to regenerate goldens without GOLDEN_OK=1)
endif
	./gps-monitor/gps-monitor --demo --page bars --dump goldens/gm-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump goldens/gm-sky.fb
	./beepy-nav/beepy-nav --demo --page nav     --dump goldens/nav-turn.fb
	./beepy-nav/beepy-nav --demo --page nav-off --dump goldens/nav-off.fb
	./beepy-nav/beepy-nav --demo --page arrows  --dump goldens/nav-arrows.fb
	./beepy-nav/beepy-nav --demo --page overview --dump goldens/nav-overview.fb
	@echo "goldens regenerated - review the diff before committing"

# 100 NAV renders, draw + resolve only (DESIGN.md 5.4 budgets 30 ms).
bench: beepy-nav/beepy-nav
	./beepy-nav/beepy-nav --demo --page nav --bench 100

# -------------------------------------------------------------------- host
#
# Compile the portable objects on the Mac so parser/renderer edits get a
# warning pass without a device round-trip. fbdev/input/serial are device-only.

HOST_OBJS = host/canvas.o host/font.o host/cover.o host/dump.o \
            host/nmea.o host/gps.o \
            host/seg.o host/arrows.o host/map.o host/gpx.o host/route.o \
            host/view_nav.o host/view_overview.o host/fix.o host/nav.o

# beepy-nav is portable end to end (no fbdev, no evdev), so the Mac can link
# and run it -- which is what makes the M2 design gate a fast loop.
HOST_NAV = host/nav.o host/view_nav.o host/view_overview.o host/seg.o \
           host/arrows.o host/map.o host/gpx.o host/route.o host/fix.o \
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
             beepy-nav/tests/test_route

test-unit: $(UNIT_TESTS) beepy-nav/tests/gpx/oversize.gpx
	./beepy-nav/tests/test_map
	./beepy-nav/tests/test_gpx
	./beepy-nav/tests/test_route

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
		*.o */*.o */*/*.o *.a */*.a
	rm -rf host

.PHONY: all check goldens host test-unit tables design-gate bench sync clean

# beepy — top-level build
#
# Two build lanes:
#   device (beepy.local, gcc 10.2.1)  — the real build; anything touching
#           /dev/fb1, evdev or termios only compiles here (linux/*.h).
#   host   (Mac, clang)               — fast dev loop for the portable
#           modules: font, canvas, nmea, gps (no fbdev/input/serial).
#
# The Mac entrypoint is `make sync`: rsync the tree to the device and run
# `make check` there. GATE: `check` byte-compares gps-monitor's --demo dumps
# against goldens/gm-*.fb captured from the pre-split build. Any diff is a
# regression; goldens regenerate only deliberately (GOLDEN_OK=1 make goldens).

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

FB_OBJS   = libbeepyfb/canvas.o libbeepyfb/font.o libbeepyfb/fbdev.o libbeepyfb/input.o
NMEA_OBJS = libnmea/nmea.o libnmea/gps.o libnmea/serial.o
GM_OBJS   = gps-monitor/main.o gps-monitor/view_bars.o gps-monitor/view_sky.o
HDRS      = $(wildcard libbeepyfb/*.h libnmea/*.h gps-monitor/*.h)

all: gps-monitor/gps-monitor

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

$(FB_OBJS) $(NMEA_OBJS) $(GM_OBJS): $(HDRS)

libbeepyfb/libbeepyfb.a: $(FB_OBJS)
	ar rcs $@ $(FB_OBJS)

libnmea/libnmea.a: $(NMEA_OBJS)
	ar rcs $@ $(NMEA_OBJS)

gps-monitor/gps-monitor: $(GM_OBJS) libnmea/libnmea.a libbeepyfb/libbeepyfb.a
	$(CC) $(CFLAGS) -o $@ $(GM_OBJS) libnmea/libnmea.a libbeepyfb/libbeepyfb.a $(LDLIBS)

check: gps-monitor/gps-monitor
	./gps-monitor/gps-monitor --demo --page bars --dump out-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump out-sky.fb
	cmp goldens/gm-bars.fb out-bars.fb
	cmp goldens/gm-sky.fb  out-sky.fb
	@echo "check: PASS - demo dumps byte-identical to goldens"

goldens: gps-monitor/gps-monitor
ifndef GOLDEN_OK
	$(error refusing to regenerate goldens without GOLDEN_OK=1)
endif
	./gps-monitor/gps-monitor --demo --page bars --dump goldens/gm-bars.fb
	./gps-monitor/gps-monitor --demo --page sky  --dump goldens/gm-sky.fb
	@echo "goldens regenerated - review the diff before committing"

# -------------------------------------------------------------------- host
#
# Compile the portable objects on the Mac so parser/renderer edits get a
# warning pass without a device round-trip. fbdev/input/serial are device-only.

HOST_OBJS = host/canvas.o host/font.o host/nmea.o host/gps.o

host: $(HOST_OBJS)
	@echo "host: portable objects compile clean"

host/%.o: libbeepyfb/%.c $(HDRS)
	@mkdir -p host
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

host/%.o: libnmea/%.c $(HDRS)
	@mkdir -p host
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

tables:
	cd beepy-nav && python3 ../tools/gen_tables.py

# -------------------------------------------------------- Mac -> device
sync:
	rsync -az -e "ssh -i $(SSHKEY)" \
		--exclude .git --exclude beepy/ --exclude beepy-buildroot/ \
		--exclude '*.png' --exclude '*.tar.gz' --exclude sim-anim.bin \
		--exclude osm-asok.json --exclude host/ \
		./ $(DEVICE):$(REMOTE_DIR)/
	ssh -i $(SSHKEY) $(DEVICE) 'make -C $(REMOTE_DIR) check'

clean:
	rm -f gps-monitor/gps-monitor out-*.fb *.o */*.o *.a */*.a
	rm -rf host

.PHONY: all check goldens host tables sync clean

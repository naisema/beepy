# beepy — top-level build
#
# Two build lanes:
#   device (beepy.local, gcc 10.2.1)  — the real build; anything touching
#           /dev/fb1, evdev or termios only compiles here (linux/*.h).
#   host   (Mac, clang)               — fast dev loop for portable modules
#           and unit tests; arrives with the M1 library split.
#
# The Mac entrypoint is `make sync`: rsync the tree to the device and run
# `make check` there. GATE: `check` byte-compares gps-monitor's --demo dumps
# against goldens/gm-*.fb captured from the pre-split build. Any diff is a
# regression; goldens regenerate only deliberately (GOLDEN_OK=1 make goldens).

CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  ?= -lm

DEVICE  ?= beepy@beepy.local
SSHKEY  ?= $(HOME)/.ssh/id_rsa
REMOTE_DIR ?= beepy-src

# ------------------------------------------------------------------ device
all: gps-monitor/gps-monitor

gps-monitor/gps-monitor: gps-monitor/gps-monitor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

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
host:
	@echo "host lane arrives with the M1 library split (portable modules only)"

tables:
	cd beepy-nav && python3 ../tools/gen_tables.py

# -------------------------------------------------------- Mac -> device
sync:
	rsync -az -e "ssh -i $(SSHKEY)" \
		--exclude .git --exclude beepy/ --exclude beepy-buildroot/ \
		--exclude '*.png' --exclude '*.tar.gz' --exclude sim-anim.bin \
		--exclude osm-asok.json \
		./ $(DEVICE):$(REMOTE_DIR)/
	ssh -i $(SSHKEY) $(DEVICE) 'make -C $(REMOTE_DIR) check'

clean:
	rm -f gps-monitor/gps-monitor out-*.fb *.o */*.o *.a

.PHONY: all check goldens host tables sync clean

# gps-monitor — the sources were split into ../libbeepyfb + ../libnmea (M1);
# the real build lives in the top-level Makefile. This wrapper keeps
# `make -C gps-monitor` and `make install` working.

PREFIX ?= /usr/local

all:
	$(MAKE) -C .. gps-monitor/gps-monitor

check:
	$(MAKE) -C .. check

clean:
	$(MAKE) -C .. clean

install: all
	install -m755 gps-monitor $(DESTDIR)$(PREFIX)/bin/gps-monitor

.PHONY: all check clean install

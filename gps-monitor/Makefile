CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS  ?= -lm
PREFIX  ?= /usr/local

all: gps-monitor

gps-monitor: gps-monitor.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: gps-monitor
	install -m755 gps-monitor $(DESTDIR)$(PREFIX)/bin/gps-monitor

# Render both pages from the built-in demo data straight to raw frames, so the
# renderer can be checked against the design mockups with no panel involved.
check: gps-monitor
	./gps-monitor --demo --page bars --dump out-bars.fb
	./gps-monitor --demo --page sky  --dump out-sky.fb
	@for f in out-bars out-sky; do \
		s=$$(wc -c < $$f.fb); \
		[ "$$s" = 384000 ] && echo "$$f.fb OK ($$s bytes)" \
		                   || { echo "$$f.fb WRONG SIZE: $$s"; exit 1; }; \
	done

clean:
	rm -f gps-monitor out-bars.fb out-sky.fb

.PHONY: all install check clean

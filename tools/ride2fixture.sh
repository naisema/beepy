#!/bin/sh
# ride2fixture.sh -- turn a ride log into a beepy-nav replay fixture.
#
#     tools/ride2fixture.sh ~/rides/20260730-181500.nmea ROUTE.gpx NAME
#
# DESIGN.md 7.6: the ride log exists so that a failure in the field becomes a
# regression test rather than a story. This is the step that does the becoming.
# It copies the log into beepy-nav/tests/replay/, replays it headlessly against
# the route to produce the per-fix and per-frame traces, and prints the
# Makefile lines to paste into test-replay.
#
# It also does the one check nothing else can: if the device wrote a .tsv
# beside the log -- and it always does -- the replayed trace is compared
# against what the DEVICE actually computed from the same bytes. The route
# maths is a pure function of the sentences, so those columns must agree
# exactly. A disagreement means the replay is not a rehearsal of the ride, and
# every assertion built on it would be measuring the wrong program. That is
# worth knowing before the fixture is committed, not after.
#
# The heading and residual columns are deliberately NOT compared: they are
# functions of the FRAME cadence (DESIGN.md 6.1's EWMA is over a time
# constant), and a live 8 Hz clock driven by mono_now() does not land on the
# same grid as a replay's. Nor is `presented`, for the same reason.
#
#   NAV=...   which binary to replay with (default: whichever is built)
#   FPS=...   frames per second for the replay (default 8, the real rate)
set -eu

usage() {
    sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}
[ $# -eq 3 ] || usage

LOG=$1
ROUTE=$2
NAME=$3
FPS=${FPS:-8}

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
RDIR=$ROOT/beepy-nav/tests/replay

if [ -z "${NAV:-}" ]; then
    # The host binary when there is one -- this is normally run on the Mac,
    # after scp'ing a log off the device -- and the device build otherwise.
    if [ -x "$ROOT/host/beepy-nav" ]; then NAV=$ROOT/host/beepy-nav
    elif [ -x "$ROOT/beepy-nav/beepy-nav" ]; then NAV=$ROOT/beepy-nav/beepy-nav
    else
        echo "ride2fixture: no beepy-nav built; make host, or set NAV=" >&2
        exit 1
    fi
fi

[ -r "$LOG" ] || { echo "ride2fixture: cannot read $LOG" >&2; exit 1; }
[ -r "$ROUTE" ] || { echo "ride2fixture: cannot read $ROUTE" >&2; exit 1; }
case $NAME in
    *[!A-Za-z0-9_-]*|"") echo "ride2fixture: NAME must be [A-Za-z0-9_-]+" >&2
                         exit 1 ;;
esac

mkdir -p "$RDIR"
# Copied, not moved and not symlinked: the fixture has to survive the log
# being tidied off the device, and a fixture that is a link into somebody's
# home directory is not a fixture.
cp "$LOG" "$RDIR/$NAME.nmea"
echo "ride2fixture: $RDIR/$NAME.nmea  ($(wc -c < "$RDIR/$NAME.nmea") bytes)"

"$NAV" --route "$ROUTE" --replay "$RDIR/$NAME.nmea" --headless --no-pace \
    --fps "$FPS" \
    --trace "$RDIR/$NAME.tsv" \
    --trace-frames "$RDIR/$NAME-frames.tsv"

DEVTSV=${LOG%.nmea}.tsv
if [ -r "$DEVTSV" ]; then
    echo "ride2fixture: comparing the replay against the device's own trace"
    python3 - "$DEVTSV" "$RDIR/$NAME.tsv" <<'PY'
import sys

# The columns that are pure per-fix functions of the sentences. Everything
# else on the row is a function of the frame clock and cannot agree between a
# live run and a replay -- see the header comment.
COLS = ("t", "lat", "lon", "seg", "along_m", "off_m", "off_latched",
        "cue_i", "cue_m", "togo_m", "pct")
TOL = {"lat": 1e-9, "lon": 1e-9}


def load(path):
    names, rows = None, []
    for ln in open(path):
        ln = ln.rstrip("\n")
        if not ln:
            continue
        if ln.startswith("#"):
            names = names or ln[1:].split("\t")
            continue
        rows.append(dict(zip(names, (float(v) for v in ln.split("\t")))))
    return rows


dev, rep = load(sys.argv[1]), load(sys.argv[2])
if not dev:
    print("  the device trace has no fix rows -- nothing to compare "
          "(a ride with no fix is still a valid fixture)")
    raise SystemExit(0)
# A device trace SHORTER than the replay is the ordinary killed-run case and
# not a fault: the raw bytes of the last epoch reach the disk before the trace
# row for them does, so a SIGKILL between the two leaves exactly this. Longer
# is a real problem -- the replay lost sentences the device saw.
if len(dev) > len(rep):
    print(f"  MISMATCH  the device saw {len(dev)} fixes, the replay only "
          f"{len(rep)}")
    raise SystemExit(1)
short = len(rep) - len(dev)
if short:
    print(f"  the device trace is {short} row(s) short of the replay -- "
          f"expected if the run was killed; comparing the {len(dev)} in common")
bad = 0
for i, (a, b) in enumerate(zip(dev, rep)):
    for c in COLS:
        if c not in a or c not in b:
            continue
        if abs(a[c] - b[c]) > TOL.get(c, 1e-6):
            print(f"  MISMATCH  row {i} {c}: device {a[c]:g}, replay {b[c]:g}")
            bad += 1
            if bad > 5:
                raise SystemExit(1)
print(f"  the replay reproduces all {len(dev)} device fixes exactly"
      if not bad else "  the replay does NOT reproduce the ride")
raise SystemExit(1 if bad else 0)
PY
else
    echo "ride2fixture: no $DEVTSV beside the log; skipping the cross-check"
fi

FIXES=$(grep -vc '^#' "$RDIR/$NAME.tsv" || true)
cat <<EOF

--- paste into the Makefile ---

# in REPLAYS, so \`make clean\` and the device sync agree about it:
#   \$(RDIR)/$NAME.nmea
# NOTE: unlike the mknmea fixtures this one is a RECORDING and has no rule
# that regenerates it -- it must be committed.

	@echo "--- T-$(echo "$NAME" | tr '[:lower:]-' '[:upper:]_'): a recorded ride"
	\$(NAV) --route $(printf '%s' "$ROUTE" | sed "s#^$ROOT/##") \\
		--replay \$(RDIR)/$NAME.nmea --headless \$(FPS8) \\
		--trace \$(RDIR)/$NAME.tsv \\
		--trace-frames \$(RDIR)/$NAME-frames.tsv
	\$(ASSERT) \$(RDIR)/$NAME.tsv --monotone-up cue_i
	\$(ASSERT) \$(RDIR)/$NAME-frames.tsv --dr-closed-form 0.005

--- $FIXES fixes; replace the assertions above with what the failure was ---

EOF

#!/usr/bin/env python3
"""
assert_trace.py -- assertions over a `beepy-nav --trace` TSV.

DESIGN.md 10 states the route maths as properties of a whole ride rather than
of a single frame: "% DONE climbs monotonically to 100", "TO GO decreases
monotonically", "the latch fires once and clears once". Those are exactly the
things a per-frame golden cannot see, so they are checked here, over the row
per fix the navigator writes.

    assert_trace.py TRACE.tsv --monotone-up pct --monotone-down togo_m \\
                              --zero off_latched --final pct '>=' 99

  --monotone-up COL     never decreases (a tolerance of one ULP-ish 1e-6)
  --monotone-down COL   never increases
  --max COL V           every value <= V
  --always COL OP V     every value satisfies OP V   (OP: < <= > >= == !=)
  --final COL OP V      the LAST row satisfies OP V
  --constant COL [TOL]  every value within TOL (default 1e-9) of the first
  --latch-count COL N   the column, read as 0/1, rises exactly N times
  --zero COL            every value is exactly zero
  --any COL OP V        at least one row satisfies OP V -- the guard against
                        an assertion that passes because nothing happened
  --finite              no NaN and no infinity anywhere in the trace. Worth its
                        own assertion because every other one here is written
                        with a comparison, and every comparison against NaN is
                        false -- so a column that has quietly become NaN makes
                        --max, --always and --monotone-* pass, silently. It is
                        the state a page rendered from an unset frame reaches
                        first (DESIGN.md 1.5: no route means no snap, so a
                        position derived from one would be 0/0)
  --rows                print the row count and exit 0

Over a `--trace-frames` TSV, which is one row per rendered FRAME rather than
per fix, four more. These check DESIGN.md 6.1/6.3/6.4/7.5, all of which happen
eight times a second and none of which a per-fix trace can see:

  --dr-closed-form TOL  the drawn position is exactly the extrapolation 6.3
                        specifies: base + bearing(course).speed.dt, plus the
                        correction being eased in
  --dr-ease             a correction of 5 m or less eases over exactly three
                        frames (3/4, 2/4, 1/4, gone); a larger one is taken
                        whole on the fix's own frame
  --head-ewma TAU TOL   the heading trace is the circular EWMA of 6.1 with
                        that time constant, and is frozen below 3 km/h
  --cue-led N           7.5's alerts: each of the three rungs fires at most
                        once per cue, never while the off-route latch is set,
                        never on a row that is not a fix, and at least N cues
                        rang all three
  --led-rungs N         exactly N rungs ring over the whole ride -- the check
                        that a mute suppressed what it should and nothing else
  --led-quiet T0 T1     nothing rings between those ride seconds
  --settles COL         once the column reaches zero it stays there -- 6.4's
                        frame skip on a display that has stopped changing
  --nofix SECS          1.1's NO FIX rule, over every gap the trace contains:
                        the row does not appear before SECS have passed, does
                        appear within SECS + 1, is gone on the frame after the
                        fixes return, and while it is up the countdown the
                        panel prints and the position it draws do not move

Each assertion prints PASS or FAIL with the offending row; the exit status is
0 only when all of them pass.
"""
import math
import sys

OPS = {"<": lambda a, b: a < b, "<=": lambda a, b: a <= b,
       ">": lambda a, b: a > b, ">=": lambda a, b: a >= b,
       "==": lambda a, b: a == b, "!=": lambda a, b: a != b}

EPS = 1e-6


def load(path):
    """-> (column names, list of dicts of floats)"""
    names, rows = None, []
    for ln in open(path):
        ln = ln.rstrip("\n")
        if not ln:
            continue
        if ln.startswith("#"):
            if names is None:
                names = ln[1:].split("\t")
            continue
        vals = ln.split("\t")
        rows.append({n: float(v) for n, v in zip(names, vals)})
    if names is None:
        raise SystemExit(f"{path}: no '#' header row")
    return names, rows


class Checker:
    def __init__(self, path):
        self.path = path
        self.names, self.rows = load(path)
        self.bad = 0

    def col(self, name):
        if name not in self.names:
            raise SystemExit(f"{self.path}: no column {name!r}; have "
                             + " ".join(self.names))
        return [r[name] for r in self.rows]

    def report(self, ok, what, detail=""):
        print(f"  {'PASS' if ok else 'FAIL'}  {what}"
              + (f"   {detail}" if detail else ""))
        self.bad += not ok

    def monotone(self, name, up):
        v = self.col(name)
        worst = None
        for i in range(1, len(v)):
            d = v[i] - v[i - 1]
            if (d < -EPS) if up else (d > EPS):
                if worst is None or abs(d) > abs(worst[2]):
                    worst = (i, v[i], d)
        self.report(worst is None,
                    f"{name} monotone {'up' if up else 'down'}",
                    "" if worst is None else
                    f"row {worst[0]} = {worst[1]:g}, step {worst[2]:+g}")

    def maximum(self, name, lim):
        v = self.col(name)
        bad = [(i, x) for i, x in enumerate(v) if x > lim]
        self.report(not bad, f"{name} <= {lim:g}",
                    "" if not bad else
                    f"{len(bad)} rows, worst row {max(bad, key=lambda p: p[1])}")

    def always(self, name, op, val):
        v = self.col(name)
        bad = [(i, x) for i, x in enumerate(v) if not OPS[op](x, val)]
        self.report(not bad, f"{name} always {op} {val:g}",
                    "" if not bad else f"{len(bad)} rows, first {bad[0]}")

    def final(self, name, op, val):
        v = self.col(name)
        if not v:
            self.report(False, f"final {name} {op} {val:g}", "no rows")
            return
        self.report(OPS[op](v[-1], val), f"final {name} {op} {val:g}",
                    f"last = {v[-1]:g}")

    def constant(self, name, tol):
        v = self.col(name)
        if not v:
            self.report(False, f"{name} constant", "no rows")
            return
        bad = [(i, x) for i, x in enumerate(v) if abs(x - v[0]) > tol]
        self.report(not bad, f"{name} constant (tol {tol:g})",
                    f"first = {v[0]:g}" if not bad else
                    f"{len(bad)} rows differ, first {bad[0]}")

    def latch_count(self, name, want):
        v = self.col(name)
        rises = sum(1 for i in range(1, len(v))
                    if v[i - 1] < 0.5 <= v[i])
        if v and v[0] >= 0.5:
            rises += 1                      # already latched on the first row
        falls = sum(1 for i in range(1, len(v)) if v[i - 1] >= 0.5 > v[i])
        self.report(rises == want, f"{name} rises exactly {want}x",
                    f"rose {rises}x, fell {falls}x")

    def finite(self):
        bad = []
        for i, r in enumerate(self.rows):
            for n in self.names:
                if not math.isfinite(r.get(n, 0.0)):
                    bad.append((i, n, r[n]))
        self.report(not bad, f"every cell of {len(self.names)} columns finite",
                    f"{len(self.rows)} rows" if not bad else
                    f"{len(bad)} cells, first {bad[0]}")

    def any_(self, name, op, val):
        v = self.col(name)
        ok = any(OPS[op](x, val) for x in v)
        self.report(ok, f"{name} is {op} {val:g} somewhere",
                    f"{sum(1 for x in v if OPS[op](x, val))} of {len(v)} rows")

    # ------------------------------------------------ one row per FRAME

    def dr_closed_form(self, tol):
        """DESIGN.md 6.3: pos(t) = last_fix + bearing(course).speed.(t-t_fix),
        plus whatever is left of the correction being eased in. Recomputed
        here from the columns the navigator wrote, never from its answer."""
        need = ("base_e", "base_n", "base_crs", "base_spd", "dt",
                "err_e", "err_n", "ease_w", "dr_e", "dr_n")
        c = {n: self.col(n) for n in need}
        worst, at = 0.0, -1
        for i in range(len(self.rows)):
            crs = math.radians(c["base_crs"][i])
            pe = c["base_e"][i] + math.sin(crs) * c["base_spd"][i] * c["dt"][i]
            pn = c["base_n"][i] + math.cos(crs) * c["base_spd"][i] * c["dt"][i]
            d = max(abs(pe + c["err_e"][i] * c["ease_w"][i] - c["dr_e"][i]),
                    abs(pn + c["err_n"][i] * c["ease_w"][i] - c["dr_n"][i]))
            if d > worst:
                worst, at = d, i
        self.report(worst <= tol,
                    f"drawn position matches the closed form (tol {tol:g} m)",
                    f"worst {worst:.3g} m at row {at} of {len(self.rows)}")

    def dr_ease(self):
        """6.3: within 5 m, ease across it over three frames; beyond 5 m,
        snap. The weights are 3/4, 2/4, 1/4 and then gone, so the correction
        is spread over exactly three frames and the fourth is clean."""
        isfix, fe = self.col("isfix"), self.col("fix_err")
        ew = self.col("ease_w")
        de, dn = self.col("dr_e"), self.col("dr_n")
        be, bn = self.col("base_e"), self.col("base_n")
        want = (0.75, 0.5, 0.25, 0.0)
        bad, eased, snapped = [], 0, 0
        for i, f in enumerate(isfix):
            if f < 0.5:
                continue
            if 0.0 < fe[i] <= 5.0:
                eased += 1
                for j, w in enumerate(want):
                    k = i + j
                    if k >= len(ew):
                        break
                    # A fix arriving mid-ease restarts it, which is correct;
                    # the sequence is only claimed while no fix interrupts.
                    if j and isfix[k] >= 0.5:
                        break
                    if abs(ew[k] - w) > 1e-9:
                        bad.append((k, f"ease_w {ew[k]:g}, want {w:g}"))
                        break
            elif fe[i] > 5.0:
                snapped += 1
                if abs(ew[i]) > 1e-9:
                    bad.append((i, f"snap left ease_w {ew[i]:g}"))
                # Taken whole: on the fix's own frame dt is zero, so the
                # position drawn IS the fix and not a blend towards it.
                elif abs(de[i] - be[i]) > 1e-3 or abs(dn[i] - bn[i]) > 1e-3:
                    bad.append((i, "snap did not land on the fix"))
        self.report(not bad, "<= 5 m eases over 3 frames, > 5 m snaps in 1",
                    f"{eased} eased, {snapped} snapped"
                    if not bad else f"{len(bad)} bad, first {bad[0]}")
        self.report(eased > 0, "the trace contains an eased correction",
                    f"{eased}")

    def head_ewma(self, tau, tol):
        """6.1: a circular EWMA over a TIME constant -- alpha = 1 - exp(-dt/tau)
        -- frozen below 3 km/h. tau is the caller's, and must be the one the
        code uses; this recomputes the whole trace from the previous row's
        heading and checks every step."""
        t, h = self.col("t"), self.col("heading_deg")
        crs, spd = self.col("course_deg"), self.col("base_spd")
        isfix = self.col("isfix")
        first = next((i for i, f in enumerate(isfix) if f >= 0.5), None)
        if first is None:
            self.report(False, "heading EWMA", "no fix rows")
            return
        worst, at, frozen, moving = 0.0, -1, 0, 0
        for i in range(first + 1, len(t)):
            kmh = spd[i] * 3.6
            # Right on the freeze threshold either answer is defensible and
            # the trace's 4 decimal places cannot say which side it was.
            if abs(kmh - 3.0) < 0.01:
                continue
            if kmh < 3.0:
                want = h[i - 1]
                frozen += 1
            else:
                dt = t[i] - t[i - 1]
                alpha = 1.0 - math.exp(-dt / tau) if dt > 0.0 else 0.0
                d = (crs[i] - h[i - 1] + 180.0) % 360.0 - 180.0
                want = h[i - 1] + d * alpha
                moving += 1
            err = abs((h[i] - want + 180.0) % 360.0 - 180.0)
            if err > worst:
                worst, at = err, i
        self.report(worst <= tol,
                    f"heading is the EWMA of tau = {tau:g} s (tol {tol:g} deg)",
                    f"worst {worst:.3g} deg at row {at}; "
                    f"{moving} moving, {frozen} frozen")
        # Which of the two branches a given ride exercises is the ride's
        # business; that BOTH are exercised is asserted by the caller, with
        # --any over base_spd, across the two fixtures that have them.

    def cue_led(self, want_full):
        """7.5: 500 m, 200 m and 50 m, one flash each per cue, and nothing at
        all while the off-route latch is set -- off route the announced cue is
        not the junction ahead, so a flash would be an instruction to turn
        where there is no turn."""
        led, off = self.col("led"), self.col("off_latched")
        cue, isfix = self.col("cue_i"), self.col("isfix")
        seen, bad = {}, []
        offrows = sum(1 for x in off if x)
        for i, m in enumerate(led):
            m = int(m)
            if not m:
                continue
            if off[i]:
                bad.append((i, "fired while off route"))
            if isfix[i] < 0.5:
                bad.append((i, "fired on a frame that is not a fix"))
            s = seen.setdefault(int(cue[i]), set())
            for b in range(3):
                if not (m & (1 << b)):
                    continue
                if b in s:
                    bad.append((i, f"cue {int(cue[i])} rang rung {b} twice"))
                s.add(b)
        full = sum(1 for v in seen.values() if len(v) == 3)
        self.report(not bad,
                    "each rung fires once per cue, never off route",
                    f"{len(seen)} cues rang, {offrows} rows off route"
                    if not bad else f"{len(bad)} bad, first {bad[0]}")
        self.report(full >= want_full, f"at least {want_full} cues rang all "
                    f"three rungs", f"{full} of {len(seen)}")

    def led_rungs(self, want):
        """The total number of RUNGS rung, not of rows: two rungs crossed on
        one fix arrive in one row as a two-bit mask, and counting rows would
        make the expected number depend on how fast the ride approached the
        junction."""
        n = sum(bin(int(m)).count("1") for m in self.col("led"))
        self.report(n == want, f"exactly {want} rungs ring over the ride",
                    f"{n}")

    def led_quiet(self, t0, t1):
        """A muted stretch. Stated over an interval rather than per cue
        because that is what the rider did: they switched the alerts off at
        one moment and on again at another."""
        t, led = self.col("t"), self.col("led")
        bad = [(i, t[i], int(led[i]))
               for i in range(len(t)) if t0 <= t[i] <= t1 and led[i]]
        self.report(not bad, f"nothing rings between t={t0:g} and t={t1:g}",
                    "" if not bad else f"{len(bad)} fired, first {bad[0]}")

    def settles(self, name):
        """6.4: the frame is skipped when it memcmps equal to the last one
        presented, so a display that has stopped changing costs nothing. On a
        genuinely still ride that means the presents stop -- and never
        resume."""
        v = self.col(name)
        first = next((i for i, x in enumerate(v) if x == 0.0), None)
        if first is None:
            self.report(False, f"{name} settles to zero",
                        f"never zero in {len(v)} rows")
            return
        after = [(i, x) for i, x in enumerate(v) if i > first and x != 0.0]
        self.report(not after, f"{name} stops at the first identical frame",
                    f"row {first} of {len(v)}, {len(v) - first} skipped"
                    if not after else f"{len(after)} later, first {after[0]}")

    def nofix(self, secs):
        """DESIGN.md 1.1: NO FIX after `secs` without a valid fix, and nothing
        derived from position moving while it is up.

        The gaps are found in the trace rather than named by the caller: a
        fixture that stopped emitting fixes somewhere other than where the
        Makefile thought would otherwise be asserted about the wrong seconds.
        A gap is a run of frames between two fix rows longer than `secs`."""
        t, isfix, nf = self.col("t"), self.col("isfix"), self.col("nofix")
        cq = self.col("cue_q")
        de, dn = self.col("dr_e"), self.col("dr_n")
        fixes = [i for i, f in enumerate(isfix) if f >= 0.5]
        if not fixes:
            self.report(False, "NO FIX", "no fix rows in the trace")
            return

        gaps, early, late, moved, stuck = [], [], [], [], []
        for a, b in zip(fixes, fixes[1:]):
            if t[b] - t[a] <= secs:
                continue
            gaps.append((t[a], t[b]))
            for i in range(a, b + 1):
                # Before SECS have elapsed the panel must say nothing: a
                # warning that fires on one dropped sentence is a warning a
                # rider learns to ignore.
                if t[i] - t[a] < secs and nf[i]:
                    early.append((i, t[i]))
            # ... and it must have appeared within a second of the deadline.
            if not any(nf[i] for i in range(a, b + 1)
                       if t[i] <= t[a] + secs + 1.0):
                late.append((a, t[a]))
            # Frozen while the row is up. The drawn position stopped two
            # seconds into the gap (DR_MAX_EXTRAP), which is a second before
            # the row appeared, so "constant over the whole nofix stretch" is
            # exactly the claim -- no phantom distance crosses the gap.
            up = [i for i in range(a, b + 1) if nf[i]]
            for i in up[1:]:
                if abs(cq[i] - cq[up[0]]) > 1e-9:
                    stuck.append((i, f"cue_q {cq[i]:g} != {cq[up[0]]:g}"))
                if (abs(de[i] - de[up[0]]) > 1e-6
                        or abs(dn[i] - dn[up[0]]) > 1e-6):
                    moved.append((i, f"drawn position moved {de[i] - de[up[0]]:+.3f},"
                                     f"{dn[i] - dn[up[0]]:+.3f} m"))
        # Recovery: cleared on the fix that ends the gap, so no frame from
        # there on may still carry it until the next gap opens.
        back = [(i, t[i]) for a, b in zip(fixes, fixes[1:])
                if t[b] - t[a] > secs
                for i in range(b, min(b + 20, len(t)))
                if nf[i] and isfix[i] >= 0.5]

        self.report(bool(gaps), f"the trace contains a gap longer than {secs:g} s",
                    f"{len(gaps)} gaps, first {gaps[0] if gaps else '-'}")
        self.report(not early, f"NO FIX does not appear inside {secs:g} s",
                    "" if not early else f"{len(early)} rows, first {early[0]}")
        self.report(not late, f"NO FIX appears within {secs + 1:g} s of the gap",
                    "" if not late else f"{len(late)} gaps, first {late[0]}")
        self.report(not back, "NO FIX is gone on the fix that ends the gap",
                    "" if not back else f"{len(back)} rows, first {back[0]}")
        self.report(not stuck, "the countdown does not advance during a gap",
                    "" if not stuck else f"{len(stuck)} rows, first {stuck[0]}")
        self.report(not moved, "the drawn position is frozen during a gap",
                    "" if not moved else f"{len(moved)} rows, first {moved[0]}")

    def zero(self, name):
        v = self.col(name)
        bad = [(i, x) for i, x in enumerate(v) if x != 0.0]
        self.report(not bad, f"{name} is zero throughout",
                    "" if not bad else f"{len(bad)} rows, first {bad[0]}")


def main(argv):
    if not argv:
        raise SystemExit(__doc__.strip())
    c = Checker(argv[0])
    print(f"{argv[0]}: {len(c.rows)} rows")
    it = iter(argv[1:])
    for a in it:
        if a == "--monotone-up":
            c.monotone(next(it), True)
        elif a == "--monotone-down":
            c.monotone(next(it), False)
        elif a == "--max":
            n = next(it)
            c.maximum(n, float(next(it)))
        elif a == "--always":
            n, op = next(it), next(it)
            c.always(n, op, float(next(it)))
        elif a == "--final":
            n, op = next(it), next(it)
            c.final(n, op, float(next(it)))
        elif a == "--constant":
            n = next(it)
            c.constant(n, 1e-9)
        elif a == "--constant-tol":
            n = next(it)
            c.constant(n, float(next(it)))
        elif a == "--latch-count":
            n = next(it)
            c.latch_count(n, int(next(it)))
        elif a == "--zero":
            c.zero(next(it))
        elif a == "--any":
            n, op = next(it), next(it)
            c.any_(n, op, float(next(it)))
        elif a == "--finite":
            c.finite()
        elif a == "--dr-closed-form":
            c.dr_closed_form(float(next(it)))
        elif a == "--dr-ease":
            c.dr_ease()
        elif a == "--head-ewma":
            tau = float(next(it))
            c.head_ewma(tau, float(next(it)))
        elif a == "--cue-led":
            c.cue_led(int(next(it)))
        elif a == "--led-rungs":
            c.led_rungs(int(next(it)))
        elif a == "--led-quiet":
            t0 = float(next(it))
            c.led_quiet(t0, float(next(it)))
        elif a == "--settles":
            c.settles(next(it))
        elif a == "--nofix":
            c.nofix(float(next(it)))
        elif a == "--rows":
            pass
        else:
            raise SystemExit(__doc__.strip())
    if not c.rows:
        print("  FAIL  the trace is empty")
        c.bad += 1
    print(f"{argv[0]}: {'PASS' if not c.bad else f'FAIL ({c.bad})'}")
    return 1 if c.bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

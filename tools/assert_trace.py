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
  --rows                print the row count and exit 0

Each assertion prints PASS or FAIL with the offending row; the exit status is
0 only when all of them pass.
"""
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

#!/usr/bin/env python3
"""mkpacefix.py -- synthesise the pacing trace a BROKEN player would produce.

T-VID-NOSPRINT needs a trace that beepy-vid must never emit, so it cannot come
from beepy-vid. This writes the one a backlog-dumping player produces: after a
stall it shows every frame it owes as fast as it can, then resumes cadence. It
drops nothing, shows everything, and the stall is still its only long gap --
so --dropped, --max-frame-gap and --finite all pass it. Only a MINIMUM gap
sees it, which is the point of the assertion it exists to prove.

Pure stdlib, no clock, no randomness: the same bytes every run.
"""
import argparse

FPS, N, STALL_AT, STALL, BURST = 8.0, 24, 5, 0.500, 0.004


def sprint_rows():
    rows, t = [], 0.0
    for i in range(N):
        if i == STALL_AT:
            t += STALL
            for k in range(4):                 # the backlog, dumped
                rows.append((t + k * BURST, STALL_AT + k))
            t += 4 * BURST
        elif STALL_AT < i < STALL_AT + 4:
            continue                            # already emitted above
        else:
            rows.append((t, i))
            t += 1.0 / FPS
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sprint", required=True)
    a = ap.parse_args()
    with open(a.sprint, "w") as f:
        f.write("#t\tpts\tidx\tpresented\twritten\tdropped\n")
        for t, i in sprint_rows():
            f.write("%.6f\t%.6f\t%d\t1\t1\t0\n" % (t, i / FPS, i))


if __name__ == "__main__":
    main()

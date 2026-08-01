#!/usr/bin/env python3
"""fakesink.py -- an audio sink that consumes at a rate you choose.

This is what makes A/V sync gateable without a speaker. beepy-vid derives its
clock from bytes the sink accepts, so a sink that drains deliberately slowly
must drag the video with it -- and a player that ignores the audio clock and
free-runs on CLOCK_MONOTONIC looks identical in every other respect. Only a
sink that lies about its rate can tell them apart.

    --rate N        bytes per second to consume (the true rate)
    --factor F      consume F times as fast; 0.99 is a sink running 1% slow
    --buffer N      bytes to swallow instantly before throttling, standing in
                    for the 150-250 ms a real A2DP sink holds

Pure stdlib, and the only clock it touches is its own consumption schedule, so
the player's behaviour is what varies and this is not.
"""
import argparse
import sys
import time


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rate", type=float, required=True)
    ap.add_argument("--factor", type=float, default=1.0)
    ap.add_argument("--buffer", type=int, default=0)
    ap.add_argument("--report", help="write total bytes consumed here at exit")
    a = ap.parse_args()

    rate = a.rate * a.factor
    total = 0
    t0 = time.monotonic()
    swallowed = 0
    inp = sys.stdin.buffer

    while True:
        chunk = inp.read(4096)
        if not chunk:
            break
        total += len(chunk)
        if swallowed < a.buffer:
            swallowed += len(chunk)
            continue
        # Hold the pipe closed until this many bytes are "played". The player
        # sees back-pressure exactly as it would from a real sink.
        due = t0 + (total - a.buffer) / rate
        now = time.monotonic()
        if due > now:
            time.sleep(due - now)

    if a.report:
        with open(a.report, "w") as f:
            f.write("%d\n" % total)


if __name__ == "__main__":
    main()

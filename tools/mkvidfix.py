#!/usr/bin/env python3
"""mkvidfix.py -- generate the .vid test fixtures, deterministically.

Pure Python standard library: no numpy, no Pillow, no ffmpeg. That is the whole
point. tools/mkvid.py needs numpy and ffmpeg needs a Mac, but a fixture that
`make check` depends on must be reproducible from a seed in either lane, which
is the argument Makefile:349-352 already makes for the .nmea replays -- they
come back byte for byte from mknmea.py and a seed, so they are gitignored
rather than committed.

The content is chosen so that the encoder's cases are all exercised:

  * a static textured background        -> rows that never change
  * a bar moving 3 px/frame             -> a narrow dirty row span
  * a full-frame flash every 8 frames   -> a hard cut, so a keyframe is forced
  * two consecutive identical frames    -> the size == 0 path

    python3 tools/mkvidfix.py --gray8 clip.gray8 --pcm clip.s16
"""
import argparse
import math
import struct

W, H = 400, 225          # the 16:9 image rect, before letterboxing to 400x240
NFRAME = 24
RATE, CHANNELS = 24000, 2
# A quarter second: enough for the reader's audio section to be exercised and
# for pack_audio() to be gated, short enough that the committed .vid stays
# small. `make sync` rsyncs the whole tree on every device run.
AUDIO_SECONDS = 0.25


def lcg(seed):
    """A named, fixed generator. random.Random would also be reproducible, but
    only for a given Python; this is reproducible for all time."""
    x = seed & 0xFFFFFFFF
    while True:
        x = (1103515245 * x + 12345) & 0x7FFFFFFF
        yield x >> 16


def background():
    """Static texture: a smooth ramp plus fixed per-pixel noise."""
    rnd = lcg(20260801)
    rows = []
    for y in range(H):
        row = bytearray(W)
        for x in range(W):
            v = 110 + 55 * math.sin(x / 41.0) + 35 * math.cos(y / 27.0)
            v += (next(rnd) % 21) - 10
            row[x] = max(0, min(255, int(v)))
        rows.append(row)
    return rows


def frames():
    bg = background()
    out = []
    for k in range(NFRAME):
        # frame 13 repeats frame 12 exactly, so the packer must emit size == 0
        src = 12 if k == 13 else k
        flash = (src % 8) == 7          # a hard cut every 8 frames
        buf = bytearray(W * H)
        bar = (src * 3) % (W - 24)
        for y in range(H):
            base = bg[y]
            off = y * W
            if flash:
                for x in range(W):
                    buf[off + x] = 255 - base[x]
            else:
                buf[off:off + W] = base
        if not flash:
            # a moving bright bar, 24 px wide, spanning rows 60..164 only
            for y in range(60, 165):
                off = y * W
                for x in range(bar, bar + 24):
                    buf[off + x] = 245
        out.append(bytes(buf))
    return out


def audio():
    n = int(RATE * AUDIO_SECONDS)
    out = bytearray()
    for i in range(n):
        t = i / RATE
        v = int(9000 * math.sin(2 * math.pi * 440 * t))
        out += struct.pack("<hh", v, v)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gray8", required=True)
    ap.add_argument("--pcm")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    fr = frames()
    with open(a.gray8, "wb") as f:
        for x in fr:
            f.write(x)
    if a.pcm:
        with open(a.pcm, "wb") as f:
            f.write(audio())
    if not a.quiet:
        print("mkvidfix: %d frames %dx%d -> %s" % (len(fr), W, H, a.gray8))
        if a.pcm:
            print("mkvidfix: %.1f s %d Hz %d ch -> %s"
                  % (AUDIO_SECONDS, RATE, CHANNELS, a.pcm))


if __name__ == "__main__":
    main()

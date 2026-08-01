#!/usr/bin/env python3
"""mkvid.py -- build a .vid pack for beepy-vid. Mac side; see beepy-vid/DESIGN.md.

The pipeline is split at the gray8 boundary, and the split is the point:

    IN.mp4  --ffmpeg-->  clip.gray8 + clip.s16      OUTSIDE the gate
    gray8 + s16  --mkvid.py-->  clip.vid            INSIDE the gate

ffmpeg's scaler output depends on its version, its SIMD path and its build. Put
that inside a `cmp`-exact determinism gate and the gate silently becomes "the
same ffmpeg", so a Homebrew upgrade reddens a golden with nothing changed. The
repo already solved this once: pbf2osm.py turns a Geofabrik extract into the
committed osm-asok.json, and mktiles.py -- the deterministic half -- is what
the gate exercises. Same shape here.

Everything from gray8 onwards is integer arithmetic. There are no float ties to
break, which is what lets these packs be compared with `cmp` where the nav
design gate needs a 480-pixel tolerance (Makefile:586-594 exists because a
coverage value landed on exactly 50% and the tie-break flipped).

    python3 tools/mkvid.py --gray8 clip.gray8 --size 400x225 --pcm clip.s16 \
                           -o clip.vid --fps 24 --gop 24
    python3 tools/mkvid.py --video film.mp4 -o film.vid
    python3 tools/mkvid.py --info film.vid
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import zlib

import numpy as np

PANEL_W, PANEL_H = 400, 240
STRIDE = PANEL_W // 8
PLANE = STRIDE * PANEL_H

MAGIC = b"BEEPYVID"
VERSION = 1
HEADER_BYTES = 64
NSECT = 3
IDX_ENTRY = 12

PACKF_INK_MSB = 0x0001
PACKF_DEFLATE = 0x0002
PACKF_AUDIO = 0x0004
FRAMEF_KEY = 0x01

RAW, DEFLATE, XOR_SPANS, XOR_DEFLATE = 0, 1, 2, 3
MODE_NAME = {RAW: "RAW", DEFLATE: "DEFLATE", XOR_SPANS: "XOR_SPANS",
             XOR_DEFLATE: "XOR_DEFLATE"}

# u32 offsets cap the container at 4 GiB. Stated rather than guarded around,
# matching mkpack.py's own u32 offsets; ~4 hours at 24 fps with audio.
MAX_PACK = 0xFFFFFFFF


# ------------------------------------------------------------------ dither

def bayer(n):
    m = np.array([[0]], dtype=np.int64)
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    return m


def threshold_tile(kind):
    """Integer thresholds spanning 0..255 with the matrix mean at 128.

    Written out as integers deliberately: the comparison luma < T then has no
    tie to break. A float threshold would put values exactly on the boundary
    and the rounding would decide the pixel, which is the failure mode
    design_gate.py:22-32 exists because of.
    """
    if kind == "bayer4":
        return bayer(4) * 16 + 8          # 0..15 -> 8..248
    if kind == "bayer8":
        return bayer(8) * 4 + 2           # 0..63 -> 2..254
    raise SystemExit("mkvid: unknown dither %r" % kind)


def dither_frames(frames, w, h, x0, y0, kind, hysteresis, bars_ink):
    """gray8 frames -> packed 1bpp planes, ink = 1 = black, MSB leftmost.

    The matrix is anchored to the IMAGE RECT, not to screen (0,0). Anchoring to
    the screen would give the top image row a phase-shifted pattern wherever
    y0 is not a multiple of the cell, and the seam would sit exactly on the
    letterbox boundary.

    Hysteresis makes a pixel reluctant to change: it is what keeps the
    frame-to-frame XOR sparse, and measurement puts it ahead of the matrix
    choice for compression (DESIGN.md 3.7). It costs edge smearing in motion,
    which no byte count can see.
    """
    m = threshold_tile(kind)
    n = m.shape[0]
    T = np.tile(m, (h // n + 2, w // n + 2))[:h, :w]
    planes = []
    prev_ink = None
    for f in frames:
        g = np.frombuffer(f, dtype=np.uint8).reshape(h, w).astype(np.int64)
        thr = T if (not hysteresis or prev_ink is None) else \
            T + np.where(prev_ink, hysteresis, -hysteresis)
        ink = g < thr
        prev_ink = ink
        full = np.zeros((PANEL_H, PANEL_W), dtype=bool)
        if bars_ink:
            full[:, :] = True
        full[y0:y0 + h, x0:x0 + w] = ink
        planes.append(np.packbits(full, axis=1).tobytes())
    return planes


# ------------------------------------------------------------------ encode

def deflate(b):
    c = zlib.compressobj(9, zlib.DEFLATED, -15)
    return c.compress(b) + c.flush()


def dirty_rows(cur, prv):
    a = np.frombuffer(cur, np.uint8).reshape(PANEL_H, STRIDE)
    b = np.frombuffer(prv, np.uint8).reshape(PANEL_H, STRIDE)
    d = np.nonzero((a ^ b).any(axis=1))[0]
    if len(d) == 0:
        return None
    return int(d[0]), int(d[-1])


def spans_payload(cur, prv):
    """Per-row byte spans of the XOR. A row is split when the gap between dirty
    bytes exceeds 3, the entry overhead -- a fixed greedy rule, stated so the
    output is reproducible rather than merely deterministic today."""
    a = np.frombuffer(cur, np.uint8).reshape(PANEL_H, STRIDE)
    b = np.frombuffer(prv, np.uint8).reshape(PANEL_H, STRIDE)
    x = a ^ b
    out = bytearray(b"\x00\x00")
    nrow = 0
    for y in np.nonzero(x.any(axis=1))[0]:
        idx = np.nonzero(x[y])[0]
        start = prev = int(idx[0])
        for i in list(int(v) for v in idx[1:]) + [None]:
            if i is None or i - prev > 3:
                cnt = prev - start + 1
                out += bytes((int(y), start, cnt)) + x[y, start:start + cnt].tobytes()
                nrow += 1
                if i is not None:
                    start = i
            if i is not None:
                prev = i
    out[0:2] = struct.pack("<H", nrow)
    return bytes(out)


def encode_frame(i, planes, keyframe, allow_deflate):
    """Return (mode, flags, y0, y1, payload). Best of the candidates, ties
    broken by ascending mode number -- stated, because min() over equal-length
    byte strings is otherwise arbitrary and would make the pack depend on dict
    ordering."""
    cur = planes[i]
    if keyframe:
        cands = [(RAW, cur)]
        if allow_deflate:
            cands.append((DEFLATE, deflate(cur)))
        mode, pay = min(cands, key=lambda mp: (len(mp[1]), mp[0]))
        return mode, FRAMEF_KEY, 0, PANEL_H - 1, pay

    prv = planes[i - 1]
    span = dirty_rows(cur, prv)
    if span is None:
        return RAW, 0, 255, 0, b""        # identical: size 0, no SPI at all
    y0, y1 = span
    band = cur[y0 * STRIDE:(y1 + 1) * STRIDE]
    xorb = bytes(np.frombuffer(band, np.uint8) ^
                 np.frombuffer(prv[y0 * STRIDE:(y1 + 1) * STRIDE], np.uint8))
    cands = [(RAW, band), (XOR_SPANS, spans_payload(cur, prv))]
    if allow_deflate:
        cands.append((DEFLATE, deflate(band)))
        cands.append((XOR_DEFLATE, deflate(xorb)))
    mode, pay = min(cands, key=lambda mp: (len(mp[1]), mp[0]))
    return mode, 0, y0, y1, pay


def build(planes, fps_num, fps_den, gop, audio, audio_rate, audio_ch,
          img, hysteresis, dither_id, allow_deflate):
    n = len(planes)
    entries, arena = [], bytearray()
    used_deflate = False
    since = 0
    for i in range(n):
        key = (i == 0) or (since >= gop - 1)
        mode, flags, y0, y1, pay = encode_frame(i, planes, key, allow_deflate)
        if not key and mode in (DEFLATE, XOR_DEFLATE) and len(pay) >= PLANE:
            pass  # nothing special; kept for readability of the branch above
        since = 0 if flags & FRAMEF_KEY else since + 1
        if mode in (DEFLATE, XOR_DEFLATE):
            used_deflate = True
        entries.append([mode, flags, y0, y1, pay])
        arena += pay

    idx_off = HEADER_BYTES + NSECT * 8
    frames_off = idx_off + n * IDX_ENTRY
    audio_off = frames_off + len(arena)
    total = audio_off + len(audio)
    if total > MAX_PACK:
        raise SystemExit("mkvid: pack would exceed the 4 GiB u32 offset limit")

    flags = PACKF_INK_MSB
    if used_deflate:
        flags |= PACKF_DEFLATE
    if audio:
        flags |= PACKF_AUDIO

    hdr = bytearray(HEADER_BYTES)
    hdr[0:8] = MAGIC
    struct.pack_into("<HHHH", hdr, 8, VERSION, HEADER_BYTES, flags, NSECT)
    struct.pack_into("<HH", hdr, 16, PANEL_W, PANEL_H)
    struct.pack_into("<IIII", hdr, 20, n, fps_num, fps_den, gop)
    struct.pack_into("<I", hdr, 36, audio_rate if audio else 0)
    struct.pack_into("<HH", hdr, 40, audio_ch if audio else 0, 16 if audio else 0)
    struct.pack_into("<I", hdr, 44, len(audio))
    struct.pack_into("<HHHH", hdr, 48, img[0], img[1], img[2], img[3])
    struct.pack_into("<HH", hdr, 56, hysteresis, dither_id)

    sect = struct.pack("<IIIIII", idx_off, n, frames_off, len(arena),
                       audio_off, len(audio))

    idx = bytearray()
    at = frames_off
    for mode, flg, y0, y1, pay in entries:
        off = at if pay else 0
        idx += struct.pack("<IIBBBB", off, len(pay), mode, flg, y0, y1)
        at += len(pay)

    return bytes(hdr) + sect + bytes(idx) + bytes(arena) + audio, entries


# -------------------------------------------------------------------- fit

def fit_rect(sw, sh):
    """Letterbox into the 400x240 panel. The odd pixel goes right and bottom
    (floor), stated so two builds agree."""
    if sw * PANEL_H >= sh * PANEL_W:
        w = PANEL_W
        h = max(1, (PANEL_W * sh + sw // 2) // sw)
    else:
        h = PANEL_H
        w = max(1, (PANEL_H * sw + sh // 2) // sh)
    w, h = min(w, PANEL_W), min(h, PANEL_H)
    return (PANEL_W - w) // 2, (PANEL_H - h) // 2, w, h


# ----------------------------------------------------------------- ffmpeg

def run_ffmpeg(path, out_gray, out_pcm, fps, rate, ch, ss, dur):
    probe = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "stream=width,height", "-of", "csv=p=0:s=x", path],
        capture_output=True, text=True, check=True).stdout.strip()
    sw, sh = (int(v) for v in probe.split("x")[:2])
    x0, y0, w, h = fit_rect(sw, sh)
    cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
           "-fflags", "+bitexact", "-flags", "+bitexact"]
    if ss:
        cmd += ["-ss", str(ss)]
    cmd += ["-i", path]
    # -t belongs in EACH output's option group. Placed once after -i it limits
    # only the first output, which produced a 30-second video with a
    # 156-second soundtrack -- the pack was ten times larger than it needed to
    # be and the audio outlived the picture by two minutes.
    lim = ["-t", str(dur)] if dur else []
    cmd += lim + ["-map", "0:v", "-vf",
                  "scale=%d:%d:flags=area:sws_dither=none,format=gray" % (w, h),
                  "-r", fps, "-pix_fmt", "gray", "-f", "rawvideo",
                  "-threads", "1", out_gray]
    cmd += lim + ["-map", "0:a?", "-af", "aresample=%d" % rate, "-ac", str(ch),
                  "-ar", str(rate), "-sample_fmt", "s16", "-f", "s16le",
                  out_pcm]
    subprocess.run(cmd, check=True)
    return w, h, x0, y0


# ------------------------------------------------------------------- info

def info(path):
    b = open(path, "rb").read()
    if len(b) < HEADER_BYTES or b[0:8] != MAGIC:
        raise SystemExit("mkvid: %s is not a .vid pack" % path)
    ver, hb, flags, nsect = struct.unpack_from("<HHHH", b, 8)
    w, h = struct.unpack_from("<HH", b, 16)
    n, fn, fd, gop = struct.unpack_from("<IIII", b, 20)
    arate, = struct.unpack_from("<I", b, 36)
    ach, abits = struct.unpack_from("<HH", b, 40)
    abytes, = struct.unpack_from("<I", b, 44)
    ix, iy, iw, ih = struct.unpack_from("<HHHH", b, 48)
    hyst, dith = struct.unpack_from("<HH", b, 56)
    print("magic      BEEPYVID  version %d  header %d  nsect %d" % (ver, hb, nsect))
    print("flags      0x%04x%s%s%s" % (flags,
          " ink-msb" if flags & PACKF_INK_MSB else "",
          " deflate" if flags & PACKF_DEFLATE else "",
          " audio" if flags & PACKF_AUDIO else ""))
    print("frame      %dx%d   image rect %d,%d %dx%d" % (w, h, ix, iy, iw, ih))
    print("frames     %d   fps %d/%d (%.3f)   gop %d" % (n, fn, fd, fn / fd, gop))
    print("audio      %d Hz  %d ch  %d bit  %d bytes" % (arate, ach, abits, abytes))
    print("dither     %s   hysteresis %d" % ({0: "bayer8", 1: "bayer4"}.get(dith, dith), hyst))
    counts = {}
    tot = 0
    for i in range(n):
        off = HEADER_BYTES + NSECT * 8 + i * IDX_ENTRY
        _, size, mode = struct.unpack_from("<IIB", b, off)
        counts[mode] = counts.get(mode, 0) + 1
        tot += size
    print("modes      " + "  ".join("%s=%d" % (MODE_NAME.get(m, m), counts[m])
                                    for m in sorted(counts)))
    print("payload    %d bytes total, %.0f B/frame mean" % (tot, tot / n))
    print("sha256     %s" % hashlib.sha256(b).hexdigest())


# ------------------------------------------------------------------- main

def parse_fps(s):
    if "/" in s:
        a, b = s.split("/", 1)
        return int(a), int(b)
    if "." in s:
        raise SystemExit("mkvid: --fps must be an integer or a ratio like "
                         "24000/1001; a float rate would round differently "
                         "between builds")
    return int(s), 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--video")
    ap.add_argument("--gray8")
    ap.add_argument("--size", help="WxH of the gray8 frames")
    ap.add_argument("--pcm")
    ap.add_argument("--info")
    ap.add_argument("--verify", metavar="PLANES",
                    help="compare a viddecode output against our own dither")
    ap.add_argument("-o", "--out")
    ap.add_argument("--fps", default="24")
    ap.add_argument("--gop", type=int, default=24)
    ap.add_argument("--hysteresis", type=int, default=8)
    ap.add_argument("--dither", default="bayer4", choices=["bayer4", "bayer8"])
    ap.add_argument("--no-deflate", action="store_true")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--bars", default="ink", choices=["ink", "paper"])
    ap.add_argument("--audio-rate", type=int, default=24000)
    ap.add_argument("--audio-channels", type=int, default=2)
    ap.add_argument("--start")
    ap.add_argument("--duration")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    if a.info:
        info(a.info)
        return
    if not a.out:
        raise SystemExit("mkvid: -o is required")
    if a.gop < 1:
        raise SystemExit("mkvid: --gop must be at least 1")
    if not 0 <= a.hysteresis <= 127:
        raise SystemExit("mkvid: --hysteresis must be 0..127")

    fps_num, fps_den = parse_fps(a.fps)

    tmp_gray = tmp_pcm = None
    if a.video:
        tmp_gray, tmp_pcm = a.out + ".gray8.tmp", a.out + ".s16.tmp"
        w, h, x0, y0 = run_ffmpeg(a.video, tmp_gray, tmp_pcm, a.fps,
                                  a.audio_rate, a.audio_channels,
                                  a.start, a.duration)
        gray_path, pcm_path = tmp_gray, tmp_pcm
    elif a.gray8:
        if not a.size:
            raise SystemExit("mkvid: --gray8 needs --size WxH")
        w, h = (int(v) for v in a.size.lower().split("x"))
        x0, y0, fw, fh = fit_rect(w, h)
        if (fw, fh) != (w, h):
            # The gray8 is taken as already scaled to its rect; refuse rather
            # than resample, because resampling here would put a second,
            # different scaler inside the gate.
            x0, y0 = (PANEL_W - w) // 2, (PANEL_H - h) // 2
            if w > PANEL_W or h > PANEL_H:
                raise SystemExit("mkvid: gray8 %dx%d does not fit the panel"
                                 % (w, h))
        gray_path, pcm_path = a.gray8, a.pcm
    else:
        raise SystemExit("mkvid: one of --video, --gray8 or --info is required")

    raw = open(gray_path, "rb").read()
    fsz = w * h
    if fsz == 0 or len(raw) % fsz:
        raise SystemExit("mkvid: %s is %d bytes, not a whole number of %dx%d "
                         "frames" % (gray_path, len(raw), w, h))
    frames = [raw[i * fsz:(i + 1) * fsz] for i in range(len(raw) // fsz)]
    if not frames:
        raise SystemExit("mkvid: no frames")

    audio = b""
    if pcm_path and not a.no_audio and os.path.exists(pcm_path):
        audio = open(pcm_path, "rb").read()
        unit = a.audio_channels * 2
        audio = audio[:len(audio) - (len(audio) % unit)]

    planes = dither_frames(frames, w, h, x0, y0, a.dither, a.hysteresis,
                           a.bars == "ink")
    dither_id = 1 if a.dither == "bayer4" else 0

    if a.verify:
        # The C reader decoded a pack; we re-dither the same gray8 here. If the
        # two disagree the pipeline is broken somewhere between the span coder
        # and inflate, and comparing whole frames names which one.
        got = open(a.verify, "rb").read()
        if len(got) != len(planes) * PLANE:
            raise SystemExit("mkvid --verify: %s holds %d bytes, expected %d "
                             "(%d frames x %d)"
                             % (a.verify, len(got), len(planes) * PLANE,
                                len(planes), PLANE))
        bad = [i for i in range(len(planes))
               if got[i * PLANE:(i + 1) * PLANE] != planes[i]]
        if bad:
            i = bad[0]
            g = got[i * PLANE:(i + 1) * PLANE]
            j = next(k for k in range(PLANE) if g[k] != planes[i][k])
            raise SystemExit(
                "mkvid --verify: %d of %d frames differ; first is frame %d "
                "at byte %d (row %d): decoder %02x, dither %02x"
                % (len(bad), len(planes), i, j, j // STRIDE, g[j], planes[i][j]))
        # Anti-vacuity: a decoder that returned frame 0 forever would match a
        # single-frame clip perfectly, so require the frames to be distinct.
        distinct = len(set(planes))
        if distinct < 2:
            raise SystemExit("mkvid --verify: the fixture has only %d distinct "
                             "frame(s); it cannot detect a stuck decoder"
                             % distinct)
        if not a.quiet:
            print("mkvid --verify: %d frames match, %d distinct"
                  % (len(planes), distinct))
        return
    blob, entries = build(planes, fps_num, fps_den, a.gop, audio,
                          a.audio_rate, a.audio_channels,
                          (x0, y0, w, h), a.hysteresis, dither_id,
                          not a.no_deflate)

    with open(a.out, "wb") as f:
        f.write(blob)
    for t in (tmp_gray, tmp_pcm):
        if t and os.path.exists(t):
            os.remove(t)

    if not a.quiet:
        pay = sum(len(e[4]) for e in entries)
        print("mkvid: %d frames %dx%d at %d,%d  fps %d/%d  gop %d  %s h=%d"
              % (len(frames), w, h, x0, y0, fps_num, fps_den, a.gop,
                 a.dither, a.hysteresis))
        print("mkvid: %d bytes payload, %.0f B/frame mean, %d bytes total"
              % (pay, pay / len(frames), len(blob)))
        print("mkvid: sha256 %s" % hashlib.sha256(blob).hexdigest())


if __name__ == "__main__":
    main()

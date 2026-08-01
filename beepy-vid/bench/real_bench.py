#!/usr/bin/env python3
"""Re-measure beepy-vid DESIGN.md 3.7 on REAL footage, replacing the synthetic
upper bound of 3.7.1.  Same encoders, same dither, real photographic detail."""
import subprocess, sys, zlib, os
import numpy as np

W, H = 400, 225
STRIDE = W // 8
RAWB = STRIDE * H
TMP = "/private/tmp/claude-502/-Users-suwat-sai-Workspace-beepy/bcab9ac4-b8b6-4a8f-b8d8-1e708f07458c/scratchpad"


def bayer(n):
    m = np.array([[0]], dtype=np.int64)
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    return m


B8 = bayer(8) * 4 + 2
B4 = bayer(4) * 16 + 8


def extract(src, ss, dur, rate, out):
    """The spec's own invocation: area prefilter, no swscale dither, bitexact."""
    if os.path.exists(out) and os.path.getsize(out) > 0:
        return out
    cmd = ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error",
           "-fflags", "+bitexact", "-flags", "+bitexact",
           "-ss", str(ss), "-i", src, "-t", str(dur),
           "-map", "0:v",
           "-vf", f"scale={W}:{H}:flags=area:sws_dither=none,format=gray",
           "-r", rate, "-pix_fmt", "gray", "-f", "rawvideo", out]
    subprocess.run(cmd, check=True)
    return out


def load(path):
    b = np.fromfile(path, dtype=np.uint8)
    n = len(b) // (W * H)
    return b[: n * W * H].reshape(n, H, W)


def dither(frames, mat, hyst=0):
    n = mat.shape[0]
    T = np.tile(mat, (H // n + 1, W // n + 1))[:H, :W]
    planes, prev = [], None
    for f in frames:
        thr = T + np.where(prev, hyst, -hyst) if (hyst and prev is not None) else T
        ink = f.astype(np.int64) < thr
        prev = ink
        planes.append(np.packbits(ink, axis=1).tobytes())
    return planes


def spans(cur, prv):
    a = np.frombuffer(cur, np.uint8).reshape(H, STRIDE)
    b = np.frombuffer(prv, np.uint8).reshape(H, STRIDE)
    x = a ^ b
    out = bytearray(b"\0\0")
    nrow = 0
    for y in np.nonzero(x.any(axis=1))[0]:
        idx = np.nonzero(x[y])[0]
        start = prev = idx[0]
        for i in list(idx[1:]) + [None]:
            if i is None or i - prev > 3:
                n = prev - start + 1
                out += bytes((y, start, n)) + x[y, start:start + n].tobytes()
                nrow += 1
                if i is not None:
                    start = i
            if i is not None:
                prev = i
    out[0:2] = nrow.to_bytes(2, "little")
    return bytes(out)


def encode(planes):
    tot = {"RAW": 0, "DEFLATE": 0, "XOR_SPANS": 0, "XOR_DEFLATE": 0, "best": 0}
    rows, picks = [], {}
    for i, p in enumerate(planes):
        c = {"RAW": RAWB, "DEFLATE": len(zlib.compress(p, 9))}
        if i:
            q = planes[i - 1]
            a = np.frombuffer(p, np.uint8).reshape(H, STRIDE)
            b = np.frombuffer(q, np.uint8).reshape(H, STRIDE)
            d = np.nonzero((a ^ b).any(axis=1))[0]
            rows.append(0 if not len(d) else int(d[-1]) - int(d[0]) + 1)
            c["RAW"] = (rows[-1] * STRIDE) or 12
            c["XOR_SPANS"] = len(spans(p, q))
            c["XOR_DEFLATE"] = len(zlib.compress(
                bytes(np.frombuffer(p, np.uint8) ^ np.frombuffer(q, np.uint8)), 9))
        else:
            rows.append(H)
        for k, v in c.items():
            tot[k] += v
        best = min(c.items(), key=lambda kv: (kv[1], kv[0]))
        tot["best"] += best[1]
        picks[best[0]] = picks.get(best[0], 0) + 1
    n = len(planes)
    return {k: v / n for k, v in tot.items()}, float(np.mean(rows)), picks


def churn(planes):
    f = [np.unpackbits(np.frombuffer(planes[i], np.uint8)
                       ^ np.frombuffer(planes[i - 1], np.uint8)).sum() / (W * H)
         for i in range(1, len(planes))]
    return float(np.mean(f))


HOME = os.path.expanduser("~")
CLIPS = [
    ("animation", f"{HOME}/Movies/4K Video Downloader+/Our Floating Dreams   A Mickey Mouse Cartoon   Disney Shorts.mp4", 70, "24000/1001"),
    ("live-action", f"{HOME}/Movies/4K Video Downloader+/Rue Doo Kan(Warin).mp4", 60, "30"),
    ("live-action-2", f"{HOME}/Movies/4K Video Downloader+/Rue Doo Kan(Warin).mp4", 165, "30"),
]
DUR = 8

sets = {}
for name, src, ss, rate in CLIPS:
    g = extract(src, ss, DUR, rate, f"{TMP}/{name}.gray8")
    fr = load(g)
    sets[name] = fr
    print(f"{name:<14} {len(fr)} frames @ {rate}  from t={ss}s", file=sys.stderr)

print(f"\nREAL FOOTAGE — {W}x{H}, raw = {RAWB} B/frame\n")
for mname, mat, hy in (("Bayer4 h=0", B4, 0), ("Bayer4 h=8", B4, 8),
                       ("Bayer4 h=16", B4, 16), ("Bayer8 h=8", B8, 8)):
    print(f"=== {mname} ===")
    print(f"{'clip':<15}{'RAW':>7}{'DEFL':>7}{'SPANS':>8}{'XORDEF':>8}"
          f"{'best':>7}{'save':>7}{'churn':>7}{'rows':>6}   MB/min@24")
    for cname, fr in sets.items():
        pl = dither(fr, mat, hy)
        avg, rows, picks = encode(pl)
        save = 100 * (1 - avg["best"] / RAWB)
        mb = avg["best"] * 24 * 60 / 1e6
        print(f"{cname:<15}{avg['RAW']:>7.0f}{avg['DEFLATE']:>7.0f}"
              f"{avg['XOR_SPANS']:>8.0f}{avg['XOR_DEFLATE']:>8.0f}"
              f"{avg['best']:>7.0f}{save:>6.0f}%{churn(pl):>6.1%}{rows:>6.0f}"
              f"{mb:>10.1f}")
    print()

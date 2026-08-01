#!/usr/bin/env python3
"""Retire beepy-vid/DESIGN.md 3.7's modelled compression table with measurements.

Synthetic 400x225 gray8 clips standing in for three archetypes, dithered with
screen-anchored ordered matrices, then encoded four ways.  Everything integer;
no ffmpeg, no PIL in the measured path.
"""
import zlib
import numpy as np

W, H, FPS = 400, 225, 24
STRIDE = W // 8
RAWB = STRIDE * H          # 11250


def bayer(n):
    m = np.array([[0]], dtype=np.int64)
    while m.shape[0] < n:
        m = np.block([[4 * m, 4 * m + 2], [4 * m + 3, 4 * m + 1]])
    return m


B8 = bayer(8) * 4 + 2          # 0..63 -> 2..254
B4 = bayer(4) * 16 + 8         # 0..15 -> 8..248


def scene_base(seed):
    rng = np.random.default_rng(seed)
    y, x = np.mgrid[0:H, 0:W * 2]
    g = (120 + 60 * np.sin(x / 47.0) + 40 * np.cos(y / 31.0)).astype(np.float64)
    g += rng.normal(0, 6, g.shape)              # texture
    return np.clip(g, 0, 255)


def clip_locked(n):
    """Static frame, a subject occupying ~25% moving inside it, plus sensor noise."""
    base = scene_base(1)[:, :W]
    rng = np.random.default_rng(2)
    out = []
    for k in range(n):
        f = base.copy()
        cx = 200 + int(30 * np.sin(k / 8.0))
        f[60:200, cx - 70:cx + 70] = np.clip(
            170 + 50 * np.sin((np.arange(140) + k * 3) / 9.0)[None, :], 0, 255)
        f += rng.normal(0, 1.5, f.shape)        # +-2 level sensor noise
        out.append(np.clip(f, 0, 255).astype(np.uint8))
    return out


def clip_pan(n):
    base = scene_base(3)
    rng = np.random.default_rng(4)
    out = []
    for k in range(n):
        f = base[:, k:k + W].astype(np.float64) + rng.normal(0, 1.5, (H, W))
        out.append(np.clip(f, 0, 255).astype(np.uint8))
    return out


def clip_cut(n):
    out, per = [], 24
    for c in range((n + per - 1) // per):
        base = scene_base(10 + c)
        for k in range(min(per, n - c * per)):
            f = base[:, k * 2:k * 2 + W]
            out.append(np.clip(f, 0, 255).astype(np.uint8))
    return out


def dither(frames, mat, hyst=0):
    """Screen-anchored ordered dither.  Returns packed 1bpp planes, ink=1=black."""
    n = mat.shape[0]
    T = np.tile(mat, (H // n + 1, W // n + 1))[:H, :W]
    planes, prev = [], None
    for f in frames:
        if hyst and prev is not None:
            thr = T + np.where(prev, hyst, -hyst)
        else:
            thr = T
        ink = f.astype(np.int64) < thr           # dark -> ink
        prev = ink
        planes.append(np.packbits(ink, axis=1).tobytes())
    return planes


def spans(cur, prv):
    """mode 2: per-row byte spans of the XOR, split when the gap exceeds 3."""
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


def dirty_rows(cur, prv):
    a = np.frombuffer(cur, np.uint8).reshape(H, STRIDE)
    b = np.frombuffer(prv, np.uint8).reshape(H, STRIDE)
    d = np.nonzero((a ^ b).any(axis=1))[0]
    return (int(d[0]), int(d[-1])) if len(d) else (255, 0)


def encode(planes):
    tot = {"RAW": 0, "DEFLATE": 0, "XOR_SPANS": 0, "XOR_DEFLATE": 0, "best": 0}
    rows, picks = [], {}
    for i, p in enumerate(planes):
        cands = {"RAW": RAWB, "DEFLATE": len(zlib.compress(p, 9))}
        if i == 0:
            rows.append(H)
        else:
            q = planes[i - 1]
            y0, y1 = dirty_rows(p, q)
            rows.append(0 if y1 < y0 else y1 - y0 + 1)
            band = (y1 - y0 + 1) * STRIDE if y1 >= y0 else 0
            cands["RAW"] = band or 12
            x = bytes(np.frombuffer(p, np.uint8) ^ np.frombuffer(q, np.uint8))
            cands["XOR_SPANS"] = len(spans(p, q))
            cands["XOR_DEFLATE"] = len(zlib.compress(x, 9))
        for k, v in cands.items():
            tot[k] += v
        best = min(cands.items(), key=lambda kv: (kv[1], kv[0]))
        tot["best"] += best[1]
        picks[best[0]] = picks.get(best[0], 0) + 1
    n = len(planes)
    return {k: v / n for k, v in tot.items()}, rows, picks


def churn(planes):
    """mean fraction of bits that flip frame to frame -- temporal stability."""
    f = []
    for i in range(1, len(planes)):
        x = np.frombuffer(planes[i], np.uint8) ^ np.frombuffer(planes[i - 1], np.uint8)
        f.append(np.unpackbits(x).sum() / (W * H))
    return float(np.mean(f))


N = 72
clips = {"locked-off": clip_locked(N), "slow pan": clip_pan(N), "cut-heavy": clip_cut(N)}

print(f"{N} frames, {W}x{H}, raw = {RAWB} B/frame, {FPS} fps\n")
for mname, mat, hy in (("Bayer8 h=0", B8, 0), ("Bayer8 h=8", B8, 8),
                       ("Bayer4 h=0", B4, 0), ("Bayer4 h=8", B4, 8)):
    print(f"=== {mname} ===")
    print(f"{'clip':<12}{'RAW':>8}{'DEFL':>8}{'SPANS':>9}{'XORDEF':>9}"
          f"{'best':>8}{'save':>7}{'churn':>8}{'rows':>7}")
    for cname, frames in clips.items():
        pl = dither(frames, mat, hy)
        avg, rows, picks = encode(pl)
        save = 100 * (1 - avg["best"] / RAWB)
        print(f"{cname:<12}{avg['RAW']:>8.0f}{avg['DEFLATE']:>8.0f}"
              f"{avg['XOR_SPANS']:>9.0f}{avg['XOR_DEFLATE']:>9.0f}"
              f"{avg['best']:>8.0f}{save:>6.0f}%{churn(pl):>7.1%}"
              f"{np.mean(rows):>7.0f}")
    print()

# the temporal-stability claim that rules out error diffusion
print("=== error diffusion, for the temporal-stability claim ===")


def fs(frames):
    out = []
    for f in frames:
        g = f.astype(np.float64).copy()
        for y in range(H):
            for x in range(W):
                old = g[y, x]
                new = 0.0 if old < 128 else 255.0
                e = old - new
                g[y, x] = new
                if x + 1 < W:
                    g[y, x + 1] += e * 7 / 16
                if y + 1 < H:
                    if x:
                        g[y + 1, x - 1] += e * 3 / 16
                    g[y + 1, x] += e * 5 / 16
                    if x + 1 < W:
                        g[y + 1, x + 1] += e * 1 / 16
        out.append(np.packbits(g < 128, axis=1).tobytes())
    return out


sub = clips["locked-off"][:8]
print(f"locked-off, Floyd-Steinberg churn: {churn(fs(sub)):.1%}")
print(f"locked-off, Bayer8 h=0     churn: {churn(dither(sub, B8, 0)):.1%}")
print(f"locked-off, Bayer8 h=8     churn: {churn(dither(sub, B8, 8)):.1%}")

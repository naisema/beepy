#!/usr/bin/env python3
"""
fbdiff.py -- compare two 400x240 1-bit frames, in any of the formats this
repo produces: 384000-byte raw XRGB dumps (fb_dump / png2fb), 12000-byte
packed 1bpp canvases, or 400x240 PNGs.

    fbdiff.py A B [--mask panel] [--mask x0,y0,x1,y1] [--mask-list F]
                  [--max-px N] [--edges-only] [--out diff.png]

--mask panel        ignore x < 130 (the NAV turn panel + divider)
--mask x0,y0,x1,y1  ignore a rectangle (inclusive); repeatable
--mask-list FILE    ignore the individual pixels listed in FILE, one "x y" per
                    line. Counted and reported separately from --mask, as
                    "at-threshold": the intended use is the set of pixels the
                    reference renderer itself resolved at EXACTLY 50 % coverage,
                    where its own >= comparison broke the tie and a hair of
                    geometry either way flips the answer. Such a pixel is not
                    evidence of a displaced shape, which is what --edges-only
                    is there to catch.
--max-px N          pass if unmasked differing pixels <= N (default 0)
--edges-only        additionally require every differing pixel to sit on an
                    ink/paper edge in BOTH frames (an opposite-colour
                    4-neighbour) -- shape-boundary drift, not missing shapes
--out diff.png      3x visual: agreeing ink pale grey, A-only diffs solid
                    black 3x3, B-only diffs a black 3x3 outline

Exit status 0/1.
"""
import sys

W, H = 400, 240


def load(path):
    data = open(path, "rb").read() if not path.endswith(".png") else None
    if data is not None and len(data) == W * H * 4:          # XRGB dump
        return [data[4 * (y * W + x)] == 0 for y in range(H) for x in range(W)]
    if data is not None and len(data) == W * H // 8:         # packed 1bpp
        return [bool(data[(y * W + x) // 8] & (0x80 >> (x % 8)))
                for y in range(H) for x in range(W)]
    from PIL import Image
    img = Image.open(path).convert("L")
    if img.size != (W, H):
        raise SystemExit(f"{path}: expected {W}x{H}, got {img.size}")
    px = img.load()
    return [px[x, y] < 128 for y in range(H) for x in range(W)]


def on_edge(f, x, y):
    v = f[y * W + x]
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < W and 0 <= ny < H and f[ny * W + nx] != v:
            return True
    return False


def main(argv):
    args, masks, max_px, edges, out = [], [], 0, False, None
    at_thresh = set()
    it = iter(argv)
    for a in it:
        if a == "--mask-list":
            for ln in open(next(it)):
                ln = ln.split("#")[0].split()
                if ln:
                    at_thresh.add((int(ln[0]), int(ln[1])))
        elif a == "--mask":
            m = next(it)
            masks.append((0, 0, 129, H - 1) if m == "panel"
                         else tuple(int(v) for v in m.split(",")))
        elif a == "--max-px":
            max_px = int(next(it))
        elif a == "--edges-only":
            edges = True
        elif a == "--out":
            out = next(it)
        else:
            args.append(a)
    if len(args) != 2:
        raise SystemExit(__doc__.strip())
    fa, fb = load(args[0]), load(args[1])

    def masked(x, y):
        return any(x0 <= x <= x1 and y0 <= y <= y1 for x0, y0, x1, y1 in masks)

    diffs, hidden, tied, nonedge = [], 0, 0, 0
    for y in range(H):
        for x in range(W):
            if fa[y * W + x] == fb[y * W + x]:
                continue
            if masked(x, y):
                hidden += 1
                continue
            if (x, y) in at_thresh:
                tied += 1
                continue
            diffs.append((x, y))
            if edges and not (on_edge(fa, x, y) and on_edge(fb, x, y)):
                nonedge += 1

    if out:
        from PIL import Image, ImageDraw
        img = Image.new("L", (W * 3, H * 3), 255)
        d = ImageDraw.Draw(img)
        for y in range(H):
            for x in range(W):
                if fa[y * W + x] and fa[y * W + x] == fb[y * W + x]:
                    d.rectangle([x * 3, y * 3, x * 3 + 2, y * 3 + 2], fill=200)
        for x, y in diffs:
            if fa[y * W + x]:                                # A-only: solid
                d.rectangle([x * 3, y * 3, x * 3 + 2, y * 3 + 2], fill=0)
            else:                                            # B-only: outline
                d.rectangle([x * 3, y * 3, x * 3 + 2, y * 3 + 2], outline=0)
        img.save(out)

    ok = len(diffs) <= max_px and (not edges or nonedge == 0)
    print(f"fbdiff: {len(diffs)} differing px (limit {max_px})"
          + (f", {hidden} more inside masks" if masks else "")
          + (f", {tied} at-threshold" if at_thresh else "")
          + (f", {nonedge} fail edges-only" if edges else "")
          + f" -> {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

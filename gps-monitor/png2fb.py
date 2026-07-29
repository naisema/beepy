#!/usr/bin/env python3
"""
Convert a 400x240 1-bit PNG into a raw framebuffer frame for the Beepy panel.

Output format is taken from the device's own known-good
root_overlay/opt/shutdownimage.fb, which contains exactly two pixel words:

    black = 00 00 00 ff        white = ff ff ff ff

i.e. XRGB8888 little-endian (B,G,R,X) with the unused X byte set to 0xff.
384000 bytes total = 400 * 240 * 4, matching /dev/fb1's stride of 1600.

Usage: python3 png2fb.py fb-combined.png [fb-combined.fb]
"""

import sys
from PIL import Image

W, H = 400, 240
BLACK = b"\x00\x00\x00\xff"
WHITE = b"\xff\xff\xff\xff"


def convert(src, dst):
    img = Image.open(src).convert("1")
    if img.size != (W, H):
        raise SystemExit(f"{src}: expected {W}x{H}, got {img.size[0]}x{img.size[1]}")

    px = img.load()
    out = bytearray()
    for y in range(H):
        for x in range(W):
            out += WHITE if px[x, y] else BLACK

    assert len(out) == W * H * 4 == 384000, len(out)
    with open(dst, "wb") as f:
        f.write(out)
    black = sum(1 for i in range(0, len(out), 4) if out[i] == 0)
    print(f"{dst}: {len(out)} bytes, {black} black px, {W*H-black} white px")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__.strip())
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit(".", 1)[0] + ".fb"
    convert(src, dst)

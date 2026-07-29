#!/usr/bin/env python3
"""
Design mockups for the graphical (framebuffer) GPS monitor on the Beepy.

Rendered at the panel's exact native geometry -- 400x240 -- in 1-bit mode, which
is what /dev/fb1 ultimately drives: a 32bpp XRGB fbdev in front of a 1-bit Sharp
memory LCD. Drawing in mode "1" disables anti-aliasing, so glyphs and lines come
out as crisp on/off pixels like the bitmap font the C program will embed. Nothing
relies on grey: the only halftone is explicit checkerboard dither, which a 1-bit
panel can genuinely display.

Framebuffer pixels are square (400x240 buffer on a 400x240 panel), so circles
here are actually circular -- no aspect correction, unlike the 50x15 text mode.

Two views, each using the whole screen, and two font scales so the readable one
can be picked by looking at the actual panel. Layout is derived from font metrics
rather than hardcoded, so changing a scale re-fits everything.

Runs on macOS or on the Beepy itself (Python 3.9 + PIL 8.1 are installed there);
the font is picked from whatever is available.

Outputs, each also as -3x for viewing:
    fb-bars.png     fb-sky.png       large  (status 14 / text 12)
    fb-bars-xl.png  fb-sky-xl.png    extra  (status 17 / text 15)
"""

import math
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 240
BLACK, WHITE = 0, 1          # mode "1": 0 = black ink, 1 = white paper


def _font(size):
    """First available monospace font. Menlo on macOS, DejaVu on the Beepy."""
    for path in ("/System/Library/Fonts/Menlo.ttc",
                 "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
                 "/System/Library/Fonts/Supplemental/Courier New.ttf"):
        try:
            return ImageFont.truetype(path, size)
        except OSError:
            continue
    return ImageFont.load_default()


# ------------------------------------------------------------------ font scale
class Scale:
    """A font scale plus every layout metric derived from it."""

    def __init__(self, suffix, status, info, small, tiny):
        self.suffix = suffix
        self.status = _font(status)
        self.info = _font(info)
        self.small = _font(small)
        self.tiny = _font(tiny)
        # Line heights: glyph box plus leading.
        self.lh_small = self.small.getbbox("Ag")[3] + 3
        self.lh_tiny = self.tiny.getbbox("Ag")[3] + 3
        self.status_h = self.status.getbbox("Ag")[3] + 4
        self.marker_r = max(4, small // 2 - 1)


# XL everywhere. Bar labels stack ("02" over "G") because at this size a
# horizontal "G02" does not fit a 26 px slot -- see draw_bars().
SCALES = [Scale("", 17, 15, 15, 15)]

# ---------------------------------------------------------------- sample data
# (constellation, prn, elevation, azimuth, snr dBHz, used in fix)
SATS = [
    ("G", 2, 67, 210, 44, True),
    ("G", 5, 54, 118, 41, True),
    ("G", 7, 41, 302, 38, True),
    ("G", 13, 33, 95, 35, True),
    ("G", 21, 28, 201, 33, True),
    ("G", 19, 36, 240, 31, True),
    ("G", 24, 22, 155, 29, False),
    ("R", 66, 61, 340, 28, True),
    ("R", 73, 44, 55, 26, True),
    ("R", 75, 19, 28, 22, True),
    ("G", 30, 12, 266, 19, False),
    ("R", 81, 15, 70, 15, False),
    ("R", 84, 9, 300, 11, False),
    ("G", 11, 8, 190, 8, False),
]
SEL = 0

FIX = dict(mode="3D", used=9, view=14, hdop=0.9, pdop=1.4, vdop=1.1,
           lat="13.75632N", lon="100.50184E", alt="18M", spd="0.3", crs="142",
           utc="12:34:56Z", port="TTYACM0", age="0S", crc=0)

SNR_MIN, SNR_MAX = 10, 50


# ------------------------------------------------------------------ primitives
def text(d, xy, s, font, fill=BLACK):
    d.text(xy, s, font=font, fill=fill)


def ctext(d, cx, y, s, font, fill=BLACK):
    d.text((cx - d.textlength(s, font=font) / 2, y), s, font=font, fill=fill)


def checker(d, box, phase=0):
    """50% ordered dither -- the only halftone a 1-bit panel can show."""
    x0, y0, x1, y1 = box
    for y in range(y0, y1):
        for x in range(x0, x1):
            if (x + y + phase) % 2 == 0:
                d.point((x, y), fill=BLACK)


def ring(d, cx, cy, r, fill=None, outline=BLACK):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=fill, outline=outline)


def dotted_circle(d, cx, cy, r, step=9):
    n = max(12, int(2 * math.pi * r / step))
    for i in range(n):
        a = 2 * math.pi * i / n
        d.point((round(cx + r * math.sin(a)), round(cy - r * math.cos(a))), fill=BLACK)


class Occ:
    """Coarse occupancy grid driving sky label placement."""

    CELL = 4

    def __init__(self):
        self.taken = set()

    def _cells(self, x0, y0, w, h):
        for y in range(int(y0), int(y0 + h), self.CELL):
            for x in range(int(x0), int(x0 + w), self.CELL):
                yield (x // self.CELL, y // self.CELL)

    def seed(self, x0, y0, w, h):
        self.taken.update(self._cells(x0, y0, w, h))

    def claim(self, x0, y0, w, h, bounds):
        bx0, by0, bx1, by1 = bounds
        if x0 < bx0 or y0 < by0 or x0 + w > bx1 or y0 + h > by1:
            return False
        cells = set(self._cells(x0, y0, w, h))
        if cells & self.taken:
            return False
        self.taken |= cells
        return True


def constellation_rows():
    names = {"G": "GPS", "R": "GLO", "E": "GAL", "B": "BDS"}
    out = []
    for code in ("G", "R", "E", "B"):
        grp = [s for s in SATS if s[0] == code]
        if grp:
            out.append(f"{names[code]} {sum(1 for s in grp if s[5])}/{len(grp)}")
    return out


def bar_h(snr, span):
    if snr <= SNR_MIN:
        return 0
    return max(2, round((min(snr, SNR_MAX) - SNR_MIN) / (SNR_MAX - SNR_MIN) * span))


# ---------------------------------------------------------------- bar renderer
def draw_bars(d, sc, x0, y0, x1, y1):
    """
    Full-width vertical SNR bargraph with the SNR value above each bar.

    The PRN label mode is chosen by measuring the font against the slot width,
    so a larger font degrades gracefully instead of overlapping:

        "G02" on one line  ->  "02" over "G" stacked  ->  "02" alone

    Stacking is preferred over dropping to a bare number because it keeps the
    constellation letter at full size; the bare number is the last resort (still
    unambiguous, since GPS PRNs are 1-32 and GLONASS 65-96, but harder to read
    at a glance).
    """
    n = len(SATS)
    ax_x = x0 + int(d.textlength("50", font=sc.small)) + 6
    plotw = x1 - (ax_x + 6)
    slot = min(34, plotw // n)
    bw = max(3, slot - 6)

    w3 = d.textlength("G02", font=sc.tiny)
    w2 = d.textlength("02", font=sc.tiny)
    if w3 <= slot - 1:
        mode = "full"
    elif w2 <= slot - 1:
        mode = "stack"          # keeps the constellation letter, costs a row
    else:
        mode = "num"

    label_h = sc.lh_tiny * (2 if mode == "stack" else 1)
    base_y = y1 - label_h
    top_y = y0 + sc.lh_tiny + 2          # room for the SNR value above a full bar
    span = base_y - top_y

    for db in (50, 40, 30, 20, 10):
        y = base_y - round((db - SNR_MIN) / (SNR_MAX - SNR_MIN) * span)
        text(d, (x0, y - sc.lh_small / 2), f"{db}", sc.small)
        for x in range(ax_x + 2, x1, 6):
            d.point((x, y), fill=BLACK)
    d.line([ax_x, top_y, ax_x, base_y], fill=BLACK)
    d.line([ax_x, base_y, x1 - 1, base_y], fill=BLACK)

    for i, (sysc, prn, el, az, snr, used) in enumerate(SATS):
        bx = ax_x + 6 + i * slot
        if bx + bw > x1:
            break
        cx = bx + bw // 2
        h = bar_h(snr, span)

        if h == 0:
            # At or below the noise floor: no bar, but the satellite keeps its
            # slot, its label and a baseline stub so you can see it is tracked.
            d.line([bx, base_y - 1, bx + bw, base_y - 1], fill=BLACK)
        elif used:
            d.rectangle([bx, base_y - h, bx + bw, base_y - 1], fill=BLACK)
        else:
            d.rectangle([bx, base_y - h, bx + bw, base_y - 1],
                        fill=WHITE, outline=BLACK)
            checker(d, (bx + 1, base_y - h + 1, bx + bw, base_y - 1))

        if snr > 0:
            ctext(d, cx + 1, max(y0, base_y - h - sc.lh_tiny), f"{snr}", sc.tiny)
        if i == SEL:
            d.rectangle([bx - 3, base_y - h - 2, bx + bw + 3, base_y + 1],
                        fill=None, outline=BLACK)

        if mode == "full":
            ctext(d, cx + 1, base_y + 2, f"{sysc}{prn:02d}", sc.tiny)
        elif mode == "num":
            ctext(d, cx + 1, base_y + 2, f"{prn:02d}", sc.tiny)
        else:
            ctext(d, cx + 1, base_y + 2, f"{prn:02d}", sc.tiny)
            ctext(d, cx + 1, base_y + 2 + sc.lh_tiny, sysc, sc.tiny)
    return mode


# ---------------------------------------------------------------- sky renderer
def sky_xy(cx, cy, r, el, az):
    k = (1.0 - el / 90.0) * r
    a = math.radians(az)
    return round(cx + k * math.sin(a)), round(cy - k * math.cos(a))


def draw_sat_marker(d, x, y, used, snr, selected, r):
    if snr <= 0:
        d.rectangle([x - 1, y - 1, x + 1, y + 1], fill=BLACK)   # in view, no signal
    elif used:
        ring(d, x, y, r, fill=BLACK)                             # used in fix
    else:
        ring(d, x, y, r, fill=WHITE, outline=BLACK)              # tracked, unused
    if selected:
        ring(d, x, y, r + 3, fill=None, outline=BLACK)


def draw_sky(d, sc, x0, y0, x1, y1):
    pad = int(d.textlength("N", font=sc.small)) + 6
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    r = min(x1 - x0, y1 - y0) // 2 - pad

    dotted_circle(d, cx, cy, r)                  # horizon, el 0
    dotted_circle(d, cx, cy, r * 2 // 3)         # el 30
    dotted_circle(d, cx, cy, r // 3)             # el 60
    d.line([cx - 4, cy, cx + 4, cy], fill=BLACK)     # zenith
    d.line([cx, cy - 4, cx, cy + 4], fill=BLACK)

    occ = Occ()
    bounds = (x0, y0, x1, y1)
    ch = sc.lh_small
    cw = d.textlength("N", font=sc.small)
    for lbl, lx, ly in (("N", cx - cw / 2, cy - r - ch), ("S", cx - cw / 2, cy + r + 2),
                        ("W", cx - r - cw - 4, cy - ch / 2),
                        ("E", cx + r + 4, cy - ch / 2)):
        text(d, (lx, ly), lbl, sc.small)
        occ.seed(lx, ly, cw, ch)

    pos = []
    for (sysc, prn, el, az, snr, used) in SATS:
        x, y = sky_xy(cx, cy, r, el, az)
        pos.append((x, y))
        occ.seed(x - sc.marker_r - 1, y - sc.marker_r - 1,
                 2 * sc.marker_r + 3, 2 * sc.marker_r + 3)

    # Selected first, then strongest first: when space runs out it is the
    # weakest satellites that lose their labels.
    order = [SEL] + sorted((i for i in range(len(SATS)) if i != SEL),
                           key=lambda i: -SATS[i][4])
    hidden = 0
    for i in order:
        sysc, prn, el, az, snr, used = SATS[i]
        x, y = pos[i]
        draw_sat_marker(d, x, y, used, snr, i == SEL, sc.marker_r)
        gap = sc.marker_r + 2
        # Prefer the full "G02" tag; fall back to the bare PRN, which is still
        # unambiguous because GPS and GLONASS PRN ranges do not overlap.
        for tag in (f"{sysc}{prn:02d}", f"{prn:02d}"):
            tw = d.textlength(tag, font=sc.small)
            for lx, ly in ((x + gap, y - ch / 2), (x - tw - gap, y - ch / 2),
                           (x - tw / 2, y - gap - ch), (x - tw / 2, y + gap)):
                if occ.claim(lx, ly, tw, ch, bounds):
                    text(d, (lx, ly), tag, sc.small)
                    break
            else:
                continue
            break
        else:
            hidden += 1
    return hidden


# ------------------------------------------------------------------ chrome
def status_bar(d, sc, page):
    d.rectangle([0, 0, W - 1, sc.status_h], fill=BLACK)
    text(d, (3, 1), f"{page} {FIX['mode']} {FIX['used']}/{FIX['view']}SAT "
                    f"HDOP{FIX['hdop']}", sc.status, fill=WHITE)
    tw = d.textlength(FIX["utc"], font=sc.status)
    text(d, (W - tw - 3, 1), FIX["utc"], sc.status, fill=WHITE)


def finish(img, name):
    img.save(f"{name}.png")
    (img.convert("L").resize((W * 3, H * 3), Image.NEAREST)
        .convert("1").save(f"{name}-3x.png"))
    print(f"wrote {name}.png / {name}-3x.png")


# ------------------------------------------------------------------- pages
def page_bars(sc):
    img = Image.new("1", (W, H), WHITE)
    d = ImageDraw.Draw(img)
    status_bar(d, sc, "BARS")

    foot_h = 2 * sc.lh_small
    y1 = H - foot_h - 3
    mode = draw_bars(d, sc, 0, sc.status_h + 3, W - 2, y1)
    d.line([0, y1 + 1, W - 1, y1 + 1], fill=BLACK)

    sysc, prn, el, az, snr, used = SATS[SEL]
    text(d, (3, y1 + 4),
         f"{FIX['lat']} {FIX['lon']} ALT{FIX['alt']}", sc.small)
    text(d, (3, y1 + 4 + sc.lh_small),
         f"SEL {sysc}{prn:02d} EL{el} AZ{az}  AGE{FIX['age']} CRC{FIX['crc']}"
         f"  2=SKY", sc.small)
    finish(img, f"fb-bars{sc.suffix}")
    return mode


def page_sky(sc):
    img = Image.new("1", (W, H), WHITE)
    d = ImageDraw.Draw(img)
    status_bar(d, sc, "SKY ")

    # Stats column is sized from the font; the sky is height-limited anyway, so
    # widening the column costs no radius.
    colw = int(d.textlength("DOP P1.4 H0.9", font=sc.small)) + 6
    skx1 = W - colw - 4
    hidden = draw_sky(d, sc, 0, sc.status_h + 3, skx1, H - 1)
    d.line([skx1 + 2, sc.status_h + 3, skx1 + 2, H - 1], fill=BLACK)

    sysc, prn, el, az, snr, used = SATS[SEL]
    # Rows are tagged: blank spacers are decoration and get dropped first when
    # the column is short, so content is never silently cut instead.
    rows = [("SELECTED", 0), (f" {sysc}{prn:02d} {snr}DB", 0),
            (f" EL{el} AZ{az}", 0), (" IN FIX" if used else " UNUSED", 0),
            ("", 1),
            (f"DOP P{FIX['pdop']} H{FIX['hdop']}", 0), (f"    V{FIX['vdop']}", 0),
            ("", 1)]
    rows += [(r, 0) for r in constellation_rows()]
    rows += [("", 1),
             (f"AGE {FIX['age']}" + (f" +{hidden}HID" if hidden else ""), 0),
             ("1=BARS N/P", 0)]

    x = skx1 + 6
    y0 = sc.status_h + 5
    avail = (H - y0) // sc.lh_small
    while len(rows) > avail and any(sp for _, sp in rows):
        for i in range(len(rows) - 1, -1, -1):
            if rows[i][1]:
                del rows[i]
                break

    y = y0
    for s, _ in rows[:avail]:
        text(d, (x, y), s, sc.small)
        y += sc.lh_small
    dropped = max(0, len(rows) - avail)
    finish(img, f"fb-sky{sc.suffix}")
    return hidden, dropped


if __name__ == "__main__":
    for sc in SCALES:
        mode = page_bars(sc)
        hidden, dropped = page_sky(sc)
        print(f"  scale{sc.suffix or ' (large)'}: bar labels={mode}, "
              f"sky labels hidden={hidden}, stats rows dropped={dropped}, "
              f"marker r={sc.marker_r}")

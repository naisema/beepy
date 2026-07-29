#!/usr/bin/env python3
"""
Design mockups for beepy-nav -- a two-page GPS route navigator on the Beepy's
400x240 1-bit Sharp memory LCD.

    NAV       inverted turn panel on the left, live map on the right
    OVERVIEW  the whole route fitted to the screen

Layout follows the reference screenshot in this directory. Measuring that image
settled the rendering question: it holds 239 distinct grey levels, 8.5% of its
pixels sitting between the two plateaus at 22 (ink) and 224 (paper). It is a
two-tone design with **anti-aliased edges** -- the smoothness is entirely in the
edge pixels.

The panel cannot show those greys: sharp-drm thresholds at mono_cutoff (default
32) with no dithering, so a 50%-grey edge pixel snaps to white. But that only
rules out *emitting* grey. Anti-aliasing done here and resolved to a black/white
pattern reaches the panel intact, because the driver's threshold never sees a
grey value.

So everything is drawn into an 8-bit coverage canvas at 4x, box-downsampled to
400x240, and resolved with a plain 50% threshold. Bayer dithering was tried and
rejected on evidence: resolving the reference itself both ways shows the
threshold reproducing it cleanly while the matrix speckles every flat and frays
the numerals -- 4x supersampling has already put the ink in the right pixel, so
there is no residual tone worth diffusing. resolve(dither=True) reproduces the
rejected version; page_smooth() renders the comparison.

Thin features (streets, track dashes, the scale bar) skip the coverage canvas
and are drawn aliased at 1x, because a 1 px diagonal has no room to anti-alias
and resolves to a broken dotted line. In C the 4x supersample is replaced by
analytic coverage -- signed distance for strokes and discs, pre-rendered bitmap
tables for glyphs; see DESIGN.md 5.3.

Outputs (each also -3x for viewing):
    nav-turn        NAV page, with the synthetic stand-in basemap
    nav-turn-osm    NAV page over REAL OSM roads (Asok junction, Bangkok)
    nav-turn-nomap  NAV page as phase 2 ships it: route and track only
    nav-turn-off    NAV page, off route
    nav-overview    OVERVIEW page
    nav-arrows      the nine cue glyphs at three sizes
    nav-smooth      50% threshold vs Bayer dither, side by side
"""

import math
from PIL import Image, ImageDraw, ImageFont

W, H = 400, 240
SS = 4                              # supersample factor
INK, PAPER = 0, 255                 # 8-bit coverage canvas

CELL_W, CELL_H = 6, 8               # 5x7 font cell, must match gps-monitor.c
PANEL_W = 128                       # turn panel: 32% of the width, as in the
MAP_X = PANEL_W + 2                 # reference screenshot

# 8x8 Bayer, kept only so page_smooth() can show why it is *not* used.
BAYER8 = [[0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
          [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
          [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
          [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]]


def resolve(cov, dither=False):
    """
    8-bit coverage at 4x -> the 400x240 1-bit frame the panel receives.

    A plain 50% threshold, and that is a measured choice rather than laziness.
    Downsampling the reference screenshot to 400x240 and resolving it both ways
    (see DESIGN.md 5.3) shows the threshold reproducing it cleanly while Bayer
    speckles every flat and frays the stems of the numerals -- because 4x
    supersampling has already put the ink in the right pixel. There is no
    residual grey worth diffusing, and dithering only re-introduces noise.
    """
    small = cov.resize((W, H), Image.BOX)
    if not dither:
        # C-speed threshold (the animation renderer calls this per frame).
        # dither=Image.NONE matters: a bare convert("1") Floyd-Steinbergs.
        return (small.point(lambda v: 255 if v >= 128 else 0)
                     .convert("1", dither=Image.NONE))
    src, out = small.load(), Image.new("1", (W, H), 1)
    dst = out.load()
    for y in range(H):
        row = BAYER8[y & 7]
        for x in range(W):
            v = src[x, y]
            if v <= 20:
                dst[x, y] = 0
            elif v >= 235:
                dst[x, y] = 1
            else:
                dst[x, y] = 1 if v > 20 + 215 * (row[x & 7] + 0.5) / 64 else 0
    return out


class Canvas:
    """Draws in 1x coordinates onto a 4x 8-bit buffer."""

    def __init__(self):
        self.img = Image.new("L", (W * SS, H * SS), PAPER)
        self.d = ImageDraw.Draw(self.img)
        # Hairlines get their own 1x layer. Anti-aliasing helps big smooth shapes
        # -- the arrow, the numerals, the discs, the thick route -- but a 1-2 px
        # street picks up a fringe on both sides and reads as hairy rather than
        # thin. So thin features are drawn at 1x, aliased, then blown up NEAREST
        # into the 4x canvas: each 1x pixel becomes a solid 4x4 block, which
        # box-downsamples back to exactly itself and survives resolve() untouched.
        self.hair = Image.new("L", (W, H), PAPER)
        self.hd = ImageDraw.Draw(self.hair)

    def hairline(self, a, b, width=1, ink=INK):
        self.hd.line([round(a[0]), round(a[1]), round(b[0]), round(b[1])],
                     fill=ink, width=max(1, int(round(width))))

    def flush_hairlines(self):
        """Paste the 1x layer into the 4x canvas. Call before drawing over it."""
        mask = self.hair.resize((W * SS, H * SS), Image.NEAREST)
        self.img.paste(mask, (0, 0), Image.eval(mask, lambda v: 255 - v))
        self.hair = Image.new("L", (W, H), PAPER)
        self.hd = ImageDraw.Draw(self.hair)

    def _s(self, pts):
        return [(p[0] * SS, p[1] * SS) for p in pts]

    def rect(self, x0, y0, x1, y1, ink=INK):
        self.d.rectangle([x0 * SS, y0 * SS, (x1 + 1) * SS - 1, (y1 + 1) * SS - 1],
                         fill=ink)

    def line(self, a, b, width=1, ink=INK):
        """
        Minimum 2 px. A 1 px diagonal has no room to anti-alias -- coverage
        spreads over two pixels at roughly half each, and neither survives a 50%
        threshold cleanly, so the line breaks up. Anything on this canvas that
        must read as a line gets 2 px; genuinely thin features go through
        hairline() instead, and axis-aligned chrome uses rect(), which is
        pixel-aligned and therefore always exactly solid.
        """
        w = max(2.0, width)
        self.d.line(self._s([a, b]), fill=ink, width=max(1, int(round(w * SS))))

    def disc(self, x, y, r, ink=INK):
        self.d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS],
                       fill=ink)

    def ring(self, x, y, r, width=1, ink=INK, fill=None):
        self.d.ellipse([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS],
                       fill=fill, outline=ink, width=max(1, int(round(width * SS))))

    def poly(self, pts, ink=INK):
        self.d.polygon(self._s(pts), fill=ink)

    def arc(self, x, y, r, a0, a1, width=1, ink=INK):
        self.d.arc([(x - r) * SS, (y - r) * SS, (x + r) * SS, (y + r) * SS],
                   a0, a1, fill=ink, width=max(1, int(round(width * SS))))

    def point(self, x, y, ink=INK):
        self.rect(x, y, x, y, ink)


# --------------------------------------------------------------- 5x7 font
# Read out of gps-monitor.c rather than redrawn here, and drawn as exact SS x SS
# blocks so the box filter reproduces it bit for bit. Small text stays crisp;
# dithering it would only blur glyphs that are already only 5 px wide.
FONT_SRC = "../libbeepyfb/font.c"

EXTRA = {                                   # fallback only; the C table wins
    "%": (0x19, 0x1A, 0x02, 0x04, 0x08, 0x0B, 0x13),
    ">": (0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08),
    "<": (0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02),
    "*": (0x00, 0x0A, 0x04, 0x1F, 0x04, 0x0A, 0x00),
}


def load_font():
    import re
    try:
        src = open(FONT_SRC).read()
    except OSError:
        return dict(EXTRA)
    glyphs = {}
    body = src.split("static const unsigned char FONT")[1].split("};")[0]
    for m in re.finditer(r"\[(\d+)\]\s*=\s*\{([^}]*)\}", body):
        glyphs[chr(32 + int(m.group(1)))] = tuple(
            int(v, 0) for v in m.group(2).split(","))
    # Every glyph transcribed into EXTRA must exist in the C table and match
    # bit for bit, so a transcription typo cannot silently shift the mockups.
    for ch, rows in EXTRA.items():
        assert glyphs.get(ch) == rows, \
            f"EXTRA[{ch!r}] disagrees with {FONT_SRC}: {glyphs.get(ch)} != {rows}"
    return glyphs


FONT = load_font()


def tw(s, scale):
    """Exactly gps-monitor.c's text_w(): pitch 6*scale, trailing gap included."""
    return len(s) * CELL_W * scale


def text(c, x, y, s, scale=1, ink=INK):
    """
    Bitmap glyphs as exact SS x SS blocks. Coordinates are rounded first: a
    fractional x splits every block across two output pixels, each at half
    coverage, and the whole string resolves to mush. That is what turned
    "TO GO" into grey soup in an earlier render.
    """
    x, y = round(x), round(y)
    for i, ch in enumerate(s.upper()):
        rows = FONT.get(ch)
        if not rows:
            continue
        gx = x + i * CELL_W * scale
        for ry, bits in enumerate(rows):
            for bx in range(5):
                if bits & (0x10 >> bx):
                    c.rect(gx + bx * scale, y + ry * scale,
                           gx + bx * scale + scale - 1, y + ry * scale + scale - 1,
                           ink)
    return tw(s, scale)


def rtext(c, x_right, y, s, scale=1, ink=INK):
    return text(c, x_right - tw(s, scale), y, s, scale, ink)


# ------------------------------------------------------- typeface numerals
# The reference sets its distance in a bold grotesque, not a segment display.
# In C these become pre-rendered 1-bit bitmaps generated by this same script --
# 10 digits plus "." at two sizes is under 4 KB of table, against 319 MB free.
FACES = ("/System/Library/Fonts/Supplemental/Arial Bold.ttf",
         "/System/Library/Fonts/Helvetica.ttc",
         "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")
_face_cache = {}

# Once the generated tables exist (tools/gen_tables.py -> src/numerals.h,
# tools/gen_labels.py -> src/labels.h), num() blits them with integer
# advances instead of rasterizing through PIL -- the same tables, the same
# advances and the same set-selection rule as seg.c, so mockup and C
# numerals are identical BY CONSTRUCTION rather than by resemblance. The
# PIL path below survives as the fallback when the headers are absent and
# for strings outside the tables (search queries), and it is still what
# the table generators themselves render with.
TABLE_FILES = ("src/numerals.h", "src/labels.h")


def load_num_tables():
    import re
    sets = {}
    for path in TABLE_FILES:
        try:
            src = open(path).read()
        except OSError:
            continue
        bits = {m.group(1): bytes(int(v, 0) for v in
                                  re.findall(r"0x[0-9a-fA-F]+", m.group(2)))
                for m in re.finditer(
                    r"static const unsigned char (\w+)_bits\[\] = \{([^}]*)\}",
                    src)}
        for m in re.finditer(r"static const glyph_t (\w+)\[\] = \{(.*?)\};",
                             src, re.S):
            tab = {}
            for e in re.finditer(
                    r'\{\s*"([^"]+)",\s*(-?\d+),\s*(-?\d+),\s*(-?\d+),'
                    r'\s*(-?\d+),\s*(-?\d+),\s*(\w+)_bits\s*\}', m.group(2)):
                tab[e.group(1)] = (int(e.group(2)), int(e.group(3)),
                                   int(e.group(4)), int(e.group(5)),
                                   int(e.group(6)), bits[e.group(7)])
            sets[m.group(1)] = tab
    return sets if "NUM54" in sets else None


NUMTAB = load_num_tables()


def _table_layout(s, cap_px):
    """
    -> (ink width, [(glyph, pen offset), ...]) from the generated tables, or
    None when the string needs the PIL fallback. Glyph selection mirrors
    seg.c exactly: whole-string labels first, whole-string unit tags next,
    then per-character digits from the set the cap selects -- NUM54 at
    cap >= 54, NUM22 below. Only two sizes exist (DESIGN.md 5.2), so a mid
    cap is a request for the nearer prepared size, not a new rasterization.
    Glyph tuple: (w, h, dx, dy, adv, rows).
    """
    if NUMTAB is None:
        return None
    for name in ("LABELS", "UNITS22"):
        g = NUMTAB.get(name, {}).get(s)
        if g:
            return g[0], [(g, 0)]
    tab = NUMTAB["NUM54" if cap_px >= 54 else "NUM22"]
    if not s or not all(ch in tab for ch in s):
        return None
    pen, out = 0, []
    for ch in s:
        out.append((tab[ch], pen))
        pen += tab[ch][4]
    return pen - out[-1][0][4] + out[-1][0][0], out


def face(cap_px):
    """A font sized so digit cap height is cap_px at 1x (so cap_px*SS at 4x)."""
    if cap_px in _face_cache:
        return _face_cache[cap_px]
    for path in FACES:
        try:
            size = int(cap_px * SS * 1.38)
            f = ImageFont.truetype(path, size)
            box = f.getbbox("8")
            f = ImageFont.truetype(path, int(size * cap_px * SS / (box[3] - box[1])))
            _face_cache[cap_px] = f
            return f
        except OSError:
            continue
    raise SystemExit("no bold face found")


def num_size(c, s, cap_px):
    t = _table_layout(s, cap_px)
    if t is not None:
        return t[0], cap_px
    f = face(cap_px)
    b = c.d.textbbox((0, 0), s, font=f)
    return (b[2] - b[0]) / SS, (b[3] - b[1]) / SS


NUM_MIN_CAP = 16        # below this, use the bitmap font instead -- see num()


def num(c, x, y, s, cap_px, ink=INK, anchor="lt"):
    """
    Draw with a real typeface. x,y is the top-left of the ink box.

    Only above NUM_MIN_CAP. A dithered outline needs a few pixels of stem to
    survive: at cap 10 the fringe is as wide as the stroke and "TO GO KM ETA"
    resolved to grey mush. Small text is the 5x7 bitmap font, drawn as exact
    blocks -- crisp beats smooth once a glyph is only a few pixels tall.
    """
    if cap_px < NUM_MIN_CAP:
        scale = 1 if cap_px < 12 else 2
        w = tw(s, scale)
        bx = x if anchor == "lt" else (x - w / 2 if anchor == "ct" else x - w)
        text(c, round(bx), round(y), s, scale, ink)
        return w
    t = _table_layout(s, cap_px)
    if t is not None:
        # Table blit: integer pen advances, glyph ink drawn as exact SS x SS
        # blocks (same as text()), (dx, dy) placing '.' and '-' at their
        # type-correct heights. Identical arithmetic to seg.c's num_draw.
        w, glyphs = t
        bx = round(x if anchor == "lt"
                   else (x - w / 2 if anchor == "ct" else x - w))
        by = round(y)
        for (gw, gh, gdx, gdy, _adv, rows), pen in glyphs:
            stride = (gw + 7) // 8
            for ry in range(gh):
                run = -1
                for gx in range(gw + 1):
                    on = gx < gw and rows[ry * stride + gx // 8] & (0x80 >> (gx % 8))
                    if on and run < 0:
                        run = gx
                    elif not on and run >= 0:
                        c.rect(bx + pen + gdx + run, by + gdy + ry,
                               bx + pen + gdx + gx - 1, by + gdy + ry, ink)
                        run = -1
        return w
    f = face(cap_px)
    b = c.d.textbbox((0, 0), s, font=f)
    px, py = x * SS - b[0], y * SS - b[1]
    if anchor == "ct":
        px = x * SS - (b[0] + b[2]) / 2
    elif anchor == "rt":
        px = x * SS - b[2]
    c.d.text((px, py), s, font=f, fill=ink)
    return (b[2] - b[0]) / SS


# ------------------------------------------------------------------ turn glyphs
ARROWS = ("straight", "slight-left", "left", "sharp-left",
          "slight-right", "right", "sharp-right", "uturn", "dest")


def arrow(c, x0, y0, s, kind, ink=INK):
    """
    Cue glyphs as filled polygons, proportioned as fractions of the box so one
    routine serves the 76 px panel arrow and the 16 px inline one.

    The reference arrow is a single shape -- straight stem, square elbow, one
    broad triangular head -- so these are built the same way: a stroke path plus
    a head, drawn at 4x and resolved with the rest of the frame.
    """
    cx = x0 + s / 2
    hw = max(0.6, s * 0.10)                   # stem half-width
    hh = max(2.4, s * 0.26)                   # head half-width
    hl = max(3.5, s * 0.30)                   # head length

    def head(tx, ty, dx, dy):
        px, py = -dy, dx
        bx, by = tx - dx * hl, ty - dy * hl
        c.poly([(tx, ty), (bx + px * hh, by + py * hh),
                (bx - px * hh, by - py * hh)], ink)

    def bar(ax, ay, bx, by, cap=False):
        c.line((ax, ay), (bx, by), hw * 2, ink)
        if cap:
            c.disc(bx, by, hw, ink)

    if kind == "straight":
        bar(cx, y0 + s, cx, y0 + hl)
        head(cx, y0 + s * 0.02, 0, -1)

    elif kind in ("left", "right"):
        sgn = -1 if kind == "left" else 1
        ty = y0 + s * 0.36
        bar(cx, y0 + s, cx, ty, cap=True)
        x_tip = cx + sgn * s * 0.44
        bar(cx, ty, x_tip - sgn * hl, ty)
        head(x_tip, ty, sgn, 0)

    elif kind in ("slight-left", "slight-right"):
        sgn = -1 if "left" in kind else 1
        ty = y0 + s * 0.56
        dx, dy = sgn * 0.55, -0.84
        tip = (cx + sgn * s * 0.36, y0 + s * 0.04)
        bar(cx, y0 + s, cx, ty, cap=True)
        bar(cx, ty, tip[0] - dx * hl, tip[1] - dy * hl)
        head(tip[0], tip[1], dx, dy)

    elif kind in ("sharp-left", "sharp-right"):
        # Climb, then strike back down and outward at 135 degrees. A head
        # pointing below its own corner is the one cue "sharp" needs, and it is
        # the only version that still reads at 16 px.
        sgn = -1 if "left" in kind else 1
        ty = y0 + s * 0.28
        dx, dy = sgn * 0.7, 0.7
        tip = (cx + sgn * s * 0.44, y0 + s * 0.74)
        bar(cx, y0 + s, cx, ty, cap=True)
        bar(cx, ty, tip[0] - dx * hl, tip[1] - dy * hl)
        head(tip[0], tip[1], dx, dy)

    elif kind == "uturn":
        r = s * 0.23
        top = y0 + s * 0.30
        c.arc(cx, top, r, 180, 360, hw * 2, ink)
        bar(cx + r, top, cx + r, y0 + s)
        bar(cx - r, top, cx - r, y0 + s * 0.50 - hl)
        head(cx - r, y0 + s * 0.62, 0, 1)

    elif kind == "dest":                       # chequered flag
        fx, fy = cx - s * 0.20, y0 + s * 0.08
        fw, fh = s * 0.46, s * 0.34
        bar(fx, fy, fx, y0 + s)
        cw, chh = fw / 4, fh / 3
        for r in range(3):
            for col in range(4):
                if (r + col) % 2 == 0:
                    c.rect(fx + col * cw, fy + r * chh,
                           fx + (col + 1) * cw - 1, fy + (r + 1) * chh - 1, ink)
        c.d.rectangle([fx * SS, fy * SS, (fx + fw) * SS, (fy + fh) * SS],
                      outline=ink, width=SS)


# ------------------------------------------------------------------ sample state
NAV = dict(turn=410, kind="right", street="THANON SUKHUMVIT", spd=24,
           then_d="150M", then_kind="left",
           togo="12.6", eta="10:42", route="SUKHUMVIT LOOP",
           total="31.0", done="59", cues=11, cue_i=3,
           sats=9, fix="3D", batt=86, clock="09:40")


def fmt_dist(m):
    return (f"{m}", "M") if m < 1000 else (f"{m / 1000:.1f}", "KM")


# ------------------------------------------------------------------ geometry
# The stretch ahead of the fix is the reference's composition: straight on for
# 410 m, a right turn, 150 m east, then north again -- so the NAV page shows the
# staircase the screenshot shows, with the route exiting the top of the map.
ROUTE_M = [(0, -900), (0, -400), (40, -150), (40, 260), (190, 260), (190, 620),
           (430, 700), (560, 980), (430, 1180), (120, 1260), (-220, 1210),
           (-430, 1040), (-470, 780), (-330, 560), (-120, 520)]
POS_I, POS_F = 2, 0.0
TURN_I = 3                    # the announced cue: the route bend itself
PIN_I = 4                     # the pin marks the cue AFTER it -- the bend plus
                              # the panel already announce TURN_I, and this is
                              # what the reference pins (see DESIGN.md 1.1)
ZOOMS = [1.5, 2.5, 4, 6, 10, 15, 25, 40, 60, 100, 150, 250]


def lerp(a, b, f):
    return (a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f)


def clip_poly(pts, box):
    """Cohen-Sutherland per segment: without it the map paints over the panel."""
    x0b, y0b, x1b, y1b = box

    def code(x, y):
        return ((x < x0b) | ((x > x1b) << 1) | ((y < y0b) << 2) | ((y > y1b) << 3))

    out = []
    for (ax, ay), (bx, by) in zip(pts, pts[1:]):
        ca, cb = code(ax, ay), code(bx, by)
        while True:
            if not (ca | cb):
                out.append(((ax, ay), (bx, by)))
                break
            if ca & cb:
                break
            cc = ca or cb
            if cc & 8:
                x, y = ax + (bx - ax) * (y1b - ay) / (by - ay), y1b
            elif cc & 4:
                x, y = ax + (bx - ax) * (y0b - ay) / (by - ay), y0b
            elif cc & 2:
                x, y = x1b, ay + (by - ay) * (x1b - ax) / (bx - ax)
            else:
                x, y = x0b, ay + (by - ay) * (x0b - ax) / (bx - ax)
            if cc == ca:
                ax, ay, ca = x, y, code(x, y)
            else:
                bx, by, cb = x, y, code(x, y)
    return out


def project(pts, org, mpp, cx, cy, theta):
    ct, st = math.cos(theta), math.sin(theta)
    out = []
    for e, n in pts:
        e, n = e - org[0], n - org[1]
        out.append((cx + (e * ct - n * st) / mpp, cy - (e * st + n * ct) / mpp))
    return out


def round_corners(pts, radius=25.0, min_deg=12.0):
    """
    Replace sharp vertices with quadratic arcs of `radius` metres, in world
    space so a junction keeps the same real radius at every zoom. A GPX is a
    chain of straight chords; drawn raw the route reads as a polygon.
    """
    if len(pts) < 3:
        return list(pts)
    out = [pts[0]]
    for i in range(1, len(pts) - 1):
        p, cc, n = pts[i - 1], pts[i], pts[i + 1]
        v1, v2 = (p[0] - cc[0], p[1] - cc[1]), (n[0] - cc[0], n[1] - cc[1])
        l1, l2 = math.hypot(*v1), math.hypot(*v2)
        if l1 < 1e-6 or l2 < 1e-6:
            continue
        cosang = (v1[0] * v2[0] + v1[1] * v2[1]) / (l1 * l2)
        turn = 180.0 - math.degrees(math.acos(max(-1.0, min(1.0, cosang))))
        if turn < min_deg:
            out.append(cc)
            continue
        r = min(radius, l1 / 2, l2 / 2)
        a = (cc[0] + v1[0] / l1 * r, cc[1] + v1[1] / l1 * r)
        b = (cc[0] + v2[0] / l2 * r, cc[1] + v2[1] / l2 * r)
        steps = max(3, int(turn / 10))
        for s in range(steps + 1):
            t = s / steps
            out.append(((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * cc[0] + t * t * b[0],
                        (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * cc[1] + t * t * b[1]))
    out.append(pts[-1])
    return out


def stroke(c, segs, width, ink=INK):
    """Thick polyline with round joins and caps."""
    if not segs:
        return
    for a, b in segs:
        c.line(a, b, width, ink)
    r = width / 2
    c.disc(*segs[0][0], r, ink)
    c.disc(*segs[-1][1], r, ink)
    for (a, b), (p, e) in zip(segs, segs[1:]):
        if b != p:                                  # clipping split the line
            c.disc(*b, r, ink)
            c.disc(*p, r, ink)
            continue
        v1, v2 = (b[0] - a[0], b[1] - a[1]), (e[0] - p[0], e[1] - p[1])
        l1, l2 = math.hypot(*v1), math.hypot(*v2)
        if l1 < 1e-6 or l2 < 1e-6:
            continue
        cosang = (v1[0] * v2[0] + v1[1] * v2[1]) / (l1 * l2)
        if math.degrees(math.acos(max(-1.0, min(1.0, cosang)))) > 15:
            c.disc(*b, r, ink)      # only real joins; every arc point is lumpy


def dashed(c, segs, on=5, off=5, width=1, ink=INK):
    """Ridden track. Thin, so it goes through the crisp hairline layer."""
    carry = 0.0
    for (x0, y0), (x1, y1) in segs:
        seg = math.hypot(x1 - x0, y1 - y0)
        if seg < 0.01:
            continue
        pos = 0.0
        while pos < seg:
            end = min(seg, pos + (on if carry < on else off))
            if carry < on:
                c.hairline((x0 + (x1 - x0) * pos / seg, y0 + (y1 - y0) * pos / seg),
                           (x0 + (x1 - x0) * end / seg, y0 + (y1 - y0) * end / seg),
                           width, ink)
            carry = (carry + (end - pos)) % (on + off)
            pos = end


def cased_route(c, segs, outer=8, inner=4):
    """White casing under a black core: the route stays legible over streets."""
    stroke(c, segs, outer, PAPER)
    stroke(c, segs, inner, INK)


# ------------------------------------------------------------------ map marks
def position_marker(c, x, y, r=13, ang=0.0):
    """
    White disc, black ring, solid black chevron -- which is what the reference
    resolves to at threshold (an earlier draft had it inverted). The white fill
    is also the halo: it cuts the black route so the marker never merges into it.

    `ang` (radians, clockwise) points the chevron along the ACTUAL course. On a
    course-up map the rotation is smoothed, so mid-turn the map lags the bike by
    up to a second; drawn straight up, the chevron points off the road you are
    demonstrably on. It gets the residual angle (raw course minus map rotation)
    and stays true while the map catches up.
    """
    c.disc(x, y, r + 2.2, PAPER)
    c.disc(x, y, r, INK)
    c.disc(x, y, r - 2.2, PAPER)
    k = r * 0.62
    ca, sa = math.cos(ang), math.sin(ang)
    for shape in ([(0, k), (k * 0.85, -k * 0.85), (0, -k * 0.32),
                   (-k * 0.85, -k * 0.85)],):
        c.poly([(x + px * ca + py * sa, y + px * sa - py * ca)
                for px, py in shape], INK)


def pin(c, x, y, r=10):
    """Teardrop with a hole, tip at (x, y), as in the reference."""
    c.disc(x, y - r, r + 1.6, PAPER)
    c.poly([(x - (r + 1.6) * 0.76, y - r * 0.7), (x + (r + 1.6) * 0.76, y - r * 0.7),
            (x, y + 2.4)], PAPER)
    c.disc(x, y - r, r, INK)
    c.poly([(x - r * 0.70, y - r * 0.70), (x + r * 0.70, y - r * 0.70), (x, y + 1)],
           INK)
    c.disc(x, y - r, r * 0.36, PAPER)


def compass(c, x, y, theta=0.0, r=11):
    """
    Filled disc with a reversed N, plus a needle on the rim pointing to world
    north. On a course-up map the badge alone would be a lie -- north is
    whichever way the needle says, and it rotates with the map.
    """
    nx, ny = -math.sin(theta), -math.cos(theta)     # world north on screen
    c.disc(x, y, r + 1.6, PAPER)
    # The needle is detached from the badge by a 1 px white gap -- fused to the
    # rim it read as a droplet, not a pointer.
    base, tip = r + 2.5, r + 9
    px, py = -ny, nx

    def needle(b, t, hwidth, ink):
        c.poly([(x + nx * t, y + ny * t),
                (x + nx * b + px * hwidth, y + ny * b + py * hwidth),
                (x + nx * b - px * hwidth, y + ny * b - py * hwidth)], ink)

    needle(base - 1.4, tip + 1.6, 5.4, PAPER)       # halo, survives any street
    needle(base, tip, 4.0, INK)
    c.disc(x, y, r, INK)
    text(c, x - 6, y - 7, "N", 2, PAPER)


def speed_badge(c, x, y, kmh, r=27):
    """
    Current speed in a white circle, top-right of the map: black ring, the
    number at ~24 px cap with KM/H beneath. Same ring construction as the
    position marker, so the two read as one family of round instruments.
    """
    c.disc(x, y, r + 2.2, PAPER)
    c.disc(x, y, r, INK)
    c.disc(x, y, r - 2.2, PAPER)
    s = f"{max(0, int(round(kmh)))}"
    cap = 26
    while num_size(c, s, cap)[0] > 2 * (r - 7):
        cap -= 2
    num(c, x, y - 19, s, cap, INK, "ct")
    text(c, x - 12, y + 11, "KM/H", 1, INK)


def scale_bar(c, x, y, mpp):
    for m in (25, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000):
        px = m / mpp
        if 50 <= px <= 100:
            break
    px = int(px)
    c.rect(x - 2, y - 22, x + px + 2, y + 1, PAPER)
    c.rect(x, y, x + px, y + 1)                     # pixel-aligned: stays solid
    c.rect(x, y - 5, x + 1, y)
    c.rect(x + px - 1, y - 5, x + px, y)
    text(c, x + 2, y - 22, f"{m}M" if m < 1000 else f"{m // 1000}KM", 2)


# --------------------------------------------------------------- fake basemap
# Stands in for the optional raster tile layer (DESIGN.md 6.5). Hand-placed
# roads with gentle bends rather than a generated grid: the reference map is an
# irregular city fabric, and a lattice of straight lines read instantly as
# synthetic. Curves come from round_corners() with a large radius, so the same
# code path that smooths the route smooths the streets.
STREETS = [
    # north-south-ish
    ([(-380, -1200), (-310, -350), (-400, 500), (-300, 1500)], 1),
    ([(-90, -1200), (-150, -400), (-80, 300), (-170, 1500)], 1),
    ([(330, -1200), (390, -200), (300, 700), (380, 1500)], 2),
    ([(640, -1200), (580, -100), (670, 800), (600, 1500)], 1),
    ([(-680, -1200), (-620, -200), (-700, 900)], 1),
    # east-west-ish
    ([(-1200, -520), (-300, -450), (500, -540), (1200, -460)], 1),
    ([(-1200, 30), (-400, -40), (400, 50), (1200, -20)], 2),
    ([(-1200, 480), (-300, 420), (500, 510), (1200, 440)], 1),
    ([(-1200, 950), (-200, 880), (700, 990), (1200, 910)], 1),
    # diagonals and a crescent, because real cities have them
    ([(-1200, -850), (-250, -100), (600, 550), (1200, 1000)], 1),
    ([(-900, 1500), (-150, 650), (350, -250), (800, -1200)], 1),
    ([(-760, 150), (-560, 450), (-540, 850), (-740, 1150)], 1),
]


def draw_streets(c, org, mpp, cx, cy, theta, box, streets=None):
    for pts, wgt in (STREETS if streets is None else streets):
        segs = clip_poly(project(round_corners(pts, 180, 4),
                                 org, mpp, cx, cy, theta), box)
        for a, b in segs:
            c.hairline(a, b, wgt)


# ------------------------------------------------------------- real OSM data
# osm-asok.json is a one-off Overpass extract of every road around the Asok /
# Sukhumvit junction in Bangkok (ODbL, (c) OpenStreetMap contributors). This is
# the honest version of the basemap question: Google's tiles cannot legally be
# cached offline or re-rendered, and no online map style survives a 1-bit
# threshold anyway -- the production path is exactly this, OSM geometry drawn
# in our own cartography, pre-rendered into tiles on the Mac (DESIGN.md 6.5).
OSM_FILE = "osm-asok.json"
OSM_WEIGHT = {"motorway": 2, "trunk": 2, "primary": 2, "secondary": 2,
              "tertiary": 1, "unclassified": 1, "residential": 1,
              "living_street": 1}


def _chain(points, start, step_key, limit):
    """
    Greedy centreline: from `start`, repeatedly hop 30-130 m to the point that
    advances along step_key with the least sideways drift. The heavy lateral
    penalty is what keeps the chain on ONE carriageway of a divided road --
    at weight 3 it zig-zagged between the two sides of Asok and the route
    kinked; at 8 it holds its side.
    """
    pts, cur = [start], start
    while True:
        best, bestd = None, 1e9
        for p in points:
            ahead = step_key(p) - step_key(cur)
            if not (30 < ahead < 130):
                continue
            lateral = math.dist(p, cur) - ahead
            d = math.dist(p, cur) + 8 * lateral
            if d < bestd:
                best, bestd = p, d
        if best is None or abs(step_key(best) - step_key(start)) > limit:
            return pts
        pts.append(best)
        cur = best


_FRAME = None


def osm_frame():
    """(ways, name(), met()) in the junction-origin metre frame, cached --
    shared by the street layer, the route builder, search and the router, so
    every consumer of the extract agrees on coordinates."""
    global _FRAME
    if _FRAME is not None:
        return _FRAME
    import json
    import os
    if not os.path.exists(OSM_FILE):
        return None
    ways = [w for w in json.load(open(OSM_FILE))["elements"]
            if w["type"] == "way" and "geometry" in w]

    def name(w):
        return w["tags"].get("name:en") or w["tags"].get("name", "")

    # Junction of the two named roads = closest pair of their points. The
    # approach street is "Ratchadaphisek Road": south of Sukhumvit that is the
    # real name of the Asok corridor -- the extract has zero "Asok Montri"
    # points south of the junction, which is how this scenario found out.
    asok = [w for w in ways
            if "Asok Montri" in name(w) or name(w) == "Ratchadaphisek Road"]
    sukh = [w for w in ways if name(w) == "Sukhumvit Road"]
    best = (1e18, None)
    for wa in asok:
        for pa in wa["geometry"][::2]:
            for wb in sukh:
                for pb in wb["geometry"][::2]:
                    d = (pa["lat"] - pb["lat"]) ** 2 + (pa["lon"] - pb["lon"]) ** 2
                    if d < best[0]:
                        best = (d, pa)
    lat0, lon0 = best[1]["lat"], best[1]["lon"]
    ke, kn = 111320 * math.cos(math.radians(lat0)), 110540

    def met(p):
        return ((p["lon"] - lon0) * ke, (p["lat"] - lat0) * kn)

    _FRAME = (ways, name, met)
    return _FRAME


def load_osm():
    """-> (streets, route dict) from the extract, or None if it is absent."""
    fr = osm_frame()
    if fr is None:
        return None
    ways, name, m = fr

    streets = []
    for w in ways:
        streets.append(([m(p) for p in w["geometry"]],
                        OSM_WEIGHT.get(w["tags"]["highway"], 1)))

    # Route: north up Asok Montri to the junction, right, east on Sukhumvit.
    asok = [w for w in ways
            if "Asok Montri" in name(w) or name(w) == "Ratchadaphisek Road"]
    sukh = [w for w in ways if name(w) == "Sukhumvit Road"]
    apts = [m(p) for w in asok for p in w["geometry"]]
    spts = [m(p) for w in sukh for p in w["geometry"]]
    south = _chain(apts, (0.0, 0.0), lambda p: -p[1], 460)   # away from J
    east = _chain(spts, (0.0, 0.0), lambda p: p[0], 420)
    pts = list(reversed(south[1:])) + east                   # J appears once

    # Second cue: first soi mouth on Sukhumvit more than 100 m past the
    # junction. The route then turns INTO that soi -- a real route would --
    # which also gives the ride simulation a full cue sequence to play.
    pin_e = None
    for w in ways:
        if not name(w).startswith("Soi Sukhumvit"):
            continue
        for p in w["geometry"]:
            e, n = m(p)
            if 100 < e < 320 and abs(n) < 22:
                if pin_e is None or e < pin_e[0]:
                    pin_e = (e, n, name(w))
    turn_i = len(south) - 1                                   # J's index in pts
    pin_i = turn_i
    while pin_i + 1 < len(pts) and pts[pin_i + 1][0] < (pin_e[0] if pin_e else 160):
        pin_i += 1
    pts = pts[:pin_i + 1]                                     # cut at the soi
    if pin_e:
        soi_pts = [m(p) for w in ways if name(w) == pin_e[2]
                   for p in w["geometry"]]
        up = _chain(soi_pts, pts[-1], lambda p: p[1], 220)    # north, into it
        if len(up) < 3:
            up = _chain(soi_pts, pts[-1], lambda p: -p[1], 220)
        pts += up[1:]

    dist = sum(math.dist(a, b) for a, b in zip(pts[:turn_i], pts[1:turn_i + 1]))
    then = sum(math.dist(a, b)
               for a, b in zip(pts[turn_i:pin_i], pts[turn_i + 1:pin_i + 1]))
    route = dict(pts=pts, pos_i=0, pos_f=0.0, turn_i=turn_i, pin_i=pin_i,
                 turn=int(round(dist / 10) * 10),
                 then_d=f"{int(round(then / 10) * 10)}M",
                 cues=[(turn_i, "right"), (pin_i, "left"),
                       (len(pts) - 1, "dest")],
                 dest=(pin_e[2].upper() if pin_e else "DESTINATION"))
    return streets, route


# ----------------------------------------------------- search and the router
def search_places(query, pos, limit=5):
    """
    Token-AND match over street names in the pack, nearest first.
    -> [(dist_m, bearing_rad, NAME, (e, n)), ...]
    """
    ways, name, met = osm_frame()
    toks = query.upper().split()
    if not toks:
        return []
    best = {}
    for w in ways:
        nm = name(w).upper()
        if not nm or not all(t in nm for t in toks):
            continue
        for p in w["geometry"][::3]:
            pt = met(p)
            d = math.dist(pt, pos)
            if nm not in best or d < best[nm][0]:
                best[nm] = (d, pt)
    out = [(d, math.atan2(pt[0] - pos[0], pt[1] - pos[1]), nm, pt)
           for nm, (d, pt) in best.items()]
    return sorted(out)[:limit]


def route_graph():
    """
    Road graph keyed on exact shared coordinates (ways that join share a node,
    so their geometry carries identical lat/lon there). Measured on the Asok
    extract: 2803 nodes built in ~3 ms, Dijkstra end-to-end in ~0.1 ms -- a
    corridor pack routes instantly even at Pi Zero speeds.
    """
    ways, name, met = osm_frame()
    adj, coord = {}, {}
    for w in ways:
        g = w["geometry"]
        for a, b in zip(g, g[1:]):
            ka = (round(a["lat"] * 1e7), round(a["lon"] * 1e7))
            kb = (round(b["lat"] * 1e7), round(b["lon"] * 1e7))
            pa, pb = met(a), met(b)
            d = math.dist(pa, pb)
            adj.setdefault(ka, []).append((kb, d))
            adj.setdefault(kb, []).append((ka, d))
            coord[ka], coord[kb] = pa, pb
    return adj, coord


def route_to(pos, dest_pt):
    """Dijkstra shortest path pos -> dest_pt. -> (pts, metres). Oneway tags are
    ignored here (mockup); the production router must respect them."""
    import heapq
    adj, coord = route_graph()
    src = min(adj, key=lambda k: math.dist(coord[k], pos))
    dst = min(adj, key=lambda k: math.dist(coord[k], dest_pt))
    dist, prev, pq = {src: 0.0}, {}, [(0.0, src)]
    while pq:
        d, u = heapq.heappop(pq)
        if u == dst:
            break
        if d > dist.get(u, 1e18):
            continue
        for v, w_ in adj[u]:
            nd = d + w_
            if nd < dist.get(v, 1e18):
                dist[v], prev[v] = nd, u
                heapq.heappush(pq, (nd, v))
    path = [dst]
    while path[-1] != src:
        path.append(prev[path[-1]])
    return [coord[k] for k in reversed(path)], dist[dst]


# ----------------------------------------------- pre-ride: search and confirm
def compass8(b):
    return ("N", "NE", "E", "SE", "S", "SW", "W", "NW")[int(
        ((math.degrees(b) + 22.5) % 360) // 45)]


def render_search(query, pos, sel=0):
    """
    FIND screen: the query being typed, matches nearest-first. Driven by the
    Beepy's own QWERTY -- type-to-filter is this hardware's home advantage, so
    there is no on-screen keyboard and no cursor chasing.
    """
    c = Canvas()
    c.rect(0, 0, W - 1, 25, INK)
    text(c, 6, 5, "FIND", 2, PAPER)
    hits = search_places(query, pos)
    rtext(c, W - 6, 5, f"{len(hits)} HIT" + ("S" if len(hits) != 1 else ""),
          2, PAPER)

    num(c, 8, 34, query, 24, INK)
    qw = num_size(c, query, 24)[0]
    c.rect(14 + qw, 36, 14 + qw + 12, 58, INK)          # block cursor

    y = 74
    for i, (d, b, nm, _) in enumerate(hits[:4]):
        ink, paper = (PAPER, INK) if i == sel else (INK, PAPER)
        if i == sel:
            c.rect(0, y - 4, W - 1, y + 22, INK)
        text(c, 8, y, nm[:19], 2, ink)
        dtxt = (f"{d:.0f}M" if d < 1000 else f"{d/1000:.1f}KM") + f" {compass8(b)}"
        rtext(c, W - 8, y, dtxt, 2, ink)
        y += 34
    text(c, 8, H - 20, "TYPE TO FILTER   ENTER = ROUTE", 2)
    return resolve(c.img)


def render_confirm(rt, est_kmh=17.0):
    """
    Route overview + the one decision: GO or not. Same cartography as the
    OVERVIEW page -- this IS the overview, fitted to the planned route, with
    the strip swapped for distance / time / the confirm keys.
    """
    c = Canvas()
    pts = rt["pts"]
    strip = 42
    my0, my1, pad = 26, H - strip - 1, 20
    es, ns = [p[0] for p in pts], [p[1] for p in pts]
    org = ((min(es) + max(es)) / 2, (min(ns) + max(ns)) / 2)
    mpp = max((max(es) - min(es)) / (W - 2 * pad),
              (max(ns) - min(ns)) / (my1 - my0 - 2 * pad), 1.0)
    cx, cy = W / 2, (my0 + my1) / 2
    box = (0, my0, W - 1, my1)

    cased_route(c, clip_poly(project(round_corners(pts), org, mpp, cx, cy, 0.0),
                             box), outer=8, inner=4.5)
    scr = project(pts, org, mpp, cx, cy, 0.0)
    for i, _k in rt["cues"][:-1]:
        c.disc(*scr[i], 2.6, PAPER)
        c.disc(*scr[i], 1.8, INK)
    position_marker(c, *scr[0], 11)
    arrow(c, scr[-1][0] - 11, scr[-1][1] - 22, 22, "dest")

    compass(c, W - 21, my0 + 4 + 21)
    scale_bar(c, 7, my1 - 5, mpp)
    text(c, 6, 5, f"TO {rt['dest']}"[:33], 2)

    total = sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))
    mins = max(1, int(round(total / 1000 / est_kmh * 60)))
    c.rect(0, H - strip, W - 1, H - 1, INK)
    dtxt = f"{total/1000:.1f}KM" if total >= 950 else f"{total:.0f}M"
    text(c, 6, H - strip + 5, f"{dtxt}  EST {mins} MIN", 2, PAPER)
    text(c, 6, H - strip + 23, f"{len(rt['cues']) - 1} TURNS", 2, PAPER)
    rtext(c, W - 6, H - strip + 5, "ENTER = GO", 2, PAPER)
    rtext(c, W - 6, H - strip + 23, "Q = CANCEL", 2, PAPER)
    return resolve(c.img)


def page_search():
    osm = load_osm()
    if osm:
        finish(render_search("SOI 23", osm[1]["pts"][0]), "nav-search")


def page_confirm():
    osm = load_osm()
    if osm:
        finish(render_confirm(osm[1]), "nav-confirm")


# ------------------------------------------------------------------- the pages
def cue_distance(pts, pos, pos_i, turn_i):
    total, prev = 0.0, pos
    for i in range(pos_i + 1, turn_i + 1):
        total += math.dist(prev, pts[i])
        prev = pts[i]
    return total


def auto_zoom(dist, ahead_px):
    for mpp in ZOOMS:
        if dist / mpp <= 0.8 * ahead_px:
            return mpp
    return ZOOMS[-1]


def turn_panel(c, off=None):
    """
    Left third, inverted: arrow over distance over unit, the stack from the
    reference. The strip along the bottom carries the state that has nowhere
    else to live now that there is no status bar.
    """
    c.rect(0, 0, PANEL_W - 1, H - 1, INK)

    if off:
        # Off route the junction distance is meaningless, so it is withheld
        # rather than frozen: a stale "410 M" beside a route you are not on is
        # the worst thing this panel could say. No helper text either -- the
        # dotted tie-line on the map already points back to the route.
        num(c, PANEL_W / 2, 10, "OFF", 24, PAPER, "ct")
        num(c, PANEL_W / 2, 44, "ROUTE", 24, PAPER, "ct")
        cap = 60
        while num_size(c, str(off), cap)[0] > PANEL_W - 10:
            cap -= 2
        num(c, PANEL_W / 2, 96, str(off), cap, PAPER, "ct")
        num(c, PANEL_W / 2, 96 + cap + 10, "M AWAY", 20, PAPER, "ct")
    else:
        arrow(c, (PANEL_W - 76) / 2, 6, 76, NAV["kind"], PAPER)
        val, unit = fmt_dist(NAV["turn"])
        cap = 64
        # PANEL_W - 8, not - 10: integer advances round a 3-digit NUM54
        # string to 119 px, and a 118 px limit would demote it to the 22 px
        # set by that single pixel. The visual margin is still 4+ px a side.
        while num_size(c, val, cap)[0] > PANEL_W - 8:
            cap -= 2
        num(c, PANEL_W / 2, 92, val, cap, PAPER, "ct")
        num(c, PANEL_W / 2, 92 + cap + 8, unit, 24, PAPER, "ct")

    c.rect(6, H - 50, PANEL_W - 7, H - 50, PAPER)
    if not off:
        # Next cue preview: a 20 px glyph says THEN, the distance says when.
        arrow(c, 6, H - 44, 20, NAV["then_kind"], PAPER)
        text(c, 34, H - 40, NAV["then_d"], 2, PAPER)
    # Battery and clock at scale 2. Fix state earns panel space only when it is
    # a problem: NO FIX replaces this line, inverted -- a healthy receiver is
    # not information (DESIGN.md 1.1).
    text(c, 5, H - 19, f"{NAV['batt']}% {NAV['clock']}", 2, PAPER)


def page_nav(name, basemap=False, off=None, course_up=True, dither=False,
             route=None, streets=None):
    c = Canvas()
    rt = route or dict(pts=ROUTE_M, pos_i=POS_I, pos_f=POS_F,
                       turn_i=TURN_I, pin_i=PIN_I,
                       turn=NAV["turn"], then_d=NAV["then_d"])
    pts, pos_i, turn_i = rt["pts"], rt["pos_i"], rt["turn_i"]

    on_route = lerp(pts[pos_i], pts[pos_i + 1], rt["pos_f"])
    nxt = pts[pos_i + 1]
    heading = math.atan2(nxt[0] - on_route[0], nxt[1] - on_route[1])
    # +heading, not -: project() rotates the world by +theta, so putting the
    # course at the top of the screen takes theta = heading. The sign never
    # showed on these static pages (their heading is a few degrees off north,
    # where +h and -h are indistinguishable) -- the ride simulation's 90-degree
    # turn is what exposed it: with -heading the street ahead swung AWAY from
    # the chevron instead of staying under it.
    theta = heading if course_up else 0.0
    perp = (math.cos(heading), -math.sin(heading))
    pos = (on_route[0] + perp[0] * off, on_route[1] + perp[1] * off) if off \
        else on_route

    cx = MAP_X + (W - MAP_X) / 2
    cy = H * 0.72                        # fix sits low: two thirds is road ahead
    mpp = auto_zoom(cue_distance(pts, on_route, pos_i, turn_i), cy)
    box = (MAP_X, 0, W - 1, H - 1)

    if basemap:
        draw_streets(c, pos, mpp, cx, cy, theta, box, streets)

    scr = project(pts, pos, mpp, cx, cy, theta)          # markers, unrounded
    track_w = pts[:pos_i + 1]
    if off:
        track_w = track_w + [lerp(pts[pos_i], on_route, 0.55)]
    dashed(c, clip_poly(project(round_corners(track_w + [pos]),
                                pos, mpp, cx, cy, theta), box), width=1)

    c.flush_hairlines()          # streets and track land under the route casing
    cased_route(c, clip_poly(project(round_corners([on_route] + pts[pos_i + 1:]),
                                     pos, mpp, cx, cy, theta), box),
                outer=10, inner=6)

    if off:
        ox, oy = project([on_route], pos, mpp, cx, cy, theta)[0]
        for t in range(0, 100, 7):
            c.disc(cx + (ox - cx) * t / 100, cy + (oy - cy) * t / 100, 1.5)
    else:
        # The pin marks the cue after the announced one: the announced junction
        # is already the bend in the route plus the whole left panel, and this
        # is where the reference puts its teardrop.
        tx, ty = scr[rt["pin_i"]]
        if MAP_X + 10 < tx < W - 10 and 24 < ty < H - 10:
            pin(c, tx, ty)

    position_marker(c, cx, cy)
    compass(c, MAP_X + 21, 27, theta)
    speed_badge(c, W - 33, 33, NAV["spd"])
    scale_bar(c, MAP_X + 7, H - 8, mpp)

    saved = (NAV["turn"], NAV["then_d"])
    NAV["turn"], NAV["then_d"] = rt["turn"], rt["then_d"]
    turn_panel(c, off)
    NAV["turn"], NAV["then_d"] = saved
    c.rect(PANEL_W, 0, PANEL_W, H - 1, INK)
    finish(resolve(c.img, dither), name)


def page_overview():
    c = Canvas()
    strip = 42
    my0 = 24                     # title band: the route must never run under it
    my1 = H - strip - 1
    pad = 16
    es, ns = [p[0] for p in ROUTE_M], [p[1] for p in ROUTE_M]
    org = ((min(es) + max(es)) / 2, (min(ns) + max(ns)) / 2)
    mpp = max((max(es) - min(es)) / (W - 2 * pad),
              (max(ns) - min(ns)) / (my1 - my0 - 2 * pad))
    cx, cy = W / 2, (my0 + my1) / 2
    box = (0, my0, W - 1, my1)
    scr = project(ROUTE_M, org, mpp, cx, cy, 0.0)
    pos = lerp(ROUTE_M[POS_I], ROUTE_M[POS_I + 1], POS_F)
    px, py = project([pos], org, mpp, cx, cy, 0.0)[0]

    dashed(c, clip_poly(project(round_corners(ROUTE_M[:POS_I + 1] + [pos]),
                                org, mpp, cx, cy, 0.0), box), width=1)
    c.flush_hairlines()
    cased_route(c, clip_poly(project(round_corners([pos] + ROUTE_M[POS_I + 1:]),
                                     org, mpp, cx, cy, 0.0), box),
                outer=8, inner=4.5)

    for i in (3, 4, 6, 8, 10, 12):          # every cue, so turn count reads
        c.disc(*scr[i], 2.6, PAPER)
        c.disc(*scr[i], 1.8, INK)

    c.disc(*scr[0], 5.5, PAPER)
    c.ring(*scr[0], 4.5, 2.0, INK)
    arrow(c, scr[-1][0] - 11, scr[-1][1] - 22, 22, "dest")
    position_marker(c, px, py, 9)

    compass(c, W - 21, 27)
    scale_bar(c, 7, my1 - 5, mpp)
    # Route name and total length share the title line; that frees the strip's
    # second line, which is what lets everything in it run at scale 2.
    text(c, 6, 4, f"{NAV['route']}  {NAV['total']}KM", 2)

    c.rect(0, H - strip, W - 1, H - 1, INK)
    text(c, 6, H - strip + 5, f"{NAV['done']}% DONE", 2, PAPER)
    text(c, 6, H - strip + 23, f"{NAV['cue_i']}/{NAV['cues']} CUES  ETA {NAV['eta']}",
         2, PAPER)
    wd = num(c, W - 8, H - strip + 5, f"{NAV['togo']}", 32, PAPER, "rt")
    rtext(c, W - 14 - wd, H - strip + 5, "TO GO", 2, PAPER)
    rtext(c, W - 14 - wd, H - strip + 23, "KM", 2, PAPER)
    finish(resolve(c.img), "nav-overview")


def page_arrows():
    c = Canvas()
    c.rect(0, 0, W - 1, 17, INK)
    text(c, 4, 5, "CUE GLYPH SET", 1, PAPER)
    rtext(c, W - 4, 5, "76 / 40 / 24 / 16 PX", 1, PAPER)
    names = ("STR", "SLTL", "LEFT", "SHPL", "SLTR", "RGHT", "SHPR", "UTRN", "END")
    x = 4
    for kind, name in zip(ARROWS, names):
        arrow(c, x, 24, 40, kind)
        arrow(c, x + 8, 70, 24, kind)
        arrow(c, x + 12, 100, 16, kind)
        text(c, x, 120, name, 1)
        x += 44
    c.rect(0, 133, W - 1, 133)
    arrow(c, 4, 140, 76, "right")
    # Full size means the NAV page's size: the 54 px set (the caption says
    # so). The unit drops to y 208 to keep clear of the taller digits.
    num(c, 88, 146, "410", 54)
    num(c, 88, 208, "M", 20)
    text(c, 208, 150, "PANEL ARROW AND", 2)
    text(c, 208, 172, "DISTANCE AT FULL", 2)
    text(c, 208, 194, "SIZE, AS THE NAV", 2)
    text(c, 208, 216, "PAGE DRAWS THEM", 2)
    finish(resolve(c.img), "nav-arrows")


def page_smooth():
    """
    The whole question, side by side: identical geometry, resolved two ways.
    Judge it on the panel -- 3x makes dithering obviously softer, and the honest
    question is whether it still helps at 1x in daylight.
    """
    for tag, dith in (("hard", False), ("dither", True)):
        page_nav(f"nav-smooth-{tag}", basemap=True, dither=dith)

    a = Image.open("nav-smooth-hard.png").convert("L")
    b = Image.open("nav-smooth-dither.png").convert("L")
    img = Image.new("1", (W, H), 1)
    img.paste(a.crop((0, 0, 200, 240)).convert("1"), (0, 0))
    img.paste(b.crop((0, 0, 200, 240)).convert("1"), (200, 0))
    d = ImageDraw.Draw(img)
    d.line([200, 0, 200, 239], fill=0)
    d.rectangle([0, 224, 399, 239], fill=0)
    c = Canvas()
    text(c, 4, 228, "LEFT HARD THRESHOLD", 1, PAPER)
    text(c, 208, 228, "RIGHT SUPERSAMPLE + BAYER", 1, PAPER)
    img.paste(resolve(c.img).crop((0, 224, 400, 240)), (0, 224))
    finish(img, "nav-smooth")


def finish(img, name):
    img.save(f"{name}.png")
    (img.convert("L").resize((W * 3, H * 3), Image.NEAREST)
        .convert("1").save(f"{name}-3x.png"))
    print(f"wrote {name}.png / {name}-3x.png")


if __name__ == "__main__":
    page_nav("nav-turn", basemap=True)
    osm = load_osm()
    if osm:
        page_nav("nav-turn-osm", basemap=True, route=osm[1], streets=osm[0])
        page_search()
        page_confirm()
    page_nav("nav-turn-nomap")
    page_nav("nav-turn-off", basemap=True, off=85)
    page_overview()
    page_arrows()
    page_smooth()

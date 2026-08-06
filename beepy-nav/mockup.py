#!/usr/bin/env python3
"""
Design mockups for beepy-nav -- a two-page GPS route navigator on the Beepy's
400x240 1-bit Sharp memory LCD.

    NAV       inverted turn panel on the left, live map on the right
    MAP       where you are, full width, with no route loaded
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
    nav-map         MAP page: where you are, with no route loaded
    nav-map-nofix   the same, with the fix lost after having had one
    nav-map-wait    the same page before the first fix has ever arrived
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
# tools/gen_labels.py -> src/labels.h, tools/gen_query.py -> src/query24.h),
# num() blits them with integer advances instead of rasterizing through PIL --
# the same tables, the same advances and the same set-selection rule as seg.c,
# so mockup and C numerals are identical BY CONSTRUCTION rather than by
# resemblance. The PIL path below survives as the fallback when the headers are
# absent, and it is still what the table generators themselves render with.
#
# The FIND query used to be the one string left on the PIL path (M6: the device
# has no TrueType rasterizer, so "24 px bold" had to become a table like every
# other size). QUERY24 is that table; nav-search.png moved when it landed, by
# the integer-advance tracking the table imposes.
TABLE_FILES = ("src/numerals.h", "src/labels.h", "src/query24.h")


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
    # QUERY24 is asked for by cap, and only by the one cap the FIND page uses.
    # Nothing else on any page sets anything at 24, so no other frame can pick
    # up letterforms where it was expecting digits.
    if cap_px == QUERY_CAP and "QUERY24" in NUMTAB:
        tab = NUMTAB["QUERY24"]
    else:
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


def num_pen(c, s, cap_px):
    """
    Total pen ADVANCE, which is where the next glyph would start -- not where
    the ink stops. seg.c's num_advance(); see it for why the FIND cursor needs
    the difference (a trailing space makes the ink box narrower, so a caret at
    the ink edge walks backwards over the previous letter).
    """
    t = _table_layout(s, cap_px)
    if t is not None:
        return sum(g[4] for g, _pen in t[1])
    if not s:
        return 0.0
    return c.d.textlength(s, font=face(cap_px)) / SS


NUM_MIN_CAP = 16        # below this, use the bitmap font instead -- see num()
QUERY_CAP = 24          # the FIND query's cap, and QUERY24's (DESIGN.md 1.4)


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
           sats=9, fix="3D", batt=86, clock="09:40", togo_m=12600,
           remain="44 MIN", eta_txt="10:42 ETA")


CUE_FLOOR = 10


def cue_quantise(m):
    """
    DESIGN.md 1.1.1 -- what the rider is shown, not what was measured. Floored
    onto a ladder that coarsens with distance: 100 m steps beyond a kilometre,
    50 m from 200 m out, 10 m inside that, holding at the bottom rung for the
    last few metres. Coarse where there is nothing to do yet, fine where you
    are about to act.

    Must stay identical to cue_quantise() in src/route.c -- the design gate
    compares the two renderers pixel for pixel.
    """
    m = int(m)
    if m >= 1000:
        return m - m % 100
    if m >= 200:
        return m - m % 50
    if m >= CUE_FLOOR:
        return m - m % 10
    return CUE_FLOOR


def fmt_dist(m):
    """Quantise, then choose the unit."""
    q = cue_quantise(m)
    return (f"{q}", "M") if q < 1000 else (f"{q / 1000:.1f}", "KM")


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


# ------------------------------------------------------- 3D nav view (6.6)
#
# This mirrors beepy-nav/src/nav3d.c and persp.c, and the word "mirrors" is
# doing real work: the design gate byte-compares the two, so every rule here is
# transcribed rather than reinvented. Where the C rounds, this rounds the same
# way; where the C bails, this bails.
#
# Two things make that tractable. The ribbon sort tie-breaks on the edge's own
# NODE INDICES rather than on gather order, so this file can walk the pack in
# whatever order Python likes and still paint the identical sequence -- no ring
# walk and no radix sort to reproduce. And the pack is read DIRECTLY, rather
# than re-derived from osm-asok.json: deriving it would mean reimplementing
# mkpack.py's way splitting and class assignment, and the gate would then be
# comparing two copies of the pipeline instead of two renderers.

ROADS_PACK = "tests/roads/asok.roads"

# Transcribed from nav3d.c, which took them from mktiles.py's OSM_WIDTH_M.
N3_W_M = (5.0, 14.0, 12.0, 11.0, 9.0, 7.0, 6.0, 5.0, 4.0)
N3_CULL_M = (900.0, 4000.0, 3000.0, 2000.0, 1400.0, 900.0, 600.0, 450.0, 350.0)
N3_ROUTE_W_M = 9.0
N3_ROUTE_CAP_PX = 20.0
N3_MIN_SEG_PX = 3.0
N3_MIN_W_PX = 0.8
N3_PITCH_DEG = 70.0
N3_FOCAL = 120.0
N3_CHEVRON_FRAC = 0.72

_ROADS = None


def roads_pack(path=None):
    """(en, adj, edge, lat0, lon0, klat, klon) from a BNAVROAD pack, cached.
    Format per tools/mkpack.py; only the three graph sections are read."""
    global _ROADS
    if _ROADS is not None:
        return _ROADS
    import os
    import struct
    path = path or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               ROADS_PACK)
    if not os.path.exists(path):
        return None
    f = open(path, "rb")
    hdr = f.read(64)
    if hdr[:8] != b"BNAVROAD":
        return None
    nsect = struct.unpack_from("<H", hdr, 14)[0]
    lat0, lon0, klat, klon = struct.unpack_from("<dddd", hdr, 20)
    cscale = struct.unpack_from("<I", hdr, 52)[0]
    hb = struct.unpack_from("<H", hdr, 10)[0]
    f.seek(hb)
    sect = [struct.unpack("<II", f.read(8)) for _ in range(nsect)]
    (noff, ncnt), (aoff, acnt), (eoff, ecnt) = sect[0], sect[1], sect[2]
    f.seek(noff)
    raw = f.read(8 * ncnt)
    ke = klon * math.cos(math.radians(lat0))
    en = []
    for i in range(ncnt):
        la, lo = struct.unpack_from("<ii", raw, 8 * i)
        en.append(((lo / cscale - lon0) * ke, (la / cscale - lat0) * klat))
    f.seek(aoff)
    raw = f.read(4 * acnt)
    adj = list(struct.unpack("<%dI" % acnt, raw))
    f.seek(eoff)
    raw = f.read(14 * ecnt)
    edge = []
    for j in range(ecnt):
        to, _lmm, fl, _nm = struct.unpack_from("<IIHI", raw, 14 * j)
        edge.append((to, (fl >> 1) & 0xF))
    f.close()
    _ROADS = (en, adj, edge, lat0, lon0, klat, ke)
    return _ROADS


class Persp:
    """persp.c, transcribed. Pitch 90 is EXACTLY the top-down affine -- the
    invariant t_pitch90_is_2d asserts on the C side."""

    def __init__(self, pitch_deg, focal, nadir_mpp, heading_rad, vw, vh,
                 chevron_frac):
        pitch_deg = min(90.0, max(1.0, pitch_deg))
        self.p = math.radians(pitch_deg)
        self.f = focal
        self.h = nadir_mpp * focal
        self.sp, self.cp = math.sin(self.p), math.cos(self.p)
        self.cx, self.cy = vw / 2.0, vh / 2.0
        self.fwd = (math.sin(heading_rad), math.cos(heading_rad))
        self.rgt = (math.cos(heading_rad), -math.sin(heading_rad))
        v_r = self.cy - chevron_frac * vh
        den = self.f * self.sp - v_r * self.cp
        self.back = 0.0 if abs(den) < 1e-12 else \
            (self.h / den) * (v_r * self.sp + self.f * self.cp)
        self.has_horizon = self.cp > 1e-12
        self.horizon_v = (self.f * self.sp / self.cp) if self.has_horizon else 0.0

    def to_ground(self, de, dn):
        return (de * self.fwd[0] + dn * self.fwd[1],
                de * self.rgt[0] + dn * self.rgt[1])

    def screen(self, fwd, lat):
        """(x, y, t) or None. t is the metres a pixel covers there."""
        Y = fwd + self.back
        den = -(Y * self.cp + self.h * self.sp)
        if abs(den) < 1e-12:
            return None
        v = (self.h * self.f * self.cp - Y * self.f * self.sp) / den
        if self.has_horizon and v >= self.horizon_v - 1e-9:
            return None
        tt = self.f * self.sp - v * self.cp
        if not tt > 1e-12:
            return None
        tt = self.h / tt
        if not tt > 0.0:
            return None
        return (self.cx + lat / tt, self.cy - v, tt)


class Bits:
    """A 1x bit plane with nav3d.c's rasteriser, not PIL's.

    This class exists because PIL's line and polygon rules are NOT the C's:
    measured at pitch 70 the two disagreed by 2220 pixels of ink -- 3685
    python-only against 1465 C-only -- while agreeing on every road they chose
    to draw. Selection was already at parity; only the rasteriser was not. The
    fix has to be here rather than in the C, because the C is what ships."""

    def __init__(self, w, h):
        self.w, self.h = w, h
        self.px = bytearray(w * h)

    def set(self, x, y, on):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = 1 if on else 0

    @staticmethod
    def q256(v):
        """nav3d.c's q256(): 1/256 px, so an ulp cannot cross a rounding
        boundary. See the comment there -- this is 6.5's fixed-point argument."""
        return math.floor(v * 256.0 + 0.5) / 256.0

    def line(self, fx0, fy0, fx1, fy1, on):
        """nav3d.c's line(): endpoints rounded to nearest, plain Bresenham,
        with the same iteration guard so a near-horizon segment cannot run
        away."""
        x0 = math.floor(self.q256(fx0) + 0.5); y0 = math.floor(self.q256(fy0) + 0.5)
        x1 = math.floor(self.q256(fx1) + 0.5); y1 = math.floor(self.q256(fy1) + 0.5)
        dx, dy = abs(x1 - x0), abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        guard = min(dx + dy + 4, 4 * (400 + 240))
        while True:
            self.set(x0, y0, on)
            if (x0 == x1 and y0 == y1) or guard <= 0:
                return
            guard -= 1
            e2 = 2 * err
            if e2 > -dy:
                err -= dy; x0 += sx
            if e2 < dx:
                err += dx; y0 += sy

    def quad_fill(self, q, on):
        """nav3d.c's quad_fill(): convex scanline, half-open edge rule."""
        q = [self.q256(v) for v in q]
        ys = [q[2 * i + 1] for i in range(4)]
        ymin, ymax = min(ys), max(ys)
        if ymin < 0.0:
            ymin = 0.0
        if ymax > float(self.h - 1):
            ymax = float(self.h - 1)
        for y in range(int(math.floor(ymin + 0.5)),
                       int(math.floor(ymax + 0.5)) + 1):
            yc = float(y)
            xl, xr, hit = 1e300, -1e300, 0
            for i in range(4):
                j = (i + 1) & 3
                ya, yb = q[2 * i + 1], q[2 * j + 1]
                xa2, xb2 = q[2 * i], q[2 * j]
                if ya == yb:
                    continue
                if not ((ya <= yc < yb) or (yb <= yc < ya)):
                    continue
                xx = xa2 + (yc - ya) / (yb - ya) * (xb2 - xa2)
                xl = min(xl, xx); xr = max(xr, xx)
                hit += 1
            if hit < 2:
                continue
            # The SPAN ends, quantised as nav3d.c does -- see the comment
            # there. xl and xr are interpolated, so bounding their inputs does
            # not bound them.
            xa = max(0, int(math.floor(self.q256(xl) + 0.5)))
            xb = min(self.w - 1, int(math.floor(self.q256(xr) + 0.5)))
            for x in range(xa, xb + 1):
                self.set(x, y, on)


def _n3_ribbon(bits, cam, fa, la, fb, lb, w, cap_px, mode):
    """nav3d.c's ribbon(). mode: 0 edges only, 1 paper fill + edges, 2 solid.
    The half-width is capped PER VERTEX -- per segment notches the edge."""
    df, dl = fb - fa, lb - la
    L = math.sqrt(df * df + dl * dl)
    if not L > 1e-9:
        return 0
    pf, pl = -dl / L, df / L
    ends = []
    for (f0, l0) in ((fa, la), (fb, lb)):
        s = cam.screen(f0, l0)
        if s is None:
            return 0
        hw = w / 2.0
        if cap_px > 0.0 and hw > cap_px / 2.0 * s[2]:
            hw = cap_px / 2.0 * s[2]
        p1 = cam.screen(f0 + pf * hw, l0 + pl * hw)
        p2 = cam.screen(f0 - pf * hw, l0 - pl * hw)
        if p1 is None or p2 is None:
            return 0
        ends.append((p1, p2))
    q = [ends[0][0][0], ends[0][0][1], ends[1][0][0], ends[1][0][1],
         ends[1][1][0], ends[1][1][1], ends[0][1][0], ends[0][1][1]]
    if mode == 2:
        bits.quad_fill(q, 1)
        return 1
    if mode == 1:
        bits.quad_fill(q, 0)
    bits.line(q[0], q[1], q[2], q[3], 1)
    bits.line(q[4], q[5], q[6], q[7], 1)
    return 1


def draw_nav3d(c, lat, lon, heading, mpp, route_en, knockouts,
               x0=None, vw=None, vh=None, pitch_deg=N3_PITCH_DEG):
    """The 3D map plane, into the canvas's 1x layer. Returns ribbons drawn, or
    0 with nothing drawn when there is no pack -- which is how a checkout with
    no fixture still renders the 2D page."""
    pack = roads_pack()
    if pack is None:
        return 0
    en, adj, edge, lat0, lon0, klat, ke = pack
    x0 = MAP_X if x0 is None else x0
    vw = (W - MAP_X) if vw is None else vw
    vh = H if vh is None else vh
    cam = Persp(pitch_deg, N3_FOCAL, mpp, heading, float(vw), float(vh),
                N3_CHEVRON_FRAC)
    e0 = (lon - lon0) * ke
    n0 = (lat - lat0) * klat
    bits = Bits(vw, vh)

    # Gather. The C walks cells outward from the rider because it has 1.7
    # million nodes; the committed fixture has a few thousand and they are all
    # inside the widest cull, so a plain scan sees the identical set. The sort
    # key does not depend on the order either way.
    ribs = []
    for nd in range(len(en)):
        fa, la = cam.to_ground(en[nd][0] - e0, en[nd][1] - n0)
        for j in range(adj[nd], adj[nd + 1]):
            to, cls = edge[j]
            if to <= nd or to >= len(en):
                continue
            if cls < 0 or cls >= 9:
                cls = 0
            fb, lb = cam.to_ground(en[to][0] - e0, en[to][1] - n0)
            da = math.sqrt(fa * fa + la * la)
            db = math.sqrt(fb * fb + lb * lb)
            d = min(da, db)
            if d > N3_CULL_M[cls] or d > N3_CULL_M[1]:
                continue
            sa = cam.screen(fa, la)
            sb = cam.screen(fb, lb)
            if sa is None or sb is None:
                continue
            w = N3_W_M[cls]
            if w / max(sa[2], sb[2]) < N3_MIN_W_PX:
                continue
            ribs.append((max(sa[2], sb[2]), nd, to, fa, la, fb, lb, cls,
                         min(sa[2], sb[2])))
    # Far to near, tie-broken on node indices: a total order both sides share.
    ribs.sort(key=lambda r: (-r[0], r[1], r[2]))

    drawn = 0
    for (_tf, _na, _nb, fa, la, fb, lb, cls, tnear) in ribs:
        sa = cam.screen(fa, la)
        sb = cam.screen(fb, lb)
        if sa is None or sb is None:
            continue
        if math.hypot(sb[0] - sa[0], sb[1] - sa[1]) < N3_MIN_SEG_PX:
            continue
        w = N3_W_M[cls]
        drawn += _n3_ribbon(bits, cam, fa, la, fb, lb, w, 0.0,
                            1 if (w / tnear) >= 2.2 else 0)
    for i in range(len(route_en) - 1):
        fa, la = cam.to_ground(route_en[i][0] - e0, route_en[i][1] - n0)
        fb, lb = cam.to_ground(route_en[i + 1][0] - e0, route_en[i + 1][1] - n0)
        drawn += _n3_ribbon(bits, cam, fa, la, fb, lb, N3_ROUTE_W_M,
                            N3_ROUTE_CAP_PX, 2)
    for ko in knockouts:
        if ko[0] > 0.0:                       # (r, cx, cy)
            r, kcx, kcy = ko[0], ko[1], ko[2]
            for yy in range(int(math.floor(kcy - r)), int(math.ceil(kcy + r)) + 1):
                dy = float(yy) - kcy
                dx2 = r * r - dy * dy
                if dx2 < 0.0:
                    continue
                for xx in range(int(math.floor(kcx - math.sqrt(dx2))),
                                int(math.ceil(kcx + math.sqrt(dx2))) + 1):
                    bits.set(xx, yy, 0)
        else:                                 # (0, x0, y0, x1, y1)
            for yy in range(int(math.floor(ko[2])), int(math.ceil(ko[4])) + 1):
                for xx in range(int(math.floor(ko[1])), int(math.ceil(ko[3])) + 1):
                    bits.set(xx, yy, 0)

    # Into the canvas's 1x layer, which is the aliased path -- the same place
    # hairlines go, and the mockup's equivalent of one cov_blit_bits().
    for y in range(vh):
        row = y * vw
        for x in range(vw):
            if bits.px[row + x]:
                c.hd.point((x0 + x, y), fill=INK)
    return drawn


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
    global _OSM_ORIGIN
    _OSM_ORIGIN = (lat0, lon0, ke, kn)
    return _FRAME


_OSM_ORIGIN = None


def osm_origin():
    """(lat0, lon0, ke, kn) of the junction frame every mockup coordinate is
    measured from. Exposed because the 3D view (DESIGN.md 6.6) hands points to
    a roads pack that keeps its OWN reference, and lat/lon is the only language
    two tangent frames share -- the same reason nav.c needs geo_unproject()."""
    if _OSM_ORIGIN is None:
        osm_frame()
    return _OSM_ORIGIN


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


def render_search(query, pos, sel=0, nhits=None):
    """
    FIND screen: the query being typed, matches nearest-first. Driven by the
    Beepy's own QWERTY -- type-to-filter is this hardware's home advantage, so
    there is no on-screen keyboard and no cursor chasing.

    `nhits` is the TOTAL number of matches, which is not the same as the length
    of the list below it: search_places() stops at its limit, and a title bar
    that reads "5 HITS" when there are ninety is not the "live hit count"
    DESIGN.md 1.4 promises. The C side (search.c) returns the total separately;
    here the default keeps the old behaviour for a caller that has only a list.
    """
    c = Canvas()
    c.rect(0, 0, W - 1, 25, INK)
    text(c, 6, 5, "FIND", 2, PAPER)
    hits = search_places(query, pos)
    n = len(hits) if nhits is None else nhits
    rtext(c, W - 6, 5, f"{n} HIT" + ("S" if n != 1 else ""), 2, PAPER)

    num(c, 8, 34, query, QUERY_CAP, INK)
    # The pen, not the ink box: see num_pen().
    qw = num_pen(c, query, QUERY_CAP)
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


def render_confirm(rt, est_kmh=17.0, mode="BIKE"):
    """
    Route overview + the one decision: GO or not. Same cartography as the
    OVERVIEW page -- this IS the overview, fitted to the planned route, with
    the strip swapped for distance / time / the confirm keys.

    `mode` is DESIGN.md 7.7's travel mode, and it is on this page because this is
    where a rider commits to a route the mode SHAPED: bike and car do not return
    a refinement of the same line, they return 43.8 km and 56.4 km over different
    roads. It shares row two's right column with the cancel key, because a key
    the page does not advertise is a key the rider has no way to know about
    (DESIGN.md 2) -- and the mode has to be togglable HERE, where changing it
    rebuilds the route and the change is visible in the figures on the left.
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
    # T and the toll answer, right-aligned in the title row (DESIGN.md 7.7.1).
    # The badge is `T TOLL?` here because a mockup has no router: UNKNOWN is
    # what a GPX and an OSRM reply also report, so this frozen frame is the
    # not-told case. The key rides WITH the badge rather than on the strip --
    # 7.7 built that strip for four half-lines, and T TOLL beside M BIKE
    # measured into the turn count at 128 turns.
    toll = "T TOLL?"
    text(c, 6, 5, f"TO {rt['dest']}"[:33 - len(toll) - 2], 2)
    rtext(c, W - 6, 5, toll, 2)

    total = sum(math.dist(a, b) for a, b in zip(pts, pts[1:]))
    mins = max(1, int(round(total / 1000 / est_kmh * 60)))
    c.rect(0, H - strip, W - 1, H - 1, INK)
    dtxt = f"{total/1000:.1f}KM" if total >= 950 else f"{total:.0f}M"
    text(c, 6, H - strip + 5, f"{dtxt}  EST {mins} MIN", 2, PAPER)
    text(c, 6, H - strip + 23, f"{len(rt['cues']) - 1} TURNS", 2, PAPER)
    rtext(c, W - 6, H - strip + 5, "ENTER = GO", 2, PAPER)
    rtext(c, W - 6, H - strip + 23, f"M {mode}  Q CANCEL", 2, PAPER)
    return resolve(c.img)


def page_search():
    osm = load_osm()
    if osm:
        finish(render_search("SOI 23", osm[1]["pts"][0]), "nav-search")


def page_confirm():
    osm = load_osm()
    if osm:
        finish(render_confirm(osm[1]), "nav-confirm")


# The SAVE page's own copy of the MAP page's coordinate. Defined here rather than
# read from MAP_LAT/MAP_LON below because this function is above them in the file
# and moving either would reorder a section for no reason -- and the two being
# the same digits is the point: the row this page shows is the row nav-map.png
# shows, so a build that formatted one differently could not pass both frames.
SAVE_LAT, SAVE_LON = 13.88510, 100.37850
SAVE_NPLACE, SAVE_MAX = 3, 8


def page_save(name, typed=False):
    """
    DESIGN.md 1.4.8: the name of a favourite being typed, over the coordinate it
    is about to be written against.

    FIND's field inside QUIT's frame, and every constant below is one of theirs --
    the 26 px title bar, the query's cap-top at 34, the 12x22 block cursor 14 px
    in, the 42 px strip. This page invented no geometry, which is exactly why it
    can be byte-compared in full: it is text and block fills all the way down,
    with no coverage shape anywhere on it.

    `typed` picks the field's second state. Unedited, the default name is a
    SELECTION -- an ink bar with the name in paper and no cursor -- because the
    first character typed replaces the whole field, and inverted-means-
    replaceable is the one convention a 1-bit panel has for saying so before it
    happens.
    """
    c = Canvas()
    nm = "CAFE" if typed else "PLACE 4"
    c.rect(0, 0, W - 1, 25, INK)
    text(c, 6, 5, "SAVE PLACE", 2, PAPER)
    rtext(c, W - 6, 5, f"{SAVE_NPLACE} OF {SAVE_MAX}", 2, PAPER)

    # The pen advance and not the ink box, for num_pen()'s own reason.
    qw = num_pen(c, nm, QUERY_CAP)
    if typed:
        num(c, 8, 34, nm, QUERY_CAP, INK)
        c.rect(14 + qw, 36, 14 + qw + 12, 58, INK)      # FIND's block cursor
    else:
        c.rect(8 - 4, 36 - 4, 8 + qw + 4, 58 + 4, INK)  # the selection
        num(c, 8, 34, nm, QUERY_CAP, PAPER)

    # Where. The same five decimals and the same single space the MAP strip uses,
    # because this is the row the rider just read there -- and it is what the
    # places file gets written with, so screen, page and file all say one thing.
    # At scale 3: it is the SUBJECT of this page, not a status row.
    text(c, 8, 74, f"{SAVE_LAT:.5f} {SAVE_LON:.5f}", 3)

    strip = 42
    c.rect(0, H - strip, W - 1, H - 1, INK)
    text(c, 6, H - strip + 5, "ENTER = SAVE", 2, PAPER)
    rtext(c, W - 6, H - strip + 5, "ESC = CANCEL", 2, PAPER)
    # The instruction, on the row the refusals of 1.4.8 displace when there is
    # one. Esc and not Q, because on this page Q types a Q.
    text(c, 6, H - strip + 23, "TYPE A NAME", 2, PAPER)
    finish(resolve(c.img), name)


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


def turn_panel(c, off=None, nofix=False):
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

    # Below the rule, three centred rows: how long is left, how far is left,
    # and when you arrive. This displaced the next-cue preview; the cue after
    # the announced one is still marked by the teardrop on the map, so what
    # went is the preview, not the information.
    #
    # Three rows and not one "bottom line", because both values cannot share
    # a line at a readable size: the budget is ten characters (12 px each on
    # a 128 px panel) and "12.6KM 1:42PM" is thirteen.
    c.rect(6, H - 51, PANEL_W - 7, H - 51, PAPER)

    def row(y, s):
        if s:
            text(c, (PANEL_W - tw(s, 2)) // 2, y, s, 2, PAPER)

    row(192, NAV["remain"])
    togo = NAV["togo_m"]
    if togo < 1000:
        row(208, f"{int(togo) - int(togo) % 50}M")
    else:
        row(208, f"{math.floor(togo / 100) / 10:.1f}KM")

    # DESIGN.md 1.1: "GPS state earns panel space only when it is a problem --
    # NO FIX replaces the bottom row, inverted, when the fix is lost." A paper
    # bar with ink text, in the arrival row's own 16 px cell: the panel is
    # solid ink, so a bar of paper is the one treatment here that cannot be
    # read as ordinary content, and there is no spare line to put it on.
    if nofix:
        c.rect(0, H - 16, PANEL_W - 1, H - 1, PAPER)
        text(c, (PANEL_W - tw("NO FIX", 2)) // 2, H - 16, "NO FIX", 2, INK)
    else:
        row(224, NAV["eta_txt"])


def page_nav(name, basemap=False, off=None, course_up=True, dither=False,
             route=None, streets=None, nofix=False, view3d=False):
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

    if view3d:
        # DESIGN.md 6.6. The 3D branch replaces the basemap, the track AND the
        # route, exactly as view_nav.c's does: the route has to go through the
        # same camera as the roads under it, and a flat route line over a
        # tilted map would be a picture of two different places.
        pack = roads_pack()
        if pack is not None:
            _en, _adj, _edge, plat0, plon0, pklat, pke = pack
            # The route, moved into the ROADS pack's frame. mockup's own world
            # frame is the junction origin (osm_frame()), so this is the same
            # conversion nav.c does with geo_unproject() + roads_project().
            # THE FRAME, and the one thing here that is not a transcription.
            # view_nav.c's ASOK_ROUTE is referenced to its own FIRST POINT,
            # which is where the pack's (lat0, lon0) lands -- so route metres
            # and pack metres coincide there with no offset. These coordinates
            # are referenced to the computed junction instead (osm_frame()), so
            # reaching the pack's frame means subtracting the route's own start.
            # That offset is invisible on every 2D page, where project() works
            # relative to the fix, and it is exactly what has to be right in 3D,
            # which converts absolutely.
            m0e, m0n = pts[0]
            r3d = [(pe - m0e, pn - m0n)
                   for (pe, pn) in [on_route] + pts[pos_i + 1:]]
            rpe, rpn = pos[0] - m0e, pos[1] - m0n
            rlat = plat0 + rpn / pklat
            rlon = plon0 + rpe / pke
            ko = ((15.0, 21.0, 27.0),                      # compass
                  (31.0, float(W - 33 - MAP_X), 33.0),     # speed badge
                  (0.0, 2.0, float(H - 26), 80.0, float(H - 1)))  # scale bar
            draw_nav3d(c, rlat, rlon, theta, mpp, r3d, ko)
        c.flush_hairlines()
    else:
        if basemap:
            draw_streets(c, pos, mpp, cx, cy, theta, box, streets)

    scr = project(pts, pos, mpp, cx, cy, theta)          # markers, unrounded
    track_w = pts[:pos_i + 1]
    if off:
        track_w = track_w + [lerp(pts[pos_i], on_route, 0.55)]
    if not view3d:
        dashed(c, clip_poly(project(round_corners(track_w + [pos]),
                                    pos, mpp, cx, cy, theta), box), width=1)

        c.flush_hairlines()      # streets and track land under the route casing
        cased_route(c, clip_poly(project(round_corners([on_route] + pts[pos_i + 1:]),
                                         pos, mpp, cx, cy, theta), box),
                    outer=10, inner=6)

    if view3d:
        # Neither the off-route tie line nor the pin is drawn in 3D, and that
        # matches view_nav.c: both are marks placed by the FLAT projection, and
        # a flat mark dropped onto a tilted map points at the wrong ground. The
        # C wraps them in the same else branch this does. Leaving the pin in was
        # worth 1949 differing pixels against the C page -- balanced between the
        # two sides, which is the signature of a mark in the wrong place rather
        # than a rasteriser that disagrees.
        pass
    elif off:
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
    turn_panel(c, off, nofix)
    NAV["turn"], NAV["then_d"] = saved
    c.rect(PANEL_W, 0, PANEL_W, H - 1, INK)
    finish(resolve(c.img, dither), name)


def page_nav3d(name="nav-3d"):
    """The frozen 3D state (DESIGN.md 6.6), matched to view_nav_3d_demo().

    Three overrides on the OSM route, and each one is a fixture decision the C
    side makes too:
      pos_i 2   -- two vertices in, so there is road behind the fix as well as
                   ahead; view_nav_3d_demo() uses the same index.
      turn 410  -- the DESIGN's sample panel, quantised to 400, rather than the
                   tiles demo's 420. The panel is not what this page tests, so
                   it should be the one every other frozen page agrees on.
      then_d    -- likewise.
    Returns None when osm-asok.json is absent, exactly as the other OSM pages
    do, so a checkout without the extract still renders everything else."""
    osm = load_osm()
    if osm is None:
        return None
    r = dict(osm[1])
    r["pos_i"] = 2
    r["pos_f"] = 0.0
    r["turn"] = 410
    r["then_d"] = "150M"
    return page_nav(name, view3d=True, route=r)


# ------------------------------------------------------------- MAP (DESIGN 1.5)
# The same cartography as page_nav()'s map half, full width, with no route: open
# the program and see where you are. Every mark is the one page_nav() uses --
# there is only one set of them, and a second copy would be a second thing to
# keep in step with this file.
#
# Two things are deliberately different, and both follow from there being no
# route: the fix sits at the CENTRE rather than at 0.72 of the height (the 0.72
# bias exists to show the road ahead, and with no route there is no ahead), and
# the strip along the bottom carries where you are and the keys that get you out
# of here instead of five progress figures there is no route to have.
MAP_STRIP = 42                  # with a position: coordinates + hints
MAP_STRIP_WAIT = 24             # without one: the hints alone
MAP_MPP = 6.0                   # the default rung; Z/X move it, A comes back
MAP_TRACK_N = 4                 # ROUTE_M[:4] read as a breadcrumb, not a route
MAP_LAT, MAP_LON = 13.88510, 100.37850      # the device's own desk
MAP_SATS = 9
# Four keys now (S SAVE, DESIGN.md 1.4.8), so the separators went from three
# spaces to two: 32 characters at scale 2 is 384 px of the 400 and 33 would not
# fit. S had to go on this row rather than being left off it -- 1.5 makes the row
# the page's whole advertisement, and a key absent from it was never claimed.
MAP_HINTS = "F FIND  R ROUTES  S SAVE  Q QUIT"
# 21 px from the edge is where page_nav() puts the compass, but that page's map
# starts at x 130 with a panel to its left. Here the needle's 21.6 px reach would
# leave the frame whenever the rotation points it at the margin, so the badge
# sits four pixels further in and nothing else moves.
MAP_COMPASS_X = 25


def map_strip(c, pos_txt, nofix=False, note=None):
    """
    The MAP page's inverted bottom strip. Two scale-2 rows when there is a
    position -- where you are, then the keys -- and the hint row alone when
    there is not. The hint sits at the same y either way (H - 19): the strip
    GROWS a row when there is something to put in it, so the one line a rider
    navigates by never moves.

    Nothing here is below scale 2, which is the panel's measured floor and the
    reason chooser.py's own hint was enlarged.
    """
    strip = MAP_STRIP if pos_txt else MAP_STRIP_WAIT
    c.rect(0, H - strip, W - 1, H - 1, INK)
    if pos_txt:
        # A transient confirmation displaces the coordinates for a second and a
        # half, by exactly the argument turn_panel() makes for arrival: they
        # change slowly, and they are not what the rider pressed a key about.
        text(c, 6, H - strip + 5, note or pos_txt, 2, PAPER)
        if nofix:
            # DESIGN.md 1.1.2's treatment, in the one row this page has for it.
            # The strip is solid ink, so a bar of paper is again the only
            # emphasis available -- and it sits BESIDE the coordinates rather
            # than over them, because a frozen position is still the last thing
            # known and worth reading. That is the difference from the panel,
            # where the row it takes had a competing value in it.
            wd = tw("NO FIX", 2)
            c.rect(W - 12 - wd, H - strip + 3, W - 4, H - strip + 20, PAPER)
            text(c, W - 8 - wd, H - strip + 5, "NO FIX", 2, INK)
    text(c, 6, H - 19, MAP_HINTS, 2, PAPER)


def page_map(name, nofix=False, streets=None, note=None):
    c = Canvas()
    # ROUTE_M's first four points read as a track already travelled rather than
    # as a route to follow. There is no route on this page, and a synthetic
    # trace is the only honest fixture for one.
    trk = ROUTE_M[:MAP_TRACK_N]
    pos = ROUTE_M[MAP_TRACK_N]
    # The direction the track ran INTO the fix. page_nav() takes its fallback
    # rotation from the route ahead of the fix; there is no ahead here, so it
    # comes from the leg just ridden -- which is what a receiver's smoothed
    # course would be reporting anyway, and a mockup has no receiver.
    heading = math.atan2(pos[0] - trk[-1][0], pos[1] - trk[-1][1])
    theta = heading                      # course-up; O toggles it, as on NAV
    mpp = MAP_MPP
    cx = W / 2
    cy = (H - MAP_STRIP) / 2             # the CENTRE of the map, not 0.72 of it
    box = (0, 0, W - 1, H - MAP_STRIP - 1)

    if streets:
        draw_streets(c, pos, mpp, cx, cy, theta, box, streets)
    # The breadcrumb, dashed exactly as page_nav() dashes its ridden track --
    # and NOT corner-rounded, which that one is: a GPX is a chain of survey
    # chords and reads as a polygon drawn raw, while a GPS trace is already a
    # dense wander and rounding it would invent bends the rider did not take.
    dashed(c, clip_poly(project(trk + [pos], pos, mpp, cx, cy, theta), box),
           width=1)
    c.flush_hairlines()

    position_marker(c, cx, cy)
    compass(c, MAP_COMPASS_X, 27, theta)
    speed_badge(c, W - 33, 33, NAV["spd"])
    scale_bar(c, 7, H - MAP_STRIP - 6, mpp)
    map_strip(c, f"{MAP_LAT:.5f} {MAP_LON:.5f}", nofix, note)
    finish(resolve(c.img), name)


def page_map_wait(name, sats=MAP_SATS):
    """
    Before the first fix. There is nothing to centre a map on, so none of it is
    drawn: no marker, no compass, no scale bar, no streets. A map drawn about a
    guessed position is the one thing a navigator must never put on a screen,
    and an empty frame that says what it is waiting for is not a lesser page --
    it is the only honest one.
    """
    c = Canvas()
    cy = (H - MAP_STRIP_WAIT) / 2
    s = "WAITING FOR FIX"
    text(c, (W - tw(s, 3)) // 2, cy - 24, s, 3)
    # The satellite count only when the receiver is talking at all. The line
    # above does not move when it appears: a message that jumps as the receiver
    # warms up reads as a glitch rather than as progress.
    if sats >= 0:
        t = "1 SATELLITE" if sats == 1 else f"{sats} SATELLITES"
        text(c, (W - tw(t, 2)) // 2, cy + 10, t, 2)
    map_strip(c, None)
    finish(resolve(c.img), name)


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
        page_nav3d()
    page_nav("nav-turn-nomap")
    page_nav("nav-turn-off", basemap=True, off=85)
    page_map("nav-map")
    page_map("nav-map-nofix", nofix=True)
    page_save("nav-save")
    page_save("nav-save-typed", typed=True)
    page_map_wait("nav-map-wait")
    page_overview()
    page_arrows()
    page_smooth()

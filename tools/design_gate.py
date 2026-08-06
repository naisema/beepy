#!/usr/bin/env python3
"""
design_gate.py -- the M2 acceptance gate: C frames vs the design mockups.

Renders the reference frames straight out of beepy-nav/mockup.py (no PNG
round trip, no hand-copied numbers), renders the same pages with the C
binary, and compares them with fbdiff.py.

    python3 tools/design_gate.py [--bin host/beepy-nav] [--out DIR]

Acceptance, per page:
  * ZERO differing pixels anywhere that is exact by construction -- the
    whole turn panel (x < 130), the 5x7 text, the numeral blits, the
    hairline dashes and the scale bar. These are blitted or block-filled,
    so "close" would mean a bug.
  * fewer than 480 differing pixels overall (0.5% of the frame), and
  * every remaining difference sitting on an ink/paper edge in BOTH frames
    (--edges-only): shape-boundary drift in the coverage shapes, which is
    where cover.c's per-shape composite differs from Pillow's single 4x
    buffer, and never a missing or displaced shape.

Pixels the MOCKUP itself resolved at exactly 50 % coverage are exempt, and
the exemption is computed here rather than hand-listed: this script has the
reference's pre-resolve coverage buffer, so it writes out every coordinate
whose downsampled value is exactly 128 and hands the list to fbdiff as
--mask-list. Those pixels were decided by resolve()'s own `>= 128`, and a
hair of geometry either way flips them -- they cannot distinguish a correct
renderer from a displaced shape, which is the only thing --edges-only is
trying to see. The count is printed so the exemption stays visible, and it
is 0, 1 or 2 on every page as this is written -- the exemption exists
because such a pixel cannot tell a correct renderer from a displaced one,
not because any page needs it to pass.

The first bullet is checked by running fbdiff twice: once unmasked for the
total, once with the page's EXACT region masked. Differences inside that
region are the masked count, and the gate requires it to be zero -- the mask
is used to measure the region, not to excuse it.

For the NAV pages the exact region is the turn panel (`--mask panel`, x < 130).
The MAP page of DESIGN.md 1.5 has no panel; its exact-by-construction region is
the inverted bottom strip, which is a block fill plus 5x7 text and nothing else.
So the region is a per-page property, and the fifth element of each PAGES entry
is the fbdiff mask that names it.

Needs Pillow, so this is a Mac-lane target: `make design-gate`.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NAVDIR = os.path.join(ROOT, "beepy-nav")
W, H = 400, 240
BLACK = b"\x00\x00\x00\xff"
WHITE = b"\xff\xff\xff\xff"

MAX_PX = 480          # 0.5% of the frame
PANEL_MAX = 0         # exact-by-construction: the panel must be identical

# The road pack the FIND page searches (DESIGN.md 1.4). Committed, like the
# tile fixture, and built by tools/mkpack.py from the same extract mockup.py
# reads -- so the two sides of the gate are searching the same city.
ROADS = os.path.join(NAVDIR, "tests", "roads", "asok.roads")

# The exact-by-construction regions, as fbdiff masks. "panel" is fbdiff's own
# name for x < 130; the MAP strip is given as a rectangle because it is a region
# only this gate cares about.
EXACT_PANEL = ("panel",)
EXACT_MAP_STRIP = ("0,198,399,239",)       # MAP with a position: the 42 px strip
EXACT_MAP_WAIT = ("0,216,399,239",)        # and without one: the hint row alone
# The SAVE page of DESIGN.md 1.4.8 is text and block fills END TO END -- a title
# bar, a 24 px glyph table, a scale-3 coordinate, two strip rows and one
# rectangle. There is no coverage shape anywhere on it, so the exact-by-
# construction region is the WHOLE FRAME and this is the strictest entry in the
# gate: not "within 480 pixels", but identical.
EXACT_ALL = ("0,0,399,239",)

# page name -> how mockup.py renders it, the exact-by-construction region, and
# any extra flags the C binary needs.
# nav-turn-off is rendered here WITHOUT the synthetic basemap, because
# view_nav.c draws route and track only (DESIGN.md phase 2); the streets layer
# arrives with the tile work.
PAGES = (
    ("nav", "nav", lambda m, n: m.page_nav(n, basemap=False), EXACT_PANEL, ()),
    ("nav-off", "nav-off", lambda m, n: m.page_nav(n, basemap=False, off=85),
     EXACT_PANEL, ()),
    # The NO FIX state of DESIGN.md 1.1: the same turn page with the bottom
    # row inverted. It earns a gate entry of its own because the inversion is
    # the only place on this screen where the panel's polarity flips, and a
    # renderer that got it a pixel out would still "look right".
    ("nav-nofix", "nav-nofix",
     lambda m, n: m.page_nav(n, basemap=False, nofix=True), EXACT_PANEL, ()),
    # The MAP page of DESIGN.md 1.5, in all three of its states. It is the same
    # cartography as the NAV map in a different frame, so the gate is what says
    # the frame moved and the marks did not; and the two states that are not the
    # ordinary case are exactly the ones a renderer can get wrong while looking
    # right -- an inverted NO FIX a pixel out, and a map that should not be
    # there at all.
    ("map", "nav-map", lambda m, n: m.page_map(n), EXACT_MAP_STRIP, ()),
    ("map-nofix", "nav-map-nofix", lambda m, n: m.page_map(n, nofix=True),
     EXACT_MAP_STRIP, ()),
    ("map-wait", "nav-map-wait", lambda m, n: m.page_map_wait(n),
     EXACT_MAP_WAIT, ()),
    ("overview", "nav-overview", lambda m, n: m.page_overview(), EXACT_PANEL,
     ()),
    ("arrows", "nav-arrows", lambda m, n: m.page_arrows(), EXACT_PANEL, ()),
    # The 3D view of DESIGN.md 6.6, and the FIRST entry in this gate whose MAP
    # CONTENT is compared at all. Every other page passes basemap=False and
    # nav-turn-osm is not here, because a raster basemap cannot be reproduced
    # from osm-asok.json -- the two sides would be two renderers of two
    # different inputs. This one can be: mockup.py reads the same .roads pack
    # the C does, so the only thing left to disagree about is the drawing.
    #
    # It is also the first entry to spend any of MAX_PX. It differs by THREE
    # pixels, a vertical run at one fill-span boundary, where every other page
    # spends zero. That is worth stating rather than burying: the road plane is
    # byte-identical in isolation -- 426 ribbons, 3904 ink px, 0 differing of
    # 64 800 -- the panel here is exact, and these three survive coordinate
    # quantisation to 1/256 px on both the quad corners and the interpolated
    # span ends. So they are NOT an ulp at a rounding boundary, and they are
    # unfinished business rather than an accepted cost.
    ("nav-3d", "nav-3d", lambda m, n: m.page_nav3d(n), EXACT_PANEL,
     ("--view3d", "--roads", ROADS)),
    # M6. FIND is here because its 24 px query became a generated glyph table
    # (tools/gen_query.py) rather than a live TrueType render: the device has no
    # rasterizer, so the only way "24 px bold" could survive the port was to
    # make mockup and panel blit the same bitmaps -- and the moment they do, the
    # page can be byte-compared like every other. It is also the one gate entry
    # that checks a SEARCH: the row and its "464M NE" are computed from the
    # committed pack on both sides, not drawn from a fixture.
    ("find", "nav-search", lambda m, n: m.page_search(), EXACT_PANEL,
     ("--roads", ROADS)),
    ("confirm", "nav-confirm", lambda m, n: m.page_confirm(), EXACT_PANEL, ()),
    # SAVE (DESIGN.md 1.4.8), in both states of its field. Two entries because
    # the difference between them IS the feature -- a selected default that the
    # first keystroke replaces, against an edited name with a caret -- and a
    # renderer that drew the second for both would look perfectly reasonable.
    ("save", "nav-save", lambda m, n: m.page_save(n), EXACT_ALL, ()),
    ("save-typed", "nav-save-typed",
     lambda m, n: m.page_save(n, typed=True), EXACT_ALL, ()),
)


def references(outdir):
    """Render mockup.py's own frames into raw .fb files. -> {name: (fb, tie)}"""
    sys.path.insert(0, NAVDIR)
    cwd = os.getcwd()
    os.chdir(NAVDIR)                      # mockup.py reads ../ and src/ by path
    try:
        from PIL import Image
        import mockup
        grabbed, covs = {}, {}
        mockup.finish = lambda img, name: grabbed.setdefault(name, img.copy())
        # Tap resolve() for the coverage buffer on its way to the threshold.
        plain = mockup.resolve

        def spy(cov, dither=False):
            covs["last"] = cov
            return plain(cov, dither)

        mockup.resolve = spy
        for page, mockup_name, render, _exact, _flags in PAGES:
            render(mockup, mockup_name)
            img = grabbed[mockup_name]
            small = covs["last"].resize((W, H), Image.BOX).load()
            tie = os.path.join(outdir, f"tie-{page}.txt")
            with open(tie, "w") as fh:
                fh.write("# reference pixels at exactly 50%% coverage\n")
                for y in range(H):
                    for x in range(W):
                        if small[x, y] == 128:
                            fh.write(f"{x} {y}\n")
            grabbed[page + "!tie"] = tie
            px = img.convert("1").load()
            out = bytearray()
            for y in range(H):
                for x in range(W):
                    out += WHITE if px[x, y] else BLACK
            assert len(out) == W * H * 4
            path = os.path.join(outdir, f"ref-{page}.fb")
            open(path, "wb").write(out)
            grabbed[page] = path
        return {p: (grabbed[p], grabbed[p + "!tie"]) for p, _, _, _, _ in PAGES}
    finally:
        os.chdir(cwd)


def fbdiff(a, b, *extra):
    """-> (differing, hidden-by-mask, edges-only failures, ok)"""
    cmd = [sys.executable, os.path.join(ROOT, "tools", "fbdiff.py"), a, b]
    r = subprocess.run(cmd + list(extra), capture_output=True, text=True)
    line = r.stdout.strip()
    n = lambda key, dflt=0: next(
        (int(w.split()[0]) for w in line.split(", ") if key in w), dflt)
    diff = int(line.split(":")[1].split()[0])
    return (diff, n("inside masks"), n("at-threshold"), n("fail edges-only"),
            r.returncode == 0, line)


def main(argv):
    binary = os.path.join(ROOT, "host", "beepy-nav")
    outdir = "/tmp/beepy-nav-gate"
    it = iter(argv)
    for a in it:
        if a == "--bin":
            binary = next(it)
        elif a == "--out":
            outdir = next(it)
        else:
            raise SystemExit(__doc__.strip())
    os.makedirs(outdir, exist_ok=True)

    refs = references(outdir)
    bad = 0
    for page, _, _, exact, flags in PAGES:
        ref, tie = refs[page]
        got = os.path.join(outdir, f"c-{page}.fb")
        subprocess.run([binary, "--demo", "--page", page, "--dump", got,
                        *flags], check=True, capture_output=True)
        masks = [a for m in exact for a in ("--mask", m)]
        total, _, tied, _, _, _ = fbdiff(got, ref, "--mask-list", tie,
                                         "--max-px", str(MAX_PX))
        _, panel, _, nonedge, ok, _ = fbdiff(
            got, ref, *masks, "--mask-list", tie,
            "--max-px", str(MAX_PX), "--edges-only",
            "--out", os.path.join(outdir, f"diff-{page}.png"))
        verdict = (total <= MAX_PX and panel <= PANEL_MAX and nonedge == 0
                   and ok)
        bad += not verdict
        print(f"{page:9s} total {total:4d} / {MAX_PX}   exact {panel:4d} / "
              f"{PANEL_MAX}   non-edge {nonedge:4d}   tied {tied:3d}   "
              f"{'PASS' if verdict else 'FAIL'}")
    print("design gate:", "PASS" if not bad else f"FAIL ({bad} pages)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

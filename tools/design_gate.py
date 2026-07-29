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
trying to see. The count is printed so the exemption stays visible; it is
84 pixels on the OVERVIEW page and 0 on the M2 pages.

The first bullet is checked by running fbdiff twice: once unmasked for the
total, once with --mask panel. Panel differences are the masked count, and
the gate requires it to be zero -- the mask is used to measure the region,
not to excuse it.

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

# page name -> how mockup.py renders it. nav-turn-off is rendered here
# WITHOUT the synthetic basemap, because view_nav.c draws route and track
# only (DESIGN.md phase 2); the streets layer arrives with the tile work.
PAGES = (
    ("nav", "nav", lambda m, n: m.page_nav(n, basemap=False)),
    ("nav-off", "nav-off", lambda m, n: m.page_nav(n, basemap=False, off=85)),
    ("overview", "nav-overview", lambda m, n: m.page_overview()),
    ("arrows", "nav-arrows", lambda m, n: m.page_arrows()),
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
        for page, mockup_name, render in PAGES:
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
        return {p: (grabbed[p], grabbed[p + "!tie"]) for p, _, _ in PAGES}
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
    for page, _, _ in PAGES:
        ref, tie = refs[page]
        got = os.path.join(outdir, f"c-{page}.fb")
        subprocess.run([binary, "--demo", "--page", page, "--dump", got],
                       check=True, capture_output=True)
        total, _, tied, _, _, _ = fbdiff(got, ref, "--mask-list", tie,
                                         "--max-px", str(MAX_PX))
        _, panel, _, nonedge, ok, _ = fbdiff(
            got, ref, "--mask", "panel", "--mask-list", tie,
            "--max-px", str(MAX_PX), "--edges-only",
            "--out", os.path.join(outdir, f"diff-{page}.png"))
        verdict = (total <= MAX_PX and panel <= PANEL_MAX and nonedge == 0
                   and ok)
        bad += not verdict
        print(f"{page:9s} total {total:4d} / {MAX_PX}   panel {panel:4d} / "
              f"{PANEL_MAX}   non-edge {nonedge:4d}   tied {tied:3d}   "
              f"{'PASS' if verdict else 'FAIL'}")
    print("design gate:", "PASS" if not bad else f"FAIL ({bad} pages)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

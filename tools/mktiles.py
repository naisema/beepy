#!/usr/bin/env python3
"""
mktiles.py -- build a 1-bit OSM basemap tile pack for beepy-nav (DESIGN.md 6.5).

    python3 tools/mktiles.py --osm EXTRACT.json --route ROUTE.gpx -o PACK.tiles
                             [--corridor M] [--zooms LIST] [--quiet]
    python3 tools/mktiles.py --info PACK.tiles

Mac-side only: it wants Pillow, and the device has neither Pillow nor a reason
to render anything it can read pre-rendered. The output is the one artefact the
device sees, and beepy-nav/src/tile.c is the only thing that reads it.

WHAT IT DOES

  1. Projects the GPX onto the local tangent plane of DESIGN.md 6.1, referenced
     to the route's FIRST point -- the same rule route.c's route_load() uses, so
     a pack built from a route and that route agree on coordinates exactly.
  2. Cuts the Overpass extract to a corridor: every way with a vertex within
     --corridor metres of the route polyline (2 km by default, DESIGN.md 6.5).
  3. Classifies each surviving way into mockup.py's two weights -- 2 px for
     motorway/trunk/primary/secondary, 1 px for everything else -- and drops
     the highway values that are not roads at all (footway, steps, service...).
  4. Rasterizes them, aliased, into 256x256 1-bit tiles, one grid per zoom rung.

DETERMINISM is a requirement, not a nicety: the tests build the pack twice and
compare sha256. Nothing here reads the clock, the environment or a hash seed;
ways are visited in extract order; every coordinate is floored to an integer
pack pixel ONCE, globally, before any tile is drawn. That last point is also
what makes the seams invisible -- a way crossing a tile boundary is drawn from
the same integer endpoints on both sides, and Bresenham is translation
invariant under integer offsets, so the two halves meet exactly.

------------------------------------------------------------------ PACK FORMAT

Little-endian throughout, fixed-width fields, no implicit padding: every
structure below is a whole number of its own largest field and the offsets are
written out. The device freads it; it never parses text.

  HEADER, 64 bytes at offset 0
    off  size  type    field
      0     8  char[8] magic, "BNAVTILE" (no NUL)
      8     2  u16     version, 1
     10     2  u16     header_bytes, 64 -- the zoom table starts here
     12     2  u16     tile_w, 256
     14     2  u16     tile_h, 256
     16     2  u16     nzoom
     18     2  u16     flags; bit 0 = rows are MSB-first, 1 = ink (always set)
     20     8  f64     lat0, projection reference, degrees
     28     8  f64     lon0
     36     8  f64     m_per_deg_lat, 110540.0
     44     8  f64     m_per_deg_lon, 111320.0 (multiplied by cos(lat0) in use)
     52     8  f64     corridor_m, what was cut -- informational
     60     4  u32     ntiles, non-blank tiles in the file

  ZOOM TABLE, nzoom entries of 32 bytes at offset header_bytes
      0     8  f64     mpp, metres per pixel -- a rung of MAP_ZOOMS
      8     4  i32     tx0, tile-x index of grid column 0
     12     4  i32     ty0, tile-y index of grid row 0
     16     4  u32     nx
     20     4  u32     ny
     24     4  u32     index_off, file offset of this zoom's index
     28     4  u32     reserved, 0

  INDEX, one per zoom: nx*ny u32 file offsets, row-major (x fastest).
  0 means the tile is blank and no bytes were written for it.

  TILE DATA: tile_h rows of (tile_w+7)/8 bytes, MSB first, bit set = ink.
  256x256 is 8192 bytes, DESIGN.md 6.5's "8 KB per tile".

  PROJECTION. A world point (lat, lon) is metres east/north of (lat0, lon0):

      e = (lon - lon0) * m_per_deg_lon * cos(lat0)
      n = (lat - lat0) * m_per_deg_lat

  and the pack-pixel address at a zoom of `mpp` metres per pixel is

      px = floor(e / mpp)      py = floor(-n / mpp)

  -- y down, so py increases southward and a tile's row 0 is its northern
  edge. Tile (tx, ty) holds px in [tx*256, tx*256+256) and py likewise; pixel
  (col, row) inside it is px = tx*256 + col, py = ty*256 + row.

  A reader that wants a pack in a DIFFERENT tangent frame (the live navigator,
  whose frame is referenced to its route's first point) converts by translation:
  project the pack's (lat0, lon0) into the reader's frame and subtract. The two
  frames differ in scale by cos(lat0_pack)/cos(lat0_reader), which is exactly 1
  when the pack was built from that route and is under a centimetre per
  kilometre for any pack reused within a corridor's width of its own origin.

------------------------------------------------------------------------------
"""
import argparse
import hashlib
import json
import math
import os
import re
import struct
import sys

MAGIC = b"BNAVTILE"
VERSION = 1
HEADER_BYTES = 64
ZOOM_ENTRY = 32
FLAG_MSB_INK = 1

# DESIGN.md 6.1's ladder, metres per pixel.
MAP_ZOOMS = (1.5, 2.5, 4, 6, 10, 15, 25, 40, 60, 100, 150, 250)

# route.h's constants, so the projection is bit-for-bit the navigator's.
M_PER_DEG_LAT = 110540.0
M_PER_DEG_LON = 111320.0

# The NAV map is x 130..399 (DESIGN.md 1.1) -- 270 px of visible width. The
# default zoom set is every rung whose visible width still fits inside the
# corridor the pack was cut to; see pick_zooms().
MAP_W = 270

# mockup.py's OSM_WEIGHT, verbatim. Anything else that IS a road gets 1.
OSM_WEIGHT = {"motorway": 2, "trunk": 2, "primary": 2, "secondary": 2,
              "tertiary": 1, "unclassified": 1, "residential": 1,
              "living_street": 1}

# Tagged `highway` but not a road: at 4 m/px a car park aisle and a footbridge
# are the same one-pixel line as the street they hang off, and a city's worth
# of them turns the basemap into felt. The Asok extract contains none of these,
# so this changes nothing there -- it is what keeps a wider extract usable.
NOT_A_ROAD = {"footway", "path", "steps", "cycleway", "pedestrian", "bridleway",
              "track", "corridor", "platform", "proposed", "construction",
              "raceway", "bus_guideway", "escape", "service", "elevator",
              "rest_area", "services", "busway", "via_ferrata"}

TILE_W = TILE_H = 256


# --------------------------------------------------------------------- input

def read_gpx(path):
    """-> [(lat, lon), ...] in file order. The same tolerant scan gpx.c does:
    trkpt, rtept and wpt all count, and nothing else in the file matters."""
    text = open(path, "r", encoding="utf-8", errors="replace").read()
    pts = []
    for m in re.finditer(r"<(?:trk|rte|w)pt\b([^>]*)>", text):
        attrs = m.group(1)
        la = re.search(r'lat\s*=\s*"([^"]+)"', attrs)
        lo = re.search(r'lon\s*=\s*"([^"]+)"', attrs)
        if la and lo:
            pts.append((float(la.group(1)), float(lo.group(1))))
    if len(pts) < 2:
        raise SystemExit(f"mktiles: {path}: fewer than two points")
    return pts


def read_osm(path):
    """-> [(name, weight, [(lat, lon), ...]), ...] for every drawable way."""
    doc = json.load(open(path, "r", encoding="utf-8"))
    out = []
    for el in doc.get("elements", ()):
        if el.get("type") != "way" or "geometry" not in el:
            continue
        hw = el.get("tags", {}).get("highway")
        if not hw or hw in NOT_A_ROAD:
            continue
        geom = [(p["lat"], p["lon"]) for p in el["geometry"]]
        if len(geom) < 2:
            continue
        out.append((el.get("tags", {}).get("name", ""), OSM_WEIGHT.get(hw, 1),
                    geom))
    return out


# ---------------------------------------------------------------- projection

class Frame:
    """DESIGN.md 6.1's local tangent plane, referenced to (lat0, lon0)."""

    def __init__(self, lat0, lon0):
        self.lat0 = lat0
        self.lon0 = lon0
        self.ke = M_PER_DEG_LON * math.cos(math.radians(lat0))
        self.kn = M_PER_DEG_LAT

    def met(self, lat, lon):
        return ((lon - self.lon0) * self.ke, (lat - self.lat0) * self.kn)


# ------------------------------------------------------------- corridor cut

class Corridor:
    """Point-to-polyline distance test, gridded so a large extract does not
    become O(ways x segments). Cell size is the corridor width, so the answer
    for a point is always inside its own cell's 3x3 neighbourhood."""

    def __init__(self, route_en, width):
        self.w = width
        self.cell = max(width, 1.0)
        self.grid = {}
        for a, b in zip(route_en, route_en[1:]):
            # Walk the segment in half-cell steps and register it in every
            # cell it passes through: a 3 km leg must not be filed under its
            # endpoints alone.
            seg = math.dist(a, b)
            steps = max(1, int(seg / (self.cell / 2.0)) + 1)
            for s in range(steps + 1):
                t = s / steps
                p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
                self.grid.setdefault(self._key(p), []).append((a, b))

    def _key(self, p):
        return (int(math.floor(p[0] / self.cell)),
                int(math.floor(p[1] / self.cell)))

    @staticmethod
    def _seg_dist(p, a, b):
        vx, vy = b[0] - a[0], b[1] - a[1]
        L2 = vx * vx + vy * vy
        if L2 < 1e-12:
            return math.dist(p, a)
        t = ((p[0] - a[0]) * vx + (p[1] - a[1]) * vy) / L2
        t = 0.0 if t < 0.0 else (1.0 if t > 1.0 else t)
        return math.dist(p, (a[0] + vx * t, a[1] + vy * t))

    def near(self, p):
        cx, cy = self._key(p)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for a, b in self.grid.get((cx + dx, cy + dy), ()):
                    if self._seg_dist(p, a, b) <= self.w:
                        return True
        return False


class Box:
    """
    A rectangular region, in metres, with Corridor's interface.

    A corridor is the right shape for one ride and the wrong shape for an area:
    cutting a province with --route needs a fake GPX and a half-width wider than
    the province, which is how home.tiles was built and why its corners were
    empty. This is the honest alternative -- an explicit extent, no route.
    """

    def __init__(self, min_e, min_n, max_e, max_n):
        self.b = (min_e, min_n, max_e, max_n)

    def near(self, p):
        min_e, min_n, max_e, max_n = self.b
        return min_e <= p[0] <= max_e and min_n <= p[1] <= max_n

    def bounds(self):
        return self.b


# ----------------------------------------------------------------- the pack

def pick_zooms(corridor_m):
    """Which rungs of DESIGN.md 6.1's ladder to render.

    Not all twelve. A pack cut to +/- `corridor` metres is 2*corridor wide, and
    the NAV map is 270 px across; at a rung coarser than 2*corridor/270 the
    screen is wider than anything the pack contains, so the streets would stop
    partway across the display and the basemap would read as a rendering fault
    rather than as a map. That is the honest ceiling, and it follows from the
    corridor rather than from taste: 2 km gives 1.5 .. 10 m/px, 2.5 km reaches
    15, and the coarse end of the ladder (40 m/px and up, where auto-zoom only
    goes when the next cue is more than 5 km away and there is by definition
    nothing to look at) is never rendered by default.

    --zooms overrides this, including with rungs off the ladder entirely."""
    zs = [z for z in MAP_ZOOMS if z * MAP_W <= 2.0 * corridor_m]
    return zs or [MAP_ZOOMS[0]]


def parse_zooms(spec):
    if spec.strip().lower() == "all":
        return list(MAP_ZOOMS)
    out = []
    for tok in spec.replace(",", " ").split():
        v = float(tok)
        if not v > 0.0:
            raise SystemExit(f"mktiles: bad zoom {tok!r}")
        out.append(v)
    if not out:
        raise SystemExit("mktiles: --zooms is empty")
    return sorted(set(out))


def build_zoom(ways_en, mpp, region, verbose):
    """-> (tx0, ty0, nx, ny, {(ix, iy): bytes}) for one rung.

    `ways_en` is [(weight, [(e, n), ...])]; `region` is the corridor's bounding
    box in metres, which is what fixes the grid extent. Tiles with no ink are
    not returned at all -- an L-shaped corridor is mostly empty grid, and a
    blank tile costs an index slot rather than 8 KB."""
    from PIL import Image, ImageDraw

    def pix(p):
        return (int(math.floor(p[0] / mpp)), int(math.floor(-p[1] / mpp)))

    min_e, min_n, max_e, max_n = region
    # The region's corners in pack pixels. -n flips the order of the n pair.
    rx0, ry1 = pix((min_e, min_n))
    rx1, ry0 = pix((max_e, max_n))
    # Python's // is already floor division, which is what the tile grid
    # needs: pack pixel -1 belongs to tile -1, not to tile 0.
    tx0, tx1 = rx0 // TILE_W, rx1 // TILE_W
    ty0, ty1 = ry0 // TILE_H, ry1 // TILE_H
    nx, ny = tx1 - tx0 + 1, ty1 - ty0 + 1

    # Every way's integer pixel chain, computed ONCE: this is what makes the
    # seams exact and the whole thing deterministic.
    chains = []
    for wgt, en in ways_en:
        xy = [pix(p) for p in en]
        # Collapse runs of identical pixels; at 25 m/px a 500-point way is a
        # handful of distinct pixels and PIL should not be asked to draw the
        # same zero-length segment 490 times.
        chain = [xy[0]]
        for p in xy[1:]:
            if p != chain[-1]:
                chain.append(p)
        if len(chain) < 2:
            chain.append((chain[0][0] + 1, chain[0][1]))
        bx0 = min(p[0] for p in chain)
        bx1 = max(p[0] for p in chain)
        by0 = min(p[1] for p in chain)
        by1 = max(p[1] for p in chain)
        chains.append((wgt, chain, bx0, by0, bx1, by1))

    # Bucket each chain into the tiles its bounding box spans, so a tile only
    # ever looks at ways that can reach it.
    #
    # The obvious loop -- every tile against every way -- is fine for a route
    # corridor and painful for a country: Thailand's four coarse rungs are about
    # 28k tiles against 100k ways, and that version took 24 min 57 s. This is
    # the same rejection test done once per way instead of once per (way, tile)
    # pair, and the same build then takes under 90 s. A long motorway registers
    # in many buckets, which is the cost, and it is bounded by its own length.
    #
    # Both versions were run over the same extract and produced the SAME pack,
    # sha256 903ad884...: the speed-up moves no pixel, which is the only thing
    # that makes it safe to keep.
    buckets = {}
    for item in chains:
        wgt, chain, bx0, by0, bx1, by1 = item
        pad = wgt  # a 2 px line reaches a pixel past its centreline
        for ty in range((by0 - pad) // TILE_H, (by1 + pad) // TILE_H + 1):
            for tx in range((bx0 - pad) // TILE_W, (bx1 + pad) // TILE_W + 1):
                if tx0 <= tx <= tx1 and ty0 <= ty <= ty1:
                    buckets.setdefault((tx, ty), []).append(item)

    tiles = {}
    for (tx, ty), here in buckets.items():
        ix, iy = tx - tx0, ty - ty0
        ox, oy = tx * TILE_W, ty * TILE_H
        img = Image.new("1", (TILE_W, TILE_H), 0)
        draw = ImageDraw.Draw(img)
        for wgt, chain, bx0, by0, bx1, by1 in here:
            draw.line([(x - ox, y - oy) for x, y in chain], fill=1,
                      width=wgt, joint="curve" if wgt > 1 else None)
        raw = img.tobytes()
        if not any(raw):
            continue
        tiles[(ix, iy)] = raw
    if verbose:
        print(f"  {mpp:6.1f} m/px  grid {nx}x{ny} at ({tx0},{ty0})  "
              f"{len(tiles)} tiles", file=sys.stderr)
    return tx0, ty0, nx, ny, tiles


def write_pack(path, frame, corridor_m, zooms):
    """`zooms` is [(mpp, tx0, ty0, nx, ny, {(ix, iy): bytes})], in rung order."""
    nz = len(zooms)
    index_off = HEADER_BYTES + ZOOM_ENTRY * nz
    # Lay out every index first, then the tile payloads: the header and the
    # tables are then one contiguous read on the device.
    offs = []
    for _, _, _, nx, ny, _ in zooms:
        offs.append(index_off)
        index_off += 4 * nx * ny
    data_off = index_off

    blobs = []
    indexes = []
    ntiles = 0
    for (mpp, tx0, ty0, nx, ny, tiles), _ in zip(zooms, offs):
        idx = [0] * (nx * ny)
        for iy in range(ny):
            for ix in range(nx):
                raw = tiles.get((ix, iy))
                if raw is None:
                    continue
                idx[iy * nx + ix] = data_off
                blobs.append(raw)
                data_off += len(raw)
                ntiles += 1
        indexes.append(idx)

    with open(path, "wb") as f:
        f.write(struct.pack("<8sHHHHHHdddddI", MAGIC, VERSION, HEADER_BYTES,
                            TILE_W, TILE_H, nz, FLAG_MSB_INK,
                            frame.lat0, frame.lon0, M_PER_DEG_LAT,
                            M_PER_DEG_LON, corridor_m, ntiles))
        for (mpp, tx0, ty0, nx, ny, _), off in zip(zooms, offs):
            f.write(struct.pack("<diiIIII", mpp, tx0, ty0, nx, ny, off, 0))
        for idx in indexes:
            f.write(struct.pack(f"<{len(idx)}I", *idx))
        for raw in blobs:
            f.write(raw)
    return ntiles


def info(path):
    with open(path, "rb") as f:
        blob = f.read()
    (magic, ver, hb, tw, th, nz, flags, lat0, lon0, klat, klon, corr,
     ntiles) = struct.unpack_from("<8sHHHHHHdddddI", blob, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: not a beepy-nav tile pack")
    print(f"{path}: v{ver}  {tw}x{th}  {nz} zooms  {ntiles} tiles  "
          f"{len(blob)} bytes")
    print(f"  reference {lat0:.7f},{lon0:.7f}  corridor {corr:.0f} m  "
          f"flags 0x{flags:x}  m/deg {klat:g},{klon:g}")
    for i in range(nz):
        mpp, tx0, ty0, nx, ny, off, _ = struct.unpack_from(
            "<diiIIII", blob, hb + ZOOM_ENTRY * i)
        idx = struct.unpack_from(f"<{nx * ny}I", blob, off)
        used = sum(1 for v in idx if v)
        print(f"  {mpp:6.1f} m/px  grid {nx}x{ny} at ({tx0},{ty0})  "
              f"{used} tiles  {used * (tw // 8) * th / 1024:.0f} KB")


# --------------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="build a 1-bit OSM basemap tile pack (DESIGN.md 6.5)")
    ap.add_argument("--osm", help="Overpass JSON extract")
    ap.add_argument("--route", help="the GPX whose corridor is cut")
    ap.add_argument("-o", "--out", help="pack to write")
    ap.add_argument("--corridor", type=float, default=2000.0,
                    metavar="M", help="corridor half-width, metres (2000)")
    ap.add_argument("--bbox", metavar="LAT0,LON0,LAT1,LON1",
                    help="cut a rectangle instead of a route corridor -- what "
                         "an area pack wants (no --route needed)")
    ap.add_argument("--ref", metavar="LAT,LON",
                    help="centre of a square region, with --radius; the "
                         "projection reference either way")
    ap.add_argument("--radius", type=float, default=None, metavar="M",
                    help="half-side of the --ref square, metres")
    ap.add_argument("--frame", metavar="LAT,LON",
                    help="projection reference, when it must differ from the "
                         "region's own centre -- which is what merging two "
                         "packs into one needs, since tile addressing is "
                         "relative to the frame and only packs that share one "
                         "can be joined (tools/mergetiles.py)")
    ap.add_argument("--zooms", default=None, metavar="LIST",
                    help="comma-separated m/px, or 'all'; default follows "
                         "--corridor (see pick_zooms)")
    ap.add_argument("--info", metavar="PACK", help="describe a pack and exit")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    if a.info:
        info(a.info)
        return 0
    nsel = sum(1 for x in (a.route, a.bbox, a.ref) if x)
    if not (a.osm and a.out) or nsel != 1:
        ap.error("--osm, -o and exactly one of --route / --bbox / --ref")
    if a.ref and a.radius is None:
        ap.error("--ref needs --radius M")

    if a.route:
        route_ll = read_gpx(a.route)
        frame = Frame(route_ll[0][0], route_ll[0][1])
        route_en = [frame.met(la, lo) for la, lo in route_ll]
        sel = Corridor(route_en, a.corridor)
        # The grid covers the corridor, not the extract: a way kept because one
        # end reached in stays whole, and its far end is simply outside it.
        min_e = min(p[0] for p in route_en) - a.corridor
        max_e = max(p[0] for p in route_en) + a.corridor
        min_n = min(p[1] for p in route_en) - a.corridor
        max_n = max(p[1] for p in route_en) + a.corridor
        extent = a.corridor
    elif a.bbox:
        la0, lo0, la1, lo1 = (float(v) for v in a.bbox.split(","))
        if la1 < la0:
            la0, la1 = la1, la0
        if lo1 < lo0:
            lo0, lo1 = lo1, lo0
        # Reference at the centre, so metres stay small and symmetric across
        # the region -- the tangent-plane error of DESIGN.md 6.1 grows with
        # distance from the reference, and a corner reference doubles it.
        frame = Frame((la0 + la1) / 2.0, (lo0 + lo1) / 2.0)
        c0, c1 = frame.met(la0, lo0), frame.met(la1, lo1)
        min_e, min_n, max_e, max_n = c0[0], c0[1], c1[0], c1[1]
        sel = Box(min_e, min_n, max_e, max_n)
        extent = max(max_e - min_e, max_n - min_n) / 2.0
    else:
        la, lo = (float(v) for v in a.ref.split(","))
        frame = Frame(la, lo)
        r = a.radius
        min_e, min_n, max_e, max_n = -r, -r, r, r
        sel = Box(min_e, min_n, max_e, max_n)
        extent = r

    if a.frame:
        # Re-express the region in the requested frame. The SELECTION is
        # unchanged -- the same ways, the same rectangle on the ground -- but
        # every metre, and so every tile boundary, is now measured from the
        # shared reference.
        fla, flo = (float(v) for v in a.frame.split(","))
        old = frame
        frame = Frame(fla, flo)
        # Corner round-trip: metres -> lat/lon in the old frame -> metres in
        # the new one. Frame has no inverse, so go through the ratio it used.
        def relocate(e, n):
            la = old.lat0 + n / M_PER_DEG_LAT
            lo = old.lon0 + e / (M_PER_DEG_LON *
                                 math.cos(math.radians(old.lat0)))
            return frame.met(la, lo)
        (min_e, min_n) = relocate(min_e, min_n)
        (max_e, max_n) = relocate(max_e, max_n)
        if a.route:
            route_en = [relocate(e, n) for e, n in route_en]
            sel = Corridor(route_en, a.corridor)
        else:
            sel = Box(min_e, min_n, max_e, max_n)

    ways = read_osm(a.osm)
    kept = []
    for _, wgt, geom in ways:
        en = [frame.met(la_, lo_) for la_, lo_ in geom]
        if any(sel.near(p) for p in en):
            kept.append((wgt, en))

    region = (min_e, min_n, max_e, max_n)

    zlist = parse_zooms(a.zooms) if a.zooms else pick_zooms(extent)
    if not a.quiet:
        what = (f"within {a.corridor:.0f} m of {os.path.basename(a.route)}"
                if a.route else
                f"inside {(max_e - min_e) / 1000:.0f}x"
                f"{(max_n - min_n) / 1000:.0f} km")
        print(f"mktiles: {len(ways)} ways, {len(kept)} {what}; "
              f"reference {frame.lat0:.7f},{frame.lon0:.7f}", file=sys.stderr)
        print(f"mktiles: zooms {', '.join(f'{z:g}' for z in zlist)}",
              file=sys.stderr)

    built = []
    for mpp in zlist:
        tx0, ty0, nx, ny, tiles = build_zoom(kept, mpp, region, not a.quiet)
        built.append((mpp, tx0, ty0, nx, ny, tiles))

    ntiles = write_pack(a.out, frame, extent, built)
    size = os.path.getsize(a.out)
    if not a.quiet:
        sha = hashlib.sha256(open(a.out, "rb").read()).hexdigest()
        print(f"mktiles: {a.out}: {ntiles} tiles, {size} bytes "
              f"({size / 1024.0:.0f} KB)", file=sys.stderr)
        print(f"mktiles: sha256 {sha}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

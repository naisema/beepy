#!/usr/bin/env python3
"""
Join several .tiles packs into one, so a single basemap can be detailed where
you ride and coarse everywhere else.

Why this is a separate tool rather than a flag on mktiles.py: the packs it
joins are cut from DIFFERENT extracts over DIFFERENT regions. A nationwide
rung wants a country of major roads; a 4 m/px rung wants every residential
street within 20 km and would be absurd over a country -- Thailand at 4 m/px
is 490,000 tiles. One build cannot want both, so two builds and a join.

    tools/mktiles.py --osm region.json  --ref 13.885,100.378 --radius 20000 \\
                     --frame 13.05,101.50 --zooms 4,6,10 -o fine.tiles
    tools/mktiles.py --osm country.json --bbox 5.5,97.3,20.6,105.7 \\
                     --frame 13.05,101.50 --zooms 15,25,40,60 -o coarse.tiles
    tools/mergetiles.py fine.tiles coarse.tiles -o all.tiles

THE ONE RULE: every input must share a projection frame. Tile addressing is
`floor(e / mpp)` in the pack's own frame (DESIGN.md 6.5), so tiles cut against
a different reference name different ground. That is not something this tool
can paper over -- it refuses, and mktiles' --frame is how you avoid it.

A zoom rung appearing in two inputs is UNIONED when the two cover different
ground, which is what puts fine detail for two cities in one basemap. The grid
grows to the bounding box of both and every tile keeps its own address; where
both packs hold the SAME cell the first wins, and that collision is counted on
stderr because it is the only case where a choice is being made.

This used to take the rung from the first pack and discard the rest, on the
grounds that "interleaving two of them would mean deciding which pack owns each
tile". That is true only where they overlap, and two cities 750 km apart do not:
Bangkok's 1.5 m/px grid sits at tile y -264 and Songkhla's at y +1659. What the
old rule cost was a rider who had to swap packs to change province.

THE PRICE IS INDEX, and it is small enough to be worth naming: a rung's index is
one u32 per grid CELL whether or not a tile is there, so a bounding box spanning
both cities pays for the empty middle. Measured on exactly that pair, all five
fine rungs together: 1.43 MB against 300 MB of tiles. A union of two areas on
opposite sides of the planet would cost 60 MB of index and deserves the refusal
it does not get -- if that ever matters, cut the rung as two packs and stop.
"""

import argparse
import os
import struct
import sys

MAGIC = b"BNAVTILE"
VERSION = 1          # dense index: one u32 per grid CELL
VERSION_SPARSE = 2   # sparse index: one (key, offset) pair per non-empty tile

# When a rung's dense index would exceed this many cells, the output is written
# sparse instead. It is the reader's own ceiling (beepy-nav/src/tile.c), so a pack
# this tool produces is always one the device will open.
#
# The number that forced this: four regions 1 300 km apart at 0.375 m/px share a
# bounding box of 2013 x 13717 cells, and the index is 4 bytes per cell whether a
# tile is there or not -- 105 MB, resident at open, on a device with 426 MB. The
# same tiles sparse are 1.2 MB, because 156 000 of those 27 million cells have
# anything in them. Between Songkhla and Chiang Mai the rest is sea and jungle.
DENSE_MAX_CELLS = 4 * 1024 * 1024
HEADER_BYTES = 64
ZOOM_ENTRY = 32
HDR = "<8sHHHHHHdddddI"
ZE = "<diiIIII"


def read_pack(path):
    with open(path, "rb") as f:
        blob = f.read()
    if len(blob) < HEADER_BYTES or blob[:8] != MAGIC:
        raise SystemExit(f"mergetiles: {path}: not a .tiles pack")
    (_, ver, hb, tw, th, nz, flags, lat0, lon0, klat, klon, extent,
     ntiles) = struct.unpack_from(HDR, blob, 0)
    if ver not in (VERSION, VERSION_SPARSE):
        raise SystemExit(f"mergetiles: {path}: version {ver}, expected "
                         f"{VERSION} or {VERSION_SPARSE}")
    rungs = []
    for i in range(nz):
        mpp, tx0, ty0, nx, ny, ioff, count = struct.unpack_from(
            ZE, blob, hb + ZOOM_ENTRY * i)
        # Lift each tile's bytes out now; the offsets are rewritten on output.
        tiles = {}
        if ver == VERSION_SPARSE:
            pairs = struct.unpack_from(f"<{2 * count}I", blob, ioff)
            for j in range(count):
                k, off = pairs[2 * j], pairs[2 * j + 1]
                if off:
                    tiles[(k % nx, k // nx)] = blob[off:off + (tw * th // 8)]
        else:
            idx = struct.unpack_from(f"<{nx * ny}I", blob, ioff)
            for k, off in enumerate(idx):
                if off:
                    tiles[(k % nx, k // nx)] = blob[off:off + (tw * th // 8)]
        rungs.append((mpp, tx0, ty0, nx, ny, tiles))
    return dict(tw=tw, th=th, flags=flags, lat0=lat0, lon0=lon0, klat=klat,
                klon=klon, extent=extent, rungs=rungs, path=path)


def main():
    ap = argparse.ArgumentParser(
        description="join .tiles packs that share a projection frame")
    ap.add_argument("packs", nargs="+")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--sparse", action="store_true",
                    help="force the sparse index (pack v2) whatever the grid "
                         "size. Sparse is chosen automatically once a rung's "
                         "dense index would exceed the reader's ceiling; this "
                         "exists so a TEST can exercise the encoding without "
                         "depending on two fixtures being a particular distance "
                         "apart, which is a coincidence a test should not rest "
                         "on")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    packs = [read_pack(p) for p in a.packs]
    base = packs[0]
    for p in packs[1:]:
        for k in ("lat0", "lon0", "klat", "klon", "tw", "th", "flags"):
            if p[k] != base[k]:
                raise SystemExit(
                    f"mergetiles: {p['path']} disagrees with {base['path']} "
                    f"on {k} ({p[k]} vs {base[k]}).\n"
                    f"  Tiles are addressed in the pack's own frame, so packs "
                    f"cut against different references\n"
                    f"  name different ground. Rebuild with a shared "
                    f"mktiles --frame LAT,LON.")

    # mpp -> [x0, y0, x1, y1, {cell: raw}, [source names], collisions]
    merged, order = {}, []
    for p in packs:
        who = os.path.basename(p["path"])
        for mpp, tx0, ty0, nx, ny, tiles in p["rungs"]:
            # Absolute tile addresses, so two grids with different origins can
            # be compared at all. floor(e / mpp / tw) is the same number in both
            # packs BECAUSE the frame check above passed.
            cells = {(tx0 + ix, ty0 + iy): raw for (ix, iy), raw in
                     tiles.items()}
            if mpp not in merged:
                merged[mpp] = [tx0, ty0, tx0 + nx, ty0 + ny, cells, [who], 0]
                order.append(mpp)
                continue
            m = merged[mpp]
            hits = 0
            for k, raw in cells.items():
                if k in m[4]:
                    hits += 1          # the first pack keeps it
                else:
                    m[4][k] = raw
            m[0] = min(m[0], tx0)
            m[1] = min(m[1], ty0)
            m[2] = max(m[2], tx0 + nx)
            m[3] = max(m[3], ty0 + ny)
            m[5].append(who)
            m[6] += hits

    rungs = []
    for mpp in sorted(order):
        x0, y0, x1, y1, cells, who, hits = merged[mpp]
        nx, ny = x1 - x0, y1 - y0
        rungs.append((mpp, x0, y0, nx, ny,
                      {(k[0] - x0, k[1] - y0): raw for k, raw in
                       cells.items()}))
        if not a.quiet and len(who) > 1:
            note = f" ({hits} cells kept from {who[0]})" if hits else ""
            print(f"mergetiles: {mpp:g} m/px unioned from "
                  f"{', '.join(who)}{note}", file=sys.stderr)
    seen = {mpp: ", ".join(merged[mpp][5]) for mpp in order}

    nz = len(rungs)
    # SPARSE if any rung's dense index would be over the reader's ceiling. One
    # decision for the whole pack, not per rung: a reader that had to switch
    # encodings between rungs of one file is a reader with two code paths and one
    # of them untested.
    sparse = a.sparse or any((nx * ny) > DENSE_MAX_CELLS
                             for _, _, _, nx, ny, _ in rungs)
    version = VERSION_SPARSE if sparse else VERSION

    off = HEADER_BYTES + ZOOM_ENTRY * nz
    ioffs = []
    for _, _, _, nx, ny, tiles in rungs:
        ioffs.append(off)
        off += (8 * len(tiles)) if sparse else (4 * nx * ny)
    data = off

    indexes, blobs, ntiles = [], [], 0
    for (mpp, tx0, ty0, nx, ny, tiles) in rungs:
        # Sorted by (row, column) either way: the dense index needs it for
        # locality and the sparse one needs it because the reader BINARY SEARCHES
        # the keys. Same order, one reason each.
        items = sorted(tiles.items(), key=lambda kv: (kv[0][1], kv[0][0]))
        if sparse:
            pairs = []
            for (ix, iy), raw in items:
                pairs.append((iy * nx + ix, data))
                blobs.append(raw)
                data += len(raw)
                ntiles += 1
            indexes.append(pairs)
        else:
            idx = [0] * (nx * ny)
            for (ix, iy), raw in items:
                idx[iy * nx + ix] = data
                blobs.append(raw)
                data += len(raw)
                ntiles += 1
            indexes.append(idx)

    extent = max(p["extent"] for p in packs)
    with open(a.out, "wb") as f:
        f.write(struct.pack(HDR, MAGIC, version, HEADER_BYTES, base["tw"],
                            base["th"], nz, base["flags"], base["lat0"],
                            base["lon0"], base["klat"], base["klon"], extent,
                            ntiles))
        for (mpp, tx0, ty0, nx, ny, tiles), io in zip(rungs, ioffs):
            # The last u32 was always zero in v1; in v2 it is the tile count, so
            # the zoom entry does not change size and a v1 reader still sees a
            # well-formed table (with a version it will refuse, which is the
            # point).
            f.write(struct.pack(ZE, mpp, tx0, ty0, nx, ny, io,
                                len(tiles) if sparse else 0))
        for idx in indexes:
            if sparse:
                flat = [v for pair in idx for v in pair]
                f.write(struct.pack(f"<{len(flat)}I", *flat))
            else:
                f.write(struct.pack(f"<{len(idx)}I", *idx))
        for raw in blobs:
            f.write(raw)

    if not a.quiet:
        for mpp, tx0, ty0, nx, ny, tiles in rungs:
            print(f"  {mpp:6.1f} m/px  grid {nx}x{ny} at ({tx0},{ty0})  "
                  f"{len(tiles)} tiles  from {seen[mpp]}", file=sys.stderr)
        print(f"mergetiles: {a.out}: {ntiles} tiles, "
              f"{os.path.getsize(a.out)} bytes "
              f"({os.path.getsize(a.out) // 1024} KB)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

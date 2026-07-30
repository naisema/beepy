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

A zoom rung appearing in two inputs is taken from the FIRST that has it, and
said so on stderr: rungs are whole grids, and interleaving two of them would
mean deciding which pack owns each tile, which is a merge of maps and not of
files.
"""

import argparse
import os
import struct
import sys

MAGIC = b"BNAVTILE"
VERSION = 1
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
    if ver != VERSION:
        raise SystemExit(f"mergetiles: {path}: version {ver}, expected {VERSION}")
    rungs = []
    for i in range(nz):
        mpp, tx0, ty0, nx, ny, ioff, _ = struct.unpack_from(
            ZE, blob, hb + ZOOM_ENTRY * i)
        idx = struct.unpack_from(f"<{nx * ny}I", blob, ioff)
        # Lift each tile's bytes out now; the offsets are rewritten on output.
        tiles = {}
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

    rungs, seen = [], {}
    for p in packs:
        for r in p["rungs"]:
            mpp = r[0]
            if mpp in seen:
                if not a.quiet:
                    print(f"mergetiles: {mpp:g} m/px already came from "
                          f"{seen[mpp]}, skipping {p['path']}'s",
                          file=sys.stderr)
                continue
            seen[mpp] = os.path.basename(p["path"])
            rungs.append(r)
    rungs.sort(key=lambda r: r[0])

    nz = len(rungs)
    off = HEADER_BYTES + ZOOM_ENTRY * nz
    ioffs = []
    for _, _, _, nx, ny, _ in rungs:
        ioffs.append(off)
        off += 4 * nx * ny
    data = off

    indexes, blobs, ntiles = [], [], 0
    for (mpp, tx0, ty0, nx, ny, tiles) in rungs:
        idx = [0] * (nx * ny)
        for (ix, iy), raw in sorted(tiles.items(), key=lambda kv: (kv[0][1],
                                                                   kv[0][0])):
            idx[iy * nx + ix] = data
            blobs.append(raw)
            data += len(raw)
            ntiles += 1
        indexes.append(idx)

    extent = max(p["extent"] for p in packs)
    with open(a.out, "wb") as f:
        f.write(struct.pack(HDR, MAGIC, VERSION, HEADER_BYTES, base["tw"],
                            base["th"], nz, base["flags"], base["lat0"],
                            base["lon0"], base["klat"], base["klon"], extent,
                            ntiles))
        for (mpp, tx0, ty0, nx, ny, _), io in zip(rungs, ioffs):
            f.write(struct.pack(ZE, mpp, tx0, ty0, nx, ny, io, 0))
        for idx in indexes:
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

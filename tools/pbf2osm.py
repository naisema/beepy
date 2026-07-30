#!/usr/bin/env python3
"""
Convert a .osm.pbf extract into the Overpass-JSON shape mktiles.py and
mkpack.py already read, keeping only the road classes they care about and
only inside a bounding box.

Why this exists: a whole country cannot come from Overpass. The public API
refuses a query that size (and rightly), so a nationwide basemap has to start
from a Geofabrik extract -- which is PBF, a format neither tool speaks. Rather
than teach both tools a second input format, this writes the one they already
have, so nothing downstream changes and every existing test still describes the
code that runs.

Output is `{"elements": [{"type": "way", "tags": {...}, "geometry": [...]}]}`,
which is what `[out:json]; out geom;` produces.

    tools/pbf2osm.py thailand.osm.pbf --bbox 5.5,97.3,20.6,105.7 -o th.json
    tools/pbf2osm.py thailand.osm.pbf --bbox ... --classes trunk,primary -o x.json

Needs pyosmium (`uv pip install osmium`). Streams, so memory stays flat, but
node locations for a whole country do not fit in a dict -- hence the two
passes and the on-disk node cache osmium provides.
"""

import argparse
import json
import sys

import osmium

# mktiles.py's OSM_WEIGHT keys, plus the link roads: a slip road that is
# dropped leaves a junction looking unconnected on the map.
ROADS = ("motorway", "motorway_link", "trunk", "trunk_link", "primary",
         "primary_link", "secondary", "secondary_link", "tertiary",
         "tertiary_link", "unclassified", "residential", "living_street")

# What a coarse nationwide pack wants: at 15 m/px a residential street is one
# pixel wide and there are millions of them, so they cost size and add nothing.
COARSE = ("motorway", "motorway_link", "trunk", "trunk_link", "primary",
          "primary_link", "secondary", "secondary_link")


class Collect(osmium.SimpleHandler):
    def __init__(self, classes, bbox, keep_names):
        super().__init__()
        self.classes = set(classes)
        self.bbox = bbox  # (lat0, lon0, lat1, lon1) or None
        self.keep_names = keep_names
        self.out = []
        self.seen = 0
        self.dropped_bbox = 0

    def way(self, w):
        hw = w.tags.get("highway")
        if hw not in self.classes:
            return
        self.seen += 1
        try:
            geom = [(n.lat, n.lon) for n in w.nodes if n.location.valid()]
        except osmium.InvalidLocationError:
            return
        if len(geom) < 2:
            return
        if self.bbox:
            la0, lo0, la1, lo1 = self.bbox
            if not any(la0 <= la <= la1 and lo0 <= lo <= lo1 for la, lo in geom):
                self.dropped_bbox += 1
                return
        tags = {"highway": hw}
        if self.keep_names:
            for k in ("name:en", "name", "oneway"):
                if k in w.tags:
                    tags[k] = w.tags[k]
        elif "oneway" in w.tags:
            tags["oneway"] = w.tags["oneway"]
        self.out.append({"type": "way", "id": w.id, "tags": tags,
                         "geometry": [{"lat": la, "lon": lo}
                                      for la, lo in geom]})


def main():
    ap = argparse.ArgumentParser(
        description="PBF -> the Overpass JSON mktiles/mkpack already read")
    ap.add_argument("pbf")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--bbox", metavar="LAT0,LON0,LAT1,LON1",
                    help="keep only ways touching this box")
    ap.add_argument("--classes", default="all",
                    help="'all' (every road class), 'coarse' (motorway..."
                         "secondary, for a nationwide pack), or a "
                         "comma-separated list")
    ap.add_argument("--no-names", action="store_true",
                    help="drop name tags: a basemap never draws them, and for "
                         "a country they are most of the JSON")
    a = ap.parse_args()

    if a.classes == "all":
        classes = ROADS
    elif a.classes == "coarse":
        classes = COARSE
    else:
        classes = tuple(x.strip() for x in a.classes.split(","))

    bbox = None
    if a.bbox:
        v = [float(x) for x in a.bbox.split(",")]
        bbox = (min(v[0], v[2]), min(v[1], v[3]),
                max(v[0], v[2]), max(v[1], v[3]))

    h = Collect(classes, bbox, not a.no_names)
    # locations=True gives every way its node coordinates; the flex-mem index
    # holds a country's nodes in a few hundred MB rather than a dict's several
    # gigabytes.
    h.apply_file(a.pbf, locations=True, idx="flex_mem")

    with open(a.out, "w") as f:
        json.dump({"elements": h.out}, f, separators=(",", ":"))
    import os
    print(f"pbf2osm: {h.seen} ways in {len(classes)} classes, "
          f"{len(h.out)} kept"
          + (f", {h.dropped_bbox} outside the box" if bbox else ""),
          file=sys.stderr)
    print(f"pbf2osm: {a.out}: {os.path.getsize(a.out) / 1e6:.1f} MB",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

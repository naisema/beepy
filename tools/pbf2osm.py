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

import unicodedata

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

# The keys that make a feature a DESTINATION rather than a road: a school, a
# hospital, a station, a market. Any value will do -- the filter that actually
# matters is that the thing has a name, because an unnamed feature cannot be
# searched for and is not worth a byte. `place` is here for the district and
# suburb names a rider thinks in ("BANG KHAE") as much as for shops.
#
# Keys, not key=value pairs, on purpose. A whitelist of values would need
# maintaining against a tag scheme that grows every year, and would silently
# drop whatever was invented last; the name test does the same job without
# the maintenance.
POI_KEYS = ("amenity", "shop", "leisure", "tourism", "office", "healthcare",
            "craft", "railway", "aeroway", "public_transport", "historic",
            "emergency", "place")


# The searchable form of a name, or None.
#
# NFKD-FOLD BEFORE THE ASCII TEST, and that is a correction rather than a
# refinement. DESIGN.md 1.4.2 drops a name with no ASCII form because the 5x7
# font is A-Z/0-9 and there is no glyph for U+0E0B -- a shaping problem, not a
# spelling one. An ACCENTED LATIN letter is nothing like that case: it folds to
# the letter it is, losing nothing a rider reads, and the font uppercases
# everything anyway.
#
# A plain isascii() got that wrong in the field. `Cafe Amazon` is spelled
# `Cafe\u0301 Amazon` in OSM at most PTT stations, so the branch 3.4 km from the
# rider's house was invisible while the ones somebody had typed without the
# accent were searchable nineteen kilometres away. Transliterated Thai has the
# same problem with macrons -- `Bang Yai`.
#
# Thai still drops, correctly: stripping combining marks from Thai leaves Thai.
# So do the letters NFKD will not decompose at all -- OE and the German sharp s
# among them -- and those would need a transliteration table rather than a fold,
# which is a bigger claim than this function makes.
def ascii_form(raw):
    if not raw:
        return None
    folded = "".join(c for c in unicodedata.normalize("NFKD", raw)
                     if not unicodedata.combining(c))
    return folded if folded.isascii() else None


def _poi_name(tags):
    """The searchable form, or None. Same rule as mkpack.py's ascii_form():
    name:en first, then name, folded, and nothing at all if what is left is not
    ASCII. Duplicated in the two tools deliberately -- they run separately -- and
    the two MUST agree, because pbf2osm decides what reaches the extract and
    mkpack decides what reaches the pack."""
    return ascii_form(tags.get("name:en") or tags.get("name") or "")


class Collect(osmium.SimpleHandler):
    def __init__(self, classes, bbox, keep_names, want_pois):
        super().__init__()
        self.classes = set(classes)
        self.bbox = bbox  # (lat0, lon0, lat1, lon1) or None
        self.keep_names = keep_names
        self.want_pois = want_pois
        self.out = []
        self.seen = 0
        self.dropped_bbox = 0
        self.pois = 0
        self.pois_nonascii = 0

    def _inside(self, la, lo):
        if not self.bbox:
            return True
        la0, lo0, la1, lo1 = self.bbox
        return la0 <= la <= la1 and lo0 <= lo <= lo1

    def _poi(self, o, la, lo):
        """Emit one destination as an Overpass `node` element. An AREA becomes
        its centroid, which is what `out center` would have given -- good
        enough because the router snaps a destination to the nearest road node
        anyway (router.c's snap()), so the metres between a campus centroid and
        its gate are absorbed by a step that has to happen regardless."""
        key = next((k for k in POI_KEYS if k in o.tags), None)
        if key is None:
            return
        name = _poi_name(o.tags)
        if name is None:
            if o.tags.get("name"):
                self.pois_nonascii += 1
            return
        if not self._inside(la, lo):
            return
        tags = {key: o.tags.get(key), "name": name}
        if "name:en" in o.tags:
            tags["name:en"] = o.tags["name:en"]
        self.out.append({"type": "node", "id": o.id, "lat": la, "lon": lo,
                         "tags": tags})
        self.pois += 1

    def node(self, n):
        if self.want_pois and n.location.valid():
            self._poi(n, n.location.lat, n.location.lon)

    def way(self, w):
        hw = w.tags.get("highway")
        if hw not in self.classes:
            # Not a road we draw -- but it may still be somewhere to go. A
            # school, a mall and a park are all closed ways in OSM.
            if self.want_pois:
                try:
                    pts = [(n.lat, n.lon) for n in w.nodes
                           if n.location.valid()]
                except osmium.InvalidLocationError:
                    return
                if pts:
                    self._poi(w, sum(p[0] for p in pts) / len(pts),
                              sum(p[1] for p in pts) / len(pts))
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
                    help="'none' (POIs only), 'all' (every road class), "
                         "'coarse' (motorway..."
                         "secondary, for a nationwide pack), or a "
                         "comma-separated list")
    ap.add_argument("--no-names", action="store_true",
                    help="drop name tags: a basemap never draws them, and for "
                         "a country they are most of the JSON")
    ap.add_argument("--pois", action="store_true", default=None,
                    help="also emit named destinations -- schools, stations, "
                         "shops -- as Overpass `node` elements, an area as its "
                         "centroid. On by default unless --no-names or "
                         "--classes coarse says this is a basemap extract")
    ap.add_argument("--no-pois", dest="pois", action="store_false",
                    help="roads only")
    a = ap.parse_args()

    if a.classes == "none":
        # POIs and nothing else. A nationwide destination index does not want a
        # nationwide road graph beside it: mkpack.py's PLACES/POINTS/STRINGS are
        # 5.8 MB for the whole country and the graph would be half a gigabyte of
        # RAM on a 512 MB device (DESIGN.md 1.4.7). This is how the two are cut
        # apart -- one pass for the ground you route over, one for the names you
        # can search.
        classes = ()
    elif a.classes == "all":
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

    # A basemap extract wants none of this: tiles draw geometry and never a
    # label, so destinations there would be megabytes that nothing reads.
    want_pois = a.pois
    if want_pois is None:
        want_pois = not (a.no_names or a.classes == "coarse")

    h = Collect(classes, bbox, not a.no_names, want_pois)
    # locations=True gives every way its node coordinates; the flex-mem index
    # holds a country's nodes in a few hundred MB rather than a dict's several
    # gigabytes.
    h.apply_file(a.pbf, locations=True, idx="flex_mem")

    with open(a.out, "w") as f:
        json.dump({"elements": h.out}, f, separators=(",", ":"))
    import os
    print(f"pbf2osm: {h.seen} ways in {len(classes)} classes, "
          f"{len(h.out) - h.pois} kept"
          + (f", {h.dropped_bbox} outside the box" if bbox else ""),
          file=sys.stderr)
    if want_pois:
        print(f"pbf2osm: {h.pois} named destinations, "
              f"{h.pois_nonascii} dropped for having no ASCII name",
              file=sys.stderr)
    print(f"pbf2osm: {a.out}: {os.path.getsize(a.out) / 1e6:.1f} MB",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

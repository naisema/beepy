#!/usr/bin/env python3
"""
mkpack.py -- build the road/name pack beepy-nav searches and routes over
             (DESIGN.md 1.4).

    python3 tools/mkpack.py --osm EXTRACT.json -o PACK.roads
                            [--route ROUTE.gpx | --ref LAT,LON]
                            [--ignore-oneway] [--repeat NxM] [--quiet]
    python3 tools/mkpack.py --info PACK.roads

Mac-side only, like tools/mktiles.py: it wants an Overpass extract, and the
device has neither the extract nor a JSON parser. The output is the one artefact
the device sees, and beepy-nav/src/search.c is the only thing that reads it.

WHAT IT DOES

  1. Projects the extract onto the local tangent plane of DESIGN.md 6.1,
     referenced to --route's FIRST point (route.c's own rule, and the rule
     mktiles.py uses), or to --ref, or -- failing both -- to the first node of
     the first way in file order. Any of the three is deterministic.
  2. Builds the road graph by joining ways on EXACT shared coordinates. That is
     what mockup.py's route_graph() does, and it is what OSM guarantees: two
     ways that meet share a node, so their geometry carries identical lat/lon
     there. On osm-asok.json it produces 2 803 nodes, the figure DESIGN.md 1.4
     quotes.
  3. Emits the graph DIRECTED, honouring `oneway`. A oneway way contributes one
     edge per segment and no reverse; everything else contributes two. The
     mockup router ignored oneway and said so; production must not, which is
     the whole point of --ignore-oneway existing here: it builds the WRONG pack
     on purpose, so the test that a oneway is respected can be shown to fail
     without the fix (T-ONEWAY).
  4. Indexes the searchable names: `name:en` falling back to `name`, uppercased,
     ASCII only -- the 5x7 font is A-Z/0-9 and there is no glyph for U+0E0B. A
     name with no ASCII form is COUNTED and the count goes in the header, so the
     device can say how much of the pack it cannot show rather than silently
     missing places.
  5. For each name, every third vertex of every way carrying it, as the
     candidate coordinates. Every third and not one: search_places() ranks by
     distance from the rider, so the representative point of a 2 km road has to
     be chosen at query time, not at pack time. mockup.py samples `[::3]` and
     the device does the same arithmetic over the same points.
  6. Indexes the DESTINATIONS in the extract -- schools, stations, markets --
     which pbf2osm.py emits as Overpass `node` elements, an area having become
     its centroid. They join the same PLACES table as the streets, carrying one
     candidate point where a street carries every third vertex, and that is the
     whole change: nothing on the device distinguishes them. search_places()
     ranks by distance to the nearest candidate either way, and the router
     snaps whatever it is handed to the nearest graph node, so a school
     centroid inside a campus routes to the road outside it for free.
     `--no-pois` builds the pack as it was before, streets only.

DETERMINISM is a requirement, not a nicety: `make test-roads` builds the pack
twice and compares sha256 against the committed fixture. Nothing here reads the
clock, the environment or a hash seed; ways are visited in extract order, node
indices are assigned in first-seen order, and the place table is sorted by name.

------------------------------------------------------------------ PACK FORMAT

Little-endian throughout, fixed-width fields, no text parsing on the device.
The header deliberately reuses DESIGN.md 6.5's shape -- magic, u16 version,
u16 header_bytes, then lat0/lon0 and the two metres-per-degree constants at the
same offsets -- because two packs read by the same program should not differ in
the eight fields they have in common.

  HEADER, 64 bytes at offset 0
    off  size  type    field
      0     8  char[8] magic, "BNAVROAD" (no NUL)
      8     2  u16     version, 1
     10     2  u16     header_bytes, 64 -- the section table starts here
     12     2  u16     flags; bit 0 = oneway honoured (reverse edges omitted)
     14     2  u16     nsect, 6
     16     4  u32     nway, ways that reached the graph -- informational
     20     8  f64     lat0, projection reference, degrees
     28     8  f64     lon0
     36     8  f64     m_per_deg_lat, 110540.0
     44     8  f64     m_per_deg_lon, 111320.0 (multiplied by cos(lat0) in use)
     52     4  u32     coord_scale, 10000000 -- the lat/lon fixed-point divisor
     56     4  u32     ndropped, names with no ASCII form (see above)
     60     4  u32     reserved, 0

  SECTION TABLE, nsect entries of 8 bytes at header_bytes, in this fixed order
      0     4  u32     off, file offset of the section
      4     4  u32     count, entries (meaning per section)

      0 NODES    count = nodes.     8 bytes each: i32 lat*1e7, i32 lon*1e7
      1 ADJ      count = nodes + 1. 4 bytes each: u32 first edge of node i,
                 so node i's outgoing edges are ADJ[i] .. ADJ[i+1]-1. The
                 edge table is therefore already sorted by source node and the
                 source index is implied -- one indirection per node expansion
                 instead of a search.
      2 EDGES    count = directed edges. 14 bytes each (12 before v3):
                     0  4  u32  to, destination node index
                     4  4  u32  len_mm, segment length in millimetres
                     8  2  u16  flags; bit 0 = the parent way is oneway, so
                                the reverse edge is deliberately ABSENT
                                bits 1-4 = road class, coarse-to-fine, so
                                "above class N" is a comparison (v2)
                                bit 5 = the way is tolled, `toll=yes` only (v4)
                                bits 6-15 spare
                    10  4  u32  name, index into PLACES, 0xffffffff = unnamed.
                                WIDENED IN v3: it was a u16, which capped the
                                PLACES table at 65 534 and stopped a nationwide
                                destination index dead at 123 731 names. Four
                                bytes an edge is the whole price.
      3 PLACES   count = distinct searchable names. 12 bytes each:
                     0  4  u32  name_off, byte offset into STRINGS
                     4  4  u32  point_first, index into POINTS
                     8  4  u32  point_count
                 Sorted by name (plain byte order), so the table is stable
                 across builds and a future prefix search has somewhere to go.
      4 POINTS   count = candidate coordinates. 8 bytes each, as NODES.
      5 STRINGS  count = bytes. NUL-terminated uppercase ASCII, in PLACES order.

  Lengths are millimetres, not floats: they are the numbers Dijkstra adds up,
  they are computed once here in the pack's own frame, and an integer is what
  keeps the SUM identical across two compilers. u32 millimetres reaches
  4 295 km, which is longer than any OSM way segment.

  The sum, and not the whole router: router.c's snap() picks the endpoints with
  hypot(), which differs in the last ulp between platforms, so a near-tie can
  resolve differently. T-ONEWAY measures it -- 142 routable pairs on macOS, 146
  on the device, same pack, every run. See DESIGN.md 1.4.1.

  PROJECTION, exactly DESIGN.md 6.1 and exactly mktiles.py's:

      e = (lon - lon0) * m_per_deg_lon * cos(lat0)
      n = (lat - lat0) * m_per_deg_lat

  Unlike the tile pack, this one is NOT re-referenced to a route's frame. There
  is no route yet when FIND runs -- building one is what FIND is for -- so the
  pack's own frame is the only frame there is, and everything that crosses the
  boundary (the rider's fix on the way in, the routed path on the way out) is
  lat/lon. That is one fewer moving part than tiles_bind_route() needs, and it
  is why search.h has no bind call.

------------------------------------------------------------------------------
"""
import argparse
import hashlib
import json
import math
import os
import struct
import unicodedata
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mktiles                                                   # noqa: E402

MAGIC = b"BNAVROAD"
VERSION = 4
HEADER_BYTES = 64
SECT_ENTRY = 8
NSECT = 6
FLAG_ONEWAY = 1
COORD_SCALE = 10000000
NAME_NONE = 0xFFFFFFFF   # v3: was 0xFFFF, a u16
# One EDGES record on disk. 12 before v3 widened `name` from u16 to u32 -- and it
# is a NAME here because the section-offset table below computed it separately
# from the writer, so a literal 12 in one place and a `<IIHI` in the other put
# every section after EDGES two bytes per edge too early. --info found it.
EDGE_BYTES = 14

EDGE_ONEWAY = 1
# EDGES.flags bits 1-4: the road class, so the DEVICE can route a bicycle
# differently from a car (DESIGN.md 7.7). The pack has always known this and
# always thrown it away -- `highway` was read to decide routability and then
# discarded, which is why an offline bike route could use a motorway.
#
# Four bits, sixteen slots, nine used. Ordered coarse-to-fine so a future
# "avoid anything above class N" is a comparison rather than a set.
EDGE_CLASS_SHIFT = 1
EDGE_CLASS_MASK = 0x1E
# EDGES.flags bit 5: the way is tolled (DESIGN.md 7.7.1). Bit 5 because 1-4 are
# the class and the class is ordered coarse-to-fine to keep "above class N" a
# comparison -- widening it to five bits to squeeze toll in would have broken
# that for no gain when ten bits are still spare.
#
# ONLY `toll=yes` COUNTS. OSM also carries toll=no (an explicit statement that
# it is free, which is not a toll road) and a handful of
# toll:hgv / toll:motorcar variants that qualify it by vehicle. A navigator that
# treated any `toll` key as tolled would refuse free roads that had been
# surveyed carefully enough to say so, which is the opposite of what a careful
# surveyor deserves.
EDGE_TOLL = 0x20
TOLL_YES = {"yes", "true", "1"}
ROAD_CLASS = {
    "motorway": 1, "motorway_link": 1,
    "trunk": 2, "trunk_link": 2,
    "primary": 3, "primary_link": 3,
    "secondary": 4, "secondary_link": 4,
    "tertiary": 5, "tertiary_link": 5,
    "unclassified": 6,
    "residential": 7,
    "living_street": 8,
}

# route.h's constants, so the projection is bit-for-bit the navigator's.
M_PER_DEG_LAT = mktiles.M_PER_DEG_LAT
M_PER_DEG_LON = mktiles.M_PER_DEG_LON

# The same "tagged highway but not a road" list the basemap uses, imported
# rather than copied: a pack that draws a footbridge and a pack that routes up
# one are the same mistake, and the two tools must not drift apart. On
# osm-asok.json it matches nothing at all -- every one of the 557 ways is a
# real carriageway -- so the 2 803 nodes of DESIGN.md 1.4 are unaffected by it.
NOT_A_ROAD = mktiles.NOT_A_ROAD

# `oneway` values that mean "forward only" and "backward only". Anything else,
# including absent and the explicit `no`, is two-way. OSM also uses
# oneway=alternating / reversible, which are two-way as far as a bicycle
# navigator is concerned and are treated as such by falling through.
ONEWAY_FWD = {"yes", "true", "1"}
ONEWAY_REV = {"-1", "reverse"}

# mockup.py's search sampling: every third vertex of every way carrying a name.
PLACE_STEP = 3


# --------------------------------------------------------------------- input

def read_osm(path):
    """-> [(name, oneway, [(lat, lon), ...]), ...] for every routable way.

    `name` is the searchable form: name:en, else name, uppercased -- or None
    when there is no ASCII form at all, which the caller counts."""
    doc = json.load(open(path, "r", encoding="utf-8"))
    out = []
    dropped = set()
    for el in doc.get("elements", ()):
        if el.get("type") != "way" or "geometry" not in el:
            continue
        tags = el.get("tags", {})
        hw = tags.get("highway")
        if not hw or hw in NOT_A_ROAD:
            continue
        geom = [(p["lat"], p["lon"]) for p in el["geometry"]]
        if len(geom) < 2:
            continue
        raw = tags.get("name:en") or tags.get("name") or ""
        folded = ascii_form(raw)
        name = folded.upper() if folded else None
        if raw and name is None:
            dropped.add(raw)
        out.append((name or None, tags.get("oneway", ""), geom,
                    ROAD_CLASS.get(hw, 0),
                    1 if tags.get("toll", "") in TOLL_YES else 0))
    return out, dropped


def ascii_form(raw):
    """The searchable form of a name, or None.

    NFKD-FOLD BEFORE THE ASCII TEST, and that is a correction rather than a
    refinement. DESIGN.md 1.4.2 drops a name with no ASCII form because the 5x7
    font is A-Z/0-9 and there is no glyph for U+0E0B -- a shaping problem, not a
    spelling one. An ACCENTED LATIN letter is nothing like that case: it folds to
    the letter it is, losing nothing a rider reads, and the font uppercases
    everything anyway.

    A plain isascii() got that wrong in the field. Cafe Amazon is spelled with an
    acute accent in OSM at most PTT stations, so the branch 3.4 km from the
    rider's house was invisible while the ones somebody had typed without the
    accent were searchable nineteen kilometres away. Transliterated Thai has the
    same problem with macrons.

    Thai still drops, correctly: stripping combining marks from Thai leaves Thai.
    So do the letters NFKD will not decompose at all -- OE and the German sharp s
    among them -- and those want a transliteration table rather than a fold,
    which is a bigger claim than this function makes.

    The same rule lives in pbf2osm.py's _poi_name(). The two tools run separately
    and MUST agree: that one decides what reaches the extract, this one decides
    what reaches the pack.
    """
    if not raw:
        return None
    folded = "".join(c for c in unicodedata.normalize("NFKD", raw)
                     if not unicodedata.combining(c))
    return folded if folded.isascii() else None


def read_pois(path):
    """-> [(name, lat, lon), ...] for every named destination in the extract.

    A destination is an Overpass `node` element -- which is what pbf2osm.py
    emits for both POI nodes and the centroid of a POI area. It carries no
    geometry, takes part in no graph, and exists only to be searched for and
    then routed to by the same nearest-node snap a street hit uses.

    Deliberately NOT filtered by tag here. pbf2osm.py decided what counts as a
    destination; repeating that judgement in a second place is how the two
    drift apart. A node with a usable name is a destination."""
    doc = json.load(open(path, "r", encoding="utf-8"))
    out = []
    dropped = set()
    for el in doc.get("elements", ()):
        if el.get("type") != "node" or "lat" not in el or "lon" not in el:
            continue
        tags = el.get("tags", {})
        raw = tags.get("name:en") or tags.get("name") or ""
        if not raw:
            continue
        folded = ascii_form(raw)
        if not folded:
            dropped.add(raw)
            continue
        out.append((folded.upper(), float(el["lat"]), float(el["lon"])))
    return out, dropped


def read_ref(route_path, ref_spec, ways):
    """The projection reference. --route's first point, else --ref, else the
    first vertex of the first way -- all three deterministic, in that order of
    preference because a pack built for a route should share its frame."""
    if route_path:
        return mktiles.read_gpx(route_path)[0]
    if ref_spec:
        try:
            la, lo = (float(v) for v in ref_spec.replace(",", " ").split())
        except ValueError:
            raise SystemExit(f"mkpack: bad --ref {ref_spec!r}")
        return (la, lo)
    if not ways:
        raise SystemExit("mkpack: the extract has no routable ways")
    return ways[0][2][0]


# ----------------------------------------------------------------- the graph

class Graph:
    """Ways joined on exact shared coordinates, emitted directed.

    Node identity is the rounded 1e-7-degree pair, which is both what OSM
    guarantees for a shared node and what the pack stores -- so the identity
    the device reads back is the identity the join was made on, with no
    tolerance to argue about."""

    def __init__(self, frame, honour_oneway):
        self.frame = frame
        self.honour = honour_oneway
        self.index = {}      # (lat_e7, lon_e7) -> node index
        self.key = []        # node index -> (lat_e7, lon_e7)
        self.out = []        # node index -> [(to, len_mm, flags, name)]

    def _node(self, lat, lon):
        k = (round(lat * COORD_SCALE), round(lon * COORD_SCALE))
        i = self.index.get(k)
        if i is None:
            i = len(self.key)
            self.index[k] = i
            self.key.append(k)
            self.out.append([])
        return i

    def add_way(self, oneway, geom, name_i, cls=0, toll=0):
        fwd = oneway not in ONEWAY_REV
        bwd = oneway not in ONEWAY_FWD
        tagged = not (fwd and bwd)
        if not self.honour:
            fwd = bwd = True
        flags = EDGE_ONEWAY if (tagged and self.honour) else 0
        flags |= (cls << EDGE_CLASS_SHIFT) & EDGE_CLASS_MASK
        # Both directions of a tolled way are tolled: the flag is a property of
        # the carriageway, not of travelling along it one way.
        if toll:
            flags |= EDGE_TOLL
        for a, b in zip(geom, geom[1:]):
            ia, ib = self._node(*a), self._node(*b)
            if ia == ib:
                continue          # a way doubling back on its own node
            pa = self.frame.met(*a)
            pb = self.frame.met(*b)
            mm = int(round(math.dist(pa, pb) * 1000.0))
            if mm > 0xFFFFFFFF:
                raise SystemExit("mkpack: a way segment longer than 4295 km")
            if fwd:
                self.out[ia].append((ib, mm, flags, name_i))
            if bwd:
                self.out[ib].append((ia, mm, flags, name_i))

    def csr(self):
        """-> (adj, edges). `adj` is nnode+1 offsets; `edges` is flat."""
        adj = [0]
        edges = []
        for lst in self.out:
            edges.extend(lst)
            adj.append(len(edges))
        return adj, edges


# ----------------------------------------------------------------- the pack

def build(ways, pois, frame, honour_oneway):
    """-> (graph, places, points, strings) with every table already ordered."""
    # Names first, so an edge can carry its place index. Sorted by name, which
    # is what makes the table stable between builds and between extracts that
    # differ only in element order.
    by_name = {}
    for name, _ow, geom, _cls, _toll in ways:
        if name:
            by_name.setdefault(name, []).append(geom)
    # A destination joins the same table as a street, carrying a single point
    # where a street carries every third vertex. Nothing downstream needs to
    # know which it was: search_places() ranks by distance to the nearest
    # candidate point either way, and the router snaps whatever it is given to
    # the nearest graph node. That is why POIs cost no format change.
    #
    # A destination whose name is also a street name MERGES with it, which is
    # correct rather than merely convenient: "SILOM" is one thing to search
    # for, and the ranking then picks whichever of its points is nearest.
    for name, la, lo in pois:
        by_name.setdefault(name, []).append([(la, lo)])
    order = sorted(by_name)
    name_i = {n: i for i, n in enumerate(order)}
    # EDGES carries the place index as u32 since v3, so the ceiling is four
    # billion and this check is a formality. It stays because the failure it
    # guards is a SILENT WRAP that renames roads at random -- and because the
    # u16 it replaced hit its 65 534 limit for real, at 123 731 names, the first
    # time a nationwide destination index was tried.
    if len(order) > NAME_NONE - 1:
        raise SystemExit(
            f"mkpack: {len(order)} names, but EDGES stores a place index as "
            f"u32 and {NAME_NONE} means 'unnamed', so {NAME_NONE - 1} is the "
            f"limit.")

    strings = bytearray()
    points = []
    places = []
    for n in order:
        off = len(strings)
        strings += n.encode("ascii") + b"\0"
        first = len(points)
        for geom in by_name[n]:
            for la, lo in geom[::PLACE_STEP]:
                points.append((round(la * COORD_SCALE), round(lo * COORD_SCALE)))
        places.append((off, first, len(points) - first))

    g = Graph(frame, honour_oneway)
    for name, ow, geom, cls, toll in ways:
        g.add_way(ow, geom, name_i.get(name, NAME_NONE) if name else NAME_NONE,
                  cls, toll)
    return g, places, points, strings


def write_pack(path, frame, g, places, points, strings, nway, ndropped,
               honour_oneway):
    adj, edges = g.csr()
    nnode = len(g.key)
    sections = [
        (8 * nnode, nnode),
        (4 * len(adj), len(adj)),
        (EDGE_BYTES * len(edges), len(edges)),
        (12 * len(places), len(places)),
        (8 * len(points), len(points)),
        (len(strings), len(strings)),
    ]
    off = HEADER_BYTES + SECT_ENTRY * NSECT
    table = []
    for size, count in sections:
        table.append((off, count))
        off += size

    with open(path, "wb") as f:
        f.write(struct.pack("<8sHHHHIddddIII", MAGIC, VERSION, HEADER_BYTES,
                            FLAG_ONEWAY if honour_oneway else 0, NSECT, nway,
                            frame.lat0, frame.lon0, M_PER_DEG_LAT,
                            M_PER_DEG_LON, COORD_SCALE, ndropped, 0))
        for o, c in table:
            f.write(struct.pack("<II", o, c))
        for la, lo in g.key:
            f.write(struct.pack("<ii", la, lo))
        f.write(struct.pack(f"<{len(adj)}I", *adj))
        for to, mm, flags, nm in edges:
            f.write(struct.pack("<IIHI", to, mm, flags, nm))
        for o, first, count in places:
            f.write(struct.pack("<III", o, first, count))
        for la, lo in points:
            f.write(struct.pack("<ii", la, lo))
        f.write(bytes(strings))
    return nnode, len(edges)


def repeat_ways(ways, spec):
    """Tile the extract into an nx x ny grid, each copy offset by the extract's
    own span. A SCALE fixture and nothing else: the geometry is real but the
    city is not, so this exists purely to time a 10^5-node pack on the device
    when no larger real extract is to hand. Named in the output so nobody can
    mistake the result for a measurement of a real city."""
    try:
        nx, ny = (int(v) for v in spec.lower().split("x"))
    except ValueError:
        raise SystemExit(f"mkpack: bad --repeat {spec!r}")
    if nx < 1 or ny < 1 or nx * ny > 400:
        raise SystemExit("mkpack: --repeat out of range")
    lats = [la for _, _, g, _c in ways for la, _ in g]
    lons = [lo for _, _, g, _c in ways for _, lo in g]
    dla = (max(lats) - min(lats)) * 1.05
    dlo = (max(lons) - min(lons)) * 1.05
    out = []
    for iy in range(ny):
        for ix in range(nx):
            for name, ow, geom, cls, toll in ways:
                out.append((name, ow,
                            [(la + iy * dla, lo + ix * dlo) for la, lo in geom],
                            cls, toll))
    return out, nx, ny


# --------------------------------------------------------------------- info

def info(path):
    with open(path, "rb") as f:
        blob = f.read()
    (magic, ver, hb, flags, nsect, nway, lat0, lon0, klat, klon, scale,
     ndrop, _) = struct.unpack_from("<8sHHHHIddddIII", blob, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: not a beepy-nav road pack")
    tab = [struct.unpack_from("<II", blob, hb + SECT_ENTRY * i)
           for i in range(nsect)]
    names = ("nodes", "adj", "edges", "places", "points", "strings")
    print(f"{path}: v{ver}  {len(blob)} bytes  "
          f"oneway {'honoured' if flags & FLAG_ONEWAY else 'IGNORED'}")
    print(f"  reference {lat0:.7f},{lon0:.7f}  m/deg {klat:g},{klon:g}  "
          f"scale 1e{round(math.log10(scale))}")
    print(f"  {nway} ways indexed, {ndrop} names dropped (no ASCII form)")
    for nm, (off, count) in zip(names, tab):
        print(f"  {nm:8s} {count:8d} at {off}")
    # Tolled edges, counted rather than trusted. A 45 MB region pack built from
    # an extract whose `toll` tags were stripped is indistinguishable from a good
    # one by every other line here -- same version, same edge count -- and the
    # symptom on the device would be `tolls = avoid` quietly doing nothing.
    eoff, ecount = tab[2]
    ntoll = sum(1 for i in range(ecount)
                if struct.unpack_from("<H", blob, eoff + EDGE_BYTES * i + 8)[0]
                & EDGE_TOLL)
    print(f"  tolled edges {ntoll} of {ecount}"
          f"{'  -- NONE: was `toll` kept by the extract?' if not ntoll else ''}")
    off, count = tab[3]
    poff, _ = tab[4]
    soff, _ = tab[5]
    for i in range(min(count, 6)):
        no, pf, pc = struct.unpack_from("<III", blob, off + 12 * i)
        end = blob.index(b"\0", soff + no)
        print(f"    place {i}: {blob[soff + no:end].decode()!r}  "
              f"{pc} points at {pf}")
    if count > 6:
        print(f"    ... {count - 6} more")


# --------------------------------------------------------------------- main

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="build the road/name pack beepy-nav searches and routes "
                    "over (DESIGN.md 1.4)")
    ap.add_argument("--osm", help="Overpass JSON extract")
    ap.add_argument("-o", "--out", help="pack to write")
    ap.add_argument("--route", help="GPX whose first point is the reference")
    ap.add_argument("--ref", metavar="LAT,LON",
                    help="the reference point, when there is no route")
    ap.add_argument("--ignore-oneway", action="store_true",
                    help="build every edge both ways -- the WRONG pack, on "
                         "purpose, so T-ONEWAY can be shown to fail without "
                         "the fix")
    ap.add_argument("--repeat", metavar="NxM",
                    help="tile the extract into an NxM grid: a synthetic "
                         "scale fixture, never a real city")
    ap.add_argument("--pois-osm", metavar="FILE",
                    help="take destinations from THIS extract instead of "
                         "--osm's. The graph and the destination index do not "
                         "have to cover the same ground: a nationwide name "
                         "index is 5.8 MB resident and a nationwide graph would "
                         "be half a gigabyte, so the two are cut separately and "
                         "joined here (DESIGN.md 1.4.7). Road names still come "
                         "from --osm, because a road name is a property of a "
                         "way in the graph.")
    ap.add_argument("--no-pois", action="store_true",
                    help="index street names only, ignoring the destinations "
                         "in the extract -- the pack as it was before they "
                         "were searchable")
    ap.add_argument("--info", metavar="PACK", help="describe a pack and exit")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args(argv)

    if a.info:
        info(a.info)
        return 0
    if not (a.osm and a.out):
        ap.error("--osm and -o are both required")

    ways, dropped = read_osm(a.osm)
    pois, poi_dropped = ([], set()) if a.no_pois \
        else read_pois(a.pois_osm or a.osm)
    # One count, covering both, because the header's promise (DESIGN.md
    # 1.4) is 'how much of this pack you cannot be shown' -- and a
    # destination named only in Thai is exactly as invisible as a street
    # named only in Thai.
    dropped = dropped | poi_dropped
    ref = read_ref(a.route, a.ref, ways)
    frame = mktiles.Frame(ref[0], ref[1])
    tiled = None
    if a.repeat:
        ways, rx, ry = repeat_ways(ways, a.repeat)
        tiled = (rx, ry)

    g, places, points, strings = build(ways, pois, frame,
                                       not a.ignore_oneway)
    nnode, nedge = write_pack(a.out, frame, g, places, points, strings,
                              len(ways), len(dropped),
                              not a.ignore_oneway)
    size = os.path.getsize(a.out)
    if not a.quiet:
        print(f"mkpack: {len(ways)} routable ways"
              + (f" (tiled {tiled[0]}x{tiled[1]} -- SYNTHETIC)" if tiled else "")
              + f", reference {frame.lat0:.7f},{frame.lon0:.7f}",
              file=sys.stderr)
        print(f"mkpack: {nnode} nodes, {nedge} directed edges, "
              f"{len(places)} names, {len(points)} place points, "
              f"oneway {'IGNORED' if a.ignore_oneway else 'honoured'}",
              file=sys.stderr)
        if pois:
            print(f"mkpack: {len(pois)} destinations indexed alongside the "
                  f"streets", file=sys.stderr)
        print(f"mkpack: {len(dropped)} names dropped for having no ASCII form"
              + (": " + ", ".join(sorted(dropped)[:3]) if dropped else ""),
              file=sys.stderr)
        sha = hashlib.sha256(open(a.out, "rb").read()).hexdigest()
        print(f"mkpack: {a.out}: {size} bytes ({size / 1024.0:.0f} KB)",
              file=sys.stderr)
        print(f"mkpack: sha256 {sha}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

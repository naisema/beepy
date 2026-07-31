#!/usr/bin/env python3
"""mknetfix.py -- the router-response fixtures for T-NETROUTE / T-NETROUTE-BAD.

Mac-side, like everything else in tools/. The outputs are COMMITTED rather than
built by `make check`, because `make check` runs on the device and a gate that
needs python3 and this script on the Pi to have any fixtures at all is a gate
with a second thing to go wrong. `make netfix` regenerates them here; the rule
exists so that no fixture in this repo is a file somebody made by hand once
(DESIGN.md 10, and the N1 postmortem that cost a gate run over exactly that).

    beepy-nav/tests/net/valhalla-bike.json   INPUT, a real capture -- not ours
    beepy-nav/tests/net/osrm-bike.json       the same ride, in OSRM's shape
    beepy-nav/tests/net/osrm-poly6.json      ...encoded polyline6, which OSRM
                                             does when asked and cannot say
    beepy-nav/tests/net/bad-*.{json,html}    the seven ways a 200 OK lies

WHY THE OSRM FIXTURE IS TRANSCODED AND NOT CAPTURED. There is no OSRM server
this project may ask for a bicycle route: the public demo is car-only, ignores
the profile in the path, and returned the identical 53 km/h route for bike,
cycling, driving and foot (the measurement is in the phase 11 plan, and it is
why the config ships no default URL). So the geometry here is REAL -- it is
FOSSGIS Valhalla's own bicycle shape for the rider's own HOME->WORK -- re-encoded
into the format and vocabulary an OSRM server would have used for it. What is
synthetic is the wrapper, and the wrapper is exactly what the adapter reads.
"""
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
NET = os.path.join(HERE, "..", "beepy-nav", "tests", "net")
SRC = os.path.join(NET, "valhalla-bike.json")

# ------------------------------------------------------------------ polyline


def decode(s, prec):
    factor = 10.0**prec
    out, i, lat, lon = [], 0, 0, 0
    while i < len(s):
        for axis in (0, 1):
            shift, result = 0, 0
            while True:
                b = ord(s[i]) - 63
                i += 1
                result |= (b & 0x1F) << shift
                shift += 5
                if b < 0x20:
                    break
            d = ~(result >> 1) if (result & 1) else (result >> 1)
            if axis == 0:
                lat += d
            else:
                lon += d
        out.append((lat / factor, lon / factor))
    return out


def encode(pts, prec):
    factor = 10.0**prec
    out, plat, plon = [], 0, 0

    def one(v):
        v <<= 1
        if v < 0:
            v = ~v
        s = ""
        while v >= 0x20:
            s += chr((0x20 | (v & 0x1F)) + 63)
            v >>= 5
        return s + chr(v + 63)

    for lat, lon in pts:
        ilat, ilon = int(round(lat * factor)), int(round(lon * factor))
        out.append(one(ilat - plat))
        out.append(one(ilon - plon))
        plat, plon = ilat, ilon
    return "".join(out)


# ---------------------------------------------------------------- geometry

M_LAT, M_LON = 110540.0, 111320.0


def metres(pts):
    """Route length in the local-tangent plane route.c projects onto."""
    lat0, lon0 = pts[0]
    k = math.cos(math.radians(lat0))
    en = [((lo - lon0) * M_LON * k, (la - lat0) * M_LAT) for la, lo in pts]
    return sum(
        math.hypot(en[i][0] - en[i - 1][0], en[i][1] - en[i - 1][1])
        for i in range(1, len(en))
    )


def bearing(pts, a, b):
    lat0, lon0 = pts[0]
    k = math.cos(math.radians(lat0))
    p = [((pts[i][1] - lon0) * M_LON * k, (pts[i][0] - lat0) * M_LAT) for i in (a, b)]
    return round(math.degrees(math.atan2(p[1][0] - p[0][0], p[1][1] - p[0][1]))) % 360


# ------------------------------------------------------------------- OSRM
#
# One step per Valhalla maneuver, at the same shape indices, in OSRM's
# vocabulary. `modifier` is filled in because a real response has it, and the
# adapter ignores every one of them: kinds come from the geometry (DESIGN.md
# 7.9). The two that are NOT decoration are `type` = arrive, which is the only
# way to know a leg ended, and roundabout/exit roundabout, which is the only way
# to know two maneuvers are one thing.

OSRM_STEP = [
    #  type,             modifier,       name
    ("depart", "left", ""),
    ("turn", "left", ""),
    ("turn", "right", ""),
    ("roundabout", "straight", ""),
    ("exit roundabout", "straight", ""),
    ("turn", "right", "นบ.1009"),
    ("turn", "left", ""),
    ("turn", "left", ""),
    ("turn", "right", ""),
    ("turn", "left", ""),
    ("turn", "right", ""),
    ("arrive", "right", ""),
]


def osrm(pts, prec, idx, dist_m):
    steps = []
    for k, (ty, mod, name) in enumerate(OSRM_STEP):
        i0 = idx[k]
        i1 = idx[k + 1] if k + 1 < len(idx) else len(pts) - 1
        sub = pts[i0 : i1 + 1] if i1 > i0 else [pts[i0]]
        steps.append(
            {
                "geometry": encode(sub, prec),
                "maneuver": {
                    "bearing_after": bearing(pts, i0, min(i0 + 1, len(pts) - 1)),
                    "bearing_before": bearing(pts, max(i0 - 1, 0), i0),
                    # OSRM says lon,lat. Getting that pair the wrong way round
                    # puts a Bangkok route in Somalia, which is the one bug in
                    # this file a fixture can catch by itself.
                    "location": [round(pts[i0][1], prec), round(pts[i0][0], prec)],
                    "type": ty,
                    "modifier": mod,
                },
                "mode": "cycling",
                "name": name,
                "distance": round(metres(sub) if len(sub) > 1 else 0.0, 1),
                "duration": round((metres(sub) if len(sub) > 1 else 0.0) / 4.6, 1),
            }
        )
    return {
        "code": "Ok",
        "waypoints": [
            {
                "hint": "",
                "name": "",
                "location": [round(pts[0][1], prec), round(pts[0][0], prec)],
                "distance": 0.0,
            },
            {
                "hint": "",
                "name": "",
                "location": [round(pts[-1][1], prec), round(pts[-1][0], prec)],
                "distance": 0.0,
            },
        ],
        "routes": [
            {
                "geometry": encode(pts, prec),
                "legs": [
                    {
                        "steps": steps,
                        "summary": "",
                        "distance": round(dist_m, 1),
                        "duration": round(dist_m / 4.6, 1),
                        "weight": round(dist_m / 4.6, 1),
                    }
                ],
                "weight_name": "cyclability",
                "distance": round(dist_m, 1),
                "duration": round(dist_m / 4.6, 1),
                "weight": round(dist_m / 4.6, 1),
            }
        ],
    }


# -------------------------------------------------------------------- main


def write(name, data, binary=False):
    path = os.path.join(NET, name)
    if binary:
        with open(path, "wb") as f:
            f.write(data)
        n = len(data)
    else:
        with open(path, "w", encoding="utf-8") as f:
            f.write(data)
        n = len(data.encode("utf-8"))
    print("  %-24s %7d bytes" % (name, n))


def main():
    raw = open(SRC, "rb").read()
    src = json.loads(raw)
    leg = src["trip"]["legs"][0]
    pts = decode(leg["shape"], 6)
    idx = [m["begin_shape_index"] for m in src["trip"]["legs"][0]["maneuvers"]]
    if len(idx) != len(OSRM_STEP):
        sys.exit("mknetfix: %d maneuvers but %d steps hard-coded" % (len(idx), len(OSRM_STEP)))
    print("mknetfix: %d points, %.1f m tangent-plane, %d maneuvers"
          % (len(pts), metres(pts), len(idx)))

    # OSRM reports metres, always, and its default geometry precision is 5.
    # The distance it states is the ONLY thing that lets the adapter tell a
    # polyline5 body from a polyline6 one, so it is computed from the geometry
    # after the round trip through the encoder rather than copied from Valhalla.
    p5 = decode(encode(pts, 5), 5)
    write("osrm-bike.json", json.dumps(osrm(p5, 5, idx, metres(p5)), indent=1,
                                       ensure_ascii=False) + "\n")
    p6 = decode(encode(pts, 6), 6)
    write("osrm-poly6.json", json.dumps(osrm(p6, 6, idx, metres(p6)), indent=1,
                                        ensure_ascii=False) + "\n")

    # ------------------------------------------------- the seven bad replies
    #
    # None of these is hypothetical. A truncated body is a connection dropped
    # in a lift; an empty one and an HTML one are both a captive portal; a 200
    # carrying an error object is what Valhalla does when there is no path; and
    # a shape that does not decode is what a proxy that "helpfully" rewrote the
    # body leaves behind.
    write("bad-empty.json", b"", binary=True)
    write("bad-truncated.json", raw[: len(raw) * 2 // 3], binary=True)
    write("bad-malformed.json",
          '{"trip": {"legs": [{"shape": "o`nnYovrm~D", '
          '"maneuvers": [],}], "summary": {"length": 4.969}, '
          '"units": "kilometers"}}\n')
    write("bad-noroute.json",
          json.dumps({"error_code": 442,
                      "error": "No path could be found for input",
                      "status_code": 400,
                      "status": "Bad Request"}, indent=1) + "\n")
    write("bad-captive.html",
          "<!DOCTYPE html>\n<html><head><title>Sign in</title></head>\n"
          "<body><h1>Wi-Fi login required</h1>\n"
          "<form action=\"/login\"><input name=\"user\"></form>\n"
          "</body></html>\n")
    # A shape whose alphabet is wrong: '!' is 33, below the 63 the encoding
    # subtracts, so a decoder that does not check produces a route in the
    # Southern Ocean instead of refusing.
    write("bad-shape.json",
          json.dumps({"trip": {"units": "kilometers",
                               "summary": {"length": 4.969},
                               "legs": [{"shape": "o`nnY!ovrm~D",
                                         "summary": {"length": 4.969},
                                         "maneuvers": []}]}}, indent=1) + "\n")
    # Two points 30 cm apart: valid JSON, valid polyline, and not a route --
    # route_prepare() refuses anything shorter than the 50 m its bearing arms
    # need, and the refusal has to arrive as a message and not as a crash.
    write("bad-tooshort.json",
          json.dumps({"trip": {"units": "kilometers",
                               "summary": {"length": 0.0003},
                               "legs": [{"shape": encode(
                                   [(13.88495, 100.37849),
                                    (13.884953, 100.378492)], 6),
                                         "summary": {"length": 0.0003},
                                         "maneuvers": []}]}}, indent=1) + "\n")


if __name__ == "__main__":
    main()

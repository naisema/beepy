#!/usr/bin/env python3
"""
Emit oversize.gpx -- more track points than DESIGN.md 7.1's 20 000 cap, so
the decimation path is exercised for real.

Generated rather than committed: 25 000 points is ~1.2 MB, which is larger
than the rest of this repository put together and would be rsynced to the
device on every `make sync`. The Makefile builds it before test_gpx runs and
.gitignore keeps it out.

The shape is a slow arc, densely sampled at ~2 m, so decimating 25 000 points
to 20 000 must not move either end and must barely change the length -- both
of which the unit test asserts.

    python3 beepy-nav/tests/gpx/gen_oversize.py [OUT]
"""
import math
import os
import sys

N = 25000
LAT0, LON0 = 13.737500, 100.561000
MLAT = 1.0 / 110540.0
MLON = 1.0 / (111320.0 * math.cos(math.radians(LAT0)))
RADIUS = 8000.0                       # metres; a gentle 90-degree arc


def main(out):
    with open(out, "w") as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<gpx version="1.1" creator="gen_oversize.py">\n'
                "  <trk>\n    <name>Oversize Arc</name>\n    <trkseg>\n")
        for i in range(N):
            a = (math.pi / 2) * i / (N - 1)
            e = RADIUS * math.sin(a)
            n = RADIUS * (1.0 - math.cos(a))
            f.write(f'      <trkpt lat="{LAT0 + n * MLAT:.7f}" '
                    f'lon="{LON0 + e * MLON:.7f}"/>\n')
        f.write("    </trkseg>\n  </trk>\n</gpx>\n")
    print(f"{out}: {N} points, {os.path.getsize(out)} bytes")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else
         os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "oversize.gpx"))

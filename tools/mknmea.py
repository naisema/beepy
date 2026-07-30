#!/usr/bin/env python3
"""
mknmea.py -- synthetic NMEA rides, seeded and deterministic.

DESIGN.md 10 wants the route maths verified by "replaying a GPX ride against
its own route". There is no such ride: the device's receiver has never been
carried along any of these routes. So one is manufactured -- a bicycle walked
along the GPX at a chosen speed, emitting the same RMC + VTG + GGA at the same
1 Hz the u-blox does (DESIGN.md 3), with correct XOR checksums.

Deterministic by construction: every random draw comes from one seeded
generator, so a fixture regenerated on another machine is byte-identical and a
replay assertion that passes today passes tomorrow.

    tools/mknmea.py --gpx R.gpx --speed 25 --hz 1 -o ride.nmea
    tools/mknmea.py --gpx R.gpx --detour 100:400:200 -o detour.nmea
    tools/mknmea.py --gpx R.gpx --speed 0 --duration 600 -o stationary.nmea

  --gpx FILE          the route to ride (or --demo for the built-in one)
  --speed KMH         constant cruise (default 25)
  --profile A:V,B:V   piecewise-linear speed profile, metres along : km/h
  --brake             slow to 13 km/h within 45 m of a corner over 30 deg
  --hz N              sentences per second (default 1)
  --seed N            RNG seed (default 1)
  --noise SIGMA       gaussian position jitter, metres
  --hold-jitter M     jitter while holding still, metres (default 3); 0 is
                    the deliberately unrealistic fixture DESIGN.md 6.4 needs
--stationary MIN[@AT]   hold still for MIN minutes at AT metres along
                          (default at the start), with 1-3 m of jitter
  --detour OFF:AT:LEN     splice an excursion peaking OFF metres off the
                          route, starting at AT and lasting LEN metres
  --dropout A:B       emit nothing between second A and second B
  --nofix A:B         emit V-status / quality-0 sentences between A and B
  --duration SEC      stop after SEC seconds even if the route is unfinished
  --start HH:MM:SS    UTC of the first sentence (default 09:40:00)
  --date DDMMYY       RMC date field (default 290726)
  -o FILE             output (default stdout)
"""
import math
import random
import re
import sys

M_LAT = 110540.0
M_LON = 111320.0

# The mockup's own geometry, in metres, referenced at the Asok junction. Used
# when there is no GPX to hand -- it has the staircase the NAV page was drawn
# around, so a demo replay looks like the design.
DEMO_LAT0, DEMO_LON0 = 13.7375, 100.561
DEMO_ROUTE_M = [(0, -900), (0, -400), (40, -150), (40, 260), (190, 260),
                (190, 620), (430, 700), (560, 980), (430, 1180), (120, 1260),
                (-220, 1210), (-430, 1040), (-470, 780), (-330, 560),
                (-120, 520)]


def read_gpx(path):
    """Every <trkpt>/<rtept> lat/lon, in file order. Attributes in either
    order, which is the same tolerance the C scanner has."""
    src = open(path).read()
    pts = []
    for m in re.finditer(r"<(?:\w+:)?(?:trkpt|rtept)\s([^>]*)>", src):
        attrs = dict(re.findall(r"(\w+)\s*=\s*[\"']([^\"']*)[\"']", m.group(1)))
        if "lat" in attrs and "lon" in attrs:
            pts.append((float(attrs["lat"]), float(attrs["lon"])))
    if len(pts) < 2:
        raise SystemExit(f"{path}: fewer than two points")
    return pts


def to_metres(pts):
    lat0, lon0 = pts[0]
    k = math.cos(math.radians(lat0))
    return (lat0, lon0,
            [((lon - lon0) * M_LON * k, (lat - lat0) * M_LAT) for lat, lon in pts])


def to_latlon(lat0, lon0, e, n):
    k = math.cos(math.radians(lat0))
    return lat0 + n / M_LAT, lon0 + e / (M_LON * k)


def densify(en, step=1.0):
    """Resample to ~`step` metres and return (points, cumulative distance)."""
    out, cum = [en[0]], [0.0]
    for a, b in zip(en, en[1:]):
        d = math.dist(a, b)
        k = max(1, int(round(d / step)))
        for i in range(1, k + 1):
            t = i / k
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
            cum.append(cum[-1] + d / k)
    return out, cum


def at_along(dense, cum, s):
    """Position and unit tangent at distance s."""
    if s <= 0:
        i = 0
    elif s >= cum[-1]:
        i = len(dense) - 2
    else:
        lo, hi = 0, len(cum) - 1
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if cum[mid] <= s:
                lo = mid
            else:
                hi = mid
        i = lo
    a, b = dense[i], dense[min(i + 1, len(dense) - 1)]
    seg = cum[min(i + 1, len(cum) - 1)] - cum[i]
    t = (s - cum[i]) / seg if seg > 1e-9 else 0.0
    p = (a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t)
    d = math.hypot(b[0] - a[0], b[1] - a[1])
    tan = ((b[0] - a[0]) / d, (b[1] - a[1]) / d) if d > 1e-9 else (0.0, 1.0)
    return p, tan


def corners(dense, cum, arm=25.0, min_deg=30.0):
    """Distances along the route where the bearing swings more than min_deg
    -- the same test route.c's deriver applies, so --brake slows for the
    corners the panel is about to announce."""
    out, s = [], arm
    while s < cum[-1] - arm:
        (ax, ay), _ = at_along(dense, cum, s - arm)
        (bx, by), _ = at_along(dense, cum, s)
        (cx, cy), _ = at_along(dense, cum, s + arm)
        b1 = math.degrees(math.atan2(bx - ax, by - ay))
        b2 = math.degrees(math.atan2(cx - bx, cy - by))
        d = (b2 - b1 + 180) % 360 - 180
        if abs(d) > min_deg:
            out.append(s)
        s += 10.0
    return out


def nmea(body):
    ck = 0
    for c in body:
        ck ^= ord(c)
    return f"${body}*{ck:02X}\r\n"


def dm(v, deg_digits):
    """Decimal degrees -> NMEA ddmm.mmmm plus its hemisphere letter."""
    hemi = ("N" if v >= 0 else "S") if deg_digits == 2 else \
           ("E" if v >= 0 else "W")
    v = abs(v)
    d = int(v)
    return f"{d:0{deg_digits}d}{(v - d) * 60:07.4f}", hemi


def sentences(t_utc, lat, lon, kmh, course, fix, sats, hdop, date):
    hh = int(t_utc // 3600) % 24
    mm = int(t_utc // 60) % 60
    ss = t_utc - (int(t_utc) // 60) * 60
    tm = f"{hh:02d}{mm:02d}{ss:05.2f}"
    la, ns = dm(lat, 2)
    lo, ew = dm(lon, 3)
    kn = kmh / 1.852
    st = "A" if fix else "V"
    out = [nmea(f"GPRMC,{tm},{st},{la},{ns},{lo},{ew},{kn:.2f},{course:.1f},"
                f"{date},,,{'A' if fix else 'N'}")]
    out.append(nmea(f"GPVTG,{course:.1f},T,,M,{kn:.2f},N,{kmh:.2f},K,"
                    f"{'A' if fix else 'N'}"))
    out.append(nmea(f"GPGGA,{tm},{la},{ns},{lo},{ew},{1 if fix else 0},"
                    f"{sats:02d},{hdop:.1f},12.0,M,-30.0,M,,"))
    return out


def parse_range(s, what):
    a, b = s.split(":")
    return float(a), float(b)


def main(argv):
    gpx = None
    demo = False
    speed = 25.0
    profile = None
    brake = False
    hz = 1.0
    seed = 1
    noise = 0.0
    hold_jitter = 3.0
    stationary = None
    detour = None
    dropout = nofix = None
    duration = None
    start = "09:40:00"
    date = "290726"
    out = None

    it = iter(argv)
    for a in it:
        if a == "--gpx":
            gpx = next(it)
        elif a == "--demo":
            demo = True
        elif a == "--speed":
            speed = float(next(it))
        elif a == "--profile":
            profile = [tuple(float(v) for v in p.split(":"))
                       for p in next(it).split(",")]
        elif a == "--brake":
            brake = True
        elif a == "--hz":
            hz = float(next(it))
        elif a == "--seed":
            seed = int(next(it))
        elif a == "--noise":
            noise = float(next(it))
        elif a == "--hold-jitter":
            hold_jitter = float(next(it))
        elif a == "--stationary":
            v = next(it)
            mins, _, at = v.partition("@")
            stationary = (float(mins) * 60.0, float(at or 0.0))
        elif a == "--detour":
            off, at, ln = (float(v) for v in next(it).split(":"))
            detour = (off, at, ln)
        elif a == "--dropout":
            dropout = parse_range(next(it), a)
        elif a == "--nofix":
            nofix = parse_range(next(it), a)
        elif a == "--duration":
            duration = float(next(it))
        elif a == "--start":
            start = next(it)
        elif a == "--date":
            date = next(it)
        elif a == "-o":
            out = next(it)
        else:
            raise SystemExit(__doc__.strip())

    rng = random.Random(seed)
    if demo or not gpx:
        lat0, lon0 = DEMO_LAT0, DEMO_LON0
        en = DEMO_ROUTE_M
    else:
        lat0, lon0, en = to_metres(read_gpx(gpx))
    dense, cum = densify(en)
    total = cum[-1]
    turns = corners(dense, cum) if brake else []

    h, m, s = (int(v) for v in start.split(":"))
    t_utc = h * 3600 + m * 60 + s
    dt = 1.0 / hz

    def speed_at(sm):
        if profile:
            if sm <= profile[0][0]:
                v = profile[0][1]
            elif sm >= profile[-1][0]:
                v = profile[-1][1]
            else:
                v = profile[-1][1]
                for (a0, v0), (a1, v1) in zip(profile, profile[1:]):
                    if a0 <= sm <= a1:
                        f = (sm - a0) / (a1 - a0) if a1 > a0 else 0.0
                        v = v0 + (v1 - v0) * f
                        break
        else:
            v = speed
        if brake:
            for c in turns:
                if -20.0 < c - sm < 45.0:
                    v = min(v, 13.0)
        return v

    lines = []
    sm, t = 0.0, 0.0
    held = 0.0
    prev = None
    det_dir = None
    while True:
        if duration is not None and t > duration + 1e-9:
            break
        if duration is None and sm >= total - 0.5:
            break
        if duration is None and stationary is None and speed <= 0 and not profile:
            raise SystemExit("--speed 0 needs --duration")

        (e, n), (tx, ty) = at_along(dense, cum, sm)
        course = math.degrees(math.atan2(tx, ty)) % 360.0
        v = speed_at(sm)

        # Holding still: a few metres of jitter and no reported speed, which
        # is what a real receiver does at a light and what makes the heading
        # freeze of DESIGN.md 6.1 worth having.
        #
        # --hold-jitter 0 turns it off. That is not realistic and is not meant
        # to be: it is the fixture DESIGN.md 6.4's frame skip needs. A display
        # that has genuinely stopped changing must stop being sent, and under
        # even a metre of jitter the map still shifts by a fraction of a pixel
        # and the claim cannot be stated exactly.
        holding = stationary is not None and sm >= stationary[1] and \
            held < stationary[0]
        if holding:
            v = 0.0
            if hold_jitter > 0.0:
                e += rng.uniform(-hold_jitter, hold_jitter)
                n += rng.uniform(-hold_jitter, hold_jitter)
            held += dt

        # A detour is a real excursion, not a teleport: the offset eases out
        # and back over LEN metres, so the latch sees it cross 40 m, dwell,
        # and cross back under 25 m.
        #
        # The direction is the tangent where the excursion STARTS, held fixed
        # for its length. Recomputing it per step looks tidier and is wrong:
        # if the route corners inside the detour the perpendicular swings
        # ninety degrees and the rider teleports across the junction, which
        # is a discontinuity no receiver ever emits.
        if detour:
            off, at, ln = detour
            if at <= sm <= at + ln:
                if det_dir is None:
                    det_dir = (-ty, tx)
                f = math.sin(math.pi * (sm - at) / ln)
                e += det_dir[0] * off * f
                n += det_dir[1] * off * f

        if noise > 0.0:
            e += rng.gauss(0.0, noise)
            n += rng.gauss(0.0, noise)

        # Course as actually travelled, so a detour turns the map with it.
        if prev is not None and math.dist(prev, (e, n)) > 0.3:
            course = math.degrees(math.atan2(e - prev[0], n - prev[1])) % 360.0
        prev = (e, n)

        lat, lon = to_latlon(lat0, lon0, e, n)
        quiet = dropout and dropout[0] <= t <= dropout[1]
        lost = nofix and nofix[0] <= t <= nofix[1]
        if not quiet:
            lines += sentences(t_utc + t, lat, lon, 0.0 if holding else v,
                               course, 0 if lost else 1,
                               4 if lost else 9, 3.4 if lost else 0.9, date)

        if not holding:
            sm += v / 3.6 * dt
        t += dt

    text = "".join(lines)
    if out:
        open(out, "w", newline="").write(text)
        print(f"{out}: {len(lines)} sentences, {len(lines)//3} epochs, "
              f"{t:.0f} s, {min(sm, total):.0f} m of {total:.0f} m",
              file=sys.stderr)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main(sys.argv[1:])

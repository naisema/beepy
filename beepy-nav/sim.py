#!/usr/bin/env python3
"""
Ride simulation for beepy-nav: renders the real Asok -> Sukhumvit -> soi route
(from osm-asok.json) as an animation, frame by frame, exactly as the C program
would draw it -- live countdown, course-up rotation through the turns, stepped
auto-zoom, the ridden track growing behind the marker, clock ticking.

This is the design's dynamic behaviour made visible before any C exists:
 - heading = direction 8 m ahead on the route, circular-EWMA smoothed (a=0.3
   per frame at 6 fps), frozen below 3 km/h  (DESIGN.md 6.1)
 - zoom = auto ladder keyed to the distance to the active cue (6.1)
 - speed = 28 km/h cruise, braking to ~13 km/h for each turn, 1.5 m/s^2
   accelerator, full stop at the destination
 - cue model: active cue announced on the panel, following cue in the THEN
   slot and pinned on the map (1.1)

Output: sim-anim.bin -- packed 1-bit frames, 12000 bytes each (400x240, MSB
first, 1 = white), plus three preview PNGs. Play on the Beepy with fbplay.c.

Usage: python3 sim.py [--fps 6] [--preview-only]
"""

import math
import sys

import mockup as m

FPS = 6
CRUISE = 28 / 3.6                 # m/s
TURNSPD = 13 / 3.6
ACCEL = 1.5                       # m/s^2
BRAKE_ZONE = 45.0                 # m before a turn to be at TURNSPD


def densify(pts, step=1.0):
    """Resample at `step` metres. Returns (points, along, per-vertex along)."""
    out, marks = [pts[0]], [0.0]
    for a, b in zip(pts, pts[1:]):
        seg = math.dist(a, b)
        for i in range(1, max(1, int(seg / step)) + 1):
            t = i / max(1, int(seg / step))
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
        marks.append(marks[-1] + seg)
    along = [0.0]
    for a, b in zip(out, out[1:]):
        along.append(along[-1] + math.dist(a, b))
    return out, along, marks


def wrap(a):
    while a > math.pi:
        a -= 2 * math.pi
    while a < -math.pi:
        a += 2 * math.pi
    return a


def render_frame(streets, rt, dense, along, cum_v, s, heading, raw, v, t):
    pts = rt["pts"]
    cues = rt["cues"]

    # Active cue: first one still ahead. The following one feeds THEN + pin.
    cue_along = [cum_v[i] for i, _ in cues]
    act = next((k for k, ca in enumerate(cue_along) if ca > s + 1.0),
               len(cues) - 1)
    cue_d = max(0.0, cue_along[act] - s)
    kind = cues[act][1]
    nxt = cues[act + 1] if act + 1 < len(cues) else None

    # Position + course-up rotation.
    idx = min(range(len(along)), key=lambda i: abs(along[i] - s))
    pos = dense[idx]
    theta = heading                  # +heading: see page_nav in mockup.py

    c = m.Canvas()
    cx = m.MAP_X + (m.W - m.MAP_X) / 2
    cy = m.H * 0.72
    mpp = m.auto_zoom(cue_d, cy)
    box = (m.MAP_X, 0, m.W - 1, m.H - 1)

    m.draw_streets(c, pos, mpp, cx, cy, theta, box, streets)

    # Ridden track: every ~10 m of what is behind, ending at the marker.
    back = [dense[i] for i in range(0, idx, 10)] + [pos]
    m.dashed(c, m.clip_poly(m.project(back, pos, mpp, cx, cy, theta), box),
             width=1)
    c.flush_hairlines()

    # Route ahead: the remaining original vertices, corner-rounded.
    ahead = [pos] + [p for p, a in zip(pts, cum_v) if a > s + 1.0]
    m.cased_route(c, m.clip_poly(m.project(m.round_corners(ahead),
                                           pos, mpp, cx, cy, theta), box),
                  outer=10, inner=6)

    if nxt:
        tx, ty = m.project([pts[nxt[0]]], pos, mpp, cx, cy, theta)[0]
        if m.MAP_X + 10 < tx < m.W - 10 and 24 < ty < m.H - 10:
            m.pin(c, tx, ty)
    ex, ey = m.project([pts[-1]], pos, mpp, cx, cy, theta)[0]
    if m.MAP_X + 12 < ex < m.W - 12 and 24 < ey < m.H - 4:
        m.arrow(c, ex - 9, ey - 19, 19, "dest")

    # Chevron gets the residual angle: actual course minus the smoothed map
    # rotation, so it stays on the road while the course-up rotation catches up.
    m.position_marker(c, cx, cy, ang=wrap(raw - heading))
    m.compass(c, m.MAP_X + 21, 27, theta)
    m.speed_badge(c, m.W - 33, 33, v * 3.6)
    m.scale_bar(c, m.MAP_X + 7, m.H - 8, mpp)

    m.NAV["kind"] = kind
    m.NAV["turn"] = int(round(cue_d / 10) * 10) if cue_d > 15 else int(cue_d)
    m.NAV["then_d"] = (f"{int(round((cue_along[act + 1] - cue_along[act]) / 10) * 10)}M"
                       if nxt else "")
    m.NAV["then_kind"] = nxt[1] if nxt else "dest"
    m.NAV["clock"] = f"{9:02d}:{40 + int(t) // 60:02d}"
    m.turn_panel(c)
    c.rect(m.PANEL_W, 0, m.PANEL_W, m.H - 1, m.INK)
    return m.resolve(c.img)


def main():
    fps = FPS
    if "--fps" in sys.argv:
        fps = int(sys.argv[sys.argv.index("--fps") + 1])
    dt = 1.0 / fps

    streets, rt = m.load_osm()
    dense, along, cum_v = densify(rt["pts"])
    total = along[-1]
    cue_along = [cum_v[i] for i, _ in rt["cues"]]
    turn_cues = cue_along[:-1]                      # dest handled by the stop

    print(f"route {total:.0f} m, cues at " +
          ", ".join(f"{a:.0f}" for a in cue_along))

    s, v, t = 0.0, CRUISE, 0.0
    heading = math.atan2(dense[8][0] - dense[0][0], dense[8][1] - dense[0][1])
    frames, previews = [], {}

    # The pre-ride flow: FIND being typed on the Beepy's own keyboard, the hit
    # narrowing to one, then the routed overview waiting on ENTER = GO.
    if "--no-intro" not in sys.argv:
        for q, hold in (("S", 4), ("SO", 3), ("SOI", 3), ("SOI 2", 3),
                        ("SOI 23", 12)):
            frames += [m.render_search(q, dense[0]).tobytes()] * hold
        frames += [m.render_confirm(rt).tobytes()] * (3 * fps)
        print(f"intro: {len(frames)} frames (search + confirm)")
    while s < total - 0.5:
        # Target speed: brake for an upcoming turn, crawl the last metres,
        # stop at the destination.
        tv = CRUISE
        for ca in turn_cues:
            gap = ca - s
            if -20 < gap < BRAKE_ZONE:
                tv = min(tv, TURNSPD)
        if total - s < 40:
            tv = min(tv, math.sqrt(max(0.1, 2 * ACCEL * (total - s))))
        v += max(-ACCEL * dt * 2.5, min(ACCEL * dt, tv - v))
        s += v * dt
        t += dt

        # Heading: direction to the point 8 m ahead, EWMA, frozen when slow.
        idx = min(int(s), len(dense) - 9)
        raw = math.atan2(dense[idx + 8][0] - dense[idx][0],
                         dense[idx + 8][1] - dense[idx][1])
        if v > 3 / 3.6:
            heading += wrap(raw - heading) * 0.3
        else:
            raw = heading

        frames.append(render_frame(streets, rt, dense, along, cum_v,
                                   s, heading, raw, v, t).tobytes())
        for tag, at in (("start", 5.0), ("turn", cue_along[0]),
                        ("soi", cue_along[1] + 30)):
            if tag not in previews and s >= at:
                previews[tag] = len(frames) - 1
        if len(frames) % 60 == 0:
            print(f"  {len(frames)} frames, s={s:.0f} m, v={v*3.6:.0f} km/h")

    frames += [frames[-1]] * (2 * fps)              # hold the arrival
    with open("sim-anim.bin", "wb") as f:
        f.writelines(frames)
    for tag, i in previews.items():
        from PIL import Image
        img = Image.frombytes("1", (m.W, m.H), frames[i])
        (img.convert("L").resize((m.W * 3, m.H * 3), Image.NEAREST)
            .convert("1").save(f"sim-{tag}-3x.png"))
    print(f"sim-anim.bin: {len(frames)} frames at {fps} fps "
          f"({len(frames) / fps:.0f} s, {len(frames) * 12000 / 1e6:.1f} MB), "
          f"previews: {', '.join(previews)}")


if __name__ == "__main__":
    main()

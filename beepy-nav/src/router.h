/* beepy-nav/src/router.h -- on-device Dijkstra over the road pack
 * (DESIGN.md 1.4).
 *
 * "Routing is on-device Dijkstra over the pack's road graph -- ways joined on
 * exact shared coordinates." The graph is already built (tools/mkpack.py did
 * the joining), already DIRECTED, and already in CSR form, so this file is the
 * search and nothing else: a binary heap, one relaxation per edge, and the
 * result written into a route_t so that cues, snapping, the NAV page and the
 * OVERVIEW page cannot tell a routed destination from a GPX.
 *
 * ONEWAY IS THE POINT. mockup.py's router ignored it and said so; a navigator
 * that sends a rider the wrong way up a Bangkok primary is worse than no
 * navigator. The restriction lives in the pack -- a oneway way contributes no
 * reverse edge -- so honouring it costs this file nothing at all, and
 * tests/test_search.c's T-ONEWAY checks every hop of 203 routes against the
 * pack's own adjacency rather than trusting that.
 *
 * Portable C, libc + libm. It compiles and runs in the host lane.
 */
#ifndef BEEPY_NAV_ROUTER_H
#define BEEPY_NAV_ROUTER_H

#include "route.h"
#include "search.h"

/* Endpoints attach to NODES, not to the interiors of segments: a node snap is
 * one comparison per node and an edge snap is a projection per edge, for a
 * difference of at most half a node spacing -- 10 to 30 m in OSM city data, and
 * the route's first leg swallows it.
 *
 * Every node within ROUTER_SNAP_M is a candidate, though, and that is not
 * decoration. Measured on osm-asok.json: the nearest node to DESIGN.md 1.4's
 * own start point sits on Ratchadaphisek Road's SOUTHBOUND carriageway, which
 * is oneway, and from there the destination is unreachable -- 11 of 2 803 nodes
 * are. The northbound carriageway is 20 m away. So a single-nearest snap turns
 * "honour oneway" into "refuse to route", and the fix is to let the search
 * start from any node the rider could plausibly be on and let Dijkstra decide.
 * 25 m is the distance route.h already calls being on the road
 * (ROUTE_OFF_CLEAR_M).
 *
 * With one node inside the radius this IS a nearest-node snap; the cap keeps a
 * dense city pack from seeding a hundred sources. */
#define ROUTER_SNAP_M 25.0
#define ROUTER_SNAP_MAX 32

/* Vertices a routed path may have. A 4 000-node path at OSM's ~20 m spacing is
 * 80 km, which is further than this device's battery goes. */
#define ROUTER_MAXPT 8192

/* The shortest legal path from (se, sn) to (de, dn), both in the PACK's frame
 * (search.h: roads_project()). Fills `nodes` with the node indices in order and
 * returns how many; -1 on failure with a one-line reason in `why`.
 *
 * `*total_m` is the whole polyline including the two access legs -- the walk
 * from the rider's actual position to the first node and from the last node to
 * the point that was asked for -- because that is the distance the rider will
 * cover and therefore the one CONFIRM must show. */
int router_path(const roads_t *g, double se, double sn, double de, double dn,
                int mode, int *nodes, int maxn, double *total_m, char *why, int nwhy);

/* The same, as a route_t: the requested start, the path, the requested
 * destination, then route_prepare() + route_cues_derive() + route_cues_finish()
 * -- the identical preparation route_load() gives a GPX, which is what makes
 * everything downstream unable to tell the two apart. `name` becomes the
 * route's name (the CONFIRM title and the OVERVIEW title line); NULL gives
 * "DESTINATION".
 *
 * 0 on success. On failure `out` is left initialised-and-empty and `why`
 * carries the reason -- an unreachable destination is a message on the panel,
 * never a crash and never a half-built route. */
/* How far a destination may be from the nearest road in the pack before the
 * offline answer stops being about that destination (DESIGN.md 7.7). A POI
 * centroid is 50-300 m from its gate; 2 km is generous for that and far short
 * of the pack edge. */
#define ROUTER_MAX_SNAP_M 2000.0

int router_to(const roads_t *g, double se, double sn, double de, double dn,
              int mode, const char *name, route_t *out, char *why, int nwhy);

#endif /* BEEPY_NAV_ROUTER_H */

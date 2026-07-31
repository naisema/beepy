/* beepy-nav/src/netroute.h -- making a route out of what a router said.
 *
 * netfetch.c produces BYTES and deliberately knows nothing about them; this is
 * the other half. It turns one router response into a route_t that is
 * indistinguishable from a GPX or an offline Dijkstra result, because DESIGN.md
 * 1.4 claims nothing downstream of a route_t can tell where it came from, and
 * a second way of building one is exactly how that claim would quietly stop
 * being true.
 *
 * TWO ADAPTERS, ONE OUTPUT. Valhalla answers with `trip.legs[].shape` plus
 * `maneuvers`; OSRM with `routes[0].geometry` plus `steps`. Both encode the
 * geometry as a Google polyline and neither says at what precision, which is
 * the trap this module exists to survive -- see below.
 *
 * POSITIONS FROM THE ROUTER, KINDS FROM GEOMETRY. The router knows where a
 * maneuver is; §7.4's classifier decides what it is, at that vertex, with the
 * same +/-25 m arms a derived cue uses. So there is no table mapping thirty-odd
 * Valhalla types onto nine cue kinds -- two enumerations, both free to grow
 * upstream, and a mismatch that would fail silently as a wrong arrow. What
 * little of the type IS read is structural and cannot be got from geometry:
 * where a leg ENDS (you have arrived), and which two maneuvers are one
 * roundabout. Anything unrecognised falls through to the classifier, which is
 * the right default rather than a hole.
 *
 * ON FAILURE THE OUTPUT IS NOT TOUCHED. Not overwritten, not zeroed, not
 * route_init()ed -- a rider mid-ride is FOLLOWING the route in `out`, and a
 * reroute that fails must leave them following it. Everything is built in a
 * local route_t and moved into place on the last line.
 *
 * Portable C: libc + libm, no pixels and no sockets. It links into
 * tests/test_netroute.c on its own and runs in either lane.
 */
#ifndef BEEPY_NAV_NETROUTE_H
#define BEEPY_NAV_NETROUTE_H

#include "route.h"

#define NETROUTE_WHY 96

/* Matches config.h's `router_type` strings. */
enum { NETROUTE_VALHALLA = 0, NETROUTE_OSRM = 1 };

/* NETROUTE_* for "valhalla" / "osrm", -1 for anything else. config.c already
 * refuses the third spelling; this is what turns the accepted one into a
 * number without a strcmp at the call site. */
int netroute_type(const char *name);

/* Google-encoded polyline, at `precision` decimal digits (5 or 6).
 *
 * POLYLINE6 IS NOT THE DEFAULT ANYWHERE BUT VALHALLA, and getting it wrong is
 * the one bug in this module that looks like success: a 1e6 body read at 1e5
 * yields a route of the right SHAPE at a tenth the size, sitting plausibly on
 * the map a tenth of the way to its destination. netroute_parse() therefore
 * never trusts the precision -- it checks the decoded length against the
 * distance the response states, and tries the other precision when they
 * disagree by more than a factor of NETROUTE_LEN_TOL.
 *
 * Allocates `*out` (caller frees) and returns the point count; -1 with `why`
 * filled on a bad alphabet, a truncated varint, an odd number of values, an
 * empty string, or more than ROUTE_MAXPT points. */
int netroute_polyline(const char *s, int precision, pt_t **out, char *why,
                      int nwhy);

/* A factor of two, and it is loose on purpose. Our length is measured on
 * §6.1's tangent plane over the shape we were sent; the router's is measured
 * on the geodesic along the road, and `overview=simplified` legitimately sends
 * a shape several percent short of it. Two is far beyond any of that and far
 * short of the ten a precision mix-up produces, which is the only thing this
 * check is for. */
#define NETROUTE_LEN_TOL 2.0

/* Routers whose maneuvers this build understands, per response. More than a
 * handful of via points is not a thing this device asks for. */
#define NETROUTE_MAXLEG 64

/* Parse one response into `out`. `json` is the NUL-terminated body netfetch
 * left behind; `type` is NETROUTE_*; `name` becomes the route's name (NULL
 * gives "DESTINATION", as router_to() does).
 *
 * 0 on success. -1 with a one-line reason in `why` and `out` UNTOUCHED
 * otherwise -- and the reasons are the point of this function as much as the
 * routes are. A captive portal, a truncated body, a 200 OK carrying an error
 * object and a shape that does not decode are four different things, and a
 * rider who is told "NO ROUTE" for all four learns nothing. */
int netroute_parse(const char *json, int type, const char *name, route_t *out,
                   char *why, int nwhy);

#endif /* BEEPY_NAV_NETROUTE_H */

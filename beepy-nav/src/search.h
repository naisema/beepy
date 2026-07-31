/* beepy-nav/src/search.h -- the road/name pack, and the FIND search over it
 * (DESIGN.md 1.4).
 *
 * A road pack is a file built on the Mac by tools/mkpack.py from an Overpass
 * extract: a node table, a DIRECTED edge table that honours `oneway`, the
 * street names in ASCII, and the sampled coordinates each name is searchable
 * at. Its complete format is written out at the top of mkpack.py and in
 * DESIGN.md 1.4; nothing here parses text.
 *
 * The pack is NOT bound to a route's frame the way a tile pack is
 * (tiles_bind_route). There is no route when FIND runs -- building one is what
 * FIND is for -- so the pack's own tangent frame is the only frame available,
 * and everything crossing the boundary is lat/lon: roads_project() takes the
 * rider's fix in, roads_unproject() hands the routed path back out. One fewer
 * moving part, and one fewer thing that can be bound to the wrong route.
 *
 * "Nothing is searchable that is not in the pack", and the pack knows how much
 * it had to leave out: roads_ndropped() is the count of names with no ASCII
 * form, which the FIND page is honest about rather than silently missing.
 *
 * Portable C, libc + libm only. It compiles and runs in the host lane, which is
 * what lets tests/test_search.c link it on its own.
 */
#ifndef BEEPY_NAV_SEARCH_H
#define BEEPY_NAV_SEARCH_H

typedef struct roads roads_t;

/* One directed edge, decoded. `to` is a node index; `len_mm` is the segment
 * length in millimetres as mkpack.py measured it in the PACK's frame -- an
 * integer, because it is the number Dijkstra adds up and "the same extract
 * gives the same route" has to survive two compilers. */
typedef struct {
    unsigned int to;
    unsigned int len_mm;
    unsigned short flags;
    /* WIDENED IN PACK v3. It was a u16, which capped the PLACES table at 65 534
     * and is where a nationwide destination index stopped: 123 731 names for
     * Thailand against a ceiling of 65 534 (DESIGN.md 1.4.7). Four bytes an edge
     * on disk, and on this device the in-memory struct was already padded to
     * eight-byte alignment, so the resident cost of the wider field is smaller
     * than the file's. */
    unsigned int name;
} roadedge_t;

/* EDGE flags. ROADEDGE_ONEWAY says the parent way is oneway, so the REVERSE of
 * this edge is deliberately absent from the table -- which is the whole
 * mechanism by which the router of DESIGN.md 1.4 cannot go the wrong way up a
 * street. It is a flag and not merely an omission so a test can tell "the pack
 * honours oneway" from "the pack happens not to contain that edge". */
#define ROADEDGE_ONEWAY 1u

/* Bits 1-4: the road class, coarse-to-fine, so a bicycle can be kept off a
 * motorway offline (DESIGN.md 7.7). It arrived in pack version 2; before that
 * the packer read `highway` to decide routability and then threw it away,
 * which is why an offline bike route could take an expressway.
 *
 * Ordered, so "avoid anything above N" is a comparison and not a set. 0 means
 * the packer did not recognise the tag -- treated as routable by everything,
 * because refusing to route over an unfamiliar road class would make the pack
 * less useful every time OSM invents one. */
#define ROADEDGE_CLASS_SHIFT 1u
#define ROADEDGE_CLASS_MASK 0x1Eu
#define ROADEDGE_CLASS(f) (((f) & ROADEDGE_CLASS_MASK) >> ROADEDGE_CLASS_SHIFT)
#define ROADCLASS_MOTORWAY 1
#define ROADCLASS_TRUNK 2
#define ROADCLASS_PRIMARY 3

/* Travel modes (DESIGN.md 7.7). They pick the online costing and, offline,
 * which road classes the router will use. */
#define NAV_MODE_BIKE 0
#define NAV_MODE_CAR 1

#define ROADS_NAME_NONE 0xffffffffu  /* v3: was 0xffffu */

/* The graph, in CSR form: node i's outgoing edges are edge[adj[i]] up to
 * edge[adj[i+1]-1]. Borrowed pointers into the open pack, valid until
 * roads_close(). */
typedef struct {
    int nnode;
    const double *en;          /* 2*nnode, metres in the pack's frame */
    const unsigned int *adj;   /* nnode + 1 */
    int nedge;
    const roadedge_t *edge;
} roadgraph_t;

/* A search hit. `name` points into the pack's string table. */
#define PLACE_NAME_MAX 64
typedef struct {
    const char *name;   /* uppercase ASCII, NUL-terminated, owned by the pack */
    double dist_m;      /* to the nearest candidate point of that name       */
    double bearing;     /* radians clockwise from north, to the same point   */
    double e, n;        /* that point, in the pack's frame                   */
    double lat, lon;    /* and in degrees, which is what the router wants    */
    int place;          /* its index in the pack's place table               */
} place_t;

/* Open a pack, or NULL. `why` (optional, `nwhy` bytes) receives a one-line
 * reason on failure -- the caller prints it once and carries on without a
 * search index, exactly as tiles_open() does for the basemap. */
roads_t *roads_open(const char *path, char *why, int nwhy);
void roads_close(roads_t *g);

/* DESIGN.md 6.1's tangent plane, about the pack's own reference. */
void roads_project(const roads_t *g, double lat, double lon, double *e,
                   double *n);
void roads_unproject(const roads_t *g, double e, double n, double *lat,
                     double *lon);

void roads_graph(const roads_t *g, roadgraph_t *out);

/* Provenance, for the one line on stderr and for tests. */
double roads_ref_lat(const roads_t *g);
double roads_ref_lon(const roads_t *g);
int roads_nnode(const roads_t *g);
int roads_nedge(const roads_t *g);
int roads_nplace(const roads_t *g);
/* Names the pack could not index because they have no ASCII form (DESIGN.md
 * 1.4: the 5x7 font is A-Z/0-9). */
int roads_ndropped(const roads_t *g);
/* 1 when mkpack.py built the edge table directed; 0 for an --ignore-oneway
 * pack, which exists only so T-ONEWAY can fail without the fix. */
int roads_honours_oneway(const roads_t *g);
/* The place table, for tests and for T-ONEWAY's node picking. */
const char *roads_place_name(const roads_t *g, int i);

/* Token-AND over the name table, nearest first (DESIGN.md 1.4, and
 * mockup.py's search_places() constant for constant):
 *
 *   - the query is uppercased and split on whitespace; an empty query matches
 *     nothing at all, which is the state the FIND page opens in;
 *   - a name matches when EVERY token appears in it as a substring -- not as a
 *     word, so "SUK 23" finds "SOI SUKHUMVIT 23";
 *   - a name's distance is the distance to the nearest of its candidate points,
 *     so the representative coordinate of a 2 km road is chosen at query time
 *     and not at pack time;
 *   - ranked by (distance, bearing, name), which is the order Python's
 *     `sorted()` gives the mockup's tuples.
 *
 * Returns the TOTAL number of matching names and fills up to `max` of them.
 * The total rather than the length of the list, because DESIGN.md 1.4 calls the
 * title bar's number a "live hit count" and a count that stops at the size of
 * the visible list is not one -- the mockup's own hit count was capped at its
 * limit, and that is the one place this port does not follow it. */
/* 1.4's matching rule as a predicate, for names that are not in the pack --
 * the saved places of 1.4.6. Same tokenise-and-substring test search_places()
 * applies, exported rather than copied. An empty query matches nothing, which
 * is what makes "no query" and "no match" the same answer here. */
int search_name_matches(const char *name, const char *query);

int search_places(const roads_t *g, const char *query, double from_e,
                  double from_n, place_t *out, int max);

/* "N", "NE", ... for a bearing in radians clockwise from north. */
const char *search_compass8(double bearing);

#endif /* BEEPY_NAV_SEARCH_H */

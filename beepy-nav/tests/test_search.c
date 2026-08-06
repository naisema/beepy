/* beepy-nav/tests/test_search.c -- the road pack, the FIND search and the
 * router (DESIGN.md 1.4).
 *
 * Three kinds of assertion live here, and they need three kinds of fixture:
 *
 *   - the SEMANTICS of search_places() and of the pack reader, over
 *     tests/roads/names.roads: twelve nodes and two names, hand-written as
 *     tests/roads/names.json so that every branch of the indexer has a case
 *     (name:en preferred, an ASCII name as the fallback, a Thai name dropped
 *     and counted, an unnamed way still routable, a footway not routable at
 *     all). Small enough that every expected number below is arithmetic on a
 *     0.001-degree grid rather than a value read off a previous run.
 *
 *   - T-ONEWAY, over the real Asok extract, in BOTH forms: the pack
 *     tools/mkpack.py builds by default and the one it builds with
 *     --ignore-oneway. Two packs and not one because a test that cannot fail is
 *     worthless: the second is the mockup router's behaviour, frozen, and the
 *     three hand-picked pairs below each traverse a oneway backwards on it.
 *
 *   - the reference measurement DESIGN.md 1.4 quotes, so the figure in the
 *     design can be re-derived rather than believed.
 *
 * Runs in both lanes, inside `make check` on the device: no Pillow, no JSON, no
 * network -- the packs are committed, and `make test-roads` is the Mac-side
 * check that they are still what mkpack.py produces.
 *
 *     make test-unit
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h" /* CFG_TOLLS_* for the router calls */
#include "router.h"
#include "search.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NAMES_PACK "beepy-nav/tests/roads/names.roads"
#define ASOK_PACK "beepy-nav/tests/roads/asok.roads"
#define ASOK_OPEN_PACK "beepy-nav/tests/roads/asok-nooneway.roads"
#define TMP_PACK "out-test-search.roads"

static int failures;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void
checkf(int ok, const char *fmt, double a, double b)
{
    if (!ok) {
        printf("FAIL ");
        printf(fmt, a, b);
        printf("\n");
        failures++;
    }
}

/* names.json's grid step, 0.001 degrees, in metres -- taken from the pack's own
 * projection rather than written out with a hand-computed cos(13.74 deg), which
 * was wrong in the fourth decimal and made the first version of this file argue
 * with itself over four millimetres. */
static double DLAT_M, DLON_M;

static void
grid_steps(const roads_t *g)
{
    roads_project(g, 13.741, 100.561, &DLON_M, &DLAT_M);
}

static roads_t *
must_open(const char *path)
{
    char why[128];
    roads_t *g = roads_open(path, why, (int)sizeof why);
    if (!g)
        printf("FAIL cannot open %s: %s\n", path, why);
    return g;
}

/* ------------------------------------------------------- the pack reader */

static void
t_open(void)
{
    roads_t *g = must_open(NAMES_PACK);
    double e, n, lat, lon;
    if (!g) {
        failures++;
        return;
    }
    /* Every one of these is counted by hand off names.json; see its comment.
     * 12 nodes because way 2 and way 4 share the vertices way 1 and way 2 end
     * on, which is the exact-shared-coordinate join under test. */
    check(roads_nnode(g) == 12, "names.roads has 12 joined nodes");
    /* way 1 oneway: 3 edges. Ways 2, 3, 4, 5 two-way: 4 + 2 + 8 + 2. */
    check(roads_nedge(g) == 19, "names.roads has 19 directed edges");
    check(roads_nplace(g) == 2, "two searchable names");
    check(roads_ndropped(g) == 1, "one name dropped for having no ASCII form");
    check(roads_honours_oneway(g), "the default pack honours oneway");
    grid_steps(g);

    /* name:en beats name (way 1 is 'MAIN ROAD' in English and Thai), and an
     * ASCII `name` with no name:en is the fallback (way 2). Sorted by name. */
    check(roads_place_name(g, 0) && !strcmp(roads_place_name(g, 0), "MAIN ROAD"),
          "place 0 is the name:en form");
    check(roads_place_name(g, 1) &&
              !strcmp(roads_place_name(g, 1), "SECOND STREET"),
          "place 1 fell back to name");

    /* The reference is names.json's own first vertex, so it projects to the
     * origin -- and back again. */
    roads_project(g, 13.740, 100.560, &e, &n);
    checkf(fabs(e) < 1e-9 && fabs(n) < 1e-9, "reference projects to (%g, %g)",
           e, n);
    roads_project(g, 13.741, 100.561, &e, &n);
    checkf(fabs(n - 110.540) < 1e-6, "0.001 deg lat is %.4f m, not %.4f",
           110.540, n);
    /* DESIGN.md 6.1's east scale: 111320 m per degree TIMES cos(lat0). The
     * cosine is of the reference latitude and not of the point, which is the
     * whole definition of a tangent plane -- and the reason this is written as
     * the formula rather than as a number, after the first draft got the number
     * wrong in the fourth decimal. */
    {
        double want = 0.001 * 111320.0 *
                      cos(roads_ref_lat(g) * (M_PI / 180.0));
        checkf(fabs(e - want) < 1e-9, "0.001 deg lon is %.4f m, not %.4f", want,
               e);
    }
    roads_unproject(g, e, n, &lat, &lon);
    checkf(fabs(lat - 13.741) < 1e-12 && fabs(lon - 100.561) < 1e-12,
           "unproject round-trips to (%.9f, %.9f)", lat, lon);
    roads_close(g);
}

/* A pack with one byte changed must be refused, not half-read. Derived from the
 * committed fixture rather than built here, so the corruption is the only
 * difference between a pack that opens and one that does not. */
static void
corrupt(long at, int to, const char *what)
{
    char why[128];
    unsigned char *buf;
    long n;
    roads_t *g;
    FILE *f = fopen(NAMES_PACK, "rb");
    if (!f) {
        printf("FAIL cannot read %s\n", NAMES_PACK);
        failures++;
        return;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = (unsigned char *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        printf("FAIL cannot read %s\n", NAMES_PACK);
        failures++;
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    if (at >= 0)
        buf[at] = (unsigned char)to;
    f = fopen(TMP_PACK, "wb");
    if (!f) {
        printf("FAIL cannot write %s\n", TMP_PACK);
        failures++;
        free(buf);
        return;
    }
    /* at < 0 means "truncate to `to` bytes" instead of patching one. */
    fwrite(buf, 1, at < 0 ? (size_t)to : (size_t)n, f);
    fclose(f);
    free(buf);
    why[0] = '\0';
    g = roads_open(TMP_PACK, why, (int)sizeof why);
    check(g == NULL && why[0] != '\0', what);
    roads_close(g);
    remove(TMP_PACK);
}

static void
t_refuses(void)
{
    char why[128];
    check(roads_open("no-such-pack.roads", why, (int)sizeof why) == NULL,
          "a missing pack is refused");
    check(roads_open(NULL, why, (int)sizeof why) == NULL,
          "a NULL path is refused");
    corrupt(0, 'X', "a bad magic is refused");
    corrupt(8, 9, "an unsupported version is refused");
    corrupt(10, 32, "an unexpected header size is refused");
    corrupt(52, 0, "an unexpected coordinate scale is refused");
    corrupt(-1, 40, "a pack truncated inside the header is refused");
    corrupt(-1, 200, "a pack truncated inside its tables is refused");
    /* The section table's first offset. Pointing NODES at byte 0 puts it
     * inside the header, which the reader must not accept. */
    corrupt(64, 0, "a section inside the header is refused");
    /* And the accessors have to survive a NULL pack, because that is the state
     * the whole feature degrades to (nav.c prints one line and carries on). */
    check(roads_nnode(NULL) == 0 && roads_nedge(NULL) == 0 &&
              roads_nplace(NULL) == 0 && roads_ndropped(NULL) == 0 &&
              roads_honours_oneway(NULL) == 0 &&
              roads_place_name(NULL, 0) == NULL,
          "every accessor survives a NULL pack");
    check(search_places(NULL, "MAIN", 0, 0, NULL, 0) == 0,
          "searching a NULL pack finds nothing");
}

/* ------------------------------------------------------------- the search */

static void
t_search(void)
{
    roads_t *g = must_open(NAMES_PACK);
    place_t hit[4];
    int n;
    if (!g) {
        failures++;
        return;
    }
    grid_steps(g);

    /* Token-AND, and the tokens are SUBSTRINGS rather than words: that is what
     * lets "SUK 23" find "SOI SUKHUMVIT 23", which is the query DESIGN.md 1.4's
     * own screenshot types. */
    check(search_places(g, "MAIN", 0, 0, hit, 4) == 1, "one token, one name");
    check(search_places(g, "MAIN ROAD", 0, 0, hit, 4) == 1, "both tokens match");
    check(search_places(g, "ROAD MAIN", 0, 0, hit, 4) == 1,
          "token order does not matter");
    check(search_places(g, "AIN OA", 0, 0, hit, 4) == 1,
          "tokens are substrings, not words");
    check(search_places(g, "MAIN STREET", 0, 0, hit, 4) == 0,
          "every token must match, not any");
    check(search_places(g, "R", 0, 0, hit, 4) == 2, "one letter matches both");
    check(search_places(g, "main road", 0, 0, hit, 4) == 1,
          "the query is folded to upper case");
    check(search_places(g, "  MAIN   ROAD  ", 0, 0, hit, 4) == 1,
          "runs of whitespace are one separator");

    /* An empty query matches nothing -- the state the FIND page opens in. If it
     * matched everything the page would open showing four arbitrary streets. */
    check(search_places(g, "", 0, 0, hit, 4) == 0, "an empty query finds none");
    check(search_places(g, "   ", 0, 0, hit, 4) == 0, "nor does whitespace");
    check(search_places(g, NULL, 0, 0, hit, 4) == 0, "nor does NULL");

    /* Zero matches, which is a state the page has to draw honestly. */
    check(search_places(g, "ZZQX", 0, 0, hit, 4) == 0, "a miss finds nothing");

    /* Nearest first, and the distance is to the nearest CANDIDATE point of the
     * name -- MAIN ROAD's are way 1's vertices 0 and 3 and way 5's vertex 0, so
     * from 0.003 deg north of the origin the near one is 0.0002 deg away.
     * SECOND STREET has a single candidate, at the origin. */
    n = search_places(g, "R", 0.0, 3.0 * DLAT_M, hit, 4);
    check(n == 2, "both names still match from further north");
    check(n == 2 && !strcmp(hit[0].name, "MAIN ROAD"), "MAIN ROAD is nearer");
    check(n == 2 && !strcmp(hit[1].name, "SECOND STREET"),
          "SECOND STREET is further");
    checkf(n == 2 && fabs(hit[0].dist_m - 0.0) < 1e-6,
           "MAIN ROAD is on top of the query point, not %.3f m away (%g)",
           n == 2 ? hit[0].dist_m : -1.0, 0.0);
    checkf(n == 2 && fabs(hit[1].dist_m - 3.0 * DLAT_M) < 1e-6,
           "SECOND STREET is %.3f m away, not %.3f", 3.0 * DLAT_M,
           n == 2 ? hit[1].dist_m : -1.0);
    check(n == 2 && !strcmp(search_compass8(hit[1].bearing), "S"),
          "and it is due south");

    /* The count is the TOTAL, not the length of the list: DESIGN.md 1.4 calls
     * the title bar's number a live hit count, and the mockup's own was capped
     * at its limit. This is the one place the port does not follow it. */
    memset(hit, 0, sizeof hit);
    check(search_places(g, "R", 0.0, 3.0 * DLAT_M, hit, 1) == 2,
          "the total is returned even when only one fits");
    check(!strcmp(hit[0].name, "MAIN ROAD"),
          "and the one that fits is the nearest");
    check(search_places(g, "R", 0, 0, hit, 0) == 2,
          "a zero-length list still counts");

    /* A hit carries lat/lon as well as metres, because that is what the router
     * writes into the route_t. */
    n = search_places(g, "SECOND", 0, 0, hit, 4);
    checkf(n == 1 && fabs(hit[0].lat - 13.740) < 1e-9 &&
               fabs(hit[0].lon - 100.560) < 1e-9,
           "SECOND STREET's coordinate is (%.6f, %.6f)",
           n == 1 ? hit[0].lat : 0.0, n == 1 ? hit[0].lon : 0.0);
    roads_close(g);
}

static void
t_compass(void)
{
    /* The eight sectors are 45 degrees wide and centred on their name, so every
     * boundary is at an odd multiple of 22.5. Checked either side of two of
     * them, because an off-by-half-a-sector is the classic error here. */
    check(!strcmp(search_compass8(0.0), "N"), "0 is N");
    check(!strcmp(search_compass8(22.4 * M_PI / 180.0), "N"), "22.4 is still N");
    check(!strcmp(search_compass8(22.6 * M_PI / 180.0), "NE"), "22.6 is NE");
    check(!strcmp(search_compass8(M_PI / 4.0), "NE"), "45 is NE");
    check(!strcmp(search_compass8(M_PI / 2.0), "E"), "90 is E");
    check(!strcmp(search_compass8(M_PI), "S"), "180 is S");
    check(!strcmp(search_compass8(-M_PI / 2.0), "W"), "-90 is W");
    check(!strcmp(search_compass8(-M_PI / 4.0), "NW"), "-45 is NW");
    check(!strcmp(search_compass8(337.6 * M_PI / 180.0), "N"), "337.6 wraps to N");
}

/* ------------------------------------------------------------- the router */

/* Is (u -> v) an edge of `G`? The whole of T-ONEWAY is this question asked of
 * every hop of every route, against the pack's own adjacency rather than
 * against a re-derivation of the rule. */
static int
hop_legal(const roadgraph_t *G, int u, int v)
{
    unsigned int k;
    for (k = G->adj[u]; k < G->adj[u + 1]; k++)
        if ((int)G->edge[k].to == v)
            return 1;
    return 0;
}

/* Illegal hops in `nodes`, judged against the DIRECTED graph. */
static int
count_violations(const roadgraph_t *GA, const int *nodes, int n)
{
    int i, bad = 0;
    for (i = 1; i < n; i++)
        if (!hop_legal(GA, nodes[i - 1], nodes[i]))
            bad++;
    return bad;
}

static void
t_router_small(void)
{
    roads_t *g = must_open(NAMES_PACK);
    roadgraph_t G;
    char why[128];
    int nodes[ROUTER_MAXPT], n;
    double total = 0.0, around;
    route_t rt;
    if (!g) {
        failures++;
        return;
    }
    roads_graph(g, &G);
    grid_steps(g);

    /* MAIN ROAD runs one way NORTH, from names.json's vertex 0 to its vertex 3.
     * So southbound the router has to go round three sides of the block: east
     * along SECOND STREET, north up the unnamed way, then west. 0.002 deg of
     * longitude twice and 0.003 of latitude once. */
    around = 2.0 * 2.0 * DLON_M + 3.0 * DLAT_M;
    n = router_path(g, 0.0, 3.0 * DLAT_M, 0.0, 0.0, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes, ROUTER_MAXPT, &total,
                    why, (int)sizeof why, NULL);
    check(n == 7, "southbound is seven nodes the long way round");
    /* 5 mm, because edge lengths are stored as integer millimetres (mkpack.py)
     * and six of them are summed here. */
    checkf(fabs(total - around) < 5e-3, "the long way round is %.3f m, not %.3f",
           around, total);
    check(count_violations(&G, nodes, n) == 0, "and it breaks no oneway");

    /* Northbound is the street itself: four nodes, three segments. */
    n = router_path(g, 0.0, 0.0, 0.0, 3.0 * DLAT_M, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes, ROUTER_MAXPT, &total,
                    why, (int)sizeof why, NULL);
    check(n == 4, "northbound is the street itself");
    checkf(fabs(total - 3.0 * DLAT_M) < 5e-3,
           "which is %.3f m, not the %.3f the long way costs", 3.0 * DLAT_M,
           total);

    /* An unreachable destination is a message and nothing else. Way 5 is a
     * second carriageway that joins nothing in this extract, which is exactly
     * what the edge of a real pack looks like. */
    why[0] = '\0';
    n = router_path(g, 0.0, 0.0, -1.0 * DLON_M, 3.0 * DLAT_M, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes,
                    ROUTER_MAXPT, &total, why, (int)sizeof why, NULL);
    check(n < 0 && why[0] != '\0', "an unreachable destination says so");
    check(router_to(g, 0.0, 0.0, -1.0 * DLON_M, 3.0 * DLAT_M, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0,
                    "NOWHERE", &rt,
                    why, (int)sizeof why, NULL) != 0,
          "and router_to(, NULL) fails the same way");
    check(rt.npt == 0 && rt.pt == NULL,
          "leaving no half-built route behind it");
    /* A pack that is not there at all: the same failure, not a crash. */
    check(router_path(NULL, 0, 0, 1, 1, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes, ROUTER_MAXPT, &total, why,
                      (int)sizeof why, NULL) < 0,
          "routing with no pack says so");
    check(router_to(NULL, 0, 0, 1, 1, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, NULL, &rt, why,
                    (int)sizeof why, NULL) != 0,
          "and so does router_to(, NULL)");

    /* The route_t a ride will be given: the rider's own position first, the
     * destination last, prepared and cued exactly as route_load() leaves a GPX.
     * Started 40 m east of the origin so the access leg is real. */
    check(router_to(g, 40.0, 3.0 * DLAT_M, 0.0, 0.0, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0,
                    "SECOND STREET", &rt, why,
                    (int)sizeof why, NULL) == 0,
          "router_to(, NULL) builds a route");
    check(rt.prepared, "prepared");
    /* Seven path nodes, plus the rider's position 40 m off the first of them --
     * and NOT plus the destination, which is node 0 itself and was collapsed
     * into it (router.c's DUP_M). A zero-length last segment would have no
     * bearing for route_cues_derive() to classify. */
    check(rt.npt == 8, "the position is prepended, the duplicate end is not");
    check(rt.ncue >= 1 && rt.cue[rt.ncue - 1].kind == CUE_DEST,
          "and a destination cue at the end");
    check(!strcmp(rt.name, "SECOND STREET"), "carrying the place's name");
    checkf(rt.total_m > around && rt.total_m < around + 60.0,
           "and a length between %.1f and that plus the access leg (%.1f)",
           around, rt.total_m);
    check(fabs(rt.en[0]) < 1e-9 && fabs(rt.en[1]) < 1e-9,
          "route.c re-references it to its own first point");
    route_free(&rt);
    roads_close(g);
}

/* ------------------------------------------------------------- T-ONEWAY
 *
 * 200 seeded pairs plus three hand-picked ones, over the real extract, with
 * every hop judged against the pack's own directed adjacency. The seed is fixed
 * so a failure is reproducible; the pairs are node indices because the pack is
 * a committed fixture whose determinism `make test-roads` asserts.
 */
#define ONEWAY_PAIRS 200

/* Three pairs on which the UNDIRECTED shortest path provably rides a oneway
 * backwards. Each was found by routing on the --ignore-oneway pack and checking
 * the result against the directed one; each is checked BOTH ways below, so the
 * assertion fails if the fix is removed rather than merely passing when it is
 * present.
 *
 *   1  DESIGN.md 1.4's own reference pair -- the Asok route's first and last
 *      points. Its 824 m "shortest path" goes 22 hops the wrong way up
 *      Ratchadaphisek Road, which is oneway southbound. The legal route is
 *      864 m. The design's own measurement was a wrong-way route.
 *   2  the minimal case: ONE illegal hop, on Sukhumvit Road. 500 m wrong way
 *      against 623 m legal -- the kind of shortcut that looks plausible on a
 *      screen and is a dual carriageway in person.
 *   3  the expensive case: 860 m wrong way against 1 949 m legal, an unnamed
 *      oneway loop. Honouring the tag more than doubles the ride, which is
 *      precisely why a router is tempted to ignore it.
 */
typedef struct {
    int src, dst;         /* node indices, or -1 to use the coordinates */
    double se, sn, de, dn;
    const char *what;
} advpair_t;

static const advpair_t ADVERSARIAL[3] = {
    {-1, -1, 0.0, 0.0, 320.8778, 511.3470,
     "1.4's reference pair rides Ratchadaphisek the wrong way"},
    {792, 1619, 0, 0, 0, 0, "one illegal hop on Sukhumvit Road"},
    {2203, 1955, 0, 0, 0, 0, "the pair oneway costs the most"},
};

/* CPU seconds. Only for the timing LINE this file prints -- nothing is asserted
 * on it, because a test that fails on a busy machine is a test that gets
 * deleted. DESIGN.md 1.4 quotes routing times, so the way to re-derive them
 * belongs somewhere it will be run.
 *
 * clock() and not clock_gettime(): under -std=c11 the device's gcc 10.2.1 hides
 * every POSIX clock behind a feature-test macro, and adding one to a unit test
 * to print a diagnostic is the wrong trade. CPU time is also the more honest
 * measure of a benchmark. */
static double
mono(void)
{
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void
t_oneway(void)
{
    roads_t *A = must_open(ASOK_PACK);
    roads_t *B = must_open(ASOK_OPEN_PACK);
    roadgraph_t GA, GB;
    static int nodes[ROUTER_MAXPT];
    char why[128];
    unsigned long seed = 20260730u;
    int i, routed = 0, viol = 0, adv_ok = 0, adv_caught = 0;
    double total, t0;

    if (!A || !B) {
        failures++;
        roads_close(A);
        roads_close(B);
        return;
    }
    roads_graph(A, &GA);
    roads_graph(B, &GB);
    check(roads_honours_oneway(A), "the Asok pack honours oneway");
    check(!roads_honours_oneway(B), "and the control pack does not");
    check(GA.nnode == GB.nnode, "both packs have the same nodes");
    check(GA.nedge < GB.nedge, "the directed one has fewer edges");
    /* DESIGN.md 1.4: "2 803 nodes". Asserted, not quoted. */
    check(GA.nnode == 2803, "2803 nodes, as DESIGN.md 1.4 measured");
    check(roads_nplace(A) == 117 && roads_ndropped(A) == 3,
          "117 searchable names, 3 dropped for having no ASCII form");

    /* A plain LCG, written out: the point is that the pairs are the same on
     * every machine and in every build, not that they are good random numbers. */
    t0 = mono();
    for (i = 0; i < ONEWAY_PAIRS; i++) {
        int s, d, n;
        seed = seed * 1103515245u + 12345u;
        s = (int)((seed >> 8) % (unsigned long)GA.nnode);
        seed = seed * 1103515245u + 12345u;
        d = (int)((seed >> 8) % (unsigned long)GA.nnode);
        if (s == d)
            continue;
        n = router_path(A, GA.en[2 * s], GA.en[2 * s + 1], GA.en[2 * d],
                        GA.en[2 * d + 1], NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes,
                        ROUTER_MAXPT, &total, why,
                        (int)sizeof why, NULL);
        if (n < 2)
            continue; /* unreachable under oneway: a legal answer, not a hop */
        routed++;
        viol += count_violations(&GA, nodes, n);
    }
    check(viol == 0, "T-ONEWAY: zero oneway violations over 200 seeded pairs");
    /* Without this the assertion above passes on a router that never finds
     * anything. The bar is deliberately a fraction rather than the measured
     * number: what matters is that the sample is not vacuous. */
    printf("    T-ONEWAY: %d of %d seeded pairs routed, %d violations, "
           "%.2f ms each\n",
           routed, ONEWAY_PAIRS, viol,
           routed ? (mono() - t0) * 1000.0 / routed : 0.0);
    check(routed >= ONEWAY_PAIRS / 2, "and most of those 200 pairs routed");

    for (i = 0; i < 3; i++) {
        const advpair_t *p = &ADVERSARIAL[i];
        double se, sn, de, dn;
        int n;
        if (p->src >= 0) {
            se = GA.en[2 * p->src];
            sn = GA.en[2 * p->src + 1];
            de = GA.en[2 * p->dst];
            dn = GA.en[2 * p->dst + 1];
        } else {
            se = p->se;
            sn = p->sn;
            de = p->de;
            dn = p->dn;
        }
        /* The control: on a pack built the mockup's way, this pair's shortest
         * path IS illegal. If this stops being true the pair has stopped being
         * adversarial and the assertion below has stopped meaning anything. */
        n = router_path(B, se, sn, de, dn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes, ROUTER_MAXPT, &total, why,
                        (int)sizeof why, NULL);
        if (n >= 2 && count_violations(&GA, nodes, n) > 0)
            adv_caught++;
        else
            printf("FAIL %s: not adversarial any more\n", p->what);
        /* And with oneway honoured it is legal. */
        n = router_path(A, se, sn, de, dn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes, ROUTER_MAXPT, &total, why,
                        (int)sizeof why, NULL);
        if (n >= 2 && count_violations(&GA, nodes, n) == 0)
            adv_ok++;
        else
            printf("FAIL %s: the legal route was not found\n", p->what);
    }
    check(adv_caught == 3, "all three adversarial pairs fail without the fix");
    check(adv_ok == 3, "and all three are legal with it");
    roads_close(A);
    roads_close(B);
}

/* ------------------------------------------ DESIGN.md 1.4's own measurement */

static void
t_reference_path(void)
{
    /* "the 824 m result matched the hand-built demo route within 3 m."
     *
     * Re-derived here, and the derivation is the interesting part: that figure
     * came from a router that IGNORED oneway, so it is reproducible only on the
     * control pack -- which is where it is asserted. The oneway-honouring
     * router answers 864 m for the same two points, and both numbers are pinned
     * so that neither can drift unnoticed.
     *
     * The endpoints are the Asok route's first and last vertices, in the pack's
     * own frame: the pack is referenced to that route's first point, so the
     * start is exactly the origin (tools/mkpack.py --route). */
    roads_t *A = must_open(ASOK_PACK);
    roads_t *B = must_open(ASOK_OPEN_PACK);
    static int nodes[ROUTER_MAXPT];
    char why[128];
    double open_m = 0.0, legal_m = 0.0;
    const double HAND_BUILT_M = 827.0116; /* mockup.py's load_osm() route */
    int n;

    if (!A || !B) {
        failures++;
        roads_close(A);
        roads_close(B);
        return;
    }
    n = router_path(B, 0.0, 0.0, 320.8778, 511.3470, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes, ROUTER_MAXPT,
                    &open_m, why, (int)sizeof why, NULL);
    check(n > 2, "the reference pair routes on the oneway-ignoring pack");
    checkf(fabs(open_m - 824.0) <= 3.0,
           "1.4's 824 m comes back as %.1f m (delta %.1f)", open_m,
           open_m - 824.0);
    checkf(fabs(open_m - HAND_BUILT_M) <= 4.0,
           "and is within 4 m of the hand-built %.1f m route (%.1f)",
           HAND_BUILT_M, open_m - HAND_BUILT_M);

    n = router_path(A, 0.0, 0.0, 320.8778, 511.3470, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, nodes, ROUTER_MAXPT,
                    &legal_m, why, (int)sizeof why, NULL);
    check(n > 2, "and it routes legally too, thanks to the 25 m snap");
    checkf(legal_m > open_m,
           "the legal route is longer: %.1f m against %.1f", legal_m, open_m);
    checkf(fabs(legal_m - 864.5) <= 1.0,
           "and it is 864.5 m, not %.1f (delta %.1f)", legal_m,
           legal_m - 864.5);
    roads_close(A);
    roads_close(B);
}

/* DESIGN.md 7.7: the road class the pack learned in v2, and the two things a
 * router does with it.
 *
 * tests/roads/modes.json is two ways between the same pair of points -- a
 * short MOTORWAY and a longer RESIDENTIAL dogleg -- so "the mode changed the
 * route" is a LENGTH and not a flag. A router that ignored class would return
 * the motorway to both callers and this would see one number twice. */

/* Pack v4's toll bit, the same way t_modes() tests v2's class: tolls.json puts a
 * short TOLLWAY and a longer FREE ROAD dogleg between the same two points, so
 * "avoiding tolls changed the route" is a LENGTH and not a flag. A router that
 * ignored the bit would hand both callers the tollway and this would see one
 * number twice.
 *
 * BOTH WAYS ARE PRIMARY in that fixture, which is what stops this passing on
 * 7.7's code: if the short way were a motorway, class alone would already
 * refuse it for a bicycle and the toll bit could be absent entirely. */
static void
t_tolls(void)
{
    char why[160];
    route_t allow, avoid;
    double se, sn, de, dn, spe, spn, fe, fn;
    roads_t *g = roads_open("beepy-nav/tests/roads/tolls.roads", why,
                            (int)sizeof why);

    check(g != NULL, "the tolls fixture opens");
    if (!g)
        return;
    roads_project(g, 13.740, 100.560, &se, &sn);
    roads_project(g, 13.744, 100.560, &de, &dn);

    check(router_to(g, se, sn, de, dn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, "X",
                    &allow, why, (int)sizeof why, NULL) == 0,
          "a car allowed tolls routes across");
    check(router_to(g, se, sn, de, dn, NAV_MODE_CAR, CFG_TOLLS_AVOID, 0, "X",
                    &avoid, why, (int)sizeof why, NULL) == 0,
          "and so does one avoiding them -- by the free road");
    check(allow.total_m < avoid.total_m,
          "the tolled route is shorter than the free one");
    check(avoid.total_m > 1.5 * allow.total_m,
          "and the detour is a real one, not a rounding");
    /* The badge on CONFIRM comes from these, and they are measured on the
     * chosen edges rather than copied from the policy. */
    check(allow.toll == ROUTE_TOLL_YES,
          "the tolled route reports ROUTE_TOLL_YES");
    check(avoid.toll == ROUTE_TOLL_NO,
          "and the free one reports ROUTE_TOLL_NO, not UNKNOWN");
    route_free(&allow);
    route_free(&avoid);

    /* Where the only road is tolled, avoiding tolls means no route -- the same
     * honest refusal 7.7 gives a bicycle on a motorway-only spur. */
    roads_project(g, 13.748, 100.560, &spe, &spn);
    check(router_to(g, se, sn, spe, spn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, "X",
                    &allow, why, (int)sizeof why, NULL) == 0,
          "a car allowed tolls reaches a toll-only spur");
    route_free(&allow);
    check(router_to(g, se, sn, spe, spn, NAV_MODE_CAR, CFG_TOLLS_AVOID, 0, "X",
                    &avoid, why, (int)sizeof why, NULL) != 0,
          "and one avoiding them is refused it rather than sent up it");

    /* toll=no is OSM saying the road is FREE. It must stay routable when tolls
     * are avoided: a packer that set the bit for any `toll` key would refuse
     * the ways someone bothered to survey. */
    roads_project(g, 13.736, 100.562, &fe, &fn);
    check(router_to(g, se, sn, fe, fn, NAV_MODE_CAR, CFG_TOLLS_AVOID, 0, "X",
                    &avoid, why, (int)sizeof why, NULL) == 0,
          "toll=no is free, and stays routable when tolls are avoided");
    check(avoid.toll == ROUTE_TOLL_NO, "and reports itself untolled");
    route_free(&avoid);
    roads_close(g);
}

static void
t_modes(void)
{
    char why[160];
    route_t bike, car;
    double se, sn, de, dn, spe, spn;
    roads_t *g = roads_open("beepy-nav/tests/roads/modes.roads", why,
                            (int)sizeof why);

    check(g != NULL, "the modes fixture opens");
    if (!g)
        return;
    roads_project(g, 13.740, 100.560, &se, &sn);
    roads_project(g, 13.744, 100.560, &de, &dn);

    check(router_to(g, se, sn, de, dn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, "X", &car, why,
                    (int)sizeof why, NULL) == 0,
          "a car routes across");
    check(router_to(g, se, sn, de, dn, NAV_MODE_BIKE, CFG_TOLLS_ALLOW, 0, "X", &bike, why,
                    (int)sizeof why, NULL) == 0,
          "and so does a bicycle -- by another road");
    /* The whole point: not merely both routable, but DIFFERENT. The car takes
     * the motorway because it is shorter; the bicycle may not, so it is
     * strictly longer. */
    check(car.total_m < bike.total_m,
          "the car's route is shorter than the bicycle's");
    check(bike.total_m > 1.5 * car.total_m,
          "and the bicycle's detour is a real one, not a rounding");
    route_free(&car);
    route_free(&bike);

    /* Where the ONLY road is a motorway, a bicycle gets no route at all --
     * which is the honest answer, and the condition that makes asking an
     * online router with a real cycling profile worth 1.4 seconds. */
    roads_project(g, 13.748, 100.560, &spe, &spn);
    check(router_to(g, se, sn, spe, spn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, "X", &car, why,
                    (int)sizeof why, NULL) == 0,
          "a car reaches a motorway-only spur");
    route_free(&car);
    check(router_to(g, se, sn, spe, spn, NAV_MODE_BIKE, CFG_TOLLS_ALLOW, 0, "X", &bike, why,
                    (int)sizeof why, NULL) != 0,
          "a bicycle is refused it, rather than sent up a motorway");

    /* And the lie that pack v2 also fixes: snap() falls back to the nearest
     * node however far away it is, so a destination outside the pack used to
     * come back as a confident route to the pack's edge. 40 km out must now
     * refuse and say how far. */
    check(router_to(g, se, sn, de + 40000.0, dn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, "X", &car, why,
                    (int)sizeof why, NULL) != 0,
          "a destination 40 km outside the pack is refused");
    check(strstr(why, "outside this map") != NULL,
          "and the reason says so rather than blaming the roads");
    roads_close(g);
}

/* DESIGN.md 7.7.2's class WEIGHTING, which is a different claim from t_modes()'s
 * class EXCLUSION and needs its own fixture to be testable at all.
 *
 * bias.roads is a short residential zigzag against a longer primary between the
 * same two points. modes.roads cannot serve: there the big road is the SHORTER
 * one, so shortest-distance already picks it and this test could not fail.
 *
 * Three assertions, and each fails on its own:
 *   - with the bias, a car takes the LONGER road (the whole feature);
 *   - without it, the same car takes the shorter one (so the bias is what did
 *     it, and not the fixture merely having one sensible route);
 *   - a bicycle is byte-identical either way (7.7.2 leaves bike neutral, and a
 *     weighting that leaked into the bicycle would be a safety decision nobody
 *     took).
 *
 * And the fourth, which is the one the change could most easily get wrong: the
 * REPORTED length is true metres and not the weighted cost. The primary route
 * costs about 0.85 of its length, so a router reporting its own cost would say
 * roughly 1180 m for a 1392 m road -- shorter than the zigzag it just rejected
 * for being too long, which is the sort of number a rider would notice and never
 * be able to explain. */
static void
t_class_bias(void)
{
    char why[160];
    route_t plain, biased, bike, bike_biased;
    double se, sn, de, dn;
    roads_t *g = roads_open("beepy-nav/tests/roads/bias.roads", why,
                            (int)sizeof why);

    check(g != NULL, "the bias fixture opens");
    if (!g)
        return;
    /* From the south end to the far stub's tip, so the destination is not the
     * junction where the two candidate roads meet -- a target sitting exactly
     * there would let snap() pick either and the assertion would flap. */
    roads_project(g, 13.740, 100.560, &se, &sn);
    roads_project(g, 13.749, 100.560, &de, &dn);

    check(router_to(g, se, sn, de, dn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 0, "X",
                    &plain, why, (int)sizeof why, NULL) == 0,
          "a car routes over the bias fixture with no bias");
    check(router_to(g, se, sn, de, dn, NAV_MODE_CAR, CFG_TOLLS_ALLOW, 1, "X",
                    &biased, why, (int)sizeof why, NULL) == 0,
          "and with it");
    check(biased.total_m > plain.total_m,
          "the biased car route is LONGER -- it took the big road");
    /* A real margin and not a rounding: the primary is 1.30x the zigzag here, so
     * anything under 1.2 means the router found some third path and the fixture
     * has stopped testing what it says it tests. */
    check(biased.total_m > 1.2 * plain.total_m,
          "and longer by the margin the fixture was built with");
    /* True metres, not the cost. Costed at 0.85 the primary would report about
     * 1180 m -- less than the 1095 m zigzag -- so this is the assertion that says
     * dist[] and *total_m are two different numbers now. */
    check(biased.total_m > 1300.0 && biased.total_m < 1500.0,
          "and it is reported as true metres rather than as its own cost");
    route_free(&plain);
    route_free(&biased);

    check(router_to(g, se, sn, de, dn, NAV_MODE_BIKE, CFG_TOLLS_ALLOW, 0, "X",
                    &bike, why, (int)sizeof why, NULL) == 0,
          "a bicycle routes over it");
    check(router_to(g, se, sn, de, dn, NAV_MODE_BIKE, CFG_TOLLS_ALLOW, 1, "X",
                    &bike_biased, why, (int)sizeof why, NULL) == 0,
          "and with the bias asked for");
    check(bike.total_m == bike_biased.total_m && bike.npt == bike_biased.npt,
          "and gets the identical route -- the bias is car-only, by design");
    route_free(&bike);
    route_free(&bike_biased);
    roads_close(g);
}

int
main(void)
{
    t_open();
    t_refuses();
    t_search();
    t_compass();
    t_router_small();
    t_oneway();
    t_reference_path();
    t_modes();
    t_class_bias();
    t_tolls();
    if (failures) {
        printf("test_search: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_search: OK\n");
    return 0;
}

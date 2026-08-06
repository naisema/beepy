/* beepy-nav/src/router.c -- see router.h.
 *
 * A textbook Dijkstra, and textbook is the point: DESIGN.md 1.4 measured the
 * work at 0.1 ms on a corridor pack and 10^5-10^6 nodes at "tens of ms", which
 * is inside one 125 ms frame of DESIGN.md 6.3 -- so A*, bidirectional search
 * and contraction hierarchies would all be complexity bought with nothing. The
 * only non-obvious parts are the multi-source snap (argued in router.h) and the
 * early exit, and both are commented where they happen.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h" /* CFG_TOLLS_* -- a rider policy, not a pack fact */
#include "router.h"

/* Vertices closer together than this are one vertex: a zero-length segment has
 * no bearing, and route_cues_derive() resamples along the polyline. It happens
 * when the rider is standing on a node, which is exactly the case the demo and
 * every test fixture start from. */
#define DUP_M 0.2

typedef struct {
    double d;
    int node;
} heapent_t;

typedef struct {
    heapent_t *e;
    int n, max;
} heap_t;

static void
heap_push(heap_t *h, double d, int node)
{
    int i;
    if (h->n >= h->max)
        return; /* cannot happen: the heap is sized for one push per edge */
    i = h->n++;
    h->e[i].d = d;
    h->e[i].node = node;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (h->e[p].d <= h->e[i].d)
            break;
        {
            heapent_t t = h->e[p];
            h->e[p] = h->e[i];
            h->e[i] = t;
        }
        i = p;
    }
}

static int
heap_pop(heap_t *h, double *d, int *node)
{
    int i = 0;
    if (h->n <= 0)
        return 0;
    *d = h->e[0].d;
    *node = h->e[0].node;
    h->e[0] = h->e[--h->n];
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->n && h->e[l].d < h->e[m].d)
            m = l;
        if (r < h->n && h->e[r].d < h->e[m].d)
            m = r;
        if (m == i)
            break;
        {
            heapent_t t = h->e[m];
            h->e[m] = h->e[i];
            h->e[i] = t;
        }
        i = m;
    }
    return 1;
}

/* Nodes within ROUTER_SNAP_M of (e, n), nearest first, at most
 * ROUTER_SNAP_MAX; or the single nearest node when nothing is inside the
 * radius, which is what makes this a nearest-node snap on a sparse pack.
 * Returns the count, with the access distances in `cost`. */
static int
snap(const roadgraph_t *G, double e, double n, int *node, double *cost)
{
    int i, held = 0, nearest = -1;
    double best = -1.0;

    for (i = 0; i < G->nnode; i++) {
        double d = hypot(G->en[2 * i] - e, G->en[2 * i + 1] - n);
        int j;
        if (best < 0.0 || d < best) {
            best = d;
            nearest = i;
        }
        if (d > ROUTER_SNAP_M)
            continue;
        if (held == ROUTER_SNAP_MAX && d >= cost[held - 1])
            continue;
        j = held < ROUTER_SNAP_MAX ? held : ROUTER_SNAP_MAX - 1;
        /* Ties broken by node index, so the candidate set -- and therefore the
         * route -- is the same on every machine and in every build. */
        while (j > 0 && (d < cost[j - 1] ||
                         (d == cost[j - 1] && i < node[j - 1]))) {
            cost[j] = cost[j - 1];
            node[j] = node[j - 1];
            j--;
        }
        cost[j] = d;
        node[j] = i;
        if (held < ROUTER_SNAP_MAX)
            held++;
    }
    if (held == 0 && nearest >= 0) {
        node[0] = nearest;
        cost[0] = best;
        held = 1;
    }
    return held;
}

/* Which road classes a mode will use AT ALL (DESIGN.md 7.7).
 *
 * Still exclusion and not weighting, and the distinction now carries weight of
 * its own: 7.7.2's class_weight() below decides what a road COSTS, and this
 * decides whether it may be used. A bicycle refused a motorway is a rule about
 * the law and the rider's safety, and no cost could express it -- a multiplier
 * of 50 is still a road the router will take when the alternative is long
 * enough.
 *
 * 7.7's original argument for having no weighting at all was that "len_mm is
 * what Dijkstra adds up and what the route reports". That premise is gone:
 * router_to() discards the total it is handed, so *total_m is now measured off
 * the chosen path and the two numbers are separate (7.7.2).
 *
 * Class 0 -- a `highway` tag the packer did not recognise -- is allowed by
 * everything. Refusing an unfamiliar class would make the pack worse every
 * time OSM invents one, and the failure would be silent. */
static int
mode_allows(int mode, int tolls, unsigned flags)
{
    int cls;

    /* Tolls first, and for both vehicles. A bicycle is already off motorway and
     * trunk, which is most of what is tolled here -- but a tolled bridge on a
     * primary is neither, and a rider who said avoid meant avoid. */
    if (tolls == CFG_TOLLS_AVOID && (flags & ROADEDGE_TOLL))
        return 0;
    if (mode != NAV_MODE_BIKE)
        return 1;
    cls = (int)ROADEDGE_CLASS(flags);
    /* A bicycle is not allowed on a motorway and has no business on a Thai
     * trunk road. If that leaves no route, the honest answer is no route --
     * and then the online router, which has a real bicycle profile, is asked. */
    return cls != ROADCLASS_MOTORWAY && cls != ROADCLASS_TRUNK;
}

/* What a metre of each road class COSTS the search (DESIGN.md 7.7.2).
 *
 * 7.7 refused this and said why: "len_mm is what Dijkstra adds up and what the
 * route reports, and a cost multiplier would make those two different numbers".
 * The premise stopped being true -- router_to() discards the total it is handed
 * and the length the rider reads comes from route_prepare()'s geometry -- so the
 * two numbers already exist and only this file still conflated them. The cost is
 * now weighted and *total_m is measured off the chosen path, which is the same
 * split the toll block below was written to survive.
 *
 * WHY AT ALL: shortest-distance sent a car 15.95 km through 32 turns of soi to a
 * destination Valhalla reached in 19.66 km with 13, and the shorter answer is
 * not the better one for a vehicle that fits on a main road. Measured on the
 * rider's own region pack, HOME to SALAYA CLOCK TOWER.
 *
 * BIKE IS NEUTRAL, every class 1.0, and that is a decision rather than a gap:
 * 7.7 already keeps a bicycle off motorway and trunk by EXCLUSION, a bicycle
 * genuinely does want the short way, and steering one onto a Thai primary to
 * save turns is a safety judgement this program is in no position to make.
 *
 * Class 0 is 1.0 for mode_allows()'s reason turned around: it means "a tag the
 * packer did not know", which is as likely to be an arterial as an alley, and a
 * penalty there would quietly worsen every route over a pack built by a newer
 * packer. */
static const double CLASS_W_CAR[ROADCLASS_N] = {
    1.00, /* 0 unrecognised -- neutral, deliberately        */
    0.70, /* 1 motorway                                     */
    0.75, /* 2 trunk                                        */
    0.85, /* 3 primary                                      */
    0.95, /* 4 secondary                                    */
    1.00, /* 5 tertiary -- the pivot: neither helped nor    */
          /*   hindered, so a tertiary route is costed at   */
          /*   its true length and everything else is       */
          /*   cheaper or dearer than that                  */
    1.30, /* 6 unclassified                                 */
    1.50, /* 7 residential                                  */
    2.00  /* 8 living_street                                 */
};

static double
class_weight(int mode, int major, unsigned flags)
{
    int cls;

    if (mode == NAV_MODE_BIKE || !major)
        return 1.0;
    cls = (int)ROADEDGE_CLASS(flags);
    if (cls < 0 || cls >= ROADCLASS_N)
        return 1.0; /* a pack from a newer packer: neutral, never refused */
    return CLASS_W_CAR[cls];
}

/* One place, so no refusal below can set a reason and forget the code. */
static int
refuse(char *why, int nwhy, int *code, int rc, const char *fmt, ...)
{
    va_list ap;

    if (code)
        *code = rc;
    if (why && nwhy > 0) {
        va_start(ap, fmt);
        vsnprintf(why, (size_t)nwhy, fmt, ap);
        va_end(ap);
    }
    return -1;
}

int
router_path(const roads_t *g, double se, double sn, double de, double dn,
            int mode, int tolls, int major, int *nodes, int maxn,
            double *total_m,
            char *why, int nwhy, int *code)
{
    roadgraph_t G;
    int src[ROUTER_SNAP_MAX], tgt[ROUTER_SNAP_MAX];
    double srcc[ROUTER_SNAP_MAX], tgtc[ROUTER_SNAP_MAX];
    int nsrc, ntgt, i, u, best_node = -1, count = 0, rc = -1;
    double d, best_total = -1.0;
    double *dist = NULL;
    int *prev = NULL;
    signed char *mark = NULL;
    heap_t h;

    h.e = NULL;
    h.n = h.max = 0;
    if (total_m)
        *total_m = 0.0;
    if (why && nwhy > 0)
        *why = '\0';
    if (code)
        *code = RC_OK;
    if (!g || !nodes || maxn < 2)
        return refuse(why, nwhy, code, RC_NOPACK, "no road pack loaded");
    roads_graph(g, &G);
    if (G.nnode < 2 || G.nedge < 1)
        return refuse(why, nwhy, code, RC_NOPACK,
                      "the pack has no road graph");

    nsrc = snap(&G, se, sn, src, srcc);
    ntgt = snap(&G, de, dn, tgt, tgtc);
    if (nsrc <= 0 || ntgt <= 0)
        return refuse(why, nwhy, code, RC_NOSNAP,
                      "no road near the start or the finish");
    /* snap() falls back to the single NEAREST node when nothing is within its
     * radius, which is what carries a POI centroid the 150 m to its gate. Past
     * a point that stops being a bridge and becomes a lie: a destination
     * outside the pack snaps to whatever node sits at the pack's edge, and the
     * rider gets a confident route to somewhere they did not ask for.
     *
     * So: refuse. The caller can then ask a router that HAS the ground -- and
     * "too far" is exactly the condition that makes going online worth the
     * 1.4 seconds. */
    if (tgtc[0] > ROUTER_MAX_SNAP_M || srcc[0] > ROUTER_MAX_SNAP_M)
        return refuse(why, nwhy, code, RC_OFFMAP, "%.0f km outside this map",
                      (tgtc[0] > srcc[0] ? tgtc[0] : srcc[0]) / 1000.0);

    dist = (double *)malloc((size_t)G.nnode * sizeof *dist);
    prev = (int *)malloc((size_t)G.nnode * sizeof *prev);
    mark = (signed char *)calloc((size_t)G.nnode, 1);
    /* One push per edge, plus the seeds: an exact ceiling, so heap_push()
     * never has to grow and never has to fail in a way that matters. */
    h.max = G.nedge + nsrc + 1;
    h.e = (heapent_t *)malloc((size_t)h.max * sizeof *h.e);
    if (!dist || !prev || !mark || !h.e) {
        refuse(why, nwhy, code, RC_NOMEM, "out of memory routing");
        goto done;
    }
    for (i = 0; i < G.nnode; i++) {
        dist[i] = HUGE_VAL;
        prev[i] = -1;
    }
    for (i = 0; i < ntgt; i++)
        mark[tgt[i]] = 1;
    for (i = 0; i < nsrc; i++)
        if (srcc[i] < dist[src[i]]) {
            dist[src[i]] = srcc[i];
            heap_push(&h, srcc[i], src[i]);
        }

    while (heap_pop(&h, &d, &u)) {
        unsigned int k;
        if (d > dist[u])
            continue; /* a stale entry: lazy deletion, no decrease-key */
        /* Every node still in the queue is at least `d` from the start, and
         * every target's total is its own distance plus a non-negative access
         * leg, so nothing left can beat what we already have. Exact, not a
         * heuristic -- which is why this is a break and not an approximation. */
        if (best_total >= 0.0 && d >= best_total)
            break;
        if (mark[u]) {
            for (i = 0; i < ntgt; i++)
                if (tgt[i] == u) {
                    double tot = d + tgtc[i];
                    if (best_total < 0.0 || tot < best_total) {
                        best_total = tot;
                        best_node = u;
                    }
                    break;
                }
        }
        for (k = G.adj[u]; k < G.adj[u + 1]; k++) {
            int v = (int)G.edge[k].to;
            double nd;
            if (!mode_allows(mode, tolls, G.edge[k].flags))
                continue;
            /* WEIGHTED metres, not metres (DESIGN.md 7.7.2). dist[] is now a
             * cost and no longer a distance -- which is why *total_m is measured
             * off the path below instead of being read out of it. */
            nd = d + G.edge[k].len_mm / 1000.0 *
                         class_weight(mode, major, G.edge[k].flags);
            if (nd < dist[v]) {
                dist[v] = nd;
                prev[v] = u;
                heap_push(&h, nd, v);
            }
        }
    }

    if (best_node < 0) {
        refuse(why, nwhy, code, RC_UNREACHABLE, "no road route to there");
        goto done;
    }
    /* Walk back, then reverse in place: the predecessor chain is the only
     * record of the path and it runs the wrong way. */
    for (u = best_node; u >= 0; u = prev[u]) {
        if (count >= maxn) {
            refuse(why, nwhy, code, RC_TOOLONG,
                   "the route is too long to follow");
            goto done;
        }
        nodes[count++] = u;
    }
    for (i = 0; i < count / 2; i++) {
        int t = nodes[i];
        nodes[i] = nodes[count - 1 - i];
        nodes[count - 1 - i] = t;
    }
    /* TRUE METRES, measured off the path Dijkstra chose -- because best_total is
     * a weighted cost now and would report a 20 km route as 17 (DESIGN.md
     * 7.7.2). The same walk the toll block in router_to() does, for the same
     * reason and at the same price: a scan of one node's out-list per hop, over
     * a path of at most ROUTER_MAXPT nodes.
     *
     * The access legs are added UNWEIGHTED at both ends, as they are costed:
     * they are straight lines to a road rather than travel along one, and there
     * is no class to ask about.
     *
     * Where two edges join the same pair of nodes -- a service lane beside its
     * parent road -- the one Dijkstra relaxed is the CHEAPEST, so that is the
     * one measured. `prev` records nodes and not edges, so this has to be
     * re-derived rather than remembered; picking the shortest, or the first,
     * would report a length off a different road from the one being ridden. */
    if (total_m) {
        double t = 0.0;
        int j;
        for (j = 0; j < nsrc; j++)
            if (src[j] == nodes[0]) {
                t += srcc[j];
                break;
            }
        for (j = 0; j + 1 < count; j++) {
            unsigned int k;
            double bestc = -1.0, bestlen = 0.0;
            for (k = G.adj[nodes[j]]; k < G.adj[nodes[j] + 1]; k++) {
                double len, c;
                if ((int)G.edge[k].to != nodes[j + 1])
                    continue;
                if (!mode_allows(mode, tolls, G.edge[k].flags))
                    continue;
                len = G.edge[k].len_mm / 1000.0;
                c = len * class_weight(mode, major, G.edge[k].flags);
                if (bestc < 0.0 || c < bestc) {
                    bestc = c;
                    bestlen = len;
                }
            }
            t += bestlen;
        }
        for (j = 0; j < ntgt; j++)
            if (tgt[j] == best_node) {
                t += tgtc[j];
                break;
            }
        *total_m = t;
    }
    rc = count;

done:
    free(dist);
    free(prev);
    free(mark);
    free(h.e);
    return rc;
}

int
router_to(const roads_t *g, double se, double sn, double de, double dn,
          int mode, int tolls, int major, const char *name, route_t *out,
          char *why, int nwhy, int *code)
{
    static int nodes[ROUTER_MAXPT];
    roadgraph_t G;
    double total = 0.0, lat, lon;
    int n, i, np = 0;
    pt_t *pt;

    if (!out)
        return -1;
    route_init(out);
    n = router_path(g, se, sn, de, dn, mode, tolls, major, nodes, ROUTER_MAXPT,
                    &total, why, nwhy, code);
    if (n < 1)
        return -1;
    roads_graph(g, &G);

    /* The rider's own position first and the point they asked for last: a route
     * that begins at a graph node twenty metres away would start by telling
     * them to ride to somewhere they can already see. */
    pt = (pt_t *)calloc((size_t)n + 2, sizeof *pt);
    if (!pt)
        return refuse(why, nwhy, code, RC_NOMEM,
                      "out of memory building the route");
    roads_unproject(g, se, sn, &lat, &lon);
    pt[np].lat = lat;
    pt[np].lon = lon;
    np++;
    for (i = 0; i < n; i++) {
        double e = G.en[2 * nodes[i]], nn = G.en[2 * nodes[i] + 1];
        double pe, pn;
        roads_project(g, pt[np - 1].lat, pt[np - 1].lon, &pe, &pn);
        if (hypot(e - pe, nn - pn) < DUP_M)
            continue;
        roads_unproject(g, e, nn, &lat, &lon);
        pt[np].lat = lat;
        pt[np].lon = lon;
        np++;
    }
    {
        double pe, pn;
        roads_project(g, pt[np - 1].lat, pt[np - 1].lon, &pe, &pn);
        if (hypot(de - pe, dn - pn) >= DUP_M) {
            roads_unproject(g, de, dn, &lat, &lon);
            pt[np].lat = lat;
            pt[np].lon = lon;
            np++;
        }
    }
    if (np < 2) {
        free(pt);
        return refuse(why, nwhy, code, RC_HERE, "you are already there");
    }

    /* out->toll, MEASURED on the edges Dijkstra actually chose.
     *
     * With CFG_TOLLS_AVOID this can only come out NO, and that is the point of
     * measuring rather than assigning: the badge on CONFIRM then states a fact
     * about the route instead of echoing the setting back, and it stays true if
     * an exclusion is ever relaxed into a weighting. It also catches a packer
     * that never set the bit -- every route would read NO TOLL, and the fixture
     * in tests/roads exists to fail when it does.
     *
     * The edge lookup is a scan of one node's out-list, which is a handful of
     * entries in CSR order; the path is at most ROUTER_MAXPT nodes. */
    {
        int tolled = 0;
        for (i = 0; i + 1 < n && !tolled; i++) {
            unsigned int k;
            for (k = G.adj[nodes[i]]; k < G.adj[nodes[i] + 1]; k++)
                if ((int)G.edge[k].to == nodes[i + 1]) {
                    tolled = (G.edge[k].flags & ROADEDGE_TOLL) != 0;
                    break;
                }
        }
        out->toll = tolled ? ROUTE_TOLL_YES : ROUTE_TOLL_NO;
    }

    out->pt = pt;
    out->npt = np;
    snprintf(out->name, sizeof out->name, "%s",
             name && *name ? name : "DESTINATION");
    if (route_prepare(out)) {
        route_free(out);
        return refuse(why, nwhy, code, RC_TOOSHORT,
                      "the route is too short to follow");
    }
    /* Exactly what route_load() does to a GPX with no cues of its own
     * (DESIGN.md 7.4): derive them from the geometry, then finish. A routed
     * destination and a downloaded one are the same object from here on. */
    route_cues_derive(out);
    route_cues_finish(out);
    return 0;
}

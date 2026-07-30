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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int
router_path(const roads_t *g, double se, double sn, double de, double dn,
            int *nodes, int maxn, double *total_m, char *why, int nwhy)
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
    if (!g || !nodes || maxn < 2) {
        if (why)
            snprintf(why, (size_t)nwhy, "no road pack loaded");
        return -1;
    }
    roads_graph(g, &G);
    if (G.nnode < 2 || G.nedge < 1) {
        if (why)
            snprintf(why, (size_t)nwhy, "the pack has no road graph");
        return -1;
    }

    nsrc = snap(&G, se, sn, src, srcc);
    ntgt = snap(&G, de, dn, tgt, tgtc);
    if (nsrc <= 0 || ntgt <= 0) {
        if (why)
            snprintf(why, (size_t)nwhy, "no road near the start or the finish");
        return -1;
    }

    dist = (double *)malloc((size_t)G.nnode * sizeof *dist);
    prev = (int *)malloc((size_t)G.nnode * sizeof *prev);
    mark = (signed char *)calloc((size_t)G.nnode, 1);
    /* One push per edge, plus the seeds: an exact ceiling, so heap_push()
     * never has to grow and never has to fail in a way that matters. */
    h.max = G.nedge + nsrc + 1;
    h.e = (heapent_t *)malloc((size_t)h.max * sizeof *h.e);
    if (!dist || !prev || !mark || !h.e) {
        if (why)
            snprintf(why, (size_t)nwhy, "out of memory routing");
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
            double nd = d + G.edge[k].len_mm / 1000.0;
            if (nd < dist[v]) {
                dist[v] = nd;
                prev[v] = u;
                heap_push(&h, nd, v);
            }
        }
    }

    if (best_node < 0) {
        if (why)
            snprintf(why, (size_t)nwhy, "no road route to there");
        goto done;
    }
    /* Walk back, then reverse in place: the predecessor chain is the only
     * record of the path and it runs the wrong way. */
    for (u = best_node; u >= 0; u = prev[u]) {
        if (count >= maxn) {
            if (why)
                snprintf(why, (size_t)nwhy, "the route is too long to follow");
            goto done;
        }
        nodes[count++] = u;
    }
    for (i = 0; i < count / 2; i++) {
        int t = nodes[i];
        nodes[i] = nodes[count - 1 - i];
        nodes[count - 1 - i] = t;
    }
    if (total_m)
        *total_m = best_total;
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
          const char *name, route_t *out, char *why, int nwhy)
{
    static int nodes[ROUTER_MAXPT];
    roadgraph_t G;
    double total = 0.0, lat, lon;
    int n, i, np = 0;
    pt_t *pt;

    if (!out)
        return -1;
    route_init(out);
    n = router_path(g, se, sn, de, dn, nodes, ROUTER_MAXPT, &total, why, nwhy);
    if (n < 1)
        return -1;
    roads_graph(g, &G);

    /* The rider's own position first and the point they asked for last: a route
     * that begins at a graph node twenty metres away would start by telling
     * them to ride to somewhere they can already see. */
    pt = (pt_t *)calloc((size_t)n + 2, sizeof *pt);
    if (!pt) {
        if (why && nwhy > 0)
            snprintf(why, (size_t)nwhy, "out of memory building the route");
        return -1;
    }
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
        if (why && nwhy > 0)
            snprintf(why, (size_t)nwhy, "you are already there");
        return -1;
    }

    out->pt = pt;
    out->npt = np;
    snprintf(out->name, sizeof out->name, "%s",
             name && *name ? name : "DESTINATION");
    if (route_prepare(out)) {
        route_free(out);
        if (why && nwhy > 0)
            snprintf(why, (size_t)nwhy, "the route is too short to follow");
        return -1;
    }
    /* Exactly what route_load() does to a GPX with no cues of its own
     * (DESIGN.md 7.4): derive them from the geometry, then finish. A routed
     * destination and a downloaded one are the same object from here on. */
    route_cues_derive(out);
    route_cues_finish(out);
    return 0;
}

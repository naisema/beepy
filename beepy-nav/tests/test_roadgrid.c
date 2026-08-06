/* beepy-nav/tests/test_roadgrid.c -- the spatial index behind the 3D nav view.
 *
 * The graph is built by hand here, node coordinates only, because that is all
 * roadgrid reads. No pack, no Pillow, so this runs on the device inside
 * `make check`.
 *
 * The assertions worth having are the ones a small fixture cannot make by
 * accident: that a query returns a SUPERSET of the true square and never misses
 * a node inside it, that an extent far past any allocatable dense grid still
 * resolves (the mistake the sparse TILE index shipped with -- a 4x4 fixture
 * cannot tell a per-cell bound from a per-occupied-cell one), and that the sort
 * is stable, since two compilers must agree on the goldens downstream.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "roadgrid.h"

static int fails;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        fails++;
    }
}

/* A graph of loose nodes: no edges, which the index never looks at. */
static void
mkgraph(roadgraph_t *g, double *en, int nnode)
{
    memset(g, 0, sizeof *g);
    g->nnode = nnode;
    g->en = en;
    g->nedge = 0;
    g->adj = NULL;
    g->edge = NULL;
}

static int
contains(const int *v, int n, int want)
{
    int i;
    for (i = 0; i < n; i++)
        if (v[i] == want)
            return 1;
    return 0;
}

static void
t_basic(void)
{
    /* Four nodes, one per cell of a 2x2 grid at 100 m cells. */
    static double en[] = { 10.0, 10.0,      /* 0: cell (0,0) */
                           150.0, 10.0,     /* 1: cell (1,0) */
                           10.0, 150.0,     /* 2: cell (0,1) */
                           150.0, 150.0 };  /* 3: cell (1,1) */
    roadgraph_t g;
    roadgrid_t *rg;
    int out[16], n;

    mkgraph(&g, en, 4);
    rg = roadgrid_build(&g, 100.0);
    check(rg != NULL, "a grid builds");
    if (!rg)
        return;
    check(roadgrid_nnode(rg) == 4, "every node is indexed");
    check(roadgrid_ncell(rg) == 4, "four nodes in four cells give four cells");
    check(roadgrid_cell_m(rg) == 100.0, "the cell size is remembered");

    /* A tight query about node 0. Cell granularity means its own cell is
     * returned whole, so this is about not MISSING it. */
    n = roadgrid_nodes(rg, 10.0, 10.0, 1.0, out, 16);
    check(n >= 1 && contains(out, n, 0), "a tight query finds the node in it");
    check(!contains(out, n, 3), "and not the node two cells away");

    /* A query spanning everything. */
    n = roadgrid_nodes(rg, 80.0, 80.0, 200.0, out, 16);
    check(n == 4, "a query over the whole grid returns all four");

    /* Empty space inside the extent: a hole must be a miss, not the nearest
     * cell's contents. Cell (0,0)'s node is at (10,10); ask about (10, 1010),
     * which is cell (0,10) and holds nothing. */
    n = roadgrid_nodes(rg, 10.0, 1010.0, 1.0, out, 16);
    check(n == 0, "an empty cell inside the extent returns nothing");
    roadgrid_free(rg);
}

static void
t_negative_coords(void)
{
    /* The floor-versus-truncate case. These two nodes are 20 m apart across the
     * origin and MUST land in different cells at 10 m; with (int) truncation
     * both would be cell 0 and the index would fuse two disjoint places. */
    static double en[] = { -15.0, -15.0, 15.0, 15.0 };
    roadgraph_t g;
    roadgrid_t *rg;
    int out[8], n;

    mkgraph(&g, en, 2);
    rg = roadgrid_build(&g, 10.0);
    check(rg != NULL, "a grid over negative coordinates builds");
    if (!rg)
        return;
    check(roadgrid_ncell(rg) == 2, "nodes either side of the origin are 2 cells");
    n = roadgrid_nodes(rg, -15.0, -15.0, 1.0, out, 8);
    check(n == 1 && out[0] == 0, "the negative node is found alone");
    n = roadgrid_nodes(rg, 15.0, 15.0, 1.0, out, 8);
    check(n == 1 && out[0] == 1, "and the positive node is found alone");
    roadgrid_free(rg);
}

static void
t_huge_extent(void)
{
    /* THE CASE A SMALL FIXTURE CANNOT REACH, and the one the sparse tile index
     * got wrong: an extent whose cell count is far past anything that could be
     * allocated densely, holding almost nothing.
     *
     * Two nodes 1200 km apart at 256 m cells is an extent of about 4688 x 4688
     * = 22 million cells. A dense index would be 88 MB and rightly refused; this
     * one holds two cells and 24 bytes of key. If the bound were ever written
     * against the EXTENT rather than the occupancy, this fails. */
    static double en[] = { 0.0, 0.0, 1200000.0, 1200000.0 };
    roadgraph_t g;
    roadgrid_t *rg;
    int out[8], n;

    mkgraph(&g, en, 2);
    rg = roadgrid_build(&g, 256.0);
    check(rg != NULL, "a 22-million-cell extent builds -- the bound is occupancy");
    if (!rg)
        return;
    check(roadgrid_ncell(rg) == 2, "and holds exactly two occupied cells");
    check(roadgrid_bytes(rg) < 4096, "in well under 4 KB, not 88 MB");
    n = roadgrid_nodes(rg, 1200000.0, 1200000.0, 10.0, out, 8);
    check(n == 1 && out[0] == 1,
          "the far node resolves 22 million cells from the origin");
    n = roadgrid_nodes(rg, 600000.0, 600000.0, 10.0, out, 8);
    check(n == 0, "and the empty middle stays empty");
    roadgrid_free(rg);
}

static void
t_stable_and_dense(void)
{
    /* Many nodes in ONE cell. Two things: none is lost, and they come back in
     * increasing node order -- which is what "stable radix sort" buys and what
     * two compilers must agree on before any golden downstream can be
     * byte-compared. A qsort could return any permutation here. */
    enum { N = 500 };
    static double en[2 * N];
    roadgraph_t g;
    roadgrid_t *rg;
    int out[N + 8], n, i, ordered = 1;

    for (i = 0; i < N; i++) {
        en[2 * i] = 1000.0 + (double)i * 0.01;      /* all inside one 256 m cell */
        en[2 * i + 1] = 2000.0 + (double)i * 0.01;
    }
    mkgraph(&g, en, N);
    rg = roadgrid_build(&g, 256.0);
    check(rg != NULL, "a dense single cell builds");
    if (!rg)
        return;
    check(roadgrid_ncell(rg) == 1, "500 nodes 5 m apart are one cell");
    n = roadgrid_nodes(rg, 1002.0, 2002.0, 5.0, out, N + 8);
    check(n == N, "and all 500 come back");
    for (i = 1; i < n; i++)
        if (out[i] <= out[i - 1])
            ordered = 0;
    check(ordered, "in increasing node order -- the sort is stable");

    /* The clamp. A caller with room for ten gets ten, not a smashed stack. */
    n = roadgrid_nodes(rg, 1002.0, 2002.0, 5.0, out, 10);
    check(n == 10, "a short buffer is filled and reported, not overrun");
    roadgrid_free(rg);
}

static void
t_refusals(void)
{
    static double en[] = { 0.0, 0.0 };
    roadgraph_t g;
    int out[4];

    mkgraph(&g, en, 1);
    check(roadgrid_build(&g, 0.0) == NULL, "a zero cell size is refused");
    check(roadgrid_build(&g, -5.0) == NULL, "a negative cell size is refused");
    check(roadgrid_build(NULL, 100.0) == NULL, "a null graph is refused");
    mkgraph(&g, en, 0);
    check(roadgrid_build(&g, 100.0) == NULL, "an empty graph is refused");
    check(roadgrid_nodes(NULL, 0, 0, 10, out, 4) == 0, "a null index returns 0");
    /* And the accessors survive it, because a caller that failed to build still
     * prints a provenance line. */
    check(roadgrid_ncell(NULL) == 0 && roadgrid_nnode(NULL) == 0 &&
          roadgrid_bytes(NULL) == 0, "the accessors tolerate NULL");
    roadgrid_free(NULL);
}

int
main(void)
{
    t_basic();
    t_negative_coords();
    t_huge_extent();
    t_stable_and_dense();
    t_refusals();
    if (fails) {
        printf("test_roadgrid: %d FAILURES\n", fails);
        return 1;
    }
    printf("test_roadgrid: OK\n");
    return 0;
}

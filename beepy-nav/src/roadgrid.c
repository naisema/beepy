/* beepy-nav/src/roadgrid.c -- see roadgrid.h. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "roadgrid.h"

struct roadgrid {
    double cell_m;
    int32_t gx0, gy0;        /* the grid origin, in cells                   */
    uint32_t nx, ny;         /* the extent, in cells -- NOT allocated       */
    int nnode;

    uint32_t *order;         /* nnode node indices, grouped by cell         */
    uint32_t *ckey;          /* ncell keys, strictly increasing             */
    uint32_t *cfirst;        /* ncell + 1 offsets into `order`              */
    int ncell;
};

static int32_t
cell_of(double v, double cell_m)
{
    /* floor and not truncation: a node 3 m west of the origin belongs to cell
     * -1, and (int)(-3.0/256.0) would put it in cell 0 alongside a node 3 m
     * east. The grid would still be usable but two disjoint areas would share
     * a key, which is exactly the bug the tile grid's arithmetic-shift comment
     * warns about. */
    return (int32_t)floor(v / cell_m);
}

/* Stable LSD radix sort on the 32-bit key, four 8-bit passes.
 *
 * Not qsort, for two reasons that both matter here. 1.74 million nodes is about
 * 36 million comparisons through a function pointer, which is seconds on a Pi
 * Zero 2 W and would be paid on the first 3D frame -- a visible stall. And this
 * project byte-compares goldens between clang and gcc: qsort is not required to
 * be stable, the two libraries do not use the same algorithm, and two nodes in
 * one cell could come out in either order. A stable radix sort has one answer.
 */
static int
radix_by_key(const uint32_t *key, int n, uint32_t *order)
{
    uint32_t *tmp = (uint32_t *)malloc((size_t)n * sizeof *tmp);
    int pass, i;
    if (!tmp)
        return 0;
    for (i = 0; i < n; i++)
        order[i] = (uint32_t)i;
    for (pass = 0; pass < 4; pass++) {
        size_t count[257];
        int shift = pass * 8;
        memset(count, 0, sizeof count);
        for (i = 0; i < n; i++)
            count[((key[order[i]] >> shift) & 0xFFu) + 1]++;
        for (i = 0; i < 256; i++)
            count[i + 1] += count[i];
        for (i = 0; i < n; i++) {
            uint32_t nd = order[i];
            tmp[count[(key[nd] >> shift) & 0xFFu]++] = nd;
        }
        memcpy(order, tmp, (size_t)n * sizeof *order);
    }
    free(tmp);
    return 1;
}

roadgrid_t *
roadgrid_build(const roadgraph_t *g, double cell_m)
{
    roadgrid_t *rg;
    uint32_t *key = NULL;
    double e0, n0, e1, n1;
    int64_t nx, ny;
    int i, ncell;

    if (!g || g->nnode <= 0 || !g->en || !(cell_m > 0.0))
        return NULL;

    e0 = e1 = g->en[0];
    n0 = n1 = g->en[1];
    for (i = 1; i < g->nnode; i++) {
        double e = g->en[2 * i], n = g->en[2 * i + 1];
        if (e < e0) e0 = e;
        if (e > e1) e1 = e;
        if (n < n0) n0 = n;
        if (n > n1) n1 = n;
    }

    rg = (roadgrid_t *)calloc(1, sizeof *rg);
    if (!rg)
        return NULL;
    rg->cell_m = cell_m;
    rg->nnode = g->nnode;
    rg->gx0 = cell_of(e0, cell_m);
    rg->gy0 = cell_of(n0, cell_m);
    nx = (int64_t)cell_of(e1, cell_m) - rg->gx0 + 1;
    ny = (int64_t)cell_of(n1, cell_m) - rg->gy0 + 1;
    /* The key is gy*nx + gx as a u32, so the EXTENT is what is bounded here --
     * never the occupied count, which is what gets allocated. At 256 m cells
     * this refuses only a pack spanning some 16 000 km. */
    if (nx <= 0 || ny <= 0 || nx * ny > 0xFFFFFFFFLL) {
        free(rg);
        return NULL;
    }
    rg->nx = (uint32_t)nx;
    rg->ny = (uint32_t)ny;

    key = (uint32_t *)malloc((size_t)g->nnode * sizeof *key);
    rg->order = (uint32_t *)malloc((size_t)g->nnode * sizeof *rg->order);
    if (!key || !rg->order)
        goto bad;
    for (i = 0; i < g->nnode; i++) {
        uint32_t gx = (uint32_t)(cell_of(g->en[2 * i], cell_m) - rg->gx0);
        uint32_t gy = (uint32_t)(cell_of(g->en[2 * i + 1], cell_m) - rg->gy0);
        key[i] = gy * rg->nx + gx;
    }
    if (!radix_by_key(key, g->nnode, rg->order))
        goto bad;

    /* One run per occupied cell, and the runs are already adjacent. */
    ncell = 0;
    for (i = 0; i < g->nnode; i++)
        if (i == 0 || key[rg->order[i]] != key[rg->order[i - 1]])
            ncell++;
    rg->ckey = (uint32_t *)malloc((size_t)ncell * sizeof *rg->ckey);
    rg->cfirst = (uint32_t *)malloc((size_t)(ncell + 1) * sizeof *rg->cfirst);
    if (!rg->ckey || !rg->cfirst)
        goto bad;
    rg->ncell = 0;
    for (i = 0; i < g->nnode; i++) {
        if (i == 0 || key[rg->order[i]] != key[rg->order[i - 1]]) {
            rg->ckey[rg->ncell] = key[rg->order[i]];
            rg->cfirst[rg->ncell] = (uint32_t)i;
            rg->ncell++;
        }
    }
    rg->cfirst[rg->ncell] = (uint32_t)g->nnode;
    free(key);
    return rg;

bad:
    free(key);
    roadgrid_free(rg);
    return NULL;
}

void
roadgrid_free(roadgrid_t *rg)
{
    if (!rg)
        return;
    free(rg->order);
    free(rg->ckey);
    free(rg->cfirst);
    free(rg);
}

/* The index of `k` in ckey, or -1. Plain lower-bound binary search; the array
 * is strictly increasing by construction, so no verification pass is needed. */
static int
find_cell(const roadgrid_t *rg, uint32_t k)
{
    int lo = 0, hi = rg->ncell - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (rg->ckey[mid] == k)
            return mid;
        if (rg->ckey[mid] < k)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int
roadgrid_nodes(const roadgrid_t *rg, double e, double n, double half_m,
               int *out, int max)
{
    int32_t gxa, gxb, gya, gyb, gx, gy;
    int wrote = 0;

    if (!rg || !out || max <= 0 || !(half_m >= 0.0))
        return 0;
    gxa = cell_of(e - half_m, rg->cell_m) - rg->gx0;
    gxb = cell_of(e + half_m, rg->cell_m) - rg->gx0;
    gya = cell_of(n - half_m, rg->cell_m) - rg->gy0;
    gyb = cell_of(n + half_m, rg->cell_m) - rg->gy0;
    /* Clamp to the grid rather than skipping: a rider at the edge of the pack
     * has a query square that is half outside it, and the inside half is still
     * worth drawing. */
    if (gxa < 0) gxa = 0;
    if (gya < 0) gya = 0;
    if (gxb >= (int32_t)rg->nx) gxb = (int32_t)rg->nx - 1;
    if (gyb >= (int32_t)rg->ny) gyb = (int32_t)rg->ny - 1;

    for (gy = gya; gy <= gyb; gy++) {
        for (gx = gxa; gx <= gxb; gx++) {
            int c = find_cell(rg, (uint32_t)gy * rg->nx + (uint32_t)gx);
            uint32_t j;
            if (c < 0)
                continue;
            for (j = rg->cfirst[c]; j < rg->cfirst[c + 1]; j++) {
                if (wrote >= max)
                    return wrote;
                out[wrote++] = (int)rg->order[j];
            }
        }
    }
    return wrote;
}

void
roadgrid_cell_at(const roadgrid_t *rg, double e, double n, int *gx, int *gy)
{
    if (!rg) {
        if (gx) *gx = 0;
        if (gy) *gy = 0;
        return;
    }
    if (gx) *gx = (int)(cell_of(e, rg->cell_m) - rg->gx0);
    if (gy) *gy = (int)(cell_of(n, rg->cell_m) - rg->gy0);
}

const unsigned int *
roadgrid_cell(const roadgrid_t *rg, int gx, int gy, int *count)
{
    int c;
    if (count)
        *count = 0;
    if (!rg || gx < 0 || gy < 0 || gx >= (int)rg->nx || gy >= (int)rg->ny)
        return NULL;
    c = find_cell(rg, (uint32_t)gy * rg->nx + (uint32_t)gx);
    if (c < 0)
        return NULL;
    if (count)
        *count = (int)(rg->cfirst[c + 1] - rg->cfirst[c]);
    return &rg->order[rg->cfirst[c]];
}

int
roadgrid_ncell(const roadgrid_t *rg)
{
    return rg ? rg->ncell : 0;
}

int
roadgrid_nnode(const roadgrid_t *rg)
{
    return rg ? rg->nnode : 0;
}

double
roadgrid_cell_m(const roadgrid_t *rg)
{
    return rg ? rg->cell_m : 0.0;
}

long
roadgrid_bytes(const roadgrid_t *rg)
{
    if (!rg)
        return 0;
    return (long)sizeof *rg +
           (long)rg->nnode * (long)sizeof *rg->order +
           (long)rg->ncell * (long)sizeof *rg->ckey +
           (long)(rg->ncell + 1) * (long)sizeof *rg->cfirst;
}

/* beepy-nav/src/roadgrid.h -- a spatial index over the road graph's nodes.
 *
 * WHY THIS EXISTS. The roads pack has six sections -- NODES, ADJ, EDGES,
 * PLACES, POINTS, STRINGS -- and not one of them is spatial. Nothing needed one
 * until now: router.c's snap() scans all 1 738 370 nodes once per route, which
 * is paid at the moment the rider presses ENTER and never again. The 3D nav view
 * (DESIGN.md 6.6) needs "every road near the rider" on EVERY FRAME at 8 Hz.
 *
 * MEASURED ON THE DEVICE, allregions.roads, a 1.2 km half-square about Asok:
 *
 *     full scan of 1 738 370 nodes    33.2 ms per frame   27% of the budget
 *     this index                       0.088 ms per frame  0.07%
 *     build                          950 ms once, 7.41 MB, 102 686 cells
 *
 * 27% is the honest figure and it is not "impossible" -- an earlier draft of
 * this comment said the scan would be the whole frame budget, which was wrong
 * and worth correcting. It is still a quarter of every frame spent finding what
 * to draw before drawing any of it, on a page that also has to project and fill
 * some three thousand ribbons. 378x for 7.4 MB is the trade.
 *
 * The index is built in RAM from the pack rather than stored in it: putting it
 * in the pack would bump the format and invalidate every pack already on a
 * device, to save a cost paid once per boot. It is built LAZILY, on the first 3D
 * frame, so a rider who never leaves 2D never pays for it.
 *
 * THE 950 ms IS A VISIBLE STALL and the caller must treat it as one -- at 8 Hz
 * it drops about seven frames, so it belongs behind a note (7.5) on the frame
 * where the rider presses the key, not silently in the middle of a ride.
 *
 * The shape is the sparse tile index again (6.5): sorted keys, binary search.
 * A uniform grid over the pack's own metre frame would be 34 million cells for
 * four Thai regions at 256 m, and 0.4% of them hold a node.
 */
#ifndef BEEPY_NAV_ROADGRID_H
#define BEEPY_NAV_ROADGRID_H

#include "search.h"

typedef struct roadgrid roadgrid_t;

/* Build an index over `g`'s nodes with square cells of `cell_m` metres.
 * Returns NULL on out-of-memory or if the graph's extent needs more than a u32
 * of cell key, which for 256 m cells would mean a pack spanning 16 000 km.
 * `g` must outlive the returned index -- only node coordinates are read. */
roadgrid_t *roadgrid_build(const roadgraph_t *g, double cell_m);
void roadgrid_free(roadgrid_t *rg);

/* Node indices whose CELL intersects the axis-aligned square of half-width
 * `half_m` about (e, n), in the pack's frame. Writes at most `max` of them to
 * `out` and returns the number written; the return is clamped, so a caller that
 * cares whether it saw everything compares against `max`.
 *
 * Cell granularity means the result is a superset of the true square -- a node
 * up to one cell outside can be returned. Callers draw with their own distance
 * culling anyway, so filtering here would be work done twice. */
int roadgrid_nodes(const roadgrid_t *rg, double e, double n, double half_m,
                   int *out, int max);

/* ---------------------------------------------------------------- streaming
 *
 * roadgrid_nodes() above fills a buffer, which means a CAP, and a cap means
 * truncation. Its truncation order is the cell scan order -- south to north --
 * which is the worst possible one: in a dense city a 4 km query overruns any
 * sane buffer and what gets dropped is whichever half of the map the scan
 * reached last, possibly including the ground the rider is standing on. That is
 * how the first 3D frames came out mysteriously sparse.
 *
 * So a drawing caller does not use it. It asks for the rider's cell and walks
 * rings outward, taking cells until it has enough -- then anything dropped is
 * the FARTHEST material, which is what the distance and width culls would have
 * discarded anyway. It also needs no buffer and no per-frame malloc. */

/* The cell containing (e, n), in this index's cell coordinates. Out-of-extent
 * points still return a coordinate; roadgrid_cell() simply finds nothing. */
void roadgrid_cell_at(const roadgrid_t *rg, double e, double n,
                      int *gx, int *gy);

/* The nodes of one cell, as a borrowed pointer into the index, or NULL. The
 * order is increasing node index -- the stable sort guarantees it. */
const unsigned int *roadgrid_cell(const roadgrid_t *rg, int gx, int gy,
                                  int *count);

/* Provenance, for the unit tests and the one stderr line. */
int roadgrid_ncell(const roadgrid_t *rg);   /* OCCUPIED cells, not the extent */
int roadgrid_nnode(const roadgrid_t *rg);
double roadgrid_cell_m(const roadgrid_t *rg);
/* Bytes held, so DESIGN.md's figure can be asserted rather than remembered. */
long roadgrid_bytes(const roadgrid_t *rg);

#endif /* BEEPY_NAV_ROADGRID_H */

/* beepy-nav/src/tile.h -- the optional OSM raster basemap (DESIGN.md 6.5).
 *
 * A tile pack is a file built on the Mac by tools/mktiles.py: 1-bit 256x256
 * tiles of OSM street geometry, pre-rendered per zoom rung of DESIGN.md 6.1's
 * ladder, in the local tangent frame of 6.1. Its complete format is written
 * out at the top of mktiles.py and in DESIGN.md 6.5; nothing here parses text.
 *
 * The layer is OPTIONAL in the strongest sense: with no pack open -- because
 * none was configured, because the file is missing, because it is corrupt,
 * because the current zoom is not a rung the pack carries, or because the view
 * has left the corridor -- every one of those paths draws exactly nothing, and
 * the frame is byte-identical to the frame the navigator drew before this file
 * existed. `make check` is that assertion: the five original nav goldens are
 * rendered with no pack and must still compare equal.
 *
 * Portable C, libc only (plus libbeepyfb for the blit). It compiles and runs
 * in the host lane.
 */
#ifndef BEEPY_NAV_TILE_H
#define BEEPY_NAV_TILE_H

#include "libbeepyfb/cover.h"

typedef struct tiles tiles_t;

/* Where and how to sample: the same projection parameters view_nav_map()
 * hands map_project(), plus the destination rectangle.
 *
 *   e' = (e - org_e)*cos(theta) - (n - org_n)*sin(theta)
 *   sx = cx + e'/mpp                    (map.h's convention, +theta)
 *
 * `mpp` must equal one of the pack's zoom rungs exactly or nothing is drawn:
 * a resampled rung would be a blurred street grid, and this layer's whole
 * argument for being a raster is that it stays crisp.
 */
typedef struct {
    double mpp;
    double cx, cy;
    double theta;
    double org_e, org_n; /* world metres at (cx, cy) */
    int x0, y0, x1, y1;  /* destination rect, inclusive */
} tileview_t;

/* Open a pack, or NULL. `why` (optional, `nwhy` bytes) receives a one-line
 * reason on failure -- the caller prints it once and carries on without a
 * basemap. The pack's own tangent frame is assumed until tiles_bind_route()
 * says otherwise. */
tiles_t *tiles_open(const char *path, char *why, int nwhy);
void tiles_close(tiles_t *t);

/* Tell the pack which frame the caller's world metres are in: the navigator's
 * origin is its route's first point (route.c), which is the same point
 * mktiles.py referenced the pack to when the pack was built FOR that route --
 * in which case this is exactly a zero offset. A pack reused with a different
 * route gets the translation between the two references. */
void tiles_bind_route(tiles_t *t, double lat0, double lon0);

/* Blit the basemap under everything else. Returns the number of tiles that
 * contributed -- 0 means the frame is untouched, which is the case for every
 * "no basemap here" reason listed above. */
int tiles_blit(cov_t *c, tiles_t *t, const tileview_t *v);

/* For --basemap's one line of provenance and for tests. */
double tiles_ref_lat(const tiles_t *t);
double tiles_ref_lon(const tiles_t *t);
int tiles_nzoom(const tiles_t *t);
double tiles_zoom(const tiles_t *t, int i);
int tiles_count(const tiles_t *t);

/* DESIGN.md 6.5: "Attribution ... goes in the README and on the OVERVIEW
 * title line when tiles are present." The 5x7 font has no glyph for U+00A9,
 * so the screen spells it. */
#define TILES_ATTRIB "(C) OPENSTREETMAP CONTRIBUTORS"

#endif /* BEEPY_NAV_TILE_H */

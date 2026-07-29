/* beepy-nav/src/gpx.h -- the hand-written GPX scanner (DESIGN.md 7.1).
 *
 * Not an XML parser: find <trkpt or <rtept, pull lat/lon in either order,
 * then <ele> <name> <cmt> <desc> <sym> <type> up to the closing tag. Entity
 * decoding (&amp; &lt; &gt; &quot; &apos; &#nn;) happens in text content
 * only, never in attribute values or tag names.
 *
 * libexpat is installed on the device with headers, so this is a choice:
 * the GPX that matters is machine-generated and flat, and zero dependencies
 * is what keeps a Buildroot package possible.
 *
 * Failure is total. A malformed file produces an error naming the line and
 * NO route -- half a route is worse than none, because the display cannot
 * tell you it is only following part of the way.
 */
#ifndef BEEPY_NAV_GPX_H
#define BEEPY_NAV_GPX_H

#include <stddef.h>

#include "route.h"

/* Refuse absurd input before it becomes an allocation. 2 M raw points is
 * ~100x the cap and ~120 MB of GPX text. */
#define GPX_MAXRAW 2000000

/* Parse an in-memory document. `r` is initialised by the call; on failure it
 * is left empty and err (if non-NULL) holds "line N: ...". 0 / -1. */
int gpx_parse(const char *xml, size_t len, route_t *r, char *err,
              size_t errsz);

/* Read the file and parse it. Adds "path: " in front of the message. */
int gpx_load(const char *path, route_t *r, char *err, size_t errsz);

/* CUE_* for a GPX <sym> or <type> value ("Left", "Slight Right", "TRT" ...),
 * or -1 when the string names nothing we draw. Case-insensitive, and
 * separators (space, dash, underscore) are ignored, because the emitters
 * disagree about all three. */
int gpx_sym_kind(const char *s);

#endif /* BEEPY_NAV_GPX_H */

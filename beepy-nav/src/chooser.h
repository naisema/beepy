/* beepy-nav/src/chooser.h -- the startup route list (DESIGN.md 2, D8).
 *
 * "Run with no route, it lists the .gpx files in /home/beepy/routes and
 * waits for a selection -- a startup chooser, not a page."
 *
 * The list must be DRAWN, not printed. fbterm is SIGSTOPped for as long as
 * the panel is owned, so there is no console to print to and no stdin to
 * read; the keys come from evdev exactly as they do in the main loop. That
 * splits the work in two, and the split is the reason for this header:
 *
 *   view_chooser()  portable, pixels only -- so the page can be dumped and
 *                   looked at without a device.
 *   chooser_scan()  portable, the directory listing.
 *   chooser_run()   device-only, in nav.c: the panel and the evdev loop.
 */
#ifndef BEEPY_NAV_CHOOSER_H
#define BEEPY_NAV_CHOOSER_H

#include <stddef.h>

#include "libbeepyfb/cover.h"

#define CHOOSER_MAX 64
#define CHOOSER_NAME 40

typedef struct {
    char path[CHOOSER_MAX][256];
    char name[CHOOSER_MAX][CHOOSER_NAME]; /* the basename, no .gpx */
    int n;
    int sel;
    int top; /* first visible row, for lists longer than the screen */
    char dir[192];
} chooser_t;

/* Every *.gpx in `dir`, sorted by name. Returns the count (0 is not an
 * error -- the page says so, which is more use than a message on a console
 * nobody can see). */
int chooser_scan(chooser_t *c, const char *dir);

/* ~/routes, or $BEEPY_ROUTES when it is set. */
void chooser_default_dir(char *buf, size_t n);

void view_chooser(cov_t *c, const chooser_t *ch);

/* N/P move, wrapping; keeps the selection on screen. */
void chooser_move(chooser_t *c, int delta);

#endif /* BEEPY_NAV_CHOOSER_H */

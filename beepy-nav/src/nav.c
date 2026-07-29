/* beepy-nav -- route navigator for the Beepy's 400x240 Sharp panel.
 *
 * M2 is the rendering milestone: this front end draws one static page and
 * either writes it out as a raw frame or times the renderer. The live GPS
 * loop, pages and key bindings arrive in M3.
 *
 *   beepy-nav --demo --page nav|nav-off|arrows --dump FILE
 *   beepy-nav --demo --page nav --bench 100
 *
 * The dump is the panel's own format (384000 bytes of XRGB), so it can be
 * shown with fbshow, compared with cmp against goldens/, or diffed against
 * the design mockups with tools/fbdiff.py -- on the device or on the Mac.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/dump.h"

#include "view.h"

static const char USAGE[] =
    "usage: beepy-nav --demo --page nav|nav-off|arrows [--dump FILE]\n"
    "                 [--bench N]\n"
    "\n"
    "  --demo        render the static design state (the only source in M2)\n"
    "  --page P      nav (turn panel + map), nav-off (off route), arrows\n"
    "  --dump FILE   write the frame as 384000 raw XRGB bytes\n"
    "  --bench N     time N draw+resolve cycles and print ms/frame\n";

enum { PAGE_NAV, PAGE_NAV_OFF, PAGE_ARROWS };

static void
render(cov_t *cov, canvas_t *cv, int page)
{
    cov_begin(cov);
    switch (page) {
    case PAGE_NAV_OFF:
        view_nav_demo(cov, 85);
        break;
    case PAGE_ARROWS:
        view_arrows(cov);
        break;
    default:
        view_nav_demo(cov, 0);
        break;
    }
    cov_resolve(cov, cv);
}

/* The coverage buffer is 96 KB; keep it out of the stack. */
static cov_t COV;

int
main(int argc, char **argv)
{
    const char *dumppath = NULL;
    int page = PAGE_NAV, bench = 0, demo = 0, i;
    canvas_t *cv;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--demo"))
            demo = 1;
        else if (!strcmp(a, "--page") && i + 1 < argc) {
            const char *p = argv[++i];
            if (!strcmp(p, "nav"))
                page = PAGE_NAV;
            else if (!strcmp(p, "nav-off"))
                page = PAGE_NAV_OFF;
            else if (!strcmp(p, "arrows"))
                page = PAGE_ARROWS;
            else {
                fprintf(stderr, "unknown page: %s\n%s", p, USAGE);
                return 2;
            }
        } else if (!strcmp(a, "--dump") && i + 1 < argc)
            dumppath = argv[++i];
        else if (!strcmp(a, "--bench") && i + 1 < argc)
            bench = atoi(argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(USAGE, stdout);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n%s", a, USAGE);
            return 2;
        }
    }
    if (!demo) {
        /* M2 has no live source yet, so --demo is not optional; saying so
         * is friendlier than drawing the demo state unasked. */
        fputs("beepy-nav: M2 renders --demo pages only\n", stderr);
        fputs(USAGE, stderr);
        return 2;
    }
    if (!dumppath && bench <= 0) {
        fputs("beepy-nav: nothing to do (--dump or --bench)\n", stderr);
        fputs(USAGE, stderr);
        return 2;
    }

    cv = canvas_new(SCR_W, SCR_H);
    if (!cv) {
        fputs("beepy-nav: out of memory\n", stderr);
        return 1;
    }

    if (bench > 0) {
        /* draw + resolve only: no file writes, no panel, no allocation. */
        struct timespec t0, t1;
        double ms;
        render(&COV, cv, page); /* warm the caches; not counted */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (i = 0; i < bench; i++)
            render(&COV, cv, page);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = ((double)(t1.tv_sec - t0.tv_sec) * 1e3 +
              (double)(t1.tv_nsec - t0.tv_nsec) / 1e6) /
             bench;
        printf("bench: %d frames, %.3f ms/frame\n", bench, ms);
    } else {
        render(&COV, cv, page);
    }

    if (dumppath) {
        if (canvas_dump(cv, dumppath) < 0) {
            perror(dumppath);
            return 1;
        }
        fprintf(stderr, "wrote %s\n", dumppath);
    }
    return 0;
}

/* beepy-nav/src/chooser.c -- see chooser.h.
 *
 * Portable: dirent.h and the 5x7 font, nothing else. The evdev half lives in
 * nav.c, behind NAV_DEVICE.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chooser.h"

#define W SCR_W
#define H SCR_H

/* Scale 2 throughout, like every other page: DESIGN.md 1.1's floor exists
 * because scale-1 text was unreadable on the physical panel, and a list you
 * squint at while straddling a bike is no better than a distance you squint
 * at. Rows are 22 px, which is the glyph cell (16) plus breathing space. */
#define ROW_H 22
#define TITLE_H 24
#define FOOT_H 20
#define ROWS ((H - TITLE_H - FOOT_H) / ROW_H)

static int
cmp_name(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void
chooser_default_dir(char *buf, size_t n)
{
    const char *env = getenv("BEEPY_ROUTES");
    const char *home = getenv("HOME");
    if (env && *env)
        snprintf(buf, n, "%s", env);
    else
        snprintf(buf, n, "%s/routes", home && *home ? home : "/home/beepy");
}

int
chooser_scan(chooser_t *c, const char *dir)
{
    static char names[CHOOSER_MAX][CHOOSER_NAME];
    DIR *d;
    struct dirent *e;
    int i, n = 0;

    memset(c, 0, sizeof *c);
    snprintf(c->dir, sizeof c->dir, "%.*s", (int)sizeof c->dir - 1, dir);
    d = opendir(dir);
    if (!d)
        return 0;
    while ((e = readdir(d)) && n < CHOOSER_MAX) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".gpx"))
            continue;
        if (e->d_name[0] == '.')
            continue;
        /* Deliberate truncation, stated as a precision: a 250-character
         * route name is not going to fit on a 400 px panel either. */
        snprintf(names[n], CHOOSER_NAME, "%.*s", CHOOSER_NAME - 1,
                 e->d_name);
        n++;
    }
    closedir(d);
    /* readdir order is whatever the filesystem feels like; a list that
     * reorders itself between runs is unusable with two keys. */
    qsort(names, (size_t)n, CHOOSER_NAME, cmp_name);
    for (i = 0; i < n; i++) {
        size_t len;
        snprintf(c->path[i], sizeof c->path[i], "%.*s/%.*s",
                 (int)sizeof c->dir - 1, dir, CHOOSER_NAME - 1,
                 names[i]);
        snprintf(c->name[i], CHOOSER_NAME, "%.*s", CHOOSER_NAME - 1,
                 names[i]);
        len = strlen(c->name[i]);
        if (len > 4)
            c->name[i][len - 4] = '\0'; /* drop .gpx: it is all of them */
    }
    c->n = n;
    return n;
}

void
chooser_move(chooser_t *c, int delta)
{
    if (c->n <= 0)
        return;
    c->sel = (c->sel + delta % c->n + c->n) % c->n;
    if (c->sel < c->top)
        c->top = c->sel;
    if (c->sel >= c->top + ROWS)
        c->top = c->sel - ROWS + 1;
}

void
view_chooser(cov_t *c, const chooser_t *ch)
{
    char buf[64];
    int i;

    cov_fill_rect(c, 0, 0, W - 1, TITLE_H - 1, COV_INK);
    cov_text(c, 6, 4, "ROUTES", 2, COV_PAPER);
    if (ch->n > 0)
        snprintf(buf, sizeof buf, "%d/%d", ch->sel + 1, ch->n);
    else
        snprintf(buf, sizeof buf, "NONE");
    cov_text(c, W - 6 - cov_text_w(buf, 2), 4, buf, 2, COV_PAPER);

    if (ch->n == 0) {
        /* An empty directory is the likeliest first-run state, so it says
         * where it looked rather than just "none". The path is scale 1 --
         * the one exception to the floor, because it is read once, standing
         * still, and 33 characters of scale 2 will not hold a path. */
        cov_text(c, 6, TITLE_H + 16, "NO .GPX FILES IN", 2, COV_INK);
        cov_text(c, 6, TITLE_H + 40, ch->dir, 1, COV_INK);
        cov_text(c, 6, TITLE_H + 64, "COPY ONE THERE, OR USE", 2, COV_INK);
        cov_text(c, 6, TITLE_H + 86, "--ROUTE FILE.GPX", 2, COV_INK);
    }
    for (i = 0; i < ROWS && ch->top + i < ch->n; i++) {
        int k = ch->top + i, y = TITLE_H + i * ROW_H;
        int on = k == ch->sel;
        if (on)
            cov_fill_rect(c, 0, y, W - 1, y + ROW_H - 1, COV_INK);
        cov_text(c, 6, y + 3, ch->name[k], 2, on ? COV_PAPER : COV_INK);
    }

    cov_fill_rect(c, 0, H - FOOT_H, W - 1, H - 1, COV_INK);
    cov_text(c, 6, H - FOOT_H + 3, "N/P MOVE  ENTER GO  Q QUIT", 1,
             COV_PAPER);
}

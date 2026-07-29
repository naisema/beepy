/* beepy-nav/tests/test_gpx.c -- the GPX scanner against tests/gpx/.
 *
 * One case per fixture, and every malformed fixture must fail with a message
 * naming a line -- DESIGN.md 7.1's "fails with a message naming the line,
 * rather than half-loading" is the requirement that makes a hand-written
 * scanner defensible, so it is checked, not assumed.
 *
 *     make test-unit
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "gpx.h"
#include "route.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

static int failures;
static const char *DIR = "beepy-nav/tests/gpx";

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void
eq_int(long got, long want, const char *what)
{
    if (got != want) {
        printf("FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static void
eq_str(const char *got, const char *want, const char *what)
{
    if (strcmp(got, want)) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

static void
close_to(double got, double want, double tol, const char *what)
{
    if (!(fabs(got - want) <= tol)) {
        printf("FAIL %s: got %.9g, want %.9g\n", what, got, want);
        failures++;
    }
}

static void
path_of(char *buf, size_t n, const char *file)
{
    snprintf(buf, n, "%s/%s", DIR, file);
}

/* ------------------------------------------------------------ well formed */

/* A <trk> with elevation and timestamps and no cues whatsoever: the Komoot
 * export, and the case DESIGN.md 7.4 says must fall back to derivation. */
static void
test_komoot(void)
{
    route_t r;
    char p[256], err[256];
    path_of(p, sizeof p, "komoot-style.gpx");
    if (gpx_load(p, &r, err, sizeof err)) {
        printf("FAIL komoot: %s\n", err);
        failures++;
        return;
    }
    eq_int(r.npt, 61, "komoot point count");
    eq_int(r.ncue, 0, "komoot has no cues in the file");
    eq_str(r.name, "Asok Test Ride", "komoot route name");
    close_to(r.pt[0].ele, 2.0, 1e-9, "komoot first ele");
    close_to(r.pt[60].ele, 8.0, 1e-9, "komoot last ele");
    /* 300 m north then 300 m east, sampled at 10 m. */
    check(route_prepare(&r) == 0, "komoot prepare");
    close_to(r.total_m, 600.0, 1.0, "komoot total length");
    route_free(&r);
}

/* A <rte> of <rtept> with <sym>/<cmt>: the cues are IN the file and must be
 * preferred over anything derivation would invent. */
static void
test_rwgps(void)
{
    route_t r;
    char p[256], err[256];
    path_of(p, sizeof p, "rwgps-style.gpx");
    if (gpx_load(p, &r, err, sizeof err)) {
        printf("FAIL rwgps: %s\n", err);
        failures++;
        return;
    }
    eq_int(r.npt, 7, "rwgps point count");
    eq_int(r.ncue, 4, "rwgps cue count");
    eq_str(r.name, "RWGPS Cue Test", "rwgps route name");
    eq_int(r.cue[0].idx, 0, "rwgps cue 0 index");
    eq_int(r.cue[0].kind, CUE_STRAIGHT, "rwgps cue 0 kind");
    eq_int(r.cue[1].idx, 2, "rwgps cue 1 index");
    eq_int(r.cue[1].kind, CUE_RIGHT, "rwgps cue 1 kind");
    eq_int(r.cue[2].idx, 4, "rwgps cue 2 index");
    eq_int(r.cue[2].kind, CUE_SLIGHT_LEFT, "rwgps cue 2 kind");
    eq_int(r.cue[3].idx, 6, "rwgps cue 3 index");
    eq_int(r.cue[3].kind, CUE_DEST, "rwgps cue 3 kind");
    /* <cmt> beats <desc> beats <name>, whatever order they appear in. */
    eq_str(r.cue[1].name, "Turn right & go east", "rwgps cue 1 name from cmt");
    route_free(&r);
}

/* No <name> anywhere, no <ele>, self-closing points. */
static void
test_bare(void)
{
    route_t r;
    char p[256], err[256];
    path_of(p, sizeof p, "bare-trk.gpx");
    if (gpx_load(p, &r, err, sizeof err)) {
        printf("FAIL bare: %s\n", err);
        failures++;
        return;
    }
    eq_int(r.npt, 5, "bare point count");
    eq_int(r.ncue, 0, "bare cue count");
    eq_str(r.name, "ROUTE", "bare route name falls back");
    close_to(r.pt[0].ele, 0.0, 1e-9, "bare ele defaults to 0");
    route_free(&r);
}

/* lat/lon in EITHER order, single and double quotes, extra attributes. */
static void
test_attrs_reversed(void)
{
    route_t r, r2;
    char p[256], q[256], err[256];
    int i;
    path_of(p, sizeof p, "attrs-reversed.gpx");
    path_of(q, sizeof q, "bare-trk.gpx");
    if (gpx_load(p, &r, err, sizeof err)) {
        printf("FAIL attrs-reversed: %s\n", err);
        failures++;
        return;
    }
    eq_int(r.npt, 4, "attrs-reversed point count");
    /* The first four points are the same places as bare-trk's, so the order
     * of the attributes provably did not change the reading. */
    if (!gpx_load(q, &r2, err, sizeof err)) {
        for (i = 0; i < 4; i++) {
            close_to(r.pt[i].lat, r2.pt[i].lat, 1e-12, "reversed lat matches");
            close_to(r.pt[i].lon, r2.pt[i].lon, 1e-12, "reversed lon matches");
        }
        route_free(&r2);
    }
    route_free(&r);
}

/* DESIGN.md 7.1: decode in TEXT fields only. */
static void
test_entities(void)
{
    route_t r;
    char p[256], err[256];
    path_of(p, sizeof p, "entities.gpx");
    if (gpx_load(p, &r, err, sizeof err)) {
        printf("FAIL entities: %s\n", err);
        failures++;
        return;
    }
    eq_int(r.npt, 3, "entities point count");
    eq_str(r.name, "Ben & Jerry's <Loop> \"A\" AB", "entities in route name");
    eq_int(r.ncue, 3, "entities cue count");
    eq_str(r.cue[0].name, "Start & go", "entity &amp; in a cmt");
    eq_str(r.cue[1].name, "Turn right <here> onto R&D Road",
           "entities &lt; &gt; &#38; in a cmt");
    /* &#x2014; is an em dash: numeric references become UTF-8, so a name
     * still round-trips as bytes even when the 5x7 font cannot draw it. */
    eq_str(r.cue[2].name, "Arrive at \"The End\" \xe2\x80\x94 done",
           "entities &quot; and &#x2014; in a cmt");
    route_free(&r);
}

/* DESIGN.md 7.1: decimate on load to the 20 000 cap. */
static void
test_oversize(void)
{
    route_t r;
    char p[256], err[256];
    double raw_lat_last, raw_lon_last;
    path_of(p, sizeof p, "oversize.gpx");
    if (gpx_load(p, &r, err, sizeof err)) {
        printf("FAIL oversize: %s (run: make beepy-nav/tests/gpx/oversize.gpx)\n",
               err);
        failures++;
        return;
    }
    eq_int(r.npt, ROUTE_MAXPT, "oversize decimated to the cap");
    eq_int(r.decimated, 25000, "oversize raw count recorded");
    /* Both ends survive exactly: a finish that moved would corrupt TO GO. */
    raw_lat_last = 13.7375 + 8000.0 / 110540.0;
    close_to(r.pt[0].lat, 13.7375, 1e-6, "oversize keeps the first point");
    close_to(r.pt[ROUTE_MAXPT - 1].lat, raw_lat_last, 1e-5,
             "oversize keeps the last point");
    raw_lon_last = 100.561 + 8000.0 / (111320.0 * cos(13.7375 * M_PI / 180.0));
    close_to(r.pt[ROUTE_MAXPT - 1].lon, raw_lon_last, 1e-5,
             "oversize keeps the last longitude");
    /* A quarter circle of radius 8000 m is pi/2 * 8000 = 12566 m; dropping
     * one point in five off a 2 m sampling must not shorten it measurably. */
    check(route_prepare(&r) == 0, "oversize prepare");
    close_to(r.total_m, M_PI / 2.0 * 8000.0, 5.0, "oversize length preserved");
    route_free(&r);
}

/* ---------------------------------------------------------------- broken */

/* Every one of these must return -1, leave no route behind, and say which
 * line is at fault. */
static void
bad(const char *file, const char *expect_substr)
{
    route_t r;
    char p[256], err[256];
    int rc;
    path_of(p, sizeof p, file);
    err[0] = '\0';
    rc = gpx_load(p, &r, err, sizeof err);
    if (rc == 0) {
        printf("FAIL %s: loaded %d points, expected an error\n", file, r.npt);
        failures++;
        route_free(&r);
        return;
    }
    eq_int(r.npt, 0, "malformed file yields no points");
    eq_int(r.pt != NULL, 0, "malformed file yields no allocation");
    if (!strstr(err, "line ")) {
        printf("FAIL %s: message does not name a line: \"%s\"\n", file, err);
        failures++;
    }
    if (expect_substr && !strstr(err, expect_substr)) {
        printf("FAIL %s: message \"%s\" lacks \"%s\"\n", file, err,
               expect_substr);
        failures++;
    }
    printf("  %-20s -> %s\n", file, err);
    route_free(&r);
}

/* -------------------------------------------------------------- sym table */

static void
test_sym_kind(void)
{
    eq_int(gpx_sym_kind("Left"), CUE_LEFT, "sym Left");
    eq_int(gpx_sym_kind("turn left"), CUE_LEFT, "sym turn left");
    eq_int(gpx_sym_kind("Slight Right"), CUE_SLIGHT_RIGHT, "sym Slight Right");
    eq_int(gpx_sym_kind("slight-right"), CUE_SLIGHT_RIGHT, "sym slight-right");
    eq_int(gpx_sym_kind("SHARP_LEFT"), CUE_SHARP_LEFT, "sym SHARP_LEFT");
    eq_int(gpx_sym_kind("uturn"), CUE_UTURN, "sym uturn");
    eq_int(gpx_sym_kind("U-Turn"), CUE_UTURN, "sym U-Turn");
    eq_int(gpx_sym_kind("Destination"), CUE_DEST, "sym Destination");
    eq_int(gpx_sym_kind("Summit"), -1, "sym Summit is not a manoeuvre");
    eq_int(gpx_sym_kind(""), -1, "empty sym");
}

int
main(int argc, char **argv)
{
    if (argc > 1)
        DIR = argv[1];
    test_sym_kind();
    test_komoot();
    test_rwgps();
    test_bare();
    test_attrs_reversed();
    test_entities();
    test_oversize();
    bad("truncated.gpx", "unterminated");
    bad("missing-lon.gpx", "no lon attribute");
    bad("nonnumeric-lat.gpx", "not a number");
    bad("empty.gpx", "no <trkpt> or <rtept>");
    printf("test_gpx: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

/* beepy-nav/tests/test_netroute.c -- T-NETROUTE and T-NETROUTE-BAD.
 *
 * No network here either, and for a stronger reason than in test_netfetch.c:
 * these are exact numbers. `beepy-nav/tests/net/valhalla-bike.json` is a real
 * FOSSGIS Valhalla bicycle response for the rider's own HOME->WORK, and its
 * correct decode is 177 points from 13.88495,100.37849 to 13.90008,100.38916 --
 * a fact established independently of this C, in the phase 11 plan, before any
 * of it was written. A test that asked a live server would be asserting
 * against whatever OSM looked like this morning.
 *
 * The OSRM fixtures are the same ride transcoded (tools/mknetfix.py says why:
 * there is no OSRM server this project may ask for a bicycle route), so the
 * three of them assert the same geometry through three different readers -- and
 * one of them is encoded polyline6 where OSRM's default is polyline5, which is
 * the mistake that looks like success.
 *
 * T-NETROUTE-BAD is the other half and the half that matters on a bicycle. A
 * fetch fails far more often than it succeeds -- a dropped hotspot, a cafe
 * captive portal, a server having a bad day -- and each case has to leave the
 * rider following the route they were already following, and say something
 * different enough to act on.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netroute.h"

static int failures, checks;

static void
check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static char *
slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *b;

    if (!f) {
        printf("FAIL cannot open %s\n", path);
        failures++;
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (b && n > 0 && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    if (b)
        b[n < 0 ? 0 : n] = '\0';
    fclose(f);
    return b;
}

#define NET "beepy-nav/tests/net/"

/* The nine cues the geometry of this ride produces, at the vertices the
 * ROUTER named. Positions from the router, kinds from geometry (DESIGN.md
 * 7.9), and the numbers below are what that rule means in practice:
 *
 *   - the departure at vertex 0 is NOT here. Every router reports one and it
 *     is not a turn; there is no 25 m of route behind it to take a bearing
 *     over, so route_turn_at() returns 0 and the straight cue is dropped.
 *   - the roundabout at vertices 20..26 is not here either, and that is the
 *     collapse working rather than failing: enter and exit are one cue,
 *     classified across the whole circle, and the first exit of a roundabout
 *     comes out 15 degrees off the way you went in. The rider carries
 *     straight on, so there is nothing to tell them.
 *   - vertex 45 is `Turn right onto นบ.1009` and its name is EMPTY. The 5x7
 *     font has no glyph for U+0E19 (1.4.2), so it is dropped exactly as the
 *     pack drops it, and the panel shows an arrow with no name line rather
 *     than four blanks.
 */
static const struct {
    int idx, kind;
    double theta;
} WANT[] = {
    { 8, CUE_LEFT, -56.8 },      { 15, CUE_RIGHT, 68.3 },
    { 45, CUE_RIGHT, 91.2 },     { 133, CUE_LEFT, -94.6 },
    { 153, CUE_SLIGHT_LEFT, -49.7 }, { 156, CUE_RIGHT, 60.9 },
    { 164, CUE_LEFT, -76.4 },    { 166, CUE_RIGHT, 88.9 },
    { 176, CUE_DEST, 0.0 }
};
#define NWANT ((int)(sizeof WANT / sizeof WANT[0]))

/* polyline5's grid is 1.1 m, so a transcoded fixture moves every vertex by up
 * to half of that and the bearings over a 25 m arm move with it. Measured
 * worst case across the nine: 2.1 degrees, at vertex 156. Five is slack
 * enough not to be brittle and tight enough that a WRONG classifier could not
 * hide inside it -- and the kinds themselves are compared exactly, which is
 * the assertion that actually matters. */
#define THETA_SLACK 5.0

static void
one_good(const char *path, int type, int want_pts, double want_m,
         double slack_m)
{
    char *body = slurp(path), why[NETROUTE_WHY];
    route_t r;
    int i;

    if (!body)
        return;
    memset(&r, 0, sizeof r);
    why[0] = '\0';
    if (netroute_parse(body, type, "WORK", &r, why, sizeof why) != 0) {
        printf("FAIL %s did not parse: %s\n", path, why);
        failures++;
        free(body);
        return;
    }
    checks++;
    printf("    %-22s %d points  %.0f m  %d cues  \"%s\"\n",
           path + strlen(NET), r.npt, r.total_m, r.ncue, r.name);
    check(r.npt == want_pts, "point count");
    check(fabs(r.total_m - want_m) < slack_m, "route length");
    check(fabs(r.pt[0].lat - 13.88495) < 1e-5 &&
          fabs(r.pt[0].lon - 100.37849) < 1e-5,
          "the route starts where the capture starts");
    check(fabs(r.pt[r.npt - 1].lat - 13.90008) < 1e-5 &&
          fabs(r.pt[r.npt - 1].lon - 100.38916) < 1e-5,
          "and ends where it ends");
    check(!strcmp(r.name, "WORK"), "the caller's name is the route's name");
    check(r.ncue == NWANT, "cue count");
    if (r.ncue == NWANT) {
        for (i = 0; i < NWANT; i++) {
            char what[64];
            snprintf(what, sizeof what, "cue %d sits on vertex %d", i,
                     WANT[i].idx);
            check(r.cue[i].idx == WANT[i].idx, what);
            snprintf(what, sizeof what, "cue %d is kind %d", i, WANT[i].kind);
            check(r.cue[i].kind == WANT[i].kind, what);
            snprintf(what, sizeof what, "cue %d turns %+.0f deg", i,
                     WANT[i].theta);
            check(fabs(r.cue[i].theta_deg - WANT[i].theta) < THETA_SLACK,
                  what);
            snprintf(what, sizeof what, "cue %d along_m matches cum[]", i);
            check(fabs(r.cue[i].along_m - r.cum[WANT[i].idx]) < 0.01, what);
            /* Ascending, because everything downstream walks the list once:
             * route_cue_ahead() takes the first cue past nv->along. */
            if (i)
                check(r.cue[i].along_m > r.cue[i - 1].along_m,
                      "cues are in route order");
        }
        check(r.cue[NWANT - 1].kind == CUE_DEST &&
              fabs(r.cue[NWANT - 1].along_m - r.total_m) < 0.01,
              "the last cue is the destination, at the end");
        check(r.cue[2].name[0] == '\0',
              "a Thai street name is dropped, not drawn as bytes");
    }
    /* The claim of 1.4 that nothing downstream can tell where a route came
     * from: a fetched one arrives PREPARED, with cum[], en[] and a bbox, just
     * as route_load() and router_to() leave one. */
    check(r.prepared && r.cum && r.en, "the route arrives prepared");
    check(r.cum[0] == 0.0 && r.cum[r.npt - 1] == r.total_m,
          "cum[] runs from 0 to total_m");
    check(r.max_e > r.min_e && r.max_n > r.min_n, "and the bbox is real");
    route_free(&r);
    free(body);
}

/* Every one of these has to leave the route the rider is following ALONE.
 * Not zeroed, not reinitialised -- that is netroute_parse()'s contract and it
 * is checked here against a live route_t, not against a fresh one, because a
 * function that only clobbers output it was given for real is a function whose
 * tests pass. */
static void
one_bad(route_t *live, const char *path, int type, const char *expect)
{
    char *body = slurp(path), why[NETROUTE_WHY];
    int rc, before_npt = live->npt, before_ncue = live->ncue;
    double before_m = live->total_m;
    pt_t *before_pt = live->pt;

    if (!body)
        return;
    why[0] = '\0';
    rc = netroute_parse(body, type, "WORK", live, why, sizeof why);
    printf("    %-22s %s\n", path + strlen(NET), why);
    check(rc == -1, path);
    check(live->npt == before_npt && live->ncue == before_ncue &&
          live->pt == before_pt && live->total_m == before_m,
          "the route in flight is untouched");
    check(why[0] != '\0', "and the refusal says something");
    check(strstr(why, expect) != NULL, expect);
    free(body);
}

int
main(void)
{
    route_t live;
    char why[NETROUTE_WHY];
    pt_t *pt = NULL;
    int n;

    check(netroute_type("valhalla") == NETROUTE_VALHALLA &&
          netroute_type("osrm") == NETROUTE_OSRM &&
          netroute_type("brouter") == -1 && netroute_type(NULL) == -1,
          "router_type maps to an adapter, and only those two");

    /* --------------------------------------------------- the decoder alone */

    /* The two-point example from Google's own encoding description, which is
     * the only polyline in this test whose expected output was not produced by
     * this project. At 1e5: (38.5, -120.2) and (40.7, -120.95). */
    n = netroute_polyline("_p~iF~ps|U_ulLnnqC_mqNvxq`@", 5, &pt, why,
                          sizeof why);
    check(n == 3, "the reference polyline decodes to three points");
    if (n == 3) {
        check(fabs(pt[0].lat - 38.5) < 1e-6 && fabs(pt[0].lon + 120.2) < 1e-6,
              "first point");
        check(fabs(pt[2].lat - 43.252) < 1e-6 &&
              fabs(pt[2].lon + 126.453) < 1e-6, "last point");
    }
    free(pt);
    pt = NULL;

    /* Read at 1e6 the same string is a tenth the size, in the right shape,
     * off the coast of Somalia -- plausible on a map, which is why
     * netroute_parse() checks the length rather than trusting anybody. */
    n = netroute_polyline("_p~iF~ps|U_ulLnnqC_mqNvxq`@", 6, &pt, why,
                          sizeof why);
    check(n == 3 && fabs(pt[0].lat - 3.85) < 1e-6,
          "and at the wrong precision decodes to a tenth of it");
    free(pt);
    pt = NULL;

    check(netroute_polyline("", 6, &pt, why, sizeof why) == -1 && !pt,
          "an empty shape is refused");
    check(netroute_polyline("_p~iF~ps|", 6, &pt, why, sizeof why) == -1 && !pt,
          "and a shape ending mid-number");
    check(netroute_polyline("_p~iF", 6, &pt, why, sizeof why) == -1 && !pt,
          "and one with a latitude and no longitude");
    check(netroute_polyline("_p~iF!~ps|U", 6, &pt, why, sizeof why) == -1 &&
          !pt, "and one containing a byte outside the alphabet");
    /* Seven continuation groups, 35 bits. Six is all an honest coordinate
     * needs, and `long` is 32 bits on the device: this is the input that would
     * be signed overflow rather than a wrong answer. */
    check(netroute_polyline("_______?_______?", 6, &pt, why, sizeof why) == -1 &&
          !pt && strstr(why, "too big") != NULL,
          "and one whose number is too big to be a coordinate");

    /* ------------------------------------------------------- T-NETROUTE */

    printf("--- T-NETROUTE: three readings of one ride\n");
    one_good(NET "valhalla-bike.json", NETROUTE_VALHALLA, 177, 4953.3, 1.0);
    /* polyline5 rounds every vertex onto a 1.1 m grid, so the length moves.
     * 5 m on 4 953 is a tenth of a percent; a precision mix-up is a factor of
     * ten, and that is the distance this tolerance has to stay clear of. */
    one_good(NET "osrm-bike.json", NETROUTE_OSRM, 177, 4953.3, 5.0);
    /* The one that would silently be a tenth of a route: polyline6 from a
     * server whose default is polyline5 and which does not say which it sent.
     * It parses to the SAME numbers as the others, which is the whole claim. */
    one_good(NET "osrm-poly6.json", NETROUTE_OSRM, 177, 4953.3, 1.0);

    /* --------------------------------------------------- T-NETROUTE-BAD */

    printf("--- T-NETROUTE-BAD: a route in flight, and nine bad replies\n");
    memset(&live, 0, sizeof live);
    {
        char *body = slurp(NET "valhalla-bike.json");
        if (!body || netroute_parse(body, NETROUTE_VALHALLA, "WORK", &live, why,
                                    sizeof why) != 0) {
            printf("FAIL cannot set up the in-flight route: %s\n", why);
            free(body);
            return 1;
        }
        free(body);
    }

    one_bad(&live, NET "bad-empty.json", NETROUTE_VALHALLA, "empty");
    one_bad(&live, NET "bad-captive.html", NETROUTE_VALHALLA, "web page");
    one_bad(&live, NET "bad-truncated.json", NETROUTE_VALHALLA, "truncated");
    one_bad(&live, NET "bad-malformed.json", NETROUTE_VALHALLA, "malformed");
    /* Its own words, not ours: "no path could be found" tells a rider to pick
     * somewhere else; "the reply has no route in it" tells them the program is
     * broken, which would be a lie. */
    one_bad(&live, NET "bad-noroute.json", NETROUTE_VALHALLA, "No path");
    one_bad(&live, NET "bad-shape.json", NETROUTE_VALHALLA, "polyline");
    one_bad(&live, NET "bad-tooshort.json", NETROUTE_VALHALLA, "only 0 m");
    /* A well-formed Valhalla reply read by the OSRM adapter, which is what a
     * wrong `router_type` in the config file looks like from here. */
    one_bad(&live, NET "valhalla-bike.json", NETROUTE_OSRM, "no route in it");
    /* And the reverse. The Valhalla adapter finds no `trip`, and OSRM's own
     * `code` is the nearest thing to a reason in the body. */
    one_bad(&live, NET "osrm-bike.json", NETROUTE_VALHALLA, "Ok");

    /* Not from a file: a body with a shape whose length contradicts the
     * distance beside it, which is the precision defence firing on a response
     * neither retry can rescue. */
    {
        const char *bogus =
            "{\"trip\":{\"units\":\"kilometers\",\"summary\":{\"length\":40.0},"
            "\"legs\":[{\"shape\":\"o`nnYovrm~DGhB]tJwFd`A}@hYY`MTjr@\","
            "\"maneuvers\":[]}]}}";
        why[0] = '\0';
        check(netroute_parse(bogus, NETROUTE_VALHALLA, NULL, &live, why,
                             sizeof why) == -1 &&
              strstr(why, "but the reply says") != NULL,
              "a shape that contradicts its own stated distance is refused");
        printf("    %-22s %s\n", "(length mismatch)", why);
    }

    /* An ASCII street name DOES reach the cue -- the fixture cannot show this
     * because every name in that corner of Nonthaburi is Thai, and a test that
     * only ever saw names dropped would pass just as well if names were never
     * read at all. Hand-built, and small enough to read. */
    {
        /* An L: the capture's own start point, then 60 steps of +0.0001 deg
         * east and 60 of +0.0001 deg north -- 650 m of arm each side of one
         * right-angle corner, which is 26 times what the classifier's 25 m
         * arms need. Encoding a delta of 100 microdegrees is "gE" and a delta
         * of zero is "?", so the shape is those two groups repeated; writing an
         * encoder here would be a second implementation of the thing under
         * test, and the one in tools/mknetfix.py is where that belongs.
         *
         * The corner is at vertex 60: the segment into it runs east and the
         * one out of it runs north, so it is a LEFT turn -- and the maneuver
         * below declares Valhalla type 10, which is `turn right`. The cue comes
         * out CUE_LEFT. That disagreement is the assertion: kinds come from the
         * geometry, and no table maps a router's type onto an arrow (7.9). Get
         * that backwards and this is the test that says so, on a shape whose
         * answer can be read off the encoding by hand. */
        char shape[512], body[1024];
        route_t r;
        size_t w;
        int k;

        snprintf(shape, sizeof shape, "%s", "o`nnYovrm~D");
        w = strlen(shape);
        for (k = 0; k < 60; k++, w += 3)
            memcpy(shape + w, "?gE", 3); /* lat +0, lon +100: due east  */
        for (k = 0; k < 60; k++, w += 3)
            memcpy(shape + w, "gE?", 3); /* lat +100, lon +0: due north */
        shape[w] = '\0';

        snprintf(body, sizeof body,
                 "{\"trip\":{\"units\":\"kilometers\",\"summary\":"
                 "{\"length\":1.311},\"legs\":[{\"shape\":\"%s\","
                 "\"maneuvers\":[{\"type\":2,\"begin_shape_index\":0},"
                 "{\"type\":10,\"begin_shape_index\":60,"
                 "\"street_names\":[\"MAIN ST\"]},"
                 "{\"type\":5,\"begin_shape_index\":120}]}]}}",
                 shape);
        memset(&r, 0, sizeof r);
        why[0] = '\0';
        if (netroute_parse(body, NETROUTE_VALHALLA, NULL, &r, why,
                           sizeof why) != 0) {
            printf("FAIL the synthetic right turn did not parse: %s\n", why);
            failures++;
        } else {
            printf("    %-22s %d points  %.0f m  %d cues\n", "(synthetic L)",
                   r.npt, r.total_m, r.ncue);
            check(r.npt == 121, "the synthetic route has 121 points");
            check(r.ncue == 2, "one turn and one destination");
            if (r.ncue == 2) {
                check(r.cue[0].idx == 60, "the cue sits on the corner");
                check(r.cue[0].kind == CUE_LEFT,
                      "the geometry says left where the router said type 10");
                check(fabs(r.cue[0].theta_deg + 90.0) < 1.0,
                      "ninety degrees of it, measured and not looked up");
                check(!strcmp(r.cue[0].name, "MAIN ST"),
                      "carrying the ASCII street name the router gave it");
                check(r.cue[1].kind == CUE_DEST, "then the destination");
            }
            check(!strcmp(r.name, "DESTINATION"),
                  "a NULL name gives DESTINATION, as router_to() does");
            route_free(&r);
        }
    }

    /* The roundabout collapse, on a ring the captured ride cannot test.
     *
     * Its roundabout comes out 15 degrees off the way it went in, so the
     * collapsed cue classifies straight and is dropped -- correct, and it
     * proves nothing about WHERE the bearing out was taken. This one is built
     * so the two answers differ by 180 degrees of meaning: 330 m north, then a
     * ring entered by turning LEFT (Thailand drives on the left, so its
     * roundabouts run clockwise), leaving eastward, then 330 m east.
     *
     *     collapsed across the ring, vertices 30..42:  +90  CUE_RIGHT
     *     classified at the entry alone, vertex 30:    -90  CUE_LEFT
     *
     * A rider told LEFT here would ride into the ring and stay in it. */
    {
        char shape[512], body[1024];
        route_t r;
        size_t w;
        int k;

        snprintf(shape, sizeof shape, "%s", "o`nnYovrm~D");
        w = strlen(shape);
        for (k = 0; k < 30; k++, w += 3)
            memcpy(shape + w, "gE?", 3); /* north, up to the entry at 30 */
        for (k = 0; k < 3; k++, w += 3)
            memcpy(shape + w, "?fE", 3); /* west: into the ring, turning left */
        for (k = 0; k < 3; k++, w += 3)
            memcpy(shape + w, "gE?", 3); /* north, round it */
        for (k = 0; k < 36; k++, w += 3)
            memcpy(shape + w, "?gE", 3); /* east: out at 42, and away */
        shape[w] = '\0';

        snprintf(body, sizeof body,
                 "{\"trip\":{\"units\":\"kilometers\",\"summary\":"
                 "{\"length\":0.786},\"legs\":[{\"shape\":\"%s\","
                 "\"maneuvers\":[{\"type\":2,\"begin_shape_index\":0},"
                 "{\"type\":26,\"begin_shape_index\":30},"
                 "{\"type\":27,\"begin_shape_index\":42},"
                 "{\"type\":5,\"begin_shape_index\":72}]}]}}",
                 shape);
        memset(&r, 0, sizeof r);
        why[0] = '\0';
        if (netroute_parse(body, NETROUTE_VALHALLA, NULL, &r, why,
                           sizeof why) != 0) {
            printf("FAIL the synthetic roundabout did not parse: %s\n", why);
            failures++;
        } else {
            printf("    %-22s %d points  %.0f m  %d cues\n", "(synthetic ring)",
                   r.npt, r.total_m, r.ncue);
            check(r.npt == 73, "the ring route has 73 points");
            check(r.ncue == 2, "enter and exit are ONE cue, plus the "
                               "destination");
            if (r.ncue == 2) {
                check(r.cue[0].idx == 30,
                      "and it sits at the entry, where the rider must act");
                check(r.cue[0].kind == CUE_RIGHT,
                      "classified across the ring: right, where they come out");
                check(fabs(r.cue[0].theta_deg - 90.0) < 1.0,
                      "+90 and not the -90 the entry alone would have given");
                check(r.cue[1].kind == CUE_DEST, "then the destination");
            }
            route_free(&r);
        }
    }

    /* Miles, because Valhalla answers in whatever units it was asked for and
     * says which -- and reading 3.1 as kilometres would fail the length check
     * on a route that is perfectly good. */
    {
        char *body = slurp(NET "valhalla-bike.json");
        char *u;
        route_t r;
        if (body && (u = strstr(body, "\"units\":\"kilometers\"")) != NULL) {
            /* Rewritten in place at the same width, so the body stays valid
             * JSON without a second copy: the value is padded with the
             * whitespace JSON allows before a comma. 4.969 km is 3.088 mi. */
            memcpy(u, "\"units\":\"miles\"     ", 20);
            u = strstr(body, "\"length\":4.969");
            while (u) {
                memcpy(u, "\"length\":3.088", 14);
                u = strstr(u + 1, "\"length\":4.969");
            }
            memset(&r, 0, sizeof r);
            why[0] = '\0';
            check(netroute_parse(body, NETROUTE_VALHALLA, NULL, &r, why,
                                 sizeof why) == 0,
                  "a reply in miles parses");
            if (r.npt) {
                check(r.npt == 177 && fabs(r.total_m - 4953.3) < 1.0,
                      "to the same route, because units are only the check");
                route_free(&r);
            } else {
                printf("    (miles) %s\n", why);
            }
        }
        free(body);
    }

    route_free(&live);

    if (failures) {
        printf("test_netroute: %d FAILURES of %d\n", failures, checks);
        return 1;
    }
    printf("test_netroute: OK  %d assertions\n", checks);
    return 0;
}

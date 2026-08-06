/* beepy-nav/src/netroute.c -- see netroute.h. DESIGN.md 7.9. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netroute.h"

static void
say(char *why, int nwhy, const char *fmt, ...)
{
    va_list ap;

    if (!why || nwhy <= 0)
        return;
    va_start(ap, fmt);
    vsnprintf(why, (size_t)nwhy, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------- JSON
 *
 * A SCANNER, NOT A PARSER: no tree, no allocation, no ownership. Every
 * function here takes a pointer INTO the response and returns a pointer into
 * it, and the response is the only copy of anything. That is not
 * miniaturisation for its own sake -- the bodies are up to a megabyte
 * (netfetch.h's cap) on a device with 512 MB and a framebuffer in it, and a
 * parsed tree of a 20 000-point route would cost more than the route does.
 *
 * The cost is that a value is only validated when it is READ, which for a
 * truncated body means damage past the last field we happen to want would go
 * unnoticed. So netroute_parse() walks the whole document once first and
 * refuses it entire if any of it is malformed -- one linear pass, before any
 * of it is believed. A response is either well-formed JSON or it is a captive
 * portal, and telling those two apart is most of this module's job.
 */

/* `[[[[[...` in a megabyte of body is a million stack frames. The deepest
 * thing either router nests is about six. */
#define JDEPTH 32

static const char *
jws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* One past the complete JSON value at `p`, or NULL if there is not one. */
static const char *
jend(const char *p, int depth)
{
    p = jws(p);
    if (depth > JDEPTH)
        return NULL;
    if (*p == '"') {
        for (p++; *p != '"'; p++) {
            if (!*p)
                return NULL;
            if (*p == '\\') {
                if (!p[1])
                    return NULL;
                p++;
            }
        }
        return p + 1;
    }
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        int obj = *p == '{';
        p = jws(p + 1);
        if (*p == close)
            return p + 1;
        for (;;) {
            if (obj) {
                p = jws(p);
                if (*p != '"')
                    return NULL;
                p = jend(p, depth + 1); /* the key */
                if (!p)
                    return NULL;
                p = jws(p);
                if (*p != ':')
                    return NULL;
                p++;
            }
            p = jend(p, depth + 1);
            if (!p)
                return NULL;
            p = jws(p);
            if (*p == ',') {
                p++;
                continue;
            }
            return *p == close ? p + 1 : NULL;
        }
    }
    if (!strncmp(p, "true", 4))
        return p + 4;
    if (!strncmp(p, "false", 5))
        return p + 5;
    if (!strncmp(p, "null", 4))
        return p + 4;
    if (*p == '-' || (*p >= '0' && *p <= '9')) {
        char *e;
        strtod(p, &e);
        return e == p ? NULL : e;
    }
    return NULL;
}

/* The value of `key` in the object at `obj`, or NULL. Keys are compared as
 * literal bytes: neither router escapes anything in a key, and a scanner that
 * unescaped them would need a buffer to do it in. */
static const char *
jget(const char *obj, const char *key)
{
    size_t n = strlen(key);
    const char *p;

    if (!obj)
        return NULL;
    p = jws(obj);
    if (*p != '{')
        return NULL;
    p = jws(p + 1);
    if (*p == '}')
        return NULL;
    for (;;) {
        const char *k = p, *v;
        if (*p != '"')
            return NULL;
        p = jend(p, 0);
        if (!p)
            return NULL;
        v = jws(p);
        if (*v != ':')
            return NULL;
        v = jws(v + 1);
        if ((size_t)(p - k) == n + 2 && !memcmp(k + 1, key, n))
            return v;
        p = jend(v, 0);
        if (!p)
            return NULL;
        p = jws(p);
        if (*p != ',')
            return NULL;
        p = jws(p + 1);
    }
}

/* First element of the array at `arr`; NULL when empty or not an array. */
static const char *
jfirst(const char *arr)
{
    const char *p;

    if (!arr)
        return NULL;
    p = jws(arr);
    if (*p != '[')
        return NULL;
    p = jws(p + 1);
    return *p == ']' ? NULL : p;
}

/* The element after `v`, or NULL at the end of the array. Iteration rather
 * than an index: jget()/jend() are linear, so element-at-i in a loop is
 * quadratic, and `steps` in a city route is not short. */
static const char *
jnext(const char *v)
{
    const char *p = jend(v, 0);

    if (!p)
        return NULL;
    p = jws(p);
    return *p == ',' ? jws(p + 1) : NULL;
}

/* A JSON `true` or `false`. Returns 0 and sets *b, or -1 for anything else --
 * including a missing key and a `null`, because "the server did not say" is a
 * third answer and not a quiet false. The caller turns the -1 into
 * ROUTE_TOLL_UNKNOWN rather than into NO. */
static int
jbool(const char *v, int *b)
{
    if (!v)
        return -1;
    v = jws(v);
    if (!strncmp(v, "true", 4)) {
        *b = 1;
        return 0;
    }
    if (!strncmp(v, "false", 5)) {
        *b = 0;
        return 0;
    }
    return -1;
}

static int
jnum(const char *v, double *out)
{
    char *e;
    double d;

    if (!v)
        return -1;
    v = jws(v);
    if (*v != '-' && !(*v >= '0' && *v <= '9'))
        return -1;
    d = strtod(v, &e);
    if (e == v)
        return -1;
    *out = d;
    return 0;
}

/* Copy the string at `v` into `out`, unescaped. Returns 0, or -1 when `v` is
 * not a string.
 *
 * A \uXXXX above 007F sets *wide and is NOT decoded, which is the whole
 * treatment non-ASCII gets here: the 5x7 font is A-Z/0-9 (1.4.2), so the
 * caller drops the name rather than showing mojibake, and a UTF-8 encoder
 * would exist only to produce bytes nothing can draw. Raw high bytes -- how
 * the captured fixture actually carries its Thai -- set it too. */
static int
jstr(const char *v, char *out, size_t n, int *wide)
{
    size_t w = 0;

    *wide = 0;
    if (!v)
        return -1;
    v = jws(v);
    if (*v != '"')
        return -1;
    for (v++; *v && *v != '"'; v++) {
        char c;
        if ((unsigned char)*v >= 0x80) {
            *wide = 1;
            continue;
        }
        if (*v != '\\') {
            c = *v;
        } else {
            switch (*++v) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                unsigned long u = 0;
                int i;
                for (i = 1; i <= 4; i++) {
                    char h = v[i];
                    if (h >= '0' && h <= '9')
                        u = u * 16 + (unsigned long)(h - '0');
                    else if (h >= 'a' && h <= 'f')
                        u = u * 16 + (unsigned long)(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F')
                        u = u * 16 + (unsigned long)(h - 'A' + 10);
                    else
                        return -1;
                }
                v += 4;
                if (u >= 0x80) {
                    *wide = 1;
                    continue;
                }
                c = (char)u;
                break;
            }
            default: c = *v; break;
            }
        }
        if (w + 1 < n)
            out[w++] = c;
    }
    if (w < n)
        out[w] = '\0';
    else if (n)
        out[n - 1] = '\0';
    return 0;
}

/* The string at `v`, unescaped, on the heap; NULL if it is not a string or
 * there is no memory. The caller frees.
 *
 * THE SHAPE CANNOT BE DECODED IN PLACE, and the first version of this file
 * tried to. A polyline character is `chunk + 63`, so its alphabet is 63..126 --
 * which contains 92, the backslash, which JSON must escape. All three committed
 * fixtures carry one. Read in place, a shape is a string with `\\` in the middle
 * and no terminator, and the decoder walks off the end of it into the rest of
 * the response. */
static char *
jstrdup(const char *v)
{
    const char *e;
    char *out;
    int wide;

    if (!v)
        return NULL;
    v = jws(v);
    e = jend(v, 0);
    if (!e || *v != '"')
        return NULL;
    out = malloc((size_t)(e - v)); /* the quotes pay for the NUL */
    if (!out)
        return NULL;
    if (jstr(v, out, (size_t)(e - v), &wide) != 0) {
        free(out);
        return NULL;
    }
    return out;
}

/* --------------------------------------------------------------- polyline */

int
netroute_polyline(const char *s, int precision, pt_t **out, char *why,
                  int nwhy)
{
    double factor = precision == 6 ? 1e6 : 1e5;
    long nterm = 0, i, npt;
    long lat = 0, lon = 0;
    const char *p;
    pt_t *pt;

    *out = NULL;
    if (!s || !*s) {
        say(why, nwhy, "the reply carries no shape at all");
        return -1;
    }
    /* Precount by terminators -- every varint ends in a byte below 0x20 after
     * the -63, two varints to a point -- so the decode allocates once instead
     * of growing, and an odd count is caught before any of it is believed. */
    for (p = s; *p; p++) {
        int c = (unsigned char)*p - 63;
        if (c < 0 || c > 63) {
            say(why, nwhy, "shape is not an encoded polyline");
            return -1;
        }
        if (c < 0x20)
            nterm++;
    }
    if (nterm == 0 || (nterm & 1)) {
        say(why, nwhy, "shape ends mid-number");
        return -1;
    }
    npt = nterm / 2;
    if (npt > ROUTE_MAXPT) {
        say(why, nwhy, "route has %ld points, more than %d", npt,
            ROUTE_MAXPT);
        return -1;
    }
    pt = calloc((size_t)npt, sizeof *pt);
    if (!pt) {
        say(why, nwhy, "out of memory for %ld points", npt);
        return -1;
    }

    p = s;
    for (i = 0; i < npt; i++) {
        int axis;
        for (axis = 0; axis < 2; axis++) {
            unsigned long acc = 0;
            int shift = 0, c;
            do {
                c = (unsigned char)*p++ - 63;
                /* Six groups, 30 bits, and not one more -- a coordinate at 1e6
                 * needs 28 (180 000 000 < 2^28), so six is exactly enough for
                 * an honest one. The bound is not tidiness: `long` is 32 bits
                 * on this device, and with deltas capped at 2^29 and every
                 * point checked against +/-90 degrees below, the running total
                 * cannot reach 2^31. Signed overflow is undefined behaviour,
                 * and the input is a stranger's server. */
                if (shift > 25) {
                    free(pt);
                    say(why, nwhy, "shape has a number too big to be a "
                                   "coordinate");
                    return -1;
                }
                acc |= (unsigned long)(c & 0x1f) << shift;
                shift += 5;
            } while (c >= 0x20);
            {
                long d = (acc & 1) ? ~(long)(acc >> 1) : (long)(acc >> 1);
                if (axis == 0)
                    lat += d;
                else
                    lon += d;
            }
        }
        pt[i].lat = (double)lat / factor;
        pt[i].lon = (double)lon / factor;
        if (pt[i].lat < -90.0 || pt[i].lat > 90.0 || pt[i].lon < -180.0 ||
            pt[i].lon > 180.0) {
            /* The cheap half of the precision defence, and the only half that
             * works on a route near the equator: 1e6 read as 1e5 puts Bangkok
             * at latitude 138. The other half is the length check in
             * netroute_parse(), which catches the cases this cannot. */
            say(why, nwhy, "point %ld is at %.4f,%.4f -- not on Earth", i,
                pt[i].lat, pt[i].lon);
            free(pt);
            return -1;
        }
    }
    *out = pt;
    return (int)npt;
}

/* ---------------------------------------------------------------- geometry
 *
 * One decoded geometry, plus where each leg begins in it. Valhalla numbers its
 * maneuvers per leg, so a two-leg trip has two maneuvers at shape index 0 and
 * only the base tells them apart. */

typedef struct {
    pt_t *pt;
    int npt;
    int base[NETROUTE_MAXLEG];
    int nleg;
} shape_t;

static void
shape_free(shape_t *s)
{
    free(s->pt);
    memset(s, 0, sizeof *s);
}

/* Append one encoded leg. The joint between two legs is the same point twice
 * -- the end of one and the start of the next -- and a zero-length segment
 * there would be a division by nothing in route_prepare()'s bearings. */
static int
shape_add(shape_t *s, const char *enc, int prec, char *why, int nwhy)
{
    pt_t *lp = NULL, *grown;
    int n, skip = 0;

    if (s->nleg >= NETROUTE_MAXLEG) {
        say(why, nwhy, "more than %d legs", NETROUTE_MAXLEG);
        return -1;
    }
    n = netroute_polyline(enc, prec, &lp, why, nwhy);
    if (n < 0)
        return -1;
    if (s->npt > 0 && fabs(lp[0].lat - s->pt[s->npt - 1].lat) < 1e-9 &&
        fabs(lp[0].lon - s->pt[s->npt - 1].lon) < 1e-9)
        skip = 1;
    if (s->npt + n - skip > ROUTE_MAXPT) {
        say(why, nwhy, "route has more than %d points", ROUTE_MAXPT);
        free(lp);
        return -1;
    }
    grown = realloc(s->pt, (size_t)(s->npt + n - skip) * sizeof *grown);
    if (!grown) {
        say(why, nwhy, "out of memory joining legs");
        free(lp);
        return -1;
    }
    s->pt = grown;
    s->base[s->nleg++] = skip ? s->npt - 1 : s->npt;
    memcpy(s->pt + s->npt, lp + skip, (size_t)(n - skip) * sizeof *lp);
    s->npt += n - skip;
    free(lp);
    return 0;
}

/* §6.1's tangent-plane length, which is what route_prepare() will compute and
 * therefore the number to compare against what the response claims. */
static double
shape_len(const shape_t *s)
{
    double total = 0.0, pe = 0.0, pn = 0.0;
    int i;

    for (i = 0; i < s->npt; i++) {
        double e, n;
        geo_project(s->pt[0].lat, s->pt[0].lon, s->pt[i].lat, s->pt[i].lon, &e,
                    &n);
        if (i)
            total += hypot(e - pe, n - pn);
        pe = e;
        pn = n;
    }
    return total;
}

/* ---------------------------------------------------------------- adapters
 *
 * Each fills a shape_t at a given precision and reports the distance the
 * response CLAIMS, in metres. Called twice at most, once per precision, by
 * decode_shape() below.
 */

static int
vh_shape(const char *trip, int prec, shape_t *s, char *why, int nwhy)
{
    const char *leg;

    for (leg = jfirst(jget(trip, "legs")); leg; leg = jnext(leg)) {
        char *enc = jstrdup(jget(leg, "shape"));
        int rc;
        if (!enc) {
            say(why, nwhy, "a leg has no shape");
            return -1;
        }
        rc = shape_add(s, enc, prec, why, nwhy);
        free(enc);
        if (rc)
            return -1;
    }
    if (s->nleg == 0) {
        say(why, nwhy, "the reply has no legs");
        return -1;
    }
    return 0;
}

static int
osrm_shape(const char *route, int prec, shape_t *s, char *why, int nwhy)
{
    const char *g = jget(route, "geometry");
    char *enc;
    int rc;

    if (!g || jws(g)[0] != '"') {
        /* `geometries=geojson` is a config the parser cannot read, and saying
         * so beats "shape is not an encoded polyline" about a nested array. */
        say(why, nwhy, g ? "shape is not a polyline (geojson?)"
                         : "the reply has no geometry");
        return -1;
    }
    enc = jstrdup(g);
    if (!enc) {
        say(why, nwhy, "out of memory for the shape");
        return -1;
    }
    rc = shape_add(s, enc, prec, why, nwhy);
    free(enc);
    return rc;
}

/* Decode at the precision the router is expected to use, check the result
 * against the distance the response states, and try the other precision when
 * they disagree by more than a factor of NETROUTE_LEN_TOL. See netroute.h --
 * this is the one failure in the module that would otherwise look like
 * success. */
static int
decode_shape(const char *node, int type, double stated, shape_t *s, int *prec,
             char *why, int nwhy)
{
    int want = type == NETROUTE_VALHALLA ? 6 : 5;
    int attempt;
    char first[NETROUTE_WHY];

    first[0] = '\0';
    for (attempt = 0; attempt < 2; attempt++) {
        double got;
        int p = attempt == 0 ? want : (want == 6 ? 5 : 6);
        char sub[NETROUTE_WHY];

        memset(s, 0, sizeof *s);
        sub[0] = '\0';
        if ((type == NETROUTE_VALHALLA ? vh_shape(node, p, s, sub, sizeof sub)
                                       : osrm_shape(node, p, s, sub,
                                                    sizeof sub)) == 0) {
            got = shape_len(s);
            if (got * NETROUTE_LEN_TOL >= stated &&
                got <= stated * NETROUTE_LEN_TOL) {
                *prec = p;
                return 0;
            }
            snprintf(sub, sizeof sub,
                     "shape is %.0f m but the reply says %.0f m", got, stated);
        }
        shape_free(s);
        if (attempt == 0)
            snprintf(first, sizeof first, "%s", sub);
    }
    /* The reason quoted is the one from the precision the CONFIGURED router
     * uses, not from the retry. The retry is a rescue attempt on a hunch about
     * somebody else's server; when it also fails, what the rider needs to hear
     * is what went wrong with the router they actually named. */
    say(why, nwhy, "%s", first);
    return -1;
}

/* ------------------------------------------------------------------- cues */

/* How many maneuvers, over every leg -- so the cue array is allocated once and
 * exactly, rather than ROUTE_MAXCUE (295 KB) for a route with nine turns. */
static int
count_cues(const char *node, int type)
{
    const char *leg;
    int n = 0;

    if (type == NETROUTE_OSRM) {
        for (leg = jfirst(jget(node, "legs")); leg; leg = jnext(leg)) {
            const char *st;
            for (st = jfirst(jget(leg, "steps")); st; st = jnext(st))
                n++;
        }
        return n;
    }
    for (leg = jfirst(jget(node, "legs")); leg; leg = jnext(leg)) {
        const char *m;
        for (m = jfirst(jget(leg, "maneuvers")); m; m = jnext(m))
            n++;
    }
    return n;
}

/* One cue, from a vertex pair and a name. `in_i`/`out_i` differ only for a
 * collapsed roundabout. Straight cues are dropped exactly as
 * route_cues_derive() drops them -- a maneuver the geometry says is not a turn
 * is a road that changed name, and an arrow for it would be noise on a panel
 * that has room for one instruction. */
static void
add_cue(route_t *r, int in_i, int out_i, int dest, const char *name)
{
    double theta;
    int kind;

    if (in_i < 0 || in_i >= r->npt || out_i < in_i || out_i >= r->npt)
        return;
    if (dest) {
        theta = 0.0;
        kind = CUE_DEST;
    } else {
        theta = route_turn_at(r, r->cum[in_i], r->cum[out_i]);
        kind = route_classify(theta);
        if (kind == CUE_STRAIGHT)
            return;
    }
    memset(&r->cue[r->ncue], 0, sizeof r->cue[r->ncue]);
    r->cue[r->ncue].idx = in_i;
    r->cue[r->ncue].kind = kind;
    r->cue[r->ncue].theta_deg = theta;
    r->cue[r->ncue].along_m = r->cum[in_i];
    if (name)
        snprintf(r->cue[r->ncue].name, CUE_NAME, "%s", name);
    r->ncue++;
}

/* A street name, or "" when it has none this device can draw (1.4.2). */
static void
cue_name(const char *v, char *out, size_t n)
{
    int wide = 0;

    out[0] = '\0';
    if (!v || jstr(v, out, n, &wide) != 0 || wide)
        out[0] = '\0';
}

/* Valhalla maneuver types. Only the structural ones: 1-3 (start) need no entry
 * because route_turn_at() refuses a vertex with no room for an arm behind it,
 * which is what a departure is. */
#define VH_DEST_A 4 /* destination, destination_right, destination_left */
#define VH_DEST_C 6
#define VH_ROUNDABOUT_IN 26
#define VH_ROUNDABOUT_OUT 27

static void
vh_cues(const char *trip, route_t *r, const int *base, int nleg)
{
    const char *leg;
    int li = 0;

    for (leg = jfirst(jget(trip, "legs")); leg && li < nleg;
         leg = jnext(leg), li++) {
        const char *m;
        for (m = jfirst(jget(leg, "maneuvers")); m;) {
            double t = 0.0, bi = 0.0;
            char name[CUE_NAME];
            const char *sn;
            int in_i, out_i;

            if (jnum(jget(m, "type"), &t) != 0 ||
                jnum(jget(m, "begin_shape_index"), &bi) != 0) {
                m = jnext(m);
                continue;
            }
            in_i = out_i = base[li] + (int)bi;
            sn = jfirst(jget(m, "street_names"));
            cue_name(sn, name, sizeof name);

            if ((int)t >= VH_DEST_A && (int)t <= VH_DEST_C) {
                add_cue(r, in_i, out_i, 1, name);
                m = jnext(m);
                continue;
            }
            if ((int)t == VH_ROUNDABOUT_IN) {
                /* Collapse: enter and exit are one cue, classified across the
                 * whole circle, so the rider is told where they come OUT.
                 *
                 * The search for the exit stops at the first maneuver that is
                 * neither enter nor exit -- a roundabout is a CONTIGUOUS run of
                 * them. Scanning to the end of the leg instead would, on a
                 * reply with an enter and no exit, pair the entry with a
                 * roundabout two kilometres later and silently drop every turn
                 * between the two. */
                const char *j = jnext(m);
                while (j) {
                    double jt = 0.0;
                    if (jnum(jget(j, "type"), &jt) != 0)
                        break;
                    if ((int)jt == VH_ROUNDABOUT_OUT)
                        break;
                    if ((int)jt != VH_ROUNDABOUT_IN) {
                        j = NULL;
                        break;
                    }
                    j = jnext(j);
                }
                if (j) {
                    double ji = 0.0;
                    if (jnum(jget(j, "begin_shape_index"), &ji) == 0)
                        out_i = base[li] + (int)ji;
                    m = jnext(j);
                } else {
                    m = jnext(m);
                }
                add_cue(r, in_i, out_i, 0, name);
                continue;
            }
            add_cue(r, in_i, out_i, 0, name);
            m = jnext(m);
        }
    }
}

/* The nearest route vertex to a lat/lon, searching FORWARD from `from`.
 *
 * OSRM steps carry a location and no shape index, so the index has to be
 * found. Forward-only because the steps are in route order and a route that
 * crosses itself -- an out-and-back, which is most of a Sunday ride -- has two
 * vertices equally near a junction, and the second time past it is the one
 * that is meant. */
static int
vertex_near(const route_t *r, int from, double lat, double lon)
{
    double best = 1e30;
    int i, bi = from;

    for (i = from; i < r->npt; i++) {
        double dlat = r->pt[i].lat - lat, dlon = r->pt[i].lon - lon;
        double d = dlat * dlat + dlon * dlon;
        if (d < best) {
            best = d;
            bi = i;
        }
    }
    return bi;
}

static int
osrm_loc(const char *step, double *lat, double *lon)
{
    const char *mv = jget(step, "maneuver"), *e;

    if (!mv)
        return -1;
    /* [lon, lat]. That order is OSRM's, it is the reverse of every other
     * coordinate in this program, and getting it wrong puts a Bangkok route in
     * Somalia. */
    e = jfirst(jget(mv, "location"));
    if (!e || jnum(e, lon) != 0)
        return -1;
    e = jnext(e);
    if (!e || jnum(e, lat) != 0)
        return -1;
    return 0;
}

/* `maneuver.type`, or "" -- the only three spellings this file acts on are
 * arrive and the two halves of a roundabout. `modifier` is never read: it says
 * left or right, and left or right is the geometry's to decide (7.9). */
static void
osrm_type(const char *step, char *out, size_t n)
{
    const char *mv = jget(step, "maneuver");
    int wide;

    out[0] = '\0';
    if (mv)
        jstr(jget(mv, "type"), out, n, &wide);
}

/* OSRM spells a roundabout `roundabout`/`exit roundabout` and a big one
 * `rotary`/`exit rotary`. (`roundabout turn` is one step with no exit, so it
 * falls through to the classifier, which is where it belongs.) */
static int
osrm_ring_in(const char *ty)
{
    return !strcmp(ty, "roundabout") || !strcmp(ty, "rotary");
}

static int
osrm_ring_out(const char *ty)
{
    return !strcmp(ty, "exit roundabout") || !strcmp(ty, "exit rotary");
}

static void
osrm_cues(const char *route, route_t *r)
{
    const char *leg;
    int prev = 0;

    for (leg = jfirst(jget(route, "legs")); leg; leg = jnext(leg)) {
        const char *st;
        for (st = jfirst(jget(leg, "steps")); st;) {
            double lat, lon;
            char name[CUE_NAME], ty[32];
            int in_i, out_i;

            osrm_type(st, ty, sizeof ty);
            if (osrm_loc(st, &lat, &lon) != 0) {
                st = jnext(st);
                continue;
            }
            in_i = out_i = vertex_near(r, prev, lat, lon);
            prev = in_i;
            cue_name(jget(st, "name"), name, sizeof name);

            if (!strcmp(ty, "arrive")) {
                add_cue(r, in_i, out_i, 1, name);
                st = jnext(st);
                continue;
            }
            if (osrm_ring_in(ty)) {
                /* Contiguous, for the reason vh_cues() gives: an enter with no
                 * exit must not pair with the next roundabout down the road. */
                const char *j = jnext(st);
                while (j) {
                    char jt[32];
                    osrm_type(j, jt, sizeof jt);
                    if (osrm_ring_out(jt))
                        break;
                    if (!osrm_ring_in(jt)) {
                        j = NULL;
                        break;
                    }
                    j = jnext(j);
                }
                if (j) {
                    double jlat, jlon;
                    if (osrm_loc(j, &jlat, &jlon) == 0) {
                        out_i = vertex_near(r, in_i, jlat, jlon);
                        prev = out_i;
                    }
                    st = jnext(j);
                } else {
                    st = jnext(st);
                }
                add_cue(r, in_i, out_i, 0, name);
                continue;
            }
            add_cue(r, in_i, out_i, 0, name);
            st = jnext(st);
        }
    }
}

/* -------------------------------------------------------------------- API */

int
netroute_type(const char *name)
{
    if (!name)
        return -1;
    if (!strcmp(name, "valhalla"))
        return NETROUTE_VALHALLA;
    if (!strcmp(name, "osrm"))
        return NETROUTE_OSRM;
    return -1;
}

/* Valhalla says kilometres unless it says miles, and it says which. Reading
 * `units` is not fussiness: a `units=miles` request comes back with
 * `length: 2.7` for the same ride, and believing that is kilometres turns the
 * precision cross-check below into a coin toss. */
static double
vh_metres(const char *trip, const char *summary)
{
    double len = 0.0;
    char units[16];
    int wide;

    if (jnum(jget(summary, "length"), &len) != 0)
        return -1.0;
    if (jstr(jget(trip, "units"), units, sizeof units, &wide) == 0 &&
        !strcmp(units, "miles"))
        return len * 1609.344;
    return len * 1000.0;
}

/* One place, so no exit below can set a reason and forget the code -- router.c's
 * refuse() for the same reason. */
static int
nr_fail(char *why, int nwhy, int *code, int nr, const char *msg)
{
    if (code)
        *code = nr;
    say(why, nwhy, "%s", msg);
    return -1;
}

int
netroute_parse(const char *json, int type, const char *name, route_t *out,
               char *why, int nwhy, int *code)
{
    const char *root, *end, *node, *err;
    route_t r;
    shape_t s;
    double stated = -1.0;
    int prec = 0, ncue;

    if (!out)
        return -1;
    if (code)
        *code = NR_OK;
    if (!json || !*jws(json))
        return nr_fail(why, nwhy, code, NR_BADREPLY, "the reply was empty");
    root = jws(json);
    if (*root != '{') {
        /* A login page, and the commonest failure on a bicycle: the phone
         * hotspot dropped and the device joined a cafe's captive portal, which
         * answers 200 OK to everything. */
        return nr_fail(why, nwhy, code, NR_BADREPLY,
                       root[0] == '<' ? "got a web page, not a route"
                                      : "the reply is not JSON");
    }
    /* Validate all of it before believing any of it -- see the note above the
     * scanner. This is what makes a body truncated after the last field we
     * happen to read a refusal rather than a short route. */
    end = jend(root, 0);
    if (!end || *jws(end))
        return nr_fail(why, nwhy, code, NR_BADREPLY,
                       "the reply is truncated or malformed JSON");

    if (type == NETROUTE_VALHALLA) {
        node = jget(root, "trip");
        if (node)
            stated = vh_metres(node, jget(node, "summary"));
    } else {
        node = jfirst(jget(root, "routes"));
        if (node && jnum(jget(node, "distance"), &stated) != 0)
            stated = -1.0;
    }
    if (!node) {
        /* A 200 OK carrying an error object: Valhalla's 442, OSRM's
         * NoRoute/NoSegment. Its own words beat ours -- "no path could be
         * found" tells a rider to pick a different destination, and "the reply
         * has no route in it" tells them the program is broken. */
        char msg[NETROUTE_WHY];
        double ec = 0.0;
        int wide;
        err = jget(root, "error");
        if (!err)
            err = jget(root, "message");
        if (!err)
            err = jget(root, "code");
        if (err && jstr(err, msg, sizeof msg, &wide) == 0 && msg[0]) {
            /* TOO FAR is its own answer, and the reason this code exists at all.
             * A server that caps a bicycle at 200 km refusing a 203 km trip is
             * saying "not from me", not "there is no way there" -- and a panel
             * reading NO ROUTE tells the rider the opposite of the truth. Read as
             * a NUMBER: error_code is Valhalla's API contract, the sentence
             * beside it is not. */
            if (jnum(jget(root, "error_code"), &ec) == 0 &&
                (int)ec == NETROUTE_VH_TOOFAR)
                return nr_fail(why, nwhy, code, NR_TOOFAR, msg);
            return nr_fail(why, nwhy, code, NR_REFUSED, msg);
        }
        return nr_fail(why, nwhy, code, NR_BADREPLY,
                       "the reply has no route in it");
    }
    if (!(stated > 0.0))
        return nr_fail(why, nwhy, code, NR_BADREPLY,
                       "the reply states no distance");
    /* Everything past here is "the bytes arrived and are not a route", which is
     * NR_BADREPLY to a caller: the router already answered, so a shape that will
     * not decode is a proxy or a precision problem and not the router's refusal.
     * decode_shape() writes the detail into `why` itself. */
    if (decode_shape(node, type, stated, &s, &prec, why, nwhy)) {
        if (code)
            *code = NR_BADREPLY;
        return -1;
    }

    route_init(&r);
    /* AFTER route_init, whose memset would otherwise wipe it back to UNKNOWN.
     * Only Valhalla is asked: OSRM's reply carries no such field, and reading
     * a missing key as false would put NO TOLLS on a page that has no idea. */
    if (type == NETROUTE_VALHALLA) {
        int t;
        if (jbool(jget(jget(node, "summary"), "has_toll"), &t) == 0)
            r.toll = t ? ROUTE_TOLL_YES : ROUTE_TOLL_NO;
    }
    r.pt = s.pt;
    r.npt = s.npt;
    s.pt = NULL; /* r owns the points now; s keeps only the leg bases */
    snprintf(r.name, sizeof r.name, "%s", name && *name ? name
                                                        : "DESTINATION");
    if (route_prepare(&r)) {
        route_free(&r);
        return nr_fail(why, nwhy, code, NR_BADREPLY,
                       "the route is too short to follow");
    }
    /* Shorter than one pair of bearing arms and nothing in it can be
     * classified -- route_cues_derive() gives up at the same threshold -- so
     * what would come back is a line with a flag on the end and no
     * instructions. It is also what a router returns when it snapped both ends
     * to one node, which is router_to()'s "you are already there". */
    if (r.total_m < 2 * CUE_BEARING_M) {
        char msg[NETROUTE_WHY];
        double m = r.total_m;
        route_free(&r);
        snprintf(msg, sizeof msg, "the route is only %.0f m long", m);
        return nr_fail(why, nwhy, code, NR_BADREPLY, msg);
    }
    ncue = count_cues(node, type);
    r.cue = calloc((size_t)ncue + 1, sizeof *r.cue);
    if (!r.cue) {
        route_free(&r);
        say(why, nwhy, "out of memory for %d cues", ncue);
        if (code)
            *code = NR_BADREPLY;
        return -1;
    }
    r.ncue = 0;
    if (type == NETROUTE_VALHALLA)
        vh_cues(node, &r, s.base, s.nleg);
    else
        osrm_cues(node, &r);
    /* The same finish a GPX and an offline route get: fill in what the cue
     * list is missing and put a CUE_DEST on the end if the router did not. */
    if (route_cues_finish(&r) < 0) {
        route_free(&r);
        return nr_fail(why, nwhy, code, NR_BADREPLY,
                       "out of memory finishing the cues");
    }
    *out = r; /* the last line, and the first time `out` is touched at all */
    return 0;
}

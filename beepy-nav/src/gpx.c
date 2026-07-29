/* beepy-nav/src/gpx.c -- see gpx.h. */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gpx.h"

/* --------------------------------------------------------------- scanner */

typedef struct {
    const char *p, *end;
    int line;
    char *err;
    size_t errsz;
    int failed;
} scan_t;

/* Every message carries the line, and the FIRST one wins: once the scanner
 * is off the rails its later complaints describe the wreckage, not the
 * cause. DESIGN.md 7.1 only requires the line; naming the element too is
 * what makes the message actionable without opening the file. */
static void
fail(scan_t *s, const char *fmt, ...)
{
    va_list ap;
    char body[160];
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    if (!s->failed && s->err && s->errsz)
        snprintf(s->err, s->errsz, "line %d: %s", s->line, body);
    s->failed = 1;
}

static void
adv(scan_t *s, size_t n)
{
    while (n-- && s->p < s->end) {
        if (*s->p == '\n')
            s->line++;
        s->p++;
    }
}

static int
at_end(const scan_t *s)
{
    return s->p >= s->end;
}

/* Advance to the next occurrence of `lit`, counting lines. 0 when the
 * document ends first (the cursor is then at the end). */
static int
seek(scan_t *s, const char *lit)
{
    size_t n = strlen(lit);
    while (s->p + n <= s->end) {
        if (*s->p == lit[0] && !memcmp(s->p, lit, n))
            return 1;
        adv(s, 1);
    }
    while (!at_end(s))
        adv(s, 1);
    return 0;
}

static void
skip_ws(scan_t *s)
{
    while (!at_end(s) && isspace((unsigned char)*s->p))
        adv(s, 1);
}

static int
name_char(int c)
{
    return isalnum(c) || c == '_' || c == '-' || c == ':' || c == '.';
}

/* Read the element or attribute name at the cursor into buf, lowercased and
 * with any namespace prefix dropped -- "gpx:trkpt" and "trkpt" are the same
 * element here, since nothing depends on which namespace declared it.
 * Returns the length, 0 when there is no name at the cursor. */
static size_t
take_name(scan_t *s, char *buf, size_t bufsz)
{
    const char *start = s->p, *q = s->p, *colon;
    size_t n;
    while (q < s->end && name_char((unsigned char)*q))
        q++;
    colon = memchr(start, ':', (size_t)(q - start));
    if (colon)
        start = colon + 1;
    n = (size_t)(q - start);
    if (n >= bufsz)
        n = bufsz - 1;
    {
        size_t i;
        for (i = 0; i < n; i++)
            buf[i] = (char)tolower((unsigned char)start[i]);
    }
    buf[n] = '\0';
    adv(s, (size_t)(q - s->p));
    return n;
}

/* --------------------------------------------------------------- entities */

/* DESIGN.md 7.1: decode in TEXT fields only. Attribute values are numbers
 * and never carry entities in any emitter we target, and decoding them would
 * mean deciding what "&amp;" means inside lat=" ", which is nothing good. */
static void
decode_entities(char *s)
{
    char *r = s, *w = s;
    while (*r) {
        if (*r != '&') {
            *w++ = *r++;
            continue;
        }
        if (!strncmp(r, "&amp;", 5)) {
            *w++ = '&';
            r += 5;
        } else if (!strncmp(r, "&lt;", 4)) {
            *w++ = '<';
            r += 4;
        } else if (!strncmp(r, "&gt;", 4)) {
            *w++ = '>';
            r += 4;
        } else if (!strncmp(r, "&quot;", 6)) {
            *w++ = '"';
            r += 6;
        } else if (!strncmp(r, "&apos;", 6)) {
            *w++ = '\'';
            r += 6;
        } else if (r[1] == '#') {
            const char *q = r + 2;
            long v;
            char *endp;
            int hex = (*q == 'x' || *q == 'X');
            v = strtol(hex ? q + 1 : q, &endp, hex ? 16 : 10);
            if (endp != (hex ? q + 1 : q) && *endp == ';' && v > 0 &&
                v < 0x110000) {
                /* UTF-8, so a decoded name still round-trips through the
                 * 5x7 font's ASCII fold rather than becoming a stray byte. */
                if (v < 0x80) {
                    *w++ = (char)v;
                } else if (v < 0x800) {
                    *w++ = (char)(0xC0 | (v >> 6));
                    *w++ = (char)(0x80 | (v & 0x3F));
                } else if (v < 0x10000) {
                    *w++ = (char)(0xE0 | (v >> 12));
                    *w++ = (char)(0x80 | ((v >> 6) & 0x3F));
                    *w++ = (char)(0x80 | (v & 0x3F));
                } else {
                    *w++ = (char)(0xF0 | (v >> 18));
                    *w++ = (char)(0x80 | ((v >> 12) & 0x3F));
                    *w++ = (char)(0x80 | ((v >> 6) & 0x3F));
                    *w++ = (char)(0x80 | (v & 0x3F));
                }
                r = endp + 1;
            } else {
                *w++ = *r++; /* not an entity; a literal ampersand */
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static void
trim(char *s)
{
    char *a = s, *b;
    while (*a && isspace((unsigned char)*a))
        a++;
    if (a != s)
        memmove(s, a, strlen(a) + 1);
    b = s + strlen(s);
    while (b > s && isspace((unsigned char)b[-1]))
        *--b = '\0';
}

/* --------------------------------------------------------------- sym map */

/* Fold to lowercase alphanumerics so "Slight Right", "slight-right" and
 * "SlightRight" all hash to the same key. */
static void
fold(const char *s, char *out, size_t outsz)
{
    size_t n = 0;
    for (; *s && n + 1 < outsz; s++)
        if (isalnum((unsigned char)*s))
            out[n++] = (char)tolower((unsigned char)*s);
    out[n] = '\0';
}

int
gpx_sym_kind(const char *s)
{
    static const struct {
        const char *key;
        int kind;
    } MAP[] = {
        {"straight", CUE_STRAIGHT},        {"continue", CUE_STRAIGHT},
        {"gostraight", CUE_STRAIGHT},      {"tst", CUE_STRAIGHT},
        {"slightleft", CUE_SLIGHT_LEFT},   {"bearleft", CUE_SLIGHT_LEFT},
        {"tsll", CUE_SLIGHT_LEFT},         {"left", CUE_LEFT},
        {"turnleft", CUE_LEFT},            {"tl", CUE_LEFT},
        {"sharpleft", CUE_SHARP_LEFT},     {"tshl", CUE_SHARP_LEFT},
        {"slightright", CUE_SLIGHT_RIGHT}, {"bearright", CUE_SLIGHT_RIGHT},
        {"tslr", CUE_SLIGHT_RIGHT},        {"right", CUE_RIGHT},
        {"turnright", CUE_RIGHT},          {"tr", CUE_RIGHT},
        {"sharpright", CUE_SHARP_RIGHT},   {"tshr", CUE_SHARP_RIGHT},
        {"uturn", CUE_UTURN},              {"tu", CUE_UTURN},
        {"destination", CUE_DEST},         {"end", CUE_DEST},
        {"finish", CUE_DEST},              {"arrive", CUE_DEST},
    };
    char key[64];
    size_t i;
    if (!s)
        return -1;
    fold(s, key, sizeof key);
    if (!key[0])
        return -1;
    for (i = 0; i < sizeof MAP / sizeof MAP[0]; i++)
        if (!strcmp(MAP[i].key, key))
            return MAP[i].kind;
    return -1;
}

/* ----------------------------------------------------------------- points */

typedef struct {
    pt_t *pt;
    int n, cap;
    cue_t *cue;
    int ncue, ccap;
} accum_t;

static int
push_pt(accum_t *a, double lat, double lon, double ele)
{
    if (a->n >= a->cap) {
        int cap = a->cap ? a->cap * 2 : 256;
        pt_t *np = realloc(a->pt, (size_t)cap * sizeof *np);
        if (!np)
            return -1;
        a->pt = np;
        a->cap = cap;
    }
    a->pt[a->n].lat = lat;
    a->pt[a->n].lon = lon;
    a->pt[a->n].ele = ele;
    a->n++;
    return 0;
}

static int
push_cue(accum_t *a, int idx, int kind, const char *name)
{
    if (a->ncue >= a->ccap) {
        int cap = a->ccap ? a->ccap * 2 : 64;
        cue_t *nc = realloc(a->cue, (size_t)cap * sizeof *nc);
        if (!nc)
            return -1;
        a->cue = nc;
        a->ccap = cap;
    }
    memset(&a->cue[a->ncue], 0, sizeof a->cue[a->ncue]);
    a->cue[a->ncue].idx = idx;
    a->cue[a->ncue].kind = kind;
    if (name)
        snprintf(a->cue[a->ncue].name, CUE_NAME, "%s", name);
    a->ncue++;
    return 0;
}

/* A number that must consume its whole (trimmed) token: "12.5x" and "" are
 * both errors, which is the difference between strtod and a parser. */
static int
parse_num(const char *s, double *out)
{
    char *endp;
    double v;
    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return -1;
    v = strtod(s, &endp);
    if (endp == s)
        return -1;
    while (*endp && isspace((unsigned char)*endp))
        endp++;
    if (*endp)
        return -1;
    *out = v;
    return 0;
}

/* Read the text content of the element whose '>' the cursor has just passed,
 * up to its </close>. Text is entity-decoded; markup inside is an error,
 * because a <name> containing elements is not something this scanner can
 * honestly flatten. */
static int
read_text(scan_t *s, const char *tag, char *buf, size_t bufsz)
{
    const char *start = s->p;
    size_t n;
    char close[64];
    snprintf(close, sizeof close, "</%s", tag);
    while (!at_end(s) && *s->p != '<')
        adv(s, 1);
    n = (size_t)(s->p - start);
    if (at_end(s)) {
        fail(s, "unterminated <%s>", tag);
        return -1;
    }
    if (n >= bufsz)
        n = bufsz - 1;
    memcpy(buf, start, n);
    buf[n] = '\0';
    decode_entities(buf);
    trim(buf);
    /* The cursor is on '<'. It must be this element's close tag. */
    if (strncmp(s->p, close, strlen(close)) ||
        name_char((unsigned char)s->p[strlen(close)])) {
        fail(s, "<%s> is not closed before the next element", tag);
        return -1;
    }
    if (!seek(s, ">")) {
        fail(s, "unterminated </%s>", tag);
        return -1;
    }
    adv(s, 1);
    return 0;
}

/* Skip an element whose opening tag has just been consumed up to and
 * including '>', honouring nesting. self_closed short-circuits. */
static int
skip_element(scan_t *s, const char *tag, int self_closed)
{
    int depth = 1;
    char name[64];
    if (self_closed)
        return 0;
    while (depth > 0) {
        if (!seek(s, "<")) {
            fail(s, "unterminated <%s>", tag);
            return -1;
        }
        adv(s, 1);
        if (!at_end(s) && *s->p == '/') {
            adv(s, 1);
            take_name(s, name, sizeof name);
            if (!strcmp(name, tag))
                depth--;
            if (!seek(s, ">")) {
                fail(s, "unterminated </%s>", tag);
                return -1;
            }
            adv(s, 1);
        } else if (!at_end(s) && (*s->p == '!' || *s->p == '?')) {
            if (!seek(s, ">")) {
                fail(s, "unterminated <%c...>", *s->p);
                return -1;
            }
            adv(s, 1);
        } else {
            char inner[64];
            int selfc = 0;
            take_name(s, inner, sizeof inner);
            while (!at_end(s) && *s->p != '>') {
                if (*s->p == '/' && s->p + 1 < s->end && s->p[1] == '>')
                    selfc = 1;
                adv(s, 1);
            }
            if (at_end(s)) {
                fail(s, "unterminated <%s>", inner);
                return -1;
            }
            adv(s, 1);
            if (!selfc && !strcmp(inner, tag))
                depth++;
        }
    }
    return 0;
}

/* One <trkpt>/<rtept>. The cursor sits just past the tag name. */
static int
parse_point(scan_t *s, const char *tag, accum_t *acc)
{
    double lat = 0, lon = 0, ele = 0;
    int have_lat = 0, have_lon = 0, self_closed = 0;
    int kind = -1, name_rank = 0;
    char cue_name[CUE_NAME] = "";
    char attr[64], val[128];

    for (;;) {
        skip_ws(s);
        if (at_end(s)) {
            fail(s, "unterminated <%s>", tag);
            return -1;
        }
        if (*s->p == '>') {
            adv(s, 1);
            break;
        }
        if (*s->p == '/') {
            adv(s, 1);
            if (at_end(s) || *s->p != '>') {
                fail(s, "stray '/' in <%s>", tag);
                return -1;
            }
            adv(s, 1);
            self_closed = 1;
            break;
        }
        if (!take_name(s, attr, sizeof attr)) {
            fail(s, "unexpected '%c' in <%s>", *s->p, tag);
            return -1;
        }
        skip_ws(s);
        if (at_end(s) || *s->p != '=') {
            fail(s, "attribute %s of <%s> has no value", attr, tag);
            return -1;
        }
        adv(s, 1);
        skip_ws(s);
        if (at_end(s) || (*s->p != '"' && *s->p != '\'')) {
            fail(s, "attribute %s of <%s> is not quoted", attr, tag);
            return -1;
        }
        {
            char q = *s->p;
            const char *start;
            size_t n;
            adv(s, 1);
            start = s->p;
            while (!at_end(s) && *s->p != q)
                adv(s, 1);
            if (at_end(s)) {
                fail(s, "unterminated value for %s of <%s>", attr, tag);
                return -1;
            }
            n = (size_t)(s->p - start);
            if (n >= sizeof val)
                n = sizeof val - 1;
            memcpy(val, start, n);
            val[n] = '\0';
            adv(s, 1);
        }
        /* lat and lon in EITHER order -- the whole point of reading the
         * attributes rather than matching a fixed pattern. */
        if (!strcmp(attr, "lat")) {
            if (parse_num(val, &lat)) {
                fail(s, "<%s> lat=\"%s\" is not a number", tag, val);
                return -1;
            }
            if (lat < -90.0 || lat > 90.0) {
                fail(s, "<%s> lat=\"%s\" is out of range", tag, val);
                return -1;
            }
            have_lat = 1;
        } else if (!strcmp(attr, "lon")) {
            if (parse_num(val, &lon)) {
                fail(s, "<%s> lon=\"%s\" is not a number", tag, val);
                return -1;
            }
            if (lon < -180.0 || lon > 180.0) {
                fail(s, "<%s> lon=\"%s\" is out of range", tag, val);
                return -1;
            }
            have_lon = 1;
        }
    }
    if (!have_lat) {
        fail(s, "<%s> has no lat attribute", tag);
        return -1;
    }
    if (!have_lon) {
        fail(s, "<%s> has no lon attribute", tag);
        return -1;
    }

    if (!self_closed) {
        for (;;) {
            char child[64];
            if (!seek(s, "<")) {
                fail(s, "unterminated <%s>", tag);
                return -1;
            }
            adv(s, 1);
            if (at_end(s)) {
                fail(s, "unterminated <%s>", tag);
                return -1;
            }
            if (*s->p == '/') {
                adv(s, 1);
                take_name(s, child, sizeof child);
                if (strcmp(child, tag)) {
                    fail(s, "</%s> closes <%s>", child, tag);
                    return -1;
                }
                if (!seek(s, ">")) {
                    fail(s, "unterminated </%s>", tag);
                    return -1;
                }
                adv(s, 1);
                break;
            }
            if (*s->p == '!' || *s->p == '?') {
                if (!seek(s, ">")) {
                    fail(s, "unterminated <%c...>", *s->p);
                    return -1;
                }
                adv(s, 1);
                continue;
            }
            if (!take_name(s, child, sizeof child)) {
                fail(s, "unexpected '%c' inside <%s>", *s->p, tag);
                return -1;
            }
            {
                int selfc = 0;
                while (!at_end(s) && *s->p != '>') {
                    if (*s->p == '/' && s->p + 1 < s->end && s->p[1] == '>')
                        selfc = 1;
                    adv(s, 1);
                }
                if (at_end(s)) {
                    fail(s, "unterminated <%s> inside <%s>", child, tag);
                    return -1;
                }
                adv(s, 1);
                if (selfc)
                    continue;
            }
            if (!strcmp(child, "ele")) {
                char buf[64];
                if (read_text(s, child, buf, sizeof buf))
                    return -1;
                if (buf[0] && parse_num(buf, &ele)) {
                    fail(s, "<ele>%s</ele> is not a number", buf);
                    return -1;
                }
            } else if (!strcmp(child, "sym") || !strcmp(child, "type")) {
                char buf[96];
                int k;
                if (read_text(s, child, buf, sizeof buf))
                    return -1;
                /* <sym> wins over <type>: RideWithGPS puts the manoeuvre in
                 * sym and a category ("Cue") in type, so first-match-wins in
                 * document order would depend on their ordering. */
                k = gpx_sym_kind(buf);
                if (k >= 0 && (kind < 0 || !strcmp(child, "sym")))
                    kind = k;
            } else if (!strcmp(child, "cmt") || !strcmp(child, "desc") ||
                       !strcmp(child, "name")) {
                char buf[CUE_NAME * 2];
                /* cmt beats desc beats name: the emitters put the spoken
                 * instruction in cmt, a longer form in desc and a bare
                 * street or waypoint label in name. Ranked rather than
                 * first-wins, so document order cannot decide it. */
                int rank = !strcmp(child, "cmt")    ? 3
                           : !strcmp(child, "desc") ? 2
                                                    : 1;
                if (read_text(s, child, buf, sizeof buf))
                    return -1;
                if (buf[0] && rank > name_rank) {
                    /* buf is deliberately twice CUE_NAME so a long
                     * instruction is read whole and trimmed rather than cut
                     * mid-entity; the precision is where it gets shortened. */
                    snprintf(cue_name, sizeof cue_name, "%.*s",
                             (int)sizeof cue_name - 1, buf);
                    name_rank = rank;
                }
            } else {
                if (skip_element(s, child, 0))
                    return -1;
            }
        }
    }

    if (push_pt(acc, lat, lon, ele)) {
        fail(s, "out of memory");
        return -1;
    }
    if (acc->n > GPX_MAXRAW) {
        fail(s, "more than %d points", GPX_MAXRAW);
        return -1;
    }
    /* A cue is a point the file has labelled. A bare <name> is not enough --
     * every Komoot trkpt would become one -- so a manoeuvre symbol is
     * required, and the name rides along with it. */
    if (kind >= 0 && push_cue(acc, acc->n - 1, kind, cue_name)) {
        fail(s, "out of memory");
        return -1;
    }
    return 0;
}

/* ---------------------------------------------------------------- loading */

/* DESIGN.md 7.1: decimate to the cap. Evenly spaced by index and keeping
 * both ends exactly -- a route whose finish moved would break TO GO. */
static void
decimate(accum_t *a, int cap)
{
    int i;
    if (a->n <= cap)
        return;
    for (i = 0; i < cap; i++)
        a->pt[i] = a->pt[(int)((long)i * (a->n - 1) / (cap - 1))];
    /* Cue indices refer to raw points; move them onto the kept ones. */
    for (i = 0; i < a->ncue; i++) {
        long src = a->cue[i].idx;
        int j = (int)((src * (cap - 1) + (a->n - 1) / 2) / (a->n - 1));
        a->cue[i].idx = j < 0 ? 0 : (j > cap - 1 ? cap - 1 : j);
    }
    a->n = cap;
}

int
gpx_parse(const char *xml, size_t len, route_t *r, char *err, size_t errsz)
{
    scan_t s;
    accum_t acc;
    char tag[64];
    char rname[ROUTE_NAME] = "";
    int in_point_container = 0;

    route_init(r);
    memset(&acc, 0, sizeof acc);
    s.p = xml;
    s.end = xml + len;
    s.line = 1;
    s.err = err;
    s.errsz = errsz;
    s.failed = 0;
    if (err && errsz)
        err[0] = '\0';

    while (!s.failed && seek(&s, "<")) {
        if (s.p + 4 <= s.end && !memcmp(s.p, "<!--", 4)) {
            adv(&s, 4);
            if (!seek(&s, "-->")) {
                fail(&s, "unterminated comment");
                break;
            }
            adv(&s, 3);
            continue;
        }
        adv(&s, 1);
        if (at_end(&s)) {
            fail(&s, "unterminated tag");
            break;
        }
        if (*s.p == '?' || *s.p == '!') {
            if (!seek(&s, ">")) {
                fail(&s, "unterminated <%c...>", *s.p);
                break;
            }
            adv(&s, 1);
            continue;
        }
        if (*s.p == '/') {
            adv(&s, 1);
            take_name(&s, tag, sizeof tag);
            if (!strcmp(tag, "trk") || !strcmp(tag, "rte") ||
                !strcmp(tag, "trkseg"))
                in_point_container = 0;
            if (!seek(&s, ">")) {
                fail(&s, "unterminated </%s>", tag);
                break;
            }
            adv(&s, 1);
            continue;
        }
        if (!take_name(&s, tag, sizeof tag)) {
            fail(&s, "'<' is not followed by a tag name");
            break;
        }
        if (!strcmp(tag, "trkpt") || !strcmp(tag, "rtept")) {
            if (parse_point(&s, tag, &acc))
                break;
            continue;
        }
        /* Everything else: consume the attributes, then decide. */
        {
            int selfc = 0;
            while (!at_end(&s) && *s.p != '>') {
                if (*s.p == '"' || *s.p == '\'') {
                    char q = *s.p;
                    adv(&s, 1);
                    while (!at_end(&s) && *s.p != q)
                        adv(&s, 1);
                    if (at_end(&s))
                        break;
                    adv(&s, 1);
                    continue;
                }
                if (*s.p == '/' && s.p + 1 < s.end && s.p[1] == '>')
                    selfc = 1;
                adv(&s, 1);
            }
            if (at_end(&s)) {
                fail(&s, "unterminated <%s>", tag);
                break;
            }
            adv(&s, 1);
            if (selfc)
                continue;
            if (!strcmp(tag, "trk") || !strcmp(tag, "rte") ||
                !strcmp(tag, "trkseg") || !strcmp(tag, "gpx") ||
                !strcmp(tag, "metadata")) {
                if (!strcmp(tag, "trk") || !strcmp(tag, "rte"))
                    in_point_container = 1;
                continue; /* descend */
            }
            if (!strcmp(tag, "name") && !rname[0]) {
                /* The route's own name: <metadata><name> or <trk><name>,
                 * first one wins. Never a <name> inside a point -- those are
                 * consumed by parse_point and never reach here. */
                char buf[ROUTE_NAME * 2];
                if (read_text(&s, tag, buf, sizeof buf))
                    break;
                snprintf(rname, sizeof rname, "%.*s", (int)sizeof rname - 1,
                         buf);
                (void)in_point_container;
                continue;
            }
            if (skip_element(&s, tag, 0))
                break;
        }
    }

    if (!s.failed && acc.n < 2) {
        /* Fail rather than half-load: one point is not a route, and the
         * pages have no honest way to say so. */
        if (acc.n == 0)
            fail(&s, "no <trkpt> or <rtept> in the file");
        else
            fail(&s, "only 1 point in the file");
    }
    if (s.failed) {
        free(acc.pt);
        free(acc.cue);
        return -1;
    }

    r->decimated = acc.n;
    decimate(&acc, ROUTE_MAXPT);
    r->pt = acc.pt;
    r->npt = acc.n;
    r->cue = acc.cue;
    r->ncue = acc.ncue;
    snprintf(r->name, ROUTE_NAME, "%s", rname[0] ? rname : "ROUTE");
    return 0;
}

int
gpx_load(const char *path, route_t *r, char *err, size_t errsz)
{
    FILE *f;
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char sub[192];
    int rc;

    route_init(r);
    f = fopen(path, "rb");
    if (!f) {
        if (err && errsz)
            snprintf(err, errsz, "%s: cannot open", path);
        return -1;
    }
    for (;;) {
        size_t got;
        if (len + 65536 + 1 > cap) {
            size_t ncap = cap ? cap * 2 : 262144;
            char *nb;
            while (ncap < len + 65536 + 1)
                ncap *= 2;
            nb = realloc(buf, ncap);
            if (!nb) {
                free(buf);
                fclose(f);
                if (err && errsz)
                    snprintf(err, errsz, "%s: out of memory", path);
                return -1;
            }
            buf = nb;
            cap = ncap;
        }
        got = fread(buf + len, 1, 65536, f);
        len += got;
        if (got < 65536)
            break;
    }
    if (ferror(f)) {
        free(buf);
        fclose(f);
        if (err && errsz)
            snprintf(err, errsz, "%s: read error", path);
        return -1;
    }
    fclose(f);
    buf[len] = '\0';

    rc = gpx_parse(buf, len, r, sub, sizeof sub);
    free(buf);
    if (rc && err && errsz)
        snprintf(err, errsz, "%s: %s", path, sub);
    return rc;
}

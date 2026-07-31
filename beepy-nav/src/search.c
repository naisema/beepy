/* beepy-nav/src/search.c -- see search.h. The reader for tools/mkpack.py's
 * road pack, and the token-AND search of DESIGN.md 1.4.
 *
 * Two things in here are decisions rather than transcription:
 *
 *   - the pack is read WHOLE into memory, unlike the tile pack, which streams
 *     8 KB tiles through a twelve-slot LRU. A tile pack is a raster and its
 *     working set is the nine tiles under one frame; a road graph has no
 *     working set at all -- Dijkstra touches every node it can reach, and a
 *     search touches every name. The Asok pack is 101 KB and a whole-city pack
 *     is a few megabytes against 319 MB free (DESIGN.md 3), so paging it would
 *     buy nothing and cost a seek per node expansion;
 *   - the tables are DECODED at open into native arrays rather than pointed at
 *     in a mapped buffer. The pack is little-endian by definition and the same
 *     byte-at-a-time readers tile.c uses do the conversion, so the reader never
 *     silently depends on the host agreeing, nor on an aligned load being legal
 *     at an arbitrary file offset.
 */
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "search.h"

#ifndef M_PI /* glibc hides it under -std=c11 (strict ISO) */
#define M_PI 3.14159265358979323846
#endif

#define MAGIC "BNAVROAD"
#define ROADS_VERSION 2
#define HDR_BYTES 64
#define SECT_ENTRY 8
#define NSECT 6
#define FLAG_ONEWAY 1u

/* Sections, in the fixed order mkpack.py writes them. */
enum { S_NODES, S_ADJ, S_EDGES, S_PLACES, S_POINTS, S_STRINGS };

/* Sanity ceilings. Not budgets -- a whole-city pack is 10^5 nodes and
 * DESIGN.md 1.4 expects 10^6 to work -- but a corrupt header must not be
 * allowed to ask for a terabyte before the first read fails. */
#define MAX_NODES 8000000
#define MAX_EDGES 24000000
#define MAX_PLACES 4000000
#define MAX_POINTS 24000000
#define MAX_STRINGS 128000000

/* DESIGN.md 1.4's tokenisation: a query is words, and no rider types twelve of
 * them at a handlebar. */
#define MAX_TOKENS 8
#define MAX_TOKEN 32

struct roads {
    double lat0, lon0;
    double klat, klon;   /* metres per degree, as written */
    double ke;           /* klon * cos(lat0), computed once */
    unsigned int flags;
    unsigned int ndropped;
    unsigned int nway;

    int nnode;
    double *en;          /* 2*nnode, pack metres */
    int32_t *nll;        /* 2*nnode, lat/lon in 1e-7 degrees */
    unsigned int *adj;   /* nnode + 1 */
    int nedge;
    roadedge_t *edge;

    int nplace;
    unsigned int *pname; /* nplace, byte offset into `str` */
    unsigned int *pfirst;
    unsigned int *pcount;

    int npoint;
    double *pen;         /* 2*npoint, pack metres */

    int nstr;
    char *str;
};

/* ------------------------------------------------------- little-endian I/O
 *
 * The same byte-at-a-time readers tile.c uses, for the same reason: the pack is
 * little-endian by definition and the reader must not depend on the host
 * agreeing, nor on an aligned load being legal at an arbitrary offset. */

static uint32_t
rd_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t
rd_u16(const unsigned char *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t
rd_i32(const unsigned char *p)
{
    return (int32_t)rd_u32(p);
}

static double
rd_f64(const unsigned char *p)
{
    uint64_t u = 0;
    double d;
    int i;
    for (i = 7; i >= 0; i--)
        u = (u << 8) | p[i];
    memcpy(&d, &u, sizeof d);
    return d;
}

static void
fail(char *why, int nwhy, const char *msg)
{
    if (why && nwhy > 0)
        snprintf(why, (size_t)nwhy, "%s", msg);
}

/* Read a whole section into a malloc'd buffer of `n` records of `rec` bytes. */
static unsigned char *
sect_read(FILE *f, long fsize, uint32_t off, uint32_t n, int rec, char *why,
          int nwhy)
{
    unsigned char *buf;
    size_t bytes;
    if ((uint64_t)n * (uint64_t)rec > (uint64_t)0x40000000) {
        fail(why, nwhy, "a section larger than a gigabyte");
        return NULL;
    }
    bytes = (size_t)n * (size_t)rec;
    if ((long)off < HDR_BYTES || (long)off + (long)bytes > fsize) {
        fail(why, nwhy, "a section runs outside the file");
        return NULL;
    }
    /* One byte over, so a zero-length section still returns a pointer and the
     * caller does not have to special-case an empty table. */
    buf = (unsigned char *)malloc(bytes + 1);
    if (!buf) {
        fail(why, nwhy, "out of memory");
        return NULL;
    }
    buf[bytes] = 0;
    if (bytes && (fseek(f, (long)off, SEEK_SET) != 0 ||
                  fread(buf, 1, bytes, f) != bytes)) {
        fail(why, nwhy, "truncated section");
        free(buf);
        return NULL;
    }
    return buf;
}

/* ------------------------------------------------------------ open / close */

roads_t *
roads_open(const char *path, char *why, int nwhy)
{
    unsigned char hdr[HDR_BYTES];
    unsigned char stab[SECT_ENTRY * NSECT];
    unsigned char *raw = NULL;
    uint32_t soff[NSECT], scount[NSECT], scale;
    roads_t *g;
    FILE *f = NULL;
    long fsize;
    int i;

    fail(why, nwhy, "");
    if (!path || !*path) {
        fail(why, nwhy, "no path");
        return NULL;
    }
    g = (roads_t *)calloc(1, sizeof *g);
    if (!g) {
        fail(why, nwhy, "out of memory");
        return NULL;
    }
    f = fopen(path, "rb");
    if (!f) {
        fail(why, nwhy, "cannot open");
        free(g);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (fsize = ftell(f)) < HDR_BYTES) {
        fail(why, nwhy, "too short to be a road pack");
        goto bad;
    }
    rewind(f);
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) {
        fail(why, nwhy, "truncated header");
        goto bad;
    }
    if (memcmp(hdr, MAGIC, 8) != 0) {
        fail(why, nwhy, "not a beepy-nav road pack");
        goto bad;
    }
    if (rd_u16(hdr + 8) != ROADS_VERSION) {
        /* Which version, which is wanted, and what to run. This message is the
         * one a rider actually meets after a version bump -- 7.7's road class
         * took BNAVROAD to v2 and every pack built before it reads as v1 -- and
         * "unsupported pack version" on its own sent them to the source to find
         * out what to do about it. The tile format did not move, which is why the
         * command named here rebuilds only this pack. */
        char msg[96];
        snprintf(msg, sizeof msg,
                 "road pack is v%u, need v%u: tools/mkmaps.sh --roads-only",
                 rd_u16(hdr + 8), (unsigned)ROADS_VERSION);
        fail(why, nwhy, msg);
        goto bad;
    }
    if (rd_u16(hdr + 10) != HDR_BYTES) {
        fail(why, nwhy, "unexpected header size");
        goto bad;
    }
    g->flags = rd_u16(hdr + 12);
    if (rd_u16(hdr + 14) != NSECT) {
        fail(why, nwhy, "unexpected section count");
        goto bad;
    }
    g->nway = rd_u32(hdr + 16);
    g->lat0 = rd_f64(hdr + 20);
    g->lon0 = rd_f64(hdr + 28);
    g->klat = rd_f64(hdr + 36);
    g->klon = rd_f64(hdr + 44);
    scale = rd_u32(hdr + 52);
    g->ndropped = rd_u32(hdr + 56);
    if (!(g->klat > 0.0) || !(g->klon > 0.0)) {
        fail(why, nwhy, "bad projection constants");
        goto bad;
    }
    if (scale != 10000000u) {
        fail(why, nwhy, "unexpected coordinate scale");
        goto bad;
    }
    g->ke = g->klon * cos(g->lat0 * (M_PI / 180.0));
    if (!(fabs(g->ke) > 1.0)) {
        /* The reference is within a degree of a pole, where a tangent plane in
         * degrees of longitude is not a projection at all. */
        fail(why, nwhy, "reference too near a pole");
        goto bad;
    }

    if (fread(stab, 1, sizeof stab, f) != sizeof stab) {
        fail(why, nwhy, "truncated section table");
        goto bad;
    }
    for (i = 0; i < NSECT; i++) {
        soff[i] = rd_u32(stab + SECT_ENTRY * i);
        scount[i] = rd_u32(stab + SECT_ENTRY * i + 4);
    }
    if (scount[S_NODES] < 2 || scount[S_NODES] > MAX_NODES ||
        scount[S_ADJ] != scount[S_NODES] + 1 ||
        scount[S_EDGES] > MAX_EDGES || scount[S_PLACES] > MAX_PLACES ||
        scount[S_POINTS] > MAX_POINTS || scount[S_STRINGS] > MAX_STRINGS) {
        fail(why, nwhy, "implausible section counts");
        goto bad;
    }
    g->nnode = (int)scount[S_NODES];
    g->nedge = (int)scount[S_EDGES];
    g->nplace = (int)scount[S_PLACES];
    g->npoint = (int)scount[S_POINTS];
    g->nstr = (int)scount[S_STRINGS];

    /* --- nodes: lat/lon as stored, and metres for everything that measures */
    raw = sect_read(f, fsize, soff[S_NODES], scount[S_NODES], 8, why, nwhy);
    if (!raw)
        goto bad;
    g->nll = (int32_t *)malloc((size_t)g->nnode * 2 * sizeof *g->nll);
    g->en = (double *)malloc((size_t)g->nnode * 2 * sizeof *g->en);
    if (!g->nll || !g->en) {
        fail(why, nwhy, "out of memory");
        goto bad;
    }
    for (i = 0; i < g->nnode; i++) {
        int32_t la = rd_i32(raw + 8 * i), lo = rd_i32(raw + 8 * i + 4);
        g->nll[2 * i] = la;
        g->nll[2 * i + 1] = lo;
        roads_project(g, la / 1e7, lo / 1e7, &g->en[2 * i], &g->en[2 * i + 1]);
    }
    free(raw);
    raw = NULL;

    /* --- CSR offsets */
    raw = sect_read(f, fsize, soff[S_ADJ], scount[S_ADJ], 4, why, nwhy);
    if (!raw)
        goto bad;
    g->adj = (unsigned int *)malloc((size_t)(g->nnode + 1) * sizeof *g->adj);
    if (!g->adj) {
        fail(why, nwhy, "out of memory");
        goto bad;
    }
    for (i = 0; i <= g->nnode; i++)
        g->adj[i] = rd_u32(raw + 4 * i);
    free(raw);
    raw = NULL;
    /* Validated once, here, so the router's inner loop can trust it: monotone,
     * starting at 0, ending at the edge count. A pack that fails this would
     * otherwise read past the edge table on some node nobody visits in the
     * test suite. */
    if (g->adj[0] != 0 || g->adj[g->nnode] != (unsigned int)g->nedge) {
        fail(why, nwhy, "adjacency does not span the edge table");
        goto bad;
    }
    for (i = 0; i < g->nnode; i++)
        if (g->adj[i] > g->adj[i + 1]) {
            fail(why, nwhy, "adjacency is not monotone");
            goto bad;
        }

    /* --- edges */
    raw = sect_read(f, fsize, soff[S_EDGES], scount[S_EDGES], 12, why, nwhy);
    if (!raw)
        goto bad;
    g->edge = (roadedge_t *)malloc((size_t)(g->nedge + 1) * sizeof *g->edge);
    if (!g->edge) {
        fail(why, nwhy, "out of memory");
        goto bad;
    }
    memset(&g->edge[g->nedge], 0, sizeof g->edge[0]);
    for (i = 0; i < g->nedge; i++) {
        g->edge[i].to = rd_u32(raw + 12 * i);
        g->edge[i].len_mm = rd_u32(raw + 12 * i + 4);
        g->edge[i].flags = rd_u16(raw + 12 * i + 8);
        g->edge[i].name = rd_u16(raw + 12 * i + 10);
        if (g->edge[i].to >= (unsigned int)g->nnode) {
            fail(why, nwhy, "an edge points outside the node table");
            goto bad;
        }
    }
    free(raw);
    raw = NULL;

    /* --- places */
    raw = sect_read(f, fsize, soff[S_PLACES], scount[S_PLACES], 12, why, nwhy);
    if (!raw)
        goto bad;
    g->pname = (unsigned int *)malloc((size_t)(g->nplace + 1) * sizeof *g->pname);
    g->pfirst = (unsigned int *)malloc((size_t)(g->nplace + 1) * sizeof *g->pfirst);
    g->pcount = (unsigned int *)malloc((size_t)(g->nplace + 1) * sizeof *g->pcount);
    if (!g->pname || !g->pfirst || !g->pcount) {
        fail(why, nwhy, "out of memory");
        goto bad;
    }
    for (i = 0; i < g->nplace; i++) {
        g->pname[i] = rd_u32(raw + 12 * i);
        g->pfirst[i] = rd_u32(raw + 12 * i + 4);
        g->pcount[i] = rd_u32(raw + 12 * i + 8);
        if (g->pname[i] >= (unsigned int)g->nstr ||
            (uint64_t)g->pfirst[i] + g->pcount[i] > (uint64_t)g->npoint) {
            fail(why, nwhy, "a place points outside its tables");
            goto bad;
        }
    }
    free(raw);
    raw = NULL;

    /* --- place points */
    raw = sect_read(f, fsize, soff[S_POINTS], scount[S_POINTS], 8, why, nwhy);
    if (!raw)
        goto bad;
    g->pen = (double *)malloc((size_t)(g->npoint + 1) * 2 * sizeof *g->pen);
    if (!g->pen) {
        fail(why, nwhy, "out of memory");
        goto bad;
    }
    g->pen[2 * g->npoint] = g->pen[2 * g->npoint + 1] = 0.0;
    for (i = 0; i < g->npoint; i++)
        roads_project(g, rd_i32(raw + 8 * i) / 1e7, rd_i32(raw + 8 * i + 4) / 1e7,
                      &g->pen[2 * i], &g->pen[2 * i + 1]);
    free(raw);
    raw = NULL;

    /* --- strings. The last byte must be a NUL or a name could run off the end
     * of the buffer; sect_read() writes one past the section for exactly that,
     * and this asserts the pack meant it. */
    raw = sect_read(f, fsize, soff[S_STRINGS], scount[S_STRINGS], 1, why, nwhy);
    if (!raw)
        goto bad;
    g->str = (char *)raw;
    raw = NULL;
    if (g->nstr > 0 && g->str[g->nstr - 1] != '\0') {
        fail(why, nwhy, "the string table is not NUL-terminated");
        goto bad;
    }

    fclose(f);
    return g;

bad:
    free(raw);
    if (f)
        fclose(f);
    roads_close(g);
    return NULL;
}

void
roads_close(roads_t *g)
{
    if (!g)
        return;
    free(g->en);
    free(g->nll);
    free(g->adj);
    free(g->edge);
    free(g->pname);
    free(g->pfirst);
    free(g->pcount);
    free(g->pen);
    free(g->str);
    free(g);
}

/* --------------------------------------------------------------- geometry */

void
roads_project(const roads_t *g, double lat, double lon, double *e, double *n)
{
    if (!g) {
        *e = *n = 0.0;
        return;
    }
    *e = (lon - g->lon0) * g->ke;
    *n = (lat - g->lat0) * g->klat;
}

void
roads_unproject(const roads_t *g, double e, double n, double *lat, double *lon)
{
    if (!g) {
        *lat = *lon = 0.0;
        return;
    }
    *lat = g->lat0 + n / g->klat;
    *lon = g->lon0 + e / g->ke;
}

void
roads_graph(const roads_t *g, roadgraph_t *out)
{
    if (!out)
        return;
    if (!g) {
        memset(out, 0, sizeof *out);
        return;
    }
    out->nnode = g->nnode;
    out->en = g->en;
    out->adj = g->adj;
    out->nedge = g->nedge;
    out->edge = g->edge;
}

double
roads_ref_lat(const roads_t *g)
{
    return g ? g->lat0 : 0.0;
}
double
roads_ref_lon(const roads_t *g)
{
    return g ? g->lon0 : 0.0;
}
int
roads_nnode(const roads_t *g)
{
    return g ? g->nnode : 0;
}
int
roads_nedge(const roads_t *g)
{
    return g ? g->nedge : 0;
}
int
roads_nplace(const roads_t *g)
{
    return g ? g->nplace : 0;
}
int
roads_ndropped(const roads_t *g)
{
    return g ? (int)g->ndropped : 0;
}
int
roads_honours_oneway(const roads_t *g)
{
    return g && (g->flags & FLAG_ONEWAY) ? 1 : 0;
}
const char *
roads_place_name(const roads_t *g, int i)
{
    if (!g || i < 0 || i >= g->nplace)
        return NULL;
    return g->str + g->pname[i];
}

/* ----------------------------------------------------------------- search */

const char *
search_compass8(double bearing)
{
    static const char *const P[8] = {"N", "NE", "E", "SE",
                                     "S", "SW", "W", "NW"};
    double deg = bearing * (180.0 / M_PI) + 22.5;
    int k;
    /* Python's % on a float is always non-negative for a positive modulus;
     * C's fmod is not, hence the fold. */
    deg = fmod(deg, 360.0);
    if (deg < 0.0)
        deg += 360.0;
    k = (int)(deg / 45.0);
    if (k < 0)
        k = 0;
    if (k > 7)
        k = 7;
    return P[k];
}

/* `query.upper().split()`. Returns the token count, or -1 when a token is
 * longer than MAX_TOKEN -- which no street name contains either, so a query
 * that long has no matches and saying "none" is the right answer. */
static int
tokenise(const char *q, char tok[MAX_TOKENS][MAX_TOKEN])
{
    int n = 0;
    if (!q)
        return 0;
    while (*q && n < MAX_TOKENS) {
        int k = 0;
        while (*q && isspace((unsigned char)*q))
            q++;
        if (!*q)
            break;
        while (*q && !isspace((unsigned char)*q)) {
            if (k >= MAX_TOKEN - 1)
                return -1;
            tok[n][k++] = (char)toupper((unsigned char)*q);
            q++;
        }
        tok[n][k] = '\0';
        n++;
    }
    return n;
}

/* Python's tuple ordering on (dist, bearing, name): a < b. */
static int
hit_before(const place_t *a, const place_t *b)
{
    if (a->dist_m != b->dist_m)
        return a->dist_m < b->dist_m;
    if (a->bearing != b->bearing)
        return a->bearing < b->bearing;
    return strcmp(a->name, b->name) < 0;
}

/* 1.4's matching rule, on its own, so that a name which is NOT in the pack --
 * a saved place from the config (1.4.6) -- is filtered by exactly the same
 * test as one that is. Two implementations of "does this match" would drift on
 * the first day someone changed the rule in one of them. */
int
search_name_matches(const char *name, const char *query)
{
    char tok[MAX_TOKENS][MAX_TOKEN];
    int ntok, t;

    if (!name || !*name)
        return 0;
    ntok = tokenise(query, tok);
    if (ntok <= 0)
        return 0;
    for (t = 0; t < ntok; t++)
        if (!strstr(name, tok[t]))
            return 0;
    return 1;
}

int
search_places(const roads_t *g, const char *query, double from_e, double from_n,
              place_t *out, int max)
{
    char tok[MAX_TOKENS][MAX_TOKEN];
    int ntok, i, t, total = 0, held = 0;

    if (!g || max < 0)
        return 0;
    ntok = tokenise(query, tok);
    /* An empty query matches nothing -- which is the state the FIND page opens
     * in, and the reason its title bar reads "0 HITS" before a key is pressed
     * rather than listing the whole pack. */
    if (ntok <= 0)
        return 0;

    for (i = 0; i < g->nplace; i++) {
        const char *nm = g->str + g->pname[i];
        double best = -1.0, be = 0.0, bn = 0.0;
        unsigned int k;
        place_t h;

        /* The same test search_name_matches() exports, spelled inline only
         * because the tokens are already split and re-splitting them once per
         * name in the pack would be 29 546 needless tokenise() calls. The two
         * must agree; that they do is asserted in tests/test_search.c. */
        for (t = 0; t < ntok; t++)
            if (!strstr(nm, tok[t]))
                break;
        if (t < ntok)
            continue;
        for (k = 0; k < g->pcount[i]; k++) {
            unsigned int p = g->pfirst[i] + k;
            double d = hypot(g->pen[2 * p] - from_e, g->pen[2 * p + 1] - from_n);
            if (best < 0.0 || d < best) {
                best = d;
                be = g->pen[2 * p];
                bn = g->pen[2 * p + 1];
            }
        }
        if (best < 0.0)
            continue; /* a name with no coordinates cannot be navigated to */
        total++;
        if (max <= 0)
            continue;

        h.name = nm;
        h.dist_m = best;
        h.bearing = atan2(be - from_e, bn - from_n);
        h.e = be;
        h.n = bn;
        roads_unproject(g, be, bn, &h.lat, &h.lon);
        h.place = i;
        /* Insertion into a bounded, sorted list: the page shows four and the
         * caller asks for a handful, so ranking the whole pack would be a sort
         * of 4 000 names to throw away 3 996 of them. */
        if (held == max && !hit_before(&h, &out[held - 1]))
            continue;
        {
            int j = held < max ? held : max - 1;
            while (j > 0 && hit_before(&h, &out[j - 1])) {
                out[j] = out[j - 1];
                j--;
            }
            out[j] = h;
            if (held < max)
                held++;
        }
    }
    return total;
}

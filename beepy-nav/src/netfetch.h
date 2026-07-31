/* beepy-nav/src/netfetch.h -- fetching bytes without stopping the ride.
 *
 * The first thing in this program that reaches off the device, and the first
 * that can fail for a reason no fixture predicts. Both facts shape it more
 * than what it fetches does, which is why this module knows nothing about
 * routes, JSON or Valhalla -- it produces BYTES, and netroute.c makes sense of
 * them.
 *
 * WHY A CHILD PROCESS. A fetch takes about 1.4 seconds against the router
 * measured in the plan; the render loop runs at 8 Hz, so a blocking call would
 * drop eleven frames and freeze the display mid-corner. A child can be started,
 * ignored for a hundred frames and reaped when convenient -- and a server that
 * hangs cannot corrupt program state, because it is not in the program. That
 * last property is also what lets a timeout be a kill() rather than a hope.
 *
 * WHY THE COMMAND IS CONFIG. `fetch_cmd` is a shell string, not a compiled-in
 * curl invocation, and that single choice is what makes "no test touches the
 * network" STRUCTURAL rather than a promise:
 *
 *     fetch_cmd = curl -s -m 10 --max-filesize 1000000 ...   (the default)
 *     fetch_cmd = cat beepy-nav/tests/net/valhalla-bike.json (what tests set)
 *     fetch_cmd = sh -c 'sleep 1.5; cat ...'                 (a SLOW fetch,
 *                                                             deterministically)
 *
 * The URL and the request body reach the command through the ENVIRONMENT
 * ($BEEPY_URL, $BEEPY_BODY) and never by string interpolation. A URL from a
 * config file spliced into a shell line is a quoting bug waiting for the first
 * destination with an ampersand in it.
 *
 * Portable-ish C: POSIX fork/exec/waitpid. Not device-specific, so the host
 * lane compiles and tests it.
 */
#ifndef BEEPY_NAV_NETFETCH_H
#define BEEPY_NAV_NETFETCH_H

#include <sys/types.h>

#define NETFETCH_PATH 256
#define NETFETCH_WHY 96
/* A 43.8 km bicycle route measured 39 KB. A megabyte is 25x that and still
 * refuses a server having a bad day; the fetcher is told the same limit so the
 * refusal happens at the socket rather than after the disk fills. */
#define NETFETCH_MAX_BYTES 1000000
/* Long enough for a slow cellular round trip, short enough that a rider who
 * pressed ENTER has not concluded the program is dead. */
#define NETFETCH_TIMEOUT_S 10.0

enum {
    NF_IDLE = 0, /* nothing in flight                                     */
    NF_RUNNING,  /* a child is fetching; poll() again next frame          */
    NF_READY,    /* bytes are in `buf`, owned by the caller until _free()  */
    NF_FAILED    /* `why` says which of the many ways it went wrong        */
};

typedef struct {
    int state;
    pid_t pid;
    int fd;
    double t0;    /* monotonic seconds when the child was started */
    char path[NETFETCH_PATH];
    char body[NETFETCH_PATH];
    char *buf;    /* NF_READY only; NUL-terminated for the parser's sake */
    long len;
    char why[NETFETCH_WHY];
} netfetch_t;

/* Start a fetch. `body`, when non-NULL, is written to a temp file whose path
 * the command finds in $BEEPY_BODY -- Valhalla wants a JSON POST. Returns 0 on
 * a started child; -1 with `why` filled otherwise. Starting a fetch while one
 * is running is a caller error and returns -1 rather than leaking a child. */
int netfetch_start(netfetch_t *f, const char *cmd, const char *url,
                   const char *body);

/* Reap without blocking. Returns the new state.
 *
 * Takes no clock, and that is deliberate: the deadline is measured on
 * CLOCK_MONOTONIC inside, not on the ride clock. A timeout is a statement about
 * how long a server has really had, and in a `--no-pace` replay the ride clock
 * covers ten minutes in a few seconds -- which made the first slow-fetch test
 * time out after eleven milliseconds of real time. */
int netfetch_poll(netfetch_t *f);

/* Kill an in-flight child and drop everything. Safe on any state, and safe
 * from a signal handler's cleanup path, which is why it takes no locks and
 * allocates nothing. */
void netfetch_cancel(netfetch_t *f);

#endif /* BEEPY_NAV_NETFETCH_H */

/* beepy-nav/tests/test_netfetch.c -- the fetcher, and the eight ways it ends.
 *
 * No network. Every case here is a shell command that behaves like a fetcher
 * behaving badly, which is the point of `fetch_cmd` being config: the failure
 * modes that matter -- a server that hangs, one that returns nothing, one that
 * is not there -- are exactly the ones a real network cannot be asked to
 * produce on demand.
 *
 * The timeout case really does take NETFETCH_TIMEOUT_S seconds of wall clock.
 * It is the only slow test in the suite and it earns it: the deadline is the
 * one thing here that protects a rider from a program that has stopped.
 */
/* Strict ISO hides clock_gettime, CLOCK_MONOTONIC and usleep on glibc, and
 * macOS declares them anyway -- the same trap netfetch.c documents, walked
 * into a second time in the same afternoon because the fix went into the
 * module and not into its test. */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "netfetch.h"

static int failures;

static void
check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static double
wall(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* nanosleep and not usleep: usleep was REMOVED from POSIX.1-2008, so the
 * feature macro above hides it on glibc -- while macOS declares it regardless,
 * which is how a "clean" strict-ISO check on the Mac still produced a warning
 * on the device. The only compiler whose opinion counts here is that one. */
static void
nap_ms(long ms)
{
    struct timespec t;

    t.tv_sec = ms / 1000;
    t.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&t, NULL);
}

/* Drive one fetch to a conclusion, the way the ride loop does: poll, do
 * something else, poll again. `budget` bounds the test, not the fetch. */
static int
run(netfetch_t *f, const char *cmd, const char *body, double budget,
    int *polls)
{
    double t0 = wall();
    int st;

    *polls = 0;
    if (netfetch_start(f, cmd, "http://example.invalid/x", body) != 0)
        return NF_FAILED;
    do {
        nap_ms(5);
        st = netfetch_poll(f);
        (*polls)++;
    } while (st == NF_RUNNING && wall() - t0 < budget);
    return st;
}

int
main(void)
{
    netfetch_t f;
    int st, polls;

    memset(&f, 0, sizeof f);

    st = run(&f, "printf 'hello world'", NULL, 5.0, &polls);
    check(st == NF_READY, "a command that writes bytes succeeds");
    check(f.len == 11 && !strcmp(f.buf, "hello world"),
          "and the bytes are the ones it wrote, NUL-terminated");
    netfetch_cancel(&f);

    /* The claim the whole module exists for: a fetch that takes as long as a
     * real one does not block its caller. 1.5 s against a 5 ms poll is at
     * least 200 polls; a blocking implementation would show one. */
    st = run(&f, "sh -c 'sleep 1.5; printf SLOWBYTES'", NULL, 5.0, &polls);
    check(st == NF_READY, "a slow fetch still succeeds");
    check(polls > 100, "and the caller polled throughout instead of blocking");
    netfetch_cancel(&f);

    /* The request body and the URL reach the command through the environment,
     * never spliced into the shell line -- a destination with an ampersand in
     * its name would otherwise become two commands. */
    st = run(&f, "sh -c 'cat \"$BEEPY_BODY\"'", "{\"q\":1}", 5.0, &polls);
    check(st == NF_READY && f.buf && !strcmp(f.buf, "{\"q\":1}"),
          "the request body reaches the command by $BEEPY_BODY");
    netfetch_cancel(&f);

    st = run(&f, "sh -c 'printf %s \"$BEEPY_URL\"'", NULL, 5.0, &polls);
    check(st == NF_READY && f.buf &&
          !strcmp(f.buf, "http://example.invalid/x"),
          "and the URL by $BEEPY_URL, unquoted and unmangled");
    netfetch_cancel(&f);

    st = run(&f, "sh -c 'exit 7'", NULL, 5.0, &polls);
    check(st == NF_FAILED, "a non-zero exit fails");
    netfetch_cancel(&f);

    /* 200 OK with an empty body is what a captive portal and a rate limiter
     * both look like from here. It is a failure, not an empty route. */
    st = run(&f, "true", NULL, 5.0, &polls);
    check(st == NF_FAILED && strstr(f.why, "empty") != NULL,
          "an empty reply fails, and says which kind of nothing it got");
    netfetch_cancel(&f);

    st = run(&f, "definitely-not-a-command-1234", NULL, 5.0, &polls);
    check(st == NF_FAILED, "a missing fetcher fails rather than hangs");
    netfetch_cancel(&f);

    st = run(&f, "", NULL, 5.0, &polls);
    check(st == NF_FAILED, "and so does an empty fetch_cmd");
    netfetch_cancel(&f);

    /* The slow one. A server that accepts the connection and then says nothing
     * is the failure a rider actually meets, and the only defence is a clock. */
    {
        double t0 = wall(), took;
        st = run(&f, "sleep 60", NULL, NETFETCH_TIMEOUT_S + 5.0, &polls);
        took = wall() - t0;
        check(st == NF_FAILED && strstr(f.why, "timed out") != NULL,
              "a fetcher that never answers times out");
        check(took >= NETFETCH_TIMEOUT_S && took < NETFETCH_TIMEOUT_S + 2.0,
              "at the deadline, not before it and not much after");
        printf("    T-NETFETCH-TIMEOUT: killed after %.1f s (limit %.0f)\n",
               took, NETFETCH_TIMEOUT_S);
        netfetch_cancel(&f);
    }

    if (failures) {
        printf("test_netfetch: %d FAILURES\n", failures);
        return 1;
    }
    printf("test_netfetch: OK\n");
    return 0;
}

/* libbeepyfb/tick.h -- a monotonic clock and a sleep, shared.
 *
 * An honest note about where this lives: timing has nothing to do with a
 * framebuffer, and putting it in libbeepyfb makes the library's name slightly
 * untrue. The alternative is a fourth top-level directory for two functions,
 * which is worse. mono_now() and sleep_s() are static in nav.c:1618-1638,
 * netfetch.c:38 has a third clock_gettime, and beepy-vid would be the fourth
 * copy; one of them documents the _GNU_SOURCE wrinkle and the others do not.
 *
 * Header-only static inline, so this adds no object to FB_OBJS.
 *
 * THE INCLUDING .c MUST DEFINE _GNU_SOURCE (or _POSIX_C_SOURCE >= 199309L)
 * before its includes. The build is -std=c11, strict ISO, and glibc hides
 * clock_gettime, nanosleep and CLOCK_MONOTONIC behind that; macOS libc
 * declares them regardless, so omitting it builds clean on the Mac and fails
 * on the device. netfetch.c:3-9 documents the same trap. Defining it here
 * instead would be worse: a header cannot know it is the first thing to pull
 * in <time.h>, and if it is not, the macro arrives too late to matter.
 */
#ifndef BEEPYFB_TICK_H
#define BEEPYFB_TICK_H

#include <time.h>

/* CLOCK_MONOTONIC, so a clock step during a ride or a film cannot make time
 * run backwards. Seconds, with the fraction -- a double holds a monotonic
 * clock to well under a microsecond for the ~50 days a Beepy stays up. */
static inline double
mono_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static inline void
sleep_s(double s)
{
    struct timespec ts;
    if (s <= 0)
        return;
    ts.tv_sec = (time_t)s;
    ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
    nanosleep(&ts, NULL);
}

static inline void
sleep_until(double due)
{
    sleep_s(due - mono_now());
}

#endif /* BEEPYFB_TICK_H */

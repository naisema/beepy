/* beepy-vid -- play a pre-dithered 1-bit video pack on the Beepy panel.
 *
 *   beepy-vid FILM.vid
 *   beepy-vid --demo --page play --pack FILM.vid --at 12 --dump out.fb
 *
 * See beepy-vid/DESIGN.md. The short version of what governs this file:
 *
 *  - The panel is bandwidth limited, not refresh limited:
 *    panel_fps(rows) = 420000 / (52*rows + 2), so 33.1 fps full screen and
 *    35.9 for the 400x225 stage. 24 fps needs no back-pressure; above that it
 *    does (DESIGN.md 0.1, 4.3).
 *  - NEVER flood. 150 frames written back to back produced FOUR panel
 *    updates: damage-merge discards the rest. A player that renders as fast
 *    as it can displays a slideshow.
 *  - Never resubmit an unchanged frame. A memory LCD holds its image with no
 *    power and no bus traffic, so a repeat costs literally nothing -- unless
 *    you write it, which costs a full transfer for no visible change.
 *  - The evdev grab is the lock, and it must be honoured (DESIGN.md 8.7).
 *
 * Portable end to end: fbdev and evdev are behind VID_DEVICE, so --demo runs
 * on the Mac and the design gate is a fast loop. Audio, the library page and
 * the transient/paused OSD states are M6/M7; this is the skeleton.
 */
/* Before every include, and it has to be: the build is -std=c11, which is
 * STRICT ISO, and under strict ISO glibc hides kill() and CLOCK_MONOTONIC
 * behind this. macOS libc declares them regardless, so this file compiled
 * clean in the host lane and would not build on the device -- exactly the trap
 * netfetch.c:3-9 already documents, and the reason `make check` runs there. */
#define _GNU_SOURCE

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#define VID_DEVICE 1
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include "libbeepyfb/fbdev.h"
#include "libbeepyfb/input.h"
#endif

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/dump.h"
#include "libbeepyfb/tick.h"

#include "pack.h"
#include "view_play.h"

static const char USAGE[] =
"usage: beepy-vid FILM.vid [--fps N] [--start SECONDS]\n"
"       beepy-vid --demo --page play [--pack FILE | --no-pack] [--at N]\n"
"                 [--paused] --dump FILE\n"
"\n"
"  --demo        render one frame and dump it; no panel, no keyboard\n"
"  --at N        which frame the demo renders\n"
"  --paused      demo the paused band\n"
"  --no-pack     demo the no-pack page; an explicit fixture, never the\n"
"                absence of a flag, so the test cannot read a real film\n"
"  --dump FILE   write the 384000-byte XRGB frame and exit\n";

/* ------------------------------------------------------------- signals */

#if defined(VID_DEVICE)
/* A film runs unattended for twenty minutes, which is exactly when an ssh
 * session drops. fbterm is SIGSTOPped while we own the panel and a missed
 * SIGCONT leaves the device looking dead (CLAUDE.md), so the pids are captured
 * up front and the handler is async-signal-safe: kill() and _exit() only.
 * fb_release() must NOT be called here -- it ends in system() for the tmux
 * nudge (fbdev.c:95), and system() is not async-signal-safe.
 *
 * nav.c installs SIGINT and SIGTERM. SIGHUP is added because beepy-nav/fbanim
 * traps it and nav.c does not: the shell wrapper is more careful than the C
 * program, and an ssh drop sends HUP. */
static volatile sig_atomic_t g_paused_pids[8];
static volatile sig_atomic_t g_npaused;
static volatile sig_atomic_t g_quit;

static void
on_fatal(int sig)
{
    for (int i = 0; i < (int)g_npaused; i++)
        kill((pid_t)g_paused_pids[i], SIGCONT);
    signal(sig, SIG_DFL);
    raise(sig);
}

static void
on_quit(int sig)
{
    (void)sig;
    g_quit = 1;
}
#endif

/* ------------------------------------------------------------ playback */

typedef struct {
    pack_t pack;
    int have_pack;
    canvas_t *cv;          /* what is presented: video plus the band */
    unsigned char *plane;  /* the decode target and the XOR reference */
    uint32_t frame;        /* next frame to present */
    double fps;
    int paused, ended;
} player_t;

static double
frame_seconds(const player_t *p)
{
    return (double)p->pack.fps_den / (double)p->pack.fps_num;
}

static double
pack_seconds(const player_t *p)
{
    return p->pack.nframe * frame_seconds(p);
}

/* Decode up to and including frame i, walking from the nearest keyframe when
 * the chain cannot be continued. A delta frame is the next frame's premise, so
 * intermediates are decoded but not presented. */
static int
decode_to(player_t *p, uint32_t i, uint32_t from_known)
{
    long k;
    uint32_t s;

    if (i >= p->pack.nframe)
        return -1;
    if (from_known != (uint32_t)-1 && i > from_known && i - from_known <= p->pack.gop)
        s = from_known + 1;
    else {
        k = pack_keyframe_before(&p->pack, i);
        if (k < 0)
            return -1;
        s = (uint32_t)k;
    }
    for (uint32_t j = s; j <= i; j++)
        if (pack_frame(&p->pack, j, p->plane) < 0) {
            fprintf(stderr, "beepy-vid: frame %u: %s\n", j, p->pack.err);
            return -1;
        }
    return 0;
}

static void
compose(player_t *p, canvas_t *cv)
{
    osd_t o;
    memset(&o, 0, sizeof o);
    o.paused = p->paused;
    o.ended = p->ended;
    o.nopack = !p->have_pack;
    if (p->have_pack) {
        o.t = p->frame * frame_seconds(p);
        o.total = pack_seconds(p);
        memcpy(cv->bits, p->plane, p->pack.plane_bytes);
    }
    view_play(cv, &o);
}

/* ----------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
    player_t P;
    const char *path = NULL, *dump = NULL, *page = "play";
    int demo = 0, nopack = 0, at = 0, paused = 0;
    double start = 0, fps_override = 0;

    memset(&P, 0, sizeof P);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--demo"))
            demo = 1;
        else if (!strcmp(a, "--no-pack"))
            nopack = 1;
        else if (!strcmp(a, "--paused"))
            paused = 1;
        else if (!strcmp(a, "--page") && i + 1 < argc)
            page = argv[++i];
        else if (!strcmp(a, "--pack") && i + 1 < argc)
            path = argv[++i];
        else if (!strcmp(a, "--dump") && i + 1 < argc)
            dump = argv[++i];
        else if (!strcmp(a, "--at") && i + 1 < argc)
            at = atoi(argv[++i]);
        else if (!strcmp(a, "--fps") && i + 1 < argc)
            fps_override = atof(argv[++i]);
        else if (!strcmp(a, "--start") && i + 1 < argc)
            start = atof(argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(USAGE, stdout);
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "beepy-vid: unknown option %s\n\n%s", a, USAGE);
            return 2;
        } else
            path = a;
    }
    if (strcmp(page, "play") != 0) {
        fprintf(stderr, "beepy-vid: unknown page %s\n", page);
        return 2;
    }

    if (!nopack && path) {
        if (pack_open(&P.pack, path) != 0) {
            /* Refusing at open is the contract: a pack the device cannot read
             * is indistinguishable from one it can until it is opened. In demo
             * mode fall through to the no-pack page so an unreadable pack and
             * an absent one produce the SAME frame, which is what the gate
             * compares. */
            fprintf(stderr, "beepy-vid: %s: %s\n", path, P.pack.err);
            pack_close(&P.pack);
            if (!demo)
                return 1;
        } else {
            P.have_pack = 1;
        }
    }

    P.cv = canvas_new(SCR_W, SCR_H);
    if (!P.cv)
        return 1;
    canvas_clear(P.cv, PAPER);
    if (P.have_pack) {
        P.plane = calloc(1, P.pack.plane_bytes);
        if (!P.plane)
            return 1;
        P.fps = fps_override > 0 ? fps_override
                                 : (double)P.pack.fps_num / P.pack.fps_den;
    }

    /* ----------------------------------------------------------- demo */
    if (demo) {
        if (!dump) {
            fprintf(stderr, "beepy-vid: --demo needs --dump FILE\n");
            return 2;
        }
        P.paused = paused;
        if (P.have_pack) {
            if (at < 0 || (uint32_t)at >= P.pack.nframe) {
                fprintf(stderr, "beepy-vid: --at %d is outside 0..%u\n",
                        at, P.pack.nframe - 1);
                return 2;
            }
            if (decode_to(&P, (uint32_t)at, (uint32_t)-1) != 0)
                return 1;
            P.frame = (uint32_t)at;
        }
        compose(&P, P.cv);
        if (canvas_dump(P.cv, dump) != 0) {
            fprintf(stderr, "beepy-vid: cannot write %s\n", dump);
            return 1;
        }
        printf("beepy-vid: wrote %s\n", dump);
        pack_close(&P.pack);
        return 0;
    }

    if (!P.have_pack) {
        fputs(USAGE, stderr);
        return 2;
    }

#if !defined(VID_DEVICE)
    (void)start; /* only the device path seeks; --demo uses --at */
    fprintf(stderr, "beepy-vid: playback needs the device; use --demo here\n");
    return 2;
#else
    {
        fb_t fb;
        struct pollfd pfd[MAX_EVDEV];
        uint32_t last_shown = (uint32_t)-1;
        double t0;
        int rc = 0;

        /* The grab BEFORE the panel, and refuse if it fails.
         *
         * input.h:29-31 states it: a reader that loses the grab "receives no
         * events at all while the holder runs". If beepy-nav is running it
         * already holds event0, so taking the panel anyway would draw over
         * nav's frames and accept no keys -- unresponsive and unquittable from
         * the panel. nav.c's ride path ignores this deliberately; a film
         * player must not. */
        evdev_open(1);
        if (evdev_count() == 0) {
            fprintf(stderr, "beepy-vid: no keyboard found\n");
            return 1;
        }
        if (evdev_grab_failed()) {
            fprintf(stderr, "beepy-vid: the keyboard is held by another program "
                            "(beepy-nav?); refusing to take the panel\n");
            evdev_close();
            return 1;
        }
        if (fb_open(&fb, "/dev/fb1") < 0) {
            evdev_close();
            return 1;
        }
        fb_take(&fb);
        for (int i = 0; i < fb.npaused && i < 8; i++)
            g_paused_pids[i] = fb.paused[i];
        g_npaused = fb.npaused < 8 ? fb.npaused : 8;

        signal(SIGINT, on_quit);
        signal(SIGTERM, on_quit);
        signal(SIGHUP, on_quit);
        signal(SIGSEGV, on_fatal);
        signal(SIGBUS, on_fatal);
        signal(SIGFPE, on_fatal);
        signal(SIGILL, on_fatal);
        signal(SIGABRT, on_fatal);

        P.frame = (uint32_t)(start / frame_seconds(&P));
        if (P.frame >= P.pack.nframe)
            P.frame = 0;
        if (decode_to(&P, P.frame, (uint32_t)-1) != 0)
            rc = 1;
        t0 = mono_now() - P.frame / P.fps;

        while (!rc && !g_quit) {
            double now = mono_now();
            uint32_t due;

            if (P.paused || P.ended)
                due = P.frame;
            else {
                double el = now - t0;
                long d = (long)(el * P.fps);
                if (d < 0)
                    d = 0;
                due = (uint32_t)d;
                if (due >= P.pack.nframe) {
                    due = P.pack.nframe - 1;
                    P.ended = 1;
                }
            }

            if (due != last_shown) {
                if (due != P.frame) {
                    if (decode_to(&P, due, P.frame) != 0) {
                        rc = 1;
                        break;
                    }
                    P.frame = due;
                }
                compose(&P, P.cv);
                /* Whole frame: the band changes every second and the stage
                 * every frame, so a row-range present would cover almost
                 * everything anyway. Narrowing it is M5's job, with the
                 * pack's own y0/y1 to drive it. */
                if (fb_present(&fb, P.cv) != 0) {
                    fprintf(stderr, "beepy-vid: present failed\n");
                    rc = 1;
                    break;
                }
                last_shown = due;
            }

            /* poll() with a computed timeout, capped at one frame period so a
             * keypress is seen within 42 ms at 24 fps -- under the ~100 ms
             * where a key stops feeling immediate. */
            {
                int n = evdev_count();
                double next = P.paused || P.ended
                                  ? 0.100
                                  : (t0 + (due + 1) / P.fps) - mono_now();
                int ms = (int)(next * 1000);
                if (ms < 0)
                    ms = 0;
                if (ms > 100)
                    ms = 100;
                for (int i = 0; i < n; i++) {
                    pfd[i].fd = evdev_fd(i);
                    pfd[i].events = POLLIN;
                    pfd[i].revents = 0;
                }
                if (poll(pfd, n, ms) > 0) {
                    for (int i = 0; i < n; i++) {
                        int code, val;
                        if (!(pfd[i].revents & POLLIN))
                            continue;
                        while (evdev_next_key(pfd[i].fd, &code, &val)) {
                            if (val == 0)
                                continue;
                            switch (code) {
                            case KEY_Q:
                            case KEY_ESC:
                                g_quit = 1;
                                break;
                            case KEY_SPACE:
                                if (P.ended)
                                    break;
                                P.paused = !P.paused;
                                if (!P.paused)
                                    t0 = mono_now() - P.frame / P.fps;
                                last_shown = (uint32_t)-1; /* redraw the band */
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
            }
        }

        fb_release(&fb);
        evdev_close();
        pack_close(&P.pack);
        return rc;
    }
#endif
}

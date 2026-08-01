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
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <unistd.h>

#include "libbeepyfb/fbdev.h"
#include "libbeepyfb/input.h"
#endif

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/dump.h"
#include "libbeepyfb/tick.h"

#include "audio.h"
#include "pack.h"
#include "view_pages.h"
#include "view_play.h"

/* The library demo's fixture, built here rather than scanned from a
 * directory.
 *
 * CLAUDE.md's rule, learned four times: "A test whose absent case reads the
 * device config is not a test." A --page library that listed ~/videos would
 * render whatever happens to be on the machine, so the golden would be a
 * photograph of one device and would move whenever a file was added. These
 * five entries are the fixture, and they are chosen to exercise what the page
 * has to get right: a long name, a short one, a never-opened pack (which must
 * show nothing rather than 0%), a part-watched one, and an unreadable one. */
static lib_item_t DEMO_ITEMS[] = {
    { "BIG BUCK BUNNY",        596.5, 24, 1, 1, 148000000, 62, 1 },
    { "THE RED BALLOON",      2052.0, 24, 1, 1,  91000000, -1, 1 },
    { "TRAIN ARRIVING",         49.0, 24, 1, 0,   4000000,  0, 1 },
    { "LECTURE 04 DITHERING",  3600.0, 12, 1, 1, 210000000, 88, 1 },
    { "WEDDING RAW",           420.0, 24, 1, 1,  60000000, -1, 0 },
};

static const char USAGE[] =
"usage: beepy-vid FILM.vid [--fps N] [--start SECONDS]\n"
"       beepy-vid FILM.vid --headless [--trace-frames T] [--stall-at N:MS]\n"
"       beepy-vid --demo --page play [--pack FILE | --no-pack] [--at N]\n"
"                 [--paused] [--transient TEXT] --dump FILE\n"
"       beepy-vid --demo --page library|library-empty|end|help|sync\n"
"                 --dump FILE\n"
"\n"
"  --demo        render one frame and dump it; no panel, no keyboard\n"
"  --at N        which frame the demo renders\n"
"  --paused      demo the paused band\n"
"  --no-pack     demo the no-pack page; an explicit fixture, never the\n"
"                absence of a flag, so the test cannot read a real film\n"
"  --dump FILE   write the 384000-byte XRGB frame and exit\n"
"  --headless    schedule and decode, present nothing; runs to the end\n"
"  --trace-frames T   one TSV row per scheduling decision\n"
"  --stall-at N:MS    inject a stall before frame N, so the drop count is\n"
"                arithmetic rather than a measurement of the machine\n"
"  --audio-cmd CMD    sink to feed; %r and %c become rate and channels.\n"
"                A command and not a linked library, so a test can\n"
"                substitute a fake sink and the gate never touches a\n"
"                speaker (see audio.h)\n"
"  --no-audio    silent, and the clock falls back to CLOCK_MONOTONIC\n"
"  --av-offset MS     A2DP lag; positive delays the video to match\n"
"  --volume N    0..10, 10 is unity and the default. Attenuation only:\n"
"                above unity would clip, and cannot make a speaker louder\n"
"                than its own amplifier. While playing, the - and + keys\n"
"                (symbol layer) do the same thing\n";

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

/* Back-pressure, for rates above 24 fps.
 *
 * write() to /dev/fb1 returns into a shadow buffer and a workqueue transfers
 * later, so the syscall gives no indication that the panel has caught up.
 * Frames submitted while the previous transfer is still running are MERGED
 * AWAY: measured at 400x225, a plain nanosleep loop lands 100% of frames at
 * 24 fps and only 84.7% at 30, while waiting for the transfer lands 100% at
 * both (DESIGN.md 4.3). The cliff is a phase problem, not a bandwidth one.
 *
 * The kernel's own SPI message counter is the only signal available without
 * taking DRM master. Reading a small sysfs file per frame is ugly; it is also
 * exact, and it is how every panel number in DESIGN.md 0 was obtained.
 *
 * Returns -1 when the counter is unreadable, and the caller then simply does
 * not wait -- on a kernel that does not expose it, 24 fps still works.
 */
#define SPI_MSGS "/sys/class/spi_master/spi0/spi0.0/statistics/messages"

static long
spi_messages(void)
{
    char buf[32];
    ssize_t n;
    int fd = open(SPI_MSGS, O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = 0;
    return strtol(buf, NULL, 10);
}

/* Wait until the counter moves past `was`, or until the deadline. Bounded, so
 * a driver that stops counting stalls playback by one frame rather than
 * hanging the player. */
static void
spi_wait(long was, double deadline)
{
    if (was < 0)
        return;
    while (mono_now() < deadline) {
        if (spi_messages() != was)
            return;
    }
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

    /* pacing instrumentation */
    FILE *trace;
    long dropped;          /* cumulative, never reset */
    int stall_at;          /* frame index, -1 for none */
    double stall_s;
    const char *transient;
    /* A transient the LOOP owns, as opposed to the one --demo is handed. It
     * needs storage that outlives the keypress and a deadline, because unlike
     * the paused band it has nothing to hold it on screen. */
    char osd_msg[24];
    double osd_until;      /* mono seconds; 0 means no loop-owned transient */

    /* audio */
    audio_t au;
    const char *audio_cmd;
    double av_offset;      /* seconds; A2DP lag, positive delays the video */
    uint32_t audio_pos;    /* byte offset into the AUDIO section */
    int audio_done;        /* the track is fully fed; the sink is draining */
    double audio_end_t, audio_end_wall;
    double last_el;        /* the clock never runs backwards */
    unsigned char abuf[8192];
} player_t;

/* One row per scheduling decision, whether or not a frame was shown.
 *
 * presented and written are separate columns deliberately. A frame the
 * scheduler skipped and a frame that was identical to its predecessor both
 * produce "no SPI transfer", and conflating them would make the size==0 skip
 * indistinguishable from a missed deadline -- which is exactly how a pacing
 * bug hides. DESIGN.md 8.5.
 */
static void
trace_row(player_t *p, double t, uint32_t idx, int presented, int written)
{
    if (!p->trace)
        return;
    fprintf(p->trace, "%.6f\t%.6f\t%u\t%d\t%d\t%ld\n",
            t, idx * (p->have_pack ? (double)p->pack.fps_den / p->pack.fps_num
                                   : 0.0),
            idx, presented, written, p->dropped);
}

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
    o.transient = p->transient;
    if (p->have_pack) {
        o.t = p->frame * frame_seconds(p);
        o.total = pack_seconds(p);
        memcpy(cv->bits, p->plane, p->pack.plane_bytes);
    }
    view_play(cv, &o);
}

/* WHAT THE ALT LAYER ACTUALLY SENDS, which is not what it prints.
 *
 * There is no - or + key on this keyboard; both are on the physical alt layer.
 * beepy-kbd does NOT emit KEY_MINUS for alt+key. Its
 * map_phys_alt_keycode() (input_modifiers.c) does, in full:
 *
 *     keycode += 119; // See map file for result keys
 *
 * and beepy-kbd.map then assigns the shifted codes under "# Physical Alt key":
 *
 *     keycode 142 = minus
 *     keycode 143 = plus
 *
 * That map is a CONSOLE keymap, applied by the tty layer. A player that
 * EVIOCGRABs the device -- which this one must, see the grab argument in the
 * device path -- reads evdev directly and the translation never runs, so what
 * arrives is the bare 142 and 143.
 *
 * This shipped broken once: bound to KEY_MINUS and KEY_EQUAL, alt+key produced
 * no volume change and no error, because those codes are simply never sent by
 * this keyboard. KEY_MINUS is kept below for a USB keyboard, but it is the
 * fallback and these two are the real binding.
 *
 * Deliberately not spelled KEY_SLEEP and KEY_WAKEUP, which is what 142 and 143
 * are in input-event-codes.h. The names would be accurate about the constant
 * and a lie about the key: nothing here is asking the device to sleep. */
#define BEEPY_ALT_MINUS 142
#define BEEPY_ALT_PLUS  143

/* Keys exist only where there is a keyboard, and the host lane builds with
 * -Wall -Wextra: unguarded, this is an unused static there. */
#if defined(VID_DEVICE)
/* A volume change and the OSD that reports it.
 *
 * The report is not decoration. On this keyboard - and + are on the symbol
 * layer, so a press that produced nothing audible has two indistinguishable
 * explanations -- the key never arrived, or it arrived and the speaker ignores
 * the level -- and the second is a documented property of the VOX-DC here.
 * Naming the level on the panel separates them, and it is the only feedback
 * available: the band has no room for a permanent readout.
 *
 * A sink that is not open reports that instead of silently counting a level
 * nothing will ever apply. --no-audio and a missing speaker both land here.
 */
static void
volume_step(player_t *p, int delta, double now)
{
    if (!audio_ok(&p->au))
        snprintf(p->osd_msg, sizeof p->osd_msg, "NO AUDIO");
    else {
        int v = audio_set_vol(&p->au, audio_vol(&p->au) + delta);
        if (v == 0)
            snprintf(p->osd_msg, sizeof p->osd_msg, "MUTE");
        else
            snprintf(p->osd_msg, sizeof p->osd_msg, "VOLUME %d", v);
    }
    p->transient = p->osd_msg;
    /* Long enough to read after a burst of presses, short enough not to sit
     * over the film. The paused panel outranks it either way (view_play.c). */
    p->osd_until = now + 1.2;
}
#endif /* VID_DEVICE */

/* ----------------------------------------------------------------- main */

int
main(int argc, char **argv)
{
    player_t P;
    const char *path = NULL, *dump = NULL, *page = "play";
    int demo = 0, nopack = 0, at = 0, paused = 0, headless = 0;
    /* Held here rather than written straight into P.au, because audio_open()
     * memsets the struct and would discard a --volume parsed before it. */
    int vol = AUDIO_VOL_MAX;
    const char *transient = NULL;
    int noaudio = 0;
    double start = 0, fps_override = 0;
    const char *tracepath = NULL;

    memset(&P, 0, sizeof P);
    P.stall_at = -1;
    /* Not zero. audio_ok() tests fd >= 0, and a zeroed struct claims fd 0 --
     * standard input -- so a player started with --no-audio would feed the
     * sink through its own stdin and report that the sink went away. */
    P.au.fd = -1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--demo"))
            demo = 1;
        else if (!strcmp(a, "--no-pack"))
            nopack = 1;
        else if (!strcmp(a, "--paused"))
            paused = 1;
        else if (!strcmp(a, "--transient") && i + 1 < argc)
            transient = argv[++i];
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
        else if (!strcmp(a, "--headless"))
            headless = 1;
        else if (!strcmp(a, "--audio-cmd") && i + 1 < argc)
            P.audio_cmd = argv[++i];
        else if (!strcmp(a, "--no-audio"))
            P.audio_cmd = NULL, noaudio = 1;
        else if (!strcmp(a, "--av-offset") && i + 1 < argc)
            P.av_offset = atof(argv[++i]) / 1000.0;
        else if (!strcmp(a, "--volume") && i + 1 < argc)
            vol = atoi(argv[++i]);
        else if (!strcmp(a, "--trace-frames") && i + 1 < argc)
            tracepath = argv[++i];
        else if (!strcmp(a, "--stall-at") && i + 1 < argc) {
            /* N:MS -- a synthetic stall before frame N. See the loop. */
            const char *v = argv[++i];
            const char *c = strchr(v, ':');
            if (!c) {
                fprintf(stderr, "beepy-vid: --stall-at wants FRAME:MS\n");
                return 2;
            }
            P.stall_at = atoi(v);
            P.stall_s = atof(c + 1) / 1000.0;
        }
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(USAGE, stdout);
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "beepy-vid: unknown option %s\n\n%s", a, USAGE);
            return 2;
        } else
            path = a;
    }
    /* Pages that need no pack at all are rendered and dumped here, before any
     * of the playback machinery is set up. */
    if (demo && dump && strcmp(page, "play") != 0) {
        canvas_t *cv = canvas_new(SCR_W, SCR_H);
        lib_t L;
        if (!cv)
            return 1;
        canvas_clear(cv, PAPER);
        memset(&L, 0, sizeof L);
        L.dir = "/HOME/BEEPY/VIDEOS";
        if (!strcmp(page, "library")) {
            L.items = DEMO_ITEMS;
            L.n = (int)(sizeof DEMO_ITEMS / sizeof DEMO_ITEMS[0]);
            L.sel = 1; /* not row 0: a selection bar drawn at a fixed row
                        * would look identical whether or not it tracks sel */
            view_library(cv, &L);
        } else if (!strcmp(page, "library-empty")) {
            view_library(cv, &L);
        } else if (!strcmp(page, "end")) {
            view_end(cv, "THE RED BALLOON", 2052.0, 1, 5);
        } else if (!strcmp(page, "help")) {
            view_help(cv);
        } else if (!strcmp(page, "sync")) {
            /* An explicit fixture again: a real sink name and a real reported
             * latency would make the golden a photograph of one speaker. */
            view_sync(cv, "SPOTLESS D1", 210, 42, 1);
        } else {
            fprintf(stderr, "beepy-vid: unknown page %s\n", page);
            return 2;
        }
        if (canvas_dump(cv, dump) != 0) {
            fprintf(stderr, "beepy-vid: cannot write %s\n", dump);
            return 1;
        }
        printf("beepy-vid: wrote %s\n", dump);
        return 0;
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

    if (tracepath) {
        P.trace = fopen(tracepath, "w");
        if (!P.trace) {
            fprintf(stderr, "beepy-vid: cannot write %s\n", tracepath);
            return 1;
        }
        /* The '#' is load-bearing: tools/assert_trace.py:73-86 takes the
         * commented line as the column names and treats everything else as
         * data. Without it the loader has no names and every row is a
         * TypeError. */
        fputs("#t\tpts\tidx\tpresented\twritten\tdropped\n", P.trace);
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
        P.transient = transient;
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

    /* ------------------------------------------------------- headless
     *
     * The scheduler is portable and this is what makes it testable: the same
     * loop, the same clock arithmetic, the same drop policy, with the panel
     * and the keyboard removed. Pacing assertions therefore run in the Mac
     * lane instead of costing a 25-minute device round trip, which is the
     * split DESIGN.md 8.1 requires. Runs to the end of the pack and stops, so
     * a test terminates without needing a key.
     */
    if (headless) {
        double t0 = mono_now() - P.frame / P.fps;
        uint32_t last_shown = (uint32_t)-1;

        if (!noaudio && P.audio_cmd && P.pack.audio_bytes &&
            audio_open(&P.au, P.audio_cmd, (int)P.pack.audio_rate,
                       P.pack.audio_ch, P.pack.audio_bits) != 0)
            return 1;
        audio_set_vol(&P.au, vol);

        for (;;) {
            double now = mono_now(), clock;
            long d;
            uint32_t due;
            int written = 0;

            /* Feed whatever the sink will take. In the steady state this
             * accepts nothing at all, which is the back-pressure the clock is
             * read from -- not a failure. */
            if (audio_ok(&P.au) && !P.audio_done) {
                long got = pack_audio(&P.pack, P.audio_pos, P.abuf,
                                      sizeof P.abuf);
                if (got > 0) {
                    long w;
                    audio_apply_vol(&P.au, P.abuf, (size_t)got);
                    w = audio_feed(&P.au, P.abuf, (size_t)got);
                    if (w < 0) {
                        fprintf(stderr, "beepy-vid: the audio sink went away\n");
                        return 1;
                    }
                    P.audio_pos += (uint32_t)w;
                } else {
                    /* The track is fully handed over. From here the sink is
                     * draining what it already holds and accepts nothing more,
                     * so bytes-accepted STOPS ADVANCING -- and a clock derived
                     * from it stops with it. Freezing the picture on the last
                     * frame while the sink still has 150-250 ms to play is
                     * wrong, and looping on a clock that will never reach the
                     * end is worse: it hangs. Latch the audio position and run
                     * the remainder on the monotonic clock, which is exactly
                     * as accurate as the sink's remaining buffer is long. */
                    P.audio_done = 1;
                    P.audio_end_t = audio_time(&P.au, P.av_offset);
                    P.audio_end_wall = now;
                }
            }

            /* AUDIO IS MASTER. Video is scheduled from the sink's own
             * consumption, never from a free-running timer: the two drift by
             * seconds over a feature film, and the drift is the failure that
             * ships because a 30-second test cannot see it. With no audio the
             * fallback is the monotonic clock and the arithmetic below is
             * unchanged. */
            if (P.audio_done)
                clock = P.audio_end_t + (now - P.audio_end_wall);
            else if (audio_ok(&P.au) && audio_primed(&P.au))
                clock = audio_clock(&P.au, P.av_offset, now);
            else
                clock = now - t0;
            if (clock < P.last_el)
                clock = P.last_el;
            P.last_el = clock;

            d = (long)(clock * P.fps);
            if (d < 0)
                d = 0;
            if ((uint32_t)d >= P.pack.nframe) {
                trace_row(&P, now - t0, P.frame, 0, 0);
                break;
            }
            due = (uint32_t)d;

            {
                int presented = (due != last_shown);
                if (presented) {
                    /* Frames between the last shown and the one now due are
                     * dropped, not shown late: they are already in the past,
                     * and showing them would be a burst of motion that never
                     * happened followed by a picture ahead of its sound. */
                    if (due > P.frame + 1)
                        P.dropped += (long)(due - P.frame - 1);
                    if (due != P.frame) {
                        if (decode_to(&P, due, P.frame) != 0)
                            return 1;
                        P.frame = due;
                    }
                    compose(&P, P.cv);
                    written = 1;
                    last_shown = due;
                }
                trace_row(&P, now - t0, due, presented, written);
            }

            /* The injected stall. A real CPU hog on a 2-core Pi Zero is a
             * flaky test and this repo has paid for flaky timing four times
             * (Makefile:978); a synthetic stall makes the answer arithmetic
             * with exactly one right value. */
            if (P.stall_at >= 0 && due == (uint32_t)P.stall_at) {
                sleep_s(P.stall_s);
                P.stall_at = -1;
            }
            if (audio_ok(&P.au) && !P.audio_done)
                sleep_s(0.002); /* the sink sets the pace; just do not spin */
            else if (P.audio_done)
                sleep_until(P.audio_end_wall + ((due + 1) / P.fps - P.audio_end_t));
            else
                sleep_until(t0 + (due + 1) / P.fps);
        }
        audio_close(&P.au);
        if (P.trace)
            fclose(P.trace);
        pack_close(&P.pack);
        return 0;
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
        /* Only above 24 fps: at or below it the measurement says a plain
         * paced loop lands every frame, and a sysfs read per frame is not
         * free. */
        int backpressure = (P.fps > 24.5) && (spi_messages() >= 0);

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

        /* The sink, on the device path too. This was missing until a listener
         * pointed out that the film was silent: audio was wired into the
         * headless loop only, so test-vidsync -- which runs headless by
         * construction -- passed against a player that had no audio at all on
         * the panel. A gate that exercises a different loop from the one that
         * ships is not a gate for the one that ships. */
        if (!noaudio && P.audio_cmd && P.pack.audio_bytes &&
            audio_open(&P.au, P.audio_cmd, (int)P.pack.audio_rate,
                       P.pack.audio_ch, P.pack.audio_bits) != 0) {
            fb_release(&fb);
            evdev_close();
            return 1;
        }
        audio_set_vol(&P.au, vol);

        P.frame = (uint32_t)(start / frame_seconds(&P));
        if (P.frame >= P.pack.nframe)
            P.frame = 0;
        if (decode_to(&P, P.frame, (uint32_t)-1) != 0)
            rc = 1;
        t0 = mono_now() - P.frame / P.fps;

        while (!rc && !g_quit) {
            double now = mono_now();
            uint32_t due;

            /* Retire the loop-owned transient. Clearing last_shown is what
             * actually removes it: the panel only changes when a frame is
             * recomposed, so an expired message with no redraw behind it would
             * stay on screen until the next frame happened to differ -- which,
             * paused, is never. */
            if (P.osd_until > 0.0 && now >= P.osd_until) {
                P.osd_until = 0.0;
                P.transient = NULL;
                last_shown = (uint32_t)-1;
            }

            /* Hand the sink whatever it will take. In the steady state this
             * accepts nothing, which IS the back-pressure the clock reads. */
            if (audio_ok(&P.au) && !P.audio_done && !P.paused) {
                long got = pack_audio(&P.pack, P.audio_pos, P.abuf,
                                      sizeof P.abuf);
                if (got > 0) {
                    long w;
                    audio_apply_vol(&P.au, P.abuf, (size_t)got);
                    w = audio_feed(&P.au, P.abuf, (size_t)got);
                    if (w > 0)
                        P.audio_pos += (uint32_t)w;
                } else {
                    P.audio_done = 1;
                    P.audio_end_t = audio_clock(&P.au, P.av_offset, now);
                    P.audio_end_wall = now;
                }
            }

            if (P.paused || P.ended)
                due = P.frame;
            else {
                double el;
                long d;
                /* Audio is master wherever there is audio. */
                if (P.audio_done)
                    el = P.audio_end_t + (now - P.audio_end_wall);
                else if (audio_ok(&P.au) && audio_primed(&P.au))
                    el = audio_clock(&P.au, P.av_offset, now);
                else
                    el = now - t0;
                /* The hand-over from the monotonic clock to the audio clock is
                 * a discontinuity, and it was measured stepping the film two
                 * frames BACKWARDS. Time in a film only goes forwards. */
                if (el < P.last_el)
                    el = P.last_el;
                P.last_el = el;
                d = (long)(el * P.fps);
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
                if (due > P.frame + 1)
                    P.dropped += (long)(due - P.frame - 1);
                compose(&P, P.cv);
                /* Whole frame: the band changes every second and the stage
                 * every frame, so a row-range present would cover almost
                 * everything anyway. Narrowing it wants the pack's own y0/y1
                 * unioned with the band, and is left for when the OSD gains
                 * its transient states. */
                {
                    long was = backpressure ? spi_messages() : -1;
                    if (fb_present(&fb, P.cv) != 0) {
                        fprintf(stderr, "beepy-vid: present failed\n");
                        rc = 1;
                        break;
                    }
                    /* Above 24 fps, submitting the next frame before this
                     * transfer completes merges it away. Wait, bounded by one
                     * frame period so a silent counter costs a frame and not
                     * the playback. */
                    spi_wait(was, mono_now() + 1.0 / P.fps);
                }
                trace_row(&P, mono_now() - t0, due, 1, 1);
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
                /* That deadline is on the MONOTONIC schedule, and with audio
                 * the clock is the sink's instead -- so sleeping to it walks
                 * straight past frame boundaries the audio clock has not
                 * reached yet, and the loop then finds itself two frames late
                 * and drops one. Measured: 125 of 240 frames presented, at the
                 * right total duration. There is no way to ask a pipe when it
                 * will next accept, so the answer is to sample often. */
                if (audio_ok(&P.au) && !P.audio_done && ms > 5)
                    ms = 5;
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
                                if (!P.paused) {
                                    t0 = mono_now() - P.frame / P.fps;
                                    /* Re-prime: the sink still holds audio
                                     * from before the pause and would play it
                                     * after the picture moved. */
                                    if (audio_ok(&P.au)) {
                                        audio_reprime(&P.au, P.audio_cmd);
                                        P.audio_pos = (uint32_t)
                                            (P.frame * frame_seconds(&P) *
                                             P.pack.audio_rate) *
                                            P.pack.audio_ch * 2;
                                        if (P.audio_pos > P.pack.audio_bytes)
                                            P.audio_pos = P.pack.audio_bytes;
                                        P.audio_done = 0;
                                    }
                                }
                                last_shown = (uint32_t)-1; /* redraw the band */
                                break;
                            /* - and + only, and NOT the arrows.
                             *
                             * The arrows were the tempting second binding:
                             * they are dedicated physical keys, where - and +
                             * sit on this keyboard's symbol layer. But
                             * view_pages.c's help page already allocates DOWN
                             * and UP to SEEK 60 SEC and LEFT and RIGHT to SEEK
                             * 10 SEC. That page is a specification the player
                             * has not caught up with yet, not a description of
                             * it -- and taking a key the spec has spent, in a
                             * player that will grow seeking, buys convenience
                             * now against a collision later.
                             *
                             * KEY_VOLUME{UP,DOWN} cost nothing to accept: the
                             * beepy-kbd bitmap carries them and nothing else
                             * here wants them. */
                            case BEEPY_ALT_MINUS:
                            case KEY_MINUS:
                            case KEY_KPMINUS:
                            case KEY_VOLUMEDOWN:
                                volume_step(&P, -1, mono_now());
                                last_shown = (uint32_t)-1;
                                break;
                            case BEEPY_ALT_PLUS:
                            case KEY_EQUAL:
                            case KEY_KPPLUS:
                            case KEY_VOLUMEUP:
                                volume_step(&P, +1, mono_now());
                                last_shown = (uint32_t)-1;
                                break;
                            default:
                                break;
                            }
                        }
                    }
                }
            }
        }

        audio_close(&P.au);
        fb_release(&fb);
        evdev_close();
        if (P.trace)
            fclose(P.trace);
        pack_close(&P.pack);
        return rc;
    }
#endif
}

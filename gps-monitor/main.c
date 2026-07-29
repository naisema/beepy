/*
 * gps-monitor -- GNSS signal monitor for the Beepy's 400x240 Sharp panel.
 *
 * Reads NMEA from a u-blox receiver on /dev/ttyACM0, draws two full-screen
 * pages (vertical SNR bargraph, polar sky view) into a 1bpp backbuffer, and
 * presents them as full XRGB frames written to /dev/fb1.
 *
 * Design notes live in DESIGN.md / STRUCTURE.md. Since the M1 split the
 * program is a thin shell over two static libraries:
 *
 *   libbeepyfb -- canvas, font, framebuffer, evdev input
 *   libnmea    -- NMEA parsing, gps state, serial port
 *
 * This file keeps only what is app policy: the main loop, argument parsing,
 * signals/cleanup, the key bindings, and the choice of pages.
 *
 * Build: make -C .. (device); the libraries' portable half also compiles on
 * the Mac via `make host`.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "libbeepyfb/canvas.h"
#include "libbeepyfb/fbdev.h"
#include "libbeepyfb/font.h"
#include "libbeepyfb/input.h"
#include "libnmea/gps.h"
#include "libnmea/nmea.h"
#include "libnmea/serial.h"
#include "gpsmon.h"

/* ------------------------------------------------------------------- main */

static volatile sig_atomic_t g_quit;

static int is_evdev(int fd)
{
    for (int i = 0; i < evdev_count(); i++)
        if (evdev_fd(i) == fd)
            return 1;
    return 0;
}

/* Letter aliases exist because the Beepy's digits need the Alt/symbol
 * modifier, which makes "1"/"2" awkward on the physical keyboard. */
static int keycode_to_char(int code)
{
    switch (code) {
    case KEY_1: return '1';
    case KEY_2: return '2';
    case KEY_TAB: return '\t';
    case KEY_B: return 'b';
    case KEY_K: return 'k';
    case KEY_V: return 'v';
    case KEY_N: return 'n';
    case KEY_P: return 'p';
    case KEY_S: return 's';
    case KEY_G: return 'g';
    case KEY_H: return 'h';
    case KEY_R: return 'r';
    case KEY_A: return 'a';
    case KEY_Q: return 'q';
    case KEY_ESC: return 'q';
    default: return 0;
    }
}

static void handle_key(int k, gps_t *gps, view_t *view, port_t *port)
{
    int idx[MAX_SATS];
    int n = gps_order(gps, view->by_snr, idx, MAX_SATS);
    int cur = sel_index(gps, view, idx, n);

    switch (k) {
    case 'q': case 'Q': case 3: g_quit = 1; break;
    case '1': case 'b': case 'B': view->page = PAGE_BARS; break;
    case '2': case 'k': case 'K': view->page = PAGE_SKY; break;
    case '\t': case 'v': case 'V':
        view->page = view->page == PAGE_BARS ? PAGE_SKY : PAGE_BARS;
        break;
    case 's': case 'S': view->by_snr = !view->by_snr; break;
    case 'g': case 'G': view->grid = (view->grid + 1) % 3; break;
    case 'h': case 'H': view->hold = !view->hold; break;
    case 'a': case 'A': view->ascii = !view->ascii; break;
    case 'r': case 'R':
        port_close(port);
        gps->connected = 0;
        port->retry_at = 0;
        break;
    case 'n': case 'N':
        if (n) {
            cur = (cur + 1) % n;
            view->sel_sys = gps->live.s[idx[cur]].sys;
            view->sel_prn = gps->live.s[idx[cur]].prn;
        }
        break;
    case 'p': case 'P':
        if (n) {
            cur = (cur + n - 1) % n;
            view->sel_sys = gps->live.s[idx[cur]].sys;
            view->sel_prn = gps->live.s[idx[cur]].prn;
        }
        break;
    default: break;
    }
}
static fb_t *g_fb;
static struct termios g_tty_saved;
static int g_tty_raw;

static void on_signal(int s)
{
    (void)s;
    g_quit = 1;
}

static void cleanup(void)
{
    if (g_tty_raw)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_tty_saved);
    evdev_close();
    if (g_fb)
        fb_release(g_fb);
}

static void tty_raw(void)
{
    struct termios t;
    if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &g_tty_saved) < 0)
        return;
    t = g_tty_saved;
    t.c_lflag &= (unsigned)~(ICANON | ECHO);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
        g_tty_raw = 1;
}

static void usage(void)
{
    fputs("usage: gps-monitor [options]\n"
          "  -d DEV        serial device (default /dev/ttyACM0)\n"
          "  -b BAUD       line speed (default 9600)\n"
          "  --replay F    read NMEA from a file instead of a device\n"
          "  --log F       append received sentences to F\n"
          "  --demo        built-in synthetic satellite set, no receiver\n"
          "  --print       dump parsed state as text and exit\n"
          "  --dump F      render one frame to F (384000 raw bytes) and exit\n"
          "  --page bars|sky   initial page (default bars)\n"
          "  --cycle N     auto-switch pages every N seconds\n"
          "  --seconds N   exit after N seconds\n"
          "  --fbdev PATH  framebuffer (default /dev/fb1)\n"
          "  --ascii       hatch fills instead of solid, for panel comparison\n"
          "  --no-evdev    do not read /dev/input/event* (stdin keys only)\n"
          "  --no-grab     read evdev without grabbing it\n"
          "keys: 1 or B bars   2 or K sky   TAB or V toggle\n"
          "      n/p select  s sort  g grid  h hold  r reopen  q quit\n"
          "      letter keys avoid the Alt modifier the digits need\n", stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/ttyACM0", *fbpath = "/dev/fb1";
    const char *replay = NULL, *logpath = NULL, *dumppath = NULL;
    int baud = 9600, demo = 0, do_print = 0, cycle = 0, seconds = 0;
    int use_evdev = 1, grab = 1;
    gps_t gps;
    view_t view = {PAGE_BARS, 1, 'G', 2, 0, 0, 0};
    nmea_rx_t rx = {{0}, 0, 0};
    port_t port = {-1, "", 9600, 0, 0};
    canvas_t *cv;
    FILE *logf = NULL;
    fb_t fb;
    time_t start, last_draw = 0, last_cycle;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(a, "-b") && i + 1 < argc) baud = atoi(argv[++i]);
        else if (!strcmp(a, "--replay") && i + 1 < argc) replay = argv[++i];
        else if (!strcmp(a, "--log") && i + 1 < argc) logpath = argv[++i];
        else if (!strcmp(a, "--dump") && i + 1 < argc) dumppath = argv[++i];
        else if (!strcmp(a, "--fbdev") && i + 1 < argc) fbpath = argv[++i];
        else if (!strcmp(a, "--cycle") && i + 1 < argc) cycle = atoi(argv[++i]);
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(a, "--page") && i + 1 < argc)
            view.page = !strcmp(argv[++i], "sky") ? PAGE_SKY : PAGE_BARS;
        else if (!strcmp(a, "--demo")) demo = 1;
        else if (!strcmp(a, "--print")) do_print = 1;
        else if (!strcmp(a, "--ascii")) view.ascii = 1;
        else if (!strcmp(a, "--no-evdev")) use_evdev = 0;
        else if (!strcmp(a, "--no-grab")) grab = 0;
        else usage();
    }

    gps_init(&gps);
    if (demo)
        load_demo(&gps);

    if (!(cv = canvas_new(SCR_W, SCR_H))) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    if (logpath && !(logf = fopen(logpath, "a")))
        fprintf(stderr, "warning: cannot open %s: %s\n", logpath, strerror(errno));

    snprintf(port.path, sizeof port.path, "%s", replay ? replay : dev);
    port.baud = baud;
    port.replay = replay != NULL;

    if (!demo) {
        if (port_open(&port) == 0)
            gps.connected = 1;
        else if (replay || dumppath || do_print) {
            fprintf(stderr, "cannot open %s: %s\n", port.path, strerror(errno));
            return 1;
        }
    }

    /* --print / --dump consume the whole source, render once, and exit: this is
     * how rendering is verified without the panel or a receiver. */
    if (do_print || dumppath) {
        if (!demo && port.fd >= 0) {
            /* A serial port never reaches EOF, so collect for a bounded window
             * instead of reading until zero -- otherwise a live receiver yields
             * only whatever happened to be buffered. A replay file does EOF, so
             * it is drained normally. */
            time_t deadline = time(NULL) + (seconds > 0 ? seconds : 3);
            char buf[512];
            ssize_t k;
            for (;;) {
                if (port.replay) {
                    if ((k = read(port.fd, buf, sizeof buf)) <= 0)
                        break;
                    nmea_feed(&rx, &gps, buf, (size_t)k);
                    continue;
                }
                if (time(NULL) >= deadline)
                    break;
                struct pollfd pf = {port.fd, POLLIN, 0};
                if (poll(&pf, 1, 500) > 0 && (pf.revents & POLLIN)) {
                    k = read(port.fd, buf, sizeof buf);
                    if (k > 0)
                        nmea_feed(&rx, &gps, buf, (size_t)k);
                }
            }
        }
        if (do_print)
            print_state(&gps);
        if (dumppath) {
            canvas_clear(cv, PAPER);
            if (view.page == PAGE_SKY)
                view_sky(cv, &gps, &view);
            else
                view_bars(cv, &gps, &view);
            if (fb_dump(cv, dumppath) < 0) {
                fprintf(stderr, "dump %s: %s\n", dumppath, strerror(errno));
                return 1;
            }
            fprintf(stderr, "wrote %s\n", dumppath);
        }
        return 0;
    }

    if (fb_open(&fb, fbpath) < 0)
        return 1;
    if (fb.w != SCR_W || fb.h != SCR_H)
        fprintf(stderr, "warning: panel is %dx%d, layout assumes %dx%d\n",
                fb.w, fb.h, SCR_W, SCR_H);
    g_fb = &fb;
    atexit(cleanup);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    tty_raw();
    if (use_evdev)
        evdev_open(grab);
    fb_take(&fb);

    start = last_cycle = time(NULL);

    while (!g_quit) {
        struct pollfd pfd[2 + MAX_EVDEV];
        int np = 0, ptimeout = 500;
        if (port.fd >= 0) {
            pfd[np].fd = port.fd;
            pfd[np].events = POLLIN;
            np++;
        }
        /* Only watch stdin when it is a terminal: with stdin closed or a pipe
         * (a non-interactive ssh run) poll would report POLLHUP forever and
         * spin the loop. */
        if (g_tty_raw) {
            pfd[np].fd = STDIN_FILENO;
            pfd[np].events = POLLIN;
            np++;
        }
        for (int i = 0; i < evdev_count(); i++) {
            pfd[np].fd = evdev_fd(i);
            pfd[np].events = POLLIN;
            np++;
        }

        if (poll(pfd, (unsigned)np, ptimeout) > 0) {
            for (int i = 0; i < np; i++) {
                if (!(pfd[i].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                if (g_tty_raw && pfd[i].fd == STDIN_FILENO) {
                    char k;
                    while (read(STDIN_FILENO, &k, 1) == 1)
                        handle_key(k, &gps, &view, &port);
                } else if (is_evdev(pfd[i].fd)) {
                    int code, value;
                    while (evdev_next_key(pfd[i].fd, &code, &value))
                        if (value == 1) {
                            int k = keycode_to_char(code);
                            if (k)
                                handle_key(k, &gps, &view, &port);
                        }
                } else {
                    char buf[512];
                    ssize_t k = read(port.fd, buf, sizeof buf);
                    if (k > 0) {
                        nmea_feed(&rx, &gps, buf, (size_t)k);
                        if (logf) {
                            fwrite(buf, 1, (size_t)k, logf);
                            fflush(logf);
                        }
                    } else if (k == 0 || (k < 0 && errno != EAGAIN && errno != EINTR)) {
                        /* Receiver vanished (this one flaps): drop the fd and
                         * retry, keeping the last known state on screen. */
                        port_close(&port);
                        gps.connected = 0;
                        port.retry_at = time(NULL) + 1;
                    }
                }
            }
        }

        time_t now = time(NULL);
        /* Must exclude demo: otherwise this reopen path opens the real device
         * on the first iteration and live NMEA overwrites the demo data. */
        if (!demo && port.fd < 0 && !port.replay && now >= port.retry_at) {
            if (port_open(&port) == 0) {
                gps.connected = 1;
                rx.len = 0;
            } else {
                port.retry_at = now + 1;
            }
        }
        if (cycle > 0 && now - last_cycle >= cycle) {
            view.page = view.page == PAGE_BARS ? PAGE_SKY : PAGE_BARS;
            last_cycle = now;
        }
        if (seconds > 0 && now - start >= seconds)
            break;

        if (!view.hold || last_draw == 0) {
            canvas_clear(cv, PAPER);
            if (view.page == PAGE_SKY)
                view_sky(cv, &gps, &view);
            else
                view_bars(cv, &gps, &view);
            fb_present(&fb, cv);
            last_draw = now;
        }
    }

    if (logf)
        fclose(logf);
    port_close(&port);
    return 0;
}

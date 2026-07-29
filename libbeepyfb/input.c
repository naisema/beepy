/* libbeepyfb/input.c -- evdev keyboards, bypassing the (paused) fbterm.
 *
 * Split out of gps-monitor.c (M1).
 *
 * On the Beepy's own console the keyboard cannot be read from stdin: fbterm is
 * what feeds keystrokes to the pty, and it is SIGSTOPped while we own the
 * panel. Reading /dev/input/event* bypasses it entirely. The beepy user is in
 * group "input", so this needs no privileges.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/input.h>

#include "input.h"

static int g_ev[MAX_EVDEV];
static int g_evn;

static int bit_set(const unsigned long *b, int n)
{
    return (b[n / (8 * sizeof(unsigned long))] >>
            (n % (8 * sizeof(unsigned long)))) & 1UL;
}

void evdev_open(int grab)
{
    for (int i = 0; i < 32 && g_evn < MAX_EVDEV; i++) {
        char path[32];
        unsigned long bits[(KEY_MAX + 8 * sizeof(unsigned long)) /
                           (8 * sizeof(unsigned long))];
        int fd;
        snprintf(path, sizeof path, "/dev/input/event%d", i);
        if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0)
            continue;
        memset(bits, 0, sizeof bits);
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0 ||
            !bit_set(bits, KEY_Q) || !bit_set(bits, KEY_TAB)) {
            close(fd); /* not a keyboard */
            continue;
        }
        /* Grabbing stops the keys we consume from also queueing up on the
         * console, which would otherwise spill into the shell on exit. The
         * kernel releases the grab when the fd closes, including on a crash. */
        if (grab)
            ioctl(fd, EVIOCGRAB, 1);
        g_ev[g_evn++] = fd;
    }
}

void evdev_close(void)
{
    for (int i = 0; i < g_evn; i++) {
        ioctl(g_ev[i], EVIOCGRAB, 0);
        close(g_ev[i]);
    }
    g_evn = 0;
}

int evdev_count(void) { return g_evn; }

int evdev_fd(int i) { return (i >= 0 && i < g_evn) ? g_ev[i] : -1; }

int evdev_next_key(int fd, int *code, int *value)
{
    struct input_event ev;
    while (read(fd, &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY) {
            *code = ev.code;
            *value = ev.value;
            return 1;
        }
    }
    return 0;
}

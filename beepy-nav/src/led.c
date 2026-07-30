/* beepy-nav/src/led.c -- see led.h. */
#include <stdio.h>

#include "led.h"

#define LED_DIR "/sys/firmware/beepy"

/* 150 ms is long enough to register in peripheral vision and short enough
 * that three of them fit inside half a second, which is the whole budget an
 * alert gets before it becomes a distraction in its own right. */
#define LED_ON_S 0.15
#define LED_OFF_S 0.15

static int g_avail;
static int g_enabled = 1;
static int g_on;
static int g_left; /* toggles remaining: 2 per blink, so it ends dark */
static double g_next;

static int
write_attr(const char *name, const char *val)
{
    char path[128];
    FILE *f;
    snprintf(path, sizeof path, LED_DIR "/%s", name);
    f = fopen(path, "w");
    if (!f)
        return -1;
    fputs(val, f);
    /* The write can still fail at close (sysfs reports EACCES late), which is
     * exactly the case this module has to survive. */
    return fclose(f) == 0 ? 0 : -1;
}

int
led_init(int enabled)
{
    FILE *probe;
    g_avail = 0;
    g_enabled = enabled;
    g_on = 0;
    g_left = 0;
    /* The probe runs whether or not alerts start enabled: it is one write of
     * "0" to a light that is already dark, and it is what lets L switch them
     * on later without re-probing from inside a keypress. */
    if (write_attr("led", "0") != 0) {
        /* Distinguish "no such device" (a Mac, or a replay on any other
         * machine) from "there but root-only", because only the second one
         * has an answer, and it is one line long. Said once at startup and
         * never again -- 7.5 degrades to visual-only, it does not nag. */
        probe = fopen(LED_DIR "/led", "r");
        if (probe)
            fclose(probe);
        /* Said only when the rider asked for alerts. Starting muted and being
         * told the mute is also broken is noise. */
        else if (enabled && (probe = fopen(LED_DIR "/fw_version", "r")) != NULL) {
            fclose(probe);
            fputs("beepy-nav: cue alerts are visual only -- "
                  "/sys/firmware/beepy/led is not writable "
                  "(install/99-beepy-led.rules)\n",
                  stderr);
        }
        return 0;
    }
    /* Green: the alert is information, not a fault. Set once -- the blink
     * only toggles `led`, so a pulse is one write instead of four. */
    write_attr("led_red", "0");
    write_attr("led_green", "255");
    write_attr("led_blue", "0");
    g_avail = 1;
    return 1;
}

int
led_available(void)
{
    return g_avail;
}

void
led_enable(int on)
{
    g_enabled = on;
    if (!on)
        led_off();
}

int
led_enabled(void)
{
    return g_enabled;
}

void
led_pulse(int blinks, double t)
{
    if (!g_avail || !g_enabled || blinks <= 0)
        return;
    /* A new alert replaces whatever was still winking. Two cues 60 m apart
     * would otherwise queue their flashes and report the second one late,
     * which is worse than not reporting it. */
    g_left = 2 * blinks;
    g_next = t;
    if (g_on) {
        write_attr("led", "0");
        g_on = 0;
    }
}

void
led_tick(double t)
{
    if (!g_avail || !g_enabled || g_left <= 0 || t < g_next)
        return;
    g_on = !g_on;
    write_attr("led", g_on ? "1" : "0");
    g_left--;
    g_next = t + (g_on ? LED_ON_S : LED_OFF_S);
}

void
led_off(void)
{
    g_left = 0;
    if (!g_avail)
        return;
    write_attr("led", "0");
    g_on = 0;
}

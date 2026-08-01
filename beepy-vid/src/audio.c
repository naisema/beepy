/* beepy-vid/src/audio.c -- see audio.h.
 *
 * Before every include, and it has to be: the build is -std=c11, which is
 * STRICT ISO, and under strict ISO glibc hides fork, execl, kill, dup2 and
 * waitpid behind this. macOS libc declares them regardless, so omitting it
 * builds clean in the host lane and fails on the device. netfetch.c:3-9
 * documents the same trap, and it caught this project once already.
 */
#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "libbeepyfb/tick.h"

#include "audio.h"

/* Substitute %r and %c. Anything else is passed through, including a bare %,
 * so a command containing a printf-style escape is not mangled. */
static void
expand_cmd(char *out, size_t n, const char *cmd, int rate, int ch)
{
    size_t o = 0;
    for (const char *p = cmd; *p && o + 24 < n; p++) {
        if (p[0] == '%' && p[1] == 'r') {
            o += (size_t)snprintf(out + o, n - o, "%d", rate);
            p++;
        } else if (p[0] == '%' && p[1] == 'c') {
            o += (size_t)snprintf(out + o, n - o, "%d", ch);
            p++;
        } else {
            out[o++] = *p;
        }
    }
    out[o < n ? o : n - 1] = 0;
}

static int
spawn(audio_t *a, const char *cmd)
{
    char line[512];
    int fds[2];

    expand_cmd(line, sizeof line, cmd, a->rate, a->ch);
    if (pipe(fds) != 0) {
        fprintf(stderr, "beepy-vid: pipe: %s\n", strerror(errno));
        return -1;
    }
    a->pid = fork();
    if (a->pid < 0) {
        fprintf(stderr, "beepy-vid: fork: %s\n", strerror(errno));
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (a->pid == 0) {
        dup2(fds[0], STDIN_FILENO);
        close(fds[0]);
        close(fds[1]);
        /* Through the shell, so audio_cmd can be a pipeline or a redirect --
         * which is exactly what the gate uses. */
        execl("/bin/sh", "sh", "-c", line, (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    a->fd = fds[1];
    fcntl(a->fd, F_SETFL, O_NONBLOCK);
    /* A sink that exits immediately would otherwise kill us with SIGPIPE on
     * the first write; we want the error, not the signal. */
    signal(SIGPIPE, SIG_IGN);
    a->accepted = 0;
    a->prefill = 0;
    a->prefilled = 0;
    a->dead = 0;
    a->clk_started = 0;
    a->t_start = mono_now();
    return 0;
}

int
audio_open(audio_t *a, const char *cmd, int rate, int ch, int bits)
{
    memset(a, 0, sizeof *a);
    a->fd = -1;
    a->rate = rate;
    a->ch = ch;
    a->bits = bits ? bits : 16;
    if (!cmd || !*cmd || rate <= 0 || ch <= 0)
        return 0; /* silent film: not an error */
    return spawn(a, cmd);
}

int
audio_ok(const audio_t *a)
{
    return a->fd >= 0 && !a->dead;
}

long
audio_feed(audio_t *a, const unsigned char *buf, size_t n)
{
    ssize_t w;
    if (!audio_ok(a) || n == 0)
        return 0;
    w = write(a->fd, buf, n);
    if (w < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* The first refusal is the moment the pipe and the sink's own
             * buffer are full. Everything accepted up to here is latency, not
             * playback, so it is subtracted from the clock forever after. */
            if (!a->prefilled) {
                a->prefilled = 1;
                a->prefill = a->accepted;
            }
            return 0;
        }
        a->dead = 1;
        return -1;
    }
    a->accepted += w;
    return w;
}

int
audio_primed(const audio_t *a)
{
    return a->prefilled;
}

double
audio_time(const audio_t *a, double offset_s)
{
    double bps;
    if (!audio_ok(a) || !a->prefilled)
        return 0.0;
    bps = (double)a->rate * a->ch * (a->bits / 8);
    if (bps <= 0)
        return 0.0;
    return (double)(a->accepted - a->prefill) / bps + offset_s;
}

double
audio_clock(audio_t *a, double offset_s, double now)
{
    double target, dt, adv, cap;

    if (!audio_ok(a) || !a->prefilled)
        return 0.0;
    target = audio_time(a, offset_s);
    if (!a->clk_started) {
        a->clk_started = 1;
        a->clk = target;
        a->clk_wall = now;
        return a->clk;
    }
    dt = now - a->clk_wall;
    a->clk_wall = now;
    if (dt < 0)
        dt = 0;
    adv = target - a->clk;
    cap = AUDIO_MAX_SLEW * dt;
    if (adv > cap)
        adv = cap;  /* absorb the gulps */
    if (adv < 0)
        adv = 0;    /* a clock that went backwards would rewind the film */
    a->clk += adv;
    return a->clk;
}

void
audio_close(audio_t *a)
{
    int st;
    if (a->fd >= 0)
        close(a->fd);
    a->fd = -1;
    if (a->pid > 0) {
        kill(a->pid, SIGTERM);
        waitpid(a->pid, &st, 0);
        a->pid = 0;
    }
}

int
audio_reprime(audio_t *a, const char *cmd)
{
    if (!cmd || !*cmd)
        return 0;
    audio_close(a);
    return spawn(a, cmd);
}

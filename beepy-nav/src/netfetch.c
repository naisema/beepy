/* beepy-nav/src/netfetch.c -- see netfetch.h for why this is a child process. */

/* Before every include, and it has to be: the build is -std=c11, which is
 * STRICT ISO, and under strict ISO glibc hides fork, execl, kill, mkstemp,
 * setenv, dup2, waitpid and CLOCK_MONOTONIC behind this. macOS libc declares
 * them regardless, so the host lane compiled this file clean and the device
 * would not -- the same trap view_nav.c already documents for M_PI, and the
 * reason `make check` runs on the device rather than on the Mac. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "netfetch.h"

/* The fetch's OWN clock, and not the caller's.
 *
 * Found by running it: netfetch_poll() used to take the ride's `t`, on the
 * reasoning that one clock is better than two. That is right for everything
 * else in this program and wrong here. A timeout is a statement about the real
 * world -- how long a server has actually had -- and the ride clock is not the
 * real world in a `--no-pace` replay, where six hundred seconds of riding go by
 * in a few. The first slow-fetch test "timed out" after ten ride-seconds that
 * took eleven milliseconds. */
static double
wall(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static void
fail(netfetch_t *f, const char *why)
{
    snprintf(f->why, sizeof f->why, "%s", why);
    f->state = NF_FAILED;
}

/* Everything transient, gone. Called on cancel, on failure and after the
 * caller has taken the bytes -- so a fetch never leaves a file behind, which
 * matters on a device whose /tmp is RAM. */
static void
scrub(netfetch_t *f)
{
    if (f->fd >= 0) {
        close(f->fd);
        f->fd = -1;
    }
    if (f->path[0]) {
        unlink(f->path);
        f->path[0] = '\0';
    }
    if (f->body[0]) {
        unlink(f->body);
        f->body[0] = '\0';
    }
}

static int
temp_file(char *out, int n, const char *tag)
{
    const char *dir = getenv("TMPDIR");
    int fd;

    if (!dir || !*dir)
        dir = "/tmp";
    snprintf(out, (size_t)n, "%s/beepy-nav-%s-%ld.XXXXXX", dir, tag,
             (long)getpid());
    fd = mkstemp(out);
    if (fd < 0)
        out[0] = '\0';
    return fd;
}

int
netfetch_start(netfetch_t *f, const char *cmd, const char *url,
               const char *body)
{
    pid_t pid;

    if (!f)
        return -1;
    if (f->state == NF_RUNNING) {
        /* Not an assertion: two fetches in flight would leak the first child
         * and race two writers onto one path. The caller decides whether that
         * is a bug; this refuses either way. */
        fail(f, "a fetch is already running");
        return -1;
    }
    memset(f, 0, sizeof *f);
    f->fd = -1;
    if (!cmd || !*cmd) {
        fail(f, "no fetch_cmd configured");
        return -1;
    }

    f->fd = temp_file(f->path, (int)sizeof f->path, "out");
    if (f->fd < 0) {
        fail(f, "cannot make a temp file");
        return -1;
    }
    if (body && *body) {
        int bfd = temp_file(f->body, (int)sizeof f->body, "req");
        if (bfd < 0) {
            scrub(f);
            fail(f, "cannot make a request file");
            return -1;
        }
        if (write(bfd, body, strlen(body)) != (ssize_t)strlen(body)) {
            close(bfd);
            scrub(f);
            fail(f, "cannot write the request");
            return -1;
        }
        close(bfd);
    }

    pid = fork();
    if (pid < 0) {
        scrub(f);
        fail(f, "cannot fork a fetcher");
        return -1;
    }
    if (pid == 0) {
        /* The child. Its stdout IS the temp file, so the command needs no
         * redirection of its own and a test fixture can be `cat FILE`.
         * stderr goes to /dev/null: curl's progress and error chatter would
         * otherwise land in the middle of the ride log. */
        int devnull = open("/dev/null", O_WRONLY);
        dup2(f->fd, STDOUT_FILENO);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        close(f->fd);
        /* Through the environment, never interpolated: a URL with an ampersand
         * in it spliced into a shell line is a bug that waits for the first
         * destination that has one. */
        setenv("BEEPY_URL", url ? url : "", 1);
        setenv("BEEPY_BODY", f->body, 1);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127); /* exec failed: the parent sees it as a non-zero status */
    }

    f->pid = pid;
    f->state = NF_RUNNING;
    f->t0 = wall();
    return 0;
}

/* Slurp the temp file. Only ever called AFTER the child has been reaped, which
 * is the whole reason there is no partial-read handling here: nothing is
 * writing to it any more. */
static void
take_bytes(netfetch_t *f)
{
    struct stat st;
    long n;

    if (fstat(f->fd, &st) != 0) {
        fail(f, "cannot size the reply");
        return;
    }
    if (st.st_size <= 0) {
        fail(f, "empty reply");
        return;
    }
    if (st.st_size > NETFETCH_MAX_BYTES) {
        fail(f, "reply too large");
        return;
    }
    n = (long)st.st_size;
    f->buf = (char *)malloc((size_t)n + 1);
    if (!f->buf) {
        fail(f, "out of memory");
        return;
    }
    if (lseek(f->fd, 0, SEEK_SET) != 0 ||
        read(f->fd, f->buf, (size_t)n) != (ssize_t)n) {
        free(f->buf);
        f->buf = NULL;
        fail(f, "cannot read the reply");
        return;
    }
    f->buf[n] = '\0'; /* the parser wants a C string, not a length */
    f->len = n;
    f->state = NF_READY;
}

int
netfetch_poll(netfetch_t *f)
{
    double now = wall();
    int status = 0;
    pid_t r;

    if (!f || f->state != NF_RUNNING)
        return f ? f->state : NF_IDLE;

    r = waitpid(f->pid, &status, WNOHANG);
    if (r == 0) {
        /* Still running. The only thing that can happen here is the deadline,
         * and it has to be a kill: a fetcher blocked on a socket that will
         * never answer is not going to notice being asked politely. */
        if (now - f->t0 > NETFETCH_TIMEOUT_S) {
            kill(f->pid, SIGKILL);
            waitpid(f->pid, &status, 0); /* it is dead; this returns at once */
            scrub(f);
            fail(f, "timed out");
            f->timedout = 1; /* after fail(), which does not clear it */
        }
        return f->state;
    }
    if (r < 0) {
        scrub(f);
        fail(f, "lost the fetcher");
        return f->state;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        /* curl's own exit codes are not worth decoding here: every one of them
         * means the same thing to a rider, and the page says NO SIGNAL. */
        scrub(f);
        fail(f, WEXITSTATUS(status) == 127 ? "no fetcher on this device"
                                           : "fetch failed");
        return f->state;
    }
    take_bytes(f);
    scrub(f); /* the bytes are in memory now; the file has no further job */
    return f->state;
}

void
netfetch_cancel(netfetch_t *f)
{
    if (!f)
        return;
    if (f->state == NF_RUNNING && f->pid > 0) {
        int status;
        kill(f->pid, SIGKILL);
        waitpid(f->pid, &status, 0);
    }
    scrub(f);
    free(f->buf);
    f->buf = NULL;
    f->len = 0;
    f->state = NF_IDLE;
    f->why[0] = '\0';
    f->timedout = 0;
}

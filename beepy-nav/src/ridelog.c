/* beepy-nav/src/ridelog.c -- see ridelog.h. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "ridelog.h"

void
ridelog_default_dir(char *buf, size_t n)
{
    const char *home = getenv("HOME");
    snprintf(buf, n, "%s/rides", home && *home ? home : "/home/beepy");
}

/* One warning per log, ever. A message repeated once a second on a stderr
 * nobody is watching is not information, and on the device stderr may be the
 * SIGSTOPped terminal underneath the panel. */
static void
warn_once(ridelog_t *rl, const char *what, const char *detail)
{
    if (rl->warned)
        return;
    rl->warned = 1;
    fprintf(stderr, "beepy-nav: ride log: %s%s%s -- riding on unlogged\n",
            what, detail ? ": " : "", detail ? detail : "");
}

/* Free megabytes on the filesystem holding `dir`, or -1 when it cannot be
 * asked. f_bavail rather than f_bfree: the reserved blocks are not ours. */
static double
free_mb(const char *dir)
{
    struct statvfs vfs;
    if (statvfs(dir, &vfs) != 0)
        return -1.0;
    return (double)vfs.f_bavail * (double)vfs.f_frsize / (1024.0 * 1024.0);
}

int
ridelog_open(ridelog_t *rl, const char *dir, time_t now)
{
    struct tm tm;
    char stamp[20];
    double mb;

    memset(rl, 0, sizeof *rl);

    /* EEXIST is the ordinary case and not an error; anything else is, and it
     * is reported through the same one warning as everything else here. */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        warn_once(rl, dir, strerror(errno));
        return -1;
    }

    /* The disk guard runs BEFORE the files are created, so a refusal leaves
     * no empty pair behind to confuse the next person through the directory. */
    mb = free_mb(dir);
    if (mb >= 0.0 && mb < (double)RIDELOG_MIN_FREE_MB) {
        char detail[96];
        snprintf(detail, sizeof detail,
                 "%.0f MB free, %d needed to start a new log", mb,
                 RIDELOG_MIN_FREE_MB);
        warn_once(rl, dir, detail);
        return -1;
    }

    /* strftime and not snprintf: gcc 10's -Wformat-truncation cannot prove
     * that tm_year + 1900 fits in four digits and says so, and widening the
     * buffer to silence a warning about a year that cannot happen is the
     * wrong answer to it. strftime is also the shorter line. */
    localtime_r(&now, &tm);
    if (strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm) == 0) {
        warn_once(rl, dir, "cannot format a timestamp for the file name");
        return -1;
    }
    snprintf(rl->raw_path, sizeof rl->raw_path, "%s/%s.nmea", dir, stamp);
    snprintf(rl->tsv_path, sizeof rl->tsv_path, "%s/%s.tsv", dir, stamp);

    /* Append, not truncate. Two runs inside one second is not a case worth
     * engineering for, but "the second one silently ate the first one's
     * evidence" is a case worth ruling out, and append rules it out for the
     * cost of one character. */
    rl->raw = fopen(rl->raw_path, "a");
    if (!rl->raw) {
        warn_once(rl, rl->raw_path, strerror(errno));
        return -1;
    }
    rl->tsv = fopen(rl->tsv_path, "a");
    if (!rl->tsv) {
        warn_once(rl, rl->tsv_path, strerror(errno));
        fclose(rl->raw);
        rl->raw = NULL;
        return -1;
    }
    fprintf(stderr, "beepy-nav: logging to %s\n", rl->raw_path);
    return 0;
}

void
ridelog_raw(ridelog_t *rl, const char *b, size_t n)
{
    if (!rl->raw || n == 0)
        return;
    if (fwrite(b, 1, n, rl->raw) != n) {
        /* Out of space mid-ride, most likely. Stop writing, say so once, and
         * leave the navigator alone: what is already on disk stays valid, and
         * a partial log is worth exactly as much as a whole one to whoever is
         * debugging the failure that is probably happening right now. */
        warn_once(rl, rl->raw_path, strerror(errno));
        fclose(rl->raw);
        rl->raw = NULL;
        if (rl->tsv) {
            fclose(rl->tsv);
            rl->tsv = NULL;
        }
        return;
    }
    rl->bytes += (unsigned long)n;
}

void
ridelog_tick(ridelog_t *rl, double t)
{
    if (!rl->raw)
        return;
    /* Every epoch. This is the SIGKILL budget, not the power-cut one: the
     * kernel keeps what fflush hands it even when the process is gone. */
    fflush(rl->raw);
    if (rl->tsv)
        fflush(rl->tsv);
    /* And this is the power-cut one, on DESIGN.md 14's 30 s. The clock is the
     * ride clock, so an unpaced replay of this code path would sync on ride
     * seconds rather than on how fast the file was read. */
    if (t - rl->last_sync < RIDELOG_FSYNC_S)
        return;
    rl->last_sync = t;
    fsync(fileno(rl->raw));
    if (rl->tsv)
        fsync(fileno(rl->tsv));
}

void
ridelog_close(ridelog_t *rl)
{
    if (!rl->raw && !rl->tsv)
        return;
    if (rl->raw) {
        fflush(rl->raw);
        fsync(fileno(rl->raw));
        fclose(rl->raw);
        rl->raw = NULL;
    }
    if (rl->tsv) {
        fflush(rl->tsv);
        fsync(fileno(rl->tsv));
        fclose(rl->tsv);
        rl->tsv = NULL;
    }
    fprintf(stderr, "beepy-nav: ride log: %lu bytes in %s\n", rl->bytes,
            rl->raw_path);
}

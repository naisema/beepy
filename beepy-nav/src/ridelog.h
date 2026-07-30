/* beepy-nav/src/ridelog.h -- the ride log: raw NMEA plus the per-fix trace.
 *
 * DESIGN.md 14 removed "ride recording" from this program and that removal
 * stands: there is no .trk journal, no GPX writer, no ODO persistence and no
 * distance accumulation. What this module writes is not a record of the ride,
 * it is a record of the RECEIVER -- the bytes that arrived, verbatim, and the
 * nav_t they produced. It exists for one reason, stated in DESIGN.md 7.6:
 * every bug in this program so far was caught by a replay, and a failure in
 * the field currently leaves nothing to replay.
 *
 * VERBATIM, and that is the whole point. Re-serialising the parsed sentences
 * would produce a clean file that reproduces none of the malformed input worth
 * studying -- the truncated sentence, the bad checksum, the field the receiver
 * left empty. The bytes go down exactly as they came off the wire, before the
 * parser sees them, including the line endings and including anything that is
 * not NMEA at all.
 *
 * CRASH-SAFE, on two different budgets. Append-only, and:
 *
 *   - fflush() on every epoch. A SIGKILL takes the process's stdio buffer with
 *     it but not the kernel's, so this -- and not the fsync -- is what makes a
 *     killed navigator lose at most the epoch in progress. It costs one
 *     write(2) a second.
 *   - fsync() at most every 30 s. This is the expensive one, it is what a
 *     power cut needs rather than a SIGKILL, and 30 s is the budget DESIGN.md
 *     14 named when it was arguing this feature out of existence. At the
 *     measured ~60 KB an hour it is about 500 bytes at risk.
 *
 * A partial file is a valid replay input by construction: the reader is
 * line-oriented and discards a final unterminated line, so the worst a kill
 * can do is lose the sentence that was mid-flight.
 *
 * LOGGING MUST NEVER TAKE THE NAVIGATOR DOWN. Every failure here is a warning
 * to stderr -- once -- and then silence; the ride goes on unlogged. A rider
 * whose navigator refused to start because a directory was read-only has been
 * failed by the program, which is the same argument config.h makes.
 *
 * Portable C: stdio, plus mkdir/fsync/statvfs, which are POSIX and present in
 * both build lanes. Nothing here is device-specific -- what is device-specific
 * is that there is a serial port to log, which is why nav.c only opens a log
 * on the wire.
 */
#ifndef BEEPY_NAV_RIDELOG_H
#define BEEPY_NAV_RIDELOG_H

#include <stdio.h>
#include <time.h>

#define RIDELOG_PATH_MAX 320

/* See the header comment: the flush is per epoch and only the fsync is on
 * this budget. */
#define RIDELOG_FSYNC_S 30.0

/* A new log is refused below this. A ride is about 60 KB an hour, so 50 MB is
 * roughly a month of continuous riding and the guard will never be the thing
 * that stops a ride -- which is the point. It is not a quota, it is the line
 * below which a filesystem is in trouble for other reasons, and the failure
 * being guarded against is not "the log got big" but "the log filled the last
 * of the disk while the rider was told nothing". Silence on a full disk is the
 * unacceptable outcome; refusing to start one more file, out loud, is not. */
#define RIDELOG_MIN_FREE_MB 50

typedef struct {
    FILE *raw; /* the NMEA byte stream, verbatim */
    FILE *tsv; /* the per-fix trace, --trace's own columns */
    char raw_path[RIDELOG_PATH_MAX];
    char tsv_path[RIDELOG_PATH_MAX];
    double last_sync; /* ride seconds */
    unsigned long bytes;
    int warned; /* the one warning this module is allowed */
} ridelog_t;

/* $HOME/rides, or /home/beepy/rides when HOME is unset -- the same shape
 * chooser.c uses for routes. */
void ridelog_default_dir(char *buf, size_t n);

/* Create `dir` if needed, check the free space, and open
 * `dir/YYYYMMDD-HHMMSS.{nmea,tsv}` for append. Returns 0 with both files open,
 * or -1 having said exactly once on stderr why not. `now` is wall-clock time:
 * the file NAME is the only place in this program that uses it, because a name
 * is for a human looking at a directory listing. */
int ridelog_open(ridelog_t *rl, const char *dir, time_t now);

/* Bytes as they came off the wire. A no-op when the log is not open, so the
 * caller never has to ask. */
void ridelog_raw(ridelog_t *rl, const char *b, size_t n);

/* Push what is buffered out to the kernel (every call, cheap) and, at most
 * every RIDELOG_FSYNC_S ride-seconds, on to the disk. Call once per epoch. */
void ridelog_tick(ridelog_t *rl, double t);

/* Flush, fsync, close. Idempotent. */
void ridelog_close(ridelog_t *rl);

#endif /* BEEPY_NAV_RIDELOG_H */

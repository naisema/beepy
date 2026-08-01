/* beepy-vid/src/audio.h -- the audio sink, and the clock it provides.
 *
 * The sink is a CHILD PROCESS fed over a pipe, named by audio_cmd, not a
 * linked ALSA or PulseAudio call. beepy-nav/src/config.h:80-83 gives the
 * argument for fetch_cmd and it transfers exactly: "that is what lets a test
 * substitute `cat FIXTURE` and keeps the gate off the network structurally
 * rather than by promise." Here it keeps `make check` off the sound card
 * structurally -- the gate sets audio_cmd to a fake sink and no assertion
 * depends on a paired speaker existing.
 *
 * It also keeps the libc-only rule (beepy-nav/DESIGN.md 4). Linking
 * libpulse-simple would buy pa_simple_get_latency(), but that reports
 * PulseAudio's own pipeline only and not the buffer inside the headset, which
 * is the larger and more variable half -- measured at 42 ms against a true
 * A2DP figure of 150-250 ms. It would buy an estimate, not a measurement, at
 * the cost of the dependency and the Buildroot packaging goal the rule
 * protects. The calibration screen is the source of truth either way.
 *
 * THE CLOCK. Once the pipe and the sink's buffer are full, the pipe accepts
 * bytes exactly as fast as the sink drains them, so bytes-accepted IS the
 * playback position:
 *
 *     t_audio = (accepted - prefill) / (rate * ch * bytes_per_sample) + offset
 *
 * That cannot drift against the speaker, because it is measured at the
 * speaker's own consumption rate. CLOCK_MONOTONIC and a headset's DAC crystal
 * drift apart by seconds over a feature film, which is why video slaves to
 * this and never the reverse.
 */
#ifndef BEEPYVID_AUDIO_H
#define BEEPYVID_AUDIO_H

#include <sys/types.h>
#include <stddef.h>

typedef struct {
    pid_t pid;
    int fd;          /* write end of the pipe, non-blocking */
    long accepted;   /* bytes the kernel has taken from us */
    long prefill;    /* bytes accepted before the first EAGAIN */
    int prefilled;
    int rate, ch, bits;
    int dead;
    /* Slew-limited clock state. See audio_clock(). */
    double clk, clk_wall;
    int clk_started;
} audio_t;

/* cmd may contain %r (rate) and %c (channels). Returns 0, or -1 with the
 * reason on stderr. A NULL or empty cmd means "no audio": every call below
 * then does nothing and audio_ok() is false, which is the silent-film path. */
int audio_open(audio_t *a, const char *cmd, int rate, int ch, int bits);

int audio_ok(const audio_t *a);

/* Write what the sink will take right now; never blocks. Returns bytes
 * accepted (0 is normal and means the sink is full, which is the steady
 * state), or -1 if the child has gone. */
long audio_feed(audio_t *a, const unsigned char *buf, size_t n);

/* Playback position in seconds, plus the A/V offset. Before the pipe has
 * filled once this is still rising with the prefill, so callers should not
 * start the video clock until audio_primed(). */
double audio_time(const audio_t *a, double offset_s);

/* The clock a scheduler should use: audio_time(), slew limited.
 *
 * BYTES ACCEPTED IS NOT BYTES PLAYED. A pipe drains and refills in gulps -- a
 * sink reads 4096, the writer tops up 8192, and in between nothing moves --
 * so a position derived from acceptance has an accurate long-term RATE and a
 * violently noisy instantaneous VALUE. Measured raw on a 240-frame clip: the
 * player presented frames in bursts 2.5 ms apart, advancing the clock at
 * roughly 33x real time, then sat still for half a second. 99 of 240 frames
 * were dropped by a sink running at exactly the right speed.
 *
 * Interpolating between acceptances does not fix it, because the jumps are in
 * the acceptances themselves. What fixes it is refusing to let the clock
 * advance faster than AUDIO_MAX_SLEW times real time: bursts are absorbed,
 * the long-term rate still comes from the sink, and a sink running slow still
 * drags the video with it because that direction is not limited at all.
 *
 * Stateful, so it must be called once per loop iteration and takes a
 * non-const audio_t. */
#define AUDIO_MAX_SLEW 1.5
double audio_clock(audio_t *a, double offset_s, double now);
int audio_primed(const audio_t *a);

/* Tear the child down and start a new one. Every discontinuity needs this --
 * seek, pause/resume, track change -- because the sink is holding 150-250 ms
 * of audio from BEFORE the discontinuity and would play it after the picture
 * moved. A quarter second of silence beats a quarter second of the wrong
 * sound. */
int audio_reprime(audio_t *a, const char *cmd);

void audio_close(audio_t *a);

#endif /* BEEPYVID_AUDIO_H */

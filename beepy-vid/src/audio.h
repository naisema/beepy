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

/* VOLUME. Attenuation applied to the samples HERE, before they reach the
 * pipe -- not `pactl set-sink-volume` and not a linked mixer API.
 *
 * Three reasons, in descending order of how much they bind:
 *
 * 1. The sink is opaque by construction. audio_cmd may be `cat >/dev/null`,
 *    a fakesink, or an ssh pipeline, and 7.3 chose that precisely so the gate
 *    never needs a sound card. A player that shelled out to pactl would work
 *    only when the sink happened to be PulseAudio, and would put a mixer call
 *    inside `make check`. Scaling the bytes we already hold works for every
 *    sink and is testable by pointing audio_cmd at a file.
 * 2. It is what the hardware leaves us anyway. README records that a speaker
 *    with no AVRCP absolute volume -- the VOX-DC is one -- cannot be turned up
 *    from the Beepy at all, so `set-sink-volume` there IS only software
 *    attenuation. Doing it ourselves is the same operation, minus the
 *    dependency, and it behaves identically across speakers that differ.
 * 3. libc only, so the Buildroot packaging goal survives.
 *
 * ATTENUATION ONLY, and that is a deliberate ceiling rather than a missing
 * feature: gain above unity would have to clip or limit, and no amount of it
 * makes a speaker louder than its own amplifier. Level 10 is unity and is the
 * default, so a player that never touches the volume writes the pack's bytes
 * through byte for byte.
 *
 * THE CLOCK IS UNAFFECTED, which is the property that makes this safe at all.
 * Scaling rewrites sample values in place and changes no byte count, and the
 * clock in this file is bytes-accepted -- so volume cannot move the film. A
 * resampling or format-converting volume control would not have that property.
 */
#define AUDIO_VOL_MAX 10

typedef struct {
    pid_t pid;
    int fd;          /* write end of the pipe, non-blocking */
    long accepted;   /* bytes the kernel has taken from us */
    long prefill;    /* bytes accepted before the first EAGAIN */
    int prefilled;
    int rate, ch, bits;
    int dead;
    int vol;         /* 0..AUDIO_VOL_MAX; 10 is unity */
    /* Slew-limited clock state. See audio_clock(). */
    double clk, clk_wall;
    int clk_started;
    double t_start;
} audio_t;

/* cmd may contain %r (rate) and %c (channels). Returns 0, or -1 with the
 * reason on stderr. A NULL or empty cmd means "no audio": every call below
 * then does nothing and audio_ok() is false, which is the silent-film path. */
int audio_open(audio_t *a, const char *cmd, int rate, int ch, int bits);

int audio_ok(const audio_t *a);

/* Write what the sink will take right now; never blocks. Returns bytes
 * accepted (0 is normal and means the sink is full, which is the steady
 * state), or -1 if the child has gone.
 *
 * KNOWN DEFECT, see beepy-vid/DESIGN.md 7.6: with a greedy consumer -- pacat
 * has its own buffer and PulseAudio another behind it -- acceptance races
 * ahead of playback and the derived clock runs fast. Measured against a real
 * A2DP sink: frames every 27.6 ms instead of 41.7, roughly 1.5x. A feed rate
 * limiter was tried and made it worse in the other direction (half speed
 * against the fake sink), so it was removed rather than shipped. */
long audio_feed(audio_t *a, const unsigned char *buf, size_t n);

/* Clamp to 0..AUDIO_VOL_MAX and store. Returns the level actually set, so a
 * caller can show what happened rather than what it asked for. */
int audio_set_vol(audio_t *a, int level);
int audio_vol(const audio_t *a);

/* Scale n bytes of S16LE in place by the current level, to be called on the
 * buffer between pack_audio() and audio_feed().
 *
 * In place and on freshly-read bytes both matter. audio_feed() is free to
 * accept only part of the buffer, and the remainder is re-read from the pack
 * next iteration -- unscaled again -- so scaling is never applied twice to the
 * same sample, and a level changed mid-buffer takes effect on the very next
 * read rather than at some flush boundary.
 *
 * Bytes are assembled explicitly rather than cast to int16_t*: the buffer
 * comes off a pack whose format is defined as little-endian regardless of the
 * host, and a cast would also be an alignment and aliasing bet for nothing.
 * A partial trailing sample (odd n) is left alone rather than half-scaled. */
void audio_apply_vol(const audio_t *a, unsigned char *buf, size_t n);

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
/* 1.05, not 1.5. A sink playing at its nominal rate CANNOT advance faster
 * than real time, so anything above 1.0 is pure headroom for jitter; 1.5 let
 * the film run half again too fast against a real A2DP sink. The slow
 * direction stays unlimited, so audio-is-master is unaffected -- a 20% slow
 * sink still stretches playback by 23%, gated in test-vidsync. */
#define AUDIO_MAX_SLEW 1.05
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

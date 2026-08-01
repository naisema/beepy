# beepy-vid — watching things on the Beepy

A video player for the 400×240 one-bit Sharp panel. Films are converted on the
Mac into a `.vid` pack; the Beepy streams the frames and does no decoding.

`DESIGN.md` is the specification and says why every number is what it is.
`PLAN.md` is how it was built. This file is how to use it.

## Once, on the Mac

```sh
brew install ffmpeg          # only needed for --video; the packer itself is numpy
```

## Make a pack

```sh
python3 tools/mkvid.py --video FILM.mp4 -o film.vid
```

Useful flags:

| flag | |
|---|---|
| `--start 70 --duration 30` | take 30 s from 1:10, rather than the whole film |
| `--fps 24` | 24 is the default and the right answer for most sources (§2.1) |
| `--dither bayer4 \| bayer8` | 4×4 is the default and measured better on both size and look |
| `--hysteresis 8` | higher makes the pack smaller and smears moving edges |
| `--no-audio` | drop the soundtrack |
| `--info film.vid` | header, mode histogram, bytes per frame, sha256 |

Measured on a real 30-second animated short: **2.6 s to convert, 3.7 MB**,
1182 bytes per frame. A 90-minute film lands around 300–700 MB, most of which
is uncompressed audio — video alone is roughly 0.6–1.4 MB/minute.

## Put it on the Beepy

```sh
scp film.vid beepy@beepy.local:~/videos/
```

## Install the player (once)

```sh
make sync                                    # builds on the device
ssh beepy@beepy.local
sudo install -m 755 ~/beepy-src/beepy-vid/beepy-vid    /usr/local/bin/beepy-vid.bin
sudo install -m 755 ~/beepy-src/beepy-vid/beepy-vid.sh /usr/local/bin/beepy-vid
```

`beepy-vid` is the wrapper and `beepy-vid.bin` is the program. **Always run the
wrapper.** It restores the console if the player is killed in a way the program
cannot catch — `kill -9`, an OOM kill, a force quit. Without it a killed player
leaves `fbterm` stopped and the device looks dead.

## Watch

```sh
beepy-vid ~/videos/film.vid
```

**Sound is on by default**, including over Bluetooth. The wrapper finds the
default PulseAudio sink, sets `XDG_RUNTIME_DIR` so the daemon is visible over
ssh, and feeds the sink at whatever rate the pack header declares. If there is
no sink — nothing paired, no daemon — it says so on stderr and plays the film
silent, which is a legitimate outcome and not a failure.

To pick a different sink, or send the audio somewhere else entirely:

```sh
beepy-vid ~/videos/film.vid --no-audio
beepy-vid ~/videos/film.vid \
  --audio-cmd "pacat --rate=%r --channels=%c --format=s16le --device=OTHER"
```

An explicit `--audio-cmd` or `--no-audio` always beats the default. So does
`--demo`, which renders pages rather than playing and never looks for a
speaker. Note the default lives in the **wrapper**, not the player: the
program itself still defaults to silence, which is what keeps `make check`
off the sound card structurally (`DESIGN.md` §7.3).

| key | |
|---|---|
| `SPACE` | play / pause |
| `alt`+`I` / `alt`+`O` | volume down / up, 11 steps — the `-` and `+` legends |
| `Q` or `ESC` | quit |

## Volume

**The keys are `alt`+`I` and `alt`+`O`.** There is no `-` or `+` key on this
keyboard; both are legends on the physical alt layer, printed on `I` and `O` at
the end of the top row (`U_  I-  O+  P@`).

Holding `alt` does not send `KEY_MINUS`. `beepy-kbd` adds **119** to the
keycode (`input_modifiers.c`, `map_phys_alt_keycode`) and its `beepy-kbd.map`
assigns the results — `keycode 142 = minus`, `keycode 143 = plus`. That map is
a *console* keymap, and the player grabs evdev directly, so it sees the bare
142 and 143 and never the translation. The player binds those. `KEY_MINUS` and
`KEY_EQUAL` are still accepted for a USB keyboard, but on the Beepy they are
never sent.

The level steps and the panel says which one you landed on —
`VOLUME 7`, or `MUTE` at the bottom, or `NO AUDIO` if there is no sink to
adjust. `--volume N` sets it from the command line for a whole run.

Three things about it that are deliberate rather than unfinished:

- **Attenuation only.** Level 10 is unity and the default, and there is
  nothing above it. Gain past unity would have to clip, and it cannot make a
  speaker louder than its own amplifier — so **turn the speaker up and come
  down from here**, not the other way round. On a speaker with no AVRCP
  absolute volume this is the only control there is anyway (see below).
- **The steps are decibels, not percent.** Ten steps over 40 dB, a little
  under 4 dB each. Evenly spaced percentages are not evenly spaced steps:
  100→90% is inaudible while 20→10% halves the apparent loudness.
- **It is done to the samples, in the player.** Not `pactl set-sink-volume`,
  which would work only when the sink happens to be PulseAudio and would put a
  mixer call inside `make check`. `DESIGN.md` §7.3 and `audio.h` carry the
  argument. The practical consequence is that it behaves the same on every
  speaker, and that `--volume` is assertable without a sound card — which is
  what `make test-vidvol` does.

**A change lags by the sink's buffer.** The pipe and PulseAudio and the
headset each hold audio scaled at the old level, and over A2DP that is a
noticeable fraction of a second before a press is audible. Pressing again
because nothing happened yet will overshoot; the on-screen level is the truth,
not your ears.

`--start SECONDS` begins partway in. `--av-offset MS` delays the video to match
a speaker that lags; A2DP is typically 150–250 ms and the default is 0.

## Bluetooth, once

The radio is enabled on this device but it is not the factory state — see
`DESIGN.md` §7.1 if you ever reflash. To connect a speaker:

```sh
bluetoothctl
  power on
  scan on
  pair <MAC>
  trust <MAC>        # this is what makes it reconnect on its own
  connect <MAC>
```

Two things that will waste your afternoon otherwise:

- **`XDG_RUNTIME_DIR`.** PulseAudio runs as a user daemon in the console
  session. Without that variable, `pactl` and `pacat` over ssh fail in a way
  that looks exactly like broken audio. The wrapper now exports it for you, so
  this no longer bites *playback* — but it still bites every `pactl` you run by
  hand over ssh, which is most of the debugging you would do here.
- **Some speakers ignore the volume you set.** A sink with no AVRCP absolute
  volume — the VOX-DC here is one, the Spotless D1 is not — cannot be turned up
  from the Beepy at all. `pactl set-sink-volume` is then only software
  attenuation and the speaker's own knob is the master.

## What it looks like

Animation and line art look excellent; this is the content the panel was born
for. Photographic video reads well in daylight scenes. **Dark scenes go flat**,
and gradients band — dithering to one bit discards tone irreversibly, so if a
film looks wrong the fix is nearly always a re-transcode (`--hysteresis`, or a
brighter source) and not the player. The library page shows each pack's
settings for exactly that reason.

## Known rough edges

- **About 5% of frames are dropped** with audio playing — 684 of 720 on the
  clip above. Unexplained; measured and written up in `DESIGN.md` §7.2.1.
- **Lip-sync has never been measured**, only reasoned about. `--av-offset`
  exists; the calibration screen is drawn but not yet wired to the keys.
- **The library page renders but is not navigable yet** — pass a file path.
- Subtitles are not implemented. When they are they will be ALL CAPS, because
  the font is uppercase-only.

## If the screen stays black after a crash

```sh
kill -CONT $(pgrep -x fbterm)
```

The wrapper does this for you. This is the manual version.

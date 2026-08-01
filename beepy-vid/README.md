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

With sound, over Bluetooth:

```sh
export XDG_RUNTIME_DIR=/run/user/1000        # or PulseAudio is invisible over ssh
SINK=$(pactl info | sed -n 's/Default Sink: //p')
beepy-vid ~/videos/film.vid \
  --audio-cmd "pacat --rate=%r --channels=%c --format=s16le --device=$SINK"
```

| key | |
|---|---|
| `SPACE` | play / pause |
| `Q` or `ESC` | quit |

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
  that looks exactly like broken audio.
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

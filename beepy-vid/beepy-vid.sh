#!/bin/sh
# beepy-vid.sh -- run the player with a guaranteed SIGCONT for fbterm.
#
# The C program handles INT, TERM, HUP and the fatal signals, but SIGSTOP
# cannot be caught and neither can SIGKILL: a kill -9, an OOM kill or a
# force-quit leaves fbterm stopped and the device looking dead
# (beepy-nav/DESIGN.md 2798 records this and offers only a manual kill -CONT).
# A shell survives its child being SIGKILLed and still runs its EXIT trap, so
# this closes a gap the C program structurally cannot. beepy-nav/fbanim does
# the same for fbshow.
#
# NOT `exec "$BIN"`. exec REPLACES this shell with the binary, and a replaced
# shell has no trap left to run -- the wrapper would look right, cost nothing,
# and protect nothing. The first version of this file did exactly that, and a
# kill -9 test left fbterm in T+ with the panel dead. The binary must stay a
# child, and this script must stay alive to bury it.
#
# Install as /usr/local/bin/beepy-vid with the binary alongside as
# beepy-vid.bin.
set -u

restore() {
    pids=$(pgrep -x fbterm 2>/dev/null) || pids=""
    [ -n "$pids" ] && kill -CONT $pids 2>/dev/null
    tmux refresh-client -S 2>/dev/null
    return 0
}
trap 'restore' EXIT
# Forward the interactive signals to the player so it can exit cleanly and
# write its resume position; the EXIT trap then runs regardless.
trap 'kill -TERM "$child" 2>/dev/null' INT TERM HUP

DIR=$(dirname "$0")
BIN="$DIR/beepy-vid.bin"
[ -x "$BIN" ] || BIN="$DIR/beepy-vid"

# Audio on by default.
#
# The player takes its sink as a child process named by audio_cmd (audio.h)
# and defaults to NONE, because DESIGN.md 7.3 keeps `make check` off the sound
# card structurally rather than by promise. That default is right for the gate
# and wrong for a person: it made "watch a film" mean pasting a three-line
# pacat pipeline out of the README every time, and a forgotten paste is a
# silent film with no error to explain it.
#
# The default belongs HERE and not in vid.c. The gate runs ./host/beepy-vid
# directly and this wrapper appears nowhere in the Makefile, so a default
# added here cannot reach an assertion -- whereas the same default in the
# argument parser would have every headless test spawn a real sink, which is
# exactly the coupling 7.3 spent its argument removing.
#
# An explicit --audio-cmd or --no-audio always wins, and --demo renders pages
# rather than playing, so none of them go looking for a speaker.
want_sink=1
for arg do
    case $arg in
        --audio-cmd | --no-audio | --demo) want_sink=0; break ;;
    esac
done

AUDIO=""
if [ "$want_sink" -eq 1 ]; then
    # PulseAudio is a user daemon in the console session. Without this an ssh
    # session cannot see it, and pactl and pacat then fail in a way that looks
    # exactly like broken audio rather than like a missing variable.
    [ -n "${XDG_RUNTIME_DIR:-}" ] || XDG_RUNTIME_DIR="/run/user/$(id -u)"
    export XDG_RUNTIME_DIR
    # %r and %c are substituted by the player from the pack header, so the
    # sink follows the pack's rate rather than assuming this script's idea of
    # it. They must survive to the binary uninterpreted -- hence no printf.
    if command -v pacat >/dev/null 2>&1; then
        sink=$(pactl info 2>/dev/null | sed -n 's/^Default Sink: //p')
        [ -n "${sink:-}" ] && AUDIO="pacat --rate=%r --channels=%c \
--format=s16le --device=$sink"
    fi
    # Say so. A silent film is a legitimate outcome here -- no speaker paired,
    # no daemon running -- but it is indistinguishable from a broken one
    # unless the wrapper names which of the two it chose.
    [ -n "$AUDIO" ] ||
        echo "beepy-vid: no PulseAudio sink found; playing silent" >&2
fi

# Ours goes first so a caller's own --audio-cmd later in the line still wins;
# the parser keeps the last one it sees.
if [ -n "$AUDIO" ]; then
    "$BIN" --audio-cmd "$AUDIO" "$@" &
else
    "$BIN" "$@" &
fi
child=$!
wait "$child"
rc=$?
# 128+n when the player was signalled; pass it on rather than inventing 0.
exit $rc

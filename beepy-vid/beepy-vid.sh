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

"$BIN" "$@" &
child=$!
wait "$child"
rc=$?
# 128+n when the player was signalled; pass it on rather than inventing 0.
exit $rc

#!/bin/bash
# Reproduce the "Would you like to sign in with a gamer profile?" input-death
# and dump every thread's stack while it is stuck.
#
#   tools/catch_signin.sh [padlog]
#
# The user reports this as one of the boot failures: the dialog comes up and
# button presses "just don't do a thing". Replaying a from-launch pad recording
# with NO start delay lands on it reliably, and the guest is NOT hung -- swaps,
# ring kicks and the walker all keep climbing -- so the game is alive and simply
# ignoring input.
#
# What to look for in the dump: a guest thread parked in rex::thread::Fence::Wait
# underneath a XAM UI call. rexglue's dialog helper (xam_ui.cpp:92 and :134) does
#     ++xam_dialogs_shown_; fence.Wait(); --xam_dialogs_shown_;
# so a dialog that is never closed blocks the calling guest thread forever AND
# pins xeXamIsUIActive() true (xam_module.cpp:28) -- which is exactly the shape
# of "the game renders but ignores every button".
set -u
PAD="${1:-/tmp/hole.padlog}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
OUT="${RESTUFF_SCRATCH:-/tmp/restuff_drive}/signin"; mkdir -p "$OUT"
DISP=":99"
cd "$BUILD" || exit 1
pgrep -x restuff >/dev/null || rm -f /dev/shm/xenia_memory_* 2>/dev/null
pkill -x Xvfb 2>/dev/null; pkill -9 -x restuff 2>/dev/null; sleep 1
Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & sleep 2
RESTUFF_PAD_REPLAY="$PAD" DISPLAY=$DISP SDL_VIDEODRIVER=x11 ./restuff >"$OUT/run.log" 2>&1 & GAME=$!
echo "[signin] launched pid $GAME; waiting for the dialog"
sleep 100
DISPLAY=$DISP ffmpeg -y -f x11grab -video_size 1280x720 -i $DISP -frames:v 1 \
    "$OUT/screen.png" >/dev/null 2>&1
if kill -0 "$GAME" 2>/dev/null; then
  gdb -p "$GAME" -batch -ex "set pagination off" -ex "thread apply all bt 30" \
      >"$OUT/stacks.txt" 2>&1 || eu-stack -p "$GAME" >"$OUT/stacks.txt" 2>&1
  echo "[signin] stacks -> $OUT/stacks.txt ($(wc -l < "$OUT/stacks.txt") lines)"
else
  echo "[signin] process already exited"
fi
kill -9 "$GAME" 2>/dev/null; pkill -x Xvfb 2>/dev/null
echo "[signin] screen -> $OUT/screen.png"

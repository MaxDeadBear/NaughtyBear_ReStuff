#!/bin/bash
# Catch a NOBOOT hang in the act and dump where every thread is stuck.
#
#   tools/catch_noboot.sh [max_attempts]
#
# The noboot failure is permanent (same ~1-in-6 rate at 95s and 150s) and dies
# EARLY: the process logs only "Loaded config from restuff.toml" and never
# reaches "present thread started". That window is inside SDK/app init, before
# any of our renderer code, so the fastest way to name it is a backtrace of the
# hung process rather than more logging.
#
# Boots repeatedly; the moment an attempt misses the boot marker past the
# deadline, dumps all thread stacks (gdb, falling back to eu-stack) and stops.
set -u
MAX="${1:-12}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
LOG="$BUILD/restuff_dbg.log"; DISP=":99"
OUT="${RESTUFF_SCRATCH:-/tmp/restuff_drive}/noboot"; mkdir -p "$OUT"
PAD="$OUT/vpad.txt"; echo 0000 > "$PAD"
cd "$BUILD" || exit 1
for i in $(seq 1 "$MAX"); do
  ps -eo args | grep -q "[r]estuff" || rm -f /dev/shm/xenia_memory_* 2>/dev/null
  pkill -f "Xvfb $DISP" 2>/dev/null; pkill -9 -x restuff 2>/dev/null; sleep 1
  Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & XVFB=$!; sleep 2
  TS=$(date '+%Y-%m-%d %H:%M:%S')
  RESTUFF_KEYPAD="$PAD" DISPLAY=$DISP SDL_VIDEODRIVER=x11 ./restuff >"$OUT/try.log" 2>&1 & GAME=$!
  sleep 45
  if awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null | grep -q "present thread started"; then
    echo "attempt $i: booted ok"
    kill -9 $GAME 2>/dev/null; pkill -f "Xvfb $DISP" 2>/dev/null; sleep 1; continue
  fi
  echo "attempt $i: NOBOOT CAUGHT (pid $GAME) -- dumping stacks"
  if kill -0 $GAME 2>/dev/null; then
    gdb -p "$GAME" -batch -ex "set pagination off" -ex "thread apply all bt 25" \
        >"$OUT/stacks.txt" 2>&1 || eu-stack -p "$GAME" >"$OUT/stacks.txt" 2>&1
    echo "  -> $OUT/stacks.txt ($(wc -l < "$OUT/stacks.txt") lines)"
  else
    echo "  -> process already DEAD (not a hang: it exited)" | tee "$OUT/stacks.txt"
  fi
  cp "$OUT/try.log" "$OUT/noboot_stdout.log" 2>/dev/null
  kill -9 $GAME 2>/dev/null; pkill -f "Xvfb $DISP" 2>/dev/null
  exit 0
done
echo "no noboot in $MAX attempts"

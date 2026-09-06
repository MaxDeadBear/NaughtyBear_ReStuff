#!/bin/bash
# Catch the "boots but never draws" failure (blue or black screen) and dump
# every thread's stack while it is stuck.
#
#   tools/catch_blank.sh [max_attempts] [ENV=VAL ...]
#
# Distinct from catch_noboot.sh: there the boot MARKER never lands. Here the
# guest boots fine ("present thread started" is logged) and then presents a
# uniform frame forever -- cornflower blue (~99,148,237, a bare clear) or black.
# Those are the user's two most common launch failures, and the screen colour is
# the only way to tell them from a healthy run, so this classifies by FRAME and
# dumps stacks the moment it sees a uniform one.
set -u
MAX="${1:-8}"; shift || true
ENVKV="${*:-RESTUFF_UNUSED=0}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
LOG="$BUILD/restuff_dbg.log"; DISP=":99"
OUT="${RESTUFF_SCRATCH:-/tmp/restuff_drive}/blank"; mkdir -p "$OUT"
PAD="$OUT/vpad.txt"; echo 0000 > "$PAD"
cd "$BUILD" || exit 1

uniform() {  # $1=png -> prints "UNIFORM <desc>" or "VARIED"
  python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB').resize((160, 90))
px = list(im.getdata()); n = len(px)
r = sum(p[0] for p in px)/n; g = sum(p[1] for p in px)/n; b = sum(p[2] for p in px)/n
lum = 0.2126*r + 0.7152*g + 0.0722*b
var = sum((0.2126*p[0]+0.7152*p[1]+0.0722*p[2]-lum)**2 for p in px)/n
print(f'UNIFORM rgb=({r:.0f},{g:.0f},{b:.0f}) var={var:.0f}' if var < 60 else f'VARIED var={var:.0f}')
PY
}

for i in $(seq 1 "$MAX"); do
  pgrep -x restuff >/dev/null || rm -f /dev/shm/xenia_memory_* 2>/dev/null
  pkill -x Xvfb 2>/dev/null; pkill -9 -x restuff 2>/dev/null; sleep 1
  Xvfb $DISP -screen 0 ${XVFB_SCREEN:-1280x720}x24 >/dev/null 2>&1 & sleep 2
  TS=$(date '+%Y-%m-%d %H:%M:%S')
  env $ENVKV RESTUFF_KEYPAD="$PAD" DISPLAY=$DISP SDL_VIDEODRIVER=x11 ./restuff \
      >"$OUT/try.log" 2>&1 & GAME=$!
  sleep 75
  DISPLAY=$DISP ffmpeg -y -f x11grab -video_size ${XVFB_WH:-1280x720} -i $DISP -frames:v 1 \
      "$OUT/a$i.png" >/dev/null 2>&1
  V=$(uniform "$OUT/a$i.png")
  BOOTED=no
  awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null \
      | grep -q "present thread started" && BOOTED=yes
  echo "attempt $i: booted=$BOOTED $V"
  case "$V" in
    UNIFORM*)
      if kill -0 "$GAME" 2>/dev/null; then
        gdb -p "$GAME" -batch -ex "set pagination off" -ex "thread apply all bt 30" \
            >"$OUT/stacks.txt" 2>&1 || eu-stack -p "$GAME" >"$OUT/stacks.txt" 2>&1
        echo "  -> CAUGHT; stacks in $OUT/stacks.txt ($(wc -l < "$OUT/stacks.txt") lines)"
        awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" > "$OUT/blank.log" 2>/dev/null
      else
        echo "  -> process already exited"
      fi
      kill -9 "$GAME" 2>/dev/null; pkill -x Xvfb 2>/dev/null
      exit 0;;
  esac
  kill -9 "$GAME" 2>/dev/null; pkill -x Xvfb 2>/dev/null; sleep 1
done
echo "no blank screen in $MAX attempts"

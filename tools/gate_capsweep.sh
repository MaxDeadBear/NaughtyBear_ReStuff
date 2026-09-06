#!/bin/bash
# Live draw-cap luminance bisection at the post-window outdoor view.
#
#   tools/gate_capsweep.sh "ENV=VAL [...]" <tag>
#
# Boots restuff, replays the proven gate_drive route through the window exit
# plus one steering burst (the score-gate view class), then stands still and
# sweeps RESTUFF_CAP_FILE live, snapping C<cap>.png per cap. Run once with the
# default winding and once with RESTUFF_LEGACY_WINDING=1: the first cap where
# the per-cap luminance DELTA between the two runs appears brackets the draw
# range that darkens M3.71 frames. [CAPEDGE] in restuff_dbg.log names the
# first-skipped draw per cap.
set -u
ENVKV="${1:-RESTUFF_UNUSED=0}"; TAG="${2:-capsweep}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
SCRATCH="${RESTUFF_SCRATCH:-/tmp/restuff_drive}"
OUT="$SCRATCH/env_$TAG"; mkdir -p "$OUT"; rm -f "$OUT"/*.png
LOG="$BUILD/restuff_dbg.log"; PAD="$SCRATCH/vpad_$TAG.txt"; echo 0000 > "$PAD"; DISP=":98"
CAPF="$SCRATCH/cap_$TAG.txt"; rm -f "$CAPF" "$SCRATCH/dltrig_$TAG"
snap() { DISPLAY=$DISP ffmpeg -y -f x11grab -video_size 1280x720 -i $DISP -frames:v 1 "$OUT/$1.png" >/dev/null 2>&1; }
press() { echo "$1" > "$PAD"; sleep 0.4; echo 0000 > "$PAD"; }
ps -eo args | grep -q "[r]estuff" || rm -f /dev/shm/xenia_memory_* 2>/dev/null
pkill -f "Xvfb $DISP" 2>/dev/null; pkill -9 -x restuff 2>/dev/null; sleep 1
Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & XVFB=$!; sleep 2
cd "$BUILD" || exit 1
( sleep 560; pkill -9 -x restuff 2>/dev/null ) >/dev/null 2>&1 & WD=$!
GAME=""
for attempt in 1 2 3 4 5 6; do
  TS=$(date +"%Y-%m-%d %H:%M:%S")
  env $ENVKV RESTUFF_KEYPAD="$PAD" RESTUFF_CAP_FILE="$CAPF" RESTUFF_DRAWLIST_FILE="$SCRATCH/dltrig_$TAG" DISPLAY=$DISP SDL_VIDEODRIVER=x11 ./restuff >>"$OUT/run.log" 2>&1 & GAME=$!
  sleep 40
  if ! awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null | /usr/bin/grep -q "LUCON.TTF"; then
    echo "attempt $attempt stalled"; kill -9 $GAME 2>/dev/null; GAME=""; sleep 2; continue
  fi
  sleep 15
  SWAPS=$(awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null | /usr/bin/grep -oE "swaps=[0-9]+" | tail -1 | /usr/bin/grep -oE "[0-9]+")
  if [ -n "${GATE_NO_SWAPCHECK:-}" ]; then echo "boot OK (swapcheck bypassed)"; break; fi
  if [ "${SWAPS:-0}" -gt 60 ]; then echo "boot OK (swaps=$SWAPS)"; break; fi
  echo "attempt $attempt stalled post-boot (swaps=${SWAPS:-0})"; kill -9 $GAME 2>/dev/null; GAME=""; sleep 2
done
[ -z "$GAME" ] && { echo STALLED; kill -9 $XVFB $WD 2>/dev/null; exit 1; }
sleep 20
press 0010; sleep 8; press 1000; sleep 8; press 1000; sleep 10; press 1000; sleep 8; press 1000; sleep 8
for i in 1 2 3 4; do kill -0 $GAME 2>/dev/null || break; press 1000; sleep 20; done
walk() { echo "0000 0 $1" > "$PAD"; sleep "$2"; echo "0000 0 0" > "$PAD"; sleep 0.6; }
turn() { echo "0000 0 0 $1 0" > "$PAD"; sleep "$2"; echo "0000 0 0 0 0" > "$PAD"; sleep 0.6; }
walk 32000 4; walk 32000 4; walk 32000 4
press 1000; sleep 4          # window exit
turn ${GATE_RX:-25000} 1.5; walk 32000 ${GATE_LWALK:-3}
snap C9999                    # uncapped anchor
touch "$SCRATCH/dltrig_$TAG"  # M3.72: dump the standing view's ordered draw list
sleep 2
for cap in 364 362 360 359 358 357 356 355; do
  kill -0 $GAME 2>/dev/null || break
  echo "$cap" > "$CAPF"
  sleep 5
  snap "C$cap"
done
rm -f "$CAPF"; sleep 3; snap C9998   # uncapped again (moment drift control)
kill -9 $GAME $XVFB $WD 2>/dev/null; pkill -f "sleep 56[0]" 2>/dev/null
echo "capsweep done -> $OUT"

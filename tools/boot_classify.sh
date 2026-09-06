#!/bin/bash
# Boot-reliability harness: boot restuff N times and CLASSIFY each attempt.
#
#   tools/boot_classify.sh [N] [ENV=VAL ...]
#
# The user needed SIX launches to get one working session (2 blue, 2 black, 1
# stuck at "sign in to gamer profile" with input dead, 1 ok). That is three
# distinct failure modes, so the first job is a measurable baseline, not a
# theory. Each attempt is labelled from the FRAME plus the draw counters --
# never from the log alone, because a run can report "boot OK (swaps=1495)" and
# still render nothing at all (seen repeatedly).
#
# Classes:
#   ok      title/menu rendered  (draws>0 and frame is not near-uniform)
#   black   frame near-black
#   blue    frame dominated by blue  (⚠️ in-game this is UNDER-LEVEL WATER with
#           geometry culled, not sky -- at boot it may be a clear-only frame;
#           the draws count is what tells them apart, so it is reported too)
#   nodraw  frame has content but zero draws were ever recorded
#   noboot  never reached the LUCON.TTF boot marker
#
# Writes <scratch>/boot_<tag>/aNN.png plus a summary table on stdout.
set -u
N="${1:-10}"; shift || true
ENVKV="${*:-RESTUFF_UNUSED=0}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
SCRATCH="${RESTUFF_SCRATCH:-/tmp/restuff_drive}"
OUT="$SCRATCH/boot_classify"; mkdir -p "$OUT"; rm -f "$OUT"/*.png
LOG="$BUILD/restuff_dbg.log"; DISP=":99"; PAD="$SCRATCH/vpad_boot.txt"; echo 0000 > "$PAD"
cd "$BUILD" || exit 1

classify() {  # $1=png $2=draws $3=after.png
  python3 - "$1" "$2" "${3:-}" <<'PY'
import sys, os
from PIL import Image
def load(p):
    return list(Image.open(p).convert('RGB').resize((160, 90)).getdata())
px = load(sys.argv[1])
n = len(px)
# Did the screen respond to input? Mean abs difference over the grabs.
after = sys.argv[3] if len(sys.argv) > 3 else ''
resp = None
if after and os.path.exists(after):
    pb = load(after)
    resp = sum(abs(a[0]-b[0])+abs(a[1]-b[1])+abs(a[2]-b[2]) for a, b in zip(px, pb))/(3.0*n)
r = sum(p[0] for p in px)/n; g = sum(p[1] for p in px)/n; b = sum(p[2] for p in px)/n
lum = 0.2126*r + 0.7152*g + 0.0722*b
draws = int(sys.argv[2])
# near-uniform => nothing meaningful was composed
var = sum((0.2126*p[0]+0.7152*p[1]+0.0722*p[2]-lum)**2 for p in px)/n
if lum < 6:                      cls = 'black'
elif b > 60 and b > r*1.6 and b > g*1.25 and var < 900: cls = 'blue'
elif draws == 0:                 cls = 'nodraw'
elif resp is not None and resp < 0.6: cls = 'deadinput'  # renders, ignores the pad
else:                            cls = 'ok'
rs = f' resp={resp:.2f}' if resp is not None else ''
print(f'{cls} lum={lum:.1f} rgb=({r:.0f},{g:.0f},{b:.0f}) var={var:.0f} draws={draws}{rs}')
PY
}

echo "=== boot_classify: $N attempts, env: $ENVKV ==="
declare -A TALLY
for i in $(seq 1 "$N"); do
  TAG=$(printf "a%02d" "$i")
  ps -eo args | grep -q "[r]estuff" || rm -f /dev/shm/xenia_memory_* 2>/dev/null
  pkill -f "Xvfb $DISP" 2>/dev/null; pkill -9 -x restuff 2>/dev/null; sleep 1
  Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & XVFB=$!; sleep 2
  TS=$(date '+%Y-%m-%d %H:%M:%S')
  env $ENVKV RESTUFF_KEYPAD="$PAD" DISPLAY=$DISP SDL_VIDEODRIVER=x11 ./restuff \
      >"$OUT/$TAG.log" 2>&1 & GAME=$!
  # BOOT_WAIT: quantized ISR delivery (RESTUFF_PUMP_LEGACY=1) boots SLOWER, so a
  # fixed deadline turns "slow" into a bogus "noboot". Tune it when comparing.
  sleep "${BOOT_WAIT:-55}"
  if ! awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null | grep -q "LUCON.TTF"; then
    RES="noboot"
  else
    DISPLAY=$DISP ffmpeg -y -f x11grab -video_size 1280x720 -i $DISP -frames:v 1 \
        "$OUT/$TAG.png" >/dev/null 2>&1
    # A rendered title is NOT a successful boot. The user's most distinctive
    # failure is the title coming up stuck on "sign in to gamer profile" with
    # ALL INPUT DEAD -- which scores as 'ok' if we only look at one frame. So
    # drive the pad and require the screen to actually CHANGE: press START/A a
    # few times and re-grab. Unchanged => the game is not accepting input.
    for _ in 1 2 3; do echo 1000 > "$PAD"; sleep 0.5; echo 0000 > "$PAD"; sleep 1.2; done
    echo 0010 > "$PAD"; sleep 0.5; echo 0000 > "$PAD"; sleep 3
    DISPLAY=$DISP ffmpeg -y -f x11grab -video_size 1280x720 -i $DISP -frames:v 1 \
        "$OUT/${TAG}_after.png" >/dev/null 2>&1
    # draws recorded since this attempt started (present-thread 'trans=' field)
    DRAWS=$(awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null \
            | grep -oE "trans=[0-9]+" | grep -oE "[0-9]+" | sort -rn | head -1)
    RES=$(classify "$OUT/$TAG.png" "${DRAWS:-0}" "$OUT/${TAG}_after.png")
  fi
  kill -9 $GAME 2>/dev/null; pkill -f "Xvfb $DISP" 2>/dev/null; sleep 1
  KEY=$(echo "$RES" | awk '{print $1}')
  TALLY[$KEY]=$(( ${TALLY[$KEY]:-0} + 1 ))
  echo "$TAG: $RES"
done
echo "=== tally ==="
for k in "${!TALLY[@]}"; do echo "  $k: ${TALLY[$k]}"; done

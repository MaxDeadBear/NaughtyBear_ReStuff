#!/bin/bash
# Matched A/B: does a COLD pipeline cache cause the early-boot hang?
#
#   tools/cold_cache_ab.sh [trials_per_arm]
#
# Deleting pipeline_cache.bin reproduced the hang on attempt 1 after 34 clean
# warm-cache boots -- but that is n=1, and a SUCCESSFUL run re-saves the cache
# at frame 1500 (~25s), so a naive loop only has one genuinely cold attempt.
# This deletes the cache before EVERY cold trial and restores the warm copy
# before every warm trial, so both arms are matched and repeatable.
#
# ⚠️ Never `pkill -f "Xvfb :99"` from an inline shell command -- the pattern
# matches the running shell's own command line and kills the harness (exit 144,
# which is how a previous attempt lost its cache-restore step). Living in a
# script file, and matching the process NAME with -x, avoids both.
set -u
N="${1:-5}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
cd "$BUILD" || exit 1
CACHE="pipeline_cache.bin"; BAK="/tmp/pipeline_cache_ab.bak"
[ -f "$CACHE" ] || { echo "no warm cache present -- run the game once first"; exit 1; }
cp "$CACHE" "$BAK"
restore() { cp -f "$BAK" "$CACHE" 2>/dev/null; echo "[cold_cache_ab] cache restored"; }
trap restore EXIT INT TERM
PAD="/tmp/vpad_ab.txt"; echo 0000 > "$PAD"

trial() {  # $1 = cold|warm ; echoes BOOTED / NOBOOT
  if [ "$1" = cold ]; then rm -f "$CACHE"; else cp -f "$BAK" "$CACHE"; fi
  pkill -x Xvfb 2>/dev/null; pkill -9 -x restuff 2>/dev/null; sleep 1
  rm -f /dev/shm/xenia_memory_* 2>/dev/null
  Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 & sleep 2
  local ts; ts=$(date '+%Y-%m-%d %H:%M:%S')
  RESTUFF_KEYPAD="$PAD" DISPLAY=:99 SDL_VIDEODRIVER=x11 ./restuff >/dev/null 2>&1 & local g=$!
  sleep 45
  local res=NOBOOT
  if awk -v ts="$ts" 'substr($0,2,19) >= ts' restuff_dbg.log 2>/dev/null \
     | grep -q "present thread started"; then res=BOOTED; fi
  kill -9 $g 2>/dev/null; pkill -x Xvfb 2>/dev/null; sleep 1
  echo "$res"
}

cb=0; cn=0; wb=0; wn=0
for i in $(seq 1 "$N"); do
  r=$(trial cold); echo "cold $i: $r"; [ "$r" = BOOTED ] && cb=$((cb+1)) || cn=$((cn+1))
  r=$(trial warm); echo "warm $i: $r"; [ "$r" = BOOTED ] && wb=$((wb+1)) || wn=$((wn+1))
done
echo "=== RESULT  cold: $cb booted / $cn noboot   |   warm: $wb booted / $wn noboot"

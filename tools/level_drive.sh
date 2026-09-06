#!/bin/bash
# Headless gameplay drive for the native Vulkan renderer.
#
#   tools/level_drive.sh "ENV=VAL [ENV2=VAL2 ...]" <tag>
#
# Boots restuff under Xvfb, navigates menus with a virtual pad, reaches
# gameplay, and snaps presented frames to <scratch>/env_<tag>/M0*.png.
# Lives in the repo on purpose: /tmp gets wiped (twice so far) and this
# harness is the only way to reach gameplay unattended.
#
# Notes / gotchas learned the hard way:
#  - Each run lands on a DIFFERENT game moment (fixed sleeps + boot jitter),
#    so cross-run A/B of moment-dependent defects (black blobs) is unreliable.
#  - restuff_dbg.log is APPENDED across runs -> always thread-partition greps:
#      T=$(grep "\[TAG\]" restuff_dbg.log | tail -1 | grep -oE "\[t[0-9]+\]")
#      grep -F "$T" restuff_dbg.log | grep "\[TAG\]"
#  - The game is SIGKILLed at the end, so anything that must persist has to be
#    written periodically, not at shutdown (see the pipeline cache).
#  - Boot can stall under load; we retry up to 6x on the LUCON.TTF boot marker.
#  - Diagnostics that sample "the first gameplay frame" are WARM-UP POISONED
#    (pipelines still building -> draws skip and look structurally absent).
#    Arm late (e.g. RESTUFF_PASS_FRAME=400).
set -u
ENVKV="${1:-RESTUFF_UNUSED=0}"; TAG="${2:-env}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
SCRATCH="${RESTUFF_SCRATCH:-/tmp/restuff_drive}"
OUT="$SCRATCH/env_$TAG"; mkdir -p "$OUT"; rm -f "$OUT"/*.png
LOG="$BUILD/restuff_dbg.log"; PAD="$SCRATCH/vpad_$TAG.txt"; echo 0000 > "$PAD"; DISP=":99"
snap() { DISPLAY=$DISP ffmpeg -y -f x11grab -video_size 1280x720 -i $DISP -frames:v 1 "$OUT/$1.png" >/dev/null 2>&1; }
press() { echo "$1" > "$PAD"; sleep 0.4; echo 0000 > "$PAD"; }
# SIGKILLed runs leak xenia_memory_* shm (300MB resident each) -> /dev/shm
# fills -> SIGBUS in Memory::Initialize at next boot. Clear orphans first.
ps -eo args | grep -q "[r]estuff" || rm -f /dev/shm/xenia_memory_* 2>/dev/null
pkill -f "Xvfb $DISP" 2>/dev/null; pkill -9 -x restuff 2>/dev/null; sleep 1
Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & XVFB=$!; sleep 2
cd "$BUILD" || exit 1
( sleep 480; pkill -9 -x restuff 2>/dev/null ) & WD=$!
GAME=""
for attempt in 1 2 3 4 5 6; do
  TS=$(date +"%Y-%m-%d %H:%M:%S")
  # Capture BOTH streams: stdout carries the early-boot preamble (before the
  # file logger opens -- the only clue when a boot stalls silently), stderr is
  # where glibc/driver crash text would land. Main telemetry stays in
  # restuff_dbg.log.
  env $ENVKV RESTUFF_KEYPAD="$PAD" DISPLAY=$DISP SDL_VIDEODRIVER=x11 ./restuff >>"$OUT/run.log" 2>&1 & GAME=$!
  sleep 40
  if ! awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null | /usr/bin/grep -q "LUCON.TTF"; then
    echo "attempt $attempt stalled (no boot marker)"; kill -9 $GAME 2>/dev/null; GAME=""; sleep 2; continue
  fi
  # Second-stage check: the guest can pass the boot marker and STILL stall
  # before ever rendering (black screen, swaps stuck single-digit). Require
  # real swap progress before trusting the run.
  sleep 15
  SWAPS=$(awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null | /usr/bin/grep -oE "swaps=[0-9]+" | tail -1 | /usr/bin/grep -oE "[0-9]+")
  if [ "${SWAPS:-0}" -gt 60 ]; then echo "boot OK (swaps=$SWAPS)"; break; fi
  echo "attempt $attempt stalled post-boot (swaps=${SWAPS:-0})"; kill -9 $GAME 2>/dev/null; GAME=""; sleep 2
done
[ -z "$GAME" ] && { echo STALLED; kill -9 $XVFB $WD 2>/dev/null; exit 1; }
sleep 20
# menu nav: down to Play Game, then through save/episode/costume, skip cutscene
press 0010; sleep 8; press 1000; sleep 8; press 1000; sleep 10; press 1000; sleep 8; press 1000; sleep 8
# Cutscene skip: a few A presses, then STOP pressing.
for i in 1 2 3 4; do kill -0 $GAME 2>/dev/null || break; press 1000; sleep 20; done
# STILL mode (default): from here on send NO input. The bear stays on his rug
# at the deterministic level-start view, so frames are COMPARABLE ACROSS RUNS
# -- which is the only way to A/B a moment-dependent defect (black blobs).
# RESTUFF_DRIVE_ACTIVE=1 restores the old button-mashing walkabout.
if [ -n "${RESTUFF_DRIVE_ACTIVE:-}" ]; then
  for i in $(seq 1 8); do kill -0 $GAME 2>/dev/null || break; press 1000; sleep 22; snap M$(printf %02d $i); done
else
  echo 0000 > "$PAD"
  for i in $(seq 1 8); do kill -0 $GAME 2>/dev/null || break; sleep 12; snap S$(printf %02d $i); done
fi
kill -9 $GAME $XVFB $WD 2>/dev/null
echo "drive $TAG done -> $OUT"

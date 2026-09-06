#!/bin/bash
# Regenerate the labelled screen references classify_boot2.py matches against.
#
# WHY THIS EXISTS: the references used to be read out of whatever run directory
# happened to have produced them, under /tmp/restuff_drive. That is tmpfs. A
# power cut wiped it and every reference went with it -- including STORAGE and
# CORRUPT, whose whole job is to stop the storage and corrupt-save prompts being
# counted as sign-ins. Losing them does not break the classifier loudly; it just
# makes it wrong in the one direction that matters. References now live in
# tools/refs/dialogs/ (on disk, in git) and this script rebuilds them.
#
# Two of the three dialogs are reachable ON DEMAND via fault injection, which is
# why they can be regenerated at all:
#   STORAGE  <- RESTUFF_DEVSEL_FAULT=1        (no storage device selected)
#   CORRUPT  <- RESTUFF_CONTENT_FAULT=create  (saved game is corrupt)
# SIGNIN cannot be forced reliably -- it is the intermittent bug under
# investigation -- so its reference is the archived LIVE repro frame kept in
# tools/padlogs/wedge_corpus_2026-08-10/.
#
# The boot-sequence screens (LEGAL/AUTOSAVE/LOADING/MENU) are just filmed: grab
# a frame every 3s from launch and pick the ones that show each screen.
#
#   usage: tools/regen_refs.sh [storage|corrupt|film|all]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$ROOT/out/build/linux-amd64-relwithdebinfo"
OUTROOT=/tmp/restuff_drive/refcap
WHAT="${1:-all}"
mkdir -p "$OUTROOT" /tmp/restuff_drive

# Take the SAME lock wedge_ab.sh takes. This script pkills restuff to get a
# clean launch, so running it beside a live A/B batch would silently murder that
# batch's game mid-run and the batch would report a failed boot instead of a
# result. -w 0: if a drive is in flight, say so and quit rather than queue.
exec 9>/tmp/restuff_drive/.drive.lock
if ! flock -w 0 9; then
  echo "ABORT: a drive holds the lock; refs must not pkill it. Holders:"
  find /proc/[0-9]*/fd -lname /tmp/restuff_drive/.drive.lock 2>/dev/null \
    | cut -d/ -f3 | sort -u | xargs -r ps -o pid,etimes,args -p
  exit 3
fi

# film <name> <seconds> [ENV=VAL ...] -- launch a boot and grab a frame every 3s
film(){
  local name=$1 secs=$2; shift 2
  local out="$OUTROOT/$name"; rm -rf "$out"; mkdir -p "$out"
  pkill -9 -x restuff >/dev/null 2>&1; pkill -x Xvfb >/dev/null 2>&1; sleep 2
  Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 & sleep 2
  cd "$B" || exit 1
  echo "  filming $name for ${secs}s with: $*"
  env "$@" RESTUFF_PAD_REPLAY=/tmp/hole.padlog RESTUFF_PAD_REPLAY_DELAY=900 \
      DISPLAY=:99 SDL_VIDEODRIVER=x11 ./restuff >"$out/run.log" 2>&1 9>&- & local game=$!
  local t
  for t in $(seq 1 $((secs/3))); do
    sleep 3
    DISPLAY=:99 ffmpeg -y -f x11grab -video_size 1280x720 -i :99 -frames:v 1 \
      "$out/$(printf f%02d "$t").png" >/dev/null 2>&1
  done
  kill -9 "$game" >/dev/null 2>&1; pkill -x Xvfb >/dev/null 2>&1; sleep 1
  echo "  -> $out ($(ls "$out"/f*.png 2>/dev/null | wc -l) frames)"
}

case "$WHAT" in
  storage) film storage 120 RESTUFF_DEVSEL_FAULT=1 ;;
  corrupt) film corrupt 120 RESTUFF_CONTENT_FAULT=create ;;
  film)    film boot 150 RESTUFF_UNUSED=0 ;;
  all)     film boot 150 RESTUFF_UNUSED=0
           film storage 120 RESTUFF_DEVSEL_FAULT=1
           film corrupt 120 RESTUFF_CONTENT_FAULT=create ;;
  *) echo "usage: regen_refs.sh [storage|corrupt|film|all]"; exit 2 ;;
esac
echo "captures in $OUTROOT -- pick frames BY EYE and copy into tools/refs/dialogs/."
echo "Do not auto-pick: these screens differ only by their text, which is exactly"
echo "what an automatic picker cannot see."

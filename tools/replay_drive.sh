#!/bin/bash
# Replay a human pad recording and snap frames at the end of it.
#
#   tools/replay_drive.sh <padlog> <tag> [extra ENV=VAL ...]
#
# Why not gate_drive.sh: under RESTUFF_PAD_REPLAY the replay takes precedence
# over the virtual pad, so the drive's own scripted menu presses are IGNORED --
# the recording has to drive the menus itself. This harness therefore does no
# navigation at all; it boots, lets the recording play, and snaps.
#
# Frames are snapped in a burst AFTER the recording's own duration has elapsed
# (the trailing pad state is held, so the bear stays put) -- that is the fixed
# viewpoint the whole point of this is to get.
#
# ⚠️ Start alignment is the known weak spot: the recording's poll 0 is the
# HUMAN's first input poll, ours arrives after a boot of variable length. If the
# bear ends up somewhere else, retry, or try RESTUFF_PAD_REPLAY_CLOCK=1.
set -u
PADLOG="${1:?usage: replay_drive.sh <padlog> <tag> [ENV=VAL ...]}"
# The padlog lives in tmpfs and EVERY REBOOT deletes it. The game then logs
# "loaded 0 events" and boots to an idle title screen -- and a whole A/B ran
# exactly that way once, producing two arms of frames of nothing (the Aug 14
# 21:07 reboot emptied it; the Aug 13 power cut did too). Self-heal from the
# git-archived route when the standard padlog is missing/empty; abort loudly
# for any other empty padlog rather than film an undriven game.
if [ ! -s "$PADLOG" ]; then
  ARCHIVED="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/padlogs/hole_route_2026-08-05.padlog"
  if [ "$(basename "$PADLOG")" = "hole.padlog" ] && [ -s "$ARCHIVED" ]; then
    cp "$ARCHIVED" "$PADLOG"
    echo "[replay] padlog was missing/empty (tmpfs reboot?) -- restored from $ARCHIVED"
  else
    echo "[replay] ABORT: padlog '$PADLOG' is missing/empty; a run without input"
    echo "[replay] just films an idle title screen and reads as a valid boot."
    exit 4
  fi
fi
TAG="${2:-replay}"; shift 2 || true
EXTRA="${*:-RESTUFF_UNUSED=0}"
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
SCRATCH="${RESTUFF_SCRATCH:-/tmp/restuff_drive}"
OUT="$SCRATCH/env_$TAG"; mkdir -p "$OUT"; rm -f "$OUT"/W*.png 2>/dev/null
LOG="$BUILD/restuff_dbg.log"; DISP="${RESTUFF_DISP:-:99}"
# RESTUFF_DISP=<:N> uses an EXTERNAL display (e.g. gamescope headless, which
# provides real DRI3 for the emulated/SDK-presenter path Xvfb cannot run) and
# skips Xvfb management entirely. Snaps still x11grab from that display.
# Boot marker: "[native_vk] present thread started" only exists on the NATIVE
# renderer. The emulated arm (replay_emulated.sh) overrides this with a
# renderer-agnostic guest-side line, else every attempt reads as "no boot".
BOOT_MARKER="${BOOT_MARKER:-present thread started}"
# Recording length in seconds, from its last timestamp (+ margin).
# How long to wait before snapping. This differs by KEYING MODE and getting it
# wrong silently truncates the route:
#  - clock mode: the recording plays on its own wall clock, so its own length
#    (+ any delay, in ms) is right.
#  - poll mode: it plays at OUR poll rate, which headless is ~15/s against the
#    ~30/s it was recorded at -- so a 1400-poll recording takes ~95s, not the
#    ~63s of wall clock it was captured over. Waiting the recorded length ends
#    the run about halfway through the route, which is exactly what made two
#    replays stop short of the gate.
DELAY=$(printf '%s\n' "$EXTRA" | grep -oE 'RESTUFF_PAD_REPLAY_DELAY=[0-9]+' | cut -d= -f2)
case "$EXTRA" in
  *RESTUFF_PAD_REPLAY_CLOCK*)
    DUR=$(awk 'END{print int($2/1000)+8}' "$PADLOG")
    DUR=$((DUR + ${DELAY:-0} / 1000));;
  *)
    POLLS=$(awk 'END{print $1}' "$PADLOG")
    RATE=${REPLAY_POLL_RATE:-15}
    DUR=$(( (POLLS + ${DELAY:-0}) / RATE + 15 ));;
esac
cd "$BUILD" || exit 1
snap() { DISPLAY=$DISP ffmpeg -y -f x11grab -video_size 1280x720 -i $DISP -frames:v 1 \
         "$OUT/$1.png" >/dev/null 2>&1; }
# Retry loop: ~1 in 3 headless launches ends on a uniform blank screen (the
# Xvfb/no-DRI3 artifact, task #14) -- without retrying, a delay sweep just
# measures which attempts got unlucky. Four earlier sweeps were wasted exactly
# that way before this was added.
uniform() {  # $1=png -> 0 if the frame is a blank artifact
  python3 - "$1" <<'PYEOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB').resize((80, 45))
px = list(im.getdata()); n = len(px)
lum = sum(0.2126*p[0]+0.7152*p[1]+0.0722*p[2] for p in px)/n
var = sum((0.2126*p[0]+0.7152*p[1]+0.0722*p[2]-lum)**2 for p in px)/n
sys.exit(0 if var < 60 else 1)
PYEOF
}
for attempt in 1 2 3 4; do
  rm -f "$OUT"/W*.png 2>/dev/null
  pgrep -x restuff >/dev/null || rm -f /dev/shm/xenia_memory_* 2>/dev/null
  # Clear stray HARNESS instances only -- never a game the USER launched.
  # Discriminate by environ: every harness boot carries RESTUFF_PAD_REPLAY
  # (this script sets it), a user launch does not. The user hit the sign-in
  # prompt in a LIVE session on 2026-08-14 while a background batch was
  # running; an unconditional `pkill -9 -x restuff` here would have killed
  # their game mid-play the moment the next drive started.
  for _p in $(pgrep -x restuff); do
    if tr '\0' '\n' < /proc/$_p/environ 2>/dev/null | grep -q '^RESTUFF_PAD_REPLAY='; then
      kill -9 $_p 2>/dev/null
    else
      echo "[replay] NOT killing pid $_p: no RESTUFF_PAD_REPLAY in environ -- user's own session"
    fi
  done
  sleep 1
  if [ -z "${RESTUFF_DISP:-}" ]; then
    # Scope to OUR display: a bare `pkill -x Xvfb` would take down any other
    # headless X server on the box (other sessions run their own).
    pkill -f "Xvfb $DISP " 2>/dev/null
    Xvfb $DISP -screen 0 1280x720x24 >/dev/null 2>&1 & sleep 2
  fi
  TS=$(date '+%Y-%m-%d %H:%M:%S')
  # RESTUFF_WRAP: optional command prefix for the game (e.g. "taskset -c 0-5"
  # for the console-like-scheduling experiments, task #24).
  env $EXTRA RESTUFF_PAD_REPLAY="$PADLOG" DISPLAY=$DISP SDL_VIDEODRIVER=x11 \
      ${RESTUFF_WRAP:-} ./restuff >"$OUT/run.log" 2>&1 & GAME=$!
  echo "[replay] attempt $attempt: booting; recording is ${DUR}s"
  # Poll for the boot marker instead of a fixed 45s: a cold-cache boot can take
  # >45s and a fixed window kills a HEALTHY boot (it did, Aug 4). The marker
  # fires at graphics init (~1s after launch), so waiting longer only delays the
  # snap burst, never mis-times it.
  BOOTED=no
  for _ in $(seq 1 24); do
    sleep 5
    if awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null | grep -q "$BOOT_MARKER"; then
      BOOTED=yes; break
    fi
    kill -0 $GAME 2>/dev/null || break
  done
  if [ "$BOOTED" = no ]; then
    echo "[replay]   no boot"; kill -9 $GAME 2>/dev/null
    [ -z "${RESTUFF_DISP:-}" ] && pkill -f "Xvfb $DISP " 2>/dev/null
    continue
  fi
  # +30: trailing pad state is held after the route ends, so overshooting just
  # holds the final viewpoint; undershooting truncates the route.
  # SNAP_EARLY=<s>: begin the snap burst before the route ends, so a burst can
  # sample a camera SWEEP in progress (view-dependent artifacts need several
  # angles, not one held viewpoint).
  sleep "$((DUR + 30 - ${SNAP_EARLY:-0}))"
  # SNAP_N / SNAP_GAP: longer bursts for camera-sweep routes (the wedge is
  # view-dependent, so a single held viewpoint cannot show it appearing and
  # disappearing). Defaults keep the original 6-frame, 2s-apart burst.
  for i in $(seq 0 $(( ${SNAP_N:-6} - 1 ))); do
    kill -0 $GAME 2>/dev/null || break
    snap "W$(printf %02d $i)"; sleep "${SNAP_GAP:-2}"
  done
  kill -9 $GAME 2>/dev/null
  [ -z "${RESTUFF_DISP:-}" ] && pkill -f "Xvfb $DISP " 2>/dev/null
  last=$(ls "$OUT"/W*.png 2>/dev/null | tail -1)
  if [ -n "$last" ] && ! uniform "$last"; then
    echo "[replay] done (attempt $attempt) -> $OUT"; exit 0
  fi
  echo "[replay]   blank-screen artifact, retrying"
done
echo "[replay] FAILED: no non-blank run in 4 attempts -> $OUT"

#!/bin/bash
# Camera-matched A/B for the water-wedge defect (task #24).
#
#   tools/wedge_ab.sh <tag> [ENV=VAL ...]
#
# WHY THIS EXISTS: replay drift lands the bear in different spots, and the
# amount of terrain in view changes with it -- so a bare "wedge? yes/no" per
# run is confounded by camera position. The VSCOUNT `indices=` total is a
# camera fingerprint: runs sharing it are looking at the same geometry set.
# The user's door viewpoint is indices=427617, where the defect is bimodal:
#   427617 with draws=285  -> wedge boot
#   427617 with draws=79   -> clean boot
# Only runs matching CAM_INDICES count; everything else is DISCARDED, not
# averaged in. (Two earlier conclusions died to exactly this confound.)
#
# Wedge presence is measured from the frames, not inferred from the draw
# count: the water wedge is a blue region in the lower-left ground area, and
# it is perfectly stable within a boot (29% vs 0% of samples, no flicker).
set -u
# ⚠️ NEVER edit this file while a run is in flight: bash reads scripts
# INCREMENTALLY, so an edit lands mid-execution and the running instance dies
# with a syntax error at whatever line it reaches next (it did, mid-arm).
TAG="${1:?usage: wedge_ab.sh <tag> [ENV=VAL ...]}"; shift || true
ENVKV="${*:-RESTUFF_UNUSED=0}"
CAM_INDICES="${CAM_INDICES:-427617}"
RUNS="${RUNS:-6}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$ROOT/out/build/linux-amd64-relwithdebinfo"
SUM="/tmp/restuff_drive/wedge_ab_$TAG.txt"
: > "$SUM"
# Serialise drives with a LOCK, not a poll-for-idle loop: two harnesses polling
# "is restuff running?" both fire during the gap BETWEEN a run's boot attempts
# and then kill each other's game (replay_drive.sh clears stray instances).
# That silently invalidated two arms before this was added.
exec 9>/tmp/restuff_drive/.drive.lock
# -w 120 + loud failure: an orphaned child that inherited fd 9 once held this
# flock for 8h (a harness reset killed the batch; its setsid'd tail lived on)
# and four successive batches queued behind it in SILENCE — zero boots all
# morning. A stale lock must fail and NAME the holder, never hang. Spawns
# below get 9>&- so no child can carry the lock past this script's death.
if ! flock -w 120 9; then
  echo "ABORT: drive lock busy >120s; fd holders:" | tee -a "$SUM"
  find /proc/[0-9]*/fd -lname /tmp/restuff_drive/.drive.lock 2>/dev/null \
    | cut -d/ -f3 | sort -u | xargs -r ps -o pid,etimes,args -p | tee -a "$SUM"
  exit 3
fi

gameplay() {  # $1=png -> "yes" if this frame is actually IN GAMEPLAY
  # A run stuck on the "Would you like to sign in with a gamer profile?" dialog
  # still produced a full verdict: MATCHED cam=427617 draws=79 wedge=4%, while
  # never reaching gameplay (user-caught, sel13/sel14). TWO defects combined --
  # the stale-VSCOUNT inheritance fixed below, and the scorer reading the pale
  # BLUE dialog box as water. The frame is the ground truth, so gate on it.
  # The objective text ("Points to go:") is orange, bottom-left, and present
  # only in gameplay: 8% on every real frame today, 0% on both dialog frames.
  python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
n = t = 0
for y in range(600, 690, 2):
    for x in range(50, 420, 2):
        r, g, b = px[x, y]; t += 1
        if r > 170 and 70 < g < 200 and b < 110: n += 1
print("yes" if 100 * n // max(t, 1) >= 3 else "no")
PY
}

score() {  # $1=png -> percent of the WHOLE frame that is water-blue
  # The old version sampled only x[80,560) y[440,620), so a wedge rendering
  # higher or further left scored 0%. Full frame, minus the HUD strip and the
  # minimap. Canonical clean frames ~0.2%, wedges 4-5%; call it at >=1.5%.
  python3 - "$1" <<'PYSCORE'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load(); W, H = im.size
n = t = 0
for y in range(150, H - 40, 3):
    for x in range(0, W, 3):
        if x > 995 and y > 495: continue      # minimap circle
        r, g, b = px[x, y]; t += 1
        if b > r + 18 and 40 < b < 175 and g > r - 10: n += 1
print(round(100.0 * n / max(t, 1), 2))
PYSCORE
}

campos() {  # $1=png -> how far this frame's camera is from the canonical one
  # cam=<indexcount> does NOT pin the camera: keyhist12/13 matched it while
  # sitting somewhere else entirely. Compare the UPPER HALF (door/tree/terrain)
  # and NOT the lower area, which is where the wedge renders -- the gate must
  # not react to the defect it gates. Calibrated on eyeballed frames:
  #   canonical 0.0 1.4 3.1 8.1 9.7 9.9  |  bad cam 15.5 22.1  |  dialog 85.8
  python3 - "$1" "$ROOT/tools/refs/canon_top.png" <<'PYCAM'
import sys
from PIL import Image
def sig(p, crop):
    im = Image.open(p).convert('L')
    if crop: im = im.crop((0, 140, 1280, 420))
    return list(im.resize((40, 10)).getdata())
a = sig(sys.argv[1], True); b = sig(sys.argv[2], False)
print(round(sum(abs(x - y) for x, y in zip(a, b)) / len(a), 1))
PYCAM
}

matched=0
for i in $(seq 1 "$RUNS"); do
  TS=$(date '+%Y-%m-%d %H:%M:%S')
  # Heartbeat to stdout (-> the batch .out file): a 0-byte .out previously
  # meant either "still on run 1" or "never started" — indistinguishable.
  echo "[$TS] ${TAG}$i launching (each attempt may take ~20min incl. retries)"
  # Live-tail LULOG lines during the run: they are emitted at LOAD TIME and
  # the 5MB log rotation sheds them long before run end (the instr batch kept
  # endpoint CHUNKDUMPs but lost every LULOG line).
  D="/tmp/restuff_drive/env_${TAG}$i"; mkdir -p "$D"
  # setsid gives the pipeline its own process GROUP so the kill below can take
  # the whole thing down; kill $! alone left the tail running (12 orphans ran
  # 11h and cross-contaminated every instr_live.log with later runs' events).
  setsid bash -c "tail -n0 -F '$B/restuff_dbg.log' 2>/dev/null | grep --line-buffered -a 'LULOG\|CHUNKDUMP' > '$D/instr_live.log'" 9>&- &
  TAILPID=$!
  timeout 1200 "$ROOT/tools/replay_drive.sh" /tmp/hole.padlog "${TAG}$i" \
    "RESTUFF_PAD_REPLAY_DELAY=900 RESTUFF_VSCOUNT=C6C4FBF7D18A3E61 $ENVKV" >/dev/null 2>&1 9>&-
  kill -- -$TAILPID 2>/dev/null   # negative pid = the whole setsid group
  # Window start = THIS RUN'S OWN game start, not the loop's TS. TS is taken
  # BEFORE replay_drive.sh clears stray games, so a lingering game from the
  # previous run keeps emitting VSCOUNT inside this run's window and the run
  # silently INHERITS the previous verdict -- sel13 reported MATCHED
  # cam=427617 draws=79 while its frame sat on the sign-in dialog, having
  # inherited sel12's numbers. LUCON.TTF is the per-boot marker (a probe that
  # always fails and always logs); its LAST occurrence is this run's final
  # boot attempt.
  GS=$(grep -ah "LUCON.TTF" "$B"/restuff_dbg.log "$B"/restuff_dbg.1.log 2>/dev/null \
       | awk -v ts="$TS" 'substr($0,2,19) >= ts' | tail -1 | cut -c2-20)
  [ -n "$GS" ] || GS="$TS"
  LINE=$(grep -ah VSCOUNT "$B"/restuff_dbg.log "$B"/restuff_dbg.1.log 2>/dev/null \
         | awk -v ts="$GS" 'substr($0,2,19) >= ts' | tail -1)
  IDX=$(printf '%s' "$LINE" | grep -oE 'indices=[0-9]+' | cut -d= -f2)
  # head -1: the VSCOUNT line also carries frame_draws=, which the pattern
  # would otherwise match a second time and print as a bogus second value.
  DRW=$(printf '%s' "$LINE" | grep -oE '(^| )draws=[0-9]+' | head -1 | cut -d= -f2)
  D="/tmp/restuff_drive/env_${TAG}$i"
  F="$D/W02.png"
  # A missing frame is usually a CRASH, not a quiet miss -- say so, or the run
  # reads as an ordinary NA and a broken arm looks merely inconclusive.
  if [ -f "$F" ]; then
    if [ "$(gameplay "$F")" = "no" ]; then
      S=NOTGAMEPLAY   # menu/dialog frame: any score off it is meaningless
    else
      S=$(score "$F")
    fi
  elif grep -qaE "corrupted|Segmentation|X connection to :99 broken" "$D/run.log" 2>/dev/null; then
    S=CRASH
  else
    S=NA
  fi
  # Preserve per-run instrument lines before the rotating logs shed them:
  # CHUNKDUMP (endpoint chunk sets) and LULOG (streamer transitions) power the
  # fine+clean vs fine+wedge diff.
  grep -ah "CHUNKDUMP\|LULOG" "$B"/restuff_dbg.log "$B"/restuff_dbg.1.log 2>/dev/null \
    | awk -v ts="$TS" 'substr($0,2,19) >= ts' > "$D/instr.log" 2>/dev/null
  if [ "$S" = "NOTGAMEPLAY" ]; then
    # Never counts, whatever the camera says: a run that never reached
    # gameplay can still carry a matching cam/draws pair.
    echo "${TAG}$i discarded (NOT GAMEPLAY -- menu/dialog frame; cam=${IDX:-none} draws=${DRW:-?})" >> "$SUM"
  elif [ "${IDX:-0}" = "$CAM_INDICES" ]; then
    CP=$(campos "$F" 2>/dev/null || echo 999)
    if awk "BEGIN{exit !($CP >= 12)}"; then
      # cam=<indexcount> matched but the camera is somewhere else entirely
      # (keyhist12/13 did exactly this). Scoring such a frame is meaningless.
      echo "${TAG}$i discarded (CAMERA MISMATCH camdist=$CP) cam=$IDX draws=${DRW:-?} wedge=${S}%" >> "$SUM"
    else
      matched=$((matched + 1))
      echo "${TAG}$i MATCHED cam=$IDX camdist=$CP draws=${DRW:-?} wedge=${S}%" >> "$SUM"
    fi
  else
    echo "${TAG}$i discarded (cam=${IDX:-none} != $CAM_INDICES) draws=${DRW:-?} wedge=${S}%" >> "$SUM"
  fi
done
echo "matched_runs=$matched of $RUNS" >> "$SUM"
cat "$SUM"

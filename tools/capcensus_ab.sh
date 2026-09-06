#!/bin/bash
# Capture-side accounting for the wedge hunt (task #24): run the user's route
# until BOTH boot outcomes are sampled, and print each one's CAPCENSUS line.
#
#   tools/capcensus_ab.sh [max_runs]
#
# The record path is already exonerated (M3.162 RECSTAT: recorded==seen, all
# skip counters zero), so what remains is whether CaptureTranslatedDraw drops
# the missing ground chunk or the guest never submits it. CAPCENSUS reports
# in/out plus a count per bail reason; CAPDROP-DISTINCT names the shaders.
#
# Waits for any in-flight drive first -- two restuff instances fight over the
# GPU and the Xvfb display, which has invalidated whole runs before.
set -u
MAX="${1:-6}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$ROOT/out/build/linux-amd64-relwithdebinfo"
SUM=/tmp/restuff_drive/capcensus_summary.txt
: > "$SUM"
while pgrep -x restuff >/dev/null 2>&1; do sleep 15; done
gotf=0; gotl=0
for i in $(seq 1 "$MAX"); do
  TS=$(date '+%Y-%m-%d %H:%M:%S')
  timeout 1200 "$ROOT/tools/replay_drive.sh" /tmp/hole.padlog cc$i \
    "RESTUFF_PAD_REPLAY_DELAY=900 RESTUFF_VSCOUNT=C6C4FBF7D18A3E61 RESTUFF_CAP_CENSUS=1" \
    >/dev/null 2>&1
  V=$(grep -ah VSCOUNT "$B"/restuff_dbg.log "$B"/restuff_dbg.1.log 2>/dev/null \
      | awk -v ts="$TS" 'substr($0,2,19) >= ts' | tail -1 | grep -oE "draws=[0-9]+" | head -1)
  C=$(grep -ah CAPCENSUS "$B"/restuff_dbg.log "$B"/restuff_dbg.1.log 2>/dev/null \
      | awk -v ts="$TS" 'substr($0,2,19) >= ts' | tail -1 | sed 's/.*CAPCENSUS //')
  D=$(grep -ah CAPDROP-DISTINCT "$B"/restuff_dbg.log "$B"/restuff_dbg.1.log 2>/dev/null \
      | awk -v ts="$TS" 'substr($0,2,19) >= ts' | wc -l)
  echo "cc$i ${V:-NO_DATA} distinct_drop_shaders=$D | ${C:-none}" >> "$SUM"
  case "$V" in
    draws=79|draws=77|draws=8*) gotf=1;;
    draws=2*) gotl=1;;
  esac
  [ "$gotf" = 1 ] && [ "$gotl" = 1 ] && break
done
cat "$SUM"

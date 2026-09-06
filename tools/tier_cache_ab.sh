#!/bin/bash
# A/B: does a COLD driver pipeline cache at level load push the guest into the
# LOW terrain tier (draws~285 = wedges) vs warm (draws~79 = clean)?
#
#   tools/tier_cache_ab.sh [pairs]
#
# Runs cold,warm per pair BACK-TO-BACK so machine conditions match within a
# pair. Readout = the endpoint [VSCOUNT] draws count for the terrain shader.
# Load average is recorded per arm -- an arm that ran under contention (or
# never booted) must be discarded, not interpreted.
set -u
PAIRS="${1:-2}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="$ROOT/out/build/linux-amd64-relwithdebinfo"
CACHE="$BUILD/pipeline_cache.bin"
STASH="/tmp/restuff_drive/pipeline_cache.stash"
SUM="/tmp/restuff_drive/tier_ab_summary.txt"
: > "$SUM"
restore() { [ -f "$STASH" ] && mv -f "$STASH" "$CACHE" && echo "[tier_ab] cache restored" >> "$SUM"; }
trap restore EXIT INT TERM
ENVS="RESTUFF_PAD_REPLAY_DELAY=900 RESTUFF_VSCOUNT=C6C4FBF7D18A3E61 RESTUFF_VSHIT=300,545"

arm() {  # $1=label $2=cold|warm
  local TS=$(date '+%Y-%m-%d %H:%M:%S')
  local L0=$(awk '{print $1}' /proc/loadavg)
  timeout 1200 "$ROOT/tools/replay_drive.sh" /tmp/hole.padlog "ab_$1" "$ENVS" >/dev/null 2>&1
  local RES=$?
  local L1=$(awk '{print $1}' /proc/loadavg)
  # newest VSCOUNT line after TS across rotated logs
  local V=$(grep -ah "VSCOUNT" "$BUILD"/restuff_dbg.log "$BUILD"/restuff_dbg.1.log 2>/dev/null \
            | awk -v ts="$TS" 'substr($0,2,19) >= ts' | tail -1 | grep -oE "draws=[0-9]+" )
  echo "$1 $2 rc=$RES load=$L0..$L1 ${V:-NO_DATA}" >> "$SUM"
}

for p in $(seq 1 "$PAIRS"); do
  mv -f "$CACHE" "$STASH" 2>/dev/null && echo "[tier_ab] pair $p: cache stashed (cold)" >> "$SUM"
  arm "p${p}cold" cold
  mv -f "$STASH" "$CACHE" 2>/dev/null && echo "[tier_ab] pair $p: cache restored (warm)" >> "$SUM"
  arm "p${p}warm" warm
done
echo "[tier_ab] DONE" >> "$SUM"
cat "$SUM"

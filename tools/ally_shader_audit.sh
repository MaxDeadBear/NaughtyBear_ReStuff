#!/bin/bash
# Offline "Ally emulation" for shader perf: compile every dumped translated
# shader with AMD's offline compiler stack (Radeon GPU Analyzer -> amdvlk/LLPC,
# the same compiler the friend's Z1 Extreme driver uses) for gfx1103 (Phoenix /
# 780M-class, the Ally Z1 Extreme iGPU) and report the two numbers that decide
# whether the GPU chokes: scratch (spill) bytes and VGPR count per shader.
#
# Usage:
#   1) Produce the dump: run any drive/session with RESTUFF_DUMP_GLSL=1
#      (cold pipeline cache: rm pipeline_cache.bin) -> glsl_dump/ beside the exe.
#   2) tools/ally_shader_audit.sh [glsl_dump_dir] [asic]
#
# Default dump dir: out/build/linux-amd64-relwithdebinfo/glsl_dump
# Default asic:     gfx1103
#
# Output: per-shader CSV + a sorted summary to stdout. Any shader with
# scratch > 0 is a spill -- the exact pathology behind the Ally 7-20fps runs
# (M4.30/M4.31). A clean audit = this class of regression cannot recur on
# RDNA3/LLPC, no hardware in the loop.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DUMP="${1:-$HERE/../out/build/linux-amd64-relwithdebinfo/glsl_dump}"
ASIC="${2:-gfx1103}"
RGA="${RGA_BIN:-/run/media/Tynan/Data/Restuff Project/re-tools/rga/rga-2.14.2.8/rga}"
OUT="${RESTUFF_SCRATCH:-/tmp/restuff_drive}/ally_audit"

[ -x "$RGA" ] || { echo "ABORT: rga not found at $RGA (set RGA_BIN)"; exit 2; }
ls "$DUMP"/*.spv >/dev/null 2>&1 || { echo "ABORT: no .spv files in $DUMP"; exit 3; }

rm -rf "$OUT"; mkdir -p "$OUT"
CSV="$OUT/audit.csv"
echo "shader,stage,vgprs,scratch_bytes,status" > "$CSV"

n=0; fail=0
for spv in "$DUMP"/*.spv; do
  base="$(basename "$spv" .spv)"
  case "$base" in
    vs_*)    stageflag=--vert; stage=vs ;;
    ps_*)    stageflag=--frag; stage=ps ;;
    probe_*) stageflag=--comp; stage=cs ;;
    *) continue ;;
  esac
  workdir="$OUT/$base"; mkdir -p "$workdir"
  if ! "$RGA" -s vk-spv-offline -c "$ASIC" "$stageflag" "$spv" \
        -a "$workdir/stats.csv" --isa "$workdir/isa.txt" \
        > "$workdir/rga.log" 2>&1; then
    echo "$base,$stage,,,COMPILE-FAIL" >> "$CSV"; fail=$((fail+1)); continue
  fi
  # RGA writes one stats csv per stage with the asic in the name; grab any.
  statfile="$(ls "$workdir"/*stats*.csv 2>/dev/null | head -1)"
  if [ -z "$statfile" ]; then
    echo "$base,$stage,,,NO-STATS" >> "$CSV"; fail=$((fail+1)); continue
  fi
  # Column names vary slightly across RGA versions; locate by header.
  read -r vgpr scratch < <(python3 - "$statfile" <<'EOF'
import csv, sys
with open(sys.argv[1]) as f:
    rows = list(csv.reader(f))
hdr = [h.strip().lower().replace("_", " ") for h in rows[0]]
def col(*keys):
    for i, h in enumerate(hdr):
        if any(k in h for k in keys):
            return i
    return None
iv = col("used vgprs")
isc = col("scratch")
r = rows[1]
print(r[iv].strip() if iv is not None else "?",
      r[isc].strip() if isc is not None else "?")
EOF
)
  echo "$base,$stage,$vgpr,$scratch,ok" >> "$CSV"
  n=$((n+1))
done

echo
echo "=== Ally shader audit: $ASIC, $n compiled, $fail failed ==="
echo "--- worst 15 by scratch (spill) bytes ---"
tail -n +2 "$CSV" | sort -t, -k4 -rn | head -15 | column -t -s,
spills=$(tail -n +2 "$CSV" | awk -F, '$4+0 > 0' | wc -l)
echo
echo "SPILLING SHADERS: $spills   (must be 0 for a healthy Ally)"
echo "full csv: $CSV"
[ "$spills" -eq 0 ] && [ "$fail" -eq 0 ]

#!/bin/bash
# Hunt a SAME-CAMERA RenderDoc capture pair for the wedge (task #24): one boot
# that shows the wedge and one that does not, BOTH at the door viewpoint
# (VSCOUNT indices=427617). Only such a pair supports a draw-for-draw
# comparison -- the first pair I compared turned out to be ~5 world units
# apart and every conclusion drawn from it had to be retracted.
#
#   tools/capture_pair.sh [max_runs]
#
# Keeps captures only from matching runs, names them by outcome, and stops as
# soon as it holds one of each.
set -u
MAX="${1:-8}"
CAM="${CAM_INDICES:-427617}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
B="$ROOT/out/build/linux-amd64-relwithdebinfo"
OUT=/tmp/restuff_drive/rdc
SUM=/tmp/restuff_drive/capture_pair.txt
mkdir -p "$OUT"; : > "$SUM"
exec 9>/tmp/restuff_drive/.drive.lock
flock 9

score() {
  python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); px = im.load()
n = t = 0
for y in range(440, 620, 4):
    for x in range(80, 560, 4):
        r, g, b = px[x, y]; t += 1
        if b > r + 18 and 40 < b < 175 and g > r - 10: n += 1
print(100 * n // max(t, 1))
PY
}

have_wedge=0; have_clean=0
for i in $(seq 1 "$MAX"); do
  rm -f "$OUT"/pair_*.rdc 2>/dev/null
  TS=$(date '+%Y-%m-%d %H:%M:%S')
  timeout 1300 "$ROOT/tools/replay_drive.sh" /tmp/hole.padlog pair$i \
    "RESTUFF_PAD_REPLAY_DELAY=900 RESTUFF_VSCOUNT=C6C4FBF7D18A3E61 \
     ENABLE_VULKAN_RENDERDOC_CAPTURE=1 RESTUFF_RDC_FRAME=900,1 RESTUFF_RDC_PATH=$OUT/pair" \
    >/dev/null 2>&1
  LINE=$(grep -ah VSCOUNT "$B"/restuff_dbg.log "$B"/restuff_dbg.1.log 2>/dev/null \
         | awk -v ts="$TS" 'substr($0,2,19) >= ts' | tail -1)
  IDX=$(printf '%s' "$LINE" | grep -oE 'indices=[0-9]+' | cut -d= -f2)
  DRW=$(printf '%s' "$LINE" | grep -oE '(^| )draws=[0-9]+' | head -1 | cut -d= -f2)
  F="/tmp/restuff_drive/env_pair$i/W02.png"
  S=$([ -f "$F" ] && score "$F" || echo NA)
  CAPTURE=$(ls -t "$OUT"/pair_*.rdc 2>/dev/null | head -1)
  VERDICT=discarded
  # FINE-FAMILY MODE (FINE_ONLY=1): both captures must be FINE-partition boots
  # (draws > 200) -- the merged/fine comparison is done; the open question is
  # what covers the foreground in a fine boot WITHOUT the wedge.
  MIN_DRAWS=$([ "${FINE_ONLY:-0}" = 1 ] && echo 200 || echo 0)
  if [ "${IDX:-0}" = "$CAM" ] && [ -n "$CAPTURE" ] && [ "$S" != NA ] \
     && [ "${DRW:-0}" -gt "$MIN_DRAWS" ]; then
    if [ "$S" -ge 15 ] && [ "$have_wedge" = 0 ]; then
      mv "$CAPTURE" "$OUT/PAIR_WEDGE.rdc"; have_wedge=1; VERDICT=kept_WEDGE
    elif [ "$S" -lt 3 ] && [ "$have_clean" = 0 ]; then
      mv "$CAPTURE" "$OUT/PAIR_CLEAN.rdc"; have_clean=1; VERDICT=kept_CLEAN
    fi
  elif [ "${ALLOW_NEAR_CLEAN:-0}" = 1 ] && [ -n "$CAPTURE" ] && [ "$S" != NA ] \
       && [ "${DRW:-0}" -gt "$MIN_DRAWS" ] && [ "$S" -lt 3 ] && [ "$have_clean" = 0 ]; then
    # WORLD-SPACE geometry diffs don't need the exact camera (vertex positions
    # are view-independent); a near-camera fine+clean capture is fully usable
    # for "where are A's foreground triangles" -- only PIXEL comparisons need
    # the exact fingerprint. Tag it distinctly so nobody mistakes it.
    mv "$CAPTURE" "$OUT/PAIR_CLEAN_NEARCAM_${IDX:-none}.rdc"; have_clean=1; VERDICT=kept_CLEAN_NEARCAM
  fi
  echo "pair$i cam=${IDX:-none} draws=${DRW:-?} wedge=${S}% $VERDICT" >> "$SUM"
  [ "$have_wedge" = 1 ] && [ "$have_clean" = 1 ] && break
done
echo "have_wedge=$have_wedge have_clean=$have_clean" >> "$SUM"
cat "$SUM"

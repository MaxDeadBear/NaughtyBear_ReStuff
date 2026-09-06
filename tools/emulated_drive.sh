#!/bin/bash
# Run the gate drive through the EMULATED renderer (the xenos plugin) instead of
# our native one, so a defect can be compared against the reference.
#
#   tools/emulated_drive.sh <tag>
#
# use_native_renderer lives in restuff.toml, not the environment, so this edits
# the staged config and ALWAYS restores it via a trap -- a previous inline
# attempt at this class of thing left a modified toml behind when it was killed.
# The emulated path has no swaps counter, hence GATE_NO_SWAPCHECK.
set -u
TAG="${1:-emu}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOML="$ROOT/out/build/linux-amd64-relwithdebinfo/restuff.toml"
BAK="/tmp/restuff_toml_emu.bak"
cp "$TOML" "$BAK" || exit 1
restore() { cp -f "$BAK" "$TOML"; echo "[emulated_drive] restuff.toml restored"; }
trap restore EXIT INT TERM
if grep -q '^use_native_renderer' "$TOML"; then
  sed -i 's/^use_native_renderer.*/use_native_renderer = false/' "$TOML"
else
  printf '\n# TEMP (auto-restored by tools/emulated_drive.sh)\nuse_native_renderer = false\n' >> "$TOML"
fi
echo "[emulated_drive] use_native_renderer = false; driving..."
GATE_NO_SWAPCHECK=1 timeout 520 "$ROOT/tools/gate_drive.sh" "RESTUFF_UNUSED=0" "$TAG"

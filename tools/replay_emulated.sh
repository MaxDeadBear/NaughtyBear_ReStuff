#!/bin/bash
# Replay a human pad recording through the EMULATED renderer (xenos plugin) for
# a reference image of the same viewpoint.
#
#   tools/replay_emulated.sh <padlog> <tag> [extra ENV=VAL ...]
#
# Same toml flip/restore discipline as emulated_drive.sh (trap-restored); the
# actual replay mechanics are delegated to replay_drive.sh so the two arms stay
# in lockstep. ⚠️ Do not run while another drive is active: the toml is global
# to the build dir.
set -u
PADLOG="${1:?usage: replay_emulated.sh <padlog> <tag> [ENV=VAL ...]}"
TAG="${2:-emuref}"; shift 2 || true
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOML="$ROOT/out/build/linux-amd64-relwithdebinfo/restuff.toml"
BAK="/tmp/restuff_toml_replayemu.bak"
if pgrep -x restuff >/dev/null; then
  echo "[replay_emulated] refusing: a restuff drive is already running" >&2
  exit 1
fi
cp "$TOML" "$BAK" || exit 1
restore() { cp -f "$BAK" "$TOML"; echo "[replay_emulated] restuff.toml restored"; }
trap restore EXIT INT TERM
if grep -q '^use_native_renderer' "$TOML"; then
  sed -i 's/^use_native_renderer.*/use_native_renderer = false/' "$TOML"
else
  printf '\n# TEMP (auto-restored by tools/replay_emulated.sh)\nuse_native_renderer = false\n' >> "$TOML"
fi
echo "[replay_emulated] use_native_renderer = false; replaying..."
# Boot marker: must be a line that ACTUALLY APPEARS in restuff_dbg.log.
# "engine.ini" stopped working the day assets/engine.ini shipped (fb06067):
# NtCreateFile only logs FAILURES, and the probe now succeeds silently -- so
# every emulated run was declared 'no boot' while the guest ran fine (this
# also retro-invalidates the 'Xvfb kills the SDK presenter' reading of those
# NAs). LUCON.TTF does not exist and still fails+logs every boot -- the same
# marker the shimretry recipe uses.
BOOT_MARKER="LUCON.TTF" REPLAY_POLL_RATE="${REPLAY_POLL_RATE:-6}" \
  "$ROOT/tools/replay_drive.sh" "$PADLOG" "$TAG" "$@"

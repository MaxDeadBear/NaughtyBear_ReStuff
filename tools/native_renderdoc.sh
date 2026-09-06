#!/usr/bin/env bash
# Launch OUR native Vulkan renderer (the dim side) with RenderDoc's capture
# layer enabled, so the same gate draw can be captured and diffed 1:1 against
# the emulated reference (NaughtyBear_ReStuff-difficulty/play_renderdoc.sh).
#
# Native is a plain Linux Vulkan app, so this is straightforward:
#   1) qrenderdoc &
#   2) tools/native_renderdoc.sh
#   3) File > Attach to Running Instance -> pick "restuff".
#   4) Navigate to the gate, click "Trigger Capture".
#
# Capture the SAME view in both builds. Then in the Texture Viewer, walk the
# event list to the big terrain/ground draw (VS c6c4fbf7 / PS 63c0650d, the
# n~25000 strip) and read the bound colour render target's pixel values right
# after it -- reference should be ~2x our values. Also inspect the shadow /
# small render targets (the reference's surf 720/360/680) to see whether ours
# are built the same.
set -u
HERE="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$HERE/out/build/linux-amd64-relwithdebinfo"
[ -x "$BUILD/restuff" ] || { echo "no restuff binary at $BUILD"; exit 2; }

export ENABLE_VULKAN_RENDERDOC_CAPTURE=1
cd "$BUILD" || exit 3
echo "[native_renderdoc] launching native renderer with RenderDoc layer; attach qrenderdoc now."
exec ./restuff

#!/bin/bash
# Launch restuff, preserve this run's log slice, and AUTO-CAPTURE a stack dump
# if the boot hangs.
#
#   tools/keepboot.sh [ENV=VAL ...]
#
# Why this exists: restuff_dbg.log is APPENDED across runs and rotates at 5MB,
# so a batch of headless drives can push a real failed-boot log out of the
# retained window entirely (it did -- six of them were lost that way).
#
# Why it also dumps stacks: the failure is "very early -- if the title screen
# appears at all, the boot succeeded", and the process stays ALIVE while stuck,
# so a backtrace names it outright. Every headless capture so far blocks the UI
# thread inside VulkanPresenter::PaintAndPresentImpl, but headless goes through
# an xcb_put_image software path that a real DRI3/Present display does NOT use
# -- so a stack from a REAL display is the missing evidence, and this grabs it
# with no manual steps.
#
# Saved to: <build>/boot_logs/<timestamp>.log  (+ .stacks.txt if it hung)
set -u
BUILD="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo"
LOG="$BUILD/restuff_dbg.log"; OUT="$BUILD/boot_logs"; mkdir -p "$OUT"
HANG_AFTER="${HANG_AFTER:-40}"   # seconds without the boot marker => call it hung
cd "$BUILD" || exit 1
TS=$(date '+%Y-%m-%d %H:%M:%S'); STAMP=$(date '+%Y%m%d-%H%M%S')
echo "[keepboot] starting; log -> $OUT/$STAMP.log (stack dump if no boot in ${HANG_AFTER}s)"
env "$@" ./restuff & GAME=$!

# Watchdog: if the boot marker never lands and the process is still alive, that
# is the hang -- dump every thread before anything can clear it.
(
  sleep "$HANG_AFTER"
  kill -0 "$GAME" 2>/dev/null || exit 0
  if awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" 2>/dev/null \
     | grep -q "present thread started"; then exit 0; fi
  echo "[keepboot] NO BOOT after ${HANG_AFTER}s and still alive -- dumping stacks"
  { gdb -p "$GAME" -batch -ex "set pagination off" -ex "thread apply all bt 30" 2>&1 \
    || eu-stack -p "$GAME" 2>&1; } > "$OUT/$STAMP.stacks.txt"
  echo "[keepboot] stacks -> $OUT/$STAMP.stacks.txt"
) & WATCH=$!

wait "$GAME"; RC=$?
kill "$WATCH" 2>/dev/null
awk -v ts="$TS" 'substr($0,2,19) >= ts' "$LOG" > "$OUT/$STAMP.log" 2>/dev/null
echo "[keepboot] exit=$RC, $(wc -l < "$OUT/$STAMP.log") lines -> $OUT/$STAMP.log"
[ -s "$OUT/$STAMP.stacks.txt" ] && echo "[keepboot] ⚠ a hang was captured: $OUT/$STAMP.stacks.txt"
exit 0

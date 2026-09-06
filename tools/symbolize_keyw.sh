#!/bin/bash
# Symbolize KEYW/HWBP hardware-watchpoint hits to guest function names.
#
#   tools/symbolize_keyw.sh <mergelog>
#
# The keywatch/hwbp poller logs "off=<hex>" = the faulting IP minus the exe
# text base. But the exe LOAD segment's VirtAddr (0x35e000) must be added
# before llvm-symbolizer, and data breakpoints trap AFTER the store, so the
# real writer is usually the instruction just before — cross-check the
# symbolized line against the recomp source. Named the composer sub_82AD3050
# this way (offset 0x20BDD47 -> 0x241bd47).
#
# ⚠️⚠️ SAME-BINARY ONLY. The offset is relative to the exe TEXT of the build
# that produced the log — every rebuild shifts function layout, so an offset
# from an old log resolves to the WRONG function against a newer binary (proof:
# the Aug-10 kw1 offset 0x20BDD47 = sub_82AD3050 in that build, but
# sub_82AD2CE0 against the Aug-11 rebuild). Run this against the binary that
# generated the log, before rebuilding.
set -u
LOG="${1:?usage: symbolize_keyw.sh <mergelog>}"
BIN="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo/restuff"
# exe text LOAD VirtAddr (R-E segment); re-read if the binary is rebuilt.
VA=$(readelf -lW "$BIN" 2>/dev/null | awk '/LOAD/ && /R E/ {print $3; exit}')
[ -n "$VA" ] || VA=0x35e000
echo "text VirtAddr=$VA  binary=$BIN"
grep -hoE "(KEYW|HWBP)[^\\n]*off=[0-9A-Fa-f]+" "$LOG" 2>/dev/null \
  | grep -oE "off=[0-9A-Fa-f]+" | sort -u | while read -r o; do
  off=${o#off=}
  [ "$off" = "0" ] && continue
  addr=$(python3 -c "print(hex($VA + 0x$off))")
  sym=$(llvm-symbolizer --obj="$BIN" --functions=linkage --demangle "$addr" 2>/dev/null | head -2 | tr '\n' ' ')
  echo "off=0x$off -> $addr : $sym"
done

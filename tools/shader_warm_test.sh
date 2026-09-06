#!/bin/bash
# ===========================================================================
# M4.40/41/42 shader-cache test helper.
#
#   tools/shader_warm_test.sh status              what the caches look like now
#   tools/shader_warm_test.sh cold                delete both caches (first-run)
#   tools/shader_warm_test.sh fake-driver         pretend the driver updated
#   tools/shader_warm_test.sh fake-build          pretend the game was updated (M4.45)
#   tools/shader_warm_test.sh fake-gpu            pretend the GPU changed
#   tools/shader_warm_test.sh restore             put the saved-good cache back
#
# Point it at whichever folder you actually launch from:
#   RESTUFF_DIR=../ReStuff-linux64 tools/shader_warm_test.sh status
#
# fake-driver / fake-gpu edit only the 32-byte Vulkan cache header, so they are
# exactly the conditions a real driver update or GPU swap produce, without
# needing either. Every mode saves a copy first; `restore` undoes it.
# ===========================================================================
set -u
DIR="${RESTUFF_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/out/build/linux-amd64-relwithdebinfo}"
PC="$DIR/pipeline_cache.bin"
SPV="$DIR/shader_spv.bin"
BAK="$DIR/pipeline_cache.bin.testbak"
MODE="${1:-status}"

hdr() {
  [ -s "$PC" ] || { echo "  pipeline_cache.bin : (absent)"; return; }
  python3 - "$PC" <<'PY'
import sys
d=open(sys.argv[1],'rb').read()
if len(d)<32: print("  pipeline_cache.bin : too small (%d bytes)"%len(d)); raise SystemExit
i=lambda a,b:int.from_bytes(d[a:b],'little')
print("  pipeline_cache.bin : %.1f MB  vendorID=0x%04X deviceID=0x%04X" % (len(d)/1048576, i(8,12), i(12,16)))
print("  cache UUID         : %s" % d[16:32].hex())
import os
idp=sys.argv[1]+".id"
if os.path.exists(idp) and os.path.getsize(idp)>=8:
    print("  build stamp        : %016x  (M4.45 sidecar; a different build discards the cache)" % int.from_bytes(open(idp,'rb').read(8),'little'))
else:
    print("  build stamp        : (none -- next launch discards and re-warms)")
PY
}

case "$MODE" in
  status)
    echo "Folder: $DIR"
    hdr
    if [ -s "$SPV" ]; then
      python3 -c "
import sys,struct
f=open('$SPV','rb'); m,v,n=struct.unpack('<III',f.read(12))
print('  shader_spv.bin     : %.1f MB, %d compiled shaders (version %d)'%(__import__('os').path.getsize('$SPV')/1048576,n,v))"
    else
      echo "  shader_spv.bin     : (absent)"
    fi
    # Coverage: the manifest is the union of every pipeline this install has
    # ever seen, so watching these two numbers grow as you play IS the
    # coverage measurement. They stop growing when a route is fully covered.
    if [ -s "$DIR/pipeline_prewarm.bin" ]; then
      python3 - "$DIR/pipeline_prewarm.bin" <<'PY'
import sys,struct
d=open(sys.argv[1],'rb').read()
_,_,nsh,nrec=struct.unpack('<IIII',d[:16])
off=16; vs=ps=0
for _ in range(nsh):
    h,isp,dw=struct.unpack('<QII',d[off:off+16]); off+=16+dw*4
    ps+=isp; vs+= (0 if isp else 1)
print("  pipeline_prewarm   : %d shaders (%d VS + %d PS), %d pipelines" % (nsh,vs,ps,nrec))
PY
    else
      echo "  pipeline_prewarm   : (absent)"
    fi
    # `&&` alone would make a missing backup the script's exit status.
    if [ -f "$BAK" ]; then echo "  (a test backup exists -- 'restore' will put it back)"; fi
    ;;
  cold)
    [ -s "$PC" ] && cp "$PC" "$BAK"
    rm -f "$PC" "$PC.id" "$SPV"
    echo "Deleted both caches. Next launch should compile shaders AND warm pipelines."
    ;;
  fake-build)
    # M4.45: pretend the game was updated -- the sidecar stamp no longer matches.
    [ -s "$PC.id" ] || { echo "No build stamp yet -- run the game once first."; exit 1; }
    [ -f "$BAK" ] || cp -p "$PC" "$BAK"
    printf 'STALEBLD' > "$PC.id"
    echo "Build stamp replaced. Next launch should log 'different build' and re-warm.";;
  fake-driver|fake-gpu)
    [ -s "$PC" ] || { echo "No pipeline_cache.bin to modify -- run the game once first."; exit 1; }
    cp "$PC" "$BAK"
    python3 - "$PC" "$MODE" <<'PY'
import sys
p,mode=sys.argv[1],sys.argv[2]
d=bytearray(open(p,'rb').read())
if mode=='fake-driver': d[16]^=0xFF          # one bit of pipelineCacheUUID
else: d[8:12]=(0x1002 if int.from_bytes(d[8:12],'little')!=0x1002 else 0x10DE).to_bytes(4,'little')
open(p,'wb').write(d)
print("Patched. Next launch should report:",
      "'different driver version'" if mode=='fake-driver' else "'different GPU'")
PY
    ;;
  restore)
    [ -f "$BAK" ] || { echo "No backup to restore."; exit 1; }
    mv "$BAK" "$PC"; echo "Restored the real pipeline cache."
    ;;
  *) echo "unknown mode '$MODE'"; exit 2;;
esac

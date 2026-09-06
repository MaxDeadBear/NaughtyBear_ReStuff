#!/bin/bash
# Splices a translator-emitted VS .vert into nb_vs_exec.cpp's GLSL slot and
# builds the CPU executor. GLM swizzle proxies don't support proxy-op-proxy
# arithmetic, so every multi-letter swizzle is wrapped in an explicit vecN().
#
#   tools/build_vs_exec.sh <file.vert> <out_binary>
set -eu
VERT="$1"; OUT="$2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${REXGLUE_SDK:-$HOME/Git/rexglue-sdk}"

# Stop at main's closing brace: nb_ucode_glsl appends a "-- glslc --" banner
# after the shader, which is not C++ and breaks the splice.
sed -n '/^void main/,/^}/p' "$VERT" \
  | sed 's/^void main()/static void glsl_main()/' \
  | perl -pe 's/\.([xyzw]{2,4})\b(?!\()/.$1()/g' \
  > "$HERE/vs_body.inc"

clang++ -std=c++23 -O1 -w -I "$HERE/.." -I "$HERE/../src" -I "$SDK/include" \
  "$HERE/nb_vs_exec.cpp" "$HERE/../src/renderer/ucode_translator.cpp" \
  -lshaderc_shared -o "$OUT"
echo "built $OUT"

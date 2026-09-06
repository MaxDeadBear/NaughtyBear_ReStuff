#!/bin/bash
# Splices a translator-emitted .frag into nb_glsl_exec.cpp and builds it, so our
# translated pixel shader can be run offline and diffed against the SDK gold
# interpreter (nb_ps_gold) on identical inputs.
#
#   tools/build_glsl_exec.sh <shader.frag> <out_binary>
#
# The .frag may be raw nb_ucode_glsl output (banner lines, a trailing "-- glslc
# --" section); everything outside main() is stripped.
set -eu
FRAG="$1"; OUT="$2"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# main() .. matching closing brace at column 0, then GLSL -> C++/GLM:
#   .xyz         -> .xyz()      (GLM swizzle member functions)
#   discard;     -> flag + return
#   vec4 r[64];  -> plain array (GLM has no GLSL array-ctor syntax issues here)
sed -n '/^void main/,/^}/p' "$FRAG" \
  | sed 's/^void main()/static void glsl_main()/' \
  | perl -pe '
      s/\.([xyzw]{2,4})\b(?!\()/.$1()/g;
      s/\bdiscard;/{ discarded = true; return; }/g;
      s/^\s*vec4 r\[64\];/  vec4 (&r)[64] = g_r;/;
    ' \
  | awk '{ print; if ($0 == "  }") { printf "  NBTRACE(%d);\n", ++n } }' > "$TMP/glsl_body.inc"

lines=$(wc -l < "$TMP/glsl_body.inc")
if [ "$lines" -lt 5 ]; then
  echo "build_glsl_exec: no main() found in $FRAG" >&2
  exit 1
fi
echo "spliced $lines lines from $(basename "$FRAG")"

clang++ -std=c++23 -O2 -w -I "$TMP" -I /usr/include "$HERE/nb_glsl_exec.cpp" -o "$OUT"
echo "built $OUT"

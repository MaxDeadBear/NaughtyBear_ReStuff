#!/bin/bash
# Splices translator-emitted VS (.vert) + PS (.frag) into nb_ps_exec.cpp's
# GLSL slots and builds the full-circle pixel executor. Swizzles become GLM
# member-function calls; `discard` becomes a flag+return.
#
#   tools/build_ps_exec.sh <file.vert> <file.frag> <out_binary>
set -eu
VERT="$1"; FRAG="$2"; OUT="$3"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SDK="${REXGLUE_SDK:-$HOME/Git/rexglue-sdk}"

splice() {
  sed -n '/^void main/,$p' "$1" \
    | sed 's/^void main()/static void glsl_main()/' \
    | perl -pe '
        s/\.([xyzw]{2,4})\b(?!\()/.$1()/g;
        s/\bdiscard;/{ discarded = true; return; }/g;
        # Xenos zero-multiply semantics: rewrite the translator'"'"'s stereotyped
        # mul/mad/dot statement shapes to the xm* helpers (env-toggleable).
        s/vr = \(\((.+?) \* (.+?) \+ (.+?)\)\);/vr = xmad($1, $2, $3);/;
        s/vr = \((.+?) \* (.+?)\);/vr = xmul($1, $2);/;
        s/vec4\(dot\(\((.+?)\)\.xyz\(\), \((.+?)\)\.xyz\(\)\)\)/vec4(xdot3(vec4($1), vec4($2)))/g;
        s/vec4\(dot\((.+?), (.+?)\)\)/vec4(xdot4(vec4($1), vec4($2)))/g;
        s/ps = ([^;=]+?) \* ([^;=]+?);/ps = xmuls($1, $2);/;
        s/^(\s*vec4 vr = .*;)$/$1 TRACEV(vr);/;
        s/^(\s*ps = .*;)$/$1 TRACES(ps);/;
        s/clamp\((.+?), 0\.0, 1\.0\)/glm::clamp($1, 0.0f, 1.0f)/g;
        s/vec4 r\[64\];/auto& r = g_regs;/;
      '
}
splice "$VERT" > "$HERE/vs_body.inc"
splice "$FRAG" > "$HERE/ps_body.inc"

clang++ -std=c++23 -O1 -w -DFMT_HEADER_ONLY=1 -I "$HERE/.." -I "$HERE/../src" -I "$SDK/include" \
  "$HERE/nb_ps_exec.cpp" "$HERE/../src/renderer/ucode_translator.cpp" \
  "$HERE/../src/renderer/guest_texture_decode.cpp" \
  -lshaderc_shared -o "$OUT"
echo "built $OUT"

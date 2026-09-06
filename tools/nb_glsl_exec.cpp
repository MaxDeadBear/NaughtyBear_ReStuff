// Offline executor for OUR translated pixel shader, built to be diffed
// component-for-component against the SDK gold interpreter (nb_ps_gold) on
// IDENTICAL inputs.
//
// Why this exists: every in-engine probe can only show what native computes;
// it can never say what the value SHOULD be. Running our GLSL and the SDK's
// reference interpreter on the same constants + same interpolators, with
// texture fetches forced to zero in BOTH, isolates the translated ALU with no
// reference build, no PM4 trace, and no live drive.
//
// The GLSL body is spliced in verbatim (see tools/build_glsl_exec.sh): GLM
// provides vec4 and swizzles, texture() returns zero exactly as the SDK
// interpreter does, and `discard` becomes an early return.
//
// Build:  tools/build_glsl_exec.sh <shader.frag> <out_binary>
// Run:    nb_glsl_exec <regs.bin> [rN=x,y,z,w ...]
//         (same rN= syntax as nb_ps_gold, so the two take identical arguments)
#define GLM_FORCE_SWIZZLE
#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace glm;
using uvec4 = glm::uvec4;

// GLSL scalar builtins: GLM covers the vector forms, but the translator also
// emits float-scalar calls with double literals (clamp(ps, 0.0, 1.0)), which
// are ambiguous without these.
static inline float max(float a, float b) { return a > b ? a : b; }
static inline float min(float a, float b) { return a < b ? a : b; }
static inline float clamp(float x, double a, double b) {
  return x < float(a) ? float(a) : (x > float(b) ? float(b) : x);
}
static inline float clamp(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
// The translator emits clamp(vec4, 0.0, 1.0) with DOUBLE literals; GLM has no
// (vec4, double, double) overload, so shaders using it failed to build.
static inline vec4 clamp(const vec4& v, double a, double b) {
  return glm::clamp(v, float(a), float(b));
}
static inline vec3 clamp(const vec3& v, double a, double b) {
  return glm::clamp(v, float(a), float(b));
}
static inline vec2 clamp(const vec2& v, double a, double b) {
  return glm::clamp(v, float(a), float(b));
}
static inline float fract(float x) { return x - std::floor(x); }
// Xenos clamped-scalar semantics (kRcpc/kRsqc/kLogc -> +/-FLT_MAX;
// kRcpf/kRsqf -> +/-0). Mirrors what the translator now emits.
static inline float _cinf(float x) {
  return std::isinf(x) ? (x > 0.0f ? 3.402823466e38f : -3.402823466e38f) : x;
}
static inline float _zinf(float x) { return std::isinf(x) ? (x > 0.0f ? 0.0f : -0.0f) : x; }
static inline float exp2f_(float x) { return std::exp2(x); }

// ---- shader environment -----------------------------------------------------
static vec4 c[256];
static uvec4 lc[8], bc[2];
struct PC {
  vec4 apc{0.0f, 7.0f, 0.0f, 0.0f};  // .y = 7 -> alpha test disabled
  uvec4 boolc[2];
  vec4 pgen{0.0f, 0.0f, 0.0f, 0.0f};
  uvec4 texexp{0, 0, 0, 0};
} pc;

struct Sampler2D {
  int unit;
};
static Sampler2D tex_0{0}, tex_1{1}, tex_2{2}, tex_3{3}, tex_4{4}, tex_5{5}, tex_6{6}, tex_7{7};

// The SDK ShaderInterpreter stores ZEROS for every texture fetch; match it so
// the two executors are comparable.
// NB_GOLD_TEX = one constant for ALL slots. That is VACUOUS for composites: a
// shader that lerps between two slots returns the same value either way, so gold
// and ours agree no matter what. NB_GOLD_TEX0..7 give PER-SLOT constants, which
// is what actually discriminates a blend.
static float g_texk = 0.0f;
static float g_texk_slot[8] = {0, 0, 0, 0, 0, 0, 0, 0};
static bool g_texk_slot_set[8] = {};
static vec4 texture(Sampler2D s, vec2) {
  const int u = (s.unit >= 0 && s.unit < 8) ? s.unit : 0;
  return vec4(g_texk_slot_set[u] ? g_texk_slot[u] : g_texk);
}

static bool bcond(int i) { return ((pc.boolc[i >> 7][(i >> 5) & 3] >> unsigned(i & 31)) & 1u) != 0u; }
static float texexp(int s) {
  unsigned p = pc.texexp[s >> 2];
  int e = int((p >> unsigned(8 * (s & 3))) & 0xFFu);
  if (e > 127) e -= 256;
  return exp2(float(e));
}

static vec4 o_0, o_1, o_2, o_3, o_4, o_5, o_6, o_7, o_8, o_9, o_10, o_11, o_12, o_13, o_14, o_15;
static vec4 col_0, col_1, col_2, col_3;
static vec4 gl_FragCoord{0.0f, 0.0f, 0.5f, 1.0f};
static bool gl_FrontFacing = true;
static float gl_FragDepth = 0.0f;
static bool discarded = false;
// The shader's register file, aliased into glsl_main() by the splice so its
// final state can be diffed against nb_ps_gold's PS_GOLD_REGS dump.
static vec4 g_r[64];

// Emitted after every translated instruction block by build_glsl_exec.sh.
// Reports the FIRST block that makes any register non-finite -- the point where
// our translation diverges from the SDK gold interpreter.
static int g_first_nan = -1;
static void NBTRACE(int blk) {
  // NB_TRACE_AT=<blk>: dump the register file at a chosen block, to see the
  // operands feeding the instruction that first diverges.
  static const int at = getenv("NB_TRACE_AT") ? atoi(getenv("NB_TRACE_AT")) : -1;
  // NB_WATCH=<reg>: print that register after EVERY block, to find where a
  // value is lost.
  static const int watch = getenv("NB_WATCH") ? atoi(getenv("NB_WATCH")) : -1;
  if (watch >= 0)
    printf("     blk %-3d r%-2d (%.6g, %.6g, %.6g, %.6g)\n", blk, watch, g_r[watch].x,
           g_r[watch].y, g_r[watch].z, g_r[watch].w);
  if (blk == at) {
    printf("  -- registers at block %d --\n", blk);
    for (int i = 0; i < 12; ++i)
      printf("     r%-2d (%.9g, %.9g, %.9g, %.9g)\n", i, g_r[i].x, g_r[i].y, g_r[i].z, g_r[i].w);
  }
  if (g_first_nan >= 0) return;
  for (int i = 0; i < 64; ++i)
    for (int k = 0; k < 4; ++k)
      if (!std::isfinite(g_r[i][k])) {
        g_first_nan = blk;
        printf("  FIRST NON-FINITE at block %d: r%d = (%g, %g, %g, %g)\n", blk, i, g_r[i].x,
               g_r[i].y, g_r[i].z, g_r[i].w);
        return;
      }
}

#include "glsl_body.inc"  // generated: the translated main(), as glsl_main()

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <regs.bin> [rN=x,y,z,w ...]\n", argv[0]);
    return 1;
  }
  std::vector<uint8_t> regs;
  {
    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    regs.resize(size_t(sz));
    if (fread(regs.data(), 1, regs.size(), f) != regs.size()) regs.clear();
    fclose(f);
  }
  if (regs.size() < 0x5003 * 4) { fprintf(stderr, "bad regs.bin\n"); return 1; }
  const uint32_t* R = reinterpret_cast<const uint32_t*>(regs.data());
  if (const char* k = getenv("NB_GOLD_TEX")) g_texk = strtof(k, nullptr);
  for (int u = 0; u < 8; ++u) {
    char nm[16];
    std::snprintf(nm, sizeof(nm), "NB_GOLD_TEX%d", u);
    if (const char* v = getenv(nm)) {
      g_texk_slot[u] = strtof(v, nullptr);
      g_texk_slot_set[u] = true;
      printf("# tex slot %d = %g\n", u, g_texk_slot[u]);
    }
  }

  // PS float constants live at 0x4000 + (ps_base + i) * 4, exactly as the SDK
  // interpreter resolves them via SQ_PS_CONST (0x2308).
  const uint32_t ps_base = R[0x2308] & 0x1FF;
  for (int i = 0; i < 256; ++i) {
    const uint32_t o = 0x4000 + (ps_base + i) * 4;
    float v[4];
    for (int k = 0; k < 4; ++k) std::memcpy(&v[k], &R[o + k], 4);
    c[i] = vec4(v[0], v[1], v[2], v[3]);
  }
  // Bool constants: 0x4900..0x4907, PS half is b128..b255 (words 4..7).
  for (int i = 0; i < 8; ++i) pc.boolc[i >> 2][i & 3] = R[0x4900 + i];
  printf("# ps_base=%u  bool[4..7]=%08X %08X %08X %08X\n", ps_base, R[0x4904], R[0x4905],
         R[0x4906], R[0x4907]);

  vec4* interp[16] = {&o_0, &o_1, &o_2,  &o_3,  &o_4,  &o_5,  &o_6,  &o_7,
                      &o_8, &o_9, &o_10, &o_11, &o_12, &o_13, &o_14, &o_15};
  for (int a = 2; a < argc; ++a) {
    const char* s = argv[a];
    if (s[0] != 'r') continue;
    char* end = nullptr;
    long idx = strtol(s + 1, &end, 10);
    if (!end || *end != '=' || idx < 0 || idx > 15) continue;
    float v[4] = {0, 0, 0, 0};
    const char* p = end + 1;
    for (int k = 0; k < 4 && p && *p; ++k) {
      v[k] = strtof(p, &end);
      p = (end && *end == ',') ? end + 1 : nullptr;
    }
    *interp[idx] = vec4(v[0], v[1], v[2], v[3]);
    printf("# in  r%-2ld (%.9g, %.9g, %.9g, %.9g)\n", idx, v[0], v[1], v[2], v[3]);
  }

  glsl_main();
  if (discarded) { printf("  OURS: discarded\n"); return 0; }
  printf("  OURS col_0 = (%.9g, %.9g, %.9g, %.9g)\n", col_0.x, col_0.y, col_0.z, col_0.w);
  if (getenv("OURS_REGS"))
    for (int i = 0; i < 16; ++i)
      printf("  r%-2d (%.9g, %.9g, %.9g, %.9g)\n", i, g_r[i].x, g_r[i].y, g_r[i].z, g_r[i].w);
  return 0;
}

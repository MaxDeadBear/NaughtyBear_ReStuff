// Full-circle pixel executor: runs OUR translated VS + PS GLSL (compiled as
// C++ via GLM) on a captured draw's real inputs -- real vertex streams, real
// constant banks, real bool constants, real DXT textures decoded by the
// renderer's own guest_texture_decode -- and compares the PS output against
// the reference's rendered pixel (the traced scene image) at each in-frustum
// vertex's screen position. Splits the world-dim into PS-math vs PS-inputs vs
// downstream-state without any live drive.
//
// Built by tools/build_ps_exec.sh (splices vs_body.inc / ps_body.inc).
//
// Run: nb_ps_exec <harness_dir> <vs_ucode.bin> [max_rows]
//   env PS_T1_ZERO=1  -> force the tex_1 (light/shadow LUT) sample to zero
//   env PS_T1_ONE=1   -> force tex_1 to white
//   env PS_DUMP_TEX=1 -> write decoded tex_N as PPMs next to the harness dir
#define GLM_FORCE_SWIZZLE
#include <glm/glm.hpp>

#include <sys/mman.h>

#include <bit>

#include <cmath>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "renderer/guest_texture_decode.h"
#include "renderer/ucode_translator.h"
#include "renderer/up_draws.h"

#include <rex/runtime.h>
namespace rex {
Runtime* Runtime::instance_ = nullptr;  // offline: no live runtime
}

using namespace glm;

static std::vector<uint8_t> ReadFile(const std::string& path) {
  std::vector<uint8_t> out;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return out;
  fseek(f, 0, SEEK_END);
  long sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(size_t(sz));
  if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
  fclose(f);
  return out;
}

// ---- sampler emulation (bilinear, repeat/clamp per fetch constant) ----
struct SamplerEmu {
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  bool wrap = true;
};
static vec4 Texel(const SamplerEmu& s, int x, int y) {
  if (s.wrap) {
    x = ((x % int(s.w)) + int(s.w)) % int(s.w);
    y = ((y % int(s.h)) + int(s.h)) % int(s.h);
  } else {
    x = std::min(std::max(x, 0), int(s.w) - 1);
    y = std::min(std::max(y, 0), int(s.h) - 1);
  }
  const uint8_t* p = s.rgba.data() + (size_t(y) * s.w + x) * 4;
  return vec4(p[0], p[1], p[2], p[3]) * (1.0f / 255.0f);
}
static vec4 SampleBilinear(const SamplerEmu& s, vec2 uv) {
  if (!s.w || !s.h) return vec4(0.0f);
  // Wrap/clamp in float space FIRST: hardware repeat of a huge float coord is
  // frac() (exactly 0.0 beyond 2^24); naive int(floor(u*w)) overflows to UB.
  if (s.wrap) {
    uv.x = uv.x - std::floor(uv.x);
    uv.y = uv.y - std::floor(uv.y);
  } else {
    uv.x = std::min(std::max(uv.x, 0.0f), 1.0f);
    uv.y = std::min(std::max(uv.y, 0.0f), 1.0f);
  }
  const float fx = uv.x * s.w - 0.5f, fy = uv.y * s.h - 0.5f;
  const int x0 = int(std::floor(fx)), y0 = int(std::floor(fy));
  const float wx = fx - x0, wy = fy - y0;
  return mix(mix(Texel(s, x0, y0), Texel(s, x0 + 1, y0), wx),
             mix(Texel(s, x0, y0 + 1), Texel(s, x0 + 1, y0 + 1), wx), wy);
}

// ---- Xenos ALU multiply semantics (Shader Model 3 / D3D9 legacy rule):
// +-0 (or denormal) times ANYTHING -- including inf/NaN -- is +0. The SDK's
// SPIR-V translator and CPU interpreter both implement this; our runtime GLSL
// currently does IEEE. PS_IEEE_MUL=1 switches the executor to IEEE to
// reproduce the runtime's current behavior for A/B.
#include <cmath>
static bool g_xenos_mul = true;
static float xm(float a, float b) {
  return (!g_xenos_mul || (a != 0.0f && b != 0.0f)) ? a * b : 0.0f;
}
static vec4 xmul(vec4 a, vec4 b) {
  return vec4(xm(a.x, b.x), xm(a.y, b.y), xm(a.z, b.z), xm(a.w, b.w));
}
static vec4 xmad(vec4 a, vec4 b, vec4 c) {
  vec4 r = xmul(a, b) + c;
  if (getenv("XMAD_DBG") && !(std::isfinite(r.x) && std::isfinite(r.y) && std::isfinite(r.z))) {
    printf("  [XMAD] a=(%g,%g,%g,%g) b=(%g,%g,%g,%g) c=(%g,%g,%g,%g) -> (%g,%g,%g,%g)\n", a.x,
           a.y, a.z, a.w, b.x, b.y, b.z, b.w, c.x, c.y, c.z, c.w, r.x, r.y, r.z, r.w);
  }
  return r;
}
static float xmuls(float a, float b) { return xm(a, b); }
static float xdot3(vec4 a, vec4 b) { return xm(a.x, b.x) + xm(a.y, b.y) + xm(a.z, b.z); }
static float xdot4(vec4 a, vec4 b) {
  return xm(a.x, b.x) + xm(a.y, b.y) + xm(a.z, b.z) + xm(a.w, b.w);
}

// ---- per-statement tracer (PS_TRACE=1: first nonfinite; =2: every stmt) ----
static int g_trace = 0;
static void trace_v(int line, vec4 v) {
  const bool bad = !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || !std::isfinite(v.w);
  if (g_trace >= 2 || (g_trace == 1 && bad))
    printf("    [%d] vr=(%g,%g,%g,%g)%s\n", line, v.x, v.y, v.z, v.w, bad ? " <-- NONFINITE" : "");
}
static void trace_s(int line, float f) {
  const bool bad = !std::isfinite(f);
  if (g_trace >= 2 || (g_trace == 1 && bad))
    printf("    [%d] ps=%g%s\n", line, f, bad ? " <-- NONFINITE" : "");
}
#define TRACEV(v) do { if (g_trace) trace_v(__LINE__, v); } while (0)
#define TRACES(v) do { if (g_trace) trace_s(__LINE__, v); } while (0)

// ---- our VS, as C++ ----
namespace vsx {
static vec4 in_0, in_1, in_2, in_3, in_4, in_5, in_6, in_7;
static vec4 o_0, o_1, o_2, o_3, o_4, o_5, o_6, o_7, o_8, o_9, o_10, o_11, o_12, o_13, o_14, o_15;
static vec4 gl_Position;
static float gl_PointSize;
static vec4 c[256];
static uvec4 lc[8];
static uvec4 bc[2];
static struct {
  vec4 ndc = vec4(1.0f, 1.0f, 0.0f, 0.0f);  // identity: gl_Position = raw oPos
  vec4 rot = vec4(0.0f);
} pc;
static bool bcond(int i) { return ((bc[i >> 7][(i >> 5) & 3] >> uint32_t(i & 31)) & 1u) != 0u; }
static vec4 g_regs[64];
#include "vs_body.inc"
}  // namespace vsx

// ---- our PS, as C++ ----
namespace psx {
static vec4 o_0, o_1, o_2, o_3, o_4, o_5, o_6, o_7, o_8, o_9;
static vec4 col_0;
static vec4 c[256];
static uvec4 lc[8];
static uvec4 bc[2];
static struct {
  vec4 apc = vec4(0.0f, 7.0f, 0.0f, 0.0f);  // alpha func 7 = always pass
  uvec4 boolc[2];
  vec4 pgen = vec4(0.0f);
} pc;
static bool bcond(int i) { return ((pc.boolc[i >> 7][(i >> 5) & 3] >> uint32_t(i & 31)) & 1u) != 0u; }
static SamplerEmu tex_0, tex_1, tex_2, tex_3;
static float g_tex_force = -1.0f;
static vec4 texture(const SamplerEmu& s, vec2 uv) {
  if (g_tex_force >= 0.0f) return vec4(g_tex_force);
  return SampleBilinear(s, uv);
}
static vec4 gl_FragCoord;
static bool gl_FrontFacing = true;
static bool discarded = false;
static vec4 g_regs[64];
#include "ps_body.inc"
}  // namespace psx

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <harness_dir> <vs_ucode.bin> [max_rows]\n", argv[0]);
    return 1;
  }
  const std::string dir = argv[1];
  if (getenv("PS_IEEE_MUL")) g_xenos_mul = false;
  if (getenv("PS_TEX_FORCE")) psx::g_tex_force = atof(getenv("PS_TEX_FORCE"));
  if (getenv("PS_TRACE")) g_trace = atoi(getenv("PS_TRACE"));
  const int max_rows = argc > 3 ? atoi(argv[3]) : 24;

  auto regs_bytes = ReadFile(dir + "/regs.bin");
  if (regs_bytes.size() < 0x5003u * 4) {
    fprintf(stderr, "bad regs.bin\n");
    return 1;
  }
  const uint32_t* regs = reinterpret_cast<const uint32_t*>(regs_bytes.data());

  uint8_t* phys = static_cast<uint8_t*>(mmap(nullptr, 0x20000000, PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (phys == MAP_FAILED) {
    perror("mmap");
    return 1;
  }
  for (const auto& e : std::filesystem::directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    uint32_t addr = 0;
    if (name.rfind("mem_", 0) == 0)
      addr = uint32_t(strtoull(name.c_str() + 4, nullptr, 16));
    else if (name.rfind("tex_", 0) == 0)
      addr = uint32_t(strtoull(name.c_str() + 4, nullptr, 16));
    else
      continue;
    auto bytes = ReadFile(e.path().string());
    if (bytes.empty() || uint64_t(addr) + bytes.size() > 0x20000000ull) continue;
    std::memcpy(phys + addr, bytes.data(), bytes.size());
  }

  // Constant banks: VS honoring SQ_VS_CONST, PS honoring SQ_PS_CONST.
  const uint32_t vsbase = regs[0x2307] & 0x1FF;
  const uint32_t psbase = regs[0x2308] & 0x1FF;
  for (int n = 0; n < 256; ++n)
    for (int k = 0; k < 4; ++k) {
      float f;
      std::memcpy(&f, &regs[(0x4000 + (vsbase + n) * 4 + k) & 0x7FFF], 4);
      vsx::c[n][k] = f;
      std::memcpy(&f, &regs[(0x4000 + (psbase + n) * 4 + k) & 0x7FFF], 4);
      psx::c[n][k] = f;
    }
  for (int i = 0; i < 8; ++i) {
    vsx::bc[i >> 2][i & 3] = regs[0x4900 + i];
    psx::pc.boolc[i >> 2][i & 3] = regs[0x4900 + i];
  }
  // PS_CONST_FILE=<txt>: override PS bank rows from a native [DRAWCONST] dump
  // (lines with cN=(..)/qN=(..)/pN=(..) pairs) -- the RUNTIME's live values.
  if (const char* cf = getenv("PS_CONST_FILE")) {
    FILE* f = fopen(cf, "r");
    int applied = 0;
    if (f) {
      char line[1024];
      while (fgets(line, sizeof(line), f)) {
        const char* p2 = line;
        while ((p2 = strpbrk(p2, "qp"))) {
          int row;
          float x, y, z, w;
          int consumed = 0;
          if (sscanf(p2, "%*[qp]%d=(%f,%f,%f,%f)%n", &row, &x, &y, &z, &w, &consumed) >= 5 &&
              consumed > 0 && row >= 0 && row < 256) {
            psx::c[row] = vec4(x, y, z, w);
            ++applied;
            p2 += consumed;
          } else {
            ++p2;
          }
        }
      }
      fclose(f);
    }
    printf("# PS_CONST_FILE applied %d rows from %s\n", applied, cf);
  }
  if (const char* cx = getenv("PS_C255X")) {
    psx::c[255].x = atof(cx);
    printf("# c255.x forced to %g\n", psx::c[255].x);
  }
  if (getenv("PS_C_HIGH_ZERO")) {
    for (int n = 240; n < 256; ++n) psx::c[n] = vec4(0.0f);
    printf("# psx c240-255 FORCED ZERO\n");
  }
  printf("# vsbase=%u psbase=%u bool4=%08X c255=(%g,%g,%g,%g) c6=(%g,%g,%g,%g) c8=(%g,%g,%g,%g)\n",
         vsbase, psbase, regs[0x4904], psx::c[255].x, psx::c[255].y, psx::c[255].z, psx::c[255].w,
         psx::c[6].x, psx::c[6].y, psx::c[6].z, psx::c[6].w, psx::c[8].x, psx::c[8].y, psx::c[8].z,
         psx::c[8].w);

  // Textures: same field extraction as the runtime (native_backend_vk).
  SamplerEmu* samplers[4] = {&psx::tex_0, &psx::tex_1, &psx::tex_2, &psx::tex_3};
  for (uint32_t slot = 0; slot < 4; ++slot) {
    const uint32_t w0 = regs[0x4800 + slot * 6];
    if ((w0 & 3) != 2) continue;
    const uint32_t w1 = regs[0x4800 + slot * 6 + 1];
    const uint32_t w2 = regs[0x4800 + slot * 6 + 2];
    restuff::renderer::GuestTextureDesc desc;
    desc.valid = true;
    desc.phys_addr = ((w1 >> 12) & 0xFFFFF) << 12;
    desc.width = (w2 & 0x1FFF) + 1;
    desc.height = ((w2 >> 13) & 0x1FFF) + 1;
    const uint32_t pitch = (w0 >> 22) & 0x1FF;
    desc.pitch_texels = pitch ? (pitch << 5) : desc.width;
    desc.format = w1 & 0x3F;
    desc.endian = (w1 >> 6) & 0x3;
    desc.tiled = (w0 >> 31) & 1;
    desc.clamp_x = (w0 >> 10) & 7;
    desc.clamp_y = (w0 >> 13) & 7;
    desc.gamma = ((w0 >> 2) & 0x3) == 0x3;
    uint32_t ow = 0, oh = 0;
    SamplerEmu& s = *samplers[slot];
    if (!restuff::renderer::DecodeGuestTexture(desc, phys + desc.phys_addr, s.rgba, ow, oh)) {
      printf("# tex%u %08X fmt=%u DECODE FAILED\n", slot, desc.phys_addr, desc.format);
      continue;
    }
    s.w = ow;
    s.h = oh;
    s.wrap = desc.clamp_x <= 1;
    printf("# tex%u %08X fmt=%u %ux%u tiled=%u gamma=%u wrap=%u\n", slot, desc.phys_addr,
           desc.format, ow, oh, desc.tiled ? 1u : 0u, desc.gamma ? 1u : 0u, s.wrap ? 1u : 0u);
    if (getenv("PS_DUMP_TEX")) {
      char nm[512];
      snprintf(nm, sizeof(nm), "%s/tex%u_decoded.ppm", dir.c_str(), slot);
      if (FILE* f = fopen(nm, "wb")) {
        fprintf(f, "P6\n%u %u\n255\n", ow, oh);
        for (size_t i = 0; i < size_t(ow) * oh; ++i) fwrite(s.rgba.data() + i * 4, 1, 3, f);
        fclose(f);
      }
    }
  }
  if (getenv("PS_TEX_ZERO")) {
    for (auto* sp : samplers) std::fill(sp->rgba.begin(), sp->rgba.end(), 0);
    printf("# ALL textures FORCED ZERO\n");
  }
  if (getenv("PS_T1_ZERO")) {
    std::fill(psx::tex_1.rgba.begin(), psx::tex_1.rgba.end(), 0);
    printf("# tex1 FORCED ZERO\n");
  }
  if (getenv("PS_T1_ONE")) {
    std::fill(psx::tex_1.rgba.begin(), psx::tex_1.rgba.end(), 255);
    printf("# tex1 FORCED WHITE\n");
  }

  // Reference scene image (tiled 1280x720 32bpp).
  auto scene = ReadFile(dir + "/scene_06560000.bin");
  const uint32_t sw = 1280, sh = 720;

  // Vertex decode setup (same as nb_vs_exec).
  auto ucode_be = ReadFile(argv[2]);
  auto t = restuff::renderer::ucode::TranslateVertexShader(
      reinterpret_cast<const uint32_t*>(ucode_be.data()), uint32_t(ucode_be.size() / 4));
  auto ib = ReadFile(dir + "/ib.bin");
  const size_t nidx = ib.size() / 2;
  const uint32_t indx_offset = regs[0x2102];

  const float vp_xs = std::bit_cast<float>(regs[0x210F]);
  const float vp_xo = std::bit_cast<float>(regs[0x2110]);
  const float vp_ys = std::bit_cast<float>(regs[0x2111]);
  const float vp_yo = std::bit_cast<float>(regs[0x2112]);
  printf("# mul=%s\n", g_xenos_mul ? "xenos" : "ieee");
  printf("# viewport xs=%g xo=%g ys=%g yo=%g scene_bytes=%zu\n", vp_xs, vp_xo, vp_ys, vp_yo,
         scene.size());

  int rows = 0;
  std::map<uint32_t, bool> seen_vtx;
  for (size_t pos = 0; pos < nidx && rows < max_rows; ++pos) {
    uint32_t vi = (uint32_t(ib[pos * 2]) << 8) | ib[pos * 2 + 1];
    if (vi == 0xFFFF) continue;
    vi = (vi + indx_offset) & 0xFFFFFF;
    if (!seen_vtx.emplace(vi, true).second) continue;

    // Decode attributes (LE-normalize + VkFormat semantics) and run the VS.
    vec4* ins[8] = {&vsx::in_0, &vsx::in_1, &vsx::in_2, &vsx::in_3,
                    &vsx::in_4, &vsx::in_5, &vsx::in_6, &vsx::in_7};
    for (size_t loc = 0; loc < t.attrs.size() && loc < 8; ++loc) {
      const auto& a = t.attrs[loc];
      const uint32_t f0 = regs[0x4800 + a.fetch_slot * 2];
      const uint32_t f1 = regs[0x4800 + a.fetch_slot * 2 + 1];
      const uint32_t vb_phys = f0 & ~0x3u;
      const uint32_t vf_endian = f1 & 3;
      uint8_t buf[16] = {0};
      // Format component/width table matching the runtime's VtxFmt.
      uint32_t comps = 4, cbytes = 4;
      switch (a.format) {
        case 6: comps = 4; cbytes = 1; break;
        case 25: case 31: comps = 2; cbytes = 2; break;
        case 26: case 32: comps = 4; cbytes = 2; break;
        case 33: case 36: comps = 1; cbytes = 4; break;
        case 34: case 37: comps = 2; cbytes = 4; break;
        case 57: comps = 3; cbytes = 4; break;
        default: break;
      }
      const uint32_t span = comps * cbytes;
      std::memcpy(buf, phys + vb_phys + size_t(vi) * a.stride_bytes + a.byte_offset, span);
      if (vf_endian == 2 || (vf_endian == 3 && cbytes == 2)) {
        for (uint32_t b = 0; b + 3 < span; b += 4) {
          std::swap(buf[b], buf[b + 3]);
          std::swap(buf[b + 1], buf[b + 2]);
        }
      } else if (vf_endian == 1) {
        for (uint32_t b = 0; b + 1 < span; b += 2) std::swap(buf[b], buf[b + 1]);
      } else if (vf_endian == 3) {
        for (uint32_t b = 0; b + 3 < span; b += 4) {
          std::swap(buf[b], buf[b + 2]);
          std::swap(buf[b + 1], buf[b + 3]);
        }
      }
      vec4 v(0.0f, 0.0f, 0.0f, 1.0f);
      for (uint32_t k = 0; k < comps; ++k) {
        float fv = 0.0f;
        if (a.format == 6) {
          fv = a.is_normalized ? float(buf[k]) / 255.0f : float(buf[k]);
        } else if (a.format == 31 || a.format == 32) {
          uint16_t u;
          std::memcpy(&u, buf + k * 2, 2);
          // half -> float
          const uint32_t sgn = (u >> 15) & 1, ex = (u >> 10) & 0x1F, mn = u & 0x3FF;
          uint32_t bits;
          if (ex == 0 && mn == 0) bits = sgn << 31;
          else if (ex == 0) {
            int e2 = -1; uint32_t mm = mn;
            while (!(mm & 0x400)) { mm <<= 1; ++e2; }
            bits = (sgn << 31) | (uint32_t(127 - 15 - e2) << 23) | ((mm & 0x3FF) << 13);
          } else if (ex == 31) bits = (sgn << 31) | 0x7F800000u | (mn << 13);
          else bits = (sgn << 31) | ((ex - 15 + 127) << 23) | (mn << 13);
          std::memcpy(&fv, &bits, 4);
        } else {
          std::memcpy(&fv, buf + k * 4, 4);
        }
        v[int(k)] = fv;
      }
      *ins[loc] = v;
    }
    vsx::glsl_main();

    const vec4 p = vsx::gl_Position;  // raw oPos (pc.ndc identity)
    if (!(p.w > 0.01f)) continue;
    const float nx = p.x / p.w, ny = p.y / p.w;
    if (nx < -0.98f || nx > 0.98f || ny < -0.98f || ny > 0.98f) continue;
    const int px = int(nx * vp_xs + vp_xo), py = int(ny * vp_ys + vp_yo);
    if (px < 2 || px >= int(sw) - 2 || py < 2 || py >= int(sh) - 2) continue;

    // Run the PS at this vertex (barycentric (1,0,0): interpolants = exports).
    psx::o_0 = vsx::o_0; psx::o_1 = vsx::o_1; psx::o_2 = vsx::o_2; psx::o_3 = vsx::o_3;
    psx::o_4 = vsx::o_4; psx::o_5 = vsx::o_5; psx::o_6 = vsx::o_6; psx::o_7 = vsx::o_7;
    psx::o_8 = vsx::o_8; psx::o_9 = vsx::o_9;
    psx::gl_FragCoord = vec4(px + 0.5f, py + 0.5f, 0.0f, 1.0f);
    psx::discarded = false;
    psx::glsl_main();

    // Reference pixel from the tiled scene image.
    uint32_t rr = 0, rg = 0, rb = 0;
    if (scene.size() >= size_t(sw) * sh * 4) {
      const uint32_t off =
          restuff::renderer::TiledBlockByteOffset(uint32_t(px), uint32_t(py), sw, 4);
      if (off + 4 <= scene.size()) {
        const uint8_t* sp = scene.data() + off;
        rr = sp[0]; rg = sp[1]; rb = sp[2];
      }
    }
    if (getenv("PS_REGS")) {
      for (int rn = 0; rn < 10; ++rn)
        printf("  PS r%-2d (%.9g, %.9g, %.9g, %.9g)\n", rn, psx::g_regs[rn].x, psx::g_regs[rn].y,
               psx::g_regs[rn].z, psx::g_regs[rn].w);
    }
    printf("vtx=%-6u px=%-4d py=%-3d ours=(%.4f,%.4f,%.4f,%.4f)%s ref_raw=(%02X,%02X,%02X)\n", vi,
           px, py, psx::col_0.x, psx::col_0.y, psx::col_0.z, psx::col_0.w,
           psx::discarded ? " DISCARD" : "", rr, rg, rb);
    ++rows;
  }
  return 0;
}

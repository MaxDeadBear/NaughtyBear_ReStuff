// Executes OUR translated VS GLSL on the CPU for the same captured draw the
// SDK ShaderInterpreter replays (nb_interp_harness), reproducing the native
// pipeline's full vertex path: fetch-constant lookup, the capture's
// LE-normalize endian pass, VkFormat attribute decode (missing components
// 0,0,0,1), then the translator-emitted GLSL body compiled as C++ via GLM.
// Divergence against the interpreter's ground-truth exports localizes the
// world-dim bug to decode vs math vs downstream-of-VS.
//
// The GLSL body is spliced in by tools/build_vs_exec.sh (swizzle-wrapped for
// GLM) as vs_body.inc next to this file.
//
// Run: nb_vs_exec <harness_dir> <vs_ucode.bin> [ib_pos ...]
#define GLM_FORCE_SWIZZLE
#include <glm/glm.hpp>

#include <sys/mman.h>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "renderer/ucode_translator.h"

using namespace glm;
namespace uc = restuff::renderer::ucode;

// ---- GLSL environment ----
static vec4 in_0, in_1, in_2, in_3, in_4, in_5, in_6, in_7;
static vec4 o_0, o_1, o_2, o_3, o_4, o_5, o_6, o_7, o_8, o_9, o_10, o_11, o_12, o_13, o_14, o_15;
static vec4 gl_Position;
static float gl_PointSize;
static vec4 c[256];
static uvec4 lc[8];
static uvec4 bc[2];
static struct {
  vec4 ndc = vec4(1.0f, 1.0f, 0.0f, 0.0f);  // identity: gl_Position == raw oPos
  vec4 rot = vec4(0.0f);
} pc;
static bool bcond(int i) { return ((bc[i >> 7][(i >> 5) & 3] >> uint32_t(i & 31)) & 1u) != 0u; }

#include "vs_body.inc"

// ---- capture loading (mirrors nb_interp_harness) ----
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

// Component count/width per xenos vertex format -- must match the runtime's
// VtxFmt (native_vk.cpp) for the endian pass to touch identical spans.
struct FmtInfo {
  uint32_t comps, bytes;
};
static FmtInfo VtxFmt(uint32_t f) {
  switch (f) {
    case 6: return {4, 1};
    case 25: return {2, 2};
    case 26: return {4, 2};
    case 31: return {2, 2};
    case 32: return {4, 2};
    case 33: case 36: return {1, 4};
    case 34: case 37: return {2, 4};
    case 57: return {3, 4};
    case 35: case 38: return {4, 4};
    default: return {4, 4};
  }
}

static float HalfToFloat(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
  uint32_t bits;
  if (e == 0) {
    if (!m) {
      bits = s << 31;
    } else {
      int ex = -1;
      uint32_t mm = m;
      while (!(mm & 0x400)) {
        mm <<= 1;
        ++ex;
      }
      bits = (s << 31) | (uint32_t(127 - 15 - ex) << 23) | ((mm & 0x3FF) << 13);
    }
  } else if (e == 31) {
    bits = (s << 31) | 0x7F800000u | (m << 13);
  } else {
    bits = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

// Decode one attribute from LE-normalized bytes exactly as the Vulkan
// vertex-input stage would from our XenosVtxVkFormat choice.
static vec4 DecodeAttr(const uint8_t* p, uint32_t fmt, bool sgn, bool nrm) {
  vec4 v(0.0f, 0.0f, 0.0f, 1.0f);
  const FmtInfo fi = VtxFmt(fmt);
  for (uint32_t k = 0; k < fi.comps; ++k) {
    float f = 0.0f;
    switch (fmt) {
      case 6: {  // R8G8B8A8 UNORM/SNORM/USCALED/SSCALED
        const uint8_t b = p[k];
        if (nrm)
          f = sgn ? std::max(-1.0f, float(int8_t(b)) / 127.0f) : float(b) / 255.0f;
        else
          f = sgn ? float(int8_t(b)) : float(b);
        break;
      }
      case 25:
      case 26: {  // R16G16(B16A16) UNORM/SNORM/USCALED/SSCALED
        uint16_t u;
        std::memcpy(&u, p + k * 2, 2);
        if (nrm)
          f = sgn ? std::max(-1.0f, float(int16_t(u)) / 32767.0f) : float(u) / 65535.0f;
        else
          f = sgn ? float(int16_t(u)) : float(u);
        break;
      }
      case 31:
      case 32: {  // R16G16(B16A16)_SFLOAT
        uint16_t u;
        std::memcpy(&u, p + k * 2, 2);
        f = HalfToFloat(u);
        break;
      }
      default: {  // 32-bit float family (33..38, 57)
        std::memcpy(&f, p + k * 4, 4);
        break;
      }
    }
    v[int(k)] = f;
  }
  return v;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s <harness_dir> <vs_ucode.bin> [ib_pos ...]\n", argv[0]);
    return 1;
  }
  const std::string dir = argv[1];

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
    if (name.rfind("mem_", 0) != 0) continue;
    const uint32_t addr = uint32_t(strtoull(name.c_str() + 4, nullptr, 16));
    auto bytes = ReadFile(e.path().string());
    if (bytes.empty() || uint64_t(addr) + bytes.size() > 0x20000000ull) continue;
    std::memcpy(phys + addr, bytes.data(), bytes.size());
  }

  // Constants exactly as the runtime uploads them: VS bank honoring
  // SQ_VS_CONST base, bool 0x4900.., loop 0x4908..
  const uint32_t vsbase = regs[0x2307] & 0x1FF;
  for (int n = 0; n < 256; ++n)
    for (int k = 0; k < 4; ++k) {
      float f;
      std::memcpy(&f, &regs[0x4000 + (vsbase + n) * 4 + k], 4);
      c[n][k] = f;
    }
  for (int i = 0; i < 8; ++i) bc[i >> 2][i & 3] = regs[0x4900 + i];
  for (int i = 0; i < 32; ++i) lc[i >> 2][i & 3] = regs[0x4908 + i];

  // Our translator's attribute table drives the decode, as in the runtime.
  auto ucode_be = ReadFile(argv[2]);
  auto t = restuff::renderer::ucode::TranslateVertexShader(
      reinterpret_cast<const uint32_t*>(ucode_be.data()), uint32_t(ucode_be.size() / 4));
  if (!t.attrs.size()) {
    fprintf(stderr, "translator produced no attrs\n");
    return 1;
  }
  printf("# attrs:");
  for (const auto& a : t.attrs)
    printf(" [slot=%u fmt=%u off=%u stride=%u sgn=%d nrm=%d]", a.fetch_slot, a.format,
           a.byte_offset, a.stride_bytes, a.is_signed, a.is_normalized);
  printf("\n");

  auto ib = ReadFile(dir + "/ib.bin");
  const size_t nidx = ib.size() / 2;
  const uint32_t indx_offset = regs[0x2102];
  const uint32_t min_index = regs[0x2101];
  const uint32_t max_index = regs[0x2100];

  std::vector<size_t> positions;
  for (int a = 3; a < argc; ++a) positions.push_back(strtoull(argv[a], nullptr, 0));
  if (positions.empty()) {
    for (size_t p = 0; p < nidx; p += nidx > 16 ? nidx / 16 : 1) positions.push_back(p);
  }

  for (size_t pos : positions) {
    if (pos >= nidx) continue;
    uint32_t vi = (uint32_t(ib[pos * 2]) << 8) | ib[pos * 2 + 1];
    if (vi == 0xFFFF) {
      printf("ib[%zu] = restart\n", pos);
      continue;
    }
    vi = (vi + indx_offset) & 0xFFFFFF;
    if (vi < min_index) vi = min_index;
    if (vi > max_index) vi = max_index;

    // Per attribute: raw guest bytes -> LE-normalize (endian from the fetch
    // constant) -> VkFormat decode.
    vec4* ins[8] = {&in_0, &in_1, &in_2, &in_3, &in_4, &in_5, &in_6, &in_7};
    for (size_t loc = 0; loc < t.attrs.size() && loc < 8; ++loc) {
      const auto& a = t.attrs[loc];
      const uint32_t f0 = regs[0x4800 + a.fetch_slot * 2];
      const uint32_t f1 = regs[0x4800 + a.fetch_slot * 2 + 1];
      const uint32_t vb_phys = f0 & ~0x3u;
      const uint32_t vf_endian = f1 & 3;
      uint8_t buf[16] = {0};
      const FmtInfo fi = VtxFmt(a.format);
      const uint32_t span = fi.comps * fi.bytes;
      std::memcpy(buf, phys + vb_phys + size_t(vi) * a.stride_bytes + a.byte_offset, span);
      switch (vf_endian) {
        case 1:
          for (uint32_t b = 0; b + 1 < span; b += 2) std::swap(buf[b], buf[b + 1]);
          break;
        case 2:
          for (uint32_t b = 0; b + 3 < span; b += 4) {
            std::swap(buf[b], buf[b + 3]);
            std::swap(buf[b + 1], buf[b + 2]);
          }
          break;
        case 3:
          if (fi.bytes == 2) {
            for (uint32_t b = 0; b + 3 < span; b += 4) {
              std::swap(buf[b], buf[b + 3]);
              std::swap(buf[b + 1], buf[b + 2]);
            }
          } else {
            for (uint32_t b = 0; b + 3 < span; b += 4) {
              std::swap(buf[b], buf[b + 2]);
              std::swap(buf[b + 1], buf[b + 3]);
            }
          }
          break;
        default:
          break;
      }
      *ins[loc] = DecodeAttr(buf, a.format, a.is_signed, a.is_normalized);
    }

    glsl_main();

    printf("ib[%zu] vtx=%u\n", pos, vi);
    printf("  in_0(%.9g,%.9g,%.9g,%.9g) in_1(%.9g,%.9g,%.9g,%.9g) in_2(%.9g,%.9g,%.9g,%.9g) "
           "in_3(%.9g,%.9g,%.9g,%.9g) in_4(%.9g,%.9g,%.9g,%.9g)\n",
           in_0.x, in_0.y, in_0.z, in_0.w, in_1.x, in_1.y, in_1.z, in_1.w, in_2.x, in_2.y, in_2.z,
           in_2.w, in_3.x, in_3.y, in_3.z, in_3.w, in_4.x, in_4.y, in_4.z, in_4.w);
    const vec4* outs[6] = {&o_0, &o_1, &o_2, &o_3, &o_4, &o_5};
    for (int n = 0; n < 6; ++n)
      printf("  o_%d (%.9g, %.9g, %.9g, %.9g)\n", n, outs[n]->x, outs[n]->y, outs[n]->z,
             outs[n]->w);
    printf("  pos (%.9g, %.9g, %.9g, %.9g)\n", gl_Position.x, gl_Position.y, gl_Position.z,
           gl_Position.w);
  }
  return 0;
}

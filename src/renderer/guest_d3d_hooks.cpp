#include "renderer/up_draws.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>  // REX_HOOK_RAW / REX_EXTERN (pulls rex/ppc/context.h)
#include <rex/logging.h>
#include <rex/runtime.h>

#include "renderer/native_backend_vk.h"

// Guest-D3D UP-draw capture + decode. Port of the friend's proven
// lua_mods/src/renderer/guest_d3d_hooks.cpp + the declaration-decode path of
// native_backend.cpp ("NB:"), WITHOUT the SDK ShaderInterpreter / AnalyzeUcode
// (not exported by librexruntime on Linux). Coverage: the stride-12 Scaleform
// quad decl (title-screen art, panels, fades — everything the friend's
// verified decl path drew). Stride-28 float-position draws (menu text) went
// through the CPU interpreter in the friend's build and are best-effort here.
//
// Guest function map (guest_d3d.h, live-verified by the friend):
//   0x82F37EF0 D3DDevice_BeginVertices(device, prim, vertexCount, strideBytes)
//                -> r3 = vertex write ptr (guest virtual), 0 on OOM
//   0x82F383D0 D3DDevice_BeginIndexedVertices(device, prim, r5, vertexCount,
//                indexCount, indexFlags(bit2=32-bit), strideBytes, ...)
//                -> r3 = HRESULT
//   0x82F3FDC8 D3D_WriteLoadAluConstantPacket(device, type, offset_vec4,
//                physaddr, count_vec4)
// Device-struct offsets: UP vtx ptr +13656, idx ptr +13660, vtx dwords +13664,
// idx dwords +13668; bound VS +0x3248, PS +0x3244; VS float consts +1920;
// texture-fetch staging block +1152 + slot*24 (6 dwords per record).

namespace restuff::renderer {
namespace {

// --- guest memory helpers -----------------------------------------------------

uint32_t ReadGuestU32(uint32_t guest_addr) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return 0;
  return std::byteswap(*runtime->memory()->TranslateVirtual<const uint32_t*>(guest_addr));
}

float ReadGuestF32(uint32_t guest_addr) {
  return std::bit_cast<float>(ReadGuestU32(guest_addr));
}

// --- pending draw slot ---------------------------------------------------------

struct PendingUpDraw {
  bool valid = false;
  bool indexed = false;
  uint32_t device = 0;
  uint32_t prim = 0;
  uint32_t vertex_count = 0;
  uint32_t index_count = 0;
  uint32_t stride = 0;  // bytes, from the Begin* API arg (ground truth)
  uint32_t vtx_addr = 0, vtx_dwords = 0;
  uint32_t idx_addr = 0, idx_dwords = 0;
  uint32_t vs_obj = 0, ps_obj = 0;  // bound shader objects (family fingerprint)
  // Constants c0..c5 snapshotted at Begin time: the game re-stages constants
  // for draw N+1 before Begin N+1, so reading them at completion time gives
  // draw N the WRONG transform (mis-placed panels).
  float rows[8][4] = {};
};

std::mutex s_pending_mutex;
PendingUpDraw s_pending;

// --- frame queue ---------------------------------------------------------------

std::mutex s_frame_mutex;
std::vector<DecodedDraw> s_building;
std::vector<DecodedDraw> s_ready;
bool s_ready_fresh = false;
// M2.3 parallel raw-draw frame (translated-shader path), same frame boundary.
std::vector<RawGuestDraw> s_raw_building;
std::vector<RawGuestDraw> s_raw_ready;
bool s_raw_ready_fresh = false;
std::atomic<uint64_t> s_frames{0};

// --- engine-loaded ALU constant blocks (NB:2697-2728) ---------------------------

std::mutex s_alu_mutex;
uint32_t s_alu_block_src[2][256] = {};  // [type][vec4 index] = guest phys of the vec4

// --- counters / logging ---------------------------------------------------------

std::atomic<uint64_t> s_total_begins{0};
std::atomic<uint64_t> s_completed{0};
std::atomic<uint64_t> s_submitted{0};
std::atomic<int> s_capture_log_budget{20};
std::atomic<int> s_census_log_budget{16};
std::atomic<int> s_unknown_stride_budget{8};

// --- empirical vertex declarations (spec B §3.1) --------------------------------

struct DeclInfo {
  bool valid = false;
  uint32_t pos_offset = 0;    // bytes
  uint32_t pos_fmt = 0;       // 25 = k_16_16, 38/57 = float3/4, 36/37 = float2
  bool has_color = false;
  uint32_t color_offset = 0;  // bytes; second (add-cxform) word at +4
  uint32_t color_count = 0;
  bool has_uv = false;
  uint32_t uv_offset = 0;     // bytes (float2)
  bool uv_via_consts = false; // UI quads: uv = (dot(pos4,c4), dot(pos4,c5))
  bool drop = false;          // decode + census only; do not render
};

DeclInfo DeclByStride(uint32_t stride_bytes) {
  DeclInfo d;
  if (stride_bytes == 12) {
    // THE title-art / Scaleform-quad decl (750x in the friend's runtime log):
    // pos k_16_16 @word0, two k_8_8_8_8 colors @words1-2 (cxform mult+add),
    // no UV attribute -> uv from c4/c5.
    d.valid = true;
    d.pos_fmt = 25;
    d.has_color = true;
    d.color_offset = 4;
    d.color_count = 2;
    d.uv_via_consts = true;
  } else if (stride_bytes == 8) {
    // Two-word Scaleform quad: pos k_16_16 @word0 + one k_8_8_8_8 color
    // @word1 (fade / solid-colour panels and the sky backdrop). UV from
    // consts when a texture is bound.
    d.valid = true;
    d.pos_fmt = 25;
    d.has_color = true;
    d.color_offset = 4;
    d.color_count = 1;
    d.uv_via_consts = true;
  } else if (stride_bytes == 28) {
    // CONFIRMED by disassembling its VS (346474C6087D8982, tools/nb_ucode_dump):
    //   vfetch_full  r2 fmt=38 float3 offset=0   -> position (w=1)
    //   vfetch_mini  r1 fmt=37 float2 offset=4w  -> UV, passed through raw
    //   vfetch_mini  r0 fmt=6  8888   offset=6w  -> vertex color
    //   o62.x = dp4(pos,c0), o62.y = dp4(pos,c1) -> same 2D transform family
    // (menu text / "Press START"; PS multiplies the k_8 glyph alpha.)
    d.valid = true;
    d.pos_fmt = 38;
    d.has_uv = true;
    d.uv_offset = 16;
    d.has_color = true;
    d.color_offset = 24;
    d.color_count = 1;
  }
  return d;
}

// --- decode helpers -------------------------------------------------------------

bool RowsValid(const float rows[8][4]) {
  // r0/r1 finite and not both-zero in .x/.y (NB:881-883).
  for (int r = 0; r < 2; ++r) {
    for (int i = 0; i < 4; ++i) {
      if (!std::isfinite(rows[r][i])) return false;
    }
  }
  return (rows[0][0] != 0.0f || rows[0][1] != 0.0f) &&
         (rows[1][0] != 0.0f || rows[1][1] != 0.0f);
}

// Reads rows c0..c5 from the device constant staging, overlaying any
// engine-loaded ALU block (which bypasses staging entirely).
void ReadConstRows(uint32_t device, float rows[8][4]) {
  for (int r = 0; r < 8; ++r) {
    for (int i = 0; i < 4; ++i) {
      rows[r][i] = ReadGuestF32(device + 1920 + 16u * r + 4u * i);
    }
  }
  std::lock_guard<std::mutex> lock(s_alu_mutex);
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return;
  for (int r = 0; r < 8; ++r) {
    const uint32_t src = s_alu_block_src[0][r];  // type 0 = vertex ALU constants
    if (!src) continue;
    const uint32_t* host = runtime->memory()->TranslatePhysical<const uint32_t*>(src);
    for (int i = 0; i < 4; ++i) {
      rows[r][i] = std::bit_cast<float>(std::byteswap(host[i]));
    }
  }
}

// Resolves a bound guest shader object to its registered ucode hash. The
// object header holds a physical-mapped virtual pointer (0xE0000000 range) to
// its ucode allocation; the ucode itself sits at a small offset inside that
// container, so probe the registry (keyed by IM_LOAD phys addr) across the
// container's first 2KB. Cached per object.
uint64_t ShaderObjHash(uint32_t obj) {
  static std::mutex s_mu;
  static std::unordered_map<uint32_t, uint64_t> s_cache;
  if (!obj) return 0;
  {
    std::lock_guard<std::mutex> lock(s_mu);
    const auto it = s_cache.find(obj);
    if (it != s_cache.end()) return it->second;
  }
  uint64_t hash = 0;
  auto* runtime = rex::Runtime::instance();
  for (uint32_t off = 0; off <= 0x40 && !hash && runtime && runtime->memory(); off += 4) {
    const uint32_t ptr = ReadGuestU32(obj + off);
    if (ptr < 0xE0000000u || ptr >= 0xFFFFF000u || (ptr & 0x3)) {
      continue;
    }
    // Follow the physical-mapped virtual pointer to host memory and scan for
    // any registered ucode blob (prefix + full-hash match). Handles
    // IM_LOAD_IMMEDIATE shaders, which have no registered phys address.
    const uint32_t* region = runtime->memory()->TranslateVirtual<const uint32_t*>(ptr);
    hash = restuff::native::FindShaderInRegion(region, 0x2000 / 4);
  }
  if (hash) {
    std::lock_guard<std::mutex> lock(s_mu);
    s_cache[obj] = hash;
  }
  return hash;
}

// Parses the first valid staged texture-fetch record (device+1152+slot*24).
// The staged record's `type` field is still 0 (D3D fills it during the
// flush), so validate by address/size only.
GuestTextureDesc ReadBoundTexture(uint32_t device) {
  GuestTextureDesc desc;
  for (uint32_t slot = 0; slot < 32; ++slot) {
    const uint32_t rec = device + 1152 + slot * 24;
    const uint32_t w0 = ReadGuestU32(rec + 0);
    const uint32_t w2 = ReadGuestU32(rec + 8);
    // xe_gpu_texture_fetch_t field extraction (verified layout, spec B §4.1):
    // w0: base_address in bits 12.. (20-bit page number is bits 12-31 of the
    //     dword after the type/sign fields) — the friend memcpy'd into the
    //     xenos struct; the bit positions below replicate its accessors.
    // Layout per xenos.h: dword0 = type:2, sign_x/y/z/w:2*4, clamp_x/y/z:3*3,
    //                     signed_rf_mode:1, dim_tbd... base_address is dword2's
    //                     bits? -- rather than re-derive, use the friend's
    //                     verified extraction: base_address = bits [12..31] of
    //                     dword 2? NO: use struct-free arithmetic below.
    (void)w0;
    (void)w2;
    // Read all six words and reinterpret via the packed struct in xenos.h.
    uint32_t words[6];
    for (int i = 0; i < 6; ++i) {
      words[i] = ReadGuestU32(rec + uint32_t(i) * 4);
    }
    // Offsets replicated from the friend's use of xe_gpu_texture_fetch_t:
    //   base_address = dword0 bits [12..31] (tf.base_address, 20 bits)
    //   endianness   = dword0 bits [10..11]
    //   pitch        = dword0? -- tf.pitch is dword? ... The friend memcpy'd
    // Simplest faithful approach: mirror the friend's struct memcpy.
    struct FetchBits {
      // dword_0
      uint32_t type : 2;            // 0 in staged records
      uint32_t sign_x : 2;
      uint32_t sign_y : 2;
      uint32_t sign_z : 2;
      uint32_t sign_w : 2;
      uint32_t clamp_x : 3;
      uint32_t clamp_y : 3;
      uint32_t clamp_z : 3;
      uint32_t signed_rf_mode_all : 1;
      uint32_t dim_tbd : 2;
      uint32_t pitch : 9;           // texels >> 5
      uint32_t tiled : 1;
      // dword_1
      uint32_t format : 6;
      uint32_t endianness : 2;
      uint32_t request_size : 2;
      uint32_t stacked : 1;
      uint32_t nearest_clamp_policy : 1;
      uint32_t base_address : 20;   // phys >> 12
      // dword_2
      uint32_t width : 13;
      uint32_t height : 13;
      uint32_t unused_size : 6;
    } tf;
    static_assert(sizeof(FetchBits) == 12);
    std::memcpy(&tf, words, sizeof(tf));
    if (tf.base_address == 0 || tf.width == 0) {
      continue;
    }
    desc.valid = true;
    desc.phys_addr = tf.base_address << 12;
    desc.width = tf.width + 1;
    desc.height = tf.height + 1;
    desc.pitch_texels = tf.pitch ? (tf.pitch << 5) : (tf.width + 1);
    desc.format = tf.format;
    desc.endian = tf.endianness;
    desc.tiled = tf.tiled != 0;
    break;
  }
  return desc;
}

}  // namespace

// --- capture API ----------------------------------------------------------------

void RecordPendingUpDraw(uint32_t device, bool indexed, uint32_t prim, uint32_t vertex_count,
                         uint32_t index_count, uint32_t stride) {
  // M2.1: all draws (UP included) are captured from the PM4 stream by the
  // walker in true submission order — the API-boundary capture would double
  // them. Keep the hooks for stats/diagnostics only.
  s_total_begins.fetch_add(1, std::memory_order_relaxed);
  (void)device; (void)indexed; (void)prim; (void)vertex_count; (void)index_count; (void)stride;
  return;
  std::lock_guard<std::mutex> lock(s_pending_mutex);
  ReadConstRows(device, s_pending.rows);  // Begin-time snapshot (constants re-staged before Begin N+1)
  s_pending.device = device;
  s_pending.indexed = indexed;
  s_pending.prim = prim;
  s_pending.vertex_count = vertex_count;
  s_pending.index_count = index_count;
  s_pending.stride = stride;
  s_pending.vs_obj = ReadGuestU32(device + 0x3248);
  s_pending.ps_obj = ReadGuestU32(device + 0x3244);
  s_pending.vtx_addr = ReadGuestU32(device + 13656);
  s_pending.vtx_dwords = ReadGuestU32(device + 13664);
  if (indexed) {
    s_pending.idx_addr = ReadGuestU32(device + 13660);
    s_pending.idx_dwords = ReadGuestU32(device + 13668);
  } else {
    s_pending.idx_addr = 0;
    s_pending.idx_dwords = 0;
  }
  s_pending.valid = s_pending.vtx_addr != 0 && s_pending.vtx_dwords != 0;
}

void StageEngineAluBlock(uint32_t type, uint32_t offset_vec4, uint32_t physaddr,
                         uint32_t count_vec4) {
  std::lock_guard<std::mutex> lock(s_alu_mutex);
  const uint32_t t = type & 1;
  for (uint32_t i = 0; i < count_vec4 && (offset_vec4 + i) < 256; ++i) {
    s_alu_block_src[t][offset_vec4 + i] = physaddr + i * 16;
  }
}

void CompletePendingUpDraw() {
  PendingUpDraw p;
  {
    std::lock_guard<std::mutex> lock(s_pending_mutex);
    if (!s_pending.valid) {
      return;
    }
    p = s_pending;
    s_pending.valid = false;
  }
  s_completed.fetch_add(1, std::memory_order_relaxed);

  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return;

  const uint32_t vtx_bytes = p.vtx_dwords * 4;
  if (!vtx_bytes || vtx_bytes > (8u << 20) || p.stride == 0 || p.vertex_count == 0) {
    return;
  }

  if (s_capture_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    const uint32_t* vtx = runtime->memory()->TranslateVirtual<const uint32_t*>(p.vtx_addr);
    REXLOG_INFO(
        "[gd3d] UP draw {} prim={} verts={} stride={} idx={} raw: {:08X} {:08X} {:08X} {:08X}",
        p.indexed ? "indexed" : "auto", p.prim, p.vertex_count, p.stride, p.index_count,
        std::byteswap(vtx[0]), std::byteswap(vtx[1]), std::byteswap(vtx[2]),
        std::byteswap(vtx[3]));
  }

  DeclInfo decl = DeclByStride(p.stride);
  if (!decl.valid) {
    if (s_unknown_stride_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const uint32_t* vtx = runtime->memory()->TranslateVirtual<const uint32_t*>(p.vtx_addr);
      REXLOG_INFO("[gd3d] up decl UNKNOWN stride={} raw v0: {:08X} {:08X} {:08X}", p.stride,
                  std::byteswap(vtx[0]), std::byteswap(vtx[1]), std::byteswap(vtx[2]));
    }
    return;
  }

  // Constants c0..c5 from the Begin-time snapshot (completion-time reads see
  // the NEXT draw's transform — the game re-stages constants before Begin N+1).
  const float (&rows)[8][4] = p.rows;
  const bool rows_valid = RowsValid(rows);

  // Copy + endian-normalize the vertex stream. Primary rule: UP stream fetch
  // constant slot 95's endian field (shadowed from the PM4 SET_CONSTANT
  // writes); fallback: k_16_16 streams are written GPU-ready, float streams
  // big-endian (verified NB:2794-2809).
  std::vector<uint32_t> words(p.vtx_dwords);
  {
    const uint32_t* src = runtime->memory()->TranslateVirtual<const uint32_t*>(p.vtx_addr);
    std::memcpy(words.data(), src, vtx_bytes);
  }
  // Endianness by declaration, NOT by the shadowed fetch constant: the shadow
  // updates when PM4 executes (at kick), which is stale for interleaved draw
  // families. The friend's verified rule: packed k_16_16 streams are written
  // GPU-ready; float streams are big-endian (text collapsed to a point when
  // its floats were read unswapped as denormals).
  const bool gpu_ready = decl.pos_fmt == 25;
  if (!gpu_ready) {
    for (auto& w : words) w = std::byteswap(w);
  }

  const uint32_t stride_words = p.stride / 4;
  const uint32_t usable_verts =
      std::min<uint32_t>(p.vertex_count, stride_words ? p.vtx_dwords / stride_words : 0);
  if (usable_verts == 0) return;

  // Texture (first valid staged fetch record).
  GuestTextureDesc tex = ReadBoundTexture(p.device);

  // --- per-vertex decode (spec B §3.3-3.6) ---
  struct TmpVert {
    float x, y;      // D3D NDC, y-up
    float u, v;
    uint32_t rgba;   // color1: R | G<<8 | B<<16 | A<<24
    uint32_t rgba2;  // color2 (quad family): .r = tex lerp, .a = alpha gate
  };
  std::vector<TmpVert> tmp(usable_verts);

  // Frontbuffer dims for the pixel->NDC fallback.
  uint32_t fb_phys, fb_w, fb_h;
  restuff::native::GetFrontbuffer(fb_phys, fb_w, fb_h);
  const float gw = fb_w ? float(fb_w) : 1280.0f;
  const float gh = fb_h ? float(fb_h) : 720.0f;

  bool any_nonzero_color = false;
  bool all_finite = true;
  float min_x = 1e9f, min_y = 1e9f, max_x = -1e9f, max_y = -1e9f;

  for (uint32_t vi = 0; vi < usable_verts; ++vi) {
    const uint32_t* v = words.data() + size_t(vi) * stride_words;
    TmpVert& o = tmp[vi];
    float pos4[4] = {0, 0, 0, 1};  // raw pre-transform position (for UV-from-consts)
    float x = 0, y = 0;

    if (decl.pos_fmt == 25) {  // k_16_16 — the title-art case
      const uint32_t w = v[decl.pos_offset / 4];
      const float sx = float(int16_t(w >> 16));
      const float sy = float(int16_t(w & 0xFFFF));
      pos4[0] = sx;
      pos4[1] = sy;
      if (rows_valid) {
        x = sx * rows[0][0] + sy * rows[0][1] + rows[0][3];
        y = sx * rows[1][0] + sy * rows[1][1] + rows[1][3];
      } else {
        const float px = sx / 16.0f, py = sy / 16.0f;
        x = px / gw * 2.0f - 1.0f;
        y = 1.0f - py / gh * 2.0f;
      }
    } else if (decl.pos_fmt == 38 || decl.pos_fmt == 57) {  // float3/float4
      float pf[4] = {0, 0, 0, 1};
      std::memcpy(pf, v + decl.pos_offset / 4, 12);
      pos4[0] = pf[0]; pos4[1] = pf[1]; pos4[2] = pf[2];
      auto dot3 = [&](const float* a, const float* r) {
        return a[0] * r[0] + a[1] * r[1] + a[2] * r[2];
      };
      const float cw = dot3(pf, rows[3]) + rows[3][3];
      const bool row3_nontrivial =
          rows[3][0] != 0.0f || rows[3][1] != 0.0f || rows[3][2] != 0.0f;
      if (rows_valid && row3_nontrivial) {
        if (cw <= 0.001f) { all_finite = false; break; }
        x = (dot3(pf, rows[0]) + rows[0][3]) / cw;
        y = (dot3(pf, rows[1]) + rows[1][3]) / cw;
      } else if (rows_valid) {
        x = dot3(pf, rows[0]) + rows[0][3];
        y = dot3(pf, rows[1]) + rows[1][3];
      } else if (std::fabs(pf[0]) <= 8.0f && std::fabs(pf[1]) <= 8.0f) {
        x = pf[0]; y = pf[1];
      } else {
        x = pf[0] / gw * 2.0f - 1.0f;
        y = 1.0f - pf[1] / gh * 2.0f;
      }
    } else {  // float2
      float gx, gy;
      std::memcpy(&gx, v + decl.pos_offset / 4, 4);
      std::memcpy(&gy, v + decl.pos_offset / 4 + 1, 4);
      pos4[0] = gx; pos4[1] = gy;
      if (std::fabs(gx) <= 8.0f && std::fabs(gy) <= 8.0f) {
        x = gx; y = gy;
      } else {
        x = gx / gw * 2.0f - 1.0f;
        y = 1.0f - gy / gh * 2.0f;
      }
    }

    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 64.0f || std::fabs(y) > 64.0f) {
      all_finite = false;
      break;
    }
    o.x = x; o.y = y;
    min_x = std::min(min_x, x); max_x = std::max(max_x, x);
    min_y = std::min(min_y, y); max_y = std::max(max_y, y);

    // UV
    float u = 0, vv = 0;
    if (decl.uv_via_consts) {
      u = pos4[0] * rows[4][0] + pos4[1] * rows[4][1] + pos4[2] * rows[4][2] + rows[4][3];
      vv = pos4[0] * rows[5][0] + pos4[1] * rows[5][1] + pos4[2] * rows[5][2] + rows[5][3];
    } else if (decl.has_uv) {
      std::memcpy(&u, v + decl.uv_offset / 4, 4);
      std::memcpy(&vv, v + decl.uv_offset / 4 + 1, 4);
      if (!std::isfinite(u) || !std::isfinite(vv)) { u = 0; vv = 0; }
    }
    o.u = u; o.v = vv;

    // Colors: logical D3DCOLOR 0xAARRGGBB after normalization. The quad PS
    // (98E0B121, disassembled) computes c = mix(color1, tex, color2.r) and
    // a = c.a * color2.a — no white-substitute needed; all-zero draws
    // (filter meshes) become genuinely invisible.
    auto repack = [](uint32_t c) -> uint32_t {
      const uint32_t a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF;
      const uint32_t g = (c >> 8) & 0xFF, b = c & 0xFF;
      return r | (g << 8) | (b << 16) | (a << 24);
    };
    uint32_t rgba = 0xFFFFFFFFu;
    uint32_t rgba2 = 0xFFFFFFFFu;  // default: pure texture, full alpha
    if (decl.has_color) {
      const uint32_t c1 = v[decl.color_offset / 4];
      rgba = repack(c1);
      if ((c1 & 0x00FFFFFFu) != 0) any_nonzero_color = true;
      if (decl.color_count >= 2) {
        rgba2 = repack(v[decl.color_offset / 4 + 1]);
      } else if (decl.pos_fmt != 25 || decl.has_uv) {
        // Text family: color1 is the text colour; texture gates alpha in the
        // dedicated pipeline. rgba2 unused.
        rgba2 = 0xFFFFFFFFu;
      } else if (rgba == 0) {
        // stride-8 single-colour quads: keep legacy white for zero colour.
        rgba = 0xFFFFFFFFu;
      }
    }
    o.rgba = rgba;
    o.rgba2 = rgba2;
  }
  (void)any_nonzero_color;
  if (!all_finite) return;

  // Pixel-space UVs (max|uv| > 2) -> normalize by texture dims. ONLY for
  // attribute UVs: const-derived UVs are affine atlas mappings in WRAP space
  // and legitimately exceed 2 on batched quads — dividing them crushes the
  // span to ~0 and renders 100x-zoomed fragments.
  if (tex.valid && decl.has_uv && !decl.uv_via_consts) {
    float max_uv = 0;
    for (const auto& t : tmp) {
      max_uv = std::max({max_uv, std::fabs(t.u), std::fabs(t.v)});
    }
    if (max_uv > 2.0f && tex.width && tex.height) {
      for (auto& t : tmp) {
        t.u /= float(tex.width);
        t.v /= float(tex.height);
      }
    }
  }
  // Const-derived UVs: wrap-shift whole-quad translates (authored tu/tv like
  // exactly -1.0) into [0,1]. Draws whose UV SPAN exceeds one tile are
  // Scaleform glow/blur FILTER meshes (vertex dump: the crisp element quad
  // sits unreferenced in the buffer while all indices hit multi-tap taps with
  // off-atlas UVs) — they belong in offscreen filter buffers we don't emulate;
  // compositing them to screen is the streak/stacked-copy corruption. Drop.
  bool drop_filter_mesh = false;
  if (tex.valid && decl.uv_via_consts && !tmp.empty()) {
    float u_min = 1e9f, u_max = -1e9f, v_min = 1e9f, v_max = -1e9f;
    for (const auto& t : tmp) {
      u_min = std::min(u_min, t.u);
      u_max = std::max(u_max, t.u);
      v_min = std::min(v_min, t.v);
      v_max = std::max(v_max, t.v);
    }
    if (u_max - u_min <= 1.001f && v_max - v_min <= 1.001f) {
      const float su = std::floor(u_min), sv = std::floor(v_min);
      if (su != 0.0f || sv != 0.0f) {
        for (auto& t : tmp) {
          t.u -= su;
          t.v -= sv;
        }
      }
    }
    // NOTE: dropping multi-tile-span draws was tried and inverted (killed real
    // art, kept streaks) — per-shader UV semantics are the true discriminator,
    // which needs the M2 shader recompiler. Render everything until then.
  }

  // Indices -> triangle list.
  std::vector<uint16_t> idx;
  if (p.indexed && p.idx_addr && p.index_count) {
    const uint16_t* src = runtime->memory()->TranslateVirtual<const uint16_t*>(p.idx_addr);
    idx.resize(p.index_count);
    for (uint32_t i = 0; i < p.index_count; ++i) {
      idx[i] = std::byteswap(src[i]);  // big-endian in guest memory
    }
  } else {
    idx.resize(usable_verts);
    for (uint32_t i = 0; i < usable_verts; ++i) idx[i] = uint16_t(i);
  }

  DecodedDraw out;
  out.tex = tex;
  out.family = (p.stride == 28) ? DrawFamily::kText : DrawFamily::kQuad;
  auto emit = [&](uint16_t i) {
    if (i >= usable_verts) return false;
    const TmpVert& t = tmp[i];
    out.verts.push_back({t.x, t.y, t.u, t.v, t.rgba, t.rgba2});
    return true;
  };
  bool ok = true;
  if (p.prim == 4) {  // triangle list
    for (size_t i = 0; i + 2 < idx.size() && ok; i += 3) {
      ok = emit(idx[i]) && emit(idx[i + 1]) && emit(idx[i + 2]);
    }
  } else if (p.prim == 5) {  // triangle fan -> list
    for (size_t i = 1; i + 1 < idx.size() && ok; ++i) {
      ok = emit(idx[0]) && emit(idx[i]) && emit(idx[i + 1]);
    }
  } else if (p.prim == 6) {  // triangle strip -> list
    for (size_t i = 0; i + 2 < idx.size() && ok; ++i) {
      ok = emit(idx[i]) && emit(idx[i + 1]) && emit(idx[i + 2]);
    }
  } else {
    return;  // points/lines/quads: not observed in UP draws
  }
  if (!ok || out.verts.empty()) return;

  // Census: dump full decode inputs for every draw of a few SETTLED frames
  // (the title intro animates ~20s; early frames mislead). Transform/UV
  // archaeology for the panels that mis-render.
  {
    const uint64_t f = s_frames.load(std::memory_order_relaxed);
    // Guest presents ~30fps: 300 = ~10s (intro), 900 = ~30s, 1350 = ~45s (settled).
    const bool census_frame = (f == 300) || (f == 900) || (f == 1350);
    if (census_frame || s_census_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_INFO(
          "[gd3d] decode f={} stride={} verts={} tris={} bbox=({:.2f},{:.2f})..({:.2f},{:.2f}) tex={} fmt={} {}x{} addr=0x{:08X}{}",
          f, p.stride, usable_verts, out.verts.size() / 3, min_x, min_y, max_x, max_y,
          tex.valid, tex.format, tex.width, tex.height, tex.phys_addr,
          decl.drop ? " DROPPED" : "");
      REXLOG_INFO(
          "[gd3d]   c0=({:.3f},{:.3f},{:.3f},{:.3f}) c1=({:.3f},{:.3f},{:.3f},{:.3f}) c2=({:.3f},{:.3f},{:.3f},{:.3f}) c3=({:.3f},{:.3f},{:.3f},{:.3f})",
          rows[0][0], rows[0][1], rows[0][2], rows[0][3], rows[1][0], rows[1][1], rows[1][2],
          rows[1][3], rows[2][0], rows[2][1], rows[2][2], rows[2][3], rows[3][0], rows[3][1],
          rows[3][2], rows[3][3]);
      REXLOG_INFO(
          "[gd3d]   c4=({:.5f},{:.5f},{:.5f},{:.5f}) c5=({:.5f},{:.5f},{:.5f},{:.5f}) uv0=({:.3f},{:.3f}) vs={:016X} ps={:016X}",
          rows[4][0], rows[4][1], rows[4][2], rows[4][3], rows[5][0], rows[5][1], rows[5][2],
          rows[5][3], tmp.empty() ? 0.f : tmp[0].u, tmp.empty() ? 0.f : tmp[0].v,
          ShaderObjHash(p.vs_obj), ShaderObjHash(p.ps_obj));
      REXLOG_INFO(
          "[gd3d]   c6=({:.5f},{:.5f},{:.5f},{:.5f}) c7=({:.5f},{:.5f},{:.5f},{:.5f}) v0raw={:08X} {:08X} v0=({:.2f},{:.2f})",
          rows[6][0], rows[6][1], rows[6][2], rows[6][3], rows[7][0], rows[7][1], rows[7][2],
          rows[7][3], words[0], stride_words > 1 ? words[1] : 0,
          tmp.empty() ? 0.f : tmp[0].x, tmp.empty() ? 0.f : tmp[0].y);
    }
  }
  if (decl.drop || drop_filter_mesh) return;

  // One-shot full-vertex dump for the lettering-atlas draws (logo repeats 4x:
  // mask geometry vs off-atlas padding archaeology).
  {
    static std::atomic<int> s_vert_dump_budget{2};
    if (tex.valid && tex.format == 2 && tex.width == 1024 &&
        s_frames.load(std::memory_order_relaxed) >= 900 &&
        s_vert_dump_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_INFO("[gd3d] VERTDUMP stride={} verts={} idx={} tex=0x{:08X}", p.stride,
                  usable_verts, p.index_count, tex.phys_addr);
      for (uint32_t vi = 0; vi < usable_verts; ++vi) {
        REXLOG_INFO("[gd3d]   v{}: pos=({:.3f},{:.3f}) uv=({:.3f},{:.3f}) rgba={:08X}", vi,
                    tmp[vi].x, tmp[vi].y, tmp[vi].u, tmp[vi].v, tmp[vi].rgba);
      }
      if (p.indexed && !idx.empty()) {
        std::string s;
        for (size_t i = 0; i < idx.size() && i < 36; ++i) {
          s += std::to_string(idx[i]);
          s += ' ';
        }
        REXLOG_INFO("[gd3d]   idx: {}", s);
      }
    }
  }

  std::lock_guard<std::mutex> lock(s_frame_mutex);
  if (s_building.size() < 4096) {  // runaway guard
    s_building.push_back(std::move(out));
    s_submitted.fetch_add(1, std::memory_order_relaxed);
  }
}

void SubmitDecodedDraw(DecodedDraw&& draw) {
  std::lock_guard<std::mutex> lock(s_frame_mutex);
  if (s_building.size() < 4096) {
    s_building.push_back(std::move(draw));
    s_submitted.fetch_add(1, std::memory_order_relaxed);
  }
}

void SubmitRawDraw(RawGuestDraw&& draw) {
  std::lock_guard<std::mutex> lock(s_frame_mutex);
  if (s_raw_building.size() < 8192) {
    s_raw_building.push_back(std::move(draw));
    s_submitted.fetch_add(1, std::memory_order_relaxed);
  }
}

// M3.13: the front-buffer pointer travels WITH the published frame. Present
// used to pair the frame it held with the LIVE VdSwap pointer, which can
// already belong to the next guest frame -- displaying the buffer the game is
// mid-rebuilding (one-frame-off), which flickers with per-frame splotches.
static uint32_t s_raw_ready_fb = 0;  // s_frame_mutex

static void D3dCensusFrame(uint32_t pm4_draws);  // M4.39, defined below

void EndGuestFrame(uint32_t frame_fb) {
  std::lock_guard<std::mutex> lock(s_frame_mutex);
  {  // M4.39: draws captured this guest frame -- the gate's denominator.
    uint32_t n = 0;
    for (const auto& d : s_raw_building)
      if (!d.is_resolve) ++n;
    D3dCensusFrame(n);
  }
  // RESTUFF_CAPTURE_TRACE=1: what the guest actually submitted this frame, and
  // whether the previous frame was still unconsumed (its draws are about to be
  // discarded by the move below -- a silent whole-frame drop when the present
  // thread is slower than the guest).
  if (getenv("RESTUFF_CAPTURE_TRACE")) {
    static std::atomic<uint64_t> s_n{0}, s_dropped{0};
    const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (s_raw_ready_fresh) s_dropped.fetch_add(1, std::memory_order_relaxed);
    if (n >= 2500 && n % 4 == 0) {
      uint32_t col = 0, pre = 0, res = 0;
      for (const auto& d : s_raw_building) {
        if (d.is_resolve) { ++res; continue; }
        if (d.color_mask) ++col; else ++pre;
      }
      REXLOG_INFO("[CAPTURE] guest_frame#{} submitted={} colour={} prepass={} resolves={} "
                  "fb=0x{:08X} prev_unconsumed={} total_dropped={}",
                  n, uint32_t(s_raw_building.size()), col, pre, res, frame_fb,
                  s_raw_ready_fresh ? 1 : 0, s_dropped.load(std::memory_order_relaxed));
    }
  }
  s_ready = std::move(s_building);
  s_building.clear();
  s_ready_fresh = true;
  s_raw_ready = std::move(s_raw_building);
  s_raw_building.clear();
  s_raw_ready_fresh = true;
  s_raw_ready_fb = frame_fb;
  s_frames.fetch_add(1, std::memory_order_relaxed);
}

// M2.4: front buffer pointer from the most recent VdSwap.
static std::atomic<uint32_t> s_front_buffer_phys{0};
void SetFrontBufferPhys(uint32_t phys) {
  s_front_buffer_phys.store(phys, std::memory_order_relaxed);
}
uint32_t GetFrontBufferPhys() { return s_front_buffer_phys.load(std::memory_order_relaxed); }

// M3.52: non-destructive "is a freshly-published guest frame waiting?" -- lets
// the present thread skip the expensive full-scene re-record when the guest
// produced nothing since the last present (it runs ~20fps but we present 60Hz).
bool HasRawFrame() {
  std::lock_guard<std::mutex> lock(s_frame_mutex);
  return s_raw_ready_fresh;
}

uint64_t GuestFrameCount() { return s_frames.load(std::memory_order_relaxed); }

bool ConsumeRawFrame(std::vector<RawGuestDraw>& out, uint32_t* frame_fb) {
  std::lock_guard<std::mutex> lock(s_frame_mutex);
  if (!s_raw_ready_fresh) return false;
  out = std::move(s_raw_ready);
  s_raw_ready.clear();
  s_raw_ready_fresh = false;
  if (frame_fb) *frame_fb = s_raw_ready_fb;
  return true;
}

bool ConsumeReadyFrame(std::vector<DecodedDraw>& out) {
  std::lock_guard<std::mutex> lock(s_frame_mutex);
  if (!s_ready_fresh) {
    return false;
  }
  out = std::move(s_ready);
  s_ready.clear();
  s_ready_fresh = false;
  return true;
}

// ---------------------------------------------------------------------------
// M4.39 (RESTUFF_D3DCENSUS=1): the Part-B gate, and nothing more.
//
// The whole "hook the guest D3D9 API instead of walking PM4" plan rests on one
// unmeasured assumption: that the engine actually CALLS the D3D9 state setters
// per draw, rather than inlining them or poking registers directly. This
// counts each candidate setter and prints it against the number of draws the
// PM4 walker captured in the same frames. Verdict is read straight off the
// ratio:
//   setter calls/frame ~ draws/frame  -> the API sees the same traffic; B is
//                                        viable and B2 (vertex declarations)
//                                        can replace our vfetch guessing.
//   setter calls/frame << draws/frame -> the engine bypasses the API; the
//                                        whole of Part B is dead, abort.
// Pure log-and-forward: every hook calls straight through, and with the env
// unset the counters are never touched.
// ---------------------------------------------------------------------------
// enum D3dFn lives in up_draws.h -- hooks.cpp counts the two draw entries it
// already owns.
static const char* const kD3dFnName[kD3dFnCount] = {
    "SetVertexShader", "SetPixelShader", "StreamDecl", "Resolve",
    "drawB",           "drawC",          "drawF",      "drawG",
    "BeginVerts",      "BeginIdxVerts",
    "emitA_82F398F0",  "emitB_82F3E088", "emitC_82F46D30",
    "PM4_DRAW_INDX",   "PM4_DRAW_INDX_2", "PM4_DRAW_INDX_2_BIN",
};
static_assert(sizeof(kD3dFnName) / sizeof(kD3dFnName[0]) == kD3dFnCount,
              "name table must cover every D3dFn");
static std::atomic<uint64_t> s_d3d_calls[kD3dFnCount];

bool D3dCensusOn() {
  static const bool on = getenv("RESTUFF_D3DCENSUS") != nullptr;
  return on;
}
void D3dCensusHit(int fn) {
  if (D3dCensusOn()) s_d3d_calls[fn].fetch_add(1, std::memory_order_relaxed);
}

// Called from EndGuestFrame with the count of draws the PM4 walker captured
// this guest frame -- the denominator the whole gate turns on.
static void D3dCensusFrame(uint32_t pm4_draws) {
  if (!D3dCensusOn()) return;
  static uint64_t s_frames_n = 0, s_pm4_total = 0;
  ++s_frames_n;
  s_pm4_total += pm4_draws;
  if (s_frames_n % 200 != 0) return;
  std::string parts;
  for (int i = 0; i < kD3dFnCount; ++i) {
    char buf[96];
    snprintf(buf, sizeof(buf), " %s=%.1f", kD3dFnName[i],
             double(s_d3d_calls[i].load(std::memory_order_relaxed)) / double(s_frames_n));
    parts += buf;
  }
  REXLOG_INFO("[D3DCENSUS] over {} guest frames: pm4_draws/frame={:.1f} | per-frame calls:{}",
              s_frames_n, double(s_pm4_total) / double(s_frames_n), parts);
}

void DumpDrawStatsIfDue() {
  using clock = std::chrono::steady_clock;
  static std::atomic<int64_t> s_last_dump{0};
  const int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(clock::now().time_since_epoch()).count();
  int64_t last = s_last_dump.load(std::memory_order_relaxed);
  if (now - last < 5 || !s_last_dump.compare_exchange_strong(last, now)) {
    return;
  }
  REXLOG_INFO("[gd3d] stats begins={} completed={} submitted={} frames={}",
              s_total_begins.load(std::memory_order_relaxed),
              s_completed.load(std::memory_order_relaxed),
              s_submitted.load(std::memory_order_relaxed),
              s_frames.load(std::memory_order_relaxed));
}

}  // namespace restuff::renderer

// --- strong-symbol overrides ------------------------------------------------
// The recompiler emits every guest function as a weak alias of __imp__<name>
// (restuff_init.h DEFINE_REX_FUNC); a strong extern "C" definition replaces it
// everywhere, including indirect calls through PPCFuncMappings.

// D3DDevice_BeginVertices(device, prim, vertexCount, strideBytes)
//   -> r3 = vertex write ptr (guest virtual), 0 on OOM.
REX_EXTERN(__imp__sub_82F37EF0);
REX_HOOK_RAW(sub_82F37EF0) {
  restuff::renderer::CompletePendingUpDraw();  // previous UP draw is complete by now
  restuff::renderer::DumpDrawStatsIfDue();
  restuff::renderer::D3dCensusHit(restuff::renderer::kFnBeginVertices);  // M4.39b
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32;
  const uint32_t vcount = ctx.r5.u32, stride = ctx.r6.u32;
  __imp__sub_82F37EF0(ctx, base);
  if (ctx.r3.u32 != 0) {
    restuff::renderer::RecordPendingUpDraw(device, /*indexed=*/false, prim, vcount, 0, stride);
  }
}

// D3DDevice_BeginIndexedVertices(device, prim, r5, vertexCount, indexCount,
//   indexFlags(bit2 = 32-bit indices), strideBytes, &outIdxPtr, ...) -> HRESULT.
REX_EXTERN(__imp__sub_82F383D0);
REX_HOOK_RAW(sub_82F383D0) {
  restuff::renderer::CompletePendingUpDraw();
  restuff::renderer::D3dCensusHit(restuff::renderer::kFnBeginIndexedVertices);  // M4.39b
  const uint32_t device = ctx.r3.u32, prim = ctx.r4.u32;
  const uint32_t vcount = ctx.r6.u32, icount = ctx.r7.u32;
  const uint32_t iflags = ctx.r8.u32, stride = ctx.r9.u32;
  __imp__sub_82F383D0(ctx, base);
  if (ctx.r3.u32 == 0) {  // S_OK
    if (iflags & 4) {
      static std::atomic<int> s_once{1};
      if (s_once.exchange(0)) {
        REXLOG_INFO("[gd3d] 32-bit-index UP draw skipped (unhandled)");
      }
    } else {
      restuff::renderer::RecordPendingUpDraw(device, /*indexed=*/true, prim, vcount, icount,
                                             stride);
    }
  }
}

// D3D_WriteLoadAluConstantPacket(device, type, offset_vec4, physaddr, count_vec4).
REX_EXTERN(__imp__sub_82F3FDC8);
REX_HOOK_RAW(sub_82F3FDC8) {
  restuff::renderer::StageEngineAluBlock(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  __imp__sub_82F3FDC8(ctx, base);
}

// --- M4.39: RESTUFF_D3DCENSUS gate hooks ------------------------------------
// Pure counters. Each calls straight through and does nothing else, so these
// are inert with the env unset. Addresses from the friend's live-verified map
// (lua_mods/src/renderer/guest_d3d.h); the ones marked "probable" there are
// exactly what this census is meant to confirm or refute -- a candidate that
// never fires tells us the address is wrong OR the engine does not call it,
// and the per-frame ratio distinguishes those from a setter that tracks draws.
#define RESTUFF_D3DCENSUS_HOOK(addr, fn)     \
  REX_EXTERN(__imp__sub_##addr);             \
  REX_HOOK_RAW(sub_##addr) {                 \
    restuff::renderer::D3dCensusHit(fn);     \
    __imp__sub_##addr(ctx, base);            \
  }

RESTUFF_D3DCENSUS_HOOK(82F2E658, restuff::renderer::kFnSetVertexShader)
RESTUFF_D3DCENSUS_HOOK(82F2E498, restuff::renderer::kFnSetPixelShader)
RESTUFF_D3DCENSUS_HOOK(82F30278, restuff::renderer::kFnStreamDecl)  // -> device+0x2ED8; the B2 target
RESTUFF_D3DCENSUS_HOOK(82F34FB0, restuff::renderer::kFnResolve)
RESTUFF_D3DCENSUS_HOOK(82F36458, restuff::renderer::kFnDrawB)
RESTUFF_D3DCENSUS_HOOK(82F36830, restuff::renderer::kFnDrawC)
RESTUFF_D3DCENSUS_HOOK(82F398F0, restuff::renderer::kFnEmitA)  // M4.39c candidates
RESTUFF_D3DCENSUS_HOOK(82F3E088, restuff::renderer::kFnEmitB)
RESTUFF_D3DCENSUS_HOOK(82F46D30, restuff::renderer::kFnEmitC)
// 82F38988 / 82F38D78 (the two CONFIRMED draw entries) are already hooked in
// src/hooks.cpp for the ibwatch/DrawBt instrumentation -- a second definition
// of the same strong symbol does not link. They call D3dCensusHit from there.

#undef RESTUFF_D3DCENSUS_HOOK

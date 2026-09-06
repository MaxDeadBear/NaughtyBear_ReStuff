#include "renderer/native_backend_vk.h"

#include <algorithm>
#include <array>  // MSVC's <utility> only forward-declares std::array
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/xenos.h>  // PM4_* opcodes, GpuSwap, Endian, kSwapSignature (all inline/header-only)
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/xthread.h>

#include "renderer/shader_pipeline.h"  // M2.3 translate+compile cache
#include "renderer/up_draws.h"  // PM4 draw capture feeds the same frame queue

// Port of NaughtyBear_ReStuff-lua_mods/src/native_backend.cpp (the friend's
// proven mini-CP), minus D3D12, shader analysis (Shader/ShaderInterpreter are
// not exported by librexruntime on Linux), and the PM4 draw capture. Line
// references in comments are into that file ("NB:").

// M2.3: route draws through the Xenos-ucode->GLSL translator path instead of the
// hand-written kPm4Decls[] heuristic. Off by default; the heuristic render stays
// the baseline until the translated path is proven. Defined at global scope
// (like use_native_renderer) so the storage symbol matches the REXCVAR_DECLARE.
// M4.6: THE 30FPS CAP, PROMOTED TO A DEFAULT AT LAST. The uncap has been
// "done" in at least three prior sessions and reverted every time -- because
// it only ever existed as the RESTUFF_VBLANK_HZ env var (M3.131, marked
// EXPERIMENTAL), whose value dies with the shell that set it. Every new
// launcher, batch file, or machine silently resurrected the hardcoded 60Hz
// (= 30fps: this title presents every 2nd vblank). It is now a CVAR with a
// 120 default (= 60fps, matching the PS3 SKU, which presents every vblank),
// persisted via restuff.toml; the env var survives as a per-run override
// (RESTUFF_VBLANK_HZ=60 restores retail pacing for A/B). Do NOT demote this
// back to env-only.
REXCVAR_DEFINE_INT32(vblank_hz, 120, "Performance",
                     "Guest vblank rate in Hz. The game presents every 2nd vblank: 120 = 60 fps, "
                     "60 = retail 30 fps.");

REXCVAR_DEFINE_BOOL(use_translated_shaders, false, "Renderer",
                    "Render via the ucode->GLSL translator instead of decl heuristics");

namespace restuff::native {
namespace {

namespace xenos = rex::graphics::xenos;
namespace spc = restuff::renderer::spc;

// GPU register shadow file (the SDK RegisterFile is not exported; NB:258).
// Covers 0x0000-0x7FFF: regs 0x2000+, ALU consts 0x4000+, fetch 0x4800+.
uint32_t s_reg_shadow[0x8000];

constexpr uint32_t kRegCpRbWptr = 0x01C5;
constexpr uint32_t kRegCoherStatusHost = 0x0A31;
constexpr uint32_t kRegScratchUmsk = 0x01DC;
constexpr uint32_t kRegScratchAddr = 0x01DD;
constexpr uint32_t kRegScratchReg0 = 0x0578;
constexpr uint32_t kRegScratchReg7 = 0x057F;

struct StreamFrame {
  uint32_t base_phys = 0;
  uint32_t size_words = 0;
  uint32_t pos = 0;
  bool is_ring = false;
  // M3.33: walker-owned snapshot of an indirect buffer's contents, captured at
  // the IB call. The game recycles its IB pools on its own CPU-side frame
  // pacing (not rptr), so while the machine is parked at a WAIT_REG_MEM the
  // pool memory can be rewritten under us -- parsing the copy is immune.
  // Null for the ring (rptr-throttled by the driver layer) and when
  // RESTUFF_NO_IB_SNAPSHOT=1.
  std::shared_ptr<std::vector<uint32_t>> owned;
};

struct Backend {
  std::atomic<bool> active{false};

  std::mutex pm4_mutex;
  std::vector<StreamFrame> pm4_stack;  // [0] = primary ring
  uint32_t ring_wptr_words = 0;
  std::atomic<bool> pm4_parked{false};
  // M3.32: true-rptr reporting. ring_ib_call_pos = ring position of the IB
  // call that opened the currently-nested IB (the oldest ring packet whose
  // effects are NOT yet consumed while parked inside it). last_kick_wptr_raw =
  // the raw write pointer of the latest kick, for resume-time rptr refresh.
  uint32_t ring_ib_call_pos = 0;
  uint32_t last_kick_wptr_raw = 0;
  // M3.55: async-mode lock-free kick. The guest render thread stores its write
  // pointer here WITHOUT taking pm4_mutex (the worker holds that for the whole
  // ~14ms walk; M3.44 blocked on it, making async neutral). The worker applies
  // this to ring_wptr_words/last_kick_wptr_raw under the mutex before it walks.
  std::atomic<uint32_t> pending_wptr_raw{0};

  std::atomic<uint32_t> guest_callback{0};
  std::atomic<uint32_t> guest_callback_data{0};
  std::atomic<uint32_t> ring_writeback_addr{0};

  std::atomic<uint64_t> pm4_event_ext_writes{0};  // M3.36 EVENT_WRITE_EXT writebacks
  std::atomic<int64_t> walker_us{0};  // M3.38: kick-thread time inside walker+capture
  std::atomic<uint32_t> pending_interrupt_mask{0};
  std::atomic<bool> seen_pm4_interrupt{false};
  std::atomic<uint32_t> pending_swap_acks{0};
  std::atomic<uint64_t> vblank_counter{0};
  // M3.34: event-driven interrupt delivery. The walker signals when it queues
  // an interrupt/swap-ack; the pump wakes immediately instead of on its next
  // 60Hz tick. Every ms the guest ISR is late is a ms the PM4 machine stays
  // PARKED at the frame's WAIT while the game's CPU overwrites command pools.
  std::mutex pump_wake_mutex;
  std::condition_variable pump_wake_cv;

  std::atomic<uint32_t> frontbuffer_phys{0}, frontbuffer_w{0}, frontbuffer_h{0};

  // Diagnostics
  std::atomic<uint64_t> ring_kick_count{0};
  std::atomic<uint64_t> pm4_fence_writes{0};
  std::atomic<uint64_t> pm4_swaps{0};
  std::atomic<uint64_t> pm4_indirect_buffers{0};
  std::atomic<uint64_t> pm4_draw_indx{0};
  std::atomic<uint64_t> pm4_bin_draws{0};   // M3.53: DRAW_INDX_(2_)BIN seen (skipped)
  std::atomic<uint64_t> pm4_bin_captured{0};  // M3.53: captured via RESTUFF_CAP_BIN
  std::atomic<uint64_t> pm4_draws_captured{0};
  std::atomic<int> pm4_log_budget{24};

  rex::system::object_ref<rex::system::XHostThread> pump_thread;
  std::atomic<bool> pump_running{false};
  rex::runtime::FunctionDispatcher* dispatcher = nullptr;
  rex::system::KernelState* kernel_state = nullptr;

  // M3.44: async capture (RESTUFF_ASYNC_CAPTURE=1). A dedicated worker runs
  // RunPm4Machine off the guest RENDER thread so the render thread returns
  // from OnRingBufferKick immediately -- CPU/GPU pipelining (guest builds
  // frame N+1 while the worker parses N). Safe because M3.32 true-rptr keeps
  // the guest from overwriting unconsumed ring bytes and M3.33 snapshots IBs.
  rex::system::object_ref<rex::system::XHostThread> capture_thread;
  std::atomic<bool> capture_running{false};
  std::mutex capture_wake_mutex;
  std::condition_variable capture_wake_cv;
  bool capture_pending = false;  // guarded by capture_wake_mutex
};

Backend& B() {
  static Backend backend;
  return backend;
}

// --- M2.0: guest shader registry (IM_LOAD / IM_LOAD_IMMEDIATE) ----------------

std::mutex s_shader_mutex;
std::unordered_map<uint32_t, GuestShaderInfo> s_shaders_by_phys;  // ucode phys -> info
std::unordered_map<uint64_t, GuestShaderInfo> s_shaders_by_hash;
struct ShaderBlob {
  GuestShaderInfo info;
  std::vector<uint32_t> be_words;  // raw big-endian ucode (tiny for UI shaders)
};
std::vector<ShaderBlob> s_shader_blobs;

// Walker-tracked currently-loaded shaders (defined with the PM4 capture below).
extern uint64_t s_current_vs_hash;
extern uint64_t s_current_ps_hash;

// FNV-1a over the raw big-endian ucode words (as they sit in guest memory).
uint64_t HashUcode(const uint32_t* be_words, uint32_t dword_count) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < dword_count; ++i) {
    h = (h ^ be_words[i]) * 1099511628211ull;
  }
  return h;
}

void DumpShaderOnce(const GuestShaderInfo& info, const uint32_t* be_words) {
  // M3.286: OPT-IN. This exists to feed the offline translation tools; it was
  // unconditional and quietly filled a shader_dump/ folder next to the exe on
  // every machine, including the release folders the user test-drove.
  static const bool on = getenv("RESTUFF_SHADER_DUMP") != nullptr;
  if (!on) return;
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories("shader_dump", ec);
  char name[96];
  std::snprintf(name, sizeof(name), "shader_dump/%s_%016llx.ucode.bin",
                info.type == 0 ? "vs" : "ps", static_cast<unsigned long long>(info.hash));
  if (fs::exists(name, ec)) {
    return;
  }
  if (FILE* f = std::fopen(name, "wb")) {
    std::fwrite(be_words, 4, info.size_dwords, f);
    std::fclose(f);
  }
}

// be_words = ucode as it sits in guest memory (big-endian words).
void RegisterGuestShader(uint32_t type, uint32_t phys_addr, const uint32_t* be_words,
                         uint32_t size_dwords) {
  if (!be_words || size_dwords == 0 || size_dwords > 0x10000) {
    return;
  }
  GuestShaderInfo info;
  info.type = type & 1;
  info.phys_addr = phys_addr;
  info.size_dwords = size_dwords;
  info.hash = HashUcode(be_words, size_dwords);

  // Track the walker's currently-loaded shaders (IM_LOAD order = bind order;
  // the walker holds pm4_mutex, so plain writes are safe).
  if (info.type == 0) {
    s_current_vs_hash = info.hash;
  } else {
    s_current_ps_hash = info.hash;
  }

  std::lock_guard<std::mutex> lock(s_shader_mutex);
  const bool fresh = !s_shaders_by_hash.contains(info.hash);
  if (phys_addr) {
    s_shaders_by_phys[phys_addr] = info;
  }
  if (fresh) {
    s_shaders_by_hash[info.hash] = info;
    s_shader_blobs.push_back({info, std::vector<uint32_t>(be_words, be_words + size_dwords)});
    DumpShaderOnce(info, be_words);
    REXLOG_INFO("[native_vk] guest {} shader: hash={:016X} {} dwords phys=0x{:08X} (total {})",
                info.type == 0 ? "VS" : "PS", info.hash, size_dwords, phys_addr,
                s_shaders_by_hash.size());
  }
}

}  // namespace

// M4.0: hand a registered shader's retained ucode to the pre-warm manifest
// writer. Linear scan is fine -- a few hundred blobs, called only on the save
// cadence (once a minute at most).
bool CopyShaderUcode(uint64_t hash, GuestShaderInfo& info_out,
                     std::vector<uint32_t>& words_out) {
  std::lock_guard<std::mutex> lock(s_shader_mutex);
  for (const auto& b : s_shader_blobs) {
    if (b.info.hash == hash) {
      info_out = b.info;
      words_out = b.be_words;
      return true;
    }
  }
  return false;
}

namespace {

// --- M2.1: PM4 draw capture ---------------------------------------------------
// The bulk of the title (and all gameplay) renders through PM4 DRAW_INDX with
// DMA vertex/index buffers — the SAME Scaleform shaders as the UP path, fed
// via vertex fetch constants. The walker's shadow state is draw-synchronized
// (SET_CONSTANT/IM_LOAD execute in-stream before the draw), so everything
// needed is at hand. Per-VS declarations from the disassembled corpus
// (tools/nb_ucode_dump):
struct Pm4VsDecl {
  uint64_t vs_hash;
  uint32_t fetch_slot;     // vertex fetch constant index
  uint32_t stride_words;
  bool pos_k16_16;         // else float3
  int color1_word;         // -1 = none
  int color2_word;
  int uv_word;             // float2 attribute offset, -1 = none
  bool uv_via_consts;      // uv = (dot4(pos,c4), dot4(pos,c5))
  bool pretransformed;     // pos already in clip space (o62 = r_vertex, no matrix)
  restuff::renderer::DrawFamily family;
};
constexpr Pm4VsDecl kPm4Decls[] = {
    // Scaleform quad: pos k16 @0, colors @1,@2, uv from c4/c5.
    {0xEE34A8BA31895FACull, 95, 3, true, 1, 2, -1, true, false,
     restuff::renderer::DrawFamily::kQuad},
    // Flat cxform quad (ps_7897 samples nothing): pos k16 @0, color @1.
    {0xC53CCCAFA9A44EBDull, 95, 2, true, 1, -1, -1, false, false,
     restuff::renderer::DrawFamily::kQuad},
    // Text: pos float3 @0, uv float2 @4, color @6.
    {0x346474C6087D8982ull, 95, 7, false, 6, -1, 4, false, false,
     restuff::renderer::DrawFamily::kText},
    // Pre-transformed float4 pos @0 + colour float3 @4 (o62 = r1 direct), fetch0.
    {0xD586AD25909212F7ull, 0, 7, false, 4, -1, -1, false, true,
     restuff::renderer::DrawFamily::kQuad},
    // Pre-transformed float2 pos @0, no colour (flat white), fetch0, stride 2w.
    {0x8859F7516ED40755ull, 0, 2, false, -1, -1, -1, false, true,
     restuff::renderer::DrawFamily::kQuad},
};

// Walker-tracked currently-loaded shaders (IM_LOAD order = bind order).
uint64_t s_current_vs_hash = 0;  // NOLINT
uint64_t s_current_ps_hash = 0;  // NOLINT
}  // namespace
// Debug accessor for cross-TU logging (which PS was current at capture time).
uint64_t CurrentPsHashForDebug() { return s_current_ps_hash; }
namespace {

float ShadowRegF32(uint32_t reg) { return std::bit_cast<float>(s_reg_shadow[reg & 0x7FFF]); }

// M3.43 walker-cost breakdown: ns spent in the per-draw const copy vs the
// vertex-stream copy+LE-normalize, surfaced per-20k-draws in the pm4 alive
// line as [WPERF]. Splits "consts (every draw)" from "dynamic-mesh recopy".
std::atomic<uint64_t> s_wperf_const_ns{0}, s_wperf_stream_ns{0}, s_wperf_calls{0};
// M3.45 breakdown probe: index-array copy (line ~645) vs whole-function total,
// so the untimed remainder of walker_ms (PM4 parse + alloc churn + submit) is
// derivable = total - const - stream - idx. Temporary diagnostic.
std::atomic<uint64_t> s_wperf_idx_ns{0}, s_wperf_total_ns{0};
// M3.45: RESTUFF_DUMP_DRAWS is a debug flag, OFF in normal runs. It was read
// via a raw getenv() (a linear scan of the whole environ block) on EVERY draw
// and EVERY constant-load packet -- ~5 scans/draw x ~48k draws/s of pure waste
// on the guest render thread (the frame-rate-critical path). Cache it once.
static const bool s_dump_draws = getenv("RESTUFF_DUMP_DRAWS") != nullptr;
// M3.317: union of viewport extents drawn since the last resolve (walker
// thread only); resolves clamp their copy rect to it, then reset it.
static float s_drawn_ext_x1 = 0.f;
static float s_drawn_ext_y1 = 0.f;
// M3.46 stream-cache thrash probe: per-20k-draws classification of stream
// lookups -- hit / new-key (address churn) / stale (content changed under a
// live key). Tells whether the every-other-frame stream_ms spike is the 4096
// nuke, address churn, or genuine per-frame dynamism.
std::atomic<uint64_t> s_str_hit{0}, s_str_new{0}, s_str_stale{0};
// M4.2: rel-fetch payload cache visibility. The M4.1 full-extent hash runs on
// the guest render thread for every rel-fetch draw and had NO counters, so its
// cost could not be sized (unlike the stream cache above). bytes_hashed is the
// number the vectorised-fingerprint decision hangs on.
std::atomic<uint64_t> s_rel_calls{0}, s_rel_bytes_hashed{0}, s_rel_hit{0},
    s_rel_rebuild{0};
// M4.2: write-generation stamp for the ALU constant file. Bumped inside
// WriteShadowRegister for any write to the float bank (0x4000..0x47FF) or the
// bool/loop bank (0x4900..0x4927); deliberately NOT the fetch constants
// (0x4800..0x48FF), which change per draw and would zero the snapshot-sharing
// hit rate. Single-threaded (only the PM4 walker writes registers), so a plain
// counter. CaptureTranslatedDraw shares one constant snapshot across
// consecutive draws whose (vs_base, ps_base, generation) match -- exact,
// write-counted reuse, never content-sampled (the M4.1 lesson).
uint64_t s_alu_const_gen = 0;
// M3.46: cross-frame stream/index cache eviction policy. The game ring-allocates
// fresh VB/IB addresses for dynamic draws (~30 new keys/frame), so the caches
// grow without bound. The old `if (size > 4096) clear()` nuked the WHOLE map --
// including the ~4000 LIVE static entries that hit every frame -- forcing a mass
// rebuild (the periodic stream_ms/total_ms spike that halves fps in heavy
// scenes). Instead, once per frame when the map exceeds the soft cap, evict only
// entries not referenced in the last kCacheEvictAge frames (the dead one-shot
// dynamic keys); the static working set, touched every frame, survives. The
// generous cap keeps eviction off the steady-state path entirely. A hard-cap
// clear() remains as a last-resort backstop for a pathologically large live set.
constexpr size_t kCacheSoftCap = 8192;
constexpr size_t kCacheHardCap = 24576;
constexpr uint64_t kCacheEvictAge = 8;
// M4.2: hash functor so the three per-draw caches (index/stream/rel) can be
// unordered_map instead of std::map (a red-black descent + rebalance per draw
// on the guest render thread). Keys stay the exact tuples/pairs -- hash
// quality affects bucket spread only; equality is still byte-precise, so
// there is NO collision-correctness hazard (unlike folding keys into u64).
struct CacheKeyHash {
  static uint64_t mix(uint64_t h, uint64_t v) {
    return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2)));
  }
  size_t operator()(const std::pair<uint32_t, uint64_t>& k) const {
    return size_t(mix(k.first, k.second));
  }
  template <typename... Ts>
  size_t operator()(const std::tuple<Ts...>& k) const {
    uint64_t h = 1469598103934665603ull;
    std::apply([&](const Ts&... vs) { ((h = mix(h, uint64_t(vs))), ...); }, k);
    return size_t(h);
  }
};

// Evict stale entries from a frame-keyed cache map, at most once per frame.
template <typename Map>
inline void EvictStaleCacheEntries(Map& m, uint64_t cur_frame, uint64_t& last_evict_frame) {
  if (m.size() <= kCacheSoftCap || last_evict_frame == cur_frame) return;
  last_evict_frame = cur_frame;
  for (auto it = m.begin(); it != m.end();) {
    if (it->second.frame_ok + kCacheEvictAge < cur_frame)
      it = m.erase(it);
    else
      ++it;
  }
  if (m.size() > kCacheHardCap) m.clear();  // backstop: live set truly enormous
}

// M3.172 (RESTUFF_EXT_TRUE=1): true screen extents for EVENT_WRITE_EXT.
// PROVEN CAUSAL: with empty extents the guest culls most of the world
// (cam 214704, 41-144 draws, all-water frames, 5/5 runs), so it really does
// consume this writeback -- yet we answer every query with a CONSTANT
// (fullscreen). Accumulate the screen-space bbox of the geometry captured
// since the last extent write and report THAT. Walker thread only.
struct ExtAccum {
  float minx = 1e30f, maxx = -1e30f, miny = 1e30f, maxy = -1e30f;
  bool any = false;
  void add(float x, float y) {
    minx = std::min(minx, x); maxx = std::max(maxx, x);
    miny = std::min(miny, y); maxy = std::max(maxy, y);
    any = true;
  }
  void reset() { *this = ExtAccum(); }
};
static ExtAccum s_ext_accum;
static const bool s_ext_true = getenv("RESTUFF_EXT_TRUE") != nullptr;
// M3.174: extent-query BRACKET state. The guest issues [BEGIN=EVENT_WRITE
// init-25][one proxy draw][END=EVENT_WRITE_EXT init-26] (EXTSEQ: draws_now
// +1 between consecutive ENDs). Walker thread executes packets and capture
// synchronously in stream order, so plain statics suffice.
static bool s_extq_open = false;
static ExtAccum s_extq_box;   // the CURRENT bracket's proxy bbox
static const bool s_extq_probe = getenv("RESTUFF_EXTQPROBE") != nullptr;

// Drop-reason diagnostics for CapturePm4Draw.
std::atomic<uint64_t> s_drop_prim{0}, s_drop_nodecl{0}, s_drop_mask{0}, s_drop_novb{0},
    s_drop_needwords{0}, s_drop_empty{0}, s_cap_ok{0}, s_call_count{0};
std::atomic<uint64_t> s_prim_hist[16] = {};
void MaybeLogDrops() {
  const uint64_t n = s_call_count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n % 10000 == 0) {
    std::string hist;
    for (int i = 0; i < 16; ++i) {
      const uint64_t c = s_prim_hist[i].load();
      if (c) hist += "p" + std::to_string(i) + "=" + std::to_string(c) + " ";
    }
    REXLOG_INFO(
        "[native_vk] DROPS ok={} prim={} nodecl={} mask={} novb={} needwords={} empty={} | prims: {}",
        s_cap_ok.load(), s_drop_prim.load(), s_drop_nodecl.load(), s_drop_mask.load(),
        s_drop_novb.load(), s_drop_needwords.load(), s_drop_empty.load(), hist);
  }
}

// Captures one PM4 DRAW_INDX into the shared frame queue. idx_base_phys = 0
// for auto-indexed draws.
// Xenos vertex format -> (component count, per-component byte size). Used to
// byteswap guest big-endian vertex data to little-endian per component so a
// VkVertexInputAttributeDescription reads it directly. (xenos::VertexFormat.)
struct FmtInfo {
  uint32_t comps, bytes;
};
FmtInfo VtxFmt(uint32_t f) {
  switch (f) {
    // k_8_8_8_8 is a PACKED 32-bit format: the guest's 8-in-32 endian swap
    // reverses the whole dword (guest bytes [A][R][G][B] -> host [B][G][R][A]),
    // so the shader's .w reads the true alpha. Treating it as 4x1-byte
    // components skipped the swap and fed alpha from the blue channel.
    case 6: return {1, 4};
    case 7: case 16: case 17: return {1, 4};      // 2_10_10_10 / 10_11_11 / 11_11_10 packed
    case 25: return {2, 2};                        // k_16_16
    case 26: return {4, 2};                        // k_16_16_16_16
    case 31: return {2, 2};                        // k_16_16_FLOAT
    case 32: return {4, 2};                        // k_16_16_16_16_FLOAT
    case 33: case 36: return {1, 4};               // k_32 / k_32_FLOAT
    case 34: case 37: return {2, 4};               // k_32_32 / k_32_32_FLOAT
    case 57: return {3, 4};                        // k_32_32_32_FLOAT
    case 35: case 38: return {4, 4};               // k_32_32_32_32 / _FLOAT
    default: return {4, 4};
  }
}

const ShaderBlob* FindBlobByHash(uint64_t hash) {
  // Registration and capture both run on the PM4 walker thread, so the vector
  // is not mutated concurrently with this lookup.
  std::lock_guard<std::mutex> lock(s_shader_mutex);
  for (const auto& b : s_shader_blobs)
    if (b.info.hash == hash) return &b;
  return nullptr;
}

// M2.3 translated-shader capture: package the draw in raw form (guest VB +
// constants + textures + shader hashes) for on-GPU transform by the translated
// shaders. Runs the real guest shaders through the translate+compile cache.
// Capture-level drop census (RESTUFF_DUMP_DRAWS): every draw that reaches
// CaptureTranslatedDraw but bails is counted by reason; unknown-shader and
// stale-fetch bails also log the hashes so dropped scene elements (e.g. the
// title backdrop's right tiles) can be identified.
static std::atomic<uint32_t> s_cap_in{0}, s_cap_out{0};
// M3.137: PT_POINTLIST accounting -- issued by the guest vs surviving capture.
static std::atomic<uint32_t> s_cap_points_in{0}, s_cap_points_out{0};
static std::atomic<uint32_t> s_cap_skip[8] = {};
static const char* kCapSkipName[8] = {"idxcount", "noblob",   "badshader", "stride0",
                                      "novb",     "stalefetch", "cmask0",   "other"};
static void CapDropLog(int reason, uint64_t vs_hash, uint64_t ps_hash, uint32_t prim,
                       uint32_t index_count) {
  s_cap_skip[reason].fetch_add(1, std::memory_order_relaxed);
  static const bool log_drops =
      s_dump_draws || getenv("RESTUFF_CAP_CENSUS") != nullptr;
  if (!log_drops) return;
  // Distinct (reason, vs) pairs always log once, regardless of budget.
  {
    static std::mutex m;
    static std::set<uint64_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (seen.insert(vs_hash ^ (uint64_t(reason) << 60)).second) {
      REXLOG_INFO("[DUMP] CAPDROP-DISTINCT {}: vs={:016X} ps={:016X} prim={} n={}",
                  kCapSkipName[reason], vs_hash, ps_hash, prim, index_count);
    }
  }
  // 4F30...55DF is the known-degenerate no-op VS (writes only position.w,
  // point-list sync draws) -- excluded so it can't exhaust the log budget.
  if (vs_hash == 0x4F3046249BD855DFull) return;
  static std::atomic<int> s_budget{80};
  if (s_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    REXLOG_INFO("[DUMP] CAPDROP {}: vs={:016X} ps={:016X} prim={} n={}", kCapSkipName[reason],
                vs_hash, ps_hash, prim, index_count);
  }
}
// Stale-payload detector (RESTUFF_DUMP_DRAWS): the game fills UP/dynamic
// vertex+index buffers AFTER queueing the draw packet (Begin returns a write
// pointer; commit is an inlined store). If the walker is caught up, capture
// reads race those CPU writes -- real hardware only fetches post-kick. Hash
// each draw's guest VB/IB region at capture time, re-hash when the walker
// reaches the frame's swap packet (guest writes long complete by then), and
// report mismatches. Walker-thread only: no locking.
struct PendingHashCheck {
  uint32_t vb_phys, vb_len, idx_phys, idx_len;
  uint64_t vb_hash, idx_hash, vs_hash;
};
static std::vector<PendingHashCheck> s_hash_checks;

// Hills-lag probe: the most recent PM4 write to 0x4801 (texture fetch slot 0
// address word) plus the guest phys of the payload word it came from. The
// capture compares the value it walked against a live re-read of that word —
// if they differ, the game patched the ring AFTER our walk consumed it.
struct Tex0WriteRec {
  uint64_t serial;
  uint32_t value;
  uint32_t payload_phys;
};
static Tex0WriteRec s_last_tex0w{};
static uint64_t s_draw_serial;
// Guest frame epoch (bumped at PM4_XE_SWAP); keys the per-frame shared vertex
// stream cache in CaptureTranslatedDraw. Walker-thread writes; atomic for the
// occasional debug read elsewhere.
static std::atomic<uint64_t> s_frame_serial{1};
// Resolve (EDRAM kCopy) telemetry: seen/submitted/dropped counts + last dest,
// walker-thread writes, read by the pump thread's alive line.
static std::atomic<uint64_t> s_res_seen{0}, s_res_sub{0}, s_res_drop{0};
static std::atomic<uint32_t> s_res_last_dst{0};
// M3.9x WORLD-DIM: aggregate counters for the RESTUFF_CWATCH_LO/HI shadow-reg
// window. A per-write log is useless here (its budget is exhausted during boot,
// long before gameplay), so also track a total write count + the last value per
// dword and print them AT THE SHAFT DRAW -- that answers "is c8 stale during
// gameplay, or is the guest actively writing it?" regardless of log volume.
static std::atomic<uint64_t> s_cw_count{0};
static std::atomic<uint32_t> s_cw_last[8];
static std::atomic<uint64_t> s_cw_count_at_last_shaft{0};
static uint64_t Fnv1a(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
  return h;
}
void DumpPm4Seq(const char* why);  // defined with the PM4 walker below

void CheckStalePayloads() {
  if (s_hash_checks.empty()) return;
  auto* memory = rex::Runtime::instance()->memory();
  if (!memory) { s_hash_checks.clear(); return; }
  uint32_t stale_vb = 0, stale_idx = 0;
  uint64_t first_stale_vs = 0;
  for (const auto& c : s_hash_checks) {
    const uint8_t* vb = memory->TranslatePhysical<const uint8_t*>(c.vb_phys);
    if (vb && Fnv1a(vb, c.vb_len) != c.vb_hash) {
      ++stale_vb;
      if (!first_stale_vs) first_stale_vs = c.vs_hash;
    }
    if (c.idx_phys) {
      const uint8_t* ib = memory->TranslatePhysical<const uint8_t*>(c.idx_phys);
      if (ib && Fnv1a(ib, c.idx_len) != c.idx_hash) ++stale_idx;
    }
  }
  static std::atomic<int> s_budget{40};
  if ((stale_vb || stale_idx) && s_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    REXLOG_INFO("[DUMP] VBSTALE: {}/{} vertex buffers, {} index buffers changed "
                "between capture and swap (first stale vs={:016X})",
                stale_vb, s_hash_checks.size(), stale_idx, first_stale_vs);
  }
  s_hash_checks.clear();
}

void MaybeCapCensus() {
  // RESTUFF_CAP_CENSUS=1: counters only (safe in gameplay -- the DUMP_DRAWS
  // bus-error lives in the heavy vertex-dump paths, not here).
  static const bool census =
      s_dump_draws || getenv("RESTUFF_CAP_CENSUS") != nullptr;
  if (!census) return;
  static std::atomic<int> s_cens{0};
  if ((s_cens.fetch_add(1, std::memory_order_relaxed) % 2000) != 1999) return;
  REXLOG_INFO(
      "[DUMP] CAPCENSUS in={} out={} idxcount={} noblob={} badshader={} stride0={} novb={} "
      "stalefetch={} cmask0={} nostreams={} | POINTS in={} out={}",
      s_cap_in.load(), s_cap_out.load(), s_cap_skip[0].load(), s_cap_skip[1].load(),
      s_cap_skip[2].load(), s_cap_skip[3].load(), s_cap_skip[4].load(), s_cap_skip[5].load(),
      s_cap_skip[6].load(), s_cap_skip[7].load(), s_cap_points_in.load(),
      s_cap_points_out.load());
}

void CaptureTranslatedDraw(uint32_t initiator, uint32_t idx_base_phys) {
  namespace rr = restuff::renderer;
  // M3.45: RAII whole-function timer (survives every early return/drop).
  // M4.2: the total/idx/const timers are 1-in-16 SAMPLED -- they were six
  // unconditional QPC reads per draw (~150ns) on the guest render thread, ~7ms
  // per second at 48k draws/s. Report time scales the three by 16; over a
  // 20k-draw window that estimate is solid. The stream timer stays exact (it
  // only runs on the rare cache-miss path).
  static std::atomic<uint64_t> s_wperf_tick{0};
  const bool wperf_sample = (s_wperf_tick.fetch_add(1, std::memory_order_relaxed) & 15) == 0;
  struct TotalTimer {
    bool on;
    std::chrono::steady_clock::time_point t0;
    explicit TotalTimer(bool o) : on(o) {
      if (on) t0 = std::chrono::steady_clock::now();
    }
    ~TotalTimer() {
      if (on)
        s_wperf_total_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count(),
                                   std::memory_order_relaxed);
    }
  } _total_timer{wperf_sample};
  const uint32_t prim = initiator & 0x3F;
  // RESTUFF_FLOORCOUNT: count 10-index draws AT THE TOP, before any drop path.
  // The reference issues THREE colour-writing floor draws per frame; native
  // emits ONE. This distinguishes "the PM4 walker never sees them" from
  // "CaptureTranslatedDraw drops them later".
  // M4.2: cached -- raw getenv() here was an environ scan per captured draw on
  // the guest render thread (the exact M3.45 bug class documented above).
  static const bool s_floorcount_top = getenv("RESTUFF_FLOORCOUNT") != nullptr;
  if (s_floorcount_top) {
    const uint32_t ic_probe = (initiator >> 16) & 0xFFFF;
    if (ic_probe == 10) {
      // Log each DISTINCT index-buffer address for 10-index draws, at the very
      // top of the capture (before any drop path). The reference issues THREE
      // floor layers with ib = 155631E4 / 15563360 / 15619000; guest
      // allocations are near-deterministic across builds, so if native only
      // ever sees ONE of those addresses the other two layers are never
      // submitted by the PM4 walker; if it sees three, they are being dropped
      // later and CapDropLog will say why.
      // Count ALL 10-index draws per frame, split by colour mask. Counting a
      // single PS hash is apples-to-oranges: the reference's three floor
      // layers use THREE DIFFERENT shader combos, so native's equivalents
      // would land under three different PS hashes too.
      static std::atomic<uint32_t> n_c{0}, n_z{0};
      static std::atomic<uint64_t> lastf{0};
      const uint64_t f = s_frame_serial.load(std::memory_order_relaxed);
      if (f != lastf.exchange(f)) {
        const uint32_t c = n_c.exchange(0), z = n_z.exchange(0);
        if (c || z) {
          static std::atomic<int> b{10};
          if (b.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[FLOORTOP] frame {}: ALL 10-index draws colour={} depth-only={}", f, c, z);
        }
      }
      if ((s_reg_shadow[0x2104] & 0xF) == 0) n_z.fetch_add(1); else n_c.fetch_add(1);
    }
  }

  const uint32_t index_count = initiator >> 16;
  s_cap_in.fetch_add(1, std::memory_order_relaxed);
  // M3.137: the missing-particle question, asked directly. An emulated hut
  // frame carries 96 PT_POINTLIST draws and ours carried none, but that was a
  // comparison between captures with very different totals (5158 vs 1098), so
  // it does not establish that POINT draws are being lost rather than never
  // issued. Count them at capture entry (before any bail) against what leaves,
  // so "the guest never draws them" and "we drop them" become distinguishable.
  if (prim == 1) s_cap_points_in.fetch_add(1, std::memory_order_relaxed);
  MaybeCapCensus();
  // Targeted probe: the hills-strip draws (fetch 0x0612C000) exist in every
  // emulated-trace title frame but never in our capture -- log every sighting
  // of that fetch at capture entry to find which bail eats them.
  if (s_dump_draws && ((s_reg_shadow[0x4801] >> 12) << 12) == 0x0612C000) {
    static std::atomic<int> s_hills{30};
    if (s_hills.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[DUMP] HILLS-DRAW seen: prim/init mode={} vs={:016X} ps={:016X}",
                  s_reg_shadow[0x2208] & 7, s_current_vs_hash, s_current_ps_hash);
  }
  // M2.4: EDRAM-copy ("resolve") draws -- RB_MODECONTROL edram_mode == kCopy
  // (6). The rect marks the region of the current scene surface to copy into
  // the RB_COPY_DEST_BASE guest texture (double-buffered front buffers at the
  // title). Submit a resolve record instead of rasterizing anything.
  // Debug register context stamped on every captured draw (incl. resolves).
  const auto stamp_dbg_regs = [](rr::RawGuestDraw& d) {
    for (int k = 0; k < 4; ++k) d.dbg_vport[k] = s_reg_shadow[0x210F + k];
    d.dbg_surf = s_reg_shadow[0x2000];
    d.dbg_winoff = s_reg_shadow[0x2080];
    d.dbg_sciss_tl = s_reg_shadow[0x2081];
    d.dbg_sciss_br = s_reg_shadow[0x2082];
    d.dbg_copyctl = s_reg_shadow[0x2318];
    d.dbg_color_clear = s_reg_shadow[0x231E];
    d.dbg_depth_clear = s_reg_shadow[0x231D];
    d.dbg_color_info = s_reg_shadow[0x2001];
    d.dbg_depth_info = s_reg_shadow[0x2002];
    d.cap_frame = s_frame_serial.load(std::memory_order_relaxed);  // M4.10
  };
  if ((s_reg_shadow[0x2208] & 7) == 6) {
    rr::RawGuestDraw d;
    d.is_resolve = true;
    stamp_dbg_regs(d);
    d.copy_dest = s_reg_shadow[0x2319];
    d.copy_w = s_reg_shadow[0x231A] & 0x3FFF;
    d.copy_h = (s_reg_shadow[0x231A] >> 16) & 0x3FFF;
    s_res_seen.fetch_add(1, std::memory_order_relaxed);
    s_res_last_dst.store(d.copy_dest, std::memory_order_relaxed);
    // On-change: first sighting of each distinct copy dest, with full register
    // context (walker thread only). Scene-transition scale -- a handful ever.
    {
      static uint32_t seen_dests[24];
      static int seen_n = 0;
      bool is_new = true;
      for (int k = 0; k < seen_n; ++k)
        if (seen_dests[k] == d.copy_dest) { is_new = false; break; }
      if (is_new && seen_n < 24) {
        seen_dests[seen_n++] = d.copy_dest;
        // RB_COPY_DEST_INFO (0x231B) has never been read by this backend. Its
        // copy_dest_exp_bias scales the resolved colour by 2^bias -- the stock
        // 360 trick for rendering a scene at reduced range into EDRAM and
        // restoring it on resolve. A bias we ignore lands as a flat brightness
        // deficit exactly like the x1.95 world dim.
        const uint32_t ci = s_reg_shadow[0x231B];
        const int exp_bias = int(ci << 10) >> 26;  // bits 16-21, sign-extended
        REXLOG_INFO(
            "[native_vk] RESOLVE-NEW dst=0x{:08X} {}x{} ctl=0x{:08X} surf=0x{:08X} "
            "vp={}/{}/{}/{} ic={} info=0x{:08X} fmt={} num={} endian={} EXP_BIAS={} "
            "(scale={})",
            d.copy_dest, d.copy_w, d.copy_h, d.dbg_copyctl, d.dbg_surf, d.dbg_vport[0],
            d.dbg_vport[1], d.dbg_vport[2], d.dbg_vport[3], index_count, ci, (ci >> 7) & 0x3F,
            (ci >> 13) & 7, ci & 7, exp_bias, std::exp2(float(exp_bias)));
      }
    }
    // exp_bias can also vary between resolves that share a destination, so
    // log each DISTINCT info word once. (M4.29: this was on-CHANGE, but the
    // frame's resolve chain legitimately alternates between ~3 info words
    // every frame -- scene/depth/bloom surfaces -- so "change" fired ~30x per
    // frame and was 99.5% of the debug log: 5MB rotations every few seconds.)
    {
      // Walker thread only (same as seen_dests above) -- plain statics.
      static uint32_t s_seen_info[16];
      static int s_seen_n = 0;
      const uint32_t ci = s_reg_shadow[0x231B];
      bool is_new = true;
      for (int k = 0; k < s_seen_n; ++k)
        if (s_seen_info[k] == ci) { is_new = false; break; }
      if (is_new && s_seen_n < 16) {
        s_seen_info[s_seen_n++] = ci;
        const int exp_bias = int(ci << 10) >> 26;
        REXLOG_INFO("[native_vk] RESOLVE-INFO-CHG info=0x{:08X} EXP_BIAS={} scale={} fmt={} "
                    "dst=0x{:08X} surf=0x{:08X}",
                    ci, exp_bias, std::exp2(float(exp_bias)), (ci >> 7) & 0x3F, d.copy_dest,
                    d.dbg_surf);
      }
    }
    // The resolve rect = the draw's vertex extent (window coords): only the
    // covered region is copied on hardware. Read the (usually 3) verts via the
    // resolve VS's vertex fetch, same as a normal draw.
    if (const ShaderBlob* rvsb = FindBlobByHash(s_current_vs_hash)) {
      const auto& rvs = spc::GetShader(rvsb->info.hash, true, rvsb->be_words.data(),
                                       rvsb->info.size_dwords);
      if (rvs.valid && !rvs.t.attrs.empty() && rvs.t.attrs[0].stride_bytes) {
        const uint32_t slot = rvs.t.attrs[0].fetch_slot;
        const uint32_t rstride = rvs.t.attrs[0].stride_bytes;
        const uint32_t rf0 = s_reg_shadow[0x4800 + slot * 2];
        const uint32_t rphys = rf0 & ~0x3u;
        if (rphys && index_count >= 3 && index_count <= 4) {
          auto* mem = rex::Runtime::instance()->memory();
          const uint8_t* rvb = mem->TranslatePhysical<const uint8_t*>(rphys);
          float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
          for (uint32_t v = 0; v < index_count; ++v) {
            uint32_t xw, yw;
            std::memcpy(&xw, rvb + v * rstride + rvs.t.attrs[0].byte_offset, 4);
            std::memcpy(&yw, rvb + v * rstride + rvs.t.attrs[0].byte_offset + 4, 4);
            xw = std::byteswap(xw);
            yw = std::byteswap(yw);
            const float x = std::bit_cast<float>(xw), y = std::bit_cast<float>(yw);
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            y0 = std::min(y0, y); y1 = std::max(y1, y);
          }
          if (x1 > x0 && y1 > y0 && x1 - x0 <= 4096.f && y1 - y0 <= 4096.f) {
            d.copy_rx = uint32_t(std::max(0.f, x0 + 0.5f));
            d.copy_ry = uint32_t(std::max(0.f, y0 + 0.5f));
            d.copy_rw = uint32_t(x1 - x0 + 0.5f);
            d.copy_rh = uint32_t(y1 - y0 + 0.5f);
          }
          if (s_dump_draws) {
            static std::atomic<int> s_rect_budget{40};
            if (s_rect_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
              REXLOG_INFO("[DUMP] RESOLVERECT dst=0x{:08X} rect=({},{} {}x{}) dest={}x{}",
                          d.copy_dest, d.copy_rx, d.copy_ry, d.copy_rw, d.copy_rh, d.copy_w,
                          d.copy_h);
          }
          // RECTPROBE (band hunt): raw resolve verts + scissor/viewport regs,
          // once per distinct rect shape, to decide guest-asked-328x182 vs
          // our-decode-inflated for the bloom-chain resolves.
          static const bool s_rectprobe = getenv("RESTUFF_RECTPROBE") != nullptr;  // M4.2: cached
          if (s_rectprobe) {
            static std::mutex s_rpmu;
            static std::set<uint64_t> s_rpseen;
            const uint64_t shape =
                (uint64_t(d.copy_dest) << 24) ^ (uint64_t(d.copy_rw) << 12) ^ d.copy_rh;
            bool fresh;
            {
              std::lock_guard<std::mutex> lk(s_rpmu);
              fresh = s_rpseen.insert(shape).second;
            }
            if (fresh)
              REXLOG_INFO("[RECTPROBE] dst=0x{:08X} destWH={}x{} verts x[{:.2f},{:.2f}] "
                          "y[{:.2f},{:.2f}] -> rect=({},{} {}x{}) sciss_tl=0x{:08X} "
                          "sciss_br=0x{:08X} surf=0x{:08X} nvert={}",
                          d.copy_dest, d.copy_w, d.copy_h, x0, x1, y0, y1, d.copy_rx,
                          d.copy_ry, d.copy_rw, d.copy_rh, d.dbg_sciss_tl, d.dbg_sciss_br,
                          d.dbg_surf, index_count);
          }
        }
      }
    }
    // M3.317 (right-edge band): the bloom-chain resolves ask for MORE than the
    // pass rendered (guest verts 328x184 around a 320x180 viewport -- padding
    // to the dest's 352 pitch). On hardware the extra columns are EDRAM junk
    // the game tolerates (dark in practice); in our model the small pass
    // renders into the scene image's corner, so the padding picks up LIVE
    // WORLD pixels (sky-bright, blue). Those ride the bright-pass into the
    // 160x90 bloom texture as a fake highlight at x~147-157 and the combine
    // smears them over screen x~1120-1240 every frame: the camera-fixed band.
    // Fix: clamp the copy rect to the union of viewport extents drawn since
    // the previous resolve; the dest's padding keeps its previous (dark)
    // content, matching hardware's steady state. Empty extent (depth/clear
    // resolve trains with no draws between) => no clamp. Kill switch:
    // RESTUFF_NO_RESCLAMP=1.
    {
      static const bool s_no_resclamp = getenv("RESTUFF_NO_RESCLAMP") != nullptr;
      if (!s_no_resclamp && s_drawn_ext_x1 >= 16.f && s_drawn_ext_y1 >= 16.f &&
          d.copy_rw && d.copy_rh) {
        const uint32_t ex = uint32_t(s_drawn_ext_x1 + 0.5f);
        const uint32_t ey = uint32_t(s_drawn_ext_y1 + 0.5f);
        if (d.copy_rx < ex && d.copy_ry < ey &&
            (d.copy_rx + d.copy_rw > ex || d.copy_ry + d.copy_rh > ey)) {
          const uint32_t nw = std::min(d.copy_rw, ex - d.copy_rx);
          const uint32_t nh = std::min(d.copy_rh, ey - d.copy_ry);
          static std::mutex s_rcmu;
          static std::set<uint64_t> s_rcseen;
          bool fresh;
          {
            std::lock_guard<std::mutex> lk(s_rcmu);
            fresh = s_rcseen.insert((uint64_t(d.copy_dest) << 28) ^
                                    (uint64_t(nw) << 14) ^ nh).second;
          }
          if (fresh)
            REXLOG_INFO("[RESCLAMP] dst=0x{:08X} rect {}x{} -> {}x{} (drawn ext {}x{})",
                        d.copy_dest, d.copy_rw, d.copy_rh, nw, nh, ex, ey);
          d.copy_rw = nw;
          d.copy_rh = nh;
        }
      }
      s_drawn_ext_x1 = 0.f;
      s_drawn_ext_y1 = 0.f;
    }
    if (d.copy_dest && d.copy_w && d.copy_h) {
      s_res_sub.fetch_add(1, std::memory_order_relaxed);
      rr::SubmitRawDraw(std::move(d));
    } else {
      // Dropped kCopy event -- clear-only resolves land here (no copy dest).
      // M4.10: unconditional now (was RESTUFF_DUMP_DRAWS-only). The guest's
      // colour fast-clear value (231E) is captured and then thrown away with
      // the record -- if the flicker is a clear we fail to replay, this line
      // is the only witness. Sampled 1-in-64 after the first 40 so a
      // clear-heavy scene can't spam the log.
      const uint64_t nth = s_res_drop.fetch_add(1, std::memory_order_relaxed);
      if (nth < 40 || (nth & 63) == 0)
        REXLOG_INFO("[RESOLVE-DROP] n={} ctl=0x{:08X} dest=0x{:08X} {}x{} clearC=0x{:08X} "
                    "clearD=0x{:08X} frame={}",
                    nth + 1, d.dbg_copyctl, d.copy_dest, d.copy_w, d.copy_h, d.dbg_color_clear,
                    d.dbg_depth_clear, d.cap_frame);
    }
    return;
  }
  // M3.317: accumulate the viewport extent this draw can reach (walker thread
  // only). Reset by every resolve above; a resolve then clamps its copy rect
  // to what was actually drawn since the previous one. Insane scales (VTE off
  // / stale regs) only ever GROW the union, so they can't cause a bad clamp
  // beyond skipping it.
  {
    const float xs = std::abs(ShadowRegF32(0x210F)), xo = ShadowRegF32(0x2110);
    const float ys = std::abs(ShadowRegF32(0x2111)), yo = ShadowRegF32(0x2112);
    if (xs >= 1.f && xs <= 4096.f && ys >= 1.f && ys <= 4096.f) {
      s_drawn_ext_x1 = std::max(s_drawn_ext_x1, xo + xs);
      s_drawn_ext_y1 = std::max(s_drawn_ext_y1, yo + ys);
    }
  }
  if (index_count == 0 || index_count > 0x10000) {
    CapDropLog(0, s_current_vs_hash, s_current_ps_hash, prim, index_count);
    return;
  }

  const ShaderBlob* vsb = FindBlobByHash(s_current_vs_hash);
  const ShaderBlob* psb = FindBlobByHash(s_current_ps_hash);
  if (!vsb || !psb) {
    CapDropLog(1, s_current_vs_hash, s_current_ps_hash, prim, index_count);
    return;
  }
  const auto& vs =
      spc::GetShader(vsb->info.hash, true, vsb->be_words.data(), vsb->info.size_dwords);
  const auto& ps =
      spc::GetShader(psb->info.hash, false, psb->be_words.data(), psb->info.size_dwords);
  // Skip shaders we can't translate/compile, and degenerate no-fetch VSes on
  // LARGE draws. M3.123: attrs-empty is NOT degenerate for tiny draws -- the
  // title's visibility PROBE POINTS (vs=4F30, prim=1, n=1, position from
  // constants, shaft-mask PS) have no vertex fetch by design, and dropping
  // them starved the flare/particle visibility feedback (gift sparkles,
  // campfire): the emulated reference renders dozens of 1-point draws per
  // frame that we binned here (~11/frame of noattrs drops in user-session
  // logs). Let attrs-empty draws through when they are small.
  // M3.138: the index_count > 8 relaxation was a size heuristic standing in for
  // the real question, and it cost this title every particle. A census run
  // showed EVERY badshader drop is this arm, all prim=13 (kQuadList), all with
  // vs.valid=true ps.valid=true attrs=0 and 12-196 indices -- procedural
  // billboard shaders that build each quad corner from constants + the vertex
  // index, working exactly as designed. Ask the shader whether it fetches at
  // all instead of guessing from the draw's size: drop only when it DOES fetch
  // and we resolved nothing (a real detection failure).
  // RESTUFF_STRICT_ATTRS=1 restores the old size-only gate for A/B.
  // A shader can also legitimately have no ATTRIBUTES while still fetching:
  // register-relative fetches (index from a GPR) are served as storage buffers,
  // never as vertex attributes. 76AF1AC3/CA2CFAB9 -- this title's particle
  // shaders -- are exactly that, reading per-particle data by index. So the
  // drop condition is "fetches, but neither attributes NOR a rel-fetch slot".
  static const bool strict_attrs = getenv("RESTUFF_STRICT_ATTRS") != nullptr;
  const bool sources_vertices_elsewhere =
      vs.t.vfetch_count == 0 || vs.t.rel_fetch_slot != ~0u || vs.t.rel_fetch_slot2 != ~0u;
  const bool attrs_unresolved = vs.t.attrs.empty() &&
                                (strict_attrs || !sources_vertices_elsewhere) && index_count > 8;
  if (!vs.valid || !ps.valid || attrs_unresolved) {
    // M3.9x: 60% of ALL draws were being dropped here (CAPCENSUS badshader
    // 428k/710k) yet every dumped shader translates+compiles clean, so split
    // the three sub-reasons and name the offenders. Prime suspect is the
    // attrs.empty() arm -- a VS whose vertex fetches we failed to detect is
    // silently discarded, which would take whole passes (e.g. shadow maps)
    // with it and is a strong candidate for the missing shadows.
    {
      static std::atomic<uint32_t> s_nvs{0}, s_nps{0}, s_nattr{0};
      const uint32_t a = (!vs.valid) ? s_nvs.fetch_add(1, std::memory_order_relaxed) : 0;
      const uint32_t b = (vs.valid && !ps.valid) ? s_nps.fetch_add(1, std::memory_order_relaxed) : 0;
      const uint32_t c =
          (vs.valid && ps.valid) ? s_nattr.fetch_add(1, std::memory_order_relaxed) : 0;
      static std::atomic<int> s_bud{40};
      if (((a | b | c) < 3 || (a + b + c) % 20000 == 0) &&
          s_bud.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[BADSHADER] vs={:016X} ps={:016X} vs.valid={} ps.valid={} attrs={} "
                    "prim={} n={} | totals: !vs={} !ps={} noattrs={}",
                    vsb->info.hash, psb->info.hash, vs.valid, ps.valid,
                    uint32_t(vs.t.attrs.size()), prim, index_count,
                    s_nvs.load(std::memory_order_relaxed), s_nps.load(std::memory_order_relaxed),
                    s_nattr.load(std::memory_order_relaxed));
    }
    CapDropLog(2, vsb->info.hash, psb->info.hash, prim, index_count);
    return;
  }

  auto* memory = rex::Runtime::instance()->memory();

  rr::RawGuestDraw d;
  stamp_dbg_regs(d);
  d.vs_hash = vsb->info.hash;
  d.ps_hash = psb->info.hash;
  d.prim = prim;
  // VGT_DRAW_INITIATOR bit 11 = index format (0 = u16, 1 = u32). The SDK notes
  // 32-bit is used "for some world draws" -- exactly the big environment
  // strips that overflowed vcount when their index words were read as u16
  // pairs (the gameplay stalefetch=42% class).
  const bool idx32 = (initiator >> 11) & 1;
  // M3.38b: index streams repeat identically frame over frame (static world
  // strips) -- widening+scanning them per draw on the render thread was a
  // large slice of walker_ms. Cache the widened array AND its max_index
  // cross-frame, revalidated per frame by a sampled probe of the source
  // bytes (same policy as the vertex-stream cache; RESTUFF_FULL_VB_HASH=1
  // upgrades the probe to full-extent).
  struct IdxCacheEntry {
    std::shared_ptr<const std::vector<uint32_t>> data;
    uint32_t max_index = 0;
    uint64_t probe = 0;
    uint64_t frame_ok = 0;
  };
  static std::unordered_map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>,
                            IdxCacheEntry, CacheKeyHash>  // M4.2: was std::map
      s_idx_cache;
  // M3.126: primitive reset is the GUEST's call, not an implicit "0xFFFF always
  // cuts". Reference (SDK primitive_processor.cpp:585-603): reset applies only
  // when PA_SU_SC_MODE_CNTL.multi_prim_ib_ena (bit 21) is set and the topology
  // is a strip/fan, and the cut value is VGT_MULTI_PRIM_IB_RESET_INDX (0x2103);
  // with 16-bit indices it is only usable when that value fits in 16 bits.
  // We previously cut every strip at 0xFFFF unconditionally, so a mesh that
  // legitimately references vertex 65535 had its strip severed -- and since
  // Vulkan RESETS strip winding parity after a restart, every triangle past the
  // false cut rasterized with inverted winding and was backface-culled. That is
  // exactly the "random culled triangles" / see-through terrain patch class.
  const bool prim_is_strip_fan = (prim == 5 || prim == 6 || prim == 0xD || prim == 3);
  const uint32_t guest_reset_indx = s_reg_shadow[0x2103];
  const bool guest_reset_ena =
      ((s_reg_shadow[0x2205] >> 21) & 1) && prim_is_strip_fan &&
      (idx32 || guest_reset_indx <= 0xFFFFu);
  // STATUS: OPT-IN (RESTUFF_GUEST_PRIMRESET=1) -- now VERIFIED WORKING, and
  // REFUTED as the cause of the "random culled triangles".
  //  - The black-world regression is fixed: the flag is plumbed through
  //    PipeReq/fd so the async pipeline pre-build keys match the rec-loop
  //    lookup. Hut and gate drives both render correctly with it on.
  //  - It changes NOTHING for the world's strips. Measured on real draws
  //    (RESTUFF_PRIMRESET_LOG=1): prim=6 su=0x00218002 ena_bit=1
  //    reset_indx=0x0000FFFF -- the guest DOES enable multi_prim_ib_ena and
  //    DOES use 0xFFFF, exactly what the legacy path hardcodes. No false cut,
  //    so no spurious strip-parity reset and no winding inversion from here.
  //  - Still more faithful for NON-strip draws (a legitimate 0xFFFF index in a
  //    triangle list was previously rewritten into the restart sentinel = a
  //    bogus huge index). Kept opt-in: no visible change on this title.
  static const bool s_guest_reset = getenv("RESTUFF_GUEST_PRIMRESET") != nullptr;
  const bool reset_ena = s_guest_reset ? guest_reset_ena : prim_is_strip_fan;
  const uint32_t reset_indx = s_guest_reset ? guest_reset_indx : (idx32 ? 0xFFFFFFFFu : 0xFFFFu);
  d.prim_reset_enabled = reset_ena;
  static const bool s_primreset_log = getenv("RESTUFF_PRIMRESET_LOG") != nullptr;  // M4.2: cached
  if (s_primreset_log && prim_is_strip_fan && index_count > 64) {
    static std::atomic<int> s_pr{24};
    if (s_pr.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[PRIMRESET] prim={} su=0x{:08X} ena_bit={} reset_indx=0x{:08X} idx32={} -> ena={}",
                  prim, s_reg_shadow[0x2205], (s_reg_shadow[0x2205] >> 21) & 1, guest_reset_indx,
                  int(idx32), int(reset_ena));
  }
  const uint64_t idx_frame = s_frame_serial.load(std::memory_order_relaxed);
  static uint64_t s_idx_evict_frame = 0;
  // M3.63: VGT_INDX_OFFSET (0x2102) -- base vertex offset the hardware adds to
  // every fetched index value (and to the auto index for non-indexed draws).
  // Both SDK reference command processors apply it on EVERY draw
  // (system_constants_.vertex_base_index); we ignored it entirely, so any draw
  // the game issues with a nonzero offset fetched the WRONG vertex window.
  const uint32_t indx_off = s_reg_shadow[0x2102];
  if (indx_off) {
    static std::atomic<int> s_iob{40};
    static std::atomic<uint32_t> s_last{0};
    if (s_last.exchange(indx_off, std::memory_order_relaxed) != indx_off &&
        s_iob.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[INDXOFF] offset={} vs={:016X} idx_count={} prim={}", indx_off,
                  d.vs_hash, index_count, prim);
  }
  EvictStaleCacheEntries(s_idx_cache, idx_frame, s_idx_evict_frame);
  static const bool idx_full_hash = getenv("RESTUFF_FULL_VB_HASH") != nullptr;
  const auto idx_probe = [&](const uint8_t* src, uint32_t len) -> uint64_t {
    if (!src) return 0;
    if (idx_full_hash) return Fnv1a(src, len) ^ (uint64_t(len) << 32);
    const uint32_t head = std::min(len, 4096u);
    uint64_t h = Fnv1a(src, head);
    if (len > 8192) {
      const uint64_t t = Fnv1a(src + len - 4096, 4096);
      h ^= (t << 1) | (t >> 63);
    }
    return h ^ (uint64_t(len) << 32);
  };
  uint32_t max_index = 0;
  std::shared_ptr<const std::vector<uint32_t>> shared_indices;
  const auto _idx_t0 =  // M3.45 (M4.2: sampled, see TotalTimer)
      wperf_sample ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  if (idx_base_phys) {
    const uint32_t src_len = index_count * (idx32 ? 4 : 2);
    // M3.316: alignment follows the index width — dword for u32, word for u16.
    // (& ~0x3u here plus the old decode-time dword mask was the wedge-class
    // hole: odd-start u16 sub-draws shifted one index early.)
    const uint32_t ib_phys = idx_base_phys & ~(idx32 ? 0x3u : 0x1u);
    const uint8_t* raw = memory->TranslatePhysical<const uint8_t*>(ib_phys);
    // Reset policy participates in the KEY: the same index bytes widen
    // differently under different cut rules.
    auto& ce = s_idx_cache[{idx_base_phys, index_count, uint32_t(idx32), indx_off,
                            reset_ena ? (reset_indx | 0x80000000u) : 0u}];
    // M3.166b: same-frame trust, identical to the vertex-stream cache below --
    // an index buffer rewritten mid-frame at the same address is served stale.
    // RESTUFF_VB_NOFRAMECACHE covers BOTH caches so one A/B settles the class.
    static const bool s_no_frame_cache_idx = getenv("RESTUFF_VB_NOFRAMECACHE") != nullptr;
    bool valid = ce.data && !s_no_frame_cache_idx && ce.frame_ok == idx_frame;
    if (!valid && ce.data && raw && idx_probe(raw, src_len) == ce.probe) {
      ce.frame_ok = idx_frame;
      valid = true;
    }
    if (!valid) {
      // M4.10 (RESTUFF_IBTORN=1): the widen loop below reads LIVE guest
      // memory element-by-element -- the game fills UP/dynamic buffers after
      // queueing the packet, so a mid-loop guest write hands us a torn index
      // array. Worse, ce.probe is hashed AFTER the loop, so the tear is
      // stored with a matching probe and revalidates as good. Probe before
      // and after; a mismatch is a caught-in-the-act torn capture.
      static const bool s_ibtorn = getenv("RESTUFF_IBTORN") != nullptr;
      const uint64_t pre_probe = (s_ibtorn && raw) ? idx_probe(raw, src_len) : 0;
      auto fresh = std::make_shared<std::vector<uint32_t>>(index_count);
      uint32_t mx = 0;
      // M3.126: a value only cuts the strip when the GUEST enabled reset, and
      // only when it equals the guest's reset index. Cuts are re-emitted as the
      // u32 restart sentinel (what the pipeline enables); non-cut values keep
      // the VGT_INDX_OFFSET bias (M3.63).
      if (idx32) {
        const uint32_t* src = reinterpret_cast<const uint32_t*>(raw);
        for (uint32_t i = 0; i < index_count; ++i) {
          const uint32_t v = std::byteswap(src[i]);
          const bool cut = reset_ena && v == reset_indx;
          (*fresh)[i] = cut ? 0xFFFFFFFFu : v + indx_off;
          if (!cut) mx = std::max(mx, v + indx_off);
        }
      } else {
        const uint16_t* src = reinterpret_cast<const uint16_t*>(
            memory->TranslatePhysical<const uint16_t*>(ib_phys));
        for (uint32_t i = 0; i < index_count; ++i) {
          const uint16_t v = std::byteswap(src[i]);
          const bool cut = reset_ena && uint32_t(v) == reset_indx;
          (*fresh)[i] = cut ? 0xFFFFFFFFu : uint32_t(v) + indx_off;
          if (!cut) mx = std::max<uint32_t>(mx, uint32_t(v) + indx_off);
        }
      }
      ce.data = std::move(fresh);
      ce.max_index = mx;
      ce.probe = idx_probe(raw, src_len);
      ce.frame_ok = idx_frame;
      if (s_ibtorn && raw && pre_probe != ce.probe) {
        static std::atomic<int> s_torn_budget{60};
        if (s_torn_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[IBTORN] ib=0x{:08X} n={} idx32={} max={} vs={:016X} frame={} "
                      "(guest wrote during widen)",
                      ib_phys, index_count, int(idx32), mx, d.vs_hash, d.cap_frame);
      }
    }
    shared_indices = ce.data;
    max_index = ce.max_index;
    d.indices_sp = shared_indices;  // M3.45: share the cached array, zero-copy
    // M3.69 EXPERIMENT (RESTUFF_STRIPS_TO_LISTS=1|2): expand triangle strips
    // to explicit lists to pin the strip-restart WINDING PARITY semantics.
    // Mode 1 = Vulkan rule (parity resets at each restart) -- must render
    // IDENTICALLY to the strip path (sanity control). Mode 2 = continuous
    // parity across restarts (candidate Xenos rule) -- if the gate's terrain
    // holes FILL, restart-parity divergence is the missing-geometry root: a
    // batch whose restart lands at an odd in-segment position renders its
    // following segment with opposite winding on hardware vs Vulkan, carving
    // coherent polygon-shaped culled patches that aggregate winding votes and
    // same-rule-both-sides discriminators cannot see.
    static const int s_s2l = [] {
      const char* e = getenv("RESTUFF_STRIPS_TO_LISTS");
      return e ? atoi(e) : 0;
    }();
    if (s_s2l && d.prim == 6) {
      const auto& src_idx = *shared_indices;
      auto lst = std::make_shared<std::vector<uint32_t>>();
      lst->reserve(src_idx.size() * 3);
      size_t seg = 0;
      uint32_t gpar = 0;  // continuous parity counter (mode 2)
      for (size_t i = 0; i + 2 < src_idx.size(); ++i) {
        if (src_idx[i] == 0xFFFFFFFFu) {
          seg = i + 1;
          if (s_s2l == 3) gpar = 0;  // mode 3: restart resets the PRIMITIVE counter
          continue;
        }
        uint32_t a = src_idx[i], b2 = src_idx[i + 1], c = src_idx[i + 2];
        if (b2 == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
        if (a == b2 || b2 == c || a == c) { ++gpar; continue; }  // degenerate still advances parity
        const bool odd = s_s2l >= 2 ? (gpar & 1) : ((i - seg) & 1);
        ++gpar;
        if (odd) std::swap(a, b2);
        lst->push_back(a);
        lst->push_back(b2);
        lst->push_back(c);
      }
      d.indices_sp = std::move(lst);
      d.prim = 4;  // triangle list
    }
  } else {
    auto seq = std::make_shared<std::vector<uint32_t>>(index_count);
    // M3.63: the auto index also starts at VGT_INDX_OFFSET on hardware.
    for (uint32_t i = 0; i < index_count; ++i) (*seq)[i] = i + indx_off;
    d.indices_sp = std::move(seq);
    max_index = index_count ? index_count - 1 + indx_off : 0;
  }
  // M3.139: kQuadList (0x0D) has no Vulkan equivalent -- XenosTopology binned it
  // into TRIANGLE_LIST "as a first cut", which reads 4-vertex quads as 3-vertex
  // triangles and shears every one of them. This title draws its particle
  // billboards as quad lists, so it matters as soon as M3.138 stops dropping
  // them. Expand to a triangle list here, using the SDK's vertex order
  // (primitive_processor.h:549): v0,v1,v2 + v0,v2,v3.
  if (d.prim == 13 && d.indices_sp && d.indices_sp->size() >= 4) {
    const auto& src = *d.indices_sp;
    const uint32_t quads = uint32_t(src.size()) / 4;
    auto tri = std::make_shared<std::vector<uint32_t>>();
    tri->reserve(size_t(quads) * 6);
    for (uint32_t q = 0; q < quads; ++q) {
      const uint32_t* v = src.data() + size_t(q) * 4;
      tri->insert(tri->end(), {v[0], v[1], v[2], v[0], v[2], v[3]});
    }
    d.indices_sp = std::move(tri);
    d.prim = 4;  // now a plain triangle list
  }
  if (wperf_sample)
    s_wperf_idx_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now() - _idx_t0)
                                 .count(),
                             std::memory_order_relaxed);
  const uint32_t vcount = max_index + 1;

  // M3.0 multi-stream capture: one VtxStream per distinct fetch slot the VS
  // reads, in first-appearance order of vs.t.attrs (the pipeline's vertex
  // bindings are built with the same rule). All streams are addressed by the
  // same vertex index, so vcount applies to each.
  // M3.4: 3D scenes issue hundreds of draws over the same multi-MB static VB;
  // per-draw copies were GBs/frame. Dedup by (vb_phys, stride, vs_hash) --
  // the vs_hash keys the LE-normalization layout -- for the current guest
  // frame; a cached copy with >= vcount entries is shared as-is, a smaller
  // one is replaced (earlier draws keep the old shared_ptr alive).
  // M3.38: the cache is CROSS-FRAME (clearing it per frame recopied and
  // re-LE-normalized the multi-MB static world VBs 1500 draws' worth every
  // frame ON THE GUEST RENDER THREAD -- walker_ms measured ~330ms/frame =
  // the 4fps). Entries revalidate once per frame against a sampled content
  // probe of the guest source (head+tail 4KB + length; the full-extent FNV
  // is available via RESTUFF_FULL_VB_HASH=1 for A/B -- the M2.4f stale-atlas
  // lesson says partial hashes CAN alias, so if stale geometry ever shows,
  // flip that env first). Probe mismatch or a larger vcount need rebuilds.
  struct StreamCacheEntry {
    std::shared_ptr<const std::vector<uint8_t>> data;
    uint32_t vcount = 0;
    uint32_t src_bytes = 0;
    uint64_t probe = 0;
    uint64_t frame_ok = 0;
    // M4.13 (RESTUFF_BLIP=1): one-frame-anomaly detection. blip_hash is a
    // FULL FNV of the current rebuilt payload (the validation `probe` is
    // sampled head+tail and can miss mid-buffer changes); prev_* hold the
    // previous content generation. An A->X->A hash sequence means content X
    // lived for exactly one generation and REVERTED -- animation never
    // reverts, a torn/misordered write does. On detection the X payload
    // (still in `data`) is diffed against the reverted-to payload.
    uint64_t blip_hash = 0;
    uint64_t prev_blip_hash = 0;
    std::shared_ptr<const std::vector<uint8_t>> prev_payload;
  };
  // M4.24 THE DOCK-FLICKER FIX: the FETCH WINDOW (vb_words) joins the key.
  // The blob-shadow decal pass draws a fixed 1001 indices every frame but
  // sizes its fetch constant to the LIVE decal count -- hardware reads
  // out-of-bounds fetches as zero, collapsing the unused tail (the user's
  // observed distance-cutoff line IS the window edge). Our capture mimics
  // that via zero-fill at copy time -- but the cache key ignored the window,
  // so cached copies from DIFFERENT window generations were served
  // interchangeably and the far tail alternated between real dark decals and
  // zeroed nothing: the Ep5 dock flicker. Keying on the window makes every
  // served copy match the game's current clamp exactly, as hardware did.
  static std::unordered_map<std::tuple<uint32_t, uint64_t, uint64_t>, StreamCacheEntry,
                            CacheKeyHash>
      s_stream_cache;  // M4.2: was std::map
  const uint64_t cur_frame = s_frame_serial.load(std::memory_order_relaxed);
  static uint64_t s_stream_evict_frame = 0;
  EvictStaleCacheEntries(s_stream_cache, cur_frame, s_stream_evict_frame);
  static const bool full_vb_hash = getenv("RESTUFF_FULL_VB_HASH") != nullptr;
  const auto vb_probe = [&](const uint8_t* src, uint32_t len) -> uint64_t {
    if (!src) return 0;
    if (full_vb_hash) return Fnv1a(src, len) ^ (uint64_t(len) << 32);
    const uint32_t head = std::min(len, 4096u);
    uint64_t h = Fnv1a(src, head);
    if (len > 8192) {
      const uint64_t t = Fnv1a(src + len - 4096, 4096);
      h ^= (t << 1) | (t >> 63);
    }
    return h ^ (uint64_t(len) << 32);
  };
  for (const auto& a : vs.t.attrs) {
    bool seen = false;
    for (const auto& s : d.streams) seen |= (s.fetch_slot == a.fetch_slot);
    if (seen) continue;
    if (a.stride_bytes == 0) {
      CapDropLog(3, vsb->info.hash, psb->info.hash, prim, index_count);
      return;
    }
    const uint32_t f0 = s_reg_shadow[0x4800 + a.fetch_slot * 2];
    const uint32_t f1 = s_reg_shadow[0x4800 + a.fetch_slot * 2 + 1];
    const uint32_t vb_phys = f0 & ~0x3u;
    const uint32_t vb_words = (f1 >> 2) & 0xFFFFFF;
    if (!vb_phys) {
      CapDropLog(4, vsb->info.hash, psb->info.hash, prim, index_count);
      return;
    }
    // Sanity gate against truly-garbage index data (a stale fetch constant or
    // misdecoded index stream would blow vcount sky-high).
    if (vcount > 0x100000) {
      CapDropLog(5, vsb->info.hash, psb->info.hash, prim, index_count);
      return;
    }
    rr::VtxStream st;
    st.fetch_slot = a.fetch_slot;
    st.stride = a.stride_bytes;
    auto& cache =
        s_stream_cache[{vb_phys, uint64_t(a.stride_bytes) | (uint64_t(vb_words) << 32),
                        vsb->info.hash}];
    const bool _had_data = static_cast<bool>(cache.data);  // M3.46 classify
    if (cache.data && cache.vcount >= vcount) {
      // M3.166 (RESTUFF_VB_NOFRAMECACHE=1): the same-frame shortcut below
      // trusts a cached stream for the REST OF THE FRAME once its key is hit,
      // with no content check. If the guest streams several chunks through one
      // reused scratch VB address inside a single frame -- which is what a
      // 285-draw frame looks like next to the 79-draw one at the same camera --
      // every later chunk is served the FIRST chunk's vertices, so its ground
      // never gets drawn (the wedge, task #24). This forces per-draw
      // revalidation so the hypothesis can be tested directly.
      static const bool s_no_frame_cache = getenv("RESTUFF_VB_NOFRAMECACHE") != nullptr;
      bool valid = !s_no_frame_cache && cache.frame_ok == cur_frame;
      // M3.167 (RESTUFF_VBSTALECHECK=1): DETECT the condition instead of
      // changing behaviour -- forcing revalidation (NOFRAMECACHE) crashes the
      // allocator under the extra rebuild churn, so it cannot serve as a test.
      // On a same-frame hit, probe anyway and report when the guest actually
      // rewrote that address mid-frame: that is the only way stale reuse can
      // blank a chunk (task #24). Costs one probe per hit; diagnostic only.
      if (valid) {
        static const bool s_stalechk = getenv("RESTUFF_VBSTALECHECK") != nullptr;
        if (s_stalechk) {
          const uint8_t* src2 = memory->TranslatePhysical<const uint8_t*>(vb_phys);
          static std::atomic<uint64_t> s_hits{0}, s_changed{0};
          const uint64_t h = s_hits.fetch_add(1, std::memory_order_relaxed) + 1;
          if (src2 && vb_probe(src2, cache.src_bytes) != cache.probe) {
            const uint64_t c = s_changed.fetch_add(1, std::memory_order_relaxed) + 1;
            static std::atomic<int> s_bud{30};
            if (s_bud.fetch_sub(1, std::memory_order_relaxed) > 0)
              REXLOG_INFO("[VBSTALE] same-frame reuse of REWRITTEN vb=0x{:08X} stride={} "
                          "vs={:016X} vcount={} | changed={}/{} hits",
                          vb_phys, a.stride_bytes, vsb->info.hash, vcount, c, h);
          }
          if ((h % 200000) == 0)
            REXLOG_INFO("[VBSTALE] same-frame hits={} rewritten={}", h,
                        s_changed.load(std::memory_order_relaxed));
        }
      }
      if (!valid) {
        const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(vb_phys);
        if (src && vb_probe(src, cache.src_bytes) == cache.probe) {
          cache.frame_ok = cur_frame;
          valid = true;
        }
      }
      if (valid) {
        s_str_hit.fetch_add(1, std::memory_order_relaxed);
        st.data = cache.data;
        if (d.streams.empty()) d.dbg_vb_phys = vb_phys;
        d.streams.push_back(std::move(st));
        continue;
      }
      {
        // Content changed: rebuild below. M4.13: the wipe must not destroy
        // the payload/hash history the BLIP detector compares against.
        StreamCacheEntry wiped{};
        wiped.data = std::move(cache.data);
        wiped.blip_hash = cache.blip_hash;
        wiped.prev_blip_hash = cache.prev_blip_hash;
        wiped.prev_payload = std::move(cache.prev_payload);
        cache = std::move(wiped);
      }
    }
    (_had_data ? s_str_stale : s_str_new).fetch_add(1, std::memory_order_relaxed);
    // Copy what the fetch constant covers; zero-fill any tail. Some streams
    // are legitimately shorter than vcount entries (e.g. the 9E4052 z-prepass
    // skinning stream), and hardware reads out-of-bounds fetches as zero --
    // dropping the whole draw here is what starved the 3D scene.
    const uint64_t avail = uint64_t(vb_words) * 4;
    const uint64_t want = uint64_t(vcount) * st.stride;
    const size_t copy_bytes = size_t(std::min<uint64_t>(want, avail));
    if (want > avail) {
      s_cap_skip[5].fetch_add(1, std::memory_order_relaxed);  // census only, draw kept
      // M3.165 (RESTUFF_SHORTSTREAM=1): this path KEEPS the draw but ZERO-FILLS
      // the vertices the fetch constant does not cover -- collapsed geometry,
      // which is a candidate for the wedge (task #24): wedge boots split the
      // same geometry into ~2x the draws, and a short window per split draw
      // would blank exactly the chunks that go missing. Logged per (vs, slot)
      // without needing RESTUFF_DUMP_DRAWS (whose vertex-dump path bus-errors).
      static const bool s_ss = getenv("RESTUFF_SHORTSTREAM") != nullptr;
      if (s_ss) {
        static std::mutex m2;
        static std::set<uint64_t> seen2;
        std::lock_guard<std::mutex> lk(m2);
        if (seen2.insert(vsb->info.hash ^ (uint64_t(a.fetch_slot) << 56)).second)
          REXLOG_INFO("[SHORTSTREAM] vs={:016X} slot={} vb_words={} stride={} vcount={} "
                      "want={} avail={} zerofill={}",
                      vsb->info.hash, a.fetch_slot, vb_words, a.stride_bytes, vcount,
                      (unsigned long long)want, (unsigned long long)avail,
                      (unsigned long long)(want - avail));
      }
      if (s_dump_draws) {
        static std::mutex m;
        static std::set<uint64_t> seen_short;
        std::lock_guard<std::mutex> lk(m);
        if (seen_short.insert(vsb->info.hash ^ a.fetch_slot).second) {
          REXLOG_INFO(
              "[DUMP] SHORT-STREAM vs={:016X} slot={} f0={:08X} f1={:08X} vb_words={} "
              "stride={} vcount={} idx32={} n={} (clamped)",
              vsb->info.hash, a.fetch_slot, f0, f1, vb_words, a.stride_bytes, vcount,
              idx32 ? 1 : 0, index_count);
        }
      }
    }
    const uint8_t* vb = memory->TranslatePhysical<const uint8_t*>(vb_phys);
    const auto t_s0 = std::chrono::steady_clock::now();  // M3.43 [WPERF]
    auto fresh = std::make_shared<std::vector<uint8_t>>();
    fresh->resize(size_t(vcount) * st.stride, 0);
    std::memcpy(fresh->data(), vb, copy_bytes);
    if (d.streams.empty()) {
      d.dbg_vb_phys = vb_phys;
      if (s_dump_draws) {
        PendingHashCheck c;
        c.vb_phys = vb_phys;
        c.vb_len = uint32_t(fresh->size());
        c.vb_hash = Fnv1a(vb, c.vb_len);
        c.vs_hash = vsb->info.hash;
        c.idx_phys = idx_base_phys;
        c.idx_len = index_count * (idx32 ? 4 : 2);
        if (idx_base_phys) {
          const uint8_t* ib = memory->TranslatePhysical<const uint8_t*>(idx_base_phys);
          c.idx_hash = ib ? Fnv1a(ib, c.idx_len) : 0;
        } else {
          c.idx_hash = 0;
        }
        s_hash_checks.push_back(c);
      }
    }
    // LE-normalize honoring the vertex fetch constant's endian mode (f1
    // bits 0-1) rather than a per-component reversal. Composing the Xenos
    // endian op with LE component layout, the net per-32-bit-word op is:
    //   1 (8in16): swap bytes within each 16-bit unit.
    //   2 (8in32): reverse the word.
    //   3 (16in32): for 16-bit comps the halves swap x LE-ization nets a
    //     full word reverse -- THE k_16_16 backdrop-cell case: the old
    //     per-16 swap exchanged X/Y for asymmetric cells, parking the felt
    //     and sky right-neighbor columns below the screen (title right
    //     band). For 32-bit comps only the halves swap.
    const uint32_t vf_endian = f1 & 3;
    // RESTUFF_DUMP_VTX=<vs_hash_hex>: hex-dump the first few RAW guest vertex
    // records for that VS *before* LE-normalization, plus the endian mode and
    // attribute layout. The PS ALU is proven bit-exact, so wrong interpolants
    // are the remaining way to get the world-dim -- and a wrong fetch decode
    // (format/endian) is how interpolants go wrong. Having the raw bytes lets
    // each attribute be decoded by hand and checked against what we deliver.
    {
      static const uint64_t want = [] {
        const char* e = getenv("RESTUFF_DUMP_VTX");
        return e ? strtoull(e, nullptr, 16) : 0ull;
      }();
      if (want && d.ps_hash == want) {
        static std::atomic<int> once{1};
        if (once.fetch_sub(1, std::memory_order_relaxed) > 0) {
          std::string lay;
          for (const auto& na : vs.t.attrs)
            if (na.fetch_slot == st.fetch_slot)
              lay += fmt::format(" [loc{} fmt={} off={} sgn={} nrm={}]", na.location, na.format,
                                 na.byte_offset, na.is_signed ? 1 : 0, na.is_normalized ? 1 : 0);
          REXLOG_INFO("[VTXDUMP] vs={:016X} stride={} vcount={} endian={} attrs:{}",
                      d.vs_hash, st.stride, vcount, vf_endian, lay);
          for (uint32_t v = 0; v < 4 && v < vcount; ++v) {
            const uint8_t* src = fresh->data() + size_t(v) * st.stride;
            std::string hex;
            for (uint32_t b = 0; b < st.stride && b < 64; ++b)
              hex += fmt::format("{:02X}{}", src[b], (b % 4 == 3) ? " " : "");
            REXLOG_INFO("[VTXDUMP]   v{} raw: {}", v, hex);
          }
          // Also write the RAW (pre-normalize) stream as mem_<guest_addr>.bin so
          // the SDK gold VS harness (nb_interp_harness) can fetch from it at the
          // address its fetch constant names -- the last piece needed to diff our
          // translated VERTEX shader against the reference, as was done for the PS.
          {
            char path[96];
            snprintf(path, sizeof(path), "mem_%08X.bin", vb_phys);
            const size_t nbytes = size_t(vcount) * st.stride;
            if (FILE* mf = fopen(path, "wb")) {
              fwrite(fresh->data(), 1, nbytes, mf);
              fclose(mf);
              REXLOG_INFO("[VTXDUMP]   wrote {} ({} bytes, vb_phys={:08X})", path, nbytes, vb_phys);
            }
            // The register shadow MUST come from THIS SAME DRAW: dumping it
            // separately (keyed on the PS hash) lands on a different draw whose
            // fetch constants point at a different buffer, and the gold VS then
            // reads unmapped memory and returns identical zeros for every vertex.
            if (FILE* rf = fopen("vtxregs.bin", "wb")) {
              // s_reg_shadow has ALREADY MOVED ON by the time this runs (the
              // capture is async relative to register writes), so a raw copy
              // names a buffer that no longer matches the payload we just
              // dumped -- the gold VS then reads unmapped memory and returns
              // identical zeros for every vertex. Emit a SELF-CONSISTENT copy:
              // force this stream's fetch constant to the f0/f1 actually used
              // to produce mem_<addr>.bin.
              std::vector<uint32_t> regs(s_reg_shadow, s_reg_shadow + 0x5003);
              regs[0x4800 + a.fetch_slot * 2] = f0;
              regs[0x4800 + a.fetch_slot * 2 + 1] = f1;
              fwrite(regs.data(), 4, regs.size(), rf);
              fclose(rf);
              REXLOG_INFO("[VTXDUMP]   wrote vtxregs.bin (slot {} pinned to {:08X})",
                          a.fetch_slot, vb_phys);
            }
            // The REAL index buffer for this draw. Synthesising sequential
            // indices is wrong: this draw uses only a handful of indices out of
            // a 20k-vertex buffer, so made-up indices evaluate entirely
            // different geometry and any comparison against it is meaningless.
            if (idx_base_phys) {
              if (const uint16_t* ib =
                      memory->TranslatePhysical<const uint16_t*>(idx_base_phys)) {
                if (FILE* bf = fopen("vtxib.bin", "wb")) {
                  fwrite(ib, 2, index_count, bf);
                  fclose(bf);
                  REXLOG_INFO("[VTXDUMP]   wrote vtxib.bin ({} indices from {:08X})", index_count,
                              idx_base_phys);
                }
              }
            }
          }
        }
      }
    }
    for (uint32_t vi = 0; vi < vcount; ++vi) {
      uint8_t* dst = fresh->data() + size_t(vi) * st.stride;
      for (const auto& na : vs.t.attrs) {
        if (na.fetch_slot != st.fetch_slot) continue;
        const FmtInfo fi = VtxFmt(na.format);
        // An attribute whose extent exceeds the stride would write into the next
        // vertex -- and past the heap allocation on the last one (glibc abort a
        // few frames later). Skip it: it could never decode meaningfully anyway.
        if (na.byte_offset + fi.comps * fi.bytes > st.stride) {
          static std::atomic<int> s_oob_log{8};
          if (s_oob_log.fetch_sub(1, std::memory_order_relaxed) > 0) {
            REXLOG_WARN(
                "[native_vk] attr exceeds stride: vs={:016X} fmt={} off={} comps={} bytes={} stride={} -- skipping swap",
                s_current_vs_hash, na.format, na.byte_offset, fi.comps, fi.bytes, st.stride);
          }
          continue;
        }
        uint8_t* p = dst + na.byte_offset;
        const uint32_t span = fi.comps * fi.bytes;
        switch (vf_endian) {
          case 1:
            for (uint32_t b = 0; b + 1 < span; b += 2) std::swap(p[b], p[b + 1]);
            break;
          case 2:
            for (uint32_t b = 0; b + 3 < span; b += 4) {
              std::swap(p[b], p[b + 3]);
              std::swap(p[b + 1], p[b + 2]);
            }
            break;
          case 3:
            if (fi.bytes == 2) {
              for (uint32_t b = 0; b + 3 < span; b += 4) {
                std::swap(p[b], p[b + 3]);
                std::swap(p[b + 1], p[b + 2]);
              }
            } else {
              for (uint32_t b = 0; b + 3 < span; b += 4) {
                std::swap(p[b], p[b + 2]);
                std::swap(p[b + 1], p[b + 3]);
              }
            }
            break;
          default:
            break;
        }
        // M3.30: 32-bit INTEGER attrs (k_32/_32_32/_32_32_32_32, formats
        // 33/34/35) previously bound as VK_FORMAT_*_SINT/UINT while every
        // translated VS declares float inputs -- integer-vs-float attribute
        // mismatch = UNDEFINED delivered values (VUID-08733; replay resolves
        // them to zero, live NVIDIA to garbage). Xenos vfetch semantics for
        // these are int->float (scaled, or normalized when the fetch says so):
        // do that conversion here in the payload and bind them as SFLOAT.
        if (na.format == 33 || na.format == 34 || na.format == 35) {
          for (uint32_t c = 0; c < fi.comps; ++c) {
            uint8_t* cp = p + c * 4;
            uint32_t u;
            std::memcpy(&u, cp, 4);
            float fv;
            if (na.is_signed) {
              const int32_t iv = int32_t(u);
              fv = na.is_normalized ? std::max(float(iv) / 2147483647.0f, -1.0f) : float(iv);
            } else {
              fv = na.is_normalized ? float(u) / 4294967295.0f : float(u);
            }
            std::memcpy(cp, &fv, 4);
          }
        }
      }
    }
    // M4.8 (RESTUFF_DEGEN_HUNT=1): dock-flicker forensics. Every falsified
    // hypothesis so far (renderer features, resolve staleness, FTZ, the 0.10
    // codegen fixes) leaves "recompiled CPU code computes wrong geometry" as
    // the standing theory -- so scan every REBUILT stream payload's float
    // components for NaN/Inf at the moment the guest's bytes change. A hit
    // names the stream (vb, stride, attr offset/format, vertex) and the VS,
    // which names the guest routine that wrote it. Post-swizzle scan: bytes
    // are LE floats here (int attrs 33/34/35 already converted above).
    static const bool s_degen_hunt = getenv("RESTUFF_DEGEN_HUNT") != nullptr;
    if (s_degen_hunt) {
      uint32_t bad = 0;
      uint32_t first_v = 0, first_off = 0, first_fmt = 0, first_bits = 0;
      for (uint32_t vi = 0; vi < vcount && bad < 1000; ++vi) {
        const uint8_t* dst = fresh->data() + size_t(vi) * st.stride;
        for (const auto& na : vs.t.attrs) {
          if (na.fetch_slot != st.fetch_slot) continue;
          const FmtInfo fi = VtxFmt(na.format);
          // Genuine float32 formats (36/37/57/38) + the int formats converted
          // to float above (33/34/35) ONLY. Packed formats 6/7/16/17 and the
          // {4,4} default also report bytes==4, and e.g. a white k_8_8_8_8
          // color (0xFFFFFFFF) bit-patterns as NaN -- the first hunt run's 80
          // hits were exactly that false positive.
          if (na.format != 33 && na.format != 34 && na.format != 35 && na.format != 36 &&
              na.format != 37 && na.format != 38 && na.format != 57)
            continue;
          if (na.byte_offset + fi.comps * fi.bytes > st.stride) continue;
          for (uint32_t c = 0; c < fi.comps; ++c) {
            uint32_t w;
            std::memcpy(&w, dst + na.byte_offset + c * 4, 4);
            if ((w & 0x7F800000u) == 0x7F800000u) {  // NaN or Inf
              if (!bad) {
                first_v = vi;
                first_off = na.byte_offset + c * 4;
                first_fmt = na.format;
                first_bits = w;
              }
              ++bad;
            }
          }
        }
      }
      if (bad) {
        static std::atomic<int> s_dh_budget{80};
        if (s_dh_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[DEGEN] STREAM vb=0x{:08X} stride={} vcount={} vs={:016X} bad={} "
                      "first(v={} off={} fmt={} bits=0x{:08X})",
                      vb_phys, st.stride, vcount, s_current_vs_hash, bad, first_v, first_off,
                      first_fmt, first_bits);
      }
    }
    // M4.13 (RESTUFF_BLIP=1): the reversion detector. Full-hash the rebuilt
    // payload; if it equals the generation BEFORE last but not the last one
    // (A -> X -> A), generation X was a one-frame anomaly. Diff X against the
    // reverted-to content and name the attribute(s) that went bad -- this is
    // the "which data goes wrong on a flicker frame" microscope, and it works
    // precisely because the flicker artifact lives for one frame.
    static const bool s_blip = getenv("RESTUFF_BLIP") != nullptr;
    if (s_blip) {
      const uint64_t nh = Fnv1a(fresh->data(), fresh->size());
      if (cache.data && cache.prev_blip_hash && nh == cache.prev_blip_hash &&
          nh != cache.blip_hash && cache.data->size() == fresh->size()) {
        // Per-VS dedupe: the attract screen proved one particle VS rotates a
        // ring of tiny scratch buffers through adjacent addresses, so each
        // ADDRESS legitimately sees content return (ring wrap) -- detail-log
        // only the first few blips per VS, and tally the rest periodically.
        // A torn write shows up as a NEW vs in the tally (or a fresh detail
        // line), not as spam. Walker thread only; plain statics are fine.
        static std::unordered_map<uint64_t, uint64_t> s_blip_by_vs;
        static uint64_t s_blip_total = 0;
        uint64_t& vs_n = s_blip_by_vs[s_current_vs_hash];
        ++vs_n;
        if (++s_blip_total % 500 == 0) {
          char tally[280] = {};
          int tp = 0;
          for (const auto& [h, n] : s_blip_by_vs) {
            if (tp > int(sizeof(tally)) - 40) break;
            tp += std::snprintf(tally + tp, sizeof(tally) - tp, " %016llX:%llu",
                                (unsigned long long)h, (unsigned long long)n);
          }
          REXLOG_INFO("[BLIP] tally total={} by_vs:{}", s_blip_total, tally);
        }
        if (vs_n <= 3) {
          const uint8_t* xa = cache.data->data();  // the anomalous generation
          const uint8_t* norm = fresh->data();     // what it reverted to
          uint32_t ndiff = 0;
          char detail[256] = {};
          int dp = 0;
          for (size_t off = 0; off + 3 < fresh->size(); off += 4) {
            uint32_t wa, wn;
            std::memcpy(&wa, xa + off, 4);
            std::memcpy(&wn, norm + off, 4);
            if (wa == wn) continue;
            if (ndiff < 4) {
              const uint32_t rel = uint32_t(off % st.stride);
              uint32_t fmt = 0;
              for (const auto& na : vs.t.attrs) {
                if (na.fetch_slot != st.fetch_slot) continue;
                const FmtInfo fi = VtxFmt(na.format);
                if (rel >= na.byte_offset && rel < na.byte_offset + fi.comps * fi.bytes)
                  fmt = na.format;
              }
              dp += std::snprintf(detail + dp, sizeof(detail) - dp,
                                  " (v=%u off=%u fmt=%u was=%08X now=%08X)",
                                  unsigned(off / st.stride), rel, fmt, wa, wn);
            }
            ++ndiff;
          }
          REXLOG_INFO("[BLIP] STREAM vb=0x{:08X} stride={} vcount={} vs={:016X} frame={} "
                      "ndiff={}{}",
                      vb_phys, st.stride, vcount, s_current_vs_hash, cur_frame, ndiff, detail);
        }
      }
      cache.prev_blip_hash = cache.blip_hash;
      cache.prev_payload = std::move(cache.data);
      cache.blip_hash = nh;
    }
    st.data = fresh;
    cache.data = std::move(fresh);
    cache.vcount = vcount;
    // M3.38: stamp the source-content probe for cross-frame revalidation.
    cache.src_bytes = uint32_t(copy_bytes);
    cache.probe = vb_probe(vb, uint32_t(copy_bytes));
    cache.frame_ok = cur_frame;
    s_wperf_stream_ns.fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t_s0)
            .count(),
        std::memory_order_relaxed);
    d.streams.push_back(std::move(st));
  }
  // M3.11: register-relative fetch stream (skinning bone data). Not a vertex
  // binding -- the whole fetch-constant range is captured and served to the VS
  // as a storage buffer. The relative fetches decode 32-bit floats, so
  // LE-normalize per 32-bit word by the fetch constant's endian mode.
  // A rel slot that is ALSO an attribute stream needs no extra capture (its
  // payload uploads as a vertex stream; the SSBO binds the same bytes). Only
  // rel-ONLY slots get captured here (one supported; two rel-only slots warn).
  // M3.101: capture the FULL fetch-constant range for EVERY rel-fetch slot.
  // A rel slot that doubles as an attribute stream previously bound only the
  // captured VERTEX WINDOW (vcount*stride) -- but the skinned shadow-volume
  // VSes index bones at word offsets far beyond it (bone_id*c254.z +
  // trunc(c9.x)), so every fetch clamped to the last vertex record and the
  // volumes rendered degenerate (empty footprint = no shadows). The full
  // range is what the guest's fetch constant actually describes.
  // M4.0: rel-fetch payload cache, mirroring s_stream_cache above. This lambda
  // used to allocate + copy + byte-swap the WHOLE fetch-constant range (up to
  // 4 MB -- the slot-95 static mesh streams) for EVERY rel-fetch draw, EVERY
  // frame, and the fresh shared_ptr each time also defeated the renderer's
  // pointer-keyed vb_cache, so the same bytes re-uploaded to the ring per
  // frame too.
  // M4.1 VALIDATION IS FULL-EXTENT, NOT SAMPLED. The first cut reused the
  // vertex streams' head+tail 4KB probe -- and shipped exactly the failure the
  // M3.38 comment predicts: animated BONE MATRICES whose first/last 4KB happen
  // to be stable (bind-pose roots, padding) probe-matched while mid-buffer
  // bones changed, so skinned draws rendered STALE POSES -- the user-visible
  // intro-cutscene background flicker (bisected to this cache via
  // RESTUFF_NO_RELCACHE). Policy now:
  //   <= 1MB (every animated bone/morph payload): full 4-lane FNV on EVERY
  //     call -- microseconds, catches all changes including mid-frame actor
  //     scratch reuse.
  //   > 1MB (the level-load-static mesh streams): full hash once per FRAME,
  //     trusted within it. Frame-granular full coverage still catches every
  //     persistent change; only a mid-frame rewrite of a multi-MB static
  //     stream could slip one frame, which the guest has never been seen to do.
  // Rebuilds allocate FRESH vectors -- shared payloads are never mutated in
  // place (the M3.315 immutability contract).
  // RESTUFF_NO_RELCACHE=1 restores the old always-copy behaviour for A/B.
  struct RelCacheEntry {
    std::shared_ptr<const std::vector<uint8_t>> data;
    uint32_t src_bytes = 0;
    uint64_t probe = 0;
    uint64_t frame_ok = 0;
    // M4.13 (RESTUFF_BLIP=1): previous content generation for the A->X->A
    // reversion detector (see StreamCacheEntry). rel probes are already
    // full-extent, so `probe` itself is the content id here.
    uint64_t prev_probe = 0;
    std::shared_ptr<const std::vector<uint8_t>> prev_payload;
  };
  static std::unordered_map<std::pair<uint32_t, uint64_t>, RelCacheEntry, CacheKeyHash>
      s_rel_cache;  // M4.2: was std::map
  static uint64_t s_rel_evict_frame = 0;
  EvictStaleCacheEntries(s_rel_cache, cur_frame, s_rel_evict_frame);
  static const bool s_no_relcache = getenv("RESTUFF_NO_RELCACHE") != nullptr;
  const auto cap_rel = [&](uint32_t slot) -> std::shared_ptr<const std::vector<uint8_t>> {
    if (slot == ~0u) return nullptr;
    const uint32_t f0 = s_reg_shadow[0x4800 + slot * 2];
    const uint32_t f1 = s_reg_shadow[0x4800 + slot * 2 + 1];
    const uint32_t vb_phys = f0 & ~0x3u;
    const uint32_t vb_words = (f1 >> 2) & 0xFFFFFF;
    if (!vb_phys || !vb_words || vb_words > (1u << 20)) return nullptr;  // <= 4 MB sanity
    const uint8_t* vb = memory->TranslatePhysical<const uint8_t*>(vb_phys);
    if (!vb) return nullptr;
    const uint32_t vf_endian = f1 & 3;
    const uint32_t src_bytes = vb_words * 4;
    s_rel_calls.fetch_add(1, std::memory_order_relaxed);
    // M4.1: full-extent 4-lane FNV (the single-lane chain is a serial multiply
    // dependency at ~1GB/s; four lanes run at memory speed -- same trick as
    // GuestTextureContentHash).
    const auto rel_hash = [](const uint8_t* p, size_t n) -> uint64_t {
      uint64_t h0 = 1469598103934665603ull, h1 = 0x9E3779B97F4A7C15ull;
      uint64_t h2 = 0xC2B2AE3D27D4EB4Full, h3 = 0x165667B19E3779F9ull;
      size_t i = 0;
      for (; i + 32 <= n; i += 32) {
        uint64_t w0, w1, w2, w3;
        std::memcpy(&w0, p + i, 8);
        std::memcpy(&w1, p + i + 8, 8);
        std::memcpy(&w2, p + i + 16, 8);
        std::memcpy(&w3, p + i + 24, 8);
        h0 = (h0 ^ w0) * 1099511628211ull;
        h1 = (h1 ^ w1) * 1099511628211ull;
        h2 = (h2 ^ w2) * 1099511628211ull;
        h3 = (h3 ^ w3) * 1099511628211ull;
      }
      for (; i < n; ++i) h0 = (h0 ^ p[i]) * 1099511628211ull;
      uint64_t h = h0 ^ (h1 * 0x9E3779B97F4A7C15ull);
      h ^= (h2 << 1) | (h2 >> 63);
      h ^= (h3 << 2) | (h3 >> 62);
      return h ^ (uint64_t(n) << 32);
    };
    RelCacheEntry* cache = nullptr;
    // The hash is taken BEFORE the copy: hash-after-copy would fingerprint
    // post-change bytes onto a pre-change payload if the guest rewrites the
    // region mid-capture, making the stale pose persistent. Hash-before-copy
    // fails safe (mismatch -> rebuild on the next call).
    uint64_t fresh_probe = 0;
    bool have_probe = false;
    if (!s_no_relcache) {
      cache = &s_rel_cache[{vb_phys, (uint64_t(vb_words) << 2) | vf_endian}];
      const bool big_static = src_bytes > (1u << 20);
      if (cache->data && cache->src_bytes == src_bytes) {
        if (big_static && cache->frame_ok == cur_frame) {
          s_rel_hit.fetch_add(1, std::memory_order_relaxed);
          return cache->data;
        }
        fresh_probe = rel_hash(vb, src_bytes);
        have_probe = true;
        s_rel_bytes_hashed.fetch_add(src_bytes, std::memory_order_relaxed);
        if (fresh_probe == cache->probe) {
          cache->frame_ok = cur_frame;
          s_rel_hit.fetch_add(1, std::memory_order_relaxed);
          return cache->data;
        }
      }
      if (!have_probe) {
        fresh_probe = rel_hash(vb, src_bytes);
        have_probe = true;
        s_rel_bytes_hashed.fetch_add(src_bytes, std::memory_order_relaxed);
      }
    }
    s_rel_rebuild.fetch_add(1, std::memory_order_relaxed);
    auto fresh = std::make_shared<std::vector<uint8_t>>(vb, vb + size_t(vb_words) * 4);
    uint8_t* p = fresh->data();
    for (size_t b = 0; b + 3 < fresh->size(); b += 4) {
      switch (vf_endian) {
        case 1: std::swap(p[b], p[b + 1]); std::swap(p[b + 2], p[b + 3]); break;
        case 2: std::swap(p[b], p[b + 3]); std::swap(p[b + 1], p[b + 2]); break;
        case 3: std::swap(p[b], p[b + 2]); std::swap(p[b + 1], p[b + 3]); break;
        default: break;
      }
    }
    // M4.8 (RESTUFF_DEGEN_HUNT=1): scan rebuilt rel-fetch (bone/morph)
    // payloads for NaN/Inf -- see the stream-scan comment above. These are
    // float-dense by construction, so a whole-payload scan is meaningful.
    static const bool s_degen_hunt_rel = getenv("RESTUFF_DEGEN_HUNT") != nullptr;
    if (s_degen_hunt_rel) {
      uint32_t bad = 0, first_off = 0, first_bits = 0;
      const uint8_t* pd = fresh->data();
      for (size_t off = 0; off + 3 < fresh->size() && bad < 1000; off += 4) {
        uint32_t w;
        std::memcpy(&w, pd + off, 4);
        if ((w & 0x7F800000u) == 0x7F800000u) {
          if (!bad) { first_off = uint32_t(off); first_bits = w; }
          ++bad;
        }
      }
      if (bad) {
        static std::atomic<int> s_dhr_budget{80};
        if (s_dhr_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[DEGEN] REL slot={} vb=0x{:08X} bytes={} vs={:016X} bad={} "
                      "first(off={} bits=0x{:08X})",
                      slot, vb_phys, fresh->size(), s_current_vs_hash, bad, first_off,
                      first_bits);
      }
    }
    if (cache) {
      // M4.13 (RESTUFF_BLIP=1): A->X->A reversion detector on bone/morph
      // payloads -- stale or torn bone matrices are the classic collapsed-
      // black-triangle generator (M4.1 precedent).
      static const bool s_blip_rel = getenv("RESTUFF_BLIP") != nullptr;
      if (s_blip_rel && have_probe) {
        if (cache->data && cache->prev_probe && fresh_probe == cache->prev_probe &&
            fresh_probe != cache->probe && cache->data->size() == fresh->size()) {
          static std::atomic<int> s_blipr_budget{40};
          if (s_blipr_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            const uint8_t* xa = cache->data->data();
            const uint8_t* norm = fresh->data();
            uint32_t ndiff = 0;
            char detail[224] = {};
            int dp = 0;
            for (size_t off = 0; off + 3 < fresh->size(); off += 4) {
              uint32_t wa, wn;
              std::memcpy(&wa, xa + off, 4);
              std::memcpy(&wn, norm + off, 4);
              if (wa == wn) continue;
              if (ndiff < 4)
                dp += std::snprintf(detail + dp, sizeof(detail) - dp,
                                    " (off=%u was=%08X now=%08X)", unsigned(off), wa, wn);
              ++ndiff;
            }
            REXLOG_INFO("[BLIP] REL slot={} vb=0x{:08X} bytes={} vs={:016X} frame={} ndiff={}{}",
                        slot, vb_phys, fresh->size(), s_current_vs_hash, cur_frame, ndiff,
                        detail);
          }
        }
        cache->prev_probe = cache->probe;
        cache->prev_payload = cache->data;
      }
      cache->data = fresh;
      cache->src_bytes = src_bytes;
      cache->probe = fresh_probe;
      cache->frame_ok = cur_frame;
    }
    return fresh;
  };
  if (vs.t.rel_fetch_slot != ~0u) {
    if (auto payload = cap_rel(vs.t.rel_fetch_slot)) {
      d.rel_stream_slot = vs.t.rel_fetch_slot;
      d.rel_stream_data = std::move(payload);
    }
  }
  if (vs.t.rel_fetch_slot2 != ~0u && vs.t.rel_fetch_slot2 != vs.t.rel_fetch_slot) {
    if (auto payload = cap_rel(vs.t.rel_fetch_slot2)) {
      d.rel_stream_slot2 = vs.t.rel_fetch_slot2;
      d.rel_stream_data2 = std::move(payload);
    }
  }
  // M3.140: "no vertex streams" is not the same as "no vertex data". A
  // register-relative shader carries its vertices in rel_stream_data (a storage
  // buffer), never in d.streams, so this gate re-drops exactly the particle
  // draws M3.138 just let past the attribute gate.
  // ⚠️ OPT-IN (RESTUFF_RELFETCH_NOSTREAMS=1) -- CORRECT IN PRINCIPLE, BUT IT
  // REGRESSES LEVEL LOADING. Reproduced twice: the drive boots, then sits on
  // the Loading screen for the whole run, with the guest issuing ~452k draws
  // against ~9.15M for the same drive with this off. No Vulkan error and no
  // crash, so it is a stall/starvation somewhere downstream of letting
  // stream-less draws into PrepareTranslatedDraws, not a validation fault.
  // Diagnose before enabling: the particle draws cannot render until it is on.
  // M3.141: the rescue MUST be narrow. Written as "has rel-fetch data" it also
  // admitted prim=4/6 SKINNED meshes -- including the bear's shadow volume
  // 9E4052352C9BEB99 with 25KB of bone data. Those shaders DECLARE vertex
  // attributes and normally render from real streams; reaching here with
  // streams.empty() means their stream capture failed, so rescuing them pushes
  // a class of broken skinned draws at the renderer and the level never
  // finishes loading. A procedural billboard shader is distinguishable: it
  // declares NO attributes at all (attrs=0) because every vertex is built from
  // the rel-fetch buffer and the vertex index. Rescue only those.
  // DEFAULT ON since M3.142 fixed the wedge (the attrs[0] OOB read above).
  // RESTUFF_NO_PARTICLES=1 disables. Verified: 2 consecutive full drives reach
  // gameplay with 1643 particle draws captured, no guest fault -- against a
  // 100% wedge rate (3/3 at exactly 150s) before the guard.
  static const bool relfetch_nostreams = getenv("RESTUFF_NO_PARTICLES") == nullptr;
  const bool relfetch_rescued = relfetch_nostreams && vs.t.attrs.empty() &&
                                (d.rel_stream_data || d.rel_stream_data2);
  // M3.141: name what this rescue actually admits. The gate is written in terms
  // of "has rel-fetch data", which is broader than "is a particle", so the
  // level-load stall may well be caused by some OTHER draw class it lets in.
  // One line per distinct (vs, prim) so the population is identifiable.
  if (relfetch_rescued) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    std::lock_guard<std::mutex> lk(m);
    if (seen.insert(vsb->info.hash ^ (uint64_t(prim) << 56)).second)
      REXLOG_INFO("[RELRESCUE] vs={:016X} ps={:016X} prim={} n={} rel1={} rel2={}",
                  vsb->info.hash, psb->info.hash, prim, index_count,
                  d.rel_stream_data ? d.rel_stream_data->size() : 0,
                  d.rel_stream_data2 ? d.rel_stream_data2->size() : 0);
  }
  // M3.142: the wedge is deterministic (3 runs, same counters +-0.7%, 150s) and
  // capture-side (it survives RESTUFF_SKIP_VS), so the LAST rescued draw before
  // the ring goes quiet is the trigger or next to it. Log every one, with the
  // fields a bad draw would show up in: index count, rel-buffer sizes, and the
  // rel fetch slots. RESTUFF_RESCUE_TRACE=1.
  if (relfetch_rescued) {
    static const bool trace = getenv("RESTUFF_RESCUE_TRACE") != nullptr;
    if (trace) {
      static std::atomic<uint32_t> seq{0};
      REXLOG_INFO("[RESCUETRACE] #{} vs={:016X} prim={} n={} slot1={} slot2={} rel1={} rel2={}",
                  seq.fetch_add(1, std::memory_order_relaxed), vsb->info.hash, prim, index_count,
                  vs.t.rel_fetch_slot, vs.t.rel_fetch_slot2,
                  d.rel_stream_data ? d.rel_stream_data->size() : 0,
                  d.rel_stream_data2 ? d.rel_stream_data2->size() : 0);
    }
  }
  if (d.streams.empty() && !relfetch_rescued) {
    // M3.128b: distinct reason -- this is "no vertex streams at all" (e.g. the
    // const-position probe draws), NOT the zero-stride attribute case at the
    // fetch loop above. They shared counter 3 and made "stride0" unreadable.
    CapDropLog(7, vsb->info.hash, psb->info.hash, prim, index_count);
    return;
  }

  // Per-draw clip transform. Matrix-transformed shaders emit clip space
  // (identity + Vulkan y-flip). Pretransformed shaders emit window coords in a
  // float position; if those exceed NDC range, map window->NDC by the 1280x720
  // render target. (Distinguishes D586AD-style NDC from 8859F7-style screen.)
  // M3.18: honor the guest viewport scale SIGNS. Hardware maps clip->window as
  // window = ndc*SCALE + OFFSET; a negative XSCALE mirrors X (user-visible:
  // move right, the bear moves left) and a positive YSCALE un-flips Y. Shots
  // change signs per camera -- hardcoding {+X,-Y} mirrored/flipped them.
  {
    const float sxs = ShadowRegF32(0x210F), sys = ShadowRegF32(0x2111);
    d.ndc[0] = (std::isfinite(sxs) && sxs < 0.0f) ? -1.0f : 1.0f;
    d.ndc[1] = (std::isfinite(sys) && sys > 0.0f) ? 1.0f : -1.0f;
  }
  d.ndc[2] = 0.0f; d.ndc[3] = 0.0f;
  // DEBUG: log guest viewport + surface regs whenever they CHANGE (the one-shot
  // startup probe missed later scene switches; the matrix-draw path currently
  // ignores XSCALE, so a non-640 title viewport would misplace every clip draw).
  if (s_dump_draws) {
    const uint32_t vx = s_reg_shadow[0x210F], vxo = s_reg_shadow[0x2110];
    const uint32_t vy = s_reg_shadow[0x2111], vyo = s_reg_shadow[0x2112];
    const uint32_t si = s_reg_shadow[0x2000];
    const uint64_t sig = (uint64_t(vx ^ (vxo << 1) ^ (vy << 2) ^ (vyo << 3)) << 32) | si;
    static std::atomic<uint64_t> s_vpsig{~0ull};
    if (s_vpsig.exchange(sig, std::memory_order_relaxed) != sig) {
      REXLOG_INFO("[DUMP] VPORT-CHG xs={} xo={} ys={} yo={} surf_pitch={} msaa={} (vs={:016X})",
                  ShadowRegF32(0x210F), ShadowRegF32(0x2110), ShadowRegF32(0x2111),
                  ShadowRegF32(0x2112), si & 0x3FFF, (si >> 16) & 3, s_current_vs_hash);
    }
  }
  // M3.142: attrs[0]/streams[0] here are UNGUARDED, and this is the exact
  // wedge behind the particle stall. This window-space heuristic inspects the
  // POSITION ATTRIBUTE's raw magnitude, so it is meaningless for a procedural
  // shader that has no attributes and no streams at all -- but it used to be
  // unreachable for those, because the streams-empty gate above dropped them
  // first. The moment the particle rescue keeps one, `vs.t.attrs[0]` indexes an
  // empty vector on the GUEST RENDER THREAD mid-PM4-walk: undefined behaviour,
  // no Vulkan error, and the ring simply stops being kicked (one rescued draw
  // was enough -- trace showed the guest died right after #0). Skip the
  // heuristic when there is no position attribute to measure.
  if (!vs.t.attrs.empty() && !d.streams.empty()) {
    const uint32_t pf = vs.t.attrs[0].format;
    const bool pos_float = pf == 31 || pf == 32 || pf == 36 || pf == 37 || pf == 38 || pf == 57;
    // Matrix VSes (oPos computed from c[] constants) emit clip space no matter
    // how large their RAW inputs are — the 3464 text family feeds window-like
    // coords (2399..4329, the page carousel) through a c0/c1 transform. Only a
    // true passthrough VS can be window-space.
    if (pos_float && !vs.t.pos_reads_consts) {
      const rr::VtxStream& ps0 = d.streams[0];  // attrs[0] defines stream 0
      float maxabs = 0.0f;
      for (uint32_t vi = 0; vi < vcount; ++vi) {
        const float* p =
            reinterpret_cast<const float*>(ps0.bytes().data() + size_t(vi) * ps0.stride +
                                           vs.t.attrs[0].byte_offset);
        maxabs = std::max(maxabs, std::max(std::fabs(p[0]), std::fabs(p[1])));
      }
      const uint32_t surf_msaa = (s_reg_shadow[0x2000] >> 16) & 3;
      if (maxabs > 2.0f) d.window_space = true;  // M3.293: UI/HUD quad
      if (maxabs > 2.0f && surf_msaa != 0) {
        // Window-space draw on an MSAA surface: guest pixel coords address
        // sample BLOCKS of the same EDRAM allocation the 1x pass sees as
        // 1360x736 (4X = 2x2 samples/pixel, 2X = 1x2). The scene target is
        // that sample grid (visible 1280x720, resolve rect origin 0,0). This
        // is how the per-frame background fill covers the whole screen with
        // one 640x360 quad -- mapping it via the viewport made it a quarter
        // rect and left the right band/corner to stale history.
        const float sx = (surf_msaa == 2) ? 2.0f : 1.0f;
        const float sy = 2.0f;
        d.ndc[0] = sx * 2.0f / 1280.0f;
        d.ndc[1] = -sy * 2.0f / 720.0f;
        d.ndc[2] = -1.0f;
        d.ndc[3] = 1.0f;
      } else if (maxabs > 2.0f) {  // window-space: map to NDC via the guest viewport
        // PA_CL_VPORT_ XSCALE/XOFFSET/YSCALE/YOFFSET: window = clip*scale+offset,
        // so ndc = (window - offset)/scale.
        const float xs = ShadowRegF32(0x210F), xo = ShadowRegF32(0x2110);
        const float ys = ShadowRegF32(0x2111), yo = ShadowRegF32(0x2112);
        if (std::isfinite(xs) && xs != 0.0f && std::isfinite(ys) && ys != 0.0f) {
          d.ndc[0] = 1.0f / xs;
          d.ndc[2] = -xo / xs;
          d.ndc[1] = 1.0f / ys;
          d.ndc[3] = -yo / ys;
        } else {
          d.ndc[0] = 2.0f / 1280.0f;
          d.ndc[1] = -2.0f / 720.0f;
          d.ndc[2] = -1.0f;
          d.ndc[3] = 1.0f;
        }
      }
    }
  }

  // D3D9 integer pixel centers (PA_SU_VTX_CNTL.PIX_CENTER=0, this title always):
  // the guest authors geometry for pixel centers at integer coords, Vulkan
  // samples at half-integer centers. Shift +0.5px so edge-abutting backdrop
  // cells don't sample their atlas' transparent guard column (the x=1023 seam)
  // and -0.5-style window quads cover pixels exactly.
  if ((s_reg_shadow[0x2302] & 1) == 0) {
    if (std::fabs(d.ndc[0]) == 1.0f && std::fabs(d.ndc[1]) == 1.0f && d.ndc[2] == 0.0f &&
        d.ndc[3] == 0.0f) {  // M3.18: matrix path now carries per-shot signs
      // clip-space passthrough: convert half a pixel into clip units via the
      // guest viewport scales (signs fold in through ndc[0]/ndc[1] and xs/ys).
      const float xs = ShadowRegF32(0x210F), ys = ShadowRegF32(0x2111);
      if (std::isfinite(xs) && xs != 0.0f && std::isfinite(ys) && ys != 0.0f) {
        d.ndc[2] += 0.5f * d.ndc[0] / xs;
        d.ndc[3] += 0.5f * d.ndc[1] / ys;
      }
    } else {
      // window-space paths: ndc[0]/ndc[1] are already px-to-NDC scales.
      d.ndc[2] += 0.5f * d.ndc[0];
      d.ndc[3] += 0.5f * d.ndc[1];
    }
  }

  // Full 256-vec4 constant blocks: the translated GLSL declares `vec4 c[256]`,
  // so the bound UBO range must cover it. Offset by the stage's SQ_*_CONST base.
  const uint32_t vs_base = s_reg_shadow[0x2307] & 0x1FF;
  const uint32_t ps_base = s_reg_shadow[0x2308] & 0x1FF;
  // M3.43: bulk-copy the 256-vec4 blocks. The shadow regs hold the raw bits
  // (ShadowRegF32 is a bit_cast), so a memcpy of the contiguous range is
  // identical to the per-element loop but replaces 2048 calls/draw with two
  // copies. s_wperf_const_ns times it (see [WPERF] in the alive line).
  // M4.2: snapshot SHARING -- if no ALU float/bool/loop register was written
  // since the previous draw and the bases match (s_alu_const_gen, bumped in
  // WriteShadowRegister), reuse the previous draw's immutable blocks instead
  // of allocating+copying 8KB. Exact by construction: the key counts actual
  // writes. RESTUFF_NO_CONSTGEN=1 restores a fresh copy per draw.
  const auto t_c0 =  // M4.2: sampled, see TotalTimer
      wperf_sample ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  {
    static const bool s_no_constgen = getenv("RESTUFF_NO_CONSTGEN") != nullptr;
    static std::shared_ptr<const rr::ConstBank::Block> s_snap_vs, s_snap_ps;
    static uint64_t s_snap_key = ~0ull;
    // gen is 64-bit and bases are 9-bit; fold bases into the low bits (gen
    // saturating into the top 46 bits would need ~2 years of writes to wrap).
    const uint64_t snap_key = (s_alu_const_gen << 18) | (uint64_t(vs_base) << 9) | ps_base;
    if (s_no_constgen || snap_key != s_snap_key || !s_snap_vs) {
      auto vsb = std::make_shared_for_overwrite<rr::ConstBank::Block>();
      auto psb = std::make_shared_for_overwrite<rr::ConstBank::Block>();
      std::memcpy(vsb->data(), &s_reg_shadow[(0x4000 + vs_base * 4) & 0x7FFF], 256 * 4 * 4);
      std::memcpy(psb->data(), &s_reg_shadow[(0x4000 + ps_base * 4) & 0x7FFF], 256 * 4 * 4);
      s_snap_vs = std::move(vsb);
      s_snap_ps = std::move(psb);
      s_snap_key = snap_key;
    }
    d.vs_consts.reset(s_snap_vs);
    d.ps_consts.reset(s_snap_ps);
  }
  if (wperf_sample)
    s_wperf_const_ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - t_c0)
                                   .count(),
                               std::memory_order_relaxed);
  // M3.174 probe: identify the PROXY draws inside extent-query brackets --
  // the true-extent fix needs their exact vertex format and transform (the
  // c0..c3 replica may not apply to screen-space quads). Logs vs hash, attr0
  // format/offset, stride, index count and the first vertex raw floats.
  if (s_extq_probe && s_extq_open && !d.streams.empty() && !vs.t.attrs.empty()) {
    static std::atomic<int> s_qp{60};
    if (s_qp.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const auto& sb = d.streams[0].bytes();
      const uint32_t stride = d.streams[0].stride;
      float f[4] = {0, 0, 0, 0};
      const size_t off = vs.t.attrs[0].byte_offset;
      if (stride && sb.size() >= off + 16) std::memcpy(f, sb.data() + off, 16);
      REXLOG_INFO("[EXTQPROBE] vs={:016X} nidx={} fmt={} off={} stride={} v0=({:.3f},{:.3f},{:.3f},{:.3f})",
                  d.vs_hash, d.idx().size(), int(vs.t.attrs[0].format),
                  vs.t.attrs[0].byte_offset, stride, f[0], f[1], f[2], f[3]);
    }
  }
  // M3.172: feed the extent accumulator with this draw's screen-space bbox.
  // Sample the position stream (attr0, f32x3) through the c0..c3 columns --
  // the same transform the winding census uses. Sampled, not exhaustive: the
  // guest only needs a bounding box, and this runs on the walker thread.
  static const bool s_ext_bracket_feed = getenv("RESTUFF_EXT_BRACKET") != nullptr;
  if ((s_ext_true || s_ext_bracket_feed) && !d.streams.empty() && !vs.t.attrs.empty() &&
      vs.t.attrs[0].format == 57 && d.vs_consts.size() >= 16) {
    const auto& sb = d.streams[0].bytes();
    const uint32_t stride = d.streams[0].stride;
    if (stride >= 12) {
      const size_t nv = sb.size() / stride;
      const size_t step = nv > 256 ? nv / 256 : 1;  // <=256 samples per draw
      for (size_t v = 0; v < nv; v += step) {
        const size_t off = v * stride + vs.t.attrs[0].byte_offset;
        if (off + 12 > sb.size()) break;
        float pos[3];
        std::memcpy(pos, sb.data() + off, 12);
        float o[4];
        for (int r = 0; r < 4; ++r)
          o[r] = pos[0] * d.vs_consts[r] + pos[1] * d.vs_consts[4 + r] +
                 pos[2] * d.vs_consts[8 + r] + d.vs_consts[12 + r];
        if (!(o[3] > 1e-6f) || !std::isfinite(o[3])) continue;
        // D3D NDC: y=+1 is the TOP of the screen, so screen y flips.
        const float sx = (o[0] / o[3] * 0.5f + 0.5f) * 1280.0f;
        const float sy = (1.0f - (o[1] / o[3] * 0.5f + 0.5f)) * 720.0f;
        if (std::isfinite(sx) && std::isfinite(sy)) {
          s_ext_accum.add(sx, sy);
          if (s_extq_open) s_extq_box.add(sx, sy);  // M3.185: feed the bracket
        }
      }
    }
  }
  if (s_wperf_calls.fetch_add(1, std::memory_order_relaxed) % 20000 == 19999) {
    // M3.45: total_ms is the whole per-draw cost; idx_ms the index-array copy;
    // misc = total - const - stream - idx = PM4 read + alloc churn + submit.
    // M4.2: const/idx/total are 1-in-16 sampled (see TotalTimer), scaled x16
    // here; stream is exact (miss-path only).
    REXLOG_INFO("[WPERF] per-20k-draws const_ms={} stream_ms={} idx_ms={} total_ms={} "
                "strm(hit={} new={} stale={}) rel(calls={} hit={} rebuild={} hash_mb={})",
                s_wperf_const_ns.exchange(0, std::memory_order_relaxed) * 16 / 1000000,
                s_wperf_stream_ns.exchange(0, std::memory_order_relaxed) / 1000000,
                s_wperf_idx_ns.exchange(0, std::memory_order_relaxed) * 16 / 1000000,
                s_wperf_total_ns.exchange(0, std::memory_order_relaxed) * 16 / 1000000,
                s_str_hit.exchange(0, std::memory_order_relaxed),
                s_str_new.exchange(0, std::memory_order_relaxed),
                s_str_stale.exchange(0, std::memory_order_relaxed),
                s_rel_calls.exchange(0, std::memory_order_relaxed),
                s_rel_hit.exchange(0, std::memory_order_relaxed),
                s_rel_rebuild.exchange(0, std::memory_order_relaxed),
                s_rel_bytes_hashed.exchange(0, std::memory_order_relaxed) >> 20);
  }
  // M3.9x WORLD-DIM: the sun-shaft PS (A17EC3C3 / 9585B8F9) darkens the whole
  // world via occ = max(1 - |world-sun|/c8.w, 0). The emulated reference runs
  // this draw INDOORS with the block ZEROED (c8=c9=c10=c11=0, c12=1) => 1/c8.w
  // = inf => occ = 0 = effect disabled; native carries the full OUTDOOR sun
  // (c8=(-22.9,54.9,-80.3,199.96)) => occ 0.37 => the ~2x dim. Log the bank we
  // actually captured + which base it came from. RESTUFF_SHAFTCONST=1.
  {
    static const bool s_shaftlog = getenv("RESTUFF_SHAFTCONST") != nullptr;
    if (s_shaftlog && (d.ps_hash == 0xA17EC3C3A107872Bull || d.ps_hash == 0x9585B8F9EC2B8F95ull) &&
        d.ps_consts.size() >= 64) {
      static std::atomic<int> s_sb{60};
      if (s_sb.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const float* c = d.ps_consts.data();
        // cw_total = writes into the watched window so far; cw_since = writes
        // since the PREVIOUS shaft draw (0 => the bank is STALE between draws).
        const uint64_t tot = s_cw_count.load(std::memory_order_relaxed);
        const uint64_t since =
            tot - s_cw_count_at_last_shaft.exchange(tot, std::memory_order_relaxed);
        // PARAM_GEN gate: if the guest did NOT enable it, native must NOT
        // clobber interpolator r[param_gen_pos] with FragCoord -- doing so
        // would replace the quad's real interpolated UV and change what depth
        // texel each pixel samples (uniform vs localized shaft).
        // NOTE: read the shadow regs DIRECTLY -- d.sq_program_cntl/context_misc
        // are assigned further below, so reading them here yields zeros.
        {
          const uint32_t spc = s_reg_shadow[0x2180], scm = s_reg_shadow[0x2181];
          // Fetch constants for slots 0..3, dword_3: exp_adjust (bits 13..18,
          // signed) + mag/min filter (bits 19..22). d.tex[] isn't parsed yet at
          // this point in the function, so read the shadow regs directly.
          for (uint32_t fs = 0; fs < 4; ++fs) {
            const uint32_t fw3 = s_reg_shadow[0x4800 + fs * 6 + 3];
            const uint32_t fw1 = s_reg_shadow[0x4800 + fs * 6 + 1];
            if (!fw1) continue;  // unbound slot
            // The shaft PS samples a SUN-OCCLUSION buffer. Its base address is
            // the link between "the composite darkens" and "something wrong
            // rendered into that buffer": match it against the RESOLVE-NEW
            // destinations to find which pass produces what we sample.
            const uint32_t fw0 = s_reg_shadow[0x4800 + fs * 6 + 0];
            const uint32_t fw2 = s_reg_shadow[0x4800 + fs * 6 + 2];
            REXLOG_INFO("[SHAFTC-TEX] slot={} dword3={:08X} exp_adjust={} magf={} minf={} "
                        "base=0x{:08X} {}x{} fmt={}",
                        fs, fw3, int32_t(fw3 << 13) >> 26, (fw3 >> 19) & 3, (fw3 >> 21) & 3,
                        (fw0 & ~0xFFFu), (fw2 & 0x1FFF) + 1, ((fw2 >> 13) & 0x1FFF) + 1,
                        fw1 & 0x3F);
          }
          REXLOG_INFO("[SHAFTC-PG] ps={:016X} sq_program_cntl={:08X} pgen_enable(bit18)={} "
                      "sq_context_misc={:08X} param_gen_pos={} nstreams={} stride0={}",
                      d.ps_hash, spc, (spc >> 18) & 1, scm, (scm >> 8) & 0x3F,
                      uint32_t(d.streams.size()),
                      d.streams.empty() ? 0u : d.streams[0].stride);
        }
        REXLOG_INFO("[SHAFTC] ps={:016X} ps_base={} (c8 dwords @0x{:04X}) "
                    "c8=({:.4f},{:.4f},{:.4f},{:.4f}) c11=({:.2f},{:.2f},{:.2f},{:.2f}) "
                    "c12=({:.2f},{:.2f},{:.2f},{:.2f}) c13=({:.6f},{:.6f}) "
                    "cw_total={} cw_since={} cw_last=({:.4f},{:.4f},{:.4f},{:.4f})",
                    d.ps_hash, ps_base, 0x4000 + (ps_base + 8) * 4, c[32], c[33], c[34], c[35],
                    c[44], c[45], c[46], c[47], c[48], c[49], c[50], c[51], c[52], c[53], tot,
                    since, std::bit_cast<float>(s_cw_last[0].load(std::memory_order_relaxed)),
                    std::bit_cast<float>(s_cw_last[1].load(std::memory_order_relaxed)),
                    std::bit_cast<float>(s_cw_last[2].load(std::memory_order_relaxed)),
                    std::bit_cast<float>(s_cw_last[3].load(std::memory_order_relaxed)));
      }
    }
  }
  // M3.9x DIM DIAGNOSTIC (RESTUFF_SHAFT_OFF=1): the emulated reference binds a
  // shaft constant block whose c8.w (sun falloff radius) is ZERO -- 1/0 = inf =>
  // occ = max(1-dist*inf, 0) = exactly 0 => the effect contributes nothing. That
  // is the ONLY way to produce the reference's measured 92% exact-zero occ
  // field. Native instead binds the enabled block (c8.w = 199.96) and so
  // darkens every pixel by ~0.36 => the global world dim. Forcing c8.w=0 here
  // emulates binding the disabled block; if the dim vanishes, the mechanism is
  // confirmed and the real fix is selecting the correct constant window.
  {
    static const bool s_shaft_off = getenv("RESTUFF_SHAFT_OFF") != nullptr;
    if (s_shaft_off && d.ps_hash == 0xA17EC3C3A107872Bull && d.ps_consts.size() >= 36)
      d.ps_consts.set(8 * 4 + 3, 0.0f);  // c8.w = falloff radius (M4.2: COW detach)
  }
  // SHADER_CONSTANT_BOOL_000_031 .. _224_255 (0x4900..0x4907): the 256 uniform
  // bool flags the PS microcode's kCondJmp branches read. Captured per-draw so
  // the translated shader can honour the game's feature gates instead of
  // flattening every conditional block (which corrupts accumulators like r3).
  for (uint32_t i = 0; i < 8; ++i) d.bool_consts[i] = s_reg_shadow[0x4900 + i];
  for (uint32_t i = 0; i < 32; ++i) d.loop_consts[i] = s_reg_shadow[0x4908 + i];
  d.sq_program_cntl = s_reg_shadow[0x2180];
  d.sq_context_misc = s_reg_shadow[0x2181];
  if (d.ps_hash == 0x1BAB95FEECAC8E97ull) {
    static std::atomic<int> s_bool_budget{4};
    if (s_bool_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[native_vk] WORLD-PS bools[4..7]={:08X} {:08X} {:08X} {:08X} "
                  "(b128..159 in [4])",
                  d.bool_consts[4], d.bool_consts[5], d.bool_consts[6], d.bool_consts[7]);
    // World-dim: dump the light block (p6 fog, p9/p11/p13/p15 light dirs,
    // p10/p12/p14 colours) so it can be diffed against the reference build's
    // ground-truth values recovered from its PM4 trace. Env-gated: this is a
    // per-draw dump and the world PS runs hundreds of times a frame.
    // (constants + textures dumped below, AFTER the texture-parsing loop --
    // d.tex[] is empty at this point.)
  }

  for (const auto& t : ps.t.textures) {
    if (t.fetch_slot >= rr::kMaxTexSlots) {
      static std::atomic<int> s_bigslot{8};
      if (s_bigslot.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_WARN("[native_vk] PS {:016X} samples texture fetch slot {} (>= {})",
                    psb->info.hash, t.fetch_slot, rr::kMaxTexSlots);
      continue;
    }
    const uint32_t slot = t.fetch_slot;
    const uint32_t base = 0x4800 + t.fetch_slot * 6;
    const uint32_t w0 = s_reg_shadow[base + 0], w1 = s_reg_shadow[base + 1],
                   w2 = s_reg_shadow[base + 2], w3 = s_reg_shadow[base + 3];
    const uint32_t ba = (w1 >> 12) & 0xFFFFF;
    const uint32_t width = (w2 & 0x1FFF) + 1, height = ((w2 >> 13) & 0x1FFF) + 1;
    // Hills-lag probe: for the cell/sprite family, compare the 0x4801 value the
    // walker applied against a LIVE re-read of the ring word it came from.
    if (s_dump_draws && t.fetch_slot == 0 &&
        s_current_vs_hash == 0xEE34A8BA31895FACull && s_last_tex0w.payload_phys) {
      const float c0w = ShadowRegF32(0x4000 + vs_base * 4 + 3);
      const bool hills_fam =
          std::fabs(c0w + 0.84624535f) < 1e-5f || std::fabs(c0w - 0.15862906f) < 1e-5f;
      static std::atomic<int> s_lagbudget{60};
      if (hills_fam && s_lagbudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
        auto* mem2 = rex::Runtime::instance()->memory();
        const uint32_t live = std::byteswap(
            *mem2->TranslatePhysical<const uint32_t*>(s_last_tex0w.payload_phys));
        REXLOG_INFO(
            "[DUMP] TEXLAG serial={} w1={:08X} lastw{{serial={} val={:08X} phys={:08X} "
            "live={:08X}}} c0w={:.8f}",
            s_draw_serial, w1, s_last_tex0w.serial, s_last_tex0w.value,
            s_last_tex0w.payload_phys, live, c0w);
      }
    }
    // DEBUG: dump the raw texture fetch for the fullscreen 8859F7 quad (RT-present
    // suspect) and, for comparison, the felt EE34 draws. Also RB surface regs.
    if (s_dump_draws &&
        (s_current_vs_hash == 0x8859F7516ED40755ull || s_current_vs_hash == 0xEE34A8BA31895FACull)) {
      static std::atomic<int> s_txlog{16};
      if (s_txlog.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[DUMP] TEXFETCH vs={:016X} slot={} w0={:08X} w1={:08X} w2={:08X} -> ba=0x{:08X} {}x{} fmt={} | RB_SURFACE(2000)={:08X} RB_COLOR(2001)={:08X} COPYDST(2318)={:08X} SU_VTX(2302)={:08X} VTE(2206)={:08X}",
                    s_current_vs_hash, t.fetch_slot, w0, w1, w2, ba << 12, width, height, w1 & 0x3F,
                    s_reg_shadow[0x2000], s_reg_shadow[0x2001], s_reg_shadow[0x2318],
                    s_reg_shadow[0x2302], s_reg_shadow[0x2206]);
    }
    if (ba && width > 1) {
      auto& gt = d.tex[slot];
      gt.valid = true;
      gt.phys_addr = ba << 12;
      gt.width = width;
      gt.height = height;
      const uint32_t pitch = (w0 >> 22) & 0x1FF;
      gt.pitch_texels = pitch ? (pitch << 5) : width;
      gt.format = w1 & 0x3F;
      gt.endian = (w1 >> 6) & 0x3;
      gt.tiled = (w0 >> 31) & 1;
      gt.clamp_x = (w0 >> 10) & 7;
      gt.clamp_y = (w0 >> 13) & 7;
      // sign_x/y/z at w0 bits 2/4/6; value 3 = GAMMA (sRGB). Colour channels
      // carry the same setting for an sRGB texture; test red.
      gt.gamma = ((w0 >> 2) & 0x3) == 0x3;
      // dword_3 EXP_ADJUST: signed 6-bit at bit 13 -- fetched values are scaled
      // by 2^exp_adjust. We ignored it entirely, which left the sun-shaft's
      // depth fetch unscaled => its world reconstruction fell inside the sun
      // falloff at every pixel => uniform screen-wide darkening (the world dim).
      gt.exp_adjust = int32_t(w3 << 13) >> 26;  // sign-extend bits 13..18
    }
  }

  // M3.9x: log the COMPOSITE draw's guest texture ADDRESSES (any draw whose
  // slot-2 is the 640x360 occlusion buffer). Placed AFTER the texture-parsing
  // loop -- d.tex[] is empty earlier in this function. The reference's
  // composite (trace draw #1778) reads slot0=05CA4000 scene / slot1=0590C000
  // depth / slot2=061BC000 occ, and 061BC000 is NOT what its shaft chain wrote
  // (0B51C000 / 0B60D000). If native's composite samples the shaft chain's
  // output instead, that mis-binding is the dim.
  {
    static const bool s_complog = getenv("RESTUFF_COMPLOG") != nullptr;
    if (s_complog && d.tex[2].valid && d.tex[2].width == 640 && d.tex[2].height == 360) {
      static std::atomic<int> s_cb{12};
      if (s_cb.fetch_sub(1, std::memory_order_relaxed) > 0) {
        // Print FORMAT too: the reference's combine (trace #1776) binds
        // slot1 = fmt 23 (a 640x360 DEPTH). If native binds a colour buffer
        // there, the combine samples the wrong thing.
        std::string s;
        for (uint32_t k = 0; k < 4; ++k) {
          const auto& t = d.tex[k];
          s += fmt::format(" s{}={:08X} {}x{} fmt={}{}", k, t.phys_addr, t.width, t.height,
                           t.format, t.valid ? "" : "(INVALID)");
        }
        REXLOG_INFO("[COMPTEX] ps={:016X}{}", psb->info.hash, s);
      }
    }
  }
  // World-dim: dump the world PS light block (p6 fog, p9/p11/p13/p15 light
  // dirs, p10/p12/p14 colours) plus its bound textures, for diffing against
  // the reference build's ground-truth values recovered from its PM4 trace.
  // Must sit AFTER the texture loop above -- d.tex[] is empty before it.
  // Post pass CABCC0E0 computes EXPORT = r0.yyyy * scene + blur -- a scale-and-
  // add on the scene. Doubling its output recovers ~96% of the reference frame
  // brightness, so a too-small scale factor is the prime world-dim suspect.
  // The factor comes out of c16/c17/c19 (+c255 literals), so dump those.
  // RESTUFF_STENCIL_CENSUS=1: how much of the frame depends on STENCIL, which
  // this backend does not implement at all (our depth image is D32_SFLOAT --
  // no stencil aspect -- and no pipeline sets stencilTestEnable). The sun-shaft
  // draw runs with RB_DEPTHCONTROL bit0 (stencil_enable) SET and func=LESS, so
  // the guest masks that pass; drawing it unmasked over every pixel is exactly
  // the observed flat ~0.36 occlusion indoors. Count draws that TEST stencil vs
  // those that WRITE it (non-KEEP zpass/fail ops) to size the fix.
  static const bool s_stencil_census = getenv("RESTUFF_STENCIL_CENSUS") != nullptr;  // M4.2: cached
  if (s_stencil_census) {
    const uint32_t dc = s_reg_shadow[0x2200];
    static std::atomic<uint64_t> s_tot{0}, s_test{0}, s_write{0};
    const bool st_en = dc & 1u;
    // stencilfail bits11-13, stencilzpass 14-16, stencilzfail 17-19 (0 = KEEP)
    const bool st_wr = st_en && (((dc >> 11) & 7) || ((dc >> 14) & 7) || ((dc >> 17) & 7));
    const uint64_t n = s_tot.fetch_add(1, std::memory_order_relaxed);
    if (st_en) s_test.fetch_add(1, std::memory_order_relaxed);
    if (st_wr) s_write.fetch_add(1, std::memory_order_relaxed);
    // Distinct stencil CONFIGURATIONS: the shaft tests "0 < stencil", so
    // something must WRITE a non-zero value or it can never pass. Log each
    // unique (func, failOp, passOp, zfailOp, ref, cmask, wmask) once.
    if (st_en) {
      const uint32_t sr = s_reg_shadow[0x210D];
      const uint64_t sig = (uint64_t(dc & 0xFFFFFF01u) << 32) | sr;
      static std::atomic<uint64_t> s_seen[24];
      static std::atomic<int> s_nseen{0};
      bool known = false;
      const int cnt = s_nseen.load(std::memory_order_relaxed);
      for (int k = 0; k < cnt; ++k)
        if (s_seen[k].load(std::memory_order_relaxed) == sig) { known = true; break; }
      if (!known && cnt < 24) {
        s_seen[cnt].store(sig, std::memory_order_relaxed);
        s_nseen.store(cnt + 1, std::memory_order_relaxed);
        REXLOG_INFO("[STENCILCFG] #{} sfunc={} fail={} zpass={} zfail={} ref={} cmask=0x{:02X} "
                    "wmask=0x{:02X} zen={} zwr={} ZFUNC={} ps={:016X} surf=0x{:08X} di=0x{:08X}",
                    cnt, (dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7, (dc >> 17) & 7,
                    sr & 0xFF, (sr >> 8) & 0xFF, (sr >> 16) & 0xFF, (dc >> 1) & 1,
                    (dc >> 2) & 1, (dc >> 4) & 7, d.ps_hash, s_reg_shadow[0x2000],
                    s_reg_shadow[0x2002]);
      }
    }
    if ((n % 20000) == 19999)
      REXLOG_INFO("[STENCIL] draws={} stencil_test={} stencil_write={} (last dc=0x{:08X} "
                  "func={} ref/mask=0x{:08X})",
                  n + 1, s_test.load(std::memory_order_relaxed),
                  s_write.load(std::memory_order_relaxed), dc, (dc >> 8) & 7,
                  s_reg_shadow[0x210D]);
  }
  // RESTUFF_POSTCHAIN=1: the WHOLE post chain in submission order. Every post
  // pass uses the same fullscreen-quad VS (0x2766CBE92CD1C91A), so keying on it
  // yields an ordered list of (shader -> inputs -> target, blend). Needed
  // because our post gains only 1.28x where the reference gains 1.57x, and the
  // bloom pyramid comes out nearly black (means 0.8..2.4/255) while the shaft
  // pyramid sits at a flat 91.5.
  // NOTE: keying on the fullscreen VS 0x2766CBE92CD1C91A MISSED post passes that
  // use a DIFFERENT fullscreen VS (e.g. ps=CABCC0E0214BDEA4 / vs=629FBF1C8FB741BA
  // writes the MAIN scene). Key on "small indexed draw that samples something"
  // instead so the enumeration is complete.
  static const bool s_postchain = getenv("RESTUFF_POSTCHAIN") != nullptr;  // M4.2: cached
  if (s_postchain &&
      (d.vs_hash == 0x2766CBE92CD1C91Aull || d.vs_hash == 0x629FBF1C8FB741BAull ||
       (d.idx().size() <= 6 && d.tex[0].valid))) {
    static std::atomic<int> s_pc{200};
    if (s_pc.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const uint32_t bc = s_reg_shadow[0x2201];
      std::string ins;
      for (uint32_t k = 0; k < rr::kMaxTexSlots; ++k)
        if (d.tex[k].valid)
          ins += fmt::format("s{}=0x{:08X}({}x{},f{}) ", k, d.tex[k].phys_addr, d.tex[k].width,
                             d.tex[k].height, d.tex[k].format);
      REXLOG_INFO("[POSTCHAIN] ps={:016X} vs={:016X} -> surf=0x{:08X} blend=0x{:08X} "
                  "csrc={} cdst={} cop={} | {}",
                  d.ps_hash, d.vs_hash, s_reg_shadow[0x2000], bc, bc & 0x1F, (bc >> 8) & 0x1F,
                  (bc >> 5) & 7, ins);
    }
  }
  // RESTUFF_SAMPLERS=<hexaddr>: which draws SAMPLE a given surface, with the
  // blend they reach the framebuffer through. The sun-shaft pass writes a
  // blurred-depth buffer; the darkening has to happen where that buffer is
  // CONSUMED, and this names the consumer (shader + blend + target surface).
  static const char* s_samplers_env = getenv("RESTUFF_SAMPLERS");  // M4.2: cached
  if (const char* sa = s_samplers_env) {
    static const uint32_t want = uint32_t(strtoul(sa, nullptr, 16));
    for (uint32_t k = 0; k < rr::kMaxTexSlots; ++k) {
      if (!d.tex[k].valid || d.tex[k].phys_addr != want) continue;
      static std::atomic<int> s_sm{40};
      if (s_sm.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const uint32_t bc = s_reg_shadow[0x2201];
        REXLOG_INFO("[SAMPLES] 0x{:08X} slot={} ps={:016X} vs={:016X} blendctl=0x{:08X} "
                    "csrc={} cdst={} cop={} surf=0x{:08X} cmask=0x{:X}",
                    want, k, d.ps_hash, d.vs_hash, bc, bc & 0x1F, (bc >> 8) & 0x1F,
                    (bc >> 5) & 7, s_reg_shadow[0x2000], s_reg_shadow[0x2104] & 0xF);
      }
      break;
    }
  }
  // What the sun-shaft PS ACTUALLY samples, read from the parsed d.tex[] rather
  // than from a hand-decode of the fetch constants (that guess disagreed with
  // the bind path -- RESTUFF_TEXBIND_LOG never fired for either address form).
  // The shaft darkens the world by 1.41x; if its occlusion source is a depth
  // surface we never populate, it samples garbage and the dim follows.
  // RESTUFF_PSTEX=<hex ps hash> retargets this probe at any shader (the post
  // composite F5443DCCB449C724 needs the same treatment as the shaft PS).
  static const uint64_t s_pstex = [] {
    const char* e = getenv("RESTUFF_PSTEX");
    return e ? strtoull(e, nullptr, 16) : 0xA17EC3C3A107872Bull;
  }();
  static const bool s_shafttex = getenv("RESTUFF_SHAFTTEX") != nullptr;  // M4.2: cached
  if (s_shafttex && d.ps_hash == s_pstex) {
    static std::atomic<int> s_st{60};
    if (s_st.fetch_sub(1, std::memory_order_relaxed) > 0) {
      for (uint32_t k = 0; k < rr::kMaxTexSlots; ++k) {
        const auto& t = d.tex[k];
        if (!t.valid) continue;
        REXLOG_INFO("[SHAFTTEX] slot={} phys=0x{:08X} {}x{} pitch={} fmt={} tiled={} gamma={}", k,
                    t.phys_addr, t.width, t.height, t.pitch_texels, t.format, t.tiled ? 1 : 0,
                    t.gamma ? 1 : 0);
      }
      // Which constant block did THIS draw capture? The game uploads two sets
      // at the same ps_base=256 -- enabled (c8.w=199.96, sun falloff radius)
      // and disabled (c8=(1,1,1,1) -> occ = 1-dist/1 -> clamps to 0). If our
      // per-draw constant snapshot races the guest's uploads we apply the
      // ENABLED block to a draw that should be inert, which is exactly a flat
      // ~0.36 occlusion indoors. Print in draw order to see the pattern.
      if (d.ps_consts.size() >= 36) {
        static std::atomic<uint32_t> s_sd{0};
        REXLOG_INFO("[SHAFTSEQ] draw#{} c8=({:.4f},{:.4f},{:.4f},{:.4f}) {}",
                    s_sd.fetch_add(1, std::memory_order_relaxed), d.ps_consts[32],
                    d.ps_consts[33], d.ps_consts[34], d.ps_consts[35],
                    d.ps_consts[35] > 1.5f ? "ENABLED" : "inert");
      }
      // How does this pass reach the framebuffer? An ADDITIVE blend can only
      // brighten, so a shaft that DARKENS would mean our occlusion term is
      // inverted; a multiplicative/alpha blend makes darkening by-design and
      // turns the question into one of magnitude. RB_BLENDCONTROL0 0x2201:
      // src bits0-4, op bits5-7, dst bits8-12 (alpha in 16-20/21-23/24-28).
      const uint32_t bc = s_reg_shadow[0x2201];
      // RB_STENCILREFMASK (0x2210): ref bits0-7, compare mask 8-15, write mask
      // 16-23. If this reads 0 for a func=LESS stencil test, the comparison is
      // 0<0 -> ALWAYS FAIL -> the pass is rejected everywhere, which would
      // explain the reference's window light-shafts being absent from ours.
      REXLOG_INFO("[SHAFTSTENCIL] stencilrefmask=0x{:08X} ref={} cmask=0x{:02X} wmask=0x{:02X} "
                  "st_en={} st_func={}",
                  s_reg_shadow[0x210D], s_reg_shadow[0x210D] & 0xFF,
                  (s_reg_shadow[0x210D] >> 8) & 0xFF, (s_reg_shadow[0x210D] >> 16) & 0xFF,
                  s_reg_shadow[0x2200] & 1, (s_reg_shadow[0x2200] >> 8) & 7);
      REXLOG_INFO("[SHAFTBLEND] blendctl=0x{:08X} csrc={} cdst={} cop={} asrc={} adst={} aop={} "
                  "cmask=0x{:X} depthctl=0x{:08X} colorinfo=0x{:08X} surf=0x{:08X}",
                  bc, bc & 0x1F, (bc >> 8) & 0x1F, (bc >> 5) & 7, (bc >> 16) & 0x1F,
                  (bc >> 24) & 0x1F, (bc >> 21) & 7, s_reg_shadow[0x2104] & 0xF,
                  s_reg_shadow[0x2200], s_reg_shadow[0x2001], s_reg_shadow[0x2000]);
    }
  }
  static const bool s_postconst = getenv("RESTUFF_POSTCONST") != nullptr;
  if (s_postconst && d.ps_hash == 0xCABCC0E0214BDEA4ull && d.ps_consts.size() >= 256 * 4) {
    static std::atomic<int> s_pc{2};
    if (s_pc.fetch_sub(1, std::memory_order_relaxed) > 0) {
      std::string s;
      for (uint32_t c : {15u, 16u, 17u, 18u, 19u, 253u, 254u, 255u})
        s += fmt::format("\n    p[{:3d}] = ({:12.6f},{:12.6f},{:12.6f},{:12.6f})", c,
                         d.ps_consts[c * 4 + 0], d.ps_consts[c * 4 + 1], d.ps_consts[c * 4 + 2],
                         d.ps_consts[c * 4 + 3]);
      REXLOG_INFO("[POSTCONST] ps=CABCC0E0214BDEA4 tex0={:08X} {}x{}{}", d.tex[0].phys_addr,
                  d.tex[0].width, d.tex[0].height, s);
    }
  }
  // Floor shader 02876DE9B27B1B35: its measured light accumulator r1 (~0.35)
  // is BELOW the structural minimum (~0.84 = four lights x the 0.4 ambient
  // floor), which is impossible unless its actual constants differ from the
  // terrain's. Dump the ones the light blocks read: c8 seed, c9..c16 dir/colour
  // pairs, c254 (the max operand, .y assumed 0.4), plus its bool word.
  // RESTUFF_DUMP_REGS=<ps_hash_hex>: write the whole guest register shadow for
  // that shader's draw to regs_<hash>.bin, in the same index space nb_ps_gold /
  // nb_interp_harness expect. Lets the SDK gold interpreter be run on EXACTLY
  // the constants native used, with no PM4-trace harness needed.
  static const uint64_t s_dumpregs = [] {
    const char* e = getenv("RESTUFF_DUMP_REGS");
    return e ? strtoull(e, nullptr, 16) : 0ull;
  }();
  if (s_dumpregs && d.ps_hash == s_dumpregs) {
    static std::atomic<int> once{1};
    if (once.fetch_sub(1, std::memory_order_relaxed) > 0) {
      char path[128];
      snprintf(path, sizeof(path), "regs_%016llX.bin", (unsigned long long)d.ps_hash);
      if (FILE* f = fopen(path, "wb")) {
        fwrite(s_reg_shadow, 4, 0x5003, f);
        fclose(f);
        REXLOG_INFO("[DUMPREGS] wrote {} ({} dwords) for ps={:016X}", path, 0x5003, d.ps_hash);
      }
    }
  }
  // RESTUFF_FLOORCOUNT=1: count how many times the floor PS is submitted per
  // frame, split by colour mask. The reference (PM4 trace) issues THREE
  // colour-writing floor draws; with SRC_ALPHA/1-SRC_ALPHA blending, repeated
  // draws converge the result toward the source colour, so drawing it fewer
  // times lands closer to the (dark) background -- a candidate mechanism for
  // the uniform ~2x deficit.
  static const bool s_fcount = getenv("RESTUFF_FLOORCOUNT") != nullptr;
  if (s_fcount && d.ps_hash == 0x02876DE9B27B1B35ull) {
    static std::atomic<uint32_t> n_colour{0}, n_depth{0};
    static std::atomic<uint64_t> last_frame{0};
    const uint64_t f = cur_frame;
    if (f != last_frame.exchange(f)) {
      const uint32_t c = n_colour.exchange(0), z = n_depth.exchange(0);
      if (c || z) {
        static std::atomic<int> b{12};
        if (b.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[FLOORCOUNT] frame {}: colour-writing floor draws={} depth-only={}", f, c, z);
      }
    }
    if ((s_reg_shadow[0x2104] & 0xF) == 0) n_depth.fetch_add(1); else n_colour.fetch_add(1);
  }
  // RESTUFF_BOOLHIST=1: histogram of bools4 over colour-writing draws, to
  // compare against the reference's per-draw distribution from the PM4 trace
  // (which records `bool=` for every draw). Only the FLOOR draw's bool word was
  // ever checked before; the native<->reference pixel fit varies per surface,
  // which is what a per-draw light-count difference would look like.
  // RESTUFF_BOOLRACE=1: is the register shadow mutating DURING one draw's
  // capture? d.bool_consts[4] was read earlier in this same function; re-read
  // the same register now and compare. Any mismatch proves the per-draw state
  // is not a consistent snapshot (which would scramble per-draw light counts
  // while leaving rarely-changing constants looking correct).
  static const bool s_brace = getenv("RESTUFF_BOOLRACE") != nullptr;
  if (s_brace) {
    const uint32_t now4 = s_reg_shadow[0x4904];
    static std::atomic<uint32_t> same{0}, diff{0};
    if (now4 == d.bool_consts[4]) same.fetch_add(1); else diff.fetch_add(1);
    static std::atomic<int> b{8};
    const uint32_t tot = same.load() + diff.load();
    if ((tot % 5000) == 4999 && b.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[BOOLRACE] draws={} stable={} MUTATED_MID_CAPTURE={}", tot, same.load(),
                  diff.load());
  }
  static const bool s_bhist = getenv("RESTUFF_BOOLHIST") != nullptr;
  if (s_bhist && (s_reg_shadow[0x2104] & 0xF) != 0) {
    static std::mutex bm;
    static std::map<uint32_t, uint32_t> hist;
    static std::atomic<uint32_t> total{0};
    {
      std::lock_guard<std::mutex> lk(bm);
      hist[d.bool_consts[4]]++;
    }
    // Report a PER-FRAME histogram, not a cumulative one: the cumulative count
    // spans menus (where bools=0 is legitimate) and cannot be compared against
    // the reference's single-gameplay-frame sample.
    static std::atomic<uint64_t> lastf{0};
    const uint64_t fnow = s_frame_serial.load(std::memory_order_relaxed);
    if (fnow != lastf.exchange(fnow)) {
      std::lock_guard<std::mutex> lk(bm);
      uint32_t tot = 0;
      for (auto& [v, c] : hist) tot += c;
      if (tot > 200) {  // gameplay frames only; menu frames are tiny
        static std::atomic<int> b{6};
        if (b.fetch_sub(1, std::memory_order_relaxed) > 0) {
          std::string h;
          for (auto& [v, c] : hist)
            h += fmt::format(" {:08X}x{}({:.1f}%)", v, c, 100.0 * double(c) / double(tot));
          REXLOG_INFO("[BOOLHIST] frame {} colour draws={}:{}", fnow, tot, h);
        }
      }
      hist.clear();
    }
    total.fetch_add(1);
  }
  static const bool s_fconst = getenv("RESTUFF_FLOORCONST") != nullptr;
  if (s_fconst && d.ps_hash == 0x02876DE9B27B1B35ull && d.ps_consts.size() >= 256 * 4) {
    static std::atomic<int> s_fc{40};
    if (s_fc.fetch_sub(1, std::memory_order_relaxed) > 0) {
      std::string s;
      for (uint32_t c : {8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u, 25u, 254u, 255u})
        s += fmt::format("\n    p[{:3d}] = ({:11.6f},{:11.6f},{:11.6f},{:11.6f})", c,
                         d.ps_consts[c * 4 + 0], d.ps_consts[c * 4 + 1], d.ps_consts[c * 4 + 2],
                         d.ps_consts[c * 4 + 3]);
      // Blend/depth state for THIS draw: the earlier BLENDLOG only sampled
      // draws with index_count > 5000, so the floor draw (idx=10) was never
      // covered. The reference's floor draw uses SRC_ALPHA/1-SRC_ALPHA.
      const uint32_t bc_f = s_reg_shadow[0x2201];
      REXLOG_INFO("[FLOORCONST] vs={:016X} bools4={:08X} idx={} tex0={}x{} "
                  "blendctl={:08X} (src={} op={} dst={}) cmask={:X} depthctl={:08X}{}",
                  d.vs_hash, d.bool_consts[4], index_count, d.tex[0].width, d.tex[0].height, bc_f,
                  bc_f & 0x1F, (bc_f >> 5) & 7, (bc_f >> 8) & 0x1F, s_reg_shadow[0x2104] & 0xF,
                  s_reg_shadow[0x2200], s);
    }
  }
  static const bool s_wconst = getenv("RESTUFF_WORLDCONST") != nullptr;
  if (s_wconst && d.ps_hash == 0x1BAB95FEECAC8E97ull && d.ps_consts.size() >= 256 * 4) {
    static std::atomic<int> s_wc_budget{2};
    if (s_wc_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      std::string tx;
      for (uint32_t k = 0; k < 4 && k < rr::kMaxTexSlots; ++k)
        tx += fmt::format(" s{}={:08X} {}x{} fmt={}{}", k, d.tex[k].phys_addr, d.tex[k].width,
                          d.tex[k].height, d.tex[k].format, d.tex[k].valid ? "" : "(INVALID)");
      auto put = [&](std::string& o, uint32_t c, const char* note) {
        o += fmt::format("\n    p[{:3d}] = ({:12.6f},{:12.6f},{:12.6f},{:12.6f})  {}", c,
                         d.ps_consts[c * 4 + 0], d.ps_consts[c * 4 + 1], d.ps_consts[c * 4 + 2],
                         d.ps_consts[c * 4 + 3], note);
      };
      std::string s;
      put(s, 8, "ambient seed");
      for (uint32_t i = 0; i < 4; ++i) {
        put(s, 9 + i * 2, "light dir");
        put(s, 10 + i * 2, "light colour");
      }
      for (uint32_t c = 17; c <= 20; ++c) put(s, c, "point-light block");
      // The shader's COMPILER LITERALS: r8 = tex*c255 + c254 is the normal-map
      // unpack and max(ndotl, c253.z) is the light clamp. The reference's PM4
      // trace never recorded these slots (0xDEADBEEF fill), so native's values
      // are the only way to check them -- a wrong unpack rescales every normal
      // and would show up as a near-uniform brightness error.
      put(s, 253, "literal: light clamp (want .z == 0)");
      put(s, 254, "literal: normal bias (want ~-1)");
      put(s, 255, "literal: normal scale (want ~2)");
      // vs_hash identifies WHICH vertex shader feeds this PS -- 10 candidate
      // vs_*.ucode.bin dumps fetch the same streams (94/95) and the reference
      // trace's `ch=` naming does not map to our XXH3 dump names, so this is
      // the only reliable way to pick the right one for the gold VS harness.
      REXLOG_INFO("[WORLDCONST] vs={:016X} bools4={:08X} tex:{}{}", d.vs_hash, d.bool_consts[4], tx,
                  s);
    }
  }
  // Identify whether a PS is world GEOMETRY or a fullscreen POST pass: a post
  // pass draws a quad (a handful of indices) into the full target, geometry
  // draws thousands. Needed to interpret PS_DEBUG=19 coverage correctly.
  static const bool s_psrole = getenv("RESTUFF_PSROLE") != nullptr;
  // RESTUFF_PSINV=1: one line per DISTINCT pixel shader with the largest draw
  // seen for it. The PS_DEBUG=19 identity map only shows who wrote the final
  // RT, so post passes hide every world shader feeding the scene buffer; this
  // inventory is how those get found (big idx = geometry, idx<=6 = fullscreen
  // quad = post/UI).
  static const bool s_psinv = getenv("RESTUFF_PSINV") != nullptr;
  if (s_psinv) {
    static std::mutex m;
    static std::map<uint64_t, uint32_t> best;
    std::lock_guard<std::mutex> lk(m);
    auto it = best.find(d.ps_hash);
    if (it == best.end() || index_count > it->second) {
      best[d.ps_hash] = index_count;
      REXLOG_INFO("[PSINV] ps={:016X} maxidx={} prim={} tex0={}x{}", d.ps_hash, index_count, prim,
                  d.tex[0].width, d.tex[0].height);
    }
  }
  if (s_psrole) {
    // PER-HASH budgets: a single shared counter is useless here -- the busiest
    // shader burns the whole allowance before the others emit a single line
    // (98E0B121 ate all 400 slots on the first run, hiding CABCC0E0 entirely).
    static std::atomic<int> s_role_a{6}, s_role_b{6}, s_role_c{6};
    std::atomic<int>* b = d.ps_hash == 0xCABCC0E0214BDEA4ull   ? &s_role_a
                          : d.ps_hash == 0x98E0B1214352B492ull ? &s_role_b
                          : d.ps_hash == 0x1BAB95FEECAC8E97ull ? &s_role_c
                                                               : nullptr;
    if (b && b->fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[PSROLE] ps={:016X} idx={} prim={} tex0={:08X} {}x{}", d.ps_hash, index_count,
                  prim, d.tex[0].phys_addr, d.tex[0].width, d.tex[0].height);
  }
  // A shader forced to emit 1.0 lands on screen at ~175/255 = 0.69. If these
  // draws are alpha-blended and the blend factors (or alpha) are wrong, that
  // attenuation IS the world-dim. Dump the guest blend state for the biggest
  // geometry draws: RB_BLENDCONTROL0 0x2201 (bits 0-4 src, 5-7 op, 8-12 dst),
  // RB_COLORCONTROL 0x2202, RB_MODECONTROL 0x2208.
  static const bool s_blend = getenv("RESTUFF_BLENDLOG") != nullptr;
  if (s_blend && index_count > 5000) {
    static std::mutex bm;
    static std::set<uint64_t> bseen;
    std::lock_guard<std::mutex> lk(bm);
    const uint32_t bc = s_reg_shadow[0x2201];
    if (bseen.insert(d.ps_hash ^ (uint64_t(bc) << 32)).second && bseen.size() <= 24)
      REXLOG_INFO("[BLEND] ps={:016X} idx={} blendctl={:08X} (src={} op={} dst={}) "
                  "colorctl={:08X} modectl={:08X}",
                  d.ps_hash, index_count, bc, bc & 0x1F, (bc >> 5) & 7, (bc >> 8) & 0x1F,
                  s_reg_shadow[0x2202], s_reg_shadow[0x2208]);
  }
  // Colour-mask-off draws are stencil/mask preparation -- invisible on hardware
  // (rendering them paints opaque rectangles that occlude everything). The
  // heuristic path skips these; the translated path must too. This is also what
  // let the fullscreen pretransformed quads (e.g. 8859F7) occlude content when
  // rect-list expansion was enabled.
  d.color_mask = s_reg_shadow[0x2104] & 0xF;
  // M3.1: colour-mask-0 draws are KEPT now -- in 3D they are the Z-PREPASS
  // (depth-only writes; colorWriteMask=0 makes them colour-invisible, exactly
  // like hardware). RESTUFF_CMASK_SKIP=1 restores the old skip for A/B.
  static const bool skip_cmask0 = getenv("RESTUFF_CMASK_SKIP") != nullptr;
  if (d.color_mask == 0) {
    CapDropLog(6, vsb->info.hash, psb->info.hash, prim, index_count);
    if (skip_cmask0) return;
  }

  const uint32_t bc = s_reg_shadow[0x2201];
  const uint32_t src_bf = bc & 0x1F, dst_bf = (bc >> 8) & 0x1F;
  if (dst_bf == 1) d.blend = rr::BlendMode::kAdditive;
  else if (src_bf == 1 && dst_bf == 7) d.blend = rr::BlendMode::kPremul;
  // M3.0: raw render state for the translated pipeline (full blend factors,
  // depth test/write, cull mode, alpha test, viewport depth range).
  d.blend_control = bc;
  d.depth_control = s_reg_shadow[0x2200];
  d.stencil_ref_mask = s_reg_shadow[0x210D];      // RB_STENCILREFMASK
  d.stencil_ref_mask_bf = s_reg_shadow[0x210C];   // RB_STENCILREFMASK_BF
  // RESTUFF_DRAWORDER: sequence-number every volume-VS draw and every
  // F5443DCC-composite draw. Answers "does the composite consume THIS frame's
  // occ (comes after the volume group) or LAST frame's (comes before)".
  static const bool s_draworder = getenv("RESTUFF_DRAWORDER") != nullptr;  // M4.2: cached
  if (s_draworder) {
    static std::atomic<uint64_t> s_seq{0};
    const uint64_t q = s_seq.fetch_add(1, std::memory_order_relaxed);
    const bool is_vol = s_current_vs_hash == 0x9E4052352C9BEB99ull ||
                        s_current_vs_hash == 0x19E09472AAC8118Dull ||
                        s_current_vs_hash == 0xEFE6B9063BDD3ECDull ||
                        s_current_vs_hash == 0x2766CBE92CD1C91Aull;  // restore quads
    const bool is_comp = s_current_ps_hash == 0xF5443DCCB449C724ull;
    if (is_vol || is_comp) {
      static std::atomic<int> s_do{60};
      if (s_do.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[DRAWORDER] seq={} {} vs={:016X} ps={:016X}", q,
                    is_comp ? "COMPOSITE" : "VOLUME", s_current_vs_hash, s_current_ps_hash);
    }
  }
  // RESTUFF_COMPQUAD: per-frame content trace of the shadow-composite draw's
  // vertex stream (PS F5443DCC). Distinguishes a CPU-reprojected quad (data
  // changes every frame -> the game compensates for the 1-frame occ lag) from
  // a static screen quad (no compensation exists -> the lag is canon).
  static const bool s_compquad = getenv("RESTUFF_COMPQUAD") != nullptr;  // M4.2: cached
  if (s_compquad && s_current_ps_hash == 0xF5443DCCB449C724ull) {
    static std::atomic<int> s_cq{16};
    if (s_cq.fetch_sub(1, std::memory_order_relaxed) > 0 && !d.streams.empty() &&
        d.streams[0].data && !d.streams[0].data->empty()) {
      const auto& sd = *d.streams[0].data;
      uint64_t h = 1469598103934665603ull;
      for (uint8_t b : sd) h = (h ^ b) * 1099511628211ull;
      float f[8] = {};
      std::memcpy(f, sd.data(), std::min(sizeof(f), sd.size()));
      REXLOG_INFO("[COMPQUAD] vs={:016X} bytes={} fnv={:016X} f0..7=({}, {}, {}, {}, {}, {}, {}, {})",
                  s_current_vs_hash, sd.size(), h, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
  }
  // RESTUFF_DUMP_VOLDRAW=<vs_hash_hex16>: once, when that VS draws, dump the
  // FULL register shadow + every active vfetch slot's raw (unswapped) guest
  // window + a manifest -- the complete input set for the offline
  // ShaderInterpreter gold harness (tools/nb_vs_gold.cpp).
  static const char* s_dump_voldraw = getenv("RESTUFF_DUMP_VOLDRAW");  // M4.2: cached
  if (const char* vdh = s_dump_voldraw) {
    // comma-separated list of vs hashes; each dumped once, files prefixed by
    // the first 8 hex digits of the hash.
    static std::mutex s_vd_mu;
    static std::set<uint64_t> s_vd_done;
    bool want = false;
    {
      std::stringstream ss(vdh);
      std::string tok;
      while (std::getline(ss, tok, ','))
        if (!tok.empty() && strtoull(tok.c_str(), nullptr, 16) == s_current_vs_hash) want = true;
    }
    char pref[32];
    snprintf(pref, sizeof(pref), "voldraw_%08llx", (unsigned long long)(s_current_vs_hash >> 32));
    bool fresh = false;
    if (want) {
      std::lock_guard<std::mutex> lk(s_vd_mu);
      fresh = s_vd_done.insert(s_current_vs_hash).second;
    }
    if (fresh) {
      char path0[96];
      snprintf(path0, sizeof(path0), "%s_regs.bin", pref);
      if (FILE* fp = fopen(path0, "wb")) {
        fwrite(s_reg_shadow, 4, 0x8000, fp);
        fclose(fp);
      }
      char pathm[96];
      snprintf(pathm, sizeof(pathm), "%s_manifest.txt", pref);
      if (FILE* mf = fopen(pathm, "w")) {
        for (uint32_t slot = 0; slot < 96; ++slot) {
          const uint32_t f0 = s_reg_shadow[0x4800 + slot * 2];
          const uint32_t f1 = s_reg_shadow[0x4800 + slot * 2 + 1];
          const uint32_t phys = f0 & ~0x3u;
          const uint32_t words = (f1 >> 2) & 0xFFFFFF;
          if (!phys || !words || words > (1u << 20)) continue;
          const uint8_t* src = memory->TranslatePhysical<const uint8_t*>(phys);
          char path[96];
          snprintf(path, sizeof(path), "%s_slot%02u.bin", pref, slot);
          if (FILE* fp = fopen(path, "wb")) {
            fwrite(src, 4, words, fp);
            fclose(fp);
          }
          fprintf(mf, "%u 0x%08X %u 0x%08X 0x%08X\n", slot, phys, words, f0, f1);
        }
        fclose(mf);
      }
      REXLOG_INFO("[native_vk] VOLDRAW dumped for {:016X}", s_current_vs_hash);
    }
  }
  // RESTUFF_VOLCLIP: log every stencil-enabled draw's full guest stencil
  // programming + clip control. Post-volume stencil shows net -1/-2 (254/255)
  // WRAP marks next to clean +1 stamps -- some draw runs DECR_WRAP on the
  // shared stencil, and its identity (surface, ops, shader) names the phantom
  // shadow's author.
  static const bool s_volclip = getenv("RESTUFF_VOLCLIP") != nullptr;  // M4.2: cached
  if (s_volclip && (d.depth_control & 1)) {
    // One line per UNIQUE stencil state (surf+dc+refmask+vs) -- a raw budget
    // drowns in the per-frame marker quad before gameplay volumes appear.
    static std::mutex s_vc_mu;
    static std::set<std::array<uint64_t, 2>> s_vc_seen;
    std::array<uint64_t, 2> key = {
        (uint64_t(s_reg_shadow[0x2000]) << 32) | d.depth_control,
        (uint64_t(s_reg_shadow[0x210D]) << 32) ^ s_current_vs_hash};
    bool fresh;
    {
      std::lock_guard<std::mutex> lk(s_vc_mu);
      fresh = s_vc_seen.size() < 64 && s_vc_seen.insert(key).second;
    }
    if (fresh) {
      const uint32_t dc = d.depth_control;
      REXLOG_INFO("[VOLCLIP] surf={:08X} dc={:08X} F(func={} fail={} zpass={} zfail={}) "
                  "B(bf={} func={} fail={} zpass={} zfail={}) refmask={:08X} clip={:08X} "
                  "z_en={} vs={:016X}",
                  s_reg_shadow[0x2000], dc, (dc >> 8) & 7, (dc >> 11) & 7, (dc >> 14) & 7,
                  (dc >> 17) & 7, (dc >> 7) & 1, (dc >> 20) & 7, (dc >> 23) & 7,
                  (dc >> 26) & 7, (dc >> 29) & 7, s_reg_shadow[0x210D], s_reg_shadow[0x2204],
                  (dc >> 1) & 1, s_current_vs_hash);
    }
  }
  d.su_mode = s_reg_shadow[0x2205];
  d.color_control = s_reg_shadow[0x2202];
  d.alpha_ref = ShadowRegF32(0x210E);
  d.vport_zscale = ShadowRegF32(0x2113);
  // The sun-shaft buffer comes out FLAT (0.27..0.38, 29 distinct values) =
  // the signature of a degenerate depth unprojection. window_z =
  // clip_z*ZSCALE + ZOFFSET, so ZSCALE=+1/ZOFFSET=0 is standard D3D depth
  // (near 0, far 1) while ZSCALE=-1/ZOFFSET=+1 is reversed. Our dumped depth
  // looks reversed (near bright); if the guest asks for STANDARD, every
  // depth-consuming shader is fed an inverted buffer.
  static const bool s_zmap = getenv("RESTUFF_ZMAP") != nullptr;  // M4.2: cached
  if (s_zmap) {
    static std::atomic<uint64_t> s_zsig{~0ull};
    const uint64_t sig = (uint64_t(s_reg_shadow[0x2113]) << 32) | s_reg_shadow[0x2114];
    if (s_zsig.exchange(sig, std::memory_order_relaxed) != sig)
      REXLOG_INFO("[ZMAP] ZSCALE={} ZOFFSET={} (raw {:08X}/{:08X}) vs={:016X} ps={:016X}",
                  ShadowRegF32(0x2113), ShadowRegF32(0x2114), s_reg_shadow[0x2113],
                  s_reg_shadow[0x2114], s_current_vs_hash, s_current_ps_hash);
  }
  d.vport_zoffset = ShadowRegF32(0x2114);

  // Rectangle-list (prim 8): 3 verts define a rect; synthesize the 4th corner
  // (v3 = v0 - v1 + v2) and expand each rect to 2 triangles. The pretransformed
  // rect-list shaders use float vertex attributes, so interpolate float-wise.
  // Safe now that unwritten PS interpolators default to 0 (the fullscreen 8859F7
  // occluder is transparent), so expanding it to full coverage is harmless.
  // Rect lists are 2D UI constructs -- only handled for single-stream draws.
  if (prim == 8 && d.streams.size() == 1 && (d.streams[0].stride % 4) == 0 &&
      index_count >= 3) {
    rr::VtxStream& s0 = d.streams[0];
    const uint32_t stride = s0.stride;
    const uint32_t fpv = stride / 4;
    const uint32_t nrect = index_count / 3;
    // Copy-on-write: the payload may be shared with other draws this frame.
    auto nvd = std::make_shared<std::vector<uint8_t>>(s0.bytes());
    std::vector<uint32_t> nidx;
    nidx.reserve(nrect * 6);
    uint32_t next = vcount;
    const auto& src_idx = d.idx();
    for (uint32_t rc = 0; rc < nrect; ++rc) {
      const uint32_t i0 = src_idx[3 * rc], i1 = src_idx[3 * rc + 1],
                     i2 = src_idx[3 * rc + 2];
      if (i0 >= vcount || i1 >= vcount || i2 >= vcount) continue;  // corrupt index: skip rect
      // Read the source vertices from the ORIGINAL payload -- nvd reallocates
      // as rects append, which would invalidate pointers into it.
      const auto* v0 = reinterpret_cast<const float*>(s0.bytes().data() + size_t(i0) * stride);
      const auto* v1 = reinterpret_cast<const float*>(s0.bytes().data() + size_t(i1) * stride);
      const auto* v2 = reinterpret_cast<const float*>(s0.bytes().data() + size_t(i2) * stride);
      std::vector<float> v3(fpv);
      for (uint32_t k = 0; k < fpv; ++k) v3[k] = v0[k] - v1[k] + v2[k];
      const uint32_t i3 = next++;
      nvd->insert(nvd->end(), reinterpret_cast<const uint8_t*>(v3.data()),
                  reinterpret_cast<const uint8_t*>(v3.data()) + stride);
      nidx.push_back(i0); nidx.push_back(i1); nidx.push_back(i2);
      nidx.push_back(i0); nidx.push_back(i2); nidx.push_back(i3);
    }
    s0.data = std::move(nvd);
    d.indices_sp = std::make_shared<std::vector<uint32_t>>(std::move(nidx));
    d.prim = 4;  // now a plain triangle list
  }

  // Quad-list (prim 0xD): each 4 indices = one quad; expand to 2 triangles.
  // Pure index rewrite (no synthesized vertices), so it works multistream.
  if (d.prim == 0xD && index_count >= 4) {
    std::vector<uint32_t> nidx;
    nidx.reserve(index_count / 4 * 6);
    const auto& src_idx = d.idx();
    for (uint32_t q = 0; q + 3 < index_count; q += 4) {
      const uint32_t i0 = src_idx[q], i1 = src_idx[q + 1], i2 = src_idx[q + 2],
                     i3 = src_idx[q + 3];
      if (i0 >= vcount || i1 >= vcount || i2 >= vcount || i3 >= vcount) continue;
      nidx.push_back(i0); nidx.push_back(i1); nidx.push_back(i2);
      nidx.push_back(i0); nidx.push_back(i2); nidx.push_back(i3);
    }
    d.indices_sp = std::make_shared<std::vector<uint32_t>>(std::move(nidx));
    d.prim = 4;
  }

  // Constant-base probe: for felt draws, scan the whole ALU constant shadow for
  // a plausible "full-width" matrix row (slope ~2.0008/16377=1.2217e-4 next to
  // an offset ~-1.0008). If found at an index != vs_base, the base association
  // is wrong and the 88%-width felt is a mis-read matrix.
  if (s_dump_draws && d.vs_hash == 0xEE34A8BA31895FACull) {
    static std::atomic<int> s_vte_budget{10};
    if (s_vte_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_INFO("[DUMP] CLIPREGS clip_cntl={:08X} su_sc={:08X} vte_cntl={:08X} vpz=({}/{}) "
                  "pgm_cntl={:08X}",
                  s_reg_shadow[0x2204], s_reg_shadow[0x2205], s_reg_shadow[0x2206],
                  ShadowRegF32(0x2113), ShadowRegF32(0x2114), s_reg_shadow[0x2180]);
    }
    static std::atomic<int> s_scan_budget{8};
    if (s_scan_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const uint32_t vs_base_dbg = s_reg_shadow[0x2307] & 0x1FF;
      REXLOG_INFO("[DUMP] CBASE vs_base={} ps_base={}", vs_base_dbg, s_reg_shadow[0x2308] & 0x1FF);
      for (uint32_t i = 0; i < 512 * 4; ++i) {
        const float f = ShadowRegF32(0x4000 + i);
        if (f > 0.000115f && f < 0.000131f) {
          // print the candidate row (this float + 3 neighbors from its vec4)
          const uint32_t vec = i / 4;
          REXLOG_INFO("[DUMP]   cand c{}[{}]: ({:.8f},{:.8f},{:.8f},{:.8f})", vec, i % 4,
                      ShadowRegF32(0x4000 + vec * 4), ShadowRegF32(0x4000 + vec * 4 + 1),
                      ShadowRegF32(0x4000 + vec * 4 + 2), ShadowRegF32(0x4000 + vec * 4 + 3));
        }
      }
    }
  }
  // Pair-diff probe: the UI draws elements twice (two panel passes); log the
  // window/scissor/viewport registers per draw so the differing register
  // between pair members can be identified.
  if (s_dump_draws &&
      (d.vs_hash == 0x346474C6087D8982ull || !getenv("RESTUFF_REGS_TEXT_ONLY"))) {
    static std::atomic<int> s_regs_budget{120};
    if (s_regs_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_INFO(
          "[DUMP] REGS vs={:016X} n={} wo={:08X} wsTL={:08X} wsBR={:08X} ssTL={:08X} ssBR={:08X} "
          "vpx={}/{} vpy={}/{} surf={:08X}",
          d.vs_hash, index_count, s_reg_shadow[0x2080], s_reg_shadow[0x2081],
          s_reg_shadow[0x2082], s_reg_shadow[0x200E], s_reg_shadow[0x200F],
          ShadowRegF32(0x210F), ShadowRegF32(0x2110), ShadowRegF32(0x2111),
          ShadowRegF32(0x2112), s_reg_shadow[0x2000]);
    }
  }
  // M4.17 (RESTUFF_DECAL_SCAN=<ps hex>): per-triangle microscope for ONE
  // draw family -- built for the blob-shadow decal pass (BBA590486A51E72A),
  // whose CPU-generated triangles occasionally rasterize as the Ep5 dock's
  // black flicker. Positions are CPU-final by the time they reach this
  // capture, so an anomalous triangle logged HERE is direct evidence of the
  // guest computation going wrong, and the raw values say HOW: 1-ULP-shaped
  // wrongness = FP rounding; wild garbage = uninitialized/OOB read.
  // Logs: hard flags (non-finite / |coord|>1e6) immediately; plus periodic
  // [DECAL] STATS with the largest-area triangle since last report (the
  // flicker triangle is large on screen -- it will dominate max_area on a
  // flicker frame), including all three vertices' full attribute dwords.
  {
    static const uint64_t s_decal_ps = [] {
      const char* e = getenv("RESTUFF_DECAL_SCAN");
      return e ? strtoull(e, nullptr, 16) : 0ull;
    }();
    if (s_decal_ps && d.ps_hash == s_decal_ps && !d.streams.empty() &&
        d.streams[0].data && !d.idx().empty()) {
      const auto& sb = *d.streams[0].data;
      const uint32_t stride = d.streams[0].stride;
      const auto& idx = d.idx();
      static bool s_layout_logged = false;
      if (!s_layout_logged) {
        s_layout_logged = true;
        char lay[256] = {};
        int lp = 0;
        for (const auto& na : vs.t.attrs)
          lp += std::snprintf(lay + lp, sizeof(lay) - lp, " (slot=%u off=%u fmt=%u)",
                              na.fetch_slot, na.byte_offset, na.format);
        REXLOG_INFO("[DECAL] layout stride={} n_attrs={} vs={:016X}:{}", stride,
                    uint32_t(vs.t.attrs.size()), d.vs_hash, lay);
      }
      static float s_max_area = -1.0f;
      static uint32_t s_max_tri[3] = {};
      static std::vector<uint8_t> s_max_verts;
      static uint64_t s_max_frame = 0;
      static uint32_t s_frames_seen = 0;
      auto vpos = [&](uint32_t vi, float* out) {
        const size_t off = size_t(vi) * stride;
        if (off + 8 > sb.size()) { out[0] = out[1] = 0; return false; }
        std::memcpy(out, sb.data() + off, 8);  // x, y (CPU-final)
        return true;
      };
      static std::atomic<int> s_flag_budget{24};
      const size_t tstep = (d.prim == 6) ? 1 : 3;  // strip: sliding window
      for (size_t t = 0; t + 2 < idx.size(); t += tstep) {
        const uint32_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
        if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
        float pa[2], pb[2], pc[2];
        if (!vpos(a, pa) || !vpos(b, pb) || !vpos(c, pc)) continue;
        bool hard = false;
        for (const float* p : {pa, pb, pc})
          if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || std::fabs(p[0]) > 1e6f ||
              std::fabs(p[1]) > 1e6f)
            hard = true;
        const float area = std::fabs((pb[0] - pa[0]) * (pc[1] - pa[1]) -
                                     (pc[0] - pa[0]) * (pb[1] - pa[1])) * 0.5f;
        if (hard && s_flag_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[DECAL] HARDFLAG tri=({},{},{}) p=({:.3g},{:.3g})({:.3g},{:.3g})"
                      "({:.3g},{:.3g}) frame={}",
                      a, b, c, pa[0], pa[1], pb[0], pb[1], pc[0], pc[1],
                      s_frame_serial.load(std::memory_order_relaxed));
        if (area > s_max_area) {
          s_max_area = area;
          s_max_tri[0] = a; s_max_tri[1] = b; s_max_tri[2] = c;
          s_max_frame = s_frame_serial.load(std::memory_order_relaxed);
          s_max_verts.clear();
          for (uint32_t vi : {a, b, c}) {
            const size_t off = size_t(vi) * stride;
            if (off + stride <= sb.size())
              s_max_verts.insert(s_max_verts.end(), sb.data() + off, sb.data() + off + stride);
          }
        }
      }
      if (++s_frames_seen % 300 == 0 && s_max_area >= 0.0f) {
        char vd[512] = {};
        int vp = 0;
        for (size_t w = 0; w + 3 < s_max_verts.size() && vp < int(sizeof(vd)) - 12; w += 4) {
          uint32_t u;
          std::memcpy(&u, s_max_verts.data() + w, 4);
          vp += std::snprintf(vd + vp, sizeof(vd) - vp, "%s%08X",
                              (w % stride) == 0 ? " | " : " ", u);
        }
        REXLOG_INFO("[DECAL] STATS frames={} max_area={:.6g} tri=({},{},{}) frame={} verts:{}",
                    s_frames_seen, s_max_area, s_max_tri[0], s_max_tri[1], s_max_tri[2],
                    s_max_frame, vd);
        s_max_area = -1.0f;
      }
    }
  }
  s_cap_out.fetch_add(1, std::memory_order_relaxed);
  if (d.prim == 1) s_cap_points_out.fetch_add(1, std::memory_order_relaxed);  // M3.137
  static const bool s_dump_pm4seq = getenv("RESTUFF_DUMP_PM4SEQ") != nullptr;  // M3.45
  if (s_dump_pm4seq && d.tex[0].phys_addr == 0x06004000) {
    static std::atomic<int> s_seqdump{2};
    if (s_seqdump.fetch_sub(1, std::memory_order_relaxed) > 0) DumpPm4Seq("felt draw");
  }
  rr::SubmitRawDraw(std::move(d));
}

void CapturePm4Draw(uint32_t initiator, uint32_t idx_base_phys) {
  namespace rr = restuff::renderer;
  ++s_draw_serial;
  if (REXCVAR_GET(use_translated_shaders)) {
    CaptureTranslatedDraw(initiator, idx_base_phys);
    return;
  }
  const uint32_t prim = initiator & 0x3F;
  const uint32_t index_count = initiator >> 16;
  s_prim_hist[prim & 0xF].fetch_add(1, std::memory_order_relaxed);
  if ((prim != 4 && prim != 5 && prim != 6 && prim != 8 && prim != 0xD) ||
      index_count == 0 || index_count > 0x10000) {
    s_drop_prim.fetch_add(1, std::memory_order_relaxed);
    return;  // trilist/fan/strip only
  }
  const Pm4VsDecl* decl = nullptr;
  for (const auto& d : kPm4Decls) {
    if (d.vs_hash == s_current_vs_hash) {
      decl = &d;
      break;
    }
  }
  if (!decl) {
    static std::atomic<int> s_unk{40};
    if (s_unk.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const uint32_t f0 = s_reg_shadow[0x4800 + 95 * 2];
      REXLOG_INFO(
          "[native_vk] pm4 UNMAPPED vs={:016X} ps={:016X} prim={} n={} fetch95=0x{:08X} colormask=0x{:X}",
          s_current_vs_hash, s_current_ps_hash, prim, index_count, f0,
          s_reg_shadow[0x2104] & 0xF);
    }
    s_drop_nodecl.fetch_add(1, std::memory_order_relaxed);
    MaybeLogDrops();
    return;
  }

  // Colour-mask-off draws are stencil/mask preparation — invisible on
  // hardware; skip (rendering them paints opaque rectangles).
  if ((s_reg_shadow[0x2104] & 0xF) == 0) {
    s_drop_mask.fetch_add(1, std::memory_order_relaxed);
    MaybeLogDrops();
    return;
  }
  // Blend mode from RB_BLENDCONTROL0 (draw-synchronized shadow).
  const uint32_t bc = s_reg_shadow[0x2201];
  const uint32_t src_bf = bc & 0x1F, dst_bf = (bc >> 8) & 0x1F;
  restuff::renderer::BlendMode blend = restuff::renderer::BlendMode::kStandard;
  if (dst_bf == 1) {  // dest ONE
    blend = restuff::renderer::BlendMode::kAdditive;
  } else if (src_bf == 1 && dst_bf == 7) {  // ONE / invSrcAlpha
    blend = restuff::renderer::BlendMode::kPremul;
  }

  auto* memory = rex::Runtime::instance()->memory();

  // Vertex buffer from the VS's vertex fetch constant (2 words per constant).
  const uint32_t f0 = s_reg_shadow[0x4800 + decl->fetch_slot * 2];
  const uint32_t f1 = s_reg_shadow[0x4800 + decl->fetch_slot * 2 + 1];
  const uint32_t vb_phys = f0 & ~0x3u;
  const uint32_t vb_words = (f1 >> 2) & 0xFFFFFF;
  const uint32_t vb_endian = f1 & 0x3;
  if (!vb_phys || vb_words < decl->stride_words) {
    s_drop_novb.fetch_add(1, std::memory_order_relaxed);
    MaybeLogDrops();
    return;
  }

  // Indices: BE u16 DMA buffer, or sequential when auto-indexed.
  std::vector<uint16_t> idx(index_count);
  if (idx_base_phys) {
    const uint16_t* src = memory->TranslatePhysical<const uint16_t*>(idx_base_phys);
    for (uint32_t i = 0; i < index_count; ++i) {
      idx[i] = std::byteswap(src[i]);
    }
  } else {
    for (uint32_t i = 0; i < index_count; ++i) {
      idx[i] = uint16_t(i);
    }
  }
  uint32_t max_index = 0;
  for (uint16_t i : idx) max_index = std::max<uint32_t>(max_index, i);
  const uint32_t need_words = (max_index + 1) * decl->stride_words;
  if (need_words > vb_words) {
    s_drop_needwords.fetch_add(1, std::memory_order_relaxed);
    MaybeLogDrops();
    return;  // stale fetch constant for this draw
  }

  // Vertex words, endian-normalized per the (draw-synchronized) fetch constant.
  const uint32_t* vb_host = memory->TranslatePhysical<const uint32_t*>(vb_phys);
  std::vector<uint32_t> words(vb_host, vb_host + need_words);
  if (vb_endian != 2) {  // 2 = k8in32: already correct on LE host
    for (auto& w : words) w = std::byteswap(w);
  }

  // Transform + UV rows from the ALU constant shadow (c0..c5). VS constants are
  // relative to the SQ_VS_CONST (0x2307) base [0:8] in vec4 units — draws with a
  // nonzero base were reading the wrong matrix (mis-scaled/placed elements).
  const uint32_t vs_base = s_reg_shadow[0x2307] & 0x1FF;
  float rows[6][4];
  for (int r = 0; r < 6; ++r) {
    for (int i = 0; i < 4; ++i) {
      rows[r][i] = ShadowRegF32(0x4000 + (vs_base + r) * 4 + i);
    }
  }
  const bool rows_valid =
      std::isfinite(rows[0][0]) && std::isfinite(rows[1][0]) &&
      (rows[0][0] != 0.0f || rows[0][1] != 0.0f) && (rows[1][0] != 0.0f || rows[1][1] != 0.0f);

  // PS cxform constants: the flat family's PS (7897) computes out = color*c2 + c3
  // from the pixel-shader ALU constants. SQ_PS_CONST (0x2308) bits[0:8] give the
  // base into the shared 0x4000 float bank (vec4 units).
  const uint32_t ps_base = s_reg_shadow[0x2308] & 0x1FF;
  float cx_mul[4] = {1, 1, 1, 1}, cx_add[4] = {0, 0, 0, 0};
  bool cxform = false;
  if (decl->family == rr::DrawFamily::kQuad && decl->color2_word < 0) {
    for (int i = 0; i < 4; ++i) {
      cx_mul[i] = ShadowRegF32(0x4000 + (ps_base + 2) * 4 + i);
      cx_add[i] = ShadowRegF32(0x4000 + (ps_base + 3) * 4 + i);
    }
    // ps_base/cxform read is unverified (constant base may differ); disabled
    // until confirmed to avoid whiting-out flat quads with garbage constants.
    cxform = false && std::isfinite(cx_mul[0]) && std::isfinite(cx_add[0]);
  }

  // Bound texture from texture fetch constant slot 0 (both UI PS's tfetch
  // fetchconst=0). Texture constants are 6-word records.
  rr::GuestTextureDesc tex;
  {
    const uint32_t w0 = s_reg_shadow[0x4800 + 0];
    const uint32_t w1 = s_reg_shadow[0x4800 + 1];
    const uint32_t w2 = s_reg_shadow[0x4800 + 2];
    // Same bit layout as the staged device records (see guest_d3d_hooks.cpp):
    const uint32_t base_address = (w1 >> 12) & 0xFFFFF;
    const uint32_t width = (w2 & 0x1FFF) + 1;
    const uint32_t height = ((w2 >> 13) & 0x1FFF) + 1;
    if (base_address && width > 1) {
      tex.valid = true;
      tex.phys_addr = base_address << 12;
      tex.width = width;
      tex.height = height;
      const uint32_t pitch = (w0 >> 22) & 0x1FF;
      tex.pitch_texels = pitch ? (pitch << 5) : width;
      tex.format = w1 & 0x3F;
      tex.endian = (w1 >> 6) & 0x3;
      tex.tiled = (w0 >> 31) & 1;
      tex.clamp_x = (w0 >> 10) & 7;
      tex.clamp_y = (w0 >> 13) & 7;
    }
  }

  rr::DecodedDraw out;
  out.family = decl->family;
  out.blend = blend;
  out.tex = tex;
  out.verts.reserve(index_count);

  auto decode_vert = [&](uint32_t vi, rr::Draw2DVertex& o) -> bool {
    const uint32_t* v = words.data() + size_t(vi) * decl->stride_words;
    float x, y;
    float pos4[3] = {0, 0, 0};
    if (decl->pretransformed) {
      float pf[2];
      std::memcpy(pf, v, 8);
      pos4[0] = pf[0];
      pos4[1] = pf[1];
      x = pf[0];  // already clip-space
      y = pf[1];
    } else if (decl->pos_k16_16) {
      const uint32_t w = v[0];
      const float sx = float(int16_t(w >> 16));
      const float sy = float(int16_t(w & 0xFFFF));
      pos4[0] = sx;
      pos4[1] = sy;
      if (!rows_valid) return false;
      x = sx * rows[0][0] + sy * rows[0][1] + rows[0][3];
      y = sx * rows[1][0] + sy * rows[1][1] + rows[1][3];
    } else {
      float pf[3];
      std::memcpy(pf, v, 12);
      pos4[0] = pf[0];
      pos4[1] = pf[1];
      pos4[2] = pf[2];
      if (!rows_valid) return false;
      x = pf[0] * rows[0][0] + pf[1] * rows[0][1] + pf[2] * rows[0][2] + rows[0][3];
      y = pf[0] * rows[1][0] + pf[1] * rows[1][1] + pf[2] * rows[1][2] + rows[1][3];
    }
    if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 4096.f ||
        std::fabs(y) > 4096.f) {
      return false;  // non-finite / absurd only; the GPU clips [-1,1]
    }
    float u = 0, vv = 0;
    if (decl->uv_via_consts) {
      u = pos4[0] * rows[4][0] + pos4[1] * rows[4][1] + pos4[2] * rows[4][2] + rows[4][3];
      vv = pos4[0] * rows[5][0] + pos4[1] * rows[5][1] + pos4[2] * rows[5][2] + rows[5][3];
    } else if (decl->uv_word >= 0) {
      std::memcpy(&u, v + decl->uv_word, 4);
      std::memcpy(&vv, v + decl->uv_word + 1, 4);
      if (!std::isfinite(u) || !std::isfinite(vv)) {
        u = vv = 0;
      }
    }
    auto repack = [](uint32_t c) {
      const uint32_t a = (c >> 24) & 0xFF, r = (c >> 16) & 0xFF;
      const uint32_t g = (c >> 8) & 0xFF, b = c & 0xFF;
      return r | (g << 8) | (b << 16) | (a << 24);
    };
    uint32_t rgba = 0xFFFFFFFFu, rgba2 = 0xFFFFFFFFu;
    if (decl->color1_word >= 0) rgba = repack(v[decl->color1_word]);
    if (decl->color2_word >= 0) {
      rgba2 = repack(v[decl->color2_word]);
    } else {
      // Flat family: fold the PS cxform (out = color*c2 + c3) into color1 and
      // render with color2.r=0 (pure colour1), color2.a=1 (opaque).
      rgba2 = 0xFF000000u;
      if (cxform && decl->color1_word >= 0) {
        const uint32_t c = v[decl->color1_word];
        float ch[4] = {((c >> 16) & 0xFF) / 255.f, ((c >> 8) & 0xFF) / 255.f,
                       (c & 0xFF) / 255.f, ((c >> 24) & 0xFF) / 255.f};
        uint32_t out_bytes[4];
        for (int i = 0; i < 4; ++i) {
          float f = ch[i] * cx_mul[i] + cx_add[i];
          f = f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
          out_bytes[i] = uint32_t(f * 255.f + 0.5f);
        }
        rgba = out_bytes[0] | (out_bytes[1] << 8) | (out_bytes[2] << 16) | (out_bytes[3] << 24);
      }
    }
    o = {x, y, u, vv, rgba, rgba2};
    return true;
  };

  rr::Draw2DVertex tri[3];
  auto emit_tri = [&](uint16_t a, uint16_t b, uint16_t c) {
    if (decode_vert(a, tri[0]) && decode_vert(b, tri[1]) && decode_vert(c, tri[2])) {
      out.verts.push_back(tri[0]);
      out.verts.push_back(tri[1]);
      out.verts.push_back(tri[2]);
    }  // skip bad triangles, keep the rest of the mesh
  };
  // Emit already-decoded vertex objects (for synthesized rect/quad corners).
  auto push3 = [&](const rr::Draw2DVertex& a, const rr::Draw2DVertex& b,
                   const rr::Draw2DVertex& c) {
    out.verts.push_back(a);
    out.verts.push_back(b);
    out.verts.push_back(c);
  };
  if (prim == 4) {  // triangle list
    for (size_t i = 0; i + 2 < idx.size(); i += 3) emit_tri(idx[i], idx[i + 1], idx[i + 2]);
  } else if (prim == 5) {  // triangle fan
    for (size_t i = 1; i + 1 < idx.size(); ++i) emit_tri(idx[0], idx[i], idx[i + 1]);
  } else if (prim == 6) {  // triangle strip
    for (size_t i = 0; i + 2 < idx.size(); ++i) emit_tri(idx[i], idx[i + 1], idx[i + 2]);
  } else if (prim == 8) {  // rectangle list: 3 verts/rect, 4th = v0 - v1 + v2
    rr::Draw2DVertex a, b, c, d;
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
      if (!decode_vert(idx[i], a) || !decode_vert(idx[i + 1], b) ||
          !decode_vert(idx[i + 2], c)) {
        continue;
      }
      d.x = a.x - b.x + c.x;
      d.y = a.y - b.y + c.y;
      d.u = a.u - b.u + c.u;
      d.v = a.v - b.v + c.v;
      d.rgba = a.rgba;
      d.rgba2 = a.rgba2;
      push3(a, b, c);
      push3(a, c, d);
    }
  } else if (prim == 0xD) {  // quad list: 4 verts/quad
    rr::Draw2DVertex q[4];
    for (size_t i = 0; i + 3 < idx.size(); i += 4) {
      if (!decode_vert(idx[i], q[0]) || !decode_vert(idx[i + 1], q[1]) ||
          !decode_vert(idx[i + 2], q[2]) || !decode_vert(idx[i + 3], q[3])) {
        continue;
      }
      push3(q[0], q[1], q[2]);
      push3(q[0], q[2], q[3]);
    }
  }
  if (out.verts.empty()) {
    s_drop_empty.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  s_cap_ok.fetch_add(1, std::memory_order_relaxed);
  MaybeLogDrops();
  // [UIRT] tilt hunt: any DECODED (2D/UI path) draw sampling a resolve-target
  // address is a candidate for the rotor that paints the world into the HUD
  // frame TRANSPOSED (the translated post chain is proven identity). Log its
  // computed clip positions + UVs: an off-diagonal position->uv mapping here
  // is the whole tilt+mirror story.
  if (tex.valid && tex.phys_addr >= 0x05000000u && tex.phys_addr < 0x0C000000u &&
      tex.width >= 256 && out.verts.size() >= 3) {
    static std::atomic<int> s_uirt{16};
    if (s_uirt.fetch_sub(1, std::memory_order_relaxed) > 0) {
      const auto& a = out.verts[0];
      const auto& b2 = out.verts[1];
      const auto& c2 = out.verts[2];
      REXLOG_INFO("[UIRT] vs={:016X} tex=0x{:08X} {}x{} fam={} n={} "
                  "v0=({:.3f},{:.3f} uv {:.3f},{:.3f}) v1=({:.3f},{:.3f} uv {:.3f},{:.3f}) "
                  "v2=({:.3f},{:.3f} uv {:.3f},{:.3f})",
                  s_current_vs_hash, tex.phys_addr, tex.width, tex.height, int(out.family),
                  uint32_t(out.verts.size()), a.x, a.y, a.u, a.v, b2.x, b2.y, b2.u, b2.v, c2.x,
                  c2.y, c2.u, c2.v);
    }
  }
  // Full-screen census: dump the first few draws of a settled frame with their
  // extent + fetch/vs so we can find the missing sky backdrop.
  {
    static std::atomic<int> s_pm4census{0};
    const uint64_t sw = B().pm4_swaps.load(std::memory_order_relaxed);
    if (false) {
      float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
      for (const auto& vtx : out.verts) {
        bx0 = std::min(bx0, vtx.x); bx1 = std::max(bx1, vtx.x);
        by0 = std::min(by0, vtx.y); by1 = std::max(by1, vtx.y);
      }
      REXLOG_INFO(
          "[native_vk] PM4CENSUS vs={:016X} ps={:016X} fam={} blend={} n={} bbox=({:.2f},{:.2f})..({:.2f},{:.2f}) tex={} fmt={} {}x{} v0rgba={:08X} v0rgba2={:08X} cx={} c2=({:.2f},{:.2f},{:.2f},{:.2f}) c3=({:.2f},{:.2f},{:.2f},{:.2f})",
          s_current_vs_hash, s_current_ps_hash, uint32_t(decl->family), uint32_t(blend),
          out.verts.size() / 3, bx0, by0, bx1, by1, tex.valid, tex.format, tex.width,
          tex.height, out.verts[0].rgba, out.verts[0].rgba2, cxform, cx_mul[0], cx_mul[1],
          cx_mul[2], cx_mul[3], cx_add[0], cx_add[1], cx_add[2], cx_add[3]);
    }
  }
  B().pm4_draws_captured.fetch_add(1, std::memory_order_relaxed);
  rr::SubmitDecodedDraw(std::move(out));
}

// --- Stream helpers (NB:1389-1404) ------------------------------------------

uint32_t FrameEnd(const StreamFrame& frame) {
  return frame.is_ring ? B().ring_wptr_words : frame.size_words;
}

uint32_t ReadWord(StreamFrame& frame) {
  const uint32_t* host;
  if (frame.owned) {
    host = frame.owned->data();
  } else {
    auto* memory = rex::Runtime::instance()->memory();
    host = memory->TranslatePhysical<const uint32_t*>(frame.base_phys);
  }
  const uint32_t value = std::byteswap(host[frame.pos]);
  frame.pos = frame.is_ring ? ((frame.pos + 1) & (frame.size_words - 1)) : (frame.pos + 1);
  return value;
}

void SkipWords(StreamFrame& frame, uint32_t count) {
  frame.pos =
      frame.is_ring ? ((frame.pos + count) & (frame.size_words - 1)) : (frame.pos + count);
}

// --- Shadow register file (NB:1406-1457) ------------------------------------

static void HandleGammaRampWrite(uint32_t r, uint32_t value);  // M3.100 (defined below)

void WriteShadowRegister(uint32_t reg, uint32_t value) {
  reg &= 0x7FFF;
  const uint32_t previous = s_reg_shadow[reg];
  s_reg_shadow[reg] = value;
  if ((reg - 0x4000u) < 0x800u || (reg - 0x4900u) < 0x28u) ++s_alu_const_gen;  // M4.2
  // M3.9x WORLD-DIM: watch an arbitrary shadow-register window (dword regs) to
  // see whether the guest EVER writes the shaft's "disabled" (zero) constants
  // that the reference has. RESTUFF_CWATCH_LO/HI=<0xhex reg>.
  {
    static const uint32_t s_cw_lo = [] {
      const char* e = getenv("RESTUFF_CWATCH_LO");
      return e ? uint32_t(strtoul(e, nullptr, 0)) : 0u;
    }();
    static const uint32_t s_cw_hi = [] {
      const char* e = getenv("RESTUFF_CWATCH_HI");
      return e ? uint32_t(strtoul(e, nullptr, 0)) : 0u;
    }();
    if (s_cw_hi && reg >= s_cw_lo && reg <= s_cw_hi) {
      s_cw_count.fetch_add(1, std::memory_order_relaxed);
      if (reg - s_cw_lo < 8) s_cw_last[reg - s_cw_lo].store(value, std::memory_order_relaxed);
      if (value != previous) {
        static std::atomic<int> s_cwb{400};
        if (s_cwb.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[CWATCH] reg=0x{:04X} {:08X}->{:08X} (f {:.5f} -> {:.5f})", reg, previous,
                      value, std::bit_cast<float>(previous), std::bit_cast<float>(value));
      }
    }
  }
  // RESTUFF_CI_TRACE=1: every RB_COLOR_INFO (0x2001) transition with the draw
  // serial -- does the walker ever see the 0D8 (glow/post) base, and does it
  // arrive BEFORE the glow draw or late? (glow-on-main routing diagnosis)
  static const bool s_ci_trace = getenv("RESTUFF_CI_TRACE") != nullptr;
  if (s_ci_trace && reg == 0x2001 && value != previous) {
    static std::atomic<int> s_cib{200};
    if (s_cib.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[CITRACE] ci {:08X} -> {:08X} at draw_serial={}", previous, value,
                  s_draw_serial);
  }
  if (reg >= kRegScratchReg0 && reg <= kRegScratchReg7) {
    // The kernel never programs SCRATCH_UMSK in this mode: ignore the mask and
    // write back whenever the writeback area is known. The game's flip
    // protocol passes its ISR handler/arg through slots 4/5 this way.
    const uint32_t scratch_reg = reg - kRegScratchReg0;
    const uint32_t scratch_addr = s_reg_shadow[kRegScratchAddr];
    static std::atomic<int> s_scratch_log_budget{16};
    if (s_scratch_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_INFO("[native_vk] scratch write reg{}=0x{:08X} (addr=0x{:08X})", scratch_reg,
                  value, scratch_addr);
    }
    if (scratch_addr) {
      auto* memory = rex::Runtime::instance()->memory();
      *memory->TranslatePhysical<uint32_t*>(scratch_addr + scratch_reg * 4) =
          std::byteswap(value);
    }
  } else if (reg == kRegScratchAddr) {
    // The game zeroes the scratch config during D3D device resets; the kernel
    // path that would re-program it (VdInitializeEngines) is a stub — latch.
    if (value == 0 && previous != 0) {
      s_reg_shadow[kRegScratchAddr] = previous;
    }
  } else if (reg == kRegCoherStatusHost) {
    s_reg_shadow[reg] = value | 0x80000000u;
  }
}

uint32_t ReadShadowRegister(uint32_t reg) {
  reg &= 0x7FFF;
  if (reg == kRegCoherStatusHost) {
    // Coherency is instantaneous for us: acknowledge by clearing.
    s_reg_shadow[reg] = 0;
    return 0;
  }
  return s_reg_shadow[reg];
}

// --- WAIT_REG_MEM evaluation (NB:1459-1479) ----------------------------------

// M3.35 park-latency telemetry (walker thread writes; pump thread reads).
static int64_t s_park_since = 0;  // steady_clock ns; 0 = not parked
static std::atomic<int64_t> s_park_total_us{0}, s_park_max_us{0};
static std::atomic<uint64_t> s_park_count{0};
// M3.47: pump re-evals of a parked wait -- unmet (genuine wait) vs cleared
// (detection latency the fast-poll recovered).
static std::atomic<uint64_t> s_park_reeval_unmet{0}, s_park_reeval_cleared{0};

// M3.49 fence probe: the guest render thread parks ~68% of the frame on
// WAIT_REG_MEM against the GPU fence pair at 0x1FCA3000/4 (the poll addrs
// 0x1FCA3002/6 carry endian bits in the low 2). Log every WRITE we make into
// that range (which packet, when) so we can see whether OUR walker signals the
// fence promptly (like the xenos plugin, 60fps) or leaves the guest waiting on
// a fallback interrupt (~2ms, 20fps). RESTUFF_FENCE_TRACE=1.
inline void MaybeLogFence(uint32_t addr, uint32_t val, const char* via) {
  static const bool trace = getenv("RESTUFF_FENCE_TRACE") != nullptr;
  if (!trace) return;
  const uint32_t a = addr & ~0x3u;
  if (a < 0x1FCA3000u || a > 0x1FCA30FFu) return;
  static std::atomic<int> s_budget{400};
  if (s_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
    REXLOG_INFO("[FENCE] write 0x{:08X} = 0x{:08X} via {}", a, val, via);
}

bool EvaluateWaitRegMem(uint32_t wait_info, uint32_t poll_addr, uint32_t ref, uint32_t mask) {
  uint32_t value;
  if (wait_info & 0x10) {
    // Memory poll: raw load, then GpuSwap by addr low bits.
    auto* memory = rex::Runtime::instance()->memory();
    value = *memory->TranslatePhysical<const uint32_t*>(poll_addr & ~0x3u);
    value = xenos::GpuSwap(value, static_cast<xenos::Endian>(poll_addr & 0x3));
  } else {
    value = ReadShadowRegister(poll_addr);
  }
  switch (wait_info & 0x7) {
    case 0x0: return false;
    case 0x1: return (value & mask) < ref;
    case 0x2: return (value & mask) <= ref;
    case 0x3: return (value & mask) == ref;
    case 0x4: return (value & mask) != ref;
    case 0x5: return (value & mask) >= ref;
    case 0x6: return (value & mask) > ref;
    default:  return true;
  }
}

// --- PM4 type-3 (NB:1798-2095, draws/shader-loads reduced to skips) ----------

// RESTUFF_DUMP_PM4SEQ: rolling log of every packet the walker dispatches
// (header word). Dumped when a felt-texture draw is captured, so the walked
// sequence can be diffed against the emulated trace's packet listing.
struct Pm4SeqRing {
  static constexpr uint32_t kN = 4096;
  uint32_t words[kN];
  std::atomic<uint32_t> pos{0};
  void Push(uint32_t w) { words[pos.fetch_add(1, std::memory_order_relaxed) % kN] = w; }
};
static Pm4SeqRing s_pm4seq;
void DumpPm4Seq(const char* why) {
  const uint32_t end = s_pm4seq.pos.load(std::memory_order_relaxed);
  std::string line;
  int n = 0;
  const uint32_t count = std::min<uint32_t>(end, 1500);
  for (uint32_t i = end - count; i != end; ++i) {
    line += fmt::format("{:08X} ", s_pm4seq.words[i % Pm4SeqRing::kN]);
    if (++n % 16 == 0) line += "\n";
  }
  REXLOG_INFO("[DUMP] PM4SEQ ({}, last {}):\n{}", why, count, line);
}

// Bin predication state (PM4_SET_BIN_MASK/SELECT): type-3 packets with the
// predicate bit (header bit 0) execute ONLY when (bin_select & bin_mask) != 0.
// The game emits per-variant predicated groups (constants + draw); executing
// every variant let later SET_CONSTANTs overwrite the selected one -- backdrop
// neighbor cells landed off-screen and wordmark/text quads parked off-canvas.
// Semantics identical to the SDK CommandProcessor (command_processor.cpp:864).
static uint64_t s_bin_select = 0xFFFFFFFFull;
static uint64_t s_bin_mask = 0xFFFFFFFFull;

// RESTUFF_IB_CRC probe state (walker thread only): parallel stack of pushed
// IB hashes, verified when the IB fully parses (pop). Head+tail words only --
// full-buffer hashing of world IBs would dominate the walker.
struct IbCrcRec {
  uint32_t base, len;
  uint64_t hash;
};
static std::vector<IbCrcRec> s_ib_crc;
static const bool s_ib_crc_on = getenv("RESTUFF_IB_CRC") != nullptr;
static uint64_t IbProbeHash(uint32_t base_phys, uint32_t words) {
  auto* mem = rex::Runtime::instance()->memory();
  const uint32_t* p = mem->TranslatePhysical<const uint32_t*>(base_phys);
  if (!p) return 0;
  uint64_t h = 1469598103934665603ull;
  const uint32_t head = std::min(words, 256u);
  for (uint32_t i = 0; i < head; ++i) h = (h ^ p[i]) * 1099511628211ull;
  if (words > 512) {
    const uint32_t* t = p + words - 256;
    for (uint32_t i = 0; i < 256; ++i) h = (h ^ t[i]) * 1099511628211ull;
  }
  return h;
}

bool ExecutePm4Type3(StreamFrame& frame, uint32_t packet, uint32_t header_pos) {
  auto& b = B();
  auto* memory = rex::Runtime::instance()->memory();

  const uint32_t opcode = (packet >> 8) & 0x7F;
  const uint32_t count = ((packet >> 16) & 0x3FFF) + 1;

  // Hills tracer: whenever the CURRENT tex fetch 0 points at the hills strip,
  // log every type-3 packet the walker dispatches (opcode + predication
  // outcome) -- pinpoints where the 0612C000 draw is lost.
  if (s_dump_draws && ((s_reg_shadow[0x4801] >> 12) << 12) == 0x0612C000) {
    static std::atomic<int> s_ht{60};
    if (s_ht.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[DUMP] HILLS-PKT op={:02X} pred={} pass={} select={:016X} mask={:016X}",
                  opcode, packet & 1, (s_bin_select & s_bin_mask) != 0, s_bin_select, s_bin_mask);
  }
  if (packet & 1) {
    // M3.160 DIAGNOSTIC (RESTUFF_NO_PRED=1): execute predicated packets
    // unconditionally. The wedge boots submit ~2x the draws (unbatched) and
    // ~240 fewer triangles -- a handful of small chunks never reach us, and
    // bin predication is the only path that legitimately drops packets. If
    // the wedge disappears with this on while the draw count stays high, our
    // bin state (these persistent globals) is eating real chunks (task #24).
    static const bool s_no_pred = getenv("RESTUFF_NO_PRED") != nullptr;
    const bool any_pass = s_no_pred || (s_bin_select & s_bin_mask) != 0;
    // M3.161 (RESTUFF_PREDSTAT=1): lightweight predication census on the FAST
    // path (RESTUFF_NO_PRED slows the game so much the replay never reaches
    // the test viewpoint, so measure instead of override). Reports how many
    // predicated packets were skipped and the live bin state -- diffing the
    // two boot outcomes says whether the wedge boots predicate MORE away.
    {
      static const bool s_predstat = getenv("RESTUFF_PREDSTAT") != nullptr;
      if (s_predstat) {
        static std::atomic<uint64_t> s_seen{0}, s_skip{0};
        const uint64_t n = s_seen.fetch_add(1, std::memory_order_relaxed) + 1;
        if (!any_pass) s_skip.fetch_add(1, std::memory_order_relaxed);
        if ((n % 50000) == 0)
          REXLOG_INFO("[PREDSTAT] seen={} skipped={} select={:016X} mask={:016X}", n,
                      s_skip.load(std::memory_order_relaxed), s_bin_select, s_bin_mask);
      }
    }
    if (s_dump_draws) {
      static std::atomic<uint64_t> s_pred_seen{0}, s_pred_skip{0};
      const uint64_t seen = s_pred_seen.fetch_add(1, std::memory_order_relaxed) + 1;
      if (!any_pass) s_pred_skip.fetch_add(1, std::memory_order_relaxed);
      if ((seen % 20000) == 1)
        REXLOG_INFO("[DUMP] PRED-CENSUS seen={} skipped={} op={:02X} select={:016X} mask={:016X}",
                    seen, s_pred_skip.load(std::memory_order_relaxed), opcode, s_bin_select,
                    s_bin_mask);
    }
    if (!any_pass || opcode == xenos::PM4_XE_SWAP) {
      SkipWords(frame, count);
      return true;
    }
  }

  switch (opcode) {
    case xenos::PM4_SET_BIN_MASK_LO:
      s_bin_mask = (s_bin_mask & 0xFFFFFFFF00000000ull) | ReadWord(frame);
      break;
    case xenos::PM4_SET_BIN_MASK_HI:
      s_bin_mask = (s_bin_mask & 0xFFFFFFFFull) | (uint64_t(ReadWord(frame)) << 32);
      break;
    case xenos::PM4_SET_BIN_SELECT_LO:
      s_bin_select = (s_bin_select & 0xFFFFFFFF00000000ull) | ReadWord(frame);
      break;
    case xenos::PM4_SET_BIN_SELECT_HI:
      s_bin_select = (s_bin_select & 0xFFFFFFFFull) | (uint64_t(ReadWord(frame)) << 32);
      break;
    case xenos::PM4_SET_BIN_MASK: {
      const uint64_t hi = ReadWord(frame), lo = ReadWord(frame);
      s_bin_mask = (hi << 32) | lo;
      break;
    }
    case xenos::PM4_SET_BIN_SELECT: {
      const uint64_t hi = ReadWord(frame), lo = ReadWord(frame);
      s_bin_select = (hi << 32) | lo;
      break;
    }
    case xenos::PM4_WAIT_REG_MEM: {
      const uint32_t wait_info = ReadWord(frame);
      const uint32_t poll_addr = ReadWord(frame);
      const uint32_t ref = ReadWord(frame);
      const uint32_t mask = ReadWord(frame);
      ReadWord(frame);  // wait interval (we poll on our own cadence)
      if (!EvaluateWaitRegMem(wait_info, poll_addr, ref, mask)) {
        // M4.29: budgeted. "Once per distinct wait" deduped against the LAST
        // key only, and ref is a per-frame fence counter -- every park was
        // "new", ~450 lines/s, the top filler of the 5MB log rotations. The
        // aggregate park-latency telemetry below covers steady state; this
        // line only earns its place during early boot bring-up.
        static std::atomic<int> s_park_log_budget{20};
        if (s_park_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
          REXLOG_INFO(
              "[native_vk] pm4 parked at WAIT info=0x{:X} poll=0x{:08X} ref=0x{:08X} mask=0x{:08X}",
              wait_info, poll_addr, ref, mask);
        }
        // M3.35 telemetry: how long do parks actually last? Every ms parked is
        // a ms the guest CPU can overwrite command pools ahead of the walker.
        if (!s_park_since) s_park_since = std::chrono::steady_clock::now().time_since_epoch().count();
        frame.pos = header_pos;  // rewind to the packet header; retry on resume
        return false;
      }
      if (s_park_since) {
        const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
        const int64_t us = (now - s_park_since) / 1000;
        s_park_since = 0;
        s_park_total_us.fetch_add(us, std::memory_order_relaxed);
        s_park_count.fetch_add(1, std::memory_order_relaxed);
        int64_t mx = s_park_max_us.load(std::memory_order_relaxed);
        while (us > mx && !s_park_max_us.compare_exchange_weak(mx, us)) {
        }
      }
      break;
    }
    case xenos::PM4_REG_RMW: {
      const uint32_t rmw_info = ReadWord(frame);
      const uint32_t and_mask = ReadWord(frame);
      const uint32_t or_mask = ReadWord(frame);
      uint32_t value = s_reg_shadow[rmw_info & 0x7FFF];
      value &= ((rmw_info >> 31) & 0x1) ? s_reg_shadow[and_mask & 0x7FFF] : and_mask;
      value |= ((rmw_info >> 30) & 0x1) ? s_reg_shadow[or_mask & 0x7FFF] : or_mask;
      WriteShadowRegister(rmw_info & 0x7FFF, value);
      break;
    }
    case xenos::PM4_REG_TO_MEM: {
      const uint32_t reg_addr = ReadWord(frame);
      uint32_t mem_addr = ReadWord(frame);
      uint32_t reg_val = ReadShadowRegister(reg_addr);
      const auto endianness = static_cast<xenos::Endian>(mem_addr & 0x3);
      mem_addr &= ~0x3u;
      reg_val = xenos::GpuSwap(reg_val, endianness);
      *memory->TranslatePhysical<uint32_t*>(mem_addr) = reg_val;
      MaybeLogFence(mem_addr, reg_val, "REG_TO_MEM");
      b.pm4_fence_writes.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    case xenos::PM4_COND_WRITE: {
      const uint32_t wait_info = ReadWord(frame);
      const uint32_t poll_reg_addr = ReadWord(frame);
      const uint32_t ref = ReadWord(frame);
      const uint32_t mask = ReadWord(frame);
      const uint32_t write_reg_addr = ReadWord(frame);
      uint32_t write_data = ReadWord(frame);
      if (EvaluateWaitRegMem(wait_info, poll_reg_addr, ref, mask)) {
        if (wait_info & 0x100) {
          const auto endianness = static_cast<xenos::Endian>(write_reg_addr & 0x3);
          const uint32_t addr = write_reg_addr & ~0x3u;
          write_data = xenos::GpuSwap(write_data, endianness);
          *memory->TranslatePhysical<uint32_t*>(addr) = write_data;
          MaybeLogFence(addr, write_data, "COND_WRITE");
        } else {
          WriteShadowRegister(write_reg_addr, write_data);
        }
        b.pm4_fence_writes.fetch_add(1, std::memory_order_relaxed);
      }
      break;
    }
    case xenos::PM4_MEM_WRITE: {
      uint32_t write_addr = ReadWord(frame);
      for (uint32_t i = 0; i < count - 1; ++i) {
        uint32_t write_data = ReadWord(frame);
        const auto endianness = static_cast<xenos::Endian>(write_addr & 0x3);
        const uint32_t addr = write_addr & ~0x3u;
        write_data = xenos::GpuSwap(write_data, endianness);
        *memory->TranslatePhysical<uint32_t*>(addr) = write_data;
        MaybeLogFence(addr, write_data, "MEM_WRITE");
        write_addr += 4;
      }
      b.pm4_fence_writes.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    case xenos::PM4_EVENT_WRITE_SHD: {
      const uint32_t initiator = ReadWord(frame);
      uint32_t address = ReadWord(frame);
      const uint32_t value = ReadWord(frame);
      uint32_t data_value;
      if ((initiator >> 31) & 0x1) {
        data_value = static_cast<uint32_t>(b.vblank_counter.load(std::memory_order_relaxed));
      } else {
        data_value = value;
      }
      const auto endianness = static_cast<xenos::Endian>(address & 0x3);
      address &= ~0x3u;
      data_value = xenos::GpuSwap(data_value, endianness);
      *memory->TranslatePhysical<uint32_t*>(address) = data_value;
      MaybeLogFence(address, data_value, "EVENT_WRITE_SHD");
      b.pm4_fence_writes.fetch_add(1, std::memory_order_relaxed);
      break;
    }
    case xenos::PM4_EVENT_WRITE_EXT: {
      // M3.36: screen-extent writeback. The GPU reports the screen extents of
      // rendered geometry to guest memory; the game's visibility system READS
      // this. We skipped it (~33/frame in gameplay, PM4-SKIP-CENSUS 0x5A) so
      // the game consumed stale/garbage extents and CULLED live objects
      // itself -- the per-frame missing-draw flicker (bear/rug/window runs
      // absent AT SUBMISSION while every walker-side audit stayed clean; the
      // SDK CommandProcessor implements this, hence the emulated path never
      // flickered). Mirror the reference behaviour: conservative full-screen
      // extents in 8-pixel units, z 0..1, endian-swapped per the address.
      const uint32_t initiator = ReadWord(frame);
      uint32_t address = ReadWord(frame);
      if (count > 2) SkipWords(frame, count - 2);
      address &= ~0x3u;
      s_extq_open = false;  // M3.174: END closes the bracket
      // M3.173 (RESTUFF_EXTSEQ=1): are EXT packets INTERLEAVED with their
      // objects' draws, or a detached tail burst? Decides whether "extents of
      // draws since the previous EXT packet" is a valid per-query answer (the
      // easy true-extent fix) or the association must come from the query
      // address. Logs (ext_n, frame draw count, initiator) every 500th packet
      // plus the first 20 of a frame: interleaved => draw count VARIES across
      // a frame's packets.
      {
        static const bool s_seq = getenv("RESTUFF_EXTSEQ") != nullptr;
        if (s_seq) {
          static std::atomic<uint64_t> s_en{0};
          const uint64_t en = s_en.fetch_add(1, std::memory_order_relaxed) + 1;
          static std::atomic<uint64_t> s_lastf{~0ull};
          const uint64_t fs = s_frame_serial.load(std::memory_order_relaxed);
          uint64_t prevf = s_lastf.exchange(fs, std::memory_order_relaxed);
          static std::atomic<int> s_burst{0};
          if (prevf != fs) s_burst.store(0, std::memory_order_relaxed);
          const int bi = s_burst.fetch_add(1, std::memory_order_relaxed);
          if (bi < 20 || (en % 500) == 0)
            REXLOG_INFO("[EXTSEQ] n={} frame={} pkt_in_frame={} draws_now={} init=0x{:X}",
                        en, fs, bi, b.pm4_draw_indx.load(std::memory_order_relaxed),
                        initiator);
        }
      }
      // M3.73 experiment (RESTUFF_EXT_EMPTY=1): report EMPTY extents instead
      // of the conservative fullscreen ones. If the game's sun-visibility
      // logic renders OCCLUDERS and checks whether their extents cover the
      // sun's screen position, "fullscreen" reads as "sun always occluded"
      // -> the global dim. Empty extents = "occluders nowhere" = sun always
      // visible; a brightness recovery confirms the semantics (the real fix
      // is then computing true per-draw screen extents at capture).
      // RESTUFF_EXT_FILE=<path>: flip the same switch LIVE, from a file polled
      // a few times a second. Every cross-run A/B on this defect has been
      // confounded by drives landing on different game moments (a sky view
      // reads 2x an interior for reasons that have nothing to do with the
      // bug); toggling mid-run compares the SAME view against itself.
      static const char* s_ext_file = getenv("RESTUFF_EXT_FILE");
      static std::atomic<bool> s_ext_live{false};
      if (s_ext_file) {
        static std::atomic<uint64_t> s_poll{0};
        if ((s_poll.fetch_add(1, std::memory_order_relaxed) & 0x3F) == 0) {
          if (FILE* f = fopen(s_ext_file, "rb")) {
            char c = '0';
            if (fread(&c, 1, 1, f) == 1) s_ext_live.store(c == '1', std::memory_order_relaxed);
            fclose(f);
          }
        }
      }
      static const bool s_ext_env = getenv("RESTUFF_EXT_EMPTY") != nullptr;
      const bool s_ext_empty = s_ext_env || s_ext_live.load(std::memory_order_relaxed);
      const uint16_t kMax = 2560 >> 3;
      // RESTUFF_EXT_BOX="minx,maxx,miny,maxy" (PIXELS): report a fixed partial
      // box instead of fullscreen/empty. Both existing modes are degenerate and
      // wrong in opposite directions -- fullscreen reads as "an occluder covers
      // the sun" (global dim), empty reads as "this object rasterized nothing"
      // (whole level culled). If the game consumes these SPATIALLY, a half-
      // screen box must produce a spatially STRUCTURED result (one side of the
      // level surviving), which pins the semantics before any real fix is built.
      static const int* s_box = [] () -> const int* {
        static int b[4];
        const char* e = getenv("RESTUFF_EXT_BOX");
        if (!e) return nullptr;
        return (sscanf(e, "%d,%d,%d,%d", &b[0], &b[1], &b[2], &b[3]) == 4) ? b : nullptr;
      }();
      // How many independent extent slots does the game keep? Distinct guest
      // addresses = distinct concurrent visibility queries; that count decides
      // whether true extents must be tracked per-query or can be frame-global.
      if (getenv("RESTUFF_EXT_CENSUS")) {
        static std::atomic<uint32_t> s_addrs[32];
        static std::atomic<int> s_naddr{0};
        bool known = false;
        const int n = s_naddr.load(std::memory_order_relaxed);
        for (int k = 0; k < n; ++k)
          if (s_addrs[k].load(std::memory_order_relaxed) == address) { known = true; break; }
        if (!known && n < 32) {
          s_addrs[n].store(address, std::memory_order_relaxed);
          s_naddr.store(n + 1, std::memory_order_relaxed);
          REXLOG_INFO("[EXT] new extent slot #{} addr=0x{:08X}", n, address);
        }
      }
      // M3.185 (RESTUFF_EXT_BRACKET=1): report the CURRENT BRACKET's box --
      // the per-query accumulator opened by EVENT_WRITE init-25 (M3.174) --
      // instead of any constant. The merge scheduler's dirty-tracking
      // consumes extents as CHANGE DETECTION: constants (any value) starve
      // it; hardware's extents vary every frame. The bracket box varies with
      // camera/content per query = the restored variation.
      static const bool s_ext_bracket = getenv("RESTUFF_EXT_BRACKET") != nullptr;
      // M3.185b: when the bracket had NO eligible draws, do not fall back to
      // the CONSTANT (constants starve the merge's change detection -- the
      // 1-in-6 stall). Jitter the conservative box by a few 8px units per
      // query so every answer varies.
      if (s_ext_bracket && !s_extq_box.any) {
        static std::atomic<uint32_t> s_jit{0};
        const uint32_t j = s_jit.fetch_add(1, std::memory_order_relaxed) & 7;
        const uint16_t jex[6] = {uint16_t(j),          uint16_t((2560 >> 3) - 1 - (j >> 1)),
                                 uint16_t(j >> 1),     uint16_t((2560 >> 3) - 1 - j), 0, 1};
        auto* jdst = memory->TranslatePhysical<uint16_t*>(address);
        for (size_t i = 0; i < 6; ++i) jdst[i] = __builtin_bswap16(jex[i]);
        b.pm4_event_ext_writes.fetch_add(1, std::memory_order_relaxed);
        break;
      }
      if (s_ext_bracket && s_extq_box.any) {
        const float bx0 = std::max(0.0f, s_extq_box.minx);
        const float bx1 = std::min(1279.0f, s_extq_box.maxx);
        const float by0 = std::max(0.0f, s_extq_box.miny);
        const float by1 = std::min(719.0f, s_extq_box.maxy);
        if (bx1 >= bx0 && by1 >= by0) {
          const uint16_t bex[6] = {uint16_t(int(bx0) >> 3), uint16_t(int(bx1) >> 3),
                                   uint16_t(int(by0) >> 3), uint16_t(int(by1) >> 3), 0, 1};
          auto* bdst = memory->TranslatePhysical<uint16_t*>(address);
          for (size_t i = 0; i < 6; ++i)
            bdst[i] = __builtin_bswap16(bex[i]);
          b.pm4_event_ext_writes.fetch_add(1, std::memory_order_relaxed);
          break;
        }
      }
      // M3.172: TRUE extents when RESTUFF_EXT_TRUE=1 -- report the screen-space
      // bbox of the geometry captured since the last extent write, instead of a
      // constant. Falls back to the conservative fullscreen box when nothing
      // was accumulated (no eligible draws), which is the historical behaviour.
      int true_box[4] = {0, 0, 0, 0};
      bool have_true = false;
      if (s_ext_true && s_ext_accum.any) {
        // Clamp the box to the screen as a RANGE INTERSECTION. Clamping each
        // edge independently produced inverted boxes ((1417,0)-(2559,-114)):
        // geometry entirely off-screen left miny floored to 0 while maxy
        // stayed negative, so have_true was false and every query silently
        // fell back to fullscreen.
        const float bx0 = std::max(0.0f, s_ext_accum.minx);
        const float bx1 = std::min(1279.0f, s_ext_accum.maxx);
        const float by0 = std::max(0.0f, s_ext_accum.miny);
        const float by1 = std::min(719.0f, s_ext_accum.maxy);
        if (bx1 >= bx0 && by1 >= by0) {
          true_box[0] = int(std::floor(bx0));
          true_box[1] = int(std::ceil(bx1));
          true_box[2] = int(std::floor(by0));
          true_box[3] = int(std::ceil(by1));
          have_true = true;
        }
      }
      if (s_ext_true) {
        static std::atomic<uint64_t> s_tn{0};
        const uint64_t tn = s_tn.fetch_add(1, std::memory_order_relaxed) + 1;
        if (getenv("RESTUFF_EXT_TRACE") && (tn % 2000) == 1)
          REXLOG_INFO("[EXTTRUE] n={} have={} box=({},{})-({},{})", tn, int(have_true),
                      true_box[0], true_box[2], true_box[1], true_box[3]);
        // Reset PER FRAME, not per write: the guest issues its extent queries
        // in BURSTS after the draws, so resetting here made the first write
        // consume the box and the next thousands report 'nothing accumulated'
        // (measured: have=0 on every sampled write). The box is the frame's
        // rendered geometry, which every query in the burst should see.
        static uint64_t s_ext_frame = ~0ull;
        const uint64_t f_now = s_frame_serial.load(std::memory_order_relaxed);
        if (f_now != s_ext_frame) {
          s_ext_frame = f_now;
          s_ext_accum.reset();
        }
      }
      const uint16_t extents[] = {
          uint16_t(have_true ? (true_box[0] >> 3)
                             : (s_box ? (s_box[0] >> 3) : (s_ext_empty ? kMax : 0))),  // min x
          uint16_t(have_true ? (true_box[1] >> 3)
                             : (s_box ? (s_box[1] >> 3) : (s_ext_empty ? 0 : kMax))),  // max x
          uint16_t(have_true ? (true_box[2] >> 3)
                             : (s_box ? (s_box[2] >> 3) : (s_ext_empty ? kMax : 0))),  // min y
          uint16_t(have_true ? (true_box[3] >> 3)
                             : (s_box ? (s_box[3] >> 3) : (s_ext_empty ? 0 : kMax))),  // max y
          0,                                                                          // min z
          1,                                                                          // max z
      };
      auto* dst = memory->TranslatePhysical<uint16_t*>(address);
      for (size_t i = 0; i < 6; ++i) {
        dst[i] = std::byteswap(extents[i]);  // guest is big-endian
      }
      b.pm4_event_ext_writes.fetch_add(1, std::memory_order_relaxed);
      // M3.171 (RESTUFF_EXTSTAT=1): is the extent channel even live, and how
      // many distinct query slots does the guest keep? Both other visibility
      // channels turned out to be UNUSED by this title (zero ZPD packets, zero
      // VIZ_QUERY packets, all session), so this is the last fabricated
      // visibility input and the only remaining cull lever (task #24).
      {
        static const bool s_es = getenv("RESTUFF_EXTSTAT") != nullptr;
        if (s_es) {
          static std::atomic<uint64_t> s_n{0};
          static std::atomic<uint32_t> s_slots[64];
          static std::atomic<int> s_nslot{0};
          bool known = false;
          const int ns = s_nslot.load(std::memory_order_relaxed);
          for (int k = 0; k < ns; ++k)
            if (s_slots[k].load(std::memory_order_relaxed) == address) { known = true; break; }
          if (!known && ns < 64) {
            s_slots[ns].store(address, std::memory_order_relaxed);
            s_nslot.store(ns + 1, std::memory_order_relaxed);
          }
          const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
          if ((n % 20000) == 0)
            REXLOG_INFO("[EXTSTAT] writes={} distinct_slots={} last_addr=0x{:08X}", n,
                        s_nslot.load(std::memory_order_relaxed), address);
        }
      }
      break;
    }
    case xenos::PM4_EVENT_WRITE_ZPD: {
      // M3.65 REWRITE (supersedes M3.56, which had TWO grave bugs):
      //  (1) PACKET OVERRUN: the packet carries ONLY the initiator (count==1;
      //      the SDK reference asserts this), but we read a second word as an
      //      "address" -- overrunning the packet and DESYNCING the whole PM4
      //      stream at every occlusion query, env-gate irrelevant. Queries fire
      //      exactly where the game tests visibility (the gate), mangling the
      //      subsequent draw packets => the camera-dependent missing walls /
      //      unstable floor, and the moving target under every winding A/B.
      //  (2) The sample-counts address comes from RB_SAMPLE_COUNT_ADDR
      //      (0x2325), not the packet.
      // Semantics (reference fallback path, host queries not implemented): the
      // D3D runtime marks an END request by storing kQueryFinished in
      // ZPass_A/B (or ZFail_A/B) and polls for the GPU to overwrite it. On
      // every ZPD: zero the struct; if it was an END, report a fake nonzero
      // ZPass so end-begin > 0 and the game never occlusion-culls its own
      // level (hidden chunks merely overdraw). RESTUFF_NO_ZPD=1 restores the
      // (broken-parse-fixed) no-write behaviour for A/B.
      const uint32_t initiator = ReadWord(frame);
      if (count > 1) SkipWords(frame, count - 1);  // consume EXACTLY the packet
      (void)initiator;
      static const bool zpd_on = getenv("RESTUFF_NO_ZPD") == nullptr;
      const uint32_t address = s_reg_shadow[0x2325] & ~0x3u;  // RB_SAMPLE_COUNT_ADDR
      if (getenv("RESTUFF_FENCE_TRACE")) {
        static std::atomic<int> s_z{40};
        if (s_z.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[ZPD] occlusion query addr=0x{:08X} count={} on={}", address, count,
                      zpd_on ? 1 : 0);
      }
      // M3.123: RESTUFF_ZPD_TRACE=1 -- dump the guest's pre-write marker words
      // so unrecognized end-request markers (queries we then answer with 0 =
      // "fully occluded") become visible. The flare/sparkle probes may mark
      // END differently from the sun query this path was verified against.
      if (getenv("RESTUFF_ZPD_TRACE") && address >= 0x1000u && address < 0x20000000u) {
        if (uint32_t* pre = memory->TranslatePhysical<uint32_t*>(address)) {
          static std::atomic<int> s_zt{120};
          if (s_zt.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[ZPDTRACE] addr=0x{:08X} pre={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} "
                        "{:08X} {:08X}",
                        address, pre[0], pre[1], pre[2], pre[3], pre[4], pre[5], pre[6], pre[7]);
        }
      }
      if (zpd_on && address >= 0x1000u && address < 0x20000000u) {
        if (uint32_t* dst = memory->TranslatePhysical<uint32_t*>(address)) {
          // xe_gpu_depth_sample_counts (le<u32>x8): Total_A, Total_B, ZFail_A,
          // ZFail_B, ZPass_A, ZPass_B, StencilFail_A, StencilFail_B. Host is
          // LE, the struct is LE: raw compares/stores are correct.
          constexpr uint32_t kFin = 0xEDFEFFFFu;  // byte_swap(0xFFFFFEED)
          const bool is_end = (dst[4] == kFin && dst[5] == kFin) ||  // via ZPass
                              (dst[2] == kFin && dst[3] == kFin);    // via ZFail
          for (int i = 0; i < 8; ++i) dst[i] = 0;
          if (is_end) {
            // M3.9x WORLD-DIM FIX: match the SDK reference EXACTLY.
            // The SDK's CommandProcessor (what the non-dim emulated build runs)
            // reports cvar query_occlusion_fake_sample_count = 1000; we reported
            // 0x100 = 256, ~4x fewer "passed" samples. This title tests SUN
            // VISIBILITY with an occlusion query and scales its light-shaft /
            // scattering by the result, so under-reporting read as "the sun is
            // mostly occluded" -> the guest computed a shaft constant that
            // darkened the whole world (flat occ ~0.36 in the fullscreen
            // (1-occ)*scene composite = the ~2x global dim vs the reference).
            // Override for A/B: RESTUFF_ZPD_SAMPLES=<n>.
            static const uint32_t kFakeSamples = [] {
              const char* e = getenv("RESTUFF_ZPD_SAMPLES");
              return e ? uint32_t(strtoul(e, nullptr, 0)) : 1000u;
            }();
            dst[0] = kFakeSamples;  // Total_A
            dst[4] = kFakeSamples;  // ZPass_A
          }
        }
      }
      break;
    }
    case xenos::PM4_VIZ_QUERY: {
      // M3.77: begin/end initiator for viz-query extent processing. The game
      // READS PA_SC_VIZ_QUERY_STATUS_0/1 (0x0C44/0x0C45, served from our
      // shadow by GpuMmioRead) to learn per-object/per-light visibility; we
      // never handled the packet, so every query read back "not visible" --
      // the per-object light assignment starved (user-observed: moving the
      // camera "frees up a light" and surfaces brighten). Mirror the SDK
      // reference exactly: on end, report the query VISIBLE.
      const uint32_t dword0 = ReadWord(frame);
      if (count > 1) SkipWords(frame, count - 1);
      const uint32_t id = dword0 & 0x3F;
      if (dword0 & 0x100) {
        if (id < 32) {
          s_reg_shadow[0x0C44] |= 1u << id;
        } else {
          s_reg_shadow[0x0C45] |= 1u << (id - 32);
        }
      }
      // M3.170 (RESTUFF_VIZTRACE=1): the wedge is ONE culled foreground chunk
      // in the guest's FINE partition, and this is the only visibility channel
      // the game actually uses here -- EVENT_WRITE_ZPD never arrives (0 packets
      // in every log; the image embeds no 0x5B headers either), so the
      // occlusion-query theory is moot. Question now: are the status BITS the
      // guest reads correct per query id? We only ever SET bits on end and
      // never clear them, so a stale set bit is safe, but an id we never see
      // an END for stays 0 = 'not visible' = culled. Census which ids begin
      // vs end (task #24).
      {
        static const bool s_viz = getenv("RESTUFF_VIZTRACE") != nullptr;
        if (s_viz) {
          static std::atomic<uint32_t> s_begin[64] = {};
          static std::atomic<uint32_t> s_end[64] = {};
          if (id < 64) ((dword0 & 0x100) ? s_end : s_begin)[id].fetch_add(1,
              std::memory_order_relaxed);
          static std::atomic<uint64_t> s_n{0};
          if ((s_n.fetch_add(1, std::memory_order_relaxed) % 20000) == 0) {
            std::string line;
            for (int i = 0; i < 64; ++i) {
              const uint32_t b0 = s_begin[i].load(std::memory_order_relaxed);
              const uint32_t e0 = s_end[i].load(std::memory_order_relaxed);
              if (b0 || e0) {
                char buf[40];
                snprintf(buf, sizeof(buf), " %d:%u/%u", i, b0, e0);
                line += buf;
              }
            }
            REXLOG_INFO("[VIZTRACE] id:begin/end{}", line);
          }
        }
      }
      break;
    }
    case xenos::PM4_EVENT_WRITE:
    case xenos::PM4_EVENT_WRITE_CFL: {
      // M3.49: currently UNHANDLED (fell to default/skip). If the guest writes
      // the 0x1FCA3000 fence via one of these "event completed -> write memory"
      // packets, our GPU never signals completion and the guest stalls on a
      // fallback interrupt (~2ms/wait = the 20-vs-60fps gap). Log the layout to
      // decide the correct write; consume exactly `count` words (stream-safe).
      static const bool trace = getenv("RESTUFF_FENCE_TRACE") != nullptr;
      uint32_t w[8] = {};
      const uint32_t n = std::min<uint32_t>(count, 8);
      for (uint32_t i = 0; i < n; ++i) w[i] = ReadWord(frame);
      if (count > 8) SkipWords(frame, count - 8);
      if (trace) {
        static std::atomic<int> s_ew{200};
        if (s_ew.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO(
              "[FENCE] EVENT_WRITE op={:02X} count={} w0={:08X} w1={:08X} w2={:08X} w3={:08X}",
              opcode, count, w[0], w[1], w[2], w[3]);
      }
      // M3.173: initiator 25 = SCREEN_EXTENT BEGIN (starts the extent
      // accumulator on hardware; sub_82F24A78 emits it as the D3D extent
      // query's Issue(BEGIN), paired with the EVENT_WRITE_EXT initiator-26
      // END). Count them against draws to prove the bracket carries draws.
      {
        static const bool s_seq = getenv("RESTUFF_EXTSEQ") != nullptr;
        if (n >= 1 && (w[0] & 0x3F) == 25) {
          // M3.174: open the extent-query bracket (walker thread, in stream
          // order with the capture calls, so plain statics are safe).
          s_extq_open = true;
          s_extq_box.reset();
          if (s_seq) {
            static std::atomic<uint64_t> s_bn{0};
            const uint64_t bn = s_bn.fetch_add(1, std::memory_order_relaxed) + 1;
            if ((bn % 500) == 1)
              REXLOG_INFO("[EXTSEQ] BEGIN n={} draws_now={}", bn,
                          b.pm4_draw_indx.load(std::memory_order_relaxed));
          }
        }
      }
      // M3.50/M3.51: raising a source-1 GPU interrupt on flush events (to prompt
      // the guest ISR to clear the 0x1FCA3000 completion fence) HARD-CRASHED the
      // guest -- spurious source-1s corrupt it, which means source-1 IS the real
      // 1-per-completion interrupt and must NOT be duplicated. So the fence is
      // NOT cleared by an extra interrupt; the guest advances it on its own
      // (CPU-contention-gated) path. Reverted; the EVENT_WRITE logging above
      // stays as a diagnostic.
      break;
    }
    case xenos::PM4_INTERRUPT: {
      const uint32_t cpu_mask = ReadWord(frame);
      b.seen_pm4_interrupt.store(true, std::memory_order_relaxed);
      b.pending_interrupt_mask.fetch_or(cpu_mask & 0x3F, std::memory_order_acq_rel);
      // M3.39: HYBRID ISR delivery. Instant (event-driven) delivery
      // destabilizes the early-boot handshake (boot A/B: 1/3 vs 3/3 with
      // 60Hz batching) but is load-bearing for gameplay fps (~21/s vs ~5/s:
      // post-capture-fix frame time is park-latency-sensitive). Quantized
      // 60Hz delivery until the guest is safely past boot, instant after.
      // RESTUFF_PUMP_LEGACY=1 forces quantized always.
      static const bool pump_legacy = getenv("RESTUFF_PUMP_LEGACY") != nullptr;
      if (!pump_legacy && b.pm4_swaps.load(std::memory_order_relaxed) > 60)
        b.pump_wake_cv.notify_one();
      break;
    }
    case xenos::PM4_XE_SWAP: {
      // PM4-level frame boundary: publish this frame's captured draws (the
      // on_swap midasm fires BEFORE the final kick, splitting frames).
      CheckStalePayloads();
      s_frame_serial.fetch_add(1, std::memory_order_relaxed);  // stream-cache epoch
      // VdSwap wrote: signature, frontbuffer physical, width, height. Parse
      // FIRST: this swap's fb pointer is the display source for the frame just
      // ended, and travels with it (M3.13 -- pairing the frame with the LIVE
      // pointer raced one frame ahead and flickered).
      uint32_t swap_fb = 0;
      const uint32_t magic = ReadWord(frame);
      if (count >= 4 && (magic == xenos::kSwapSignature ||
                         magic == std::byteswap(uint32_t(xenos::kSwapSignature)))) {
        const uint32_t fb_ptr = ReadWord(frame);
        const uint32_t fb_width = ReadWord(frame);
        const uint32_t fb_height = ReadWord(frame);
        SkipWords(frame, count - 4);
        if (fb_ptr && fb_width && fb_height && fb_width <= 4096 && fb_height <= 4096) {
          swap_fb = fb_ptr;
          b.frontbuffer_w.store(fb_width, std::memory_order_relaxed);
          b.frontbuffer_h.store(fb_height, std::memory_order_relaxed);
          b.frontbuffer_phys.store(fb_ptr, std::memory_order_release);
          restuff::renderer::SetFrontBufferPhys(fb_ptr);  // M2.4: present source
          // On-change: the game double-buffers (fb alternates between a pair);
          // log only when a value OUTSIDE the current pair appears (scene
          // transitions swap the pair -- a handful of lines ever).
          static uint32_t fb_a = 0, fb_b = 0;  // walker thread only
          if (fb_ptr != fb_a) {
            if (fb_ptr == fb_b) {
              fb_b = fb_a;
              fb_a = fb_ptr;
            } else {
              REXLOG_INFO("[native_vk] SWAP-FB new=0x{:08X} (pair was 0x{:08X}/0x{:08X}) {}x{}",
                          fb_ptr, fb_a, fb_b, fb_width, fb_height);
              fb_b = fb_a;
              fb_a = fb_ptr;
            }
          }
        }
      } else {
        SkipWords(frame, count - 1);
      }
      restuff::renderer::EndGuestFrame(swap_fb);
      const uint64_t swaps = b.pm4_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
      b.pending_swap_acks.fetch_add(1, std::memory_order_acq_rel);
      static const bool pump_legacy2 = getenv("RESTUFF_PUMP_LEGACY") != nullptr;
      if (!pump_legacy2 && b.pm4_swaps.load(std::memory_order_relaxed) > 60)
        b.pump_wake_cv.notify_one();
      if (swaps <= 4) {
        REXLOG_INFO("[native_vk] pm4 XE_SWAP #{} fb=0x{:08X} {}x{}", swaps,
                    b.frontbuffer_phys.load(std::memory_order_relaxed),
                    b.frontbuffer_w.load(std::memory_order_relaxed),
                    b.frontbuffer_h.load(std::memory_order_relaxed));
      }
      break;
    }
    case xenos::PM4_SET_CONSTANT2:
    case xenos::PM4_SET_SHADER_CONSTANTS: {
      // word0 low 16 bits = absolute register index, then count-1 values.
      const uint32_t offset_type = ReadWord(frame);
      const uint32_t index = offset_type & 0xFFFF;
      for (uint32_t i = 0; i < count - 1; ++i) {
        WriteShadowRegister(index + i, ReadWord(frame));
      }
      break;
    }
    case xenos::PM4_SET_CONSTANT: {
      // word0 = (type << 16) | index, then count-1 data words.
      const uint32_t offset_type = ReadWord(frame);
      const uint32_t index = offset_type & 0x7FF;
      uint32_t reg_base;
      switch ((offset_type >> 16) & 0xFF) {
        case 0: reg_base = 0x4000; break;  // ALU constants
        case 1: reg_base = 0x4800; break;  // fetch constants
        case 2: reg_base = 0x4900; break;  // bool constants
        case 3: reg_base = 0x4908; break;  // loop constants
        case 4: reg_base = 0x2000; break;  // registers
        default: reg_base = 0; break;
      }
      for (uint32_t i = 0; i < count - 1; ++i) {
        const uint32_t data = ReadWord(frame);
        if (reg_base) {
          WriteShadowRegister(reg_base + index + i, data);
        }
      }
      break;
    }
    case xenos::PM4_LOAD_ALU_CONSTANT: {
      // word0 = physical address, word1 = (type << 16) | index, word2 = size.
      const uint32_t address = ReadWord(frame) & 0x3FFFFFFF;
      const uint32_t offset_type = ReadWord(frame);
      const uint32_t index = offset_type & 0x7FF;
      const uint32_t size_dwords = ReadWord(frame) & 0xFFF;
      const uint32_t* src = memory->TranslatePhysical<const uint32_t*>(address);
      for (uint32_t i = 0; i < size_dwords; ++i) {
        WriteShadowRegister(0x4000 + index + i, std::byteswap(src[i]));
      }
      // Stale detector: the engine may fill this physical block AFTER queueing
      // the packet; re-hash at the swap to catch constants captured too early.
      if (s_dump_draws && size_dwords) {
        PendingHashCheck c = {};
        c.vb_phys = address;
        c.vb_len = size_dwords * 4;
        c.vb_hash = Fnv1a(reinterpret_cast<const uint8_t*>(src), c.vb_len);
        c.vs_hash = 0xC0157A7E00000000ull | index;  // marker: constant block @ reg index
        s_hash_checks.push_back(c);
      }
      break;
    }
    case xenos::PM4_IM_LOAD: {
      // word0 = physical addr | shader type (low 2 bits), word1 = start<<16 | size.
      const uint32_t addr_type = ReadWord(frame);
      const uint32_t start_size = ReadWord(frame);
      const uint32_t size_dwords = start_size & 0xFFFF;
      const uint32_t phys = addr_type & ~0x3u;
      RegisterGuestShader(addr_type & 0x3, phys,
                          memory->TranslatePhysical<const uint32_t*>(phys), size_dwords);
      break;
    }
    case xenos::PM4_IM_LOAD_IMMEDIATE: {
      // word0 = shader type, word1 = start<<16 | size; raw BE ucode follows inline.
      const uint32_t type_word = ReadWord(frame);
      const uint32_t start_size = ReadWord(frame);
      const uint32_t size_dwords = start_size & 0xFFFF;
      if (count >= 2 && size_dwords != 0 && size_dwords <= count - 2) {
        const uint32_t* host = memory->TranslatePhysical<const uint32_t*>(frame.base_phys);
        std::vector<uint32_t> ucode(size_dwords);
        for (uint32_t i = 0; i < size_dwords; ++i) {
          ucode[i] = host[frame.pos];  // raw BE words, honoring ring wrap
          frame.pos =
              frame.is_ring ? ((frame.pos + 1) & (frame.size_words - 1)) : (frame.pos + 1);
        }
        RegisterGuestShader(type_word & 0x3, 0, ucode.data(), size_dwords);
        if (count - 2 > size_dwords) {
          SkipWords(frame, count - 2 - size_dwords);
        }
      } else {
        SkipWords(frame, count >= 2 ? count - 2 : 0);
      }
      break;
    }
    case xenos::PM4_INDIRECT_BUFFER:
    case xenos::PM4_INDIRECT_BUFFER_PFD: {
      const uint32_t list_ptr = ReadWord(frame);
      const uint32_t list_length = ReadWord(frame) & 0xFFFFF;  // words
      if (b.pm4_stack.size() < 8 && list_length != 0 && list_length < (1u << 20)) {
        StreamFrame ib;
        ib.base_phys = list_ptr & 0x1FFFFFFF;
        ib.size_words = list_length;
        ib.pos = 0;
        ib.is_ring = false;
        // M3.33: snapshot the IB now (content is provably intact at the call;
        // the IB-CRC probe only ever caught corruption BETWEEN push and pop).
        static const bool no_snap = getenv("RESTUFF_NO_IB_SNAPSHOT") != nullptr;
        if (!no_snap) {
          if (const uint32_t* src =
                  memory->TranslatePhysical<const uint32_t*>(ib.base_phys)) {
            ib.owned = std::make_shared<std::vector<uint32_t>>(src, src + list_length);
          }
        }
        // M3.32: remember where in the RING this nest was opened -- while
        // parked inside it, the reported rptr must not advance past this.
        if (frame.is_ring) b.ring_ib_call_pos = header_pos;
        b.pm4_stack.push_back(ib);
        b.pm4_indirect_buffers.fetch_add(1, std::memory_order_relaxed);
        // RESTUFF_IB_CRC=1: hash IB head+tail at push, verify at pop -- detects
        // the guest overwriting command memory before the walker parses it
        // (the missing-draw-run blob suspect). Walker-thread only.
        if (s_ib_crc_on) {
          s_ib_crc.push_back({ib.base_phys, ib.size_words, IbProbeHash(ib.base_phys, ib.size_words)});
        }
        // Blob hunt: a refused IB silently drops a whole run of draws (the
        // per-frame missing-object class), so track the depth high water.
        static std::atomic<uint32_t> s_ib_hw{0};
        uint32_t hw = s_ib_hw.load(std::memory_order_relaxed);
        while (b.pm4_stack.size() > hw &&
               !s_ib_hw.compare_exchange_weak(hw, uint32_t(b.pm4_stack.size()))) {
        }
        if (b.pm4_stack.size() > 2 && b.pm4_stack.size() >= hw) {
          static std::atomic<int> s_hwlog{12};
          if (s_hwlog.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[native_vk] IB-DEPTH high-water {} (ptr=0x{:08X} len={})",
                        uint32_t(b.pm4_stack.size()), list_ptr, list_length);
        }
      } else {
        // Unconditional: each refusal is a dropped draw-run = visible defect.
        static std::atomic<int> s_ibrefuse{40};
        if (s_ibrefuse.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[native_vk] IB-REFUSED ptr=0x{:08X} len={} words stack={}", list_ptr,
                      list_length, uint32_t(b.pm4_stack.size()));
      }
      break;
    }
    case xenos::PM4_DRAW_INDX: {
      // word0 = viz query, word1 = VGT_DRAW_INITIATOR, then (if DMA-indexed)
      // word2 = index base, word3 = index size. Diagnostic for now: are the
      // missing title elements (sky, colored bears) PM4 draws?
      ReadWord(frame);  // viz
      const uint32_t initiator = ReadWord(frame);
      uint32_t index_base = 0;
      const uint32_t src_sel = (initiator >> 6) & 0x3;
      if (src_sel == 0 && count >= 4) {
        // M3.316: mask only bit 0. The old & ~0x3u dword-align rounded a
        // 16-bit-index sub-draw starting at an ODD index (base ≡ 2 mod 4) one
        // whole index early — orphan head index + the run's LAST TRIANGLE
        // dropped. Proven by the hut-wedge capture pair (Aug 25): the wedge
        // boot's 6 ground strips are odd/even slices of the clean boot's 3120-
        // index master stream; exactly the odd-start slices lose their tail
        // triangle (3 lost total, one = the wedge hole). 2-byte alignment is
        // the real hardware constraint for u16 indices.
        index_base = ReadWord(frame) & ~0x1u;
        ReadWord(frame);  // index size/endian word
        if (count > 4) SkipWords(frame, count - 4);
      } else if (count > 2) {
        SkipWords(frame, count - 2);
      }
      b.pm4_draw_indx.fetch_add(1, std::memory_order_relaxed);
      // srcsel 0 = DMA u16 indices; srcsel 2 = auto-indexed (the former UP
      // draws — captured here, in true stream order, instead of at the API).
      if (src_sel == 0 || src_sel == 2) {
        restuff::renderer::D3dCensusHit(restuff::renderer::kOpDrawIndx);  // M4.39d
        CapturePm4Draw(initiator, index_base);
      } else if (s_dump_draws) {
        // srcsel 1 = IMMEDIATE indices (inline in the packet) -- currently
        // NOT captured. Log them: a per-frame backdrop cell hiding here would
        // explain the emulated-only right-band content.
        static std::atomic<int> s_imm{80};
        if (s_imm.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[DUMP] IMM-DRAW srcsel={} prim={} n={} vs={:016X} ps={:016X} surf={}/m{}",
                      src_sel, initiator & 0x3F, (initiator >> 16) & 0xFFFF,
                      s_current_vs_hash, s_current_ps_hash, s_reg_shadow[0x2000] & 0x3FFF,
                      (s_reg_shadow[0x2000] >> 16) & 3);
      }
      break;
    }
    case xenos::PM4_DRAW_INDX_2: {
      // word0 = VGT_DRAW_INITIATOR; immediate indices may follow.
      const uint32_t initiator = ReadWord(frame);
      if (count > 1) SkipWords(frame, count - 1);
      b.pm4_draw_indx.fetch_add(1, std::memory_order_relaxed);
      restuff::renderer::D3dCensusHit(restuff::renderer::kOpDrawIndx2);  // M4.39d
      CapturePm4Draw(initiator, 0);
      break;
    }
    case xenos::PM4_DRAW_INDX_BIN:
    case xenos::PM4_DRAW_INDX_2_BIN: {
      // M3.53: binned (predicated-tiling) draw variants. The game switches to
      // tiled rendering for heavy scenes that don't fit EDRAM in one pass; large
      // geometry (e.g. the outdoor floor showing water-through when absent) can
      // be drawn ONLY through these, so skipping them drops it. We render to a
      // single full-res target (not tiled EDRAM), so the bin-ID predication is
      // moot -- capture the draw once like its non-binned base. Read exactly
      // `count` words (stream-safe) and pull the initiator from the base layout.
      b.pm4_bin_draws.fetch_add(1, std::memory_order_relaxed);
      uint32_t w[8] = {};
      const uint32_t n = std::min<uint32_t>(count, 8);
      for (uint32_t i = 0; i < n; ++i) w[i] = ReadWord(frame);
      if (count > 8) SkipWords(frame, count - 8);
      uint32_t initiator = 0, index_base = 0;
      if (opcode == xenos::PM4_DRAW_INDX_2_BIN) {
        initiator = w[0];  // base DRAW_INDX_2: word0 = VGT_DRAW_INITIATOR
      } else {             // base DRAW_INDX: word0 = viz, word1 = initiator, w2 = index base
        initiator = w[1];
        if (((initiator >> 6) & 0x3) == 0 && count >= 4)
          index_base = w[2] & ~0x1u;  // M3.316: keep bit 1 (see DRAW_INDX)
      }
      static const bool cap_bin = getenv("RESTUFF_CAP_BIN") != nullptr;
      if (cap_bin || getenv("RESTUFF_FENCE_TRACE")) {
        static std::atomic<int> s_bd{80};
        if (s_bd.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[BINDRAW] op={:02X} count={} init=0x{:08X} prim={} n={} idxbase=0x{:08X} "
                      "w0={:08X} w1={:08X} w2={:08X} w3={:08X} vs={:016X}",
                      opcode, count, initiator, initiator & 0x3F, (initiator >> 16) & 0xFFFF,
                      index_base, w[0], w[1], w[2], w[3], s_current_vs_hash);
      }
      if (cap_bin) {
        b.pm4_draw_indx.fetch_add(1, std::memory_order_relaxed);
        b.pm4_bin_captured.fetch_add(1, std::memory_order_relaxed);
        restuff::renderer::D3dCensusHit(restuff::renderer::kOpDrawIndx2Bin);  // M4.39d
        CapturePm4Draw(initiator, index_base);
      }
      break;
    }
    default:
      // State flushes and misc: GPU-only work, skip the payload.
      // (IM_LOAD_IMMEDIATE's inline ucode is part of `count`, so this is
      // stream-safe for every opcode.)
      if (s_dump_draws) {
        // Opcode census: anything frequent and unhandled is a candidate for
        // content we never render.
        static std::atomic<uint32_t> s_op_census[128];
        static std::atomic<int> s_census_tick{0};
        if (opcode < 128) s_op_census[opcode].fetch_add(1, std::memory_order_relaxed);
        if ((s_census_tick.fetch_add(1, std::memory_order_relaxed) % 100000) == 99999) {
          std::string top;
          for (uint32_t i = 0; i < 128; ++i) {
            const uint32_t n = s_op_census[i].load(std::memory_order_relaxed);
            if (n > 500) top += fmt::format("{:02X}:{} ", i, n);
          }
          REXLOG_INFO("[DUMP] PM4-SKIP-CENSUS {}", top);
        }
      }
      SkipWords(frame, count);
      break;
  }
  return true;
}

// --- Stream machine (NB:2097-2162) -------------------------------------------

void RunPm4Machine() {
  auto& b = B();
  if (b.pm4_stack.empty()) {
    return;
  }
  b.pm4_parked.store(false, std::memory_order_relaxed);

  uint32_t safety = 1u << 22;  // don't spin forever on a malformed stream
  while (safety-- != 0) {
    StreamFrame& frame = b.pm4_stack.back();
    if (frame.pos == FrameEnd(frame)) {
      if (frame.is_ring) {
        return;  // fully caught up
      }
      if (s_ib_crc_on && !s_ib_crc.empty()) {
        const IbCrcRec rec = s_ib_crc.back();
        s_ib_crc.pop_back();
        if (rec.base == frame.base_phys && rec.len == frame.size_words) {
          const uint64_t now = IbProbeHash(rec.base, rec.len);
          static std::atomic<uint64_t> s_ok{0}, s_bad{0};
          if (now != rec.hash) {
            const uint64_t bad = s_bad.fetch_add(1, std::memory_order_relaxed) + 1;
            static std::atomic<int> s_lb{20};
            if (s_lb.fetch_sub(1, std::memory_order_relaxed) > 0)
              REXLOG_INFO("[native_vk] IB-CRC MISMATCH ptr=0x{:08X} len={} (ok={} bad={})",
                          rec.base, rec.len, s_ok.load(std::memory_order_relaxed), bad);
          } else {
            const uint64_t ok = s_ok.fetch_add(1, std::memory_order_relaxed) + 1;
            if (ok % 20000 == 0)
              REXLOG_INFO("[native_vk] IB-CRC ok={} bad={}", ok,
                          s_bad.load(std::memory_order_relaxed));
          }
        }
      }
      b.pm4_stack.pop_back();
      continue;
    }

    const uint32_t header_pos = frame.pos;
    const uint32_t packet = ReadWord(frame);
    static const bool seq_log = getenv("RESTUFF_DUMP_PM4SEQ") != nullptr;
    if (seq_log) s_pm4seq.Push(packet);
    switch (packet >> 30) {
      case 0x0: {  // type-0: sequential register writes
        const uint32_t reg_count = ((packet >> 16) & 0x3FFF) + 1;
        const uint32_t base_index = packet & 0x7FFF;
        const uint32_t write_one_reg = (packet >> 15) & 0x1;
        for (uint32_t m = 0; m < reg_count; ++m) {
          const uint32_t payload_pos = frame.pos;
          const uint32_t reg_data = ReadWord(frame);
          const uint32_t reg = write_one_reg ? base_index : base_index + m;
          if (reg == 0x4801) {
            s_last_tex0w = {s_draw_serial, reg_data, frame.base_phys + payload_pos * 4};
          }
          WriteShadowRegister(reg, reg_data);
          // M3.100: the display gamma ramp arrives as PM4 TYPE-0 register
          // bursts (VdInitializeScalerCommandBuffer/VdSwap build a scaler
          // command buffer of DC register writes), NOT as CPU MMIO -- shadow
          // writes alone would swallow the sequential DC_LUT protocol.
          if (reg - 0x1918u <= (0x1934u - 0x1918u)) HandleGammaRampWrite(reg, reg_data);
        }
        break;
      }
      case 0x1: {  // type-1: two register writes
        const uint32_t reg_index_1 = packet & 0x7FF;
        const uint32_t reg_index_2 = (packet >> 11) & 0x7FF;
        const uint32_t v1 = ReadWord(frame);
        const uint32_t v2 = ReadWord(frame);
        WriteShadowRegister(reg_index_1, v1);
        WriteShadowRegister(reg_index_2, v2);
        if (reg_index_1 - 0x1918u <= (0x1934u - 0x1918u)) HandleGammaRampWrite(reg_index_1, v1);
        if (reg_index_2 - 0x1918u <= (0x1934u - 0x1918u)) HandleGammaRampWrite(reg_index_2, v2);
        break;
      }
      case 0x2:  // type-2: no-op filler
        break;
      case 0x3:
        if (!ExecutePm4Type3(b.pm4_stack.back(), packet, header_pos)) {
          b.pm4_parked.store(true, std::memory_order_release);
          return;
        }
        break;
    }
  }
  REXLOG_ERROR("[native_vk] pm4 machine safety bound hit; stream abandoned");
  b.pm4_stack.resize(1);
  b.pm4_stack[0].pos = b.ring_wptr_words;
}

// M3.32: write the guest-visible ring read pointer from the TRUE walker
// position. The old behaviour (rptr := wptr unconditionally) told the game
// the GPU had consumed everything even while the machine was PARKED at a
// WAIT_REG_MEM -- so the game recycled indirect-buffer memory the walker had
// not parsed yet, and whole draw runs vanished for a frame (the black/blue
// blobs, the window flicker). Report the oldest unconsumed ring position
// instead; the game's own ring/pool accounting then throttles exactly as it
// would against real hardware. RESTUFF_RPTR_LIE=1 restores the old behaviour.
// Caller holds pm4_mutex.
static void WriteRingRptrLocked() {
  auto& b = B();
  static const bool lie = getenv("RESTUFF_RPTR_LIE") != nullptr;
  const uint32_t writeback = b.ring_writeback_addr.load(std::memory_order_acquire);
  if (!writeback || b.pm4_stack.empty()) return;
  uint32_t reported = b.last_kick_wptr_raw;
  if (!lie) {
    // Consumed ring position: mid-IB (stack depth > 1) -> the ring packet
    // that opened the nest; otherwise the ring frame's own position (parks
    // rewind pos to the packet header, so this is exactly the first
    // unconsumed packet).
    const uint32_t ring_size = b.pm4_stack[0].size_words;
    const uint32_t consumed =
        b.pm4_stack.size() > 1 ? b.ring_ib_call_pos : b.pm4_stack[0].pos;
    const uint32_t unconsumed = (b.ring_wptr_words - consumed) & (ring_size - 1);
    reported = b.last_kick_wptr_raw - unconsumed;
  }
  auto* slot = rex::Runtime::instance()->memory()->TranslatePhysical<volatile uint32_t*>(writeback);
  if (slot) *slot = std::byteswap(reported);
  MaybeLogFence(writeback, reported, "RPTR_WRITEBACK");
}

void TryResumePm4() {
  auto& b = B();
  if (!b.pm4_parked.load(std::memory_order_acquire)) {
    return;
  }
  // M3.44: in async mode the capture worker owns RunPm4Machine -- just wake it
  // so it re-evaluates the parked WAIT_REG_MEM (its poll condition may now be
  // satisfied by an interrupt the pump just delivered).
  static const bool async_cap = getenv("RESTUFF_ASYNC_CAPTURE") != nullptr;
  if (async_cap) {
    {
      std::lock_guard<std::mutex> wl(b.capture_wake_mutex);
      b.capture_pending = true;
    }
    b.capture_wake_cv.notify_one();
    return;
  }
  std::lock_guard<std::mutex> lock(b.pm4_mutex);
  RunPm4Machine();
  // The park may have ended -- advance the guest-visible rptr so a game
  // waiting on ring drain doesn't stall against a stale value.
  WriteRingRptrLocked();
}

// M3.44: async-capture worker. Parses the ring off the guest render thread.
int CaptureWorkerMain() {
  auto& b = B();
  REXLOG_INFO("[native_vk] async capture worker running");
  while (b.capture_running.load(std::memory_order_relaxed)) {
    {
      std::unique_lock<std::mutex> wl(b.capture_wake_mutex);
      b.capture_wake_cv.wait(wl, [&] {
        return b.capture_pending || !b.capture_running.load(std::memory_order_relaxed);
      });
      b.capture_pending = false;
    }
    if (!b.capture_running.load(std::memory_order_relaxed)) break;
    const auto t0 = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lock(b.pm4_mutex);
      // M3.55: apply the latest lock-free kick wptr (guest thread stored it
      // without the mutex, so it never blocked on this walk).
      if (!b.pm4_stack.empty()) {
        const uint32_t wp = b.pending_wptr_raw.load(std::memory_order_acquire);
        b.ring_wptr_words = wp & (b.pm4_stack[0].size_words - 1);
        b.last_kick_wptr_raw = wp;
      }
      RunPm4Machine();
      WriteRingRptrLocked();
    }
    b.walker_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count(),
                          std::memory_order_relaxed);
  }
  return 0;
}

void StartCaptureWorkerOnce() {
  auto& b = B();
  if (!getenv("RESTUFF_ASYNC_CAPTURE")) return;
  if (b.capture_running.exchange(true, std::memory_order_acq_rel)) return;
  b.capture_thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(b.kernel_state, 128 * 1024, 0,
                                   [] { return CaptureWorkerMain(); }));
  b.capture_thread->set_name("NativeVk Capture");
  b.capture_thread->Create();
}

// --- Ring kick (NB:3240-3273) -------------------------------------------------

void OnRingBufferKick(uint32_t write_pointer) {
  auto& b = B();
  if (!b.active.load(std::memory_order_acquire)) {
    return;
  }
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) {
    return;
  }

  // Execute the CPU-visible packets synchronously on the kicking thread so the
  // game's next poll already sees their effects. Then publish the TRUE
  // consumed position as rptr (M3.32, see WriteRingRptrLocked) -- while parked
  // at a WAIT_REG_MEM the ring must APPEAR unconsumed or the game recycles
  // command memory the walker has not parsed (the missing-draw blobs).
  static const bool async_cap = getenv("RESTUFF_ASYNC_CAPTURE") != nullptr;
  if (async_cap) {
    // M3.55: record the new write pointer LOCK-FREE and wake the worker; the
    // guest render thread returns immediately WITHOUT taking pm4_mutex. M3.44
    // took pm4_mutex here, but the worker holds it for the entire ~14ms walk --
    // so the kick blocked ~14ms anyway and async was neutral. The worker applies
    // pending_wptr_raw under the mutex before it walks, keeping ring state
    // consistent; the guest render thread now truly overlaps the walk.
    b.pending_wptr_raw.store(write_pointer, std::memory_order_release);
    {
      std::lock_guard<std::mutex> wl(b.capture_wake_mutex);
      b.capture_pending = true;
    }
    b.capture_wake_cv.notify_one();
    b.ring_kick_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  {
    // M3.38 diag: total wall time the KICKING THREAD (the guest's render
    // thread!) spends inside our walker+capture. This is pure added latency
    // on the game's most saturated thread -- surfaced as walker_ms in the
    // pm4 alive line to split "guest code slow" from "our capture slow".
    const auto kick_t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(b.pm4_mutex);
    if (!b.pm4_stack.empty()) {
      b.ring_wptr_words = write_pointer & (b.pm4_stack[0].size_words - 1);
      b.last_kick_wptr_raw = write_pointer;
      RunPm4Machine();
    }
    WriteRingRptrLocked();
    b.walker_us.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - kick_t0)
                              .count(),
                          std::memory_order_relaxed);
  }
  b.ring_kick_count.fetch_add(1, std::memory_order_relaxed);
}

// --- MMIO handlers (NB:2256-2294) ---------------------------------------------

uint32_t GpuMmioRead(void* /*ppc_context*/, void* /*ud*/, uint32_t addr) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00: return 0x08100748;           // RB_EDRAM_TIMING
    case 0x0F01: return 0x0000200E;           // RB_BC_CONTROL
    case 0x194C: return 720;                  // R500_D1MODE_V_COUNTER
    case 0x1951: return 1;                    // interrupt status: vblank pending
    case 0x1961: return (1280u << 16) | 720;  // AVIVO_D1MODE_VIEWPORT_SIZE
    case 0x0C44:
    case 0x0C45: {
      // PA_SC_VIZ_QUERY_STATUS_0/1 -- per-query visibility bits the game reads
      // to decide object/LIGHT assignment. PM4_VIZ_QUERY never fires on this
      // title, so these shadow words are stuck at 0 = "every query invisible",
      // which would permanently cull the sun light. Count the reads (do the
      // guest even ask?) and offer RESTUFF_VIZ_ALL=1 to answer "all visible".
      static std::atomic<uint64_t> s_viz_reads{0};
      const uint64_t n = s_viz_reads.fetch_add(1, std::memory_order_relaxed);
      if (getenv("RESTUFF_VIZ_CENSUS") && (n < 8 || (n & 0xFFFF) == 0))
        REXLOG_INFO("[VIZ] status read #{} reg=0x{:04X} shadow=0x{:08X}", n, r,
                    s_reg_shadow[r & 0x7FFF]);
      static const bool s_viz_all = getenv("RESTUFF_VIZ_ALL") != nullptr;
      if (s_viz_all) return 0xFFFFFFFFu;
      return s_reg_shadow[r & 0x7FFF];
    }
    default:     return s_reg_shadow[r & 0x7FFF];
  }
}

// M3.100: guest display gamma ramp (DC_LUT_*). Semantics ported from the SDK
// CommandProcessor::WriteRegister cases: SEQ_COLOR writes R,G,B in sequence at
// RW_INDEX honouring WRITE_EN_MASK (bit0=blue,1=green,2=red) then advances;
// 30_COLOR writes a packed 10:10:10 entry (blue bits 0-9, green 10-19, red
// 20-29) and advances. PWL mode is logged once and ignored (this title uses
// the 256-entry table). Version stays 0 until the guest writes -- the default
// ramp is identity, so the present path can skip the LUT entirely.
static std::mutex s_gamma_mutex;
static uint16_t s_gamma_tab[3][256];  // [0]=r [1]=g [2]=b, 10-bit
static std::atomic<uint32_t> s_gamma_version{0};
static uint32_t s_gamma_rw_component = 0;

static void HandleGammaRampWrite(uint32_t r, uint32_t value) {
  // Census: do the guest's DC_LUT pokes reach this hook at all? First few
  // writes in the DC block are logged unconditionally (cheap, boot-time only).
  if (r >= 0x1918 && r <= 0x1934) {
    static std::atomic<int> s_dclut_log{16};
    if (s_dclut_log.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[DCLUT] write reg=0x{:04X} value=0x{:08X}", r, value);
  }
  switch (r) {
    case 0x1922:  // DC_LUT_RW_INDEX: reset the sequential component cursor
      s_gamma_rw_component = 0;
      break;
    case 0x1923: {  // DC_LUT_SEQ_COLOR (256-entry mode)
      if (s_reg_shadow[0x1921] & 1) break;  // PWL mode selected
      const uint32_t idx = s_reg_shadow[0x1922] & 0xFF;
      const uint32_t comp = s_gamma_rw_component;  // 0=r 1=g 2=b
      const bool en = (s_reg_shadow[0x1927] >> (2 - comp)) & 1;
      if (en) {
        std::lock_guard<std::mutex> lk(s_gamma_mutex);
        s_gamma_tab[comp][idx] = uint16_t((value >> 6) & 0x3FF);
      }
      if (++s_gamma_rw_component >= 3) {
        s_gamma_rw_component = 0;
        WriteShadowRegister(0x1922, (s_reg_shadow[0x1922] & ~0xFFu) | ((idx + 1) & 0xFF));
      }
      if (en) s_gamma_version.fetch_add(1, std::memory_order_release);
      break;
    }
    case 0x1925: {  // DC_LUT_30_COLOR (256-entry mode, packed write)
      if (s_reg_shadow[0x1921] & 1) break;
      const uint32_t idx = s_reg_shadow[0x1922] & 0xFF;
      const uint32_t mask = s_reg_shadow[0x1927] & 7;
      if (mask) {
        std::lock_guard<std::mutex> lk(s_gamma_mutex);
        if (mask & 0b001) s_gamma_tab[2][idx] = uint16_t(value & 0x3FF);          // blue
        if (mask & 0b010) s_gamma_tab[1][idx] = uint16_t((value >> 10) & 0x3FF);  // green
        if (mask & 0b100) s_gamma_tab[0][idx] = uint16_t((value >> 20) & 0x3FF);  // red
      }
      s_gamma_rw_component = 0;
      WriteShadowRegister(0x1922, (s_reg_shadow[0x1922] & ~0xFFu) | ((idx + 1) & 0xFF));
      if (mask) s_gamma_version.fetch_add(1, std::memory_order_release);
      break;
    }
    case 0x1924: {  // DC_LUT_PWL_DATA -- not implemented (log once)
      static std::atomic<bool> s_pwl_warned{false};
      if (!s_pwl_warned.exchange(true))
        REXLOG_INFO("[native_vk] M3.100: guest wrote a PWL gamma ramp -- NOT IMPLEMENTED");
      break;
    }
    default:
      break;
  }
}

void GpuMmioWrite(void* /*ppc_context*/, void* /*ud*/, uint32_t addr, uint32_t value) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  WriteShadowRegister(r, value);  // scratch writeback happens inside
  HandleGammaRampWrite(r, value);
  if (r == kRegCpRbWptr) {
    OnRingBufferKick(value);
  } else {
    TryResumePm4();  // a register write may satisfy a parked WAIT
  }
}

// --- Interrupt dispatch + pump (NB:1335-1367, 2168-2254) -----------------------

void DispatchGuestInterrupt(uint32_t source, uint32_t cpu) {
  auto& b = B();
  const uint32_t callback = b.guest_callback.load(std::memory_order_acquire);
  if (!callback || !b.dispatcher) {
    return;
  }
  auto* thread = rex::system::XThread::GetCurrentThread();  // pump IS an XHostThread
  if (!thread) {
    return;
  }
  // Source-1 matters per-CPU: the game's ISR clears the pending-swap bit of
  // the CPU it believes it runs on.
  thread->SetActiveCpu(static_cast<uint8_t>(cpu));
  uint64_t args[] = {source, b.guest_callback_data.load(std::memory_order_acquire)};
  const uint64_t result =
      b.dispatcher->ExecuteInterrupt(thread->thread_state(), callback, args, 2);

  static std::atomic<uint32_t> logged_sources{0};
  const uint32_t bit = 1u << (source & 31u);
  if (!(logged_sources.fetch_or(bit, std::memory_order_relaxed) & bit)) {
    REXLOG_INFO("[native_vk] first source-{} interrupt dispatched (cb=0x{:08X}, result={})",
                source, callback, result);
  }
}

int PumpThreadMain() {
  using clock = std::chrono::steady_clock;
  // M3.131: the guest paces itself off OUR vblank. This title presents on
  // every SECOND vblank, so 60 Hz here is exactly the 30 fps cap the console
  // shipped with -- the renderer is not the limit, this timer is.
  // M4.6: default now comes from the vblank_hz CVAR (120 = 60 fps -- see the
  // cvar's comment for why this is a default and must stay one); the
  // RESTUFF_VBLANK_HZ env var is a per-run override for A/B (60 = retail).
  // Judged on motion across the prior uncap sessions and the PS3 SKU's native
  // 60: the simulation reads the guest timer, it does not count vblanks.
  static const double s_vblank_hz = [] {
    const char* e = getenv("RESTUFF_VBLANK_HZ");
    const double v = e ? atof(e) : double(REXCVAR_GET(vblank_hz));
    return (v >= 10.0 && v <= 480.0) ? v : 120.0;
  }();
  const auto kVblankInterval =
      std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / s_vblank_hz));

  auto& b = B();
  auto next_tick = clock::now();
  auto last_log = clock::now();
  uint64_t vblank_count = 0;

  REXLOG_INFO("[native_vk] pump thread running ({}Hz vblank)", s_vblank_hz);

  while (b.pump_running.load(std::memory_order_relaxed)) {
    // 1) source-1 interrupts with the CPU masks PM4 INTERRUPT requested.
    uint32_t int_mask = b.pending_interrupt_mask.exchange(0, std::memory_order_acq_rel);
    for (uint32_t cpu = 0; cpu < 6; ++cpu) {
      if (int_mask & (1u << cpu)) {
        DispatchGuestInterrupt(1, cpu);
      }
    }

    // 2) Fallback for streams without PM4 INTERRUPT packets: blanket-ack each
    // guest swap on all CPUs. Disabled once a real mask has been seen.
    //
    // M3.144: the drain was UNCONDITIONAL while the dispatch was not, so every
    // ack still queued at the instant `seen_pm4_interrupt` flipped was silently
    // thrown away -- a lost wakeup by construction, in a transition that
    // happens exactly once during early boot. That is the right shape for the
    // largest boot-failure cluster (guest stuck with swaps=0 while its ISR is
    // registered and firing). Hand the last pre-flip batch over instead of
    // dropping it, ONCE.
    // It must be once: M3.50/51 established that duplicate source-1 interrupts
    // HARD-CRASH the guest (source-1 is the real 1-per-completion interrupt),
    // so this must never become a blanket re-enable in steady state.
    static bool s_fallback_handed_over = false;
    const bool pm4_int = b.seen_pm4_interrupt.load(std::memory_order_relaxed);
    uint32_t swaps = b.pending_swap_acks.exchange(0, std::memory_order_acq_rel);
    if (!pm4_int || !s_fallback_handed_over) {
      while (swaps--) {
        for (uint32_t cpu = 0; cpu < 6; ++cpu) {
          DispatchGuestInterrupt(1, cpu);
        }
      }
      if (pm4_int) s_fallback_handed_over = true;  // that was the last batch
    }

    // 3) vblank on cpu 2 -- only when its 60Hz deadline arrives. M3.34 early
    // wakes (interrupt delivery) must not inflate the vblank rate the guest
    // paces itself against.
    const bool vblank_due = clock::now() >= next_tick;
    if (vblank_due) {
      DispatchGuestInterrupt(0, 2);
      ++vblank_count;
      b.vblank_counter.store(vblank_count, std::memory_order_relaxed);
    }

    // 4) Memory-poll WAIT conditions are invisible to the MMIO hooks;
    // re-evaluate parked streams every wake.
    const bool was_parked = b.pm4_parked.load(std::memory_order_acquire);
    TryResumePm4();
    // M3.47 probe: a re-eval that found the wait STILL unmet == genuine wait
    // time (fast-poll can't help); one that cleared it == detection latency the
    // poll just recovered. The ratio says whether fast-poll is worth its spin.
    if (was_parked) {
      if (b.pm4_parked.load(std::memory_order_acquire))
        s_park_reeval_unmet.fetch_add(1, std::memory_order_relaxed);
      else
        s_park_reeval_cleared.fetch_add(1, std::memory_order_relaxed);
    }

    const auto now = clock::now();
    if (now - last_log >= std::chrono::seconds(5)) {
      REXLOG_INFO(
          "[native_vk] alive: vblanks={} ring_kicks={} walker_ms={} pm4(fences={} swaps={} ibs={} draws={}/{} bin={}/{} parked={}) res={}/{}/{} dst=0x{:08X} cb=0x{:08X}",
          vblank_count, b.ring_kick_count.load(std::memory_order_relaxed),
          b.walker_us.exchange(0, std::memory_order_relaxed) / 1000,
          b.pm4_fence_writes.load(std::memory_order_relaxed),
          b.pm4_swaps.load(std::memory_order_relaxed),
          b.pm4_indirect_buffers.load(std::memory_order_relaxed),
          b.pm4_draws_captured.load(std::memory_order_relaxed),
          b.pm4_draw_indx.load(std::memory_order_relaxed),
          b.pm4_bin_draws.load(std::memory_order_relaxed),
          b.pm4_bin_captured.load(std::memory_order_relaxed),
          b.pm4_parked.load(std::memory_order_relaxed),
          s_res_seen.load(std::memory_order_relaxed),
          s_res_sub.load(std::memory_order_relaxed),
          s_res_drop.load(std::memory_order_relaxed),
          s_res_last_dst.load(std::memory_order_relaxed),
          b.guest_callback.load(std::memory_order_relaxed));
      // M3.35: park latency since the last report (max + mean); the pool
      // overwrite window is exactly this.
      const uint64_t pc = s_park_count.exchange(0, std::memory_order_relaxed);
      const int64_t pt = s_park_total_us.exchange(0, std::memory_order_relaxed);
      const int64_t pm = s_park_max_us.exchange(0, std::memory_order_relaxed);
      const uint64_t rvu = s_park_reeval_unmet.exchange(0, std::memory_order_relaxed);
      const uint64_t rvc = s_park_reeval_cleared.exchange(0, std::memory_order_relaxed);
      if (pc)
        REXLOG_INFO("[native_vk] park latency: n={} mean={}us max={}us reeval(unmet={} cleared={})",
                    pc, pt / int64_t(pc), pm, rvu, rvc);
      last_log = now;
    }

    if (vblank_due) {
      next_tick += kVblankInterval;
      if (next_tick < now) {
        next_tick = now;  // fell behind; don't burst-dispatch
      }
    }
    // M3.34: sleep interruptibly -- a walker-side notify (interrupt/swap-ack
    // queued) wakes the loop at once so the guest ISR runs with ~zero added
    // latency; without a signal this behaves exactly like the old
    // sleep_until(next_tick). Spurious wakes just cost one empty pass.
    // M3.47: when the PM4 machine is PARKED at a memory-poll WAIT_REG_MEM, the
    // condition is satisfied by a guest memory write the MMIO hooks can't see,
    // so ONLY this loop's TryResumePm4() re-evaluation catches it. Sleeping to
    // the next 60Hz vblank meant a parked wait cost up to 16.7ms to notice
    // (measured on a slower host: park n=1647 mean=2104us max=16738us per 5s =
    // ~3.4s of every 5s the frame can't finish -- park LATENCY, not walker CPU,
    // was the ~20fps ceiling there; walker_ms was only 29% of a core). While
    // parked, poll every kParkPollUs so the wait resolves in a fraction of a ms;
    // fall back to the vblank deadline when not parked so an idle GPU never
    // spins. Re-eval is cheap (TryResumePm4 re-enters at the parked packet, does
    // one EvaluateWaitRegMem, returns if still unmet).
    constexpr auto kParkPollUs = std::chrono::microseconds(100);
    // M3.47: OPT-IN (RESTUFF_PARK_FASTPOLL=1). Default OFF: the reeval probe
    // showed the parked WAIT_REG_MEM conditions (guest-RAM fences at
    // 0x1FCA3002/6) stay unmet ~200 re-evals per park == GENUINE waits, not
    // detection latency, so fast-polling can't shorten them and only spends
    // ~6200 locked RunPm4Machine re-entries/s (risking audio-thread starvation
    // on slower hosts). Kept behind an env toggle for A/B in the heavy
    // many-short-parks regime (the user's cutscene) which headless nav can't
    // reliably reach; DO NOT default-on without proving it helps there.
    static const bool fastpoll = getenv("RESTUFF_PARK_FASTPOLL") != nullptr;
    {
      std::unique_lock<std::mutex> lk(b.pump_wake_mutex);
      auto deadline = next_tick;
      if (fastpoll && b.pm4_parked.load(std::memory_order_acquire)) {
        const auto fast = clock::now() + kParkPollUs;
        if (fast < deadline) deadline = fast;
      }
      b.pump_wake_cv.wait_until(lk, deadline);
    }
  }
  return 0;
}

void StartPumpThreadOnce() {
  auto& b = B();
  if (b.pump_running.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  b.pump_thread = rex::system::object_ref<rex::system::XHostThread>(
      new rex::system::XHostThread(b.kernel_state, 128 * 1024, 0, [] { return PumpThreadMain(); }));
  b.pump_thread->set_name("NativeVk GPU Pump");
  b.pump_thread->Create();
}

}  // namespace

// M3.100 accessors (external linkage; the table lives in the anonymous ns).
uint32_t GetGammaRampVersion() { return s_gamma_version.load(std::memory_order_acquire); }
void CopyGammaRamp(uint32_t* packed256) {
  std::lock_guard<std::mutex> lk(s_gamma_mutex);
  for (int i = 0; i < 256; ++i)
    packed256[i] = uint32_t(s_gamma_tab[0][i]) | (uint32_t(s_gamma_tab[1][i]) << 10) |
                   (uint32_t(s_gamma_tab[2][i]) << 20);
}

// --- Public API ---------------------------------------------------------------

void Initialize(rex::runtime::FunctionDispatcher* dispatcher,
                rex::system::KernelState* kernel_state) {
  auto& b = B();
  if (b.active.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  b.dispatcher = dispatcher;
  b.kernel_state = kernel_state;

  // Sane defaults the game may read before writing (RB_COLOR_MASK).
  s_reg_shadow[0x2104] = 0xF;

  // Claim the GPU register aperture. Must precede guest boot: REX_MM_LOAD_U32
  // returns uninitialized stack garbage when no range is registered.
  auto* memory = rex::Runtime::instance()->memory();
  const bool ok = memory->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, nullptr,
                                                GpuMmioRead, GpuMmioWrite);
  REXLOG_INFO("[native_vk] life-support up: MMIO 0x7FC80000 claim {}", ok ? "OK" : "FAILED");
}

void Shutdown() {
  auto& b = B();
  if (b.pump_running.exchange(false, std::memory_order_acq_rel)) {
    if (b.pump_thread) {
      b.pump_thread->Wait(0, 0, 0, nullptr);
      b.pump_thread.reset();
    }
  }
  b.active.store(false, std::memory_order_release);
}

void SetGuestInterruptCallback(uint32_t callback, uint32_t user_data) {
  auto& b = B();
  // Data first, then callback (dispatch reads callback as the gate).
  b.guest_callback_data.store(user_data, std::memory_order_release);
  b.guest_callback.store(callback, std::memory_order_release);
  REXLOG_INFO("[native_vk] guest interrupt callback = 0x{:08X} (data=0x{:08X})", callback,
              user_data);
  // Lazy pump start: the guest is provably live and kernel/dispatcher tables
  // are initialized by the time it registers its ISR.
  StartPumpThreadOnce();
  StartCaptureWorkerOnce();  // M3.44: no-op unless RESTUFF_ASYNC_CAPTURE=1
}

void SetRingBuffer(uint32_t physical_base, uint32_t size_log2) {
  auto& b = B();
  if (!b.active.load(std::memory_order_acquire)) {
    return;
  }
  std::lock_guard<std::mutex> lock(b.pm4_mutex);
  StreamFrame ring;
  ring.base_phys = physical_base & 0x1FFFFFFF;
  // Kernel convention: size in bytes = 1 << (size_log2 + 3).
  ring.size_words = (1u << (size_log2 + 3)) / 4;
  ring.pos = 0;
  ring.is_ring = true;
  b.pm4_stack.clear();
  b.pm4_stack.reserve(8);
  b.pm4_stack.push_back(ring);
  b.ring_wptr_words = 0;
  b.pending_wptr_raw.store(0, std::memory_order_relaxed);  // M3.55
  b.pm4_parked.store(false, std::memory_order_relaxed);
  REXLOG_INFO("[native_vk] ring buffer at phys 0x{:08X}, {} words (size_log2={})",
              physical_base, ring.size_words, size_log2);
}

void SetRingWritebackSlot(uint32_t physical_address, uint32_t block_size_log2) {
  auto& b = B();
  if (!b.active.load(std::memory_order_acquire)) {
    return;
  }
  b.ring_writeback_addr.store(physical_address & 0x1FFFFFFF, std::memory_order_release);
  REXLOG_INFO("[native_vk] ring rptr writeback slot at phys 0x{:08X} (block_size_log2={})",
              physical_address, block_size_log2);
}

uint32_t GetShadowRegRaw(uint32_t reg) { return s_reg_shadow[reg & 0x7FFF]; }

bool FindShaderByPhys(uint32_t phys_addr, GuestShaderInfo& out) {
  std::lock_guard<std::mutex> lock(s_shader_mutex);
  const auto it = s_shaders_by_phys.find(phys_addr);
  if (it == s_shaders_by_phys.end()) {
    return false;
  }
  out = it->second;
  return true;
}

uint64_t FindShaderInRegion(const uint32_t* be_region, uint32_t region_words) {
  if (!be_region || region_words < 4) {
    return 0;
  }
  std::lock_guard<std::mutex> lock(s_shader_mutex);
  for (uint32_t off = 0; off + 4 <= region_words; ++off) {
    for (const ShaderBlob& blob : s_shader_blobs) {
      const uint32_t n = blob.info.size_dwords;
      if (n < 4 || off + n > region_words) continue;
      const uint32_t* w = blob.be_words.data();
      if (be_region[off] != w[0] || be_region[off + 1] != w[1] ||
          be_region[off + 2] != w[2] || be_region[off + 3] != w[3]) {
        continue;
      }
      if (HashUcode(be_region + off, n) == blob.info.hash) {
        return blob.info.hash;
      }
    }
  }
  return 0;
}

void GetFrontbuffer(uint32_t& phys, uint32_t& width, uint32_t& height) {
  auto& b = B();
  phys = b.frontbuffer_phys.load(std::memory_order_acquire);
  width = b.frontbuffer_w.load(std::memory_order_relaxed);
  height = b.frontbuffer_h.load(std::memory_order_relaxed);
}

}  // namespace restuff::native

namespace restuff::renderer {
// Dump-time live guest-memory reads (up_draws.h): catches UP payloads the
// game commits after the draw packet was walked.
const uint8_t* DebugGuestPhysPtr(uint32_t phys) {
  auto* memory = rex::Runtime::instance()->memory();
  return (memory && phys) ? memory->TranslatePhysical<const uint8_t*>(phys) : nullptr;
}
uint8_t* GuestPhysPtrMut(uint32_t phys) {
  auto* memory = rex::Runtime::instance()->memory();
  return (memory && phys) ? memory->TranslatePhysical<uint8_t*>(phys) : nullptr;
}
}  // namespace restuff::renderer

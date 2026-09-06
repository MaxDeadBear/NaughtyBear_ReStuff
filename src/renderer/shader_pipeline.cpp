// M2.3 Stage 2: runtime GLSL -> SPIR-V via libshaderc. See shader_pipeline.h.
#include "renderer/shader_pipeline.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <shaderc/shaderc.h>

#include <rex/logging.h>

namespace restuff::renderer::spc {

// M3.82: insert the float-controls capabilities, extension, and execution
// modes into a compiled SPIR-V module. Section-order-preserving single pass:
// capabilities lead the module (opcode 17); the extension (10) belongs
// immediately after them; execution modes (16) follow the entry points (15).
static void InjectXenosFloatControls(std::vector<uint32_t>& spv) {
  if (spv.size() < 6) return;
  std::vector<uint32_t> out;
  out.reserve(spv.size() + 20);
  out.insert(out.end(), spv.begin(), spv.begin() + 5);
  size_t i = 5;
  bool caps_done = false, modes_done = false;
  uint32_t entry_id = 0;
  while (i < spv.size()) {
    const uint32_t op = spv[i] & 0xFFFFu;
    const uint32_t len = spv[i] >> 16;
    if (len == 0 || i + len > spv.size()) return;  // malformed: leave untouched
    if (!caps_done && op != 17) {
      out.push_back((2u << 16) | 17); out.push_back(4465);  // DenormFlushToZero
      out.push_back((2u << 16) | 17); out.push_back(4466);  // SignedZeroInfNanPreserve
      const char ext[] = "SPV_KHR_float_controls";  // 22 chars + NUL -> 6 words
      uint32_t w[6] = {};
      std::memcpy(w, ext, sizeof(ext));
      out.push_back((7u << 16) | 10);
      for (int k = 0; k < 6; ++k) out.push_back(w[k]);
      caps_done = true;
    }
    if (op == 15 && !entry_id) entry_id = spv[i + 2];
    if (!modes_done && entry_id && op != 15 && op != 16) {
      out.push_back((4u << 16) | 16); out.push_back(entry_id);
      out.push_back(4460); out.push_back(32);  // DenormFlushToZero 32
      out.push_back((4u << 16) | 16); out.push_back(entry_id);
      out.push_back(4461); out.push_back(32);  // SignedZeroInfNanPreserve 32
      modes_done = true;
    }
    out.insert(out.end(), spv.begin() + i, spv.begin() + i + len);
    i += len;
  }
  if (caps_done && modes_done) spv.swap(out);
}

// --- M4.40: persistent SPIR-V cache ----------------------------------------
namespace {

struct SpvCache {
  std::mutex mu;
  std::unordered_map<uint64_t, std::vector<uint32_t>> map;
  bool loaded = false;
  bool dirty = false;
  uint32_t hits = 0, misses = 0;
};
SpvCache& Spv() {
  static SpvCache c;
  return c;
}

bool SpvCacheDisabled() {
  static const bool off = getenv("RESTUFF_NO_SPVCACHE") != nullptr;
  return off;
}

std::string SpvCachePath() {
  if (const char* e = getenv("RESTUFF_SPVCACHE_FILE")) return e;
  // Anchored to the exe directory, same convention as pipeline_cache.bin /
  // pipeline_prewarm.bin (a CWD-relative path silently misses when launched
  // from elsewhere).
  std::string d;
#if defined(_WIN32)
  char buf[1024];
  const unsigned long n = GetModuleFileNameA(nullptr, buf, sizeof(buf));
  if (n > 0 && n < sizeof(buf)) d.assign(buf, n);
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) d.assign(buf, size_t(n));
#endif
  const size_t cut = d.find_last_of("/\\");
  return (cut == std::string::npos ? std::string() : d.substr(0, cut + 1)) + "shader_spv.bin";
}

uint64_t Fnv1a64(const void* p, size_t n) {
  const auto* b = static_cast<const unsigned char*>(p);
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= b[i];
    h *= 1099511628211ull;
  }
  return h;
}

constexpr uint32_t kSpvMagic = 0x56505352;  // "RSPV"
constexpr uint32_t kSpvVersion = 2;  // M4.45: v2 adds the build-id field

}  // namespace

uint64_t ShaderBuildId() {
  static const uint64_t id = [] {
    const char* a = ucode::TranslatorBuildStamp();
    const char* b = __DATE__ " " __TIME__;
    return Fnv1a64(a, std::strlen(a)) ^ (Fnv1a64(b, std::strlen(b)) * 0x9E3779B97F4A7C15ull);
  }();
  return id;
}

void LoadShaderSpvCache() {
  auto& c = Spv();
  std::lock_guard<std::mutex> lk(c.mu);
  if (c.loaded) return;
  c.loaded = true;
  if (SpvCacheDisabled()) return;
  const std::string path = SpvCachePath();
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return;  // absent is normal: the run just populates it
  uint32_t hdr[3] = {};
  uint64_t build_id = 0;
  size_t entries = 0;
  // M4.45: a cache written by a build whose shader emitters differ would keep
  // every old entry forever (keys are GLSL hashes; nothing evicts). Discard it
  // whole -- the re-warm from the manifest is ~2s -- rather than let each
  // release append another dead layer.
  if (std::fread(hdr, 4, 3, f) == 3 && hdr[0] == kSpvMagic && hdr[1] == kSpvVersion &&
      std::fread(&build_id, 8, 1, f) == 1 && build_id != ShaderBuildId()) {
    REXLOG_INFO("[native_vk] M4.45 SPIR-V cache is from a different build: discarding {}",
                path);
  } else if (hdr[0] == kSpvMagic && hdr[1] == kSpvVersion && build_id == ShaderBuildId()) {
    for (uint32_t i = 0; i < hdr[2]; ++i) {
      uint64_t key = 0;
      uint32_t words = 0;
      if (std::fread(&key, 8, 1, f) != 1 || std::fread(&words, 4, 1, f) != 1) break;
      // Guard a corrupt/truncated file: a bogus length would allocate wildly.
      if (words == 0 || words > (16u << 20)) break;
      std::vector<uint32_t> spv(words);
      if (std::fread(spv.data(), 4, words, f) != words) break;
      c.map.emplace(key, std::move(spv));
      ++entries;
    }
  }
  std::fclose(f);
  REXLOG_INFO("[native_vk] M4.40 SPIR-V cache loaded {} entries from {}", entries, path);
}

void SaveShaderSpvCache() {
  auto& c = Spv();
  std::lock_guard<std::mutex> lk(c.mu);
  if (SpvCacheDisabled() || !c.dirty || c.map.empty()) return;
  const std::string path = SpvCachePath();
  // Write-then-rename so an interrupted save cannot leave a truncated cache
  // that the next boot would parse as far as it could and then run short.
  const std::string tmp = path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (!f) {
    REXLOG_ERROR("[native_vk] M4.40 SPIR-V cache save failed to open {}", tmp);
    return;
  }
  const uint32_t hdr[3] = {kSpvMagic, kSpvVersion, uint32_t(c.map.size())};
  const uint64_t build_id = ShaderBuildId();
  bool ok = std::fwrite(hdr, 4, 3, f) == 3 && std::fwrite(&build_id, 8, 1, f) == 1;
  for (const auto& [key, spv] : c.map) {
    const uint32_t words = uint32_t(spv.size());
    ok = ok && std::fwrite(&key, 8, 1, f) == 1 && std::fwrite(&words, 4, 1, f) == 1 &&
         std::fwrite(spv.data(), 4, words, f) == words;
    if (!ok) break;
  }
  ok = (std::fclose(f) == 0) && ok;
  if (!ok) {
    std::remove(tmp.c_str());
    REXLOG_ERROR("[native_vk] M4.40 SPIR-V cache write failed, kept previous {}", path);
    return;
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::remove(tmp.c_str());
    REXLOG_ERROR("[native_vk] M4.40 SPIR-V cache rename failed: {}", ec.message());
    return;
  }
  c.dirty = false;
  REXLOG_INFO("[native_vk] M4.40 SPIR-V cache saved {} entries ({} hits / {} compiled this run)",
              c.map.size(), c.hits, c.misses);
}

std::vector<uint32_t> CompileGlsl(const char* glsl, bool is_vertex, bool is_compute) {
  std::vector<uint32_t> spv;
  // M3.48: reuse a per-thread compiler. shaderc_compiler_initialize() builds
  // glslang's builtin symbol tables and was being paid PER shader; the threads
  // that call this (pipeline pool + walker) are long-lived, so keep one compiler
  // each (leaked at thread exit -- bounded, freed at process teardown).
  thread_local shaderc_compiler_t compiler = shaderc_compiler_initialize();
  if (!compiler) {
    REXLOG_ERROR("[native_vk] shaderc_compiler_initialize failed");
    return spv;
  }
  shaderc_compile_options_t opts = shaderc_compile_options_initialize();
  shaderc_compile_options_set_target_env(opts, shaderc_target_env_vulkan,
                                         shaderc_env_version_vulkan_1_0);
  // M3.48: compile-time optimization level. Was `performance`, which made
  // shaderc spend 100-250ms on the big (10k-word) cutscene pixel shaders -- and
  // translation is single-threaded, so a scene's worth of new shaders serialized
  // into the multi-second "choppy open" the user sees. The Vulkan driver runs
  // its OWN optimizing compiler on the SPIR-V at vkCreateGraphicsPipeline
  // (on the parallel pipeline pool) regardless, so shaderc's performance pass is
  // largely redundant; `zero` slashes compile time with negligible runtime cost.
  // RESTUFF_SHADERC_OPT=1 restores the old performance level for A/B.
  static const bool opt_perf = getenv("RESTUFF_SHADERC_OPT") != nullptr;
  shaderc_compile_options_set_optimization_level(
      opts, opt_perf ? shaderc_optimization_level_performance : shaderc_optimization_level_zero);

  const shaderc_shader_kind kind =
      is_compute  ? shaderc_glsl_compute_shader
      : is_vertex ? shaderc_glsl_vertex_shader
                  : shaderc_glsl_fragment_shader;

  // M4.40: key on the GENERATED GLSL plus every flag that changes the emitted
  // SPIR-V (stage, shaderc opt level, float-controls injection). Keying on the
  // source text rather than the guest ucode hash is what makes the cache
  // self-invalidating: any translator change produces different GLSL, hence a
  // different key, hence a recompile -- there is no stale-shader failure mode.
  static const bool no_fctl_key = getenv("RESTUFF_NO_FLOATCTL") != nullptr;
  const size_t glsl_len = std::strlen(glsl);
  uint64_t key = Fnv1a64(glsl, glsl_len);
  const uint32_t flags =
      uint32_t(kind) | (opt_perf ? 0x1000u : 0u) | (no_fctl_key ? 0x2000u : 0u);
  key ^= Fnv1a64(&flags, sizeof(flags));
  if (!SpvCacheDisabled()) {
    LoadShaderSpvCache();
    auto& c = Spv();
    std::lock_guard<std::mutex> lk(c.mu);
    auto it = c.map.find(key);
    if (it != c.map.end()) {
      ++c.hits;
      shaderc_compile_options_release(opts);
      return it->second;  // already post-float-controls: exactly what shipped before
    }
  }

  const auto t0 = std::chrono::steady_clock::now();
  shaderc_compilation_result_t res = shaderc_compile_into_spv(
      compiler, glsl, std::strlen(glsl), kind, "guest_shader", "main", opts);
  const auto comp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
  if (comp_ms >= 20)
    REXLOG_INFO("[native_vk] shaderc {} compile {}ms (opt={})", is_vertex ? "VS" : "PS",
                comp_ms, opt_perf ? "perf" : "zero");

  if (shaderc_result_get_compilation_status(res) != shaderc_compilation_status_success) {
    REXLOG_ERROR("[native_vk] shaderc compile error: {}", shaderc_result_get_error_message(res));
  } else {
    const size_t nbytes = shaderc_result_get_length(res);
    const auto* words = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(res));
    spv.assign(words, words + nbytes / 4);
    // M3.82: stamp Xenos float semantics on the module — the SDK reference
    // sets DenormFlushToZero(32) + SignedZeroInfNanPreserve(32) execution
    // modes on its SPIR-V (spirv_translator.cpp:1166-1177; Xenos ALUs flush
    // denormals, signed zero carries VFACE). Our shaderc output sets
    // neither, and the terrain lighting runs in LOG2 space (exp2 finale,
    // 1/ln2 literal), where float-control differences become MULTIPLICATIVE
    // luminance shifts. Injects SPV_KHR_float_controls capabilities
    // (4465/4466) + the extension string + per-entry execution modes
    // (4460/4461, width 32). RESTUFF_NO_FLOATCTL=1 opts out.
    static const bool no_fctl = getenv("RESTUFF_NO_FLOATCTL") != nullptr;
    if (!no_fctl) InjectXenosFloatControls(spv);
    // M4.40: store the FINAL module (post-injection) so a cache hit returns
    // byte-for-byte what this path would have produced.
    if (!SpvCacheDisabled() && !spv.empty()) {
      auto& c = Spv();
      std::lock_guard<std::mutex> lk(c.mu);
      if (c.map.emplace(key, spv).second) {
        ++c.misses;
        c.dirty = true;
      }
    }
  }
  shaderc_result_release(res);
  shaderc_compile_options_release(opts);
  // compiler is thread_local and reused across calls -- do NOT release here.
  return spv;
}

namespace {
// GetShader inserts on the PM4 walker thread while GetCachedShader reads on the
// present thread: guard the map (element pointers stay stable across rehash,
// so returned pointers remain valid outside the lock).
std::mutex g_shader_cache_mutex;
std::unordered_map<uint64_t, CachedShader> g_shader_cache;
CachedShader g_null_shader;  // returned for a miss
}  // namespace

const CachedShader* GetCachedShader(uint64_t hash) {
  std::lock_guard<std::mutex> lock(g_shader_cache_mutex);
  auto it = g_shader_cache.find(hash);
  return it == g_shader_cache.end() ? nullptr : &it->second;
}

const CachedShader& GetShader(uint64_t hash, bool is_vertex, const uint32_t* be_words,
                              uint32_t num_dwords) {
  auto& cache = g_shader_cache;
  {
    std::lock_guard<std::mutex> lock(g_shader_cache_mutex);
    auto it = cache.find(hash);
    if (it != cache.end()) return it->second;
  }

  CachedShader cs;
  // RESTUFF_DUMP_UCODE=1: write each shader's raw big-endian ucode words once
  // (ucode_<vs|ps>_<hash>.bin) -- feeds the offline ShaderInterpreter harness.
  if (getenv("RESTUFF_DUMP_UCODE")) {
    char path[96];
    snprintf(path, sizeof(path), "ucode_%s_%016llx.bin", is_vertex ? "vs" : "ps",
             (unsigned long long)hash);
    if (FILE* fp = fopen(path, "wb")) {
      fwrite(be_words, 4, num_dwords, fp);
      fclose(fp);
    }
  }
  // M4.40: time the two stages separately. They have very different fixes --
  // translation is our code, shaderc is a third-party compile -- and an offline
  // shader cache only pays for whichever one dominates.
  using shclock = std::chrono::steady_clock;
  const auto t_begin = shclock::now();
  cs.t = is_vertex ? ucode::TranslateVertexShader(be_words, num_dwords)
                   : ucode::TranslatePixelShader(be_words, num_dwords);
  const auto t_translated = shclock::now();
  if (cs.t.ok) {
    cs.spirv = CompileGlsl(cs.t.glsl.c_str(), is_vertex);
    cs.valid = !cs.spirv.empty();
    // M3.60: compile the compute winding-probe variant (present only for
    // non-skinned matrix VSes). Failure is non-fatal -- the draw just won't
    // get an automatic winding flip.
    if (cs.valid && !cs.t.probe_glsl.empty())
      cs.probe_spirv = CompileGlsl(cs.t.probe_glsl.c_str(), /*is_vertex=*/false, /*is_compute=*/true);
  }
  // M4.32: RESTUFF_DUMP_GLSL=1 writes each translated shader's GLSL and final
  // SPIR-V (post float-controls injection -- byte-identical to what the driver
  // gets) into glsl_dump/ next to the exe. Feeds the offline AMD-compiler
  // harness (tools/ally_shader_audit.sh) that measures scratch spill for the
  // Ally's gfx1103 without needing the hardware.
  if (cs.valid && getenv("RESTUFF_DUMP_GLSL")) {
    std::error_code ec;
    std::filesystem::create_directories("glsl_dump", ec);
    char path[96];
    snprintf(path, sizeof(path), "glsl_dump/%s_%016llx.%s", is_vertex ? "vs" : "ps",
             (unsigned long long)hash, is_vertex ? "vert" : "frag");
    if (FILE* fp = fopen(path, "wb")) {
      fwrite(cs.t.glsl.data(), 1, cs.t.glsl.size(), fp);
      fclose(fp);
    }
    snprintf(path, sizeof(path), "glsl_dump/%s_%016llx.spv", is_vertex ? "vs" : "ps",
             (unsigned long long)hash);
    if (FILE* fp = fopen(path, "wb")) {
      fwrite(cs.spirv.data(), 4, cs.spirv.size(), fp);
      fclose(fp);
    }
    if (!cs.probe_spirv.empty()) {
      snprintf(path, sizeof(path), "glsl_dump/probe_%016llx.spv", (unsigned long long)hash);
      if (FILE* fp = fopen(path, "wb")) {
        fwrite(cs.probe_spirv.data(), 4, cs.probe_spirv.size(), fp);
        fclose(fp);
      }
    }
  }
  if (!cs.valid) {
    REXLOG_ERROR("[native_vk] shader translate/compile failed hash={:016X} {}: {}", hash,
                 is_vertex ? "VS" : "PS", cs.t.error.empty() ? "compile" : cs.t.error);
  } else {
    const auto us = [](auto a, auto b) {
      return std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    };
    const auto t_end = shclock::now();
    // M4.40: running totals, so one line at the end of a run sizes the whole
    // cost instead of needing the per-shader lines summed by hand.
    static std::atomic<uint64_t> s_n{0}, s_tr_us{0}, s_cc_us{0};
    const uint64_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const uint64_t tr = s_tr_us.fetch_add(uint64_t(us(t_begin, t_translated)),
                                          std::memory_order_relaxed) +
                        uint64_t(us(t_begin, t_translated));
    const uint64_t cc =
        s_cc_us.fetch_add(uint64_t(us(t_translated, t_end)), std::memory_order_relaxed) +
        uint64_t(us(t_translated, t_end));
    REXLOG_INFO("[native_vk] translated {} hash={:016X} -> {} spirv words ({} attrs {} tex) "
                "translate={}us compile={}us | total n={} translate={}ms compile={}ms{}",
                is_vertex ? "VS" : "PS", hash, cs.spirv.size(), cs.t.attrs.size(),
                cs.t.textures.size(), us(t_begin, t_translated), us(t_translated, t_end), n,
                tr / 1000, cc / 1000, cs.t.error.empty() ? "" : (" WARN:" + cs.t.error));
  }
  std::lock_guard<std::mutex> lock(g_shader_cache_mutex);
  auto [ins, _] = cache.emplace(hash, std::move(cs));
  return ins->second;
}

bool RuntimeCompileSelfTest() {
  static const char* kVS =
      "#version 450\n"
      "layout(location=0) in vec2 p;\n"
      "void main(){ gl_Position = vec4(p, 0.0, 1.0); }\n";
  static const char* kFS =
      "#version 450\n"
      "layout(location=0) out vec4 c;\n"
      "void main(){ c = vec4(1.0); }\n";
  auto vs = CompileGlsl(kVS, true);
  auto fs = CompileGlsl(kFS, false);
  const bool ok = !vs.empty() && !fs.empty();
  if (ok)
    REXLOG_INFO("[native_vk] libshaderc runtime-compile self-test OK (vs={} fs={} words)",
                vs.size(), fs.size());
  else
    REXLOG_ERROR("[native_vk] libshaderc runtime-compile self-test FAILED");
  return ok;
}

}  // namespace restuff::renderer::spc

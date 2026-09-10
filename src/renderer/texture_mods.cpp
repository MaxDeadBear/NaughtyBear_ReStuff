// Texture replacement loading. See texture_mods.h for the design notes.
//
// Windows decodes through WIC, which costs no third-party dependency and
// accepts whatever the user's editor saved (PNG, TGA, BMP, TIFF, ...). On
// other platforms replacement is compiled out and the guest texture is used
// unchanged -- dumping still works everywhere, since that is a hand-rolled TGA
// writer.

#include "renderer/texture_mods.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>
#include <unordered_set>

#ifdef _WIN32
#include <objbase.h>
#include <wincodec.h>
#endif

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>
#include <rex/logging.h>

// Settable from restuff.toml next to the exe (or the F4 settings UI); the
// RESTUFF_TEX_* env vars still work as an override for one-off runs.
REXCVAR_DEFINE_BOOL(tex_dump, false, "Modding",
    "Dump every decoded guest texture to texture_dump/<contenthash>.tga (32-bit, "
    "alpha included). Read once at the first texture upload.");
REXCVAR_DEFINE_STRING(tex_dump_format, "tga", "Modding",
    "Format for tex_dump output: \"tga\" (uncompressed, works on any platform) or "
    "\"png\" (much smaller, Windows only -- falls back to TGA elsewhere).");
REXCVAR_DEFINE_BOOL(tex_mods, false, "Modding",
    "Load texture replacements from texture_mods/<contenthash>.png (or .tga/.bmp/"
    ".tif/.jpg). Any size is fine -- guest UVs are normalised, so upscales just work.");

namespace restuff::renderer::texmod {

namespace {

std::mutex g_mutex;
std::unordered_map<uint64_t, std::shared_ptr<const Replacement>> g_cache;

// --- asynchronous loading ---------------------------------------------------
// Decoding used to happen inline in FindReplacement, which runs on the PRESENT
// thread inside the draw path -- so every replacement load blocked a frame.
// Measured: 5522 decodes across 750 textures, all on present threads; that is
// the hitching, and it was independent of texture size (shrinking 4K->2K only
// shortened each stall). Now the render thread never decodes: an unresolved
// hash is queued, the guest texture is used for a few frames, and the worker
// bumps that hash's generation when it lands so the existing live-reload path
// swaps it in.
std::unordered_set<uint64_t> g_pending;  // queued or in flight
std::deque<uint64_t> g_queue;
std::condition_variable g_cv;
std::atomic<bool> g_worker_started{false};
std::atomic<bool> g_mods_requested{false};
void EnsureLoaderStarted();

// F8 toggles the SAME cvar the settings checkbox binds to, so the overlay and
// the hotkey can never disagree. Registered at STATIC INIT on the main thread:
// registering lazily from the render thread compiled and ran but never
// dispatched, because binds must exist before input routing starts.
const bool s_texmods_bind_registered = [] {
  rex::ui::RegisterBind("bind_tex_mods", "F8", "Toggle texture replacements", [] {
    const bool now = !REXCVAR_GET(tex_mods);
    REXCVAR_SET(tex_mods, now);
    REXLOG_INFO("[texmod] replacements toggled {} (F8)", now ? "ON" : "OFF");
  });
  return true;
}();

// Global generation: bumps on ANY mod-folder change. Used as a cheap gate so
// the per-hash table below is only consulted right after something moved.
std::atomic<uint32_t> g_mod_gen{1};
// hash -> how many times that texture's file has changed. Absent == 0.
std::unordered_map<uint64_t, uint32_t> g_hash_gen;
// hash -> last seen mtime, for diffing the directory between polls (and for
// knowing which textures to revert when mods are switched off).
std::unordered_map<uint64_t, uint64_t> g_seen;

// "6FB29799BF5A602C.png" -> 0x6FB29799BF5A602C. Strict: anything not named like
// a dump is ignored, so a stray notes.txt in the folder cannot invalidate some
// arbitrary texture.
bool HashFromName(const std::filesystem::path& p, uint64_t& out) {
  const std::string stem = p.stem().string();
  if (stem.size() != 16) return false;
  out = 0;
  for (char c : stem) {
    const int v = (c >= '0' && c <= '9')   ? c - '0'
                  : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                  : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                           : -1;
    if (v < 0) return false;
    out = (out << 4) | uint64_t(v);
  }
  return true;
}

// Any extension WIC can open. Ordered so the lossless ones win when a user has
// left several files for the same hash lying around.
constexpr const char* kExts[] = {".png", ".tga", ".bmp", ".tif", ".tiff", ".jpg", ".jpeg"};

#ifdef _WIN32
// Straight (non-premultiplied) RGBA8, matching what the decode path produces.
bool DecodeFile(const std::filesystem::path& path, std::vector<uint8_t>& out, uint32_t& w,
                uint32_t& h) {
  out.clear();
  w = h = 0;
  // The render thread is not guaranteed to have COM initialised; this is
  // idempotent and we deliberately do NOT uninitialise (other subsystems --
  // the video player's MF/XAudio2 -- rely on the apartment staying up).
  // THREAD_LOCAL: COM apartments are per-thread. A plain function-local
  // static would initialise once for the process, leaving the loader thread
  // without an apartment and every CoCreateInstance failing there.
  static thread_local const bool s_com = [] {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
  }();
  (void)s_com;

  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))))
    return false;
  bool ok = false;
  IWICBitmapDecoder* dec = nullptr;
  IWICBitmapFrameDecode* frame = nullptr;
  IWICFormatConverter* conv = nullptr;
  if (SUCCEEDED(factory->CreateDecoderFromFilename(path.wstring().c_str(), nullptr, GENERIC_READ,
                                                   WICDecodeMetadataCacheOnDemand, &dec)) &&
      SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
      SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                                 WICBitmapDitherTypeNone, nullptr, 0.0,
                                 WICBitmapPaletteTypeCustom))) {
    UINT uw = 0, uh = 0;
    if (SUCCEEDED(conv->GetSize(&uw, &uh)) && uw && uh) {
      out.resize(size_t(uw) * uh * 4);
      if (SUCCEEDED(conv->CopyPixels(nullptr, uw * 4, UINT(out.size()), out.data()))) {
        w = uw;
        h = uh;
        ok = true;
      } else {
        out.clear();
      }
    }
  }
  if (conv) conv->Release();
  if (frame) frame->Release();
  if (dec) dec->Release();
  factory->Release();
  return ok;
}
#endif  // _WIN32

}  // namespace

#ifdef _WIN32
namespace {
// PNG via WIC -- the same component already used for decoding replacements, so
// this costs no extra dependency. RGBA8 in, straight (non-premultiplied) out.
bool EncodePNG(const std::filesystem::path& path, const uint8_t* rgba, uint32_t w, uint32_t h) {
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory))))
    return false;
  bool ok = false;
  IWICStream* stream = nullptr;
  IWICBitmapEncoder* enc = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;

  // BGRA, not RGBA: SetPixelFormat is an IN/OUT parameter -- WIC negotiates and
  // writes back the format it actually accepted. PNG encoders reject 32bppRGBA
  // and quietly substitute 32bppBGRA, so feeding RGBA and trusting the request
  // wrote red and blue swapped. Ask for what the encoder really supports, and
  // then VERIFY what came back rather than assuming.
  std::vector<uint8_t> bgra(size_t(w) * h * 4);
  for (size_t i = 0, n = size_t(w) * h; i < n; ++i) {
    bgra[i * 4 + 0] = rgba[i * 4 + 2];
    bgra[i * 4 + 1] = rgba[i * 4 + 1];
    bgra[i * 4 + 2] = rgba[i * 4 + 0];
    bgra[i * 4 + 3] = rgba[i * 4 + 3];
  }
  GUID fmt = GUID_WICPixelFormat32bppBGRA;
  if (SUCCEEDED(factory->CreateStream(&stream)) &&
      SUCCEEDED(stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE)) &&
      SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
      SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
      SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
      SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&fmt))) {
    if (fmt != GUID_WICPixelFormat32bppBGRA) {
      // The encoder picked something else; writing our buffer now would produce
      // wrong colours or drop alpha. Better to fail and let the TGA path run.
      static bool warned = false;
      if (!warned) {
        warned = true;
        REXLOG_WARN("[texmod] PNG encoder negotiated an unexpected pixel format; using TGA");
      }
    } else if (SUCCEEDED(frame->WritePixels(h, w * 4, UINT(bgra.size()), bgra.data())) &&
               SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit())) {
      ok = true;
    }
  }
  if (frame) frame->Release();
  if (enc) enc->Release();
  if (stream) stream->Release();
  factory->Release();
  return ok;
}
}  // namespace
#endif

bool WriteDump(uint64_t hash, const uint8_t* rgba, uint32_t w, uint32_t h) {
  if (!rgba || !w || !h) return false;
  std::error_code ec;
  std::filesystem::create_directories(DumpDir(), ec);
  const std::string base = HashName(hash);
  std::string want = REXCVAR_GET(tex_dump_format);
  for (char& c : want) c = char(::tolower(static_cast<unsigned char>(c)));
#ifdef _WIN32
  if (want == "png") {
    if (EncodePNG(DumpDir() / (base + ".png"), rgba, w, h)) return true;
    REXLOG_WARN("[texmod] PNG encode failed for {} -- falling back to TGA", base);
  }
#else
  if (want == "png") {
    static bool warned = false;
    if (!warned) {
      warned = true;
      REXLOG_WARN("[texmod] tex_dump_format=png needs WIC (Windows); using TGA");
    }
  }
#endif
  return WriteTGA(DumpDir() / (base + ".tga"), rgba, w, h);
}

uint32_t BuildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
  out.clear();
  if (!rgba || !w || !h) return 0;
  // Total size of a full pyramid is < 4/3 of level 0; reserve that up front.
  out.reserve(size_t(w) * h * 4 * 4 / 3 + 64);
  out.insert(out.end(), rgba, rgba + size_t(w) * h * 4);

  uint32_t levels = 1, sw = w, sh = h;
  size_t src_off = 0;
  while (sw > 1 || sh > 1) {
    const uint32_t dw = sw > 1 ? sw / 2 : 1;
    const uint32_t dh = sh > 1 ? sh / 2 : 1;
    const size_t dst_off = out.size();
    out.resize(dst_off + size_t(dw) * dh * 4);
    for (uint32_t y = 0; y < dh; ++y) {
      // Clamp so a 1-wide/1-tall level averages the row/column it has rather
      // than reading past the end.
      const uint32_t y0 = std::min(y * 2, sh - 1), y1 = std::min(y * 2 + 1, sh - 1);
      for (uint32_t x = 0; x < dw; ++x) {
        const uint32_t x0 = std::min(x * 2, sw - 1), x1 = std::min(x * 2 + 1, sw - 1);
        const uint8_t* a = &out[src_off + (size_t(y0) * sw + x0) * 4];
        const uint8_t* b = &out[src_off + (size_t(y0) * sw + x1) * 4];
        const uint8_t* c = &out[src_off + (size_t(y1) * sw + x0) * 4];
        const uint8_t* d = &out[src_off + (size_t(y1) * sw + x1) * 4];
        uint8_t* o = &out[dst_off + (size_t(y) * dw + x) * 4];
        for (int k = 0; k < 4; ++k)
          o[k] = uint8_t((unsigned(a[k]) + b[k] + c[k] + d[k] + 2) / 4);
      }
    }
    src_off = dst_off;
    sw = dw;
    sh = dh;
    ++levels;
  }
  return levels;
}

bool ModsEnabled() {
  // LIVE on purpose: the tex_mods checkbox in the F4 settings overlay is the
  // control surface, so ticking it has to take effect immediately. (An earlier
  // version latched this after mistaking the user's own checkbox clicks for the
  // cvar changing by itself -- which broke the checkbox.) PollModDir watches for
  // the transition and invalidates only the modded textures.
  static const bool env_on = getenv("RESTUFF_TEX_MODS") != nullptr;
  return env_on || REXCVAR_GET(tex_mods);
}

// True when this texture already has a dump on disk in the current format, so a
// second run does not rewrite the whole corpus (the in-process dedupe set is
// per-run and cannot know about earlier sessions).
bool DumpExists(uint64_t hash) {
  std::error_code ec;
  std::string want = REXCVAR_GET(tex_dump_format);
  for (char& c : want) c = char(::tolower(static_cast<unsigned char>(c)));
  const char* ext = (want == "png") ? ".png" : ".tga";
  return std::filesystem::exists(DumpDir() / (HashName(hash) + ext), ec);
}

uint32_t ModGeneration() { return g_mod_gen.load(std::memory_order_relaxed); }

uint32_t ModGenerationFor(uint64_t hash) {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_hash_gen.find(hash);
  return it == g_hash_gen.end() ? 0u : it->second;
}

namespace {
// Only the loader calls this. Directory traversal, timestamp queries and
// releasing large cached mip chains must never run on the present thread.
void ScanModDir(bool on) {
  static bool s_was_on = false;
  static bool s_force_dirty = false;
  static bool s_primed = false;
  // Keep large pixel buffers alive until after releasing g_mutex: render
  // lookups take that lock too, so freeing a pack under it still stalls them.
  std::vector<std::shared_ptr<const Replacement>> retired;
  const auto invalidate = [&retired](uint64_t hash) {
    auto it = g_cache.find(hash);
    if (it != g_cache.end()) {
      retired.push_back(std::move(it->second));
      g_cache.erase(it);
    }
    g_pending.erase(hash);
    ++g_hash_gen[hash];
  };

  if (on != s_was_on) {
    s_was_on = on;
    // A texture that HAS a mod file changes appearance in BOTH directions:
    // switching on applies the replacement, switching off must revert to the
    // guest's own art. g_seen holds that set from when mods were last on.
    size_t n = 0;
    retired.reserve(g_seen.size());
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      for (const auto& [hash, mtime] : g_seen) {
        (void)mtime;
        invalidate(hash);
        ++n;
      }
    }
    // If mods were never on, g_seen is empty and there is nothing to revert --
    // so make the scan below treat every file it finds as new.
    s_force_dirty = on;
    g_mod_gen.fetch_add(1, std::memory_order_relaxed);
    std::error_code dec;
    // DIAGNOSTIC: tex_mods has been observed flipping with nothing writing it.
    // Log the raw inputs so the next run says whether the cvar itself is
    // changing or the transition detection is at fault.
    REXLOG_INFO("[texmod] replacements {}{} -- {} texture(s) invalidated "
                "(cvar={} env={})",
                on ? "ON" : "OFF",
                (on && !std::filesystem::is_directory(ModDir(), dec)) ? " (texture_mods/ absent)"
                                                                     : "",
                n, REXCVAR_GET(tex_mods) ? 1 : 0,
                getenv("RESTUFF_TEX_MODS") != nullptr ? 1 : 0);
  }
  if (!on) return;

  std::unordered_map<uint64_t, uint64_t> cur;
  std::error_code ec;
  for (std::filesystem::directory_iterator it(ModDir(), ec), end; !ec && it != end;
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    uint64_t hash = 0;
    if (!HashFromName(it->path(), hash)) continue;
    std::error_code tec;
    const auto t = std::filesystem::last_write_time(it->path(), tec);
    if (!tec) cur[hash] = uint64_t(t.time_since_epoch().count());
  }

  if (!s_primed && !s_force_dirty) {  // first scan is the baseline
    s_primed = true;
    g_seen = std::move(cur);
    return;
  }
  s_primed = true;

  // Diff both ways: added/edited, and removed (deleting a mod must revert the
  // texture, which is just as much an invalidation).
  std::vector<uint64_t> dirty;
  for (const auto& [hash, mtime] : cur) {
    auto old = g_seen.find(hash);
    if (s_force_dirty || old == g_seen.end() || old->second != mtime) dirty.push_back(hash);
  }
  for (const auto& [hash, mtime] : g_seen) {
    (void)mtime;
    if (!cur.count(hash)) dirty.push_back(hash);
  }
  const bool forced = s_force_dirty;
  s_force_dirty = false;

  if (dirty.empty()) {
    g_seen = std::move(cur);
    return;
  }

  retired.reserve(retired.size() + dirty.size());
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint64_t hash : dirty) {
      invalidate(hash);  // only THIS texture re-resolves
    }
  }
  g_seen = std::move(cur);
  g_mod_gen.fetch_add(1, std::memory_order_relaxed);
  if (forced) {
    REXLOG_INFO("[texmod] {} replacement(s) applied", dirty.size());
  } else {
    for (uint64_t hash : dirty)
      REXLOG_INFO("[texmod] {} changed -- reloading just that texture", HashName(hash));
  }
}

// The expensive part. Runs ONLY on the loader thread, never with g_mutex held.
Replacement LoadBlocking(uint64_t hash) {
  Replacement r;
#ifdef _WIN32
  const std::string base = HashName(hash);
  std::error_code ec;
  for (const char* ext : kExts) {
    const std::filesystem::path p = ModDir() / (base + ext);
    if (!std::filesystem::exists(p, ec)) continue;
    std::vector<uint8_t> level0;
    if (DecodeFile(p, level0, r.w, r.h)) {
      // Build the pyramid HERE, on the loader thread, not on the frame path.
      r.mip_levels = BuildMipChain(level0.data(), r.w, r.h, r.rgba);
      if (r.mip_levels == 0) {
        r.rgba = std::move(level0);
        r.mip_levels = 1;
      }
      r.present = true;
      REXLOG_INFO("[texmod] {} -> {} ({}x{}, {} mips)", base, p.filename().string(), r.w,
                  r.h, r.mip_levels);
    } else {
      REXLOG_WARN("[texmod] {} found but could not be decoded", p.string());
    }
    break;
  }
#endif
  return r;
}

void LoaderMain() {
  using clock = std::chrono::steady_clock;
  auto next_poll = clock::now();
  bool was_on = false;
  for (;;) {
    const bool on = g_mods_requested.load(std::memory_order_relaxed);
    if (clock::now() >= next_poll || on != was_on) {
      ScanModDir(on);
      was_on = on;
      next_poll = clock::now() + std::chrono::milliseconds(250);
    }
    uint64_t hash = 0;
    {
      std::unique_lock<std::mutex> lk(g_mutex);
      g_cv.wait_until(lk, next_poll, [was_on] {
        return !g_queue.empty() ||
               g_mods_requested.load(std::memory_order_relaxed) != was_on;
      });
      if (clock::now() >= next_poll ||
          g_mods_requested.load(std::memory_order_relaxed) != was_on)
        continue;
      if (g_queue.empty()) continue;
      hash = g_queue.front();
      g_queue.pop_front();
      // A scan/toggle may have cancelled this request. All scans and loads
      // share this worker, so an old in-flight decode cannot win a reload.
      if (!g_pending.count(hash)) continue;
      if (!on) {
        g_pending.erase(hash);
        continue;
      }
    }
    auto r = std::make_shared<const Replacement>(LoadBlocking(hash));
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      const bool present = r->present;
      g_cache[hash] = std::move(r);
      g_pending.erase(hash);
      // A miss leaves the guest pixels unchanged. Invalidating it used to
      // cause a second decode/upload for every texture absent from the pack.
      if (present) {
        ++g_hash_gen[hash];
        g_mod_gen.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
}

void EnsureLoaderStarted() {
  if (g_worker_started.exchange(true, std::memory_order_relaxed)) return;
  // Detached on purpose: the title hard-exits the process ("Title terminated;
  // hard-exiting process"), so there is no orderly shutdown to join against.
  std::thread(LoaderMain).detach();
}

}  // namespace

void PollModDir() {
  const bool on = ModsEnabled();
  const bool changed = g_mods_requested.exchange(on, std::memory_order_relaxed) != on;
  if (on) EnsureLoaderStarted();
  if (changed) g_cv.notify_one();
}

std::shared_ptr<const Replacement> FindReplacement(uint64_t hash, uint32_t* generation) {
  PollModDir();
  std::lock_guard<std::mutex> lock(g_mutex);
  // Return the generation of these exact pixels, not a newer generation
  // sampled after the renderer has spent time uploading them.
  if (generation) {
    const auto gen = g_hash_gen.find(hash);
    *generation = gen == g_hash_gen.end() ? 0u : gen->second;
  }
  if (!g_mods_requested.load(std::memory_order_relaxed)) return nullptr;
  auto it = g_cache.find(hash);
  if (it != g_cache.end()) return it->second->present ? it->second : nullptr;
  // Not decoded yet. Queue it and return null so THIS frame draws the guest
  // texture instead of stalling on a multi-millisecond WIC decode.
  if (g_pending.insert(hash).second) {
    g_queue.push_back(hash);
    g_cv.notify_one();
  }
  return nullptr;
}

}  // namespace restuff::renderer::texmod

bool get_tex_dump() {
  return REXCVAR_GET(tex_dump);
}

void set_tex_dump(bool val) {
  REXCVAR_SET(tex_dump, val);
}

int get_tex_dump_format_index() {
  std::string fmt = REXCVAR_GET(tex_dump_format);
  for (char& c : fmt) c = char(::tolower(static_cast<unsigned char>(c)));
  return (fmt == "png") ? 1 : 0;
}

void set_tex_dump_format_index(int idx) {
  REXCVAR_SET(tex_dump_format, idx == 1 ? "png" : "tga");
}


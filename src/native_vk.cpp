#include "native_vk.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <set>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// M4.14 (RESTUFF_CBLIP_BEEP=1): audible marker for constant-anomaly events.
static void restuff_cblip_beep() { MessageBeep(0xFFFFFFFFu); }
#else
#include <dlfcn.h>
#include <renderdoc_app.h>
#include <unistd.h>  // M4.0: readlink for ExeDir()
static void restuff_cblip_beep() {}
#endif

#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/system/xvideo.h>
#include <rex/ui/vulkan/device.h>
#include <rex/ui/vulkan/presenter.h>
#include <rex/ui/vulkan/provider.h>
#include <rex/ui/vulkan/ui_samplers.h>
#include <rex/ui/vulkan/util.h>

#include "renderer/guest_texture_decode.h"
#include "renderer/native_backend_vk.h"
#include "renderer/shader_pipeline.h"
#include "renderer/up_draws.h"

// glslc-compiled SPIR-V, embedded as alignas(4) uint32_t arrays at build time
// (cmake/embed_spv.cmake).
#include <draw2d.vert.spv.h>
#include <draw2d_quad.frag.spv.h>
#include <draw2d_text.frag.spv.h>

// hooks.cpp (M3.188): time-based late watchpoint arm, checked from our
// per-frame loop below — the one site proven hot for the whole run (the
// guest draw-hook counter variant never fired in real runs).
namespace ibwatch { void TryLateMs(); void MergeTick(); }

// X_STATUS lives in namespace rex; bring it (and thus the X_STATUS_* macros,
// which expand to unqualified X_STATUS casts) into this TU.
using rex::X_STATUS;

REXCVAR_DEFINE_BOOL(use_native_renderer, true, "Renderer",
                    "Render through the native Vulkan graphics system instead of the "
                    "xenos GPU emulation plugin.");
REXCVAR_DECLARE(bool, use_translated_shaders);  // defined in native_backend_vk.cpp

namespace restuff::native { uint64_t CurrentPsHashForDebug(); }
namespace restuff {

namespace vk = rex::ui::vulkan;

namespace {

// ---------------------------------------------------------------------------
// Draw layer: renders the captured guest UP draws (renderer/up_draws.h) into
// the presenter's guest-output image. Classic single-subpass render pass with
// the acquire/release baked into attachment layouts + external dependencies
// (initialLayout UNDEFINED + loadOp CLEAR is blessed by the presenter since we
// redraw the full frame each refresh). Every refresh fence-waits, which makes
// single-buffered vertex/staging resources and immediate destroys sound.
// ---------------------------------------------------------------------------

struct TexEntry {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkDescriptorSet set = VK_NULL_HANDLE;  // allocate-only (no vkFreeDescriptorSets in the table)
  uint64_t content_hash = 0;
  uint32_t width = 0, height = 0;
  // M4.36: actual device-memory footprint of `image` (vkGetImageMemoryRequirements
  // at create time). Feeds RESTUFF_TEXCENSUS -- on a shared-memory handheld the
  // bytes held by a never-evicted cache matter more than the entry count.
  VkDeviceSize mem_bytes = 0;
  // M4.36: frame ordinal of the last cache hit. The cache has no eviction, so
  // this is the measurement that decides whether one is worth adding: how much
  // of a long session's cache is still being sampled at all.
  uint64_t last_used_frame = 0;
  // M4.0: the view was created _SRGB (gamma sign-field + RESTUFF_NO_SRGB
  // considered). Content re-uploads may reuse the image only when this and the
  // extent still match the new decode.
  bool srgb = false;
  // M4.3: the image's actual VkFormat (RGBA8 or a BC block format). In-place
  // refresh additionally requires this to match -- a BC<->RGBA flip (device
  // path change or format change at one address) must take the recreate path.
  VkFormat vkfmt = VK_FORMAT_UNDEFINED;
  bool is_depth = false;  // D32_SFLOAT resolve target (depth aspect), not color
  // M4.4: depth rt_tex entries are also usable as depth attachments (the
  // single-pass depth-fill resolve renders into them). Both lazily created on
  // first use by RecordDepthFillResolve; destroyed with the entry.
  VkImageView att_view = VK_NULL_HANDLE;  // BOTH aspects (attachment view)
  VkFramebuffer ds_fb = VK_NULL_HANDLE;   // depth_fill_rp framebuffer
  // M3.114: a resolve DEST has defined contents after its first resolve. The
  // pre-copy barrier must then use SHADER_READ_ONLY_OPTIMAL as oldLayout --
  // UNDEFINED legally discards the whole image, and partial dirty-rect
  // resolves (the occ/sun-shadow texture tracks the volume's screen extent,
  // so the rect is CAMERA-DEPENDENT) rely on everything outside the rect
  // persisting. Discarding it cut the shadow off / detached pieces of it as
  // the camera moved.
  bool resolved_once = false;
};

struct PendingUpload {
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
};

struct CachedFb {
  uint64_t version = UINT64_MAX;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer fb = VK_NULL_HANDLE;
};

struct DrawLayer {
  VkRenderPass render_pass = VK_NULL_HANDLE;
  // M3.100: present-time display gamma ramp (guest DC_LUT 256-entry table).
  // The reference bakes this into its front-buffer conversion; without it our
  // presented frame is uniformly darker with exactly the ramp's curve.
  VkDescriptorSetLayout gamma_lut_layout = VK_NULL_HANDLE;
  VkPipelineLayout gamma_pipeline_layout = VK_NULL_HANDLE;
  VkPipeline gamma_pipeline = VK_NULL_HANDLE;
  // M3.292: tone-matched variant, bound ONLY on frames with a real 3D scene
  // (trans.size() >= 64). The M3.291 curve applied globally BLINDED the 2D
  // title screen (high-key art + gain clipped to neon white -- user report);
  // 2D-only frames keep the plain LUT pipeline, whose look was verified.
  VkPipeline gamma_tone_pipeline = VK_NULL_HANDLE;
  VkImage gamma_lut_img = VK_NULL_HANDLE;
  VkDeviceMemory gamma_lut_mem = VK_NULL_HANDLE;
  VkImageView gamma_lut_view = VK_NULL_HANDLE;
  VkDescriptorPool gamma_pool = VK_NULL_HANDLE;
  VkDescriptorSet gamma_set = VK_NULL_HANDLE;
  uint32_t gamma_uploaded_version = 0;
  VkDescriptorSetLayout ds_layout = VK_NULL_HANDLE;
  // M4.37 eviction accounting (walker/present thread only, like the rest of
  // DrawLayer). ev_redecoded counts entries that were evicted and then needed
  // again -- the thrash signal: if it climbs, kColdFrames is too aggressive.
  uint64_t ev_count = 0, ev_bytes = 0, ev_redecoded = 0, ev_next_frame = 0;
  std::unordered_set<uint64_t> ev_recent_keys;  // bounded; see TexEvictSweep
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;  // current (back of desc_pools)
  // M4.35: every single-sampler pool ever created. The cache only erases an
  // entry when its guest address changes shape/format, so distinct guest
  // textures accumulate for as long as a level is played -- one 2048-set pool
  // is a session-length ceiling, not headroom (a ~10min level-1 session hit
  // 2033 and every texture needed after that bound white). Grow on demand.
  std::vector<VkDescriptorPool> desc_pools;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  // Pipeline matrix: [family][blend] (quad/text x standard/additive/premul).
  VkPipeline pipelines[2][3] = {};
  VkSampler sampler = VK_NULL_HANDLE;  // borrowed from provider ui_samplers (do NOT destroy)
  // Repeat-addressing variant: the guest fetch constant's clamp_x/clamp_y
  // select wrap for the scene backdrop grids (neighbor cells sample UV 1..2 of
  // the shared texture); clamping renders those cells as transparent edge
  // smear -- the title-screen right-band cut.
  VkSampler sampler_repeat = VK_NULL_HANDLE;  // borrowed, do NOT destroy
  // Point-sampled variant for depth resolve targets (D32 depth images can't be
  // linearly filtered, and depth values must not be blended).
  VkSampler sampler_nearest = VK_NULL_HANDLE;  // borrowed, do NOT destroy

  // One host-visible vertex buffer, grown as needed, persistently mapped.
  VkBuffer vb = VK_NULL_HANDLE;
  VkDeviceMemory vb_mem = VK_NULL_HANDLE;
  uint32_t vb_mem_type = 0;
  VkDeviceSize vb_mem_size = 0;
  VkDeviceSize vb_capacity = 0;
  uint8_t* vb_mapped = nullptr;

  // Texture cache keyed by guest physical address (key 0 = 1x1 white).
  std::unordered_map<uint64_t, TexEntry> textures;
  std::vector<PendingUpload> pending_uploads;
  std::vector<PendingUpload> retired_uploads;
  // Stale-content TexEntries awaiting GPU completion. A texture the guest
  // mutates MID-FRAME (cutscene video/dynamic atlases) can hash-mismatch on a
  // second ResolveTexture call in the same frame; destroying the old image
  // immediately would free memory the frame's already-recorded draws still
  // sample (driver use-after-free -> host heap corruption, the Play Game
  // SIGABRTs). Destroyed only after the frame fence wait.
  std::vector<TexEntry> deferred_destroy;
  // M3.0: bumped when any TexEntry is retired -- its VkImageView dies at frame
  // end, and a recycled handle value could alias a cached texture-combo set.
  // M4.5: was a bool; now a monotonic epoch so each frame SLOT resets its own
  // combo pools when its recorded epoch trails this one (a shared bool can't
  // tell two slots apart). Slots start at epoch 0 -> first frame resets empty
  // pools, harmless. The view itself dies post-fence via the retire lists, so
  // a handle can never be recycled while a frame referencing the old combo is
  // still in flight.
  uint64_t tex_retire_epoch = 1;
  // M3.136: per-FRAME memo of GuestTextureContentHash. That hash walks the
  // texture's whole byte extent (up to 1 MB) through a serial dependent-multiply
  // FNV chain, and ResolveTextureEntry called it BEFORE the cache lookup -- so a
  // texture bound by N draws was fully re-hashed N times every frame. Measured
  // at 14.7us of the 19.8us total per-draw cost (73%), i.e. ~23ms of a ~31ms
  // frame. Hashing once per texture per frame is also strictly MORE coherent
  // than before: the mid-frame hash-mismatch described in the deferred_destroy
  // comment above cannot arise if a frame only ever sees one hash per texture.
  // Keyed on the full identity the hash depends on (address + extent + format),
  // so a re-pointed or resized fetch still hashes fresh.
  std::unordered_map<uint64_t, uint64_t> content_hash_memo;
  uint64_t content_hash_memo_frame = ~0ull;
  // Free-list of recycled desc_pool descriptor sets (shared by guest textures
  // and rt_tex resolve targets -- both allocate single-sampler sets from
  // desc_pool). desc_pool has no FREE flag and the dispatch table lacks
  // vkFreeDescriptorSets, so a set can't be individually returned; instead a
  // destroyed entry's set is parked here and re-pointed (vkUpdateDescriptorSets)
  // onto the next created entry. Without this, EVERY texture re-decode leaked a
  // set (the glyph atlas alone re-decodes ~28x/s), exhausting the pool within
  // seconds; by level load no post-process render target could allocate -> the
  // 3D world never composited (flat background). Sets land here only via
  // DestroyTexEntry, which runs post-fence (deferred_destroy) or at shutdown, so
  // a recycled set is never in-flight.
  std::vector<VkDescriptorSet> tex_set_free;

  std::array<CachedFb, vk::VulkanPresenter::kMaxActiveGuestOutputImageVersions> fbs;

  // Persistent copy of the latest guest frame (redrawn until a new one lands).
  std::vector<renderer::DecodedDraw> frame_draws;
  std::vector<renderer::Draw2DVertex> vertex_scratch;

  bool ready = false;
  std::atomic<uint64_t> frames_rendered{0};
  std::atomic<uint64_t> draws_rendered{0};
};

DrawLayer& DL() {
  static DrawLayer dl;
  return dl;
}

// M4.35: add one 2048-set single-sampler pool and make it current. Called at
// init and again whenever AcquireTexSet exhausts the current pool.
bool AddTexPool(vk::VulkanDevice* dev) {
  auto& dl = DL();
  // RESTUFF_TEXPOOL_SIZE=<n> shrinks the pool so a short run exercises the
  // growth path (the real 2048 needs a ~10min session to fill).
  static const uint32_t kSets = [] {
    const char* e = getenv("RESTUFF_TEXPOOL_SIZE");
    const long v = e ? strtol(e, nullptr, 10) : 0;
    return uint32_t(v >= 8 && v <= 65536 ? v : 2048);
  }();
  VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSets};
  VkDescriptorPoolCreateInfo dp_ci = {};
  dp_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dp_ci.maxSets = kSets;
  dp_ci.poolSizeCount = 1;
  dp_ci.pPoolSizes = &pool_size;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  if (dev->functions().vkCreateDescriptorPool(dev->device(), &dp_ci, nullptr, &pool) !=
      VK_SUCCESS)
    return false;
  dl.desc_pools.push_back(pool);
  dl.desc_pool = pool;
  if (dl.desc_pools.size() > 1)
    REXLOG_INFO("[native_vk] M4.35 texture descriptor pool #{} added ({} sets each)",
                dl.desc_pools.size(), kSets);
  return true;
}

// M4.36 (RESTUFF_TEXCENSUS=1): what the never-evicted texture cache actually
// holds. Three questions the entry count alone can't answer:
//   1. BYTES -- on a shared-memory handheld this is the number that matters.
//   2. DUPLICATION -- entries sharing a content hash are the same art cached
//      again at a new guest address (the signature of a reload re-registering
//      a level's textures rather than genuinely new content).
//   3. LIVENESS -- how many entries were sampled recently. If most of a long
//      session's cache is cold, an eviction policy is both safe and worth it.
// Logged on the present-alive cadence (every 100 frames).
void TexCensus() {
  static const bool on = getenv("RESTUFF_TEXCENSUS") != nullptr;
  if (!on) return;
  auto& dl = DL();
  const uint64_t now = dl.frames_rendered.load(std::memory_order_relaxed);
  // "Cold" = not sampled in the last 600 frames (~10s at 60fps): long enough
  // that a texture still in the current view will never be misread as cold.
  constexpr uint64_t kColdAfter = 600;
  uint64_t bytes = 0, cold_n = 0, cold_bytes = 0, dup_n = 0, dup_bytes = 0;
  std::unordered_map<uint64_t, uint32_t> by_content;
  for (const auto& [key, t] : dl.textures) {
    if (t.image == VK_NULL_HANDLE) continue;
    bytes += t.mem_bytes;
    if (now > t.last_used_frame + kColdAfter) {
      ++cold_n;
      cold_bytes += t.mem_bytes;
    }
    // content_hash 1 is the sentinel for the 1x1 white/placeholder entries.
    if (t.content_hash > 1 && ++by_content[t.content_hash] > 1) {
      ++dup_n;
      dup_bytes += t.mem_bytes;
    }
  }
  const auto mb = [](uint64_t b) { return double(b) / (1024.0 * 1024.0); };
  REXLOG_INFO("[TEXCENSUS] entries={} {:.1f}MB | cold(>{}f)={} {:.1f}MB | dup_copies={} "
              "{:.1f}MB over {} distinct | pools={}",
              dl.textures.size(), mb(bytes), kColdAfter, cold_n, mb(cold_bytes), dup_n,
              mb(dup_bytes), by_content.size(), dl.desc_pools.size());
  REXLOG_INFO("[TEXCENSUS] evicted={} {:.1f}MB | re_decoded_after_evict={} (thrash if high)",
              dl.ev_count, mb(dl.ev_bytes), dl.ev_redecoded);
}

// M4.37: two-tier eviction for the guest texture cache. The cache had NO age
// policy -- entries were only ever erased when a guest address changed
// shape/format -- so it grew for the whole session (M4.35: 2033 entries after
// ~10min, exhausting the descriptor pool and turning new textures white).
// Measured by M4.36: 58% of entries / 67% of bytes are already cold after a
// single level load, so this reclaims most of the footprint.
//
// Eviction here is CHEAP in a way it is not for a render-surface pool: every
// entry is re-decodable from guest RAM, so a wrong eviction costs one
// re-decode + upload, never a lost resource. Hence plain coldest-first rather
// than a composite rank.
//
// SAFETY (violating any of these corrupts frames -- see the plan):
//  * `textures` only. Never tl.rt_tex: GPU-produced, unreconstructable, and
//    evicting resets resolved_once -> the next partial resolve legally
//    discards the whole image (the sun/occ-shadow cut-off regression).
//  * Never key 0 (the 1x1 white entry) -- the unsupported-format fallback and
//    GetTextureComboSet both depend on it existing.
//  * CALL SITE MATTERS: only from the top of PrepareTranslatedDraws, before
//    any TexEntry* is taken into resolved[]. unordered_map keeps references
//    stable across rehash but NOT across erase.
//  * Route every eviction through deferred_destroy + ++tex_retire_epoch, never
//    DestroyTexEntry directly (it recycles the descriptor set, post-fence only).
//    The epoch bump is also what invalidates the per-slot combo cache.
void TexEvictSweep(vk::VulkanDevice* dev) {
  static const bool off = getenv("RESTUFF_NO_TEXEVICT") != nullptr;
  if (off) return;
  auto& dl = DL();
  const uint64_t now = dl.frames_rendered.load(std::memory_order_relaxed);

  // Amortize: each sweep that evicts anything costs one combo-pool reset per
  // slot, so sweep periodically rather than every frame.
  static const uint64_t kPeriod = [] {
    const char* e = getenv("RESTUFF_TEXEVICT_PERIOD");
    const long v = e ? strtol(e, nullptr, 10) : 0;
    return uint64_t(v >= 1 && v <= 100000 ? v : 60);
  }();
  if (now < dl.ev_next_frame) return;
  dl.ev_next_frame = now + kPeriod;

  static const uint64_t kCold = [] {
    const char* e = getenv("RESTUFF_TEXCOLD_FRAMES");
    const long v = e ? strtol(e, nullptr, 10) : 0;
    return uint64_t(v >= 60 && v <= 100000 ? v : 600);
  }();
  // Never evict anything touched this recently, whatever the budget says.
  constexpr uint64_t kHot = 120;

  // Budget: a fraction of device-local VRAM, resolved once. An iGPU/UMA part
  // reporting a small or shared heap takes the floor. A quarter-gig ceiling is
  // ample -- the measured live working set is ~13MB.
  static uint64_t s_budget = 0;
  if (s_budget == 0) {
    if (const char* e = getenv("RESTUFF_TEXBUDGET_MB")) {
      const long v = strtol(e, nullptr, 10);
      if (v >= 1 && v <= 65536) s_budget = uint64_t(v) << 20;
    }
    if (s_budget == 0) {
      VkPhysicalDeviceMemoryProperties mp = {};
      dev->vulkan_instance()->functions().vkGetPhysicalDeviceMemoryProperties(
          dev->physical_device(), &mp);
      uint64_t vram = 0;
      for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
        if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
          vram += mp.memoryHeaps[i].size;
      s_budget = std::clamp<uint64_t>(vram / 8, 128ull << 20, 1024ull << 20);
    }
    REXLOG_INFO("[native_vk] M4.37 texture eviction ON (budget {}MB, cold>{}f, every {}f)",
                s_budget >> 20, kCold, kPeriod);
  }

  uint64_t live_bytes = 0;
  for (const auto& [key, t] : dl.textures)
    if (t.image != VK_NULL_HANDLE) live_bytes += t.mem_bytes;

  // Tier 1: drop anything cold, regardless of budget. Tier 2: if still over
  // budget, keep taking the coldest until under. Both skip key 0 and hot
  // entries. Collect first, erase after -- erasing mid-iteration invalidates.
  std::vector<std::pair<uint64_t, uint64_t>> victims;  // (age, key)
  for (const auto& [key, t] : dl.textures) {
    if (key == 0 || t.image == VK_NULL_HANDLE) continue;
    const uint64_t age = now > t.last_used_frame ? now - t.last_used_frame : 0;
    if (age <= kHot) continue;
    victims.emplace_back(age, key);
  }
  std::sort(victims.begin(), victims.end(), std::greater<>());  // coldest first

  uint64_t freed = 0, n = 0;
  for (const auto& [age, key] : victims) {
    const bool cold = age > kCold;
    const bool over_budget = live_bytes - freed > s_budget;
    if (!cold && !over_budget) break;  // sorted: nothing later qualifies either
    auto it = dl.textures.find(key);
    if (it == dl.textures.end()) continue;
    freed += it->second.mem_bytes;
    ++n;
    dl.deferred_destroy.push_back(it->second);  // freed post-fence
    ++dl.tex_retire_epoch;                      // invalidates the combo cache
    dl.textures.erase(it);
    // Thrash detection: remember what we dropped so a re-decode of the same
    // key is attributable. Bounded -- clear wholesale rather than grow.
    if (dl.ev_recent_keys.size() > 4096) dl.ev_recent_keys.clear();
    dl.ev_recent_keys.insert(key);
  }
  if (n) {
    dl.ev_count += n;
    dl.ev_bytes += freed;
    static std::atomic<int> s_log_budget{12};
    if (s_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[native_vk] M4.37 evicted {} textures ({:.1f}MB); {} left, {:.1f}MB live",
                  n, double(freed) / 1048576.0, dl.textures.size(),
                  double(live_bytes - freed) / 1048576.0);
  }
}

bool CreateDrawLayer(vk::VulkanProvider& provider) {
  auto& dl = DL();
  vk::VulkanDevice* dev = provider.vulkan_device();
  const auto& df = dev->functions();
  VkDevice device = dev->device();

  // Render pass: acquire from UNDEFINED (full-frame redraw; presenter blesses
  // discarding), release to the presenter's internal layout.
  VkAttachmentDescription att = {};
  att.format = vk::VulkanPresenter::kGuestOutputFormat;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = vk::VulkanPresenter::kGuestOutputInternalLayout;

  VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color_ref;

  VkSubpassDependency deps[2] = {};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = vk::VulkanPresenter::kGuestOutputInternalStageMask;
  deps[0].srcAccessMask = 0;  // WAR: execution-only
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstStageMask = vk::VulkanPresenter::kGuestOutputInternalStageMask;
  deps[1].dstAccessMask = vk::VulkanPresenter::kGuestOutputInternalAccessMask;

  VkRenderPassCreateInfo rp_ci = {};
  rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp_ci.attachmentCount = 1;
  rp_ci.pAttachments = &att;
  rp_ci.subpassCount = 1;
  rp_ci.pSubpasses = &sub;
  rp_ci.dependencyCount = 2;
  rp_ci.pDependencies = deps;
  if (df.vkCreateRenderPass(device, &rp_ci, nullptr, &dl.render_pass) != VK_SUCCESS) {
    REXLOG_ERROR("[native_vk] vkCreateRenderPass failed");
    return false;
  }

  // Descriptor set layout / pool (allocate-only sets).
  VkDescriptorSetLayoutBinding tex_binding = {};
  tex_binding.binding = 0;
  tex_binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  tex_binding.descriptorCount = 1;
  tex_binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dsl_ci = {};
  dsl_ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dsl_ci.bindingCount = 1;
  dsl_ci.pBindings = &tex_binding;
  df.vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &dl.ds_layout);

  // M4.35: 2048 per pool, and AcquireTexSet adds another pool when this one
  // fills. The original sizing assumed tex_set_free recycling kept the live
  // count near a level's working set (~164), but entries are only erased when
  // a guest address changes shape/format -- never merely for age -- so the
  // count tracks DISTINCT textures seen, which grows all session.
  if (!AddTexPool(dev))
    REXLOG_ERROR("[native_vk] initial texture descriptor pool creation FAILED");

  VkPushConstantRange pc_range = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};  // vec2 scale + vec2 offset
  VkPipelineLayoutCreateInfo pl_ci = {};
  pl_ci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl_ci.setLayoutCount = 1;
  pl_ci.pSetLayouts = &dl.ds_layout;
  pl_ci.pushConstantRangeCount = 1;
  pl_ci.pPushConstantRanges = &pc_range;
  df.vkCreatePipelineLayout(device, &pl_ci, nullptr, &dl.pipeline_layout);

  // Pipeline: pos2f + uv2f + rgba8, standard alpha blend, cull none, dynamic
  // viewport/scissor (matches the friend's D3D12 state).
  VkShaderModule vs = vk::util::CreateShaderModule(dev, kDraw2DVS, kDraw2DVS_size_bytes);
  VkShaderModule fs = vk::util::CreateShaderModule(dev, kDraw2DQuadFS, kDraw2DQuadFS_size_bytes);
  VkShaderModule fs_text =
      vk::util::CreateShaderModule(dev, kDraw2DTextFS, kDraw2DTextFS_size_bytes);
  if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE || fs_text == VK_NULL_HANDLE) {
    REXLOG_ERROR("[native_vk] shader module creation failed");
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  VkVertexInputBindingDescription vb_desc = {0, sizeof(renderer::Draw2DVertex),
                                             VK_VERTEX_INPUT_RATE_VERTEX};
  VkVertexInputAttributeDescription vattrs[4] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(renderer::Draw2DVertex, x)},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(renderer::Draw2DVertex, u)},
      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(renderer::Draw2DVertex, rgba)},
      {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(renderer::Draw2DVertex, rgba2)},
  };
  VkPipelineVertexInputStateCreateInfo vi = {};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &vb_desc;
  vi.vertexAttributeDescriptionCount = 4;
  vi.pVertexAttributeDescriptions = vattrs;

  VkPipelineInputAssemblyStateCreateInfo ia = {};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp = {};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs = {};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms = {};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend = {};
  blend.blendEnable = VK_TRUE;
  blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.colorBlendOp = VK_BLEND_OP_ADD;
  blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  blend.alphaBlendOp = VK_BLEND_OP_ADD;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb = {};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;

  VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn = {};
  dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;

  VkGraphicsPipelineCreateInfo gp_ci = {};
  gp_ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gp_ci.stageCount = 2;
  gp_ci.pStages = stages;
  gp_ci.pVertexInputState = &vi;
  gp_ci.pInputAssemblyState = &ia;
  gp_ci.pViewportState = &vp;
  gp_ci.pRasterizationState = &rs;
  gp_ci.pMultisampleState = &ms;
  gp_ci.pColorBlendState = &cb;
  gp_ci.pDynamicState = &dyn;
  gp_ci.layout = dl.pipeline_layout;
  gp_ci.renderPass = dl.render_pass;
  gp_ci.subpass = 0;
  VkResult pres = VK_SUCCESS;
  const VkShaderModule frags[2] = {fs, fs_text};
  for (int fam = 0; fam < 2 && pres == VK_SUCCESS; ++fam) {
    stages[1].module = frags[fam];
    for (int bm = 0; bm < 3 && pres == VK_SUCCESS; ++bm) {
      switch (bm) {
        case 0:  // standard alpha
          blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          break;
        case 1:  // additive
          blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
          blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
          break;
        case 2:  // premultiplied
          blend.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
          blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
          break;
      }
      pres = df.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_ci, nullptr,
                                          &dl.pipelines[fam][bm]);
    }
  }
  df.vkDestroyShaderModule(device, vs, nullptr);
  df.vkDestroyShaderModule(device, fs, nullptr);
  df.vkDestroyShaderModule(device, fs_text, nullptr);
  if (pres != VK_SUCCESS) {
    REXLOG_ERROR("[native_vk] vkCreateGraphicsPipelines failed ({})", int(pres));
    return false;
  }

  // Samplers: reuse the provider's UI samplers (creating our own is forbidden
  // by the SDK due to maxSamplerAllocationCount pressure). The guest texture
  // fetch constant's clamp_x/clamp_y select the address mode per texture:
  // clamp for sprites/UI, wrap for the scene backdrop grids whose neighbor
  // cells sample UV 1..2 of a shared texture (clamping those rendered the
  // cells as transparent edge smear = the title right-band cut).
  dl.sampler =
      provider.ui_samplers()->samplers()[vk::UISamplers::kSamplerIndexLinearClampToEdge];
  dl.sampler_repeat =
      provider.ui_samplers()->samplers()[vk::UISamplers::kSamplerIndexLinearRepeat];
  dl.sampler_nearest =
      provider.ui_samplers()->samplers()[vk::UISamplers::kSamplerIndexNearestClampToEdge];

  // M3.100: present-time gamma-ramp pipeline. The guest programs a display
  // gamma curve through the DC_LUT registers; the 360's display controller
  // applies it at scanout and the SDK reference bakes it into the front
  // buffer. We apply it in the present quad: same Draw2DVertex stream, set 0 =
  // the front-buffer texture (dl.ds_layout, the existing rt set), set 1 = a
  // 256x1 A2B10G10R10 LUT sampled with linear filtering (the hardware
  // interpolates between the 256 entries). Non-fatal: on any failure
  // gamma_pipeline stays null and the plain present path is used.
  {
    VkDescriptorSetLayoutBinding lb = {};
    lb.binding = 0;
    lb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    lb.descriptorCount = 1;
    lb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci = {};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings = &lb;
    if (df.vkCreateDescriptorSetLayout(device, &lci, nullptr, &dl.gamma_lut_layout) ==
        VK_SUCCESS) {
      const VkDescriptorSetLayout gsets[2] = {dl.ds_layout, dl.gamma_lut_layout};
      const VkPushConstantRange gpc = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
      VkPipelineLayoutCreateInfo gpl = {};
      gpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      gpl.setLayoutCount = 2;
      gpl.pSetLayouts = gsets;
      gpl.pushConstantRangeCount = 1;
      gpl.pPushConstantRanges = &gpc;
      df.vkCreatePipelineLayout(device, &gpl, nullptr, &dl.gamma_pipeline_layout);
    }
    VkImageCreateInfo ici = {};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    ici.extent = {256, 1, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (dl.gamma_pipeline_layout &&
        vk::util::CreateDedicatedAllocationImage(dev, ici, vk::util::MemoryPurpose::kDeviceLocal,
                                                 dl.gamma_lut_img, dl.gamma_lut_mem)) {
      VkImageViewCreateInfo vci = {};
      vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vci.image = dl.gamma_lut_img;
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      vci.subresourceRange = vk::util::InitializeSubresourceRange();
      df.vkCreateImageView(device, &vci, nullptr, &dl.gamma_lut_view);
      VkDescriptorPoolSize gps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
      VkDescriptorPoolCreateInfo gdp = {};
      gdp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      gdp.maxSets = 1;
      gdp.poolSizeCount = 1;
      gdp.pPoolSizes = &gps;
      if (dl.gamma_lut_view &&
          df.vkCreateDescriptorPool(device, &gdp, nullptr, &dl.gamma_pool) == VK_SUCCESS) {
        VkDescriptorSetAllocateInfo gai = {};
        gai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        gai.descriptorPool = dl.gamma_pool;
        gai.descriptorSetCount = 1;
        gai.pSetLayouts = &dl.gamma_lut_layout;
        if (df.vkAllocateDescriptorSets(device, &gai, &dl.gamma_set) == VK_SUCCESS) {
          VkDescriptorImageInfo gii = {dl.sampler, dl.gamma_lut_view,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          VkWriteDescriptorSet gw = {};
          gw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          gw.dstSet = dl.gamma_set;
          gw.dstBinding = 0;
          gw.descriptorCount = 1;
          gw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          gw.pImageInfo = &gii;
          df.vkUpdateDescriptorSets(device, 1, &gw, 0, nullptr);
        }
      }
    }
    static const char* kGammaVS =
        "#version 450\n"
        "layout(push_constant) uniform PC { vec4 so; } pc;\n"
        "layout(location=0) in vec2 a_pos;\n"
        "layout(location=1) in vec2 a_uv;\n"
        "layout(location=2) in vec4 a_col;\n"
        "layout(location=3) in vec4 a_col2;\n"
        "layout(location=0) out vec2 v_uv;\n"
        "void main(){ v_uv = a_uv; gl_Position = vec4(a_pos * pc.so.xy + pc.so.zw, 0.0, 1.0); }\n";
    static const char* kGammaFS =
        "#version 450\n"
        "layout(set=0, binding=0) uniform sampler2D srcTex;\n"
        "layout(set=1, binding=0) uniform sampler2D lutTex;\n"
        "layout(location=0) in vec2 v_uv;\n"
        "layout(location=0) out vec4 o;\n"
        "void main(){ vec4 c = texture(srcTex, v_uv);\n"
        "  vec3 u = c.rgb * (255.0/256.0) + (0.5/256.0);\n"
        "  o = vec4(texture(lutTex, vec2(u.r, 0.5)).r,\n"
        "           texture(lutTex, vec2(u.g, 0.5)).g,\n"
        "           texture(lutTex, vec2(u.b, 0.5)).b, 1.0); }\n";
    // M3.291/M3.292: tone-matched FS variant for 3D-scene frames only. The
    // user's matched-camera pair fits ours = 0.827 * reference^0.726 (shadow
    // SHAPES agree; the whole scene differs by one flatter display curve), so
    // scene frames present through the measured inverse min(1,g*1.209)^1.377.
    // Tune RESTUFF_TONE_POWER / RESTUFF_TONE_GAIN; RESTUFF_NO_TONE=1 disables
    // (the tone pipeline is then never created and every frame uses the plain
    // LUT path above -- which is also what 2D-only frames always use, because
    // applying this curve to the title's high-key 2D art clips it to neon).
    static char kGammaToneFS[1024];
    {
      float tp = 1.377f, tg = 1.209f;
      if (const char* e = getenv("RESTUFF_TONE_POWER")) tp = float(atof(e));
      if (const char* e = getenv("RESTUFF_TONE_GAIN")) tg = float(atof(e));
      std::snprintf(kGammaToneFS, sizeof(kGammaToneFS),
        "#version 450\n"
        "layout(set=0, binding=0) uniform sampler2D srcTex;\n"
        "layout(set=1, binding=0) uniform sampler2D lutTex;\n"
        "layout(location=0) in vec2 v_uv;\n"
        "layout(location=0) out vec4 o;\n"
        "void main(){ vec4 c = texture(srcTex, v_uv);\n"
        "  vec3 u = c.rgb * (255.0/256.0) + (0.5/256.0);\n"
        "  vec3 g = vec3(texture(lutTex, vec2(u.r, 0.5)).r,\n"
        "                texture(lutTex, vec2(u.g, 0.5)).g,\n"
        "                texture(lutTex, vec2(u.b, 0.5)).b);\n"
        "  o = vec4(pow(min(g * %f, vec3(1.0)), vec3(%f)), 1.0); }\n",
        double(tg), double(tp));
    }
    auto gvs = renderer::spc::CompileGlsl(kGammaVS, /*is_vertex=*/true);
    auto gfs = renderer::spc::CompileGlsl(kGammaFS, /*is_vertex=*/false);
    if (dl.gamma_set && !gvs.empty() && !gfs.empty()) {
      VkShaderModule gvm = vk::util::CreateShaderModule(dev, gvs.data(), gvs.size() * 4);
      VkShaderModule gfm = vk::util::CreateShaderModule(dev, gfs.data(), gfs.size() * 4);
      if (gvm && gfm) {
        VkPipelineShaderStageCreateInfo gst[2] = {};
        gst[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        gst[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        gst[0].module = gvm;
        gst[0].pName = "main";
        gst[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        gst[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        gst[1].module = gfm;
        gst[1].pName = "main";
        VkVertexInputBindingDescription gvb = {0, sizeof(renderer::Draw2DVertex),
                                               VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription gva[4] = {
            {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(renderer::Draw2DVertex, x)},
            {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(renderer::Draw2DVertex, u)},
            {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(renderer::Draw2DVertex, rgba)},
            {3, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(renderer::Draw2DVertex, rgba2)}};
        VkPipelineVertexInputStateCreateInfo gvi = {};
        gvi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        gvi.vertexBindingDescriptionCount = 1;
        gvi.pVertexBindingDescriptions = &gvb;
        gvi.vertexAttributeDescriptionCount = 4;
        gvi.pVertexAttributeDescriptions = gva;
        VkPipelineInputAssemblyStateCreateInfo gia = {};
        gia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        gia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo gvp = {};
        gvp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        gvp.viewportCount = 1;
        gvp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo grs = {};
        grs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        grs.polygonMode = VK_POLYGON_MODE_FILL;
        grs.cullMode = VK_CULL_MODE_NONE;
        grs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo gms = {};
        gms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        gms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo gds = {};
        gds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        VkPipelineColorBlendAttachmentState gba = {};
        gba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo gcb = {};
        gcb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        gcb.attachmentCount = 1;
        gcb.pAttachments = &gba;
        const VkDynamicState gdyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                               VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo gdyn = {};
        gdyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        gdyn.dynamicStateCount = 2;
        gdyn.pDynamicStates = gdyn_states;
        VkGraphicsPipelineCreateInfo ggp = {};
        ggp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ggp.stageCount = 2;
        ggp.pStages = gst;
        ggp.pVertexInputState = &gvi;
        ggp.pInputAssemblyState = &gia;
        ggp.pViewportState = &gvp;
        ggp.pRasterizationState = &grs;
        ggp.pMultisampleState = &gms;
        ggp.pDepthStencilState = &gds;
        ggp.pColorBlendState = &gcb;
        ggp.pDynamicState = &gdyn;
        ggp.layout = dl.gamma_pipeline_layout;
        ggp.renderPass = dl.render_pass;
        df.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ggp, nullptr,
                                     &dl.gamma_pipeline);
      }
      if (gvm) df.vkDestroyShaderModule(device, gvm, nullptr);
      if (gfm) df.vkDestroyShaderModule(device, gfm, nullptr);
    }
    REXLOG_INFO("[native_vk] M3.100 display gamma ramp {}",
                dl.gamma_pipeline ? "ready" : "UNAVAILABLE");
  }

  dl.ready = true;
  REXLOG_INFO("[native_vk] draw layer ready (render pass + pipeline)");
  renderer::spc::RuntimeCompileSelfTest();  // M2.3: prove libshaderc links + compiles in-process
  return true;
}

bool EnsureVertexCapacity(vk::VulkanDevice* dev, VkDeviceSize needed) {
  auto& dl = DL();
  if (needed <= dl.vb_capacity) return true;
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  // GPU idle (fence-waited last frame) -> safe to destroy immediately.
  if (dl.vb_mapped) {
    df.vkUnmapMemory(device, dl.vb_mem);
    dl.vb_mapped = nullptr;
  }
  if (dl.vb) {
    df.vkDestroyBuffer(device, dl.vb, nullptr);
    dl.vb = VK_NULL_HANDLE;
  }
  if (dl.vb_mem) {
    df.vkFreeMemory(device, dl.vb_mem, nullptr);
    dl.vb_mem = VK_NULL_HANDLE;
  }
  const VkDeviceSize cap = std::max<VkDeviceSize>(64 * 1024, std::bit_ceil<uint64_t>(needed));
  if (!vk::util::CreateDedicatedAllocationBuffer(dev, cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                                                 vk::util::MemoryPurpose::kUpload, dl.vb,
                                                 dl.vb_mem, &dl.vb_mem_type, &dl.vb_mem_size)) {
    return false;
  }
  void* mapped = nullptr;
  if (df.vkMapMemory(device, dl.vb_mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    return false;
  }
  dl.vb_mapped = static_cast<uint8_t*>(mapped);
  dl.vb_capacity = cap;
  return true;
}

void DestroyTexEntry(vk::VulkanDevice* dev, TexEntry& t) {
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  if (t.ds_fb) df.vkDestroyFramebuffer(device, t.ds_fb, nullptr);      // M4.4
  if (t.att_view) df.vkDestroyImageView(device, t.att_view, nullptr);  // M4.4
  if (t.view) df.vkDestroyImageView(device, t.view, nullptr);
  if (t.image) df.vkDestroyImage(device, t.image, nullptr);
  if (t.memory) df.vkFreeMemory(device, t.memory, nullptr);
  // Recycle the descriptor set instead of leaking it (see DrawLayer::
  // tex_set_free). Safe: DestroyTexEntry only runs post-fence or at shutdown.
  if (t.set) DL().tex_set_free.push_back(t.set);
  t = TexEntry{};
}

// Get a single-sampler descriptor set from desc_pool: reuse a recycled one from
// the free-list (the caller re-points it via vkUpdateDescriptorSets) or, failing
// that, allocate fresh. Returns VK_NULL_HANDLE only on genuine pool exhaustion.
VkDescriptorSet AcquireTexSet(vk::VulkanDevice* dev) {
  auto& dl = DL();
  if (!dl.tex_set_free.empty()) {
    VkDescriptorSet s = dl.tex_set_free.back();
    dl.tex_set_free.pop_back();
    return s;
  }
  VkDescriptorSetAllocateInfo ai = {};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = dl.desc_pool;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &dl.ds_layout;
  VkDescriptorSet s = VK_NULL_HANDLE;
  if (dev->functions().vkAllocateDescriptorSets(dev->device(), &ai, &s) == VK_SUCCESS)
    return s;
  // M4.35: pool full (OUT_OF_POOL_MEMORY / FRAGMENTED_POOL). Before this, the
  // caller took the failure as "no texture" and the draw bound the 1x1 white
  // entry -- which is what turned characters and foliage white part-way into a
  // long session. Add a pool and retry once.
  if (!AddTexPool(dev)) {
    REXLOG_ERROR("[native_vk] M4.35 texture descriptor pool growth FAILED -- draws will "
                 "bind white from here");
    return VK_NULL_HANDLE;
  }
  ai.descriptorPool = dl.desc_pool;
  s = VK_NULL_HANDLE;
  dev->functions().vkAllocateDescriptorSets(dev->device(), &ai, &s);
  return s;
}

// Creates image + view + descriptor set + staging upload for RGBA8 pixels.
// M2.4: defined after TL() below; looks up a resolve-destination image.
VkDescriptorSet LookupRtTex(uint32_t phys);

// M4.0: create + fill a staging buffer for one full-image upload and queue
// it; the copy records at the head of this frame's command buffer with
// oldLayout=UNDEFINED (legal discard -- the whole image is overwritten), so it
// also serves in-place content refreshes of an existing image. Shared by
// CreateTexEntry and the re-decode fast path in ResolveTextureEntry.
// M4.3: data_bytes = 0 keeps the historical RGBA8 w*h*4 sizing; BC uploads
// pass their tightly-packed block-stream size instead (the copy region is
// still {w,h} texels -- Vulkan sizes compressed copies by extent).
bool StageTexUpload(vk::VulkanDevice* dev, VkImage image, uint32_t w, uint32_t h,
                    const uint8_t* rgba, VkDeviceSize data_bytes = 0) {
  auto& dl = DL();
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  const VkDeviceSize bytes = data_bytes ? data_bytes : VkDeviceSize(w) * h * 4;
  PendingUpload up;
  up.image = image;
  up.width = w;
  up.height = h;
  uint32_t staging_type = 0;
  VkDeviceSize staging_size = 0;
  if (!vk::util::CreateDedicatedAllocationBuffer(dev, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                                 vk::util::MemoryPurpose::kUpload, up.staging,
                                                 up.staging_mem, &staging_type, &staging_size)) {
    return false;
  }
  void* mapped = nullptr;
  if (df.vkMapMemory(device, up.staging_mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
    df.vkDestroyBuffer(device, up.staging, nullptr);
    df.vkFreeMemory(device, up.staging_mem, nullptr);
    return false;
  }
  std::memcpy(mapped, rgba, bytes);
  vk::util::FlushMappedMemoryRange(dev, up.staging_mem, staging_type, 0, staging_size, bytes);
  df.vkUnmapMemory(device, up.staging_mem);
  dl.pending_uploads.push_back(up);
  return true;
}

// M4.3: explicit_fmt != UNDEFINED overrides the RGBA8 format choice (the BC
// path passes BC1/2/3 [_SRGB]); data_bytes rides through to StageTexUpload.
bool CreateTexEntry(vk::VulkanDevice* dev, TexEntry& t, const uint8_t* rgba, uint32_t w,
                    uint32_t h, bool wrap = false, bool srgb = false,
                    VkFormat explicit_fmt = VK_FORMAT_UNDEFINED, VkDeviceSize data_bytes = 0) {
  auto& dl = DL();
  const auto& df = dev->functions();
  VkDevice device = dev->device();

  // sRGB textures (Xenos gamma sign-field) are sampled through an _SRGB view so
  // the hardware linearizes RGB on read (alpha stays linear). RESTUFF_NO_SRGB=1
  // forces UNORM for A/B. The decoded bytes are identical; only the view's
  // transfer function differs.
  static const bool no_srgb = getenv("RESTUFF_NO_SRGB") != nullptr;
  const VkFormat tex_fmt =
      explicit_fmt != VK_FORMAT_UNDEFINED
          ? explicit_fmt
          : ((srgb && !no_srgb) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);

  VkImageCreateInfo img_ci = {};
  img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = tex_fmt;
  img_ci.extent = {w, h, 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk::util::CreateDedicatedAllocationImage(dev, img_ci, vk::util::MemoryPurpose::kDeviceLocal,
                                                t.image, t.memory)) {
    return false;
  }
  {  // M4.36: record the real allocation size for the census.
    VkMemoryRequirements mr = {};
    df.vkGetImageMemoryRequirements(device, t.image, &mr);
    t.mem_bytes = mr.size;
  }

  VkImageViewCreateInfo view_ci = {};
  view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_ci.image = t.image;
  view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_ci.format = tex_fmt;
  view_ci.subresourceRange = vk::util::InitializeSubresourceRange();
  if (df.vkCreateImageView(device, &view_ci, nullptr, &t.view) != VK_SUCCESS) {
    return false;
  }

  if (t.set == VK_NULL_HANDLE) {
    t.set = AcquireTexSet(dev);
    if (t.set == VK_NULL_HANDLE) return false;
  }
  VkDescriptorImageInfo dii = {wrap ? dl.sampler_repeat : dl.sampler, t.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet wds = {};
  wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  wds.dstSet = t.set;
  wds.dstBinding = 0;
  wds.descriptorCount = 1;
  wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wds.pImageInfo = &dii;
  df.vkUpdateDescriptorSets(device, 1, &wds, 0, nullptr);

  // Staging buffer, filled now; the copy is recorded at the head of this
  // frame's command buffer.
  if (!StageTexUpload(dev, t.image, w, h, rgba, data_bytes)) return false;

  t.width = w;
  t.height = h;
  t.srgb = tex_fmt == VK_FORMAT_R8G8B8A8_SRGB || tex_fmt == VK_FORMAT_BC1_RGBA_SRGB_BLOCK ||
           tex_fmt == VK_FORMAT_BC2_SRGB_BLOCK || tex_fmt == VK_FORMAT_BC3_SRGB_BLOCK;
  t.vkfmt = tex_fmt;  // M4.3
  return true;
}

// Resolves the descriptor set for a draw's texture (0 = white), decoding and
// caching guest textures by physical address + content hash.
const TexEntry* LookupRtTexEntry(uint32_t phys, bool want_depth);
// M4.44 RTFETCHDIM: fetch-declared extent (width<<16 | height) per COLOUR
// resolve dest, recorded whenever a draw samples that dest. RecordResolve
// sizes the image by it -- see the note there. Present thread only.
inline std::unordered_map<uint32_t, uint32_t>& RtFetchDims() {
  static std::unordered_map<uint32_t, uint32_t> m;
  return m;
}

// Decode-and-cache a guest texture; returns the cache entry (view + set) or
// nullptr on hard failure. The white 1x1 entry serves invalid/unsupported.
const TexEntry* ResolveTextureEntry(vk::VulkanDevice* dev, const renderer::GuestTextureDesc& tex) {
  auto& dl = DL();
  // Address mode comes from the fetch constant, so one guest texture can need
  // both a wrap and a clamp descriptor -- key on phys | wrap (phys is
  // page-aligned, low bit is free).
  // M3.303 EDGECLAMP also applies HERE (this per-entry set is a second bind
  // path the combo-set override does not cover): an RT-sized texture never
  // legitimately tiles, and REPEAT at u=1.0 wraps column 0 into column 1279
  // (the right-edge smudge). Kill switch RESTUFF_NO_EDGECLAMP=1.
  static const bool s_eclamp = getenv("RESTUFF_NO_EDGECLAMP") == nullptr;  // M3.314: default on, see combo-path comment
  // M3.314b: cover the whole scene-resolve downsample chain (DOF-family post
  // uses quarter/eighth res too) — none of these can legitimately tile.
  const bool rt_sized_ec =
      (tex.width == 1280 && tex.height == 720) ||
      (tex.width == 640 && tex.height == 360) ||
      (tex.width == 320 && tex.height == 180) ||
      (tex.width == 160 && tex.height == 90);
  const bool wrap = tex.valid && tex.wants_wrap() && !(s_eclamp && rt_sized_ec);
  const uint64_t key = tex.valid ? (uint64_t(tex.phys_addr) | (wrap ? 1u : 0u)) : 0;

  static const uint32_t s_texbind_log =
      getenv("RESTUFF_TEXBIND_LOG")
          ? uint32_t(strtoul(getenv("RESTUFF_TEXBIND_LOG"), nullptr, 16)) : 0;
  if (key == 0) {
    if (s_texbind_log == 0xFFFFFFFFu) {
      static std::atomic<int> s_tbw{200};
      if (s_tbw.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[TEXBIND] INVALID desc -> WHITE (fmt={} {}x{} addr=0x{:08X} valid={} "
                    "ps={:016X})",
                    tex.format, tex.width, tex.height, tex.phys_addr, int(tex.valid),
                    restuff::native::CurrentPsHashForDebug());
    }
    auto& t = dl.textures[0];
    if (t.set == VK_NULL_HANDLE || t.image == VK_NULL_HANDLE) {
      const uint8_t white[4] = {255, 255, 255, 255};
      if (!CreateTexEntry(dev, t, white, 1, 1)) return nullptr;
      t.content_hash = 1;
    }
    return &t;
  }

  // M2.4: resolve destinations are GPU-side images -- serve them from the RT
  // cache (guest memory at that address holds nothing useful).
  // RESTUFF_TEXBIND_LOG=<hexaddr>: which path serves fetches of that address
  // (rt-hit + kind, or guest-decode fallthrough) -- glow-chain diagnosis.
  // fmt 22/23 (k_24_8 / k_24_8_FLOAT) are DEPTH formats -- the sun-shaft
  // samples depth, so ask for the depth entry at this address.
  const bool want_depth = tex.format == 22 || tex.format == 23;
  if (const TexEntry* rt = LookupRtTexEntry(tex.phys_addr, want_depth)) {
    // TEXBIND_LOG=FFFFFFFF: log every RT-served address once (which resolve
    // dests are actually consumed as textures, e.g. WHICH occ buffer the
    // shadow composite reads).
    if (s_texbind_log == 0xFFFFFFFFu) {
      static std::mutex s_mu;
      static std::set<uint32_t> s_seen;
      bool fresh;
      {
        std::lock_guard<std::mutex> lk(s_mu);
        fresh = s_seen.insert(tex.phys_addr).second;
      }
      if (fresh)
        REXLOG_INFO("[TEXRT] 0x{:08X} fmt={} {}x{} ps={:016X}", tex.phys_addr, tex.format,
                    tex.width, tex.height, 0ull);
    }
    if (s_texbind_log && tex.phys_addr == s_texbind_log) {
      static std::atomic<int> s_tb{24};
      if (s_tb.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[TEXBIND] 0x{:08X} fmt={} {}x{} -> RT {} {}x{}", tex.phys_addr, tex.format,
                    tex.width, tex.height, rt->is_depth ? "DEPTH" : "color", rt->width, rt->height);
    }
    // M4.44: remember the extent the guest's fetch constant declares for this
    // dest (colour only). RecordResolve sizes the image by it.
    if (!rt->is_depth && tex.width && tex.height)
      RtFetchDims()[tex.phys_addr] = (uint32_t(tex.width) << 16) | uint32_t(tex.height);
    return rt;
  }
  if (s_texbind_log && (tex.phys_addr == s_texbind_log || s_texbind_log == 0xFFFFFFFFu)) {
    static std::atomic<int> s_tbm{48};
    if (s_tbm.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[TEXBIND] 0x{:08X} fmt={} {}x{} -> RT MISS (guest decode)", tex.phys_addr,
                  tex.format, tex.width, tex.height);
  }

  // M3.295b (#37): default PERIOD=1 (per-frame verify) again. The period-8
  // variant showed exactly the predicted failure the moment it shipped: the
  // HUD multiplier bar displayed the loading screen's controller sprite for a
  // few frames (user-spotted) -- the game DOES stream new content into reused
  // texture addresses, and the "zero re-decodes per drive" justification was
  // read from a rotation-truncated log tail. The prep win is recovered by the
  // 4-lane hash in guest_texture_decode.cpp instead (same full coverage).
  // RESTUFF_HASH_PERIOD=N stays for experiments only.
  {
    static const uint32_t s_hash_period = [] {
      const char* e = getenv("RESTUFF_HASH_PERIOD");
      const uint32_t v = e ? uint32_t(strtoul(e, nullptr, 0)) : 1u;
      return v ? v : 1u;
    }();
    if (s_hash_period > 1) {
      auto cit = dl.textures.find(key);
      if (cit != dl.textures.end() && cit->second.image != VK_NULL_HANDLE) {
        const uint64_t frame = dl.frames_rendered.load(std::memory_order_relaxed);
        if (((frame + (tex.phys_addr >> 12)) % s_hash_period) != 0) {
          cit->second.last_used_frame = frame;  // M4.36
          return &cit->second;
        }
      }
    }
  }
  // M3.136: hash this texture at most once per frame (see content_hash_memo).
  // RESTUFF_NO_HASH_MEMO=1 restores the per-draw re-hash for A/B.
  static const bool s_no_memo = getenv("RESTUFF_NO_HASH_MEMO") != nullptr;
  uint64_t hash;
  if (s_no_memo) {
    hash = renderer::GuestTextureContentHash(tex);
  } else {
    const uint64_t frame = dl.frames_rendered.load(std::memory_order_relaxed);
    if (dl.content_hash_memo_frame != frame) {
      dl.content_hash_memo.clear();
      dl.content_hash_memo_frame = frame;
    }
    uint64_t ident = 1469598103934665603ull;
    for (uint64_t part : {uint64_t(tex.phys_addr), uint64_t(tex.width), uint64_t(tex.height),
                          uint64_t(tex.format), uint64_t(tex.pitch_texels)})
      ident = (ident ^ part) * 1099511628211ull;
    auto [mit, fresh] = dl.content_hash_memo.emplace(ident, 0ull);
    if (fresh) mit->second = renderer::GuestTextureContentHash(tex);
    hash = mit->second;
  }
  auto it = dl.textures.find(key);
  if (it != dl.textures.end() && it->second.content_hash == hash &&
      it->second.image != VK_NULL_HANDLE) {
    it->second.last_used_frame = dl.frames_rendered.load(std::memory_order_relaxed);  // M4.36
    return &it->second;
  }
  if (it != dl.textures.end()) {
    static std::atomic<int> s_restuff_budget{40};
    if (s_restuff_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXLOG_INFO("[native_vk] guest texture 0x{:08X} {}x{} fmt={} content CHANGED: re-decode",
                  tex.phys_addr, tex.width, tex.height, tex.format);
    }
  }

  // M4.3: native-BC upload path. When the device enables textureCompressionBC
  // (SDK whitelist addition, runtime-checked), DXT1/3/5 guest textures upload
  // their byteswapped+untiled blocks straight into BC1/2/3 images -- no CPU
  // palette decode, 4-8x fewer upload bytes. RESTUFF_NO_BC=1 forces the CPU
  // decode; the DUMP_TEX diagnostics do too (they inspect decoded RGBA).
  static const bool s_no_bc = getenv("RESTUFF_NO_BC") != nullptr;
  static const bool s_dump_tex_any =
      getenv("RESTUFF_DUMP_TEX") != nullptr || getenv("RESTUFF_DUMP_TEX_RAW") != nullptr;
  static const bool s_no_srgb_fmt = getenv("RESTUFF_NO_SRGB") != nullptr;
  const bool is_dxt = tex.format == 18 || tex.format == 19 || tex.format == 20;
  const bool use_bc = is_dxt && dev->properties().textureCompressionBC && !s_no_bc &&
                      !s_dump_tex_any;
  VkFormat bc_fmt = VK_FORMAT_UNDEFINED;
  if (use_bc) {
    const bool sr = tex.gamma && !s_no_srgb_fmt;
    bc_fmt = tex.format == 18
                 ? (sr ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK)
             : tex.format == 19 ? (sr ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK)
                                : (sr ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK);
  }
  if (is_dxt) {  // one-shot path announcement (both directions)
    static std::atomic<int> s_bc_announced{0};
    if (s_bc_announced.exchange(1, std::memory_order_relaxed) == 0)
      REXLOG_INFO("[native_vk] M4.3 DXT textures: {} (device textureCompressionBC={} no_bc={})",
                  use_bc ? "native BC upload" : "CPU decode",
                  dev->properties().textureCompressionBC, s_no_bc);
  }
  std::vector<uint8_t> rgba;  // RGBA8 texels, or the BC block stream (M4.3)
  uint32_t w = 0, h = 0;
  const bool decoded = use_bc ? renderer::CopyGuestBCBlocks(tex, rgba, w, h)
                              : renderer::DecodeGuestTexture(tex, rgba, w, h);

  if (it != dl.textures.end()) {
    // M4.0: content changed but the shape didn't -- refresh the EXISTING image
    // in place instead of destroy+recreate. The upload records at the head of
    // this frame's command buffer (before every draw) and frames are fully
    // fence-waited, so the previous frame's reads are done and ALL of this
    // frame's draws see the new content -- strictly more coherent than the old
    // old-image/new-image split. Crucially this skips tex_views_retired, whose
    // every firing reset the translated path's whole combo-descriptor pool:
    // the ~20-28Hz glyph-atlas/odometer re-decodes each dragged a full
    // descriptor rebuild behind them. RESTUFF_NO_TEX_INPLACE=1 restores the
    // old behaviour for A/B.
    static const bool s_no_inplace = getenv("RESTUFF_NO_TEX_INPLACE") != nullptr;
    static const bool s_no_srgb_ip = getenv("RESTUFF_NO_SRGB") != nullptr;
    TexEntry& e = it->second;
    const bool want_srgb = tex.gamma && !s_no_srgb_ip;
    // M4.3: the reuse gate is now the actual VkFormat (covers srgb AND the
    // BC/RGBA split -- a path or format flip at one address must recreate).
    const VkFormat want_fmt =
        use_bc ? bc_fmt : (want_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM);
    if (!s_no_inplace && decoded && e.image != VK_NULL_HANDLE && e.width == w &&
        e.height == h && e.vkfmt == want_fmt &&
        StageTexUpload(dev, e.image, w, h, rgba.data(),
                       use_bc ? VkDeviceSize(rgba.size()) : 0)) {
      e.content_hash = hash;
      // M4.37: an in-place refresh IS a use -- without this the entry looks
      // permanently cold to the LRU sweep and gets evicted while hot.
      e.last_used_frame = dl.frames_rendered.load(std::memory_order_relaxed);
      return &e;
    }
    // Shape/format changed (or the staging alloc failed): decode + upload into
    // a fresh image. The old image may be referenced by draws already recorded
    // THIS frame -- defer its destruction to after the frame's fence wait
    // instead of destroying it mid-frame.
    dl.deferred_destroy.push_back(e);
    ++dl.tex_retire_epoch;  // M4.5: epoch (see decl)
    dl.textures.erase(it);
  }

  if (!decoded) {
    // Unsupported format: render flat via the white texture.
    return ResolveTextureEntry(dev, renderer::GuestTextureDesc{});
  }
  // RESTUFF_DUMP_TEX_RAW=1: also dump the raw guest texture bytes (pre-decode)
  // so the DXT block decode can be verified independently offline.
  if (getenv("RESTUFF_DUMP_TEX_RAW")) {
    static std::set<uint32_t> s_rawdumped;
    if (s_rawdumped.insert(tex.phys_addr).second) {
      size_t nbytes = 0;
      if (tex.format == 18) nbytes = size_t(tex.width / 4) * (tex.height / 4) * 8;   // DXT1
      else if (tex.format == 19 || tex.format == 20)
        nbytes = size_t(tex.width / 4) * (tex.height / 4) * 16;  // DXT2/3, DXT4/5
      else nbytes = size_t(tex.pitch_texels) * tex.height * 4;
      if (const uint8_t* raw = renderer::DebugGuestPhysPtr(tex.phys_addr)) {
        char path[128];
        snprintf(path, sizeof(path), "shader_dump/texraw_%08X_%ux%u_f%u_t%u.bin", tex.phys_addr,
                 tex.width, tex.height, tex.format, tex.tiled ? 1 : 0);
        if (FILE* f = fopen(path, "wb")) { fwrite(raw, 1, nbytes, f); fclose(f); }
      }
    }
  }
  // RESTUFF_DUMP_TEX=1: write each decoded texture to shader_dump/ as PPM (RGB)
  // + PGM (alpha) once per address, for eyeballing decode correctness.
  if (getenv("RESTUFF_DUMP_TEX")) {
    static std::set<uint32_t> s_dumped;
    if (s_dumped.insert(tex.phys_addr).second && w && h) {
      char path[128];
      snprintf(path, sizeof(path), "shader_dump/tex_%08X_%ux%u_f%u.ppm", tex.phys_addr, w, h,
               tex.format);
      if (FILE* f = fopen(path, "wb")) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < size_t(w) * h; ++i) fwrite(&rgba[i * 4], 1, 3, f);
        fclose(f);
      }
      snprintf(path, sizeof(path), "shader_dump/tex_%08X_%ux%u_f%u_a.pgm", tex.phys_addr, w, h,
               tex.format);
      if (FILE* f = fopen(path, "wb")) {
        fprintf(f, "P5\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < size_t(w) * h; ++i) fwrite(&rgba[i * 4 + 3], 1, 1, f);
        fclose(f);
      }
    }
  }
  TexEntry t;
  if (!CreateTexEntry(dev, t, rgba.data(), w, h, wrap, tex.gamma,
                      use_bc ? bc_fmt : VK_FORMAT_UNDEFINED,
                      use_bc ? VkDeviceSize(rgba.size()) : 0)) {
    DestroyTexEntry(dev, t);
    return nullptr;
  }
  t.content_hash = hash;
  static std::atomic<int> s_srgb_cnt{0}, s_lin_cnt{0};
  const int g_after = tex.gamma ? s_srgb_cnt.fetch_add(1, std::memory_order_relaxed) + 1
                                : (s_lin_cnt.fetch_add(1, std::memory_order_relaxed), -1);
  // Unbudgeted: announce the FIRST gamma(sRGB) texture so we know the mechanism
  // fires at all, and every 32nd after, without spamming.
  if (tex.gamma && (g_after == 1 || g_after % 32 == 0))
    REXLOG_INFO("[native_vk] GAMMA(sRGB) texture #{} 0x{:08X} {}x{} fmt={}", g_after, tex.phys_addr,
                w, h, tex.format);
  static std::atomic<int> s_tex_log_budget{12};
  if (s_tex_log_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
    REXLOG_INFO("[native_vk] guest texture 0x{:08X} {}x{} fmt={} tiled={} gamma={} decoded (srgb={} lin={})",
                tex.phys_addr, w, h, tex.format, tex.tiled, tex.gamma,
                s_srgb_cnt.load(std::memory_order_relaxed),
                s_lin_cnt.load(std::memory_order_relaxed));
  }
  // M4.37: a decode of a key we recently evicted means the sweep was wrong
  // about it -- count it so [TEXCENSUS] shows thrash.
  if (!dl.ev_recent_keys.empty() && dl.ev_recent_keys.erase(key)) ++dl.ev_redecoded;
  auto& slot = dl.textures[key];
  slot = std::move(t);
  slot.last_used_frame = dl.frames_rendered.load(std::memory_order_relaxed);  // M4.37
  return &slot;
}

VkDescriptorSet ResolveTexture(vk::VulkanDevice* dev, const renderer::GuestTextureDesc& tex) {
  const TexEntry* e = ResolveTextureEntry(dev, tex);
  return e ? e->set : VK_NULL_HANDLE;
}

VkFramebuffer GetFramebuffer(vk::VulkanDevice* dev, VkImageView view, uint64_t version,
                             uint32_t w, uint32_t h) {
  auto& dl = DL();
  const auto& df = dev->functions();
  auto& slot = dl.fbs[version % dl.fbs.size()];
  if (slot.version == version && slot.view == view) return slot.fb;
  if (slot.fb) df.vkDestroyFramebuffer(dev->device(), slot.fb, nullptr);  // GPU idle: fence-waited
  VkFramebufferCreateInfo fb_ci = {};
  fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_ci.renderPass = dl.render_pass;
  fb_ci.attachmentCount = 1;
  fb_ci.pAttachments = &view;
  fb_ci.width = w;
  fb_ci.height = h;
  fb_ci.layers = 1;
  df.vkCreateFramebuffer(dev->device(), &fb_ci, nullptr, &slot.fb);
  slot.version = version;
  slot.view = view;
  return slot.fb;
}

void DestroyDrawLayer(vk::VulkanDevice* dev) {
  auto& dl = DL();
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  for (auto& fb : dl.fbs) {
    if (fb.fb) df.vkDestroyFramebuffer(device, fb.fb, nullptr);
    fb = CachedFb{};
  }
  for (auto& [key, t] : dl.textures) {
    DestroyTexEntry(dev, t);
  }
  dl.textures.clear();
  for (auto& t : dl.deferred_destroy) DestroyTexEntry(dev, t);
  dl.deferred_destroy.clear();
  auto retire = [&](std::vector<PendingUpload>& ups) {
    for (auto& up : ups) {
      if (up.staging) df.vkDestroyBuffer(device, up.staging, nullptr);
      if (up.staging_mem) df.vkFreeMemory(device, up.staging_mem, nullptr);
    }
    ups.clear();
  };
  retire(dl.pending_uploads);
  retire(dl.retired_uploads);
  if (dl.vb_mapped) {
    df.vkUnmapMemory(device, dl.vb_mem);
    dl.vb_mapped = nullptr;
  }
  if (dl.vb) df.vkDestroyBuffer(device, dl.vb, nullptr);
  if (dl.vb_mem) df.vkFreeMemory(device, dl.vb_mem, nullptr);
  dl.vb = VK_NULL_HANDLE;
  dl.vb_mem = VK_NULL_HANDLE;
  dl.vb_capacity = 0;
  for (auto& fam : dl.pipelines) {
    for (auto& p : fam) {
      if (p) df.vkDestroyPipeline(device, p, nullptr);
      p = VK_NULL_HANDLE;
    }
  }
  if (dl.pipeline_layout) df.vkDestroyPipelineLayout(device, dl.pipeline_layout, nullptr);
  if (dl.render_pass) df.vkDestroyRenderPass(device, dl.render_pass, nullptr);
  for (VkDescriptorPool p : dl.desc_pools)  // M4.35: every grown pool
    if (p) df.vkDestroyDescriptorPool(device, p, nullptr);
  dl.desc_pools.clear();
  dl.tex_set_free.clear();  // those sets died with their pools
  if (dl.ds_layout) df.vkDestroyDescriptorSetLayout(device, dl.ds_layout, nullptr);
  dl.pipeline_layout = VK_NULL_HANDLE;
  dl.render_pass = VK_NULL_HANDLE;
  dl.desc_pool = VK_NULL_HANDLE;
  dl.ds_layout = VK_NULL_HANDLE;
  dl.ready = false;
}

}  // namespace

NativeVulkanGraphicsSystem::NativeVulkanGraphicsSystem() = default;

NativeVulkanGraphicsSystem::~NativeVulkanGraphicsSystem() { Shutdown(); }

bool NativeVulkanGraphicsSystem::has_presentation() const { return presenter_ != nullptr; }

rex::ui::GraphicsProvider* NativeVulkanGraphicsSystem::provider() const { return provider_.get(); }

rex::ui::Presenter* NativeVulkanGraphicsSystem::presenter() const { return presenter_.get(); }

X_STATUS NativeVulkanGraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext* /*app_context*/) {
  // Core Vulkan provider (not the xenos plugin). with_gpu_emulation=true: the
  // guest-GPU feature set includes depthClamp, which M3.107 needs for closed
  // shadow-volume shells (the volumes extrude past the far plane with w<=0
  // homogeneous verts; only rasterizer depth clamp keeps their caps). The
  // reference plugin device runs with the same feature set on this GPU.
  provider_ = vk::VulkanProvider::Create(/*with_gpu_emulation=*/true,
                                         /*with_presentation=*/true);
  if (!provider_) {
    REXLOG_ERROR("[native_vk] VulkanProvider::Create failed");
    return X_STATUS_UNSUCCESSFUL;
  }
  presenter_ = provider_->CreatePresenter();
  if (!presenter_) {
    REXLOG_ERROR("[native_vk] CreatePresenter failed");
    return X_STATUS_UNSUCCESSFUL;
  }

  // Command infrastructure for our per-frame submissions.
  vk::VulkanDevice* dev = provider_->vulkan_device();
  const auto& df = dev->functions();
  VkDevice device = dev->device();

  VkCommandPoolCreateInfo pool_ci = {};
  pool_ci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_ci.queueFamilyIndex = dev->queue_family_graphics_compute();
  if (df.vkCreateCommandPool(device, &pool_ci, nullptr, &cmd_pool_) != VK_SUCCESS) {
    REXLOG_ERROR("[native_vk] vkCreateCommandPool failed");
    return X_STATUS_UNSUCCESSFUL;
  }

  VkCommandBufferAllocateInfo cb_ai = {};
  cb_ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cb_ai.commandPool = cmd_pool_;
  cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_ai.commandBufferCount = 2;  // M4.5: one per frame slot
  if (df.vkAllocateCommandBuffers(device, &cb_ai, cmd_bufs_) != VK_SUCCESS) {
    REXLOG_ERROR("[native_vk] vkAllocateCommandBuffers failed");
    return X_STATUS_UNSUCCESSFUL;
  }

  VkFenceCreateInfo fence_ci = {};
  fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  for (int i = 0; i < 2; ++i) {
    if (df.vkCreateFence(device, &fence_ci, nullptr, &fences_[i]) != VK_SUCCESS) {
      REXLOG_ERROR("[native_vk] vkCreateFence failed");
      return X_STATUS_UNSUCCESSFUL;
    }
  }

  if (!CreateDrawLayer(*provider_)) {
    return X_STATUS_UNSUCCESSFUL;
  }

  REXLOG_INFO("[native_vk] presentation up (VulkanProvider + VulkanPresenter)");
  return X_STATUS_SUCCESS;
}

X_STATUS NativeVulkanGraphicsSystem::SetupGuestGpu(
    rex::runtime::FunctionDispatcher* function_dispatcher,
    rex::system::KernelState* kernel_state) {
  // M1.2a: bring up the guest life-support (MMIO claim + shadow registers +
  // mini-PM4; the interrupt pump starts lazily when the guest registers its
  // ISR). Must run before the guest boots — GPU MMIO reads are uninitialized
  // garbage until the range is claimed.
  restuff::native::Initialize(function_dispatcher, kernel_state);

  if (!running_.exchange(true)) {
    present_thread_ = std::thread(&NativeVulkanGraphicsSystem::PresentThreadMain, this);
  }
  return X_STATUS_SUCCESS;
}

void NativeVulkanGraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  restuff::native::SetGuestInterruptCallback(callback, user_data);
}

void NativeVulkanGraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  restuff::native::SetRingBuffer(ptr, size_log2);
}

void NativeVulkanGraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr,
                                                            uint32_t block_size_log2) {
  restuff::native::SetRingWritebackSlot(ptr, block_size_log2);
}

// #37: one line naming every RESTUFF_* env var this process runs with, so a
// log can always answer "was that run instrumented / which switches were on".
// Logged at present-thread start AND periodically from the alive block -- the
// dbg log truncates its own head at the 5MB rotation, so a boot-only line is
// routinely lost from any session longer than ~2 minutes.
extern "C" char** environ;
static std::string RestuffEnvSummary() {
  std::string s;
  for (char** e = environ; e && *e; ++e) {
    if (strncmp(*e, "RESTUFF_", 8) == 0) {
      if (!s.empty()) s += ' ';
      s += *e;
    }
  }
  return s.empty() ? std::string("(none)") : s;
}

void NativeVulkanGraphicsSystem::PresentThreadMain() {
  REXLOG_INFO("[native_vk] present thread started");
  REXLOG_INFO("[ENV] {}", RestuffEnvSummary());
#ifdef _WIN32
  // M3.298: request 1ms timer resolution. Windows (and Wine) default to
  // coarse timers, so every sleep_for and kernel wait overshoots by 1-15ms --
  // invisible inside a 33ms/30fps budget, fatal inside 16.7ms: the Wine
  // measurement showed guest_fps 41.6 vs 59.4 native, exactly a ~2-4ms
  // per-frame-wait tax. Process-wide, affects the guest's fence waits too.
  {
    HMODULE winmm = LoadLibraryA("winmm.dll");
    if (winmm) {
      auto tbp = reinterpret_cast<UINT(WINAPI*)(UINT)>(GetProcAddress(winmm, "timeBeginPeriod"));
      if (tbp) REXLOG_INFO("[native_vk] M3.298 timeBeginPeriod(1) -> {}", tbp(1));
    }
  }
#endif
  {
    // Log the guest-visible video mode once: the game lays out its UI from
    // this, so a wrong width/aspect here mis-sizes backdrops (the title-screen
    // right band investigation).
    rex::system::X_VIDEO_MODE vm = {};
    rex::kernel::xboxkrnl::VdQueryVideoMode(&vm);
    REXLOG_INFO(
        "[native_vk] VdQueryVideoMode: {}x{} interlaced={} widescreen={} hidef={} "
        "refresh={} standard={}",
        uint32_t(vm.display_width), uint32_t(vm.display_height),
        uint32_t(vm.is_interlaced), uint32_t(vm.is_widescreen),
        uint32_t(vm.is_hi_def), float(vm.refresh_rate), uint32_t(vm.video_standard));
  }
  // M3.52: skip the full-scene re-record when the guest produced no new frame.
  // The guest renders ~20fps but this loop presents 60Hz, so ~2 of every 3
  // presents re-record all ~2000 draws for identical content -- pure CPU on the
  // present thread that, on a core-limited host, starves the guest's fence
  // writer (the 0x1FCA3000 completion wait = the 20-vs-60fps regression, M3.49).
  // When nothing new is ready, skip (the scene target keeps the last image, so
  // no black frame) and poll again soon so the NEXT real frame still presents
  // promptly; force a present every 100ms so the ImGui overlay stays live.
  // OPT-IN for now (RESTUFF_SKIP_REDRAW=1): can't fully exercise the skip path
  // on a fast host (it always has a new frame ready), and the SDK presenter's
  // tolerance for skipped presents is unproven -- default it on once verified in
  // real gameplay. Cannot crash: we only skip when nothing new exists and the
  // last frame stays on the swapchain (no black/stale-wrong frame possible).
  static const bool skip_redraw = getenv("RESTUFF_SKIP_REDRAW") != nullptr;
  // #37 (RESTUFF_PACE60=1): the combination the M3.134 post-mortem below calls
  // for, landed together -- present only when the guest produced a new frame
  // (or the 100ms overlay repaint is due), pace to the 60 Hz deadline instead
  // of adding a fixed 16ms on top of the work, and yield 3ms when idle so the
  // guest keeps the core. Alone, deadline pacing spun on stale content and
  // starved the guest; alone, the fixed sleep caps the loop at 1/(work+16ms).
  // Default ON since M3.297: user-verified live (p50 16.73ms, hitches>33ms
  // 600/600 -> 1/600 per window; loads visibly smoother). RESTUFF_NO_PACE60=1
  // restores the legacy fixed-sleep loop.
  static const bool s_pace60 = getenv("RESTUFF_NO_PACE60") == nullptr;
  auto last_present = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_relaxed)) {
    const bool overlay_due =
        (std::chrono::steady_clock::now() - last_present) >= std::chrono::milliseconds(100);
    const bool has_new = restuff::renderer::HasRawFrame();
    if ((s_pace60 || skip_redraw) ? (has_new || overlay_due) : true) {
      // M3.134: pace to the 60 Hz budget, do not ADD to it. This used to
      // sleep_for(16ms) unconditionally AFTER presenting, so the loop ran at
      // 1/(work + 16ms) and could never reach 60 no matter how fast the frame
      // was -- with ~18ms of real work that is ~29 fps of ceiling before
      // anything else goes wrong, and it was the bulk of the "OUTSIDE our
      // code" time that the frame accounting could not explain. Sleep only the
      // REMAINDER of the interval measured from the START of the frame.
      // RESTUFF_LEGACY_PRESENT_SLEEP=1 restores the old behaviour for A/B.
      // STATUS: OPT-IN (RESTUFF_PACE_PRESENT=1) -- DEFAULT REMAINS THE OLD
      // FIXED SLEEP. Pacing to the deadline alone BLACKS THE SCREEN: with the
      // sleep gone the loop spun to ~9500 presents against ~30 guest frames/s,
      // so almost every present carried no new content (trans=0 for a whole
      // verified drive) and the guest sat parked -- the fixed sleep was also
      // acting as the yield the M3.52 comment above warns about. The pacing
      // idea is still right (1/(work+16ms) can never reach 60), but it needs
      // the skip-redraw path (present only when HasRawFrame(), which is itself
      // still opt-in) landing WITH it, plus a real yield when idle.
      static const bool s_legacy_sleep =
          getenv("RESTUFF_PACE_PRESENT") == nullptr && !s_pace60;
      const auto frame_start = std::chrono::steady_clock::now();
      PresentClearFrame();
      last_present = frame_start;
      // M3.313 (RESTUFF_DUMPGO=<dir>): smudge ground truth. Every ~2s, dump the
      // presenter's guest-output image (post gamma quad, pre paint/WSI) to a
      // timestamped PPM. Paired with same-second x11 grabs this splits the
      // right-edge smudge at the guest-output boundary WITHOUT the RenderDoc
      // frame-counter timing lottery -- the Aug-22 "present boundary" verdict
      // died to exactly that (captures were of a different scene than the
      // streaked grab; caught by LOOKING per dont-overclaim-renders).
      {
        static const char* s_dumpgo = getenv("RESTUFF_DUMPGO");
        if (s_dumpgo && presenter_) {
          static auto s_last_dump = std::chrono::steady_clock::time_point{};
          const auto nowd = std::chrono::steady_clock::now();
          if (nowd - s_last_dump >= std::chrono::milliseconds(2000)) {
            s_last_dump = nowd;
            rex::ui::RawImage img;
            if (presenter_->CaptureGuestOutput(img) && img.width && img.height) {
              const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  nowd.time_since_epoch()).count();
              char path[512];
              snprintf(path, sizeof(path), "%s/go_%lld.ppm", s_dumpgo,
                       (long long)ms);
              if (FILE* f = fopen(path, "wb")) {
                fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
                std::vector<uint8_t> row(size_t(img.width) * 3);
                for (uint32_t y = 0; y < img.height; ++y) {
                  const uint8_t* src = img.data.data() + size_t(y) * img.stride;
                  for (uint32_t x = 0; x < img.width; ++x) {
                    row[x * 3 + 0] = src[x * 4 + 0];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + 2];
                  }
                  fwrite(row.data(), 1, row.size(), f);
                }
                fclose(f);
              }
            }
          }
        }
      }
      if (s_legacy_sleep) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
      } else {
        constexpr auto kBudget = std::chrono::microseconds(16667);
        const auto target = frame_start + kBudget;
        const auto now2 = std::chrono::steady_clock::now();
#ifdef _WIN32
        // M3.298: Windows/Wine sleeps overshoot even at 1ms timer resolution;
        // sleep to ~2ms short of the deadline, spin-yield the rest.
        constexpr auto kSpinMargin = std::chrono::microseconds(2000);
        if (now2 + kSpinMargin < target)
          std::this_thread::sleep_for(target - now2 - kSpinMargin);
        while (std::chrono::steady_clock::now() < target) std::this_thread::yield();
#else
        if (now2 < target) std::this_thread::sleep_for(target - now2);
#endif
      }
    } else {
      // Nothing new to draw -- yield the core to the guest and re-poll soon.
      std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
  }
}

// ===========================================================================
// M2.3 translated-shader render path (behind use_translated_shaders). Uploads
// the raw guest vertex buffer and transforms it on-GPU with the translated
// vertex/pixel shaders; guest constants ride in dynamic UBOs, textures reuse
// the DrawLayer texture cache (set 1 == DrawLayer.ds_layout, single sampler).
// ===========================================================================
namespace {

// vec4 c[256] + uvec4 lc[8] (32 loop consts) + uvec4 bc[2] (256 bool bits),
// rounded up to a 256-byte dynamic-offset boundary.
constexpr uint32_t kConstDataBytes = 256 * 16 + 8 * 16 + 2 * 16;
constexpr uint32_t kConstBlockBytes = (kConstDataBytes + 255) & ~255u;  // 4352
// M3.11/M3.14: fixed descriptor range for the rel-fetch storage buffers; the
// vertex ring is padded by this much so dynamic offset + range always fits.
// 8 MB: slot-95 neighbor/morph reads window into multi-MB static mesh streams.
constexpr VkDeviceSize kRelStreamRange = 8u << 20;

struct RingBuf {
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  uint8_t* mapped = nullptr;
  VkDeviceSize capacity = 0;
  uint32_t mem_type = 0;
  VkDeviceSize mem_size = 0;
};

// Grows a persistently-mapped host-visible buffer (frame is fence-waited, so
// destroy-on-grow is safe). Returns true; sets *recreated when reallocated.
// M4.5: retire_sink (optional) receives the OLD buffer instead of an
// immediate destroy -- required for the SHARED vertex ring under pipelining,
// where the in-flight frame still reads the old buffer. Per-slot rings keep
// the immediate destroy (their own fence was waited before prepare).
bool EnsureRing(vk::VulkanDevice* dev, RingBuf& rb, VkDeviceSize needed,
                VkBufferUsageFlags usage, bool* recreated,
                std::vector<std::pair<VkBuffer, VkDeviceMemory>>* retire_sink = nullptr) {
  if (recreated) *recreated = false;
  if (needed <= rb.capacity) return true;
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  if (rb.mapped) { df.vkUnmapMemory(device, rb.mem); rb.mapped = nullptr; }
  if (retire_sink && (rb.buf || rb.mem)) {
    retire_sink->emplace_back(rb.buf, rb.mem);
    rb.buf = VK_NULL_HANDLE;
    rb.mem = VK_NULL_HANDLE;
  }
  if (rb.buf) { df.vkDestroyBuffer(device, rb.buf, nullptr); rb.buf = VK_NULL_HANDLE; }
  if (rb.mem) { df.vkFreeMemory(device, rb.mem, nullptr); rb.mem = VK_NULL_HANDLE; }
  const VkDeviceSize cap = std::max<VkDeviceSize>(256 * 1024, std::bit_ceil<uint64_t>(needed));
  if (!vk::util::CreateDedicatedAllocationBuffer(dev, cap, usage, vk::util::MemoryPurpose::kUpload,
                                                 rb.buf, rb.mem, &rb.mem_type, &rb.mem_size)) {
    return false;
  }
  void* m = nullptr;
  if (df.vkMapMemory(device, rb.mem, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) return false;
  rb.mapped = static_cast<uint8_t*>(m);
  rb.capacity = cap;
  if (recreated) *recreated = true;
  return true;
}

struct TranslatedLayer {
  bool init = false;
  VkDescriptorSetLayout ubo_layout = VK_NULL_HANDLE;
  VkDescriptorPool ubo_pool = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  // M3.0: set 1 = one combined-image-sampler binding per texture fetch slot
  // (0..kMaxTexSlots-1). Per-draw slot combinations are cached as
  // allocate-only descriptor sets keyed by the bound views; cache + pool are
  // reset whenever a guest texture is re-decoded (a reused view handle would
  // otherwise alias a stale combo).
  VkDescriptorSetLayout tex_layout = VK_NULL_HANDLE;
  // M4.5: per-frame-slot resources. Everything here is rewritten from scratch
  // each frame, so under pipelining each in-flight frame needs its own copy;
  // serialized mode (default) uses slot 0 only, which is exactly the
  // historical single-buffered layout. Retire lists collect resources whose
  // last GPU user is THIS slot's submitted frame; RetireSlot() destroys them
  // right after this slot's fence wait.
  struct FrameSlot {
    RingBuf ib, ubo;
    VkDescriptorSet ubo_set = VK_NULL_HANDLE;
    // Pools grow (never reset mid-frame: sets already recorded this frame
    // must stay valid); this slot's pools reset together at its own frame
    // boundary when its epoch trails the retire epoch (M3.0 aliasing rule).
    std::vector<VkDescriptorPool> tex_pools;
    std::unordered_map<uint64_t, VkDescriptorSet> tex_combos;
    uint64_t combo_epoch = 0;
    // Pipelined vb-scratch windowing: the absolute extent of this slot's
    // in-flight VOLATILE writes in the shared vb ring (scratch region, i.e.
    // everything above the promoted arena). The other slot's frame anchors at
    // the arena bump but hops over this region when they would overlap; with
    // no promotions the two regions ping-pong stably, and a promoting frame
    // absorbs at most one region's worth of dead bytes into the arena
    // (reclaimed at the next wrap/realloc).
    VkDeviceSize region_base = 0, region_end = 0;
    // Retirement (pipelined): moved here at submit, freed post-fence.
    std::vector<TexEntry> r_deferred_destroy;
    std::vector<PendingUpload> r_retired_uploads;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> r_retired_buffers;
  };
  FrameSlot slots[2];
  uint32_t slot_ix = 0;
  FrameSlot& cur() { return slots[slot_ix]; }
  RingBuf vb;
  // M3.299: cross-frame vertex-ring cache. Capture keeps stream payloads
  // alive as immutable shared vectors (the 4fps-fix cache), so payload
  // ADDRESS is content identity while we pin a reference -- re-copying
  // 37-60MB of identical bytes per frame was 75% of upload_and_rec. The ring
  // becomes a persistent bump arena: hits reuse their offset, misses append
  // at vb_bump. Entries whose pin is the last reference (capture dropped the
  // payload) are swept periodically; buffer growth invalidates all offsets
  // (EnsureRing reallocates) so the cache clears and rebuilds by missing.
  struct VbCacheEntry {
    VkDeviceSize off;
    std::shared_ptr<const std::vector<uint8_t>> pin;
    // M3.315 VBCACHE_VERIFY: content fingerprint at insert (size + crc32 of
    // up to the first 4KB). The wedge forensics (Aug 25) proved the guest
    // submits identical geometry in wedge vs clean boots yet split strips
    // rasterize wrong -- if the capture layer rebuilds payload bytes in place,
    // this cache serves stale vertices. Fingerprint mismatch on hit = the
    // immutability contract is broken by that payload.
    uint32_t fp_crc = 0;
    uint32_t fp_size = 0;
  };
  std::unordered_map<const void*, VbCacheEntry> vb_cache;
  VkDeviceSize vb_bump = 0;
  // M3.300: last frame's cache misses. A payload that misses two frames
  // running is stable content (world geometry) and earns an arena slot;
  // a first-time miss is copied to the per-frame scratch region instead --
  // dynamic payloads (skinned meshes, particles, UI text rebuilt per frame)
  // never enter the arena, so it stops growing ~1MB/frame and the constant
  // grow-realloc-clear-recopy cycle (the Wine 56.7->36 fps decay, same spot,
  // and the 24ms Linux prep spikes) stops with it.
  std::unordered_map<const void*, std::weak_ptr<const std::vector<uint8_t>>> vb_lastmiss;
  std::unordered_map<uint64_t, VkPipeline> pipelines;
  std::vector<renderer::RawGuestDraw> frame;
  // M3.59: per-frame per-shader majority sign of det(WVP linear part). Draws
  // whose sign is the minority for their shader are mirrored instances (a
  // negative-determinant world matrix) -> their winding is inverted vs the
  // shader's norm -> flip front-face so cull-back keeps the true front faces.
  std::unordered_map<uint64_t, int> wind_majority;
  bool ready = false;
  std::atomic<uint64_t> draws{0};
  // M3.15: async pipeline builder -- vkCreateGraphicsPipelines can take
  // hundreds of ms per pipeline (NVIDIA SPIR-V compile) and a level start
  // needs hundreds of pipelines; building them on the present thread stalled
  // presentation ~15 s. Misses enqueue here and the draw drops (nopipe) until
  // the worker publishes the pipeline via pipe_done.
  struct PipeReq {
    uint64_t key = 0;
    const renderer::spc::CachedShader* vs = nullptr;
    const renderer::spc::CachedShader* ps = nullptr;
    uint64_t vs_hash = 0, ps_hash = 0;
    uint32_t prim = 0, blend_control = 0, color_mask = 0xF, depth_control = 0, su_mode = 0;
    uint32_t stencil_ref_mask = 0, stencil_ref_mask_bf = 0;
    bool mirror = false;      // M3.18: viewport sign parity (flips winding)
    float vport_xscale = 0.0f;  // M3.23: for the small-viewport depth-off key
    float vport_zscale = -1.0f;  // M3.58: sign selects the depth-ALWAYS neutralize
    bool wind_flip = false;      // M3.59: mirrored-instance winding flip decision
    bool prim_reset = false;     // M3.126: guest primitive-restart enable (key input)
    // M4.0: RB_SURFACE_INFO at capture. The M3.118 neutralize_alwaysz key bit
    // tests (dbg_surf & 0x3FFF) >= 1000, but this field never rode along in the
    // request -- the worker rebuilt those draws under a key WITHOUT the bit
    // (fd.dbg_surf defaulted to 0), published under the wrong key, and the
    // present thread's key rotted in pipe_inflight forever (permanent nopipe).
    uint32_t dbg_surf = 0;
    bool prewarm = false;  // M4.0: queued by the startup replayer (see counter)
  };
  size_t prewarm_queued = 0;  // pipe_mutex: prewarm requests still in pipe_queue
  // M4.41: workers currently INSIDE a build. "queue empty" alone is not
  // "warming finished" -- the last few pipelines are still compiling in the
  // driver at that moment, and RESTUFF_PREWARM_ONLY must not exit before they
  // land in the pipeline cache.
  size_t pipe_active = 0;                 // pipe_mutex
  std::condition_variable pipe_idle_cv;   // signalled when pipe_active hits 0
  // M4.42: the replay thread has finished ENQUEUEING every record. Without
  // this the drain predicate is true the instant it is first evaluated (the
  // queue has not been filled yet) and the warm "completes" having built
  // nothing.
  bool prewarm_enqueue_done = false;  // pipe_mutex
  std::vector<std::thread> pipe_workers;  // M3.20: parallelised builder
  std::mutex pipe_mutex;
  std::condition_variable pipe_cv;
  std::deque<PipeReq> pipe_queue;
  std::vector<std::pair<uint64_t, VkPipeline>> pipe_done;
  std::unordered_set<uint64_t> pipe_inflight;
  bool pipe_quit = false;  // pipe_mutex
  // M3.20: driver pipeline cache, persisted to disk. On repeat runs the driver
  // returns compiled pipelines near-instantly, so the builder drains in ~1s
  // instead of dropping hundreds of draws for tens of seconds (the black-blob
  // root cause). Guarded by pipe_cache_mutex (workers create concurrently).
  VkPipelineCache pipe_cache = VK_NULL_HANDLE;
  std::mutex pipe_cache_mutex;
  // M4.0: pipeline pre-warm. Every first-seen pipeline request is flattened
  // into a PrewarmRec here (guarded by pipe_mutex, appended at enqueue time);
  // the set persists to disk beside the driver cache and is replayed through
  // the normal miss path at startup, so a repeat run builds a level's whole
  // pipeline population during boot instead of dropping draws (nopipe pop-in)
  // at first encounter. See SavePrewarmManifest / MaybeStartPipelinePrewarm.
  struct PrewarmRec {  // serialized verbatim -- keep POD, fixed 64 bytes
    uint64_t vs_hash = 0, ps_hash = 0;
    uint32_t prim = 0, blend_control = 0, color_mask = 0, depth_control = 0;
    uint32_t su_mode = 0, stencil_ref_mask = 0, stencil_ref_mask_bf = 0, dbg_surf = 0;
    float vport_xscale = 0.0f, vport_zscale = -1.0f;
    uint32_t flags = 0;  // bit0 mirror, bit1 wind_flip, bit2 prim_reset
    uint32_t pad = 0;
  };
  static_assert(sizeof(PrewarmRec) == 64);
  std::vector<PrewarmRec> prewarm_log;          // pipe_mutex; loaded + this run
  std::unordered_set<uint64_t> prewarm_seen;    // pipe_mutex; FNV over rec bytes
  bool prewarm_dirty = false;                   // pipe_mutex
  // Ucode blobs carried over from the loaded manifest, kept so a re-save
  // retains shaders for levels not visited this run. Written once by the
  // loader before the replay thread starts; read-only afterwards.
  struct PrewarmShader {
    uint8_t is_pixel = 0;
    std::vector<uint32_t> be_words;
  };
  std::unordered_map<uint64_t, PrewarmShader> prewarm_loaded_shaders;
  std::atomic<uint64_t> pipe_publish_count{0};  // drained pipe_done total

  // M2.4: offscreen scene target -- guest draws render here; EDRAM-copy
  // ("resolve") records blit it into rt_tex entries keyed by the guest copy
  // destination address; the present pass shows rt_tex[front buffer ptr].
  VkImage scene_img = VK_NULL_HANDLE;
  VkDeviceMemory scene_mem = VK_NULL_HANDLE;
  VkImageView scene_view = VK_NULL_HANDLE;
  // M3.1: depth attachment (D32) for the 3D pipeline. Color persists across
  // frames (EDRAM model); depth clears at each frame's first segment.
  VkImage depth_img = VK_NULL_HANDLE;
  VkDeviceMemory depth_mem = VK_NULL_HANDLE;
  VkImageView depth_view = VK_NULL_HANDLE;
  // Diagnostic (RESTUFF_MID_DEPTH): a copy of depth_img taken right after the
  // WORLD's first main segment, before post passes overwrite it -- to see the
  // near-plane occluder in the world's own depth. Dumped as rt_0000000E_*.
  VkImage dbg_depth_snap = VK_NULL_HANDLE;
  VkDeviceMemory dbg_depth_snap_mem = VK_NULL_HANDLE;
  bool dbg_depth_snap_valid = false;
  // M3.99: aux passes that name the guest's MAIN depth tile (di == main_di)
  // render at HALF the main resolution, so sharing the full-res depth image
  // (M3.98) misaligns every comparison. Instead their PRIVATE depth is
  // pre-filled with a 2x-decimated copy of the main depth: the sun/fog volume
  // mask (Carmack's-reverse z-fail, camera inside the volume) then forms
  // against real scene depth and the shaft/fog composite gets its input back.
  VkImageView depth_sample_view = VK_NULL_HANDLE;  // main depth, DEPTH aspect
  VkDescriptorPool fill_pool = VK_NULL_HANDLE;
  VkDescriptorSet fill_set = VK_NULL_HANDLE;
  bool fill_set_written = false;  // descriptor written lazily (DL samplers)
  VkPipeline fill_pipeline = VK_NULL_HANDLE;
  // M4.4: single-pass depth 2x-downsample resolve (replaces the M3.115
  // triple-move bounce when RESTUFF_DEPTH_FILL=1). Depth-only render pass over
  // the dest rt_tex + a texelFetch(2p+1) pipeline; both non-fatal on failure
  // (null -> the bounce path keeps running, M3.99 convention).
  VkRenderPass depth_fill_rp = VK_NULL_HANDLE;
  VkPipeline depth_fill_pipeline = VK_NULL_HANDLE;
  VkFramebuffer scene_fb = VK_NULL_HANDLE;
  // M3.293 scene-tone pass objects: scratch copy of the scene RT + a
  // fullscreen pipeline that writes tone(scratch) back into the scene RT at
  // the 3D->UI boundary. Created in EnsureSceneTarget.
  VkImage tone_img = VK_NULL_HANDLE;
  VkDeviceMemory tone_mem = VK_NULL_HANDLE;
  VkImageView tone_view = VK_NULL_HANDLE;
  VkDescriptorSetLayout tone_set_layout = VK_NULL_HANDLE;
  VkPipelineLayout tone_pl_layout = VK_NULL_HANDLE;
  VkPipeline tone_pipeline = VK_NULL_HANDLE;
  VkDescriptorPool tone_pool = VK_NULL_HANDLE;
  VkDescriptorSet tone_set = VK_NULL_HANDLE;
  VkRenderPass scene_rp_clear = VK_NULL_HANDLE;     // virgin frame: clear color+depth
  VkRenderPass scene_rp_newframe = VK_NULL_HANDLE;  // frame start: load color, clear depth
  VkRenderPass scene_rp_load = VK_NULL_HANDLE;      // post-resolve segment: load both
  // M3.98: clear COLOUR but LOAD depth+stencil. Aux surfaces that share the
  // guest's depth tile (same RB_DEPTH_INFO as main) must not clear it -- the
  // sun-shaft mask is built by decrementing stencil where the SCENE depth
  // fails, so a cleared depth means no mask and no shafts.
  VkRenderPass scene_rp_clearcolor = VK_NULL_HANDLE;
  // M3.106: clear colour + CLEAR STENCIL + load depth. The shared-aux volume
  // group inherits the previous frame's HUD stencil stamps (minimap circle,
  // panels) through the LOAD, and the shaft's LESS-ref=0 test shadows them --
  // the user's displaced "second shadow". Stencil starts clean; depth (which
  // the z-fail counting needs) still loads.
  VkRenderPass scene_rp_clearcolor_cs = VK_NULL_HANDLE;
  // M3.12/M3.21: AUX EDRAM surfaces -- draws whose RB_COLOR_INFO base differs
  // from the frame's main base render to their OWN surface instead of stomping
  // the main scene. M3.21 generalises this from one aux to N: the DOF/bloom
  // post chain uses several distinct bases and, sharing one surface, they
  // overwrote each other and the world (the black blobs). Same formats/passes.
  struct AuxSurface {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImage depth_img = VK_NULL_HANDLE;
    VkDeviceMemory depth_mem = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    // M3.98: aux COLOUR paired with the MAIN depth/stencil view, for draws
    // whose RB_DEPTH_INFO matches the main surface's (hardware shares one
    // EDRAM depth tile across colour surfaces; our per-surface split is the
    // "lie" the zwrites comment above already flags).
    VkFramebuffer fb_shared_depth = VK_NULL_HANDLE;
  };
  // Measured: this title uses only TWO bases in gameplay (30E main, 0D8 aux),
  // so 2 slots is ample headroom; each costs a full 1280x720 colour+depth pair.
  // M3.91: 2 -> 8. The gameplay frame uses ~6 distinct aux color bases (000,
  // 0D8, 320/400/360/160...); with only 2 slots the overflow all CLAMPED into
  // slot 2, so the auto-exposure meter's downsample pyramid shared a surface
  // with the bright sun-wash/glow passes and OVERREAD (~1.55x relative to the
  // reference meter/scene ratio) -- the game's exposure then darkened the
  // whole world to the user's reported ~0.5x. Each base now gets its own
  // surface (~7.4MB color+depth each).
  static constexpr uint32_t kAuxSurfaces = 16;
  AuxSurface aux[kAuxSurfaces];
  bool scene_ready = false;
  // M3.13: the VdSwap front-buffer pointer that travels with tl.frame -- the
  // display source matching the draws we execute (the live pointer can already
  // belong to the NEXT guest frame; pairing with it flickered).
  uint32_t frame_fb = 0;
  // M3.16: chunk merger -- the walker's XE_SWAP boundary can fire before the
  // frame's final kick, splitting one guest frame into chunks; presenting a
  // chunk missing its tail flashes black splotches for one vsync.
  std::vector<renderer::RawGuestDraw> pending_chunks;
  int pending_holds = 0;
  // Keyed by (resolve dest | depth bit), NOT by address alone. The guest
  // resolves BOTH colour and depth to the same address (0x0582C000 carries
  // colour, and the sun-shaft PS samples depth there): a single slot made the
  // two kinds evict each other, so whichever resolved last served the fetch
  // and the shaft sampled colour as depth -> garbage occlusion -> world dim.
  // Dest addresses are page-aligned, so bit 0 is free for the kind.
  std::unordered_map<uint64_t, TexEntry> rt_tex;  // (resolve dest|depth) -> image
  // Scratch buffer for image->image copies (the SDK's device function table
  // has no vkCmdBlitImage/vkCmdCopyImage; bounce through a buffer).
  VkBuffer resolve_buf = VK_NULL_HANDLE;
  VkDeviceMemory resolve_buf_mem = VK_NULL_HANDLE;
  // M3.115: transfer-only scratch for 2x depth downsampling (blitting the
  // LIVE main depth attachment blacked the frame; bounce via resolve_buf
  // into this plain image, then NEAREST-blit scratch -> rt depth entry).
  VkImage ds2x_img = VK_NULL_HANDLE;
  VkDeviceMemory ds2x_mem = VK_NULL_HANDLE;
  bool ds2x_init = false;
  // M3.89: resolve -> guest-RAM writeback. Hardware resolves WRITE guest
  // memory and the guest reads it back (auto-exposure luminance meter and
  // friends); our GPU-side rt_tex shortcut starved that feedback. Small
  // color resolves are captured into this host-visible buffer in-frame
  // (buffer-to-buffer from resolve_buf) and scattered into guest RAM in
  // the dest's tiled 8888 layout after the frame fence.
  VkBuffer wb_buf = VK_NULL_HANDLE;
  VkDeviceMemory wb_mem = VK_NULL_HANDLE;
  uint32_t wb_mem_type = 0;
  void* wb_ptr = nullptr;
  struct PendingWb {
    uint32_t dest, w, h;
    VkDeviceSize off;
  };
  std::vector<PendingWb> wb_pending;
  VkDeviceSize wb_off = 0;
};
// M4.5: pipelined-present decision, made once at first use. DEFAULT ON as of
// M4.34 (user call, Aug 27, after play hours with no artifacts): the serialized
// frame makes CPU prep and GPU work ADD instead of overlap, which on a
// GPU-bound handheld costs a whole vblank step -- the Ally's 10W gameplay
// measured CPU ~10ms + GPU ~17ms = ~28ms serial, quantising to 33ms/20-30fps.
// RESTUFF_NO_PIPELINED=1 restores the serialized frame (kill switch / A-B).
// Diagnostics that read GPU results inside the frame or assume frame-complete
// presents still force serialized mode, as does the legacy non-translated path
// (untested under pipelining) and RESTUFF_NO_VBCACHE (the whole vertex ring
// becomes per-frame scratch -- don't double 60MB).
bool PipelinedMode() {
  static const bool s = [] {
    if (getenv("RESTUFF_NO_PIPELINED") != nullptr) {
      REXLOG_INFO("[native_vk] M4.5 pipelined present OFF (RESTUFF_NO_PIPELINED)");
      return false;
    }
    static const char* const kBlockers[] = {
        "RESTUFF_RESOLVE_WB",     "RESTUFF_DUMP_SCENE",  "RESTUFF_DUMP_EACH_AUX",
        "RESTUFF_DUMP_AFTER_AUX", "RESTUFF_MID_DEPTH",   "RESTUFF_RDOC_TRIGGER",
        "RESTUFF_NO_VBCACHE",     "RESTUFF_DUMPGO",      "RESTUFF_GPUPASS_MS",
    };
    for (const char* b : kBlockers) {
      if (getenv(b)) {
        REXLOG_INFO("[native_vk] M4.5 pipelined present OFF: {} needs the serialized frame", b);
        return false;
      }
    }
    if (!REXCVAR_GET(use_translated_shaders)) {
      REXLOG_INFO("[native_vk] M4.5 pipelined present OFF: legacy 2D path");
      return false;
    }
    REXLOG_INFO("[native_vk] M4.5 pipelined present ON (2 frames in flight)");
    return true;
  }();
  return s;
}

// M4.5: free everything whose last GPU user was this slot's just-completed
// frame. Runs immediately after the slot's fence wait (pipelined mode top-of-
// callback), which preserves the M3.0/M4.0 invariants verbatim: descriptor
// sets recycled by DestroyTexEntry are never in-flight, staging buffers are
// GPU-done, and retired vb ring buffers have no remaining readers.
void RetireFrameSlot(vk::VulkanDevice* dev, TranslatedLayer::FrameSlot& slot) {
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  for (const auto& up : slot.r_retired_uploads) {
    if (up.staging) df.vkDestroyBuffer(device, up.staging, nullptr);
    if (up.staging_mem) df.vkFreeMemory(device, up.staging_mem, nullptr);
  }
  slot.r_retired_uploads.clear();
  for (auto& t : slot.r_deferred_destroy) DestroyTexEntry(dev, t);
  slot.r_deferred_destroy.clear();
  for (auto& [b, m] : slot.r_retired_buffers) {
    if (b) df.vkDestroyBuffer(device, b, nullptr);
    if (m) df.vkFreeMemory(device, m, nullptr);
  }
  slot.r_retired_buffers.clear();
}

TranslatedLayer& TL() {
  static TranslatedLayer tl;
  return tl;
}

uint64_t RtTexKey(uint32_t phys, bool is_depth) {
  return (uint64_t(phys) & ~1ull) | (is_depth ? 1ull : 0ull);
}
VkDescriptorSet LookupRtTex(uint32_t phys) {
  auto& tl = TL();
  auto it = tl.rt_tex.find(RtTexKey(phys, false));  // present path wants colour
  return it != tl.rt_tex.end() ? it->second.set : VK_NULL_HANDLE;
}
const TexEntry* LookupRtTexEntry(uint32_t phys, bool want_depth) {
  auto& tl = TL();
  auto it = tl.rt_tex.find(RtTexKey(phys, want_depth));
  if (it != tl.rt_tex.end()) return &it->second;
  // Fall back to the other kind rather than dropping to a guest-RAM decode:
  // before this split there was only ever one entry per address.
  it = tl.rt_tex.find(RtTexKey(phys, !want_depth));
  return it != tl.rt_tex.end() ? &it->second : nullptr;
}

// Xenos VertexFormat -> VkFormat. The GPU unpacks; the shader reads vec4.
VkFormat XenosVtxVkFormat(uint32_t f, bool sgn, bool nrm) {
  switch (f) {
    case 6:  // k_8_8_8_8
      return nrm ? (sgn ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM)
                 : (sgn ? VK_FORMAT_R8G8B8A8_SSCALED : VK_FORMAT_R8G8B8A8_USCALED);
    case 7:  // k_2_10_10_10
      return nrm ? (sgn ? VK_FORMAT_A2B10G10R10_SNORM_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32)
                 : (sgn ? VK_FORMAT_A2B10G10R10_SSCALED_PACK32
                        : VK_FORMAT_A2B10G10R10_USCALED_PACK32);
    case 25:  // k_16_16
      return nrm ? (sgn ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM)
                 : (sgn ? VK_FORMAT_R16G16_SSCALED : VK_FORMAT_R16G16_USCALED);
    case 26:  // k_16_16_16_16
      return nrm ? (sgn ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM)
                 : (sgn ? VK_FORMAT_R16G16B16A16_SSCALED : VK_FORMAT_R16G16B16A16_USCALED);
    case 31: return VK_FORMAT_R16G16_SFLOAT;         // k_16_16_FLOAT
    case 32: return VK_FORMAT_R16G16B16A16_SFLOAT;   // k_16_16_16_16_FLOAT
    // M3.30: 33/34/35 (32-bit integer fetches) are int->float CONVERTED in the
    // capture's LE-normalize pass (translated VSes declare float inputs; the
    // old SINT/UINT binding was an attribute type mismatch = undefined values).
    case 33: return VK_FORMAT_R32_SFLOAT;
    case 34: return VK_FORMAT_R32G32_SFLOAT;
    case 35: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 36: return VK_FORMAT_R32_SFLOAT;
    case 37: return VK_FORMAT_R32G32_SFLOAT;
    case 57: return VK_FORMAT_R32G32B32_SFLOAT;
    case 38: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_R32G32B32A32_SFLOAT;
  }
}

VkPrimitiveTopology XenosTopology(uint32_t prim) {
  switch (prim) {
    case 1: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // 4, and 8/0xD as a first cut
  }
}

void PipelineWorker(vk::VulkanDevice* dev);  // M3.15: defined after the builder

// M4.0: directory of the running executable, with trailing separator (empty
// if undiscoverable -- callers then fall back to CWD-relative, the old
// behaviour). Cache files must not depend on the launch CWD: a shortcut or
// launcher that starts the exe from another directory silently re-warmed the
// pipeline cache from nothing on every run.
const std::string& ExeDir() {
  static const std::string dir = [] {
    std::string d;
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) d.assign(buf, n);
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) d.assign(buf, size_t(n));
#endif
    const size_t cut = d.find_last_of("/\\");
    if (cut == std::string::npos) return std::string();
    return d.substr(0, cut + 1);
  }();
  return dir;
}

// M4.42: set at cache-seed time -- true when pipeline_cache.bin was missing or
// belongs to another GPU/driver, i.e. every pipeline must be rebuilt this run.
// Read by MaybeStartPipelinePrewarm to decide whether to warm before play.
bool g_pipe_cache_stale = false;

// M4.43: prewarm manifest ceilings. The manifest accumulates across sessions
// (unvisited levels' blobs are carried forward), so a full playthrough is the
// intended way to build a complete one -- and one Episode-1 route already
// costs 368 records / 243 shaders. The old 8192-record ceiling stopped adding
// SILENTLY, which would quietly cap a whole-game capture. PrewarmRec is 64 B,
// so 32768 records is 2 MB: irrelevant next to what it saves.
// ⚠️ The save-side cap and the loader's validation MUST stay equal. If a file
// is ever written past what the loader accepts, the next boot rejects the
// WHOLE manifest rather than the overflow -- losing everything, not the excess.
constexpr size_t kPrewarmMaxRecs = 32768;
constexpr size_t kPrewarmMaxShaders = 65536;

// M3.20: on-disk location for the persisted VkPipelineCache. M4.0: anchored to
// the exe's directory (was CWD-relative). RESTUFF_PIPE_CACHE overrides.
// M4.45: identity of everything that shapes a pipeline in this build -- the
// shader emitters (ShaderBuildId) plus this TU's own compile stamp (blend /
// depth / raster state mapping lives here). Kept beside the driver blob in a
// sidecar (<cache>.id) so the Vulkan header stays the driver's own.
uint64_t PipelineCacheBuildId() {
  static const uint64_t id = [] {
    const char* s = __DATE__ " " __TIME__;
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char* p = s; *p; ++p) h = (h ^ uint8_t(*p)) * 0x100000001b3ull;
    return renderer::spc::ShaderBuildId() ^ (h * 0x9E3779B97F4A7C15ull);
  }();
  return id;
}

std::string PipelineCachePath() {
  if (const char* e = getenv("RESTUFF_PIPE_CACHE")) return e;
  return ExeDir() + "pipeline_cache.bin";
}

// M4.0: on-disk location of the pipeline pre-warm manifest (PrewarmRec set +
// the guest shader ucode they reference). RESTUFF_PREWARM_FILE overrides.
std::string PrewarmManifestPath() {
  if (const char* e = getenv("RESTUFF_PREWARM_FILE")) return e;
  return ExeDir() + "pipeline_prewarm.bin";
}

// M3.20: the SDK's device function table doesn't expose the pipeline-cache
// entry points, so resolve them once via the instance's vkGetDeviceProcAddr.
PFN_vkCreatePipelineCache g_vkCreatePipelineCache = nullptr;
PFN_vkGetPipelineCacheData g_vkGetPipelineCacheData = nullptr;
PFN_vkDestroyPipelineCache g_vkDestroyPipelineCache = nullptr;
void LoadPipelineCacheFns(vk::VulkanDevice* dev) {
  auto gpa = dev->vulkan_instance()->functions().vkGetDeviceProcAddr;
  if (!gpa) return;
  VkDevice d = dev->device();
  g_vkCreatePipelineCache = (PFN_vkCreatePipelineCache)gpa(d, "vkCreatePipelineCache");
  g_vkGetPipelineCacheData = (PFN_vkGetPipelineCacheData)gpa(d, "vkGetPipelineCacheData");
  g_vkDestroyPipelineCache = (PFN_vkDestroyPipelineCache)gpa(d, "vkDestroyPipelineCache");
}
TranslatedLayer& TL();
// M3.20: serialise the driver pipeline cache to disk. Called periodically (the
// drive/watchdog SIGKILLs, so a shutdown-only save would never land) and on
// clean shutdown. Writes atomically via a temp file + rename.
void SavePipelineCache(vk::VulkanDevice* dev, bool sync = false) {
  auto& tl = TL();
  if (tl.pipe_cache == VK_NULL_HANDLE || !g_vkGetPipelineCacheData) return;
  VkDevice device = dev->device();
  size_t sz = 0;
  // vkGetPipelineCacheData is spec-safe concurrently with worker creates on the
  // same cache (no external-sync requirement), so no device wait needed.
  if (g_vkGetPipelineCacheData(device, tl.pipe_cache, &sz, nullptr) != VK_SUCCESS || !sz) return;
  std::vector<uint8_t> data(sz);
  if (g_vkGetPipelineCacheData(device, tl.pipe_cache, &sz, data.data()) != VK_SUCCESS) return;
  // M3.145: the WRITE MUST NOT HAPPEN ON THE PRESENT THREAD. This ran inline
  // inside the present callback, so every save stalled presentation for as long
  // as the filesystem took to put ~47MB down and rename it -- and the project
  // lives on an external drive. A caught blue-screen boot has the present thread
  // parked exactly here:
  //   PresentThreadMain -> PresentClearFrame -> RefreshGuestOutputImpl
  //     -> our callback -> SavePipelineCache -> rename()
  // With presentation stopped the swapchain keeps showing the bare clear, which
  // is precisely the user's "blue screen" (cornflower 99,148,237) and "black
  // screen" launches -- whichever colour the last clear left. It fires at frame
  // 1500 (~25s at 60fps), which is why the failures are all "very early, before
  // the title ever appears".
  // The Vulkan call above stays on this thread (cheap, and keeps cache access
  // where it was); only the slow I/O is handed off, detached, with the data
  // moved into the worker so nothing is shared.
  // Single-flight: saves are 3000 frames apart so overlap is unlikely, but a
  // slow filesystem is exactly the condition this fixes -- don't let a second
  // save pile onto a first that is still writing.
  static std::atomic<bool> s_saving{false};
  auto writer = [data = std::move(data), sz, sync]() mutable {
    const std::string path = PipelineCachePath();
    const std::string tmp = path + ".tmp";
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
      bool ok = std::fwrite(data.data(), 1, sz, f) == sz;
      std::fclose(f);
      if (ok) {
        // M4.1: std::filesystem::rename, NOT C rename() -- the CRT rename
        // FAILS on Windows when the destination exists (POSIX overwrites), and
        // the unchecked call meant every save after the first silently landed
        // nowhere: the cache never grew past its first snapshot and a stale
        // .tmp was left beside it. Linux never showed it.
        std::error_code rn_ec;
        std::filesystem::rename(tmp, path, rn_ec);
        ok = !rn_ec;
        if (ok) {
          static std::atomic<int> s_lg{4};
          if (s_lg.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[native_vk] pipeline cache saved {} bytes ({})", sz,
                        sync ? "sync" : "async");
          // M4.45: stamp the blob with this build's identity (see the seed).
          if (FILE* sf = std::fopen((path + ".id").c_str(), "wb")) {
            const uint64_t id = PipelineCacheBuildId();
            std::fwrite(&id, 8, 1, sf);
            std::fclose(sf);
          }
        } else {
          REXLOG_WARN("[native_vk] pipeline cache rename FAILED: {}", rn_ec.message());
        }
      }
      if (!ok) std::remove(tmp.c_str());
    }
    s_saving.store(false, std::memory_order_release);
  };
  // M4.0: shutdown hard-exits the process ~200ms after this returns, which
  // can strand a detached writer mid-file (the save is simply lost). Callers
  // off the present thread pass sync=true and eat the I/O inline -- WAITING
  // OUT any in-flight async writer first: skipping here would drop the run's
  // data, the precise failure the sync flag exists to close (the burst-settled
  // trigger fires ~5s after a load, i.e. exactly in the "load a level and
  // quit" window).
  if (sync) {
    while (s_saving.exchange(true, std::memory_order_acq_rel))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    writer();
  } else if (!s_saving.exchange(true, std::memory_order_acq_rel)) {
    std::thread(std::move(writer)).detach();
  }
}

bool SetupTranslatedLayer(vk::VulkanDevice* dev) {
  auto& tl = TL();
  if (tl.init) return tl.ready;
  tl.init = true;
  const auto& df = dev->functions();
  VkDevice device = dev->device();

  VkDescriptorSetLayoutBinding b[4] = {};
  b[0].binding = 0;
  b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  b[0].descriptorCount = 1;
  b[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  b[1].binding = 1;
  b[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
  b[1].descriptorCount = 1;
  b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  // M3.11/M3.14: raw dwords of the register-relative fetch streams (bone data,
  // mesh neighbor reads); dynamic offsets select the draw's payloads in tl.vb.
  for (int k = 2; k <= 3; ++k) {
    b[k].binding = uint32_t(k);
    b[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
    b[k].descriptorCount = 1;
    b[k].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  }
  VkDescriptorSetLayoutCreateInfo dsl = {};
  dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dsl.bindingCount = 4;
  dsl.pBindings = b;
  if (df.vkCreateDescriptorSetLayout(device, &dsl, nullptr, &tl.ubo_layout) != VK_SUCCESS)
    return false;

  // M4.5: one dynamic-offset set per frame slot (serialized mode uses slot 0).
  VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 4},
                                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 4}};
  VkDescriptorPoolCreateInfo dp = {};
  dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dp.maxSets = 2;
  dp.poolSizeCount = 2;
  dp.pPoolSizes = ps;
  if (df.vkCreateDescriptorPool(device, &dp, nullptr, &tl.ubo_pool) != VK_SUCCESS) return false;
  const VkDescriptorSetLayout ubo_layouts[2] = {tl.ubo_layout, tl.ubo_layout};
  VkDescriptorSetAllocateInfo ai = {};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = tl.ubo_pool;
  ai.descriptorSetCount = 2;
  ai.pSetLayouts = ubo_layouts;
  VkDescriptorSet ubo_sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  if (df.vkAllocateDescriptorSets(device, &ai, ubo_sets) != VK_SUCCESS) return false;
  tl.slots[0].ubo_set = ubo_sets[0];
  tl.slots[1].ubo_set = ubo_sets[1];

  // M3.0: set 1 = kMaxTexSlots combined-image-sampler bindings (the translated
  // GLSL binds sampler2D tex_<slot> at binding=<fetch slot>). Unused slots get
  // the white 1x1 texture.
  {
    VkDescriptorSetLayoutBinding tb[renderer::kMaxTexSlots] = {};
    for (uint32_t i = 0; i < renderer::kMaxTexSlots; ++i) {
      tb[i].binding = i;
      tb[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      tb[i].descriptorCount = 1;
      tb[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
    }
    VkDescriptorSetLayoutCreateInfo tdsl = {};
    tdsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tdsl.bindingCount = renderer::kMaxTexSlots;
    tdsl.pBindings = tb;
    if (df.vkCreateDescriptorSetLayout(device, &tdsl, nullptr, &tl.tex_layout) != VK_SUCCESS)
      return false;
    VkDescriptorPoolSize tps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                2048 * renderer::kMaxTexSlots};
    VkDescriptorPoolCreateInfo tdp = {};
    tdp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    tdp.maxSets = 2048;
    tdp.poolSizeCount = 1;
    tdp.pPoolSizes = &tps;
    for (auto& slot : tl.slots) {  // M4.5: one combo pool per frame slot
      VkDescriptorPool pool = VK_NULL_HANDLE;
      if (df.vkCreateDescriptorPool(device, &tdp, nullptr, &pool) != VK_SUCCESS) return false;
      slot.tex_pools.push_back(pool);
    }
  }

  const VkDescriptorSetLayout sets[2] = {tl.ubo_layout, tl.tex_layout};
  // 0..16 = VS ndc transform; 16..32 = PS alpha-test {ref, func, 0, 0};
  // 32..64 = PS bool constants (two uvec4) for kCondJmp feature guards;
  // 64..80 = PS param_gen {enable, gpr, pix_center_off, 0};
  // 96..112 = VS rotated-surface counter-rotation {enable, dir, 0, 0}.
  // M3.30 (THE TILT ROOT CAUSE): one range per stage, spanning that stage's
  // whole block. The previous THREE ranges put the VERTEX stage in two entries
  // -- illegal (VUID-00292) -- and left the VS block's rot member (offset 96)
  // OUTSIDE the vertex range [0,16] (VUID-10069): every `pc.rot` read in every
  // VS was undefined. On NVIDIA the garbage read intermittently landed >0.5,
  // firing the rot branch's `(_gp.y, -_gp.x)` axis swap -- the presented
  // 90-degree world transpose + mirrored controls, stable per moment class
  // because the driver's push-buffer garbage tracks the frame's push history.
  const VkPushConstantRange pc[1] = {
      {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128}};  // M3.288: +misc @112
  VkPipelineLayoutCreateInfo pl = {};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount = 2;
  pl.pSetLayouts = sets;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = pc;
  if (df.vkCreatePipelineLayout(device, &pl, nullptr, &tl.pipeline_layout) != VK_SUCCESS)
    return false;

  // M3.20: driver pipeline cache seeded from disk (see pipe_cache field).
  LoadPipelineCacheFns(dev);
  if (g_vkCreatePipelineCache) {
    std::vector<uint8_t> seed;
    if (FILE* f = std::fopen(PipelineCachePath().c_str(), "rb")) {
      std::fseek(f, 0, SEEK_END);
      long n = std::ftell(f);
      std::fseek(f, 0, SEEK_SET);
      if (n > 0) {
        seed.resize(size_t(n));
        if (std::fread(seed.data(), 1, seed.size(), f) != seed.size()) seed.clear();
      }
      std::fclose(f);
    }
    // M4.42: does this cache actually belong to THIS GPU + driver? Vulkan
    // silently ignores a foreign blob, so before this the only symptom of a
    // GPU swap or a driver update was every pipeline quietly recompiling
    // during play -- exactly the stutter the prewarm exists to remove, with
    // nothing in the log to say why. The 32-byte VkPipelineCacheHeaderVersionOne
    // carries vendorID/deviceID/pipelineCacheUUID, and the UUID is required to
    // change whenever the driver's compiled output could differ, so matching it
    // IS the driver-version check.
    {
      VkPhysicalDeviceProperties props = {};
      dev->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(dev->physical_device(),
                                                                        &props);
      const char* why = nullptr;
      if (seed.size() < 32) {
        why = seed.empty() ? "no cache file yet" : "cache file too small";
      } else {
        uint32_t hs = 0, hv = 0, vend = 0, devid = 0;
        std::memcpy(&hs, seed.data() + 0, 4);
        std::memcpy(&hv, seed.data() + 4, 4);
        std::memcpy(&vend, seed.data() + 8, 4);
        std::memcpy(&devid, seed.data() + 12, 4);
        if (hs < 32 || hv != VK_PIPELINE_CACHE_HEADER_VERSION_ONE) why = "unrecognised header";
        else if (vend != props.vendorID || devid != props.deviceID) why = "different GPU";
        else if (std::memcmp(seed.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0)
          why = "different driver version";
      }
      // M4.45: same GPU and driver, but was it OUR current build that wrote
      // it? The driver keeps every pipeline it was ever handed, so across
      // releases the blob would accumulate entries for shaders this build no
      // longer emits. Discard on a build change; the warm rebuilds it.
      if (!why) {
        uint64_t stored = 0;
        bool have = false;
        if (FILE* sf = std::fopen((PipelineCachePath() + ".id").c_str(), "rb")) {
          have = std::fread(&stored, 8, 1, sf) == 1;
          std::fclose(sf);
        }
        if (!have) why = "no build stamp (pre-M4.45 cache)";
        else if (stored != PipelineCacheBuildId()) why = "different build";
      }
      g_pipe_cache_stale = (why != nullptr);
      if (why) {
        seed.clear();  // don't hand the driver a blob it will discard anyway
        REXLOG_INFO("[native_vk] M4.42 pipeline cache unusable ({}): will warm before play",
                    why);
      }
    }
    VkPipelineCacheCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    ci.initialDataSize = seed.size();
    ci.pInitialData = seed.empty() ? nullptr : seed.data();
    g_vkCreatePipelineCache(device, &ci, nullptr, &tl.pipe_cache);
    REXLOG_INFO("[native_vk] pipeline cache seeded from {} bytes", seed.size());
  }

  // M3.15/M3.20: pipeline builds happen off the present thread, on several
  // worker threads so a level's ~600 distinct pipelines populate in a few
  // seconds rather than dropping draws (black blobs) for tens of seconds.
  {
    unsigned hw = std::thread::hardware_concurrency();
    unsigned n = hw > 4 ? std::min(hw - 2u, 8u) : 2u;
    if (const char* e = getenv("RESTUFF_PIPE_WORKERS")) n = std::max(1u, unsigned(atoi(e)));
    for (unsigned i = 0; i < n; ++i)
      tl.pipe_workers.emplace_back([dev] { PipelineWorker(dev); });
    REXLOG_INFO("[native_vk] {} pipeline worker(s)", n);
  }

  tl.ready = true;
  REXLOG_INFO("[native_vk] translated-shader layer ready");
  return true;
}

// M3.0: get (or build) the descriptor set binding this draw's texture slots.
// resolved[i] = the TexEntry for slot i (nullptr = white), wrap[i] = sampler
// addressing. Cached by the view-handle combination; the cache and pool are
// reset by PrepareTranslatedDraws when any guest texture was re-decoded.
VkDescriptorSet GetTextureComboSet(vk::VulkanDevice* dev,
                                   const TexEntry* resolved[renderer::kMaxTexSlots],
                                   const bool wrap[renderer::kMaxTexSlots]) {
  auto& tl = TL();
  auto& dl = DL();
  auto& fs = tl.cur();  // M4.5: per-slot combo cache/pools
  uint64_t key = 1469598103934665603ull;
  for (uint32_t i = 0; i < renderer::kMaxTexSlots; ++i) {
    const uint64_t v = resolved[i] ? uint64_t(resolved[i]->view) : 0;
    key ^= v + (wrap[i] ? 0x9E37ull : 0);
    key *= 1099511628211ull;
  }
  if (auto it = fs.tex_combos.find(key); it != fs.tex_combos.end()) return it->second;

  const auto& df = dev->functions();
  VkDescriptorSetAllocateInfo ai = {};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = fs.tex_pools.back();
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &tl.tex_layout;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (df.vkAllocateDescriptorSets(dev->device(), &ai, &set) != VK_SUCCESS) {
    // Pool exhausted. Do NOT reset mid-frame (sets already recorded into this
    // frame's command buffer must stay valid) -- grow a fresh pool instead;
    // everything resets together at the next views-retired frame boundary.
    VkDescriptorPoolSize tps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                2048 * renderer::kMaxTexSlots};
    VkDescriptorPoolCreateInfo tdp = {};
    tdp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    tdp.maxSets = 2048;
    tdp.poolSizeCount = 1;
    tdp.pPoolSizes = &tps;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (df.vkCreateDescriptorPool(dev->device(), &tdp, nullptr, &pool) != VK_SUCCESS)
      return VK_NULL_HANDLE;
    fs.tex_pools.push_back(pool);
    ai.descriptorPool = pool;
    if (df.vkAllocateDescriptorSets(dev->device(), &ai, &set) != VK_SUCCESS)
      return VK_NULL_HANDLE;
  }
  const TexEntry* white = ResolveTextureEntry(dev, renderer::GuestTextureDesc{});
  if (!white) return VK_NULL_HANDLE;
  VkDescriptorImageInfo ii[renderer::kMaxTexSlots];
  VkWriteDescriptorSet ws[renderer::kMaxTexSlots];
  for (uint32_t i = 0; i < renderer::kMaxTexSlots; ++i) {
    const TexEntry* e = resolved[i] ? resolved[i] : white;
    // Depth resolve targets (D32) must be point-sampled -- linear filtering on a
    // depth format is not guaranteed and would blend depth values.
    const VkSampler smp = e->is_depth ? dl.sampler_nearest : (wrap[i] ? dl.sampler_repeat
                                                                       : dl.sampler);
    ii[i] = {smp, e->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    ws[i] = {};
    ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    ws[i].dstSet = set;
    ws[i].dstBinding = i;
    ws[i].descriptorCount = 1;
    ws[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ws[i].pImageInfo = &ii[i];
  }
  df.vkUpdateDescriptorSets(dev->device(), renderer::kMaxTexSlots, ws, 0, nullptr);
  fs.tex_combos[key] = set;
  return set;
}

// Xenos BlendFactor -> VkBlendFactor (values from rex/graphics/xenos.h).
VkBlendFactor XenosBlendFactor(uint32_t f) {
  switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}
VkBlendOp XenosBlendOp(uint32_t op) {
  switch (op) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

// M3.59: sign of det() of the WVP matrix's linear part, read from vertex
// constants c0..c2 (.xyz). The game uploads its world-view-projection into
// c[0..3]; a MIRRORED instance (negative-determinant world matrix) flips this
// sign, and that is how a winding-inverted draw is detected. The ucode's own
// swizzles change HOW the shader multiplies but not the constants, so det(c)
// tracks det(WVP) up to a per-shader constant factor -- which the per-shader
// majority vote cancels. Returns +1 / -1, or 0 when absent/degenerate.
static int WvpDetSign(const renderer::ConstBank& c) {  // M4.2: shared snapshot type
  if (c.size() < 12) return 0;
  const float det = c[0] * (c[5] * c[10] - c[6] * c[9]) -
                    c[1] * (c[4] * c[10] - c[6] * c[8]) +
                    c[2] * (c[4] * c[9] - c[5] * c[8]);
  const float eps = 1e-12f;
  return det > eps ? 1 : det < -eps ? -1 : 0;
}

// ===========================================================================
// M3.60: runtime winding probe (RESTUFF_WINDPROBE=1).
// The missing-floor class of defect is geometry whose screen-space winding
// comes out INVERTED under our translation, so cull-back drops its front
// faces (RenderDoc pixel history: "Backface culled"). No static signal
// distinguishes those draws (state identical to correct ones; det(c) is
// meaningless because each shader shuffles its matrix into the constants), so
// we measure the truth: run the draw's OWN translated transform (a compute
// variant of its VS, emitted by the translator) on a sample of its real
// triangles, read back the clip positions, and majority-vote the winding.
// A draw whose sampled triangles are overwhelmingly back-facing under the
// pipeline's front-face setting is inverted -> flip its front face. Sheets
// (terrain/walls) vote decisively; closed meshes sit near 50% and are left
// alone (they already render correctly). Cached per draw identity; a few
// probes per frame, so a new scene classifies over a handful of frames.
namespace {

struct WindProbeCtx {
  bool tried_init = false, ok = false;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkDescriptorPool dpool = VK_NULL_HANDLE;
  VkDescriptorSet dset = VK_NULL_HANDLE;
  VkBuffer ubo = VK_NULL_HANDLE, attrs = VK_NULL_HANDLE, outb = VK_NULL_HANDLE;
  VkDeviceMemory ubo_mem = VK_NULL_HANDLE, attrs_mem = VK_NULL_HANDLE, outb_mem = VK_NULL_HANDLE;
  uint32_t ubo_type = 0, attrs_type = 0, outb_type = 0;
  void* ubo_map = nullptr;
  void* attrs_map = nullptr;
  void* outb_map = nullptr;
  VkCommandPool cpool = VK_NULL_HANDLE;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  std::unordered_map<uint64_t, VkPipeline> pipes;  // vs_hash -> compute pipeline
  // draw key -> 1 flip / 0 keep / -1..-2 retries used (unclassifiable so far)
  std::unordered_map<uint64_t, int8_t> cls;
};
WindProbeCtx g_wp;

constexpr uint32_t kWpMaxTris = 16;  // triangles sampled per draw
constexpr uint32_t kWpMaxLoc = 8;    // in_0..in_7 supported
constexpr VkDeviceSize kWpUboBytes = 256 * 16 + 8 * 16 + 2 * 16;  // c[256]+lc+bc
// M3.61: 2x regions -- base vertices [0..3nt) plus normal-offset vertices
// [3nt..6nt) (same attrs, in_0 nudged along the authored normal) for the
// per-triangle normal-vs-winding test.
constexpr VkDeviceSize kWpAttrBytes = 2 * kWpMaxTris * 3 * kWpMaxLoc * 16;
constexpr VkDeviceSize kWpOutBytes = 2 * kWpMaxTris * 3 * 16;

bool WindProbeInit(vk::VulkanDevice* dev) {
  if (g_wp.tried_init) return g_wp.ok;
  g_wp.tried_init = true;
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  VkDescriptorSetLayoutBinding binds[3] = {};
  binds[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  binds[1] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  binds[2] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo dsl_ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  dsl_ci.bindingCount = 3;
  dsl_ci.pBindings = binds;
  if (df.vkCreateDescriptorSetLayout(device, &dsl_ci, nullptr, &g_wp.dsl) != VK_SUCCESS)
    return false;
  VkPushConstantRange pcr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
  VkPipelineLayoutCreateInfo pl_ci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl_ci.setLayoutCount = 1;
  pl_ci.pSetLayouts = &g_wp.dsl;
  pl_ci.pushConstantRangeCount = 1;
  pl_ci.pPushConstantRanges = &pcr;
  if (df.vkCreatePipelineLayout(device, &pl_ci, nullptr, &g_wp.layout) != VK_SUCCESS) return false;
  VkDescriptorPoolSize psz[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                 {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
  VkDescriptorPoolCreateInfo dp_ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dp_ci.maxSets = 1;
  dp_ci.poolSizeCount = 2;
  dp_ci.pPoolSizes = psz;
  if (df.vkCreateDescriptorPool(device, &dp_ci, nullptr, &g_wp.dpool) != VK_SUCCESS) return false;
  VkDescriptorSetAllocateInfo ds_ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ds_ai.descriptorPool = g_wp.dpool;
  ds_ai.descriptorSetCount = 1;
  ds_ai.pSetLayouts = &g_wp.dsl;
  if (df.vkAllocateDescriptorSets(device, &ds_ai, &g_wp.dset) != VK_SUCCESS) return false;
  if (!vk::util::CreateDedicatedAllocationBuffer(dev, kWpUboBytes,
                                                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                                 vk::util::MemoryPurpose::kUpload, g_wp.ubo,
                                                 g_wp.ubo_mem, &g_wp.ubo_type))
    return false;
  if (!vk::util::CreateDedicatedAllocationBuffer(dev, kWpAttrBytes,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                 vk::util::MemoryPurpose::kUpload, g_wp.attrs,
                                                 g_wp.attrs_mem, &g_wp.attrs_type))
    return false;
  if (!vk::util::CreateDedicatedAllocationBuffer(dev, kWpOutBytes,
                                                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                                 vk::util::MemoryPurpose::kReadback, g_wp.outb,
                                                 g_wp.outb_mem, &g_wp.outb_type))
    return false;
  if (df.vkMapMemory(device, g_wp.ubo_mem, 0, VK_WHOLE_SIZE, 0, &g_wp.ubo_map) != VK_SUCCESS ||
      df.vkMapMemory(device, g_wp.attrs_mem, 0, VK_WHOLE_SIZE, 0, &g_wp.attrs_map) != VK_SUCCESS ||
      df.vkMapMemory(device, g_wp.outb_mem, 0, VK_WHOLE_SIZE, 0, &g_wp.outb_map) != VK_SUCCESS)
    return false;
  VkDescriptorBufferInfo bi[3] = {{g_wp.ubo, 0, kWpUboBytes},
                                  {g_wp.attrs, 0, kWpAttrBytes},
                                  {g_wp.outb, 0, kWpOutBytes}};
  VkWriteDescriptorSet wr[3] = {};
  for (int i = 0; i < 3; ++i) {
    wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[i].dstSet = g_wp.dset;
    wr[i].dstBinding = i == 0 ? 0 : (i == 1 ? 4 : 5);
    wr[i].descriptorCount = 1;
    wr[i].descriptorType =
        i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr[i].pBufferInfo = &bi[i];
  }
  df.vkUpdateDescriptorSets(device, 3, wr, 0, nullptr);
  VkCommandPoolCreateInfo cp_ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  cp_ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cp_ci.queueFamilyIndex = dev->queue_family_graphics_compute();
  if (df.vkCreateCommandPool(device, &cp_ci, nullptr, &g_wp.cpool) != VK_SUCCESS) return false;
  VkCommandBufferAllocateInfo cb_ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cb_ai.commandPool = g_wp.cpool;
  cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cb_ai.commandBufferCount = 1;
  if (df.vkAllocateCommandBuffers(device, &cb_ai, &g_wp.cmd) != VK_SUCCESS) return false;
  VkFenceCreateInfo f_ci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (df.vkCreateFence(device, &f_ci, nullptr, &g_wp.fence) != VK_SUCCESS) return false;
  g_wp.ok = true;
  return true;
}

// Stable identity for a captured draw: shader + index topology + stride.
uint64_t WindProbeKey(const renderer::RawGuestDraw& d) {
  uint64_t key = d.vs_hash * 0x9E3779B97F4A7C15ull ^ uint64_t(d.idx().size());
  const auto& ix = d.idx();
  for (size_t i = 0; i < ix.size() && i < 64; ++i) key = (key ^ ix[i]) * 1099511628211ull;
  if (!d.streams.empty()) key ^= uint64_t(d.streams[0].stride) << 48;
  return key;
}

// Runs the probe for one draw; returns 0 (keep) or 1 (flip) and caches it.
// Unclassifiable (offscreen / undecodable) draws retry on later frames, then
// park at keep.
int RunWindProbe(vk::VulkanDevice* dev, const renderer::RawGuestDraw& d,
                 const renderer::spc::CachedShader& vs, uint64_t key) {
  // Failure telemetry: every silent exit names itself (budgeted).
  auto wfail = [&](const char* why) {
    static std::atomic<int> s_wfb{60};
    if (s_wfb.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[WPROBE-] vs={:016X} fail={}", d.vs_hash, why);
  };
  auto give_up_or_retry = [&]() -> int {
    auto it = g_wp.cls.find(key);
    const int8_t used = it == g_wp.cls.end() ? 0 : it->second;
    g_wp.cls[key] = used <= -2 ? int8_t(0) : int8_t(used - 1);
    return 0;
  };
  if (!WindProbeInit(dev)) {
    wfail("init");
    return 0;
  }
  uint32_t kmax = 1;
  for (const auto& a : vs.t.attrs) kmax = std::max(kmax, a.location + 1);
  if (kmax > kWpMaxLoc) {
    wfail("kmax");
    return give_up_or_retry();
  }

  // Strip triangles with the rasterizer's parity rule (odd tris swap winding),
  // segmented by primitive-restart.
  const auto& idx = d.idx();
  std::vector<std::array<uint32_t, 3>> tris;
  size_t seg = 0;
  for (size_t i = 0; i + 2 < idx.size(); ++i) {
    if (idx[i] == 0xFFFFFFFFu) {
      seg = i + 1;
      continue;
    }
    uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
    if (b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
    if (a == b || b == c || a == c) continue;
    if (((i - seg) & 1) != 0) std::swap(a, b);
    tris.push_back({a, b, c});
  }
  if (tris.size() < 4) {
    wfail("few-tris");
    return give_up_or_retry();
  }

  // Attribute decode (streams are LE-normalized at capture).
  auto comps = [](uint32_t f) -> uint32_t {
    switch (f) { case 6: case 26: case 32: case 35: case 38: return 4;
      case 57: return 3; case 25: case 31: case 34: case 37: return 2;
      case 33: case 36: return 1; default: return 4; } };
  auto bytesz = [](uint32_t f) -> uint32_t {
    switch (f) { case 6: return 1; case 25: case 26: case 31: case 32: return 2;
      default: return 4; } };
  auto half2f = [](uint16_t h) -> float {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, f;
    if (e == 0) {
      if (!m) f = s << 31;
      else {
        e = 127 - 15 + 1;
        while (!(m & 0x400u)) { m <<= 1; --e; }
        f = (s << 31) | (e << 23) | ((m & 0x3FFu) << 13);
      }
    } else if (e == 31) f = (s << 31) | 0x7F800000u | (m << 13);
    else f = (s << 31) | ((e - 15 + 127) << 23) | (m << 13);
    float r;
    std::memcpy(&r, &f, 4);
    return r;
  };
  // fetch_slot -> stream index, first-appearance order (same rule as capture).
  // Xenos vertex-fetch constant indices span 0..95 (this title uses slot 95!).
  uint32_t slot_stream[96];
  for (auto& v : slot_stream) v = ~0u;
  uint32_t nstreams = 0;
  for (const auto& a : vs.t.attrs)
    if (a.fetch_slot < 96 && slot_stream[a.fetch_slot] == ~0u) slot_stream[a.fetch_slot] = nstreams++;
  auto decode_vertex = [&](uint32_t gi, float* dst /* kWpMaxLoc vec4s */) -> bool {
    for (const auto& a : vs.t.attrs) {
      if (a.fetch_slot >= 96 || slot_stream[a.fetch_slot] == ~0u) return false;
      const uint32_t si = slot_stream[a.fetch_slot];
      if (si >= d.streams.size() || !d.streams[si].data) return false;
      const auto& st = d.streams[si];
      const auto& bytes = st.bytes();
      const uint32_t nc = comps(a.format), bs = bytesz(a.format);
      const size_t base = size_t(gi) * st.stride + a.byte_offset;
      if (!st.stride || base + size_t(nc) * bs > bytes.size()) return false;
      float* out = dst + size_t(a.location) * 4;
      out[0] = out[1] = out[2] = 0.0f;
      out[3] = 1.0f;
      const uint8_t* p = bytes.data() + base;
      switch (a.format) {
        case 36: case 37: case 57: case 38:  // f32 x1/2/3/4
          for (uint32_t k = 0; k < nc; ++k) std::memcpy(&out[k], p + k * 4, 4);
          break;
        case 31: case 32: {  // f16 x2/4
          for (uint32_t k = 0; k < nc; ++k) {
            uint16_t h;
            std::memcpy(&h, p + k * 2, 2);
            out[k] = half2f(h);
          }
          break;
        }
        case 25: case 26: {  // i16/u16 x2/4 (+norm)
          for (uint32_t k = 0; k < nc; ++k) {
            uint16_t u;
            std::memcpy(&u, p + k * 2, 2);
            if (a.is_signed) {
              const int16_t sv = int16_t(u);
              out[k] = a.is_normalized ? std::max(float(sv) / 32767.0f, -1.0f) : float(sv);
            } else {
              out[k] = a.is_normalized ? float(u) / 65535.0f : float(u);
            }
          }
          break;
        }
        case 6: {  // u8 x4 (+norm)
          for (uint32_t k = 0; k < nc; ++k)
            out[k] = a.is_normalized ? float(p[k]) / 255.0f : float(p[k]);
          break;
        }
        default:
          return false;  // unhandled format feeding this VS -- skip draw
      }
    }
    return true;
  };

  // Sample evenly + decode; keep base vertices AND a normal-offset copy per
  // triangle (M3.61): in_0.xyz nudged along the authored vertex normal (attr
  // location 1, >=3 components -- the corpus convention). The offset run's
  // clip-z delta says which way the surface faces, giving a PER-TRIANGLE
  // inversion verdict that survives orientation-balanced batches (canyon
  // walls) where the old majority-front vote washed out to ~50%.
  const bool have_nrm = [&] {
    for (const auto& a : vs.t.attrs)
      if (a.location == 1 && comps(a.format) >= 3) return true;
    return false;
  }();
  float* am = static_cast<float*>(g_wp.attrs_map);
  const size_t step = std::max<size_t>(1, tris.size() / kWpMaxTris);
  std::vector<float> base_buf;  // nt * 3 * kWpMaxLoc*4 floats
  std::vector<float> off_buf;
  std::vector<uint8_t> tri_nrm_ok;
  base_buf.reserve(kWpMaxTris * 3 * kWpMaxLoc * 4);
  off_buf.reserve(kWpMaxTris * 3 * kWpMaxLoc * 4);
  uint32_t nt = 0;
  for (size_t t = 0; t < tris.size() && nt < kWpMaxTris; t += step) {
    float tmp[3][kWpMaxLoc * 4] = {};
    bool ok = true;
    for (int v = 0; v < 3 && ok; ++v) ok = decode_vertex(tris[t][v], tmp[v]);
    if (!ok) continue;
    // Per-triangle epsilon: a small fraction of the shortest edge (object
    // units), so the nudge is well inside float precision at any world scale.
    auto d3 = [](const float* a, const float* b) {
      const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
      return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    const float e01 = d3(tmp[0], tmp[1]), e12 = d3(tmp[1], tmp[2]), e02 = d3(tmp[0], tmp[2]);
    const float eps = std::max(0.02f * std::min({e01, e12, e02}), 1e-4f);
    bool nrm_ok = have_nrm;
    float offv[3][kWpMaxLoc * 4];
    std::memcpy(offv, tmp, sizeof(tmp));
    for (int v = 0; v < 3 && nrm_ok; ++v) {
      const float* n = tmp[v] + 4;  // location 1
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (!(len > 1e-6f) || !std::isfinite(len)) {
        nrm_ok = false;
        break;
      }
      for (int k = 0; k < 3; ++k) offv[v][k] += eps * (n[k] / len);
    }
    base_buf.insert(base_buf.end(), &tmp[0][0], &tmp[0][0] + 3 * kWpMaxLoc * 4);
    off_buf.insert(off_buf.end(), &offv[0][0], &offv[0][0] + 3 * kWpMaxLoc * 4);
    tri_nrm_ok.push_back(nrm_ok ? 1 : 0);
    ++nt;
  }
  if (nt < 4) {
    wfail("decode");
    return give_up_or_retry();
  }
  // Layout: base vertices at [0..3nt), offset vertices at [3nt..6nt).
  std::memcpy(am, base_buf.data(), base_buf.size() * 4);
  std::memcpy(am + size_t(nt) * 3 * kWpMaxLoc * 4, off_buf.data(), off_buf.size() * 4);

  // Constants UBO: c[256] + lc[8] + bc[2], zero-padded.
  uint8_t* um = static_cast<uint8_t*>(g_wp.ubo_map);
  std::memset(um, 0, kWpUboBytes);
  const size_t ncf = std::min<size_t>(d.vs_consts.size(), 256 * 4);
  if (ncf) std::memcpy(um, d.vs_consts.data(), ncf * 4);
  std::memcpy(um + 256 * 16, d.loop_consts, sizeof(d.loop_consts));
  std::memcpy(um + 256 * 16 + 8 * 16, d.bool_consts, sizeof(d.bool_consts));
  vk::util::FlushMappedMemoryRange(dev, g_wp.ubo_mem, g_wp.ubo_type);
  vk::util::FlushMappedMemoryRange(dev, g_wp.attrs_mem, g_wp.attrs_type);

  const auto& df = dev->functions();
  VkDevice device = dev->device();
  VkPipeline pipe = VK_NULL_HANDLE;
  if (auto it = g_wp.pipes.find(d.vs_hash); it != g_wp.pipes.end()) {
    pipe = it->second;
  } else {
    VkShaderModule mod = vk::util::CreateShaderModule(dev, vs.probe_spirv.data(),
                                                      vs.probe_spirv.size() * 4);
    if (mod) {
      VkComputePipelineCreateInfo cp = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
      cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      cp.stage.module = mod;
      cp.stage.pName = "main";
      cp.layout = g_wp.layout;
      if (df.vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cp, nullptr, &pipe) !=
          VK_SUCCESS)
        pipe = VK_NULL_HANDLE;
      df.vkDestroyShaderModule(device, mod, nullptr);
    }
    g_wp.pipes[d.vs_hash] = pipe;  // cache failures too (never rebuild)
  }
  if (!pipe) {
    wfail("nopipe");
    g_wp.cls[key] = 0;
    return 0;
  }

  VkCommandBufferBeginInfo cb_bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  cb_bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  df.vkBeginCommandBuffer(g_wp.cmd, &cb_bi);
  df.vkCmdBindPipeline(g_wp.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
  df.vkCmdBindDescriptorSets(g_wp.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_wp.layout, 0, 1,
                             &g_wp.dset, 0, nullptr);
  df.vkCmdPushConstants(g_wp.cmd, g_wp.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, d.ndc);
  df.vkCmdDispatch(g_wp.cmd, nt * 2, 1, 1);  // M3.61: base + normal-offset regions
  VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_HOST_READ_BIT};
  df.vkCmdPipelineBarrier(g_wp.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
  df.vkEndCommandBuffer(g_wp.cmd);
  df.vkResetFences(device, 1, &g_wp.fence);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &g_wp.cmd;
  {
    auto acq = dev->AcquireQueue(dev->queue_family_graphics_compute(), 0);
    df.vkQueueSubmit(acq.queue(), 1, &si, g_wp.fence);
  }
  df.vkWaitForFences(device, 1, &g_wp.fence, VK_TRUE, UINT64_MAX);
  VkMappedMemoryRange rng = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, g_wp.outb_mem, 0,
                             VK_WHOLE_SIZE};
  df.vkInvalidateMappedMemoryRanges(device, 1, &rng);

  // The pipeline's front-face for this draw (base CW, M3.18 mirror flip;
  // skinned never reaches here). The rasterizer compares post-ndc winding
  // against this -- and the probe measured exactly that space.
  bool front_cw = !(d.su_mode & 4);
  if (d.ndc[0] * d.ndc[1] > 0.0f) front_cw = !front_cw;
  const float* om = static_cast<const float*>(g_wp.outb_map);
  // Per-triangle readout: screen winding from the base region; surface-facing
  // from the offset region's ndc-z delta (D3D ndc z: near 0 -> far 1, so a
  // nudge along the OUTWARD normal moves z DOWN when the face points at the
  // camera). M3.61 verdict: a triangle is INVERTED when its actual front-ness
  // (winding == pipeline frontFace) disagrees with its normal's facing.
  int valid = 0, front = 0;      // majority-front vote (M3.60, primary)
  int n_ok = 0, n_inv = 0;       // normal-vs-winding (LOG-ONLY: blind here --
                                 // this title's inversion flips winding AND
                                 // normals coherently, so consistency is
                                 // invariant; gate data: front=0/16 fully
                                 // invisible draws read ninv=0 "consistent")
  double wsum_f = 0.0, wsum_b = 0.0;  // M3.62b: mean LINEAR view depth (clip w)
  int nzf = 0, nzb = 0;               // per facing set -- ndc z is far-plane
                                      // compressed (hut noise all sat at
                                      // z 0.93-0.99, deltas ~1e-3 = slope
                                      // noise); w separates in world units.
  for (uint32_t t = 0; t < nt; ++t) {
    float x[3], y[3];
    double zb = 0.0, zo = 0.0, wsum = 0.0;
    bool okt = true, okz = t < tri_nrm_ok.size() && tri_nrm_ok[t];
    for (int v = 0; v < 3 && okt; ++v) {
      const float* cp = om + (size_t(t) * 3 + v) * 4;
      const float w = cp[3];
      if (!(w > 1e-6f) || !std::isfinite(w)) { okt = false; break; }
      x[v] = cp[0] / w;
      y[v] = cp[1] / w;
      if (!std::isfinite(x[v]) || !std::isfinite(y[v])) okt = false;
      zb += double(cp[2]) / w;
      wsum += double(w);
      const float* co = om + (size_t(nt) * 3 + size_t(t) * 3 + v) * 4;
      const float wo = co[3];
      if (!(wo > 1e-6f) || !std::isfinite(wo)) okz = false;
      else zo += double(co[2]) / wo;
    }
    if (!okt) continue;
    const float a2 = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
    if (std::fabs(a2) < 1e-12f) continue;
    // Positive-height viewport: NDC y and framebuffer y share sign, so the
    // shoelace sign in NDC equals the rasterizer's framebuffer classification:
    // a2 > 0 == clockwise (y-down).
    ++valid;
    const bool is_front = (a2 > 0.0f) == front_cw;
    if (is_front) ++front;
    const double wt = wsum / 3.0;
    if (std::isfinite(wt)) {
      if (is_front) { wsum_f += wt; ++nzf; }
      else { wsum_b += wt; ++nzb; }
    }
    if (okz) {
      const double dz = (zo - zb) / 3.0;
      if (std::isfinite(dz) && std::fabs(dz) > 1e-9) {
        ++n_ok;
        if (is_front != (dz > 0.0)) ++n_inv;
      }
    }
  }
  if (valid < 4) {
    wfail("few-valid");
    return give_up_or_retry();
  }
  // Decision (M3.62):
  //  1) PRIMARY (M3.60, gate-proven: floor fills, hut identical): a cull-back
  //     draw overwhelmingly back-facing is inverted -- games don't submit
  //     perpetually invisible draws every frame.
  //  2) SECONDARY, for the mixed (orientation-balanced) wall batches facing
  //     statistics can't decide: DEPTH ORDER of the facing sets. Inverted
  //     batches render their FAR side (the near visible side is the one
  //     culled -- NO_CULL showed the walls exist), so mean ndc depth
  //     (near 0 -> far 1) of the front set sits FARTHER than the culled set.
  //     Coincident double-sided copies differ by ~0 and the margin keeps them.
  const float ff = float(front) / float(valid);
  const double wf = nzf ? wsum_f / nzf : 0.0, wbk = nzb ? wsum_b / nzb : 0.0;
  int flip = 0;
  const char* method = "front";
  if (ff <= 0.3f) {
    flip = 1;
  }
  // M3.62 postmortem -- do NOT resurrect statistical wall classifiers: the
  // depth-order rule ("inverted batches render their far side") is CONFOUNDED
  // by correct interiors viewed from inside (the far wall faces the camera,
  // near-side exteriors face away -> front set 15-60% deeper in the HUT's
  // correct geometry, in LINEAR depth). Normal-consistency is likewise blind
  // (this title inverts winding+normals coherently). Only the decisive
  // minority-front rule above has ground-truth backing. The remaining gate
  // walls need the native-vs-emulated PM4 state comparison (trace method),
  // not another aggregate heuristic. wf/wb stay in the log as telemetry.
  g_wp.cls[key] = int8_t(flip);
  static std::atomic<int> s_wpb{160};
  if (s_wpb.fetch_sub(1, std::memory_order_relaxed) > 0)
    REXLOG_INFO("[WPROBE] vs={:016X} tris={} valid={} front={:.0f}% wf={:.1f}/{} wb={:.1f}/{} "
                "nrm={}/{} cull={} -> {} ({})",
                d.vs_hash, nt, valid, ff * 100.0f, wf, nzf, wbk, nzb, n_inv, n_ok,
                (d.su_mode & 2) ? "BACK" : "none", flip ? "FLIP" : "keep", method);
  return flip;
}

}  // namespace

// M4.0 enqueue_only: the pre-warm replay thread requests builds through this
// same function so the pipeline KEY is computed by the one true code path --
// but it must never touch tl.pipelines (present-thread-owned): it skips the
// cache lookup and only takes the mutex-guarded inflight/enqueue path.
VkPipeline GetOrCreateTranslatedPipeline(vk::VulkanDevice* dev, const renderer::RawGuestDraw& d,
                                         const renderer::spc::CachedShader& vs,
                                         const renderer::spc::CachedShader& ps,
                                         bool build_now = false, bool enqueue_only = false) {
  auto& tl = TL();
  const uint64_t vs_hash = d.vs_hash, ps_hash = d.ps_hash;
  const uint32_t prim = d.prim;
  uint64_t key = vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull) ^ (uint64_t(prim) << 3);
  key = (key ^ d.blend_control) * 1099511628211ull;
  key = (key ^ d.color_mask) * 1099511628211ull;
  // M3.1: depth test/write/func (RB_DEPTHCONTROL bits 1-6) + cull/winding
  // (PA_SU_SC_MODE_CNTL bits 0-2).
  key = (key ^ (d.depth_control & 0x7E)) * 1099511628211ull;
  // M3.97: stencil enable (bit0) + func/op fields (bits 8-22) and the
  // ref/masks now select a distinct pipeline -- two draws that differ only in
  // stencil state must NOT share one.
  key = (key ^ (d.depth_control & 0x7FFF01u)) * 1099511628211ull;
  key = (key ^ uint64_t(d.stencil_ref_mask)) * 1099511628211ull;
  key = (key ^ uint64_t(d.stencil_ref_mask_bf)) * 1099511628211ull;
  // M4.2: cached -- these six ran a raw environ scan each, per draw, on the
  // present thread, ahead of the pipeline-cache lookup (the M3.45 bug class).
  static const bool s_key_force_shaftmask = getenv("RESTUFF_FORCE_SHAFTMASK") != nullptr;
  static const bool s_key_shaftmask_zflip = getenv("RESTUFF_SHAFTMASK_ZFLIP") != nullptr;
  static const bool s_key_mask_znever = getenv("RESTUFF_MASK_ZNEVER") != nullptr;
  static const bool s_key_mask_nocull = getenv("RESTUFF_MASK_NOCULL") != nullptr;
  static const bool s_key_volmask_backonly = getenv("RESTUFF_VOLMASK_BACKONLY") != nullptr;
  static const bool s_key_world_noswrite = getenv("RESTUFF_WORLD_NOSWRITE") != nullptr;
  if (s_key_force_shaftmask) key = (key ^ 0x5AFEu) * 1099511628211ull;
  if (s_key_shaftmask_zflip) key = (key ^ 0x21FDu) * 1099511628211ull;
  if (s_key_mask_znever) key = (key ^ 0x7E7Eu) * 1099511628211ull;
  if (s_key_mask_nocull) key = (key ^ 0x3C3Cu) * 1099511628211ull;
  if (s_key_volmask_backonly) key = (key ^ 0x0B0Bu) * 1099511628211ull;
  if (s_key_world_noswrite) key = (key ^ 0x1B1Bu) * 1099511628211ull;
  key = (key ^ ((d.su_mode & 7) << 8)) * 1099511628211ull;
  // M3.126: restart-enable is baked into the pipeline -- same shaders+state
  // with a different guest reset policy must not share one.
  key = (key ^ (d.prim_reset_enabled ? 0x40000u : 0u)) * 1099511628211ull;
  // M3.18: mirrored draws (viewport sign product positive) flip winding.
  const bool mirror = d.ndc[0] * d.ndc[1] > 0.0f;
  key = (key ^ (mirror ? 0x10000u : 0u)) * 1099511628211ull;
  // M3.23: small-viewport (post) draws get a depth-off pipeline variant.
  {
    float _kxs; std::memcpy(&_kxs, &d.dbg_vport[0], 4);
    const bool small_vp_k = std::isfinite(_kxs) && std::fabs(_kxs) * 2.0f < 600.0f;
    key = (key ^ (small_vp_k ? 0x20000u : 0u)) * 1099511628211ull;
  }
  // M3.58: a depth-ALWAYS write with a NON-inverted viewport (vport_zscale > 0)
  // in this reverse-Z title stamps window z=1.0 (the NEAREST value) into the
  // shared depth buffer, blacking out every later world draw it covers -- the
  // "missing floor showing the water beneath the level" defect (RenderDoc
  // depth-test overlay of the floor draw = solid red = z-fail). The whole 172-
  // draw main pass uses the inverted range (zscale=-1) except this one first
  // background/fill quad (zscale=+1, cmp=ALWAYS, z_write). On the console each
  // pass owns its EDRAM depth; our single shared buffer must not be corrupted,
  // so we drop this draw's depth WRITE below (its colour still draws; later
  // world geometry then tests against the far clear and renders). Keyed for a
  // distinct pipeline variant. RESTUFF_KEEP_ALWAYSZ=1 restores the old behaviour.
  static const bool s_keep_alwaysz = getenv("RESTUFF_KEEP_ALWAYSZ") != nullptr;
  // M3.118: scope the neutralize to MAIN-pitch surfaces. The aux shadow
  // chain's depth-RESTORE quads are ALSO depth-ALWAYS writers; neutralizing
  // them left the aux depth EMPTY (the guest builds it via clear+restore --
  // emulated eids 9832/9836/9839), which the M3.99 prefill then papered over
  // with a full current-scene copy at ~5.5x the intended values -> thin
  // volume marks -> the detached head shadow. Pitch heuristic: main = 1360,
  // aux = 720 (RB_SURFACE_INFO low bits) for this title.
  // RESTUFF_NEUTRALIZE_ALL=1 restores the old unscoped behaviour.
  static const bool s_neut_all = getenv("RESTUFF_NEUTRALIZE_ALL") != nullptr;
  const bool neutralize_alwaysz =
      !s_keep_alwaysz && ((d.depth_control >> 4) & 7) == 7 &&
      ((d.depth_control >> 2) & 1) && d.vport_zscale > 0.0f &&
      (s_neut_all || (d.dbg_surf & 0x3FFF) >= 1000);
  key = (key ^ (neutralize_alwaysz ? 0x40000u : 0u)) * 1099511628211ull;
  // M3.59 (RESTUFF_WINDDET=1): flip winding for mirrored-instance cull-back
  // draws -- those whose WVP determinant sign is the MINORITY for their shader
  // (per-frame majority vote in tl.wind_majority). Skinned draws are excluded
  // (M3.41 already handles their winding). This renders the missing floor
  // (backface-culled inverted geometry) without disturbing the correctly-wound
  // majority. The async worker can't recompute the determinant (fd carries no
  // vs_consts) so it receives the decision via d.wind_flip_hint.
  static const bool s_winddet = getenv("RESTUFF_WINDDET") != nullptr;
  // M3.71: probe/det-sign flips only exist to patch the legacy inverted base.
  static const bool s_legacy_wind_key = getenv("RESTUFF_LEGACY_WINDING") != nullptr;
  const bool is_skinned_w = vs.t.rel_fetch_slot != ~0u || vs.t.rel_fetch_slot2 != ~0u;
  bool wind_flip = false;
  if (s_legacy_wind_key && (d.su_mode & 2) && !is_skinned_w) {
    if (d.wind_flip_hint >= 0) {
      // M3.60: winding-probe decision (set in the prep loop under
      // RESTUFF_WINDPROBE; replayed by the async worker via rq.wind_flip).
      wind_flip = d.wind_flip_hint != 0;
    } else if (s_winddet) {
      // M3.59 det-sign heuristic (kept for A/B; found nothing on this title).
      const int sgn = WvpDetSign(d.vs_consts);
      auto wit = tl.wind_majority.find(d.vs_hash);
      if (sgn != 0 && wit != tl.wind_majority.end() && wit->second != 0 && sgn != wit->second)
        wind_flip = true;
    }
  }
  key = (key ^ (wind_flip ? 0x80000u : 0u)) * 1099511628211ull;
  // M3.59b (RESTUFF_FLIP_VS=<hex,hex,...>): empirically flip winding for these
  // exact VS hashes -- used to pin which shaders render the backface-culled
  // floor (the inversion is per-shader, not per-instance). Depends only on
  // vs_hash, so the async worker reproduces it from rq.vs_hash (no hint needed).
  static const std::vector<uint64_t> s_flipvs = [] {
    std::vector<uint64_t> v;
    if (const char* e = getenv("RESTUFF_FLIP_VS")) {
      for (const char* s = e; *s;) {
        char* end = nullptr;
        const uint64_t h = strtoull(s, &end, 16);
        if (end == s) break;
        v.push_back(h);
        s = (*end == ',') ? end + 1 : end;
      }
    }
    return v;
  }();
  bool flip_vs = false;
  for (uint64_t h : s_flipvs)
    if (h == d.vs_hash) { flip_vs = true; break; }
  key = (key ^ (flip_vs ? 0x100000u : 0u)) * 1099511628211ull;
  // tl.pipelines belongs to the present thread; the worker (build_now) and the
  // pre-warm replayer (enqueue_only) must not touch it (workers publish via
  // pipe_done instead).
  if (!build_now && !enqueue_only) {
    if (auto it = tl.pipelines.find(key); it != tl.pipelines.end()) return it->second;
  }
  // RESTUFF_BLACKLIST_PS=<hex>[,<hex>...]: never build pipelines for these
  // pixel shaders (draws drop as nopipe). Escape hatch for shaders that wedge
  // the NVIDIA SPIR-V compiler (ps_cbb3335f...: minutes-long compile that also
  // corrupts the host heap; cores 102658/106646).
  {
    static const std::vector<uint64_t> blacklist = [] {
      std::vector<uint64_t> v;
      if (const char* e = getenv("RESTUFF_BLACKLIST_PS")) {
        for (const char* s = e; *s;) {
          char* end = nullptr;
          const uint64_t h = strtoull(s, &end, 16);
          if (end == s) break;
          v.push_back(h);
          s = (*end == ',') ? end + 1 : end;
        }
      }
      return v;
    }();
    for (uint64_t h : blacklist) {
      if (h == ps_hash) {
        static std::atomic<int> s_bl_log{8};
        if (s_bl_log.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_WARN("[native_vk] pipeline BLACKLISTED ps={:016X} (vs={:016X})", ps_hash, vs_hash);
        // Cache the null only on the present thread (the map is its own).
        if (!build_now && !enqueue_only) tl.pipelines[key] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
      }
    }
  }

  // M3.15: on a miss, hand the build to the worker thread and skip the draw
  // until it lands (RESTUFF_SYNC_PIPELINES=1 restores inline builds for A/B).
  // M4.0: enqueue_only (pre-warm) always enqueues -- an inline build off the
  // present thread would publish into tl.pipelines, which it must not touch.
  static const bool sync_pipes = getenv("RESTUFF_SYNC_PIPELINES") != nullptr;
  if (enqueue_only || (!build_now && !sync_pipes)) {
    std::lock_guard<std::mutex> lk(tl.pipe_mutex);
    if (tl.pipe_inflight.insert(key).second) {
      TranslatedLayer::PipeReq rq;
      rq.key = key;
      rq.vs = &vs;
      rq.ps = &ps;
      rq.vs_hash = vs_hash;
      rq.ps_hash = ps_hash;
      rq.prim = prim;
      rq.blend_control = d.blend_control;
      rq.color_mask = d.color_mask;
      rq.depth_control = d.depth_control;
      rq.stencil_ref_mask = d.stencil_ref_mask;
      rq.stencil_ref_mask_bf = d.stencil_ref_mask_bf;
      rq.su_mode = d.su_mode;
      rq.mirror = mirror;
      std::memcpy(&rq.vport_xscale, &d.dbg_vport[0], 4);
      rq.vport_zscale = d.vport_zscale;  // M3.58: depth-ALWAYS neutralize key input
      rq.wind_flip = wind_flip;          // M3.59: mirrored-instance winding decision
      rq.prim_reset = d.prim_reset_enabled;  // M3.126: keep the async key in sync
      rq.dbg_surf = d.dbg_surf;  // M4.0: neutralize_alwaysz key input (M3.118)
      rq.prewarm = enqueue_only;
      // M4.0: live draw requests jump the pre-warm backlog -- a cold-cache
      // replay can hold tens of seconds of builds, and a draw the game wants
      // NOW must not sit behind it (boot UI would nopipe-drop for the whole
      // replay). With no backlog queued, live requests keep their original
      // FIFO order exactly as before M4.0.
      if (enqueue_only) {
        tl.pipe_queue.push_back(rq);
        ++tl.prewarm_queued;
      } else if (tl.prewarm_queued > 0) {
        tl.pipe_queue.push_front(rq);
      } else {
        tl.pipe_queue.push_back(rq);
      }
      tl.pipe_cv.notify_one();
      // M4.0: flatten this first-seen request into the pre-warm log; replayed
      // through this same path at the next startup so the whole population
      // builds during boot instead of at first draw. The seen-set keys on the
      // record bytes, so replayed and re-requested combos don't duplicate.
      static const bool s_prewarm_off = getenv("RESTUFF_NO_PREWARM") != nullptr;
      if (!s_prewarm_off) {
        TranslatedLayer::PrewarmRec pr;
        pr.vs_hash = vs_hash;
        pr.ps_hash = ps_hash;
        pr.prim = prim;
        pr.blend_control = d.blend_control;
        pr.color_mask = d.color_mask;
        pr.depth_control = d.depth_control;
        pr.su_mode = d.su_mode;
        pr.stencil_ref_mask = d.stencil_ref_mask;
        pr.stencil_ref_mask_bf = d.stencil_ref_mask_bf;
        pr.dbg_surf = d.dbg_surf;
        pr.vport_xscale = rq.vport_xscale;
        pr.vport_zscale = d.vport_zscale;
        pr.flags = (mirror ? 1u : 0u) | (wind_flip ? 2u : 0u) |
                   (d.prim_reset_enabled ? 4u : 0u);
        uint64_t rh = 1469598103934665603ull;
        const uint8_t* pb = reinterpret_cast<const uint8_t*>(&pr);
        for (size_t i = 0; i < sizeof(pr); ++i) rh = (rh ^ pb[i]) * 1099511628211ull;
        // Size check FIRST: inserting into the seen-set at the cap would mark
        // a rec "seen" without ever logging it (dropped from every save).
        if (tl.prewarm_log.size() < kPrewarmMaxRecs && tl.prewarm_seen.insert(rh).second) {
          tl.prewarm_log.push_back(pr);
          tl.prewarm_dirty = true;
        } else if (tl.prewarm_log.size() >= kPrewarmMaxRecs) {
          // Say so once. A capture that silently stops recording looks exactly
          // like a complete one until someone plays the level it dropped.
          static std::atomic<bool> s_warned{false};
          if (!s_warned.exchange(true))
            REXLOG_ERROR("[native_vk] prewarm manifest FULL at {} records -- further pipelines "
                         "are NOT being recorded; raise kPrewarmMaxRecs",
                         kPrewarmMaxRecs);
        }
      }
    }
    return VK_NULL_HANDLE;
  }

  const auto& df = dev->functions();
  VkDevice device = dev->device();
  VkShaderModule vsm = vk::util::CreateShaderModule(dev, vs.spirv.data(), vs.spirv.size() * 4);
  VkShaderModule fsm = vk::util::CreateShaderModule(dev, ps.spirv.data(), ps.spirv.size() * 4);
  // M3.154: this failure was SILENT -- pipeline stays null below, gets
  // published/cached (no-retry by design), and the draw nopipe-drops for the
  // rest of the boot. A transient vkCreateShaderModule failure during the
  // level-load build burst is a permanent hole for whatever that draw was.
  if (!vsm || !fsm) {
    static std::atomic<int> s_smf{40};
    if (s_smf.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_WARN("[native_vk] shader-module create FAILED vs={:016X}({}) ps={:016X}({})",
                  vs_hash, vsm ? "ok" : "NULL", ps_hash, fsm ? "ok" : "NULL");
  }
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vsm && fsm) {
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vsm;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fsm;
    stages[1].pName = "main";

    // M3.0: one vertex binding per distinct fetch slot, in first-appearance
    // order of vs.t.attrs -- the same rule the capture uses to build
    // RawGuestDraw::streams, so binding N == streams[N].
    std::vector<VkVertexInputBindingDescription> binds;
    std::vector<VkVertexInputAttributeDescription> attrs;
    std::vector<uint32_t> slot_order;
    for (const auto& a : vs.t.attrs) {
      uint32_t binding = UINT32_MAX;
      for (uint32_t b = 0; b < slot_order.size(); ++b)
        if (slot_order[b] == a.fetch_slot) binding = b;
      if (binding == UINT32_MAX) {
        binding = uint32_t(binds.size());
        slot_order.push_back(a.fetch_slot);
        binds.push_back({binding, a.stride_bytes, VK_VERTEX_INPUT_RATE_VERTEX});
      }
      attrs.push_back({a.location, binding,
                       XenosVtxVkFormat(a.format, a.is_signed, a.is_normalized), a.byte_offset});
    }
    VkPipelineVertexInputStateCreateInfo vi = {};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = uint32_t(binds.size());
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attrs.size());
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia = {};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = XenosTopology(prim);
    // M3.126: restart follows the GUEST (PA_SU_SC_MODE_CNTL.multi_prim_ib_ena
    // + VGT_MULTI_PRIM_IB_RESET_INDX, resolved at capture into
    // prim_reset_enabled); cuts were re-emitted as the u32 sentinel there.
    // Restart is only legal on strip/fan topologies in Vulkan.
    ia.primitiveRestartEnable = (d.prim_reset_enabled &&
                                 (ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                                  ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN))
                                    ? VK_TRUE
                                    : VK_FALSE;

    VkPipelineViewportStateCreateInfo vp = {};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = {};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // M3.107: depth clamp, globally, exactly like the reference plugin (every
    // pipeline in emulated_idle.rdc has depthClampEnable=1). Root cause of the
    // phantom second shadow: the sun-shadow volumes extrude past the far
    // plane, and without clamping Vulkan CLIPS the far caps -- the volume
    // opens, its front faces DECR_WRAP with no matching back INCR, and the
    // net -1 (=255) stencil region reads as "in shadow" to the shaft's
    // LESS-ref=0 test (post-volume census: {1:7418, 2:80, 254:201, 255:2332}
    // -- the 254/255s are the displaced copy). Xenos rasterization has no
    // far clip in DX clip space; clamp restores the closed shell.
    // RESTUFF_NO_DEPTH_CLAMP=1 for A/B.
    static const bool s_depth_clamp =
        dev->properties().depthClamp && getenv("RESTUFF_NO_DEPTH_CLAMP") == nullptr;
    static std::atomic<bool> s_dc_logged{false};
    if (!s_dc_logged.exchange(true))
      REXLOG_INFO("[native_vk] M3.107 depth clamp {}",
                  s_depth_clamp ? "ON"
                  : dev->properties().depthClamp ? "off (env)"
                                                : "UNAVAILABLE (device feature)");
    rs.depthClampEnable = s_depth_clamp ? VK_TRUE : VK_FALSE;
    // M3.111: fail-side depth bias for the shadow-volume mask builders
    // (dc=C07E07C3: two-sided zfail DECR/INCR_WRAP). Their front cap re-renders
    // the bear's own skin, so fragment depth lands EXACTLY on the stored value
    // and GREATER resolves by FP noise: front-cap +1 speckle crawls on the
    // bear, grazing back faces near the silhouette produce net -1 patches (the
    // displaced phantom). The pre-M3.105 2x2-max aux prefill hid this by
    // biasing every comparison the same way; at full res the bias must be
    // explicit. A few LSB toward z-fail makes self-comparisons reliably FAIL
    // (both wrap ops fire, net 0) without moving the true shadow boundary.
    // Reversed-Z GREATER passes nearer, so fail-side = smaller window z =
    // negative bias. RESTUFF_VOLBIAS=<int> overrides (0 disables).
    {
      static const int s_volbias = [] {
        const char* e = getenv("RESTUFF_VOLBIAS");
        return e ? atoi(e) : 0;  // unproven against the phantom; opt-in only
      }();
      const bool vol_sig = ((d.depth_control >> 7) & 1) &&
                           ((d.depth_control >> 17) & 7) == 7 &&
                           ((d.depth_control >> 29) & 7) == 6;
      if (vol_sig && s_volbias != 0) {
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = float(s_volbias);
      }
    }
    // M3.1: PA_SU_SC_MODE_CNTL cull_front:1@0, cull_back:1@1, face:1@2
    // (0 = CCW front).
    rs.cullMode = ((d.su_mode & 1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                  ((d.su_mode & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    // M3.125 DIAGNOSTIC (RESTUFF_NO_CULL=1): disable face culling everywhere.
    // Splits the "randomly missing / see-through triangles" class in two: if
    // the holes fill in, the geometry IS submitted and we cull it wrongly
    // (winding); if they persist, the triangles never reach the GPU (dropped
    // upstream) or lose the depth test. Diagnostic only -- leaving it on
    // renders true backfaces.
    static const bool s_no_cull = getenv("RESTUFF_NO_CULL") != nullptr;
    if (s_no_cull) rs.cullMode = VK_CULL_MODE_NONE;
    // M3.71: REFERENCE-EXACT winding. The SDK reference derives host front-face
    // purely from the face bit (vulkan/pipeline_cache.cpp:
    // front_face_clockwise = face != 0) and NEVER adjusts it -- the signed
    // ndc scale (guest viewport scale signs ride through pc.ndc, exactly like
    // the reference's ndc_scale) already places geometry in framebuffer space
    // precisely as the guest viewport would, mirrors and Y flips included, so
    // Xenos screen-space winding == Vulkan screen-space winding by
    // construction and the face bit maps 1:1. Our old base rule was the
    // OPPOSITE (assumed the Y flip mirrors winding); M3.18 (mirror parity),
    // M3.41 (skin flip) and the M3.60 probe were all partial compensations on
    // top of that inverted base -- the probe un-inverts solidly-wound meshes
    // but can nothing about mixed-winding strips (the gate terrain stitches
    // segments of BOTH windings into one strip: trace frame 5139 n=630 has
    // 190 segments one way, 118 the other), where we rendered the wrong half
    // and culled the visible half = the under-map water holes.
    // RESTUFF_LEGACY_WINDING=1 restores the old heuristic stack for A/B.
    static const bool s_legacy_winding = getenv("RESTUFF_LEGACY_WINDING") != nullptr;
    rs.frontFace =
        (d.su_mode & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    if (s_legacy_winding) {
      rs.frontFace =
          (d.su_mode & 4) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
      // M3.18: a mirrored clip transform (viewport sign parity) mirrors winding.
      if (mirror) {
        rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                               : VK_FRONT_FACE_CLOCKWISE;
      }
      // M3.41: skinned meshes fetch position through the bone rel-fetch path;
      // flipping them was "correct" only because it un-inverted the legacy
      // base (net CCW == what M3.71 now yields directly).
      // RESTUFF_NO_FLIP_SKIN=1 disables.
      static const bool flip_skin = getenv("RESTUFF_NO_FLIP_SKIN") == nullptr;
      if (flip_skin && (vs.t.rel_fetch_slot != ~0u || vs.t.rel_fetch_slot2 != ~0u)) {
        rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                               : VK_FRONT_FACE_CLOCKWISE;
      }
    }
    // M3.59 EXPERIMENT (RESTUFF_FLIP_CULLBACK=1): the missing-floor draws are
    // BACKFACE-culled -- their front faces come out CCW while frontFace=CW, so
    // they vanish (RenderDoc pixel history: "Backface culled"). This flips the
    // front-face sense for cull-back draws to test whether the whole cull-back
    // class is winding-inverted vs the cull-none majority (which masks it).
    static const bool flip_cullback = getenv("RESTUFF_FLIP_CULLBACK") != nullptr;
    const bool is_skinned = vs.t.rel_fetch_slot != ~0u || vs.t.rel_fetch_slot2 != ~0u;
    if (flip_cullback && (d.su_mode & 2) && !is_skinned) {
      rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                             : VK_FRONT_FACE_CLOCKWISE;
    }
    // M3.59: apply the mirrored-instance winding flip decided at the key above.
    if (wind_flip) {
      rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                             : VK_FRONT_FACE_CLOCKWISE;
    }
    // M3.59b: empirical per-shader winding flip (RESTUFF_FLIP_VS).
    if (flip_vs) {
      rs.frontFace = rs.frontFace == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                             : VK_FRONT_FACE_CLOCKWISE;
    }
    rs.lineWidth = 1.0f;
    // M3.26: this title draws the world twice -- a z-prepass (cm=0) then an
    // opaque colour pass (cm=F) of the SAME geometry, both zfunc GEQUAL. The
    // colour pass must pass on the equality edge against the depth the prepass
    // wrote, but the two are separate pipelines and NVIDIA does not honour
    // cross-pipeline `invariant gl_Position`, so the colour z lands ~1 ULP off
    // and GEQUAL FAILS -> no colour written. M3.27 (skip the prepass) is the
    // real fix -- float-depth ULPs made the bias fail at distance -- so the
    // bias now DEFAULTS OFF; RESTUFF_ZBIAS=<units> re-enables for experiments.
    static const float zbias = [] {
      const char* e = getenv("RESTUFF_ZBIAS");
      return e ? float(atof(e)) : 0.0f;
    }();
    if (d.color_mask && zbias != 0.0f && ((d.depth_control >> 1) & 1)) {
      rs.depthBiasEnable = VK_TRUE;
      rs.depthBiasConstantFactor = zbias;
    }
    // M4.26 THE EP5 DOCK FLICKER FIX (RenderDoc pixel-history proven): the
    // blob-shadow decal pass renders knife-edge coplanar with its receivers
    // (water/planks) -- fragZ vs receiver depth differ by ~ULPs of D32F, so
    // under reversed-Z GEQUAL the decal STROBES: it passes only on frames
    // where camera drift lands both values on the same rounding boundary
    // (capture: 2 passes / 25 frames at one pixel, dark alpha 0.63 = the
    // flicker). Xenos EDRAM depth was 24-bit float -- its coarser quantum
    // merged the pair, GEQUAL tied CONSISTENTLY, console showed a STABLE
    // shadow. A few-quanta positive bias (toward the camera in reversed-Z)
    // on the decal pipeline alone restores console behaviour.
    // RESTUFF_DECAL_BIAS=<units> tunes; 0 disables. Default 16384, found by
    // binary search at the Ep5 dock (Aug 27): 32 was invisible, 2048 flipped
    // the strobe polarity (shadow stable, LIGHT blinking through = margin
    // half-cleared), 16384 = fully stable with no bleed-through. The wide
    // value is expected: Vulkan scales float-depth bias by the PRIMITIVE's
    // max-z exponent, and the decal strips span near geometry.
    static const float s_decal_bias = [] {
      const char* e = getenv("RESTUFF_DECAL_BIAS");
      return e ? float(atof(e)) : 16384.0f;
    }();
    if (s_decal_bias != 0.0f && d.ps_hash == 0xBBA590486A51E72Aull) {
      rs.depthBiasEnable = VK_TRUE;
      rs.depthBiasConstantFactor = s_decal_bias;
    }

    VkPipelineMultisampleStateCreateInfo ms = {};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // M3.1: RB_DEPTHCONTROL z_enable:1@1, z_write_enable:1@2, zfunc:3@4
    // (Xenos CompareFunction == VkCompareOp value order). Stencil unhandled.
    // RESTUFF_NO_DEPTH=1 / RESTUFF_NO_CULL=1: A/B the 3D state (invisible-scene
    // triage: depth-fail vs culled-away).
    static const bool no_depth = getenv("RESTUFF_NO_DEPTH") != nullptr;
    static const bool no_cull = getenv("RESTUFF_NO_CULL") != nullptr;
    if (no_cull) rs.cullMode = VK_CULL_MODE_NONE;
    // M3.23: the bloom/DOF post passes render to a SMALL viewport at the corner
    // of the SHARED main depth buffer and write near-max depth (z=1.0) there
    // (confirmed by the depth dump: a z=1.0 block in the top-left corner). World
    // colour geometry overlapping it then fails GEQUAL and renders black. On
    // hardware each pass has its own EDRAM depth; my flatten shares one. These
    // composite quads need no depth, so drop depth test/write for them. Keyed
    // below so the pipeline variant is distinct. RESTUFF_NO_POST_DEPTH=1 = old.
    float _vxs; std::memcpy(&_vxs, &d.dbg_vport[0], 4);
    const bool small_vp = std::isfinite(_vxs) && std::fabs(_vxs) * 2.0f < 600.0f;
    // Measured: dropping depth on small-viewport passes did NOT reduce the
    // black, so it's OPT-IN, not default. The corner z=1.0 block in the depth
    // dump was not the on-screen occluder.
    static const bool post_depth_off = getenv("RESTUFF_POST_DEPTH_OFF") != nullptr;
    const bool drop_depth = small_vp && post_depth_off;
    VkPipelineDepthStencilStateCreateInfo ds = {};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = (no_depth || drop_depth) ? VK_FALSE : ((d.depth_control >> 1) & 1);
    ds.depthWriteEnable = (no_depth || drop_depth) ? VK_FALSE : ((d.depth_control >> 2) & 1);
    // RESTUFF_COLOR_NOZWRITE=1: colour pass (cm!=0) tests but does NOT write
    // depth (standard z-prepass). If the black vanishes, colour draws were
    // corrupting each other's depth; if it stays, the prepass depth itself
    // rejects the colour. color_mask already keys the pipeline, so no new key.
    if (getenv("RESTUFF_COLOR_NOZWRITE") && d.color_mask) ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VkCompareOp((d.depth_control >> 4) & 7);
    // M3.151 (RESTUFF_VSDEPTH_ALWAYS=<hex vs hash>): force the depth COMPARE to
    // ALWAYS for one shader's draws, leaving depth writes and every other draw
    // untouched. This is the targeted form of the depth question for the ground
    // holes -- M3.150 proved the geometry IS present at the hole (46 of 53 hole
    // pixels lie in covered cells), so the fragments must be dying downstream,
    // and depth is the first suspect. RESTUFF_NO_DEPTH=1 cannot answer it: it
    // disables depth globally, which destroys draw ordering and floods the
    // frame with backdrop. If the hole fills with this on, the depth test is
    // rejecting ground fragments; if it survives, depth is out too.
    // The VS hash already keys the pipeline, so no new key bit is needed.
    {
      static const char* dv = getenv("RESTUFF_VSDEPTH_ALWAYS");
      static const uint64_t dvh = dv ? strtoull(dv, nullptr, 16) : 0;
      if (dvh && d.vs_hash == dvh) ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }
    // M3.97 STENCIL. RB_DEPTHCONTROL (0x2200): bit0 stencil_enable, bit7
    // backface_enable, front func bits8-10 / fail 11-13 / zpass 14-16 /
    // zfail 17-19, back func bits20-22 / fail 23-25 / zpass 26-28 /
    // zfail 29-31. RB_STENCILREFMASK (0x2210): ref bits0-7, read mask 8-15,
    // write mask 16-23. Xenos CompareFunction and StencilOp both share
    // Vulkan's enum ordering, so the 3-bit fields map straight across.
    // RESTUFF_NO_STENCIL=1 restores the old always-pass behaviour for A/B.
    static const bool no_stencil = getenv("RESTUFF_NO_STENCIL") != nullptr;
    // M3.289 (RESTUFF_NO_SHAFT_FULL=1 kill-switch): the sun-shaft composite
    // (RB_DEPTHCONTROL 0x00100141, stencil func LESS -> "composite where
    // stencil > 0") tests a scene-coverage mask that on CONSOLE is non-zero
    // across the frame at shaft time -- its stamps land in EDRAM tiles the
    // main surface's stencil ALIASES. Our per-surface VkImages break that
    // aliasing, the test reads zeros, and the room-wide darkening dies: vs
    // real-360 footage of the tutorial room our mids measured 1.76x bright
    // (darks 5x) -- the July world-dim "fix" calibrated us to the SDK-emulated
    // reference, which itself lacks retail's darkening. Forcing THIS pass to
    // ALWAYS approximates "mask covers the scene"; per-pixel occlusion still
    // shapes the result (beam stays bright). The honest root fix is EDRAM
    // depth/stencil aliasing across surfaces (same class as the M3.116
    // shadow finding) -- tracked, not yet built.
    // M3.290: DEFAULT OFF — the user rejected the ALWAYS approximation with a
    // precise diagnosis: the mask this pass tests is the SHADOW SHAPE (the
    // zfail volume carve writes it), so forcing ALWAYS shadowed EVERYTHING —
    // uniform gloom, and the distinct ground shadows vanished because the
    // lit-vs-shadowed contrast is the whole point of the pass. The real defect
    // is the MASK CONTENT: our volume carve marks far too little of the
    // interior (M3.113-residual class). Kept as an opt-in diagnostic only.
    static const bool shaft_full = getenv("RESTUFF_SHAFT_FULL") != nullptr;
    const bool is_shaft = shaft_full && d.depth_control == 0x00100141u;
    ds.stencilTestEnable = (!no_stencil && (d.depth_control & 1)) ? VK_TRUE : VK_FALSE;
    if (ds.stencilTestEnable) {
      const uint32_t sr = d.stencil_ref_mask;
      VkStencilOpState front = {};
      front.failOp = VkStencilOp((d.depth_control >> 11) & 7);
      front.passOp = VkStencilOp((d.depth_control >> 14) & 7);
      front.depthFailOp = VkStencilOp((d.depth_control >> 17) & 7);
      front.compareOp = is_shaft ? VK_COMPARE_OP_ALWAYS
                                 : VkCompareOp((d.depth_control >> 8) & 7);  // M3.289
      front.compareMask = (sr >> 8) & 0xFF;
      front.writeMask = (sr >> 16) & 0xFF;
      front.reference = sr & 0xFF;
      VkStencilOpState back = front;
      if ((d.depth_control >> 7) & 1) {  // separate back-face state
        const uint32_t srb = d.stencil_ref_mask_bf;
        back.compareMask = (srb >> 8) & 0xFF;
        back.writeMask = (srb >> 16) & 0xFF;
        back.reference = srb & 0xFF;
        back.compareOp = VkCompareOp((d.depth_control >> 20) & 7);
        back.failOp = VkStencilOp((d.depth_control >> 23) & 7);
        back.passOp = VkStencilOp((d.depth_control >> 26) & 7);
        back.depthFailOp = VkStencilOp((d.depth_control >> 29) & 7);
      }
      // M3.108 RETIRED to OPT-IN (RESTUFF_VOLSTAMP=1): substituting the zfail
      // volume builders with depth-off REPLACE-255 footprint stamps made the
      // rendered shadow the whole extrusion footprint ("bounding box" -- user
      // confirmed) and leaked onto the bear. The premise was wrong: the
      // emulated pass #250 quad stamps are main-surface HUD markers, NOT the
      // volumes; the reference occ (shaft input 18922) is the compact CARVED
      // shape with the bear cut out -- i.e. the reference DOES run the zfail
      // carve. The real defect is ours alone: net -1 everted patches + net +1
      // knife-edge noise where the volume front cap re-renders the bear skin.
      static const bool no_volstamp = getenv("RESTUFF_VOLSTAMP") == nullptr;
      if (!no_volstamp && ((d.depth_control >> 7) & 1) &&
          ((d.depth_control >> 17) & 7) == 7 && ((d.depth_control >> 29) & 7) == 6) {
        static std::atomic<bool> s_vs_logged{false};
        if (!s_vs_logged.exchange(true))
          REXLOG_INFO("[native_vk] M3.108 volume-stamp substitution armed");
        ds.depthTestEnable = VK_FALSE;
        ds.depthWriteEnable = VK_FALSE;
        front.compareOp = VK_COMPARE_OP_ALWAYS;
        front.failOp = VK_STENCIL_OP_KEEP;
        front.depthFailOp = VK_STENCIL_OP_KEEP;
        front.passOp = VK_STENCIL_OP_REPLACE;
        front.reference = 0xFF;
        front.writeMask = 0x01;
        back = front;
      }
      // RESTUFF_FORCE_SHAFTMASK=1 (DIAGNOSTIC): the shaft mask builder marks
      // via zfail=DECREMENT_AND_WRAP, and that never fires (dumped stencil has
      // only the UI's 0/1, no 255). Force it to mark unconditionally so we can
      // see whether the mask GEOMETRY is right -- if the shaft then takes the
      // reference's shape, only the depth-fail condition is broken.
      // RESTUFF_SHAFTMASK_ZFLIP=1 (DIAGNOSTIC): invert the depth compare for the
      // mask builder only. The beam volume should mark where it is BEHIND scene
      // geometry; if marks appear only with the compare flipped, the depth SENSE
      // this pass sees is inverted (or the beam's own Z is wrong).
      static const bool zflip = getenv("RESTUFF_SHAFTMASK_ZFLIP") != nullptr;
      if (zflip && ((d.depth_control >> 17) & 7) == 7) {
        const VkCompareOp cur = ds.depthCompareOp;
        ds.depthCompareOp = (cur == VK_COMPARE_OP_GREATER)   ? VK_COMPARE_OP_LESS
                            : (cur == VK_COMPARE_OP_LESS)    ? VK_COMPARE_OP_GREATER
                                                             : cur;
      }
      // RESTUFF_MASK_ZNEVER=1 (DIAGNOSTIC): force the mask builder's depth
      // compare to NEVER so EVERY fragment z-fails and depthFailOp must fire.
      // If marks still do not appear, the zfail->stencil path itself is broken;
      // if they do, the depth CONTENT at mask time is the problem.
      static const bool zneverm = getenv("RESTUFF_MASK_ZNEVER") != nullptr;
      if (zneverm && ((d.depth_control >> 17) & 7) == 7) ds.depthCompareOp = VK_COMPARE_OP_NEVER;
      static const bool force_shaftmask = getenv("RESTUFF_FORCE_SHAFTMASK") != nullptr;
      if (force_shaftmask && ((d.depth_control >> 17) & 7) == 7) {
        front.compareOp = VK_COMPARE_OP_ALWAYS;
        front.passOp = VK_STENCIL_OP_REPLACE;
        front.depthFailOp = VK_STENCIL_OP_REPLACE;
        front.reference = 0xFF;
        front.writeMask = 0xFF;
        back = front;
      }
      // Log the ACTUAL built state for the mask builder (zfail=DECR_WRAP).
      // Reasoning about no_depth/small_vp is not evidence; identical results
      // under GREATER and LESS point at the depth test being OFF, in which
      // case zfail never applies and no mask can form.
      // A z-fail stencil VOLUME (front DECR, back INCR on depth-fail) only
      // counts correctly if BOTH faces rasterize. If we cull one side the
      // counting never balances and the mask collapses.
      // RESTUFF_MASK_NOCULL=1 renders the mask builder two-sided.
      // RESTUFF_WORLD_NOSWRITE=1 (DIAGNOSTIC ONLY): the main world pass writes
      // stencil 0 across the frame (passOp=REPLACE, ref=0, cmask=0, wmask=0xFF)
      // AFTER the frame-start aux mask is built, so a frame-end stencil dump can
      // never see the mask. Suppressing just that write makes the mask
      // observable. Signature-matched so nothing else is affected.
      static const bool world_nosw = getenv("RESTUFF_WORLD_NOSWRITE") != nullptr;
      if (world_nosw && front.passOp == VK_STENCIL_OP_REPLACE && front.reference == 0 &&
          front.compareMask == 0) {
        front.writeMask = 0;
        back.writeMask = 0;
      }
      // M3.102 INTERIM DEFAULT: render the shadow-volume mask builders with
      // FRONT faces culled -- back faces alone INCR on z-fail, so the mask
      // FILLS instead of self-cancelling. True Carmack's-reverse +- pairing
      // (front DECR / back INCR) produces a per-triangle sign PATCHWORK in our
      // build (net -1 exists, which consistently-oriented closed shells cannot
      // produce -> a shell subset is wound OPPOSITE the SDK's transform; the
      // holes crawl with the animation = the user's "underwater" shimmer).
      // Until that orientation root cause falls, back-only gives stable filled
      // shadows at the reference's location/intensity, at the cost of also
      // shading where hardware nets zero (volume fully behind an occluder).
      // RESTUFF_VOLMASK_PAIRED=1 restores true pairing for A/B.
      // M3.105: with the volume chain at full resolution the true +- pairing
      // is stable; back-only survives as an opt-in relic.
      static const bool mask_backonly = getenv("RESTUFF_VOLMASK_BACKONLY") != nullptr;
      if (mask_backonly && ((d.depth_control >> 17) & 7) == 7)
        rs.cullMode = VK_CULL_MODE_FRONT_BIT;
      static const bool mask_nocull = getenv("RESTUFF_MASK_NOCULL") != nullptr;
      if (mask_nocull && ((d.depth_control >> 17) & 7) == 7) rs.cullMode = VK_CULL_MODE_NONE;
      if (getenv("RESTUFF_VPLOG")) {
        static std::atomic<int> s_vp{24};
        const bool is_mask = ((d.depth_control >> 17) & 7) == 7;
        const bool is_world = ((d.depth_control >> 14) & 7) == 2 && ((d.depth_control >> 2) & 1);
        if ((is_mask || is_world) && s_vp.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[VP] {} ps={:016X} vxs={} vys={} surf_pitch={}",
                      is_mask ? "MASK " : "WORLD", d.ps_hash, _vxs,
                      std::bit_cast<float>(d.dbg_vport[2]), d.dbg_surf & 0x3FFF);
      }
      if (getenv("RESTUFF_MASKPIPE") && ((d.depth_control >> 17) & 7) == 7) {
        static std::atomic<int> s_mp{10};
        if (s_mp.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[MASKPIPE] ps={:016X} cull={} depthTest={} depthWrite={} cmpOp={} stencilTest={} "
                      "failOp={} passOp={} zfailOp={} cmpMask=0x{:02X} wMask=0x{:02X} ref={} "
                      "small_vp={} vxs={} dc=0x{:08X}",
                      d.ps_hash, int(rs.cullMode), int(ds.depthTestEnable),
                      int(ds.depthWriteEnable),
                      int(ds.depthCompareOp), int(ds.stencilTestEnable), int(front.failOp),
                      int(front.passOp), int(front.depthFailOp), front.compareMask,
                      front.writeMask, front.reference, int(small_vp), _vxs, d.depth_control);
      }
      ds.front = front;
      ds.back = back;
    }
    // M3.58: drop the corrupting near-depth write from a non-inverted-viewport
    // depth-ALWAYS draw (see the pipeline-key note above for the full rationale).
    if (neutralize_alwaysz) {
      ds.depthWriteEnable = VK_FALSE;
      static std::atomic<int> s_nz{8};
      if (s_nz.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[ALWAYSZ] neutralized depth-write: vs={:016x} ps={:016x} zscale={} zoff={}",
                    d.vs_hash, d.ps_hash, d.vport_zscale, d.vport_zoffset);
    }

    // M3.0: full RB_BLENDCONTROL translation (color_srcblend:5@0,
    // color_comb_fcn:3@5, color_destblend:5@8, alpha_srcblend:5@16,
    // alpha_comb_fcn:3@21, alpha_destblend:5@24). ONE/ZERO/ADD = blending off.
    const uint32_t bc = d.blend_control;
    const uint32_t c_src = bc & 0x1F, c_op = (bc >> 5) & 7, c_dst = (bc >> 8) & 0x1F;
    const uint32_t a_src = (bc >> 16) & 0x1F, a_op = (bc >> 21) & 7, a_dst = (bc >> 24) & 0x1F;
    VkPipelineColorBlendAttachmentState cba = {};
    // RESTUFF_NO_BLEND=1: force every translated draw opaque. Diagnostic A/B
    // for the uniform x2 deficit -- with SRC_ALPHA/1-SRC_ALPHA over many
    // overlapping layers, blending against a darker base than the reference's
    // would scale the whole scene down without altering any shader's maths.
    // If forcing opaque moves scene luminance materially, the blend/base is
    // implicated; if it barely moves, blending is not the mechanism.
    static const bool s_noblend = getenv("RESTUFF_NO_BLEND") != nullptr;
    cba.blendEnable =
        !s_noblend &&
        !(c_src == 1 && c_dst == 0 && c_op == 0 && a_src == 1 && a_dst == 0 && a_op == 0);
    cba.srcColorBlendFactor = XenosBlendFactor(c_src);
    cba.dstColorBlendFactor = XenosBlendFactor(c_dst);
    cba.colorBlendOp = XenosBlendOp(c_op);
    cba.srcAlphaBlendFactor = XenosBlendFactor(a_src);
    cba.dstAlphaBlendFactor = XenosBlendFactor(a_dst);
    cba.alphaBlendOp = XenosBlendOp(a_op);
    // RB_COLOR_MASK bits 0-3 = RGBA, same bit order as VkColorComponentFlags.
    // RESTUFF_FORCE_CMASK=1: ignore cm=0 and force RGBA writes on every draw.
    // Diagnostic to test whether cm=0 is wrongly masking off world colour.
    static const bool force_cmask = getenv("RESTUFF_FORCE_CMASK") != nullptr;
    cba.colorWriteMask = force_cmask ? 0xF : (d.color_mask & 0xF);
    VkPipelineColorBlendStateCreateInfo cb = {};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    const VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {};
    dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dyns;

    VkGraphicsPipelineCreateInfo gp = {};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = TL().pipeline_layout;
    // M3.1: translated pipelines render only inside the scene passes, which
    // now carry a depth attachment (EnsureSceneTarget runs before any draw
    // prep, so this pass exists by the time pipelines are built).
    gp.renderPass = TL().scene_rp_clear;
    // The NVIDIA SPIR-V compiler wedged the present thread for minutes on a
    // gameplay shader (core 100659: libnvidia-glvkspirv under
    // vkCreateGraphicsPipelines). BEGIN with no matching duration line in the
    // log = the offending pair.
    REXLOG_INFO("[native_vk] pipeline-create BEGIN vs={:016X} ps={:016X} prim={} dc={:02X}",
                vs_hash, ps_hash, prim, d.depth_control & 0x7E);
    const auto pc_t0 = std::chrono::steady_clock::now();
    // M3.20: build through the shared driver pipeline cache (thread-safe per
    // spec) so repeat runs and duplicate state hit the driver's compiled cache.
    const VkResult pcr =
        df.vkCreateGraphicsPipelines(device, TL().pipe_cache, 1, &gp, nullptr, &pipeline);
    if (pcr != VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
      // M3.57: perpetual `nopipe` render drops (missing floor/gate) = a FAILED
      // build cached as null. Log which pair fails + the VkResult so the state
      // combo the driver rejects is identifiable.
      static std::atomic<int> s_pf{60};
      if (s_pf.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_WARN("[native_vk] pipeline-create FAILED res={} vs={:016X} ps={:016X} prim={} "
                    "dc={:02X} su={:X} blend={:08X} cm={:X}",
                    int(pcr), vs_hash, ps_hash, prim, d.depth_control & 0x7E, d.su_mode & 7,
                    d.blend_control, d.color_mask);
    }
    const auto pc_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - pc_t0)
                           .count();
    if (pc_ms > 100)
      REXLOG_WARN("[native_vk] pipeline-create SLOW {} ms vs={:016X} ps={:016X}", pc_ms, vs_hash,
                  ps_hash);
  }
  if (vsm) df.vkDestroyShaderModule(device, vsm, nullptr);
  if (fsm) df.vkDestroyShaderModule(device, fsm, nullptr);
  // M3.154: a null publish is a PERMANENT drop for every draw that keys here
  // (cached no-retry below). It was invisible unless vkCreateGraphicsPipelines
  // itself failed -- the module-fail path above published silently. Name every
  // null so a boot-stable missing draw can be tied to its cause from the log.
  if (!pipeline) {
    static std::atomic<int> s_np{40};
    if (s_np.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_WARN("[native_vk] pipeline NULL published vs={:016X} ps={:016X} prim={} dc={:02X} "
                  "su={:X} blend={:08X} cm={:X} async={}",
                  vs_hash, ps_hash, prim, d.depth_control & 0x7E, d.su_mode & 7, d.blend_control,
                  d.color_mask, int(build_now));
  }
  if (build_now) {
    // Worker thread: tl.pipelines belongs to the present thread; publish via
    // the staging list it drains each frame. Null cached too (no retry).
    std::lock_guard<std::mutex> lk(tl.pipe_mutex);
    tl.pipe_done.emplace_back(key, pipeline);
  } else {
    tl.pipelines[key] = pipeline;  // cache even null (don't retry a broken shader)
  }
  return pipeline;
}

// M3.15: pipeline-builder worker loop. Pops requests and builds via the
// build_now path (which publishes into pipe_done for the present thread).
void PipelineWorker(vk::VulkanDevice* dev) {
  auto& tl = TL();
  for (;;) {
    TranslatedLayer::PipeReq rq;
    {
      std::unique_lock<std::mutex> lk(tl.pipe_mutex);
      tl.pipe_cv.wait(lk, [&] { return tl.pipe_quit || !tl.pipe_queue.empty(); });
      if (tl.pipe_quit) return;
      rq = tl.pipe_queue.front();
      tl.pipe_queue.pop_front();
      if (rq.prewarm && tl.prewarm_queued > 0) --tl.prewarm_queued;  // M4.0
      ++tl.pipe_active;  // M4.41: cleared below, after the build returns
    }
    renderer::RawGuestDraw fd;
    fd.vs_hash = rq.vs_hash;
    fd.ps_hash = rq.ps_hash;
    fd.prim = rq.prim;
    fd.blend_control = rq.blend_control;
    fd.color_mask = rq.color_mask;
    fd.depth_control = rq.depth_control;
    fd.stencil_ref_mask = rq.stencil_ref_mask;
    fd.stencil_ref_mask_bf = rq.stencil_ref_mask_bf;
    fd.su_mode = rq.su_mode;
    // Reconstruct the mirror parity so the rebuilt key/frontFace match.
    fd.ndc[0] = 1.0f;
    fd.ndc[1] = rq.mirror ? 1.0f : -1.0f;
    // M3.23: reconstruct the viewport xscale so the small-viewport key bit and
    // the depth-off decision match the present thread's (else the worker caches
    // under a different key and the draw re-enqueues forever = perpetual black).
    std::memcpy(&fd.dbg_vport[0], &rq.vport_xscale, 4);
    fd.vport_zscale = rq.vport_zscale;  // M3.58: keep the neutralize key consistent
    fd.wind_flip_hint = rq.wind_flip ? 1 : 0;  // M3.59: reproduce the winding decision
    fd.prim_reset_enabled = rq.prim_reset;     // M3.126: same restart key bit
    fd.dbg_surf = rq.dbg_surf;  // M4.0: M3.118 neutralize scope rides the key too
    GetOrCreateTranslatedPipeline(dev, fd, *rq.vs, *rq.ps, /*build_now=*/true);
    {  // M4.41
      std::lock_guard<std::mutex> lk(tl.pipe_mutex);
      if (tl.pipe_active > 0) --tl.pipe_active;
      if (tl.pipe_active == 0 && tl.pipe_queue.empty()) tl.pipe_idle_cv.notify_all();
    }
  }
}

// ===========================================================================
// M4.0: pipeline pre-warm -- persist every first-seen pipeline request (plus
// the guest shader ucode it references) and replay the whole set through the
// worker pool at the NEXT startup. With the driver cache warm the population
// builds in ~1s during boot; cold, it still lands during the logo/menu stretch
// instead of dropping draws (nopipe pop-in) at first encounter in a level.
// File: pipeline_prewarm.bin next to the exe (RESTUFF_PREWARM_FILE overrides;
// RESTUFF_NO_PREWARM=1 disables both recording and replay).
// Layout, all little-endian host-native:
//   u32 magic 'WPSR' (0x52535057, "RSPW" bytes), u32 version=1,
//   u32 shader_count, u32 rec_count,
//   shader_count x { u64 hash; u32 is_pixel; u32 dword_count; u32 words[]; },
//   rec_count x PrewarmRec (64 bytes each).
// Keys are NOT stored: the replay recomputes them through the normal request
// path, so env salts (RESTUFF_FORCE_SHAFTMASK etc.) always match the live run.
// ===========================================================================
constexpr uint32_t kPrewarmMagic = 0x52535057u;  // "WPSR" LE = "RSPW" on disk
constexpr uint32_t kPrewarmVersion = 1;

void SavePrewarmManifest(bool sync = false) {
  static const bool s_off = getenv("RESTUFF_NO_PREWARM") != nullptr;
  if (s_off) return;
  auto& tl = TL();
  std::vector<TranslatedLayer::PrewarmRec> recs;
  {
    std::lock_guard<std::mutex> lk(tl.pipe_mutex);
    if (!tl.prewarm_dirty) return;
    tl.prewarm_dirty = false;
    recs = tl.prewarm_log;
  }
  static std::atomic<bool> s_saving{false};
  auto writer = [recs = std::move(recs)]() {
    // M4.0b: the ucode gather runs HERE, off the present thread (it scans the
    // shader registry under its mutex and copies up to a few MB). Sources:
    // this run's registry first, then blobs carried over from the loaded
    // manifest (levels not visited this run). prewarm_loaded_shaders is
    // written once by the startup loader -- which always precedes the first
    // save -- and is read-only afterwards, so no lock is needed.
    auto& tl = TL();
    struct Blob {
      uint64_t hash;
      uint8_t is_pixel;
      std::vector<uint32_t> words;
    };
    std::vector<Blob> blobs;
    std::unordered_set<uint64_t> have;
    for (const auto& r : recs) {
      const uint64_t hs[2] = {r.vs_hash, r.ps_hash};
      for (int k = 0; k < 2; ++k) {
        if (!have.insert(hs[k]).second) continue;
        restuff::native::GuestShaderInfo info;
        std::vector<uint32_t> words;
        if (restuff::native::CopyShaderUcode(hs[k], info, words)) {
          blobs.push_back({hs[k], uint8_t(info.type & 1), std::move(words)});
        } else if (auto it = tl.prewarm_loaded_shaders.find(hs[k]);
                   it != tl.prewarm_loaded_shaders.end()) {
          blobs.push_back({hs[k], it->second.is_pixel, it->second.be_words});
        }
      }
    }
    const std::string path = PrewarmManifestPath();
    const std::string tmp = path + ".tmp";
    bool ok = false;
    if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
      const uint32_t hdr[4] = {kPrewarmMagic, kPrewarmVersion, uint32_t(blobs.size()),
                               uint32_t(recs.size())};
      ok = std::fwrite(hdr, sizeof(hdr), 1, f) == 1;
      for (const auto& b : blobs) {
        if (!ok) break;
        const uint32_t meta[2] = {b.is_pixel, uint32_t(b.words.size())};
        ok = std::fwrite(&b.hash, 8, 1, f) == 1 && std::fwrite(meta, 8, 1, f) == 1 &&
             (b.words.empty() ||
              std::fwrite(b.words.data(), 4, b.words.size(), f) == b.words.size());
      }
      if (ok && !recs.empty())
        ok = std::fwrite(recs.data(), sizeof(recs[0]), recs.size(), f) == recs.size();
      std::fclose(f);
      if (ok) {
        // M4.1: overwrite-safe rename (see the pipeline-cache writer above).
        std::error_code rn_ec;
        std::filesystem::rename(tmp, path, rn_ec);
        ok = !rn_ec;
        if (ok) {
          static std::atomic<int> s_lg{4};
          if (s_lg.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[native_vk] prewarm manifest saved: {} pipelines, {} shaders",
                        recs.size(), blobs.size());
        } else {
          REXLOG_WARN("[native_vk] prewarm manifest rename FAILED: {}", rn_ec.message());
        }
      }
      if (!ok) std::remove(tmp.c_str());
    }
    s_saving.store(false, std::memory_order_release);
  };
  // Same discipline + sync semantics as SavePipelineCache: sync callers WAIT
  // OUT an in-flight async writer (skipping would drop this run's records at
  // shutdown); an async caller that loses the single-flight race restores the
  // dirty flag so the skipped records are retried at the next trigger.
  if (sync) {
    while (s_saving.exchange(true, std::memory_order_acq_rel))
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    writer();
  } else if (s_saving.exchange(true, std::memory_order_acq_rel)) {
    std::lock_guard<std::mutex> lk(tl.pipe_mutex);
    tl.prewarm_dirty = true;
  } else {
    std::thread(std::move(writer)).detach();
  }
}

// Load the manifest and replay every record through the normal request path on
// a background thread. Call once, after SetupTranslatedLayer+EnsureSceneTarget
// (pipeline layout + the one scene render pass exist; both guest-independent).
void MaybeStartPipelinePrewarm(vk::VulkanDevice* dev) {
  static bool s_started = false;
  if (s_started) return;
  s_started = true;
  static const bool s_off = getenv("RESTUFF_NO_PREWARM") != nullptr;
  if (s_off) return;

  struct Shader {
    uint64_t hash = 0;
    uint8_t is_pixel = 0;
    std::vector<uint32_t> words;
  };
  auto shaders = std::make_shared<std::vector<Shader>>();
  auto recs = std::make_shared<std::vector<TranslatedLayer::PrewarmRec>>();
  {
    FILE* f = std::fopen(PrewarmManifestPath().c_str(), "rb");
    if (!f) return;  // first run: nothing to replay (recording still happens)
    bool ok = false;
    uint32_t hdr[4] = {};
    // Caps mirror the writer's bounds exactly: recs <= 8192 (the log cap), so
    // blobs <= 2*8192. An asymmetric reader cap would silently self-disable
    // the whole feature once a run crossed it (every save reproduces the
    // "oversized" file, every boot discards it as corrupt).
    if (std::fread(hdr, sizeof(hdr), 1, f) == 1 && hdr[0] == kPrewarmMagic &&
        hdr[1] == kPrewarmVersion && hdr[2] <= kPrewarmMaxShaders && hdr[3] <= kPrewarmMaxRecs) {
      ok = true;
      shaders->reserve(hdr[2]);
      for (uint32_t i = 0; ok && i < hdr[2]; ++i) {
        Shader s;
        uint32_t meta[2] = {};
        ok = std::fread(&s.hash, 8, 1, f) == 1 && std::fread(meta, 8, 1, f) == 1 &&
             meta[1] <= 0x10000;
        s.is_pixel = uint8_t(meta[0] & 1);  // outside the word guard: writer symmetry
        if (ok && meta[1]) {
          s.words.resize(meta[1]);
          ok = std::fread(s.words.data(), 4, meta[1], f) == meta[1];
        }
        if (ok) shaders->push_back(std::move(s));
      }
      if (ok && hdr[3]) {
        recs->resize(hdr[3]);
        ok = std::fread(recs->data(), sizeof((*recs)[0]), hdr[3], f) == hdr[3];
      }
    }
    std::fclose(f);
    if (!ok) {
      REXLOG_WARN("[native_vk] prewarm manifest unreadable/corrupt -- ignoring {}",
                  PrewarmManifestPath());
      return;
    }
  }

  auto& tl = TL();
  // Retain the loaded blobs so a re-save keeps shaders for levels not visited
  // this run (written before the replay thread or any draw uses them).
  for (const auto& s : *shaders)
    tl.prewarm_loaded_shaders.emplace(s.hash, TranslatedLayer::PrewarmShader{s.is_pixel, s.words});
  {
    // Seed the seen-set/log with the loaded records so this run's re-save is a
    // superset and the replay's own enqueues don't re-append them.
    std::lock_guard<std::mutex> lk(tl.pipe_mutex);
    for (const auto& r : *recs) {
      uint64_t rh = 1469598103934665603ull;
      const uint8_t* pb = reinterpret_cast<const uint8_t*>(&r);
      for (size_t i = 0; i < sizeof(r); ++i) rh = (rh ^ pb[i]) * 1099511628211ull;
      if (tl.prewarm_log.size() < kPrewarmMaxRecs && tl.prewarm_seen.insert(rh).second)
        tl.prewarm_log.push_back(r);
    }
  }
  REXLOG_INFO("[native_vk] prewarm: replaying {} pipelines / {} shaders from manifest",
              recs->size(), shaders->size());

  // Replay thread: translate each record's shaders (memoized, mutex-guarded
  // spc cache -- safe against the walker registering the same hashes), then
  // enqueue through the canonical request path. It never dereferences `dev`
  // (the enqueue-only path returns before any device use), so it is safe even
  // if it outlives device teardown at shutdown.
  std::thread([dev, shaders, recs] {
    auto& tl = TL();
    std::unordered_map<uint64_t, const Shader*> by_hash;
    for (const auto& s : *shaders) by_hash.emplace(s.hash, &s);
    size_t enq = 0, skip = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (const auto& r : *recs) {
      {
        std::lock_guard<std::mutex> lk(tl.pipe_mutex);
        if (tl.pipe_quit) return;
      }
      const renderer::spc::CachedShader* sh[2] = {nullptr, nullptr};
      const uint64_t hs[2] = {r.vs_hash, r.ps_hash};
      bool have_both = true;
      for (int k = 0; k < 2; ++k) {
        sh[k] = renderer::spc::GetCachedShader(hs[k]);
        if (!sh[k]) {
          if (auto it = by_hash.find(hs[k]); it != by_hash.end()) {
            // Slot k=0 is the VS: refuse a blob whose recorded stage disagrees
            // (translating ucode as the wrong stage would poison the shared
            // spc cache under this hash for the whole run).
            const bool blob_is_vertex = !it->second->is_pixel;
            if (blob_is_vertex != (k == 0)) {
              sh[k] = nullptr;
            } else {
              sh[k] = &renderer::spc::GetShader(hs[k], blob_is_vertex,
                                                it->second->words.data(),
                                                uint32_t(it->second->words.size()));
            }
          }
        }
        if (!sh[k] || !sh[k]->valid) have_both = false;
      }
      if (!have_both) {
        ++skip;
        continue;
      }
      renderer::RawGuestDraw fd;
      fd.vs_hash = r.vs_hash;
      fd.ps_hash = r.ps_hash;
      fd.prim = r.prim;
      fd.blend_control = r.blend_control;
      fd.color_mask = r.color_mask;
      fd.depth_control = r.depth_control;
      fd.stencil_ref_mask = r.stencil_ref_mask;
      fd.stencil_ref_mask_bf = r.stencil_ref_mask_bf;
      fd.su_mode = r.su_mode;
      fd.ndc[0] = 1.0f;
      fd.ndc[1] = (r.flags & 1u) ? 1.0f : -1.0f;  // mirror parity, as the worker does
      std::memcpy(&fd.dbg_vport[0], &r.vport_xscale, 4);
      fd.vport_zscale = r.vport_zscale;
      fd.wind_flip_hint = (r.flags & 2u) ? 1 : 0;
      fd.prim_reset_enabled = (r.flags & 4u) != 0;
      fd.dbg_surf = r.dbg_surf;
      GetOrCreateTranslatedPipeline(dev, fd, *sh[0], *sh[1], /*build_now=*/false,
                                    /*enqueue_only=*/true);
      ++enq;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    {  // M4.42: publish "enqueueing finished" so a drain wait can be correct.
      std::lock_guard<std::mutex> lk(tl.pipe_mutex);
      tl.prewarm_enqueue_done = true;
      tl.pipe_idle_cv.notify_all();
    }
    REXLOG_INFO("[native_vk] prewarm: {} pipelines enqueued, {} skipped, {}ms (builds continue "
                "on the worker pool)",
                enq, skip, ms);

    // M4.41 (RESTUFF_PREWARM_ONLY=1): the Proton/Fossilize-style PRE-LAUNCH
    // warm pass. Two different compiles hide behind "shader stutter":
    //   * ucode -> SPIR-V (shaderc). Device-INDEPENDENT, so it can be built
    //     once and shipped -- that is M4.40's shader_spv.bin.
    //   * SPIR-V -> GPU ISA (vkCreateGraphicsPipelines). Device- AND
    //     driver-specific, so it can NEVER be shipped; it has to be generated
    //     on the player's own machine. This is exactly why Steam ships
    //     portable Fossilize state archives and replays them locally before
    //     launch rather than shipping driver blobs.
    // pipeline_prewarm.bin is our portable archive and the loop above is the
    // replay; normally it runs in the background while you play. Under this
    // flag we instead WAIT for the pool to finish every build, flush both
    // caches, and exit -- so a launcher can warm the machine once (after
    // install, or after a driver update) and the first real run is cold-start
    // free.
    static const bool s_warm_only = getenv("RESTUFF_PREWARM_ONLY") != nullptr;
    if (s_warm_only) {
      const auto tw = std::chrono::steady_clock::now();
      {
        std::unique_lock<std::mutex> lk(tl.pipe_mutex);
        tl.pipe_idle_cv.wait(lk, [&] {
          return tl.pipe_quit ||
                 (tl.prewarm_enqueue_done && tl.pipe_queue.empty() && tl.pipe_active == 0);
        });
      }
      const auto wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - tw)
                           .count();
      SavePipelineCache(dev, /*sync=*/true);
      renderer::spc::SaveShaderSpvCache();
      REXLOG_INFO("[native_vk] M4.41 PREWARM_ONLY complete: {} pipelines built in {}ms; "
                  "pipeline_cache.bin + shader_spv.bin flushed. Exiting.",
                  enq, wms);
      // Hard exit: both caches are already written and fsync'd by their own
      // savers, and the guest is still running on other threads -- unwinding
      // through normal shutdown from here would race it for no benefit.
      std::fflush(nullptr);
      _exit(0);
    }
  }).detach();

  // M4.42: automatic warm-before-play, but ONLY when the cache cannot serve
  // this machine (first ever run, a GPU swap, or a driver update -- see the
  // header check at the cache seed). On every ordinary launch this is skipped
  // entirely and boot is exactly as before.
  //
  // We are on the PRESENT thread, on the first frame, so this holds the window
  // on its clear colour rather than letting the guest reach gameplay with
  // pipelines still compiling (which is the pop-in/stutter). SDL pumps events
  // on the main thread, so the window stays responsive. It is bounded: after
  // the timeout we give up waiting and let the remaining builds finish in the
  // background exactly as they did before this existed -- a slow machine gets
  // a worse first level, never a hang.
  static const bool s_auto_off = getenv("RESTUFF_NO_AUTO_PREWARM") != nullptr;
  if (!g_pipe_cache_stale || s_auto_off || getenv("RESTUFF_PREWARM_ONLY")) return;
  const int timeout_s = [] {
    if (const char* e = getenv("RESTUFF_AUTO_PREWARM_TIMEOUT")) {
      const int v = atoi(e);
      if (v >= 0 && v <= 3600) return v;
    }
    return 45;
  }();
  if (timeout_s == 0) return;
  REXLOG_INFO("[native_vk] M4.42 warming pipelines for this GPU/driver before play "
              "(up to {}s; RESTUFF_NO_AUTO_PREWARM=1 to skip). Run prewarm_shaders once to "
              "avoid this at launch.",
              timeout_s);
  const auto t0 = std::chrono::steady_clock::now();
  bool done;
  {
    auto& tl = TL();
    std::unique_lock<std::mutex> lk(tl.pipe_mutex);
    done = tl.pipe_idle_cv.wait_for(lk, std::chrono::seconds(timeout_s), [&] {
      return tl.pipe_quit ||
             (tl.prewarm_enqueue_done && tl.pipe_queue.empty() && tl.pipe_active == 0);
    });
  }
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  if (done) {
    // Persist immediately: the whole point is that the NEXT launch skips this.
    SavePipelineCache(dev, /*sync=*/true);
    renderer::spc::SaveShaderSpvCache();
    REXLOG_INFO("[native_vk] M4.42 warm complete in {}ms; cache saved for next launch", ms);
  } else {
    REXLOG_INFO("[native_vk] M4.42 warm hit the {}s budget ({}ms elapsed); remaining pipelines "
                "build in the background as before",
                timeout_s, ms);
  }
}

struct TransDrawRec {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorSet tex_set = VK_NULL_HANDLE;
  static constexpr uint32_t kMaxStreams = 4;
  VkDeviceSize vb_offs[kMaxStreams] = {};
  uint32_t stream_count = 0;
  VkDeviceSize ib_off = 0;
  uint32_t index_count = 0;
  uint32_t vs_ubo_off = 0, ps_ubo_off = 0;
  // M3.11/M3.14: dynamic offsets of the draw's rel-fetch stream payloads in
  // the vertex ring; 0 for draws without them (the shader then never reads).
  uint32_t vfd_off = 0, vfd2_off = 0;
  float ndc[4] = {1.0f, -1.0f, 0.0f, 0.0f};
  // M3.1: viewport depth range from PA_CL_VPORT_ZSCALE/ZOFFSET.
  float z_min = 0.0f, z_max = 1.0f;
  // M3.10: per-draw viewport rect {x,y,w,h} from PA_CL_VPORT_X/YSCALE/OFFSET +
  // PA_SC_WINDOW_OFFSET. Small guest viewports (bloom pyramid, light-buffer
  // block, tiled depth-restores) previously rendered STRETCHED to the full
  // scene because the viewport was always fullscreen.
  float vp[4] = {0.0f, 0.0f, float(1280), float(720)};
  // M3.16: per-draw guest scissor (PA_SC_WINDOW_SCISSOR_TL/BR). The HUD's
  // odometer digit strips and the multiplier bar draw the WHOLE strip and rely
  // on the scissor to show one small window of it.
  int32_t sc[4] = {0, 0, 1280, 720};  // x, y, w, h
  // M3.19: rotated-surface counter-rotation {enable, dir, 0, 0} (VS offset 96).
  float rot[4] = {0.0f, 1.0f, 0.0f, 0.0f};
  // M3.2: fragment push constants {alpha_ref, alpha_func(7=always), 0, 0}.
  float apc[4] = {0.0f, 7.0f, 0.0f, 0.0f};
  // M3.7: 256 shader bool constants (0x4900..0x4907) that the PS `if (bcond(n))`
  // guards read; pushed at fragment offset 32 as two uvec4.
  uint32_t boolc[8] = {};
  // M3.9: PARAM_GEN (SQ_PROGRAM_CNTL bit 18): the hardware injects the pixel
  // position into PS GPR param_gen_pos (SQ_CONTEXT_MISC bits 8..15) -- how
  // fullscreen post passes (DOF) get their UVs. {enable, gpr, pix_center_off, 0}
  // pushed at fragment offset 64.
  float pgen[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  // M3.9x: per-slot Xenos fetch-constant EXP_ADJUST (dword_3 bits 13..18,
  // signed) -- fetched values must be scaled by 2^exp_adjust. Packed one signed
  // byte per texture slot (slots 0-3 in [0], 4-7 in [1]) and pushed at fragment
  // offset 80 (the only free 16 bytes in the 112-byte range). We ignored this
  // field entirely, which left the sun-shaft's depth fetch unscaled => uniform
  // screen-wide occlusion instead of a localized glow = the global world dim.
  uint32_t texexp[4] = {};
  // M3.288: pc.misc (fragment offset 112). [0] bit0 = this draw's color RT is a
  // Xenos GAMMA format, so the PS must encode-on-write (see l2g in the
  // translator preamble).
  uint32_t miscpc[4] = {};
  // M3.293 scene-tone boundary: window_space mirrors the raw draw's UI flag;
  // tone_before marks the rec before which the scene RT gets the tone pass.
  bool window_space = false;
  bool tone_before = false;
  // M2.4 resolve record: copy the scene target into rt_tex[copy_dest].
  bool is_resolve = false;
  bool is_depth_resolve = false;  // RB_COPY_CONTROL copy_src_select==4: resolve
                                  // scene_depth (D32) into the dest, not color.
  // M3.58: RB_COPY_CONTROL depth_clear_enable (bit 9). The guest's EXPLICIT
  // "clear the depth EDRAM region as part of this resolve" -> the next main
  // draw segment begins a fresh depth pass and must start from cleared depth.
  // This is the true depth-pass boundary (unlike every-depth-resolve heuristics,
  // which also fire on mid-world DOF/shadow saves that must KEEP depth).
  bool depth_clears = false;
  // M3.12/M3.21: which EDRAM surface this record renders to / resolves from.
  // 0 = main scene; 1..kAuxSurfaces = aux[surf-1], assigned per frame by
  // RB_COLOR_INFO base. Previously a single bool, so every non-main base shared
  // one surface and the post passes overwrote each other + the world.
  uint32_t surf = 0;
  uint32_t dbg_copyctl = 0;  // RB_COPY_CONTROL of resolve records ([RESOLVES])
  uint64_t vs_hash = 0;  // diagnostic identity ([EAUX])
  bool is_aux() const { return surf != 0; }
  // M3.24: guest depth base (RB_DEPTH_INFO). A change signals a fresh depth
  // pass (its own EDRAM tile on hardware) -> our shared depth buffer must be
  // re-cleared there, else early-pass near-depth rejects a later pass = black.
  uint32_t di = 0;
  // M3.83: this draw writes depth (z_enable + z_write_enable). Depth resolves
  // must copy from the surface that actually WROTE the resolve's di region --
  // on hardware there is one depth EDRAM tile per RB_DEPTH_INFO base, shared
  // across color surfaces; our per-color-surface depth split is a lie the
  // resolve-source selection has to compensate for.
  bool zwrites = false;
  // M3.98: this aux record shares the MAIN depth/stencil tile (same
  // RB_DEPTH_INFO), so render it against the main depth attachment.
  bool shared_depth = false;
  // M3.99: ungated form of the same condition (surf != 0 && di == main_di);
  // drives the private-depth pre-fill regardless of the M3.98 opt-in.
  bool wants_main_depth = false;
  // M3.105: this resolve's SOURCE region was rendered at 2x (full-res volume
  // chain) and must be downsampled 2:1 into the copy dest.
  bool src_2x = false;
  uint32_t copy_dest = 0, copy_w = 0, copy_h = 0;
  uint32_t copy_rx = 0, copy_ry = 0, copy_rw = 0, copy_rh = 0;  // resolve rect
};

VkDescriptorSet ResolveTexture(vk::VulkanDevice* dev, const renderer::GuestTextureDesc& tex);

// ===========================================================================
// M2.4: offscreen scene target + resolve emulation.
// Guest draws render into tl.scene_img (not the presented image); EDRAM-copy
// draws blit the scene into rt_tex[copy_dest]; the present pass then shows
// rt_tex[front-buffer ptr] -- the same dataflow as the hardware.
// ===========================================================================

// M4.38 (B0): internal render-scale. TWO distinct spaces live here and mixing
// them corrupts every resolve:
//   * kGuestW/kGuestH -- the GUEST's framebuffer size. Resolve rects
//     (copy_rx/rw), viewport/scissor register values, rt_tex extents and guest
//     texture dimensions are ALL expressed in these, and none of them scale:
//     the guest must keep seeing a 1280x720 console.
//   * SceneW()/SceneH() -- the HOST scene target we render into, = guest * S.
// Geometry reaches us in resolution-independent clip space (the per-draw ndc[]
// push constant), so enlarging the target supersamples; each resolve then
// downscale-blits host->guest, keeping resolve contents byte-compatible with
// what the guest expects. RESTUFF_RES_SCALE=<1..4>; unset (S=1) is
// byte-identical to pre-M4.38 -- every SceneW() folds back to 1280 and every
// scale factor below to 1.
constexpr uint32_t kGuestW = 1280, kGuestH = 720;
// Resolved once, on first use, and then possibly LOWERED by the VRAM check in
// EnsureSceneTarget (which runs before any other caller). Never raised.
inline std::atomic<uint32_t>& ResScaleSlot() {
  static std::atomic<uint32_t> s{0};
  return s;
}
inline uint32_t SceneScale() {
  auto& slot = ResScaleSlot();
  uint32_t s = slot.load(std::memory_order_relaxed);
  if (s != 0) return s;
  const char* e = getenv("RESTUFF_RES_SCALE");
  uint32_t v = e ? uint32_t(strtoul(e, nullptr, 10)) : 1u;
  if (v < 1u) v = 1u;
  if (v > 4u) v = 4u;
  slot.store(v, std::memory_order_relaxed);
  return v;
}
// Colour (4 B/px) + D32S8 depth (8 B/px) for the main scene and every aux
// surface, at the given scale. This is what the scale actually costs.
inline uint64_t SceneAttachmentBytes(uint32_t s) {
  return (uint64_t(TranslatedLayer::kAuxSurfaces) + 1ull) * uint64_t(kGuestW * s) *
         uint64_t(kGuestH * s) * (4ull + 8ull);
}
inline uint32_t SceneW() { return kGuestW * SceneScale(); }
inline uint32_t SceneH() { return kGuestH * SceneScale(); }
// M3.97 STENCIL: the guest STENCIL-MASKS several passes (RB_DEPTHCONTROL bit0;
// 7% of draws test stencil, 1.7% write it). We previously used a depth-ONLY
// D32_SFLOAT attachment and never set stencilTestEnable, so every masked pass
// rendered over the whole screen -- the sun-shaft pass then smeared a flat
// ~0.36 occlusion across the frame, which IS the world dim. Attachments now
// carry stencil; sampled depth views keep the DEPTH aspect only.
constexpr VkFormat kSceneDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr VkImageAspectFlags kDepthAttAspect =
    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

bool EnsureSceneTarget(vk::VulkanDevice* dev) {
  auto& tl = TL();
  if (tl.scene_ready) return true;
  const auto& df = dev->functions();
  VkDevice device = dev->device();

  // M4.38: step the requested scale down until the attachment set fits in half
  // of device-local VRAM. Without this, an over-ambitious RESTUFF_RES_SCALE
  // fails an image allocation, EnsureSceneTarget returns false and the game
  // renders NOTHING -- a far worse outcome than quietly running at a lower
  // scale. Runs before any other SceneScale() caller, so the lowered value is
  // what the whole renderer sees.
  if (SceneScale() > 1u) {
    VkPhysicalDeviceMemoryProperties mp = {};
    dev->vulkan_instance()->functions().vkGetPhysicalDeviceMemoryProperties(
        dev->physical_device(), &mp);
    uint64_t vram = 0;
    for (uint32_t i = 0; i < mp.memoryHeapCount; ++i)
      if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
        vram += mp.memoryHeaps[i].size;
    const uint32_t want = SceneScale();
    uint32_t s = want;
    while (s > 1u && vram && SceneAttachmentBytes(s) > vram / 2) --s;
    if (s != want) {
      REXLOG_WARN(
          "[native_vk] M4.38 res_scale {} needs ~{} MB of attachments but VRAM is {} MB; "
          "clamped to {}",
          want, SceneAttachmentBytes(want) >> 20, vram >> 20, s);
      ResScaleSlot().store(s, std::memory_order_relaxed);
    }
  }

  VkImageCreateInfo img_ci = {};
  img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = vk::VulkanPresenter::kGuestOutputFormat;
  img_ci.extent = {SceneW(), SceneH(), 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk::util::CreateDedicatedAllocationImage(dev, img_ci, vk::util::MemoryPurpose::kDeviceLocal,
                                                tl.scene_img, tl.scene_mem)) {
    return false;
  }
  VkImageViewCreateInfo view_ci = {};
  view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_ci.image = tl.scene_img;
  view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_ci.format = vk::VulkanPresenter::kGuestOutputFormat;
  view_ci.subresourceRange = vk::util::InitializeSubresourceRange();
  if (df.vkCreateImageView(device, &view_ci, nullptr, &tl.scene_view) != VK_SUCCESS) return false;

  // M3.1: D32 depth attachment.
  {
    VkImageCreateInfo d_ci = img_ci;
    d_ci.format = kSceneDepthFormat;
    // TRANSFER_SRC: depth resolves copy scene_depth into a D32 rt_tex so the
    // game's depth-restore / DOF passes sample real depth, not color garbage.
    d_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT;  // M3.99: sampled by the aux depth pre-fill
    if (!vk::util::CreateDedicatedAllocationImage(
            dev, d_ci, vk::util::MemoryPurpose::kDeviceLocal, tl.depth_img, tl.depth_mem)) {
      return false;
    }
    VkImageViewCreateInfo dv_ci = {};
    dv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    dv_ci.image = tl.depth_img;
    dv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dv_ci.format = kSceneDepthFormat;
    dv_ci.subresourceRange = vk::util::InitializeSubresourceRange();
    dv_ci.subresourceRange.aspectMask = kDepthAttAspect;
    if (df.vkCreateImageView(device, &dv_ci, nullptr, &tl.depth_view) != VK_SUCCESS) return false;
  }

  // Three render passes over the same (color, depth) framebuffer:
  //   0 scene_rp_clear:    clear color + depth (virgin / RESTUFF_SCENE_CLEAR)
  //   1 scene_rp_newframe: load color (EDRAM persists), clear depth (per frame)
  //   2 scene_rp_load:     load both (segments after a mid-frame resolve)
  for (int variant = 0; variant < 5; ++variant) {
    const bool clear_color = variant == 0 || variant == 3 || variant == 4;
    const bool clear_depth = variant <= 1;
    const bool clear_stencil_only = variant == 4;
    VkAttachmentDescription atts[2] = {};
    atts[0].format = vk::VulkanPresenter::kGuestOutputFormat;
    atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[0].loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[0].initialLayout = clear_color ? VK_IMAGE_LAYOUT_UNDEFINED
                                        : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    atts[1].format = kSceneDepthFormat;
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Stencil shares the depth attachment's lifetime: DONT_CARE would discard
    // the guest's mask between passes and re-break every stencil-tested draw.
    // RESTUFF_NO_STENCIL_CLEAR=1 (DIAGNOSTIC): never clear stencil. The aux
    // mask pass runs at FRAME START and the main newframe pass clears stencil
    // straight afterwards, so an end-of-frame dump can never see the mask.
    // Keeping stencil lets the dump show whether the mask forms at all.
    static const bool s_no_sclear = getenv("RESTUFF_NO_STENCIL_CLEAR") != nullptr;
    atts[1].stencilLoadOp = ((clear_depth || clear_stencil_only) && !s_no_sclear)
                                ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                : VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].initialLayout = clear_depth ? VK_IMAGE_LAYOUT_UNDEFINED
                                        : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color_ref;
    sub.pDepthStencilAttachment = &depth_ref;
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[1].dstAccessMask =
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
        VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp_ci = {};
    rp_ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_ci.attachmentCount = 2;
    rp_ci.pAttachments = atts;
    rp_ci.subpassCount = 1;
    rp_ci.pSubpasses = &sub;
    rp_ci.dependencyCount = 2;
    rp_ci.pDependencies = deps;
    VkRenderPass* out = variant == 0   ? &tl.scene_rp_clear
                        : variant == 1 ? &tl.scene_rp_newframe
                        : variant == 2 ? &tl.scene_rp_load
                        : variant == 3 ? &tl.scene_rp_clearcolor
                                       : &tl.scene_rp_clearcolor_cs;
    if (df.vkCreateRenderPass(dev->device(), &rp_ci, nullptr, out) != VK_SUCCESS) return false;
  }

  const VkImageView fb_views[2] = {tl.scene_view, tl.depth_view};
  VkFramebufferCreateInfo fb_ci = {};
  fb_ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fb_ci.renderPass = tl.scene_rp_clear;
  fb_ci.attachmentCount = 2;
  fb_ci.pAttachments = fb_views;
  fb_ci.width = SceneW();
  fb_ci.height = SceneH();
  fb_ci.layers = 1;
  if (df.vkCreateFramebuffer(device, &fb_ci, nullptr, &tl.scene_fb) != VK_SUCCESS) return false;

  // M3.12/M3.21: N aux EDRAM surfaces (same formats/passes) for draws on
  // RB_COLOR_INFO bases other than the frame's main base -- the half-res
  // light-buffer/CoC block and each DOF/bloom post target.
  for (uint32_t s = 0; s < TranslatedLayer::kAuxSurfaces; ++s) {
    auto& ax = tl.aux[s];
    if (!vk::util::CreateDedicatedAllocationImage(dev, img_ci,
                                                  vk::util::MemoryPurpose::kDeviceLocal, ax.img,
                                                  ax.mem)) {
      return false;
    }
    VkImageViewCreateInfo av_ci = view_ci;
    av_ci.image = ax.img;
    if (df.vkCreateImageView(device, &av_ci, nullptr, &ax.view) != VK_SUCCESS) return false;
    VkImageCreateInfo ad_ci = img_ci;
    ad_ci.format = kSceneDepthFormat;
    ad_ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!vk::util::CreateDedicatedAllocationImage(dev, ad_ci, vk::util::MemoryPurpose::kDeviceLocal,
                                                  ax.depth_img, ax.depth_mem)) {
      return false;
    }
    VkImageViewCreateInfo adv_ci = {};
    adv_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    adv_ci.image = ax.depth_img;
    adv_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    adv_ci.format = kSceneDepthFormat;
    adv_ci.subresourceRange = vk::util::InitializeSubresourceRange();
    adv_ci.subresourceRange.aspectMask = kDepthAttAspect;
    if (df.vkCreateImageView(device, &adv_ci, nullptr, &ax.depth_view) != VK_SUCCESS) return false;
    const VkImageView aux_views[2] = {ax.view, ax.depth_view};
    VkFramebufferCreateInfo afb_ci = fb_ci;
    afb_ci.pAttachments = aux_views;
    if (df.vkCreateFramebuffer(device, &afb_ci, nullptr, &ax.fb) != VK_SUCCESS) return false;
    const VkImageView aux_shared[2] = {ax.view, tl.depth_view};
    afb_ci.pAttachments = aux_shared;
    if (df.vkCreateFramebuffer(device, &afb_ci, nullptr, &ax.fb_shared_depth) != VK_SUCCESS)
      return false;
  }

  // M3.99: aux-depth pre-fill machinery. Non-fatal on failure (fill_pipeline
  // stays null and the frame renders exactly as before the feature existed).
  {
    VkImageViewCreateInfo sv = {};
    sv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    sv.image = tl.depth_img;
    sv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    sv.format = kSceneDepthFormat;
    sv.subresourceRange = vk::util::InitializeSubresourceRange();
    sv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;  // sampled views: ONE aspect
    if (df.vkCreateImageView(device, &sv, nullptr, &tl.depth_sample_view) == VK_SUCCESS) {
      VkDescriptorPoolSize fps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  renderer::kMaxTexSlots};
      VkDescriptorPoolCreateInfo fdp = {};
      fdp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      fdp.maxSets = 1;
      fdp.poolSizeCount = 1;
      fdp.pPoolSizes = &fps;
      if (df.vkCreateDescriptorPool(device, &fdp, nullptr, &tl.fill_pool) == VK_SUCCESS) {
        VkDescriptorSetAllocateInfo fai = {};
        fai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        fai.descriptorPool = tl.fill_pool;
        fai.descriptorSetCount = 1;
        fai.pSetLayouts = &tl.tex_layout;
        df.vkAllocateDescriptorSets(device, &fai, &tl.fill_set);
      }
      // The scale (*2) is this title's fog-group ratio: those passes run a
      // 640x360 viewport against the 1280x720 scene depth. gl_FragCoord is at
      // pixel centers, so the ivec2 cast floors to the pixel index.
      static const char* kFillVS =
          "#version 450\n"
          "void main(){ vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
          "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
      // M3.104: CONSERVATIVE decimation -- the NEAREST depth of the 2x2 block
      // (max under reversed-Z). Point-picking left volume faces that hug
      // receiver surfaces (the shadow volume's under-floor exit cap) in
      // razor-margin z-fail territory; the comparisons flipped per pixel as
      // the animation moved = salt-and-pepper stencil nets + the crawling
      // shimmer. Nearest-of-block makes "behind the surface" decisive.
      // M3.117: the aux prefill's DEFAULT is the EDRAM pitch-ALIAS fold -- the
      // hardware truth. The volume pass names the MAIN depth tile but reads it
      // at ITS OWN surface pitch (RB_SURFACE_INFO: volume pass 0x...2D0 =
      // pitch 720; main 0x...550 = pitch 1360). EDRAM stores 80x16-sample
      // tiles linearly, so aux texel (x,y) lands in linear tile
      // (y>>4)*(720/80=9) + x/80, which the main layout places at tile
      // R=lin/17, C=lin%17 (1360/80=17) -> main texel (C*80+x%80,
      // R*16+(y&15)). Main columns >= 1280 are pitch padding (stale EDRAM on
      // hardware) -> far (0.0, reversed-Z) = z-fail = marked, matching the
      // fat bridged reference mask (its bear cutout is eaten by this fold --
      // OUR clean depth kept the cutout and detached the head shadow).
      // RESTUFF_FILL_MODE=min|max selects the old 2x2 collapses for A/B.
      // M3.117 first fold model FAILED (coverage 3.3k, worse): the linear
      // 9/17-tile fold maps the whole aux view onto main rows 0..207, and the
      // top-third depth shrinks the marks. Suspected cause: the main surface
      // is 4xMSAA, so EDRAM tiles hold SAMPLES (80x16), not pixels -- the
      // real mapping needs the SDK's own EDRAM tile addressing (plugin
      // resolve code). Until modeled right, alias stays OPT-IN.
      static const char* s_fill_env = getenv("RESTUFF_FILL_MODE");
      static const int s_fill_mode = !s_fill_env                  ? 1
                                     : !strcmp(s_fill_env, "alias") ? 0
                                     : !strcmp(s_fill_env, "min")   ? 2
                                                                    : 1;  // max
      static const char* kFillFSAlias =
          "#version 450\n"
          "layout(set=1, binding=0) uniform sampler2D tex_0;\n"
          "void main(){ ivec2 p = ivec2(gl_FragCoord.xy);\n"
          "  int lin = (p.y >> 4) * 9 + (p.x / 80);\n"
          "  ivec2 mp = ivec2((lin % 17) * 80 + (p.x % 80), (lin / 17) * 16 + (p.y & 15));\n"
          "  ivec2 sz = textureSize(tex_0, 0);\n"
          "  gl_FragDepth = (mp.x < sz.x && mp.y < sz.y) ? texelFetch(tex_0, mp, 0).r : 0.0;\n"
          "}\n";
      static const char* kFillFSMin =
          "#version 450\n"
          "layout(set=1, binding=0) uniform sampler2D tex_0;\n"
          "void main(){ ivec2 b = ivec2(gl_FragCoord.xy) * 2;\n"
          "  float d0 = texelFetch(tex_0, b, 0).r;\n"
          "  float d1 = texelFetch(tex_0, b + ivec2(1,0), 0).r;\n"
          "  float d2 = texelFetch(tex_0, b + ivec2(0,1), 0).r;\n"
          "  float d3 = texelFetch(tex_0, b + ivec2(1,1), 0).r;\n"
          "  gl_FragDepth = min(min(d0,d1), min(d2,d3)); }\n";
      static const char* kFillFSMax =
          "#version 450\n"
          "layout(set=1, binding=0) uniform sampler2D tex_0;\n"
          "void main(){ ivec2 b = ivec2(gl_FragCoord.xy) * 2;\n"
          "  float d0 = texelFetch(tex_0, b, 0).r;\n"
          "  float d1 = texelFetch(tex_0, b + ivec2(1,0), 0).r;\n"
          "  float d2 = texelFetch(tex_0, b + ivec2(0,1), 0).r;\n"
          "  float d3 = texelFetch(tex_0, b + ivec2(1,1), 0).r;\n"
          "  gl_FragDepth = max(max(d0,d1), max(d2,d3)); }\n";
      const char* kFillFS = s_fill_mode == 1 ? kFillFSMax
                            : s_fill_mode == 2 ? kFillFSMin
                                               : kFillFSAlias;
      auto fvs = renderer::spc::CompileGlsl(kFillVS, /*is_vertex=*/true);
      auto ffs = renderer::spc::CompileGlsl(kFillFS, /*is_vertex=*/false);
      if (!fvs.empty() && !ffs.empty() && tl.fill_set != VK_NULL_HANDLE) {
        VkShaderModule fvm = vk::util::CreateShaderModule(dev, fvs.data(), fvs.size() * 4);
        VkShaderModule ffm = vk::util::CreateShaderModule(dev, ffs.data(), ffs.size() * 4);
        if (fvm && ffm) {
          VkPipelineShaderStageCreateInfo st[2] = {};
          st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
          st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
          st[0].module = fvm;
          st[0].pName = "main";
          st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
          st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
          st[1].module = ffm;
          st[1].pName = "main";
          VkPipelineVertexInputStateCreateInfo vi = {};
          vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
          VkPipelineInputAssemblyStateCreateInfo ia = {};
          ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
          ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
          VkPipelineViewportStateCreateInfo vp = {};
          vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
          vp.viewportCount = 1;
          vp.scissorCount = 1;
          VkPipelineRasterizationStateCreateInfo rs = {};
          rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
          rs.polygonMode = VK_POLYGON_MODE_FILL;
          rs.cullMode = VK_CULL_MODE_NONE;
          rs.lineWidth = 1.0f;
          VkPipelineMultisampleStateCreateInfo ms = {};
          ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
          ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
          VkPipelineDepthStencilStateCreateInfo ds = {};
          ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
          ds.depthTestEnable = VK_TRUE;
          ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
          ds.depthWriteEnable = VK_TRUE;
          VkPipelineColorBlendAttachmentState cba = {};
          cba.colorWriteMask = 0;  // depth-only fill; colour untouched
          VkPipelineColorBlendStateCreateInfo cb = {};
          cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
          cb.attachmentCount = 1;
          cb.pAttachments = &cba;
          const VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
          VkPipelineDynamicStateCreateInfo dyn = {};
          dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
          dyn.dynamicStateCount = 2;
          dyn.pDynamicStates = dyns;
          VkGraphicsPipelineCreateInfo gp = {};
          gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
          gp.stageCount = 2;
          gp.pStages = st;
          gp.pVertexInputState = &vi;
          gp.pInputAssemblyState = &ia;
          gp.pViewportState = &vp;
          gp.pRasterizationState = &rs;
          gp.pMultisampleState = &ms;
          gp.pDepthStencilState = &ds;
          gp.pColorBlendState = &cb;
          gp.pDynamicState = &dyn;
          gp.layout = tl.pipeline_layout;
          gp.renderPass = tl.scene_rp_clear;  // compatible with all variants
          df.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr,
                                       &tl.fill_pipeline);
        }
        if (fvm) df.vkDestroyShaderModule(device, fvm, nullptr);
        if (ffm) df.vkDestroyShaderModule(device, ffm, nullptr);
      }
      REXLOG_INFO("[native_vk] M3.99 aux-depth pre-fill {}",
                  tl.fill_pipeline ? "ready" : "UNAVAILABLE");
    }
  }

  // M4.4: single-pass depth-resolve machinery (RESTUFF_DEPTH_FILL=1). Clones
  // the M3.99 shape: a depth-only render pass whose one D32S8 attachment stays
  // in ATTACHMENT_OPTIMAL (explicit barriers keep doing the transitions, so
  // the resolved_once oldLayout logic in RecordResolve is preserved verbatim),
  // plus a fullscreen-triangle pipeline whose FS reproduces the bounce blit's
  // NEAREST texel parity: a 2:1 NEAREST blit picks src texel 2*dst+1
  // ((dst+0.5)*2 floored), and gl_FragCoord is framebuffer-absolute so the
  // rect origin mapping (dest rx+u -> src 2rx+2u+1) falls out for free.
  // Non-fatal on failure: null pipeline -> the M3.115 bounce keeps running.
  // M4.34: DEFAULT ON (user call, Aug 27). One depth-only pass replaces the
  // M3.115 triple-move bounce, so each depth resolve costs one write of the
  // rect instead of three moves of it -- the resolve category measured 3.3ms
  // of the Ally's 16ms GPU frame. RESTUFF_NO_DEPTH_FILL=1 restores the bounce.
  if (getenv("RESTUFF_NO_DEPTH_FILL") == nullptr && tl.fill_set != VK_NULL_HANDLE) {
    VkAttachmentDescription at = {};
    at.format = kSceneDepthFormat;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every rect pixel is written
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;  // bounce moved depth only too
    at.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference dref = {0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rpci = {};
    rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpci.attachmentCount = 1;
    rpci.pAttachments = &at;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sp;
    if (df.vkCreateRenderPass(device, &rpci, nullptr, &tl.depth_fill_rp) == VK_SUCCESS) {
      static const char* kDfVS =
          "#version 450\n"
          "void main(){ vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
          "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
      static const char* kDfFS =
          "#version 450\n"
          "layout(set=1, binding=0) uniform sampler2D tex_0;\n"
          "void main(){ ivec2 p = ivec2(gl_FragCoord.xy) * 2 + ivec2(1,1);\n"
          "  ivec2 sz = textureSize(tex_0, 0);\n"
          "  p = min(p, sz - 1);\n"
          "  gl_FragDepth = texelFetch(tex_0, p, 0).r; }\n";
      auto dvs = renderer::spc::CompileGlsl(kDfVS, /*is_vertex=*/true);
      auto dfs = renderer::spc::CompileGlsl(kDfFS, /*is_vertex=*/false);
      if (!dvs.empty() && !dfs.empty()) {
        VkShaderModule dvm = vk::util::CreateShaderModule(dev, dvs.data(), dvs.size() * 4);
        VkShaderModule dfm = vk::util::CreateShaderModule(dev, dfs.data(), dfs.size() * 4);
        if (dvm && dfm) {
          VkPipelineShaderStageCreateInfo st[2] = {};
          st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
          st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
          st[0].module = dvm;
          st[0].pName = "main";
          st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
          st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
          st[1].module = dfm;
          st[1].pName = "main";
          VkPipelineVertexInputStateCreateInfo vi = {};
          vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
          VkPipelineInputAssemblyStateCreateInfo ia = {};
          ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
          ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
          VkPipelineViewportStateCreateInfo vp = {};
          vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
          vp.viewportCount = 1;
          vp.scissorCount = 1;
          VkPipelineRasterizationStateCreateInfo rs = {};
          rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
          rs.polygonMode = VK_POLYGON_MODE_FILL;
          rs.cullMode = VK_CULL_MODE_NONE;
          rs.lineWidth = 1.0f;
          VkPipelineMultisampleStateCreateInfo ms = {};
          ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
          ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
          VkPipelineDepthStencilStateCreateInfo ds = {};
          ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
          ds.depthTestEnable = VK_TRUE;
          ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
          ds.depthWriteEnable = VK_TRUE;
          VkPipelineColorBlendStateCreateInfo cb = {};
          cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
          cb.attachmentCount = 0;  // depth-only pass: no color attachments
          const VkDynamicState dyns[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
          VkPipelineDynamicStateCreateInfo dyn = {};
          dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
          dyn.dynamicStateCount = 2;
          dyn.pDynamicStates = dyns;
          VkGraphicsPipelineCreateInfo gp = {};
          gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
          gp.stageCount = 2;
          gp.pStages = st;
          gp.pVertexInputState = &vi;
          gp.pInputAssemblyState = &ia;
          gp.pViewportState = &vp;
          gp.pRasterizationState = &rs;
          gp.pMultisampleState = &ms;
          gp.pDepthStencilState = &ds;
          gp.pColorBlendState = &cb;
          gp.pDynamicState = &dyn;
          gp.layout = tl.pipeline_layout;
          gp.renderPass = tl.depth_fill_rp;
          df.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr,
                                       &tl.depth_fill_pipeline);
        }
        if (dvm) df.vkDestroyShaderModule(device, dvm, nullptr);
        if (dfm) df.vkDestroyShaderModule(device, dfm, nullptr);
      }
    }
    REXLOG_INFO("[native_vk] M4.4 depth-fill resolve {}",
                tl.depth_fill_pipeline ? "ready" : "UNAVAILABLE (bounce fallback)");
  }

  // Bounce buffer for scene -> resolve-dest copies.
  if (!vk::util::CreateDedicatedAllocationBuffer(
          dev, VkDeviceSize(SceneW()) * SceneH() * 4,
          VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          vk::util::MemoryPurpose::kDeviceLocal, tl.resolve_buf, tl.resolve_buf_mem, nullptr,
          nullptr)) {
    return false;
  }
  // M3.89: host-visible writeback capture buffer (8MB: several small resolves
  // per frame at 4B/px).
  if (!vk::util::CreateDedicatedAllocationBuffer(
          dev, VkDeviceSize(8) << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          vk::util::MemoryPurpose::kUpload, tl.wb_buf, tl.wb_mem, &tl.wb_mem_type, nullptr)) {
    REXLOG_ERROR("[native_vk] M3.89 writeback buffer create failed (writeback disabled)");
  } else if (dev->functions().vkMapMemory(device, tl.wb_mem, 0, VK_WHOLE_SIZE, 0, &tl.wb_ptr) !=
             VK_SUCCESS) {
    tl.wb_ptr = nullptr;
    REXLOG_ERROR("[native_vk] M3.89 writeback buffer map failed (writeback disabled)");
  }

  // M3.293: scene-tone pass objects. The tone is a scene property (fitted on
  // gameplay content vs the user's endorsed reference), applied to the scene
  // RT at the 3D->UI boundary inside the frame -- NEVER at present, and never
  // by frame-level heuristics (a draw-count gate fried episode select; a
  // global present curve fried the title). Skipped entirely under
  // RESTUFF_NO_TONE=1.
  if (getenv("RESTUFF_SCENE_TONE")) {  // M3.293 parked opt-in (see boundary note)
    VkImageCreateInfo tci = {};
    tci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    tci.imageType = VK_IMAGE_TYPE_2D;
    tci.format = vk::VulkanPresenter::kGuestOutputFormat;
    tci.extent = {SceneW(), SceneH(), 1};
    tci.mipLevels = 1;
    tci.arrayLayers = 1;
    tci.samples = VK_SAMPLE_COUNT_1_BIT;
    tci.tiling = VK_IMAGE_TILING_OPTIMAL;
    tci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vk::util::CreateDedicatedAllocationImage(dev, tci, vk::util::MemoryPurpose::kDeviceLocal,
                                                 tl.tone_img, tl.tone_mem)) {
      VkImageViewCreateInfo tvi = {};
      tvi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      tvi.image = tl.tone_img;
      tvi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      tvi.format = vk::VulkanPresenter::kGuestOutputFormat;
      tvi.subresourceRange = vk::util::InitializeSubresourceRange();
      df.vkCreateImageView(device, &tvi, nullptr, &tl.tone_view);
    }
    VkDescriptorSetLayoutBinding tb = {};
    tb.binding = 0;
    tb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tb.descriptorCount = 1;
    tb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo tlci = {};
    tlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tlci.bindingCount = 1;
    tlci.pBindings = &tb;
    df.vkCreateDescriptorSetLayout(device, &tlci, nullptr, &tl.tone_set_layout);
    if (tl.tone_set_layout) {
      VkPipelineLayoutCreateInfo tpl = {};
      tpl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      tpl.setLayoutCount = 1;
      tpl.pSetLayouts = &tl.tone_set_layout;
      df.vkCreatePipelineLayout(device, &tpl, nullptr, &tl.tone_pl_layout);
    }
    if (tl.tone_view && tl.tone_pl_layout) {
      VkDescriptorPoolSize tps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
      VkDescriptorPoolCreateInfo tdp = {};
      tdp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
      tdp.maxSets = 1;
      tdp.poolSizeCount = 1;
      tdp.pPoolSizes = &tps;
      if (df.vkCreateDescriptorPool(device, &tdp, nullptr, &tl.tone_pool) == VK_SUCCESS) {
        VkDescriptorSetAllocateInfo tai = {};
        tai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        tai.descriptorPool = tl.tone_pool;
        tai.descriptorSetCount = 1;
        tai.pSetLayouts = &tl.tone_set_layout;
        if (df.vkAllocateDescriptorSets(device, &tai, &tl.tone_set) == VK_SUCCESS) {
          auto& dl = DL();  // borrowed provider samplers live on the 2D layer
          VkDescriptorImageInfo tii = {dl.sampler_nearest ? dl.sampler_nearest : dl.sampler,
                                       tl.tone_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
          VkWriteDescriptorSet tw = {};
          tw.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
          tw.dstSet = tl.tone_set;
          tw.descriptorCount = 1;
          tw.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
          tw.pImageInfo = &tii;
          df.vkUpdateDescriptorSets(device, 1, &tw, 0, nullptr);
        }
      }
    }
    if (tl.tone_set) {
      float tp = 1.377f, tg = 1.209f;
      if (const char* e = getenv("RESTUFF_TONE_POWER")) tp = float(atof(e));
      if (const char* e = getenv("RESTUFF_TONE_GAIN")) tg = float(atof(e));
      static const char* kToneVS =
          "#version 450\n"
          "layout(location=0) out vec2 v_uv;\n"
          "void main(){ vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);\n"
          "  v_uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
      char tfs[640];
      std::snprintf(tfs, sizeof(tfs),
          "#version 450\n"
          "layout(set=0, binding=0) uniform sampler2D srcTex;\n"
          "layout(location=0) in vec2 v_uv;\n"
          "layout(location=0) out vec4 o;\n"
          "void main(){ vec3 c = texture(srcTex, v_uv).rgb;\n"
          "  o = vec4(pow(min(c * %f, vec3(1.0)), vec3(%f)), 1.0); }\n",
          double(tg), double(tp));
      auto tvs = renderer::spc::CompileGlsl(kToneVS, /*is_vertex=*/true);
      auto tfsb = renderer::spc::CompileGlsl(tfs, /*is_vertex=*/false);
      if (!tvs.empty() && !tfsb.empty()) {
        VkShaderModule vm = vk::util::CreateShaderModule(dev, tvs.data(), tvs.size() * 4);
        VkShaderModule fm = vk::util::CreateShaderModule(dev, tfsb.data(), tfsb.size() * 4);
        if (vm && fm) {
          VkPipelineShaderStageCreateInfo st[2] = {};
          st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
          st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
          st[0].module = vm;
          st[0].pName = "main";
          st[1] = st[0];
          st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
          st[1].module = fm;
          VkPipelineVertexInputStateCreateInfo vi = {};
          vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
          VkPipelineInputAssemblyStateCreateInfo ia = {};
          ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
          ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
          VkViewport vp = {0.f, 0.f, float(SceneW()), float(SceneH()), 0.f, 1.f};
          VkRect2D sc = {{0, 0}, {SceneW(), SceneH()}};
          VkPipelineViewportStateCreateInfo vps = {};
          vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
          vps.viewportCount = 1;
          vps.pViewports = &vp;
          vps.scissorCount = 1;
          vps.pScissors = &sc;
          VkPipelineRasterizationStateCreateInfo rs = {};
          rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
          rs.lineWidth = 1.0f;
          VkPipelineMultisampleStateCreateInfo ms = {};
          ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
          ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
          VkPipelineDepthStencilStateCreateInfo ds = {};
          ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
          VkPipelineColorBlendAttachmentState ba = {};
          ba.colorWriteMask = 0xF;
          VkPipelineColorBlendStateCreateInfo cb = {};
          cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
          cb.attachmentCount = 1;
          cb.pAttachments = &ba;
          VkGraphicsPipelineCreateInfo gp = {};
          gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
          gp.stageCount = 2;
          gp.pStages = st;
          gp.pVertexInputState = &vi;
          gp.pInputAssemblyState = &ia;
          gp.pViewportState = &vps;
          gp.pRasterizationState = &rs;
          gp.pMultisampleState = &ms;
          gp.pDepthStencilState = &ds;
          gp.pColorBlendState = &cb;
          gp.layout = tl.tone_pl_layout;
          gp.renderPass = tl.scene_rp_load;
          df.vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr,
                                       &tl.tone_pipeline);
        }
        if (vm) df.vkDestroyShaderModule(device, vm, nullptr);
        if (fm) df.vkDestroyShaderModule(device, fm, nullptr);
      }
    }
    REXLOG_INFO("[native_vk] M3.293 scene-tone pass {}",
                tl.tone_pipeline ? "ready" : "UNAVAILABLE");
  }

  tl.scene_ready = true;
  // M4.38: at S>1 this is the dominant VRAM consumer -- (1 main + kAuxSurfaces)
  // colour attachments at 4 B/px plus as many D32S8 depth attachments at 8 B/px,
  // all scaled by S^2. Log it: 16 aux surfaces make S=2 roughly 0.7 GB, which is
  // the number that decides whether a given machine can afford a scale at all.
  REXLOG_INFO("[native_vk] M2.4 scene target ready ({}x{}, res_scale={}, ~{} MB attachments)",
              SceneW(), SceneH(), SceneScale(), SceneAttachmentBytes(SceneScale()) >> 20);
  return true;
}

// Resolve-destination image: sampled by later compose draws (via ResolveTexture)
// and by the present pass. Fully overwritten by each blit, so its previous
// layout is discardable.
TexEntry* GetOrCreateRtTex(vk::VulkanDevice* dev, uint32_t dest, uint32_t w, uint32_t h,
                           bool is_depth = false) {
  auto& tl = TL();
  auto& dl = DL();
  const uint64_t rtkey = RtTexKey(dest, is_depth);
  auto it = tl.rt_tex.find(rtkey);
  if (it != tl.rt_tex.end()) {
    // M3.1: the guest reuses front-buffer addresses at different sizes across
    // scene transitions (loading->cutscene->level). Copying a new-size resolve
    // into the old-size image is an out-of-bounds GPU transfer -- recreate.
    // Also recreate if the color/depth kind flipped (format differs).
    if (it->second.width != w || it->second.height != h || it->second.is_depth != is_depth) {
      dl.deferred_destroy.push_back(it->second);
      ++dl.tex_retire_epoch;  // M4.5: epoch (see decl)
      tl.rt_tex.erase(it);
    } else {
      return &it->second;
    }
  }
  const auto& df = dev->functions();
  VkDevice device = dev->device();
  TexEntry t;
  t.is_depth = is_depth;
  const VkImageAspectFlags aspect =
      is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  const VkFormat fmt = is_depth ? kSceneDepthFormat : vk::VulkanPresenter::kGuestOutputFormat;
  VkImageCreateInfo img_ci = {};
  img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img_ci.imageType = VK_IMAGE_TYPE_2D;
  img_ci.format = fmt;
  img_ci.extent = {w, h, 1};
  img_ci.mipLevels = 1;
  img_ci.arrayLayers = 1;
  img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
  img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  img_ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  // M4.4: depth resolve targets are also renderable -- the single-pass
  // depth-fill resolve draws into them instead of the triple-move bounce.
  if (is_depth) img_ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (!vk::util::CreateDedicatedAllocationImage(dev, img_ci, vk::util::MemoryPurpose::kDeviceLocal,
                                                t.image, t.memory)) {
    return nullptr;
  }
  VkImageViewCreateInfo view_ci = {};
  view_ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_ci.image = t.image;
  view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_ci.format = fmt;
  view_ci.subresourceRange = vk::util::InitializeSubresourceRange();
  view_ci.subresourceRange.aspectMask = aspect;
  if (df.vkCreateImageView(device, &view_ci, nullptr, &t.view) != VK_SUCCESS) return nullptr;
  t.set = AcquireTexSet(dev);
  if (t.set == VK_NULL_HANDLE) return nullptr;
  VkDescriptorImageInfo dii = {is_depth ? dl.sampler_nearest : dl.sampler, t.view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet wds = {};
  wds.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  wds.dstSet = t.set;
  wds.dstBinding = 0;
  wds.descriptorCount = 1;
  wds.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wds.pImageInfo = &dii;
  df.vkUpdateDescriptorSets(device, 1, &wds, 0, nullptr);
  t.width = w;
  t.height = h;
  REXLOG_INFO("[native_vk] M2.4 resolve target 0x{:08X} {}x{}", dest, w, h);
  return &tl.rt_tex.emplace(rtkey, t).first->second;
}

// Resolve-execution telemetry (present thread): executed vs guarded-out
// (rect out of bounds / degenerate), surfaced in the present alive line.
static std::atomic<uint64_t> s_res_exec{0}, s_res_guard{0};

// RESTUFF_DUMP_AFTER_AUX=1: record the scene/stencil dump right after the
// frame-start AUX group instead of at frame end. The world pass writes stencil
// 0 across the frame afterwards, so an end-of-frame dump can NEVER show the
// shaft mask -- this captures it at the moment the shaft actually reads it.
bool RecordSceneDump(vk::VulkanDevice* dev, VkCommandBuffer cmd, bool force = false);
bool g_dump_taken_this_frame = false;
int g_aux_trans = 0;  // aux->main transitions seen this frame (dump targeting)
uint64_t g_frame_no = 0;       // frames processed (RESTUFF_DUMP_EACH_AUX targeting)
// M3.154c: RESTUFF_VSHIT hands its covering draws to the submit loop so their
// per-draw FATE (drop reason vs recorded) logs unconditionally. RENDERDROP
// cannot do this -- its seen>900 gate never sees early-index draws. Matching
// is by ELEMENT POINTER, not frame index: the submit loop iterates draws in
// surface-partitioned order, so indices do not line up (v1 logged draws of a
// different shader). tl.frame is immutable across both loops in a frame, so
// pointers are exact. Present-thread only, no locking.
std::vector<const void*> g_vshit_cover_ptrs;
bool g_vshit_probe_frame = false;
uint64_t g_pms_resolve_us = 0;  // M3.124: per-frame resolve time (present thread)
// M3.133: wall time between the END of one present cycle and the START of the
// next -- the only stretch of the frame never instrumented, and (per the
// M3.132 histogram refuting vsync) where the missing ~40ms must live.
std::chrono::steady_clock::time_point g_pms_last_end{};
uint64_t g_pms_outside_us = 0;
uint64_t g_pms_resolve_n = 0;
// M3.135: with the fixed 16ms sleep gone (M3.134), OUTSIDE is 25-40ms on the
// user's machine -- BIGGER than the sleep ever was, and now the single largest
// slice of the frame. It is not one thing, so split it into the four stretches
// it can physically be. Boundaries, in order around the loop:
//   g_pms_last_end -> cb_end        our own post-fence cleanup (staging retire,
//                                   deferred texture destroy, alive log)
//   cb_end         -> rgo_ret       the SDK presenter's work AFTER our callback
//                                   returns: its ImGui pass, the guest-output
//                                   blit into the swapchain, its own submit and
//                                   vkQueuePresentKHR
//   rgo_ret        -> rgo_call      our present loop: the pacing sleep and the
//                                   skip-redraw poll, nothing else
//   rgo_call       -> _pms_t0       ConsumeReadyFrame plus the presenter's work
//                                   BEFORE it calls us: swapchain image acquire
// Whichever dominates decides the next move, and they are mutually exclusive:
// an acquire stall is a driver/compositor fight, a post stall is the
// presenter's paint (ours to shrink), a loop stall is our own pacing.
std::chrono::steady_clock::time_point g_pms_cb_end{};
std::chrono::steady_clock::time_point g_pms_rgo_ret{};
std::chrono::steady_clock::time_point g_pms_rgo_call{};
std::chrono::steady_clock::time_point g_pms_t0{};  // == _pms_t0, for the pre split
std::chrono::steady_clock::time_point g_pms_cb_start{};  // M3.135b: callback entry
bool g_eaux_this_frame = false;

// Shared lazy device-proc lookup (the SDK function table lacks vkCmdBlitImage /
// vkCmdCopyImage; the provider loads the Vulkan loader privately, so grab the
// already-loaded loader ourselves). Platform split is only HOW the loader
// module is found: dlopen("libvulkan.so.1") vs GetModuleHandle/LoadLibrary
// ("vulkan-1.dll"); everything past vkGetDeviceProcAddr is identical.
static PFN_vkGetDeviceProcAddr LoaderGdpa() {
#ifdef _WIN32
  HMODULE vh = GetModuleHandleA("vulkan-1.dll");
  if (!vh) vh = LoadLibraryA("vulkan-1.dll");
  return vh ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(
                  GetProcAddress(vh, "vkGetDeviceProcAddr"))
            : nullptr;
#else
  void* vh = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
  if (!vh) vh = dlopen("libvulkan.so.1", RTLD_NOW);
  return vh ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(vh, "vkGetDeviceProcAddr"))
            : nullptr;
#endif
}

PFN_vkCmdBlitImage BlitFn(vk::VulkanDevice* dev) {
  static std::atomic<bool> s_tried{false};
  static PFN_vkCmdBlitImage s_fn = nullptr;
  if (!s_tried.exchange(true)) {
    if (auto gdpa = LoaderGdpa())
      s_fn = reinterpret_cast<PFN_vkCmdBlitImage>(gdpa(dev->device(), "vkCmdBlitImage"));
    REXLOG_INFO("[native_vk] M3.105 blit {}", s_fn ? "available" : "UNAVAILABLE");
  }
  return s_fn;
}

// M4.44: same lazy-lookup trick for vkCmdClearColorImage (first-use clear of
// resolve dests whose padding a partial-rect resolve never writes).
PFN_vkCmdClearColorImage ClearColorFn(vk::VulkanDevice* dev) {
  static std::atomic<bool> s_tried{false};
  static PFN_vkCmdClearColorImage s_fn = nullptr;
  if (!s_tried.exchange(true)) {
    if (auto gdpa = LoaderGdpa())
      s_fn = reinterpret_cast<PFN_vkCmdClearColorImage>(
          gdpa(dev->device(), "vkCmdClearColorImage"));
    REXLOG_INFO("[native_vk] M4.44 clear-color {}", s_fn ? "available" : "UNAVAILABLE");
  }
  return s_fn;
}

// M3.129: same lazy-lookup trick as BlitFn -- the SDK's device Functions table
// does not carry vkCmdCopyImage either.
PFN_vkCmdCopyImage CopyImageFn(vk::VulkanDevice* dev) {
  static std::atomic<bool> s_tried{false};
  static PFN_vkCmdCopyImage s_fn = nullptr;
  if (!s_tried.exchange(true)) {
    if (auto gdpa = LoaderGdpa())
      s_fn = reinterpret_cast<PFN_vkCmdCopyImage>(gdpa(dev->device(), "vkCmdCopyImage"));
    REXLOG_INFO("[native_vk] M3.129 direct resolve copy {}", s_fn ? "available" : "UNAVAILABLE");
  }
  return s_fn;
}

// M3.293: apply the scene tone curve to the scene RT, between render passes.
// Copies scene -> tone scratch, then draws tone(scratch) back over the scene
// with a fullscreen pipeline (scene_rp_load: LOADs color+depth, writes color
// only). Runs at the 3D->UI boundary so UI composites onto a toned scene.
void RecordSceneTone(vk::VulkanDevice* dev, VkCommandBuffer cmd) {
  auto& tl = TL();
  if (!tl.tone_pipeline || !tl.tone_set || !tl.tone_img) return;
  const auto& df = dev->functions();
  VkImageMemoryBarrier b[2] = {};
  b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b[0].image = tl.scene_img;
  b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b[1] = b[0];
  b[1].srcAccessMask = 0;
  b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].image = tl.tone_img;
  df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
  VkImageCopy region = {};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = region.srcSubresource;
  region.extent = {SceneW(), SceneH(), 1};
  // SDK function table lacks vkCmdCopyImage -- M3.129's lazy loader lookup.
  if (auto copy_fn = CopyImageFn(dev)) {
    copy_fn(cmd, tl.scene_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tl.tone_img,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          0, 0, nullptr, 0, nullptr, 2, b);
  VkClearValue clears[2] = {};
  VkRenderPassBeginInfo bi = {};
  bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  bi.renderPass = tl.scene_rp_load;
  bi.framebuffer = tl.scene_fb;
  bi.renderArea = {{0, 0}, {SceneW(), SceneH()}};
  bi.clearValueCount = 2;
  bi.pClearValues = clears;
  df.vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
  df.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.tone_pipeline);
  df.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.tone_pl_layout, 0, 1,
                             &tl.tone_set, 0, nullptr);
  df.vkCmdDraw(cmd, 3, 1, 0, 0);
  df.vkCmdEndRenderPass(cmd);
}

void RecordResolve(vk::VulkanDevice* dev, VkCommandBuffer cmd, const TransDrawRec& r,
                   uint32_t depth_src_surf) {
  auto& tl = TL();
  const auto& df = dev->functions();
  const bool is_depth = r.is_depth_resolve;
  // M4.44 RTFETCHDIM (the offset bloom halo): copy_w/copy_h come from
  // RB_COPY_DEST_PITCH, i.e. the dest's memory PITCH, not the texture the
  // game later declares over that memory. The bloom blur target is resolved
  // with pitch 352x182 but fetched as 322x182; hardware maps UV 1.0 to column
  // 322, while an image sized by the pitch maps it to column 352 -- the whole
  // bloom layer squeezed to 91% width, anchored left, so every highlight's
  // halo lands ~40px left of its source (user-spotted vs an RPCS3 reference).
  // Size the image by the fetch-declared extent once a draw has told us it
  // (first frame after creation still uses the pitch; the size change
  // recreates the image, which is the existing M3.1 path). Shrink only --
  // a fetch wider than the pitch would be a decode bug, not a hint.
  // Kill switch: RESTUFF_NO_RTFETCHDIM=1.
  uint32_t lw = r.copy_w, lh = r.copy_h;
  {
    static const bool s_no_fetchdim = getenv("RESTUFF_NO_RTFETCHDIM") != nullptr;
    if (!is_depth && !s_no_fetchdim) {
      auto fit = RtFetchDims().find(r.copy_dest);
      if (fit != RtFetchDims().end()) {
        const uint32_t fw = fit->second >> 16, fh = fit->second & 0xFFFFu;
        if (fw && fh && fw <= lw && fh <= lh && (fw != lw || fh != lh)) {
          static std::mutex s_fdmu;
          static std::set<uint64_t> s_fdseen;
          bool fresh;
          {
            std::lock_guard<std::mutex> lk(s_fdmu);
            fresh = s_fdseen.insert((uint64_t(r.copy_dest) << 32) | fit->second).second;
          }
          if (fresh)
            REXLOG_INFO("[RTFETCHDIM] dest=0x{:08X} pitch {}x{} -> fetch {}x{}", r.copy_dest,
                        lw, lh, fw, fh);
          lw = fw;
          lh = fh;
        }
      }
    }
  }
  TexEntry* rt = GetOrCreateRtTex(dev, r.copy_dest, lw, lh, is_depth);
  if (!rt) {
    s_res_guard.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // Depth resolves copy scene_depth (D32, depth aspect); the game's depth-restore
  // and DOF passes then sample REAL depth instead of the color garbage that
  // filling every resolve dest from scene_img (color) produced.
  // M3.12: resolves on the aux RB_COLOR_INFO base read the aux surface.
  // M3.83: for DEPTH resolves the color base is irrelevant -- hardware reads the
  // depth EDRAM tile named by RB_DEPTH_INFO. The guest saves the main scene
  // depth for the sun-glow unprojection AFTER switching RB_COLOR_INFO to the
  // half-res post surface; sourcing by color surf copied the CLEARED aux depth
  // and the glow layer reconstructed every pixel at the far plane -> zero
  // intensity -> the global world dim. The caller passes the surface whose
  // segments last WROTE depth under this resolve's di (fallback: main).
  const uint32_t ssurf = is_depth ? depth_src_surf : r.surf;
  const VkImage src_img =
      ssurf ? (is_depth ? tl.aux[ssurf - 1].depth_img : tl.aux[ssurf - 1].img)
            : (is_depth ? tl.depth_img : tl.scene_img);
  if (is_depth && ssurf != r.surf) {
    static std::atomic<int> s_dsrc{12};
    if (s_dsrc.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[native_vk] M3.83 depth resolve dst=0x{:08X} di={:03X}: src surf{} (color base "
                  "said surf{})", r.copy_dest, r.di, ssurf, r.surf);
  }
  // M4.4 (RESTUFF_DEPTH_FILL=1): single-pass depth 2x downsample. The M3.115
  // bounce moves the depth rect THREE times (image->buffer->image->blit) with
  // three serializing transfer barriers, all on the fenced critical path;
  // this replaces it with one depth-only render pass on the dest whose FS
  // texelFetches src at 2*p+1 (exact NEAREST-blit parity -- see the pipeline
  // creation comment). M3.115's actual corruption was BLITTING live depth;
  // SAMPLING it is the shipping M3.99 pre-fill mechanism, proven safe.
  // First cut covers the main-depth source only (aux depth images lack
  // SAMPLED usage); aux sources and any lazy-create failure fall through to
  // the bounce unchanged. renderArea == the resolve rect, so partial-rect
  // persistence (resolved_once, M3.114) is honored structurally -- pixels
  // outside renderArea are untouched by definition.
  // M4.34: default on; RESTUFF_NO_DEPTH_FILL=1 restores the bounce.
  static const bool s_depth_fill = getenv("RESTUFF_NO_DEPTH_FILL") == nullptr;
  // M4.38: the depth-fill FS hardcodes the 2x sample grid (texelFetch at
  // 2*p+1), so it is only valid at S=1. Above that the generalized f-factor
  // bounce below handles the downscale instead -- slower, but this path is a
  // perf optimization, not a correctness one.
  if (s_depth_fill && SceneScale() == 1u && r.src_2x && is_depth &&
      tl.depth_fill_pipeline != VK_NULL_HANDLE &&
      src_img == tl.depth_img && rt->is_depth) {
    static const bool df_full_resolve = getenv("RESTUFF_FULL_RESOLVE") != nullptr;
    uint32_t rx = df_full_resolve ? 0 : r.copy_rx, ry = df_full_resolve ? 0 : r.copy_ry;
    uint32_t cw = (!df_full_resolve && r.copy_rw) ? r.copy_rw : std::min(rt->width, kGuestW);
    uint32_t ch = (!df_full_resolve && r.copy_rh) ? r.copy_rh : std::min(rt->height, kGuestH);
    if (rx >= kGuestW || ry >= kGuestH || rx >= rt->width || ry >= rt->height) {
      s_res_guard.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    cw = std::min({cw, kGuestW - rx, rt->width - rx});
    ch = std::min({ch, kGuestH - ry, rt->height - ry});
    if (!cw || !ch) {
      s_res_guard.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // Lazily create the dest's attachment view + framebuffer (destroyed with
    // the TexEntry). Failure is non-fatal: fall through to the bounce.
    if (rt->att_view == VK_NULL_HANDLE) {
      VkImageViewCreateInfo av = {};
      av.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      av.image = rt->image;
      av.viewType = VK_IMAGE_VIEW_TYPE_2D;
      av.format = kSceneDepthFormat;
      av.subresourceRange = vk::util::InitializeSubresourceRange();
      av.subresourceRange.aspectMask = kDepthAttAspect;  // attachment view: both aspects
      df.vkCreateImageView(dev->device(), &av, nullptr, &rt->att_view);
    }
    if (rt->att_view != VK_NULL_HANDLE && rt->ds_fb == VK_NULL_HANDLE) {
      VkFramebufferCreateInfo fb = {};
      fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fb.renderPass = tl.depth_fill_rp;
      fb.attachmentCount = 1;
      fb.pAttachments = &rt->att_view;
      fb.width = rt->width;
      fb.height = rt->height;
      fb.layers = 1;
      df.vkCreateFramebuffer(dev->device(), &fb, nullptr, &rt->ds_fb);
    }
    if (rt->ds_fb != VK_NULL_HANDLE) {
      // Same lazy fill_set write as the M3.99 user (:fill_set_written) -- the
      // set samples the main depth via sampler_nearest.
      if (!tl.fill_set_written) {
        VkDescriptorImageInfo ii = {DL().sampler_nearest, tl.depth_sample_view,
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = tl.fill_set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        df.vkUpdateDescriptorSets(dev->device(), 1, &w, 0, nullptr);
        tl.fill_set_written = true;
      }
      VkImageMemoryBarrier fb2[2] = {};
      // Source main depth: ATTACHMENT -> READ_ONLY for sampling (M3.97: both
      // aspects on a combined image).
      fb2[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      fb2[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      fb2[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      fb2[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      fb2[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      fb2[0].srcQueueFamilyIndex = fb2[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      fb2[0].image = tl.depth_img;
      fb2[0].subresourceRange = vk::util::InitializeSubresourceRange();
      fb2[0].subresourceRange.aspectMask = kDepthAttAspect;
      // Dest: honor resolved_once exactly like the transfer path's b[1].
      fb2[1] = fb2[0];
      fb2[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      fb2[1].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      fb2[1].oldLayout = rt->resolved_once ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                           : VK_IMAGE_LAYOUT_UNDEFINED;
      fb2[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      fb2[1].image = rt->image;
      rt->resolved_once = true;
      df.vkCmdPipelineBarrier(cmd,
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 0, nullptr, 0, nullptr, 2, fb2);
      VkRenderPassBeginInfo bi = {};
      bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      bi.renderPass = tl.depth_fill_rp;
      bi.framebuffer = rt->ds_fb;
      bi.renderArea = {{int32_t(rx), int32_t(ry)}, {cw, ch}};
      df.vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
      VkViewport vpo = {float(rx), float(ry), float(cw), float(ch), 0.0f, 1.0f};
      VkRect2D sc = {{int32_t(rx), int32_t(ry)}, {cw, ch}};
      df.vkCmdSetViewport(cmd, 0, 1, &vpo);
      df.vkCmdSetScissor(cmd, 0, 1, &sc);
      df.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.depth_fill_pipeline);
      df.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.pipeline_layout, 1, 1,
                                 &tl.fill_set, 0, nullptr);
      df.vkCmdDraw(cmd, 3, 1, 0, 0);
      df.vkCmdEndRenderPass(cmd);
      // Dest -> SHADER_READ_ONLY; source back to ATTACHMENT (mirrors the
      // transfer path's closing barriers).
      fb2[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      fb2[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      fb2[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      fb2[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      fb2[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      fb2[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      fb2[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      fb2[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      df.vkCmdPipelineBarrier(cmd,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              0, 0, nullptr, 0, nullptr, 2, fb2);
      s_res_exec.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // fb creation failed -> fall through to the M3.115 bounce below.
  }
  // M3.97: the copy still moves only the DEPTH aspect (that is what the guest
  // samples), but BARRIERS on a combined depth/stencil image must name BOTH
  // aspects or validation rejects them (VUID-VkImageMemoryBarrier-image-03319).
  const VkImageAspectFlags aspect =
      is_depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  const VkImageAspectFlags barrier_aspect = is_depth ? kDepthAttAspect : VK_IMAGE_ASPECT_COLOR_BIT;
  const VkImageLayout src_att_layout = is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                                : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  const VkPipelineStageFlags src_stage = is_depth
                                             ? (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)
                                             : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  const VkAccessFlags src_access = is_depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                                            : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkImageMemoryBarrier b[2] = {};
  // Source scene/depth -> TRANSFER_SRC.
  b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b[0].srcAccessMask = src_access;
  b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[0].oldLayout = src_att_layout;
  b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].srcQueueFamilyIndex = b[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b[0].image = src_img;
  b[0].subresourceRange = vk::util::InitializeSubresourceRange();
  b[0].subresourceRange.aspectMask = barrier_aspect;
  // Dest -> TRANSFER_DST (discard old contents).
  b[1] = b[0];
  b[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].oldLayout = rt->resolved_once ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     : VK_IMAGE_LAYOUT_UNDEFINED;
  b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].image = rt->image;
  const bool rt_first_use = !rt->resolved_once;
  rt->resolved_once = true;
  df.vkCmdPipelineBarrier(cmd, src_stage | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
  // M4.44: a fresh dest image has UNDEFINED contents, and a partial-rect
  // resolve (the bloom pass writes 320x180 into its 322x182 dest; RESCLAMP
  // trims every bloom-chain rect) never touches the padding. Clear it once so
  // the padding is dark like hardware's steady state instead of whatever the
  // allocation held; later frames keep contents (oldLayout SHADER_READ_ONLY).
  if (rt_first_use && !is_depth) {
    if (auto clr = ClearColorFn(dev)) {
      VkClearColorValue zero = {};
      VkImageSubresourceRange rng = vk::util::InitializeSubresourceRange();
      rng.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      clr(cmd, rt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &rng);
      VkMemoryBarrier mb = {};
      mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
      mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 1, &mb, 0, nullptr, 0, nullptr);
    }
  }

  // Bounce scene -> buffer -> dest (the SDK function table lacks image-to-image
  // copies). Both images share kGuestOutputFormat, so this is a bit copy.
  // Honor the resolve rect: hardware copies only the pixels the resolve draw
  // covers, and partial-region resolves are how the game composes its front
  // buffer (full-frame copies would stomp regions resolved by other passes).
  // RESTUFF_FULL_RESOLVE=1: ignore the resolve rect and copy the whole scene
  // (diagnostic -- shows the live scene as-rendered, bypassing front-buffer
  // history from partial dirty-rect resolves).
  static const bool full_resolve = getenv("RESTUFF_FULL_RESOLVE") != nullptr;
  uint32_t rx = full_resolve ? 0 : r.copy_rx, ry = full_resolve ? 0 : r.copy_ry;
  uint32_t cw = (!full_resolve && r.copy_rw) ? r.copy_rw : std::min(rt->width, kGuestW);
  uint32_t ch = (!full_resolve && r.copy_rh) ? r.copy_rh : std::min(rt->height, kGuestH);
  // The same offset addresses both scene (src) and dest image: a rect origin
  // past EITHER extent would make an out-of-bounds transfer (driver UB / the
  // GPU scribbling neighbors -- the Play-Game heap-corruption class).
  if (rx >= kGuestW || ry >= kGuestH || rx >= rt->width || ry >= rt->height) {
    s_res_guard.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  cw = std::min({cw, kGuestW - rx, rt->width - rx});
  ch = std::min({ch, kGuestH - ry, rt->height - ry});
  if (!cw || !ch) {
    s_res_guard.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  // M3.105: a resolve of the FULL-RES volume/shaft surface downsamples 2:1
  // (the reference's resolve does the same; the 2x2 average is also what
  // smooths the sliver noise). df lacks vkCmdBlitImage; resolve it once from
  // the loader.
  if (getenv("RESTUFF_RESOLVES")) {
    static std::mutex s_rmu;
    static std::set<uint64_t> s_rseen;
    bool fresh;
    {
      std::lock_guard<std::mutex> lk(s_rmu);
      fresh = s_rseen.insert((uint64_t(r.copy_dest) << 1) | (is_depth ? 1 : 0)).second;
    }
    if (fresh)
      REXLOG_INFO("[RESOLVES] dest=0x{:08X} {}x{} {} src_2x={} di=0x{:03X} surf={} ctl=0x{:X}",
                  r.copy_dest, r.copy_w, r.copy_h, is_depth ? "DEPTH" : "color", int(r.src_2x),
                  r.di, r.surf, r.dbg_copyctl);
  }
  // M4.38: source-to-dest ratio. src_2x is the guest's own 2:1 resolve (the
  // full-res volume/shaft chain); SceneScale() is our internal supersample.
  // They compose: at S=2 a src_2x resolve reads a 4x rect. f == 1 means
  // src rect == dest rect and the plain copy path below still applies, so at
  // S=1 without src_2x nothing here changes.
  const uint32_t f = (r.src_2x ? 2u : 1u) * SceneScale();
  // M3.115 attempt 1 (REVERTED): blitting the live main D32S8 depth blacked
  // the whole frame (silent driver corruption; no fault logged). Depth
  // downsampling now happens via the 2x-decimating bounce copy below instead.
  if (f > 1u && !is_depth) {
    if (getenv("RESTUFF_OCCRECT")) {
      static std::atomic<int> s_or{40};
      if (s_or.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[OCCRECT] dest={:08X} rect=({},{} {}x{}) rt={}x{} full={}", r.copy_dest,
                    r.copy_rx, r.copy_ry, r.copy_rw, r.copy_rh, rt->width, rt->height,
                    int(full_resolve));
    }
    PFN_vkCmdBlitImage s_blit_fn = BlitFn(dev);
    if (s_blit_fn) {
      const uint32_t sx = std::min(f * rx, SceneW()), sy = std::min(f * ry, SceneH());
      const uint32_t sw = std::min(f * cw, SceneW() - sx), sh = std::min(f * ch, SceneH() - sy);
      VkImageBlit bl = {};
      bl.srcSubresource = {aspect, 0, 0, 1};
      bl.srcOffsets[0] = {int32_t(sx), int32_t(sy), 0};
      bl.srcOffsets[1] = {int32_t(sx + sw), int32_t(sy + sh), 1};
      bl.dstSubresource = {aspect, 0, 0, 1};
      bl.dstOffsets[0] = {int32_t(rx), int32_t(ry), 0};
      bl.dstOffsets[1] = {int32_t(rx + cw), int32_t(ry + ch), 1};
      s_blit_fn(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt->image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                is_depth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
      goto resolve_done;
    }
  }
  if (f > 1u && is_depth && BlitFn(dev) != nullptr) {
    // M3.115: depth resolves of the full-res volume chain must 2x-downsample
    // like the colour path -- the plain bounce copied the top-left QUADRANT
    // of the full-res depth into the shaft's 640x360 depth texture, so every
    // depth-based unprojection in the shadow shading worked from zoomed-in
    // positions (camera-motion-coupled drift/cut).
    const uint32_t sx = std::min(f * rx, SceneW()), sy = std::min(f * ry, SceneH());
    const uint32_t sw = std::min(f * cw, SceneW() - sx), sh = std::min(f * ch, SceneH() - sy);
    if (tl.ds2x_img == VK_NULL_HANDLE) {
      VkImageCreateInfo ci = {};
      ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = kSceneDepthFormat;
      ci.extent = {SceneW(), SceneH(), 1};
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.tiling = VK_IMAGE_TILING_OPTIMAL;
      ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      if (!vk::util::CreateDedicatedAllocationImage(dev, ci,
                                                    vk::util::MemoryPurpose::kDeviceLocal,
                                                    tl.ds2x_img, tl.ds2x_mem, nullptr)) {
        tl.ds2x_img = VK_NULL_HANDLE;
      }
    }
    if (tl.ds2x_img != VK_NULL_HANDLE) {
      VkBufferImageCopy reg = {};
      reg.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      reg.imageOffset = {int32_t(sx), int32_t(sy), 0};
      reg.imageExtent = {sw, sh, 1};
      df.vkCmdCopyImageToBuffer(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                tl.resolve_buf, 1, &reg);
      VkBufferMemoryBarrier bb2 = {};
      bb2.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
      bb2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      bb2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      bb2.srcQueueFamilyIndex = bb2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      bb2.buffer = tl.resolve_buf;
      bb2.size = VK_WHOLE_SIZE;
      VkImageMemoryBarrier sb = {};
      sb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      sb.srcAccessMask = tl.ds2x_init ? VK_ACCESS_TRANSFER_READ_BIT : 0;
      sb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      sb.oldLayout =
          tl.ds2x_init ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
      sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      sb.srcQueueFamilyIndex = sb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      sb.image = tl.ds2x_img;
      sb.subresourceRange = vk::util::InitializeSubresourceRange();
      sb.subresourceRange.aspectMask = kDepthAttAspect;
      df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 1, &bb2, 1, &sb);
      VkBufferImageCopy reg2 = reg;
      reg2.imageOffset = {0, 0, 0};
      df.vkCmdCopyBufferToImage(cmd, tl.resolve_buf, tl.ds2x_img,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg2);
      sb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      sb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      sb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      sb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &sb);
      tl.ds2x_init = true;
      VkImageBlit bl = {};
      bl.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      bl.srcOffsets[0] = {int32_t(0), int32_t(0), 0};
      bl.srcOffsets[1] = {int32_t(sw), int32_t(sh), 1};
      bl.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      bl.dstOffsets[0] = {int32_t(rx), int32_t(ry), 0};
      bl.dstOffsets[1] = {int32_t(rx + cw), int32_t(ry + ch), 1};
      BlitFn(dev)(cmd, tl.ds2x_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt->image,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_NEAREST);
      goto resolve_done;
    }
  }
  {
  VkBufferImageCopy region = {};
  region.imageSubresource = {aspect, 0, 0, 1};
  region.imageOffset = {int32_t(rx), int32_t(ry), 0};
  region.imageExtent = {cw, ch, 1};
  // M3.129: copy image->image directly instead of bouncing through
  // tl.resolve_buf. The bounce moved every resolve's pixels TWICE and put a
  // full transfer barrier between the halves; with ~23 resolves per frame that
  // is ~40 MB of extra traffic and 23 serialising barriers per frame, all on
  // the critical path (the present thread blocks on the frame fence). The
  // source and destination formats are IDENTICAL on both paths -- colour is
  // kGuestOutputFormat (A2B10G10R10_UNORM_PACK32) on the scene/aux images and
  // on the resolve target, depth is kSceneDepthFormat (D32_SFLOAT_S8_UINT) on
  // both -- so vkCmdCopyImage is a bit-for-bit equivalent of the round trip.
  // The buffer path stays for the resolve-writeback opt-in (which needs the
  // pixels in a host-visible buffer) and as an A/B via RESTUFF_RESOLVE_BOUNCE=1.
  static const bool s_force_bounce = getenv("RESTUFF_RESOLVE_BOUNCE") != nullptr;
  const bool wb_needs_buffer =
      getenv("RESTUFF_RESOLVE_WB") != nullptr && !is_depth && tl.wb_ptr;
  if (!s_force_bounce && !wb_needs_buffer && CopyImageFn(dev)) {
    VkImageCopy ic = {};
    ic.srcSubresource = {aspect, 0, 0, 1};
    ic.srcOffset = {int32_t(rx), int32_t(ry), 0};
    ic.dstSubresource = {aspect, 0, 0, 1};
    ic.dstOffset = {int32_t(rx), int32_t(ry), 0};
    ic.extent = {cw, ch, 1};
    CopyImageFn(dev)(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rt->image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    goto resolve_done;
  }
  df.vkCmdCopyImageToBuffer(cmd, src_img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, tl.resolve_buf, 1,
                            &region);
  VkBufferMemoryBarrier bb = {};
  bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
  bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  bb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  bb.buffer = tl.resolve_buf;
  bb.size = VK_WHOLE_SIZE;
  df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          0, nullptr, 1, &bb, 0, nullptr);
  df.vkCmdCopyBufferToImage(cmd, tl.resolve_buf, rt->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &region);
  }
resolve_done:

  // Dest -> SHADER_READ_ONLY; source back to its attachment layout.
  b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b[0].dstAccessMask = src_access;
  b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b[0].newLayout = src_att_layout;
  b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  b[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          src_stage | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                          nullptr, 2, b);
  // M3.89: capture small color resolves for the guest-RAM writeback. The
  // resolve_buf still holds this resolve's pixels (tightly packed cw x ch);
  // a buffer-to-buffer copy snapshots them before the next resolve reuses it.
  // RESTUFF_NO_RESOLVE_WB=1 opts out.
  // M3.89: opt-in (RESTUFF_RESOLVE_WB=1). Hardware-faithful (resolves write
  // guest RAM) but did NOT fix the dim (the exposure meter is texture-sampled,
  // and the tonemap composite passes its input through 1:1 -- the dim is
  // upstream in the lit-world shading, not the exposure) and it carries a
  // guest-RAM corruption surface, so it stays off until a readback needs it.
  static const bool do_wb = getenv("RESTUFF_RESOLVE_WB") != nullptr;
  if (do_wb && !is_depth && tl.wb_ptr && cw * ch <= 640u * 360u) {
    const VkDeviceSize bytes = VkDeviceSize(cw) * ch * 4;
    if (tl.wb_off + bytes <= (VkDeviceSize(8) << 20)) {
      VkBufferCopy bc = {0, tl.wb_off, bytes};
      df.vkCmdCopyBuffer(cmd, tl.resolve_buf, tl.wb_buf, 1, &bc);
      tl.wb_pending.push_back({r.copy_dest, cw, ch, tl.wb_off});
      tl.wb_off += (bytes + 255) & ~VkDeviceSize(255);
    }
  }
  s_res_exec.fetch_add(1, std::memory_order_relaxed);
}

// M3.89 post-fence: convert captured resolves (A2B10G10R10 host pixels) to the
// guest texture layout -- tiled, 32bpp, the fmt6 "raw LE read is GPU-ready
// 0xAARRGGBB" convention -- and write them into guest RAM at the copy dest,
// exactly as the hardware resolve would have.
void FlushResolveWritebacks() {
  auto& tl = TL();
  if (!tl.wb_ptr || tl.wb_pending.empty()) {
    tl.wb_off = 0;
    return;
  }
  const uint8_t* base = static_cast<const uint8_t*>(tl.wb_ptr);
  for (const auto& wb : tl.wb_pending) {
    uint8_t* dst = renderer::GuestPhysPtrMut(wb.dest);
    if (!dst) continue;
    const uint32_t* src = reinterpret_cast<const uint32_t*>(base + wb.off);
    for (uint32_t y = 0; y < wb.h; ++y) {
      for (uint32_t x = 0; x < wb.w; ++x) {
        const uint32_t v = src[y * wb.w + x];
        const uint32_t r8 = (v & 0x3FF) >> 2;
        const uint32_t g8 = ((v >> 10) & 0x3FF) >> 2;
        const uint32_t b8 = ((v >> 20) & 0x3FF) >> 2;
        const uint32_t a8 = ((v >> 30) & 0x3) * 85;
        const uint32_t argb = (a8 << 24) | (r8 << 16) | (g8 << 8) | b8;
        std::memcpy(dst + renderer::TiledBlockByteOffset(x, y, wb.w, 4), &argb, 4);
      }
    }
  }
  static std::atomic<int> s_wblog{6};
  if (s_wblog.fetch_sub(1, std::memory_order_relaxed) > 0)
    REXLOG_INFO("[native_vk] M3.89 writeback: {} resolves -> guest RAM (first dest=0x{:08X})",
                uint32_t(tl.wb_pending.size()), tl.wb_pending[0].dest);
  tl.wb_pending.clear();
  tl.wb_off = 0;
}

// Pre-render-pass: pull the raw frame, grow rings, fill vertex/index/UBO data,
// resolve textures (queues uploads), and build the per-draw record list.
std::vector<TransDrawRec> PrepareTranslatedDraws(vk::VulkanDevice* dev) {
  auto& tl = TL();
  std::vector<TransDrawRec> recs;
  // M3.135c: this function IS the frame cost. The M3.135b split showed the SDK
  // presenter's pre-callback path is 2us while the stretch from the callback
  // entry to the record section is 26-34ms -- and unlike queue_acquire, none of
  // it touches the queue mutex, so it is not the Xvfb artifact. Time the five
  // top-level stages so the expensive one is named rather than guessed at.
  static const bool s_pp = getenv("RESTUFF_PREPMS") != nullptr;
  auto _pp_now = [] { return std::chrono::steady_clock::now(); };
  const auto _pp_a = _pp_now();
  auto _pp_b = _pp_a, _pp_c = _pp_a, _pp_d = _pp_a, _pp_e = _pp_a;
  {
    std::vector<renderer::RawGuestDraw> fresh;
    uint32_t frame_fb = 0;
    if (renderer::ConsumeRawFrame(fresh, &frame_fb)) {
      // RESTUFF_CONSUME_TRACE=1: raw size of each consumed guest chunk, in
      // order, no merge logic. If chunks alternate big/small, the guest frame
      // is arriving SPLIT across multiple XE_SWAPs and the merge must reassemble
      // it; if uniform ~1400, splitting is not the problem.
      if (getenv("RESTUFF_CONSUME_TRACE")) {
        static std::atomic<uint64_t> s_cc{0};
        const uint64_t cc = s_cc.fetch_add(1, std::memory_order_relaxed);
        static const uint64_t at = getenv("RESTUFF_CONSUME_AT")
                                       ? strtoull(getenv("RESTUFF_CONSUME_AT"), nullptr, 10) : 3500;
        if (cc >= at && cc < at + 40) {
          uint32_t col = 0, pre = 0, res = 0; bool ends_fb = false;
          for (const auto& d : fresh) {
            if (d.is_resolve) { ++res; ends_fb = (frame_fb && d.copy_dest == frame_fb); continue; }
            if (d.color_mask) ++col; else ++pre;
          }
          REXLOG_INFO("[CONSUME] chunk#{} size={} colour={} prepass={} resolves={} fb=0x{:08X} "
                      "ends_on_fb={}", cc, uint32_t(fresh.size()), col, pre, res, frame_fb, ends_fb);
        }
      }
      // M3.16: accumulate chunks until the frame's own front-buffer resolve is
      // present (a complete frame ends by resolving into its VdSwap fb); a
      // chunk without it is a split-frame fragment -- showing it flashes
      // missing draws for one present. Hold-cap prevents deadlock on frames
      // that legitimately never resolve to fb (e.g. loading blanks).
      auto& pend = tl.pending_chunks;
      pend.insert(pend.end(), std::make_move_iterator(fresh.begin()),
                  std::make_move_iterator(fresh.end()));
      // Complete = the chunk's LAST resolve targets this frame's fb (frames
      // end with the post-HUD front-buffer resolve; mid-frame fb resolves from
      // the composite ping-pong don't count).
      bool complete = frame_fb == 0;
      if (!complete) {
        for (auto it = pend.rbegin(); it != pend.rend(); ++it) {
          if (!it->is_resolve) continue;
          complete = it->copy_dest == frame_fb;
          break;
        }
      }
      // RESTUFF_MERGE_TRACE=1: why each publish happened. On a STATIC scene the
      // published draw count must be constant; measured it swings 656..909, so
      // frames are being assembled inconsistently (the black blobs).
      const bool forced = !complete && (pend.size() > 6000 || tl.pending_holds + 1 > 3);
      // Log EVERY publish, but only once gameplay is running. A size filter
      // would hide exactly the pathological tiny publishes; a plain budget gets
      // eaten by menu frames. So: count consumes, start after RESTUFF_MERGE_AT.
      static std::atomic<uint64_t> s_consumes{0};
      const uint64_t consume_n = s_consumes.fetch_add(1, std::memory_order_relaxed);
      static const uint64_t merge_at = getenv("RESTUFF_MERGE_AT")
                                           ? strtoull(getenv("RESTUFF_MERGE_AT"), nullptr, 10)
                                           : 2500;
      if (getenv("RESTUFF_MERGE_TRACE") && consume_n >= merge_at) {
        uint32_t last_res = 0, fb_res = 0, ch_col = 0, ch_pre = 0, ch_coltris = 0;
        for (auto it = pend.rbegin(); it != pend.rend(); ++it)
          if (it->is_resolve) { last_res = it->copy_dest; break; }
        // How many times does THIS chunk resolve to the swap fb? >1 means the
        // "last resolve == fb" completeness test can fire on a MID-frame fb
        // resolve and publish a partial frame.
        for (const auto& d : pend) {
          if (d.is_resolve) { if (frame_fb && d.copy_dest == frame_fb) ++fb_res; continue; }
          if (d.color_mask) { ++ch_col; ch_coltris += uint32_t(d.idx().size() / 3); }
          else ++ch_pre;
        }
        static std::atomic<int> s_mb{60};
        if (s_mb.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[MERGE] fb=0x{:08X} lastres=0x{:08X} fbresolves={} complete={} holds={} "
                      "chunk(col={} coltris={} pre={}) {}",
                      frame_fb, last_res, fb_res, complete, tl.pending_holds, ch_col, ch_coltris,
                      ch_pre,
                      forced ? "FORCED-PUBLISH(incomplete)" : (complete ? "publish" : "hold"));
      }
      if (complete || pend.size() > 6000 || ++tl.pending_holds > 3) {
        tl.frame = std::move(pend);
        pend.clear();
        tl.pending_holds = 0;
        if (frame_fb) tl.frame_fb = frame_fb;
        // M4.10 [MIXFRAME]: chunk merge can stitch one published frame from
        // draws captured in DIFFERENT guest frames (different camera!). Every
        // draw now carries its walker frame serial; count publishes whose
        // draws span >1 serial. Detail-log the first 40, then a periodic
        // tally so the rate stays visible without spam.
        {
          uint64_t fmin = ~0ull, fmax = 0;
          uint32_t stale_draws = 0;
          for (const auto& d : tl.frame) {
            if (!d.cap_frame) continue;
            fmin = std::min(fmin, d.cap_frame);
            fmax = std::max(fmax, d.cap_frame);
          }
          const bool mixed = fmax > fmin && fmin != ~0ull;
          if (mixed)
            for (const auto& d : tl.frame)
              if (d.cap_frame && d.cap_frame != fmax) ++stale_draws;
          static uint64_t s_pub_n = 0, s_mix_n = 0;
          ++s_pub_n;
          if (mixed) ++s_mix_n;
          static std::atomic<int> s_mix_budget{40};
          if (mixed && s_mix_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[MIXFRAME] publish spans guest frames {}..{} ({} of {} draws stale)",
                        fmin, fmax, stale_draws, uint32_t(tl.frame.size()));
          if (s_pub_n % 300 == 0)
            REXLOG_INFO("[MIXFRAME] tally: {} mixed / {} publishes", s_mix_n, s_pub_n);
        }
        // M4.18 (RESTUFF_DMTX=1): stale-transform detector for the blob-decal
        // draw. Its geometry is proven bit-stable across flicker, yet skipping
        // the draw kills the flicker -- so the varying input is the TRANSFORM
        // (c0..c15, which CBLIP deliberately excluded as camera-varying).
        // Test: when the reference world draw's camera rows CHANGED this frame
        // but the decal draw's did NOT, the decal rendered with a stale camera
        // -- smearing its dark triangles for exactly one frame.
        {
          static const bool s_dmtx = getenv("RESTUFF_DMTX") != nullptr;
          if (s_dmtx) {
            constexpr uint64_t kDecalPs = 0xBBA590486A51E72Aull;
            constexpr uint64_t kRefPs = 0x664E751EE9E4C559ull;
            const renderer::RawGuestDraw* dec = nullptr;
            const renderer::RawGuestDraw* ref = nullptr;
            for (const auto& d : tl.frame) {
              if (d.is_resolve || d.vs_consts.size() < 64) continue;
              if (!dec && d.ps_hash == kDecalPs) dec = &d;
              if (!ref && d.ps_hash == kRefPs) ref = &d;
              if (dec && ref) break;
            }
            (void)ref;
            if (dec) {
              // v2 (run Q postmortem): the pairwise ref compare logged the
              // frame AFTER the disagreement and was polluted by ref-draw
              // pick instability. Majority vote instead: every >=64-float
              // draw votes with its camera block; the modal block is the
              // frame's true camera (verbose rows proved the decal shares it
              // bitwise in steady state). Event = decal vs consensus, at the
              // smear frame itself; matching the PREVIOUS consensus is the
              // one-frame-behind proof.
              // v3: hash ONLY c0..c3 (the 4x4 WVP). v2 hashed c0..c15, which
              // includes material rows where the decal legitimately differs
              // -> 97% false "mismatch" while the camera rows were identical.
              std::unordered_map<uint64_t, uint32_t> votes;
              uint64_t dec_h = 0;
              float modal_c3[4] = {};
              uint64_t modal_h = 0;
              uint32_t modal_n = 0;
              for (const auto& d : tl.frame) {
                if (d.is_resolve || d.vs_consts.size() < 64) continue;
                uint64_t h = 1469598103934665603ull;
                for (int i = 0; i < 16; ++i)
                  h = (h ^ std::bit_cast<uint32_t>(d.vs_consts[i])) * 1099511628211ull;
                const uint32_t n = ++votes[h];
                if (n > modal_n) {
                  modal_n = n;
                  modal_h = h;
                  for (int i = 0; i < 4; ++i) modal_c3[i] = d.vs_consts[12 + i];
                }
                if (&d == dec) dec_h = h;
              }
              static uint64_t prev_modal_h = 0;
              static float prev_modal_c3[4] = {};
              static uint64_t s_pubs = 0, s_mismatch = 0, s_one_behind = 0;
              if (modal_n && dec_h) {
                ++s_pubs;
                if (dec_h != modal_h) {
                  ++s_mismatch;
                  const bool one_behind = prev_modal_h && dec_h == prev_modal_h;
                  if (one_behind) ++s_one_behind;
                  static std::atomic<int> s_dmtx_budget{40};
                  if (s_dmtx_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
                    REXLOG_INFO("[DMTX] DECAL-OFF-CAMERA{} modal_n={} dec_c3=({:.4f},{:.4f},"
                                "{:.4f},{:.4f}) modal_c3=({:.4f},{:.4f},{:.4f},{:.4f}) "
                                "prev_modal_c3=({:.4f},{:.4f},{:.4f},{:.4f})",
                                one_behind ? " ONE-FRAME-BEHIND" : "", modal_n,
                                dec->vs_consts[12], dec->vs_consts[13], dec->vs_consts[14],
                                dec->vs_consts[15], modal_c3[0], modal_c3[1], modal_c3[2],
                                modal_c3[3], prev_modal_c3[0], prev_modal_c3[1], prev_modal_c3[2],
                                prev_modal_c3[3]);
                }
                if (s_pubs % 300 == 0)
                  REXLOG_INFO("[DMTX] tally: pubs={} decal_off_camera={} one_frame_behind={}",
                              s_pubs, s_mismatch, s_one_behind);
                prev_modal_h = modal_h;
                std::memcpy(prev_modal_c3, modal_c3, sizeof(prev_modal_c3));
              }
            }
            // M4.19 [DSTATE]: geometry, transform and colours are all proven
            // clean for the decal draw, yet skipping it kills the flicker.
            // Last unwatched inputs: per-draw RENDER STATE (blend/masks/
            // depth) and the low material constants (vs c4..c7, ps c0..c7).
            // These are static frame-to-frame for a decal pass -- log ANY
            // change with before/after. Track the first two BBA instances
            // separately (the pass draws cm=0 then cm=F; the visible one is
            // the second).
            {
              constexpr uint64_t kDecalPs2 = 0xBBA590486A51E72Aull;
              struct DState {
                uint32_t cm = 0, bl = 0, dc = 0, su = 0;
                float mat[48] = {};  // vs c4..c7 (16) + ps c0..c7 (32)
                bool valid = false;
              };
              static DState prev_inst[2];
              static uint64_t s_ds_pubs = 0, s_ds_changes = 0;
              int inst = 0;
              for (const auto& d : tl.frame) {
                if (d.is_resolve || d.ps_hash != kDecalPs2 || inst >= 2) continue;
                if (d.vs_consts.size() < 32 || d.ps_consts.size() < 32) { ++inst; continue; }
                DState cur;
                cur.cm = d.color_mask;
                cur.bl = d.blend_control;
                cur.dc = d.depth_control;
                cur.su = d.su_mode;
                for (int i = 0; i < 16; ++i) cur.mat[i] = d.vs_consts[16 + i];
                for (int i = 0; i < 32; ++i) cur.mat[16 + i] = d.ps_consts[i];
                cur.valid = true;
                DState& pv = prev_inst[inst];
                if (pv.valid) {
                  // v2 (first field run): cm/bl/dc/su NEVER changed (hard
                  // state exonerated); psc4/psc5 drift smoothly (~<1/frame,
                  // camera-tied positions) and psc2.3 is a fade scalar that
                  // ramps by ~0.04/frame. Per-frame "any change" is thus
                  // constant noise -- switch to SPIKE detection: flag JUMPS
                  // far beyond normal drift, plus any hard-state change.
                  char diffs[320] = {};
                  int dp = 0;
                  uint32_t nd = 0;
                  auto du32 = [&](const char* n, uint32_t o, uint32_t v) {
                    if (o != v) {
                      ++nd;
                      if (dp < int(sizeof(diffs)) - 40)
                        dp += std::snprintf(diffs + dp, sizeof(diffs) - dp, " %s %08X->%08X", n,
                                            o, v);
                    }
                  };
                  du32("cm", pv.cm, cur.cm);
                  du32("bl", pv.bl, cur.bl);
                  du32("dc", pv.dc, cur.dc);
                  du32("su", pv.su, cur.su);
                  for (int i = 0; i < 48; ++i) {
                    const float o = pv.mat[i], v = cur.mat[i];
                    if (std::bit_cast<uint32_t>(o) == std::bit_cast<uint32_t>(v)) continue;
                    // psc2 row (alpha/fade family): jump threshold 0.25.
                    // Everything else (positions/distances): threshold 15.
                    const bool alpha_row = (i >= 16 && (i - 16) / 4 == 2);
                    const float thr = alpha_row ? 0.25f : 15.0f;
                    const bool weird = !std::isfinite(v) || std::fabs(v - o) > thr;
                    if (!weird) continue;
                    ++nd;
                    if (dp < int(sizeof(diffs)) - 48)
                      dp += std::snprintf(diffs + dp, sizeof(diffs) - dp, " %s%d.%d %.4g->%.4g",
                                          i < 16 ? "vsc" : "psc", i < 16 ? 4 + i / 4 : (i - 16) / 4,
                                          i % 4, o, v);
                  }
                  if (nd) {
                    ++s_ds_changes;
                    static std::atomic<int> s_ds_budget{60};
                    if (s_ds_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
                      REXLOG_INFO("[DSTATE] SPIKE inst={} nd={}{}", inst, nd, diffs);
                  }
                }
                // One-shot: name the decal draw's texture bindings. Slot 0 is
                // known-empty; a depth-ish binding in slot 1/2 = the soft-
                // decal depth-fade input (the last uninspected input class).
                static std::atomic<int> s_dtex_once{2};
                if (s_dtex_once.fetch_sub(1, std::memory_order_relaxed) > 0)
                  REXLOG_INFO("[DECALTEX] inst={} t0=0x{:08X} f{} {}x{} t1=0x{:08X} f{} {}x{} "
                              "t2=0x{:08X} f{} {}x{}",
                              inst, d.tex[0].phys_addr, d.tex[0].format, d.tex[0].width,
                              d.tex[0].height, d.tex[1].phys_addr, d.tex[1].format,
                              d.tex[1].width, d.tex[1].height, d.tex[2].phys_addr,
                              d.tex[2].format, d.tex[2].width, d.tex[2].height);
                pv = cur;
                ++inst;
              }
              if (inst && ++s_ds_pubs % 300 == 0)
                REXLOG_INFO("[DSTATE] tally: pubs={} change_events={}", s_ds_pubs, s_ds_changes);
            }
          }
        }
        // M4.20 (RESTUFF_VANISH=1): the depth-dump breakthrough -- flicker
        // frames are missing their DISTANT WORLD GEOMETRY entirely (depth
        // shows empty far plane where the shore's silhouettes belong; the
        // water covers the hole in colour, which is why the magenta
        // discriminator false-negatived, and the decals merely amplify it by
        // z-passing where the land should have occluded them). This logs any
        // draw identity that is present at t-1, absent at t, present at t+1
        // -- the one-frame vanish -- plus the capture-skip counter deltas for
        // frame t, which separates "game never submitted it" (game-side
        // visibility race) from "our capture dropped it".
        {
          static const bool s_vanish = getenv("RESTUFF_VANISH") != nullptr;
          if (s_vanish) {
            // v4: identity EXCLUDES the index count -- a chunk returning at a
            // different LOD/count previously read as a new draw, hiding its
            // dropout from both the detector and the hold (the far decal
            // field churns its count EVERY frame). vb is the stable anchor.
            auto draw_id = [](const renderer::RawGuestDraw& d) {
              return d.vs_hash ^ (d.ps_hash * 0x9E3779B97F4A7C15ull) ^
                     (uint64_t(d.dbg_vb_phys) << 8);
            };
            struct IdInfo {
              uint64_t vs, ps;
              uint32_t vb, n;
            };
            static std::unordered_map<uint64_t, IdInfo> prev1, prev2;  // t-1, t-2
            static uint64_t s_v_pubs = 0, s_v_vanishes = 0;
            std::unordered_map<uint64_t, IdInfo> cur;
            for (const auto& d : tl.frame) {
              if (d.is_resolve) continue;
              cur.emplace(draw_id(d),
                          IdInfo{d.vs_hash, d.ps_hash, d.dbg_vb_phys, uint32_t(d.idx().size())});
            }
            ++s_v_pubs;
            static uint64_t s_v_static = 0;  // vb!=0 && n>=1000: real world chunks
            // v3: the exactly-one-frame test undercounted -- a race dropout
            // can last several frames and still read as fast flicker. Track
            // last-seen publish per identity; a reappearance after a gap of
            // 1..8 missed publishes is a dropout of that duration. Gap
            // histogram distinguishes flicker (short gaps, high rate) from
            // legit unload/reload (long gaps).
            {
              static std::unordered_map<uint64_t, uint64_t> last_seen;
              static uint64_t s_gap_hist[9] = {};  // [1..8] frames missed
              for (const auto& [id, info] : cur) {
                auto it = last_seen.find(id);
                if (it != last_seen.end()) {
                  const uint64_t gap = s_v_pubs - it->second - 1;
                  if (gap >= 1 && gap <= 8) {
                    ++s_v_vanishes;
                    const bool is_static = info.vb != 0 && info.n >= 1000;
                    if (is_static) {
                      ++s_v_static;
                      ++s_gap_hist[gap];
                      static std::atomic<int> s_v_budget{60};
                      if (s_v_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
                        REXLOG_INFO("[VANISH] STATIC dropout ({} frames): vs={:016X} "
                                    "ps={:016X} vb=0x{:08X} n={} (static total {})",
                                    gap, info.vs, info.ps, info.vb, info.n, s_v_static);
                    }
                  }
                }
                it == last_seen.end() ? (void)last_seen.emplace(id, s_v_pubs)
                                      : (void)(it->second = s_v_pubs);
              }
              if (s_v_pubs % 2048 == 0) {
                // Prune identities not seen for a long time (unloaded).
                for (auto it2 = last_seen.begin(); it2 != last_seen.end();)
                  it2 = (s_v_pubs - it2->second > 600) ? last_seen.erase(it2) : ++it2;
              }
              if (s_v_pubs % 300 == 0)
                REXLOG_INFO("[VANISH] tally: pubs={} static_vanishes={} all_vanishes={} "
                            "frame_draws={} gaps(1..8)=[{},{},{},{},{},{},{},{}]",
                            s_v_pubs, s_v_static, s_v_vanishes, uint32_t(cur.size()),
                            s_gap_hist[1], s_gap_hist[2], s_gap_hist[3], s_gap_hist[4],
                            s_gap_hist[5], s_gap_hist[6], s_gap_hist[7], s_gap_hist[8]);
            }
            // M4.21 (RESTUFF_CHUNK_HOLD=1): THE MITIGATION. The game's
            // visibility worker races its render thread and loses ~randomly
            // (console won on fixed thread timing; scheduling knobs measured
            // ineffective) -- static world chunks skip a frame and the water
            // shows through as the dock flicker. Fill the hole: any static
            // identity drawn at t-1 but absent at t is re-injected from the
            // t-1 capture (shared-ptr payloads still alive; constants at
            // worst one camera-step old ~ sub-pixel at 60fps). A chunk that
            // legitimately unloaded ghosts for exactly ONE frame -- the
            // injected copy never enters the previous-frame map itself.
            {
              static const bool s_hold = getenv("RESTUFF_CHUNK_HOLD") != nullptr;
              if (s_hold) {
                // v2: dropouts can last SEVERAL frames (the v1 one-frame
                // hold demonstrably didn't cover the flicker) -- keep
                // injecting a missing static chunk for up to 5 consecutive
                // frames, then let it go (legit unload).
                struct HoldEnt {
                  renderer::RawGuestDraw d;
                  uint32_t missing = 0;
                };
                static std::unordered_map<uint64_t, HoldEnt> held;
                static uint64_t s_holds = 0;
                for (const auto& d : tl.frame) {
                  if (d.is_resolve || d.dbg_vb_phys == 0) continue;
                  if (uint32_t(d.idx().size()) < 1000) continue;
                  const uint64_t id = draw_id(d);
                  if (!cur.count(id)) continue;
                  auto& e = held[id];
                  e.d = d;
                  e.missing = 0;
                }
                std::vector<const renderer::RawGuestDraw*> inject;
                for (auto& [id, e] : held) {
                  if (cur.count(id)) continue;
                  if (++e.missing <= 8) inject.push_back(&e.d);  // v3: histogram
                                                                // shows gaps to 8
                }
                // Scene-cut guard: a race drops a handful of chunks; a camera
                // cut replaces hundreds. Skip bulk injections (entries still
                // age out via `missing`, so a cut never ghosts).
                if (inject.size() > 24) inject.clear();
                for (const auto* pd : inject) {
                  tl.frame.push_back(*pd);
                  ++s_holds;
                }
                for (auto it2 = held.begin(); it2 != held.end();)
                  it2 = (it2->second.missing > 8) ? held.erase(it2) : ++it2;
                if (!inject.empty()) {
                  static std::atomic<int> s_hold_budget{30};
                  if (s_hold_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
                    REXLOG_INFO("[HOLD] re-injected {} missing static chunk(s) (total {})",
                                uint32_t(inject.size()), s_holds);
                }
              }
            }
            prev2 = std::move(prev1);
            prev1 = std::move(cur);
          }
        }
        // M4.23 (RESTUFF_DECALPOP=1): the user localized the flicker to a
        // DISTANCE CUTOFF in the blob-shadow field -- shadows correct near
        // the dock, cut off beyond a line, flicker beyond it. Track the decal
        // draw population per frame (count + per-draw sizes); the far batch
        // blinking in/out or its count oscillating shows here directly.
        {
          static const bool s_dpop = getenv("RESTUFF_DECALPOP") != nullptr;
          if (s_dpop) {
            constexpr uint64_t kDecalPs3 = 0xBBA590486A51E72Aull;
            uint32_t cnt = 0;
            uint64_t total_n = 0;
            char detail[224] = {};
            int dp = 0;
            for (const auto& d : tl.frame) {
              if (d.is_resolve || d.ps_hash != kDecalPs3) continue;
              ++cnt;
              const uint32_t n = uint32_t(d.idx().size());
              total_n += n;
              if (dp < int(sizeof(detail)) - 32)
                dp += std::snprintf(detail + dp, sizeof(detail) - dp, " (vb=%08X n=%u cm=%X)",
                                    d.dbg_vb_phys, n, d.color_mask);
            }
            static uint32_t prev_cnt = ~0u;
            static uint64_t prev_tn = 0;
            if (cnt != prev_cnt || total_n != prev_tn) {
              static std::atomic<int> s_dpop_budget{80};
              if (s_dpop_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
                REXLOG_INFO("[DECALPOP] count={} total_n={}{}", cnt, total_n, detail);
              prev_cnt = cnt;
              prev_tn = total_n;
            }
          }
        }
        // M4.14 (RESTUFF_CBLIP=1): constant-bank reversion detector. The last
        // uninspected guest data class is per-draw ALU constants -- a light or
        // material row wrong for ONE frame draws that mesh black with
        // perfectly valid geometry. Per draw identity (vs, ps, vb, ordinal),
        // hash the light-constant region (c8..c31, rows the light blocks
        // read) of BOTH banks; an A->X->A sequence is a one-frame anomaly.
        // Keyed snapshots let the diff name the exact constant and values.
        {
          static const bool s_cblip = getenv("RESTUFF_CBLIP") != nullptr;
          if (s_cblip) {
            struct CGen {
              uint64_t h = 0;
              float snap[192];  // vs c8..c31 (96 floats) + ps c8..c31
            };
            struct CEntry {
              CGen cur, prev;
              uint32_t blips = 0;
            };
            static std::unordered_map<uint64_t, CEntry> s_centries;
            static std::unordered_map<uint64_t, uint32_t> s_ordinal;
            static uint64_t s_cblip_total = 0;
            bool event_this_publish = false;
            s_ordinal.clear();
            for (const auto& d : tl.frame) {
              if (d.is_resolve || d.vs_consts.size() < 128 || d.ps_consts.size() < 128)
                continue;
              uint64_t key = d.vs_hash ^ (d.ps_hash * 0x9E3779B97F4A7C15ull) ^
                             (uint64_t(d.dbg_vb_phys) << 20);
              // First occurrence per identity per frame only: deep ordinals
              // shuffle when camera motion reorders the draw list, which broke
              // A->X->A tracking (2 caught events in 7min of fast flicker).
              if (s_ordinal[key]++ != 0) continue;
              CGen g;
              uint64_t h = 1469598103934665603ull;
              for (int ci = 0; ci < 96; ++ci) {
                g.snap[ci] = d.vs_consts[32 + ci];
                h = (h ^ std::bit_cast<uint32_t>(g.snap[ci])) * 1099511628211ull;
              }
              for (int ci = 0; ci < 96; ++ci) {
                g.snap[96 + ci] = d.ps_consts[32 + ci];
                h = (h ^ std::bit_cast<uint32_t>(g.snap[96 + ci])) * 1099511628211ull;
              }
              g.h = h;
              auto& e = s_centries[key];
              if (e.prev.h && h == e.prev.h && h != e.cur.h) {
                ++e.blips;
                ++s_cblip_total;
                event_this_publish = true;
                static std::atomic<int> s_cb_budget{40};
                if (e.blips <= 2 && s_cb_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
                  uint32_t ndiff = 0;
                  char detail[256] = {};
                  int dp = 0;
                  for (int ci = 0; ci < 192; ++ci) {
                    if (std::bit_cast<uint32_t>(e.cur.snap[ci]) ==
                        std::bit_cast<uint32_t>(g.snap[ci]))
                      continue;
                    if (ndiff < 4)
                      dp += std::snprintf(detail + dp, sizeof(detail) - dp,
                                          " (%s c%d.%d was=%.4g now=%.4g)",
                                          ci < 96 ? "vs" : "ps", 8 + (ci % 96) / 4, ci % 4,
                                          e.cur.snap[ci], g.snap[ci]);
                    ++ndiff;
                  }
                  REXLOG_INFO("[CBLIP] vs={:016X} ps={:016X} vb=0x{:08X} blips={} ndiff={}{}",
                              d.vs_hash, d.ps_hash, d.dbg_vb_phys, e.blips, ndiff, detail);
                }
              }
              e.prev = e.cur;
              e.cur = g;
            }
            // RESTUFF_CBLIP_BEEP=1: audible marker per anomaly event, so the
            // user's eyes answer the causation question directly -- does every
            // beep coincide with a visible flicker instant?
#ifdef _WIN32
            static const bool s_cb_beep = getenv("RESTUFF_CBLIP_BEEP") != nullptr;
            if (s_cb_beep && event_this_publish)
              restuff_cblip_beep();  // async system sound (see file-scope decl)
#endif
            static uint64_t s_cb_pub = 0;
            if (++s_cb_pub % 600 == 0 && s_cblip_total)
              REXLOG_INFO("[CBLIP] tally total={} entries={}", s_cblip_total,
                          uint32_t(s_centries.size()));
          }
        }
        // RESTUFF_MERGE_TRACE: log the PUBLISHED frame in the same breath, so
        // chunk-vs-frame counts cannot come from different sample windows (an
        // earlier size-filtered probe hid the tiny publishes and made these two
        // numbers look contradictory).
        if (getenv("RESTUFF_MERGE_TRACE") && consume_n >= merge_at) {
          uint32_t f_col = 0, f_pre = 0, f_coltris = 0, f_res = 0;
          for (const auto& d : tl.frame) {
            if (d.is_resolve) { ++f_res; continue; }
            if (d.color_mask) { ++f_col; f_coltris += uint32_t(d.idx().size() / 3); }
            else ++f_pre;
          }
          static std::atomic<int> s_pf{80};
          if (s_pf.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[FRAME-PUB] size={} colour={} colour_tris={} prepass={} resolves={}",
                        uint32_t(tl.frame.size()), f_col, f_coltris, f_pre, f_res);
        }
      }
    }
  }
  // RESTUFF_BEAR_TRACE=1: is Naughty's draw (vs=D55A8CD6) actually PRESENT in
  // each captured frame? Consecutive still-camera frames show the bear
  // alternating present/absent, and nothing else changes. If the draw is in
  // every frame, the flicker is in its rendering (depth/lighting); if it winks
  // in and out, the flicker is in capture/frame assembly.
  // M3.148 (RESTUFF_VSCOUNT=<hex vs hash>): per-frame census of ONE shader --
  // how many of its draws are in the frame, how many indices they carry, and
  // how many are the colour pass vs the depth prepass. Written because the
  // ground holes are missing geometry from C6C4FBF7D18A3E61 while the capture
  // census reports zero drops, so the question is whether the frame that shows
  // a hole is simply MISSING one of that shader's terrain chunks. Deliberately
  // tiny and allocation-free per draw: the existing heavy dump (DUMP_DRAWS)
  // hits a known bus error in its vertex-dump path and never survives a drive.
  {
    ibwatch::TryLateMs();
    ibwatch::MergeTick();
    static const char* vsc = getenv("RESTUFF_VSCOUNT");
    static const uint64_t want = vsc ? strtoull(vsc, nullptr, 16) : 0;
    if (want && tl.frame.size() > 200) {
      // M3.149b: sample densely (every 5th frame, ~0.3s headless) so a VSHIT
      // line can be matched to a snapped frame BY TIMESTAMP -- correlating the
      // geometry answer with a frame that visibly contains a hole is the whole
      // point, and every-20 was too sparse for that.
      static uint64_t s_vf = 0;
      if ((s_vf++ % 5) == 0) {
        uint32_t n = 0, col = 0, idx = 0;
        for (const auto& d : tl.frame) {
          if (d.is_resolve || d.vs_hash != want) continue;
          ++n;
          if (d.color_mask) ++col;
          idx += uint32_t(d.idx().size());
        }
        REXLOG_INFO("[VSCOUNT] vs={:016X} draws={} colour={} indices={} frame_draws={}", want, n,
                    col, idx, uint32_t(tl.frame.size()));
        // M3.148b: the count is steady, so no chunk is simply absent -- the
        // suspicion is a chunk drawn with WRONG POSITIONS (which would leave a
        // hole where it belongs AND stray geometry where it lands, i.e. both
        // reported symptoms at once). Scan each draw's RAW position attribute
        // for non-finite or absurd values. Bounds-checked and read-only: the
        // heavy DUMP_DRAWS path that does this bus-errors, this must not.
        uint32_t bad = 0, huge = 0;
        for (const auto& d : tl.frame) {
          if (d.is_resolve || d.vs_hash != want || d.streams.empty()) continue;
          const auto* vsb = renderer::spc::GetCachedShader(d.vs_hash);
          if (!vsb || !vsb->valid || vsb->t.attrs.empty()) continue;
          const auto& a = vsb->t.attrs[0];
          if (a.format != 57 && a.format != 38) continue;  // f32x3 / f32x4 only
          const auto& sb = d.streams[0].bytes();
          const uint32_t stride = d.streams[0].stride;
          if (!stride) continue;
          const size_t nv = sb.size() / stride;
          for (size_t v = 0; v < nv; ++v) {
            const size_t off = v * stride + a.byte_offset;
            if (off + 12 > sb.size()) break;
            float p[3];
            std::memcpy(p, sb.data() + off, 12);
            for (float f : p) {
              if (!std::isfinite(f)) { ++bad; break; }
              if (std::fabs(f) > 1.0e7f) { ++huge; break; }
            }
          }
        }
        if (bad || huge)
          REXLOG_INFO("[VSPOS] vs={:016X} NON-FINITE verts={} ABSURD(>1e7) verts={}", want, bad,
                      huge);
        // M3.149 (RESTUFF_VSHIT=<x>,<y> in 1280x720 screen pixels): does ANY of
        // this shader's draws actually COVER a given screen point? That is the
        // decisive question for the ground holes -- if no ground draw's
        // geometry reaches the hole, the guest never issues terrain there
        // (game-side culling/LOD) and the defect is upstream of us; if one does,
        // we are dropping or mis-rendering it and the defect is ours.
        // Transform is the same one the winding census uses: attr0 f32x3
        // position through the c0..c3 columns in vs_consts, then perspective
        // divide, then d.ndc to framebuffer space.
        // M3.150 (RESTUFF_VSGRID=1): same-frame geometry coverage MAP. A fixed
        // probe point cannot track a defect that moves (M3.149b), so instead
        // rasterise every triangle of this shader into a coarse 64x36 grid and
        // log which cells the geometry actually covers. Pair it with
        // RESTUFF_DUMP_SCENE=1 (which copies scene_img for the SAME frame) and
        // the two can be diffed offline: a cell that is a hole in the image but
        // COVERED here means we drop/mis-render present geometry; a cell that
        // is a hole in both means the guest issues no terrain there and the
        // defect is upstream of the renderer.
        // Rasterising per triangle (not per grid point) keeps this affordable:
        // terrain triangles are small, so each touches 0-1 cells.
        static const bool gridon = getenv("RESTUFF_VSGRID") != nullptr;
        if (gridon) {
          constexpr int GW = 64, GH = 36;
          static std::vector<uint8_t> cov;
          cov.assign(GW * GH, 0);
          for (const auto& d : tl.frame) {
            if (d.is_resolve || d.vs_hash != want || d.streams.empty()) continue;
            if (d.vs_consts.size() < 16 || d.prim != 6) continue;
            const auto* cs = renderer::spc::GetCachedShader(d.vs_hash);
            if (!cs || !cs->valid || cs->t.attrs.empty() || cs->t.attrs[0].format != 57) continue;
            const auto& sb = d.streams[0].bytes();
            const uint32_t stride = d.streams[0].stride;
            if (!stride) continue;
            const auto& idx = d.idx();
            auto proj = [&](uint32_t vi, float& ox, float& oy) -> bool {
              const size_t off = size_t(vi) * stride + cs->t.attrs[0].byte_offset;
              if (off + 12 > sb.size()) return false;
              float p[3];
              std::memcpy(p, sb.data() + off, 12);
              float o[4];
              for (int r = 0; r < 4; ++r)
                o[r] = p[0] * d.vs_consts[r] + p[1] * d.vs_consts[4 + r] +
                       p[2] * d.vs_consts[8 + r] + d.vs_consts[12 + r];
              if (!(o[3] > 0.0f) || !std::isfinite(o[3])) return false;
              ox = o[0] / o[3] * d.ndc[0];
              oy = o[1] / o[3] * d.ndc[1];
              return true;
            };
            for (size_t i = 0; i + 2 < idx.size(); ++i) {
              uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
              if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
              if (a == b || b == c || a == c) continue;
              float ax, ay, bx, by, cx, cy;
              if (!proj(a, ax, ay) || !proj(b, bx, by) || !proj(c, cx, cy)) continue;
              // NDC -> grid cells (y-down image order).
              auto gx = [&](float x) { return (x + 1.0f) * 0.5f * GW; };
              auto gy = [&](float y) { return (1.0f - y) * 0.5f * GH; };
              const float x0 = gx(std::min({ax, bx, cx})), x1 = gx(std::max({ax, bx, cx}));
              const float y0 = gy(std::max({ay, by, cy})), y1 = gy(std::min({ay, by, cy}));
              const int ix0 = std::max(0, int(std::floor(x0))), ix1 = std::min(GW - 1, int(x1));
              const int iy0 = std::max(0, int(std::floor(y0))), iy1 = std::min(GH - 1, int(y1));
              for (int yy = iy0; yy <= iy1; ++yy) {
                for (int xx = ix0; xx <= ix1; ++xx) {
                  const float px = (xx + 0.5f) / GW * 2.0f - 1.0f;
                  const float py = 1.0f - (yy + 0.5f) / GH * 2.0f;
                  const float e1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
                  const float e2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
                  const float e3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
                  const bool neg = (e1 < 0) || (e2 < 0) || (e3 < 0);
                  const bool pos = (e1 > 0) || (e2 > 0) || (e3 > 0);
                  if (!(neg && pos)) cov[yy * GW + xx] = 1;
                }
              }
            }
          }
          std::string rows;
          for (int y = 0; y < GH; ++y) {
            for (int x = 0; x < GW; x += 4) {
              uint32_t nib = 0;
              for (int k = 0; k < 4; ++k)
                if (x + k < GW && cov[y * GW + x + k]) nib |= 1u << k;
              rows += "0123456789ABCDEF"[nib];
            }
            rows += '.';
          }
          REXLOG_INFO("[VSGRID] vs={:016X} {}x{} covered={} map={}", want, GW, GH,
                      uint32_t(std::count(cov.begin(), cov.end(), uint8_t(1))), rows);
        }
        // M3.158 (RESTUFF_PSCENSUS=1): which PIXEL shaders does this VS pair
        // with, and how much geometry does each carry? The LOW-tier covering
        // draw uses a DIFFERENT ps (9473F682...) than the FULL-tier one
        // (589B5334...), so the tiers may differ by MATERIAL/TECHNIQUE, not
        // just batching -- if the full-tier ps is absent entirely from a bad
        // boot, shader/technique availability is the coin (task #24).
        // M3.159 (RESTUFF_FRAMECENSUS=1): whole-frame (vs,ps) census. The
        // per-shader TRIANGLE totals are identical between tiers while draw
        // counts differ 4x -- so the terrain geometry is the SAME and the
        // wedges cannot be missing ground. Diff every shader pair in the
        // frame to find what genuinely differs (task #24).
        static const bool framecensus = getenv("RESTUFF_FRAMECENSUS") != nullptr;
        if (framecensus) {
          std::map<std::pair<uint64_t, uint64_t>, std::pair<uint32_t, uint32_t>> all;
          for (const auto& d : tl.frame) {
            if (d.is_resolve) continue;
            auto& e = all[{d.vs_hash, d.ps_hash}];
            e.first += 1;
            e.second += uint32_t(d.idx().size() / 3);
          }
          std::string line;
          for (const auto& [k, e] : all) {
            char buf[96];
            snprintf(buf, sizeof(buf), " %016llX/%016llX:%ux%u", (unsigned long long)k.first,
                     (unsigned long long)k.second, e.first, e.second);
            line += buf;
          }
          REXLOG_INFO("[FRAMECENSUS] pairs={}{}", all.size(), line);
        }
        static const bool pscensus = getenv("RESTUFF_PSCENSUS") != nullptr;
        if (pscensus) {
          std::map<uint64_t, std::pair<uint32_t, uint32_t>> ps;  // ps -> {draws, tris}
          for (const auto& d : tl.frame) {
            if (d.is_resolve || d.vs_hash != want) continue;
            auto& e = ps[d.ps_hash];
            e.first += 1;
            e.second += uint32_t(d.idx().size() / 3);
          }
          std::string line;
          for (const auto& [h, e] : ps) {
            char buf[80];
            snprintf(buf, sizeof(buf), " %016llX:%ux%u", (unsigned long long)h, e.first, e.second);
            line += buf;
          }
          REXLOG_INFO("[PSCENSUS] vs={:016X} shaders={}{}", want, ps.size(), line);
        }
        static const char* hitenv = getenv("RESTUFF_VSHIT");
        if (hitenv) {
          float hx = 0, hy = 0;
          if (sscanf(hitenv, "%f,%f", &hx, &hy) == 2) {
            const float tx = hx / 640.0f - 1.0f;          // -> NDC x
            const float ty = 1.0f - hy / 360.0f;          // -> NDC y (y-down fb)
            uint32_t covering = 0, tested = 0, hits = 0;
            // M3.154c: every 20th probe frame, arm the fate log for the draws
            // found covering the point (consumed by the submit loop below).
            static uint32_t s_vfate_serial = 0;
            g_vshit_cover_ptrs.clear();
            g_vshit_probe_frame = (++s_vfate_serial % 20 == 0);
            for (const auto& d : tl.frame) {
              if (d.is_resolve || d.vs_hash != want || d.streams.empty()) continue;
              if (d.vs_consts.size() < 16 || d.prim != 6) continue;
              const auto* cs = renderer::spc::GetCachedShader(d.vs_hash);
              if (!cs || !cs->valid || cs->t.attrs.empty() || cs->t.attrs[0].format != 57) continue;
              const auto& sb = d.streams[0].bytes();
              const uint32_t stride = d.streams[0].stride;
              if (!stride) continue;
              ++tested;
              float mnx = 1e30f, mny = 1e30f, mxx = -1e30f, mxy = -1e30f;
              const size_t nv = sb.size() / stride;
              for (size_t v = 0; v < nv; ++v) {
                const size_t off = v * stride + cs->t.attrs[0].byte_offset;
                if (off + 12 > sb.size()) break;
                float p[3];
                std::memcpy(p, sb.data() + off, 12);
                float o[4];
                for (int r = 0; r < 4; ++r)
                  o[r] = p[0] * d.vs_consts[r] + p[1] * d.vs_consts[4 + r] +
                         p[2] * d.vs_consts[8 + r] + d.vs_consts[12 + r];
                if (!(o[3] > 0.0f) || !std::isfinite(o[3])) continue;
                const float sx = o[0] / o[3] * d.ndc[0], sy = o[1] / o[3] * d.ndc[1];
                mnx = std::min(mnx, sx); mxx = std::max(mxx, sx);
                mny = std::min(mny, sy); mxy = std::max(mxy, sy);
              }
              if (!(mnx <= tx && tx <= mxx && mny <= ty && ty <= mxy)) continue;
              ++covering;
              // Bbox coverage is far too coarse to answer anything -- hundreds
              // of overlapping terrain chunks contain any given point. Only
              // actual point-in-TRIANGLE containment says whether geometry is
              // really there. Walk the strip properly (restart + degenerates).
              const auto& idx = d.idx();
              float pz[3] = {0, 0, 0}, pw[3] = {0, 0, 0};  // covering tri z/w (M3.154d)
              auto proj = [&](uint32_t vi, float& ox, float& oy, float* oz = nullptr,
                              float* ow = nullptr) -> bool {
                const size_t off = size_t(vi) * stride + cs->t.attrs[0].byte_offset;
                if (off + 12 > sb.size()) return false;
                float p[3];
                std::memcpy(p, sb.data() + off, 12);
                float o[4];
                for (int r = 0; r < 4; ++r)
                  o[r] = p[0] * d.vs_consts[r] + p[1] * d.vs_consts[4 + r] +
                         p[2] * d.vs_consts[8 + r] + d.vs_consts[12 + r];
                if (!(o[3] > 0.0f) || !std::isfinite(o[3])) return false;
                ox = o[0] / o[3] * d.ndc[0];
                oy = o[1] / o[3] * d.ndc[1];
                if (oz) *oz = o[2] / o[3];
                if (ow) *ow = o[3];
                return true;
              };
              for (size_t i = 0; i + 2 < idx.size(); ++i) {
                uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
                if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
                if (a == b || b == c || a == c) continue;
                float ax, ay, bx, by, cx, cy;
                if (!proj(a, ax, ay, &pz[0], &pw[0]) || !proj(b, bx, by, &pz[1], &pw[1]) ||
                    !proj(c, cx, cy, &pz[2], &pw[2]))
                  continue;
                const float d1 = (tx - bx) * (ay - by) - (ax - bx) * (ty - by);
                const float d2 = (tx - cx) * (by - cy) - (bx - cx) * (ty - cy);
                const float d3 = (tx - ax) * (cy - ay) - (cx - ax) * (ty - ay);
                const bool neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                const bool pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                if (!(neg && pos)) {  // point inside this tri
                  ++hits;
                  g_vshit_cover_ptrs.push_back(&d);
                  // M3.154d: the replica only ever checked x/y -- a triangle
                  // whose Z decodes corrupt (the M3.112 fmt-57 class) still
                  // "covers" here while the GPU clipper eats it, which is the
                  // last invisible way for a covering draw to leave a hole.
                  if (g_vshit_probe_frame)
                    REXLOG_INFO("[VSHITZ] vs={:016X} ps={:016X} cm={:X} tris={} tri=({},{},{}) "
                                "z=({:.4f},{:.4f},{:.4f}) w=({:.1f},{:.1f},{:.1f})",
                                d.vs_hash, d.ps_hash, d.color_mask,
                                uint32_t(idx.size() / 3), a, b, c, pz[0], pz[1], pz[2],
                                pw[0], pw[1], pw[2]);
                  break;
                }
              }
            }
            REXLOG_INFO("[VSHIT] vs={:016X} point=({:.0f},{:.0f}) ndc=({:.3f},{:.3f}) "
                        "draws_tested={} bbox_covers={} TRIANGLE_COVERS={}",
                        want, hx, hy, tx, ty, tested, covering, hits);
          }
        }
      }
    }
  }
  if (getenv("RESTUFF_BEAR_TRACE") && tl.frame.size() > 200) {
    static std::atomic<int> s_bt{40};
    if (s_bt.fetch_sub(1, std::memory_order_relaxed) > 0) {
      // Skinned characters = VS with a register-relative fetch (bone lookup).
      // Detect generically instead of hardcoding a hash: which VS draws the
      // bear varies by costume/run, and hardcoding sent me chasing a shader
      // that wasn't even in the frame.
      std::map<uint64_t, std::array<uint32_t, 3>> sk;  // vs -> {prepass, colour, tris}
      for (const auto& d : tl.frame) {
        if (d.is_resolve) continue;
        const auto* vs = renderer::spc::GetCachedShader(d.vs_hash);
        if (!vs || !vs->valid || vs->t.rel_fetch_slot == ~0u) continue;
        auto& e = sk[d.vs_hash];
        (d.color_mask ? e[1] : e[0])++;
        e[2] += uint32_t(d.idx().size() / 3);
      }
      std::string s;
      for (auto& [h, e] : sk) {
        char b[96];
        snprintf(b, sizeof(b), " %016llX:pre=%u,col=%u,tris=%u", (unsigned long long)h, e[0], e[1],
                 e[2]);
        s += b;
      }
      // Per captured frame: how many COLOUR (cm!=0) vs prepass (cm==0) draws.
      // Measured black coverage on consecutive still frames swings 20% -> 70%
      // -> 0%; if the colour-draw count swings with it, the capture is losing
      // colour draws and that IS the blobs.
      uint32_t n_col = 0, n_pre = 0, t_col = 0;
      for (const auto& d : tl.frame) {
        if (d.is_resolve) continue;
        if (d.color_mask) { ++n_col; t_col += uint32_t(d.idx().size() / 3); }
        else ++n_pre;
      }
      REXLOG_INFO("[BEAR] framedraws={} colour_draws={} colour_tris={} prepass_draws={} skinned{}",
                  uint32_t(tl.frame.size()), n_col, t_col, n_pre, s.empty() ? " NONE" : s);
    }
  }
  // M3.15: adopt pipelines the worker finished since last frame.
  {
    std::lock_guard<std::mutex> lk(tl.pipe_mutex);
    if (!tl.pipe_done.empty())
      tl.pipe_publish_count.fetch_add(tl.pipe_done.size(), std::memory_order_relaxed);
    for (const auto& [k, p] : tl.pipe_done) {
      // M4.0: with the pre-warm replayer as a second request producer, a key
      // can (rarely) be built twice -- e.g. built live, drained, then replayed.
      // Never clobber a live entry (draws recorded this frame may reference
      // it); destroy the redundant duplicate instead. A cached null upgrades.
      auto [it, fresh] = tl.pipelines.try_emplace(k, p);
      if (!fresh && it->second != p) {
        if (it->second == VK_NULL_HANDLE) {
          it->second = p;
        } else if (p != VK_NULL_HANDLE) {
          dev->functions().vkDestroyPipeline(dev->device(), p, nullptr);
        }
      }
      tl.pipe_inflight.erase(k);
    }
    tl.pipe_done.clear();
    // M3.154b (RESTUFF_PIPESTAT=1): the remaining invisible permanent-nopipe
    // mechanism is a request that never publishes AT ALL (key rots in
    // pipe_inflight, re-enqueue suppressed). Periodically dump the queue
    // shape and how many cached entries are null; a nonzero stable inflight
    // long after load = stuck requests, nulls = failed builds (named at
    // publish time by the M3.154 warns).
    static const bool s_pipestat = getenv("RESTUFF_PIPESTAT") != nullptr;
    if (s_pipestat) {
      static uint64_t s_ps_frame = 0;
      if (++s_ps_frame % 600 == 0) {
        size_t nulls = 0;
        for (const auto& [k, p] : tl.pipelines)
          if (p == VK_NULL_HANDLE) ++nulls;
        REXLOG_INFO("[PIPESTAT] frame={} pipelines={} nulls={} inflight={} queue={}",
                    s_ps_frame, tl.pipelines.size(), nulls, tl.pipe_inflight.size(),
                    tl.pipe_queue.size());
      }
    }
  }
  if (tl.frame.empty()) return recs;
  _pp_b = _pp_now();  // M3.135c: raw-frame consume done
  // M3.59: per-shader majority sign of the WVP determinant over this frame's
  // cull-back (non-skinned) draws. Mirrored instances (negative-determinant
  // world matrix -> inverted winding -> backface-culled) are the sign MINORITY
  // for their shader and get flipped in GetOrCreateTranslatedPipeline. A split
  // below the majority threshold means det(c) isn't this shader's WVP, so it is
  // left ambiguous (no flip). Renders the missing floor without disturbing the
  // correctly-wound majority. Opt-in via RESTUFF_WINDDET for A/B.
  {
    static const bool s_wd = getenv("RESTUFF_WINDDET") != nullptr;
    if (s_wd) {
      tl.wind_majority.clear();
      std::unordered_map<uint64_t, std::pair<int, int>> tally;  // vs -> {pos,neg}
      for (const auto& d : tl.frame) {
        if (d.is_resolve || !(d.su_mode & 2)) continue;
        const auto* vs = renderer::spc::GetCachedShader(d.vs_hash);
        if (vs && (vs->t.rel_fetch_slot != ~0u || vs->t.rel_fetch_slot2 != ~0u)) continue;
        const int s = WvpDetSign(d.vs_consts);
        if (s > 0) tally[d.vs_hash].first++;
        else if (s < 0) tally[d.vs_hash].second++;
      }
      for (const auto& [h, pn] : tally) {
        const int tot = pn.first + pn.second, maj = std::max(pn.first, pn.second);
        if (tot >= 2 && maj * 100 >= tot * 60) {  // trust only a clear majority
          tl.wind_majority[h] = pn.first >= pn.second ? 1 : -1;
          const int minority = std::min(pn.first, pn.second);
          if (minority > 0) {
            static std::atomic<int> s_wb{30};
            if (s_wb.fetch_sub(1, std::memory_order_relaxed) > 0)
              REXLOG_INFO("[WINDDET] vs={:016X} pos={} neg={} -> flip {} mirrored draw(s)",
                          h, pn.first, pn.second, minority);
          }
        }
      }
    }
  }
  // M3.70 (RESTUFF_ICOUNT=1): per-frame (vb, n) instance multiplicity of
  // cull-back strips + distinct placements (c3.x bit pattern) -- instance-loss
  // detection against the reference trace's multi-instance draws.
  {
    static const bool s_ic = getenv("RESTUFF_ICOUNT") != nullptr;
    if (s_ic) {
      static std::atomic<int> s_icb{10};
      std::map<std::pair<uint32_t, uint32_t>, int> mult;
      std::map<std::pair<uint32_t, uint32_t>, std::set<uint32_t>> places;
      for (const auto& d : tl.frame) {
        if (d.is_resolve || d.prim != 6 || !(d.su_mode & 2) || !d.color_mask) continue;
        const auto k = std::make_pair(d.dbg_vb_phys, uint32_t(d.idx().size()));
        mult[k]++;
        if (d.vs_consts.size() > 15)
          places[k].insert(std::bit_cast<uint32_t>(d.vs_consts[12]));
      }
      bool any = false;
      for (const auto& [k, m] : mult)
        if (m > 1 && k.second > 500) any = true;
      // Spend the log budget only on frames that HAVE multi-instance rows
      // (menu frames have none and must not exhaust it).
      if (any && s_icb.fetch_sub(1, std::memory_order_relaxed) > 0) {
        for (const auto& [k, m] : mult)
          if (m > 1 && k.second > 500)
            REXLOG_INFO("[ICOUNT] vb={:08X} n={} inst={} places={}", k.first, k.second, m,
                        uint32_t(places[k].size()));
      }
    }
  }
  // M3.72 (RESTUFF_DRAWLIST=1): one-shot ordered identity dump of a few
  // gameplay frames -- maps a CAP_FILE luminance-cliff index to the exact
  // draw (the [CAPEDGE] per-cap budget can expire before the cap of
  // interest). One line per draw: seen index, shaders, prim, cull, masks,
  // blend, index count, textures.
  {
    // File-triggered (like CAP_FILE): the interesting frame is wherever the
    // drive is STANDING when the harness touches the file, not first-gameplay.
    static const char* s_dlf = getenv("RESTUFF_DRAWLIST_FILE");
    if (s_dlf && tl.frame.size() > 200) {
      static std::atomic<int> s_dlb{2};
      FILE* dlf = fopen(s_dlf, "rb");
      if (dlf) fclose(dlf);
      if (dlf && s_dlb.fetch_sub(1, std::memory_order_relaxed) > 0) {
        uint32_t seen = 0;
        for (const auto& d : tl.frame) {
          if (d.is_resolve) continue;
          ++seen;
          // Full our-space winding census: every strip triangle's screen sign
          // (attr0-f32x3 position via c0..c3 columns, pc.ndc applied, y-down
          // framebuffer shoelace), plus a content fingerprint (FNV of the
          // widened indices) so reference-trace draws can be matched by
          // CONTENT at the same camera and their censuses compared 1:1.
          uint32_t npos = 0, nneg = 0;
          double apos = 0.0, aneg = 0.0;
          uint64_t ch = 1469598103934665603ull;
          const auto* cs = renderer::spc::GetCachedShader(d.vs_hash);
          const bool census_ok = d.prim == 6 && cs && cs->valid && !cs->t.attrs.empty() &&
                                 cs->t.attrs[0].format == 57 && cs->t.attrs[0].byte_offset == 0 &&
                                 !d.streams.empty() && d.vs_consts.size() >= 16;
          if (census_ok) {
            const auto& idx = d.idx();
            const auto& sb = d.streams[0].bytes();
            const uint32_t stride = d.streams[0].stride;
            for (uint32_t v : idx) ch = (ch ^ v) * 1099511628211ull;
            size_t seg = 0;
            for (size_t i = 0; i + 2 < idx.size(); ++i) {
              uint32_t a = idx[i], b = idx[i + 1], c = idx[i + 2];
              if (a == 0xFFFFFFFFu) { seg = i + 1; continue; }
              if (b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) continue;
              if (a == b || b == c || a == c) continue;
              if (((i - seg) & 1) == 1) std::swap(a, b);
              float sx[3], sy[3];
              bool good = true;
              const uint32_t vv[3] = {a, b, c};
              for (int k = 0; k < 3 && good; ++k) {
                const size_t off = size_t(vv[k]) * stride;
                if (off + 12 > sb.size()) { good = false; break; }
                const float* p = reinterpret_cast<const float*>(sb.data() + off);
                float o[4];
                for (int r = 0; r < 4; ++r)
                  o[r] = p[0] * d.vs_consts[r] + p[1] * d.vs_consts[4 + r] +
                         p[2] * d.vs_consts[8 + r] + d.vs_consts[12 + r];
                if (o[3] <= 0.0f) { good = false; break; }
                const float gx = o[0] * d.ndc[0] + d.ndc[2] * o[3];
                const float gy = o[1] * d.ndc[1] + d.ndc[3] * o[3];
                sx[k] = gx / o[3];
                sy[k] = gy / o[3];
              }
              if (!good) continue;
              const float a2 =
                  (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
              if (a2 > 0) { ++npos; apos += a2; }
              else if (a2 < 0) { ++nneg; aneg += -a2; }
            }
          }
          // M3.84: the glow quad's VS is a PASSTHROUGH (pretransformed verts
          // from the 0x05 dynamic ring) -- the census matrix math above is
          // invalid for it. Print the raw captured verts to see whether the
          // guest wrote a real quad or zeros/garbage.
          if ((d.ps_hash == 0xA17EC3C3A107872Bull || d.ps_hash == 0x9585B8F9EC2B8F95ull) &&
              !d.streams.empty()) {
            REXLOG_INFO("[GLOWVTX] ps={:016X} sq_program_cntl={:08X} sq_context_misc={:08X} "
                        "ci={:08X} di={:08X} t1=0x{:08X} f{} {}x{} t2=0x{:08X}",
                        d.ps_hash, d.sq_program_cntl, d.sq_context_misc, d.dbg_color_info,
                        d.dbg_depth_info, d.tex[1].phys_addr, d.tex[1].format, d.tex[1].width,
                        d.tex[1].height, d.tex[2].phys_addr);
            const auto& sb = d.streams[0].bytes();
            const uint32_t stride = d.streams[0].stride;
            for (uint32_t v = 0; v < 4 && (size_t(v) + 1) * stride <= sb.size(); ++v) {
              const float* p = reinterpret_cast<const float*>(sb.data() + size_t(v) * stride);
              REXLOG_INFO("[GLOWVTX] v{} stride={} = ({:.3f},{:.3f},{:.3f},{:.3f}) ({:.3f},{:.3f},{:.3f},{:.3f})",
                          v, stride, p[0], p[1], p[2], p[3],
                          stride >= 32 ? p[4] : 0.0f, stride >= 32 ? p[5] : 0.0f,
                          stride >= 32 ? p[6] : 0.0f, stride >= 32 ? p[7] : 0.0f);
            }
          }
          // M3.78: FNV over the light-constant region (c8..c31 = rows the PS
          // light blocks read) -- joined by content ch against the reference
          // trace to find draws whose ASSIGNED LIGHT SET diverges.
          uint64_t lch = 1469598103934665603ull;
          if (d.vs_consts.size() >= 128) {
            for (int ci = 32; ci < 128; ++ci)
              lch = (lch ^ std::bit_cast<uint32_t>(d.vs_consts[ci])) * 1099511628211ull;
          } else {
            lch = 0;
          }
          REXLOG_INFO("[DRAWLIST] seen={} vs={:016X} ps={:016X} prim={} n={} su={:X} cm={:X} "
                      "bl={:08X} t0={:08X} ch={:016X} lch={:016X} cw={} ccw={} acw={:.4f} "
                      "accw={:.4f} ci={:03X} di={:05X} dc={:X} zsc={:.0f} c3=({:.3f},{:.3f},{:.3f},{:.3f})",
                      seen, d.vs_hash, d.ps_hash, d.prim, uint32_t(d.idx().size()), d.su_mode,
                      d.color_mask, d.blend_control, d.tex[0].phys_addr, census_ok ? ch : 0, lch,
                      npos, nneg, apos, aneg, d.dbg_color_info & 0xFFF, d.dbg_depth_info,
                      d.depth_control, d.vport_zscale,
                      d.vs_consts.size() >= 16 ? d.vs_consts[12] : 0.0f,
                      d.vs_consts.size() >= 16 ? d.vs_consts[13] : 0.0f,
                      d.vs_consts.size() >= 16 ? d.vs_consts[14] : 0.0f,
                      d.vs_consts.size() >= 16 ? d.vs_consts[15] : 0.0f);
          // Lighting-state prong: one constant-block dump per frame for the
          // terrain family -- if the game dimmed ITS OWN light constants in
          // response to sun-occlusion feedback, rows 8+ differ vs the
          // emulated trace at the same view.
          static std::atomic<int> s_cdump{2};
          // M3.96: RESTUFF_DRAWCONST_PS=<hex64> retargets the dump (default
          // stays the glow quad).
          static const uint64_t s_cdump_ps = [] {
            const char* e = getenv("RESTUFF_DRAWCONST_PS");
            return e ? strtoull(e, nullptr, 16) : 0xA17EC3C3A107872Bull;
          }();
          if (d.ps_hash == s_cdump_ps && d.vs_consts.size() >= 128 &&
              s_cdump.fetch_sub(1, std::memory_order_relaxed) > 0) {
            for (int r = 0; r < 32; r += 2)
              REXLOG_INFO("[DRAWCONST] c{}=({:.6f},{:.6f},{:.6f},{:.6f}) c{}=({:.6f},{:.6f},{:.6f},{:.6f})",
                          r, d.vs_consts[r * 4], d.vs_consts[r * 4 + 1], d.vs_consts[r * 4 + 2],
                          d.vs_consts[r * 4 + 3], r + 1, d.vs_consts[r * 4 + 4],
                          d.vs_consts[r * 4 + 5], d.vs_consts[r * 4 + 6], d.vs_consts[r * 4 + 7]);
            // M3.76: the SQ bool constants gate the PS light blocks
            // (reference at the gate: word4 = 0x0000000F). A mismatch here =
            // the terrain lights never execute = the global dim.
            REXLOG_INFO("[DRAWCONST] bool={:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                        d.bool_consts[0], d.bool_consts[1], d.bool_consts[2], d.bool_consts[3],
                        d.bool_consts[4], d.bool_consts[5], d.bool_consts[6], d.bool_consts[7]);
            // M3.79: PS constant bank rows 0-15 (the PS light colors) --
            // the last never-compared input bank.
            if (d.ps_consts.size() >= 1024) {
              uint64_t ph = 1469598103934665603ull;
              for (int ci = 160; ci < 1012; ++ci)
                ph = (ph ^ std::bit_cast<uint32_t>(d.ps_consts[ci])) * 1099511628211ull;
              REXLOG_INFO("[DRAWCONST] PHASH={:016X}", ph);
              for (int r2 = 40; r2 < 253; r2 += 2) {
                const float* a = d.ps_consts.data() + r2 * 4;
                if (a[0] || a[1] || a[2] || a[3] || a[4] || a[5] || a[6] || a[7])
                  REXLOG_INFO("[DRAWCONST] q{}=({:.6f},{:.6f},{:.6f},{:.6f}) q{}=({:.6f},{:.6f},{:.6f},{:.6f})",
                              r2, a[0], a[1], a[2], a[3], r2 + 1, a[4], a[5], a[6], a[7]);
              }
              REXLOG_INFO("[DRAWCONST] p253=({:.6f},{:.6f},{:.6f},{:.6f}) p254=({:.6f},{:.6f},{:.6f},{:.6f}) p255=({:.6f},{:.6f},{:.6f},{:.6f})",
                          d.ps_consts[253*4], d.ps_consts[253*4+1], d.ps_consts[253*4+2], d.ps_consts[253*4+3],
                          d.ps_consts[254*4], d.ps_consts[254*4+1], d.ps_consts[254*4+2], d.ps_consts[254*4+3],
                          d.ps_consts[255*4], d.ps_consts[255*4+1], d.ps_consts[255*4+2], d.ps_consts[255*4+3]);
            }
            if (d.ps_consts.size() >= 64) {
              for (int r = 0; r < 40; r += 2)
                REXLOG_INFO("[DRAWCONST] p{}=({:.6f},{:.6f},{:.6f},{:.6f}) p{}=({:.6f},{:.6f},{:.6f},{:.6f})",
                            r, d.ps_consts[r * 4], d.ps_consts[r * 4 + 1], d.ps_consts[r * 4 + 2],
                            d.ps_consts[r * 4 + 3], r + 1, d.ps_consts[r * 4 + 4],
                            d.ps_consts[r * 4 + 5], d.ps_consts[r * 4 + 6],
                            d.ps_consts[r * 4 + 7]);
            }
          }
        }
        REXLOG_INFO("[DRAWLIST] end frame draws={}", seen);
      }
    }
  }
  // M3.60 (RESTUFF_WINDPROBE=1): classify cull-back draws' true winding by
  // running their own transform on sampled triangles (see RunWindProbe). A few
  // probes per frame; results cache by draw identity, and every draw gets its
  // decision stamped in wind_flip_hint before pipeline lookup below.
  {
    // M3.71: the probe compensated for the legacy inverted front-face base;
    // with the reference-exact face rule the hint would actively HARM
    // mixed-winding strips, so probing runs only under RESTUFF_LEGACY_WINDING
    // (where M3.60 remains default-on; opt-out RESTUFF_NO_WINDPROBE=1).
    static const bool s_wprobe = getenv("RESTUFF_LEGACY_WINDING") != nullptr &&
                                 getenv("RESTUFF_NO_WINDPROBE") == nullptr;
    // RESTUFF_WINDPROBE_ALL=1: ALSO probe cull-none draws -- measurement-only
    // sign-convention validation (correct visible sheets should read ~100%
    // front). The flip is still applied ONLY to cull-back draws (gated on
    // su_mode&2 at the pipeline key), so this cannot change rendering.
    static const bool s_wprobe_all = getenv("RESTUFF_WINDPROBE_ALL") != nullptr;
    if (s_wprobe || s_wprobe_all) {
      int budget = 4;  // probes per frame (each ~a fence-waited dispatch)
      uint32_t n_gate = 0, n_cull = 0, n_shader = 0, n_cached = 0, n_probed = 0;
      for (auto& d : tl.frame) {
        if (d.is_resolve || d.prim != 6 || !d.color_mask) {
          ++n_gate;
          continue;
        }
        if (!(d.su_mode & 2) && !s_wprobe_all) {
          ++n_cull;
          continue;
        }
        const auto* vs = renderer::spc::GetCachedShader(d.vs_hash);
        if (!vs || !vs->valid || vs->probe_spirv.empty()) {  // incl. skinned
          ++n_shader;
          continue;
        }
        const uint64_t k = WindProbeKey(d);
        if (auto it = g_wp.cls.find(k); it != g_wp.cls.end()) {
          if (it->second >= 0) {  // classified
            d.wind_flip_hint = it->second;
            ++n_cached;
            continue;
          }
          if (it->second <= -3) {  // gave up
            d.wind_flip_hint = 0;
            ++n_cached;
            continue;
          }
        }
        if (budget <= 0) continue;
        --budget;
        ++n_probed;
        d.wind_flip_hint = RunWindProbe(dev, d, *vs, k);
      }
      // Periodic visibility: where do the frame's draws land in the gates?
      static std::atomic<uint32_t> s_wpsum{0};
      if ((s_wpsum.fetch_add(1, std::memory_order_relaxed) % 300) == 0)
        REXLOG_INFO("[WPROBE=] frame={} gate={} cullskip={} noprobe={} cached={} probed={}",
                    uint32_t(tl.frame.size()), n_gate, n_cull, n_shader, n_cached, n_probed);
    }
  }
  // M4.37: LRU/budget eviction. Deliberately placed HERE -- after this slot's
  // fence wait, before any TexEntry* is taken into resolved[] below, and
  // immediately before the epoch check so this frame's evictions are folded
  // into the same combo-pool reset (one reset however many entries went).
  TexEvictSweep(dev);
  // M3.0: texture views were retired since this SLOT's last frame (content
  // re-decode) -- this slot's fence has been waited by now (serialized: last
  // frame's wait; pipelined: the top-of-frame wait), so reset ITS combo pools
  // before any stale view handle can be recycled into a colliding key.
  // M4.5: per-slot epoch compare replaces the shared bool.
  if (tl.cur().combo_epoch != DL().tex_retire_epoch) {
    tl.cur().combo_epoch = DL().tex_retire_epoch;
    for (VkDescriptorPool pool : tl.cur().tex_pools)
      dev->functions().vkResetDescriptorPool(dev->device(), pool, 0);
    tl.cur().tex_combos.clear();
  }

  // One-shot draw dump (RESTUFF_DUMP_DRAWS=1, frame index RESTUFF_DUMP_FRAME).
  // Prints each draw's identity + on-screen NDC extent of its position attr so a
  // "short" backdrop (not reaching an edge) is directly visible. Debug only.
  {
    static const char* dump_env = getenv("RESTUFF_DUMP_DRAWS");
    static uint64_t s_prep_frame = 0;
    const uint64_t this_frame = s_prep_frame++;
    // Periodic snapshots (every RESTUFF_DUMP_EVERY frames, default 250) so a
    // settled frame is caught regardless of the (slow, headless) frame rate.
    static const uint64_t every =
        getenv("RESTUFF_DUMP_EVERY") ? strtoull(getenv("RESTUFF_DUMP_EVERY"), nullptr, 10) : 250;
    static int s_dumps_left = 500;
    const bool fire = dump_env && this_frame > 0 && every && (this_frame % every) == 0 &&
                      s_dumps_left-- > 0;
    if (fire) {
      auto comps = [](uint32_t f) -> uint32_t {
        switch (f) { case 6: case 26: case 32: case 35: case 38: return 4;
          case 57: return 3; case 25: case 31: case 34: case 37: return 2;
          case 33: case 36: return 1; default: return 4; } };
      auto isflt = [](uint32_t f) { return f==36||f==37||f==57||f==38||f==31||f==32; };
      auto bytesz = [](uint32_t f) -> uint32_t {
        switch (f) { case 6: return 1; case 25: case 26: case 31: case 32: return 2;
          default: return 4; } };
      (void)bytesz;
      REXLOG_INFO("[DUMP] frame {} draws={}", this_frame, tl.frame.size());
      // Duplicate census: exact-payload duplicates within one frame are the
      // signature of the PM4 walker re-walking a stream section (park/resume).
      {
        auto fnv = [](const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull) {
          for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
          return h;
        };
        std::unordered_map<uint64_t, int> counts;
        for (const auto& dd : tl.frame) {
          if (dd.is_resolve) continue;
          uint64_t h = fnv(reinterpret_cast<const uint8_t*>(&dd.vs_hash), 8);
          h = fnv(reinterpret_cast<const uint8_t*>(&dd.ps_hash), 8, h);
          for (const auto& s : dd.streams)
            if (s.data && !s.bytes().empty()) h = fnv(s.bytes().data(), s.bytes().size(), h);
          if (!dd.vs_consts.empty())
            h = fnv(reinterpret_cast<const uint8_t*>(dd.vs_consts.data()),
                    dd.vs_consts.size() * 4, h);
          ++counts[h];
        }
        int dupes = 0, groups = 0;
        for (auto& kv : counts)
          if (kv.second > 1) { ++groups; dupes += kv.second - 1; }
        REXLOG_INFO("[DUMP]   DUPES: {} extra copies in {} groups (of {} draws)", dupes, groups,
                    tl.frame.size());
      }
      uint32_t di = 0;
      for (const auto& d : tl.frame) {
        // Per-draw register context (stamped at capture): viewport, surface
        // pitch/msaa, window offset, scissor. Resolves get a dedicated row.
        const auto vpf = [&](int k) { float f; std::memcpy(&f, &d.dbg_vport[k], 4); return f; };
        const auto regctx = [&] {
          return fmt::format("VP=({:.0f},{:.0f},{:.0f},{:.0f}) surf={}/m{} wo=0x{:08X} sc=({},{})-({},{})",
              vpf(0), vpf(1), vpf(2), vpf(3), d.dbg_surf & 0x3FFF, (d.dbg_surf >> 16) & 3,
              d.dbg_winoff, d.dbg_sciss_tl & 0x7FFF, (d.dbg_sciss_tl >> 16) & 0x7FFF,
              d.dbg_sciss_br & 0x7FFF, (d.dbg_sciss_br >> 16) & 0x7FFF);
        };
        if (d.is_resolve) {
          REXLOG_INFO("[DUMP] #{:<3} RESOLVE dest=0x{:08X} {}x{} rect=({},{} {}x{}) ctl=0x{:08X} clearC=0x{:08X} clearD=0x{:08X} {}",
              di, d.copy_dest, d.copy_w, d.copy_h, d.copy_rx, d.copy_ry, d.copy_rw, d.copy_rh,
              d.dbg_copyctl, d.dbg_color_clear, d.dbg_depth_clear, regctx());
          ++di;
          continue;
        }
        const auto* vs = renderer::spc::GetCachedShader(d.vs_hash);
        // Debug rows inspect stream 0 (the position stream by construction).
        static const std::vector<uint8_t> kNullBytes;
        const renderer::VtxStream* s0p = d.streams.empty() ? nullptr : &d.streams[0];
        const std::vector<uint8_t>& s0v_bytes = (s0p && s0p->data) ? s0p->bytes() : kNullBytes;
        struct { uint32_t stride; } s0v = {s0p ? s0p->stride : 0u};
        const uint32_t stride = s0v.stride ? s0v.stride : 1;
        const uint32_t vcount = uint32_t(s0v_bytes.size() / stride);
        // Locate the position attribute (GLSL input location 0) and its flags.
        bool pfound=false; uint32_t pfmt=0, poff=0; bool psgn=false, pnrm=false;
        if (vs && vs->valid) {
          const auto* sel = vs->t.attrs.empty() ? nullptr : &vs->t.attrs[0];
          for (const auto& a : vs->t.attrs) { if (a.location == 0) { sel = &a; break; } }
          if (sel) { pfound=true; pfmt=sel->format; poff=sel->byte_offset; psgn=sel->is_signed; pnrm=sel->is_normalized; }
        }
        float nx0=1e9f,nx1=-1e9f,ny0=1e9f,ny1=-1e9f, px0=1e9f,px1=-1e9f,py0=1e9f,py1=-1e9f;
        const bool is_ee34 = (d.vs_hash == 0xEE34A8BA31895FACull);
        float c0[4]={0,0,0,0}, c1[4]={0,0,0,0};
        if (is_ee34 && d.vs_consts.size() >= 8) {
          for (int k=0;k<4;++k){ c0[k]=d.vs_consts[k]; c1[k]=d.vs_consts[4+k]; }
        }
        if (pfound) {
          const uint32_t nc = comps(pfmt);
          const uint32_t bs = bytesz(pfmt);
          for (uint32_t v=0; v<vcount; ++v) {
            const uint8_t* base = s0v_bytes.data() + size_t(v)*stride + poff;
            if (base + nc*bs > s0v_bytes.data()+s0v_bytes.size()) break;
            float p[4]={0,0,0,1};
            if (isflt(pfmt) && bs==4) {
              for (uint32_t k=0;k<nc;++k) std::memcpy(&p[k], base+k*4, 4);
            } else if (pfmt==25 || pfmt==26) {  // k_16_16(_16_16)
              for (uint32_t k=0;k<nc;++k) {
                int16_t s; uint16_t u; std::memcpy(&s, base+k*2, 2); std::memcpy(&u, base+k*2, 2);
                if (pnrm) p[k] = psgn ? std::max(float(s)/32767.0f,-1.0f) : float(u)/65535.0f;
                else p[k] = psgn ? float(s) : float(u);
              }
            } else if (bs==4) {
              for (uint32_t k=0;k<nc;++k) std::memcpy(&p[k], base+k*4, 4);
            }
            const float w = nc>=4 ? p[3] : 1.0f;
            px0=std::min(px0,p[0]); px1=std::max(px1,p[0]);
            py0=std::min(py0,p[1]); py1=std::max(py1,p[1]);
            float gx, gy;
            if (is_ee34) {  // oPos.x = c0.w + c0.x*p.y + c0.y*p.x; NDC=oPos (w=1), Y flip
              const float ox = c0[3] + c0[0]*p[1] + c0[1]*p[0];
              const float oy = c1[3] + c1[0]*p[1] + c1[1]*p[0];
              gx = ox; gy = -oy;
            } else {
              gx = p[0]*d.ndc[0] + d.ndc[2]*w;
              gy = p[1]*d.ndc[1] + d.ndc[3]*w;
            }
            nx0=std::min(nx0,gx); nx1=std::max(nx1,gx);
            ny0=std::min(ny0,gy); ny1=std::max(ny1,gy);
          }
        }
        const bool pretrans = !(d.ndc[0]==1.0f && d.ndc[1]==-1.0f && d.ndc[2]==0.0f && d.ndc[3]==0.0f);
        REXLOG_INFO("[DUMP] #{:<3} prim={} vtx={} idx={} vs={:016X} ps={:016X} blend={} cmask={:X} pretrans={} tex0[0x{:08X} {}x{} fmt{} v{} cl{}{}] pfmt={} sgn={} nrm={} POSraw x[{:.1f},{:.1f}] y[{:.1f},{:.1f}] NDC x[{:.3f},{:.3f}] y[{:.3f},{:.3f}] {}",
          di, d.prim, vcount, uint32_t(d.idx().size()), d.vs_hash, d.ps_hash, int(d.blend), d.color_mask, pretrans?1:0,
          d.tex[0].phys_addr, d.tex[0].width, d.tex[0].height, d.tex[0].format, d.tex[0].valid?1:0,
          d.tex[0].clamp_x, d.tex[0].clamp_y,
          pfmt, psgn?1:0, pnrm?1:0, px0,px1,py0,py1, nx0,nx1,ny0,ny1, regctx());
        if (is_ee34)
          REXLOG_INFO("[DUMP]     c0=({:.8f},{:.8f},{:.8f},{:.8f}) c1=({:.8f},{:.8f},{:.8f},{:.8f})",
            c0[0],c0[1],c0[2],c0[3],c1[0],c1[1],c1[2],c1[3]);
        // World forward-lit PS (1BAB95FE et al.) reads high PS constants
        // c[253..255] for lighting. If those are zero the lighting collapses.
        if (d.vs_hash == 0xC6C4FBF7D18A3E61ull && d.ps_consts.size() >= 1024) {
          auto pc = [&](int v, int k) { return d.ps_consts[v * 4 + k]; };
          REXLOG_INFO("[DUMP]     PSC c6=({:.3f},{:.3f},{:.3f},{:.3f}) c253=({:.3f},{:.3f},{:.3f},{:.3f}) "
                      "c254=({:.3f},{:.3f},{:.3f},{:.3f}) c255=({:.3f},{:.3f},{:.3f},{:.3f})",
                      pc(6,0),pc(6,1),pc(6,2),pc(6,3), pc(253,0),pc(253,1),pc(253,2),pc(253,3),
                      pc(254,0),pc(254,1),pc(254,2),pc(254,3), pc(255,0),pc(255,1),pc(255,2),pc(255,3));
        }
        // C53C hills mesh (untextured, vertex-coloured, stride 8 = pos u32 +
        // colour u32): print a few vertices to see why it renders invisible.
        if (d.vs_hash == 0xC53CCCAFA9A44EBDull && s0v.stride == 8 &&
            s0v_bytes.size() >= 6 * 8) {
          const uint8_t* live = renderer::DebugGuestPhysPtr(d.dbg_vb_phys);
          for (uint32_t v = 0; v < 6; ++v) {
            const uint8_t* p = s0v_bytes.data() + v * 8;
            int16_t px, py; std::memcpy(&px, p, 2); std::memcpy(&py, p + 2, 2);
            char lv[48] = "";
            if (live) {
              // Live guest dword (BE) -> match capture's word-reverse.
              const uint8_t* q = live + v * 8 + 4;
              snprintf(lv, sizeof(lv), " live_col=(%u,%u,%u,%u)", q[3], q[2], q[1], q[0]);
            }
            REXLOG_INFO("[DUMP]     C53C v{}: pos=({},{}) col=({},{},{},{}){}", v, px, py,
                        p[4], p[5], p[6], p[7], lv);
          }
        }
        // D586 background-fill quad: stride 28 = pos float3 + colour float3(+spare).
        // Print vertex 0 raw floats to verify the fill colour decode.
        if (d.vs_hash == 0xD586AD25909212F7ull && s0v.stride >= 24 &&
            s0v_bytes.size() >= s0v.stride) {
          const uint32_t nv = std::min<uint32_t>(4, uint32_t(s0v_bytes.size() / s0v.stride));
          for (uint32_t v = 0; v < nv; ++v) {
            float f[7] = {0};
            std::memcpy(f, s0v_bytes.data() + size_t(v) * s0v.stride,
                        std::min<size_t>(28, s0v.stride));
            REXLOG_INFO("[DUMP]     D586 v{} floats: pos=({:.2f},{:.2f},{:.2f}) col=({:.4f},{:.4f},{:.4f}) a={:.4f}",
                        v, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
          }
          // Live re-read: the game may commit UP payloads after the walker
          // captured them (BE dwords in guest memory -- swap before printing).
          if (const uint8_t* live = renderer::DebugGuestPhysPtr(d.dbg_vb_phys)) {
            float lf[7];
            for (int k = 0; k < 7; ++k) {
              uint32_t w; std::memcpy(&w, live + k * 4, 4);
              w = std::byteswap(w);
              std::memcpy(&lf[k], &w, 4);
            }
            REXLOG_INFO("[DUMP]     D586 v0 LIVE:   pos=({:.2f},{:.2f},{:.2f}) col=({:.4f},{:.4f},{:.4f}) spare={:.4f} (vb=0x{:08X})",
                        lf[0], lf[1], lf[2], lf[3], lf[4], lf[5], lf[6], d.dbg_vb_phys);
          }
        }
        // c4/c5 are the UV rows: o_0.x = dot(r1.wzxy, c4.wzxy), .y = ..c5..
        // (r1 = (pos.y, pos.x, 0, 1)). Needed to judge wrap-vs-fit intent.
        if (is_ee34 && d.vs_consts.size() >= 24)
          REXLOG_INFO("[DUMP]     c4=({:.8f},{:.8f},{:.8f},{:.8f}) c5=({:.8f},{:.8f},{:.8f},{:.8f})",
            d.vs_consts[16],d.vs_consts[17],d.vs_consts[18],d.vs_consts[19],
            d.vs_consts[20],d.vs_consts[21],d.vs_consts[22],d.vs_consts[23]);
        // Dump BOTH colour attrs (v0): in_1 (loc 1, tint) and in_2 (loc 2) --
        // the PS's final alpha is col.a *= in_2.w, so a byte-order error there
        // makes whole tiles invisible.
        if (is_ee34 && vcount) {
          // All vertices' colours for the FIRST EE34 draw of the frame (tiles
          // feather their edges: corner verts may be 0 while interiors are
          // opaque -- v0 alone is not representative).
          static std::atomic<int> s_allv{6};
          const bool full = di <= 2 && s_allv.fetch_sub(1, std::memory_order_relaxed) > 0;
          for (const auto& a : vs->t.attrs) {
            if (a.format != 6 || (a.location != 1 && a.location != 2)) continue;
            const uint32_t nv = full ? std::min<uint32_t>(vcount, 12) : 1;
            for (uint32_t v = 0; v < nv; ++v) {
              const uint8_t* b = s0v_bytes.data() + size_t(v) * stride + a.byte_offset;
              if (b + 4 > s0v_bytes.data() + s0v_bytes.size()) break;
              REXLOG_INFO("[DUMP]     v{} col_loc{}=({},{},{},{})", v, a.location, b[0], b[1],
                          b[2], b[3]);
            }
          }
        }
        // For felt draws that reach into the right strip (NDC x > 0.8): dump why
        // they render black -- decoded-texture brightness + the vertex colour
        // attribute the PS multiplies by.
        if (is_ee34 && nx1 > 0.8f && nx0 < 1.0f && ny0 < 1.0f && ny1 > -1.0f) {
          // Decoded texture mean (RGBA8): black texture vs black vertex colour.
          std::vector<uint8_t> rgba; uint32_t tw=0, th=0;
          uint64_t sum = 0;
          bool dec = d.tex[0].valid && renderer::DecodeGuestTexture(d.tex[0], rgba, tw, th);
          if (dec) { for (size_t i=0;i<rgba.size();i+=4) sum += rgba[i]+rgba[i+1]+rgba[i+2]; }
          const double tex_mean = (dec && tw*th) ? double(sum)/(3.0*tw*th) : -1.0;
          REXLOG_INFO("[DUMP]   STRIP-TILE #{}: tex 0x{:08X} {}x{} fmt{} decoded_mean={:.1f}",
                      di, d.tex[0].phys_addr, tw, th, d.tex[0].format, tex_mean);
          {
            std::string il;
            for (uint32_t ix : d.idx()) { il += std::to_string(ix); il += ' '; }
            REXLOG_INFO("[DUMP]     IDXLIST({}): {}", d.idx().size(), il);
          }
          // Full vertex table: position + both colour attrs, to see whether the
          // on-screen portion of the tile is entirely in the alpha-0 feather.
          for (uint32_t v = 0; v < vcount && v < 12; ++v) {
            const uint8_t* vb = s0v_bytes.data() + size_t(v) * stride;
            int16_t ix, iy; std::memcpy(&ix, vb + poff, 2); std::memcpy(&iy, vb + poff + 2, 2);
            uint8_t c1b[4] = {0,0,0,0}, c2b[4] = {0,0,0,0};
            for (const auto& a : vs->t.attrs) {
              if (a.format != 6) continue;
              const uint8_t* b = vb + a.byte_offset;
              if (b + 4 > s0v_bytes.data() + s0v_bytes.size()) continue;
              if (a.location == 1) std::memcpy(c1b, b, 4);
              if (a.location == 2) std::memcpy(c2b, b, 4);
            }
            const float gx = c0[3] + c0[0]*float(iy) + c0[1]*float(ix);
            REXLOG_INFO("[DUMP]     sv{} pos=({},{}) ndc_x={:.3f} loc1=({},{},{},{}) loc2=({},{},{},{})",
                        v, ix, iy, gx, c1b[0],c1b[1],c1b[2],c1b[3], c2b[0],c2b[1],c2b[2],c2b[3]);
          }
          // Vertex colour attr = location 1 (in_1) if present.
          bool col_found = false;
          if (vs && vs->valid) {
            for (const auto& a : vs->t.attrs) {
              if (a.location != 1) continue;
              col_found = true;
              for (uint32_t v=0; v<vcount && v<6; ++v) {
                const uint8_t* b = s0v_bytes.data() + size_t(v)*stride + a.byte_offset;
                if (b + 16 > s0v_bytes.data()+s0v_bytes.size()) break;
                if (a.format == 6) {  // k_8_8_8_8
                  REXLOG_INFO("[DUMP]     v{} col8888=({},{},{},{}) nrm={}", v, b[0],b[1],b[2],b[3], a.is_normalized?1:0);
                } else {  // assume float-ish: print 4 floats
                  float f[4]; std::memcpy(f, b, 16);
                  REXLOG_INFO("[DUMP]     v{} colf=({:.3f},{:.3f},{:.3f},{:.3f}) fmt={}", v, f[0],f[1],f[2],f[3], a.format);
                }
              }
              break;
            }
          }
          if (!col_found) REXLOG_INFO("[DUMP]     (no colour attr at location 1)");
        }
        ++di;
      }
      REXLOG_INFO("[DUMP] end frame {}", this_frame);
    }
  }

  // M3.29: un-transpose the game's post-chain rotor quads. The game renders
  // its 3D world TRANSPOSED into EDRAM on hardware and un-transposes it in the
  // post chain via a fullscreen quad whose vertex UVs swap axes (u tracks NDC
  // y, v tracks NDC x) -- every shader in the chain samples o_0.xy untouched
  // (composite PS EEEB2C6F, DOF PS F5443DCC, passthrough VS 2766CBE9 all
  // verified), so the transpose lives ONLY in that quad's vertex data. Our
  // world renders upright, so the designed un-transpose *introduces* the
  // presented 90-degree tilt + mirror (transpose = rotate+mirror -> also the
  // crossed controls). Detection is data-driven, not hash-keyed: a fullscreen
  // quad through the passthrough VS, sampling a texture some resolve wrote
  // this frame, whose UV deltas track the CROSS axis. Swapping each vertex's
  // (u,v) turns the transposing quad into the identity quad -- one atomic
  // patch per quad, no per-world-draw predicate (the M3.28 lesson).
  // RESTUFF_NO_UNTRANSPOSE=1 restores stock behaviour for A/B.
  _pp_c = _pp_now();  // M3.135c: probes/dumps done (all env-gated, expect ~0)
  static const bool no_untranspose = getenv("RESTUFF_NO_UNTRANSPOSE") != nullptr;
  if (!no_untranspose) {
    constexpr uint64_t kQuadVS = 0x2766CBE92CD1C91Aull;
    std::unordered_set<uint32_t> resolve_dsts;
    for (const auto& d : tl.frame)
      if (d.is_resolve) resolve_dsts.insert(d.copy_dest);
    struct PatchedVb {
      std::shared_ptr<std::vector<uint8_t>> clone;
      std::unordered_set<uint32_t> done;  // vertex indices already swapped
    };
    std::unordered_map<const std::vector<uint8_t>*, PatchedVb> patched;
    for (auto& d : tl.frame) {
      if (d.is_resolve || d.vs_hash != kQuadVS || d.streams.empty() || !d.streams[0].data)
        continue;
      bool samples_rt = false;
      for (uint32_t s = 0; s < renderer::kMaxTexSlots && !samples_rt; ++s)
        if (d.tex[s].valid && resolve_dsts.count(d.tex[s].phys_addr)) samples_rt = true;
      if (!samples_rt) continue;
      // The stream payload is the DEDUPED whole VB region (M3.4) -- the quad's
      // vertices are wherever d.idx() points, not payload verts 0..3.
      const auto& bytes = d.streams[0].bytes();
      const uint32_t stride = d.streams[0].stride;
      static std::atomic<int> s_untdbg{16};
      auto reject = [&](const char* why) {
        if ((d.ps_hash == 0xF5443DCCB449C724ull || d.ps_hash == 0x4F623CAC22E46458ull) &&
            s_untdbg.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[UNTDBG] rotor-candidate rejected: {} (idx={} stride={} vb={})", why,
                      d.idx().size(), stride, bytes.size());
      };
      if (stride < 20) { reject("stride"); continue; }
      if (d.idx().size() < 3 || d.idx().size() > 8) { reject("idxcount"); continue; }
      bool oob = false;
      for (uint32_t ix : d.idx())
        if (VkDeviceSize(ix) * stride + 20 > bytes.size()) oob = true;
      if (oob) { reject("oob"); continue; }
      auto F = [&](uint32_t k, uint32_t comp) {
        float f;
        std::memcpy(&f, bytes.data() + d.idx()[k] * stride + comp * 4, 4);
        return f;
      };
      float mx = 0, my = 0, ident = 0, cross = 0;
      for (uint32_t a = 0; a < 3; ++a) {
        mx = std::max(mx, std::abs(F(a, 0)));
        my = std::max(my, std::abs(F(a, 1)));
        for (uint32_t b = a + 1; b < 3; ++b) {
          const float dx = F(b, 0) - F(a, 0), dy = F(b, 1) - F(a, 1);
          const float du = F(b, 3) - F(a, 3), dv = F(b, 4) - F(a, 4);
          ident += std::abs(du * dx) + std::abs(dv * dy);
          cross += std::abs(du * dy) + std::abs(dv * dx);
        }
      }
      if (mx < 0.9f || my < 0.9f) { reject("not-fullscreen"); continue; }
      if (!(cross > 4.0f * ident + 1e-3f)) {
        if ((d.ps_hash == 0xF5443DCCB449C724ull || d.ps_hash == 0x4F623CAC22E46458ull) &&
            s_untdbg.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[UNTDBG] quad NOT transposed: ident={:.3f} cross={:.3f} "
                      "v0=({:.3f},{:.3f} uv {:.3f},{:.3f}) v1=({:.3f},{:.3f} uv {:.3f},{:.3f}) "
                      "v2=({:.3f},{:.3f} uv {:.3f},{:.3f})",
                      ident, cross, F(0, 0), F(0, 1), F(0, 3), F(0, 4), F(1, 0), F(1, 1), F(1, 3),
                      F(1, 4), F(2, 0), F(2, 1), F(2, 3), F(2, 4));
        continue;
      }
      auto& pv = patched[&bytes];
      if (!pv.clone) pv.clone = std::make_shared<std::vector<uint8_t>>(bytes);
      for (uint32_t ix : d.idx())
        if (pv.done.insert(ix).second)
          std::swap_ranges(pv.clone->data() + VkDeviceSize(ix) * stride + 12,
                           pv.clone->data() + VkDeviceSize(ix) * stride + 16,
                           pv.clone->data() + VkDeviceSize(ix) * stride + 16);
      d.streams[0].data = pv.clone;
      static std::atomic<int> s_unt{12};
      if (s_unt.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[UNT] un-transposed rotor quad ps={:016X} t0=0x{:08X} idx={}", d.ps_hash,
                    d.tex[0].phys_addr, uint32_t(d.idx().size()));
    }
  }

  _pp_d = _pp_now();  // M3.135c: un-transpose scan done
  // M3.299: RESTUFF_NO_VBCACHE=1 restores the per-frame full re-upload.
  static const bool s_no_vbcache = getenv("RESTUFF_NO_VBCACHE") != nullptr;
  // Sweep dead payloads every ~10s: a pin with use_count 1 is held only by
  // us -- capture has dropped the payload, its bytes can never be requested
  // again. Holes are not compacted; the wrap below reclaims them wholesale.
  {
    static uint64_t s_sweep_n = 0;
    if (!s_no_vbcache && (++s_sweep_n % 600) == 0) {
      for (auto it = tl.vb_cache.begin(); it != tl.vb_cache.end();)
        it = (it->second.pin.use_count() == 1) ? tl.vb_cache.erase(it) : std::next(it);
    }
  }
  // Wrap: bump past the cap means holes dominate -- drop everything and let
  // the frame rebuild by missing (frame is fence-waited, same safety argument
  // as EnsureRing's destroy-on-grow).
  // M4.5: under pipelining an in-place rebuild is illegal (the in-flight frame
  // still reads this buffer) -- retire the buffer to the in-flight slot's list
  // and let EnsureRing allocate a fresh one instead.
  constexpr VkDeviceSize kVbBumpCap = 384u << 20;
  if (tl.vb_bump > kVbBumpCap) {
    tl.vb_cache.clear();
    tl.vb_bump = 0;
    if (PipelinedMode() && tl.vb.buf) {
      const auto& dfw = dev->functions();
      if (tl.vb.mapped) { dfw.vkUnmapMemory(dev->device(), tl.vb.mem); tl.vb.mapped = nullptr; }
      tl.slots[tl.slot_ix ^ 1].r_retired_buffers.emplace_back(tl.vb.buf, tl.vb.mem);
      tl.vb = RingBuf{};
      for (auto& s : tl.slots) s.region_base = s.region_end = 0;  // old-buffer offsets
    }
    REXLOG_INFO("[native_vk] M3.299 vb arena wrapped at {}MB (cache rebuilt)", kVbBumpCap >> 20);
  }
  VkDeviceSize vb_total = 0, vb_stable = 0, vb_scratch = 0, ib_total = 0;
  const VkDeviceSize ubo_total = VkDeviceSize(tl.frame.size()) * 2 * kConstBlockBytes;
  // M3.4: stream payloads are shared between draws -- size and upload each
  // DISTINCT payload once per frame (keyed by payload address).
  std::unordered_map<const std::vector<uint8_t>*, VkDeviceSize> vb_uploaded;
  // M3.300: classify each miss -- stable (missed last frame too, same live
  // object => promote to arena) or scratch (first sighting => per-frame
  // region). See vb_lastmiss in TranslatedLayer.
  std::unordered_map<const void*, bool> vb_is_stable;
  std::unordered_map<const void*, std::weak_ptr<const std::vector<uint8_t>>> vb_newmiss;
  const auto size_payload = [&](const std::shared_ptr<const std::vector<uint8_t>>& sp) {
    const std::vector<uint8_t>* pv = sp.get();
    if (!vb_uploaded.emplace(pv, 0).second) return;
    const VkDeviceSize al = (pv->size() + 15) & ~VkDeviceSize(15);
    vb_total += al;
    if (!s_no_vbcache && tl.vb_cache.count(pv)) return;  // cached: no bytes
    bool stable = false;
    if (!s_no_vbcache) {
      auto lit = tl.vb_lastmiss.find(pv);
      if (lit != tl.vb_lastmiss.end()) {
        auto sp_prev = lit->second.lock();
        stable = sp_prev.get() == pv;  // same object, still alive: two-frame miss
      }
      if (!stable) vb_newmiss.emplace(pv, sp);
    }
    vb_is_stable.emplace(pv, stable);
    (stable ? vb_stable : vb_scratch) += al;
  };
  for (const auto& d : tl.frame) {
    for (const auto& s : d.streams) {
      if (!s.data) continue;
      size_payload(s.data);
    }
    if (d.rel_stream_data) size_payload(d.rel_stream_data);
    if (d.rel_stream_data2) size_payload(d.rel_stream_data2);
    ib_total += d.idx().size() * 4;  // uploaded as u32, already 4-aligned
  }
  vb_uploaded.clear();  // reused below to record actual upload offsets
  tl.vb_lastmiss = std::move(vb_newmiss);
  if (!vb_total || !ib_total) return recs;
  // M4.5: this frame's per-slot resources (== slot 0 always when serialized).
  auto& fs = tl.cur();
  // M4.5: pipelined vb write-window base. Fresh writes anchor at the arena
  // bump; if that would overlap the in-flight slot's volatile region, hop
  // over it (see FrameSlot::region_base comment for the stability argument).
  VkDeviceSize vb_wbase = s_no_vbcache ? 0 : tl.vb_bump;
  if (PipelinedMode()) {
    const auto& other = tl.slots[tl.slot_ix ^ 1];
    if (other.region_end > vb_wbase &&
        vb_wbase + vb_stable + vb_scratch > other.region_base) {
      vb_wbase = other.region_end;
    }
  }
  // M3.11: the rel-fetch storage descriptor has a fixed range; pad the ring so
  // any dynamic offset + range stays inside the buffer.
  // Ring layout per frame: [arena 0..bump) [stable-fresh) [scratch, rewritten
  // every frame). vb_wbase + stable + scratch >= vb_total always (cached bytes
  // live inside the arena), so a reallocation-forced rebuild still fits.
  const VkDeviceSize vb_need =
      (s_no_vbcache ? vb_total : vb_wbase + vb_stable + vb_scratch) + kRelStreamRange;
  bool ubo_new = false, vb_new = false;
  // M3.300: grow with 25% headroom -- EnsureRing allocates exactly what is
  // asked, and an exact-fit arena reallocated (cache-clearing) every time the
  // bump crept was the churn behind the in-place fps decay.
  // M4.5: the SHARED vb ring defers its old buffer to the in-flight slot's
  // retire list under pipelining; per-slot rings keep the immediate destroy.
  if (!EnsureRing(dev, tl.vb, vb_need + (vb_need >> 2),
                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                  &vb_new,
                  PipelinedMode() ? &tl.slots[tl.slot_ix ^ 1].r_retired_buffers : nullptr) ||
      !EnsureRing(dev, fs.ib, ib_total, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, nullptr) ||
      !EnsureRing(dev, fs.ubo, ubo_total, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo_new)) {
    return recs;
  }
  bool vb_single_cursor = s_no_vbcache;
  if (vb_new) {
    // New buffer, all cached offsets point into freed memory: rebuild this
    // frame with one cursor (every payload appended + cached; the odd
    // volatile payload cached here is swept later).
    tl.vb_cache.clear();
    tl.vb_bump = 0;
    vb_wbase = 0;  // M4.5: fresh buffer -- no in-flight regions in it
    for (auto& s : tl.slots) s.region_base = s.region_end = 0;
    vb_single_cursor = true;
  }
  // (Re)point THIS SLOT's dynamic UBO descriptors at the (possibly new) UBO
  // buffer, and the rel-fetch storage descriptor at the (possibly new) vertex
  // ring. M4.5: per-slot set; this slot's fence has been waited, so the update
  // never touches a set bound in a still-executing command buffer.
  const auto& df = dev->functions();
  {
    VkDescriptorBufferInfo bi = {fs.ubo.buf, 0, kConstBlockBytes};
    VkDescriptorBufferInfo vbi = {tl.vb.buf, 0, kRelStreamRange};
    VkWriteDescriptorSet w[4] = {};
    for (int i = 0; i < 4; ++i) {
      w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[i].dstSet = fs.ubo_set;
      w[i].dstBinding = i;
      w[i].descriptorCount = 1;
      w[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                  : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
      w[i].pBufferInfo = i < 2 ? &bi : &vbi;
    }
    df.vkUpdateDescriptorSets(dev->device(), 4, w, 0, nullptr);
  }

  // DEBUG draw isolation: RESTUFF_ONLY_VS=<hex> renders only that VS's draws;
  // RESTUFF_SKIP_VS=<hex> drops that VS's draws. For finding which draw is the
  // backdrop / who leaves the right strip black.
  _pp_e = _pp_now();  // M3.135c: sizing + ring (re)alloc + descriptors done
  // M4.2: ~200B records, ~1500/frame -- without a reserve the vector reallocs
  // (and copies) ~11 times per frame.
  recs.reserve(tl.frame.size());
  // M4.2: UBO ring dedup state -- consecutive recorded draws that share one
  // constant snapshot (ConstBank identity, see up_draws.h) reuse the previous
  // draw's ring blocks instead of re-copying 8.5KB into write-combined memory
  // per draw. Soundness mirrors the M4.0 push-constant dedup: the descriptor
  // range covers a single block, so any 256B-aligned offset is equally valid,
  // and identity covers the bool/loop banks too (same write generation).
  // RESTUFF_NO_UBO_DEDUP=1 restores a fresh pair of blocks per draw.
  static const bool s_no_ubo_dedup = getenv("RESTUFF_NO_UBO_DEDUP") != nullptr;
  VkDeviceSize ubo_cursor = 0;
  const void* ubo_prev_vs = nullptr;
  const void* ubo_prev_ps = nullptr;
  uint32_t ubo_prev_vs_off = 0, ubo_prev_ps_off = 0;
  // M4.2: index payloads are shared_ptr-shared across draws (M3.45) but were
  // re-copied into the ib ring for every draw regardless. First sighting per
  // frame uploads and caches its offset; repeats reuse it (the vertex path's
  // vb_uploaded pattern). RESTUFF_NO_IB_DEDUP=1 opts out.
  static const bool s_no_ib_dedup = getenv("RESTUFF_NO_IB_DEDUP") != nullptr;
  std::unordered_map<const void*, VkDeviceSize> ib_uploaded;
  static const char* only_env = getenv("RESTUFF_ONLY_VS");
  static const char* skip_env = getenv("RESTUFF_SKIP_VS");
  static const uint64_t only_vs = only_env ? strtoull(only_env, nullptr, 16) : 0;
  static const uint64_t skip_vs = skip_env ? strtoull(skip_env, nullptr, 16) : 0;
  // RESTUFF_SKIP_PS=<hex>: drop draws by PIXEL-shader hash (the fb-baseline
  // blit 4F623CAC shares its VS with every post quad, so VS filters can't
  // isolate it).
  static const char* skip_ps_env = getenv("RESTUFF_SKIP_PS");
  static const uint64_t skip_ps = skip_ps_env ? strtoull(skip_ps_env, nullptr, 16) : 0;

  static const char* max_env = getenv("RESTUFF_MAX_DRAWS");
  static const uint32_t max_draws_env = max_env ? uint32_t(strtoul(max_env, nullptr, 10)) : 0;
  // RESTUFF_CAP_FILE=<path>: like MAX_DRAWS but re-read every frame, so one
  // long-lived session can bisect "first N draws" live (write N, snap, repeat).
  static const char* cap_file = getenv("RESTUFF_CAP_FILE");
  uint32_t max_draws = max_draws_env;
  if (cap_file) {
    if (FILE* f = fopen(cap_file, "r")) {
      unsigned v;
      if (fscanf(f, "%u", &v) == 1) max_draws = v;
      fclose(f);
    }
  }
  // RESTUFF_ONLY_IDX=<n>: render only the n-th draw (post-VS-filter) of each frame.
  // RESTUFF_IDX_FILE=<path>: same, but re-read the index from that file every
  // frame so one long-lived session can be stepped through its draws live.
  static const char* idx_env = getenv("RESTUFF_ONLY_IDX");
  static const char* idx_file = getenv("RESTUFF_IDX_FILE");
  int only_idx = idx_env ? int(strtol(idx_env, nullptr, 10)) : -1;
  if (idx_file) {
    if (FILE* f = fopen(idx_file, "r")) {
      if (fscanf(f, "%d", &only_idx) != 1) only_idx = -1;
      fclose(f);
    }
  }
  // RESTUFF_SKIP_FILE=<path>: per-frame re-read list of PS-hash hex lines to
  // skip -- lets one live session toggle suspect draws on/off without
  // rebooting (the moment-jitter between boots poisons cross-run A/B).
  static const char* skip_file = getenv("RESTUFF_SKIP_FILE");
  // M4.15: cap raised 8 -> 64 so the dock bisect's "nuke everything known"
  // mode fits the scene's full PS cast (~36 hashes) in one file.
  uint64_t sf_hashes[64];
  int sf_count = 0;
  if (skip_file) {
    if (FILE* f = fopen(skip_file, "r")) {
      char line[64];
      while (sf_count < 64 && fgets(line, sizeof(line), f)) {
        uint64_t h = strtoull(line, nullptr, 16);
        if (h) sf_hashes[sf_count++] = h;
      }
      fclose(f);
    }
  }
  uint32_t sk_noshader=0, sk_nopipe=0, sk_notex=0, seen=0;

  // M3.12: the frame's MAIN color surface = the modal RB_COLOR_INFO base over
  // its draws; records on any other base target the aux surface. Disable with
  // RESTUFF_NO_AUX=1 for A/B.
  // M3.92: a surface is (RB_COLOR_INFO base, RB_SURFACE_INFO pitch) -- the
  // SAME base at a different pitch is a DIFFERENT EDRAM layout. The exposure
  // meter's downsample pyramid renders at the main base with pitches
  // 320/400/160; keying on base alone collapsed it ONTO the main scene, so
  // the meter resolve copied the scene's top-left corner (the HUD region)
  // instead of a true downsample -- the game's auto-exposure then darkened
  // the world (the user-reported uniform dim).
  // M3.93b: pitch-keying is OPT-IN (RESTUFF_SURFKEY_PITCH=1). Same-base
  // different-pitch surfaces ALIAS the same EDRAM bytes on hardware; base-only
  // keying approximates that as full overlap (splash fade erases the legal
  // text -- correct there; exposure meter contaminated -- wrong there), pitch
  // keying as zero overlap (meter clean; splash text TRAILS -- user-reported
  // regression, and the meter fix did not move the gameplay dim anyway). The
  // real model is an SDK-style range-tracked EDRAM arena; until then default
  // to the approximation with no user-visible regression.
  static const bool s_key_pitch = getenv("RESTUFF_SURFKEY_PITCH") != nullptr;
  auto SurfKey = [](const renderer::RawGuestDraw& d) {
    return s_key_pitch ? ((d.dbg_color_info & 0xFFF) | ((d.dbg_surf & 0x3FFF) << 12))
                       : (d.dbg_color_info & 0xFFF);
  };
  uint32_t main_ci = 0;
  {
    std::unordered_map<uint32_t, uint32_t> ci_counts;
    uint32_t best = 0;
    for (const auto& d : tl.frame) {
      if (d.is_resolve) continue;
      const uint32_t n = ++ci_counts[SurfKey(d)];
      if (n > best) { best = n; main_ci = SurfKey(d); }
    }
  }
  // M3.86: the frame's main depth base -- taken from the first draw on the
  // main color base. Segments on OTHER color bases that z-write under this
  // SAME depth base share the main depth EDRAM tile on hardware (the
  // shadow-caster pass); our per-surface depth split must not give them a
  // private, never-resolved depth.
  uint32_t main_di = 0;
  for (const auto& d : tl.frame) {
    if (d.is_resolve || SurfKey(d) != main_ci) continue;
    main_di = d.dbg_depth_info;
    break;
  }
  static const bool no_aux = getenv("RESTUFF_NO_AUX") != nullptr;
  // M3.21: map each distinct non-main RB_COLOR_INFO base to its OWN aux
  // surface, in first-seen order. Bases beyond kAuxSurfaces fall back to the
  // last slot (shared, as before) rather than stomping main.
  std::unordered_map<uint32_t, uint32_t> ci_slot;
  auto SurfFor = [&](uint32_t ci) -> uint32_t {
    if (no_aux || ci == main_ci) return 0;
    auto [it, fresh] = ci_slot.emplace(ci, 0u);
    if (fresh) {
      const uint32_t n = uint32_t(ci_slot.size());  // 1-based slot index
      it->second = n <= TranslatedLayer::kAuxSurfaces ? n : TranslatedLayer::kAuxSurfaces;
    }
    return it->second;
  };

  // RESTUFF_CAM_TRACE=1: periodically log the first matrix-draw's view-proj
  // columns. Across cutscene shots this captures both CORRECT and TILTED
  // cameras of the same scene; their relative rotation is the exact error
  // transform (axis + angle) of the tilt bug.
  static const bool cam_trace = getenv("RESTUFF_CAM_TRACE") != nullptr;
  if (cam_trace) {
    static uint64_t s_ct_frame = 0;
    if (++s_ct_frame % 120 == 0) {
      for (const auto& d : tl.frame) {
        if (d.is_resolve || d.vs_consts.size() < 16) continue;
        // Matrix draws only (skip pretransformed UI): identity ndc signature.
        if (std::fabs(d.ndc[0]) != 1.0f) continue;
        REXLOG_INFO("[CAMTRACE] f={} vs={:016X} c0=({:.4f},{:.4f},{:.4f},{:.4f}) "
                    "c1=({:.4f},{:.4f},{:.4f},{:.4f}) c2=({:.4f},{:.4f},{:.4f},{:.4f})",
                    s_ct_frame, d.vs_hash, d.vs_consts[0], d.vs_consts[1], d.vs_consts[2],
                    d.vs_consts[3], d.vs_consts[4], d.vs_consts[5], d.vs_consts[6],
                    d.vs_consts[7], d.vs_consts[8], d.vs_consts[9], d.vs_consts[10],
                    d.vs_consts[11]);
        break;
      }
    }
  }

  // RESTUFF_PAIR_TRACE=1: log each distinct (vs,ps,index_count) seen in a
  // gameplay frame, once. Identifies which pixel shader draws the characters
  // (overbright fur) -- pick it out by the high index counts.
  static const bool pair_trace = getenv("RESTUFF_PAIR_TRACE") != nullptr;
  if (pair_trace && tl.frame.size() > 200) {
    static std::mutex s_pm;
    static std::set<uint64_t> s_pseen;
    std::lock_guard<std::mutex> lk(s_pm);
    for (const auto& d : tl.frame) {
      if (d.is_resolve) continue;
      const uint64_t k = d.vs_hash ^ (d.ps_hash * 0x9E3779B97F4A7C15ull);
      if (s_pseen.insert(k).second && s_pseen.size() <= 40)
        REXLOG_INFO("[PAIR] vs={:016X} ps={:016X} idx={} tris={} tex0=0x{:08X} tex1=0x{:08X} tex2=0x{:08X}",
                    d.vs_hash, d.ps_hash, uint32_t(d.idx().size()), uint32_t(d.idx().size() / 3),
                    d.tex[0].phys_addr, d.tex[1].phys_addr, d.tex[2].phys_addr);
      // Bear PS light-constant magnitudes (once): c8=material diffuse, c9=amb,
      // c11/13/15/17 = light colours, c254/255 = literal bank. A blown value
      // here would explain the white bear while the ground stays dark.
      static std::atomic<int> s_bearc{2};
      if (d.ps_hash == 0x305619F6BDB43DCCull && d.ps_consts.size() >= 256 * 4 &&
          s_bearc.fetch_sub(1, std::memory_order_relaxed) > 0) {
        auto C = [&](int i, int c) { return d.ps_consts[i * 4 + c]; };
        REXLOG_INFO("[BEARC] c8=({:.2f},{:.2f},{:.2f},{:.2f}) c9=({:.2f},{:.2f},{:.2f}) "
                    "c11=({:.2f},{:.2f},{:.2f}) c13=({:.2f},{:.2f},{:.2f}) c15=({:.2f},{:.2f},{:.2f}) "
                    "c17=({:.2f},{:.2f},{:.2f}) c10=({:.2f},{:.2f},{:.2f}) "
                    "c254=({:.2f},{:.2f},{:.2f},{:.2f}) c255=({:.2f},{:.2f},{:.2f},{:.2f})",
                    C(8, 0), C(8, 1), C(8, 2), C(8, 3), C(9, 0), C(9, 1), C(9, 2), C(11, 0), C(11, 1),
                    C(11, 2), C(13, 0), C(13, 1), C(13, 2), C(15, 0), C(15, 1), C(15, 2), C(17, 0),
                    C(17, 1), C(17, 2), C(10, 0), C(10, 1), C(10, 2), C(254, 0), C(254, 1), C(254, 2),
                    C(254, 3), C(255, 0), C(255, 1), C(255, 2), C(255, 3));
      }
      // [CQT] the gameplay post fullscreen quads: texture bindings + first
      // three vertices (pos.xy + uv). EEEB2C6F = final bloom composite (its
      // quad measured IDENTITY). F5443DCC = the DOF composite at draw ~#753 --
      // the ONLY full-frame repaint between the upright world resolve and the
      // transposed re-resolve, and every shader in the chain samples o_0.xy
      // untouched, so if the game un-transposes anywhere it must be THIS
      // quad's vertex UVs. Gated on big frames so menu composites don't eat
      // the budget.
      static std::atomic<int> s_cqt{30};
      if ((d.ps_hash == 0xEEEB2C6F1B7482F0ull || d.ps_hash == 0xF5443DCCB449C724ull ||
           d.ps_hash == 0x4F623CAC22E46458ull) &&
          tl.frame.size() > 500 && !d.streams.empty() && d.streams[0].data &&
          s_cqt.fetch_sub(1, std::memory_order_relaxed) > 0) {
        const auto& bytes = d.streams[0].bytes();
        const uint32_t stride = d.streams[0].stride;
        auto F = [&](uint32_t v, uint32_t k) {
          float f = 0;
          if ((v * stride + (k + 1) * 4) <= bytes.size())
            std::memcpy(&f, bytes.data() + v * stride + k * 4, 4);
          return f;
        };
        REXLOG_INFO("[CQT] ps={:08X} tex0=0x{:08X} tex1=0x{:08X} tex2=0x{:08X} stride={} "
                    "v0=({:.3f},{:.3f} uv {:.3f},{:.3f}) v1=({:.3f},{:.3f} uv {:.3f},{:.3f}) "
                    "v2=({:.3f},{:.3f} uv {:.3f},{:.3f})",
                    uint32_t(d.ps_hash >> 32), d.tex[0].phys_addr, d.tex[1].phys_addr,
                    d.tex[2].phys_addr, stride, F(0, 0), F(0, 1), F(0, 3), F(0, 4), F(1, 0),
                    F(1, 1), F(1, 3), F(1, 4), F(2, 0), F(2, 1), F(2, 3), F(2, 4));
        // Declared fetch-constant geometry per slot: the swap-uv post shaders
        // only make sense on hardware if the rt-sourced textures are declared
        // TRANSPOSED (e.g. 720x1280) vs the resolve orientation (1280x720).
        REXLOG_INFO("[CQTG] ps={:08X} t0={}x{} p={} T={} t1={}x{} p={} T={} t2={}x{} p={} T={}",
                    uint32_t(d.ps_hash >> 32), d.tex[0].width, d.tex[0].height,
                    d.tex[0].pitch_texels, d.tex[0].tiled ? 1 : 0, d.tex[1].width,
                    d.tex[1].height, d.tex[1].pitch_texels, d.tex[1].tiled ? 1 : 0,
                    d.tex[2].width, d.tex[2].height, d.tex[2].pitch_texels,
                    d.tex[2].tiled ? 1 : 0);
      }
      // [FOGC] blob correlation: per-frame fog colour + fog params of the
      // terrain-family draws. The blobs render as PURE fog colour (black OR
      // sky-blue), so if c[6]/fog consts wobble across frames the constant
      // path is the blob source; if stable, the fog TERM computation is.
      static std::atomic<int> s_fogc{getenv("RESTUFF_FOGC") ? atoi(getenv("RESTUFF_FOGC")) : 0};
      if ((uint32_t(d.ps_hash >> 32) == 0x63C0650Du ||
           uint32_t(d.ps_hash >> 32) == 0x0A5C9003u) &&
          d.ps_consts.size() >= 256 * 4 &&
          s_fogc.fetch_sub(1, std::memory_order_relaxed) > 0) {
        auto C = [&](int i, int c) { return d.ps_consts[i * 4 + c]; };
        // vs_c8 = the VS-side fog block ((dist - c8.x) * c8.y feeds the fog
        // varying) -- the CPU-side captured copy. Stable here + blobs on
        // screen would move suspicion to the GPU ring upload.
        auto V = [&](int i, int c) {
          return d.vs_consts.size() >= size_t(i * 4 + 4) ? d.vs_consts[i * 4 + c] : -999.0f;
        };
        REXLOG_INFO("[FOGC] ps={:08X} tris={} c6=({:.3f},{:.3f},{:.3f},{:.3f}) "
                    "c7=({:.4f},{:.4f},{:.4f},{:.4f}) c33=({:.4f},{:.4f},{:.4f},{:.4f}) "
                    "vs_c8=({:.4f},{:.6f},{:.4f},{:.4f})",
                    uint32_t(d.ps_hash >> 32), uint32_t(d.idx().size() / 3), C(6, 0), C(6, 1),
                    C(6, 2), C(6, 3), C(7, 0), C(7, 1), C(7, 2), C(7, 3), C(33, 0), C(33, 1),
                    C(33, 2), C(33, 3), V(8, 0), V(8, 1), V(8, 2), V(8, 3));
      }
      // World terrain PS 63C0650D: col = lit*(1/exp2(fog)) + c[6]. If c[6] (fog
      // colour) is BLACK, heavy-fog regions collapse to black = the blobs.
      static std::atomic<int> s_wc{4};
      if (d.ps_hash == 0x63C0650D3D8F89AAull && d.ps_consts.size() >= 256 * 4 &&
          s_wc.fetch_sub(1, std::memory_order_relaxed) > 0) {
        auto C = [&](int i, int c) { return d.ps_consts[i * 4 + c]; };
        REXLOG_INFO("[WORLDC] c6_fogcol=({:.3f},{:.3f},{:.3f}) c7=({:.3f},{:.3f},{:.3f}) "
                    "c8=({:.3f},{:.3f},{:.3f}) c9=({:.3f},{:.3f},{:.3f}) c33=({:.3f},{:.3f}) "
                    "c35=({:.3f},{:.3f})",
                    C(6, 0), C(6, 1), C(6, 2), C(7, 0), C(7, 1), C(7, 2), C(8, 0), C(8, 1), C(8, 2),
                    C(9, 0), C(9, 1), C(9, 2), C(33, 0), C(33, 1), C(35, 0), C(35, 1));
      }
    }
  }

  // #37: split upload_and_rec -- vertex-ring copies vs ib/ubo copies vs the
  // rest (struct fill / register math), plus bytes actually memcpy'd.
  // (Declared here: the vb_upload lambda below accumulates pl_vbb.)
  static uint64_t pl_vbc = 0, pl_uboc = 0, pl_vbb = 0;
  // M4.2: byte-level split of the ibubo bucket (time alone can't rank the ib
  // copy vs the two 4KB const blocks) + a dedup dry-run: how many draws carry
  // const blocks byte-identical to the previously recorded draw's. That count
  // is the ceiling on the planned UBO-ring dedup, measured before it exists.
  static uint64_t pl_ibb = 0, pl_ubob = 0, pl_ubo_dup = 0;
  // M3.299/M3.300: cache hits reuse stored offsets and copy nothing; stable
  // fresh payloads append at the arena bump; volatile fresh payloads land in
  // the scratch region above them, rewritten every frame.
  // M4.5: both anchored at vb_wbase (== bump when serialized; may hop the
  // in-flight slot's region when pipelined).
  VkDeviceSize vb_stable_cur = s_no_vbcache ? 0 : vb_wbase, ib_off = 0;
  VkDeviceSize vb_scratch_cur = vb_stable_cur + (vb_single_cursor ? 0 : vb_stable);
  const VkDeviceSize vb_frame_start = vb_stable_cur;
  // M3.315: FNV-1a over up to the first 4KB (no zlib dependency here).
  const auto vb_fp = [](const std::vector<uint8_t>& p) -> uint32_t {
    uint32_t h = 2166136261u;
    const size_t n = std::min<size_t>(p.size(), 4096);
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
  };
  const auto vb_upload = [&](const std::shared_ptr<const std::vector<uint8_t>>& sp)
      -> VkDeviceSize {
    const auto& payload = *sp;
    static const bool s_vbverify = getenv("RESTUFF_VBCACHE_VERIFY") != nullptr;
    if (!s_no_vbcache) {
      auto cit = tl.vb_cache.find(&payload);
      if (cit != tl.vb_cache.end()) {
        // M3.315: verify the immutability contract on every hit (diagnostic,
        // opt-in). A mismatch means the capture layer rebuilt this vector's
        // bytes in place -- the cache would render STALE GEOMETRY (the wedge).
        if (s_vbverify) {
          const uint32_t crc = vb_fp(payload);
          if (payload.size() != cit->second.fp_size ||
              crc != cit->second.fp_crc) {
            static std::atomic<uint32_t> s_mut{0};
            const uint32_t m = s_mut.fetch_add(1, std::memory_order_relaxed);
            if (m < 64 || (m & 1023) == 0) {
              REXLOG_INFO("[native_vk] VBMUT#{} ptr={} size={} (was {}) crc={:08x} (was {:08x})",
                          m, (const void*)&payload, payload.size(),
                          cit->second.fp_size, crc, cit->second.fp_crc);
            }
            // Serve the FRESH bytes: re-copy over the cached slot (same size
            // class) or fall through to a fresh upload when the size changed.
            if (payload.size() == cit->second.fp_size) {
              std::memcpy(tl.vb.mapped + cit->second.off, payload.data(),
                          payload.size());
              cit->second.fp_crc = crc;
              return cit->second.off;
            }
            tl.vb_cache.erase(cit);
          } else {
            return cit->second.off;
          }
        } else {
          return cit->second.off;
        }
      }
    }
    auto [it, fresh_upload] = vb_uploaded.emplace(&payload, 0);
    if (fresh_upload) {
      bool stable = true;
      if (!vb_single_cursor) {
        auto ci = vb_is_stable.find(&payload);
        stable = ci != vb_is_stable.end() && ci->second;
      }
      VkDeviceSize& cur = stable ? vb_stable_cur : vb_scratch_cur;
      it->second = cur;
      std::memcpy(tl.vb.mapped + cur, payload.data(), payload.size());
      if (!s_no_vbcache && stable) {
        TranslatedLayer::VbCacheEntry e{cur, sp};
        if (s_vbverify) {
          e.fp_crc = vb_fp(payload);
          e.fp_size = uint32_t(payload.size());
        }
        tl.vb_cache.emplace(&payload, std::move(e));
      }
      cur += (payload.size() + 15) & ~VkDeviceSize(15);
      if (s_pp) pl_vbb += payload.size();
    }
    return it->second;
  };
  uint32_t draw_idx = 0;
  // RESTUFF_DUMP_FRAME=1: one-shot dump of one gameplay frame's draw/resolve
  // order (ps hash, prim, sampled tex addrs, resolve dests) to trace where the
  // rendered world fails to reach the front buffer.
  static std::atomic<int> s_frame_dumped{0};
  bool frame_has_pgen = false;
  for (const auto& d : tl.frame)
    if (!d.is_resolve && ((d.sq_program_cntl >> 18) & 1)) { frame_has_pgen = true; break; }
  const bool dump_frame = getenv("RESTUFF_DUMP_FRAME") && tl.frame.size() > 500 &&
                          frame_has_pgen &&
                          s_frame_dumped.fetch_add(1, std::memory_order_relaxed) == 0;
  int fdi = 0;
  // M3.135d: the record loop is ~30ms/frame over ~1500 draws (~20us each) --
  // the whole frame cost. Split each draw into the four things it does so the
  // expensive one is named. Deltas are added at each checkpoint rather than at
  // the end of the body, so the loop's many `continue`s cannot lose them.
  static uint64_t pl_pre = 0, pl_pipe = 0, pl_tex = 0, pl_up = 0, pl_n = 0;
  auto _pl_now = [] { return std::chrono::steady_clock::now(); };
  auto _pl_add = [&](uint64_t& acc, std::chrono::steady_clock::time_point& prev) {
    const auto t = _pl_now();
    acc += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t - prev).count());
    prev = t;
  };
  auto _pl_mark = _pp_a;
  for (const auto& d : tl.frame) {
    if (s_pp) _pl_mark = _pl_now();
    if (dump_frame) {
      if (d.is_resolve) {
        REXLOG_INFO("[FRAME] #{:03} RESOLVE dst=0x{:08X} {}x{} ctl=0x{:X}", fdi, d.copy_dest,
                    d.copy_w, d.copy_h, d.dbg_copyctl);
      } else {
        std::string ta;
        for (uint32_t s = 0; s < renderer::kMaxTexSlots; ++s)
          if (d.tex[s].valid) {
            char b[32];
            snprintf(b, sizeof(b), " t%u=0x%08X", s, d.tex[s].phys_addr);
            ta += b;
          }
        // Viewport extent from PA_CL_VPORT_XSCALE/YSCALE (full-size = |scale|*2).
        float xs, ys;
        std::memcpy(&xs, &d.dbg_vport[0], 4);
        std::memcpy(&ys, &d.dbg_vport[2], 4);
        REXLOG_INFO("[FRAME] #{:03} draw vs={:016X} ps={:016X} prim={} z={}{}f{} vp={:.0f}x{:.0f} "
                    "ci={:03X} di={:03X} pitch={} pg={}/{} bl={:08X}{}",
                    fdi, d.vs_hash, d.ps_hash, d.prim, (d.depth_control >> 1) & 1,
                    (d.depth_control >> 2) & 1, (d.depth_control >> 4) & 7,
                    std::abs(xs) * 2.0f, std::abs(ys) * 2.0f, d.dbg_color_info & 0xFFF,
                    d.dbg_depth_info & 0xFFF, d.dbg_surf & 0x3FFF,
                    (d.sq_program_cntl >> 18) & 1, (d.sq_context_misc >> 8) & 0xFF,
                    d.blend_control, ta);
        // For PARAM_GEN draws (the DOF family), dump the UV-scale/focus
        // constants their math runs on: c15 (1/scale?), c16 (linearize/focus),
        // c18 (uv scale), c255 (one).
        if (((d.sq_program_cntl >> 18) & 1) && d.ps_consts.size() >= 1024) {
          auto c = [&](int v, int k) { return d.ps_consts[v * 4 + k]; };
          for (int v : {15, 16, 18, 255}) {
            REXLOG_INFO("[FRAME]      c[{}]=({:.6f},{:.6f},{:.6f},{:.6f})", v, c(v, 0), c(v, 1),
                        c(v, 2), c(v, 3));
          }
        }
        // Tilt diagnosis: the world/character view-proj rows (vs c0..c3) --
        // whether the 90-degree rotation lives in the game's own matrix.
        // Overbright diagnosis: the bear VS's loop constants (count too high =
        // light accumulation overrun).
        // Camera differential: log the view-proj columns for world draws
        // ACROSS the frame (every 150th + the first), tagged with the draw
        // index -- if pass 1 and pass 2 carry different cameras, we're
        // presenting the wrong pass (suspect: pass 1 = the minimap's top-down
        // camera; the real behind-the-bear pass lives elsewhere).
        // Tilt: the composite quad's raw vertices (positions + UVs). If the
        // game un-rotates its rotated scene via transposed UVs here and our
        // attribute decode mangles them, the rotation survives to the screen.
        if (d.ps_hash == 0xEEEB2C6F1B7482F0ull && !d.streams.empty() && d.streams[0].data) {
          static std::atomic<int> s_cq_budget{2};
          if (s_cq_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            const auto& bytes = d.streams[0].bytes();
            const uint32_t stride = d.streams[0].stride;
            for (uint32_t v = 0; v < 4 && (v + 1) * stride <= bytes.size(); ++v) {
              const uint8_t* p = bytes.data() + v * stride;
              std::string hex;
              for (uint32_t b = 0; b < stride && b < 48; ++b) {
                char t[4];
                snprintf(t, sizeof(t), "%02X", p[b]);
                hex += t;
                if ((b & 3) == 3) hex += ' ';
              }
              REXLOG_INFO("[FRAME]      CQ v{} stride={} {}", v, stride, hex);
            }
          }
        }
        // Capture-base check: a wide window of the world VS's constant bank.
        // If SQ_VS_CONST mis-bases gameplay draws, c0..c3 would be a DIFFERENT
        // valid matrix (light view) and the real camera sits at another offset.
        if (d.vs_hash == 0xC6C4FBF7D18A3E61ull && d.vs_consts.size() >= 48) {
          static std::atomic<int> s_wide_budget{1};
          if (s_wide_budget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            for (int v = 0; v < 12; ++v)
              REXLOG_INFO("[FRAME]      WIDE c[{:02}]=({:.4f},{:.4f},{:.4f},{:.4f})", v,
                          d.vs_consts[v * 4], d.vs_consts[v * 4 + 1], d.vs_consts[v * 4 + 2],
                          d.vs_consts[v * 4 + 3]);
          }
        }
        if (d.vs_hash == 0x19E09472AAC8118Dull) {
          static std::atomic<int> s_mtx_budget{2};
          if (s_mtx_budget.fetch_sub(1, std::memory_order_relaxed) > 0 &&
              d.vs_consts.size() >= 16) {
            REXLOG_INFO("[FRAME]      BEAR lc14=0x{:08X} lc15=0x{:08X}", d.loop_consts[14],
                        d.loop_consts[15]);
          }
        }
        // Black-mesh diagnosis: fog colour + light-gate bools for the lit-world
        // PS family (their epilogue is col = lit*fog + c[6]; black output means
        // c[6]~0 too, i.e. suspect per-draw constant capture).
        if ((d.ps_hash == 0x7D97D67D4CE226ECull || d.ps_hash == 0xCAB7DA06F109BEFEull ||
             d.ps_hash == 0x1BAB95FEECAC8E97ull) &&
            d.ps_consts.size() >= 28) {
          auto c = [&](int v, int k) { return d.ps_consts[v * 4 + k]; };
          REXLOG_INFO("[FRAME]      c[6]=({:.4f},{:.4f},{:.4f},{:.4f}) bools[4]=0x{:08X}", c(6, 0),
                      c(6, 1), c(6, 2), c(6, 3), d.bool_consts[4]);
        }
      }
      ++fdi;
    }
    if (d.is_resolve) {  // M2.4: pass through as a resolve record, in order
      TransDrawRec r;
      r.is_resolve = true;
      r.surf = SurfFor(SurfKey(d));
      r.di = d.dbg_depth_info & 0xFFF;  // M3.83: depth resolves source by di
    {
      // M3.117: full-res chain stays DEFAULT (best-verified). The 640x360
      // chain (RESTUFF_NO_FULLRES_VOLUMES=1) + RESTUFF_FILL_MODE=alias is the
      // EDRAM-fold experiment (model still wrong -- see kFillFSAlias note).
      static const bool s_frv = getenv("RESTUFF_NO_FULLRES_VOLUMES") == nullptr;
      r.src_2x = s_frv && r.surf != 0 && d.dbg_depth_info == main_di;
    }
      // RB_COPY_CONTROL copy_src_select (bits 0-2) == 4 => depth resolve.
      r.is_depth_resolve = (d.dbg_copyctl & 0x7) == 4;
      // M3.58: depth_clear_enable (bit 9) => the guest clears depth here.
      r.depth_clears = (d.dbg_copyctl & 0x200) != 0;
      if (r.depth_clears) {
        static std::atomic<int> s_dcb{40};
        if (s_dcb.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[DCLEAR] resolve dst=0x{:08X} {}x{} ctl=0x{:X} clearval=0x{:X} {}",
                      d.copy_dest, d.copy_w, d.copy_h, d.dbg_copyctl, d.dbg_depth_clear,
                      r.is_depth_resolve ? "DEPTH-src" : "color-src");
      }
      r.copy_dest = d.copy_dest;
      r.dbg_copyctl = d.dbg_copyctl;
      r.copy_w = d.copy_w;
      r.copy_h = d.copy_h;
      r.copy_rx = d.copy_rx;
      r.copy_ry = d.copy_ry;
      r.copy_rw = d.copy_rw;
      r.copy_rh = d.copy_rh;
      // RESTUFF_PASS_TRACE: in-order [RES] log so the resolve chain interleaves
      // with [PASS] -- shows which blit (src surface, dst addr, rect, depth?)
      // writes into scene_img and where the black comes from.
      if (getenv("RESTUFF_PASS_TRACE") && tl.frame.size() > 200) {
        static std::atomic<int> s_rb{getenv("RESTUFF_RES_BUDGET")
                                         ? atoi(getenv("RESTUFF_RES_BUDGET"))
                                         : 120};
        if (s_rb.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[RES] after#{} src=surf{} ci={:03X} dst=0x{:08X} {}x{} rect=({},{},{},{}) {}",
                      seen, r.surf, d.dbg_color_info & 0xFFF, r.copy_dest, r.copy_w, r.copy_h,
                      r.copy_rx, r.copy_ry, r.copy_rw, r.copy_rh,
                      r.is_depth_resolve ? "DEPTH" : "color");
      }
      recs.push_back(r);
      continue;
    }
    if (only_vs && d.vs_hash != only_vs) continue;
    // M3.27: skip the game's Z-PREPASS draws (cm=0, ztest+zwrite, zfunc GEQUAL
    // = 973 of the 997 cm=0 draws; masks/depth-restores have other state and
    // are KEPT). On hardware the prepass is a perf optimisation: the colour
    // pass re-draws the same geometry with the same z and passes on the
    // equality edge. Our colour pass runs in a SEPARATE pipeline and NVIDIA
    // ignores cross-pipeline `invariant`, so its z lands ~1 ULP off and FAILS
    // against the prepass depth -> unwritten (black) colour; a depth bias
    // rescued near geometry but not far (float-depth ULP scales with depth),
    // and depth-only meshes with no colour twin (a skinned bear proxy) left
    // bear-shaped phantom occluders. Dropping the prepass removes the fight:
    // the colour pass writes the same depth itself, so occlusion is unchanged.
    // Verified: world colour exact-zero 76%->0%, distant trees + bear render.
    // RESTUFF_KEEP_PREPASS=1 restores the old behaviour for A/B.
    static const bool keep_prepass = getenv("RESTUFF_KEEP_PREPASS") != nullptr;
    // M3.84: only the MAIN surface's cm=0 pass is the redundant z-prepass. The
    // SHADOW-MAP pass has the same depth profile (cm=0, ztest+zwrite) but
    // renders on an aux color base (ci=0D8) into the depth that resolves to the
    // shadow texture (06177000-class) -- dropping it left the shadow map at
    // clear, killing the bear's shadow and the terrain's sun/shadow term.
    // (Compare ci against main_ci directly: SurfFor has a slot-assignment side
    // effect, and calling it for draws that are then skipped shuffles the aux
    // slot order for the whole frame -- the first M3.84 build blacked the
    // scene exactly this way.)
    if (!keep_prepass && d.color_mask == 0 && ((d.depth_control >> 1) & 1) &&
        ((d.depth_control >> 2) & 1) && ((d.depth_control >> 4) & 7) == 6 &&
        SurfKey(d) == main_ci) {
      continue;
    }
    if (skip_vs && d.vs_hash == skip_vs) continue;
    if (skip_ps && d.ps_hash == skip_ps) continue;
    ++seen;
    if (only_idx >= 0 && int(seen) - 1 != only_idx) continue;
    // continue (not break): keep processing so RESOLVE records after the cap
    // still execute -- the fb keeps getting built and the presented frame
    // becomes a valid "first N draws only" bisection readout.
    if (max_draws && seen > max_draws) {
      // [CAPEDGE] identities of the first draws beyond the cap = the bisection
      // suspects when the presented frame flips orientation at this cap value.
      static std::atomic<uint32_t> s_cap_logged{0};
      static std::atomic<int> s_cap_budget{0};
      if (seen <= max_draws + 12) {
        uint32_t prev = s_cap_logged.exchange(max_draws, std::memory_order_relaxed);
        if (prev != max_draws) s_cap_budget.store(26, std::memory_order_relaxed);
        if (s_cap_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[CAPEDGE] cap={} seen={} vs={:016X} ps={:016X} prim={} cm={:X} "
                      "t0=0x{:08X} t1=0x{:08X} t2=0x{:08X}",
                      max_draws, seen, d.vs_hash, d.ps_hash, d.prim, d.color_mask,
                      d.tex[0].phys_addr, d.tex[1].phys_addr, d.tex[2].phys_addr);
      }
      continue;
    }
    {
      bool skip = false;
      for (int i = 0; i < sf_count; ++i)
        if (sf_hashes[i] == d.ps_hash) skip = true;
      if (skip) continue;
    }
    const auto* vs = renderer::spc::GetCachedShader(d.vs_hash);
    const auto* ps = renderer::spc::GetCachedShader(d.ps_hash);
    // M3.57: RESTUFF_RENDERDROP=1 -- log which shaders are DROPPED at render
    // time (captured but never reach the GPU). Steady gameplay only (seen>900)
    // so transient first-frame pipeline-builds don't dominate. Identifies the
    // missing floor/gate draws + the reason.
    auto rdrop = [&](const char* why) {
      // M3.154c: fate log for the draws VSHIT found covering the probe point.
      if (g_vshit_probe_frame &&
          std::find(g_vshit_cover_ptrs.begin(), g_vshit_cover_ptrs.end(),
                    static_cast<const void*>(&d)) != g_vshit_cover_ptrs.end()) {
        REXLOG_INFO("[VFATE] idx={} DROP {} vs={:016X} ps={:016X} cm={:X} tris={}", seen - 1, why,
                    d.vs_hash, d.ps_hash, d.color_mask, uint32_t(d.idx().size() / 3));
      }
      static const bool on = getenv("RESTUFF_RENDERDROP") != nullptr;
      if (!on || seen <= 900) return;
      static std::mutex m;
      static std::set<uint64_t> s;
      std::lock_guard<std::mutex> lk(m);
      if (s.insert(d.vs_hash ^ (uint64_t(why[0]) << 56)).second)
        REXLOG_INFO("[RDROP] {} vs={:016X} ps={:016X} prim={} tris={}", why, d.vs_hash, d.ps_hash,
                    d.prim, uint32_t(d.idx().size() / 3));
    };
    // M3.123: mirror the walker-side rule -- attrs-empty is only "degenerate"
    // for large draws. The title's tiny const-position probe draws (visibility
    // points that gate flares/particles) legitimately fetch nothing.
    // M3.143: this is the RENDER-side twin of the gate M3.138 replaced in the
    // walker, and it was still dropping every particle billboard (attrs empty,
    // ~276 indices after quad expansion, so "> 8"). Same fix: size is not the
    // question. A shader with no attributes is only degenerate if it actually
    // fetches vertices and we resolved nothing; if it has no fetches at all, or
    // sources them through a register-relative fetch (storage buffer), it is
    // procedural and must be drawn.
    if (!vs || !ps || !vs->valid || !ps->valid ||
        (vs->t.attrs.empty() && vs->t.vfetch_count > 0 &&
         vs->t.rel_fetch_slot == ~0u && vs->t.rel_fetch_slot2 == ~0u &&
         d.idx().size() > 8)) { ++sk_noshader; rdrop("noshader"); continue; }
    // M3.27b: also skip the DEPTH-RESTORE passes (cm=0, PS exports
    // gl_FragDepth). On hardware they rebuild EDRAM depth after a resolve; in
    // our renderer depth_img persists across segments, so they are redundant --
    // and with the prepass gone they re-inject resolved depth that the colour
    // pass then z-fails against on the GEQUAL equality edge (same fight,
    // different source: the tutorial-moment ground vanished to fog-blue).
    // M3.87: scope the skip to MAIN-surface restores. The 0D8-base restore
    // quad is NOT redundant -- it is the PRODUCER of the screen-space shadow
    // chain: it rebuilds scene depth into the post pass's depth region, the
    // 640x360 depth resolve saves that region (06177000), and the world's
    // shadow/AO term samples it. Blanket-dropping it left the shadow map at
    // zero forever = no bear shadow + the term that kills floor triangles /
    // darkens the world computing against garbage.
    if (!keep_prepass && d.color_mask == 0 && ps->t.exports_depth &&
        SurfKey(d) == main_ci) {
      continue;
    }
    if (s_pp) { _pl_add(pl_pre, _pl_mark); ++pl_n; }
    VkPipeline pipeline = GetOrCreateTranslatedPipeline(dev, d, *vs, *ps);
    if (s_pp) _pl_add(pl_pipe, _pl_mark);
    if (!pipeline) { ++sk_nopipe; rdrop("nopipe"); continue; }
    // M3.123: attrs-empty probe draws (visibility points, const-position VS)
    // legitimately carry no vertex streams -- only reject empty streams when
    // the VS actually wants attributes.
    if ((d.streams.empty() && !vs->t.attrs.empty()) ||
        d.streams.size() > TransDrawRec::kMaxStreams) { ++sk_noshader; rdrop("streams"); continue; }
    // TEMP DIAG (RESTUFF_CULLTRACE=1): dump cull-BACK main-pass draws so the
    // winding-inverted floor (backface-culled front faces) can be fingerprinted
    // by shader. Distinct per (vs,tris); steady gameplay only.
    static const bool s_culltrace = getenv("RESTUFF_CULLTRACE") != nullptr;
    if (s_culltrace && d.color_mask && d.prim == 6) {
      // M3.67: dedup by INDEX CONTENT (fnv), not (vs,n) -- the old key hid
      // same-count different-content draws (e.g. a reversed copy), exactly the
      // class the draw-set census must not miss. Also widened to ALL color
      // strips (cull-none included) for the full-set diff.
      static std::mutex m; static std::set<uint64_t> s;
      uint64_t ch = 1469598103934665603ull;
      for (uint32_t v : d.idx()) ch = (ch ^ v) * 1099511628211ull;
      std::lock_guard<std::mutex> lk(m);
      if (s.insert(ch ^ d.vs_hash).second) {
        REXLOG_INFO("[CULLTRACE] tris={} vs={:016X} ps={:016X} su=0x{:X} "
                    "ndc=({:.3f},{:.3f},{:.3f},{:.3f}) prim={} relf={}",
                    uint32_t(d.idx().size() / 3), d.vs_hash, d.ps_hash, d.su_mode,
                    d.ndc[0], d.ndc[1], d.ndc[2], d.ndc[3], d.prim,
                    int(vs->t.rel_fetch_slot != ~0u || vs->t.rel_fetch_slot2 != ~0u));
        // M3.66: trace-diff row -- match key (vb phys, index count) + full WVP
        // (c0..c3) as WE captured them, comparable 1:1 against the emulated
        // trace dump's per-draw lines (nb_trace_dump vf95/c0..c3).
        const auto cf = [&](int i) { return d.vs_consts.size() > size_t(i) ? d.vs_consts[i] : 0.0f; };
        REXLOG_INFO("[CTDIFF] vb={:08X} n={} su=0x{:X} zs={:g}/{:g} "
                    "c0=({:.8f},{:.8f},{:.8f},{:.8f}) c1=({:.8f},{:.8f},{:.8f},{:.8f}) "
                    "c2=({:.8f},{:.8f},{:.8f},{:.8f}) c3=({:.8f},{:.8f},{:.8f},{:.8f})",
                    d.dbg_vb_phys, uint32_t(d.idx().size()), d.su_mode, d.vport_zscale,
                    d.vport_zoffset, cf(0), cf(1), cf(2), cf(3), cf(4), cf(5), cf(6), cf(7),
                    cf(8), cf(9), cf(10), cf(11), cf(12), cf(13), cf(14), cf(15));
        // Guest heap addresses differ across builds/sessions, so cross-run
        // matching must be by CONTENT: export our widened index list + the
        // LE-normalized stream-0 bytes for each traced draw.
        {
          char path[160];
          const unsigned long long ch16 = (unsigned long long)(ch & 0xFFFFFFFFull);
          snprintf(path, sizeof(path), "ctgeo_ib_%016llx_%u_%08llx.bin",
                   (unsigned long long)d.vs_hash, uint32_t(d.idx().size()), ch16);
          if (FILE* fp = fopen(path, "wb")) {
            fwrite(d.idx().data(), 4, d.idx().size(), fp);
            fclose(fp);
          }
          if (!d.streams.empty() && d.streams[0].data) {
            const auto& b = d.streams[0].bytes();
            snprintf(path, sizeof(path), "ctgeo_vb_%016llx_%u_%08llx.bin",
                     (unsigned long long)d.vs_hash, uint32_t(d.idx().size()), ch16);
            if (FILE* fp = fopen(path, "wb")) {
              fwrite(b.data(), 1, b.size() < (8u << 20) ? b.size() : (8u << 20), fp);
              fclose(fp);
            }
          }
        }
        // Dump the translated VS GLSL once per shader so it can be examined for
        // a winding-inverting position bug (e.g. a negated axis / mirror).
        static std::set<uint64_t> sdumped;
        if (sdumped.insert(d.vs_hash).second) {
          char path[128];
          snprintf(path, sizeof(path), "culltrace_vs_%016llx.glsl",
                   (unsigned long long)d.vs_hash);
          if (FILE* fp = fopen(path, "wb")) {
            fwrite(vs->t.glsl.data(), 1, vs->t.glsl.size(), fp);
            fclose(fp);
          }
        }
      }
    }

    TransDrawRec r;
    r.pipeline = pipeline;
    r.surf = SurfFor(SurfKey(d));
    r.window_space = d.window_space;  // M3.293
    r.vs_hash = d.vs_hash;
    r.di = d.dbg_depth_info & 0xFFF;
    r.zwrites = ((d.depth_control >> 1) & 1) && ((d.depth_control >> 2) & 1);
    // M3.86: shadow-caster draws (color-masked, z-writing, aux color base,
    // SAME depth base as the main scene) write the MAIN depth EDRAM tile on
    // hardware -- RB_DEPTH_INFO names the tile, not RB_COLOR_INFO. Route them
    // onto the main framebuffer: cm=0 means only depth is touched, exactly the
    // hardware effect (the caster pass scribbles the main depth's viewport
    // region, which the 640x360 depth resolve then reads = the shadow map).
    // With their z-writes on main, di_writer routes that resolve to main too.
    // RESTUFF_NO_CASTMAIN=1 opts out.
    static const bool no_castmain = getenv("RESTUFF_NO_CASTMAIN") != nullptr;
    if (!no_castmain && r.surf != 0 && d.color_mask == 0 && r.zwrites &&
        d.dbg_depth_info == main_di) {
      r.surf = 0;
    }
    // M3.98: an aux COLOUR surface that names the SAME depth tile as main must
    // read/write the main depth+stencil, not a private cleared one. The
    // sun-shaft mask builder (ps 76CEB1DD754C8489 on surf 0x0A8002D0) runs
    // zfail=DECREMENT_AND_WRAP with the depth test ON, so with a blank private
    // depth nothing ever z-fails, the stencil mask stays 0, and the shaft
    // (func=LESS ref=0) is rejected everywhere -> no light shafts at all.
    // RESTUFF_NO_SHAREDDEPTH=1 restores the private-depth behaviour for A/B.
    // M3.98 is OPT-IN (RESTUFF_SHAREDDEPTH=1) because it is KNOWN-WRONG for
    // aux passes that render at a DIFFERENT RESOLUTION than main: the shaft
    // mask pass has a 640-wide viewport vs the world's 1296, so sharing the
    // full-res depth makes it compare half-res geometry against the full-res
    // depth of the screen's top-left quadrant. Correct fix = populate the aux
    // surface's own depth with a DOWNSAMPLED copy at its resolution.
    static const bool shared_depth_on = getenv("RESTUFF_SHAREDDEPTH") != nullptr;
    r.wants_main_depth = r.surf != 0 && d.dbg_depth_info == main_di;
    r.shared_depth = shared_depth_on && r.wants_main_depth;
    // M3.105: the reference runs the whole volume/shaft chain at FULL
    // resolution against the MAIN depth-stencil (its mask pass is DS-only on
    // the full-res EDRAM tile; the 640x360 occ texture is made by the RESOLVE
    // downsampling afterwards). Rasterizing at the guest's half-res viewport
    // gave the edge-on sliver mesh a quarter of the samples = the irreducible
    // +-1 stencil noise. Render these segments at 2x on the main DS; the
    // resolve downsamples (src_2x). RESTUFF_NO_FULLRES_VOLUMES=1 opts out.
    static const bool s_fullres_vol = getenv("RESTUFF_NO_FULLRES_VOLUMES") == nullptr;
    if (s_fullres_vol && r.wants_main_depth) r.shared_depth = true;
    // [AUXDRAWS]: every draw of the shared-di aux group (the shadow/beam
    // volume passes). The reference's occ buffer holds the BEAR'S SHADOW
    // silhouette + the beam patch; ours has only the beam -- so either the
    // bear-volume draws are missing from our stream or they render without
    // effect (skinned transform). This names them.
    // [VOLBONES]: the skinned shadow-volume VSes index bones as
    // int(bone_id * c254.z + trunc(c9.x)) * 4 words into the rel-fetch SSBO.
    // If that range exceeds the captured payload the GLSL clamp silently
    // repeats the last matrix -> degenerate geometry -> the empty footprint.
    static const bool s_auxdraws = getenv("RESTUFF_AUXDRAWS") != nullptr;
    if (s_auxdraws &&
        (d.vs_hash == 0x9E4052352C9BEB99ull || d.vs_hash == 0x19E09472AAC8118Dull)) {
      static std::atomic<int> s_vb{8};
      if (s_vb.fetch_sub(1, std::memory_order_relaxed) > 0 && d.vs_consts.size() >= 256 * 4) {
        const float c9x = d.vs_consts[9 * 4 + 0];
        const float c254z = d.vs_consts[254 * 4 + 2];
        const size_t rel_words = d.rel_stream_data ? d.rel_stream_data->size() / 4 : 0;
        const size_t rel_words2 = d.rel_stream_data2 ? d.rel_stream_data2->size() / 4 : 0;
        // First bone matrix (12 floats at row base trunc(c9.x)*4 words): a sane
        // capture shows rotation-scale rows ~[-1,1] with translations in .w;
        // garbage/huge values mean the endian or base is still wrong.
        // Attribute table + first vertices decoded under OUR format rules.
        // Bone indices must decode as raw integers (USCALED); if their attr is
        // flagged normalized they arrive as tiny fractions, int() collapses
        // every vertex onto bones 0..2, and the volume turns into the fat
        // blob the user sees.
        if (const auto* vvs = renderer::spc::GetCachedShader(d.vs_hash)) {
          std::string at;
          for (const auto& a : vvs->t.attrs)
            at += fmt::format(" [loc{} slot{} fmt={} off={} sgn={} nrm={}]", a.location,
                              a.fetch_slot, a.format, a.byte_offset, a.is_signed ? 1 : 0,
                              a.is_normalized ? 1 : 0);
          REXLOG_INFO("[VOLATTRS] vs={:016X} attrs:{}", d.vs_hash, at);
          if (!d.streams.empty()) {
            const auto& st0 = d.streams[0];
            const uint8_t* b = st0.bytes().data();
            for (uint32_t v = 0; v < 2 && (v + 1) * st0.stride <= st0.bytes().size(); ++v) {
              std::string hex;
              for (uint32_t k = 0; k < st0.stride && k < 48; ++k)
                hex += fmt::format("{:02X}{}", b[v * st0.stride + k], (k % 4 == 3) ? " " : "");
              REXLOG_INFO("[VOLVTX] v{} stride={} {}", v, st0.stride, hex);
            }
          }
        }
        if (d.rel_stream_data && d.rel_stream_data->size() >= 4) {
          const float* fw = reinterpret_cast<const float*>(d.rel_stream_data->data());
          const size_t nw = d.rel_stream_data->size() / 4;
          const size_t base = size_t(c9x) * 4;
          std::string m;
          for (size_t k = 0; k < 12 && base + k < nw; ++k)
            m += fmt::format("{:.3g} ", fw[base + k]);
          REXLOG_INFO("[BONEMAT] c9.x={} words[{}..]: {}", c9x, base, m);
        }
        REXLOG_INFO("[VOLBONES] vs={:016X} c9.x={} c254.z={} rel_slot={} rel_words={} "
                    "rel_slot2={} rel_words2={} streams={} idx={} prim={}",
                    d.vs_hash, c9x, c254z, d.rel_stream_slot, rel_words, d.rel_stream_slot2,
                    rel_words2, uint32_t(d.streams.size()), uint32_t(d.idx().size()), d.prim);
      }
    }
    static const bool s_auxdraws2 = getenv("RESTUFF_AUXDRAWS") != nullptr;
    if (s_auxdraws2 && r.wants_main_depth) {
      // one line per DISTINCT (vs, ps, idx-count, depthctl) signature
      const uint64_t sig = (d.vs_hash * 31) ^ d.ps_hash ^ (uint64_t(d.idx().size()) << 48) ^
                           (uint64_t(d.depth_control) << 16);
      static std::atomic<uint64_t> s_seen[48];
      static std::atomic<int> s_nseen{0};
      bool known = false;
      const int n = s_nseen.load(std::memory_order_relaxed);
      for (int k = 0; k < n; ++k)
        if (s_seen[k].load(std::memory_order_relaxed) == sig) { known = true; break; }
      if (!known && n < 48) {
        s_seen[n].store(sig, std::memory_order_relaxed);
        s_nseen.store(n + 1, std::memory_order_relaxed);
        REXLOG_INFO("[AUXDRAWS] #{} surf={} vs={:016X} ps={:016X} idx={} zw={} dc=0x{:08X} "
                    "prim={} su=0x{:X} cull={} ff={}",
                    n, r.surf, d.vs_hash, d.ps_hash, uint32_t(d.idx().size()), int(r.zwrites),
                    d.depth_control, d.prim, d.su_mode, d.su_mode & 3, (d.su_mode >> 2) & 1);
      }
    }
    static const bool s_sdlog = getenv("RESTUFF_SHAREDDEPTH_LOG") != nullptr;
    if (s_sdlog) {
      static std::atomic<uint64_t> s_sd_yes{0}, s_sd_no{0};
      (r.shared_depth ? s_sd_yes : s_sd_no).fetch_add(1, std::memory_order_relaxed);
      static std::atomic<int> s_sdl{6};
      if (r.surf != 0 && s_sdl.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[SHAREDDEPTH] surf={} di=0x{:08X} main_di=0x{:08X} shared={} ps={:016X} "
                    "(yes={} no={})",
                    r.surf, d.dbg_depth_info, main_di, int(r.shared_depth), d.ps_hash,
                    s_sd_yes.load(std::memory_order_relaxed),
                    s_sd_no.load(std::memory_order_relaxed));
    }
    // M3.92 diag: the exposure-meter pyramid draws, identified by what they
    // SAMPLE (dest addresses churn per boot; the chain topology doesn't).
    {
      const uint32_t t0 = d.tex[0].phys_addr;
      const bool pyr = t0 == 0x05BC4000u || t0 == 0x06502000u || t0 == 0x064C0000u ||
                       t0 == 0x064B1000u;
      if (pyr && d.prim == 6 && d.idx().size() == 4) {
        static std::atomic<int> s_py{40};
        if (s_py.fetch_sub(1, std::memory_order_relaxed) > 0)
          REXLOG_INFO("[PYR] t0=0x{:08X} {}x{} surf={} ci={:03X} pitch={} cm={:X} dc={:X} n={}",
                      t0, d.tex[0].width, d.tex[0].height, r.surf, d.dbg_color_info & 0xFFF,
                      d.dbg_surf & 0x3FFF, d.color_mask, d.depth_control,
                      uint32_t(d.idx().size()));
      }
    }
    if (d.ps_hash == 0x9585B8F9EC2B8F95ull) {
      static std::atomic<int> s_rq{16};
      if (s_rq.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[RQROUTE] restore-quad surf={} cm={:X} zwrites={} di={:08X} main_di={:08X} "
                    "n={}",
                    r.surf, d.color_mask, int(r.zwrites), d.dbg_depth_info, main_di,
                    uint32_t(d.idx().size()));
    }
    // RESTUFF_PASS_TRACE=1: compact in-order pass log for one late (gameplay)
    // frame -- ps, target surface (main/aux), blend, cmask, tris, ndc-sig. Lets
    // us find which post pass paints the black blobs over the world in scene_img.
    static const bool pass_trace = getenv("RESTUFF_PASS_TRACE") != nullptr;
    if (pass_trace && tl.frame.size() > 200) {
      static std::atomic<int> s_pt_frames{0};
      static std::atomic<int> s_pt_budget{0};
      // Arm the logger once, on the first big frame, for its whole draw list.
      // Arm on a LATE big frame: the first ones are level-entry with pipelines
      // still building, so draws skip as nopipe and the trace shows false gaps.
      static const int arm_at = getenv("RESTUFF_PASS_FRAME")
                                    ? atoi(getenv("RESTUFF_PASS_FRAME"))
                                    : 400;
      if (seen == 1 && s_pt_frames.fetch_add(1) == arm_at) s_pt_budget.store(1500);
      if (s_pt_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[PASS] #{} vs={:016X} ps={:016X} surf{} bc={:08X} cm={:X} ztest={} zwrite={} "
                    "zfunc={} zs={:.2f} zo={:.2f} tris={}",
                    seen, d.vs_hash, d.ps_hash, r.surf, d.blend_control, d.color_mask,
                    (d.depth_control >> 1) & 1, (d.depth_control >> 2) & 1,
                    (d.depth_control >> 4) & 7, d.vport_zscale, d.vport_zoffset,
                    uint32_t(d.idx().size() / 3));
    }
    // M3.0: bind every texture slot the PS samples; unused slots ride white.
    {
      const TexEntry* resolved[renderer::kMaxTexSlots] = {};
      bool wrap[renderer::kMaxTexSlots] = {};
      for (uint32_t s = 0; s < renderer::kMaxTexSlots; ++s) {
        if (!d.tex[s].valid) continue;
        resolved[s] = ResolveTextureEntry(dev, d.tex[s]);
        wrap[s] = d.tex[s].wants_wrap();
        // M3.303 EDGECLAMP (default on, kill RESTUFF_NO_EDGECLAMP=1): the
        // right-edge smudge. The game's post passes sample the SCENE RESOLVE
        // with fetch-constant REPEAT (clamp 0,0). On hardware interpolated u
        // never exceeds (W-0.5)/W so repeat is inert; through our translation
        // (guest UVs are D3D9-half-texel-precompensated, plus the PIX_CENTER=0
        // +0.5px shift) u reaches exactly 1.0 at the last pixel and LINEAR
        // wraps 50% of column 0 into column 1279 — the user's "line smudged on
        // the camera" (f039: col1279 == 0.5*(col0+true), 80-row streak).
        // A texture with exact scene-resolve dimensions can never legitimately
        // tile, so forcing clamp there reproduces hardware output; world
        // textures (1024x1024 etc.) keep their wrap — backdrop atlas cells
        // rely on it (up_draws.h:36).
        {
          // M3.314: DEFAULT ON (kill switch RESTUFF_NO_EDGECLAMP=1). The
          // M3.303b opt-in revert cited an A/B that Aug-24 forensics proved
          // INVALID (its "same-moment" captures were of a different scene than
          // the streaked grabs, and the streak flickers per frame). The valid
          // A/B (DUMPGO guest-output dumps paired with window grabs, tutroom
          // route): baseline runs streak 130-220 blend-fit rows in the f037-43
          // window across three runs; with this clamp engaged (2000+ clamps,
          // ps=CABCC0E0214BDEA4 slot 2, 1280x720) the window drops to <=3
          // near-threshold rows. Mechanism confirmed as fetch-constant REPEAT
          // on the scene resolve + our interpolated u grazing 1.0 at the last
          // pixel; hardware's u never exceeds (W-0.5)/W so repeat is inert
          // there — clamping a can't-tile RT-sized texture reproduces hardware
          // output exactly. Residual root question (which +0.5 in this draw
          // class's UV chain lets u reach 1.0) tracked in the smudge memory.
          static const bool eclamp = getenv("RESTUFF_NO_EDGECLAMP") == nullptr;
          // M3.314b: whole downsample chain (see ResolveTextureEntry comment).
          const bool rt_sized =
              (d.tex[s].width == kGuestW && d.tex[s].height == kGuestH) ||
              (d.tex[s].width == kGuestW / 2 && d.tex[s].height == kGuestH / 2) ||
              (d.tex[s].width == kGuestW / 4 && d.tex[s].height == kGuestH / 4) ||
              (d.tex[s].width == kGuestW / 8 && d.tex[s].height == kGuestH / 8);
          if (eclamp && wrap[s] && rt_sized) {
            wrap[s] = false;
            static std::atomic<uint32_t> s_ec{0};
            const uint32_t n_ec = s_ec.fetch_add(1, std::memory_order_relaxed);
            // Periodic, not one-shot: the dbg log truncates its head at 5MB, so
            // a boot-time-only line vanishes from a full run's log.
            if (n_ec < 3 || n_ec % 2000 == 0)
              REXLOG_INFO("[EDGECLAMP] #{} clamped RT-sized wrap sample ps={:016X} slot={} {}x{}",
                          n_ec, d.ps_hash, s, d.tex[s].width, d.tex[s].height);
          }
        }
        // [WRAPFS] one-shot per shader pair: name draws sampling large
        // textures with wrap (diagnostic for the hunt above).
        // M4.2: env-gated -- fired a mutex + set probe per texture slot per
        // draw for any large wrap-sampled world texture, in perpetuity.
        static const bool s_wrapfs_census = getenv("RESTUFF_WRAPFS") != nullptr;
        if (s_wrapfs_census && wrap[s] && d.tex[s].width >= 640 && d.tex[s].height >= 360) {
          static std::mutex s_wfmu;
          static std::set<uint64_t> s_wfseen;
          std::lock_guard<std::mutex> lk(s_wfmu);
          if (s_wfseen.insert(d.ps_hash ^ (uint64_t(s) << 60)).second &&
              s_wfseen.size() <= 32)
            REXLOG_INFO("[WRAPFS] ps={:016X} vs={:016X} slot={} tex={}x{} clamp=({},{}) phys={:08X}",
                        d.ps_hash, d.vs_hash, s, d.tex[s].width, d.tex[s].height,
                        d.tex[s].clamp_x, d.tex[s].clamp_y, d.tex[s].phys_addr);
        }
      }
      r.tex_set = GetTextureComboSet(dev, resolved, wrap);
    }
    if (s_pp) _pl_add(pl_tex, _pl_mark);
    if (r.tex_set == VK_NULL_HANDLE) { ++sk_notex; rdrop("notex"); continue; }
    const auto _vb_t0 = s_pp ? _pl_now() : _pp_a;
    r.stream_count = uint32_t(d.streams.size());
    for (uint32_t s = 0; s < r.stream_count; ++s)
      r.vb_offs[s] = d.streams[s].data ? vb_upload(d.streams[s].data) : 0;
    // M3.11/M3.14: upload the rel-only payload the same deduped way, then
    // resolve each rel slot's dynamic offset -- an attribute-stream slot binds
    // the SAME bytes its vertex stream uploaded; a rel-only slot binds the
    // captured payload.
    {
      const auto up_rel = [&](const std::shared_ptr<const std::vector<uint8_t>>& sp) -> uint32_t {
        return sp ? uint32_t(vb_upload(sp)) : 0u;
      };
      const uint32_t rel_off = up_rel(d.rel_stream_data);
      const uint32_t rel_off2 = up_rel(d.rel_stream_data2);
      // M3.101: the FULL-RANGE rel payload takes priority over the vertex
      // stream window for the SSBO binding -- the stream window ends at
      // vcount*stride while relative fetches index bones far beyond it.
      const auto slot_off = [&](uint32_t slot) -> uint32_t {
        if (slot == ~0u) return 0;
        if (slot == d.rel_stream_slot && d.rel_stream_data) return rel_off;
        if (slot == d.rel_stream_slot2 && d.rel_stream_data2) return rel_off2;
        for (size_t s = 0; s < d.streams.size() && s < TransDrawRec::kMaxStreams; ++s)
          if (d.streams[s].fetch_slot == slot) return uint32_t(r.vb_offs[s]);
        return 0;
      };
      r.vfd_off = slot_off(vs->t.rel_fetch_slot);
      r.vfd2_off = slot_off(vs->t.rel_fetch_slot2);
    }
    if (s_pp) pl_vbc += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(_pl_now() - _vb_t0).count());
    bool ib_dup = false;  // M4.2: see ib_uploaded above
    r.ib_off = ib_off;
    if (!s_no_ib_dedup && d.indices_sp) {
      const auto [it, fresh] = ib_uploaded.try_emplace(d.indices_sp.get(), ib_off);
      if (!fresh) {
        r.ib_off = it->second;
        ib_dup = true;
      }
    }
    r.index_count = uint32_t(d.idx().size());
    // M4.2: UBO dedup (state declared above). ident() equality is exact
    // content equality by construction; null idents (const-less draws) never
    // dedup against each other.
    const bool ubo_dup = !s_no_ubo_dedup && d.vs_consts.ident() &&
                         d.vs_consts.ident() == ubo_prev_vs &&
                         d.ps_consts.ident() == ubo_prev_ps;
    if (ubo_dup) {
      r.vs_ubo_off = ubo_prev_vs_off;
      r.ps_ubo_off = ubo_prev_ps_off;
    } else {
      r.vs_ubo_off = uint32_t(ubo_cursor);
      r.ps_ubo_off = r.vs_ubo_off + kConstBlockBytes;
      ubo_cursor += 2 * kConstBlockBytes;
      ubo_prev_vs = d.vs_consts.ident();
      ubo_prev_ps = d.ps_consts.ident();
      ubo_prev_vs_off = r.vs_ubo_off;
      ubo_prev_ps_off = r.ps_ubo_off;
    }
    std::memcpy(r.ndc, d.ndc, sizeof(r.ndc));
    // M3.1: guest viewport depth range. window_z = clip_z*zscale + zoffset;
    // zscale 0 (VTE z disabled / 2D) keeps the full [0,1] range.
    if (std::isfinite(d.vport_zscale) && d.vport_zscale != 0.0f) {
      r.z_min = std::clamp(d.vport_zoffset, 0.0f, 1.0f);
      r.z_max = std::clamp(d.vport_zoffset + d.vport_zscale, 0.0f, 1.0f);
    }
    // M3.119 keeper (SDK draw.cpp parity): a depth-writing pixel shader gets
    // the full [0,1] window -- gl_FragDepth is window-space, and the guest's
    // aux depth-restore quads write through it.
    if (ps->t.exports_depth) {
      r.z_min = 0.0f;
      r.z_max = 1.0f;
    }
    // M3.10: guest viewport rect. window_x = ndc.x*XSCALE + XOFFSET (+ window
    // offset), so the render rect is (XOFF-|XS|, YOFF-|YS|, 2|XS|, 2|YS|).
    // The existing ndc push-const math is viewport-relative already; giving
    // Vulkan the true rect makes small passes (bloom, light buffer, tiled
    // depth-restores) land at their real screen placement instead of being
    // stretched fullscreen. Insane/zero scales keep the fullscreen fallback.
    {
      const auto vpf = [&](int k) {
        float f;
        std::memcpy(&f, &d.dbg_vport[k], 4);
        return f;
      };
      const float xs = std::abs(vpf(0)), xo = vpf(1), ys = std::abs(vpf(2)), yo = vpf(3);
      // M3.19: a viewport WIDER than the surface pitch = the game renders this
      // pass 90-degree-ROTATED into EDRAM (gameplay: vp 1296x736 on pitch 680;
      // hardware un-rotates in the tiled resolve walk). Counter-rotate in clip
      // space and keep the upright fullscreen viewport. RESTUFF_ROT_DIR=0
      // flips the direction; RESTUFF_NO_ROT=1 disables (A/B).
      // OPT-IN (RESTUFF_ROT=1): the vp>pitch predicate fires only on the
      // D586AD25 fill quads (light-buffer inits), NOT the world draws -- the
      // rotated-world theory is dead; default-on wrongly rotated those fills.
      static const bool want_rot = getenv("RESTUFF_ROT") != nullptr;
      static const char* rot_dir_env = getenv("RESTUFF_ROT_DIR");
      const uint32_t pitch_f = d.dbg_surf & 0x3FFF;
      // RESTUFF_ROT_ALL=1: rotate EVERY matrix draw (test: the game's composite
      // un-rotates by design, expecting a 90-degree-rotated EDRAM world -- ours
      // renders upright, so pre-rotating should make the presented image land
      // upright; menus may break, gameplay is the test).
      static const bool rot_all = getenv("RESTUFF_ROT_ALL") != nullptr;
      // M3.28 (DEFAULT ON): pre-rotate the WORLD passes 90 degrees. On hardware
      // this title renders the world 90-degree-ROTATED into EDRAM (its world
      // viewport is the rotated guard-band config, 1296x736) and its composite
      // un-rotates by design. Our world renders upright, so the designed
      // un-rotation was rotating the presented image (the tilt + mirrored
      // controls + the scattered/see-through hut). Pre-rotating the world
      // passes makes the composite's un-rotation land upright. The 1296-wide
      // viewport is the game's own signature for those passes (HUD/menus use
      // 1280x720 and must NOT rotate). RESTUFF_NO_PREROT=1 disables.
      // OPT-IN ONLY (RESTUFF_PREROT=1): the 1296-viewport predicate fires for
      // only SOME world draws on some boots, splitting the world into two
      // orientations in one frame -- scattered geometry, floor displaced off
      // view, under-map water exposed (user-caught). A partial pre-rotation is
      // WORSE than none. Do not default this on until the game's actual
      // rotate-signal is found and the predicate covers the world atomically.
      static const bool prerot = getenv("RESTUFF_PREROT") != nullptr;
      const bool world_vp = std::isfinite(xs) && xs * 2.0f > 1288.0f;
      if (rot_all || (prerot && world_vp)) {
        r.rot[0] = 1.0f;
        r.rot[1] = (rot_dir_env && rot_dir_env[0] == '0') ? 0.0f : 1.0f;
      } else if (want_rot && pitch_f && std::isfinite(xs) && xs * 2.0f > float(pitch_f) + 1.0f) {
        r.rot[0] = 1.0f;
        r.rot[1] = (rot_dir_env && rot_dir_env[0] == '0') ? 0.0f : 1.0f;
        // Observable: one line per distinct (vp, pitch) combo that fires.
        static std::mutex s_rm;
        static std::set<uint64_t> s_rseen;
        std::lock_guard<std::mutex> lk(s_rm);
        if (s_rseen.insert((uint64_t(pitch_f) << 32) | uint32_t(xs * 2.0f)).second &&
            s_rseen.size() <= 12)
          REXLOG_INFO("[native_vk] ROT-FIRE vp={:.0f}x{:.0f} pitch={} vs={:016X}", xs * 2.0f,
                      ys * 2.0f, pitch_f, d.vs_hash);
      } else if (std::isfinite(xs) && std::isfinite(ys) && xs >= 0.5f && ys >= 0.5f &&
                 xs <= 4096.0f && ys <= 4096.0f) {
        // PA_SC_WINDOW_OFFSET: signed 15-bit x [0:14], y [16:30].
        const auto sext15 = [](uint32_t v) {
          return int32_t(v << 17) >> 17;  // sign-extend low 15 bits
        };
        const float wx = float(sext15(d.dbg_winoff & 0x7FFF));
        const float wy = float(sext15((d.dbg_winoff >> 16) & 0x7FFF));
        r.vp[0] = xo - xs + wx;
        r.vp[1] = yo - ys + wy;
        r.vp[2] = 2.0f * xs;
        r.vp[3] = 2.0f * ys;
      }
      // M3.16: guest scissor. PA_SC_WINDOW_SCISSOR_TL: x[0:14] y[16:30] (+
      // window_offset unless bit31 disables it); BR: exclusive max corner.
      {
        const int32_t tlx = int32_t(d.dbg_sciss_tl & 0x7FFF);
        const int32_t tly = int32_t((d.dbg_sciss_tl >> 16) & 0x7FFF);
        const int32_t brx = int32_t(d.dbg_sciss_br & 0x7FFF);
        const int32_t bry = int32_t((d.dbg_sciss_br >> 16) & 0x7FFF);
        const bool no_winoff = (d.dbg_sciss_tl >> 31) & 1;
        int32_t ox = 0, oy = 0;
        if (!no_winoff) {
          const auto sext15 = [](uint32_t v) { return int32_t(v << 17) >> 17; };
          ox = sext15(d.dbg_winoff & 0x7FFF);
          oy = sext15((d.dbg_winoff >> 16) & 0x7FFF);
        }
        if (brx > tlx && bry > tly) {
          r.sc[0] = tlx + ox;
          r.sc[1] = tly + oy;
          r.sc[2] = brx - tlx;
          r.sc[3] = bry - tly;
        }
      }
      // M3.105: full-res volume chain -- double the raster window so the
      // half-res guest pass lands on the main-resolution grid it shares depth
      // with (see the flatten comment where shared_depth is forced).
      // M3.121: the SAME doubling must apply to castmain-flattened draws
      // (M3.86 reset their surf to 0, which made wants_main_depth false).
      // The guest's pitch-720 depth phase ALIASES the main depth tile: the
      // restore quads re-materialize scene depth at half-res layout in place
      // (and the guest re-restores main at full res when the phase ends).
      // Without the scale they cover only the top-left 640x360 quadrant of
      // our full-res depth and write depth sampled for the WRONG screen
      // positions -- proven by RenderDoc pixel history: restore eid wrote
      // 0.0362 over true 0.0407 at (630,330) and nothing at all at x>=640,
      // giving the stencil mask hard walls at exactly x=640/y=360 (the
      // user's "head shadow detached / shadow cuts off with camera").
      static const bool s_fullres_vol2 = getenv("RESTUFF_NO_FULLRES_VOLUMES") == nullptr;
      const bool castmain_halfres = r.surf == 0 && (d.dbg_surf & 0x3FFF) == 720;
      if (s_fullres_vol2 && (r.wants_main_depth || castmain_halfres) && !r.is_resolve) {
        for (int k2 = 0; k2 < 4; ++k2) r.vp[k2] *= 2.0f;
        for (int k2 = 0; k2 < 4; ++k2) r.sc[k2] *= 2;
        if (castmain_halfres) {
          static std::atomic<int> s_cmlog{6};
          if (s_cmlog.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[M3121] castmain half-res draw scaled 2x: vs={:016X} vp={}x{} di=0x{:03X}",
                        d.vs_hash, r.vp[2], r.vp[3], d.dbg_depth_info & 0xFFF);
        }
      }
    }
    // [NINST] count EEEB2C6F instances PER FRAME and dump EACH instance's
    // indexed vertices -- every budgeted instrument so far only ever sampled
    // the FIRST instance; a second instance with rotated vertex data would
    // explain the transposed output surviving every first-instance check.
    if (d.ps_hash == 0xEEEB2C6F1B7482F0ull) {
      static std::atomic<int> s_ninst_budget{40};
      if (s_ninst_budget.fetch_sub(1, std::memory_order_relaxed) > 0 && !d.streams.empty() &&
          d.streams[0].data && d.idx().size() >= 3) {
        const auto& by = d.streams[0].bytes();
        const uint32_t st = d.streams[0].stride;
        auto F = [&](uint32_t k, uint32_t c) {
          float f = 0;
          const uint64_t off = uint64_t(d.idx()[k]) * st + c * 4;
          if (off + 4 <= by.size()) std::memcpy(&f, by.data() + off, 4);
          return f;
        };
        REXLOG_INFO("[NINST] frame_rec={} idxs=({},{},{},{}) v0=({:.2f},{:.2f} uv {:.2f},{:.2f}) "
                    "v1=({:.2f},{:.2f} uv {:.2f},{:.2f}) v2=({:.2f},{:.2f} uv {:.2f},{:.2f})",
                    seen, d.idx()[0], d.idx()[1], d.idx()[2],
                    d.idx().size() > 3 ? d.idx()[3] : 0, F(0, 0), F(0, 1), F(0, 3), F(0, 4),
                    F(1, 0), F(1, 1), F(1, 3), F(1, 4), F(2, 0), F(2, 1), F(2, 3), F(2, 4));
      }
    }
    // [BIND] tilt hunt: the composite's ACTUAL per-draw raster state. The
    // interpolant field rasterizes transposed while the GLSL + vertex data are
    // identity -- so log what the record really carries: viewport rect,
    // scissor, ndc push consts, surf, and the vertex ring offset.
    if (d.ps_hash == 0xEEEB2C6F1B7482F0ull) {
      static std::atomic<int> s_bind{10};
      if (s_bind.fetch_sub(1, std::memory_order_relaxed) > 0) {
        float v0x = 0, v0y = 0;
        if (tl.vb.mapped) {
          std::memcpy(&v0x, tl.vb.mapped + r.vb_offs[0], 4);
          std::memcpy(&v0y, tl.vb.mapped + r.vb_offs[0] + 4, 4);
        }
        REXLOG_INFO("[BIND] ps=EEEB2C6F surf={} vp=({:.0f},{:.0f},{:.0f},{:.0f}) "
                    "sc=({},{},{},{}) ndc=({:.3f},{:.3f},{:.3f},{:.3f}) vb_off={} ring_v0=({:.3f},{:.3f}) "
                    "idx={} ibo={}",
                    r.surf, r.vp[0], r.vp[1], r.vp[2], r.vp[3], r.sc[0], r.sc[1], r.sc[2], r.sc[3],
                    r.ndc[0], r.ndc[1], r.ndc[2], r.ndc[3], uint64_t(r.vb_offs[0]), v0x, v0y,
                    r.index_count, uint64_t(r.ib_off));
      }
    }
    // M3.2: alpha test (RB_COLORCONTROL func:3@0 enable:1@3, RB_ALPHA_REF).
    r.apc[0] = d.alpha_ref;
    r.apc[1] = (d.color_control & 8) ? float(d.color_control & 7) : 7.0f;
    // M3.152 (RESTUFF_VSNOALPHA=<hex vs hash>): force the ALPHA TEST to
    // always-pass for one shader. The translated PS discards when the test
    // fails (ucode_translator.cpp:1398), and apc.y carries the compare func, so
    // setting 7 here disables it with no shader rebuild and no pipeline key
    // change. This is the last per-fragment suspect for the ground holes:
    // M3.150 proved the geometry IS present at the hole, and depth (M3.151),
    // stencil, culling, primitive restart, the Z-prepass and draw drops are all
    // refuted -- a wrongly-discarding alpha test would produce exactly this
    // patchy, texture-dependent, view-varying hole.
    {
      static const char* na = getenv("RESTUFF_VSNOALPHA");
      static const uint64_t nah = na ? strtoull(na, nullptr, 16) : 0;
      if (nah && d.vs_hash == nah) r.apc[1] = 7.0f;
      // M3.153: which INPUT to the test is wrong? Log the distinct
      // (color_control, alpha_ref) this shader actually draws with. If the
      // guest never enabled the test (cc bit3 clear) we are inventing it; if it
      // did enable it, the fault is in col_0.a and the hunt moves to the
      // texture/shader side.
      static const char* al = getenv("RESTUFF_VSALPHALOG");
      static const uint64_t alh = al ? strtoull(al, nullptr, 16) : 0;
      if (alh && d.vs_hash == alh) {
        static std::mutex m;
        static std::set<uint64_t> seen;
        const uint64_t key = (uint64_t(d.color_control) << 32) ^
                             uint64_t(std::bit_cast<uint32_t>(d.alpha_ref));
        std::lock_guard<std::mutex> lk(m);
        if (seen.insert(key).second && seen.size() <= 24)
          REXLOG_INFO("[VSALPHA] vs={:016X} color_control=0x{:08X} test_enabled={} func={} "
                      "ref={:.4f}",
                      d.vs_hash, d.color_control, (d.color_control & 8) ? 1 : 0,
                      d.color_control & 7, d.alpha_ref);
      }
    }
    std::memcpy(r.boolc, d.bool_consts, sizeof(r.boolc));
    // M3.9: PARAM_GEN pixel-position injection (see TransDrawRec::pgen).
    // pgen[2] = 0.5: PIX_CENTER=0 (D3D9 integer pixel centers) means the guest
    // sees integer window coords where Vulkan's gl_FragCoord reports x.5.
    if ((d.sq_program_cntl >> 18) & 1) {
      r.pgen[0] = 1.0f;
      r.pgen[1] = float((d.sq_context_misc >> 8) & 0x3F);
      r.pgen[2] = 0.5f;
      // M4.38: PARAM_GEN hands the shader a PIXEL position, and gl_FragCoord is
      // in HOST pixels -- at S=2 it would read 2x too large and every
      // pgen-derived UV would land off-screen. pgen.w is the reciprocal scale
      // the FS multiplies gl_FragCoord by to get back to guest pixels; 1.0 at
      // S=1, so the shader arithmetic is unchanged by default.
      r.pgen[3] = 1.0f / float(SceneScale());
    }
    // M3.9x: pack each bound texture's fetch-constant EXP_ADJUST (signed) as
    // one byte per slot so the PS can scale fetch results by 2^exp_adjust.
    for (uint32_t s = 0; s < renderer::kMaxTexSlots && s < 8; ++s) {
      const int32_t e = d.tex[s].valid ? d.tex[s].exp_adjust : 0;
      r.texexp[s >> 2] |= (uint32_t(uint8_t(int8_t(e))) << (8u * (s & 3u)));
    }
    // M3.288 (RESTUFF_NO_RT_GAMMA=1 kill-switch): gamma-RT encode-on-write.
    // RB_COLOR_INFO bits 16-19 = ColorRenderTargetFormat; 1 = k_8_8_8_8_GAMMA.
    // The console hardware-encodes shader output into gamma RTs; we sample
    // gamma textures through _SRGB views (decode) but wrote output raw, which
    // displayed linear values as if encoded = shadows lifted ~x^(1/2.2)
    // (measured ours = real360^(1/2.13) on tutorial-room footage).
    {
      static const bool rtg = getenv("RESTUFF_NO_RT_GAMMA") == nullptr;
      const uint32_t rtfmt = (d.dbg_color_info >> 16) & 0xF;
      if (rtg && rtfmt == 1) r.miscpc[0] |= 1u;
      // One-shot census: which RT formats does this title actually bind?
      static std::atomic<uint32_t> seen_fmts{0};
      const uint32_t bit = 1u << rtfmt;
      if (!(seen_fmts.fetch_or(bit, std::memory_order_relaxed) & bit))
        REXLOG_INFO("[native_vk] M3.288 RT format {} first seen (color_info={:08X}) gamma_encode={}",
                    rtfmt, d.dbg_color_info, (rtg && rtfmt == 1) ? "ON" : "off");
    }
    // 3D-state triage: one-shot log per distinct depth/cull/zscale signature
    // (which zfunc family + z direction does this title actually run?).
    // M4.2: env-gated -- this ran a mutex acquire + set descent per recorded
    // draw, forever, long after all <=24 signatures had been logged.
    static const bool s_depthstate_census = getenv("RESTUFF_DEPTHSTATE") != nullptr;
    if (s_depthstate_census) {
      const uint32_t sig = (d.depth_control & 0x7E) | ((d.su_mode & 7) << 8) |
                           (d.vport_zscale < 0.0f ? 0x10000 : 0) |
                           (d.vport_zscale != 0.0f ? 0x20000 : 0);
      static std::mutex m;
      static std::set<uint32_t> seen;
      std::lock_guard<std::mutex> lk(m);
      if (seen.insert(sig).second && seen.size() <= 24) {
        REXLOG_INFO(
            "[native_vk] DEPTHSTATE z_en={} z_wr={} zfunc={} cull={:03b} zscale={:.4f} zoff={:.4f} "
            "(vs={:016X})",
            (d.depth_control >> 1) & 1, (d.depth_control >> 2) & 1, (d.depth_control >> 4) & 7,
            d.su_mode & 7, d.vport_zscale, d.vport_zoffset, d.vs_hash);
      }
    }

    const auto _ubo_t0 = s_pp ? _pl_now() : _pp_a;
    if (!ib_dup) std::memcpy(fs.ib.mapped + ib_off, d.idx().data(), d.idx().size() * 4);
    if (!ubo_dup) {  // M4.2: dup draws reuse the previous blocks verbatim
      std::memcpy(fs.ubo.mapped + r.vs_ubo_off, d.vs_consts.data(),
                  std::min<size_t>(d.vs_consts.size() * 4, 4096));
      std::memcpy(fs.ubo.mapped + r.ps_ubo_off, d.ps_consts.data(),
                  std::min<size_t>(d.ps_consts.size() * 4, 4096));
      // M3.14: loop constants (lc[8]) + bool bits (bc[2]) after c[256] in both
      // stage blocks.
      for (const uint32_t off : {r.vs_ubo_off, r.ps_ubo_off}) {
        std::memcpy(fs.ubo.mapped + off + 4096, d.loop_consts, sizeof(d.loop_consts));
        std::memcpy(fs.ubo.mapped + off + 4096 + sizeof(d.loop_consts), d.bool_consts,
                    sizeof(d.bool_consts));
      }
    }
    if (s_pp) pl_uboc += uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(_pl_now() - _ubo_t0).count());
    if (s_pp) {
      pl_ibb += d.idx().size() * 4;
      pl_ubob += 2 * kConstBlockBytes;
      // Dedup census (measurement runs only): with M4.2 snapshot sharing,
      // block-pointer equality IS content equality (same capture key => same
      // generation => byte-identical floats AND untouched bool/loop banks).
      // draw_idx > 0 gate: the statics hold pointers from the PREVIOUS frame
      // after consume; only compare within this frame (compare-only, no deref).
      static const void* prev_vs = nullptr;
      static const void* prev_ps = nullptr;
      if (draw_idx > 0 && prev_vs && prev_vs == d.vs_consts.ident() &&
          prev_ps == d.ps_consts.ident()) {
        ++pl_ubo_dup;
      }
      prev_vs = d.vs_consts.ident();
      prev_ps = d.ps_consts.ident();
    }

    static const bool s_dumprecs = getenv("RESTUFF_DUMP_RECS") != nullptr;
    if (s_dumprecs) {
      static std::atomic<int> s_rec_budget{200};
      if (s_rec_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[DUMP] REC{} vs={:016X} pipe={} tex={} vb={} ib={} n={} ubo={}/{} "
                    "ndc=({:.4f},{:.4f},{:.4f},{:.4f})",
                    recs.size(), d.vs_hash, (const void*)r.pipeline, (const void*)r.tex_set,
                    (uint64_t)r.vb_offs[0], (uint64_t)r.ib_off, r.index_count, r.vs_ubo_off,
                    r.ps_ubo_off, r.ndc[0], r.ndc[1], r.ndc[2], r.ndc[3]);
    }
    if (!ib_dup) ib_off += d.idx().size() * 4;  // M4.2: dup reused an offset
    ++draw_idx;
    // M3.154c: covering-draw fate -- this one made it into the frame's records.
    if (g_vshit_probe_frame &&
        std::find(g_vshit_cover_ptrs.begin(), g_vshit_cover_ptrs.end(),
                  static_cast<const void*>(&d)) != g_vshit_cover_ptrs.end()) {
      REXLOG_INFO("[VFATE] idx={} RECORDED vs={:016X} ps={:016X} cm={:X} tris={} dc={:02X} su={:X} "
                  "blend={:08X} apc=({:.2f},{:.0f}) surf={}",
                  seen - 1, d.vs_hash, d.ps_hash, d.color_mask, uint32_t(d.idx().size() / 3),
                  d.depth_control & 0xFF, d.su_mode & 7, d.blend_control, r.apc[0], r.apc[1],
                  r.surf);
    }
    recs.push_back(r);
    if (s_pp) _pl_add(pl_up, _pl_mark);
  }
  g_vshit_probe_frame = false;  // M3.154c: one submit pass per armed probe frame
  // M4.2: report threshold env-tunable -- 150k draws (~100 frames) is too
  // coarse for short A/B runs; RESTUFF_PREPLOOP_N=15000 gets a line in ~10s.
  static const uint64_t s_preploop_n = [] {
    const char* v = getenv("RESTUFF_PREPLOOP_N");
    return v ? std::strtoull(v, nullptr, 10) : 150000ull;
  }();
  if (s_pp && pl_n >= s_preploop_n) {
    REXLOG_INFO("[PREPLOOP] per-draw avg over {} draws: filter_and_consts={}ns pipeline={}ns "
                "textures={}ns upload_and_rec={}ns (vbcopy={}ns ibubo={}ns rest={}ns "
                "vb_bytes/draw={}) ib_bytes/draw={} ubo_bytes/draw={} ubo_dup_pct={}",
                pl_n, pl_pre / pl_n, pl_pipe / pl_n, pl_tex / pl_n, pl_up / pl_n,
                pl_vbc / pl_n, pl_uboc / pl_n,
                (pl_up - std::min(pl_up, pl_vbc + pl_uboc)) / pl_n, pl_vbb / pl_n,
                pl_ibb / pl_n, pl_ubob / pl_n, pl_ubo_dup * 100 / pl_n);
    pl_pre = pl_pipe = pl_tex = pl_up = pl_n = 0;
    pl_vbc = pl_uboc = pl_vbb = 0;
    pl_ibb = pl_ubob = pl_ubo_dup = 0;
  }
  // M3.299/M3.300: only this frame's fresh region (stable-fresh + scratch,
  // contiguous above the arena) needs flushing; cached regions were flushed
  // the frame they were written. Align the start down to a generous atom
  // multiple. Only stable growth persists into the bump.
  {
    const VkDeviceSize vb_end = std::max(vb_stable_cur, vb_scratch_cur);
    const VkDeviceSize fl_start = vb_frame_start & ~VkDeviceSize(255);
    if (vb_end > fl_start)
      vk::util::FlushMappedMemoryRange(dev, tl.vb.mem, tl.vb.mem_type, fl_start, tl.vb.mem_size,
                                       vb_end - fl_start);
  }
  if (!s_no_vbcache) tl.vb_bump = vb_stable_cur;
  // M4.5: record this frame's volatile region (post-promotion) so the other
  // slot's next frame can avoid it while this one is in flight.
  if (PipelinedMode()) {
    fs.region_base = std::max(tl.vb_bump, vb_frame_start);
    fs.region_end = std::max(fs.region_base, std::max(vb_stable_cur, vb_scratch_cur));
  }
  vk::util::FlushMappedMemoryRange(dev, fs.ib.mem, fs.ib.mem_type, 0, fs.ib.mem_size, ib_off);
  // M4.2: flush the deduped extent (ubo_cursor), not draws*2 blocks.
  vk::util::FlushMappedMemoryRange(dev, fs.ubo.mem, fs.ubo.mem_type, 0, fs.ubo.mem_size,
                                   ubo_cursor);
  // M3.168 (RESTUFF_CHUNKDUMP=<hex vs>): dump the IDENTITY of every chunk this
  // shader draws in a frame -- vertex-buffer address, index-buffer address and
  // index count. Diffing this list between a wedge boot and a clean boot AT THE
  // SAME CAMERA says exactly which terrain chunks one has and the other lacks,
  // with no GPU capture and no shader-transform replication (my replica-based
  // coverage probes were invalid for these draws). Logged once per run.
  {
    static const char* cde = getenv("RESTUFF_CHUNKDUMP");
    static const uint64_t cd_vs = cde ? strtoull(cde, nullptr, 16) : 0;
    // Dump PERIODICALLY, not once: a single dump fires at the first qualifying
    // frame, which is early gameplay -- a different camera from the endpoint
    // the wedge is judged at, so cross-run diffs compared unrelated moments.
    // Taking the LAST line of a run now gives the endpoint frame.
    static std::atomic<uint64_t> s_cd_n{0};
    static const uint32_t s_cd_every = [] {
      const char* e = getenv("RESTUFF_CHUNKDUMP_EVERY");
      return e ? uint32_t(strtoul(e, nullptr, 0)) : 400u;
    }();
    if (cd_vs && tl.frame.size() > 200 &&
        (s_cd_n.fetch_add(1, std::memory_order_relaxed) % s_cd_every) == 0) {
      // M3.175: CONTENT identity. Size multisets proved too weak (identical
      // multisets carried different geometry across boots), and the A-family
      // partition is MISSING ~351 indices vs B. h= is an FNV-1a over the
      // draw's index VALUES (order-sensitive): chunks equal across boots iff
      // the same indices in the same order -- heap-address-independent, so
      // cross-boot content diffs become possible. RESTUFF_CHUNKHASH=1 adds it
      // (plain dumps stay byte-compatible with existing parsers).
      static const bool s_cd_hash = getenv("RESTUFF_CHUNKHASH") != nullptr;
      std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> chunks;
      for (const auto& d2 : tl.frame) {
        if (d2.is_resolve || d2.vs_hash != cd_vs) continue;
        const uint32_t vb = d2.streams.empty() ? 0u : d2.dbg_vb_phys;
        uint32_t h = 0;
        if (s_cd_hash) {
          h = 2166136261u;
          for (const uint32_t ix : d2.idx()) {
            h ^= ix;
            h *= 16777619u;
          }
          // M3.176: fold in the VERTEX BYTES. Index-only hashes proved A and
          // clean-fine carry the SAME index structure and totals while the
          // frame still has a hole -- the remaining place the geometry can
          // differ is the vertex data itself (a raced VB fill leaves
          // degenerate triangles without changing any count). Hashing the
          // stream content makes that visible: same chunk, different vhash
          // across boots = the smoking gun.
          if (!d2.streams.empty()) {
            const auto& vbytes = d2.streams[0].bytes();
            const size_t step = vbytes.size() > 4096 ? 16 : 4;  // sampled FNV
            for (size_t i = 0; i + 4 <= vbytes.size(); i += step) {
              uint32_t w;
              std::memcpy(&w, vbytes.data() + i, 4);
              h ^= w;
              h *= 16777619u;
            }
          }
        }
        chunks[{vb, uint32_t(d2.idx().size()), d2.color_mask, h}] += 1;
      }
      std::string line;
      for (const auto& [k, n] : chunks) {
        char buf[80];
        if (s_cd_hash)
          snprintf(buf, sizeof(buf), " %08X:%u/cm%X/h%08X", std::get<0>(k), std::get<1>(k),
                   std::get<2>(k), std::get<3>(k));
        else
          snprintf(buf, sizeof(buf), " %08X:%u/cm%X", std::get<0>(k), std::get<1>(k),
                   std::get<2>(k));
        line += buf;
      }
      REXLOG_INFO("[CHUNKDUMP] vs={:016X} chunks={}{}", cd_vs, chunks.size(), line);
    }
  }
  // M3.164 (RESTUFF_IDXPROBE=<n>): name the guest shaders behind a draw with a
  // known index count. The wedge's winning draw is 421 indices in the GPU
  // capture (the water), but RenderDoc IDs are not guest hashes -- this maps
  // the two so the draw can be skipped by hash (RESTUFF_SKIP_PS) to see what
  // is behind the wedge: ground (drawn but losing depth) or backdrop (absent).
  {
    static const char* ipe = getenv("RESTUFF_IDXPROBE");
    static const uint32_t want_idx = ipe ? uint32_t(strtoul(ipe, nullptr, 10)) : 0;
    if (want_idx) {
      static std::mutex m;
      static std::set<uint64_t> seen_pairs;
      for (const auto& d2 : tl.frame) {
        if (d2.is_resolve || d2.idx().size() != want_idx) continue;
        const uint64_t key = d2.vs_hash ^ (d2.ps_hash << 1);
        std::lock_guard<std::mutex> lk(m);
        if (seen_pairs.insert(key).second)
          REXLOG_INFO("[IDXPROBE] n={} vs={:016X} ps={:016X} prim={} cm={:X} dc={:02X} blend={:08X}",
                      want_idx, d2.vs_hash, d2.ps_hash, d2.prim, d2.color_mask,
                      d2.depth_control & 0xFF, d2.blend_control);
      }
    }
  }
  // M3.162 (RESTUFF_RECSTAT=1): captured-vs-RECORDED accounting, the fork the
  // GPU captures left open -- a wedge boot's ground chunk is absent from the
  // GPU stream, but is it dropped HERE or never captured? Unlike PASS_TRACE's
  // [CHAIN] (which never fires on this path) this is gated on nothing but its
  // own env and logs every 200th frame.
  {
    static const bool s_recstat = getenv("RESTUFF_RECSTAT") != nullptr;
    if (s_recstat) {
      static std::atomic<uint64_t> s_rf{0};
      const uint64_t f = s_rf.fetch_add(1, std::memory_order_relaxed);
      if ((f % 200) == 0 && tl.frame.size() > 200)
        REXLOG_INFO("[RECSTAT] frame={} captured={} seen={} recorded={} noshader={} nopipe={} "
                    "notex={}",
                    f, uint32_t(tl.frame.size()), seen, draw_idx, sk_noshader, sk_nopipe,
                    sk_notex);
    }
  }
  if (getenv("RESTUFF_DUMP_DRAWS") && (sk_noshader | sk_nopipe | sk_notex)) {
    static std::atomic<int> s_sklog{20};
    if (s_sklog.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[DUMP] skips: noshader={} nopipe={} notex={} submitted={} seen={}",
                  sk_noshader, sk_nopipe, sk_notex, draw_idx, seen);
  }
  // RESTUFF_PASS_TRACE: skip tally on REAL gameplay frames (seen>900). Placed
  // here, after the record loop, so seen/draw_idx/sk_* are actually computed
  // (an earlier copy sat before the loop where they were all zero). Answers:
  // of a full ~1400-draw frame, how many draws never reach the GPU?
  // Log EVERY frame once past warm-up (no seen>900 gate -- that gate hid the
  // black-spike frames, which are exactly the LOW-seen ones). The spread of
  // seen across consecutive frames is the whole question.
  if (getenv("RESTUFF_PASS_TRACE")) {
    static std::atomic<uint64_t> s_cf{0};
    const uint64_t cf = s_cf.fetch_add(1, std::memory_order_relaxed);
    static const uint64_t at = getenv("RESTUFF_CHAIN_AT")
                                   ? strtoull(getenv("RESTUFF_CHAIN_AT"), nullptr, 10) : 3000;
    static std::atomic<int> s_chain{getenv("RESTUFF_CHAIN_N") ? atoi(getenv("RESTUFF_CHAIN_N"))
                                                              : 40};
    if (cf >= at && s_chain.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[CHAIN] seen={} recorded={} nopipe={} notex={} framedraws={}", seen, draw_idx,
                  sk_nopipe, sk_notex, uint32_t(tl.frame.size()));
  }
  // M3.135c: report the stage split every 100 frames. Only the final return
  // path is instrumented -- the early-outs above are the "no frame this time"
  // cases, which are not what costs 30ms.
  if (s_pp) {
    const auto f = _pp_now();
    auto us = [](auto a, auto b) {
      return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
    };
    static uint64_t n = 0, consume = 0, probes = 0, untr = 0, ring = 0, mainl = 0, ndraws = 0;
    consume += us(_pp_a, _pp_b);
    probes += us(_pp_b, _pp_c);
    untr += us(_pp_c, _pp_d);
    ring += us(_pp_d, _pp_e);
    mainl += us(_pp_e, f);
    ndraws += tl.frame.size();
    if (++n % 100 == 0) {
      REXLOG_INFO("[PREPMS] 100-frame avg PrepareTranslatedDraws: consume={}us probes={}us "
                  "untranspose={}us sizing_ring={}us main_loop={}us | draws/frame={}",
                  consume / 100, probes / 100, untr / 100, ring / 100, mainl / 100, ndraws / 100);
      consume = probes = untr = ring = mainl = ndraws = 0;
    }
  }
  // M3.293: place the scene-tone boundary. The tone is a property of the 3D
  // SCENE (fitted on gameplay content vs the user's reference), so it must be
  // applied to the scene RT after the last scene contributor and before UI
  // lands on top -- NOT to whole frames (a frame gate fried the episode-select
  // screen, whose 3D vignette tripped it; and before that the title). Rule:
  // mark the first MAIN-SURFACE window-space draw that follows >= 64
  // main-surface non-window 3D draws. Frames with a scene but no UI tail get
  // the tone after the last rec (flag on a synthetic end marker via
  // tone_at_end below is avoided by marking none -- the consumer handles it).
  {
    // M3.293 PARKED OPT-IN (RESTUFF_SCENE_TONE=1): the injected pass is an
    // empirically-fitted curve over an UNIDENTIFIED mechanism -- the user's
    // architectural call, accepted: "the game is fine, it's us that isn't";
    // painting over the composite is symptom management. Default build ships
    // stock brightness until the mechanism is found (prime unaudited lead:
    // Xenos RESOLVE DESTINATION gamma, RB_COPY_DEST_INFO 0x2319 -- see the
    // handoff header in memory/restuff-m3-3d-pipeline.md).
    static const bool scene_tone = getenv("RESTUFF_SCENE_TONE") != nullptr;
    if (scene_tone) {
      size_t main3d = 0, ws = 0;
      bool marked = false;
      for (auto& r : recs) {
        if (r.is_resolve || r.surf != 0) continue;
        if (!r.window_space) { ++main3d; continue; }
        ++ws;
        if (!marked && main3d >= 64) { r.tone_before = true; marked = true; }
      }
      // Diagnostic (bounded): does the boundary rule ever fire, and what does
      // a frame look like when it does not?
      static std::atomic<uint32_t> s_bdbg{0};
      if (main3d >= 64 && s_bdbg.fetch_add(1, std::memory_order_relaxed) < 6)
        REXLOG_INFO("[native_vk] M3.293 boundary: main3d={} window_space={} marked={}",
                    main3d, ws, marked ? "YES" : "NO");
    }
  }
  return recs;
}

// In-render-pass: bind + draw translated records [first, last).

void RecordTranslatedDraws(vk::VulkanDevice* dev, VkCommandBuffer cmd,
                           const std::vector<TransDrawRec>& recs, size_t first, size_t last) {
  auto& tl = TL();
  const auto& df = dev->functions();
  VkPipeline bound = VK_NULL_HANDLE;
  VkDescriptorSet bound_tex = VK_NULL_HANDLE;  // M4.0: skip redundant set-1 binds
  // M4.0: the 7 per-draw vkCmdPushConstants calls collapse into ONE [0,128)
  // update, skipped entirely when the block matches the previous draw's (the
  // most common case: ndc {1,-1,0,0}, same bools, same alpha state). Push
  // constants persist across pipeline binds of the same layout, so the skip is
  // sound for exactly the reason the viewport/scissor filters above it are.
  alignas(16) uint8_t pc_cur[128];
  alignas(16) uint8_t pc_last[128];
  bool pc_valid = false;
  // M4.2: last-bound state for the vertex/index/set-0 bind filters below.
  VkDeviceSize vb_last_offs[TransDrawRec::kMaxStreams] = {};
  uint32_t vb_stream_count = 0;
  bool vb_valid = false;
  VkDeviceSize ib_last_off = 0;
  bool ib_valid = false;
  uint32_t dyn_last[4] = {};
  bool dyn_valid = false;
  float z_min = 0.0f, z_max = 1.0f;  // matches the segment's initial viewport
  // M4.38: r.vp / r.sc arrive in GUEST pixels (decoded straight from the
  // viewport registers), so the dedup compare stays in guest space and the
  // scale is applied only when handing the rect to Vulkan. At S=1 both are
  // identities and this is the pre-M4.38 code exactly.
  const float kVpScale = float(SceneScale());
  float cur_vp[4] = {0.0f, 0.0f, float(kGuestW), float(kGuestH)};
  int32_t cur_sc[4] = {0, 0, int32_t(kGuestW), int32_t(kGuestH)};
  // RESTUFF_SKIP_VS=<hex16,hex16,...> (DIAGNOSTIC): drop draws whose VS hash
  // is listed -- per-draw isolation of multi-draw effects (shadow volumes).
  static const std::set<uint64_t> s_skip_vs = [] {
    std::set<uint64_t> v;
    if (const char* e = getenv("RESTUFF_SKIP_VS")) {
      std::stringstream ss(e);
      std::string tok;
      while (std::getline(ss, tok, ',')) if (!tok.empty()) v.insert(strtoull(tok.c_str(), nullptr, 16));
    }
    return v;
  }();
  for (size_t i = first; i < last; ++i) {
    const auto& r = recs[i];
    if (r.is_resolve) continue;
    if (!s_skip_vs.empty() && s_skip_vs.count(r.vs_hash)) continue;
    if (r.pipeline != bound) {
      df.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
      bound = r.pipeline;
    }
    if (r.z_min != z_min || r.z_max != z_max || std::memcmp(r.vp, cur_vp, sizeof(cur_vp)) != 0) {
      z_min = r.z_min;
      z_max = r.z_max;
      std::memcpy(cur_vp, r.vp, sizeof(cur_vp));
      VkViewport viewport = {cur_vp[0] * kVpScale, cur_vp[1] * kVpScale, cur_vp[2] * kVpScale,
                             cur_vp[3] * kVpScale, z_min, z_max};
      df.vkCmdSetViewport(cmd, 0, 1, &viewport);
    }
    // M3.16: guest scissor, clamped to the framebuffer.
    if (std::memcmp(r.sc, cur_sc, sizeof(cur_sc)) != 0) {
      std::memcpy(cur_sc, r.sc, sizeof(cur_sc));
      // M4.38: clamp in guest space (the register values' own units) exactly as
      // before, then scale the finished rect up to the host target.
      const int32_t S = int32_t(SceneScale());
      const int32_t gx0 = std::clamp(cur_sc[0], 0, int32_t(kGuestW));
      const int32_t gy0 = std::clamp(cur_sc[1], 0, int32_t(kGuestH));
      const int32_t gx1 = std::clamp(cur_sc[0] + cur_sc[2], gx0, int32_t(kGuestW));
      const int32_t gy1 = std::clamp(cur_sc[1] + cur_sc[3], gy0, int32_t(kGuestH));
      const int32_t x0 = gx0 * S, y0 = gy0 * S;
      VkRect2D scissor = {{x0, y0}, {uint32_t((gx1 - gx0) * S), uint32_t((gy1 - gy0) * S)}};
      if (scissor.extent.width == 0) scissor.extent.width = 1;
      if (scissor.extent.height == 0) scissor.extent.height = 1;
      df.vkCmdSetScissor(cmd, 0, 1, &scissor);
    }
    // M3.30: one VERTEX|FRAGMENT range [0,128) -- every push must carry both
    // stage bits (each byte's stageFlags must cover all ranges containing it).
    // M4.0: assembled into one block, pushed only when it changed (see above).
    constexpr VkShaderStageFlags kPcStages =
        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    std::memcpy(pc_cur + 0, r.ndc, 16);
    std::memcpy(pc_cur + 16, r.apc, 16);
    std::memcpy(pc_cur + 32, r.boolc, 32);
    std::memcpy(pc_cur + 64, r.pgen, 16);
    std::memcpy(pc_cur + 80, r.texexp, 16);
    std::memcpy(pc_cur + 96, r.rot, 16);
    std::memcpy(pc_cur + 112, r.miscpc, 16);  // M3.288
    // RESTUFF_NO_RECORD_DEDUP=1: push every draw + rebind every tex_set (the
    // pre-M4.0 behaviour) -- A/B lever for the record-loop dedup pair.
    static const bool s_no_dedup = getenv("RESTUFF_NO_RECORD_DEDUP") != nullptr;
    if (s_no_dedup || !pc_valid || std::memcmp(pc_cur, pc_last, sizeof(pc_cur)) != 0) {
      df.vkCmdPushConstants(cmd, tl.pipeline_layout, kPcStages, 0, sizeof(pc_cur), pc_cur);
      std::memcpy(pc_last, pc_cur, sizeof(pc_cur));
      pc_valid = true;
    }
    // M4.2: unchanged-state filters for the three remaining unconditional
    // per-draw binds, same soundness argument as the viewport/scissor/push
    // filters above. The set-0 filter only pays off because the M4.2 UBO ring
    // dedup makes consecutive dyn offsets actually repeat.
    if (s_no_dedup || !vb_valid || r.stream_count != vb_stream_count ||
        std::memcmp(r.vb_offs, vb_last_offs, sizeof(r.vb_offs)) != 0) {
      VkBuffer vbufs[TransDrawRec::kMaxStreams];
      for (uint32_t s = 0; s < r.stream_count; ++s) vbufs[s] = tl.vb.buf;
      if (r.stream_count) df.vkCmdBindVertexBuffers(cmd, 0, r.stream_count, vbufs, r.vb_offs);
      vb_stream_count = r.stream_count;
      std::memcpy(vb_last_offs, r.vb_offs, sizeof(vb_last_offs));
      vb_valid = true;
    }
    if (s_no_dedup || !ib_valid || r.ib_off != ib_last_off) {
      df.vkCmdBindIndexBuffer(cmd, tl.cur().ib.buf, r.ib_off, VK_INDEX_TYPE_UINT32);
      ib_last_off = r.ib_off;
      ib_valid = true;
    }
    const uint32_t dyn[4] = {r.vs_ubo_off, r.ps_ubo_off, r.vfd_off, r.vfd2_off};
    if (s_no_dedup || !dyn_valid || std::memcmp(dyn, dyn_last, sizeof(dyn)) != 0) {
      df.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.pipeline_layout, 0, 1,
                                 &tl.cur().ubo_set, 4, dyn);
      std::memcpy(dyn_last, dyn, sizeof(dyn_last));
      dyn_valid = true;
    }
    if (s_no_dedup || r.tex_set != bound_tex) {
      df.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.pipeline_layout, 1, 1,
                                 &r.tex_set, 0, nullptr);
      bound_tex = r.tex_set;
    }
    df.vkCmdDrawIndexed(cmd, r.index_count, 1, 0, 0, 0);
  }
  tl.draws.fetch_add(last - first, std::memory_order_relaxed);
}

// M2.4: record a whole guest frame against the offscreen scene target --
// draw segments between resolve records render inside scene render passes
// (first segment clears, later ones load); each resolve blits the scene into
// its destination texture. Runs OUTSIDE the presented render pass.
// M4.33 (RESTUFF_GPUPASS_MS=1): GPU-side per-pass frame cost breakdown via
// timestamp queries. The CPU-side diags (PRESENTMS*) can say the GPU owns the
// frame (gpu_fence_wait) but not WHERE inside it; this writes a GPU timestamp
// at every region boundary of the frame command buffer and attributes each
// delta to the region that just ended: main-scene draw segments, aux
// (shadow/bloom mask) segments, resolves (the EDRAM-model RT copies), the
// tone pass, the present/gamma pass, and the frame-head upload/barrier
// prologue. Logs a [GPUPASS] 100-frame average, same cadence as PRESENTMS.
// Needs the serialized frame (registered as a PipelinedMode blocker): results
// are read after the frame fence, so the readback never stalls the GPU.
namespace {
enum GpCat : uint8_t { kGpUpload, kGpMain, kGpAux, kGpResolve, kGpTone, kGpPresent, kGpNumCats };
struct GpuPassDiag {
  static constexpr uint32_t kMaxStamps = 256;
  VkQueryPool pool = VK_NULL_HANDLE;
  bool broken = false;         // pool creation failed -- stay off for good
  uint8_t cat[kMaxStamps] = {};
  uint32_t n = 0;              // stamps recorded this frame (0 = not begun)
  uint32_t dropped = 0;
  double period_ns = 0.0;
  uint64_t acc_us[kGpNumCats] = {};
  uint64_t frames = 0;
};
GpuPassDiag g_gp;
bool GpOn() {
  static const bool on = getenv("RESTUFF_GPUPASS_MS") != nullptr;
  return on;
}
void GpFrameBegin(vk::VulkanDevice* dev, VkCommandBuffer cmd) {
  if (!GpOn() || g_gp.broken) return;
  const auto& df = dev->functions();
  if (g_gp.pool == VK_NULL_HANDLE) {
    VkPhysicalDeviceProperties pp = {};
    dev->vulkan_instance()->functions().vkGetPhysicalDeviceProperties(dev->physical_device(),
                                                                      &pp);
    g_gp.period_ns = pp.limits.timestampPeriod;
    VkQueryPoolCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = GpuPassDiag::kMaxStamps;
    if (g_gp.period_ns <= 0.0 ||
        df.vkCreateQueryPool(dev->device(), &ci, nullptr, &g_gp.pool) != VK_SUCCESS) {
      REXLOG_ERROR("[GPUPASS] timestamp queries unavailable (period={}ns) -- diag disabled",
                   g_gp.period_ns);
      g_gp.pool = VK_NULL_HANDLE;
      g_gp.broken = true;
      return;
    }
    REXLOG_INFO("[GPUPASS] GPU per-pass timing on ({}ns/tick)", g_gp.period_ns);
  }
  df.vkCmdResetQueryPool(cmd, g_gp.pool, 0, GpuPassDiag::kMaxStamps);
  df.vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gp.pool, 0);
  g_gp.n = 1;  // stamp 0 is the frame base
}
void GpMark(vk::VulkanDevice* dev, VkCommandBuffer cmd, GpCat c) {
  if (g_gp.n == 0) return;  // diag off or frame not begun
  if (g_gp.n >= GpuPassDiag::kMaxStamps) {
    ++g_gp.dropped;
    return;
  }
  dev->functions().vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_gp.pool,
                                       g_gp.n);
  g_gp.cat[g_gp.n] = uint8_t(c);
  ++g_gp.n;
}
void GpCollect(vk::VulkanDevice* dev) {
  const uint32_t n = g_gp.n;
  g_gp.n = 0;
  if (n < 2) return;
  uint64_t ts[GpuPassDiag::kMaxStamps];
  // The frame fence has been waited on, so results are already available;
  // WAIT_BIT is a formality and cannot stall.
  if (dev->functions().vkGetQueryPoolResults(dev->device(), g_gp.pool, 0, n, sizeof(ts), ts,
                                             sizeof(uint64_t),
                                             VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) !=
      VK_SUCCESS)
    return;
  for (uint32_t k = 1; k < n; ++k) {
    const uint64_t dt = ts[k] >= ts[k - 1] ? ts[k] - ts[k - 1] : 0;
    g_gp.acc_us[g_gp.cat[k]] += uint64_t(double(dt) * g_gp.period_ns / 1000.0);
  }
  if (++g_gp.frames % 100 == 0) {
    REXLOG_INFO("[GPUPASS] 100-frame avg GPU: main={}us aux={}us resolve={}us tone={}us "
                "present={}us upload={}us total={}us{}",
                g_gp.acc_us[kGpMain] / 100, g_gp.acc_us[kGpAux] / 100,
                g_gp.acc_us[kGpResolve] / 100, g_gp.acc_us[kGpTone] / 100,
                g_gp.acc_us[kGpPresent] / 100, g_gp.acc_us[kGpUpload] / 100,
                (g_gp.acc_us[kGpMain] + g_gp.acc_us[kGpAux] + g_gp.acc_us[kGpResolve] +
                 g_gp.acc_us[kGpTone] + g_gp.acc_us[kGpPresent] + g_gp.acc_us[kGpUpload]) /
                    100,
                g_gp.dropped ? " (stamp budget exceeded; tail lumped)" : "");
    for (auto& a : g_gp.acc_us) a = 0;
    g_gp.dropped = 0;
  }
}
}  // namespace

void RecordSceneFrame(vk::VulkanDevice* dev, VkCommandBuffer cmd,
                      const std::vector<TransDrawRec>& recs) {
  auto& tl = TL();
  const auto& df = dev->functions();
  // M3.124 (RESTUFF_PRESENTMS=1): present-side frame cost breakdown. User
  // telemetry shows the guest at a solid 30 swaps/s while presents run ~20/s,
  // so the ceiling is THIS thread; split record-total vs the resolve slice to
  // aim the next optimization. Logs every 100 gameplay-sized frames.
  static const bool s_pms = getenv("RESTUFF_PRESENTMS") != nullptr;
  const auto _pms_t0 = std::chrono::steady_clock::now();
  g_pms_t0 = _pms_t0;  // M3.135
  if (s_pms && g_pms_last_end.time_since_epoch().count())
    g_pms_outside_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                                    _pms_t0 - g_pms_last_end).count());
  struct PmsScope {
    bool on;
    std::chrono::steady_clock::time_point t0;
    size_t nrecs;
    ~PmsScope() {
      if (!on || nrecs < 200) return;
      static uint64_t s_frames = 0, s_total_us = 0, s_res_us = 0, s_resn = 0;
      const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
      s_total_us += us;
      s_res_us += g_pms_resolve_us;
      s_resn += g_pms_resolve_n;
      if (++s_frames % 100 == 0) {
        REXLOG_INFO("[PRESENTMS] 100-frame avg: record_total={}us resolve={}us ({} resolves) recs={}",
                    s_total_us / 100, s_res_us / 100, s_resn / 100, nrecs);
        s_total_us = s_res_us = s_resn = 0;
      }
    }
  } _pms{s_pms, _pms_t0, recs.size()};
  g_pms_resolve_us = 0;
  g_pms_resolve_n = 0;
  bool first_segment = true;
  uint32_t main_di = ~0u;  // M3.24: depth base currently in the main depth buffer
  // M3.83: di -> surf of the last segment that wrote depth under that base this
  // frame. Depth resolves source from it (fallback: main scene depth).
  std::unordered_map<uint32_t, uint32_t> di_writer;
  bool depth_saved = false;  // M3.25: a main depth resolve happened; next main
                             // segment starts a fresh depth pass -> clear depth
  bool pending_depth_clear = false;  // M3.58: a guest depth_clear_enable resolve
                                     // was seen -> clear depth at the next main
                                     // segment (the true depth-pass boundary).
  static bool scene_virgin = true;  // very first pass must initialize layouts
  // Clear values: color transparent-black (alpha 1); depth 0.0 = the FAR
  // plane. This title renders 3D with an INVERTED depth range
  // (PA_CL_VPORT_ZSCALE=-1, ZOFFSET=1 -> window z = 1 - clip z) and
  // zfunc GREATEREQUAL, so clearing to 1.0 parked the clear at the NEAR
  // plane and every world fragment z-failed (invisible 3D scene, frozen
  // loading screen via EDRAM color persistence).
  VkClearValue clears[2] = {};
  clears[0].color.float32[3] = 1.0f;
  // M4.10 flicker discriminator (RESTUFF_CLEAR_RGB=r,g,b e.g. "1,0,1"): tint
  // the MAIN scene colour clear. A black flicker artifact that turns this
  // colour is a HOLE (missing draw / un-replayed clear showing through); one
  // that stays black is geometry actually drawn with black shading. Pair with
  // RESTUFF_SCENE_CLEAR=1 -- under default EDRAM colour persistence a hole
  // shows last frame's pixels, not the clear colour.
  // AUX segments always take clears_plain: their colour feeds post-processing
  // (shadow/bloom masks), and tinting them green-cast the entire scene on the
  // first field test (inverse/subtractive blends downstream of the masks).
  struct ClearRGB { float r = 0, g = 0, b = 0; bool on = false; };
  static const ClearRGB s_clear_rgb = [] {
    ClearRGB c;
    if (const char* e = getenv("RESTUFF_CLEAR_RGB"))
      c.on = std::sscanf(e, "%f,%f,%f", &c.r, &c.g, &c.b) == 3;
    return c;
  }();
  // RESTUFF_DEPTH_CLEAR_ONE=1 (DIAGNOSTIC): clear depth to 1.0 (NEAREST under
  // this title's reversed-Z) instead of 0.0. Under GREATER every subsequent
  // fragment then z-FAILS, so if the stencil zfail path works at all the shaft
  // volume must mark. Distinguishes "zfail path broken" from "depth content
  // wrong when the mask pass runs".
  static const bool s_clear_one = getenv("RESTUFF_DEPTH_CLEAR_ONE") != nullptr;
  clears[1].depthStencil = {s_clear_one ? 1.0f : 0.0f, 0};
  VkClearValue clears_plain[2] = {clears[0], clears[1]};  // untinted, for AUX
  if (s_clear_rgb.on) {
    clears[0].color.float32[0] = s_clear_rgb.r;
    clears[0].color.float32[1] = s_clear_rgb.g;
    clears[0].color.float32[2] = s_clear_rgb.b;
  }
  // M3.12/M3.21: main scene (EDRAM colour persistence + per-frame depth clear)
  // plus N aux surfaces (scratch: each cleared at its own first segment of the
  // frame). Segments split on resolves AND on surface changes.
  bool aux_first[TranslatedLayer::kAuxSurfaces];
  for (auto& f : aux_first) f = true;
  g_dump_taken_this_frame = false;
  g_aux_trans = 0;
  {
    // RESTUFF_DUMP_EACH_AUX=<stride>: burst-dump per-segment stencil+depth on
    // qualifying (gameplay-sized) frames number stride, 2*stride, 3*stride,
    // 4*stride -- fixed absolute frame ordinals miss gameplay on ~run-to-run
    // timing variance; one of four strided bursts always lands.
    static const char* eaux = getenv("RESTUFF_DUMP_EACH_AUX");
    g_eaux_this_frame = false;
    if (eaux && recs.size() > 200) {
      const uint64_t stride = std::max<uint64_t>(1, strtoull(eaux, nullptr, 10));
      ++g_frame_no;
      g_eaux_this_frame = g_frame_no % stride == 0 && g_frame_no <= 4 * stride;
    }
  }
  // M3.120: headless RenderDoc capture. RESTUFF_RDC_FRAME=<n>[,count] queues
  // in-app captures on gameplay-sized frames n..n+count-1 (own counter -- the
  // EACH_AUX one above only ticks when that env is set). Needs the capture
  // layer in the process (ENABLE_VULKAN_RENDERDOC_CAPTURE=1); we attach to the
  // already-loaded librenderdoc only (RTLD_NOLOAD), never pull it in ourselves.
  // RESTUFF_RDC_PATH sets the capture file template (dir must exist).
#ifndef _WIN32  // Linux-harness tooling; renderdoc_app.h isn't included on Win
  {
    static const char* rdcf = getenv("RESTUFF_RDC_FRAME");
    if (rdcf && recs.size() > 200) {
      // 1_0_1 typedef + pre-rename member names: the SDK ships a pre-1.1.0
      // renderdoc_app.h on the -I path (shadows /usr/include); these names
      // compile against any header vintage and the members used here sit in
      // the version-stable head of the struct, which every runtime serves.
      static RENDERDOC_API_1_0_1* rdoc = [] {
        RENDERDOC_API_1_0_1* api = nullptr;
        if (void* h = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD)) {
          auto get = reinterpret_cast<pRENDERDOC_GetAPI>(dlsym(h, "RENDERDOC_GetAPI"));
          if (!get || get(eRENDERDOC_API_Version_1_0_1, reinterpret_cast<void**>(&api)) != 1)
            api = nullptr;
        }
        if (api) {
          if (const char* p = getenv("RESTUFF_RDC_PATH")) api->SetLogFilePathTemplate(p);
          REXLOG_INFO("[native_vk] RenderDoc in-app API attached (template={})",
                      api->GetLogFilePathTemplate());
        } else {
          REXLOG_ERROR("[native_vk] RESTUFF_RDC_FRAME set but librenderdoc not in process "
                       "(run with ENABLE_VULKAN_RENDERDOC_CAPTURE=1)");
        }
        return api;
      }();
      static const uint64_t rdc_target = strtoull(rdcf, nullptr, 10);
      static const uint32_t rdc_count = [] {
        const char* c = strchr(getenv("RESTUFF_RDC_FRAME"), ',');
        return c ? std::max(1, atoi(c + 1)) : 1;
      }();
      static uint64_t rdc_nth = 0;
      ++rdc_nth;
      if (rdc_nth % 500 == 0)
        REXLOG_INFO("[native_vk] RDC gameplay-frame counter {} (target {})", rdc_nth, rdc_target);
      if (rdoc && rdc_nth >= rdc_target && rdc_nth < rdc_target + rdc_count) {
        rdoc->TriggerCapture();
        REXLOG_INFO("[native_vk] RenderDoc TriggerCapture at gameplay frame {} ({} captures so far)",
                    rdc_nth, rdoc->GetNumCaptures());
      }
    }
  }
#endif  // !_WIN32
  auto FbFor = [&](uint32_t surf, bool shared_depth = false) {
    if (!surf) return tl.scene_fb;
    const auto& ax = tl.aux[surf - 1];
    return shared_depth ? ax.fb_shared_depth : ax.fb;
  };
  // M4.22 (RESTUFF_SEGSUM=1): per-frame surface-assignment census. The frame
  // arrives COMPLETE (merge audit clean) yet a flicker frame's depth is
  // missing the far field -- so the draws must be landing on the WRONG
  // SURFACE (modal-vote main_ci flip / per-draw aux misassignment). One line
  // per frame: main-scene vs aux record counts. A flicker frame = main
  // count cratering while an aux count inflates.
  {
    static const bool s_segsum = getenv("RESTUFF_SEGSUM") != nullptr;
    if (s_segsum) {
      static uint64_t s_ss_frame = 0;
      uint32_t per_surf[1 + TranslatedLayer::kAuxSurfaces] = {};
      uint32_t res_n = 0;
      for (const auto& r : recs) {
        if (r.is_resolve) { ++res_n; continue; }
        if (r.surf < 1 + TranslatedLayer::kAuxSurfaces) ++per_surf[r.surf];
      }
      REXLOG_INFO("[SEGSUM] f={} recs={} s0={} aux=({},{},{},{}) res={}", ++s_ss_frame,
                  uint32_t(recs.size()), per_surf[0], per_surf[1],
                  TranslatedLayer::kAuxSurfaces > 1 ? per_surf[2] : 0,
                  TranslatedLayer::kAuxSurfaces > 2 ? per_surf[3] : 0,
                  TranslatedLayer::kAuxSurfaces > 3 ? per_surf[4] : 0, res_n);
    }
  }
  size_t i = 0;
  while (i < recs.size()) {
    if (recs[i].is_resolve) {
      // A resolve whose source surface has had no pass this frame still needs
      // that surface in attachment layout; run an empty (init) pass first.
      const uint32_t sf = recs[i].surf;
      const bool a = sf != 0;
      bool& first = a ? aux_first[sf - 1] : first_segment;
      if (first) {
        VkRenderPassBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        bi.renderPass = a ? tl.scene_rp_clear
                          : (scene_virgin ? tl.scene_rp_clear : tl.scene_rp_newframe);
        bi.framebuffer = FbFor(sf);  // PRIVATE depth: this pass clears depth
        bi.renderArea = {{0, 0}, {SceneW(), SceneH()}};
        bi.clearValueCount = 2;
        bi.pClearValues = a ? clears_plain : clears;  // M4.10: no tint on aux
        df.vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        df.vkCmdEndRenderPass(cmd);
        first = false;
        if (!a) scene_virgin = false;
      }
      // M3.25: a DEPTH resolve ends the current depth pass (its result is saved
      // to a depth texture). The next main draw segment therefore begins a new
      // depth pass and must start from cleared depth -- else its colour z-fails
      // the saved pass's near-depth still sitting in our shared buffer (=black).
      if (recs[i].is_depth_resolve && recs[i].surf == 0) depth_saved = true;
      // M3.58: honour the guest's real depth clear (RB_COPY_CONTROL bit 9). This
      // resolve clears the depth EDRAM region; the next main segment must start
      // from cleared depth. Applies to any resolve carrying the bit (a depth
      // resolve that also clears, or a color resolve that clears depth too).
      if (recs[i].depth_clears) pending_depth_clear = true;
      // M3.83: depth resolves read the surface that last WROTE depth under this
      // di this frame (hardware: the di-named EDRAM tile), not the color base.
      uint32_t depth_src = 0;
      if (recs[i].is_depth_resolve) {
        auto w = di_writer.find(recs[i].di);
        if (w != di_writer.end()) depth_src = w->second;
      }
      if (s_pms) {
        const auto rt0 = std::chrono::steady_clock::now();
        RecordResolve(dev, cmd, recs[i], depth_src);
        g_pms_resolve_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - rt0)
                                .count();
        ++g_pms_resolve_n;
      } else {
        RecordResolve(dev, cmd, recs[i], depth_src);
      }
      GpMark(dev, cmd, kGpResolve);
      ++i;
      continue;
    }
    // M3.293: a tone_before rec starts its own segment; the tone pass runs
    // between passes right here, so the UI segment composites onto the toned
    // scene.
    if (recs[i].tone_before) {
      RecordSceneTone(dev, cmd);
      GpMark(dev, cmd, kGpTone);
    }
    size_t j = i;
    while (j < recs.size() && !recs[j].is_resolve && recs[j].surf == recs[i].surf &&
           !(j > i && recs[j].tone_before))
      ++j;
    for (size_t k = i; k < j; ++k)
      if (recs[k].zwrites) di_writer[recs[k].di] = recs[k].surf;  // M3.83
    const uint32_t sf = recs[i].surf;
    const bool a = sf != 0;
    bool just_transitioned = false;  // this main segment directly follows an aux group
    {
      // Capture the stencil the moment the frame-start AUX group finishes --
      // i.e. exactly what the shaft saw -- before the world pass overwrites
      // stencil with 0 across the frame.
      static bool aux_seen = false;
      if (a) aux_seen = true;
      if (!a && aux_seen) {
        just_transitioned = true;
        ++g_aux_trans;
        aux_seen = false;
        // RESTUFF_DUMP_AFTER_AUX=N: capture at the Nth aux->main transition
        // (N=1 lands after the minimap-stamp group g1; N=2 after the volume
        // group -- the state the shaft actually consumed). Marking the frame
        // taken even when the every-gate declines keeps the end-of-frame site
        // from also calling RecordSceneDump -- otherwise both sites advance
        // the shared frame counter and the every-gate parity strands every
        // firing at end-of-frame (post-HUD state, useless for aux questions).
        const char* daa = getenv("RESTUFF_DUMP_AFTER_AUX");
        if (daa && !g_dump_taken_this_frame && g_aux_trans == atoi(daa)) {
          RecordSceneDump(dev, cmd);
          g_dump_taken_this_frame = true;
        }
      }
    }
    // EDRAM persistence (hardware truth): scene COLOR is never cleared between
    // frames -- the game repaints what changes. DEPTH clears at each frame's
    // first segment (no cross-frame depth reuse in this title's PM4 stream).
    // RESTUFF_SCENE_CLEAR=1 restores per-frame color clearing for A/B.
    static const bool no_clear = getenv("RESTUFF_SCENE_CLEAR") == nullptr;
    bool& first = a ? aux_first[sf - 1] : first_segment;
    // M3.99: first segment of an aux group that names the MAIN depth tile gets
    // its private depth pre-filled with 2x-decimated main depth (see the TL
    // fields). Skips frame 1 (main depth still UNDEFINED) and the M3.98 opt-in
    // path (which binds the main depth directly). RESTUFF_NO_AUXDEPTH_FILL=1
    // disables.
    static const bool s_no_fill = getenv("RESTUFF_NO_AUXDEPTH_FILL") != nullptr;
    static bool s_depth_ever = false;
    const bool m399_fill = !s_no_fill && a && first && recs[i].wants_main_depth &&
                           !recs[i].shared_depth && tl.fill_pipeline != VK_NULL_HANDLE &&
                           s_depth_ever;
    VkRenderPassBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    if (a) {
      // Shared-depth aux: clear the aux COLOUR on its first segment but never
      // the depth/stencil -- that tile belongs to the main scene.
      bi.renderPass = recs[i].shared_depth
                          ? (first ? tl.scene_rp_clearcolor_cs : tl.scene_rp_load)
                          : (first ? tl.scene_rp_clear : tl.scene_rp_load);
    } else {
      bi.renderPass = (scene_virgin || (first && !no_clear)) ? tl.scene_rp_clear
                      : first                                ? tl.scene_rp_newframe
                                                             : tl.scene_rp_load;
      // M3.24 (OPT-IN, RESTUFF_DEPTH_REPASS=1): clear depth when the guest's
      // depth base changes mid-frame. MEASURED WORSE (60-79% black vs ~25%): di
      // changes at points that are NOT true depth-pass boundaries, so this
      // clears depth the later geometry legitimately needs. Kept as a lever;
      // the real fix must honour the guest's ACTUAL depth-clear PM4 events.
      static const bool depth_repass = getenv("RESTUFF_DEPTH_REPASS") != nullptr;
      if (depth_repass && !first && recs[i].di != main_di)
        bi.renderPass = tl.scene_rp_newframe;
      main_di = recs[i].di;
      // M3.25 (OPT-IN, RESTUFF_DEPTH_REPASS2=1): clear depth for the first main
      // segment after a main depth resolve. MEASURED 0% black BUT washed-flat
      // haze -- the depth resolves happen MID-world, and the depth buffer is
      // also consumed by the DOF pass, so clearing it wipes both the world's
      // own occlusion and the DOF depth input. Not the fix; see the note below.
      static const bool depth_repass2 = getenv("RESTUFF_DEPTH_REPASS2") != nullptr;
      if (depth_repass2 && !first && depth_saved) bi.renderPass = tl.scene_rp_newframe;
      depth_saved = false;
      // M3.58 (DEFAULT ON; RESTUFF_NO_DEPTH_HONOR=1 for A/B): clear depth at the
      // first main segment after a guest depth_clear_enable resolve -- the game's
      // true "fresh depth pass" boundary. Precise where DEPTH_REPASS2 was not: it
      // fires ONLY on real clears, so mid-world DOF/shadow depth saves keep their
      // occlusion (no wash-out haze) while a genuinely-fresh pass (the floor,
      // z-failing against a stale saved near-depth) starts clean and renders.
      static const bool honor_dclear = getenv("RESTUFF_NO_DEPTH_HONOR") == nullptr;
      if (honor_dclear && !first && pending_depth_clear)
        bi.renderPass = tl.scene_rp_newframe;
      pending_depth_clear = false;
      // RESTUFF_DEPTH_CLEAR_ALL=1: clear depth at EVERY main segment (A/B).
      static const bool clr_all = getenv("RESTUFF_DEPTH_CLEAR_ALL") != nullptr;
      if (clr_all && !first) bi.renderPass = tl.scene_rp_newframe;
      scene_virgin = false;
    }
    // Shared-depth aux segments render against the MAIN depth/stencil.
    if (getenv("RESTUFF_SEGLOG")) {
      // Only AUX segments matter here, and only once the frame is big enough to
      // be gameplay (early menu frames are 1 draw each). main_drawn tells us
      // whether the world has already written depth when the mask pass runs --
      // if the aux segment precedes it, nothing can z-fail and no mask forms.
      static std::atomic<int> s_seg{60};
      static uint32_t main_drawn = 0;
      if (sf == 0) main_drawn += uint32_t(j - i);
      if ((sf != 0 || just_transitioned) && recs.size() > 200 &&
          s_seg.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[SEG] surf={} shared={} draws={} main_drawn_before={} nrecs={} rp={}", sf,
                    int(recs[i].shared_depth), int(j - i), main_drawn, int(recs.size()),
                    bi.renderPass == tl.scene_rp_clear           ? "clear"
                    : bi.renderPass == tl.scene_rp_newframe      ? "newframe"
                    : bi.renderPass == tl.scene_rp_clearcolor    ? "clearcolor"
                    : bi.renderPass == tl.scene_rp_clearcolor_cs ? "clearcolor_CS"
                                                                 : "load");
    }
    bi.framebuffer = FbFor(sf, recs[i].shared_depth);
    if (getenv("RESTUFF_FBLOG") && sf != 0) {
      static std::atomic<int> s_fb{12};
      if (s_fb.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[FB] surf={} shared_flag={} bound_is_shared={} draws={}", sf,
                    int(recs[i].shared_depth),
                    int(bi.framebuffer == tl.aux[sf - 1].fb_shared_depth), int(j - i));
    }
    bi.renderArea = {{0, 0}, {SceneW(), SceneH()}};
    bi.clearValueCount = 2;
    bi.pClearValues = a ? clears_plain : clears;  // M4.10: no tint on aux
    if (m399_fill) {
      // Main depth: attachment -> shader-readable for the fill's texelFetch.
      VkImageMemoryBarrier mb = {};
      mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      mb.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      mb.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      mb.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      mb.image = tl.depth_img;
      mb.subresourceRange = vk::util::InitializeSubresourceRange();
      mb.subresourceRange.aspectMask = kDepthAttAspect;
      df.vkCmdPipelineBarrier(cmd,
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                              1, &mb);
      if (!tl.fill_set_written) {
        VkDescriptorImageInfo ii = {DL().sampler_nearest, tl.depth_sample_view,
                                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w = {};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = tl.fill_set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        df.vkUpdateDescriptorSets(dev->device(), 1, &w, 0, nullptr);
        tl.fill_set_written = true;
      }
    }
    df.vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0.0f, 0.0f, float(SceneW()), float(SceneH()), 0.0f, 1.0f};
    df.vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {SceneW(), SceneH()}};
    df.vkCmdSetScissor(cmd, 0, 1, &scissor);
    if (m399_fill) {
      // Depth-only fullscreen triangle over the aux pass's viewport; guest
      // volume draws then z-fail against REAL scene depth. Viewport/scissor
      // restored to full afterwards (RecordTranslatedDraws assumes it).
      const float fw = recs[i].vp[2] > 0.0f ? recs[i].vp[2] : 640.0f;
      const float fh = recs[i].vp[3] > 0.0f ? recs[i].vp[3] : 360.0f;
      VkViewport fvp = {0.0f, 0.0f, fw, fh, 0.0f, 1.0f};
      VkRect2D fsc = {{0, 0}, {uint32_t(fw), uint32_t(fh)}};
      df.vkCmdSetViewport(cmd, 0, 1, &fvp);
      df.vkCmdSetScissor(cmd, 0, 1, &fsc);
      df.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.fill_pipeline);
      df.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl.pipeline_layout, 1, 1,
                                 &tl.fill_set, 0, nullptr);
      df.vkCmdDraw(cmd, 3, 1, 0, 0);
      df.vkCmdSetViewport(cmd, 0, 1, &viewport);
      df.vkCmdSetScissor(cmd, 0, 1, &scissor);
      static std::atomic<uint64_t> s_fill_count{0};
      const uint64_t fc = s_fill_count.fetch_add(1, std::memory_order_relaxed);
      if (fc < 4 || (fc & 0x3FF) == 0)
        REXLOG_INFO("[M399] fill #{} surf={} vp={}x{} di=0x{:03X}", fc, sf,
                    int(recs[i].vp[2]), int(recs[i].vp[3]), recs[i].di);
    }
    RecordTranslatedDraws(dev, cmd, recs, i, j);
    df.vkCmdEndRenderPass(cmd);
    GpMark(dev, cmd, a ? kGpAux : kGpMain);
    // RESTUFF_DUMP_EACH_AUX=<frame#>: on that one frame, snapshot the
    // stencil after EVERY aux segment -- per-segment attribution of the
    // volume group's marks (which draw writes the net -1s).
    if (g_eaux_this_frame && !a) {
      static std::atomic<int> s_wz{6};
      if (s_wz.fetch_sub(1, std::memory_order_relaxed) > 0)
        REXLOG_INFO("[EAUX] WORLD seg draws={} vs={:016X} z=[{},{}]", int(j - i),
                    recs[i].vs_hash, recs[i].z_min, recs[i].z_max);
    }
    if (g_eaux_this_frame && (a || (j - i) < 8)) {
      g_dump_taken_this_frame = true;  // route this frame into WriteSceneDumpPpm
      for (size_t k2 = i; k2 < j; ++k2)
        REXLOG_INFO("[EAUX] surf={} shared={} draw {}/{} vs={:016X} di=0x{:03X} vp={}x{} "
                    "z=[{},{}]",
                    sf, int(recs[i].shared_depth), int(k2 - i + 1), int(j - i),
                    recs[k2].vs_hash, recs[k2].di, int(recs[k2].vp[2]),
                    int(recs[k2].vp[3]), recs[k2].z_min, recs[k2].z_max);
      RecordSceneDump(dev, cmd, /*force=*/true);
    }
    if (m399_fill) {
      VkImageMemoryBarrier mb = {};
      mb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      mb.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
      mb.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      mb.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
      mb.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
      mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      mb.image = tl.depth_img;
      mb.subresourceRange = vk::util::InitializeSubresourceRange();
      mb.subresourceRange.aspectMask = kDepthAttAspect;
      df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                  VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                              0, 0, nullptr, 0, nullptr, 1, &mb);
    }
    if (!a) s_depth_ever = true;  // main depth now has content + known layout
    first = false;
    i = j;
  }
}

// Diagnostic (RESTUFF_DUMP_SCENE=1): records a full-frame copy of scene_img
// (which is in COLOR_ATTACHMENT_OPTIMAL after RecordSceneFrame) into a lazily
// allocated host-visible buffer. write_scene_dump_ppm() then writes it to
// scene_dump/ after the frame fence. Answers the ground-truth question "did the
// 3D world geometry actually land in the scene target?" independently of the
// resolve/present path. Returns true when a copy was recorded this frame.
VkBuffer g_scene_dump_buf = VK_NULL_HANDLE;
VkDeviceMemory g_scene_dump_mem = VK_NULL_HANDLE;
void* g_scene_dump_ptr = nullptr;
struct SceneDumpEntry { uint32_t addr; uint32_t w, h; VkDeviceSize offset; bool is_depth; bool is_stencil = false; };
std::vector<SceneDumpEntry> g_scene_dump_entries;

bool RecordSceneDump(vk::VulkanDevice* dev, VkCommandBuffer cmd, bool force) {
  static const char* env = getenv("RESTUFF_DUMP_SCENE");
  if (!env && !force) return false;
  static uint64_t frame = 0;
  static const uint64_t every =
      getenv("RESTUFF_DUMP_EVERY") ? strtoull(getenv("RESTUFF_DUMP_EVERY"), nullptr, 10) : 250;
  // RESTUFF_DUMP_START=N: don't dump until frame N (skip menu frames so a
  // DUMP_EVERY=1 burst lands on consecutive GAMEPLAY frames).
  static const uint64_t start =
      getenv("RESTUFF_DUMP_START") ? strtoull(getenv("RESTUFF_DUMP_START"), nullptr, 10) : 0;
  const uint64_t f = frame++;
  if ((f % 1000) == 0)
    REXLOG_INFO("[DUMPDBG] f={} start={} every={}", f, start, every);
  if (!force) {
    if (f < start) return false;
    const bool fire = ((f - start) % every) == every - 1;
    if (!fire) return false;
  }
  REXLOG_INFO("[DUMPDBG] FIRE f={}", f);
  const auto& df = dev->functions();
  // RESTUFF_DUMP_RT: unset -> dump scene_img; "all" -> dump every rt_tex (the
  // whole resolve chain, so we can see exactly where the world is lost); <hex>
  // -> that one rt_tex. rt images are SHADER_READ_ONLY_OPTIMAL post-resolve.
  static const char* rt_env = getenv("RESTUFF_DUMP_RT");
  static const bool dump_all = rt_env && strcmp(rt_env, "all") == 0;
  static const uint32_t rt_addr =
      (rt_env && !dump_all) ? uint32_t(strtoul(rt_env, nullptr, 16)) : 0;
  // 64 MB buffer: fits scene_img plus every rt_tex packed at 4 B/px.
  // *3 so colour+depth+mid-depth (RESTUFF_DUMP_DEPTH/MID_DEPTH) all fit.
  // M3.83: DUMP_AUX also appends every rt_tex (glow-chain stage inspection).
  const VkDeviceSize kCap = (dump_all || getenv("RESTUFF_DUMP_AUX"))
                                ? (192ull << 20)  // 16 aux + all rt_tex fit
                            : getenv("RESTUFF_DUMP_EACH_AUX")
                                ? (192ull << 20)  // per-segment stencil+depth
                                : VkDeviceSize(SceneW()) * SceneH() * 4 * 3;
  if (!g_scene_dump_buf) {
    if (!vk::util::CreateDedicatedAllocationBuffer(dev, kCap, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                                   vk::util::MemoryPurpose::kUpload,
                                                   g_scene_dump_buf, g_scene_dump_mem, nullptr,
                                                   nullptr)) {
      REXLOG_ERROR("[DUMPDBG] dump buffer create FAILED (cap={})", (uint64_t)kCap);
      return false;
    }
    if (df.vkMapMemory(dev->device(), g_scene_dump_mem, 0, VK_WHOLE_SIZE, 0, &g_scene_dump_ptr) !=
        VK_SUCCESS) {
      REXLOG_ERROR("[DUMPDBG] dump buffer map FAILED");
      return false;
    }
  }
  auto& tl = TL();

  // Build the list of (image, addr, w, h, layout, aspect) to copy.
  struct Src { VkImage image; uint32_t addr, w, h; VkImageLayout layout; VkImageAspectFlags aspect; };
  std::vector<Src> srcs;
  const VkImageAspectFlags kColor = VK_IMAGE_ASPECT_COLOR_BIT, kDepth = VK_IMAGE_ASPECT_DEPTH_BIT;
  if (dump_all) {
    // scene_img too (addr 0 -> scene_NNN.ppm): same-frame orientation
    // comparisons need the working surface alongside its resolves.
    srcs.push_back(
        {tl.scene_img, 0, SceneW(), SceneH(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColor});
    for (auto& [addr, t] : tl.rt_tex)
      if (t.image && t.width && t.height)
        srcs.push_back({t.image, uint32_t(addr & ~1ull), t.width, t.height,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        t.is_depth ? kDepth : kColor});
  } else if (rt_addr) {
    // rt_tex is keyed by (addr|depth-bit); a requested address may be either
    // kind (the sun-shaft's source at 0x0582C000 is DEPTH), so try both.
    auto it = tl.rt_tex.find(RtTexKey(rt_addr, false));
    if (it == tl.rt_tex.end()) it = tl.rt_tex.find(RtTexKey(rt_addr, true));
    if (it == tl.rt_tex.end() || !it->second.image) return false;
    srcs.push_back({it->second.image, rt_addr, it->second.width, it->second.height,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    it->second.is_depth ? kDepth : kColor});
  } else if (getenv("RESTUFF_DUMP_AUX")) {
    // Dump the scene (scene_NNN) AND the aux surfaces (rt_000000A0/A1) that hold
    // the light/shadow buffer the world shader multiplies by. If an aux surface
    // is black where the scene is black, the modulation is the cause.
    srcs.push_back(
        {tl.scene_img, 0, SceneW(), SceneH(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColor});
    for (uint32_t a = 0; a < TranslatedLayer::kAuxSurfaces; ++a)
      if (tl.aux[a].img)
        srcs.push_back({tl.aux[a].img, 0xA0 + a, SceneW(), SceneH(),
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColor});
    // M3.83: plus every resolve target -- the glow chain is aux render ->
    // resolve (0B51E000) -> composite, and any stage can be the dead one.
    for (auto& [addr, t] : tl.rt_tex)
      if (t.image && t.width && t.height)
        srcs.push_back({t.image, uint32_t(addr & ~1ull), t.width, t.height,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        t.is_depth ? kDepth : kColor});
  } else if (getenv("RESTUFF_DUMP_DEPTH")) {
    // Dump colour (scene_NNN) AND depth (rt_0000000D_NNN) for the SAME frame so
    // the black region can be correlated with the near-depth region.
    srcs.push_back(
        {tl.scene_img, 0, SceneW(), SceneH(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColor});
    // RESTUFF_DUMP_STENCIL=1: the guest's stencil MASK itself (aspect
    // STENCIL, 1 byte/px) -- the shaft only draws where this is non-zero, so
    // this is the direct answer to "did the mask form".
    if (getenv("RESTUFF_DUMP_STENCIL"))
      srcs.push_back({tl.depth_img, 0x5, SceneW(), SceneH(),
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_ASPECT_STENCIL_BIT});
    // RESTUFF_DUMP_AUXDEPTH=1: aux[0]'s private depth (0xA0D) + stencil (0xA05)
    // -- shows whether the M3.99 pre-fill landed and whether the volume marks
    // formed against it.
    if (getenv("RESTUFF_DUMP_AUXDEPTH") && tl.aux[0].depth_img) {
      srcs.push_back({tl.aux[0].depth_img, 0xA0D, SceneW(), SceneH(),
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_ASPECT_DEPTH_BIT});
      srcs.push_back({tl.aux[0].depth_img, 0xA05, SceneW(), SceneH(),
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_ASPECT_STENCIL_BIT});
    }
    srcs.push_back({tl.depth_img, 0xD, SceneW(), SceneH(),
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, kDepth});
  } else {
    srcs.push_back(
        {tl.scene_img, 0, SceneW(), SceneH(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kColor});
  }

  // Forced (per-aux-segment) calls APPEND: each snapshot gets its own buffer
  // region and PPM seq; clearing per call would leave only the last segment.
  if (force) {
    srcs.clear();
    srcs.push_back({tl.depth_img, 0x5, SceneW(), SceneH(),
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_ASPECT_STENCIL_BIT});
    srcs.push_back({tl.depth_img, 0xD, SceneW(), SceneH(),
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_ASPECT_DEPTH_BIT});
  } else {
    g_scene_dump_entries.clear();
  }
  VkDeviceSize offset = 0;
  for (const SceneDumpEntry& e : g_scene_dump_entries)
    offset = std::max(offset,
                      e.offset + VkDeviceSize(e.w) * e.h * (e.is_stencil ? 1 : 4));
  for (const Src& s : srcs) {
    // S8_UINT is 1 byte/px; colour and D32 are 4.
    const VkDeviceSize sz =
        VkDeviceSize(s.w) * s.h * ((s.aspect == VK_IMAGE_ASPECT_STENCIL_BIT) ? 1 : 4);
    if (offset + sz > kCap) break;
    const bool color_att = s.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkPipelineStageFlags stage = color_att ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                                 : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkAccessFlags access =
        color_att ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = access;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = s.layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = s.image;
    b.subresourceRange = vk::util::InitializeSubresourceRange();
    // combined depth/stencil images must name BOTH aspects in a barrier
    b.subresourceRange.aspectMask =
        (s.aspect == VK_IMAGE_ASPECT_DEPTH_BIT) ? kDepthAttAspect : s.aspect;
    df.vkCmdPipelineBarrier(cmd, stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                            &b);
    VkBufferImageCopy region = {};
    region.bufferOffset = offset;
    region.imageSubresource = {s.aspect, 0, 0, 1};  // copy: DEPTH aspect only
    region.imageExtent = {s.w, s.h, 1};
    df.vkCmdCopyImageToBuffer(cmd, s.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_scene_dump_buf, 1,
                              &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = access;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = s.layout;
    df.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, stage, 0, 0, nullptr, 0, nullptr, 1,
                            &b);
    g_scene_dump_entries.push_back(
        {s.addr, s.w, s.h, offset, s.aspect == VK_IMAGE_ASPECT_DEPTH_BIT,
         s.aspect == VK_IMAGE_ASPECT_STENCIL_BIT});
    offset += sz;
  }
  return !g_scene_dump_entries.empty();
}

void WriteSceneDumpPpm() {
  if (!g_scene_dump_ptr) return;
  static int idx = 0;
  const uint8_t* base = static_cast<const uint8_t*>(g_scene_dump_ptr);
  // Per-aux-segment (EACH_AUX) batches carry MANY entries of the same image;
  // a per-call seq would collapse them onto one filename. Number per entry,
  // bumping seq whenever an (addr,size) repeats within the batch.
  std::set<std::tuple<uint32_t, uint32_t, uint32_t>> seen_in_batch;
  int seq = idx;
  for (const SceneDumpEntry& e : g_scene_dump_entries) {
    if (!seen_in_batch.insert({e.addr, e.w, e.h}).second) {
      ++seq;
      seen_in_batch.clear();
      seen_in_batch.insert({e.addr, e.w, e.h});
    }
    idx = std::max(idx, seq + 1);
    // RESTUFF_DUMP_RAWDEPTH=1: raw f32 sidecar for depth entries -- numeric
    // comparison against the reference capture's raw depth (PPM quantization
    // made value-scale comparisons invalid).
    if (e.is_depth && getenv("RESTUFF_DUMP_RAWDEPTH")) {
      char rpath[160];
      snprintf(rpath, sizeof(rpath), "scene_dump/rt_%08X_%ux%u_%03d.f32", e.addr, e.w, e.h, seq);
      if (FILE* rf = fopen(rpath, "wb")) {
        fwrite(base + e.offset, 4, size_t(e.w) * e.h, rf);
        fclose(rf);
      }
    }
    char path[160];
    if (e.addr)
      snprintf(path, sizeof(path), "scene_dump/rt_%08X_%ux%u_%03d.ppm", e.addr, e.w, e.h, seq);
    else
      snprintf(path, sizeof(path), "scene_dump/scene_%03d.ppm", seq);
    if (FILE* f = fopen(path, "wb")) {
      fprintf(f, "P6\n%u %u\n255\n", e.w, e.h);
      const uint8_t* p = base + e.offset;
      for (size_t i = 0; i < size_t(e.w) * e.h; ++i) {
        if (e.is_stencil) {
          // S8_UINT: one byte per pixel, written straight through so a
          // non-zero mask is immediately visible.
          const uint8_t v = p[i];
          fwrite(&v, 1, 1, f); fwrite(&v, 1, 1, f); fwrite(&v, 1, 1, f);
        } else if (e.is_depth) {
          // D32 float depth -> grayscale (near..far reads as a smooth ramp,
          // vs the rainbow color garbage the old color-fill produced).
          float d;
          std::memcpy(&d, p + i * 4, 4);
          uint8_t g = uint8_t(std::clamp(d, 0.0f, 1.0f) * 255.0f + 0.5f);
          fwrite(&g, 1, 1, f); fwrite(&g, 1, 1, f); fwrite(&g, 1, 1, f);
        } else {
          // scene_img is VK_FORMAT_A2B10G10R10_UNORM_PACK32 (kGuestOutputFormat)
          // -- a BIT-packed 10/10/10/2, NOT byte-aligned RGBA8. Reading raw
          // bytes 0..2 scrambles the channels and produces a rainbow-speckle
          // artifact on otherwise-correct geometry (this decoder bug, not the
          // renderer, was the long-hunted "rainbow garbage"). Unpack properly.
          uint32_t v;
          std::memcpy(&v, p + i * 4, 4);
          const uint8_t r = uint8_t((v & 0x3FF) >> 2);
          const uint8_t g = uint8_t(((v >> 10) & 0x3FF) >> 2);
          const uint8_t b = uint8_t(((v >> 20) & 0x3FF) >> 2);
          fwrite(&r, 1, 1, f); fwrite(&g, 1, 1, f); fwrite(&b, 1, 1, f);
        }
      }
      fclose(f);
    }
  }
  if (!g_scene_dump_entries.empty())
    REXLOG_INFO("[native_vk] scene dump written: {} images seq {}", g_scene_dump_entries.size(),
                seq);
}

}  // namespace

bool NativeVulkanGraphicsSystem::PresentClearFrame() {
  if (!presenter_ || !provider_) {
    return false;
  }
  auto& dl = DL();

  // M3.135: start of the "call the presenter" stretch (see g_pms_cb_end).
  g_pms_rgo_call = std::chrono::steady_clock::now();

  // Pull the latest published guest frame (keep drawing the previous copy when
  // the game presents slower than we refresh). The translated path pulls its own
  // raw frame inside PrepareTranslatedDraws.
  if (!REXCVAR_GET(use_translated_shaders)) {
    std::vector<renderer::DecodedDraw> fresh;
    if (renderer::ConsumeReadyFrame(fresh)) {
      dl.frame_draws = std::move(fresh);
    }
  }

  const bool _rgo_ok = presenter_->RefreshGuestOutput(
      1280, 720, 16, 9,
      [this, &dl](rex::ui::Presenter::GuestOutputRefreshContext& base) -> bool {
        auto& ctx =
            static_cast<vk::VulkanPresenter::VulkanGuestOutputRefreshContext&>(base);
        vk::VulkanDevice* dev = provider_->vulkan_device();
        const auto& df = dev->functions();
        VkDevice device = dev->device();
        // M3.135b: the presenter reaches our callback almost immediately --
        // RefreshGuestOutput is a triple-buffered MAILBOX (presenter.cpp:553),
        // with no swapchain acquire and no fence wait on this path (the real
        // Paint, which does vkAcquireNextImageKHR, runs on the UI thread). So
        // the stretch from the RefreshGuestOutput call to the record section is
        // mostly OUR OWN draw preparation below, not the SDK's. Timestamp the
        // callback entry to tell the two apart instead of assuming.
        g_pms_cb_start = std::chrono::steady_clock::now();

        // M4.5: frame-slot selection. Serialized mode (default) pins slot 0
        // and waits right after submit exactly as always. Pipelined mode
        // (RESTUFF_PIPELINED=1, see PipelinedMode) alternates slots: wait HERE
        // for the fence of the frame submitted two callbacks ago from this
        // slot, retire its resources, then prepare/record/submit without a
        // trailing wait -- the CPU works on frame N+1 while the GPU runs N.
        // The locals shadow the historical single-buffered member names so
        // the body below stays textually unchanged.
        const bool pipelined = PipelinedMode();
        TL().slot_ix = frame_slot_;
        VkCommandBuffer cmd_buf_ = cmd_bufs_[frame_slot_];
        VkFence fence_ = fences_[frame_slot_];
        if (pipelined && slot_submitted_[frame_slot_]) {
          df.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
          slot_submitted_[frame_slot_] = false;
          RetireFrameSlot(dev, TL().slots[frame_slot_]);
        }

        // Resolve textures + build the frame's draws (may queue staging uploads
        // for this command buffer). Translated path = raw VB + on-GPU transform;
        // else the heuristic path = CPU-transformed Draw2DVertex.
        const bool translated = REXCVAR_GET(use_translated_shaders);
        std::vector<TransDrawRec> trans;
        struct DrawRange {
          uint32_t first = 0, count = 0;
          VkDescriptorSet set = VK_NULL_HANDLE;
          VkPipeline pipeline = VK_NULL_HANDLE;
        };
        std::vector<DrawRange> ranges;
        auto& verts = dl.vertex_scratch;
        verts.clear();
        if (translated) {
          if (SetupTranslatedLayer(dev) && EnsureSceneTarget(dev)) {
            // M4.0: everything a pipeline build needs now exists (layout +
            // scene render pass, both guest-independent) -- start replaying
            // the recorded pipeline population while the guest still boots.
            MaybeStartPipelinePrewarm(dev);
            trans = PrepareTranslatedDraws(dev);
          }
          // Present quad (fullscreen, D3D NDC, y-up): samples the resolved
          // front buffer inside the swapchain pass. kPremul blend makes it an
          // opaque copy over the cleared backdrop regardless of source alpha.
          verts.push_back({-1.f, 1.f, 0.f, 0.f, 0xFFFFFFFFu, 0xFFFFFFFFu});
          verts.push_back({1.f, 1.f, 1.f, 0.f, 0xFFFFFFFFu, 0xFFFFFFFFu});
          verts.push_back({-1.f, -1.f, 0.f, 1.f, 0xFFFFFFFFu, 0xFFFFFFFFu});
          verts.push_back({1.f, 1.f, 1.f, 0.f, 0xFFFFFFFFu, 0xFFFFFFFFu});
          verts.push_back({1.f, -1.f, 1.f, 1.f, 0xFFFFFFFFu, 0xFFFFFFFFu});
          verts.push_back({-1.f, -1.f, 0.f, 1.f, 0xFFFFFFFFu, 0xFFFFFFFFu});
        } else if (dl.ready) {
          ranges.reserve(dl.frame_draws.size());
          for (const auto& d : dl.frame_draws) {
            if (d.verts.empty()) continue;
            VkDescriptorSet set = ResolveTexture(dev, d.tex);
            if (set == VK_NULL_HANDLE) continue;
            DrawRange r;
            r.first = uint32_t(verts.size());
            r.count = uint32_t(d.verts.size());
            r.set = set;
            r.pipeline = dl.pipelines[d.family == renderer::DrawFamily::kText ? 1 : 0]
                                     [uint32_t(d.blend)];
            verts.insert(verts.end(), d.verts.begin(), d.verts.end());
            ranges.push_back(r);
          }
        }
        {
          const VkDeviceSize vb_bytes = verts.size() * sizeof(renderer::Draw2DVertex);
          if (vb_bytes) {
            if (!EnsureVertexCapacity(dev, vb_bytes)) {
              ranges.clear();
              verts.clear();
            } else {
              // M4.5: under pipelining the previous frame may still read this
              // buffer. In translated mode (the only pipelined mode) the
              // content is the constant fullscreen present quad, so identical
              // rewrites are skipped outright; the only actual write is the
              // very first frame, before anything was ever submitted.
              bool write_2d = true;
              if (pipelined) {
                uint64_t sig = 1469598103934665603ull;
                const uint8_t* p = reinterpret_cast<const uint8_t*>(verts.data());
                for (VkDeviceSize i = 0; i < vb_bytes; ++i)
                  sig = (sig ^ p[i]) * 1099511628211ull;
                static uint64_t s_2d_sig = 0;
                write_2d = sig != s_2d_sig;
                s_2d_sig = sig;
              }
              if (write_2d) {
                std::memcpy(dl.vb_mapped, verts.data(), vb_bytes);
                vk::util::FlushMappedMemoryRange(dev, dl.vb_mem, dl.vb_mem_type, 0,
                                                 dl.vb_mem_size, vb_bytes);
              }
            }
          }
        }

        // M3.100: (re)upload the guest gamma-ramp LUT when it changed. Rides
        // the standard pending-upload flow (UNDEFINED -> TRANSFER -> SHADER
        // READ barriers + staging retirement).
        static const bool s_no_ramp = getenv("RESTUFF_NO_GAMMA_RAMP") != nullptr;
        {
          const uint32_t rv = restuff::native::GetGammaRampVersion();
          if (!s_no_ramp && dl.gamma_pipeline && rv != 0 && rv != dl.gamma_uploaded_version) {
            uint32_t packed[256];
            restuff::native::CopyGammaRamp(packed);
            PendingUpload up;
            up.image = dl.gamma_lut_img;
            up.width = 256;
            up.height = 1;
            uint32_t st = 0;
            VkDeviceSize ss = 0;
            if (vk::util::CreateDedicatedAllocationBuffer(
                    dev, sizeof(packed), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    vk::util::MemoryPurpose::kUpload, up.staging, up.staging_mem, &st, &ss)) {
              void* mapped = nullptr;
              if (df.vkMapMemory(device, up.staging_mem, 0, VK_WHOLE_SIZE, 0, &mapped) ==
                  VK_SUCCESS) {
                std::memcpy(mapped, packed, sizeof(packed));
                vk::util::FlushMappedMemoryRange(dev, up.staging_mem, st, 0, ss, sizeof(packed));
                df.vkUnmapMemory(device, up.staging_mem);
                dl.pending_uploads.push_back(up);
                dl.gamma_uploaded_version = rv;
                REXLOG_INFO("[native_vk] M3.100 gamma ramp uploaded (version {})", rv);
              }
            }
          }
        }

        VkCommandBufferBeginInfo bi = {};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        df.vkBeginCommandBuffer(cmd_buf_, &bi);  // implicit reset (pool flag)
        GpFrameBegin(dev, cmd_buf_);  // M4.33: reset+base stamp (no-op unless on)

        // M4.5 frame-head global barrier (pipelined only): everything GPU-side
        // that used to rely on the between-frames fence wait -- in-place
        // texture refreshes (M4.0 WAR), resolve_buf/ds2x scratch reuse, the
        // persistent EDRAM-model scene/aux/rt images -- is a same-queue hazard
        // between cmd buffer N and N+1. One all-commands barrier makes frame
        // N+1's GPU work execute after frame N's completes, restoring exactly
        // the ordering the fence provided. It costs nothing we wanted to keep:
        // the goal is CPU/GPU overlap, and GPU work is inherently serial on
        // the single queue (scene content is order-dependent regardless).
        if (pipelined) {
          VkMemoryBarrier mb = {};
          mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
          mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
          mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
          df.vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                  VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0,
                                  nullptr);
        }

        // Record pending texture uploads before the render pass.
        for (const auto& up : dl.pending_uploads) {
          VkImageMemoryBarrier b = {};
          b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
          b.srcAccessMask = 0;
          b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
          b.image = up.image;
          b.subresourceRange = vk::util::InitializeSubresourceRange();
          df.vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                                  &b);
          VkBufferImageCopy region = {};
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {up.width, up.height, 1};
          df.vkCmdCopyBufferToImage(cmd_buf_, up.staging, up.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
          b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
          b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          df.vkCmdPipelineBarrier(cmd_buf_, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                                  nullptr, 1, &b);
          dl.retired_uploads.push_back(up);
        }
        dl.pending_uploads.clear();

        // M2.4: render the guest frame into the offscreen scene target and
        // perform its resolves BEFORE the presented render pass.
        bool scene_dumped = false;
        GpMark(dev, cmd_buf_, kGpUpload);  // M4.33: frame head -> here = uploads/barriers
        if (translated && !trans.empty()) {
          RecordSceneFrame(dev, cmd_buf_, trans);
          // may already have fired mid-frame (RESTUFF_DUMP_AFTER_AUX)
          scene_dumped = g_dump_taken_this_frame || RecordSceneDump(dev, cmd_buf_);
        }

        // Render pass: loadOp CLEAR is both the backdrop and the no-draws
        // fallback (cornflower until real frames land, black once they do).
        VkClearValue clear = {};
        if (translated ? trans.empty() : dl.frame_draws.empty()) {
          clear.color.float32[0] = 0.39f;
          clear.color.float32[1] = 0.58f;
          clear.color.float32[2] = 0.93f;
        } else {
          // M4.10: same RESTUFF_CLEAR_RGB tint on the swapchain backdrop, so
          // a whole-frame drop (nothing composited) is unambiguous too.
          struct ClearRGB { float r, g, b; bool on; };
          static const ClearRGB s_bd_rgb = [] {
            ClearRGB c{0, 0, 0, false};
            if (const char* e = getenv("RESTUFF_CLEAR_RGB"))
              c.on = std::sscanf(e, "%f,%f,%f", &c.r, &c.g, &c.b) == 3;
            return c;
          }();
          if (s_bd_rgb.on) {
            clear.color.float32[0] = s_bd_rgb.r;
            clear.color.float32[1] = s_bd_rgb.g;
            clear.color.float32[2] = s_bd_rgb.b;
          }
        }
        clear.color.float32[3] = 1.0f;
        VkRenderPassBeginInfo rp_bi = {};
        rp_bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rp_bi.renderPass = dl.render_pass;
        rp_bi.framebuffer =
            GetFramebuffer(dev, ctx.image_view(), ctx.image_version(), 1280, 720);
        rp_bi.renderArea = {{0, 0}, {1280, 720}};
        rp_bi.clearValueCount = 1;
        rp_bi.pClearValues = &clear;
        df.vkCmdBeginRenderPass(cmd_buf_, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);

        {
          VkViewport viewport = {0.0f, 0.0f, 1280.0f, 720.0f, 0.0f, 1.0f};
          df.vkCmdSetViewport(cmd_buf_, 0, 1, &viewport);
          VkRect2D scissor = {{0, 0}, {1280, 720}};
          df.vkCmdSetScissor(cmd_buf_, 0, 1, &scissor);
          // Guest positions are D3D NDC (y-up): flip to Vulkan clip space.
          const float pc[4] = {1.0f, -1.0f, 0.0f, 0.0f};  // scale.xy, offset.xy
          if (translated) {
            // Present the resolved front buffer (rt_tex[fb_ptr]) as a
            // fullscreen quad through the heuristic 2D pipeline.
            auto& tl = TL();
            auto it = tl.rt_tex.find(tl.frame_fb ? tl.frame_fb : renderer::GetFrontBufferPhys());
            if (it != tl.rt_tex.end() && !verts.empty()) {
              // M3.100: present through the display gamma ramp once the guest
              // has programmed one (identity before that -- skip the LUT).
              const bool use_ramp = !s_no_ramp && dl.gamma_pipeline &&
                                    dl.gamma_uploaded_version != 0;
              const VkPipelineLayout play =
                  use_ramp ? dl.gamma_pipeline_layout : dl.pipeline_layout;
              df.vkCmdPushConstants(cmd_buf_, play, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc),
                                    pc);
              const VkDeviceSize vb_offset = 0;
              df.vkCmdBindVertexBuffers(cmd_buf_, 0, 1, &dl.vb, &vb_offset);
              // M3.293: tone moved INTO the frame (scene-RT pass at the 3D->UI
              // boundary; see RecordSceneTone). Present is plain LUT for every
              // frame -- no frame-level tone selection ever again (a count gate
              // fried episode select; a global curve fried the title).
              df.vkCmdBindPipeline(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   use_ramp
                                       ? dl.gamma_pipeline
                                       : dl.pipelines[0][uint32_t(renderer::BlendMode::kPremul)]);
              df.vkCmdBindDescriptorSets(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 0, 1,
                                         &it->second.set, 0, nullptr);
              if (use_ramp)
                df.vkCmdBindDescriptorSets(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS, play, 1, 1,
                                           &dl.gamma_set, 0, nullptr);
              df.vkCmdDraw(cmd_buf_, 6, 1, 0, 0);
            }
          } else if (!ranges.empty()) {
            df.vkCmdPushConstants(cmd_buf_, dl.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                                  sizeof(pc), pc);
            const VkDeviceSize vb_offset = 0;
            df.vkCmdBindVertexBuffers(cmd_buf_, 0, 1, &dl.vb, &vb_offset);
            VkDescriptorSet bound = VK_NULL_HANDLE;
            VkPipeline bound_pipeline = VK_NULL_HANDLE;
            for (const DrawRange& r : ranges) {
              if (r.pipeline != bound_pipeline) {
                df.vkCmdBindPipeline(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipeline);
                bound_pipeline = r.pipeline;
              }
              if (r.set != bound) {
                df.vkCmdBindDescriptorSets(cmd_buf_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                           dl.pipeline_layout, 0, 1, &r.set, 0, nullptr);
                bound = r.set;
              }
              df.vkCmdDraw(cmd_buf_, r.count, 1, r.first, 0);
            }
            dl.draws_rendered.fetch_add(ranges.size(), std::memory_order_relaxed);
          }
        }

        df.vkCmdEndRenderPass(cmd_buf_);  // -> kGuestOutputInternalLayout via finalLayout
        GpMark(dev, cmd_buf_, kGpPresent);  // M4.33: swapchain quad + gamma LUT
        df.vkEndCommandBuffer(cmd_buf_);

        df.vkResetFences(device, 1, &fence_);
        VkSubmitInfo si = {};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd_buf_;
        // M3.124b: phase timing for the present thread (RESTUFF_PRESENTMS=1).
        // The frame is fully serialized here -- record, submit, BLOCK on the
        // whole GPU frame, then memcpy every resolve into guest RAM -- so the
        // frame period is the sum of these, and this says which one owns it.
        static const bool s_pms2 = getenv("RESTUFF_PRESENTMS") != nullptr;
        const auto _p_sub0 = std::chrono::steady_clock::now();
        auto _p_acq1 = _p_sub0;
        {
          auto acq = dev->AcquireQueue(dev->queue_family_graphics_compute(), 0);
          _p_acq1 = std::chrono::steady_clock::now();  // time spent WAITING for the queue
          df.vkQueueSubmit(acq.queue(), 1, &si, fence_);
        }
        const auto _p_wait0 = std::chrono::steady_clock::now();
        // Contract: all work must complete before the refresher returns.
        // M4.5 (pipelined): the wait moved to the TOP of the next callback --
        // the guest-output image is published GPU-incomplete, and correctness
        // rests on the SDK paint sharing our single queue (submission order)
        // plus the frame-head barrier. gpu_fence_wait in [PRESENTMS2] reads ~0
        // in this mode; the residual wait shows up at the next frame's top.
        if (!pipelined) {
          df.vkWaitForFences(device, 1, &fence_, VK_TRUE, UINT64_MAX);
        }
        const auto _p_wb0 = std::chrono::steady_clock::now();

        // GPU done: scene_img copy (if dumped) is now readable -> write PPM.
        if (scene_dumped) WriteSceneDumpPpm();
        // M3.89: resolves are executed -> write their pixels into guest RAM.
        FlushResolveWritebacks();
        // M4.33: read the frame's pass timestamps (fence already waited --
        // GPUPASS blocks pipelined mode, so the frame is provably complete).
        GpCollect(dev);
        if (s_pms2) {
          const auto now = std::chrono::steady_clock::now();
          auto us = [](auto a, auto b) {
            return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
          };
          // M3.132: is the ~24 fps ceiling VSYNC-QUANTISED? On a 60 Hz display a
          // FIFO swapchain can only present on 16.67 ms boundaries, so a 41 ms
          // average must be a MIX of 2-interval (33.3 ms) and 3-interval
          // (50 ms) frames -- bimodal. Anything else (a smooth spread around
          // 41 ms) means presentation is not the throttle and the wait is
          // elsewhere. The user's point stands that a 60 Hz monitor cannot
          // produce a steady 24; this bucketing tells the two apart instead of
          // reasoning about averages.
          {
            static auto last = std::chrono::steady_clock::now();
            static uint64_t pn = 0, b1 = 0, b2 = 0, b3 = 0, bo = 0, onvs = 0;
            const auto t = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration_cast<std::chrono::microseconds>(t - last).count() / 1000.0;
            last = t;
            if (pn) {  // skip the first, it spans startup
              if (ms < 25.0) ++b1;
              else if (ms < 42.0) ++b2;
              else if (ms < 58.0) ++b3;
              else ++bo;
              const double k = ms / 16.67;
              if (std::fabs(k - std::round(k)) < 0.18 && ms > 8.0) ++onvs;
            }
            if (++pn % 100 == 0) {
              REXLOG_INFO("[PRESENTHIST] last100 present periods: <25ms={} 25-42ms={} "
                          "42-58ms={} >58ms={} | near-16.67ms-multiple={}",
                          b1, b2, b3, bo, onvs);
              b1 = b2 = b3 = bo = onvs = 0;
            }
          }
          static uint64_t n = 0, acq = 0, sub = 0, wait = 0, wb = 0, outside = 0;
          outside += g_pms_outside_us;
          acq += us(_p_sub0, _p_acq1);
          sub += us(_p_acq1, _p_wait0);
          wait += us(_p_wait0, _p_wb0);
          wb += us(_p_wb0, now);
          if (++n % 100 == 0) {
            REXLOG_INFO("[PRESENTMS2] 100-frame avg: OUTSIDE_our_code={}us | queue_acquire={}us "
                        "submit={}us gpu_fence_wait={}us writeback={}us",
                        outside / 100, acq / 100, sub / 100, wait / 100, wb / 100);
            acq = sub = wait = wb = outside = 0;
          }
          g_pms_last_end = std::chrono::steady_clock::now();
        }

        if (!pipelined) {
          // GPU provably done: retire staging buffers + stale texture entries.
          for (const auto& up : dl.retired_uploads) {
            if (up.staging) df.vkDestroyBuffer(device, up.staging, nullptr);
            if (up.staging_mem) df.vkFreeMemory(device, up.staging_mem, nullptr);
          }
          dl.retired_uploads.clear();
          for (auto& t : dl.deferred_destroy) DestroyTexEntry(dev, t);
          dl.deferred_destroy.clear();
        } else {
          // M4.5: the GPU is still running this frame -- park everything on
          // THIS slot's retire lists; RetireFrameSlot frees them right after
          // this slot's fence wait (top of the callback after next).
          auto& slot = TL().slots[frame_slot_];
          for (auto& up : dl.retired_uploads) slot.r_retired_uploads.push_back(up);
          dl.retired_uploads.clear();
          for (auto& t : dl.deferred_destroy) slot.r_deferred_destroy.push_back(t);
          dl.deferred_destroy.clear();
          slot_submitted_[frame_slot_] = true;
          frame_slot_ ^= 1;
        }

        const uint64_t fr = dl.frames_rendered.fetch_add(1, std::memory_order_relaxed) + 1;
        // RESTUFF_RDOC_TRIGGER=<path>: when the file is non-empty, fire a
        // RenderDoc capture of the next presented frame and truncate the file
        // -- lets the drive harness capture EXACTLY the HUD-gameplay moment.
        // Requires launching under RenderDoc (renderdoccmd capture) so the
        // SDK's RenderDocAPI::CreateIfConnected() found the module.
        {
          static const char* rdt = getenv("RESTUFF_RDOC_TRIGGER");
          if (rdt && (fr % 10 == 0)) {
            bool fire = false;
            if (FILE* f = fopen(rdt, "r")) {
              fire = fgetc(f) != EOF;
              fclose(f);
            }
            // M4.25: the SDK's renderdoc_api() probes for renderdoc.dll at
            // instance-creation time -- BEFORE vkCreateInstance loads the
            // implicit capture layer -- so it caches null even when the layer
            // is demonstrably live (manual overlay captures work). Lazy
            // fallback at trigger time, when the module is guaranteed loaded.
            auto rd_get = [&]() -> const RENDERDOC_API_1_0_0* {
              if (auto* rd = dev->vulkan_instance()->renderdoc_api()) return rd->api_1_0_0();
#ifdef _WIN32
              static const RENDERDOC_API_1_0_0* s_fb = [] {
                RENDERDOC_API_1_0_0* api = nullptr;
                if (HMODULE m = GetModuleHandleA("renderdoc.dll"))
                  if (auto get = reinterpret_cast<pRENDERDOC_GetAPI>(
                          GetProcAddress(m, "RENDERDOC_GetAPI")))
                    get(eRENDERDOC_API_Version_1_0_0, reinterpret_cast<void**>(&api));
                return static_cast<const RENDERDOC_API_1_0_0*>(api);
              }();
              return s_fb;
#else
              return nullptr;
#endif
            };
            static uint64_t s_rdoc_end_at = 0;
            if (fire && !s_rdoc_end_at) {
              if (const auto* api = rd_get()) {
                // Explicit bracket spanning ~30 presented frames: the guest
                // scene renders on its own cadence, so a single-present
                // TriggerCapture kept catching presenter-blit-only frames.
                api->StartFrameCapture(nullptr, nullptr);
                s_rdoc_end_at = fr + 30;
                REXLOG_INFO("[native_vk] RenderDoc capture STARTED (frame {})", fr);
              } else {
                REXLOG_WARN("[native_vk] RDOC_TRIGGER set but RenderDoc not connected");
              }
              if (FILE* f = fopen(rdt, "w")) fclose(f);
            }
            if (s_rdoc_end_at && fr >= s_rdoc_end_at) {
              if (const auto* api = rd_get()) api->EndFrameCapture(nullptr, nullptr);
              REXLOG_INFO("[native_vk] RenderDoc capture ENDED (frame {})", fr);
              s_rdoc_end_at = 0;
            }
          }
        }
        // M3.20: persist the pipeline cache periodically -- the drive/watchdog
        // SIGKILLs, so a shutdown-only save never lands. First save early (once
        // the level's pipelines are mostly built), then occasionally.
        // M4.0: also save ~5s after a pipeline-build burst goes quiet -- a run
        // that loaded a level and exited before frame 1500 used to lose the
        // whole burst (the next run re-compiled everything = the load pop-in).
        {
          static uint64_t s_pubs_seen = 0, s_pubs_saved = 0, s_last_pub_fr = 0;
          const uint64_t pubs = TL().pipe_publish_count.load(std::memory_order_relaxed);
          if (pubs != s_pubs_seen) {
            s_pubs_seen = pubs;
            s_last_pub_fr = fr;
          }
          const bool burst_settled =
              pubs != s_pubs_saved && s_last_pub_fr && fr >= s_last_pub_fr + 300;
          if (fr == 1500 || (fr > 1500 && fr % 3000 == 0) || burst_settled) {
            s_pubs_saved = pubs;
            SavePipelineCache(dev);
            SavePrewarmManifest();
            renderer::spc::SaveShaderSpvCache();  // M4.40 (no-op if nothing new)
          }
        }
        // #37: re-log the env summary each rotation-sized interval (see
        // RestuffEnvSummary) so it survives the log head being truncated.
        if (fr % 3000 == 0) REXLOG_INFO("[ENV] {}", RestuffEnvSummary());
        if (fr % 100 == 0) {
          const uint32_t fb = TL().frame_fb ? TL().frame_fb : renderer::GetFrontBufferPhys();
          uint32_t tdraws = 0, tres = 0, tpgen = 0;
          for (const auto& t : trans) {
            (t.is_resolve ? tres : tdraws)++;
            if (t.pgen[0] > 0.5f) ++tpgen;
          }
          REXLOG_INFO(
              "[native_vk] present alive: frames={} trans={}(draw={} res={} pgen={}) textures={} "
              "verts={} res_exec={} res_guard={} fb=0x{:08X} fbhit={}",
              fr, trans.size(), tdraws, tres, tpgen, dl.textures.size(),
              verts.size(), s_res_exec.load(std::memory_order_relaxed),
              s_res_guard.load(std::memory_order_relaxed), fb,
              TL().rt_tex.count(fb) ? 1 : 0);
          TexCensus();  // M4.36 (RESTUFF_TEXCENSUS=1), no-op otherwise
        }
        // #37 (RESTUFF_PACE=1): pacing histogram. The complaint is hitches vs
        // emulation's constant 60 -- averages hide exactly that, so report
        // windowed percentiles of the PRESENTED frame interval plus hitch
        // counts. Headless caveat: swapchain cadence under Xvfb is not the
        // user's; only CPU-side spike sources read from these headlessly.
        {
          static const bool s_pace = getenv("RESTUFF_PACE") != nullptr;
          if (s_pace) {
            static std::chrono::steady_clock::time_point s_prev{};
            static std::vector<uint32_t> s_iv;
            const auto pnow = std::chrono::steady_clock::now();
            if (s_prev.time_since_epoch().count()) {
              s_iv.push_back(uint32_t(
                  std::chrono::duration_cast<std::chrono::microseconds>(pnow - s_prev).count()));
              if (s_iv.size() >= 600) {
                std::sort(s_iv.begin(), s_iv.end());
                auto q = [&](double p) { return s_iv[size_t(p * (s_iv.size() - 1))]; };
                uint32_t h20 = 0, h33 = 0;
                for (uint32_t v : s_iv) { h20 += v > 20000; h33 += v > 33000; }
                // Guest production rate over the same window: if guest_fps is
                // low, the game itself is the cap and no present pacing helps.
                static uint64_t s_gf_prev = 0;
                static std::chrono::steady_clock::time_point s_win_start{};
                const uint64_t gf = restuff::renderer::GuestFrameCount();
                const double win_s =
                    s_win_start.time_since_epoch().count()
                        ? std::chrono::duration<double>(pnow - s_win_start).count() : 0.0;
                const double gfps = (win_s > 0.0 && s_gf_prev) ? (gf - s_gf_prev) / win_s : 0.0;
                REXLOG_INFO("[PACE] present interval over {} frames: p50={}us p95={}us p99={}us "
                            "max={}us hitch>20ms={} hitch>33ms={} | guest_fps={:.1f}",
                            s_iv.size(), q(0.5), q(0.95), q(0.99), s_iv.back(), h20, h33, gfps);
                s_gf_prev = gf;
                s_win_start = pnow;
                s_iv.clear();
              }
            }
            s_prev = pnow;
          }
        }
        ctx.SetIs8bpc(true);
        g_pms_cb_end = std::chrono::steady_clock::now();  // M3.135
        return true;
      });

  // M3.135: close the loop on OUTSIDE. Everything below is a few clock reads on
  // a thread that is already spending tens of milliseconds per frame.
  {
    static const bool s_pms = getenv("RESTUFF_PRESENTMS") != nullptr;
    if (s_pms) {
      const auto rgo_ret = std::chrono::steady_clock::now();
      auto us = [](auto a, auto b) {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(b - a).count());
      };
      static uint64_t n = 0, tail = 0, post = 0, loop = 0, pre = 0, prep = 0;
      // Accumulate only when this iteration produced the full, ordered ring:
      //   rgo_call < t0 < last_end < cb_end < rgo_ret
      // and the previous iteration left an rgo_ret to measure the loop from.
      // Frames where the record path did not run (t0 stale) fail the ordering
      // and are dropped rather than averaged in as garbage.
      if (g_pms_rgo_ret.time_since_epoch().count() && g_pms_rgo_call < g_pms_t0 &&
          g_pms_t0 < g_pms_last_end && g_pms_last_end < g_pms_cb_end &&
          g_pms_rgo_ret < g_pms_rgo_call) {
        const bool cbs = g_pms_rgo_call <= g_pms_cb_start && g_pms_cb_start <= g_pms_t0;
        pre += cbs ? us(g_pms_rgo_call, g_pms_cb_start) : us(g_pms_rgo_call, g_pms_t0);
        const uint64_t prep1 = cbs ? us(g_pms_cb_start, g_pms_t0) : 0;
        prep += prep1;
        tail += us(g_pms_last_end, g_pms_cb_end);
        post += us(g_pms_cb_end, rgo_ret);
        loop += us(g_pms_rgo_ret, g_pms_rgo_call);
        // #37: prep TAIL, not just the mean -- a steady 7ms and a 5ms-with-
        // 40ms-spikes prep average the same but pace nothing alike.
        static std::vector<uint32_t> prep_s;
        prep_s.push_back(uint32_t(prep1));
        if (++n % 100 == 0) {
          std::sort(prep_s.begin(), prep_s.end());
          REXLOG_INFO("[PRESENTMS3] 100-frame avg OUTSIDE split: sdk_prologue={}us "
                      "our_draw_prep={}us our_cleanup={}us sdk_epilogue={}us "
                      "our_loop_sleep={}us | prep p95={}us p99={}us max={}us",
                      pre / 100, prep / 100, tail / 100, post / 100, loop / 100,
                      prep_s[size_t(0.95 * (prep_s.size() - 1))],
                      prep_s[size_t(0.99 * (prep_s.size() - 1))], prep_s.back());
          tail = post = loop = pre = prep = 0;
          prep_s.clear();
        }
      }
      g_pms_rgo_ret = rgo_ret;
    }
  }
  return _rgo_ok;
}

void NativeVulkanGraphicsSystem::Shutdown() {
  // Stop the guest-facing pump first: it touches guest memory / the
  // dispatcher, and XHostThread teardown needs kernel state still alive.
  restuff::native::Shutdown();
  if (running_.exchange(false)) {
    if (present_thread_.joinable()) {
      present_thread_.join();
    }
  }
  // M3.15/M3.20: stop the pipeline-builder workers before the device goes away.
  {
    auto& tl = TL();
    {
      std::lock_guard<std::mutex> lk(tl.pipe_mutex);
      tl.pipe_quit = true;
    }
    tl.pipe_cv.notify_all();
    for (auto& w : tl.pipe_workers)
      if (w.joinable()) w.join();
    tl.pipe_workers.clear();
  }
  if (provider_) {
    vk::VulkanDevice* dev = provider_->vulkan_device();
    if (dev) {
      const auto& df = dev->functions();
      VkDevice device = dev->device();
      // M3.20: persist the driver pipeline cache so the next run's ~600
      // pipelines return near-instantly (no black-blob catch-up window).
      // M4.0: sync -- the process hard-exits ~200ms after Shutdown returns,
      // which stranded the old detached writer mid-file (save lost). Same for
      // the pre-warm manifest.
      SavePrewarmManifest(/*sync=*/true);
      renderer::spc::SaveShaderSpvCache();  // M4.40: synchronous by construction
      if (auto& tl = TL(); tl.pipe_cache != VK_NULL_HANDLE && g_vkGetPipelineCacheData) {
        SavePipelineCache(dev, /*sync=*/true);
        if (g_vkDestroyPipelineCache)
          g_vkDestroyPipelineCache(device, tl.pipe_cache, nullptr);
        tl.pipe_cache = VK_NULL_HANDLE;
      }
      // The present thread waits its fence after each submit (serialized), so
      // our work is idle by the time we join above. M4.5: pipelined mode may
      // still have one frame in flight -- wait its fence and retire before any
      // teardown touches its resources.
      for (int i = 0; i < 2; ++i) {
        if (slot_submitted_[i] && fences_[i] != VK_NULL_HANDLE) {
          df.vkWaitForFences(device, 1, &fences_[i], VK_TRUE, UINT64_MAX);
          slot_submitted_[i] = false;
          RetireFrameSlot(dev, TL().slots[i]);
        }
      }
      DestroyDrawLayer(dev);
      for (int i = 0; i < 2; ++i) {
        if (fences_[i] != VK_NULL_HANDLE) {
          df.vkDestroyFence(device, fences_[i], nullptr);
          fences_[i] = VK_NULL_HANDLE;
        }
      }
      if (cmd_pool_ != VK_NULL_HANDLE) {
        df.vkDestroyCommandPool(device, cmd_pool_, nullptr);
        cmd_pool_ = VK_NULL_HANDLE;
        cmd_bufs_[0] = cmd_bufs_[1] = VK_NULL_HANDLE;
      }
    }
  }
  presenter_.reset();
  provider_.reset();
}

}  // namespace restuff

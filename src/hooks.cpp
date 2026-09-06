#include <atomic>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <random>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#include <process.h>
#define getpid _getpid
#endif  // _WIN32

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>

#include "renderer/up_draws.h"  // native renderer: UP-draw capture frame hooks
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/mmio_handler.h>
#include <rex/ui/keybinds.h>

#include "video_player.h"

// Defined in native_vk.cpp: selects the native Vulkan renderer vs the xenos
// GPU emulation plugin (used to gate xenos-only paths like the wireframe poke).
REXCVAR_DECLARE(bool, use_native_renderer);

// M4.6: range/validator widened -- the old 20/30/60 whitelist was the THIRD
// hidden cap layer (after the vblank default and the env-only uncap) that kept
// silently re-imposing 60: values like 120 were rejected without a log line
// and the busy-wait below fell back to the 60 default. High-rate values are
// experimental (pair with vblank_hz = 2x the target in restuff.toml).
// M4.27: 0 = limiter OFF entirely. The frame rate is then bounded only by
// vblank_hz/2 (the game presents every 2nd guest vblank) and the machine.
REXCVAR_DEFINE_INT32(fps_cap, 60, "Performance",
                     "Software frame rate cap. 0 = uncapped (vblank_hz/2 still bounds); "
                     "common values: 20, 30, 60; up to 240 experimental.")
    .range(0, 240)
    .validator([](std::string_view v) {
        const int n = atoi(std::string(v).c_str());
        return (n == 0 || n >= 20) && n <= 240;
    });

REXCVAR_DEFINE_BOOL(wireframe, false, "Cheats", "Force wireframe rendering on all geometry.");

// --- Health regeneration --------------------------------------------------
// Continuously regenerates the player's health to balance the later levels
// (the sequel has regen). Rate is a fraction of MAX HP per second, so the heal
// amount scales with the costume's Life stat (Life drives max HP).
// M3.287: DEFAULT OFF. Regen is a fork-lineage gameplay MOD (retail Naughty
// Bear has none; the sequel does) and it was silently active during a session
// the user was judging for retail parity. Ship behaviour must be retail;
// anyone who wants the mod can flip the cvar.
REXCVAR_DEFINE_BOOL(health_regen, true, "Gameplay",
    "Enable continuous player health regeneration (gameplay mod; not retail).");
REXCVAR_DEFINE_DOUBLE(health_regen_rate, 0.0025, "Gameplay",
    "Regen per second as a fraction of max HP (0.0025 = slow trickle).");
REXCVAR_DEFINE_BOOL(health_regen_debug, false, "Gameplay",
    "Log player hp/max ~once per second (for tuning).");
REXCVAR_DEFINE_BOOL(health_regen_hud, true, "Gameplay",
    "Heal through the game's damage pipeline so the on-screen health bar "
    "updates. Disable for a silent field write if it misbehaves.");

// --- Title/attract sky recolor ----------------------------------------------
// The felt menus set a per-screen background color via a "SetBackgroundColor"
// GAS tag (Execute = sub_82CD17B0). The title/attract screen uses a yellow
// background; the main menu uses blue (which is why the sky "turns blue on
// start"). on_set_bg_color rewrites the yellow tag to a tunable blue.
REXCVAR_DEFINE_BOOL(sky_recolor, true, "Modding",
    "Recolor the yellow title/attract background to blue (SetBackgroundColor tag).");
REXCVAR_DEFINE_BOOL(sky_recolor_debug, false, "Modding",
    "Log every SetBackgroundColor RGB (for finding/tuning the title color).");
REXCVAR_DEFINE_INT32(sky_r, 186, "Modding",
    "Title sky replacement red (0-255). Default #BAD5F4, the artwork's own "
    "light felt blue.").range(0, 255);
REXCVAR_DEFINE_INT32(sky_g, 213, "Modding",
    "Title sky replacement green (0-255).").range(0, 255);
REXCVAR_DEFINE_INT32(sky_b, 244, "Modding",
    "Title sky replacement blue (0-255).").range(0, 255);
REXCVAR_DEFINE_INT32(sky_cid, 13, "Modding",
    "First Scaleform character id of the felt sky shapes (-1 = off). The sky is "
    "cids 13..15: base, day layer, warm overlay.").range(-1, 65535);
REXCVAR_DEFINE_INT32(sky_cid2, 15, "Modding",
    "Last Scaleform character id of the felt sky shapes (inclusive).").range(-1, 65535);
REXCVAR_DEFINE_INT32(sky_cid_art, 14, "Modding",
    "Char id of the pre-composited title artwork bitmap (tint stripped instead "
    "of solid-filled so its texture survives; -1 = none).").range(-1, 65535);

// Shows the live level score and next medal target as a row of the game's own
// felt objective panel (the objective list top-left in a level). Injection
// happens through the panel's GFx invoke wrappers; see update_score_objective.
REXCVAR_DEFINE_BOOL(score_objective, true, "Gameplay",
    "Show the live score and next medal target as a native row in the in-level "
    "objective list.");
REXCVAR_DEFINE_BOOL(score_objective_debug, false, "Gameplay",
    "Log objective-box bookkeeping for the native score row.");

// --- Free camera -------------------------------------------------------------
// Detaches hg::Camera from the follow system and flies it directly with WASD
// + mouse look. See update_freecam / on_camera_update below for the mechanism.
REXCVAR_DEFINE_BOOL(freecam, false, "Gameplay",
    "Free camera: detach the camera from the follow system and fly it with "
    "WASD (+ Space/Ctrl up-down, Shift = fast) and mouse look. Toggle: F6.");
REXCVAR_DEFINE_DOUBLE(freecam_speed, 8.0, "Gameplay",
    "Free camera fly speed in world units/sec (Shift multiplies by 3).");
REXCVAR_DEFINE_DOUBLE(freecam_sensitivity, 0.0025, "Gameplay",
    "Free camera mouse-look sensitivity (radians/pixel).");

// --- Local co-op recon (M1) --------------------------------------------------
// READ-ONLY probe answering one question: can a campaign level host a second
// player? HazingPlayerManager keeps players in a std::vector<Player*> at
// mgr+48/+52 (same walk get_player_object uses), and the engine exposes
// indexed/per-user-id accessors (GetPlayer, SetActivePlayerActorFromUserId,
// GetSpawnerIds = sub_8281E310) because NETWORK multiplayer put real player
// actors in levels. If campaign levels carry spawner/player capacity for more
// than one, local co-op has somewhere to stand; if not, that reshapes the plan.
// Dumps the vector, each player, and the manager's own fields (a user-id or
// spawner list should be visible as structure). Nothing is written.
REXCVAR_DEFINE_BOOL(coop_probe, false, "Modding",
    "Log HazingPlayerManager player-vector + manager layout once in a level "
    "(local co-op reconnaissance; read-only).");

// M2 experiment: call the guest player-creation function ONCE and look at what
// comes back. sub_8281C678(mgr, a2) allocates a HazingPlayerActor (5392 bytes)
// and constructs it with spawnerId = the CURRENT player count -- so with one
// player live it must return an actor tagged 1. a2 is a polymorphic pointer the
// constructor queries virtually, but sub_82833BB8 explicitly handles a2 == 0
// (substituting -1), so NULL is a legal argument and nothing has to be guessed.
// The result is an ORPHAN: create never touches the roster vector (its caller
// does the push_back), so this cannot disturb the live player list -- the
// vector is logged before and after to prove exactly that.
REXCVAR_DEFINE_BOOL(coop_spawn_test, false, "Modding",
    "ONE-SHOT: call the guest player-creation function in a level and log the "
    "result WITHOUT adding it to the roster (local co-op M2 probe).");

// Dump every loaded Lua chunk's bytecode to lua_dump/<path> (basis: lua_mods
// branch). Use to extract the per-level medal score targets.
REXCVAR_DEFINE_BOOL(lua_dump_originals, false, "Modding",
    "Dump every loaded Lua chunk's original bytecode to lua_dump/<path>.");
REXCVAR_DEFINE_BOOL(unlock_all, false, "Cheats",
    "Unlock all costumes/content (calls UnlockAllUnlockables continuously).");

// Defined below on_swap; called once per presented frame.
static void update_health_regen(double dt);
static void update_level_score();
static void update_score_objective();
static void update_attract(double dt);
static void update_bearcam();
static void update_freecam();
static void update_coop_probe();
static void update_coop_spawn_test();
static void maybe_unlock_all();
// Set Windows timer resolution to 1ms for the lifetime of the process.
// Default is 15.6ms which causes sleep_until to overshoot badly. On Linux the
// monotonic clock/nanosleep are already high-resolution, so this is a no-op.
static const bool s_timer_res_set = []() {
#ifdef _WIN32
  timeBeginPeriod(1);
#endif
  return true;
}();

static double s_fps_display = 0.0;
static int    s_frame_count = 0;
static auto   s_last_time   = std::chrono::high_resolution_clock::now();

// ---------------------------------------------------------------------------
// XMA kick shim
// ---------------------------------------------------------------------------
// EXPERIMENT (default OFF -- semantics were wrong, kept for reference): the
// game writes XMA register 0x0601 with 0x02000000/0x03000000 thousands of
// times while the title storyboard is frozen. First read: undocumented kick.
// WRONG: the SDK source itself notes 0x0601 "is written to with 0x02000000
// and 0x03000000 around a lock operation" -- lock-protocol chatter that real
// Xenia also ignores. Remapping it to Context0Kick kicks contexts with no
// pending work and the kick handler's WaitForWorkDone() never returns,
// hanging the guest render thread inside the MMIO store. Left in (off) as
// scaffolding for further XMA experiments.
REXCVAR_DEFINE_BOOL(xma_kick_shim, false, "Audio",
    "EXPERIMENTAL: remap XMA register 0x0601 writes to Context0Kick. Known "
    "to hang the guest; do not enable.");

static rex::runtime::MMIOWriteCallback s_xma_orig_write = nullptr;

static void xma_write_shim(void* ppc_context, void* callback_context, uint32_t addr,
                           uint32_t value) {
    const uint32_t r = (addr & 0xFFFF) / 4;
    if (r == 0x601) {
        static std::atomic<int> s_log{4};
        if (s_log.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[hooks] XMA reg-0601 kick remapped to Context0Kick (mask=0x{:08X})",
                        __builtin_bswap32(value));
        s_xma_orig_write(ppc_context, callback_context, addr + (0x650 - 0x601) * 4, value);
        return;
    }
    s_xma_orig_write(ppc_context, callback_context, addr, value);
}

// Lazily installed from on_swap: the XMA MMIO range only exists once the audio
// system has initialized. Swapping the range's write callback in place is a
// single pointer store; a racing store on another thread just takes the old
// callback once (benign).
static void install_xma_kick_shim() {
    static bool s_done = false;
    if (s_done || !REXCVAR_GET(xma_kick_shim)) return;
    auto* mmio = rex::runtime::MMIOHandler::global_handler();
    if (!mmio) return;
    auto* range = mmio->LookupRange(0x7FEA0000);
    if (!range || !range->write) return;
    if (range->write == &xma_write_shim) { s_done = true; return; }
    s_xma_orig_write = range->write;
    range->write = &xma_write_shim;
    s_done = true;
    REXLOG_INFO("[hooks] XMA kick shim installed (reg 0601 -> Context0Kick)");
}

// Hooked after `li r10,2` @ 0x82B01E64 in graphics-device init (sub_82B01C40):
// that literal becomes D3DPRESENT_PARAMETERS.PresentationInterval — the retail
// X360 build's hardcoded 30fps cap (present every 2nd 60Hz vblank; the engine's
// max-fps cvar is registered but never consumed downstream, so no config can
// change it). RESTUFF_FPS60=1 rewrites it to INTERVAL_ONE for a 60fps unlock.
// Opt-in: whether game logic is frame-tied (Havok FIXED30FPS step mode exists
// in the binary) is unverified — if the game runs double-speed with this on,
// that is the answer, not a hook bug.
void on_present_interval(PPCRegister& r10) {
    static const bool s_fps60 = getenv("RESTUFF_FPS60") != nullptr;
    if (s_fps60 && r10.u32 == 2) {
        r10.u32 = 1;
        REXLOG_INFO("[hooks] fps60: PresentationInterval 2 -> 1 (60fps unlock ON)");
    }
}

// Hooked at sub_82F33920 entry (deferred-swap builder; r3 = D3D device). This
// function reads PresentationInterval from dev+13804 (0x35EC) every presented
// frame and schedules the flip that many vblanks after the previous one — the
// real 30fps pacer. RESTUFF_FPS60=1 forces the field to 1 right before it is
// consumed. Whether the game then runs at correct speed depends on whether its
// logic ticks per vblank (fine) or per rendered frame (2x) — measured by film.
void on_swap_build(PPCRegister& r3) {
    static const bool s_fps60 = getenv("RESTUFF_FPS60") != nullptr;
    if (!s_fps60) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* p = mem->virtual_membase() + r3.u32 + 13804;
    if (!(p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)) {
        static std::atomic<int> s_log{4};
        if (s_log.fetch_sub(1, std::memory_order_relaxed) > 0)
            REXLOG_INFO("[hooks] fps60: pacer interval was {} -> 1 (dev=0x{:08X})",
                        (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                            (uint32_t(p[2]) << 8) | p[3], r3.u32);
        p[0] = p[1] = p[2] = 0;
        p[3] = 1;
    }
}

// Hooked at 0x82F33FF4 (bl VdSwap) — fires once per presented frame.
// Runs the software frame limiter then updates the FPS counter.
void on_swap() {
    using clock    = std::chrono::high_resolution_clock;
    using duration = std::chrono::duration<double>;

    // Native renderer: finalize the in-flight UP draw (the game fills Begin*'s
    // buffers after Begin returns; commit is an inlined store) and publish the
    // frame's captured draw list. Runs on the guest render thread just before
    // VdSwap. CPU-side only — no presentation happens here.
    restuff::renderer::CompletePendingUpDraw();  // stats only (PM4 path captures draws)

    // --- Software frame limiter ---
    // M4.27: fps_cap 0 disables the limiter (vblank_hz/2 still paces the sim).
    static auto last_swap = clock::now();
    auto now = clock::now();
    const int cap = REXCVAR_GET(fps_cap);
    if (cap > 0) {
        const double target_interval = 1.0 / static_cast<double>(cap);
        const auto deadline = last_swap + duration(target_interval);

        // Coarse sleep until ~2ms before deadline, then spin for accuracy.
        const auto sleep_until = deadline - std::chrono::milliseconds(2);
        if (now < sleep_until)
            std::this_thread::sleep_until(sleep_until);
        while (clock::now() < deadline) {}
    }

    last_swap = clock::now();

    // --- Wireframe override (register file) ---
    // Belt-and-suspenders: also patch the register file so any draw that
    // reads the cached register outside of the ring-buffer path is correct.
    // Only valid on the xenos plugin path: with the native renderer injected,
    // graphics_system() is a NativeVulkanGraphicsSystem (a bare
    // IGraphicsSystem), and this downcast would write far past the end of the
    // much smaller object.
    if (!REXCVAR_GET(use_native_renderer))
    if (auto* gs = static_cast<rex::graphics::GraphicsSystem*>(
            rex::Runtime::instance()->graphics_system())) {
        auto& reg = gs->register_file()->values[0x2205];
        if (REXCVAR_GET(wireframe))
            reg = (reg & ~0x7F8u) | 0x128u;
        else
            reg = (reg & ~0x7F8u);
    }

    // --- FPS counter (updates display value once per second) ---
    ++s_frame_count;
    now = clock::now();
    const duration elapsed = now - s_last_time;
    if (elapsed.count() >= 1.0) {
        s_fps_display = s_frame_count / elapsed.count();
        s_frame_count = 0;
        s_last_time   = now;
    }

    // --- Out-of-combat health regen ---
    {
        static auto regen_last = clock::now();
        const auto  regen_now  = clock::now();
        double dt = duration(regen_now - regen_last).count();
        regen_last = regen_now;
        if (dt > 0.25) dt = 0.25;  // clamp across pauses / level loads
        update_health_regen(dt);
        // Attract-mode video shares the same clamped delta.
        update_attract(dt);
    }

    // Cache the live level score for the trophy overlay.
    update_level_score();

    // Keep the native score row in the objective panel current.
    update_score_objective();

    // Bear-action PIP cutaway (banner pronoun rewrite / mode gating).
    update_bearcam();

    // Free camera: fly the render camera with WASD + mouse when active.
    update_freecam();

    // M1 local co-op reconnaissance (read-only, coop_probe cvar).
    update_coop_probe();
    update_coop_spawn_test();

    // One-shot "unlock everything" when requested via the unlock_all cvar.
    maybe_unlock_all();

    // Install the XMA kick shim once the audio system's MMIO range exists.
    install_xma_kick_shim();

}

double get_fps() { return s_fps_display; }

// ---------------------------------------------------------------------------
// Health regeneration
// ---------------------------------------------------------------------------
// Runs from on_swap (guest thread, once per presented frame). Walks the guest
// object graph to the player's damage interface and tops up HP when the player
// has been out of combat for health_regen_delay seconds.
//
// Object graph (see IDA map; base 0x82000000):
//   HazingPlayerManager* singleton  = *(0x83326814)
//   player[0]                       = *(*(mgr + 48))          (player vector begin)
//   main character object (attrs)   = *(player[0] + 0x1508)   (sub_8281C290)
//   DamageableComponent             via handle at attrs+124/128/132 (sub_8265E598)
//   IDamageable interface           = component + 32
// IDamageable vtable slots: +16 GetRemainingHitPoints()->float,
//   +20 SetRemainingHitPoints(float), +24 GetBaseHitPoints()->float.
//
// Every guest pointer is range-checked and the resulting HP values are sanity
// gated, so a wrong offset degrades to "do nothing" rather than crashing.

namespace {

inline bool ptr_ok(uint32_t a) {
    return a >= 0x10000u && a < 0xC0000000u && (a & 3u) == 0u;
}

inline uint32_t rd32(uint8_t* base, uint32_t addr) {
    const uint32_t off = (addr >= 0xE0000000u) ? 0x1000u : 0u;
    return __builtin_bswap32(
        *reinterpret_cast<volatile uint32_t*>(base + addr + off));
}

// Guest memory is reserved as a full 4GB range but only committed where the
// title actually allocated, so dereferencing an arbitrary field value can fault.
// host_readable verifies the host page is committed+readable first.
//
// VirtualQuery is expensive here (it walks the process VAD tree, which is huge
// for the 4GB guest mapping), so results are cached per 4KB page in a small
// direct-mapped table -- a scan touches each page once instead of every read.
inline bool host_readable(uint8_t* base, uint32_t addr) {
    constexpr uint32_t kN = 128;
    static uintptr_t s_tag[kN];
    static bool      s_val[kN];
    static bool      s_init = false;
    if (!s_init) { for (auto& t : s_tag) t = ~uintptr_t(0); s_init = true; }

    const uint32_t off = (addr >= 0xE0000000u) ? 0x1000u : 0u;
    const uintptr_t h = reinterpret_cast<uintptr_t>(base + addr + off);
    const uintptr_t page = h >> 12;
    const uint32_t slot = static_cast<uint32_t>(page) & (kN - 1);
    if (s_tag[slot] == page) return s_val[slot];

    // Cross-platform page-protection probe (wraps VirtualQuery on Windows and
    // the equivalent page-table walk on Linux). Readable == the page is mapped
    // with some access other than kNoAccess (covers reserved/uncommitted pages).
    size_t query_len = 1;  // QueryProtect rounds up to a full page
    rex::memory::PageAccess access = rex::memory::PageAccess::kNoAccess;
    const bool ok =
        rex::memory::QueryProtect(reinterpret_cast<void*>(h), query_len, access) &&
        access != rex::memory::PageAccess::kNoAccess;
    s_tag[slot] = page;
    s_val[slot] = ok;
    return ok;
}

// Page-safe read. Sets ok=false (and returns 0) if the address isn't readable.
inline uint32_t rd32_safe(uint8_t* base, uint32_t addr, bool& ok) {
    ok = host_readable(base, addr);
    return ok ? rd32(base, addr) : 0u;
}

// A correctly-resolved DamageableComponent's IDamageable subobject (component
// + 32) has this vtable pointer as its first word -- used to validate a captured
// pointer. Components are referenced by handle (not raw pointers), so the
// damageable can't be found by scanning the player object; instead on_read_hp
// captures it from the constant HP reads (see below).
constexpr uint32_t kDamageableVtable = 0x8222CD0Cu;

// The player's IDamageable interface, captured by on_read_hp. Written on the
// game thread, read on the render thread (on_swap) -> atomic. Doubles as an
// "in a level" signal for the trophy/score readout.
std::atomic<uint32_t> g_player_idmg{0};

// Live current-level score (GetCurrentLevelTotalScore), cached for the overlay.
// -1 = not in a level.
std::atomic<int> g_level_score{-1};

// Current level's GradeScore object (captured from on_grade_query) and the four
// resolved medal thresholds (Bronze..Platinum), queried from it. 0 = unknown.
std::atomic<uint32_t> g_grade_score{0};
std::atomic<int> g_trophy_auto[4] = {{0}, {0}, {0}, {0}};

// Live HazingScoreMgr (captured from inject_score_bonus). +0x88 = float total
// score (the on-screen HUD score), which is what medals are graded against.
std::atomic<uint32_t> g_score_mgr{0};

// UnlockableManager (captured from on_get_unlockable), for the unlock_all cvar.
std::atomic<uint32_t> g_unlock_mgr{0};

// Local player's game object (gameObject::ModeledObject) via the player-manager
// singleton: mgr=*(0x83326814) -> player[0]=*(*(mgr+48)) -> *(player[0]+36).
uint32_t get_player_object(uint8_t* base) {
    bool ok = false;
    const uint32_t mgr = rd32_safe(base, 0x83326814, ok);
    if (!ok || !ptr_ok(mgr)) return 0;
    const uint32_t begin = rd32_safe(base, mgr + 48, ok);
    if (!ok || !ptr_ok(begin)) return 0;
    const uint32_t end = rd32_safe(base, mgr + 52, ok);
    if (!ok || end <= begin) return 0;
    const uint32_t p0 = rd32_safe(base, begin, ok);
    if (!ok || !ptr_ok(p0)) return 0;
    const uint32_t obj = rd32_safe(base, p0 + 36, ok);
    return (ok && ptr_ok(obj)) ? obj : 0;
}

// Returns the captured player IDamageable, or 0 if none/stale. Cheap: just
// validates the cached pointer still looks like a live damageable.
uint32_t resolve_player_damageable(uint8_t* base) {
    const uint32_t idmg = g_player_idmg.load(std::memory_order_relaxed);
    if (!idmg) return 0;
    bool ok = false;
    if (rd32_safe(base, idmg, ok) == kDamageableVtable && ok) return idmg;
    g_player_idmg.store(0, std::memory_order_relaxed);  // freed / level changed
    return 0;
}

// HP lives directly on the IDamageable subobject (idmg = component + 32):
//   GetRemainingHitPoints(): lfs f1, 0x14(this)  -> current HP
//   GetBaseHitPoints():      lfs f1, 0x10(this)  -> max/base HP
// We read/write the floats directly instead of calling the virtual setter:
// SetRemainingHitPoints dispatches a HealthChangedMsg, and doing that every
// frame from inside the VdSwap hook re-enters the renderer and hangs (black
// screen). A direct field write has no such side effect.
constexpr uint32_t kOffCurHp = 0x14;
constexpr uint32_t kOffMaxHp = 0x10;

inline float rd_f32(uint8_t* base, uint32_t addr) {
    const uint32_t raw = rd32(base, addr);
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}
inline void wr_f32(uint8_t* base, uint32_t addr, float v) {
    uint32_t raw;
    std::memcpy(&raw, &v, 4);
    const uint32_t off = (addr >= 0xE0000000u) ? 0x1000u : 0u;
    *reinterpret_cast<volatile uint32_t*>(base + addr + off) = __builtin_bswap32(raw);
}

}  // namespace

static void update_health_regen(double dt) {
    if (!REXCVAR_GET(health_regen)) return;

    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    static double dbg_accum = 0.0;

    const uint32_t idmg = resolve_player_damageable(base);

    const float hp    = idmg ? rd_f32(base, idmg + kOffCurHp) : -1.0f;  // current HP
    const float maxhp = idmg ? rd_f32(base, idmg + kOffMaxHp) : -1.0f;  // max/base HP

    if (REXCVAR_GET(health_regen_debug)) {
        dbg_accum += dt;
        if (dbg_accum >= 1.0) {
            dbg_accum = 0.0;
            REXLOG_INFO("[regen] idmg={:08X} hp={:.1f} max={:.1f}", idmg, hp, maxhp);
        }
    }

    if (!idmg) return;  // not in gameplay

    // Sanity gate: bail (without acting) on implausible values.
    if (maxhp <= 0.0f || maxhp > 100000.0f || hp < 0.0f || hp > maxhp + 1.0f) return;
    if (hp <= 0.0f) return;   // dead -> never revive

    // Don't regen while paused (game logic/time is frozen but on_swap still fires
    // for the pause UI). Checked here, in-gameplay, because the pause-state globals
    // aren't valid at startup/menus.
    if (auto* fd = rex::Runtime::instance()->function_dispatcher()) {
        if (auto* fn = fd->GetFunction(0x827CBF10u)) {  // GameStateUtil::IsPauseMenuActive
            if (rex::ppc::GuestToHostFunction<int>(*fn) != 0) return;
        }
    }

    // Accumulate time and apply ~10x/sec. ApplyDamage runs the full damage/message
    // pipeline, so we don't want to fire it every frame (effect spam / cost).
    static double accum = 0.0;
    accum += dt;
    if (hp >= maxhp) { accum = 0.0; return; }  // already full
    if (accum < 0.1) return;

    float heal = static_cast<float>(REXCVAR_GET(health_regen_rate) * accum) * maxhp;
    accum = 0.0;
    if (heal <= 0.0f) return;
    if (hp + heal > maxhp) heal = maxhp - hp;

    if (REXCVAR_GET(health_regen_hud)) {
        // Heal via ApplyDamage(this, -amount): negative damage heals, and it runs
        // through the component's damage handler, which fires HealthChangedMsg so
        // the on-screen health bar climbs (same path incoming damage uses).
        if (auto* fd = rex::Runtime::instance()->function_dispatcher()) {
            if (auto* fn = fd->GetFunction(0x829119A8u)) {  // ApplyDamage
                rex::ppc::GuestToHostFunction<void>(*fn, idmg, -heal);
            }
        }
    } else {
        wr_f32(base, idmg + kOffCurHp, hp + heal);  // functional only (no HUD update)
    }
}

// Caches the live current-level score (GetCurrentLevelTotalScore = sub_826A4240)
// for the trophy overlay. Gated on being in a level (validated player damageable)
// so we never call into the score manager before it exists.
static void update_level_score() {
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;
    if (!resolve_player_damageable(base)) {  // not in a level
        g_level_score.store(-1, std::memory_order_relaxed);
        return;
    }
    // Live HUD score = float at HazingScoreMgr+0x88 (what medals grade against).
    const uint32_t mgr = g_score_mgr.load(std::memory_order_relaxed);
    if (mgr >= 0x10000u && mgr < 0xC0000000u) {
        const uint32_t addr = mgr + 0x88;
        const uint32_t off = (addr >= 0xE0000000u) ? 0x1000u : 0u;
        const uint32_t raw =
            __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(base + addr + off));
        float s;
        std::memcpy(&s, &raw, 4);
        if (s >= 0.0f && s < 1e9f) g_level_score.store(static_cast<int>(s),
                                                       std::memory_order_relaxed);
    }

    auto* fd = rex::Runtime::instance()->function_dispatcher();
    if (!fd) return;

    // Resolve the four medal thresholds from the captured GradeScore (~1/sec).
    // GetScoreWithGrade (sub_826FAFD8) returns ascending values; we sort them
    // into Bronze..Platinum to be robust to the grade enum's ordering.
    static int throttle = 0;
    const uint32_t gs = g_grade_score.load(std::memory_order_relaxed);
    if (gs && (throttle++ % 60) == 0) {
        if (auto* q = fd->GetFunction(0x826FAFD8u)) {  // GradeScore::GetScoreWithGrade
            // Grade enum: eBRONZE=3, eSILVER=4, eGOLD=5, ePLATINE=6.
            // GetScoreWithGrade returns a float (compared to the float score).
            int v[4];
            for (int g = 0; g < 4; ++g)
                v[g] = static_cast<int>(rex::ppc::GuestToHostFunction<float>(*q, gs, 3 + g));
            for (int a = 0; a < 4; ++a)
                for (int b = a + 1; b < 4; ++b)
                    if (v[b] < v[a]) { int t = v[a]; v[a] = v[b]; v[b] = t; }
            for (int g = 0; g < 4; ++g)
                g_trophy_auto[g].store(v[g], std::memory_order_relaxed);
        }
    }

    if (REXCVAR_GET(health_regen_debug)) {
        static int dbg = 0;
        if ((dbg++ % 120) == 0) {
            REXLOG_INFO("[trophy] score={} gs={:08X} thr={} {} {} {}",
                        g_level_score.load(std::memory_order_relaxed), gs,
                        g_trophy_auto[0].load(std::memory_order_relaxed),
                        g_trophy_auto[1].load(std::memory_order_relaxed),
                        g_trophy_auto[2].load(std::memory_order_relaxed),
                        g_trophy_auto[3].load(std::memory_order_relaxed));
        }
    }
}

// --- Overlay accessors (trophy / score readout) ---
int get_level_score() { return g_level_score.load(std::memory_order_relaxed); }

int get_trophy_target(int tier) {
    if (tier < 0 || tier > 3) return 0;
    // Auto-resolved from the game's GradeScore (see on_grade_query); 0 = unknown.
    return g_trophy_auto[tier].load(std::memory_order_relaxed);
}

// Hooked at DamageableComponent::GetRemainingHitPoints entry (0x82A22348). r3 is
// the IDamageable interface (component + 32). This is read constantly, so it lets
// us capture the player's damageable -- identified by the component holding a
// pointer to the local player's game object. Once captured this is ~free (early
// out); the actual regen runs in update_health_regen.
void on_read_hp(PPCRegister& r3) {
    if (g_player_idmg.load(std::memory_order_relaxed)) return;  // already captured

    // Fires very often; only do the owner search occasionally until captured.
    static std::atomic<uint32_t> ctr{0};
    if ((ctr.fetch_add(1, std::memory_order_relaxed) & 0x1Fu) != 0) return;

    const uint32_t idmg = r3.u32;
    if (!ptr_ok(idmg)) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    bool ok = false;
    if (rd32_safe(base, idmg, ok) != kDamageableVtable || !ok) return;  // not a damageable iface

    const uint32_t player_obj = get_player_object(base);
    if (!player_obj) return;

    // Only the player's damageable references the local player's game object.
    const uint32_t comp = idmg - 32;
    for (uint32_t off = 0; off < 0x400; off += 4) {
        if (rd32_safe(base, comp + off, ok) == player_obj && ok) {
            g_player_idmg.store(idmg, std::memory_order_relaxed);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Lua chunk dump + mod injection (basis: lua_mods branch)
// ---------------------------------------------------------------------------
// Hooked at lua_load (0x82BB5010): r5 = reader data {buf,size}, r6 = chunk name.
//  - lua_dump_originals: write each chunk's bytecode to lua_dump/<path>.
//  - Always: if lua_mods/<path> (or lua_mods/<basename>) exists, load it into
//    guest memory and repoint the reader at it, so the game runs YOUR chunk.
//    Mod files must be Xbox-360 Lua 5.1 bytecode (header "\x1bLuaQ", big-endian).

// Normalize a guest chunk path to a lua_dump-relative key.
static std::string lua_mod_key(const char* path, size_t len) {
    std::string s(path, len);
    for (char& c : s) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (s.size() > 3 && s[1] == ':' && s[2] == '/') s.erase(0, 3);  // strip "z:/"
    return s;
}

static void dump_lua_original(const std::string& key, rex::memory::Memory* mem, uint32_t data) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen[key]++) return;  // once per chunk
    const uint32_t buf  = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(data));
    const uint32_t size = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(data + 4));
    if (!buf || !size || size > (16u << 20)) return;
    const std::filesystem::path out_path = std::filesystem::path("lua_dump") / key;
    std::error_code ec;
    std::filesystem::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path, std::ios::binary);
    if (out) out.write(mem->TranslateVirtual<const char*>(buf), size);
}

// Load lua_mods/<rel> into guest system memory; returns {guest_addr, size} or {0,0}.
static std::pair<uint32_t, uint32_t> load_mod_file(const std::string& rel) {
    std::ifstream f("lua_mods/" + rel, std::ios::binary | std::ios::ate);
    if (!f) return {0, 0};
    const std::streamsize len = f.tellg();
    if (len <= 0 || len >= (16 << 20)) return {0, 0};
    f.seekg(0);
    std::vector<char> bytes(static_cast<size_t>(len));
    if (!f.read(bytes.data(), len)) return {0, 0};
    auto* mem = rex::system::kernel_memory();
    const uint32_t addr = mem ? mem->SystemHeapAlloc(static_cast<uint32_t>(len), 0x20) : 0u;
    if (!addr) { REXLOG_WARN("[lua] mod '{}' alloc failed", rel); return {0, 0}; }
    std::memcpy(mem->TranslateVirtual<uint8_t*>(addr), bytes.data(), static_cast<size_t>(len));
    REXLOG_INFO("[lua] mod loaded '{}' ({} bytes) -> guest {:08X}", rel, static_cast<int>(len), addr);
    return {addr, static_cast<uint32_t>(len)};
}

// Replacement bytecode for a chunk key: try the full path, then the basename.
// Cached (including negative results, so we don't re-stat every load).
static std::pair<uint32_t, uint32_t> get_lua_replacement(const std::string& key) {
    static std::mutex m;
    static std::unordered_map<std::string, std::pair<uint32_t, uint32_t>> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::pair<uint32_t, uint32_t> r = load_mod_file(key);
    if (!r.first) {
        const size_t slash = key.find_last_of('/');
        const std::string bn = (slash == std::string::npos) ? key : key.substr(slash + 1);
        if (bn != key) r = load_mod_file(bn);
    }
    cache.emplace(key, r);
    return r;
}

// Midasm hook at lua_load entry: r5 = reader data {buf,size}, r6 = chunk name.
// Dumps the original (when enabled) and, if a matching file exists under
// lua_mods/, repoints the reader at it so the game runs the modded chunk.
void on_lua_load(PPCRegister& r5, PPCRegister& r6) {
    const uint32_t data = r5.u32;
    const uint32_t name_addr = r6.u32;
    if (!data || !name_addr) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    const char* path = mem->TranslateVirtual<const char*>(name_addr);
    size_t len = 0;
    while (len < 256 && path[len]) ++len;
    const std::string key = lua_mod_key(path, len);

    if (REXCVAR_GET(lua_dump_originals)) dump_lua_original(key, mem, data);

    const auto repl = get_lua_replacement(key);
    if (repl.first) {
        *mem->TranslateVirtual<uint32_t*>(data + 0) = __builtin_bswap32(repl.first);
        *mem->TranslateVirtual<uint32_t*>(data + 4) = __builtin_bswap32(repl.second);
        REXLOG_INFO("[lua] INJECT '{}' <- {} bytes", key, repl.second);
    }
}

// Midasm hook at GradeScore::GetScoreWithGrade entry (0x826FAFD8): r3 = the
// current level's GradeScore object. Capturing it lets the overlay query the
// active medal thresholds (see update_level_score). Resets the cached thresholds
// when the object changes (new level / game mode).
void on_grade_query(PPCRegister& r3) {
    const uint32_t gs = r3.u32;
    if (gs < 0x10000u || gs >= 0xC0000000u || (gs & 3u) != 0u) return;  // implausible ptr
    if (g_grade_score.exchange(gs, std::memory_order_relaxed) != gs) {
        for (auto& a : g_trophy_auto) a.store(0, std::memory_order_relaxed);  // re-query
    }
}

// Hooked at UnlockableManager::GetUnlockable entry (sub_826F5648): r3 = manager.
void on_get_unlockable(PPCRegister& r3) {
    const uint32_t m = r3.u32;
    if (m >= 0x10000u && m < 0xC0000000u && (m & 3u) == 0u)
        g_unlock_mgr.store(m, std::memory_order_relaxed);
}

// Resolve the UnlockableManager directly via the engine's service locator,
// independent of any menu. The game stores it as a lazily-created singleton in
// its Ref registry: the Ref ctor (sub_8243C0F8) returns the registered instance
// or creates one via the factory (sub_82430788 = alloc 292 + ctor sub_826F75A0).
// This is the same path the save-load code (sub_827E5E48) uses to get it, so we
// always get the real instance. Returns the guest manager pointer, or 0.
static uint32_t resolve_unlock_mgr() {
    auto* fd  = rex::Runtime::instance()->function_dispatcher();
    auto* mem = rex::system::kernel_memory();
    if (!fd || !mem) return 0;
    auto* ctor = fd->GetFunction(0x8243C0F8u);  // Ref<UnlockableManager> ctor
    if (!ctor) return 0;
    // 8-byte guest Ref struct {manager*, refnode*}; allocate once, reuse.
    static uint32_t ref_buf = mem->SystemHeapAlloc(8, 0x20);
    if (!ref_buf) return 0;
    *mem->TranslateVirtual<uint32_t*>(ref_buf)     = 0;
    *mem->TranslateVirtual<uint32_t*>(ref_buf + 4) = 0;
    rex::ppc::GuestToHostFunction<void>(*ctor, ref_buf, 0x82430788u);
    // We intentionally do NOT release the ref (sub_824394A8): leaking one
    // reference keeps the singleton alive for the rest of the session.
    return __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(ref_buf));
}

// While unlock_all is set, force every unlockable's "unlocked" flag on by
// calling the game's UnlockAllUnlockables(manager, true) (sub_826F4E78, which
// sets byte +12 = 1 on every entry in the manager's four unlockable vectors).
// The manager comes from on_get_unlockable if that fired, otherwise we resolve
// it ourselves. Re-applied ~1/sec so content stays unlocked if the game
// re-evaluates locks; leave the cvar on as a toggle.
static void maybe_unlock_all() {
    if (!REXCVAR_GET(unlock_all)) return;
    uint32_t m = g_unlock_mgr.load(std::memory_order_relaxed);
    if (!m) {
        m = resolve_unlock_mgr();
        if (m < 0x10000u || m >= 0xC0000000u || (m & 3u) != 0u) return;  // not ready
        g_unlock_mgr.store(m, std::memory_order_relaxed);
        REXLOG_INFO("[unlock] resolved UnlockableManager {:08X}", m);
    }
    static int tick = 0;
    if (tick++ % 60 != 0) return;  // ~once per second (on_swap is per-frame)
    if (auto* fd = rex::Runtime::instance()->function_dispatcher()) {
        if (auto* fn = fd->GetFunction(0x826F4E78u))  // UnlockAllUnlockables(mgr, true)
            rex::ppc::GuestToHostFunction<void>(*fn, m, 1);
    }
}

// --- Guest input suppression ------------------------------------------------
// While the difficulty panel is open it reads the pad host-side (InputSystem),
// so the guest must not see the same presses or the game menu underneath would
// navigate too. The title funnels ALL pad reads through two XamInput wrappers;
// overriding them (strong symbol over the codegen'd weak alias) blanks what
// the game sees while suppression is on:
//   sub_830B3EF0: r3=user, r4=X_INPUT_STATE* -> XamInputGetState. Run the
//     original, then zero the 12-byte X_INPUT_GAMEPAD at state+4 (packet
//     number stays, so the game just sees "connected, nothing pressed").
//   sub_830B3FC8: keystroke wrapper -> XamInputGetKeystrokeEx. Return
//     X_ERROR_EMPTY (0x10D2, "no keystrokes queued") without calling it.
//
// Suppression does NOT lift the instant the panel closes: the A press that
// picked a difficulty is usually still held, and the game's edge detector
// would see it as a brand-new press and activate the menu item behind the
// panel. So closing enters a "held until release" state that keeps blanking
// until the pad reads fully idle, then clears itself.

enum : int { kInputPass = 0, kInputBlank = 1, kInputUntilRelease = 2 };
static std::atomic<int> g_input_suppress{kInputPass};

// Mirrors "a cinematic is on screen" for the input hooks -- a cheap atomic
// instead of the video request mutex, which these hot paths poll constantly.
static std::atomic<bool> g_video_active{false};

// Attract mode: declared early so the pad hooks below can dismiss a cinematic.
void note_input_activity();

// True when the 12-byte X_INPUT_GAMEPAD shows nothing pressed: buttons (u16)
// and both triggers zero, sticks inside a wide deadzone (big-endian s16s, so
// test the high byte). Wide on purpose so a drifting stick isn't "input".
static bool pad_idle(const uint8_t* pad) {
    if (pad[0] || pad[1] || pad[2] || pad[3]) return false;
    for (int i = 4; i < 12; i += 2) {
        const uint8_t hi = pad[i];
        if (hi >= 0x20 && hi < 0xE0) return false;
    }
    return true;
}

void set_guest_input_suppressed(bool on) {
    g_input_suppress.store(on ? kInputBlank : kInputUntilRelease,
                           std::memory_order_relaxed);
}

// M3.247: bitmask of pad indices our runtime reports as CONNECTED.
// PADLOG measured user=0 -> 0 (SUCCESS) and user=1 -> 0 (SUCCESS) while users
// 2 and 3 correctly answer 0x48F (ERROR_DEVICE_NOT_CONNECTED). So the title
// sees TWO controllers, and XamUserGetSigninState(1) returns 0 -- a connected
// controller with no profile, which is exactly what makes it raise
// StateSigninPrompt. Recorded here (cheap, unconditional) and consumed by the
// XamUserGetSigninState hook far below.
// M3.260 (RESTUFF_LOGPID=1): give every BOOT its own log file.
//
// All 43 probe sites appended to one shared mergelog.txt, so attributing a line
// to a run meant counting lines in order -- and the harness RETRIES failed
// boots, so line N does not correspond to run N. I used that ordering to claim
// "the boot that showed the dialog cached [1 0 0 0]", and that attribution was
// not sound. With one file per process the mapping is exact.
// M3.282: the evidence probes are DEFAULT ON so the bug can be captured the
// first time it hits the USER -- but they all wrote into /tmp/restuff_drive,
// which only exists because the test harness mkdir's it. On the user's own
// machine (and on the Windows build, where /tmp is not even a path) every
// fopen failed and the probes recorded NOTHING, silently. A default-on
// instrument that cannot write on the one machine that reproduces the bug is
// not an instrument. Create the directory, and fall back to a path beside the
// executable if that is not writable.
static const char* restuff_logpath() {
  static const std::string path = [] {
    const std::string leaf = getenv("RESTUFF_LOGPID")
                                 ? "mergelog_" + std::to_string(getpid()) + ".txt"
                                 : std::string("mergelog.txt");
    std::error_code ec;
    std::filesystem::create_directories("/tmp/restuff_drive", ec);
    std::string chosen;
    const std::string primary = "/tmp/restuff_drive/" + leaf;
    if (FILE* probe = fopen(primary.c_str(), "a")) {  // writable?
      fclose(probe);
      chosen = primary;
    } else {
      // Fall back next to the running executable -- reachable on Windows too,
      // and somewhere the user can actually find it to send back.
      std::filesystem::path cwd = std::filesystem::current_path(ec);
      chosen = !ec ? (cwd / ("restuff_" + leaf)).string()
                   : std::string("restuff_") + leaf;
    }
    // M3.285: rotation cap. The non-PID log APPENDS across every session, so on
    // a player's machine it would grow forever (~4KB/boot). At 512KB, keep the
    // most recent half so the tail -- the boot being reported -- is never lost.
    const auto sz = std::filesystem::file_size(chosen, ec);
    if (!ec && sz > 512 * 1024) {
      std::string tail;
      if (FILE* f = fopen(chosen.c_str(), "rb")) {
        tail.resize(256 * 1024);
        fseek(f, -long(tail.size()), SEEK_END);
        tail.resize(fread(tail.data(), 1, tail.size(), f));
        fclose(f);
      }
      if (FILE* f = fopen(chosen.c_str(), "wb")) {
        fputs("[rotated: older sessions dropped]\n", f);
        fwrite(tail.data(), 1, tail.size(), f);
        fclose(f);
      }
    }
    return chosen;
  }();
  return path.c_str();
}

// M3.265: the sign-in evidence probes are DEFAULT ON (kill switch
// RESTUFF_NO_SIGNIN_EVIDENCE=1). Six theories have now been refuted and the
// remaining ones only distinguish themselves on a boot that actually shows the
// prompt -- which is ~2% here and cannot be reproduced on demand. The user CAN
// hit it. So the evidence must be captured the first time it happens to them,
// not require them to know which env var to set beforehand. All three probes
// are bounded: SIGNCACHE is one line per boot, MSGBOX logs nothing at all on a
// healthy boot (verified), KEYSTROKE is capped.
static bool restuff_signin_evidence() {
  static const bool on = getenv("RESTUFF_NO_SIGNIN_EVIDENCE") == nullptr;
  return on;
}

static std::atomic<uint32_t> g_pad_connected{0};

// M3.257: make the title's CACHED per-slot sign-in belief agree with the
// connectivity we actually report.
//
// sub_82B2F708 polls all four slots ONCE and caches the answers at
// *(profileMgr + 136 + 4*slot), then clears its refresh flag; the title reads
// that cache from then on and never asks XAM again. The poll runs BEFORE our
// pad layer has answered anything, so g_pad_connected is still empty and
// M3.247's upgrade never fires (SIGNINFIX count is 0, measured). The cache is
// therefore frozen at "pad 1 is a controller with no profile", which is what
// the title prompts about.
//
// Rather than hide pad 1 (RESTUFF_PAD1_OFF, which would also hide a REAL second
// controller, and this game has multiplayer), fix the disagreement: the moment
// we learn a slot is connected, make its cached sign-in state say the local user
// owns it. That invents nothing for absent pads -- only slots we ourselves
// report as connected are touched -- and it is the truthful answer for a runtime
// with exactly one local user.
static std::atomic<uint32_t> g_profile_mgr{0};

static void restuff_fedump_tick(uint8_t* base);

static void restuff_signcache_sync(uint8_t* base) {
  // M3.283: DEFAULT ON (kill-switch RESTUFF_NO_SIGNCACHE_FIX=1). The old
  // opt-in spelling RESTUFF_SIGNCACHE_FIX needs no special case: with the
  // default on it is simply redundant, and the kill-switch always wins.
  //
  // Flipped after the bug was finally captured ON THE USER'S OWN SESSION
  // (2026-08-14, mergelog.txt): 5.6 minutes into live play, another process's
  // virtual gamepad connected as pad 1 (PADMASK 1->3 at t=339s), ~15s later the
  // title ran its new-player signin check for user 1 (lr=82840A18, twice) and
  // raised "Would you like to sign in with a gamer profile?" -- with the real
  // save-loss consequence that "Continue Without Sign In" disables saving.
  //
  // The invariant this enforces: this runtime hosts exactly ONE local human, so
  // any pad it reports CONNECTED belongs to that human and is signed in
  // locally. Writing that into the title's slot cache makes the prompt's
  // trigger state -- connected controller with no profile -- unrepresentable,
  // regardless of WHICH second gamepad appears (another session's uinput pad,
  // Steam Input's virtual X360 pad, a real second controller) or WHEN.
  // Suppressing the device instead was rejected: the prompt predates any
  // specific pad source, and hiding pad 1 would break a genuine second
  // controller.
  //
  // Evidence at flip time (thin, but mechanism-complete): forced-pad arms give
  // 1/1 triggered boots stalled without the repair vs 0/2 with it, and the
  // repair is the only arm where the title's user-1 query came back consistent.
  // Slot 0 is never written here (it already caches 1 on every boot observed --
  // 42/42 logs), so the user-0 save path is untouched.
  static const bool fix = getenv("RESTUFF_NO_SIGNCACHE_FIX") == nullptr;
  if (!fix) return;
  const uint32_t mgr = g_profile_mgr.load(std::memory_order_relaxed);
  if (mgr < 0x10000u || mgr >= 0xC0000000u - 160u) return;
  const uint32_t mask = g_pad_connected.load(std::memory_order_relaxed);
  for (uint32_t slot = 0; slot < 4; ++slot) {
    if (!(mask & (1u << slot))) continue;          // never invent absent pads
    const uint32_t a = mgr + 136 + 4 * slot;
    uint32_t v;
    std::memcpy(&v, base + a, 4);
    if (__builtin_bswap32(v) != 0) continue;       // already has a profile
    const uint32_t one = __builtin_bswap32(1u);    // eSIGNIN_LOCALLY
    std::memcpy(base + a, &one, 4);
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 8) {  // xamlog is declared further down; write directly here
      if (FILE* f = fopen(restuff_logpath(), "a")) {
        fprintf(f, "SIGNCACHE-FIX#%u slot=%u cache 0->1 (mgr=%08X)\n", k, slot, mgr);
        fclose(f);
      }
    }
  }
}

REX_EXTERN(__imp__sub_830B3EF0);
REX_HOOK_RAW(sub_830B3EF0) {
    const uint32_t state = ctx.r4.u32;
    const uint32_t user = ctx.r3.u32;
    __imp__sub_830B3EF0(ctx, base);
    // M3.248 (RESTUFF_PAD1_OFF=1, OPT-IN): remove the phantom controller at
    // source by reporting slots >= 1 as ERROR_DEVICE_NOT_CONNECTED.
    //
    // Why this and not M3.247 alone: sfix11 showed the popup STILL appears with
    // M3.247 active, and the log ORDER says why -- the title's 4-pad signin
    // refresh (lr=82B2F750) runs BEFORE the first pad read:
    //     line 1: XAM#0 GetSigninState(user=0) ...
    //     line 5: PADLOG t=0ms user=0 -> connected
    // so the connected-bitmask M3.247 consults is still empty when the title
    // caches its per-pad states, and the cache already says "pad 1 has no
    // profile". M3.247 fires later (t=29.5s) and cannot undo that.
    // This hook runs at the earliest possible point, so fixing connectivity
    // here is ordered correctly.
    // OPT-IN because it would also hide a genuine second controller.
    //
    // ⛔ M3.280 CORRECTION: this used to add "the phantom is almost certainly the
    // NOP driver". It is not, and the SDK source says so outright --
    // nop_input_driver.cpp returns X_ERROR_DEVICE_NOT_CONNECTED for EVERY
    // user_index != 0, and mnk_input_driver.cpp serves only mnk_user_index
    // (cvar, default 0). CreateDefaultInputSystem registers SDL(0) + MnK(0) +
    // NOP, where that `nop_index` argument is a window Z-ORDER, not a user slot.
    // With one controller attached nothing claims index 1, which is exactly what
    // M3.258 measured. Pad 1 reads connected only when a second real or virtual
    // gamepad genuinely exists (on this machine, a uinput pad from another
    // process). So this whole knob suppresses a REAL device, and suppression is
    // the wrong fix anyway: the prompt predates that device, and a fix that only
    // holds when nothing else is running is not a fix. Keep this as a
    // diagnostic; the consistency repair (restuff_signcache_sync) is the fix.
    {
      static const bool pad1off = getenv("RESTUFF_PAD1_OFF") != nullptr;
      if (pad1off && user >= 1 && ctx.r3.u32 == 0) {
        ctx.r3.u64 = 0x48F;  // ERROR_DEVICE_NOT_CONNECTED, as slots 2/3 already say
      }
      // M3.281 (RESTUFF_PAD1_ON=1, OPT-IN, TEST ONLY): the INVERSE knob -- force
      // pad 1 to report CONNECTED. This is a REPRODUCER, never a fix.
      //
      // Why it is needed: the prompt's natural rate is far too low and too
      // erratic to A/B against. A baseline arm of 12 boots scored 0 sign-ins, so
      // the fix arm had nothing to prevent and the comparison had no power --
      // and the rate depends on whether some other process happens to be
      // presenting a second gamepad, which is not a controllable variable.
      // This recreates the exact condition on demand instead: a second
      // controller that the title will find has no profile.
      //
      // It deliberately preserves the ORDERING that matters -- the mask is still
      // only populated at the first pad read, which is AFTER the title caches
      // its four slot states -- so it reproduces the real race rather than a
      // sanitised version of it. With this, "does restuff_signcache_sync repair
      // it in time?" is answerable in a handful of boots instead of hundreds.
      static const bool pad1on = getenv("RESTUFF_PAD1_ON") != nullptr;
      if (pad1on && user == 1 && ctx.r3.u32 != 0) {
        ctx.r3.u64 = 0;  // X_ERROR_SUCCESS == connected
      }
    }
    if (user < 4) {
      const uint32_t bit = 1u << user;
      // M3.280 (DEFAULT ON, cheap): log every CHANGE to the connected mask.
      //
      // The only existing sample of this mask is inside SIGNCACHE, which fires
      // once at ~t=0 when the mask is still 0 -- so a pad appearing LATER was
      // invisible on every boot ever captured.
      //
      // Why that gap matters: in a 3-boot batch, the ONE boot that showed the
      // sign-in prompt is the only one that logged SIGNINFIX (user=1, 0->1,
      // lr=82840A18); the two healthy boots have no such line and instead walk
      // on to the content/device phase ~34s in. But SIGNINFIX requires TWO
      // things at once -- pad 1 reporting connected AND the title querying
      // user 1's signin state -- so its absence does not say which was missing.
      // This log separates them: it records pad-1 connectivity on EVERY boot,
      // whether or not the title ever asks about it. If prompt boots turn out
      // to be exactly the boots where a phantom pad 1 appears, the trigger is
      // ours, not the game's; if pad 1 appears on healthy boots too, the
      // phantom-pad theory is dead and the query itself is the anomaly.
      //
      // Logging only on change keeps this out of the hot path: XamInputGetState
      // runs per-pad per-frame, but the mask settles in the first seconds.
      const uint32_t prev = (ctx.r3.u32 == 0)
          ? g_pad_connected.fetch_or(bit, std::memory_order_relaxed)
          : g_pad_connected.fetch_and(~bit, std::memory_order_relaxed);
      const uint32_t now = (ctx.r3.u32 == 0) ? (prev | bit) : (prev & ~bit);
      if (now != prev) {
        static std::atomic<uint32_t> np{0};
        const uint32_t q = np.fetch_add(1, std::memory_order_relaxed);
        // raw fopen + local clock, not xamlog::Line / ibwatch::SinceArmMs --
        // both are declared ~700-2700 lines below this hook. Same pattern the
        // PADLOG probe just below uses; only ordering matters here.
        if (q < 16) {
          static const auto tp0 = std::chrono::steady_clock::now();
          const long pms = long(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - tp0)
                                    .count());
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "PADMASK#%u t=%ldms(proc) user=%u %s mask=%X->%X%s\n", q,
                    pms, user,
                    ctx.r3.u32 == 0 ? "CONNECTED" : "gone", prev, now,
                    (ctx.r3.u32 == 0 && user >= 1)
                        ? "  <<< PHANTOM 2nd PAD (suspected prompt trigger)" : "");
            fclose(f);
          }
        }
      }
      restuff_signcache_sync(base);   // ordered AFTER we know: fixes the race
      restuff_fedump_tick(base);      // M3.279: 1 Hz start-menu sub-state timeline
    }
    // M3.246 (RESTUFF_PADLOG=1): which pads do we report as CONNECTED?
    //
    // Live reproduction of the sign-in popup (2026-08-11, captured in
    // padlogs/wedge_corpus_2026-08-10/signin_LIVE_repro_*) showed the stuck boot
    // making a call HEALTHY BOOTS NEVER MAKE:
    //     healthy: XAM#4 t=181ms  user=0 -> 1   lr=82840A18
    //     stuck  : XAM#4 t=183ms  user=0 -> 1   lr=82840A18
    //              XAM#5 t=18816ms user=1 -> 0  lr=82840A18   <-- extra, pad 1
    // sub_82840A00 takes the user index as its argument and bails unless that
    // user's signin state is 1 or 2, so querying pad 1 yields "no profile".
    // The likely trigger is a PHANTOM CONTROLLER: if XamInputGetState reports
    // success (= connected) for user 1, the title sees a controller with no
    // profile and raises StateSigninPrompt. This logs the return code per user
    // index so that can be confirmed or killed -- a success for users 1..3 is
    // the smoking gun, an error means the phantom-pad theory is wrong.
    {
      static const bool pl = getenv("RESTUFF_PADLOG") != nullptr;
      if (pl) {
        // NOTE: xamlog::Line is declared ~2500 lines below this hook, so write
        // directly rather than reordering the file for a probe.
        static std::atomic<uint32_t> seen[4] = {};
        if (user < 4 && seen[user].fetch_add(1, std::memory_order_relaxed) < 3) {
          // ibwatch::SinceArmMs is also declared below this point, so use a
          // local process-relative clock; only ordering matters here.
          static const auto t0 = std::chrono::steady_clock::now();
          const long ms = long(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - t0)
                                   .count());
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "PADLOG t=%ldms(proc) user=%u -> ret=%08X (0 == connected)\n",
                    ms, user, ctx.r3.u32);
            fclose(f);
          }
        }
      }
    }
    // env RESTUFF_KEYPAD=<file>: virtual pad for headless test driving. The
    // file holds a hex X_INPUT_GAMEPAD_* button mask (e.g. "0010" = START);
    // it is OR'd into user-0 reads and the packet number bumps on change so
    // the game's edge detector sees a fresh press. Inactive unless set —
    // desktop/keyboard/gamepad behavior is untouched.
    if (state && user == 0 && ctx.r3.u32 == 0) {
        static const char* vpad_path = getenv("RESTUFF_KEYPAD");
        static const char* replay_env = getenv("RESTUFF_PAD_REPLAY");
        if (vpad_path || replay_env) {
            // File format: "BBBB" (hex button mask) optionally followed by
            // "LX LY RX RY" signed stick values (-32768..32767) -- M3.68 lets
            // headless drives WALK (the gate area is unreachable by buttons).
            uint32_t vbtn = 0;
            int lx = 0, ly = 0, rx = 0, ry = 0, got = 0;
            int lt = 0, rt = 0;  // M3.130c: analog triggers (RT kicks doors open)
            // RESTUFF_PAD_REPLAY=<file>: play back a RESTUFF_PAD_RECORD capture.
            // Advances by input-poll index (the recording's own clock), so a
            // headless run reproduces the human's route step for step even when
            // it runs at a fraction of the speed. Takes precedence over the
            // single-state keypad file; the trailing state is held after the
            // recording ends.
            static const char* replay_path = getenv("RESTUFF_PAD_REPLAY");
            if (replay_path) {
                struct PadEv { uint64_t poll, ms; uint32_t btn; int lx, ly, rx, ry, lt, rt; };
                static std::vector<PadEv> evs;
                static bool loaded = false;
                static size_t cur = 0;
                static uint64_t rpoll = 0;
                if (!loaded) {
                    loaded = true;
                    if (FILE* f = fopen(replay_path, "rb")) {
                        char line[160];
                        while (fgets(line, sizeof(line), f)) {
                            if (line[0] == '#') continue;
                            PadEv e{};
                            unsigned long long pl = 0, ms = 0;
                            if (sscanf(line, "%llu %llu %x %d %d %d %d %d %d", &pl, &ms, &e.btn,
                                       &e.lx, &e.ly, &e.rx, &e.ry, &e.lt, &e.rt) >= 7) {
                                e.poll = pl;
                                e.ms = ms;
                                evs.push_back(e);
                            }
                        }
                        fclose(f);
                    }
                    fprintf(stderr, "[PAD-REPLAY] loaded %zu events from %s\n", evs.size(),
                            replay_path);
                }
                // RESTUFF_PAD_REPLAY_CLOCK=1 keys playback on the recording's
                // WALL CLOCK instead of its poll index. Which one reproduces a
                // route depends on whether the game integrates delta time
                // (clock-true) or steps per frame (poll-true) -- unknown for
                // this title, so both are available and can be tried in turn.
                static const bool s_clock = getenv("RESTUFF_PAD_REPLAY_CLOCK") != nullptr;
                static const auto rt0 = std::chrono::steady_clock::now();
                // RESTUFF_PAD_REPLAY_DELAY=<ms|polls>: hold the recording at its
                // first event for this long before advancing. START ALIGNMENT is
                // the whole difficulty of replay -- the recording's poll 0 is the
                // HUMAN's first input poll, ours arrives after a boot of variable
                // (and usually longer) length, so every early press fires before
                // the screen it was meant for exists. A replay of a route to the
                // gate stalled on the "sign in with a gamer profile?" dialog for
                // exactly this reason: the A presses that would dismiss it had
                // already been spent. Shifting the whole timeline is the cheap
                // fix, and the right value is empirical -- sweep it.
                // Units follow the keying mode: ms when _CLOCK=1, polls otherwise.
                static const uint64_t s_delay = [] {
                    const char* e = getenv("RESTUFF_PAD_REPLAY_DELAY");
                    return e ? strtoull(e, nullptr, 10) : 0ull;
                }();
                const uint64_t now_ms =
                    s_clock ? uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - rt0).count())
                            : 0;
                // Subtract the delay from our own progress (saturating), so the
                // recording simply starts later on the same relative timeline.
                const uint64_t key = s_clock ? (now_ms > s_delay ? now_ms - s_delay : 0)
                                             : (rpoll > s_delay ? rpoll - s_delay : 0);
                while (cur + 1 < evs.size() &&
                       (s_clock ? evs[cur + 1].ms <= key : evs[cur + 1].poll <= key))
                    ++cur;
                if (!evs.empty() && (s_clock ? evs[cur].ms <= key : evs[cur].poll <= key)) {
                    vbtn = evs[cur].btn;
                    lx = evs[cur].lx; ly = evs[cur].ly;
                    rx = evs[cur].rx; ry = evs[cur].ry;
                    lt = evs[cur].lt; rt = evs[cur].rt;
                    got = 5;
                }
                ++rpoll;
            } else if (FILE* f = vpad_path ? fopen(vpad_path, "rb") : nullptr) {
                char buf[64] = {};
                if (fread(buf, 1, sizeof(buf) - 1, f) > 0)
                    got = sscanf(buf, "%x %d %d %d %d", &vbtn, &lx, &ly, &rx, &ry);
                fclose(f);
            }
            static std::atomic<uint64_t> s_prev{0};
            static std::atomic<uint32_t> s_pktbump{0};
            const uint64_t sig = (uint64_t(vbtn) << 40) ^ (uint64_t(uint16_t(lx)) << 32) ^
                                 (uint64_t(uint16_t(ly)) << 16) ^ (uint16_t(rx) ^ (uint32_t(uint16_t(ry)) << 8));
            if (s_prev.exchange(sig, std::memory_order_relaxed) != sig)
                s_pktbump.fetch_add(1, std::memory_order_relaxed);
            const uint32_t a = state;
            const uint32_t o = (a >= 0xE0000000u) ? 0x1000u : 0u;
            uint8_t* st = base + a + o;
            // M3.85: the virtual pad OWNS the input when active. This used to
            // OR into the real GetState result ("headless has no real pad") --
            // but a connected DualSense the user is playing ANOTHER game with
            // leaked its buttons/sticks into the drive and contaminated the
            // scripted route. Zero the whole gamepad first; only vpad applies.
            std::memset(st + 4, 0, 12);
            if (vbtn) {
                st[4] |= uint8_t(vbtn >> 8);
                st[5] |= uint8_t(vbtn);
            }
            // X_INPUT_GAMEPAD at state+4: buttons+0, triggers+2, LX+4 LY+6
            // RX+8 RY+10 (guest big-endian). Headless has no real pad, so
            // plain stores are safe.
            auto put16 = [&](uint32_t off, int v) {
                st[4 + off] = uint8_t(uint16_t(v) >> 8);
                st[4 + off + 1] = uint8_t(uint16_t(v));
            };
            if (got >= 3) {
                put16(4, lx);
                put16(6, ly);
            }
            if (got >= 5) {
                put16(8, rx);
                put16(10, ry);
            }
            // M3.130c: analog triggers. The memset above cleared them and only
            // the sticks/buttons were being restored, so a replayed route lost
            // every trigger press -- including the RT that kicks the hut door
            // open, which is why a recording that went outside replayed as the
            // bear stuck indoors. left_trigger is at +2, right_trigger at +3.
            st[4 + 2] = uint8_t(lt);
            st[4 + 3] = uint8_t(rt);
            const uint32_t pkt = (uint32_t(st[0]) << 24) | (uint32_t(st[1]) << 16) |
                                 (uint32_t(st[2]) << 8) | st[3];
            const uint32_t npkt = pkt + s_pktbump.load(std::memory_order_relaxed);
            st[0] = uint8_t(npkt >> 24); st[1] = uint8_t(npkt >> 16);
            st[2] = uint8_t(npkt >> 8);  st[3] = uint8_t(npkt);
        }
    }
    // RESTUFF_PAD_RECORD=<file>: log the pad the GAME RECEIVES each input poll, so a human
    // can play a route once and have a headless drive reproduce it exactly.
    // Placed AFTER the virtual-pad apply so it captures what the guest
    // actually sees -- real pad in a human session, vpad/replay in a drive
    // (which makes the recorder self-testable against a scripted route).
    // Keyed on POLL INDEX, not wall clock: the game polls once per frame, so
    // replaying by index reproduces the same per-frame input regardless of how
    // much slower the headless run is. Line format:
    //   <poll> <btn_hex> <lx> <ly> <rx> <ry> <lt> <rt>
    // One line per CHANGE (plus the first poll), so files stay small.
    if (state && user == 0 && ctx.r3.u32 == 0) {
        static const char* rec_path = getenv("RESTUFF_PAD_RECORD");
        if (rec_path) {
            const uint32_t a0 = state;
            const uint32_t o0 = (a0 >= 0xE0000000u) ? 0x1000u : 0u;
            const uint8_t* st0 = base + a0 + o0;
            auto rd16 = [&](uint32_t off) -> int {
                return int16_t((uint16_t(st0[4 + off]) << 8) | st0[4 + off + 1]);
            };
            const uint32_t btn = (uint32_t(st0[4]) << 8) | st0[5];
            const int lt = st0[6], rt = st0[7];
            const int lx = rd16(4), ly = rd16(6), rx = rd16(8), ry = rd16(10);
            static FILE* rf = nullptr;
            static const auto t0 = std::chrono::steady_clock::now();
            static uint64_t poll = 0;
            static uint64_t prev_sig = ~0ull;
            if (!rf) {
                rf = fopen(rec_path, "wb");
                if (rf) fprintf(rf, "# restuff pad recording: poll ms btn lx ly rx ry lt rt\n");
            }
            const uint64_t sig = (uint64_t(btn) << 48) ^ (uint64_t(uint16_t(lx)) << 32) ^
                                 (uint64_t(uint16_t(ly)) << 16) ^ uint64_t(uint16_t(rx)) ^
                                 (uint64_t(uint16_t(ry)) << 8) ^ (uint64_t(lt) << 24) ^
                                 (uint64_t(rt) << 40);
            if (rf && sig != prev_sig) {
                prev_sig = sig;
                const uint64_t ms = uint64_t(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count());
                fprintf(rf, "%llu %llu %04X %d %d %d %d %d %d\n", (unsigned long long)poll,
                        (unsigned long long)ms, btn, lx, ly, rx, ry, lt, rt);
                fflush(rf);  // the drive SIGKILLs the game; never buffer the route
            }
            ++poll;
        }
    }
    // DEBUG (env RESTUFF_DUMP_INPUT): what does the guest's pad funnel see?
    if (getenv("RESTUFF_DUMP_INPUT") && state) {
        static std::atomic<int> s_inbudget{60};
        const uint32_t addr2 = state + 4;
        const uint32_t off2 = (addr2 >= 0xE0000000u) ? 0x1000u : 0u;
        const uint8_t* pad2 = base + addr2 + off2;
        const uint32_t buttons = (uint32_t(pad2[0]) << 8) | pad2[1];
        if ((buttons || ctx.r3.u32 != 0) && s_inbudget.fetch_sub(1, std::memory_order_relaxed) > 0) {
            REXLOG_INFO("[INPUT] GetState user={} ret=0x{:08X} buttons=0x{:04X}", user,
                        (uint32_t)ctx.r3.u32, buttons);
        }
    }
    int mode = g_input_suppress.load(std::memory_order_relaxed);
    if (mode == kInputPass || !state) return;
    if (ctx.r3.u32 != 0) {  // pad not connected: nothing held, nothing to blank
        if (mode == kInputUntilRelease)
            g_input_suppress.store(kInputPass, std::memory_order_relaxed);
        return;
    }

    const uint32_t addr = state + 4;  // X_INPUT_GAMEPAD part
    const uint32_t off = (addr >= 0xE0000000u) ? 0x1000u : 0u;
    uint8_t* pad = base + addr + off;

    // ANY pad activity dismisses a cinematic. Done here, on the raw state
    // before it gets blanked, because XamInputGetKeystroke only reports a
    // subset of the buttons -- this way every button/trigger/stick works.
    if (g_video_active.load(std::memory_order_relaxed) && !pad_idle(pad))
        note_input_activity();

    if (mode == kInputUntilRelease) {
        // buttons (u16) + triggers (2x u8) idle, sticks inside a wide deadzone
        // (big-endian s16s; check the high byte: 0x00..0x1F / 0xE0..0xFF).
        bool idle = pad[0] == 0 && pad[1] == 0 && pad[2] == 0 && pad[3] == 0;
        for (int i = 4; idle && i < 12; i += 2) {
            const uint8_t hi = pad[i];
            idle = (hi < 0x20) || (hi >= 0xE0);
        }
        if (idle) {
            g_input_suppress.store(kInputPass, std::memory_order_relaxed);
            return;
        }
    }
    std::memset(pad, 0, 12);
}

REX_EXTERN(__imp__sub_830B3FC8);
REX_HOOK_RAW(sub_830B3FC8) {
    if (g_input_suppress.load(std::memory_order_relaxed) != kInputPass) {
        // While a cinematic is up input is suppressed, but we still need to
        // SEE the dismissing press. Read the real keystroke for our own use,
        // then hand the game X_ERROR_EMPTY so it can't also act on it.
        if (g_video_active.load(std::memory_order_relaxed)) {
            __imp__sub_830B3FC8(ctx, base);
            if (ctx.r3.u32 == 0) note_input_activity();
        }
        ctx.r3.u64 = 0x10D2;  // X_ERROR_EMPTY
        return;
    }
    // M3.263: does the game get told the WRONG PAD pressed the button?
    //
    // This wrapper is XamInputGetKeystrokeEx, and it has a writeback the
    // GetState path does not (SDK xam_input.cpp:166):
    //     user_index = *user_index_ptr;
    //     if ((user_index & 0xFF) == 0xFF || (flags & ANY_USER)) user_index = 0;
    //     result = is->GetKeystroke(user_index, flags, keystroke);
    //     if (XSUCCEEDED(result)) *user_index_ptr = keystroke->user_index;  // <--
    // The QUERY is pinned to user 0, but the ANSWER hands back whatever the
    // driver wrote into the keystroke. A driver that reports success without
    // setting that field gives the game a press attributed to another pad --
    // and a press from a pad with no profile is exactly what the sign-in
    // prompt asks about. (InputSystem::GetKeystroke also stops at the first
    // driver returning SUCCESS *or* EMPTY, and the NOP driver returns EMPTY
    // for user 0, so driver ORDER matters on this path too.)
    //
    // r3 = user_index_ptr, r4 = flags, r5 = keystroke*.
    const uint32_t uptr = ctx.r3.u32, kflags = ctx.r4.u32;
    __imp__sub_830B3FC8(ctx, base);
    const uint32_t res = ctx.r3.u32;
    // Both pad and keyboard funnel through here, so it doubles as the
    // "player is still here" signal for the attract timer.
    if (res == 0) note_input_activity();
    if (uptr < 0x10000u || uptr >= 0xC0000000u - 4u) return;
    uint32_t rawbe;
    std::memcpy(&rawbe, base + uptr, 4);
    const uint32_t kuser = __builtin_bswap32(rawbe);
    // RESTUFF_KEYSTROKE_FAULT=1 forces the writeback to pad 1, to test ON
    // DEMAND whether "the game thinks another pad pressed it" raises the
    // prompt. Fault injection settled the previous hypothesis in 3 boots
    // instead of a 30-boot lottery at ~2% per boot.
    static const bool kfault = getenv("RESTUFF_KEYSTROKE_FAULT") != nullptr;
    if (kfault && res == 0) {
        const uint32_t one = __builtin_bswap32(1u);
        std::memcpy(base + uptr, &one, 4);
        static std::atomic<uint32_t> nkf{0};
        const uint32_t z = nkf.fetch_add(1, std::memory_order_relaxed);
        if (z < 4) {  // xamlog/ibwatch are declared further down this file
            if (FILE* f = fopen(restuff_logpath(), "a")) {
                fprintf(f, "KEYFAULT#%u forced keystroke user %u->1\n", z, kuser);
                fclose(f);
            }
        }
    }
    // PROVE THE INSTRUMENT IS ALIVE (third time this trap has come up): the
    // filter below only fires on SUCCESS, so silence would be ambiguous between
    // "no wrong-pad keystroke" and "this wrapper is never called". Report the
    // first few calls unconditionally, with the raw result, so the difference
    // is visible. If every call returns 0x10D2 (X_ERROR_EMPTY) then the replay
    // harness drives GetState only and never exercises the keystroke path at
    // all -- in which case this hypothesis cannot be tested headlessly.
    if (restuff_signin_evidence()) {
        static std::atomic<uint32_t> nalive{0};
        const uint32_t a = nalive.fetch_add(1, std::memory_order_relaxed);
        if (a < 6) {
            if (FILE* f = fopen(restuff_logpath(), "a")) {
                fprintf(f, "KEYSTROKE-ALIVE#%u user=%u flags=%08X res=%08X\n", a,
                        kuser, kflags, res);
                fclose(f);
            }
        }
    }
    if (restuff_signin_evidence() && (res == 0 || kuser != 0)) {
        static std::atomic<uint32_t> nks{0};
        const uint32_t k = nks.fetch_add(1, std::memory_order_relaxed);
        if (k < 24 || kuser != 0) {
            if (FILE* f = fopen(restuff_logpath(), "a")) {
                fprintf(f, "KEYSTROKE#%u user=%u flags=%08X res=%08X%s\n", k, kuser,
                        kflags, res, kuser != 0 ? "   <<< NOT PAD 0" : "");
                fclose(f);
            }
        }
    }
}

// Midasm hook at the SetBackgroundColor GAS tag's Execute (sub_82CD17B0). r3 is
// the 8-byte tag object; bytes +5/+6/+7 are R/G/B (the tag's describe method
// sub_82CD2DC8 prints "SetBackgroundColor: (%d %d %d)" from tag[5..7], and the
// Execute assembles ARGB from *(uint32*)(tag+4) and applies it to the screen via
// sub_82CDABB8). The title/attract screen sets a yellow background; the main
// menu sets blue. Rewrite the yellow tag to the cvar-defined blue so the title
// sky matches. Runs at menu speed (not per-draw), so the cost is irrelevant.
void on_set_bg_color(PPCRegister& r3) {
    const uint32_t tag = r3.u32;
    if (!ptr_ok(tag)) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base || !host_readable(base, tag)) return;

    uint8_t* p = base + tag;  // tag < 0xC0000000 (ptr_ok), so no high-range offset
    const int R = p[5], G = p[6], B = p[7];

    if (REXCVAR_GET(sky_recolor_debug)) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 64)
            REXLOG_INFO("[sky] SetBackgroundColor rgb=({},{},{})", R, G, B);
    }

    if (!REXCVAR_GET(sky_recolor)) return;
    // Yellow = high red & green, low blue. The title is ~(250,230,66); the blue
    // menu (B > R) is left untouched.
    const bool yellowish = R >= 170 && G >= 130 && B <= 150 &&
                           R >= B + 40 && G >= B + 30;
    if (!yellowish) return;

    auto clamp8 = [](int v) -> uint8_t {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    p[5] = clamp8(REXCVAR_GET(sky_r));
    p[6] = clamp8(REXCVAR_GET(sky_g));
    p[7] = clamp8(REXCVAR_GET(sky_b));
}

// --- Title sky recolor (Scaleform) ------------------------------------------
// The felt title scene is a Scaleform (GFx) movie. The sky is two solid-fill
// shapes: base `sky_cid` (13) under a warm overlay `sky_cid2` (15). The title
// draws them with a yellow baked fill; the main menu re-tints them blue via the
// timeline. We force both to a blue solid cxform (mul=0, add=(sky_r,g,b)).
//
// The cxform of a GFxPlaceObject2 tag lives at record+12 as 8 big-endian floats
// [Rmul,Radd,Gmul,Gadd,Bmul,Badd,Amul,Aadd] (out = src*mul + add); char id =
// u16 at +82, depth = u32 at +76, "has cxform" flag byte at +87, mode dword at
// +92 (0=place, 1=move, 2=replace).
//
// Recoloring must be PERMANENT (menu->Back replays the same tag records, so a
// one-time rewrite of the records at parse sticks for the whole session) and
// must not touch other movies -- boot logos / loading screens / HUD reuse the
// same low char ids for logos, text, and the loading-icon outline.
//
// Movie scoping BY NAME (all other schemes failed in testing: a game-state gate
// misses the parse (startmenu parses at boot, before its state activates);
// depth-matching MOVE tags hijacks unrelated fade animations; a late trigger
// cid (245) both arrives after the streamed intro already placed the sky AND
// turned out to also exist in NBPlayerHud). The parser stream's loader context
// (stream - 28) holds the movie's NAME at +0x280 -- verified live: "StartMenu",
// "SavingIcon", "popup", "NBPlayerHud", "button_images", "GFxFontLib_Glyphs".
// Matching "StartMenu" identifies the right movie at its FIRST tag, before the
// sky executes, so the records are blue from the first frame shown.
inline bool stream_is_startmenu(uint8_t* base, uint32_t stream) {
    if (stream < 28u) return false;
    bool ok = false;
    const uint32_t p = rd32_safe(base, stream - 28u + 0x280u, ok);
    if (!ok || !ptr_ok(p) || !host_readable(base, p) || !host_readable(base, p + 9))
        return false;
    static const char kName[] = "StartMenu";
    for (int i = 0; i < 9; ++i)
        if (static_cast<char>(base[p + i]) != kName[i]) return false;
    return base[p + 9] == 0;  // exact match only
}

// Inclusive cid range [sky_cid .. sky_cid2]. The sky is three stacked shapes
// (13 base / 14 day layer / 15 warm overlay) -- dropping 14 leaves the settled
// title yellow. Safe as a range because the name scoping below already limits
// the rewrite to the StartMenu movie.
inline bool is_sky_cid(int cid) {
    const int lo = REXCVAR_GET(sky_cid);
    if (lo < 0) return false;
    int hi = REXCVAR_GET(sky_cid2);
    if (hi < lo) hi = lo;
    return cid >= lo && cid <= hi;
}
// Write a blue solid-fill cxform (mul=0, add=blue) into a tag record and mark it
// present so Execute applies it.
inline void apply_sky_blue(uint8_t* base, uint32_t rec) {
    auto clampf = [](int v) -> float {
        return static_cast<float>(v < 0 ? 0 : (v > 255 ? 255 : v));
    };
    base[rec + 87] = 1;
    wr_f32(base, rec + 12, 0.f);                          // R mul
    wr_f32(base, rec + 20, 0.f);                          // G mul
    wr_f32(base, rec + 28, 0.f);                          // B mul
    wr_f32(base, rec + 16, clampf(REXCVAR_GET(sky_r)));   // R add
    wr_f32(base, rec + 24, clampf(REXCVAR_GET(sky_g)));   // G add
    wr_f32(base, rec + 32, clampf(REXCVAR_GET(sky_b)));   // B add
    wr_f32(base, rec + 36, 1.f);                          // A mul (keep alpha)
    wr_f32(base, rec + 40, 0.f);                          // A add
}
// Neutralize a warm tint on TEXTURED content: identity color channels (the
// warm cast disappears and the artwork's native colors show through -- the
// title-scene bitmap has a BLUE sky baked in). Alpha pair untouched so fade
// animations survive. A solid fill here would flatten the texture into a
// featureless blue sheet (verified user-visible regression).
inline void apply_sky_identity(uint8_t* base, uint32_t rec) {
    base[rec + 87] = 1;
    wr_f32(base, rec + 12, 1.f);   // R mul
    wr_f32(base, rec + 20, 1.f);   // G mul
    wr_f32(base, rec + 28, 1.f);   // B mul
    wr_f32(base, rec + 16, 0.f);   // R add
    wr_f32(base, rec + 24, 0.f);   // G add
    wr_f32(base, rec + 32, 0.f);   // B add
}
// True for the warm day-sky tint cxform: a strongly warm ADD term (high red,
// low blue). The title's yellow is NOT a solid fill -- it's cid 14 placed with
// mul=(0.50,0.61,0.12) add=(176,111,23) over the blue felt, and the timeline
// carries the SAME cxform in a cid-less MOVE record that navigating back from
// the menu replays (the verified cause of the Back->yellow revert). Matching on
// the add term alone catches both. Non-matches by construction: menu blue
// add=(102,153,255) (B high), boot creams (B ~ 230), bear tints add=(255,-26,0)
// (G negative) / (41,20,0) (R low), fade ramps add=(x,x,x) gray.
static inline bool warm_tint_cxform(uint8_t* base, uint32_t rec) {
    const float ar = rd_f32(base, rec + 16), ag = rd_f32(base, rec + 24),
                ab = rd_f32(base, rec + 32);
    const float mr = rd_f32(base, rec + 12), mg = rd_f32(base, rec + 20),
                mb = rd_f32(base, rec + 28);
    // YELLOW-specific, not merely "warm": the day tint keeps GREEN close to RED
    // (add G/R = 111/176 = 0.63; mul G=0.61 >= R=0.50), whereas the scene-
    // transition RED overlay crushes green -- an earlier warm-only predicate
    // recolored that overlay blue (user-visible regression). Both rules below
    // require green ~ red.
    // Full day tint: strongly warm add with G >= 0.55*R.
    if (ar >= 150.f && ag >= 80.f && ab <= 80.f && (ar - ab) >= 100.f &&
        ag >= 0.55f * ar)
        return true;
    // Tween ramp frames (the yellow flash on menu->Back): blue MUL crushed while
    // green mul stays >= red mul (yellow keeps G; red overlays crush it). Still
    // excluded: yellow-BEAR tint mul=(.6,.6,.6) (equal channels -> mr-mb=0),
    // fades (gray), cool/negative tints.
    return (mr - mb) >= 0.10f && (mg - mb) >= 0.08f && mg >= mr - 0.02f &&
           ab <= ar && ab <= 80.f;
}

// Parse-time, name-scoped: recolor a record iff the movie being parsed is the
// StartMenu movie AND it is either a sky-cid placement or a warm-tint re-tint
// (the day-sky MOVE tags). Stateless -- the name is re-read per matching record
// (a handful of byte reads at a rare event), so stream-address reuse across
// movie loads can't confuse it.
static inline void force_sky_cxform(uint8_t* base, uint32_t rec, uint32_t stream) {
    if (!REXCVAR_GET(sky_recolor)) return;
    const int cid = (base[rec + 82] << 8) | base[rec + 83];
    bool warm = false;
    if (!is_sky_cid(cid)) {
        warm = warm_tint_cxform(base, rec);
        if (!warm) return;
    }
    if (!stream_is_startmenu(base, stream)) return;
    if (REXCVAR_GET(sky_recolor_debug))
        REXLOG_INFO("[sky] recolored {} cid={} mul=({:.2f},{:.2f},{:.2f}) add=({:.0f},{:.0f},{:.0f})",
                    warm ? "warm-tint" : "sky-cid", cid,
                    rd_f32(base, rec + 12), rd_f32(base, rec + 20), rd_f32(base, rec + 28),
                    rd_f32(base, rec + 16), rd_f32(base, rec + 24), rd_f32(base, rec + 32));
    // cid 14 is the pre-composited title ARTWORK BITMAP (blue sky baked in),
    // shown yellow only via a warm tint; strip the tint (identity) so the
    // artwork keeps its texture. Same for cid-less warm-tint records (they tint
    // that bitmap). Only the felt SHAPES (13, 15) take the solid menu blue.
    if (warm || cid == REXCVAR_GET(sky_cid_art))
        apply_sky_identity(base, rec);
    else
        apply_sky_blue(base, rec);
}

// --- ActionScript Color re-tints (the menu->Back yellow flash) ---------------
// The Back transition re-yellows the sky at RUNTIME via AS2 Color.setTransform
// (a scripted tween writes the sky's color per frame), which is invisible to
// the parse hooks. Both Color.setRGB (sub_82C88130) and Color.setTransform
// (sub_82C87B38) converge on writing 8 floats into the target character at +44
// ([mul,add] pairs for 3 color channels + alpha; R/B channel order unconfirmed,
// so both orientations are tested) followed by a dirty-notify virtual. Hooked
// right before that notify, after the write. Same yellow-only discriminators as
// the parse predicate, so red overlays / cool tints are untouched.
static inline void as_color_fixup(uint8_t* base, uint32_t chr) {
    if (!REXCVAR_GET(sky_recolor)) return;
    if (!ptr_ok(chr) || !host_readable(base, chr + 44) ||
        !host_readable(base, chr + 75))
        return;
    float f[8];
    for (int i = 0; i < 8; ++i) f[i] = rd_f32(base, chr + 44 + 4 * i);
    if (REXCVAR_GET(sky_recolor_debug)) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 400)
            REXLOG_INFO("[as] chr={:08X} cx=({:.2f},{:.0f})({:.2f},{:.0f})({:.2f},{:.0f})({:.2f},{:.0f})",
                        chr, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
    }
    auto yellow = [](float mr, float ar, float mg, float ag, float mb, float ab) {
        if (ar >= 150.f && ag >= 80.f && ab <= 80.f && (ar - ab) >= 100.f &&
            ag >= 0.55f * ar)
            return true;
        return (mr - mb) >= 0.10f && (mg - mb) >= 0.08f && mg >= mr - 0.02f &&
               ab <= ar && ab <= 80.f;
    };
    const bool ordA = yellow(f[0], f[1], f[2], f[3], f[4], f[5]);   // [R G B]
    const bool ordB = !ordA &&
                      yellow(f[4], f[5], f[2], f[3], f[0], f[1]);   // [B G R]
    if (!ordA && !ordB) return;
    // Neutralize to IDENTITY (order-independent): the tinted content is the
    // title artwork bitmap whose native sky is already blue, so stripping the
    // warm tint both fixes the color and keeps the texture. Alpha pair (f[6],
    // f[7]) untouched: the tween's fade stays intact.
    wr_f32(base, chr + 44, 1.f);   // c0 mul
    wr_f32(base, chr + 52, 1.f);   // c1 mul
    wr_f32(base, chr + 60, 1.f);   // c2 mul
    wr_f32(base, chr + 48, 0.f);   // c0 add
    wr_f32(base, chr + 56, 0.f);   // c1 add
    wr_f32(base, chr + 64, 0.f);   // c2 add
    if (REXCVAR_GET(sky_recolor_debug))
        REXLOG_INFO("[as] neutralized yellow AS re-tint (order {})", ordA ? "RGB" : "BGR");
}

// Midasm hooks at the tails of GASColor::setRGB (0x82C88274, char in r30) and
// GASColor::setTransform (0x82C87DF4, char in r26), after the cxform write and
// before the dirty-notify call.
void on_as_setrgb(PPCRegister& r30) {
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (base) as_color_fixup(base, r30.u32);
}
void on_as_settransform(PPCRegister& r26) {
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (base) as_color_fixup(base, r26.u32);
}

// Hook at the char-id store in the PlaceObject2/3 parser (0x82CD682C). r29 = the
// record, r31 = the parser stream (identifies which movie is being parsed);
// fires for EVERY placement (including the title's baked, cxform-less sky
// placement), so this is where the sky records are collected/recolored.
void on_gfx_place(PPCRegister& r29, PPCRegister& r31) {
    const uint32_t rec = r29.u32;
    if (!ptr_ok(rec)) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base || !host_readable(base, rec) || !host_readable(base, rec + 88)) return;

    if (REXCVAR_GET(sky_recolor_debug)) {
        const uint16_t cid = static_cast<uint16_t>((base[rec + 82] << 8) | base[rec + 83]);
        if (cid != 0) {
            static std::atomic<uint32_t> n{0};
            if (n.fetch_add(1, std::memory_order_relaxed) < 300)
                REXLOG_INFO("[pl] s={:08X} cid={}", r31.u32, cid);
        }
        // On each NEW stream, scan the loader context (stream-28) for pointers
        // to ASCII strings -- hunting the movie filename offset so the recolor
        // can identify the startmenu movie at parse START (streamed playback
        // places the sky before the late cid-245 trigger arrives).
        static uint32_t last_scanned = 0;
        const uint32_t stream = r31.u32;
        if (stream != last_scanned && stream >= 28u) {
            last_scanned = stream;
            const uint32_t ctx = stream - 28u;
            static std::atomic<uint32_t> scans{0};
            if (scans.fetch_add(1, std::memory_order_relaxed) < 12) {
                for (uint32_t off = 0; off < 0x300; off += 4) {
                    bool ok = false;
                    const uint32_t p = rd32_safe(base, ctx + off, ok);
                    if (!ok || !ptr_ok(p) || !host_readable(base, p) ||
                        !host_readable(base, p + 63))
                        continue;
                    char buf[64];
                    int len = 0;
                    for (; len < 63; ++len) {
                        const char c = static_cast<char>(base[p + len]);
                        if (c == 0) break;
                        if (c < 0x20 || c > 0x7e) { len = -1; break; }
                        buf[len] = c;
                    }
                    if (len >= 4 && len < 63) {
                        buf[len] = 0;
                        REXLOG_INFO("[nm] s={:08X} ctx+{:03X} -> \"{}\"", stream, off, buf);
                    }
                }
            }
        }
    }
    force_sky_cxform(base, rec, r31.u32);
}

// Hook after the PlaceObject2/3 cxform read (0x82CD6868). r29 = record, r31 =
// parser stream. Re-applies the sky override in case a supplied cxform
// overwrote the placement-time injection (tag layout: cxform read after cid).
void on_gfx_cxform(PPCRegister& r29, PPCRegister& r31) {
    const uint32_t rec = r29.u32;
    if (!ptr_ok(rec)) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base || !host_readable(base, rec) || !host_readable(base, rec + 88)) return;

    if (REXCVAR_GET(sky_recolor_debug)) {
        const uint16_t cid = static_cast<uint16_t>((base[rec + 82] << 8) | base[rec + 83]);
        float cx[8];
        for (int i = 0; i < 8; ++i) cx[i] = rd_f32(base, rec + 12 + 4 * i);
        const bool ident = cx[0] == 1.f && cx[2] == 1.f && cx[4] == 1.f &&
                           cx[1] == 0.f && cx[3] == 0.f && cx[5] == 0.f;
        if (is_sky_cid(cid) || !ident) {
            static std::atomic<uint32_t> n{0};
            if (n.fetch_add(1, std::memory_order_relaxed) < 220)
                REXLOG_INFO("[cx] s={:08X} cid={} mul=({:.2f},{:.2f},{:.2f}) add=({:.0f},{:.0f},{:.0f}){}",
                            r31.u32, cid, cx[0], cx[2], cx[4], cx[1], cx[3], cx[5],
                            is_sky_cid(cid) ? " <-SKY" : "");
        }
    }
    force_sky_cxform(base, rec, r31.u32);
}

// Midasm hook at GFxPlaceObject2::Execute entry (sub_82CD1538). r3 = the tag.
// LOG-ONLY: rewriting tags here by depth hijacked unrelated per-frame fade
// animations (depth values are reused by every movie), so the recolor happens
// exclusively at parse (stream-scoped, above). Kept for diagnosis: shows which
// sky tags replay at runtime (e.g. on menu->Back).
void on_gfx_execute(PPCRegister& r3) {
    if (!REXCVAR_GET(sky_recolor_debug)) return;
    const uint32_t tag = r3.u32;
    if (!ptr_ok(tag)) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base || !host_readable(base, tag) || !host_readable(base, tag + 96)) return;

    const uint32_t mode = rd32(base, tag + 92);
    const int      cid  = (base[tag + 82] << 8) | base[tag + 83];
    if ((mode != 1u && is_sky_cid(cid)) || warm_tint_cxform(base, tag)) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 200)
            REXLOG_INFO("[ex] m={} cid={} d={} add=({:.0f},{:.0f},{:.0f})",
                        mode, cid, rd32(base, tag + 76), rd_f32(base, tag + 16),
                        rd_f32(base, tag + 24), rd_f32(base, tag + 32));
    }
}

// ---------------------------------------------------------------------------
// Native score row in the objective panel
// ---------------------------------------------------------------------------
// The in-level objective panel is driven by hazingHud::ObjectiveMonitorManager
// through four GFx invokes on the NBPlayerHud movie (manager singleton at
// 0x8332F5E0, lazily created by sub_82707C70):
//   sub_82706150(hud)             -> Invoke("ClearObjectiveBox")
//   sub_827060B8(hud, text)       -> Invoke("AddObjective", "%s", text)
//   sub_82706168(hud, idx, text)  -> Invoke("SetObjectiveText", "%f %s", ...)
//   sub_827060D0(hud, idx, st, b) -> Invoke("SetObjectiveState", "%f %f %f",...)
// The campaign refresh (sub_8271A038, run every manager tick) rebuilds the box
// as Clear + one AddObjective per revealed objective, then updates rows
// POSITIONALLY -- every game update targets an index below the game's own row
// count. So a row we append at the END can only be disturbed by the next
// Clear, which our on_objbox_clear hook observes; on_obj_refresh_end then
// re-appends the row right after the rebuild completes, on the same thread,
// so game rows and our row can never interleave. (Multiplayer modes build the
// box elsewhere -- sub_8271A038 never runs there, so we never inject.)
//
// The invoke wrapper self-guards (returns without invoking unless the movie at
// hud+20 is loaded and the ready byte at hud+3924 is set), and it formats the
// "%s" into the AS call synchronously, so one reusable guest string buffer is
// safe to overwrite between calls.

namespace {

constexpr uint32_t kHudMgrGlobal    = 0x8332F5E0u;  // NBPlayerHud manager singleton
constexpr uint32_t kObjPanelFlag    = 0x8332FB50u;  // SetShowObjectivePanel byte
constexpr uint32_t kFnAddObjective  = 0x827060B8u;  // (hud, text)
constexpr uint32_t kFnSetObjText    = 0x82706168u;  // (hud, idx, text)
constexpr uint32_t kFnSetObjState   = 0x827060D0u;  // (hud, idx, state, animate)
                                                    // state: 0=active 1=done 2=failed

// Row bookkeeping. rows = rows the GAME added since the last clear; score_row =
// our appended row's index (-1 = not present). self flags our own wrapper
// calls so on_objbox_add doesn't count them.
std::atomic<int>  g_objbox_rows{0};
std::atomic<int>  g_score_row{-1};
std::atomic<bool> g_objbox_self{false};
std::atomic<bool> g_obj_in_refresh{false};
std::atomic<int>  g_score_row_state{0};   // last SetObjectiveState we pushed
std::atomic<uint32_t> g_obj_mgr{0};       // ObjectiveMonitorManager (refresh r3)

uint32_t g_score_text_guest = 0;        // reusable guest string buffer
char     g_score_text_last[80] = {0};   // last text pushed (skip no-op updates)

// NBPlayerHud manager, or 0 if it doesn't exist / isn't ready for invokes.
// Mirrors the guard the game's own update uses (movie ptr + ready byte).
uint32_t objbox_hud(uint8_t* base) {
    bool ok = false;
    const uint32_t hud = rd32_safe(base, kHudMgrGlobal, ok);
    if (!ok || !ptr_ok(hud)) return 0;
    const uint32_t movie = rd32_safe(base, hud + 20, ok);
    if (!ok || !ptr_ok(movie)) return 0;
    if (!host_readable(base, hud + 3924) || !base[hud + 3924]) return 0;
    return hud;
}

// Compose the row text. False = nothing to show (not in a level). *complete is
// set when every known medal threshold is beaten (drives the row's checkmark).
// Plain ASCII only -- the felt HUD font's glyph coverage is unverified, so no
// thousands separators or fancy punctuation.
bool build_score_row_text(char* out, size_t n, bool* complete) {
    *complete = false;
    const int score = g_level_score.load(std::memory_order_relaxed);
    if (score < 0) return false;
    static const char* kMedals[4] = {"Bronze", "Silver", "Gold", "Platinum"};
    int next = -1, target = 0;
    for (int i = 0; i < 4; ++i) {
        const int t = g_trophy_auto[i].load(std::memory_order_relaxed);
        if (t > 0 && score < t) { next = i; target = t; break; }
    }
    if (next >= 0)
        std::snprintf(out, n, "Score %d - %s in %d", score, kMedals[next],
                      target - score);
    else if (g_trophy_auto[3].load(std::memory_order_relaxed) > 0) {
        std::snprintf(out, n, "Score %d - Platinum!", score);
        *complete = true;
    } else
        std::snprintf(out, n, "Score %d", score);
    return true;
}

// Push the score text into the box: append a new row (append=true) or rewrite
// our existing row. No-ops safely when the HUD/panel isn't up or the text is
// unchanged.
void objbox_push_score(bool append) {
    if (!REXCVAR_GET(score_objective)) return;
    auto* mem = rex::system::kernel_memory();
    auto* fd  = rex::Runtime::instance()->function_dispatcher();
    if (!mem || !fd) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;
    if (!base[kObjPanelFlag]) return;  // panel disabled by script
    const uint32_t hud = objbox_hud(base);
    if (!hud) return;

    char text[80];
    bool complete = false;
    if (!build_score_row_text(text, sizeof(text), &complete)) return;
    const bool text_same = !append && std::strcmp(text, g_score_text_last) == 0;
    const int  want_state = complete ? 1 : 0;
    if (text_same && want_state == g_score_row_state.load(std::memory_order_relaxed))
        return;

    if (!g_score_text_guest)
        g_score_text_guest = mem->SystemHeapAlloc(sizeof(text), 0x20);
    if (!g_score_text_guest) return;
    std::memcpy(mem->TranslateVirtual<char*>(g_score_text_guest), text,
                std::strlen(text) + 1);

    int idx;
    if (append) {
        auto* fn = fd->GetFunction(kFnAddObjective);
        if (!fn) return;
        idx = g_objbox_rows.load(std::memory_order_relaxed);
        g_objbox_self.store(true, std::memory_order_relaxed);
        rex::ppc::GuestToHostFunction<void>(*fn, hud, g_score_text_guest);
        g_objbox_self.store(false, std::memory_order_relaxed);
        g_score_row.store(idx, std::memory_order_relaxed);
        g_score_row_state.store(0, std::memory_order_relaxed);  // fresh row
        if (REXCVAR_GET(score_objective_debug))
            REXLOG_INFO("[objbox] append row {} '{}'", idx, text);
    } else {
        idx = g_score_row.load(std::memory_order_relaxed);
        if (idx < 0) return;
        if (!text_same) {
            auto* fn = fd->GetFunction(kFnSetObjText);
            if (!fn) return;
            g_objbox_self.store(true, std::memory_order_relaxed);
            rex::ppc::GuestToHostFunction<void>(*fn, hud,
                                                static_cast<uint32_t>(idx),
                                                g_score_text_guest);
            g_objbox_self.store(false, std::memory_order_relaxed);
        }
    }
    std::memcpy(g_score_text_last, text, std::strlen(text) + 1);

    // Checkmark: when every medal is beaten, complete the row exactly like the
    // game completes its own (SetObjectiveState 1, animated). Fresh rows start
    // active in the Flash movie, so this re-applies after every re-append. The
    // reverse transition covers the score-cheat reset while in the level.
    if (want_state != g_score_row_state.load(std::memory_order_relaxed)) {
        if (auto* fn = fd->GetFunction(kFnSetObjState)) {
            g_objbox_self.store(true, std::memory_order_relaxed);
            rex::ppc::GuestToHostFunction<void>(
                *fn, hud, static_cast<uint32_t>(idx),
                static_cast<uint32_t>(want_state),
                static_cast<uint32_t>(want_state == 1 ? 1 : 0));
            g_objbox_self.store(false, std::memory_order_relaxed);
            g_score_row_state.store(want_state, std::memory_order_relaxed);
            if (REXCVAR_GET(score_objective_debug))
                REXLOG_INFO("[objbox] row {} state -> {}", idx, want_state);
        }
    }
}

}  // namespace

// Runtime toggle for the native row (F10 double-tap in trophy_overlay.h; also
// editable in the F4 settings menu since it's a plain cvar).
bool get_score_objective()       { return REXCVAR_GET(score_objective); }
void set_score_objective(bool v) { REXCVAR_SET(score_objective, v); }

// Per-frame (on_swap): live-update our row's text. Appends happen exclusively
// in on_obj_refresh_end (game thread, right after the rebuild), so this only
// rewrites an existing row -- and never during a refresh.
static void update_score_objective() {
    static int tick = 0;
    if (tick++ % 15 != 0) return;  // ~4 Hz; the row is a readout, not a counter
    if (g_score_row.load(std::memory_order_relaxed) < 0) return;
    if (g_obj_in_refresh.load(std::memory_order_relaxed)) return;

    if (!REXCVAR_GET(score_objective)) {
        // Toggled off with a live row. The box has no per-row remove (blanking
        // the text leaves the row's icon behind), so set the manager's
        // rows-dirty byte (mgr+28) -- the same flag its own objective changes
        // set -- and the next refresh does Clear + re-add of just the game's
        // rows. Our clear hook then resets the bookkeeping, and with the cvar
        // off the refresh tail won't re-append.
        auto* mem = rex::system::kernel_memory();
        if (!mem) return;
        uint8_t* base = mem->virtual_membase();
        const uint32_t mgr = g_obj_mgr.load(std::memory_order_relaxed);
        if (!base || !ptr_ok(mgr) || !host_readable(base, mgr + 28)) return;
        base[mgr + 28] = 1;
        if (REXCVAR_GET(score_objective_debug))
            REXLOG_INFO("[objbox] toggle off -> forcing panel rebuild");
        return;
    }
    objbox_push_score(/*append=*/false);
}

// Midasm hook at the ClearObjectiveBox wrapper entry (0x82706150). The box is
// now empty: forget our row and the game-row count.
void on_objbox_clear() {
    g_objbox_rows.store(0, std::memory_order_relaxed);
    g_score_row.store(-1, std::memory_order_relaxed);
    g_score_row_state.store(0, std::memory_order_relaxed);
    g_score_text_last[0] = '\0';
    if (REXCVAR_GET(score_objective_debug)) REXLOG_INFO("[objbox] clear");
}

// Midasm hook at the AddObjective wrapper entry (0x827060B8). r3 = hud,
// r4 = row text. Counts game rows; our own appends are flagged and skipped.
// Campaign adds are always preceded by a Clear in the same refresh, so they
// can't land after our row -- but if a script ever adds one directly, swap:
// rewrite OUR row to the incoming game text and let this call push the score
// text instead, keeping ours last and the game's positional indices intact.
void on_objbox_add(PPCRegister& r3, PPCRegister& r4) {
    if (g_objbox_self.load(std::memory_order_relaxed)) return;
    const int rows = g_objbox_rows.fetch_add(1, std::memory_order_relaxed);
    const int ours = g_score_row.load(std::memory_order_relaxed);
    if (ours < 0) return;

    if (REXCVAR_GET(score_objective_debug))
        REXLOG_INFO("[objbox] out-of-band add with score row at {} (rows={})",
                    ours, rows);
    auto* fd = rex::Runtime::instance()->function_dispatcher();
    auto* fn = fd ? fd->GetFunction(kFnSetObjText) : nullptr;
    if (!fn || !g_score_text_guest || !g_score_text_last[0]) {
        // Can't swap -- drop our bookkeeping; the box self-heals at next Clear.
        g_score_row.store(-1, std::memory_order_relaxed);
        g_score_text_last[0] = '\0';
        return;
    }
    g_objbox_self.store(true, std::memory_order_relaxed);
    rex::ppc::GuestToHostFunction<void>(*fn, r3.u32,
                                        static_cast<uint32_t>(ours), r4.u32);
    g_objbox_self.store(false, std::memory_order_relaxed);
    r4.u32 = g_score_text_guest;               // this add now appends OUR text
    g_score_row.store(rows + 1, std::memory_order_relaxed);
    g_score_row_state.store(0, std::memory_order_relaxed);  // lands on a fresh row
}

// Midasm hooks at the campaign refresh (sub_8271A038) entry and common tail.
// The tail is the one safe spot to (re)append: the box is fully rebuilt, and
// we're on the same thread as the builder, so nothing can interleave.
void on_obj_refresh_begin(PPCRegister& r3) {
    if (ptr_ok(r3.u32)) g_obj_mgr.store(r3.u32, std::memory_order_relaxed);
    g_obj_in_refresh.store(true, std::memory_order_relaxed);
}

void on_obj_refresh_end() {
    g_obj_in_refresh.store(false, std::memory_order_relaxed);
    if (g_score_row.load(std::memory_order_relaxed) >= 0) return;  // still there
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base || !resolve_player_damageable(base)) return;  // not in gameplay
    objbox_push_score(/*append=*/true);
}

// ---------------------------------------------------------------------------
// Bear-action PIP cutaway toggle
// ---------------------------------------------------------------------------
// When a bear starts a notable action ("X IS TRYING TO ESCAPE", the self-oof,
// calling for help, ...) the level scripts call
// hazingCamera::PIPManager::SendEvent(mgr, bear, "event") -- native
// sub_8285D240 -- which queues a PIPEvent. PIPManager::Update (sub_8285DA30)
// then activates it: shows the HUD PIP window (banner + inset frame) and
// renders the bear into the inset (the live "PIP camera").
//   bearcam 0: SendEvent is swallowed before anything is queued -- no banner,
//              no frame, no camera (skips the extra inset render entirely).
//   bearcam 1: vanilla (frame + banner + live camera), with the pronoun rewrite
//              applied to the banner (on_pip_string).
// Cutscene PIPs (HazingCutsceneManager) and the level-scripted "pipevents_"
// player events (sub_82447DF0) use different submission paths and are
// deliberately untouched.
//   PIPManager singleton    = *(0x83326818).
//   CancelCurrentEvent(mgr) = sub_82854E50 -- kills a running cutaway when the
//     feature is turned off.

REXCVAR_DEFINE_INT32(bearcam, 1, "Gameplay",
    "Bear-action cutaway: 0=off entirely (no banner, no frame, no PIP camera), "
    "1=native.")
    .range(0, 1);
REXCVAR_DEFINE_BOOL(bearcam_debug, false, "Gameplay",
    "Log every PIPManager::SendEvent (event name + bear + verdict).");
// Per-bear pronouns for the native banner, keyed by the bear's script name
// (see bearcam_debug logs for the exact names). Unlisted bears default to he.
REXCVAR_DEFINE_STRING(bearcam_pronouns, "", "Gameplay",
    "Bear pronouns for the native banner: 'cuddles=she,giggles=they'. "
    "Values: he, she, they, it.");

namespace {

constexpr uint32_t kPipMgrGlobal    = 0x83326818u;  // hazingCamera::PIPManager*
constexpr uint32_t kFnPipCancelCur  = 0x82854E50u;  // CancelCurrentEvent(mgr)
constexpr uint32_t kFnNameToString  = 0x829585E8u;  // hashedname -> cstring(buf,n)

// Last SetPIPNPCName display name ("CUDDLES"), the pronoun-match fallback for
// banner lines that don't repeat the bear's name. Guarded by g_bearcam_mx.
std::mutex g_bearcam_mx;
char g_pip_npc[48] = {0};

// Page-safe copy of a guest NUL-terminated string (readability checked at
// the start and at every 4K page crossing).
void read_guest_cstr(uint8_t* base, uint32_t addr, char* out, size_t n) {
    out[0] = '\0';
    if (n < 2 || addr < 0x10000u || addr >= 0xC0000000u) return;
    size_t o = 0;
    for (; o + 1 < n; ++o) {
        const uint32_t a = addr + static_cast<uint32_t>(o);
        if ((o == 0 || (a & 0xFFFu) == 0) && !host_readable(base, a)) break;
        const char c = static_cast<char>(base[a]);
        if (!c) break;
        out[o] = c;
    }
    out[o] = '\0';
}

// Pronoun class for a bear from the bearcam_pronouns map ("name=she,...").
// 'h' (default), 's'he, 't'hey, 'i't. When `text` is set the entry keys are
// matched as case-insensitive substrings of it (used for the native banner,
// where only the finished line is available); otherwise `key` must equal the
// entry key exactly.
char bearcam_pronoun_class(const char* key, const char* text) {
    const std::string map = REXCVAR_GET(bearcam_pronouns);
    size_t pos = 0;
    while (pos < map.size()) {
        while (pos < map.size() && (map[pos] == ',' || map[pos] == ' ')) ++pos;
        const size_t eq = map.find('=', pos);
        if (eq == std::string::npos) break;
        size_t end = map.find(',', eq);
        if (end == std::string::npos) end = map.size();

        char entry[48];
        size_t elen = 0;
        for (size_t i = pos; i < eq && elen + 1 < sizeof(entry); ++i)
            entry[elen++] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(map[i])));
        entry[elen] = '\0';

        bool match = false;
        if (elen) {
            if (text) {
                // Case-insensitive substring search of entry in text.
                const size_t tlen = std::strlen(text);
                for (size_t s = 0; !match && s + elen <= tlen; ++s) {
                    match = true;
                    for (size_t i = 0; i < elen; ++i) {
                        if (std::tolower(static_cast<unsigned char>(
                                text[s + i])) != entry[i]) {
                            match = false;
                            break;
                        }
                    }
                }
            } else if (key) {
                match = std::strcmp(entry, key) == 0;
            }
        }
        if (match) {
            const char v = (eq + 1 < map.size())
                ? static_cast<char>(
                      std::tolower(static_cast<unsigned char>(map[eq + 1])))
                : 'h';
            return (v == 's' || v == 't' || v == 'i') ? v : 'h';
        }
        pos = end + 1;
    }
    return 'h';
}

}  // namespace

// Strong override of PIPManager::SendEvent (sub_8285D240): r3 = manager,
// r4 = subject bear (gameObject::ModeledObject), r5 = event name cstring.
// Mode 0 swallows the call (nothing is queued: no frame, no camera); mode 1
// passes straight through. For the debug log the bear's script name is resolved
// the same way GetObjectHashedName does: hash = subject->vtbl[+20](subject),
// then the name registry stringifies it (sub_829585E8).
REX_EXTERN(__imp__sub_8285D240);
REX_HOOK_RAW(sub_8285D240) {
    const int mode = REXCVAR_GET(bearcam);
    const bool debug = REXCVAR_GET(bearcam_debug);

    if (debug) {
        char name[64];
        read_guest_cstr(base, ctx.r5.u32, name, sizeof(name));
        char bear[48] = {0};
        auto* mem = rex::system::kernel_memory();
        auto* fd  = rex::Runtime::instance()->function_dispatcher();
        const uint32_t obj = ctx.r4.u32;
        bool ok = false;
        const uint32_t vt = (mem && fd) ? rd32_safe(base, obj, ok) : 0;
        const uint32_t fn_name =
            (ok && ptr_ok(vt)) ? rd32_safe(base, vt + 20, ok) : 0;
        if (ok && fn_name) {
            if (auto* fget = fd->GetFunction(fn_name)) {
                const uint32_t hash =
                    rex::ppc::GuestToHostFunction<uint32_t>(*fget, obj);
                static uint32_t scratch = 0;
                if (hash && !scratch)
                    scratch = mem->SystemHeapAlloc(64, 0x20);
                if (hash && scratch) {
                    if (auto* fstr = fd->GetFunction(kFnNameToString)) {
                        char* host = mem->TranslateVirtual<char*>(scratch);
                        host[0] = '\0';
                        rex::ppc::GuestToHostFunction<void>(
                            *fstr, hash, scratch, 47u);
                        for (size_t i = 0; i < sizeof(bear) - 1; ++i) {
                            const char c = host[i];
                            if (!c) break;
                            bear[i] = (c >= 0x20 && c <= 0x7e) ? c : '?';
                        }
                    }
                }
            }
        }
        REXLOG_INFO("[bearcam] SendEvent '{}' bear='{}' ({:08X}) -> {}", name,
                    bear, ctx.r4.u32, mode == 0 ? "blocked" : "native");
    }

    if (mode != 0)  // native lets the event run; mode 0 swallows it
        __imp__sub_8285D240(ctx, base);
}

// Midasm hook at the SetPIPString invoke wrapper entry (0x827068D0): r3 = hud,
// r4 = the finished NATIVE banner line ("CUDDLES IS OOFING HIMSELF"). When a
// bear from the bearcam_pronouns map is named in the line, masculine pronoun
// words are rewritten (word-boundary, longest-first: HIMSELF/HIS/HIM/HE) and
// r4 is redirected to a guest scratch copy. The invoke formats "%s"
// synchronously, so one reusable scratch buffer is safe.
void on_pip_string(PPCRegister& r3, PPCRegister& r4) {
    (void)r3;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    char text[192];
    read_guest_cstr(base, r4.u32, text, sizeof(text));
    if (!text[0]) return;

    char cls = bearcam_pronoun_class(nullptr, text);
    if (cls == 'h') {
        // Line may not repeat the bear's name -- fall back to the PIP window's
        // last displayed NPC name.
        char npc[sizeof(g_pip_npc)];
        {
            std::lock_guard<std::mutex> lock(g_bearcam_mx);
            std::memcpy(npc, g_pip_npc, sizeof(npc));
        }
        if (npc[0]) cls = bearcam_pronoun_class(nullptr, npc);
    }
    if (cls == 'h') return;  // default: leave the native line untouched

    struct Sub { const char* from; const char* she; const char* they; const char* it; };
    static const Sub kSubs[] = {
        {"HIMSELF", "HERSELF", "THEMSELF", "ITSELF"},
        {"HIS",     "HER",     "THEIR",    "ITS"},
        {"HIM",     "HER",     "THEM",     "IT"},
        {"HE",      "SHE",     "THEY",     "IT"},
    };

    char out[256];
    size_t o = 0;
    const size_t tlen = std::strlen(text);
    size_t i = 0;
    bool changed = false;
    while (i < tlen && o + 1 < sizeof(out)) {
        bool replaced = false;
        const bool at_boundary =
            i == 0 || !std::isalpha(static_cast<unsigned char>(text[i - 1]));
        if (at_boundary) {
            for (const Sub& s : kSubs) {
                const size_t flen = std::strlen(s.from);
                if (i + flen > tlen) continue;
                bool m = true;
                for (size_t k = 0; k < flen; ++k)
                    if (std::toupper(static_cast<unsigned char>(text[i + k])) !=
                        s.from[k]) { m = false; break; }
                if (!m) continue;
                if (i + flen < tlen &&
                    std::isalpha(static_cast<unsigned char>(text[i + flen])))
                    continue;  // inside a longer word
                const char* rep = (cls == 's') ? s.she
                                : (cls == 't') ? s.they
                                               : s.it;
                for (const char* p = rep; *p && o + 1 < sizeof(out); ++p)
                    out[o++] = *p;
                i += flen;
                replaced = true;
                changed = true;
                break;
            }
        }
        if (!replaced) out[o++] = text[i++];
    }
    out[o] = '\0';
    if (!changed) return;

    static uint32_t scratch = 0;
    if (!scratch) scratch = mem->SystemHeapAlloc(sizeof(out), 0x20);
    if (!scratch) return;
    std::memcpy(mem->TranslateVirtual<char*>(scratch), out, o + 1);
    r4.u32 = scratch;
    if (REXCVAR_GET(bearcam_debug))
        REXLOG_INFO("[bearcam] banner rewrite: '{}' -> '{}'", text, out);
}

// Midasm hook at the SetPIPNPCName invoke wrapper entry (0x82706A60): r4 =
// the PIP window's NPC display name. Kept as the pronoun-match fallback.
void on_pip_npcname(PPCRegister& r4) {
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;
    char npc[sizeof(g_pip_npc)];
    read_guest_cstr(base, r4.u32, npc, sizeof(npc));
    if (!npc[0]) return;
    std::lock_guard<std::mutex> lock(g_bearcam_mx);
    std::memcpy(g_pip_npc, npc, sizeof(npc));
}

// Per-frame (on_swap). Three jobs:
// Per-frame (on_swap): when the feature is turned fully OFF (-> mode 0), cancel
// any cutaway that is still up. Repeated for ~1.5s so a just-activated event
// gets killed as it becomes current (new events can't arrive -- the override
// swallows them in mode 0).
static void update_bearcam() {
    static int prev_mode = 1;
    static int drain = 0;

    const int mode = REXCVAR_GET(bearcam);
    if (mode != prev_mode) {
        if (mode == 0 && prev_mode != 0) drain = 90;  // turned off: drain a cutaway
        prev_mode = mode;
    }
    if (drain <= 0) return;

    auto* mem = rex::system::kernel_memory();
    auto* fd  = rex::Runtime::instance()->function_dispatcher();
    if (!mem || !fd) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    bool ok = false;
    const uint32_t pmgr = rd32_safe(base, kPipMgrGlobal, ok);
    if (!ok || !ptr_ok(pmgr)) { drain = 0; return; }

    --drain;
    if (auto* fn = fd->GetFunction(kFnPipCancelCur))
        rex::ppc::GuestToHostFunction<void>(*fn, pmgr);
}

// ---------------------------------------------------------------------------
// Free camera
// ---------------------------------------------------------------------------
// hg::Camera (the guest render-side camera object) layout, from disasm of its
// SWIG wrappers (GetCameraMatrix returns this+0x10, GetViewMatrix returns
// this+0x240, UpdateViewMatrix's AltiVec copies confirm the sizes):
//   +0x10  camera-to-world matrix, 4x4 float, row-major D3D convention
//          assumed (row0=right, row1=up, row2=forward, row3=position, each
//          row one 16-byte float4) -- UNVERIFIED, tune/flip if the view
//          looks rotated/mirrored once tested in-game.
//   +0x50  projection matrix (left untouched -- read back and passed through).
//   +0x240 derived view matrix, recomputed by UpdateViewMatrix; never written
//          directly.
// hg::Camera::GetCurrentCamera() = sub_82AEAF90 (0 args, returns the live
// camera pointer directly -- no manifest hook needed, called like ApplyDamage
// elsewhere in this file via FunctionDispatcher).
// hg::Camera::UpdateViewMatrix(this, camMatrix*, projMatrix*) = sub_82AEF310:
// copies both 4x4s in via AltiVec vector loads/stores and recomputes the view
// matrix cache -- this is the one call that makes the renderer actually use
// a new camera position/orientation.
//
// hazingCamera::MainCameraParameters::smCameraFollowAllowed is a plain global
// bool (byte_833381B2, confirmed via its SWIG setter: a one-line
// `byte_833381B2 = value`) that gates the normal follow-camera update.
// Forced to 0 while freecam is active so the follow system doesn't
// immediately re-clobber our matrix every frame; restored to 1 on exit.
//
// Movement/look poll raw Win32 state directly (GetAsyncKeyState + cursor
// recenter) -- the same primitives the native mouse+keyboard feature's
// on_kb_update/on_mouse_update hooks use for their own purposes (see
// [[native-mkb-feature]] in memory) -- entirely independent of mkb_enable/
// GameInput bindings, so this works regardless of that feature's state.

namespace {

constexpr uint32_t kFnGetCurrentCamera  = 0x82AEAF90u;
constexpr uint32_t kCameraFollowAllowed = 0x833381B2u;  // plain global bool

struct FreecamState {
    bool active = false;
    bool seeded = false;
    bool cursor_captured = false;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f, pitch = 0.f;  // radians
    POINT center{};
};
FreecamState g_freecam;

// Builds a camera-to-world matrix from g_freecam's position/yaw/pitch and
// writes it into a 64-byte guest scratch buffer (16 big-endian floats).
void freecam_write_matrix(uint8_t* base, uint32_t guest_buf) {
    const float cy = std::cos(g_freecam.yaw),   sy = std::sin(g_freecam.yaw);
    const float cp = std::cos(g_freecam.pitch), sp = std::sin(g_freecam.pitch);
    // Forward = yaw around Y then pitch around local X; right = world-Y x
    // forward (keeps roll at zero); up = forward x right (NOT right x
    // forward -- that pointed straight down, confirmed in-game: with
    // yaw=pitch=0, forward=(0,0,1), right=(1,0,0), right x forward =
    // (0,-1,0). Swapped operand order to get the correct (0,1,0).
    const float fx = sy * cp, fy = sp, fz = cy * cp;
    const float rx = cy, ry = 0.f, rz = -sy;
    const float ux = fy * rz - fz * ry;
    const float uy = fz * rx - fx * rz;
    const float uz = fx * ry - fy * rx;

    const float rows[4][4] = {
        {rx, ry, rz, 0.f},
        {ux, uy, uz, 0.f},
        {fx, fy, fz, 0.f},
        {g_freecam.x, g_freecam.y, g_freecam.z, 1.f},
    };
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            wr_f32(base, guest_buf + r * 16 + c * 4, rows[r][c]);
}

}  // namespace

// F6 keybind: flip the cvar and immediately (re)seed / release the cursor
// capture + guest input suppression. Registered once for the app's lifetime
// (no owning dialog), mirroring the one-time static-init pattern used for
// the Windows timer resolution near the top of this file.
void toggle_freecam() {
    const bool now = !REXCVAR_GET(freecam);
    REXCVAR_SET(freecam, now);
    g_freecam.seeded = false;
    set_guest_input_suppressed(now);
    REXLOG_INFO("[freecam] {}", now ? "on" : "off");
}

static const bool s_freecam_bind_registered = []() {
    rex::ui::RegisterBind("bind_freecam", "F6", "Toggle free camera",
                          [] { toggle_freecam(); });
    return true;
}();

// The real per-frame camera-follow entry (found by tracing UpdateViewMatrix's
// non-SWIG callers): sets a dirty-flag bit on the camera (*a1 |= 0x40000000),
// updates the dword_83346DCC "current camera" fallback global to point at
// it, runs frustum/visibility bookkeeping (sub_828BDC80, sub_82B986B0), and
// THEN calls hg::Camera::UpdateViewMatrix(a1, a2, a3) -- entirely
// UNCONDITIONAL on smCameraFollowAllowed (that flag does not gate this at
// all, despite the name). This runs every frame and immediately re-clobbers
// whatever a naive on_swap-time write does, which is why forcing
// smCameraFollowAllowed alone did nothing.
//
// First attempt swallowed this whole function while freecam was active --
// that crashed elsewhere in the engine (an access violation several frames
// later, deep in unrelated rendering code), almost certainly because
// skipping it also skipped the frustum/visibility bookkeeping that other
// systems depend on running every frame, not just the camera matrix push.
//
// Fix: don't swallow anything. Hook the ENTRY (r4 = a2 = pointer to the new
// camera matrix the follow system just computed for this frame) and, while
// freecam is active, substitute r4 with our own scratch buffer holding the
// free-cam's matrix. The rest of the function runs completely unmodified --
// dirty flag, dword_83346DCC, frustum calls (which use a1/a3, untouched) --
// only the UpdateViewMatrix call at the end ends up pushing OUR matrix
// instead of the follow system's, through the exact same code path and
// timing the game itself uses, so no side effect is skipped and no
// ordering assumption about on_swap vs. this update is needed.
void on_camera_update(PPCRegister& r4) {
    if (!REXCVAR_GET(freecam) || !g_freecam.seeded) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;
    static uint32_t scratch = 0;
    if (!scratch) scratch = mem->SystemHeapAlloc(64, 0x20);
    if (!scratch) return;
    freecam_write_matrix(base, scratch);
    r4.u32 = scratch;
}

// Per-frame (on_swap): handles the on/off transition (seed position from the
// live camera, flip smCameraFollowAllowed -- kept mostly as a secondary
// hint, see note above), click-and-drag mouse look (RMB held), and WASD
// movement (active any time freecam is on, independent of RMB) into
// g_freecam's position/yaw/pitch. The actual push into the renderer happens
// in on_camera_update above, whenever the guest's own per-frame camera
// update fires -- not here.
static void update_freecam() {
    // freecam is a transient dev toggle, not a setting. The SDK persists
    // non-default cvars back to restuff.toml, so quitting while it was on
    // wrote `freecam = true` and the NEXT launch silently re-entered free
    // camera the instant a level finished loading -- no keypress involved
    // (observed: transition active=true 0.6s after startup, with no
    // preceding toggle_freecam log line). Always start detached; F6 opts in.
    static bool s_startup_cleared = false;
    if (!s_startup_cleared) {
        s_startup_cleared = true;
        if (REXCVAR_GET(freecam)) {
            REXCVAR_SET(freecam, false);
            REXLOG_INFO("[freecam] ignoring persisted freecam=true at startup");
        }
        g_freecam.active = false;
        g_freecam.seeded = false;
        return;
    }

    const bool want = REXCVAR_GET(freecam);
    if (want != g_freecam.active) {
        auto* mem = rex::system::kernel_memory();
        uint8_t* base = mem ? mem->virtual_membase() : nullptr;
        if (base) base[kCameraFollowAllowed] = want ? 0 : 1;

        if (want) {
            // Seed position from the live camera's assumed translation row
            // (row3: cam+0x10+48..+60) so there's no visual pop on toggle.
            // Yaw/pitch seed to 0 rather than inverting the rotation blind.
            auto* fd = rex::Runtime::instance()->function_dispatcher();
            if (fd && base) {
                if (auto* fn_get = fd->GetFunction(kFnGetCurrentCamera)) {
                    const uint32_t cam = rex::ppc::GuestToHostFunction<uint32_t>(*fn_get);
                    if (ptr_ok(cam)) {
                        g_freecam.x = rd_f32(base, cam + 0x10 + 48 + 0);
                        g_freecam.y = rd_f32(base, cam + 0x10 + 48 + 4);
                        g_freecam.z = rd_f32(base, cam + 0x10 + 48 + 8);
                    }
                }
            }
            g_freecam.yaw = 0.f;
            g_freecam.pitch = 0.f;
            g_freecam.seeded = true;
        } else if (g_freecam.cursor_captured) {
            // Toggled off mid-drag: release the cursor defensively.
#ifdef _WIN32
            ClipCursor(nullptr);
            ShowCursor(TRUE);
#endif
            g_freecam.cursor_captured = false;
        }
        g_freecam.active = want;
        REXLOG_INFO("[freecam] transition: active={}", want);
    }
    if (!want) return;

    // The look/movement poll below reads raw Win32 keyboard+cursor state.
    // This tree also builds for Linux, where those entry points don't exist,
    // so freecam compiles there but stays inert (toggle + camera push still
    // work; only host input is absent). Port to SDL input to enable it.
#ifdef _WIN32
    // Click-and-drag mouse look: only while the right mouse button is held.
    // Cursor is hidden + recentered every frame during the drag (so deltas
    // don't run out of screen space), then released back to normal on RMB up.
    const bool rmb_down = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    if (rmb_down && !g_freecam.cursor_captured) {
        HWND fg = GetForegroundWindow();
        RECT rc;
        if (fg && GetClientRect(fg, &rc)) {
            POINT tl{rc.left, rc.top};
            ClientToScreen(fg, &tl);
            g_freecam.center = {tl.x + (rc.right - rc.left) / 2,
                                tl.y + (rc.bottom - rc.top) / 2};
            SetCursorPos(g_freecam.center.x, g_freecam.center.y);
            ShowCursor(FALSE);
            g_freecam.cursor_captured = true;
        }
    } else if (!rmb_down && g_freecam.cursor_captured) {
        ClipCursor(nullptr);
        ShowCursor(TRUE);
        g_freecam.cursor_captured = false;
    }

    if (g_freecam.cursor_captured) {
        POINT p;
        if (GetCursorPos(&p)) {
            const int dx = p.x - g_freecam.center.x;
            const int dy = p.y - g_freecam.center.y;
            if (dx || dy) {
                const float sens = static_cast<float>(REXCVAR_GET(freecam_sensitivity));
                // Camera actually looks along -forward (row2 is the camera's
                // back axis, standard RH view). Flip both deltas so mouse-right
                // turns the view right and mouse-up tilts it up.
                g_freecam.yaw -= dx * sens;
                g_freecam.pitch += dy * sens;
                constexpr float kMaxPitch = 1.5533f;  // ~89 degrees
                if (g_freecam.pitch > kMaxPitch) g_freecam.pitch = kMaxPitch;
                if (g_freecam.pitch < -kMaxPitch) g_freecam.pitch = -kMaxPitch;
                SetCursorPos(g_freecam.center.x, g_freecam.center.y);
            }
        }
    }

    // Movement: WASD + Space/Ctrl for up/down, Shift to go faster. Active
    // regardless of RMB/mouse-look, matching common editor-camera conventions.
    static auto last = std::chrono::high_resolution_clock::now();
    const auto now_t = std::chrono::high_resolution_clock::now();
    double dt = std::chrono::duration<double>(now_t - last).count();
    last = now_t;
    if (dt > 0.25) dt = 0.25;  // clamp across pauses/hitches

    float speed = static_cast<float>(REXCVAR_GET(freecam_speed) * dt);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) speed *= 3.0f;

    const float cy = std::cos(g_freecam.yaw), sy = std::sin(g_freecam.yaw);
    const float cp = std::cos(g_freecam.pitch), sp = std::sin(g_freecam.pitch);
    const float fx = sy * cp, fy = sp, fz = cy * cp;
    const float rx = cy, rz = -sy;

    // Forward (into the view) is -F: the camera looks along -row2, so W moves
    // along -F and S along +F.
    if (GetAsyncKeyState('W') & 0x8000) {
        g_freecam.x -= fx * speed; g_freecam.y -= fy * speed; g_freecam.z -= fz * speed;
    }
    if (GetAsyncKeyState('S') & 0x8000) {
        g_freecam.x += fx * speed; g_freecam.y += fy * speed; g_freecam.z += fz * speed;
    }
    if (GetAsyncKeyState('D') & 0x8000) { g_freecam.x += rx * speed; g_freecam.z += rz * speed; }
    if (GetAsyncKeyState('A') & 0x8000) { g_freecam.x -= rx * speed; g_freecam.z -= rz * speed; }
    if (GetAsyncKeyState(VK_SPACE) & 0x8000)   g_freecam.y += speed;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) g_freecam.y -= speed;

    static int dbg_tick = 0;
    if (dbg_tick++ % 60 == 0) {
        REXLOG_INFO("[freecam] pos=({:.1f},{:.1f},{:.1f}) yaw={:.2f} pitch={:.2f}",
                    g_freecam.x, g_freecam.y, g_freecam.z, g_freecam.yaw, g_freecam.pitch);
    }
#endif  // _WIN32
}

// ---------------------------------------------------------------------------
// Local co-op recon (M1) -- read-only
// ---------------------------------------------------------------------------
static void update_coop_probe() {
    if (!REXCVAR_GET(coop_probe)) return;
    static int tick = 0;
    if (tick++ % 120 != 0) return;  // ~2s at 60fps; this is a dump, not a poll

    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    bool ok = false;
    const uint32_t mgr = rd32_safe(base, 0x83326814, ok);
    if (!ok || !ptr_ok(mgr)) return;                 // no manager yet (frontend)
    const uint32_t begin = rd32_safe(base, mgr + 48, ok);
    if (!ok || !ptr_ok(begin)) return;
    const uint32_t end = rd32_safe(base, mgr + 52, ok);
    if (!ok || end < begin) return;

    const uint32_t count = (end - begin) / 4u;
    const uint32_t cap_end = rd32_safe(base, mgr + 56, ok);   // vector capacity end
    const uint32_t capacity = (ok && cap_end >= begin) ? (cap_end - begin) / 4u : 0u;

    // Is a player actually IN the world yet? player[0]+36 is the game object;
    // it stays NULL in the frontend. This has to be part of the change
    // signature: entering a level changes neither mgr nor count (the manager
    // persists and the count stays 1), so keying on those alone went quiet
    // exactly when the level -- the interesting state -- finally loaded.
    uint32_t p0 = 0, obj0 = 0;
    if (count) {
        p0 = rd32_safe(base, begin, ok);
        if (ok && ptr_ok(p0)) obj0 = rd32_safe(base, p0 + 36, ok);
    }

    static uint32_t last_sig = 0;
    const uint32_t sig = mgr ^ (count * 2654435761u) ^ (capacity * 40503u) ^
                         (obj0 ? 0x5BD1E995u : 0u);
    if (sig == last_sig) return;
    last_sig = sig;

    REXLOG_INFO("[coop] mgr={:08X} vec begin={:08X} end={:08X} players={} capacity={} in_level={}",
                mgr, begin, end, count, capacity, obj0 ? "YES" : "no");

    for (uint32_t i = 0; i < count && i < 8; ++i) {
        const uint32_t p = rd32_safe(base, begin + i * 4u, ok);
        if (!ok || !ptr_ok(p)) { REXLOG_INFO("[coop]   player[{}] = <bad>", i); continue; }
        const uint32_t obj = rd32_safe(base, p + 36, ok);
        // GetSpawnerIds (sub_8281E310) is just a loop over the player vector
        // reading player+0x14C0 -- so the spawner id is a plain read, and
        // calling the guest function would add allocation + a destructor for
        // nothing. FindPlayerForSpawnerId takes a hazingPlayer::ActorSpawnerID,
        // so this value identifies WHICH player slot the actor occupies.
        bool sok = false;
        const uint32_t spawner_id = rd32_safe(base, p + 0x14C0, sok);
        REXLOG_INFO("[coop]   player[{}] = {:08X}  obj(+36)={:08X}  spawner_id(+14C0)={:08X}",
                    i, p, ok ? obj : 0, sok ? spawner_id : 0xFFFFFFFFu);
        // Small field dump: a user id / pad index / spawner id should stand out
        // as a small integer among pointers.
        for (uint32_t row = 0; row < 0x60; row += 16) {
            char line[160];
            int o = std::snprintf(line, sizeof(line), "[coop]     p+%02X:", row);
            for (uint32_t off = row; off < row + 16; off += 4) {
                const uint32_t w = rd32_safe(base, p + off, ok);
                o += std::snprintf(line + o, sizeof(line) - (size_t)o, " %08X", ok ? w : 0);
            }
            REXLOG_INFO("{}", line);
        }
    }

    // Manager fields: the spawner list (GetSpawnerIds = sub_8281E310 reads it)
    // and any active-actor/user-id bookkeeping live in here. 'V' marks a word
    // that looks like a live guest pointer, which is how the vectors show up.
    for (uint32_t row = 0; row < 0x100; row += 16) {
        char line[160];
        int o = std::snprintf(line, sizeof(line), "[coop]   mgr+%02X:", row);
        for (uint32_t off = row; off < row + 16; off += 4) {
            const uint32_t w = rd32_safe(base, mgr + off, ok);
            const char tag = (ok && ptr_ok(w) && host_readable(base, w)) ? 'V' : ' ';
            o += std::snprintf(line + o, sizeof(line) - (size_t)o, " %08X%c", ok ? w : 0, tag);
        }
        REXLOG_INFO("{}", line);
    }

    // gameSpawner::SpawnerManager -- the LEVEL's spawn-point registry, and the
    // thing that actually decides whether a second player has anywhere to go.
    // Its SWIG GetInstance (sub_82649260) just returns this global, so no call
    // is needed. Vectors show up here as begin/end/capacity pointer triples the
    // same way the player vector does at mgr+0x30.
    const uint32_t smgr = rd32_safe(base, 0x83326770, ok);
    if (!ok || !ptr_ok(smgr)) {
        REXLOG_INFO("[coop] SpawnerManager: <null>");
        return;
    }
    REXLOG_INFO("[coop] SpawnerManager = {:08X}", smgr);
    for (uint32_t row = 0; row < 0x100; row += 16) {
        char line[160];
        int o = std::snprintf(line, sizeof(line), "[coop]   smgr+%02X:", row);
        for (uint32_t off = row; off < row + 16; off += 4) {
            const uint32_t w = rd32_safe(base, smgr + off, ok);
            const char tag = (ok && ptr_ok(w) && host_readable(base, w)) ? 'V' : ' ';
            o += std::snprintf(line + o, sizeof(line) - (size_t)o, " %08X%c", ok ? w : 0, tag);
        }
        REXLOG_INFO("{}", line);
    }
}

// ---------------------------------------------------------------------------
// Local co-op M2 experiment -- one-shot, orphan create
// ---------------------------------------------------------------------------
static void update_coop_spawn_test() {
    if (!REXCVAR_GET(coop_spawn_test)) return;
    static bool fired = false;
    if (fired) return;

    auto* mem = rex::system::kernel_memory();
    auto* rt  = rex::Runtime::instance();
    if (!mem || !rt) return;
    auto* fd = rt->function_dispatcher();
    if (!fd) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    bool ok = false;
    const uint32_t mgr = rd32_safe(base, 0x83326814, ok);
    if (!ok || !ptr_ok(mgr)) return;
    const uint32_t begin = rd32_safe(base, mgr + 48, ok);
    if (!ok || !ptr_ok(begin)) return;
    const uint32_t end = rd32_safe(base, mgr + 52, ok);
    if (!ok || end <= begin) return;

    // Only fire in a LIVE level: player[0] must have its game object wired,
    // otherwise the engine state the constructor touches may not be up yet.
    const uint32_t p0 = rd32_safe(base, begin, ok);
    if (!ok || !ptr_ok(p0)) return;
    const uint32_t obj0 = rd32_safe(base, p0 + 36, ok);
    if (!ok || !ptr_ok(obj0)) return;

    auto* fn = fd->GetFunction(0x8281C678u);
    if (!fn) {
        fired = true;
        REXLOG_WARN("[coop] SPAWN TEST: 0x8281C678 not registered with the dispatcher "
                    "-- add it to [entrypoint.functions] in restuff_manifest.toml");
        return;
    }

    fired = true;
    const uint32_t before = (end - begin) / 4u;
    REXLOG_INFO("[coop] SPAWN TEST: calling sub_8281C678(mgr={:08X}, a2=0); players before={}",
                mgr, before);

    const uint32_t np = rex::ppc::GuestToHostFunction<uint32_t>(*fn, mgr, 0u);

    // The roster must be byte-identical: create only allocates + constructs.
    const uint32_t begin2 = rd32_safe(base, mgr + 48, ok);
    const uint32_t end2   = rd32_safe(base, mgr + 52, ok);
    const uint32_t after  = (ok && end2 > begin2) ? (end2 - begin2) / 4u : 0u;
    REXLOG_INFO("[coop] SPAWN TEST: returned {:08X}; roster begin={:08X} end={:08X} players={} ({})",
                np, begin2, end2, after,
                (begin2 == begin && end2 == end) ? "roster UNCHANGED, as expected"
                                                 : "ROSTER MOVED -- unexpected");

    if (!ptr_ok(np) || !host_readable(base, np)) {
        REXLOG_WARN("[coop] SPAWN TEST: returned pointer is not readable -- create failed");
        return;
    }
    const uint32_t vt  = rd32_safe(base, np, ok);
    const uint32_t sid = rd32_safe(base, np + 0x14C0, ok);
    REXLOG_INFO("[coop] SPAWN TEST: new actor vtable={:08X} (expect 82228D98), "
                "spawner_id={:08X} (expect 00000001)", vt, sid);
    for (uint32_t row = 0; row < 0x60; row += 16) {
        char line[160];
        int o = std::snprintf(line, sizeof(line), "[coop]   new+%02X:", row);
        for (uint32_t off = row; off < row + 16; off += 4) {
            const uint32_t w = rd32_safe(base, np + off, ok);
            o += std::snprintf(line + o, sizeof(line) - (size_t)o, " %08X", ok ? w : 0);
        }
        REXLOG_INFO("{}", line);
    }
}

// --- Attract-mode video ------------------------------------------------------
// Plays a host-side MP4 fullscreen via the video overlay (src/video_player.h,
// src/video_overlay.h). play_attract_video() is the "call a function to play it"
// entry point -- safe from any thread -- so a title-screen idle timer (or F7)
// can start it. Any key/mouse press cancels playback.
REXCVAR_DEFINE_STRING(video_file, "attract.mp4", "Video",
    "The specific trailer to play (used when attract_random is off, and as the "
    "fallback if the trailer folder is empty). Absolute, or relative to the "
    "working directory / exe folder / assets folder.");
REXCVAR_DEFINE_BOOL(attract_random, true, "Video",
    "Pick a random trailer from attract_video_dir each time. Off = always play "
    "video_file.");
REXCVAR_DEFINE_STRING(attract_video_dir, "trailers", "Video",
    "Folder scanned for trailers when attract_random is on. Drop any number of "
    "videos in here. Relative to the working directory / exe folder / assets.");

// Resolves a path against cwd, the exe folder, and assets/. Empty if missing.
// Works for both files and directories.
static std::filesystem::path resolve_asset_path(const std::string& want) {
    if (want.empty()) return {};
    std::error_code ec;
    const std::filesystem::path rel(want);
    if (rel.is_absolute()) {
        return std::filesystem::exists(rel, ec) ? rel : std::filesystem::path{};
    }
    for (std::filesystem::path base :
         {std::filesystem::current_path(ec), rex::filesystem::GetExecutableFolder()}) {
        for (; !base.empty(); base = base.parent_path()) {
            for (const std::filesystem::path& cand : {base / rel, base / "assets" / rel}) {
                if (std::filesystem::exists(cand, ec)) return cand;
            }
            if (base == base.parent_path()) break;
        }
    }
    return {};
}

// Container formats Media Foundation can open out of the box. .mp4 is safest.
// Compared in the path's native (wide) form: path::string() converts via the
// active code page and throws on anything it can't represent.
static bool is_video_file(const std::filesystem::path& p) {
    std::wstring e = p.extension().native();
    std::transform(e.begin(), e.end(), e.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return e == L".mp4" || e == L".m4v" || e == L".mov" || e == L".wmv" || e == L".avi";
}

// Every playable trailer in attract_video_dir, sorted so ordering is stable.
static std::vector<std::filesystem::path> list_attract_videos() {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path dir = resolve_asset_path(REXCVAR_GET(attract_video_dir));
    std::error_code ec;
    if (dir.empty() || !std::filesystem::is_directory(dir, ec)) return out;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec) && is_video_file(entry.path())) {
            out.push_back(entry.path());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

// What to play now: a random trailer from the folder when attract_random is on
// (and the folder has any), otherwise the single video_file.
static std::filesystem::path pick_attract_video() {
    if (REXCVAR_GET(attract_random)) {
        const std::vector<std::filesystem::path> vids = list_attract_videos();
        if (!vids.empty()) {
            static std::mt19937 rng{std::random_device{}()};
            // Compared as paths, not strings: path::string() throws on a
            // filename the active code page can't represent (accents etc.).
            static std::filesystem::path last;
            size_t i = std::uniform_int_distribution<size_t>(0, vids.size() - 1)(rng);
            // Avoid playing the same one twice in a row -- but ONLY with 3+
            // videos. With exactly 2, "never repeat" isn't a nudge, it's forced
            // alternation (A,B,A,B) that ignores the roll entirely, so below 3
            // we leave the draw alone and let it be genuinely random.
            if (vids.size() >= 3 && vids[i] == last) {
                i = (i + 1) % vids.size();
            }
            last = vids[i];
            return vids[i];
        }
    }
    return resolve_asset_path(REXCVAR_GET(video_file));
}

// Start the attract cinematic. Call from anywhere (e.g. an idle timer).
void play_attract_video() {
    const std::filesystem::path p = pick_attract_video();
    if (p.empty()) {
        REXLOG_WARN("[video] nothing to play: no videos in '{}' and '{}' not found",
                    REXCVAR_GET(attract_video_dir), REXCVAR_GET(video_file));
        return;
    }
    REXLOG_INFO("[video] playing {}", restuff::PathToUtf8(p));
    restuff::PlayVideo(p);
}

// Stop it early (e.g. on guest controller input, which ImGui never sees).
void stop_attract_video() { restuff::StopVideo(); }

REXCVAR_DEFINE_BOOL(attract_enabled, true, "Video",
    "Play the attract cinematic after sitting idle on the menus/title screen.");
REXCVAR_DEFINE_DOUBLE(attract_delay, 45.0, "Video",
    "Seconds of no input on the front-end before the attract cinematic plays.")
    .range(5.0, 900.0);

// Seconds since the player last did anything. Written from the input hook (game
// thread) and the per-frame tick, so keep it atomic.
static std::atomic<double> g_attract_idle{0.0};
// Set once the title screen's own load unit ("StartMenu") has streamed in, so
// the timer can't run during the boot logos before the menu even exists.
static std::atomic<bool> g_attract_saw_startmenu{false};

// Called whenever a real keystroke is dequeued: the player is present, so reset
// the timer and drop out of any cinematic that's running.
void note_input_activity() {
    g_attract_idle.store(0.0, std::memory_order_relaxed);
    if (restuff::IsVideoPlaying()) restuff::StopVideo();
}

// Called for every load unit the game streams. Any streaming means we're still
// booting or moving between screens -- not sitting idle -- so hold the timer at
// zero. Seeing "StartMenu" is what unlocks the timer in the first place.
void note_loadunit_activity(const char* name) {
    g_attract_idle.store(0.0, std::memory_order_relaxed);
    if (name && std::strcmp(name, "startmenu") == 0) {
        g_attract_saw_startmenu.store(true, std::memory_order_relaxed);
    }
}

// Per-frame attract timer. Only counts on the FRONT-END: resolve_player_damageable
// is non-null exactly while a level is live, so gameplay can never trigger this.
static void update_attract(double dt) {
    // Mirror playback into guest input suppression. While a cinematic is up the
    // game must not see the pad at all, or the button that dismisses the video
    // also activates whatever menu item is underneath. Clearing it enters the
    // "held until release" state, so the dismissing press can't leak through
    // once the video is gone. Runs before any early-out so the F7 path is
    // covered too, not just the attract timer.
    {
        static bool was_playing = false;
        const bool playing = restuff::IsVideoPlaying();
        if (playing != was_playing) {
            g_video_active.store(playing, std::memory_order_relaxed);
            set_guest_input_suppressed(playing);
            was_playing = playing;
        }
    }

    if (!REXCVAR_GET(attract_enabled)) {
        g_attract_idle.store(0.0, std::memory_order_relaxed);
        return;
    }
    // Already showing one (or the player is in a level) -> hold the timer at 0.
    if (restuff::IsVideoPlaying()) {
        g_attract_idle.store(0.0, std::memory_order_relaxed);
        return;
    }
    // Don't count during the boot logos: wait until the title screen exists.
    if (!g_attract_saw_startmenu.load(std::memory_order_relaxed)) return;
    auto* mem = rex::system::kernel_memory();
    uint8_t* base = mem ? mem->virtual_membase() : nullptr;
    if (base && resolve_player_damageable(base)) {  // in gameplay
        g_attract_idle.store(0.0, std::memory_order_relaxed);
        return;
    }

    const double t = g_attract_idle.load(std::memory_order_relaxed) + dt;
    if (t >= REXCVAR_GET(attract_delay)) {
        g_attract_idle.store(0.0, std::memory_order_relaxed);
        REXLOG_INFO("[video] attract: {:.0f}s idle on the front-end", t);
        play_attract_video();
    } else {
        g_attract_idle.store(t, std::memory_order_relaxed);
    }
}

static const bool s_video_bind_registered = []() {
    rex::ui::RegisterBind("bind_play_video", "F7", "Play attract video",
                          [] { play_attract_video(); });
    return true;
}();

// ---------------------------------------------------------------------------
// Score bonus cheat
// ---------------------------------------------------------------------------
// inject_score_bonus hooks sub_8280F428 (HazingScoreMgr::GetTotalScore) just
// before it returns, after the accumulation loop has finished.  r31 holds the
// HazingScoreMgr* (PPC guest address); offset 0x88 (136) is the cached float
// total that both return paths read.  We add g_score_bonus here so that every
// score query the game makes reflects the cheat amount.

static std::atomic<float> g_score_bonus{0.0f};

void add_score_cheat(float amount) {
    g_score_bonus.fetch_add(amount, std::memory_order_relaxed);
}

void reset_score_cheat() {
    g_score_bonus.store(0.0f, std::memory_order_relaxed);
}

float get_score_bonus() {
    return g_score_bonus.load(std::memory_order_relaxed);
}

void set_wireframe(bool val) { REXCVAR_SET(wireframe, val); }
bool get_wireframe()        { return REXCVAR_GET(wireframe); }

void set_unlock_all(bool val) {
    REXCVAR_SET(unlock_all, val);
    if (!val) g_unlock_mgr.store(0, std::memory_order_relaxed);  // re-resolve next time
}
bool get_unlock_all()        { return REXCVAR_GET(unlock_all); }

// ---------------------------------------------------------------------------
// Wireframe ring-buffer intercept
// ---------------------------------------------------------------------------
// Context registers 0x2200-0x220B are flushed to the PM4 ring buffer as
// type-0 packets by sub_82F47D40 (and its ring-buffer-overflow helper
// sub_82F476F8) before each draw call.
//
// Two hook pairs track which register is currently being written:
//
//   on_ctx_reg_header — fires when the type-0 header word
//       ((count-1)<<16 | start_reg) is about to be written to the ring
//       buffer.  Records the starting register index and resets the
//       per-register counter.
//
//   on_ctx_reg_data — fires when each register value is about to be written.
//       If the current register is PA_SU_SC_MODE_CNTL (0x2205) and the
//       wireframe cvar is set, forces the poly-mode bits in r10 before the
//       stwu commits the word.  The shadow in guest memory is left intact so
//       the game's own state tracking is not corrupted.
//
// Hook addresses:
//   sub_82F47D40 inline path — header 0x82f47dd4, data 0x82f47de4
//   sub_82F476F8 overflow path — header 0x82f47768, data 0x82f47778

static uint32_t s_ctx_run_start = 0;
static uint32_t s_ctx_run_idx   = 0;

// Called at each type-0 header write (same function for both paths).
void on_ctx_reg_header(PPCRegister& r10) {
    s_ctx_run_start = r10.u32 & 0xFFFF;
    s_ctx_run_idx   = 0;
}

// Called at each type-0 data write (same function for both paths).
void on_ctx_reg_data(PPCRegister& r10) {
    const uint32_t current_reg = s_ctx_run_start + s_ctx_run_idx;
    ++s_ctx_run_idx;

    if (current_reg == 0x2205 && REXCVAR_GET(wireframe))
        r10.u32 = (r10.u32 & ~0x7F8u) | 0x128u;
}



// Hook at 0x8280F4DC (cmplwi cr6,r11,0), after_instruction = true.
// Fires once per GetTotalScore call after accumulation, before return branch.
void inject_score_bonus(PPCRegister& r31) {
    g_score_mgr.store(r31.u32, std::memory_order_relaxed);  // capture live HazingScoreMgr

    const float bonus = g_score_bonus.load(std::memory_order_relaxed);
    if (bonus <= 0.0f) return;

    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    // Score float lives at HazingScoreMgr + 0x88 (big-endian in PPC memory).
    const uint32_t guest_addr = r31.u32 + 0x88;
    const uint32_t host_offset = (guest_addr >= 0xE0000000u) ? 0x1000u : 0u;
    volatile uint32_t* ptr =
        reinterpret_cast<volatile uint32_t*>(base + guest_addr + host_offset);

    uint32_t raw = __builtin_bswap32(*ptr);
    float score;
    std::memcpy(&score, &raw, 4);
    score += bonus;
    std::memcpy(&raw, &score, 4);
    *ptr = __builtin_bswap32(raw);
}

// --- M3.37 diag: KeWaitForMultipleObjects census -----------------------------
// The SDK runtime implements multi-object waits as a ~1ms nanosleep POLL loop
// (PosixConditionBase::WaitMultiple); gameplay chains these into ~200ms+
// frames while menus (shallow wait graphs) stay fast. Interpose the dynamic
// import (strong def in the executable wins; forward via dlsym(RTLD_NEXT))
// and census call count x duration under RESTUFF_WAIT_CENSUS=1.
#ifndef _WIN32
#include <dlfcn.h>

extern "C" REX_FUNC(__imp__KeWaitForMultipleObjects) {
  using Fn = void (*)(PPCContext&, uint8_t*);
  static Fn orig =
      reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "__imp__KeWaitForMultipleObjects"));
  static const bool census = getenv("RESTUFF_WAIT_CENSUS") != nullptr;
  if (!orig) {
    REXLOG_ERROR("[WAITC] original KeWaitForMultipleObjects not found");
    return;
  }
  if (!census) {
    orig(ctx, base);
    return;
  }
  const uint32_t count = ctx.r3.u32;
  const auto t0 = std::chrono::steady_clock::now();
  orig(ctx, base);
  const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count();
  static std::atomic<uint64_t> s_calls{0}, s_us{0}, s_long{0};
  static std::atomic<uint64_t> s_bycount[8] = {};
  s_calls.fetch_add(1, std::memory_order_relaxed);
  s_us.fetch_add(us, std::memory_order_relaxed);
  if (us > 5000) s_long.fetch_add(1, std::memory_order_relaxed);
  s_bycount[count < 8 ? count : 7].fetch_add(1, std::memory_order_relaxed);
  const uint64_t n = s_calls.fetch_add(0, std::memory_order_relaxed);
  if (n % 2000 == 0) {
    REXLOG_INFO(
        "[WAITC] calls={} total_ms={} long5ms={} bycount c1={} c2={} c3={} c4={} c5={} c6={} c7p={}",
        n, s_us.load(std::memory_order_relaxed) / 1000,
        s_long.load(std::memory_order_relaxed),
        s_bycount[1].load(), s_bycount[2].load(), s_bycount[3].load(),
        s_bycount[4].load(), s_bycount[5].load(), s_bycount[6].load(),
        s_bycount[7].load());
  }
}
#endif

// ---------------------------------------------------------------------------
// M3.177 (RESTUFF_IBWATCH=physlo,physhi): name the terrain PARTITION BUILDER.
// The wedge is a proven coverage gap baked into family A's index buffers at
// build time (geometric proof, commit 1bc8cf4); the writer of those IBs IS
// the builder. mprotect the host pages backing the guest-physical range
// read-only; every FIRST write to a page faults; the SIGSEGV handler records
// (guest page, host PC) into a lock-free ring, unprotects that page, and
// resumes. PCs symbolize offline against the recompiled sub_82xxxxxx symbols
// (nm). Armed lazily at the first LULOG-era hook call so `base` is known and
// protection lands before the terrain LUs parse (~85s into boot).
#ifndef _WIN32
#include <csignal>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif
// M3.192 (RESTUFF_MERGELOG=1): defer-gate outcome counters. mrg5 proved the
// merger's INPUTS (records, order, keys) are identical across wedge/merged
// boots, so the fork must be the defer path: records intersecting an active
// channel's region get queued + signalled for the SECOND-stage merge (the
// drain). Prediction: wedge boots pass ~0 defer gates, merged boots >0.
namespace mergegate {
static std::atomic<uint32_t> g_bfe0_calls{0}, g_bfe0_true{0};
static std::atomic<uint32_t> g_0768_calls{0}, g_0768_true{0};
static std::atomic<uint32_t> g_sig{0};
// M3.200: the render-path reconcile CEASES at ~120s on wedge boots (mrg17)
// while merged boots reconcile to run end — a deadline/retry-cap expiry is
// the fork. These split which side stops: the per-frame walk dispatcher
// (sub_8297ED80) or its per-object pre-emit gate (sub_829C77B8).
static std::atomic<uint32_t> g_ed80{0}, g_77b8{0};
// M3.205: distinct residency arrays seen at the reconciler (begin<<32|end),
// dumped 1Hz from MergeTick ALL RUN — the reconciler goes quiet at ~84s but
// the late registration lands at 90-120s, invisible to callsite sampling.
static std::atomic<uint64_t> g_arrs[8];
}

#ifndef _WIN32
namespace ibwatch {
static uint8_t* g_base = nullptr;
static uintptr_t g_spans[4][2] = {};   // host ranges (physical + 3 VA windows)
static uintptr_t g_text_lo = 0, g_text_hi = 0;  // exe text range for stack-scan
static struct sigaction g_prev;
struct Hit { uint32_t guest_page; uint32_t ms; uintptr_t pc; uintptr_t bt[8]; };
static Hit g_hits[4096];
static std::atomic<uint32_t> g_n{0};
static std::atomic<uint32_t> g_drained{0};
static std::atomic<bool> g_reset_after_drain{false};
// M3.188: load-vs-gameplay is a TIMING claim, so every hit carries a clock —
// ms since Arm() (which fires ~5s into a run). Without this the late2 batch's
// hits were unplaceable and the load-era-vs-merge question stayed open.
static std::atomic<int64_t> g_t0_ms{0};
static int64_t NowMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);  // async-signal-safe (raw syscall class)
  return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

static void Handler(int sig, siginfo_t* si, void* uc_v) {
  const uintptr_t a = reinterpret_cast<uintptr_t>(si->si_addr);
  bool in_span = false;
  for (int k = 0; k < 4; ++k)
    if (a >= g_spans[k][0] && a < g_spans[k][1]) { in_span = true; break; }
  if (in_span) {
    ucontext_t* uc = static_cast<ucontext_t*>(uc_v);
    const uintptr_t pc = uc->uc_mcontext.gregs[REG_RIP];
    const uint32_t n = g_n.fetch_add(1, std::memory_order_relaxed);
    if (n < 4096) {
      g_hits[n].guest_page =
          uint32_t((a & ~uintptr_t(4095)) - reinterpret_cast<uintptr_t>(g_base));  // guest VA page (window bits kept)
      g_hits[n].ms = uint32_t(NowMs() - g_t0_ms.load(std::memory_order_relaxed));
      g_hits[n].pc = pc;
      // M3.177e: RelWithDebInfo omits frame pointers (the RBP walk returned
      // nothing) -- STACK-SCAN instead: take the first 3 stack words landing
      // inside the exe text range. Noisy but names callers reliably enough
      // to attribute memcpy-class writers.
      for (int d = 0; d < 8; ++d) g_hits[n].bt[d] = 0;
      const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(
          uc->uc_mcontext.gregs[REG_RSP] & ~uintptr_t(7));
      int found = 0;
      // 8 frames deep, 2048 words: the second dispatch table's handler frames
      // sat below the 3-frame horizon (grep of all captured stacks: zero
      // 82AEBxxx/82AECxxx frames).
      for (int w = 0; w < 2048 && found < 8; ++w) {
        const uintptr_t v = sp[w];
        if (v >= g_text_lo && v < g_text_hi) g_hits[n].bt[found++] = v;
      }
    }
    // raw syscall: mprotect isn't on the official async-signal-safe list, but
    // the direct syscall has no libc state; page-granular RW restore.
    syscall(SYS_mprotect, a & ~uintptr_t(4095), 4096, PROT_READ | PROT_WRITE);
    return;
  }
  if (g_prev.sa_flags & SA_SIGINFO) {
    if (g_prev.sa_sigaction) { g_prev.sa_sigaction(sig, si, uc_v); return; }
  } else if (g_prev.sa_handler == SIG_DFL || g_prev.sa_handler == SIG_IGN) {
    signal(sig, g_prev.sa_handler);
    raise(sig);
    return;
  } else if (g_prev.sa_handler) {
    g_prev.sa_handler(sig);
    return;
  }
  signal(sig, SIG_DFL);
  raise(sig);
}

static void Arm(uint8_t* base) {
  static std::atomic<bool> s_armed{false};
  bool expected = false;
  if (!s_armed.compare_exchange_strong(expected, true)) return;
  const char* e = getenv("RESTUFF_IBWATCH");
  if (!e) return;
  uint32_t lo = 0, hi = 0;
  if (sscanf(e, "%x,%x", &lo, &hi) != 2 || hi <= lo) return;
  g_base = base;
  struct sigaction sa{};
  sa.sa_sigaction = Handler;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_prev);
  // The guest CPU reaches physical p through the 360's VA WINDOWS -- cached
  // 0xA0000000+p, writecombined 0xC0000000+p, uncached 0xE0000000+p -- which
  // are DISTINCT virtual ranges here (the first arm protected only base+p and
  // caught zero writes; the builder writes through a window). Protect every
  // alias; record spans so the handler matches any of them.
  // Log the exe load base so the (ASLR) fault PCs symbolize offline:
  // slide = base_from_maps; nm file offsets + slide = runtime PCs.
  {
    FILE* f = fopen("/proc/self/maps", "r");
    if (f) {
      char line[512];
      while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "/restuff")) {
          line[strcspn(line, "\n")] = 0;
          REXLOG_INFO("[IBWATCH] map {}", line);
          uintptr_t lo = 0, hi = 0;
          char perms[8] = {};
          if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3) {
            if (strchr(perms, 'x')) { g_text_lo = lo; g_text_hi = hi; break; }
          }
        }
      }
      fclose(f);
    }
  }
  g_t0_ms.store(NowMs(), std::memory_order_relaxed);
  static const uint32_t kAliases[4] = {0x00000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u};
  for (int k = 0; k < 4; ++k) {
    const uintptr_t alo = reinterpret_cast<uintptr_t>(base) + kAliases[k] + lo;
    const uintptr_t ahi = reinterpret_cast<uintptr_t>(base) + kAliases[k] + hi;
    g_spans[k][0] = alo;
    g_spans[k][1] = ahi;
    const int rc = mprotect(reinterpret_cast<void*>(alo & ~uintptr_t(4095)),
                            ((ahi + 4095) & ~uintptr_t(4095)) - (alo & ~uintptr_t(4095)),
                            PROT_READ);
    REXLOG_INFO("[IBWATCH] alias {:08X}+[{:08X},{:08X}) mprotect rc={}", kAliases[k], lo,
                hi, rc);
  }
}

static void ArmLate() {
  // One-shot late arm; no re-arm ever follows. Range from
  // RESTUFF_IBWATCH_LATE_RANGE=<lo,hi hex> (default = the 1B compose range).
  if (!g_base) return;  // Arm() must have run (installs the handler); require
                        // RESTUFF_IBWATCH too (any range) for the handler.
  uint32_t lo = 0x1B000000u, hi = 0x1C000000u;
  if (const char* r = getenv("RESTUFF_IBWATCH_LATE_RANGE")) {
    uint32_t l2 = 0, h2 = 0;
    if (sscanf(r, "%x,%x", &l2, &h2) == 2 && h2 > l2) { lo = l2; hi = h2; }
  }
  static const uint32_t kAl[4] = {0x00000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u};
  for (int k = 0; k < 4; ++k) {
    const uintptr_t alo = reinterpret_cast<uintptr_t>(g_base) + kAl[k] + lo;
    const uintptr_t ahi = reinterpret_cast<uintptr_t>(g_base) + kAl[k] + hi;
    g_spans[k][0] = alo;
    g_spans[k][1] = ahi;
    mprotect(reinterpret_cast<void*>(alo & ~uintptr_t(4095)),
             ((ahi + 4095) & ~uintptr_t(4095)) - (alo & ~uintptr_t(4095)), PROT_READ);
  }
  // The arm moment goes to the SIDE FILE, not just REXLOG: the late2 batch's
  // "zero 1B writes" was unfalsifiable because nothing rotation-proof recorded
  // whether the arm ever happened (it hadn't).
  const long tarm = long(NowMs() - g_t0_ms.load(std::memory_order_relaxed));
  REXLOG_INFO("[IBWATCH] LATE arm over {:08X}-{:08X} (one-shot, t={}ms)", lo, hi, tarm);
  if (FILE* f2 = fopen("/tmp/restuff_drive/ibwatch_hits.txt", "a")) {
    fprintf(f2, "late-arm t=%ldms range=%08X,%08X\n", tarm, lo, hi);
    fclose(f2);
  }
}

void ArmAt(uint32_t lo, uint32_t hi) {
  // M3.201: one-shot watch over a RUNTIME-DISCOVERED range (the sector
  // objects live on the heap; only the STAB hook knows their addresses).
  // Same spans/protect logic as ArmLate; requires Arm() to have installed
  // the handler.
  if (!g_base || hi <= lo) return;
  static const uint32_t kAl[4] = {0x00000000u, 0xA0000000u, 0xC0000000u, 0xE0000000u};
  for (int k = 0; k < 4; ++k) {
    const uintptr_t alo = reinterpret_cast<uintptr_t>(g_base) + kAl[k] + lo;
    const uintptr_t ahi = reinterpret_cast<uintptr_t>(g_base) + kAl[k] + hi;
    g_spans[k][0] = alo;
    g_spans[k][1] = ahi;
    mprotect(reinterpret_cast<void*>(alo & ~uintptr_t(4095)),
             ((ahi + 4095) & ~uintptr_t(4095)) - (alo & ~uintptr_t(4095)), PROT_READ);
  }
  const long tarm = long(NowMs() - g_t0_ms.load(std::memory_order_relaxed));
  if (FILE* f2 = fopen("/tmp/restuff_drive/ibwatch_hits.txt", "a")) {
    fprintf(f2, "armat t=%ldms range=%08X,%08X\n", tarm, lo, hi);
    fclose(f2);
  }
}

void TryLateMs() {
  // M3.188 time-based one-shot late arm (RESTUFF_IBWATCH_LATE_MS=<ms since
  // Arm()>): the draw-hook call-counter variant NEVER FIRED in real runs
  // (sub_82F38988 unexercised on this path — 4 boots, zero LATE arms), so
  // "late" is now a CLOCK, checked from our per-frame renderer loop (the one
  // site proven hot all run) plus the guest hooks.
  static const uint32_t after_ms = [] {
    const char* e = getenv("RESTUFF_IBWATCH_LATE_MS");
    return e ? uint32_t(strtoul(e, nullptr, 0)) : 0u;
  }();
  if (!after_ms || !g_base) return;
  static std::atomic<bool> s_fired{false};
  if (s_fired.load(std::memory_order_relaxed)) return;
  if (NowMs() - g_t0_ms.load(std::memory_order_relaxed) < int64_t(after_ms)) return;
  bool expected = false;
  if (s_fired.compare_exchange_strong(expected, true)) ArmLate();
}

static void Rearm() {
  // RESTUFF_IBWATCH_REARM=<sec>: re-protect all aliases every <sec> so each
  // EPOCH records its first-writers -- the one-shot arm only caught load-time
  // writes; the DOOR-MERGE rebuild happens minutes later.
  static const int every = [] {
    const char* e = getenv("RESTUFF_IBWATCH_REARM");
    return e ? atoi(e) : 0;
  }();
  if (!every || !g_base) return;
  static std::atomic<int64_t> s_last{0};
  const int64_t now =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t last = s_last.load(std::memory_order_relaxed);
  if (last && now - last < every) return;
  if (!s_last.compare_exchange_strong(last, now)) return;
  for (int k = 0; k < 4; ++k) {
    if (!g_spans[k][0]) continue;
    mprotect(reinterpret_cast<void*>(g_spans[k][0] & ~uintptr_t(4095)),
             ((g_spans[k][1] + 4095) & ~uintptr_t(4095)) - (g_spans[k][0] & ~uintptr_t(4095)),
             PROT_READ);
  }
  // M3.186c: DO NOT reset here — resetting before the caller's Drain lost
  // entries and left g_drained pinned at the cap (no drains ever again), and
  // the reset+rearm interplay hung pacer3 for 2h. Instead set a flag; Drain
  // performs drain-then-reset atomically on its (single) calling thread.
  g_reset_after_drain.store(true, std::memory_order_relaxed);
  REXLOG_INFO("[IBWATCH] rearmed (epoch t={}s)", now);
  if (FILE* f2 = fopen("/tmp/restuff_drive/ibwatch_hits.txt", "a")) {
    fprintf(f2, "rearmed t=%lds\n", static_cast<long>(now));
    fclose(f2);
  }
}

static void Drain() {
  const uint32_t n = std::min<uint32_t>(g_n.load(std::memory_order_relaxed), 4096);
  uint32_t d = g_drained.load(std::memory_order_relaxed);
  if (d >= n) return;
  // Side file, not REXLOG: the 5MB log rotation shed the early epochs' hits.
  static FILE* f = fopen("/tmp/restuff_drive/ibwatch_hits.txt", "a");
  while (d < n) {
    if (f)
      fprintf(f, "hit#%u guest_page=%08X t=%ums pc=0x%lx bt=0x%lx,0x%lx,0x%lx,0x%lx,0x%lx,0x%lx,0x%lx,0x%lx text=0x%lx\n",
              d, g_hits[d].guest_page, g_hits[d].ms, g_hits[d].pc, g_hits[d].bt[0], g_hits[d].bt[1],
              g_hits[d].bt[2], g_hits[d].bt[3], g_hits[d].bt[4], g_hits[d].bt[5],
              g_hits[d].bt[6], g_hits[d].bt[7], g_text_lo);
    ++d;
  }
  if (f) fflush(f);
  g_drained.store(d, std::memory_order_relaxed);
  if (g_reset_after_drain.exchange(false, std::memory_order_relaxed)) {
    // Epoch boundary: restart the ring so gameplay-era epochs can record.
    g_n.store(0, std::memory_order_relaxed);
    g_drained.store(0, std::memory_order_relaxed);
  }
}

int64_t SinceArmMs() { return NowMs() - g_t0_ms.load(std::memory_order_relaxed); }

void MergeTick() {
  // M3.189 (RESTUFF_MERGELOG=1): 1Hz peek of the merge machinery's control
  // globals (named via the late3 chain decompiles: scheduler sub_829CA0C8
  // reads 832D4310 + 83341EA0, dispatcher sub_8297E4C8 is gated by 83341E9C,
  // dirty-check sub_82AD8AB0 compares stamp 832E7E74). A per-boot diff of
  // these between wedge and merged outcomes is the frozen-lottery probe.
  // Logs only on CHANGE of the masks/bytes (the stamp ticks every frame and
  // is carried, not a trigger).
  static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
  if (!on || !g_base) return;
  static std::atomic<int64_t> s_last{0};
  const int64_t now = NowMs();
  int64_t last = s_last.load(std::memory_order_relaxed);
  if (last && now - last < 1000) return;
  if (!s_last.compare_exchange_strong(last, now)) return;
  uint64_t mask, ea0;
  uint32_t stamp;
  memcpy(&mask, g_base + 0x832D4310u, 8);
  memcpy(&ea0, g_base + 0x83341EA0u, 8);
  memcpy(&stamp, g_base + 0x832E7E74u, 4);
  mask = __builtin_bswap64(mask);
  ea0 = __builtin_bswap64(ea0);
  stamp = __builtin_bswap32(stamp);
  const uint8_t e9c = g_base[0x83341E9Cu], mode = g_base[0x832E750Au],
                cap = g_base[0x83346DBDu];
  static uint64_t p_mask = ~0ull, p_ea0 = ~0ull;
  static uint8_t p_e9c = 0xFF, p_mode = 0xFF;
  static bool first = true;
  if (first || mask != p_mask || ea0 != p_ea0 || e9c != p_e9c || mode != p_mode) {
    if (FILE* f = fopen(restuff_logpath(), "a")) {
      fprintf(f, "PEEK t=%ldms e9c=%02X ea0=%016llX mask=%016llX mode=%02X cap=%02X stamp=%u\n",
              long(SinceArmMs()), e9c, (unsigned long long)ea0,
              (unsigned long long)mask, mode, cap, stamp);
      fclose(f);
    }
    first = false; p_mask = mask; p_ea0 = ea0; p_e9c = e9c; p_mode = mode;
  }
  // M3.192: 1Hz defer-gate state — counters, the 6×28B channel registry at
  // the object *(u32*)832E74F0, and the per-channel float threshold table
  // (base computed by the COMPILER from the decompile's displacement; never
  // hand-derive guest constants).
  if (FILE* f = fopen(restuff_logpath(), "a")) {
    const uint32_t v11 = [&] {
      uint32_t v; memcpy(&v, g_base + 0x832E74F0u, 4); return __builtin_bswap32(v);
    }();
    fprintf(f, "GATE t=%ldms bfe0=%u/%u c768=%u/%u sig=%u ed80=%u c77b8=%u v11=%08X",
            long(SinceArmMs()),
            mergegate::g_bfe0_true.load(std::memory_order_relaxed),
            mergegate::g_bfe0_calls.load(std::memory_order_relaxed),
            mergegate::g_0768_true.load(std::memory_order_relaxed),
            mergegate::g_0768_calls.load(std::memory_order_relaxed),
            mergegate::g_sig.load(std::memory_order_relaxed),
            mergegate::g_ed80.load(std::memory_order_relaxed),
            mergegate::g_77b8.load(std::memory_order_relaxed), v11);
    if (v11 >= 0x10000 && v11 < 0xC0000000u - 168) {
      fprintf(f, " reg=");
      for (int i = 0; i < 168; ++i) fprintf(f, "%02X", g_base[v11 + i]);
    }
    const uint32_t th_base = uint32_t(-2094123932LL);
    if (th_base >= 0x10000 && th_base < 0xC0000000u - 24) {
      fprintf(f, " th=[");
      for (int i = 0; i < 6; ++i) {
        uint32_t u; memcpy(&u, g_base + th_base + 4 * i, 4);
        u = __builtin_bswap32(u);
        float fv; memcpy(&fv, &u, 4);
        fprintf(f, "%s%g", i ? "," : "", fv);
      }
      fprintf(f, "]");
    }
    // M3.195b: the walk-context struct is INLINE at 0x83366090 (live peek:
    // +0 = zero qword, +8.. = SIX per-class object pointers — the first
    // dump chased a null pointer). Dump the inline 96B block plus each
    // class object's first 32B; a wedge boot's stuck dirty state must show
    // as a persistent nonzero somewhere in here.
    {
      fprintf(f, " dmask=");
      for (int i = 0; i < 96; ++i) fprintf(f, "%02X", g_base[0x83366090u + i]);
      for (int c = 0; c < 6; ++c) {
        uint32_t cp; memcpy(&cp, g_base + 0x83366098u + 4 * c, 4);
        cp = __builtin_bswap32(cp);
        if (cp >= 0x10000 && cp < 0xC0000000u - 32) {
          fprintf(f, " c%d@%08X=", c, cp);
          for (int i = 0; i < 32; ++i) fprintf(f, "%02X", g_base[cp + i]);
        }
      }
    }
    fprintf(f, "\n");
    // M3.205: all-run 1Hz dumps of every residency array the reconciler
    // ever touched (bounds re-read live; entries move as inserts land).
    for (int gi = 0; gi < 8; ++gi) {
      const uint64_t rec64 = mergegate::g_arrs[gi].load(std::memory_order_relaxed);
      if (!rec64) continue;
      const uint32_t lo = uint32_t(rec64 >> 32), hi = uint32_t(rec64);
      if (lo < 0x10000 || hi <= lo || hi - lo > 65 * 8 || hi >= 0xC0000000u) continue;
      fprintf(f, "RARR2 g=%d t=%ldms b=%08X q=", gi, long(SinceArmMs()), lo);
      for (uint32_t e = lo; e + 8 <= hi; e += 8) {
        uint64_t q; memcpy(&q, g_base + e, 8);
        fprintf(f, "%016llX ", (unsigned long long)__builtin_bswap64(q));
      }
      fprintf(f, "\n");
    }
    fclose(f);
  }
}
}  // namespace ibwatch
#else   // _WIN32
// The mprotect/SIGSEGV watch machinery is Linux diagnostic tooling (opt-in via
// RESTUFF_IBWATCH etc.) and never ships behaviour. Windows builds get the CLOCK
// -- SinceArmMs stamps every evidence log line -- and inert arm/drain stubs.
namespace ibwatch {
static int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static std::atomic<int64_t> g_t0_ms{NowMs()};  // process start = the arm epoch
int64_t SinceArmMs() { return NowMs() - g_t0_ms.load(std::memory_order_relaxed); }
[[maybe_unused]] static void Arm(uint8_t*) {}
[[maybe_unused]] static void ArmLate() {}
[[maybe_unused]] static void Rearm() {}
[[maybe_unused]] static void Drain() {}
void ArmAt(uint32_t, uint32_t) {}
void TryLateMs() {}
void MergeTick() {}
}  // namespace ibwatch (win32 stubs)
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// M3.210 (RESTUFF_HWBP=1): HARDWARE data watchpoint on the sector ATTACH word.
//
// The attach observable is sector_entry+36 (STAB/MW): NULL on wedge boots, a
// heap pointer (A809xxxx-class) on merged ones. Its WRITER is the direct-attach
// step merged boots run in the sweep window — the last unnamed function in the
// chain, and the consumer the FORGE (M3.208) could not reach by making the
// doomed residency query pass.
//
// Page-granular mprotect cannot name it: the table's pages carry routine
// scheduler bookkeeping, so a one-shot always fires on an unrelated store
// (M3.201 and M3.202 both died exactly this way). A hardware breakpoint
// watches those 4 bytes and nothing else, and the perf sample carries the
// writing instruction's IP.
//
// Two facts shape the design:
//   - x86 debug registers are PER-THREAD, so one perf event is opened per
//     thread in /proc/self/task (re-scanned on every re-arm, so threads
//     created later are covered too).
//   - WHICH sector attaches varies per boot (M3.209b), so the arm is
//     ADAPTIVE: watch 4 still-NULL sectors and let a 2ms poller re-arm onto
//     other still-NULL ones whenever a transition lands on an unwatched
//     sector. The poller's transition log (HWTR) is itself a 50x finer
//     bracket than MW's 10Hz sampling, so the run is informative even if no
//     breakpoint fires.
//
// Read-only against the guest; every write here is to our own log. Env-gated,
// so the default build is untouched.
#include <algorithm>
#ifndef _WIN32
#include <dirent.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <sys/mman.h>

namespace hwbp {
static uint8_t* g_base = nullptr;
// The scheduler runs TWO contexts: a front-end one (r3=A660C7E0, table
// A6617C80, constant across boots, +36 never populated) and the per-level
// GAMEPLAY one created at ~84s (r3/table are fresh heap each boot). mrg37
// proved the 60-75s MW window only ever saw the front-end — so the table is
// tracked LIVE here and the watch re-arms whenever it changes, instead of
// latching whatever context happened to run first.
static std::atomic<uint32_t> g_tab_live{0};
static uint32_t g_tab = 0;  // the table the current watches are armed on
static std::atomic<bool> g_started{false};
static uintptr_t g_text_lo = 0, g_text_hi = 0;

static const size_t kRingPages = 2;  // 1 header page + 1 data page
static const size_t kDataSize = 4096;

struct Ring {
  int fd = -1;
  void* mm = nullptr;
  int sector = -1;
  int tid = 0;
};
static std::vector<Ring> g_rings;
static std::vector<int> g_armed;

static FILE* Open() { return fopen(restuff_logpath(), "a"); }

static uint32_t AttachWord(int sct) {
  uint32_t w;
  memcpy(&w, g_base + g_tab + 120 + 56 * sct + 36, 4);
  return __builtin_bswap32(w);
}

// A sector is LIVE if it owns work items (the +12..+23 vector triple is set).
// mrg37: only ~10 of the 64 are, and only those ever attach — so watching a
// dead sector's word burns a debug register on an address nothing writes.
static bool IsLive(int sct) {
  const uint8_t* e = g_base + g_tab + 120 + 56 * sct;
  for (int o = 12; o < 24; ++o)
    if (e[o]) return true;
  return false;
}

static void ReadTextRange() {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (!strstr(line, "/restuff")) continue;
    uintptr_t lo = 0, hi = 0;
    char perms[8] = {};
    if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && strchr(perms, 'x')) {
      g_text_lo = lo;
      g_text_hi = hi;
      break;
    }
  }
  fclose(f);
}

static int OpenBp(uintptr_t addr, int tid) {
  perf_event_attr a;
  memset(&a, 0, sizeof(a));
  a.type = PERF_TYPE_BREAKPOINT;
  a.size = sizeof(a);
  a.bp_type = HW_BREAKPOINT_W;
  a.bp_addr = addr;
  a.bp_len = HW_BREAKPOINT_LEN_4;
  a.sample_period = 1;
  a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
  a.exclude_kernel = 1;
  a.exclude_hv = 1;
  return int(syscall(__NR_perf_event_open, &a, tid, -1, -1, 0));
}

static void DisarmAll() {
  for (auto& r : g_rings) {
    if (r.mm) munmap(r.mm, kRingPages * 4096);
    if (r.fd >= 0) close(r.fd);
  }
  g_rings.clear();
}

// Arm `sectors` on every thread that exists right now.
static void ArmSectors(const std::vector<int>& sectors) {
  DisarmAll();
  g_armed = sectors;
  DIR* d = opendir("/proc/self/task");
  if (!d) return;
  int threads = 0, ok = 0, fail = 0;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const int tid = atoi(e->d_name);
    if (!tid) continue;
    ++threads;
    for (int sct : sectors) {
      const uintptr_t addr =
          reinterpret_cast<uintptr_t>(g_base) + g_tab + 120 + 56 * sct + 36;
      const int fd = OpenBp(addr, tid);
      if (fd < 0) { ++fail; continue; }
      void* mm = mmap(nullptr, kRingPages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mm == MAP_FAILED) { close(fd); ++fail; continue; }
      Ring r;
      r.fd = fd; r.mm = mm; r.sector = sct; r.tid = tid;
      g_rings.push_back(r);
      ++ok;
    }
  }
  closedir(d);
  if (FILE* f = Open()) {
    fprintf(f, "HWARM t=%ldms tab=%08X sectors=", long(ibwatch::SinceArmMs()), g_tab);
    for (int s : sectors) fprintf(f, "%d,", s);
    fprintf(f, " threads=%d armed=%d failed=%d text=%016llX\n", threads, ok, fail,
            (unsigned long long)g_text_lo);
    fclose(f);
  }
}

// The ring is a byte-counter window; records can straddle the wrap point.
static void RingCopy(const unsigned char* dbase, uint64_t off, void* dst, size_t n) {
  const size_t o = size_t(off % kDataSize);
  const size_t first = std::min(n, kDataSize - o);
  memcpy(dst, dbase + o, first);
  if (first < n) memcpy(static_cast<unsigned char*>(dst) + first, dbase, n - first);
}

static void DrainRings() {
  for (auto& r : g_rings) {
    auto* mp = static_cast<perf_event_mmap_page*>(r.mm);
    uint64_t head = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = mp->data_tail;
    if (tail == head) continue;
    const unsigned char* dbase = static_cast<unsigned char*>(r.mm) + 4096;
    while (tail < head) {
      perf_event_header h;
      RingCopy(dbase, tail, &h, sizeof(h));
      if (h.size < sizeof(h) || h.size > kDataSize) break;
      if (h.type == PERF_RECORD_SAMPLE) {
        uint64_t ip = 0;
        uint32_t pid = 0, tid = 0;
        RingCopy(dbase, tail + sizeof(h), &ip, 8);
        RingCopy(dbase, tail + sizeof(h) + 8, &pid, 4);
        RingCopy(dbase, tail + sizeof(h) + 12, &tid, 4);
        if (FILE* f = Open()) {
          // ip-text = the offset to symbolize offline against nm output.
          fprintf(f, "HWBP t=%ldms sect=%d ip=%016llX off=%llX tid=%u val=%08X\n",
                  long(ibwatch::SinceArmMs()), r.sector, (unsigned long long)ip,
                  (unsigned long long)(g_text_lo && ip >= g_text_lo ? ip - g_text_lo : 0),
                  tid, AttachWord(r.sector));
          fclose(f);
        }
      }
      tail += h.size;
    }
    __atomic_store_n(&mp->data_tail, head, __ATOMIC_RELEASE);
  }
}

// Prefer LIVE-but-unattached sectors (a wedge's door sector is exactly one of
// those); top up with live-attached ones, whose re-writes — mrg37 boot2 showed
// a clear+set pair at 99-100s — come from the same code path.
static std::vector<int> PickSectors() {
  std::vector<int> pick, spare;
  for (int i = 0; i < 64; ++i) {
    if (!IsLive(i)) continue;
    if (AttachWord(i) == 0) { if (pick.size() < 4) pick.push_back(i); }
    else spare.push_back(i);
  }
  for (size_t k = 0; k < spare.size() && pick.size() < 4; ++k) pick.push_back(spare[k]);
  return pick;
}

static void Poller() {
  uint32_t prev[64] = {};
  const int64_t stop_ms = ibwatch::SinceArmMs() + 180000;
  bool armed_once = false;
  while (ibwatch::SinceArmMs() < stop_ms) {
    const uint32_t live = g_tab_live.load(std::memory_order_relaxed);
    if (live && live != g_tab) {
      // New scheduler context (front-end -> gameplay, or a realloc): the old
      // watches point into a dead table.
      g_tab = live;
      for (int i = 0; i < 64; ++i) prev[i] = AttachWord(i);
      ArmSectors(PickSectors());
      armed_once = true;
    } else if (armed_once) {
      DrainRings();
      bool need_rearm = false;
      for (int i = 0; i < 64; ++i) {
        const uint32_t now = AttachWord(i);
        if (now == prev[i]) continue;
        if (FILE* f = Open()) {
          fprintf(f, "HWTR t=%ldms sect=%d %08X->%08X armed=%d live=%d\n",
                  long(ibwatch::SinceArmMs()), i, prev[i], now,
                  int(std::find(g_armed.begin(), g_armed.end(), i) != g_armed.end()),
                  int(IsLive(i)));
          fclose(f);
        }
        // A transition on a sector we were not watching means the writer ran
        // where no debug register could see it — move the watch.
        if (std::find(g_armed.begin(), g_armed.end(), i) == g_armed.end())
          need_rearm = true;
        prev[i] = now;
      }
      if (need_rearm) {
        std::vector<int> next = PickSectors();
        if (!next.empty() && next != g_armed) ArmSectors(next);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  DrainRings();
  DisarmAll();
  if (FILE* f = Open()) {
    fprintf(f, "HWEND t=%ldms\n", long(ibwatch::SinceArmMs()));
    fclose(f);
  }
}

// Called from the merge-scheduler hook on EVERY pass: publishes the table the
// scheduler is actually working on (cheap), and starts the poller once.
void Note(uint8_t* base, uint32_t tab) {
  g_base = base;
  g_tab_live.store(tab, std::memory_order_relaxed);
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;
  ReadTextRange();
  std::thread(Poller).detach();
}
}  // namespace hwbp
#else   // _WIN32: perf-event hardware breakpoints are Linux diagnostic tooling
namespace hwbp {
void Note(uint8_t*, uint32_t) {}
}  // namespace hwbp (win32 stub)
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// M3.222 (RESTUFF_KEYWATCH=1): NAME THE WRITER of byte23 bit2.
//
// M3.221 settled the question that mattered: those keys are MUTATED, not
// created set — 48 key objects seen CLEAR were later found SET (04 and 06),
// every one of them in a single burst at t=74131ms. A write to a knowable
// address is exactly what a hardware breakpoint catches, so arm on byte 23 of
// a few still-clear key objects before the burst and let the writer identify
// itself.
//
// Byte granularity (the hwbp watch above is 4-byte and sector-table specific),
// and per-thread like all x86 debug registers. Kept separate rather than
// refactoring hwbp: that one is load-bearing for a different probe.
#ifndef _WIN32
namespace keywatch {
static const size_t kPages = 2;
static const size_t kData = 4096;
struct W { int fd = -1; void* mm = nullptr; uint32_t addr = 0; };
static std::vector<W> g_w;
static uintptr_t g_text = 0;
static std::atomic<bool> g_started{false};

static void ReadText() {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return;
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    if (!strstr(line, "/restuff")) continue;
    uintptr_t lo = 0, hi = 0; char perms[8] = {};
    if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 && strchr(perms, 'x')) {
      g_text = lo; break;
    }
  }
  fclose(f);
}

static void Drain() {
  for (auto& w : g_w) {
    auto* mp = static_cast<perf_event_mmap_page*>(w.mm);
    uint64_t head = __atomic_load_n(&mp->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = mp->data_tail;
    const unsigned char* db = static_cast<unsigned char*>(w.mm) + 4096;
    while (tail < head) {
      perf_event_header h;
      const size_t o = size_t(tail % kData);
      memcpy(&h, db + o, sizeof(h) <= kData - o ? sizeof(h) : 0);
      if (h.size < sizeof(h) || h.size > kData) break;
      if (h.type == PERF_RECORD_SAMPLE && o + sizeof(h) + 16 <= kData) {
        uint64_t ip = 0; uint32_t pid = 0, tid = 0;
        memcpy(&ip, db + o + sizeof(h), 8);
        memcpy(&pid, db + o + sizeof(h) + 8, 4);
        memcpy(&tid, db + o + sizeof(h) + 12, 4);
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "KEYW t=%ldms guest=%08X ip=%016llX off=%llX tid=%u\n",
                  long(ibwatch::SinceArmMs()), w.addr, (unsigned long long)ip,
                  (unsigned long long)(g_text && ip >= g_text ? ip - g_text : 0), tid);
          fclose(f);
        }
      }
      tail += h.size;
    }
    __atomic_store_n(&mp->data_tail, head, __ATOMIC_RELEASE);
  }
}

// addrs = guest addresses of the byte to watch (key object + 23).
void Arm(uint8_t* base, const std::vector<uint32_t>& addrs) {
  bool expected = false;
  if (!g_started.compare_exchange_strong(expected, true)) return;
  ReadText();
  DIR* d = opendir("/proc/self/task");
  if (!d) return;
  int threads = 0, ok = 0, fail = 0;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] == '.') continue;
    const int tid = atoi(e->d_name);
    if (!tid) continue;
    ++threads;
    for (uint32_t ga : addrs) {
      perf_event_attr a; memset(&a, 0, sizeof(a));
      a.type = PERF_TYPE_BREAKPOINT; a.size = sizeof(a);
      a.bp_type = HW_BREAKPOINT_W;
      a.bp_addr = reinterpret_cast<uintptr_t>(base) + ga;
      a.bp_len = HW_BREAKPOINT_LEN_1;
      a.sample_period = 1;
      a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
      a.exclude_kernel = 1; a.exclude_hv = 1;
      const int fd = int(syscall(__NR_perf_event_open, &a, tid, -1, -1, 0));
      if (fd < 0) { ++fail; continue; }
      void* mm = mmap(nullptr, kPages * 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (mm == MAP_FAILED) { close(fd); ++fail; continue; }
      W w; w.fd = fd; w.mm = mm; w.addr = ga;
      g_w.push_back(w);
      ++ok;
    }
  }
  closedir(d);
  if (FILE* f = fopen(restuff_logpath(), "a")) {
    fprintf(f, "KEYWARM t=%ldms watching=%zu threads=%d armed=%d failed=%d text=%016llX addrs=",
            long(ibwatch::SinceArmMs()), addrs.size(), threads, ok, fail,
            (unsigned long long)g_text);
    for (uint32_t ga : addrs) fprintf(f, "%08X,", ga);
    fprintf(f, "\n");
    fclose(f);
  }
  std::thread([] {
    const int64_t stop = ibwatch::SinceArmMs() + 120000;
    while (ibwatch::SinceArmMs() < stop) {
      Drain();
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    Drain();
  }).detach();
}
}  // namespace keywatch
#else   // _WIN32: same Linux-only watch tooling as hwbp above
namespace keywatch {
void Arm(uint8_t*, const std::vector<uint32_t>&) {}
}  // namespace keywatch (win32 stub)
#endif  // !_WIN32

// ---------------------------------------------------------------------------
// M3.223 (RESTUFF_LISTLOG=1): the light-list FILTER at the composer.
//
// sub_82AD3050's prologue (recomp 50.cpp:61809-61878) names its arguments:
// r5 = SOURCE light list (bytes at +0, count at +64), r6 = DEST list it
// fills, r7 = the key object (r7+16 = the mask qword the wedge hinges on),
// and *(r4+8) = a table of per-light entries whose +96 holds a 128-bit
// vector. The first loop FILTER-COPIES: for each light b in source, it loads
// that light's vector and calls sub_82B953C8(ctx, vec); only one outcome
// appends b to dest. So which lights a merged chunk keeps is decided by that
// per-light test — and the wedge is door chunks KEEPING light 2 where clean
// boots drop it.
//
// This logs, for composer calls in the window: the source list, the dest list
// after the call, the key pointer, and each source light's 16-byte vector.
// One batch answers both open questions at once: does light 2's VECTOR differ
// across families (data root), or is the vector identical and the test's
// other input (the node bounds in r3) what differs (geometry/bounds root)?
// M3.232 (RESTUFF_LISTFIX=1): THE ROOT FIX — publish the node light list
// ATOMICALLY instead of reset-then-append.
//
// sub_82B0CBC8 (recomp 52.cpp:51062) rebuilds a node's light list as:
//     *(r3+64) = 0;                       // count reset
//     for (i...) if (bit i of *(r4+16)) list[count++] = i;
// The composer (sub_82AD3050) reads that list from ANOTHER thread, so it can
// observe it mid-rebuild — count==1 with only index 2 appended is the [2]
// downgrade that poisons the merge key and causes the wedge. Named by the
// LISTWATCH hardware watchpoint (117851 hits).
//
// Fix: run the rebuild with the count word held at its OLD value, then store
// the final count once at the end. Readers therefore see either the old
// complete list or the new complete list, never a torn one. Implemented by
// snapshotting the count before, letting the guest run, and (since the guest
// writes the bytes in place) restoring a consistent count last — the byte
// array is rebuilt identically each pass, so publishing the final count is
// the single ordering point that matters.
// Set while sub_82B0CBC8 is rebuilding a node light list; read by the composer
// hook so it never bakes a half-appended list into a merge key.
static std::atomic<int> g_list_rebuilding{0};

// M3.233 (RESTUFF_MASKLOG=1): prove the bitmask model and locate its owner.
//
// The light "list" is a per-call STACK temp rebuilt here by filtering the
// 64-bit light bitmask at *(r4+16); everything runs on one thread (tid data),
// so there is no torn read — a [2] list means the BITMASK ITSELF was
// bit-2-only at that instant. This logs the owner (r4), the mask value, and
// the resulting count, always keeping single-bit cases. Confirms the model and
// hands the owner address to a hardware watch on +16, which will name which of
// sub_82AD8598/8600/8668/86D0/92D0 writes the collapsed value.
REX_EXTERN(__imp__sub_82B0CBC8);
REX_HOOK_RAW(sub_82B0CBC8) {
  {
    static const bool ml = getenv("RESTUFF_MASKLOG") != nullptr;
    if (ml) {
      const uint32_t owner = ctx.r4.u32, lst = ctx.r3.u32;
      uint64_t mask = 0;
      if (owner >= 0x10000 && owner < 0xC0000000u - 24) {
        std::memcpy(&mask, base + owner + 16, 8);
        mask = __builtin_bswap64(mask);
      }
      const int bits = __builtin_popcountll(mask);
      static std::atomic<uint32_t> n{0}, few{0};
      const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
      const bool single = bits > 0 && bits <= 2;
      // M3.244: ALSO keep one sample per second. This mask (owner+16) is the
      // very word sub_82AD3460's compose gate tests, and the late window is what
      // decides the wedge: clean boots reach mask==0 (gate SKIPS, composition
      // stops) while wedge boots keep bit 2 set (mask==0x04, composes forever).
      // The old filter fills its 300-sample low-bit budget early, so late data
      // arrived only via the 1-in-20000 rule -- just TWO samples per boot, which
      // is too thin to call a root proven. One per second makes the late window
      // dense at negligible cost.
      long t_now = long(ibwatch::SinceArmMs());
      static std::atomic<long> s_sec{-1};
      long prev_sec = s_sec.load(std::memory_order_relaxed);
      const bool tick = t_now > 0 && (t_now / 1000) != prev_sec &&
                        s_sec.compare_exchange_strong(prev_sec, t_now / 1000);
      if (k < 20 || tick ||
          (single && few.fetch_add(1, std::memory_order_relaxed) < 300) ||
          (k % 20000) == 0) {
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "MASK#%u t=%ldms owner=%08X list=%08X bits=%d mask=%016llX\n", k,
                  long(ibwatch::SinceArmMs()), owner, lst, bits,
                  (unsigned long long)mask);
          fclose(f);
        }
      }
    }
  }
  static const bool on = getenv("RESTUFF_LISTFIX") != nullptr;
  if (!on) { __imp__sub_82B0CBC8(ctx, base); return; }
  const uint32_t list = ctx.r3.u32;
  uint32_t before = 0;
  const bool ok = list >= 0x10000 && list < 0xC0000000u - 140;
  if (ok) {
    std::memcpy(&before, base + list + 64, 4);  // BE, kept as-is
  }
  // ⚠️ Restoring the LONGER count (the first design) is WRONG: shrinks are
  // NORMAL. Settled, never-downgrading boots still show ~223-236 shrinks and
  // ~214-223 grows per boot as lights move in and out of node ranges — so
  // blocking shrinks would pin stale lights permanently and mis-light the
  // world to cure the wedge. The real distinction is TORN vs SETTLED, not
  // short vs long.
  //
  // Seqlock-style publish instead: hold the count at 0 for the duration of the
  // rebuild (a reader seeing 0 skips/uses nothing rather than a partial list),
  // then store the true final count once. Readers observe either "empty,
  // rebuilding" or the complete new list — never a half-appended one.
  // ⚠️ Pre-zeroing the count here is ALSO useless: the guest's own rebuild
  // already starts by zeroing and then appends incrementally, so the torn
  // window IS that append loop, inside the call. Nothing done before or after
  // __imp__ can make it atomic from the outside.
  //
  // The fix has to sit on the READER side, where we do control the timing:
  // the composer (sub_82AD3050) must not consume a list while a rebuild is in
  // flight. RESTUFF_LISTFIX therefore only MARKS the in-flight window here;
  // the composer hook checks the mark and, when set, leaves the previously
  // published mask alone instead of baking a partial list into the merge key.
  // (Mark is host-side state — no guest memory is touched, so a mistake here
  // cannot corrupt the title.)
  g_list_rebuilding.fetch_add(1, std::memory_order_acq_rel);
  __imp__sub_82B0CBC8(ctx, base);
  g_list_rebuilding.fetch_sub(1, std::memory_order_acq_rel);
  uint32_t after = 0;
  std::memcpy(&after, base + list + 64, 4);
  const uint32_t b = __builtin_bswap32(before), a = __builtin_bswap32(after);
  static std::atomic<uint32_t> n{0};
  const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
  if (k < 8 || (k % 50000) == 0) {
    if (FILE* f = fopen(restuff_logpath(), "a")) {
      fprintf(f, "LISTFIX#%u t=%ldms list=%08X %u->%u (rebuild window marked)\n", k,
              long(ibwatch::SinceArmMs()), list, b, a);
      fclose(f);
    }
  }
}

// M3.243 counters (declared here because the composer hook below is the place
// g_comp_calls has to be bumped; see the block near sub_82AD3460 for why).
static std::atomic<uint64_t> g_rec_calls{0}, g_comp_calls{0};
REX_EXTERN(__imp__sub_82AD3050);
REX_HOOK_RAW(sub_82AD3050) {
  g_comp_calls.fetch_add(1, std::memory_order_relaxed);
  static const bool on = getenv("RESTUFF_LISTLOG") != nullptr;
  if (!on) { __imp__sub_82AD3050(ctx, base); return; }
  const uint32_t r4 = ctx.r4.u32, src = ctx.r5.u32, dst = ctx.r6.u32, key = ctx.r7.u32;
  // M3.238: ctx.lr is CLOBBERED by the __imp__ call below, so snapshot the
  // caller here, not at the fprintf.
  const uint32_t lr0 = uint32_t(ctx.lr);
  const long t = long(ibwatch::SinceArmMs());
  auto rd32 = [&](uint32_t a) {
    uint32_t v = 0;
    if (a >= 0x10000 && a < 0xC0000000u - 4) { std::memcpy(&v, base + a, 4); v = __builtin_bswap32(v); }
    return v;
  };
  uint32_t nsrc = 0;
  uint8_t sl[16] = {};
  const bool win = t >= 60000 && t <= 200000;
  if (win && src >= 0x10000 && src < 0xC0000000u - 80) {
    nsrc = rd32(src + 64);
    if (nsrc > 16) nsrc = 16;
    std::memcpy(sl, base + src, nsrc);
  }
  // M3.231 (RESTUFF_LISTWATCH=1): NAME THE DOWNGRADE WRITER. Collect a few
  // source lists seen FULL (count>=3) before ~73s, then hardware-watch each
  // list's count word (srcp+64). The writer that later RESETS the count to 1
  // (the perpetual full->[2] downgrade) faults and names itself in KEYW. Uses
  // the proven keywatch pattern; the count word is a stable per-list address.
  {
    static const bool lw = getenv("RESTUFF_LISTWATCH") != nullptr;
    if (lw && win) {
      static std::mutex mu;
      static std::vector<uint32_t> full_counts;
      static std::atomic<bool> armed{false};
      if (!armed.load(std::memory_order_relaxed)) {
        if (t < 73000) {
          if (nsrc >= 3 && src >= 0x10000 && src < 0xC0000000u - 68) {
            std::lock_guard<std::mutex> lk(mu);
            const uint32_t cw = src + 64;
            if (full_counts.size() < 4 &&
                std::find(full_counts.begin(), full_counts.end(), cw) == full_counts.end())
              full_counts.push_back(cw);
          }
        } else {
          bool e = false;
          if (armed.compare_exchange_strong(e, true)) {
            std::lock_guard<std::mutex> lk(mu);
            if (!full_counts.empty()) keywatch::Arm(base, full_counts);
          }
        }
      }
    }
  }
  __imp__sub_82AD3050(ctx, base);
  if (!win || !nsrc) return;
  // Decimate, but ALWAYS keep calls whose source contains light 2 (capped) —
  // those are the decisive ones and they are rare on clean boots.
  static std::atomic<uint32_t> s_n{0}, s_l2{0};
  const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
  bool has2 = false;
  for (uint32_t i = 0; i < nsrc; ++i) has2 |= sl[i] == 2;
  // M3.234 (RESTUFF_LLBURST=<ms>): log EVERY call for one burst once t passes
  // the threshold. The %997 decimation keeps roughly ONE sample per frame,
  // which cannot answer the decisive question about the light indices: are
  // they node-local ordinals (so the merger key's byte23 = OR(1<<idx) compares
  // values that mean different physical lights in different records -- a real
  // key bug), or indices into a per-frame global table (so the comparison is
  // valid and byte23 differences are legitimate)? Both predict the observed
  // "same index, different light, 33ms apart", because 33ms is a frame apart.
  // Only MANY nodes sampled within the SAME frame separate them: if two src
  // lists in one frame disagree on what index k is, the ordinals are local.
  static const char* burst_env = getenv("RESTUFF_LLBURST");
  static const long burst_t = burst_env ? atol(burst_env) : 0;
  static std::atomic<uint32_t> s_burst{0};
  const bool burst = burst_t && t >= burst_t &&
                     s_burst.fetch_add(1, std::memory_order_relaxed) < 6000;
  const bool keep = burst || (n % 997) == 0 ||
                    (has2 && s_l2.fetch_add(1, std::memory_order_relaxed) < 400);
  if (!keep) return;
  uint32_t ndst = 0;
  uint8_t dl[16] = {};
  if (dst >= 0x10000 && dst < 0xC0000000u - 80) {
    ndst = rd32(dst + 64);
    if (ndst > 16) ndst = 16;
    std::memcpy(dl, base + dst, ndst);
  }
  if (FILE* f = fopen(restuff_logpath(), "a")) {
    // M3.230: also log the source-list POINTER (r5) — the downgrade root hunt
    // needs it: the [2] list is threaded down from the top of the composer
    // recursion (r5=r6 in sub_82AD3288), so the writer that RESETS the list to
    // one entry is upstream. With the list's address, a hardware watchpoint on
    // its count word (+64) names that writer directly (keywatch pattern).
    // M3.238: also log r4 (the owner being composed) and the CALLER lr. The
    // Aug-11 ll1 re-analysis located the wedge's discriminator exactly: all
    // boots compose identically from 60-100s, then at t~100s composition either
    // STOPS DEAD (clean: last LL at 100.2s though the process lives to 260s) or
    // continues at a steady ~145 samples/10s to the end (wedge). It is not one
    // stuck node -- the wedge boot recomposes 482 distinct keys late, ~half
    // taking the light-2 exclusion (dst '' 761 / '2,' 613), which is exactly
    // the 76/87 byte-23 split KEYHIST sees. So the question is WHO keeps
    // driving the pass after 100s, and the call site answers it.
    fprintf(f, "LL#%u t=%ldms key=%08X srcp=%08X r4=%08X lr=%08X src=[", n, t,
            key, src, r4, lr0);
    for (uint32_t i = 0; i < nsrc; ++i) fprintf(f, "%u,", sl[i]);
    fprintf(f, "] dst=[");
    for (uint32_t i = 0; i < ndst; ++i) fprintf(f, "%u,", dl[i]);
    fprintf(f, "]");
    // Each source light's 128-bit vector at *(lighttab + 4*b) + 96.
    const uint32_t tab = rd32(r4 + 8);
    for (uint32_t i = 0; i < nsrc && tab; ++i) {
      const uint32_t e = rd32(tab + 4u * sl[i]);
      fprintf(f, " L%u=", sl[i]);
      if (e >= 0x10000 && e < 0xC0000000u - 112) {
        for (int q = 0; q < 16; ++q) fprintf(f, "%02X", *(base + e + 96 + q));
      } else {
        fprintf(f, "BADPTR");
      }
    }
    fprintf(f, "\n");
    fclose(f);
  }
}

// M3.156 (RESTUFF_LULOG=1): guest LoadUnitStreamer observation. The low-LOD
// "water wedge" boots are a race: the level's load units stream during the
// intro cutscene, gameplay starting early freezes whatever tier finished, and
// the streamer never retries (task #24). These hooks log every load unit the
// LU-reader thread steps (state machine field at LU+52, states 0..6; name via
// *(LU+28)+20 -- the state-6 branch of sub_82B760B8 reads it the same way),
// so a FULL-vs-LOW boot diff shows exactly which LUs a bad boot never
// processes -- and where the enqueue gate for the fix lives.
static inline uint32_t LuLoadBE32(uint8_t* base, uint32_t va) {
  if (va < 0x10000 || va >= 0xC0000000u) return 0;
  uint32_t v;
  std::memcpy(&v, base + va, 4);
  return __builtin_bswap32(v);
}

static void LuLog(const char* stage, PPCContext& ctx, uint8_t* base) {
  // M3.187 (RESTUFF_IBWATCH_LATE=<draw-hook calls>): arm ONCE after load
  // (proxied by N draw-entry calls), never re-arm — the reprotect storm over
  // actively-written pages hangs the guest (pacer3/4); a single late arm
  // catches first-writers of the gameplay era safely (the load-arm variant
  // was proven safe by pacer1).
  ibwatch::Arm(base);
  ibwatch::TryLateMs();
  ibwatch::Rearm();
  ibwatch::Drain();
  // M3.183 (RESTUFF_FORCE_1E9C=0|1): force the graphics-init caps byte that
  // gates the dynamic-rebuild path at BOTH levels (sub_8297E5B0 walk +
  // sub_8297E410 per-object). The value that kills the A path WHILE terrain
  // renders is hardware's; then the ctor's setter (8297F5D0/E8) names the
  // upstream cap. Re-asserted from this hook (fires all load) so the ctor's
  // own write cannot stick the other value.
  {
    static const char* fv = getenv("RESTUFF_FORCE_1E9C");
    if (fv) base[0x83341E9C] = uint8_t(fv[0] == '1');
  }
  static const bool on = getenv("RESTUFF_LULOG") != nullptr;
  if (!on) return;
  const uint32_t lu = ctx.r3.u32;
  const uint32_t state = LuLoadBE32(base, lu + 52) & 0x7FFFFFFF;
  const uint32_t name_obj = LuLoadBE32(base, lu + 28);
  const uint32_t name_ptr = name_obj ? LuLoadBE32(base, name_obj + 20) : 0;
  char name[64];
  name[0] = '?';
  name[1] = 0;
  if (name_ptr >= 0x10000 && name_ptr < 0xC0000000u) {
    std::memcpy(name, base + name_ptr, 63);
    name[63] = 0;
    for (char* c = name; *c; ++c) {
      if (!isprint(static_cast<unsigned char>(*c))) {
        *c = 0;
        break;
      }
    }
  }
  REXLOG_INFO("[LULOG] {} lu={:08X} state={} name={}", stage, lu, state, name);
  // M3.215: mirror into the mergelog on the ARM clock. The streaming timeline
  // (REXLOG, wall clock, and shed by the 5MB rotation) and the merge timeline
  // (mergelog, arm-relative ms) could not be compared before, which is exactly
  // the comparison the frozen-at-load result calls for: the burst is a
  // consumer and the streamer is its producer, so whether the last unit lands
  // BEFORE or AFTER the 65-70s burst is the mechanism, per boot.
  static const bool mirror = getenv("RESTUFF_MERGELOG") != nullptr;
  if (mirror) {
    if (FILE* f = fopen(restuff_logpath(), "a")) {
      fprintf(f, "LU t=%ldms %s lu=%08X state=%u name=%s\n", long(ibwatch::SinceArmMs()),
              stage, lu, state, name);
      fclose(f);
    }
  }
}

// M3.178 (RESTUFF_PARKLOG=1): the LU placer's backpressure park. On dest-full
// the placer job waits INFINITELY on a kernel object (sub_82B9C690 ->
// WaitForSingleObject(handle, -1)) and can be woken either by SPACE (drain
// freed the buffer -> placement continues -> family B) or by ABORT (v0[9]
// set, job state CAS'd to 2 -> tail never places -> family A's missing
// stitch). Log every park with the job pointer and, after the wait returns,
// the abort flag -- direct dynamic proof of which wake reason each boot got,
// plus timing. r3 = job's wait-handle slot (&job[8] per the decompile; the
// wrapper receives a1 = &v0[8]... actually a1 IS v0[8]'s address holder:
// sub_82B9C690(a1){ return Wait(*a1, -1); } receives v0[8] = a POINTER to
// the handle. The hook logs the handle value and the JOB base derived from
// it (job = a1 - 32 bytes when called with &job[8]).
// M3.181 (RESTUFF_CMDCENSUS=1): per-handler LU command census via the ONE
// shared ring-copy helper (raw asm blob sub_8308F720, called through
// byte_8308F718[8] by every command handler and the ring drain). Direct
// hooks on the handlers are useless (indirect dispatch bypasses routing;
// PLACELOG proved 0 hits) -- but the CRT memcpy sub_8308FC60 is reached by
// DIRECT calls (v1 hooked the ring-element helper sub_8308F720: exactly ONE
// handler-range call per boot -- wrong chokepoint; the watchpoint chains
// always showed sub_8308FC60 as the copy), so
// its guest LR identifies the caller. Histogram LRs in the handler module
// range; drain to the side file. A handler whose count differs between
// wedge and clean boots names the diverging COMMAND.
REX_EXTERN(__imp__sub_8308FC60);
REX_HOOK_RAW(sub_8308FC60) {
  static const bool on = getenv("RESTUFF_CMDCENSUS") != nullptr;
  if (on) {
    const uint32_t lr = uint32_t(ctx.lr);
    // No LR filter: the handlers TAIL-JUMP into memcpy (LR then points at
    // the ring drain, not the handler), so a handler-range filter sees
    // nothing (v2 produced an EMPTY side file). Log the full spectrum with a
    // capped map; the drain interval keeps the volume safe.
    {
      static std::mutex m;
      static std::map<uint32_t, uint64_t> counts;
      static uint64_t total = 0;
      std::lock_guard<std::mutex> lk(m);
      if (counts.size() < 4096) ++counts[lr];
      if ((++total % 200000) == 1) {
        if (FILE* f = fopen("/tmp/restuff_drive/cmdcensus.txt", "a")) {
          fprintf(f, "census t=%lld total=%llu:",
                  (long long)std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count(),
                  (unsigned long long)total);
          for (const auto& [l, n] : counts) fprintf(f, " %08X=%llu", l, (unsigned long long)n);
          fprintf(f, "\n");
          fclose(f);
        }
      }
    }
  }
  __imp__sub_8308FC60(ctx, base);
}

// M3.184 (RESTUFF_DESCLOG=1): log the 36-byte tree-build DESCRIPTORS. The
// within-fine writer chains are byte-identical between wedge and clean fine
// boots (ibw73 vs ibw72: identical histograms) -- the builders differ in the
// DATA they are fed, not the path. sub_82ADB160 (directly called from
// 829E688C/829E75D8 -> hookable) copies the descriptor twice then builds;
// r4 holds the source descriptor pointer per the decompile idiom (copy from
// (r4) via sub_829CF4F0(v1, v0-in-r4...)). Log 36 bytes from r4 and r5 to
// the side file per call; diffing descriptor SETS across boots names the
// A-vs-B grouping selector input.
REX_EXTERN(__imp__sub_82ADB160);
REX_HOOK_RAW(sub_82ADB160) {
  static const bool on = getenv("RESTUFF_DESCLOG") != nullptr;
  if (on) {
    // v2: ATOMIC single-write lines (v1's multi-fprintf raced across threads
    // and garbled the file) + a per-process sequence cap so only the LOAD-era
    // calls log (~12 builds vs 42k/boot per-frame traffic).
    static std::atomic<uint32_t> s_seq{0};
    const uint32_t seq = s_seq.fetch_add(1, std::memory_order_relaxed);
    if (seq < 600) {
      char line[256];
      int o = snprintf(line, sizeof(line), "desc#%u r4=%08X |", seq, ctx.r4.u32);
      const uint32_t src = ctx.r4.u32;
      if (src >= 0x10000 && src < 0xC0000000u)
        for (int i = 0; i < 36 && o < int(sizeof(line)) - 3; ++i)
          o += snprintf(line + o, sizeof(line) - o, "%02X", base[src + i]);
      o += snprintf(line + o, sizeof(line) - o, "\n");
      static std::mutex m;
      static FILE* f = fopen("/tmp/restuff_drive/desclog.txt", "a");
      if (f) {
        std::lock_guard<std::mutex> lk(m);
        fwrite(line, 1, size_t(o), f);
        fflush(f);
      }
    }
  }
  __imp__sub_82ADB160(ctx, base);
}

// M3.182 (RESTUFF_NO_DYNTREE=1): skip the load-time dynamic spatial-tree
// build loop (sub_829E7538: for i < *(obj+60) do build). The watchpoint
// chains prove this loop runs ONLY in wedge (family A) boots (~12 registered
// items; clean boots have count==0), and its output is the A partition whose
// proven ground-stitch gap IS the wedge. Skipping it forces every boot onto
// the count==0 path -- the CLEAN build. If camera-matched fine boots come
// out clean under this switch, the causal loop is closed end to end and the
// skip stands as a native pin while the ~12-item REGISTRATION (the true
// root) is traced. Direct static call site exists (8297E494) so the hook
// routes.
REX_EXTERN(__imp__sub_829E7538);
REX_HOOK_RAW(sub_829E7538) {
  static const bool skip = getenv("RESTUFF_NO_DYNTREE") != nullptr;
  if (skip) {
    static std::atomic<int> s_n{0};
    if (s_n.fetch_add(1, std::memory_order_relaxed) < 8)
      REXLOG_INFO("[NODYNTREE] skipped dynamic tree-build loop");
    return;
  }
  __imp__sub_829E7538(ctx, base);
}

// M3.179 (RESTUFF_PLACELOG=1): the LU PLACER's completion state. Entry logs
// the job (r3) cursor/end; exit logs cursor/end/abort. A job exiting with
// cursor < end and abort set IS the truncated placement (family A's missing
// tail). Far sharper than PARKLOG (sub_82B9C690 proved to be the engine's
// generic event wait -- 45k calls/boot).
REX_EXTERN(__imp__sub_82B66108);
REX_HOOK_RAW(sub_82B66108) {
  static const bool on = getenv("RESTUFF_PLACELOG") != nullptr;
  const uint32_t job = ctx.r3.u32;
  uint32_t pre[2] = {0, 0};
  if (on && job >= 0x10000 && job < 0xC0000000u) {
    std::memcpy(pre, base + job + 12, 8);  // v0[3]=cursor, v0[4]=end (BE)
    pre[0] = __builtin_bswap32(pre[0]);
    pre[1] = __builtin_bswap32(pre[1]);
  }
  __imp__sub_82B66108(ctx, base);
  if (on && job >= 0x10000 && job < 0xC0000000u) {
    uint32_t post[2] = {0, 0}, abrt = 0, dest = 0;
    std::memcpy(post, base + job + 12, 8);
    std::memcpy(&abrt, base + job + 36, 4);
    std::memcpy(&dest, base + job + 8, 4);   // v0[2] = dest cursor
    post[0] = __builtin_bswap32(post[0]);
    post[1] = __builtin_bswap32(post[1]);
    abrt = __builtin_bswap32(abrt);
    dest = __builtin_bswap32(dest);
    const bool complete = post[0] >= post[1];
    static std::atomic<int> s_budget{400};
    if ((!complete || abrt) && s_budget.fetch_sub(1, std::memory_order_relaxed) > 0)
      REXLOG_INFO("[PLACELOG] job={:08X} INCOMPLETE cur={}/{} (pre {}/{}) abort={} dest=0x{:X}",
                  job, post[0], post[1], pre[0], pre[1], abrt, dest);
    else {
      static std::atomic<uint32_t> s_done{0};
      const uint32_t d = s_done.fetch_add(1, std::memory_order_relaxed) + 1;
      if ((d % 200) == 1)
        REXLOG_INFO("[PLACELOG] complete#{} job={:08X} cur={}/{}", d, job, post[0], post[1]);
    }
  }
}

REX_EXTERN(__imp__sub_82B9C690);
REX_HOOK_RAW(sub_82B9C690) {
  // M3.180: PARKLOG v2 -- LR-filtered. v1 logged 45k parks/boot (this is the
  // engine's generic event wait) and derived a bogus job pointer (the arg is
  // the VALUE of job[8], a pointer to a handle -- no job base recoverable).
  // The PLACER's own parks are identified by the guest LR: return address
  // inside sub_82B66108..sub_82B663B8. Those are the dest-full backpressure
  // parks of the LU placement; their count, timing and durations per boot are
  // the family discriminator. (Direct PLACELOG hooking failed: the placer is
  // only invoked THROUGH THE COMMAND DISPATCH TABLE -- indirect calls bypass
  // REX_HOOK_RAW routing; zero hook hits across 3 boots proved it.)
  static const bool on = getenv("RESTUFF_PARKLOG") != nullptr;
  const uint32_t lr = uint32_t(ctx.lr);
  const bool placer = lr >= 0x82B66108u && lr < 0x82B66400u;
  uint64_t t0 = 0;
  if (on && placer) {
    t0 = std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  __imp__sub_82B9C690(ctx, base);
  if (on && placer) {
    const uint64_t t1 = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count();
    REXLOG_INFO("[PARKLOG2] placer park lr={:08X} waited_us={}", lr,
                (unsigned long long)(t1 - t0));
    // Side file too -- the 5MB log rotation shed the whole park2 batch's
    // per-boot profiles minutes after capture (same lesson as IBWATCH).
    static FILE* f = fopen("/tmp/restuff_drive/parklog.txt", "a");
    if (f) {
      fprintf(f, "%lld park lr=%08X us=%llu\n",
              (long long)std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::steady_clock::now().time_since_epoch()).count(),
              lr, (unsigned long long)(t1 - t0));
      fflush(f);
    }
  }
}

REX_EXTERN(__imp__sub_82B75B58);
REX_HOOK_RAW(sub_82B75B58) {
  LuLog("open", ctx, base);
  __imp__sub_82B75B58(ctx, base);
}
REX_EXTERN(__imp__sub_82B6BD90);
REX_HOOK_RAW(sub_82B6BD90) {
  LuLog("read", ctx, base);
  __imp__sub_82B6BD90(ctx, base);
}
REX_EXTERN(__imp__sub_82B69318);
REX_HOOK_RAW(sub_82B69318) {
  LuLog("parse", ctx, base);
  const uint32_t lu = ctx.r3.u32;
  __imp__sub_82B69318(ctx, base);
  ctx.r3.u32 = lu;  // scratch for the post-log only; caller ignores r3
  LuLog("parsed", ctx, base);
}

// M3.157 (RESTUFF_DRAWBT=1): guest CALLER attribution for the two D3D
// VB-draw entries (sub_82F38988 = DrawVertices-like, sub_82F38D78 =
// DrawIndexedVertices-like; found via the DRAW_INDX 0x2200 packet immediates,
// task #24). Logs each UNIQUE (entry, lr, prim) once with a short backchain
// walk -- diffing the caller sets between a FULL-tier and LOW-tier boot names
// the scene-graph node path that emits 79-vs-285 terrain draws, which leads
// to the representation-choice branch (the root of the water-wedge coin).
static void DrawBt(const char* which, PPCContext& ctx, uint8_t* base) {
  static const bool on = getenv("RESTUFF_DRAWBT") != nullptr;
  if (!on) return;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t prim = ctx.r4.u32;
  const uint64_t key = (uint64_t(lr) << 8) ^ prim ^ (which[0] == 'i' ? 0x80 : 0);
  static std::mutex mu;
  static std::unordered_set<uint64_t> seen;
  {
    std::lock_guard<std::mutex> lk(mu);
    if (!seen.insert(key).second) return;
  }
  // short guest backchain walk: [r1] -> backchain, LR slot at bc+4? On this
  // ABI the saved LR of the CALLER's caller sits at [bc + 4] after mflr
  // spill; walk defensively and just print what looks like code addresses.
  uint32_t bc = ctx.r1.u32;
  char chain[160];
  int off = 0;
  for (int d = 0; d < 5 && bc >= 0x10000 && bc < 0xC0000000u; ++d) {
    uint32_t nbc, cand;
    std::memcpy(&nbc, base + bc, 4);
    nbc = __builtin_bswap32(nbc);
    if (nbc <= bc || nbc >= 0xC0000000u) break;
    std::memcpy(&cand, base + nbc + 4, 4);
    cand = __builtin_bswap32(cand);
    if (cand >= 0x82000000 && cand < 0x83400000) {
      off += snprintf(chain + off, sizeof(chain) - off, " %08X", cand);
      if (off > 140) break;
    }
    bc = nbc;
  }
  REXLOG_INFO("[DRAWBT] {} lr={:08X} prim={} r5={} r6={} chain{}", which, lr, prim, ctx.r5.u32,
              ctx.r6.u32, chain);
}

REX_EXTERN(__imp__sub_82F38988);
REX_HOOK_RAW(sub_82F38988) {
  {
    static const uint32_t late = [] {
      const char* e = getenv("RESTUFF_IBWATCH_LATE");
      return e ? uint32_t(strtoul(e, nullptr, 0)) : 0u;
    }();
    if (late) {
      static std::atomic<uint32_t> s_calls{0};
      const uint32_t c = s_calls.fetch_add(1, std::memory_order_relaxed) + 1;
      if (c == late) ibwatch::ArmLate();
    }
  }
  ibwatch::TryLateMs();
  ibwatch::Rearm();
  ibwatch::Drain();
  DrawBt("draw", ctx, base);
  restuff::renderer::D3dCensusHit(restuff::renderer::kFnDrawF);  // M4.39
  __imp__sub_82F38988(ctx, base);
}
REX_EXTERN(__imp__sub_82F38D78);
REX_HOOK_RAW(sub_82F38D78) {
  ibwatch::TryLateMs();
  ibwatch::Rearm();
  ibwatch::Drain();
  DrawBt("idraw", ctx, base);
  restuff::renderer::D3dCensusHit(restuff::renderer::kFnDrawG);  // M4.39
  __imp__sub_82F38D78(ctx, base);
}

// M3.190 (RESTUFF_MERGELOG=1): merge scheduler + merger entry logs. The
// scheduler's r4 is the dirty-sector BIT-SET it splits via the 832D4310 /
// 83341EA0 masks (late3 decompiles) — per-pass bits on wedge-vs-merged boots
// show which sectors keep re-dirtying and never converge. The merger log is
// cadence-only (its record parsing waits until r3's identity is confirmed).
// Side-filed: rotation sheds REXLOG lines.
REX_EXTERN(__imp__sub_829CA0C8);
REX_HOOK_RAW(sub_829CA0C8) {
  // M3.212 (RESTUFF_FORCEBITS=<t0,t1>[,cap]): RE-DIRTY THE SECTORS AFTER LOAD.
  //
  // The decisive question the frozen-at-load result raises: are a wedge boot's
  // missing merged representations impossible to build later, or merely never
  // asked for again? mrg37 showed the sector table is complete the moment it
  // becomes visible (~84s) and never changes across 26k further scheduler
  // passes — so nothing ever re-asks. If the products CAN be built once
  // everything is resident, re-dirtying heals the boot and names the fix
  // ("re-trigger the merge after load"); if it cannot, the products depend on
  // load-window state that is gone, and the fix has to move earlier.
  //
  // r4 is the dirty bit-set the scheduler splits. We only ever re-assert bits
  // the ENGINE ITSELF scheduled during the burst (accumulated below), never a
  // fabricated bit — an invalid sector index would walk a bogus 56-byte entry.
  // Capped, because the scheduler runs ~150x/s and forcing every pass would
  // swamp the frame loop rather than test anything.
  {
    static std::atomic<uint64_t> s_union{0};
    static std::atomic<uint32_t> s_forced{0};
    static const struct Cfg {
      long t0 = 0, t1 = 0;
      uint32_t cap = 50;
      Cfg() {
        if (const char* e = getenv("RESTUFF_FORCEBITS")) {
          unsigned c = 50;
          if (sscanf(e, "%ld,%ld,%u", &t0, &t1, &c) >= 2) cap = c;
        }
      }
    } cfg;
    const long tf = long(ibwatch::SinceArmMs());
    if (tf < 75000) s_union.fetch_or(ctx.r4.u64, std::memory_order_relaxed);
    if (cfg.t1 && tf >= cfg.t0 && tf <= cfg.t1) {
      const uint32_t n = s_forced.load(std::memory_order_relaxed);
      if (n < cfg.cap) {
        const uint64_t u = s_union.load(std::memory_order_relaxed);
        if (u && (ctx.r4.u64 | u) != ctx.r4.u64) {
          s_forced.fetch_add(1, std::memory_order_relaxed);
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "FORCE#%u t=%ldms was=%016llX now=%016llX\n", n, tf,
                    (unsigned long long)ctx.r4.u64, (unsigned long long)(ctx.r4.u64 | u));
            fclose(f);
          }
          ctx.r4.u64 |= u;
        }
      }
    }
  }
  static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
  if (on) {
    if (FILE* f = fopen(restuff_logpath(), "a")) {
      fprintf(f, "SCHED t=%ldms bits=%016llX r3=%08X\n", long(ibwatch::SinceArmMs()),
              (unsigned long long)ctx.r4.u64, ctx.r3.u32);
      fclose(f);
    }
    // M3.198 (RESTUFF_MERGELOG=1): 1Hz raw dump of the FULL 64x56-byte
    // sector table at *(r3+532)+120 (the scheduler iterates exactly this).
    // M3.197's walk dump turned out to be a small pending list, not the
    // table. Byte-offset diffs across boots name sector (off/56) and field
    // (off%56); the family-correlated field is the selector.
    static std::atomic<int64_t> s_last{0};
    const int64_t now = ibwatch::SinceArmMs();
    int64_t last = s_last.load(std::memory_order_relaxed);
    if (now - last >= 1000 && s_last.compare_exchange_strong(last, now)) {
      uint32_t tab;
      memcpy(&tab, base + ctx.r3.u32 + 532, 4);
      tab = __builtin_bswap32(tab);
      // M3.201: at t>=100s, one-shot-arm a watch on a live sector OBJECT's
      // +124 flag word (page-granular): the ~120s writer that clears the
      // test-enable bit4 — the give-up — lands in ibwatch_hits with its PC.
      // Uses the pre-emit pending list at *(r3+532)'s +12 (same deref as
      // sub_829C77B8); requires RESTUFF_IBWATCH set (handler) and NO
      // LATE_MS arm in the same run (spans are shared).
      {
        static std::atomic<bool> s_armed{false};
        const long tn = long(ibwatch::SinceArmMs());
        if (tn >= 117500 && !s_armed.load(std::memory_order_relaxed)) {
          bool expected = false;
          if (s_armed.compare_exchange_strong(expected, true)) {
            uint32_t desc, lo0;
            memcpy(&desc, base + ctx.r3.u32 + 532, 4);
            desc = __builtin_bswap32(desc);
            memcpy(&lo0, base + desc + 12, 4);
            lo0 = __builtin_bswap32(lo0);
            uint32_t obj = 0;
            if (lo0 >= 0x10000 && lo0 < 0xC0000000u - 8) {
              memcpy(&obj, base + lo0 + 4, 4);
              obj = __builtin_bswap32(obj);
            }
            // M3.201c: zero flag-page writes post-118s on BOTH families —
            // the give-up is REMOVAL from this pending list (~120s), so
            // watch the list DESCRIPTOR page (bounds at desc+12/+16) for
            // the remover's PC instead of the object's flags.
            if (desc >= 0x10000 && desc < 0xC0000000u - 128) {
              const uint32_t pg = (desc + 12) & ~4095u;
              ibwatch::ArmAt(pg, pg + 4096);
            }
          }
        }
      }
      // M3.202: snapshot the pending list at EVERY scheduler pass in the
      // deadline window [110s,130s] — bounds + first 16 entries (ptr +
      // counter word). Shows exactly what leaves the list and when; the
      // remover's PC comes from sub_829C5238's callers statically.
      {
        const long tn2 = long(ibwatch::SinceArmMs());
        if (tn2 >= 110000 && tn2 <= 130000) {
          uint32_t d2, lo2, hi2;
          memcpy(&d2, base + ctx.r3.u32 + 532, 4);
          d2 = __builtin_bswap32(d2);
          if (d2 >= 0x10000 && d2 < 0xC0000000u - 20) {
            memcpy(&lo2, base + d2 + 12, 4);
            memcpy(&hi2, base + d2 + 16, 4);
            lo2 = __builtin_bswap32(lo2);
            hi2 = __builtin_bswap32(hi2);
            if (FILE* f = fopen(restuff_logpath(), "a")) {
              fprintf(f, "PLIST t=%ldms n=%u e=", tn2,
                      hi2 > lo2 ? (hi2 - lo2) / 8 : 0);
              for (uint32_t e = lo2; e + 8 <= hi2 && e < lo2 + 128; e += 8) {
                uint32_t w0, w1;
                memcpy(&w0, base + e, 4);
                memcpy(&w1, base + e + 4, 4);
                fprintf(f, "%08X:%08X ", __builtin_bswap32(w0), __builtin_bswap32(w1));
              }
              fprintf(f, "\n");
              fclose(f);
            }
          }
        }
      }
      // M3.203 (RESTUFF_WEDGE_FIX=1): THE FIX. Each sector polls "is my
      // merged representation resident yet?" under a per-sector budget
      // (test-enable = +124 bit4); budgets exhaust by ~120s and sectors
      // whose residency registered late freeze unattached — the fine
      // partition's authored stitch gap stays visible = the wedge. Late
      // attaches provably work (the door-heal), so: for every entry still
      // in the pending list past 60s whose test-enable is off, turn it
      // back on. The machinery re-observes, the late registration lands,
      // the attach completes, the entry drains — hardware's outcome.
      {
        static const bool fix = getenv("RESTUFF_WEDGE_FIX") != nullptr;
        const long tf = long(ibwatch::SinceArmMs());
        if (fix && tf >= 60000) {
          uint32_t d3, lo3, hi3;
          memcpy(&d3, base + ctx.r3.u32 + 532, 4);
          d3 = __builtin_bswap32(d3);
          if (d3 >= 0x10000 && d3 < 0xC0000000u - 20) {
            memcpy(&lo3, base + d3 + 12, 4);
            memcpy(&hi3, base + d3 + 16, 4);
            lo3 = __builtin_bswap32(lo3);
            hi3 = __builtin_bswap32(hi3);
            static std::atomic<uint32_t> s_relit{0};
            for (uint32_t e = lo3; e + 8 <= hi3 && e < lo3 + 512; e += 8) {
              uint32_t obj;
              memcpy(&obj, base + e + 4, 4);
              obj = __builtin_bswap32(obj);
              if (obj < 0x10000 || obj >= 0xC0000000u - 128) continue;
              uint32_t fl;
              memcpy(&fl, base + obj + 124, 4);
              fl = __builtin_bswap32(fl);
              if ((fl & 0x30) != 0x30) {
                // v2: re-light BOTH test-enables — bit4 (pre-emit pair 2/4)
                // AND bit5 (reconciler pair 3/5, the mode-word writer);
                // bit4 alone left mrg241 wedged.
                const uint32_t nf = __builtin_bswap32(fl | 0x30);
                memcpy(base + obj + 124, &nf, 4);
                const uint32_t k = s_relit.fetch_add(1, std::memory_order_relaxed);
                if (k < 200) {
                  if (FILE* f = fopen(restuff_logpath(), "a")) {
                    fprintf(f, "RELIT#%u t=%ldms obj=%08X fl=%08X\n", k, tf, obj, fl);
                    fclose(f);
                  }
                }
              }
            }
          }
        }
      }
      // M3.210 (RESTUFF_HWBP=1): publish the table this pass is working on.
      // Called on EVERY pass, not once: the front-end context runs first and
      // for ~2000 passes, so a one-shot latch would arm the debug registers on
      // a table whose attach words are never written (mrg37).
      {
        static const bool hw_on = getenv("RESTUFF_HWBP") != nullptr;
        if (hw_on) {
          uint32_t tbh;
          memcpy(&tbh, base + ctx.r3.u32 + 532, 4);
          tbh = __builtin_bswap32(tbh);
          if (tbh >= 0x10000 && tbh < 0xC0000000u - 3800) hwbp::Note(base, tbh);
        }
      }
      // M3.209: 10Hz bracket of the ATTACH moment — the per-view mode word
      // (entry+36) per sector, 60-75s. On merged boots the 0000->set
      // transition timestamp bounds the writer to ~100ms; the two-stage
      // arm then lands inside it.
      {
        static std::atomic<int64_t> s_mw{0};
        const long tm = long(ibwatch::SinceArmMs());
        if (tm >= 60000 && tm <= 75000) {
          int64_t ml = s_mw.load(std::memory_order_relaxed);
          if (tm - ml >= 100 && s_mw.compare_exchange_strong(ml, tm)) {
            uint32_t tb;
            memcpy(&tb, base + ctx.r3.u32 + 532, 4);
            tb = __builtin_bswap32(tb);
            if (tb >= 0x10000 && tb < 0xC0000000u - 3800) {
              if (FILE* f = fopen(restuff_logpath(), "a")) {
                fprintf(f, "MW t=%ldms ", tm);
                for (int sct = 0; sct < 64; ++sct) {
                  uint32_t mw;
                  memcpy(&mw, base + tb + 120 + 56 * sct + 36, 4);
                  fprintf(f, "%08X ", __builtin_bswap32(mw));
                }
                fprintf(f, "\n");
                fclose(f);
              }
            }
          }
        }
      }
      if (tab >= 0x10000 && tab < 0xC0000000u - 3800) {
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "STAB t=%ldms tab=%08X d=", long(now), tab);
          for (int i = 0; i < 3584; ++i) fprintf(f, "%02X", base[tab + 120 + i]);
          // M3.198b: self-documenting attach targets — for every sector whose
          // +36 attach pointer is set (merged boots only), append the target's
          // first 16B so the attached object's VTABLE lands in the log.
          for (int s = 0; s < 64; ++s) {
            uint32_t ap;
            memcpy(&ap, base + tab + 120 + 56 * s + 36, 4);
            ap = __builtin_bswap32(ap);
            if (ap >= 0x10000 && ap < 0xC0000000u - 16) {
              fprintf(f, " s%d@%08X=", s, ap);
              for (int i = 0; i < 16; ++i) fprintf(f, "%02X", base[ap + i]);
            }
          }
          fprintf(f, "\n");
          fclose(f);
        }
      }
    }
  }
  __imp__sub_829CA0C8(ctx, base);
}
REX_EXTERN(__imp__sub_82AF97D8);
REX_HOOK_RAW(sub_82AF97D8) {
  // M3.236 (Aug 11): HOISTED OUT OF THE RESTUFF_MERGELOG GATE.
  // These blocks used to sit inside this hook's `if (on)` where
  // on = getenv("RESTUFF_MERGELOG"), so the DEFAULT-ON wedge fix (M3.229)
  // only ran when that debug env var was set -- i.e. never, for a normal
  // user launch. Proven by M3.235 logging nothing on a run that set
  // RESTUFF_KEYHIST=1 but not RESTUFF_MERGELOG. The earlier 'dfltfix1
  // DEFAULT-ON PASS' batch passed no env at all, so its 3/3 clean boots did
  // NOT exercise the fix -- that result is VOID and must be re-run.
  {
  // M3.235 (RESTUFF_KEYHIST=1): the ERA-MIXING test, and a correction to
  // the M3.229 comment below. The mask1 batch (3 in-level boots: mask12
  // clean / mask14 WEDGE / mask15 clean) REFUTED "the wedge is a light-list
  // downgrade": the downgrade to [2] is UNIVERSAL and follows an identical
  // trajectory in all three boots (mean lights/list 6.1 -> 1.0; 62% [2] at
  // 95-100s; 100% [2] at 100-105s in the clean boots too). It cannot be the
  // discriminator. What DOES differ is that the clean boots stop calling
  // the composer at ~105s while the wedge boot keeps calling it at a steady
  // rate through 210s+ (the %997 sampler never stops, so that is a real
  // stop, not decimation running out).
  //
  // Working model: the merge key BAKES the light-exclusion set, and the
  // light set legitimately shrinks over time. Records are (re)composed
  // incrementally, so the record array can hold keys minted in different
  // ERAS. Adjacent records from different eras have unequal keys and refuse
  // to coalesce -> 285 draws -> the authored stitch gap shows as water.
  // Whether the door records straddle the collapse is timing-dependent,
  // which is exactly the observed ~50% boot-to-boot split.
  //
  // This probe tests that directly and cheaply: once per second, histogram
  // byte 23 over the whole record array. Uniform histogram = one era (model
  // predicts clean); a MIX that grows after ~100s = eras coexisting (model
  // predicts wedge). Runs BEFORE the KEYFIX loop so it sees pre-fix values.
  {
    static const bool kh = getenv("RESTUFF_KEYHIST") != nullptr;
    if (kh) {
      const long t = long(ibwatch::SinceArmMs());
      static std::atomic<long> s_last{-1};
      const long sec = t / 1000;
      long prev = s_last.load(std::memory_order_relaxed);
      if (t > 0 && sec != prev &&
          s_last.compare_exchange_strong(prev, sec)) {
        const uint32_t obj = ctx.r3.u32;
        if (obj >= 0x10000 && obj < 0xC0000000u - 128) {
          uint32_t recp, nrec;
          std::memcpy(&recp, base + obj + 112, 4);
          std::memcpy(&nrec, base + obj + 116, 4);
          recp = __builtin_bswap32(recp);
          nrec = __builtin_bswap32(nrec);
          if (recp >= 0x10000 && nrec && nrec < 100000 &&
              recp < 0xC0000000u - 64ull * (nrec < 4096 ? nrec : 4096)) {
            uint32_t hist[256] = {};
            uint32_t seen = 0;
            const uint32_t scan = nrec < 4096 ? nrec : 4096;
            for (uint32_t r = 0; r < scan; ++r) {
              uint32_t keyp;
              std::memcpy(&keyp, base + recp + 64 * r + 36, 4);
              keyp = __builtin_bswap32(keyp);
              if (keyp < 0x10000 || keyp >= 0xC0000000u - 32) continue;
              hist[*(base + keyp + 23)]++;
              ++seen;
            }
            if (FILE* f = fopen(restuff_logpath(), "a")) {
              fprintf(f, "KEYHIST t=%ldms nrec=%u scanned=%u", t, nrec, seen);
              for (uint32_t v = 0; v < 256; ++v)
                if (hist[v]) fprintf(f, " %02X:%u", v, hist[v]);
              fprintf(f, "\n");
              fclose(f);
            }
          }
        }
      }
    }
  }
  // M3.229 (DEFAULT ON, kill-switch RESTUFF_NO_KEYFIX=1): the WEDGE FIX.
  //
  // The wedge = a perpetual light-list DOWNGRADE re-baking a spurious
  // "exclude light 2" (byte 23 = 0x04 exactly) into door-area merge keys,
  // which blocks coalescing (285 draws vs 79) and exposes the authored
  // stitch gap as water. Clearing exactly those keys made 8/8 boots clean,
  // eye-verified, with lighting identical to a naturally-clean boot
  // (kf21 vs caller12). Exact-0x04 only: legit states (0x08/0x0C load-era,
  // 0x06) pass through — widening the match corrupted them once (kf1).
  // Lean standalone loop, separate from the env-gated diagnostics below;
  // user accepted default-on Aug 11 while the deeper root (who downgrades
  // the node light list) is still hunted.
  {
    // M3.253: KEYFIX is now OFF by default. It rewrote the merge key's byte 23
    // -- the LAST link in the chain -- which is a band-aid; M3.252 releases the
    // leaked binding at the source and lets the guest prune the light itself.
    // Keeping both would hide a regression in the real fix. Opt back in with
    // RESTUFF_KEYFIX=1 if a comparison is ever needed.
    static const bool keyfix_off = getenv("RESTUFF_KEYFIX") == nullptr;
    // M3.232b: with LISTFIX on, also clear while a light-list rebuild is in
    // flight — that is exactly the torn window whose partial list produces
    // the spurious exclusion, so the key written during it must not keep a
    // stale 0x04. (Independent of KEYFIX's own gate; both paths clear the
    // same wrong bit, this one targeted at the race window.)
    const bool rebuilding = g_list_rebuilding.load(std::memory_order_acquire) > 0;
    const uint32_t obj = ctx.r3.u32;
    if ((!keyfix_off || rebuilding) && obj >= 0x10000 && obj < 0xC0000000u - 128) {
      uint32_t recp, nrec;
      std::memcpy(&recp, base + obj + 112, 4);
      std::memcpy(&nrec, base + obj + 116, 4);
      recp = __builtin_bswap32(recp);
      nrec = __builtin_bswap32(nrec);
      // M3.240: the old guard only reserved room for EIGHT records (64 * 8)
      // because the loop below used to stop at 512 and, in practice, at ~3
      // records before that. Now that the scan follows nrec, the bound has to
      // follow it too or a record array near the top of the address space would
      // read out of bounds. Same shape KEYHIST above already uses.
      const uint32_t scan = nrec < 4096 ? nrec : 4096;
      if (recp >= 0x10000 && nrec && nrec < 100000 &&
          recp < 0xC0000000u - 64ull * scan - 40) {
        // M3.240: was `r = 1` and a 512 cap, both unjustified and both real
        // coverage holes. Record 0 was never cleared at all, and across 3333
        // KEYHIST samples nrec reached 585 -- so on that array records 512..584
        // kept their spurious 0x04 and could still split the runs. Arrays are
        // 62..585 here and the user visits areas this harness never does, so
        // scan from 0 and cap at 4096 (same bound KEYHIST uses; it only reads
        // 4 bytes and conditionally writes 1 per record, so the cost is
        // negligible and still bounded). The exact-0x04 match is unchanged --
        // widening THAT is what corrupted legit 0x0C/0x08 states in kf1.
        for (uint32_t r = 0; r < scan; ++r) {
          uint32_t keyp;
          std::memcpy(&keyp, base + recp + 64 * r + 36, 4);
          keyp = __builtin_bswap32(keyp);
          if (keyp < 0x10000 || keyp >= 0xC0000000u - 32) continue;
          if (*(base + keyp + 23) == 0x04) *(base + keyp + 23) = 0;
        }
      }
    }
  }
  }
  static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
  if (on) {
    static std::atomic<uint32_t> s_n{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n < 2000 || (n & 63) == 0) {
      if (FILE* f = fopen(restuff_logpath(), "a")) {
        fprintf(f, "MERGER#%u t=%ldms r3=%08X r4=%08X\n", n, long(ibwatch::SinceArmMs()),
                ctx.r3.u32, ctx.r4.u32);
        fclose(f);
      }
    }
    // M3.217 (RESTUFF_BITWATCH=1): WHEN does the wedge's render-state first exist?
    //
    // door1 named the fork: one bit — byte 23, bit 2 of the merger's 32-byte
    // state key. Both clean boots carry ZERO records with it; both wedge boots
    // carry EXACTLY 1632, and those records' bboxes (x 81.4..93.0) sit on the
    // wedge itself (the Aug-7 geometric proof put the hole at world x=81.8).
    // The merger only coalesces while adjacent keys are equal, so those 1632
    // split the runs, 285 draws are emitted instead of 79, and the fine
    // partition's authored stitch gap shows.
    //
    // The remaining question is WHEN that second state object comes into use:
    // at load (a streaming/tier decision, which matches this being the same
    // defect as the low-LOD-tier bug) or dynamically at the door. Logging the
    // FIRST sighting brackets it. Cheap: three records per call, one byte
    // tested, and only the first hit plus a decimated trail are written.
    {
      static const bool bw = getenv("RESTUFF_BITWATCH") != nullptr;
      if (bw) {
        const uint32_t obj = ctx.r3.u32;
        if (obj >= 0x10000 && obj < 0xC0000000u - 128) {
          uint32_t recp, nrec;
          std::memcpy(&recp, base + obj + 112, 4);
          std::memcpy(&nrec, base + obj + 116, 4);
          recp = __builtin_bswap32(recp);
          nrec = __builtin_bswap32(nrec);
          if (recp >= 0x10000 && recp < 0xC0000000u - 64 * 8 && nrec && nrec < 100000) {
            // M3.220b: scan the WHOLE record array, not 3 entries. keyfix1
            // showed the key objects are many (B5D537B0, B5D52150, B5D50B80,
            // B5D523D0 ... all distinct) sharing only a few distinct VALUES,
            // so patching 3 records per call reached a small fraction of them
            // — 40k+ writes and still plenty unpatched, which left that test
            // under-powered rather than negative.
            const uint32_t scan = nrec < 512 ? nrec : 512;
            for (uint32_t r = 1; r < scan; ++r) {
              uint32_t keyp;
              std::memcpy(&keyp, base + recp + 64 * r + 36, 4);
              keyp = __builtin_bswap32(keyp);
              if (keyp < 0x10000 || keyp >= 0xC0000000u - 32) continue;
              const uint8_t flags = *(base + keyp + 23);
              // M3.222 (RESTUFF_KEYWATCH=1): collect a few key bytes that are
              // still CLEAR, then arm hardware watches on them just before the
              // mutation burst (observed at t=74131ms) so the writer names
              // itself. The atomic count keeps this off the lock once four are
              // banked — the scan runs per record, per merger call.
              {
                static const bool kw = getenv("RESTUFF_KEYWATCH") != nullptr;
                if (kw) {
                  static std::mutex kmu;
                  static std::vector<uint32_t> cand;
                  static std::atomic<int> ncand{0};
                  static std::atomic<bool> armed{false};
                  const long tk = long(ibwatch::SinceArmMs());
                  if (!armed.load(std::memory_order_relaxed)) {
                    if (tk < 72000) {
                      if (!(flags & 0x04) && ncand.load(std::memory_order_relaxed) < 4) {
                        std::lock_guard<std::mutex> lk(kmu);
                        if (cand.size() < 4 &&
                            std::find(cand.begin(), cand.end(), keyp + 23) == cand.end()) {
                          cand.push_back(keyp + 23);
                          ncand.store(int(cand.size()), std::memory_order_relaxed);
                        }
                      }
                    } else {
                      bool e = false;
                      if (armed.compare_exchange_strong(e, true)) {
                        std::lock_guard<std::mutex> lk(kmu);
                        if (!cand.empty()) keywatch::Arm(base, cand);
                      }
                    }
                  }
                }
              }
              // M3.221: is a 0x04 key a FRESHLY CREATED object that was always
              // set, or an existing 0x00 object that got MUTATED? That splits
              // the root hunt in two — "a different state object was CHOSEN
              // for these records" vs "this object's bit was WRITTEN" — and
              // each points somewhere else. Remember every key address seen
              // clear, and shout if one of them later turns up set.
              {
                static const bool track = getenv("RESTUFF_KEYORIGIN") != nullptr;
                if (track) {
                  static std::mutex mu;
                  static std::unordered_set<uint32_t> seen_clear;
                  std::lock_guard<std::mutex> lk(mu);
                  if (!(flags & 0x04)) {
                    if (seen_clear.size() < 200000) seen_clear.insert(keyp);
                  } else if (seen_clear.count(keyp)) {
                    static std::atomic<uint32_t> nmut{0};
                    const uint32_t k = nmut.fetch_add(1, std::memory_order_relaxed);
                    if (k < 12) {
                      if (FILE* f = fopen(restuff_logpath(), "a")) {
                        fprintf(f, "KEYMUT#%u t=%ldms keyp=%08X was CLEAR now %02X\n", k,
                                long(ibwatch::SinceArmMs()), keyp, flags);
                        fclose(f);
                      }
                    }
                    seen_clear.erase(keyp);
                  }
                }
              }
              if (!(flags & 0x04)) continue;
              // M3.220 (RESTUFF_KEYFIX=1): clear the bit and let the runs merge.
              //
              // door1 re-analysis: the door-area geometry is IDENTICAL across
              // families — same 2976 records, same 91 distinct count pairs —
              // and the ONLY difference is that 1632 of them point at a state
              // key carrying byte23 bit2. A clean boot renders those very same
              // triangles with the plain key, so clearing the bit asks the
              // guest to do exactly what a clean boot already does; it is not
              // inventing a state the title never uses.
              //
              // This is a CAUSAL TEST first and a candidate fix second: the
              // merger coalesces only while adjacent keys are equal, so if
              // this bit is really what splits the runs, a wedge boot should
              // turn clean (draws 285 -> 79). If it does not, the bit is a
              // passenger and the chain is wrong.
              // M3.220b: only the EXACT 0x04 keys. keyfix1 fired on `flags &
              // 0x04`, which also caught the legitimate load-era 0x0C state
              // (bits 2+3) and rewrote it to 0x08 — 64 of the 75 logged writes
              // were 0C->08, i.e. the test spent most of its effort corrupting
              // normal state, and draws drifted 285->287 as a result. Only the
              // door-era 0x04 keys are the anomaly clean boots never have.
              static const bool keyfix = getenv("RESTUFF_KEYFIX") != nullptr;
              if (keyfix && flags == 0x04) {
                static std::atomic<uint32_t> s_fixed{0};
                *(base + keyp + 23) = 0;
                const uint32_t nf = s_fixed.fetch_add(1, std::memory_order_relaxed);
                if (nf < 8 || (nf % 20000) == 0) {
                  if (FILE* f = fopen(restuff_logpath(), "a")) {
                    fprintf(f, "KEYFIX#%u t=%ldms keyp=%08X %02X->%02X\n", nf,
                            long(ibwatch::SinceArmMs()), keyp, flags,
                            uint8_t(flags & ~0x04));
                    fclose(f);
                  }
                }
              }
              static std::atomic<uint32_t> s_hits{0};
              const uint32_t h = s_hits.fetch_add(1, std::memory_order_relaxed);
              if (h == 0 || (h % 5000) == 0) {
                uint8_t rb[64];
                std::memcpy(rb, base + recp + 64 * r, 64);
                if (FILE* f = fopen(restuff_logpath(), "a")) {
                  fprintf(f, "BIT#%u t=%ldms keyp=%08X flags=%02X rec=", h,
                          long(ibwatch::SinceArmMs()), keyp, flags);
                  for (int q = 0; q < 64; ++q) fprintf(f, "%02X", rb[q]);
                  fprintf(f, "\n");
                  fclose(f);
                }
              }
              break;
            }
          }
        }
      }
    }
    // M3.191 (RESTUFF_MKEY=<t0,t1 ms>): during the window, dump the merger's
    // 64-byte record at r3 plus the 32-byte state key its +36 pointer names
    // (the decompile's _R30[9]; runs coalesce only while these keys are
    // byte-equal and indices fit 0xFFFF). The initial sweep rewrites ALL 4096
    // pages on every boot, both families (mrg3) — coverage never diverges, so
    // the wedge must live in these key VALUES. Byte-diff merged-vs-wedge.
    static const auto win = [] {
      long a = 0, b = 0;
      if (const char* e = getenv("RESTUFF_MKEY")) sscanf(e, "%ld,%ld", &a, &b);
      return std::pair<long, long>(a, b);
    }();
    if (win.second) {
      const long t = long(ibwatch::SinceArmMs());
      if (t >= win.first && t <= win.second) {
        static std::atomic<uint32_t> s_k{0};
        const uint32_t k = s_k.fetch_add(1, std::memory_order_relaxed);
        // M3.213: the cap is the sample, so it must be stated. At the old
        // fixed 900 two boots looked byte-identical (900 calls, 2662 recs,
        // 1465 key-equal adjacencies) — but the burst runs several thousand
        // calls, so that only ever showed a deterministic PROLOGUE. Raise it
        // (RESTUFF_MKEY_CAP) before concluding the inputs match.
        static const uint32_t kcap =
            getenv("RESTUFF_MKEY_CAP") ? uint32_t(atol(getenv("RESTUFF_MKEY_CAP"))) : 900;
        if (k < kcap) {
          // v2 (mrg4 decode): r3 = the chunk OBJECT (vtable 8227FA6C, bounds
          // floats, 1B pointers), not the packed record. The record array the
          // merger iterates hangs off it at +112 (count +116), records are
          // 64B starting at +64, and each record's 32-byte key block is named
          // by ITS +36 pointer. Dump the object head + first records + keys.
          const uint32_t obj = ctx.r3.u32;
          if (obj >= 0x10000 && obj < 0xC0000000u - 128) {
            uint8_t ob[128];
            std::memcpy(ob, base + obj, 128);
            auto be32 = [](const uint8_t* p) {
              uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v);
            };
            const uint32_t recp = be32(ob + 112), nrec = be32(ob + 116);
            if (FILE* f = fopen(restuff_logpath(), "a")) {
              fprintf(f, "MKEY#%u t=%ldms r3=%08X obj=", k, t, obj);
              for (int i = 0; i < 128; ++i) fprintf(f, "%02X", ob[i]);
              fprintf(f, " recp=%08X n=%u\n", recp, nrec);
              const bool rok = recp >= 0x10000 && recp < 0xC0000000u - 64 * 8;
              for (uint32_t r = 1; rok && r <= 3 && r < nrec; ++r) {
                uint8_t rb[64];
                std::memcpy(rb, base + recp + 64 * r, 64);
                const uint32_t keyp = be32(rb + 36);
                uint8_t kb[32] = {};
                const bool kok = keyp >= 0x10000 && keyp < 0xC0000000u - 32;
                if (kok) std::memcpy(kb, base + keyp, 32);
                fprintf(f, "MREC#%u.%u rec=", k, r);
                for (int i = 0; i < 64; ++i) fprintf(f, "%02X", rb[i]);
                fprintf(f, " keyp=%08X key=", kok ? keyp : 0);
                for (int i = 0; i < 32; ++i) fprintf(f, "%02X", kb[i]);
                fprintf(f, "\n");
              }
              fclose(f);
            }
          }
        }
      }
    }
  }
  // M3.214 (RESTUFF_BURSTDELAY=<t0,t1,us>[,budget_ms]): STRETCH THE MERGE
  // BURST on purpose.
  //
  // Noticed by accident: batches that log heavily inside the 65-70s burst come
  // out MERGED (mk1 2/2 with RESTUFF_MKEY's ~800KB of fprintf in that window)
  // while the same build without it came out 3/4 and 1/1 WEDGE — and an older
  // note recorded a boot going merged under a 3-core spinner. The merged boot
  // in mrg37 also had the widest burst (342/1367/340 calls per second vs a
  // sharp 342/1662/54).
  //
  // If the burst processes whatever is ready at the moment it runs, stretching
  // it lets more become ready and more representations get attached. This is
  // the controlled version of that accident: sleep a fixed amount per merger
  // call inside the window, with a total budget so a bad value cannot hang the
  // boot. Confirming it gives the first causal handle on the lottery — a fix
  // then means ordering the burst after its prerequisites, not sleeping.
  {
    static const struct BD {
      long t0 = 0, t1 = 0;
      long us = 0, budget_ms = 400;
      BD() {
        if (const char* e = getenv("RESTUFF_BURSTDELAY")) {
          long b = 400;
          if (sscanf(e, "%ld,%ld,%ld,%ld", &t0, &t1, &us, &b) >= 3) budget_ms = b;
        }
      }
    } bd;
    if (bd.us > 0) {
      const long t = long(ibwatch::SinceArmMs());
      if (t >= bd.t0 && t <= bd.t1) {
        static std::atomic<long> s_spent_us{0};
        const long spent = s_spent_us.load(std::memory_order_relaxed);
        if (spent < bd.budget_ms * 1000) {
          s_spent_us.fetch_add(bd.us, std::memory_order_relaxed);
          std::this_thread::sleep_for(std::chrono::microseconds(bd.us));
        }
      }
    }
  }
  // M3.211 (RESTUFF_MERGELOG=1): per-call merge OUTCOME.
  //
  // mrg37 settled that call VOLUME is family-independent — pre-84s MERGER
  // totals were 2871/2865/2909/2880 across 3 wedge boots and 1 merged one,
  // and the worst wedge ran MORE calls than a milder one — while the sector
  // table each boot ends up with differs threefold and is frozen from the
  // moment it becomes visible. So the fork is what each call DECIDES, not how
  // often it runs. The merger coalesces adjacent runs in place, so the record
  // count before vs after this call IS the amount it managed to coalesce
  // (decompile: runs merge while the 32-byte keys stay equal AND the combined
  // index total fits 0xFFFF).
  //
  // Aggregated, not per-call: 13k lines/boot would shed to log rotation and
  // the shape we need is the per-second coalesce RATE across the 65-70s burst.
  if (on) {
    const uint32_t obj = ctx.r3.u32;
    uint32_t nin = 0;
    const bool ok = obj >= 0x10000 && obj < 0xC0000000u - 128;
    if (ok) {
      std::memcpy(&nin, base + obj + 116, 4);
      nin = __builtin_bswap32(nin);
    }
    __imp__sub_82AF97D8(ctx, base);
    if (ok) {
      uint32_t nout = 0;
      std::memcpy(&nout, base + obj + 116, 4);
      nout = __builtin_bswap32(nout);
      static std::atomic<uint32_t> s_calls{0}, s_shrunk{0};
      static std::atomic<uint64_t> s_in{0}, s_out{0};
      static std::atomic<int64_t> s_last{0};
      // Records are 64B; a wild count means we mis-read a freed object.
      if (nin < 100000 && nout < 100000) {
        s_calls.fetch_add(1, std::memory_order_relaxed);
        s_in.fetch_add(nin, std::memory_order_relaxed);
        s_out.fetch_add(nout, std::memory_order_relaxed);
        if (nout < nin) s_shrunk.fetch_add(1, std::memory_order_relaxed);
      }
      const int64_t now = ibwatch::SinceArmMs();
      int64_t last = s_last.load(std::memory_order_relaxed);
      if (now - last >= 1000 && s_last.compare_exchange_strong(last, now)) {
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "MOUT t=%ldms calls=%u shrunk=%u in=%llu out=%llu\n", long(now),
                  s_calls.load(std::memory_order_relaxed),
                  s_shrunk.load(std::memory_order_relaxed),
                  (unsigned long long)s_in.load(std::memory_order_relaxed),
                  (unsigned long long)s_out.load(std::memory_order_relaxed));
          fclose(f);
        }
      }
    }
    return;
  }
  __imp__sub_82AF97D8(ctx, base);
}
REX_EXTERN(__imp__sub_82AEBFE0);
REX_HOOK_RAW(sub_82AEBFE0) {
  __imp__sub_82AEBFE0(ctx, base);
  mergegate::g_bfe0_calls.fetch_add(1, std::memory_order_relaxed);
  if (uint8_t(ctx.r3.u32)) mergegate::g_bfe0_true.fetch_add(1, std::memory_order_relaxed);
}
// ---------------------------------------------------------------------------
// M3.218 (RESTUFF_XAMLOG=1): who asks about gamer profiles, and what do they
// hear?
//
// The "Would you like to sign in with a gamer profile?" dialog is the GAME's
// own UI (felt texture, game font -- not a 360 blade), and it is
// UNDISMISSABLE here: answering it hangs the title, because "Sign In" leads to
// a system sign-in overlay this recomp does not have. So the fix must stop it
// appearing; there is nothing to press.
//
// Statically it should never appear. sub_830B31A8 takes a fast path when
//   XamGetSystemVersion() < 0x20096B00   (ours returns 0)  AND
//   XamUserGetSigninState(user) == 1     (ours returns 1)
// and only otherwise falls through to XamUserCheckPrivilege. But the SDK
// returns the signin state ONLY for user_index 0 -- ports 1..3 get 0 -- and
// the prompt is INTERMITTENT, which no static return can explain. So log what
// is actually asked and answered:
//   sub_830B3198 = the XamUserGetSigninState thunk  (which port is queried?)
//   sub_830B31A8 = the checker                      (which branch is taken?)
//   sub_830B2F68 = the XamShowSigninUI thunk        (the smoking gun)
// Capped, and anything answering "not signed in" is always logged.
namespace xamlog {
static void Line(const char* fmt, ...) {
  if (FILE* f = fopen(restuff_logpath(), "a")) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
  }
}
}  // namespace xamlog

REX_EXTERN(__imp__sub_830B3198);
REX_HOOK_RAW(sub_830B3198) {  // XamUserGetSigninState(user_index)
  static const bool on = getenv("RESTUFF_XAMLOG") != nullptr;
  const uint32_t idx = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_830B3198(ctx, base);
  // M3.262 (RESTUFF_SIGNIN_FAULT=1, OPT-IN, TEST ONLY): prove the causal chain
  // instead of waiting for a ~2%-per-boot race.
  //
  // Healthy boots (13/13 identical) answer the player-construction query
  //     XAM#4 GetSigninState(user=0) -> 1  lr=82840A18
  // while a stuck boot asks about user 1 there and gets 0. If "that site sees
  // not-signed-in" is really what raises the prompt, forcing a 0 at EXACTLY
  // that call site must reproduce the prompt on demand. If it does not, the
  // chain is wrong and I need a different explanation -- either way it settles
  // it in three boots rather than thirty.
  {
    // M3.271: fault at ANY signin call site, not just the player-construction
    // one. A healthy boot makes five non-sweep queries and they are all
    // user=0 -> 1; the prompt's own gate must be one of them. Faulting
    // 82840A18 was refuted (3/3 gameplay), so walk the rest:
    //     RESTUFF_SIGNIN_FAULT_LR=82B2BE44 | 82B2C990 | 82B2C568
    // (the storage result proved this class of prompt IS reachable by breaking
    // one XAM answer, so the remaining sites are worth one arm each).
    static const bool fault = getenv("RESTUFF_SIGNIN_FAULT") != nullptr;
    static const uint32_t fault_lr = [] {
      const char* v = getenv("RESTUFF_SIGNIN_FAULT_LR");
      return v ? uint32_t(strtoul(v, nullptr, 16)) : 0u;
    }();
    if (fault_lr && lr == fault_lr) {
      ctx.r3.u64 = 0;
      static std::atomic<uint32_t> nl{0};
      const uint32_t z = nl.fetch_add(1, std::memory_order_relaxed);
      if (z < 4)
        xamlog::Line("SIGNINFAULTLR#%u t=%ldms forced user=%u -> 0 at lr=%08X\n",
                     z, long(ibwatch::SinceArmMs()), idx, lr);
    }
    if (fault && lr == 0x82840A18u) {
      ctx.r3.u64 = 0;  // pretend this user has no profile
      static std::atomic<uint32_t> nf{0};
      const uint32_t z = nf.fetch_add(1, std::memory_order_relaxed);
      if (z < 4)
        xamlog::Line("SIGNINFAULT#%u t=%ldms forced user=%u -> 0 at lr=%08X\n",
                     z, long(ibwatch::SinceArmMs()), idx, lr);
    }
  }
  // M3.247 (DEFAULT ON, kill-switch RESTUFF_NO_SIGNIN_FIX=1): THE SIGN-IN FIX.
  //
  // Root cause, measured by M3.246 PADLOG: our runtime reports TWO controllers
  // connected --
  //     user=0 -> 00000000 (SUCCESS)     user=1 -> 00000000 (SUCCESS)
  //     user=2 -> 0000048F (NOT_CONNECTED)  user=3 -> 0000048F
  // -- while XamUserGetSigninState answers "signed in" for user 0 only. So the
  // title sees a CONNECTED CONTROLLER WITH NO PROFILE on pad 1 and raises its
  // own StateSigninPrompt; choosing "Sign In" then enters the no-op
  // XamShowSigninUI stub and never returns. That matches the live capture
  // (sub_82840A00 queried user=1 and got 0 at t=18.8s, a call healthy boots
  // never make) and explains the intermittency without invoking load.
  //
  // Fix: report the local user as signed in for ANY pad we claim is connected.
  // That is the truthful answer for this runtime -- there is one local user, and
  // if we are going to present pad 1 as a controller then it belongs to them.
  // Deliberately NOT "force every index to 1" (that would invent profiles for
  // genuinely absent pads 2/3, which the title can legitimately distinguish),
  // and deliberately not "report pad 1 disconnected" (that would break a real
  // second controller if one is actually present).
  {
    static const bool disabled = getenv("RESTUFF_NO_SIGNIN_FIX") != nullptr;
    // M3.258 (DEFAULT ON, same kill-switch): slot 0 is upgraded UNCONDITIONALLY.
    //
    // Measured correction to the M3.246/M3.247 premise above: pad 1 now reports
    // 0000048F (NOT connected) WITHOUT RESTUFF_PAD1_OFF -- the phantom second
    // controller is simply not present in this build, so "connected pad with no
    // profile" cannot be the trigger any more, and PAD1_OFF is a no-op.
    //
    // What IS measured (RESTUFF_SIGNCACHE): sub_82B2F708 caches the four slot
    // states ONCE at *(profileMgr+136+4*slot) and the title reads only that
    // cache afterwards. A normal boot caches [1 0 0 0]. The only way the title
    // can end up believing nobody is signed in is for slot 0 to answer 0 at that
    // single poll -- a startup race on our own user state, which also explains
    // the ~12% intermittency.
    //
    // The gate below cannot cover it: padmask is 0 at poll time (logged), so the
    // upgrade never fires (SIGNINFIX count is 0). For slot 0 the pad gate is
    // wrong anyway -- this runtime always has exactly one local user, whether or
    // not a pad has been enumerated yet. Answering "signed in locally" for slot 0
    // is the truthful, order-independent answer, and it can neither invent nor
    // hide a controller.
    if (!disabled && idx == 0 && ctx.r3.u32 == 0) {
      ctx.r3.u64 = 1;  // eSIGNIN_LOCALLY -- the one local user, always
      static std::atomic<uint32_t> nz{0};
      const uint32_t z = nz.fetch_add(1, std::memory_order_relaxed);
      if (z < 8)
        xamlog::Line("SIGNINFIX0#%u t=%ldms user=0 0->1 (RACE CAUGHT: XAM said"
                     " not-signed-in) lr=%08X\n",
                     z, long(ibwatch::SinceArmMs()), lr);
    }
    if (!disabled && idx < 4 && ctx.r3.u32 == 0 &&
        (g_pad_connected.load(std::memory_order_relaxed) & (1u << idx))) {
      ctx.r3.u64 = 1;  // eSIGNIN_LOCALLY
      static std::atomic<uint32_t> nfix{0};
      const uint32_t f = nfix.fetch_add(1, std::memory_order_relaxed);
      if (f < 8)
        xamlog::Line("SIGNINFIX#%u t=%ldms user=%u 0->1 (pad reported CONNECTED)"
                     " lr=%08X\n",
                     f, long(ibwatch::SinceArmMs()), idx, lr);
    }
  }
  if (on) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 60 || ctx.r3.u32 != 1)
      xamlog::Line("XAM#%u t=%ldms GetSigninState(user=%u) -> %u  lr=%08X\n", k,
                   long(ibwatch::SinceArmMs()), idx, ctx.r3.u32, lr);
  }
}
REX_EXTERN(__imp__sub_830B31A8);
REX_HOOK_RAW(sub_830B31A8) {  // the signin-state check that gates the prompt
  static const bool on = getenv("RESTUFF_XAMLOG") != nullptr;
  const uint32_t idx = ctx.r3.u32, outp = ctx.r5.u32, lr = uint32_t(ctx.lr);
  __imp__sub_830B31A8(ctx, base);
  if (on) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    uint32_t outv = 0xFFFFFFFFu;
    if (outp >= 0x10000 && outp < 0xC0000000u - 4) {
      std::memcpy(&outv, base + outp, 4);
      outv = __builtin_bswap32(outv);
    }
    if (k < 60 || outv != 0)
      xamlog::Line("XAMCHK#%u t=%ldms check(user=%u) out=%08X lr=%08X\n", k,
                   long(ibwatch::SinceArmMs()), idx, outv, lr);
  }
}
// M3.241 (RESTUFF_MEMOLOG=1): THE ACTUAL SITE WHERE COMPOSITION STOPS.
//
// sub_82AD8AB0 (recomp.51.cpp:2714) does two things in order:
//     sub_82B0CBC8(...)          // the light-list REBUILD -- unconditional
//     ... then a memoisation check, and only if it FAILS:
//     sub_82AD3460(...)          // -> the composer sub_82AD3050 (all 1525 LL
//                                //    rows in dirty11 carry lr=82AD350C, i.e.
//                                //    this one call site, so there is no
//                                //    caller diversity to chase)
// The check is:
//     if (sub_82B0A7D0(...) == 0) skip;
//     same = memcmp(node+64, cur+0, 32) == 0;      // FOUR 64-bit words
//     if (same && *(node+104) == *(0x836E7E74)) skip;   // generation match
//     else compose;
// i.e. "state block unchanged AND generation already current -> don't recompose".
//
// This explains the one thing the earlier models could not: MASK# (logged inside
// sub_82B0CBC8) keeps flowing on CLEAN boots to 249s while LL (the composer)
// stops dead at ~100s -- because the rebuild is BEFORE the check and the
// composer is AFTER it. So the wedge is a MEMOISATION MISS, and there are
// exactly two ways to miss: the 32-byte block keeps changing, or the generation
// never matches. These counters separate them.
//
// (This supersedes the M3.239 +732 dirty-flag theory, which the first dirty11
// data killed outright: set=4/5 versus clear=1820+, clrseen == clrtaken == clear
// so the gate is always open, and glob=0 means that path is not even taken.)
// M3.243: the memo model is REFUTED as the wedge discriminator -- memo11 (WEDGE
// 285dr) and memo12 (CLEAN 79dr) have IDENTICAL counters (late calls +32550 vs
// +32536, same 100% both, genmatch 0.0% both, compose == calls both). So
// everything at sub_82AD8AB0 behaves the same on both boot kinds and the
// divergence is strictly BELOW it, inside sub_82AD3460's recursive walk:
//
//   sub_82AD3460(ctx, node, desc, out):
//     same2 = 32-byte block node+48..80 vs desc+0..32   (a SECOND such compare)
//     if (same2 && *(desc+16) == 0 && *(desc+24) != 0) sub_82AD3050(...)  // compose
//     for (child in list at node+136, stride 144) sub_82AD3460(child ...)  // recurse
//
// Two counters settle where it splits: entries into the recursion, and actual
// composer calls. Equal entries with different composer counts => the internal
// gate is the discriminator; different entry counts => the child list / walk is.
REX_EXTERN(__imp__sub_82AD3460);
REX_HOOK_RAW(sub_82AD3460) {
  g_rec_calls.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82AD3460(ctx, base);
}

static std::atomic<uint64_t> g_memo_calls{0}, g_memo_same{0}, g_memo_gen{0},
    g_memo_compose{0};
static std::atomic<uint32_t> g_memo_gennow{0}, g_memo_gennode{0};
// The memo check is only REACHED when sub_82B0A7D0(...) != 0 (the test directly
// before it). That is a third way composition could stop -- clean boots might
// bail HERE rather than hit the memo -- and without counting it, a clean and a
// wedge boot could show identical same%/genmatch% while differing entirely.
static std::atomic<uint64_t> g_gate_calls{0}, g_gate_pass{0};
REX_EXTERN(__imp__sub_82B0A7D0);
REX_HOOK_RAW(sub_82B0A7D0) {
  __imp__sub_82B0A7D0(ctx, base);
  g_gate_calls.fetch_add(1, std::memory_order_relaxed);
  if ((ctx.r3.u32 & 0xFF) != 0) g_gate_pass.fetch_add(1, std::memory_order_relaxed);
}
REX_EXTERN(__imp__sub_82AD8AB0);
REX_HOOK_RAW(sub_82AD8AB0) {
  static const bool on = getenv("RESTUFF_MEMOLOG") != nullptr;
  if (on) {
    const uint32_t node = ctx.r3.u32, cur = ctx.r6.u32;
    if (node >= 0x10000 && node < 0xC0000000u - 112 && cur >= 0x10000 &&
        cur < 0xC0000000u - 32) {
      const bool same = std::memcmp(base + node + 64, base + cur, 32) == 0;
      uint32_t gnode = 0, gnow = 0;
      std::memcpy(&gnode, base + node + 104, 4);
      std::memcpy(&gnow, base + 0x836E7E74, 4);
      gnode = __builtin_bswap32(gnode);
      gnow = __builtin_bswap32(gnow);
      g_memo_calls.fetch_add(1, std::memory_order_relaxed);
      if (same) g_memo_same.fetch_add(1, std::memory_order_relaxed);
      if (gnode == gnow) g_memo_gen.fetch_add(1, std::memory_order_relaxed);
      if (!(same && gnode == gnow))
        g_memo_compose.fetch_add(1, std::memory_order_relaxed);
      g_memo_gennow.store(gnow, std::memory_order_relaxed);
      g_memo_gennode.store(gnode, std::memory_order_relaxed);
      const long t = long(ibwatch::SinceArmMs());
      static std::atomic<long> s_last{-1};
      const long sec = t / 1000;
      long prev = s_last.load(std::memory_order_relaxed);
      if (t > 0 && sec != prev && s_last.compare_exchange_strong(prev, sec)) {
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f,
                  "MEMO t=%ldms calls=%llu same=%llu genmatch=%llu compose=%llu "
                  "gennow=%08X gennode=%08X gate=%llu gatepass=%llu "
                  "rec=%llu comp=%llu\n",
                  t, (unsigned long long)g_memo_calls.load(std::memory_order_relaxed),
                  (unsigned long long)g_memo_same.load(std::memory_order_relaxed),
                  (unsigned long long)g_memo_gen.load(std::memory_order_relaxed),
                  (unsigned long long)g_memo_compose.load(std::memory_order_relaxed),
                  gnow, gnode,
                  (unsigned long long)g_gate_calls.load(std::memory_order_relaxed),
                  (unsigned long long)g_gate_pass.load(std::memory_order_relaxed),
                  (unsigned long long)g_rec_calls.load(std::memory_order_relaxed),
                  (unsigned long long)g_comp_calls.load(std::memory_order_relaxed));
          fclose(f);
        }
      }
    }
  }
  __imp__sub_82AD8AB0(ctx, base);
}

// M3.239 (RESTUFF_DIRTYLOG=1): WHY does composition never stop on wedge boots?
//
// Aug-11 ll1 re-analysis narrowed the wedge to a single binary event. All boots
// compose identically from 60-100s; at t~100s clean boots STOP calling the
// composer entirely (last LL 100.2s, process alive to 260s) while wedge boots
// keep going at ~145 samples/10s to the end. Crucially SCHED / MERGER / QKEY
// keep running at IDENTICAL rates on both (3000/1172/4462 per 20s), so nothing
// halts globally -- only the light-list rebuild path stops. That makes the
// wedge a STUCK-DIRTY problem, not a lighting problem.
//
// The gate is in sub_8297E410 (recomp.39.cpp:51060+), which reaches the prune
// (sub_829C7970) and the dyntree (sub_829E7538 -> sub_82ADB160 -> sub_82AD8AB0
// -> sub_82B0CBC8 -> composer sub_82AD3050) only when ALL of:
//     *(r3 + 4)            != 0     (byte flag on the object)
//     *(0x83341E9C)        != 0     (global byte)
//     *(*(r3 + 0) + 732)   != 0     (per-scene byte -- the dirty-flag candidate)
// So log those three once a second. If the +732 byte drops to 0 at ~100s on
// clean boots and stays nonzero on wedge boots, it IS the flag, and the next
// question is simply who clears it (keywatch its address -- it is a stable
// guest address, unlike the stack owners MASKLOG found).
// The +732 byte has exactly TWO writers in the whole image, and they are a
// matched dirty/clear pair (found by grepping REX_STORE_U8(... + 732)):
//   sub_829CC8C0 (recomp.42.cpp:15055)  li r11,1 ; stb r11,732(r31)  -> SET
//   sub_829CCB48 (recomp.42.cpp:15352)  li r27,0 ; stb r27,732(r28)  -> CLEAR
// and sub_829CCB48 reads 532(r28) just before clearing -- the same list offset
// the prune sub_829C7970 walks -- so it is the "process the queue, then mark
// clean" step. Counting both per second separates the only two ways the flag
// can stay stuck: something keeps RE-DIRTYING it (set count stays high), or the
// CLEAR stops running / bails before the store (clear count drops to zero).
// And the CLEAR is CONDITIONAL at its only call site, sub_82965290
// (recomp.38.cpp:62013):
//     if (*(r3 + 2484) == 0) goto skip;
//     if (*(r3 + 2464) == 0) goto skip;
//     sub_829CCB48(r3 + 112);        // process the list, then 732 = 0
// so a null in EITHER field means the dirty flag is never cleared, composition
// never stops, and keys keep being re-minted during the single-light era --
// exactly the wedge signature. g_clr_seen counts entries to sub_82965290,
// g_clr_taken counts the times both fields passed, so "the gate is closed"
// and "the caller stopped running" are distinguishable.
static std::atomic<uint64_t> g_dirty_set{0}, g_dirty_clear{0};
static std::atomic<uint64_t> g_clr_seen{0}, g_clr_taken{0};
static std::atomic<uint32_t> g_clr_f2484{0}, g_clr_f2464{0};
REX_EXTERN(__imp__sub_82965290);
REX_HOOK_RAW(sub_82965290) {
  const uint32_t self = ctx.r3.u32;
  if (self >= 0x10000 && self < 0xC0000000u - 2500) {
    uint32_t a = 0, b = 0;
    std::memcpy(&a, base + self + 2484, 4);
    std::memcpy(&b, base + self + 2464, 4);
    a = __builtin_bswap32(a);
    b = __builtin_bswap32(b);
    g_clr_f2484.store(a, std::memory_order_relaxed);
    g_clr_f2464.store(b, std::memory_order_relaxed);
    g_clr_seen.fetch_add(1, std::memory_order_relaxed);
    if (a && b) g_clr_taken.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__sub_82965290(ctx, base);
}
REX_EXTERN(__imp__sub_829CC8C0);
REX_HOOK_RAW(sub_829CC8C0) {
  g_dirty_set.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_829CC8C0(ctx, base);
}
REX_EXTERN(__imp__sub_829CCB48);
REX_HOOK_RAW(sub_829CCB48) {
  g_dirty_clear.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_829CCB48(ctx, base);
}

REX_EXTERN(__imp__sub_8297E410);
REX_HOOK_RAW(sub_8297E410) {
  static const bool on = getenv("RESTUFF_DIRTYLOG") != nullptr;
  if (on) {
    const long t = long(ibwatch::SinceArmMs());
    static std::atomic<long> s_last{-1};
    const long sec = t / 1000;
    long prev = s_last.load(std::memory_order_relaxed);
    if (t > 0 && sec != prev && s_last.compare_exchange_strong(prev, sec)) {
      const uint32_t obj = ctx.r3.u32;
      auto rd8 = [&](uint32_t a) -> int {
        if (a < 0x10000 || a >= 0xC0000000u) return -1;
        return *(base + a);
      };
      uint32_t sub = 0;
      if (obj >= 0x10000 && obj < 0xC0000000u - 4) {
        std::memcpy(&sub, base + obj, 4);
        sub = __builtin_bswap32(sub);
      }
      if (FILE* f = fopen(restuff_logpath(), "a")) {
        fprintf(f,
                "DIRTY t=%ldms obj=%08X f4=%d glob=%d sub=%08X dirty732=%d "
                "set=%llu clear=%llu clrseen=%llu clrtaken=%llu "
                "f2484=%08X f2464=%08X\n",
                t, obj, rd8(obj + 4), rd8(0x83341E9C), sub,
                sub ? rd8(sub + 732) : -1,
                (unsigned long long)g_dirty_set.load(std::memory_order_relaxed),
                (unsigned long long)g_dirty_clear.load(std::memory_order_relaxed),
                (unsigned long long)g_clr_seen.load(std::memory_order_relaxed),
                (unsigned long long)g_clr_taken.load(std::memory_order_relaxed),
                g_clr_f2484.load(std::memory_order_relaxed),
                g_clr_f2464.load(std::memory_order_relaxed));
        fclose(f);
      }
    }
  }
  __imp__sub_8297E410(ctx, base);
}

// M3.251 (RESTUFF_SMLOG=1): OBSERVE the title's sign-in state machine.
//
// Four XAM-layer MANIPULATIONS have now failed or been confounded (M3.224
// notification ordering, M3.226 slot cache, M3.247 per-slot signin state,
// M3.248 phantom-pad removal -- the last one demonstrably perturbs the drive,
// p=0.0014, so it cannot even serve as a causal test). The popup is the title's
// OWN StateSigninPrompt and provably never calls XamShowSigninUI. So stop
// manipulating and observe the state machine itself.
//
// The notify pump sub_82B33318 dispatches XN_SYS_UI into
// sub_82B3B5F8(r30+84, param-1, ...), and that function reads *(r3+4) -- a
// small state id (1/2/3, matching the AVSignInStateUiNotShown / ShowingUi /
// UiShown RTTI names) -- and dispatches on it. Logging the state BEFORE and
// AFTER each event shows the exact transition sequence, and on a dialog boot
// which transition precedes the prompt. Observation only; changes nothing, so
// it cannot confound itself the way PAD1_OFF did.
REX_EXTERN(__imp__sub_82B3B5F8);
REX_HOOK_RAW(sub_82B3B5F8) {
  static const bool on = getenv("RESTUFF_SMLOG") != nullptr;
  if (!on) { __imp__sub_82B3B5F8(ctx, base); return; }
  const uint32_t self = ctx.r3.u32, arg = ctx.r4.u32, lr0 = uint32_t(ctx.lr);
  auto rd = [&](uint32_t a) {
    uint32_t v = 0;
    if (a >= 0x10000 && a < 0xC0000000u - 4) {
      std::memcpy(&v, base + a, 4);
      v = __builtin_bswap32(v);
    }
    return v;
  };
  const uint32_t before = rd(self + 4);
  __imp__sub_82B3B5F8(ctx, base);
  const uint32_t after = rd(self + 4);
  static std::atomic<uint32_t> n{0};
  const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
  if (k < 200 || before != after)
    xamlog::Line("SM#%u t=%ldms obj=%08X state %u->%u arg=%08X ret=%08X lr=%08X\n", k,
                 long(ibwatch::SinceArmMs()), self, before, after, arg,
                 ctx.r3.u32, lr0);
}

// Set once the TITLE actually asks for the sign-in overlay (XamShowSigninUI).
// Until then, any XN_SYS_UI transition we deliver is a phantom -- see M3.242.
static std::atomic<bool> g_signin_ui_requested{false};

// M3.237 (RESTUFF_NOTIFYLOG=1): what notifications do we actually deliver?
//
// The Aug-11 archived trace from a STUCK boot (signin_hang_trace_xam12.txt)
// kills the "transient signin race" theory outright: XamUserGetSigninState
// (user=0) answers 1 on EVERY call, including during the hang -- zero
// occurrences of "-> 0" in the whole trace. So the prompt is NOT gated on the
// signin state, and the 4-pad polls at lr=82B335C0 are a CONSEQUENCE, not the
// cause: they sit inside sub_82B33318, which is the title's NOTIFICATION PUMP.
// It calls sub_83233304(handle, 0, &id, &param) -- the XNotifyGetNext shape --
// and switches on id 9/10/11/15, i.e. XN_SYS_UI(9) and XN_SYS_SIGNINCHANGED
// (10). The ~3s retry cadence is therefore driven by what we push into that
// queue, which is exactly what this logs: every notification the guest dequeues,
// with its param. If a spurious XN_SYS_SIGNINCHANGED (the SDK broadcasts one
// from XamShowSigninUI, and enqueues UI on/off at listener registration) is
// what makes the front-end re-run its gate and raise StateSigninPrompt, it
// shows up here as a notification that arrives just before the prompt does.
// NOTE: 0x83233304 is NOT recompiled guest code -- restuff_init.cpp:69754 maps
// it to `__imp__XNotifyGetNext`, which both CONFIRMS the identification and
// means it must be interposed as a dynamic import (strong def here wins,
// forward via dlsym(RTLD_NEXT)), exactly like __imp__KeWaitForMultipleObjects
// above. REX_EXTERN/REX_HOOK_RAW(sub_83233304) would not link.
#ifndef _WIN32
extern "C" REX_FUNC(__imp__XNotifyGetNext) {
  using Fn = void (*)(PPCContext&, uint8_t*);
  static Fn orig = reinterpret_cast<Fn>(dlsym(RTLD_NEXT, "__imp__XNotifyGetNext"));
  static const bool on = getenv("RESTUFF_NOTIFYLOG") != nullptr;
  if (!orig) {
    REXLOG_ERROR("[NOTIFY] original XNotifyGetNext not found");
    return;
  }
  const uint32_t idp = ctx.r5.u32, parp = ctx.r6.u32;
  orig(ctx, base);
  if (ctx.r3.u32 == 0) return;  // returns false when the queue is empty
  auto rd0 = [&](uint32_t a) {
    uint32_t v = 0;
    if (a >= 0x10000 && a < 0xC0000000u - 4) {
      std::memcpy(&v, base + a, 4);
      v = __builtin_bswap32(v);
    }
    return v;
  };
  // M3.242 (RESTUFF_SIGNIN_UIFILTER=1, opt-in experiment): drop the PHANTOM
  // system-UI notifications the title never asked for.
  //
  // The title's pump sub_82B33318 dispatches XN_SYS_UI (id 9) straight into
  // what is almost certainly its sign-in state machine:
  //     if (id == 9) sub_82B3B5F8(r30 + 84, param - 1, ...);
  // (the image carries AVSignInStateMachine / AVSignInStateUiNotShown /
  //  AVSignInStateShowingUi / AVSignInStateUiShown), while id 10
  //  (XN_SYS_SIGNINCHANGED) only stores its param at *(r30+168).
  //
  // But M3.237 measured an XN_SYS_UI on->off PAIR arriving at ~44s on EVERY
  // healthy boot, delivered to four listeners -- and no UI was ever requested
  // at that point. That pair is the SDK's listener-registration artefact
  // (kernel_state.cpp enqueues UI on then off when a listener registers). So
  // the title's sign-in state machine is being told an overlay opened and
  // closed before anything asked for one, which is exactly the kind of phantom
  // transition that could leave it in a state where it later raises
  // StateSigninPrompt -- and it is timing-dependent, matching the ~40%
  // intermittency that no static return value could explain.
  //
  // The experiment: swallow XN_SYS_UI until the title actually calls
  // XamShowSigninUI. Dropping is safe because the guest polls this queue every
  // frame; reporting "empty" just means it asks again. If the prompt stops
  // appearing across a batch, the phantom pair is the root and the real fix
  // belongs upstream in the SDK (do not enqueue a UI transition for a UI that
  // was never shown).
  {
    static const bool filt = getenv("RESTUFF_SIGNIN_UIFILTER") != nullptr;
    if (filt && rd0(idp) == 0x00000009u &&
        !g_signin_ui_requested.load(std::memory_order_acquire)) {
      static std::atomic<uint32_t> dropped{0};
      const uint32_t d = dropped.fetch_add(1, std::memory_order_relaxed);
      if (d < 16)
        xamlog::Line("NOTIFYDROP#%u t=%ldms id=00000009 param=%08X (phantom UI)\n",
                     d, long(ibwatch::SinceArmMs()), rd0(parp));
      ctx.r3.u32 = 0;  // report "queue empty"; the guest polls again next frame
      return;
    }
  }
  if (!on) return;
  auto rd = [&](uint32_t a) {
    uint32_t v = 0;
    if (a >= 0x10000 && a < 0xC0000000u - 4) {
      std::memcpy(&v, base + a, 4);
      v = __builtin_bswap32(v);
    }
    return v;
  };
  static std::atomic<uint32_t> n{0};
  const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
  if (k < 400 || (k % 500) == 0)
    xamlog::Line("NOTIFY#%u t=%ldms id=%08X param=%08X lr=%08X\n", k,
                 long(ibwatch::SinceArmMs()), rd(idp), rd(parp), uint32_t(ctx.lr));
}
#endif  // !_WIN32

REX_EXTERN(__imp__sub_830B2F68);
REX_HOOK_RAW(sub_830B2F68) {  // XamShowSigninUI -- the game asking for the blade
  // From here on a system-UI transition is legitimate, so M3.242 stops
  // filtering XN_SYS_UI. Set unconditionally (not under RESTUFF_XAMLOG).
  g_signin_ui_requested.store(true, std::memory_order_release);
  static const bool on = getenv("RESTUFF_XAMLOG") != nullptr;
  if (on) {
    // lr is inside sub_82B2DE68 (0x82B2DEB4); walk the guest stack to find the
    // return into whichever SCREEN invoked it, so a stuck boot names the caller
    // that my sub_82B2E030 fix must actually cover. The 4 callers of
    // sub_82B2DE68 are 0x827E3270 (sub_827E2F08), 0x82820A6C, 0x82820A7C,
    // 0x82877FD8 — scan the stack window for one of those return addresses.
    uint32_t screen_lr = 0;
    for (uint32_t off = 0; off <= 256 && !screen_lr; off += 4) {
      uint32_t w;
      std::memcpy(&w, base + ((ctx.r1.u32 + off) & 0x7FFFFFFF), 4);
      w = __builtin_bswap32(w);
      if (w == 0x827E3270u || w == 0x82820A6Cu || w == 0x82820A7Cu || w == 0x82877FD8u)
        screen_lr = w;
    }
    xamlog::Line("XAMSHOW t=%ldms ShowSigninUI(r3=%08X r4=%08X) lr=%08X screen_ret=%08X\n",
                 long(ibwatch::SinceArmMs()), ctx.r3.u32, ctx.r4.u32, uint32_t(ctx.lr),
                 screen_lr);
  }
  // M3.224 (DEFAULT ON, kill-switch RESTUFF_NO_SIGNIN_FIX=1): make the
  // sign-in overlay COMPLETE so the title stops looping and hanging.
  //
  // USER-FACING BUG, not a harness artifact: the "sign in with a gamer
  // profile?" prompt hangs the game for anyone — a slower machine, or just
  // background load (a video open) that delays the boot — and answering it
  // makes it worse. The decompiled loop is exact. The game's prompt gate
  // sub_82B2DD30 returns true while a UI-state object has field +8 in {3,4}
  // AND its "done" field +0x4C == 0; the outer step sub_82B2DE68 then calls
  // XamShowSigninUI (thunk sub_830B2F68) and retries every ~3s (xam12 trace:
  // ShowSigninUI at 77374/80341/83241/... ms) because +0x4C never gets set.
  //
  // Root: the SDK's XamShowSigninUI (xam_user.cpp:492) broadcasts
  // XN_SYS_SIGNINCHANGED then XN_SYS_UI(OFF) — a UI-off with NO preceding
  // UI-on. The game's listener only advances its state machine on the on->off
  // TRANSITION (the SDK's own new-listener path, kernel_state.cpp:797-798,
  // enqueues UI on THEN off for exactly this reason), so a lone off is
  // ignored and the overlay it is waiting to see open never opens.
  //
  // Fix: send UI-ON before the SDK's pair, so the guest observes the full
  // on -> SIGNINCHANGED -> off sequence a real sign-in produces and the gate's
  // +0x4C is set. The profile is already reported signed-in (state 1), so this
  // is the honest completion, not a fabricated state. Idempotent and cheap;
  // default-on because the prompt is a hard hang when it is hit. The old
  // env-GATED M3.219 never executed in any batch (it only fired on boots that
  // did not hang), which is why this ships default-on WITH a kill-switch
  // instead.
  // ⚠️ DISPROVEN (sfix2): this fired 28 times on stuck boots and the ~3s
  // ShowSigninUI loop CONTINUED (237/240/243/246s). The game's prompt gate is
  // NOT released by an XN_SYS_UI(on) broadcast — it loops on the state object's
  // +0x4C flag (sub_82B2DD30), which this does not set. Left OFF by default;
  // the real fix drives that flag (M3.225 below). Opt in only for experiments.
  static const bool try_ui = getenv("RESTUFF_SIGNIN_UIFIX") != nullptr;
  if (try_ui) {
    if (auto* ks = REX_KERNEL_STATE()) ks->BroadcastNotification(0x00000009, 1);
  }
  __imp__sub_830B2F68(ctx, base);
}
// M3.225 (RESTUFF_SIGNINLOG=1): the prompt gate's state object.
//
// sub_82B2DD30 decides whether to (re)show the prompt: it returns "show" while
// *(a1+76) is non-null with mode field +8 in {3,4} AND resolved field +0x4C
// == 0. The loop persists because +0x4C never becomes non-zero. Log the object
// and both fields per call so a stuck boot shows exactly what is frozen — and
// whether forcing +0x4C=1 (mark resolved, what a real dismissal does) is the
// intervention. Read-only here; the forcing variant is gated separately.
// M3.226 (DEFAULT ON, kill-switch RESTUFF_NO_SIGNIN_FIX=1): make the sign-in
// prompt NOT APPEAR, which is what the user asked for — you cannot dismiss it,
// so it must not show.
//
// The sign-in SCREEN sub_827E2F08 scans slots 0..3 via sub_82B2E030(screen,
// slot); if any returns 1 or 2 (a valid signed-in user) it sets its
// "user found" flag and SKIPS the prompt entirely. Healthy boots reach
// gameplay precisely because a slot reports a user in time; slow/loaded boots
// lose that race — the per-slot cache is still 0 when the screen renders — so
// no user is found and it enters the ShowSigninUI loop (which then hangs,
// because XamShowSigninUI signs nobody in).
//
// Fix: for the screen's OWN call (return address 0x827E3214, the bl inside
// sub_827E2F08) with slot 0, report a valid user (1). This reproduces the
// healthy path exactly — user 0 IS reported signed in (XamUserGetSigninState
// (0)==1, GetName/GetXUID valid), so claiming a user on slot 0 is consistent,
// not fabricated. The screen sets its flag, skips the prompt, and proceeds.
// Only this one call site is affected; the other 17 callers of sub_82B2E030
// are untouched. Default-on because the prompt is a hard hang.
REX_EXTERN(__imp__sub_82B2E030);
REX_HOOK_RAW(sub_82B2E030) {
  static const bool disabled = getenv("RESTUFF_NO_SIGNIN_FIX") != nullptr;
  static const bool log = getenv("RESTUFF_SIGNINLOG") != nullptr;
  const uint32_t lr = uint32_t(ctx.lr), slot = ctx.r4.u32, scr = ctx.r3.u32;
  __imp__sub_82B2E030(ctx, base);
  const uint32_t orig = ctx.r3.u32;
  if (log) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 60) xamlog::Line("USERCHK#%u t=%ldms lr=%08X scr=%08X slot=%u -> %u\n", k,
                             long(ibwatch::SinceArmMs()), lr, scr, slot, orig);
  }
  // M3.226c: correct the stale-cache RACE for slot 0. USERCHK proved slot 0
  // returns 1 (user 0 signed in) in the normal case and slots 1-3 return 0,
  // matching XamUserGetSigninState (user 0 == 1). The hang is a window where
  // slot 0's per-screen cache transiently reads 0 before it is populated; a
  // screen that renders in that window finds no user and enters the
  // ShowSigninUI loop. Forcing slot 0's 0 -> 1 corrects that stale read to the
  // global truth (user 0 IS signed in), so it is consistent for every caller,
  // not just the one screen — the earlier LR filter (0x827E3214) targeted the
  // wrong call site (the real one is 0x82878D58) and never fired. Only 0 -> 1
  // for slot 0; real 1/2 values and all other slots pass through untouched.
  if (!disabled && slot == 0 && orig == 0) {
    ctx.r3.u64 = 1;
  }
  (void)lr;
}

// M3.227 (RESTUFF_SIGNINLOG=1): name the screen that drives the ShowSigninUI
// loop. M3.226c is DISPROVEN — the stuck boot's slot-0 user check returns 1
// identically to the healthy boots (fix226c: 120/120 slot0->1, stuck boot
// included), so "no user found" is not the fork. The ShowSigninUI loop appears
// only on the stuck boot, starting ~14.7s, and the stack-walk for screen_ret
// found nothing. ctx.lr at THIS hook is the exact return address into the
// caller of sub_82B2DE68 (the "if gate then ShowSigninUI" step), so it names
// the screen precisely.
REX_EXTERN(__imp__sub_82B2DE68);
REX_HOOK_RAW(sub_82B2DE68) {
  static const bool on = getenv("RESTUFF_SIGNINLOG") != nullptr;
  if (on) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 30)
      xamlog::Line("SIGNSTEP#%u t=%ldms caller_lr=%08X r3=%08X\n", k,
                   long(ibwatch::SinceArmMs()), uint32_t(ctx.lr), ctx.r3.u32);
  }
  // M3.228 (RESTUFF_SIGNIN_SKIP=1): DETERMINISTIC de-fang. This step is the
  // sole ShowSigninUI trigger and the gate is always armed (mode 4, resolved 0
  // on every boot), so whenever a menu handler reaches here it hangs on the
  // no-op ShowSigninUI stub. Return 0 (the gate-false result) WITHOUT calling
  // through, so ShowSigninUI never runs — the "Sign In" action becomes inert
  // instead of a hang. Does NOT touch the per-slot check sub_82B2E030 (which
  // also calls the gate), so the auto-scan screen is unaffected. Gated until
  // proven; if it stops the hang without breaking "Continue Without Sign In",
  // it becomes the fix.
  // DEFAULT ON (kill-switch RESTUFF_NO_SIGNIN_FIX=1). "Continue Without Sign
  // In" cannot route here — a wrapper that calls the ShowSigninUI trigger IS
  // the "Sign In" action by construction — so de-fanging only neuters the
  // hanging path. Proven to remove the loop (res1/force-resolve drove
  // XAMSHOW->0), but NOT verifiable headless: the poll-keyed replay cannot
  // navigate the dialog to "Continue," so de-fanged boots still park in the
  // harness (res1 seg3/seg11: XAMSHOW=0 yet NOTGAMEPLAY). Needs a human to
  // confirm the dialog is answerable on a real run.
  static const bool disabled = getenv("RESTUFF_NO_SIGNIN_FIX") != nullptr;
  if (!disabled) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82B2DE68(ctx, base);
}

REX_EXTERN(__imp__sub_82B2DD30);
REX_HOOK_RAW(sub_82B2DD30) {
  static const bool on = getenv("RESTUFF_SIGNINLOG") != nullptr;
  static const bool force = getenv("RESTUFF_SIGNIN_RESOLVE") != nullptr;
  const uint32_t a1 = ctx.r3.u32;
  uint32_t obj = 0, mode = 0xFFFFFFFF, resolved = 0xFFFFFFFF;
  if (a1 >= 0x10000 && a1 < 0xC0000000u - 80) {
    std::memcpy(&obj, base + a1 + 76, 4);
    obj = __builtin_bswap32(obj);
    if (obj >= 0x10000 && obj < 0xC0000000u - 0x50) {
      std::memcpy(&mode, base + obj + 8, 4);   mode = __builtin_bswap32(mode);
      std::memcpy(&resolved, base + obj + 0x4C, 4); resolved = __builtin_bswap32(resolved);
      // M3.225b: mark the prompt RESOLVED, exactly what a successful dismissal
      // does — the gate then returns 0 and the loop ends. Gated; prove it
      // reaches gameplay before trusting it.
      if (force && (mode == 3 || mode == 4) && resolved == 0) {
        const uint32_t one = __builtin_bswap32(1u);
        std::memcpy(base + obj + 0x4C, &one, 4);
      }
    }
  }
  if (on) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 40 || (k % 200) == 0)
      xamlog::Line("SIGNGATE#%u t=%ldms a1=%08X obj=%08X mode=%d resolved=%08X\n", k,
                   long(ibwatch::SinceArmMs()), a1, obj, int(mode), resolved);
  }
  __imp__sub_82B2DD30(ctx, base);
}

REX_EXTERN(__imp__sub_8297ED80);
REX_HOOK_RAW(sub_8297ED80) {
  mergegate::g_ed80.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_8297ED80(ctx, base);
}
REX_EXTERN(__imp__sub_829C77B8);
REX_HOOK_RAW(sub_829C77B8) {
  mergegate::g_77b8.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_829C77B8(ctx, base);
}
// M3.216 (RESTUFF_SELLOG=1): THE SELECTOR's passing set.
//
// Reached by elimination, not by picking another field to believe in. Every
// measurable property of the merge subsystem is family-blind: call volume,
// merger inputs (byte-identical over the sampled prologue on boots that ended
// 79/79/285 draws), burst start and shape (mk1 boot1 MERGED 68809ms
// 342/570/1124 vs boot3 WEDGE 68866ms 342/548/1146 — near-identical, opposite
// outcomes), and the resulting sector table (a merged boot scored LOWER than a
// wedge boot). The merger runs ~4000x/SECOND, i.e. per FRAME over the visible
// set — so 285-vs-79 is decided by what the render walk FEEDS it.
//
// sub_8297ED80 tests each object on a per-VIEW mode word and a flag bit, then
// calls this for the ones that PASS. Its rate is therefore the size of the
// passing set. If that rate does not separate the families either, the
// divergence is downstream of selection entirely and the emit path is next.
//
// Rate only, logged 1Hz: this fires per object per view per frame, so per-call
// lines would swamp the log and perturb the very timing under test.
REX_EXTERN(__imp__sub_829E8A70);
REX_HOOK_RAW(sub_829E8A70) {
  static const bool on = getenv("RESTUFF_SELLOG") != nullptr;
  // SEVEN functions call this; only sub_8297ED80 is the selector, so an
  // unfiltered rate would mix in six unrelated call sites and mean nothing.
  // The call is a plain bl, so the return address lands inside the caller.
  const bool from_selector = ctx.lr >= 0x8297ED80u && ctx.lr < 0x8297F5F8u;
  if (on && from_selector) {
    static std::atomic<uint32_t> s_n{0};
    static std::atomic<int64_t> s_last{0};
    const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
    const int64_t now = ibwatch::SinceArmMs();
    int64_t last = s_last.load(std::memory_order_relaxed);
    if (now - last >= 1000 && s_last.compare_exchange_strong(last, now)) {
      // One sampled object's 7x8B per-view record (mode at slot+4, flags at
      // slot+6) travels with the rate so the values can be diffed later.
      const uint32_t obj = ctx.r3.u32;
      char rec[128];
      rec[0] = 0;
      if (obj >= 0x10000 && obj < 0xC0000000u - 32) {
        uint32_t p;
        std::memcpy(&p, base + obj + 20, 4);
        p = __builtin_bswap32(p);
        if (p >= 0x10000 && p < 0xC0000000u - 128) {
          char* w = rec;
          for (int k = 0; k < 7; ++k) {
            uint16_t mode, flags;
            std::memcpy(&mode, base + p + 64 + 8 * k + 4, 2);
            std::memcpy(&flags, base + p + 64 + 8 * k + 6, 2);
            w += snprintf(w, sizeof(rec) - (w - rec), "%04X:%04X ",
                          __builtin_bswap16(mode), __builtin_bswap16(flags));
          }
        }
      }
      if (FILE* f = fopen(restuff_logpath(), "a")) {
        fprintf(f, "SEL t=%ldms passes=%u obj=%08X slots=%s\n", long(now), n, obj, rec);
        fclose(f);
      }
    }
  }
  __imp__sub_829E8A70(ctx, base);
}
REX_EXTERN(__imp__sub_829CF620);
REX_HOOK_RAW(sub_829CF620) {
  // M3.206 (RESTUFF_MERGELOG=1): the QUERY KEYS. Single callsite (inside
  // sub_829D0708) so args are deterministic: r6 = the key pointer. The
  // registry is static+identical across families (3 hashes); the wedge
  // boot's door sector must query a key missing from it — the key SETS
  // per family, diffed against the registry, end the hunt.
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      const long t = long(ibwatch::SinceArmMs());
      // M3.206b: window opens at 20s — mode-word diffs exist by 70s, so the
      // decisive queries (incl. the never-seen 3rd registry hash) land
      // BEFORE 55s; every prior residency window watched the aftermath.
      if (t >= 20000) {
        const uint32_t kp = ctx.r6.u32;
        uint64_t key = 0;
        if (kp >= 0x10000 && kp < 0xC0000000u - 8) {
          memcpy(&key, base + kp, 8);
          key = __builtin_bswap64(key);
        }
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
        if (n < 20000 || (n & 15) == 0) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "QKEY t=%ldms k=%016llX\n", t, (unsigned long long)key);
            fclose(f);
          }
        }
        // M3.207b: the finder's registers are PROVEN (this hook's r6 caught
        // the right keys); r4/r5 here are the searched array's bounds. When
        // the doomed hash queries, log everything undecimated.
        if (uint32_t(key >> 32) == 0x202A49F6u) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "DOOM t=%ldms k=%016llX r3=%08X r4=%08X r5=%08X r7=%08X\n",
                    t, (unsigned long long)key, ctx.r3.u32, ctx.r4.u32,
                    ctx.r5.u32, ctx.r7.u32);
            fclose(f);
          }
        }
      }
    }
  }
  {
    // M3.207c: result-struct forensics — capture what the finder RETURNS
    // for the doomed key vs (sampled) passing keys; the forged-hit write
    // must reproduce a passing struct exactly.
    static const bool on3 = getenv("RESTUFF_MERGELOG") != nullptr;
    const uint32_t rp = ctx.r3.u32, kp3 = ctx.r6.u32;
    const uint32_t flr = uint32_t(ctx.lr);
    const uint32_t fa4 = ctx.r4.u32, fa5 = ctx.r5.u32, fa7 = ctx.r7.u32,
                   fr31 = ctx.r31.u32;
    uint64_t k3 = 0;
    if (on3 && kp3 >= 0x10000 && kp3 < 0xC0000000u - 8) {
      memcpy(&k3, base + kp3, 8);
      k3 = __builtin_bswap64(k3);
    }
    __imp__sub_829CF620(ctx, base);
    // M3.312j FMISS: TRUE miss census at the finder itself. The contains-hook
    // census proved the persistent 5E442194 walker misses never pass through
    // sub_829D0708 (its only light-key misses are the registrar's dedup
    // lookups, lr=829EC650) — some caller reaches the finder directly. Log
    // genuine misses (returned element absent/≠key) for 04000009 keys with
    // the pre-call lr + arg regs so the real site and its record register
    // become visible.
    if (on3 && uint32_t(k3) == 0x04000009u) {
      uint32_t elem = 0; uint64_t ek = ~0ull;
      if (rp >= 0x10000 && rp < 0xC0000000u - 8) {
        uint32_t w; memcpy(&w, base + rp + 4, 4);
        elem = __builtin_bswap32(w);
      }
      if (elem >= 0x10000 && elem < 0xC0000000u - 8) {
        uint64_t v; memcpy(&v, base + elem, 8);
        ek = __builtin_bswap64(v);
      }
      if (ek != k3) {
        static std::atomic<uint32_t> s_fm{0};
        const uint32_t fm = s_fm.fetch_add(1, std::memory_order_relaxed);
        if (fm < 200 || (fm & 255) == 0) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "FMISS#%u t=%ldms lr=%08X k=%016llX r4=%08X r5=%08X "
                       "r7=%08X r31=%08X el=%08X\n",
                    fm, long(ibwatch::SinceArmMs()), flr,
                    (unsigned long long)k3, fa4, fa5, fa7, fr31, elem);
            fclose(f);
          }
        }
      }
    }
    // M3.208 (RESTUFF_WEDGE_FIX2=1): THE FORGE. The doomed key's search
    // misses every registry by construction; patch the returned iterator's
    // found-pointer to the query key's own address — key >= itself passes
    // sub_829D0708's version compare, the end-check can't fire (stack VA !=
    // array end), and callers consume only the boolean. The door sector
    // then attaches exactly as if its registration existed — the outcome
    // hardware timing always produced.
    // M3.305 (RESTUFF_FORGE2=1, DIAGNOSTIC): the deep-L1 triangle. KEYFIX2
    // engaged (20 clears on the triangle-box records) and the triangle stayed —
    // byte-23 exclusion REFUTED for this site. Remaining mechanism from the
    // original hunt: the DOOMED KEY → sector-never-attaches path (this forge,
    // shipped for 202A49F6). F136BA40_04000009 misses every registry 203/203 at
    // the spot, is never KREG-inserted, never KERASE'd. Forge its hit exactly
    // like M3.208 and let the user eyeball whether the triangle fills.
    {
      static const bool forge2 = getenv("RESTUFF_FORGE2") != nullptr;
      if (forge2 && k3 == 0xF136BA4004000009ull &&
          rp >= 0x10000 && rp < 0xC0000000u - 8) {
        const uint32_t lo_be2 = __builtin_bswap32(kp3);
        memcpy(base + rp + 4, &lo_be2, 4);
        static std::atomic<uint32_t> s_f2{0};
        const uint32_t fk2 = s_f2.fetch_add(1, std::memory_order_relaxed);
        if (fk2 < 10 || fk2 % 1000 == 0) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "FORGE2#%u t=%ldms k=%016llX\n", fk2,
                    long(ibwatch::SinceArmMs()), (unsigned long long)k3);
            fclose(f);
          }
        }
      }
    }
    {
      static const bool fix2 = getenv("RESTUFF_WEDGE_FIX2") != nullptr;
      if (fix2 && uint32_t(k3 >> 32) == 0x202A49F6u &&
          rp >= 0x10000 && rp < 0xC0000000u - 8) {
        const uint32_t lo_be = __builtin_bswap32(kp3);
        memcpy(base + rp + 4, &lo_be, 4);
        static std::atomic<uint32_t> s_f{0};
        const uint32_t fk = s_f.fetch_add(1, std::memory_order_relaxed);
        if (fk < 50) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "FORGE#%u t=%ldms k=%016llX\n", fk,
                    long(ibwatch::SinceArmMs()), (unsigned long long)k3);
            fclose(f);
          }
        }
      }
    }
    // M3.301: DOOM3 — generalize the result capture to ANY 04-signature key.
    // The Aug-22 stand-at-the-triangle log showed two 04-keys (F136BA40,
    // 041F15FD) re-minted every other frame that DOOM2's hardcoded hashes
    // never covered; whether their registry search hits or misses is the
    // discriminator between "stale light survives via bit5" and "doomed
    // unregistered key" for the deep-level-1 triangle.
    if (on3 && (uint32_t(k3) >> 24) == 0x04u) {
      static std::atomic<uint32_t> s_d3{0};
      const uint32_t d3 = s_d3.fetch_add(1, std::memory_order_relaxed);
      if (d3 < 2000 || (d3 & 63) == 0) {
        uint64_t res = 0;
        if (rp >= 0x10000 && rp < 0xC0000000u - 8) {
          memcpy(&res, base + rp, 8);
          res = __builtin_bswap64(res);
        }
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "DOOM3 t=%ldms k=%016llX res=%016llX r4=%08X r5=%08X\n",
                  long(ibwatch::SinceArmMs()), (unsigned long long)k3,
                  (unsigned long long)res, ctx.r4.u32, ctx.r5.u32);
          fclose(f);
        }
      }
    }
    // M3.312f REGDUMP: on a light-key (04000009) miss, one-shot per (array,
    // key): dump the ENTIRE searched array [r4,r5) as u64s plus the returned
    // element. Distinguishes "key truly absent" (registrar desync — wrote a
    // different array) from "key present but lower_bound misses" (array not
    // sorted under the search's comparator). Run-5 finding that forced this:
    // our F136BA40 insert grew the very array the walker probes, yet the
    // walker kept missing it 147x while 7AE156CE/92549C8D inserts into the
    // SAME array healed instantly.
    if (on3 && uint32_t(k3) == 0x04000009u) {
      const uint32_t ab = ctx.r4.u32, ae = ctx.r5.u32;
      uint32_t elem = 0;
      if (rp >= 0x10000 && rp < 0xC0000000u - 8) {
        uint32_t w; std::memcpy(&w, base + rp + 4, 4);
        elem = __builtin_bswap32(w);
      }
      bool hit = false;
      if (elem >= ab && elem + 8 <= ae) {
        uint64_t ek; std::memcpy(&ek, base + elem, 8);
        hit = (__builtin_bswap64(ek) == k3);
      }
      if (!hit && ab >= 0x10000 && ae > ab && ae - ab <= 8 * 64 &&
          ae < 0xC0000000u) {
        static std::mutex s_rdmu;
        static std::set<std::pair<uint32_t, uint64_t>> s_rdseen;
        bool fresh = false;
        { std::lock_guard<std::mutex> g(s_rdmu);
          if (s_rdseen.size() < 512)
            fresh = s_rdseen.insert({ab, k3}).second; }
        if (fresh) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "REGDUMP t=%ldms k=%016llX a=[%08X,%08X) el=%08X n=%u:",
                    long(ibwatch::SinceArmMs()), (unsigned long long)k3,
                    ab, ae, elem, (ae - ab) / 8);
            for (uint32_t p = ab; p + 8 <= ae; p += 8) {
              uint64_t v; std::memcpy(&v, base + p, 8);
              fprintf(f, " %016llX",
                      (unsigned long long)__builtin_bswap64(v));
            }
            fprintf(f, "\n");
            fclose(f);
          }
        }
      }
    }
    if (on3 && (uint32_t(k3 >> 32) == 0x202A49F6u ||
                uint32_t(k3 >> 32) == 0x5E442194u)) {
      uint64_t res = 0;
      if (rp >= 0x10000 && rp < 0xC0000000u - 8) {
        memcpy(&res, base + rp, 8);
        res = __builtin_bswap64(res);
      }
      if (FILE* f = fopen(restuff_logpath(), "a")) {
        fprintf(f, "DOOM2 t=%ldms k=%016llX res=%016llX r4=%08X r5=%08X\n",
                long(ibwatch::SinceArmMs()), (unsigned long long)k3,
                (unsigned long long)res, ctx.r4.u32, ctx.r5.u32);
        fclose(f);
      }
    }
  }
}
REX_EXTERN(__imp__sub_829D0708);
REX_EXTERN(__imp__sub_829EC5D0);
REX_HOOK_RAW(sub_829D0708) {
  // M3.199 (RESTUFF_MERGELOG=1): the residency test itself — a binary search
  // over a sorted qword array + a >= compare; the per-view mode words flip
  // only when it returns 1. Log (key, verdict) per call in the window: on
  // wedge boots the door sectors' lookups fail, and the key values show
  // whether the entry is ABSENT (insert never ran) or the sequence compare
  // failed (stamp race). r5 = key ptr per PPC convention.
  // M3.207: capture the DOOMED KEY's query context. When any caller's key
  // deref is 202A49F6xxxx, log ALL regs + LR undecimated — the array bounds
  // it searched are the forge-fix's insertion target.
  {
    static const bool on2 = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on2) {
      const uint32_t kp2 = ctx.r6.u32;
      if (kp2 >= 0x10000 && kp2 < 0xC0000000u - 8) {
        uint32_t hi2;
        memcpy(&hi2, base + kp2, 4);
        if (__builtin_bswap32(hi2) == 0x202A49F6u) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "DOOM t=%ldms lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n",
                    long(ibwatch::SinceArmMs()), uint32_t(ctx.lr), ctx.r3.u32,
                    ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
            fclose(f);
          }
        }
      }
    }
  }
  // M3.199c: register patterns showed MULTIPLE callsites with different arg
  // shapes (r6 = pointer at one site, small ints at others) — log raw
  // r3..r8 + LR, no derefs (also removes this hook's unmapped-read crash
  // exposure), and segregate by caller offline before interpreting.
  const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32,
                 a6 = ctx.r6.u32, a7 = ctx.r7.u32, a8 = ctx.r8.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  const uint32_t mf_r31 = ctx.r31.u32;
  __imp__sub_829D0708(ctx, base);
  // M3.312i MISSFIX (RESTUFF_BIT5FIX=1): the delivery that closes every gap.
  // Ground truth (IDA container-v2 recon + BSCAN2 + REGDUMP): rec+0x84 is a
  // sorted std::vector<u64> of 04000009 light keys; healthy records carry ALL
  // session light keys there; doomed blob records carry an authored SUBSET and
  // are often reachable only through this reconciler query (never via a
  // walk-table our sub_829C7970 hook can scan) — every earlier fix lost either
  // a visibility or a timing race to that. Here the miss itself is the
  // trigger: reconciler site (lr=829C7A8C), key type 04000009, result=0.
  // The walker keeps the current record in nonvolatile r31; validate it
  // before trusting: r31 is the record iff its +0x84 holder's begin/end equal
  // the probed bounds [r3,r4). Then complete the registry with the engine's
  // own registrar (dedups, keeps sortedness) and bump the +0x98 change
  // counter exactly like the engine's apply path. Next query (walks repeat
  // 30-60/s) finds the key -> light kept -> merge coalesces. lr gate also
  // prevents any recursion via the registrar's internal contains.
  {
    static const bool mfx = getenv("RESTUFF_BIT5FIX") != nullptr;
    // MFPROBE: calibration one-shots — dump raw regs at the reconciler site so
    // a wrong register-shape assumption is visible instead of silently gating
    // MISSFIX off (v1 assumed result in r3 + key ptr in r6 and never fired).
    if (mfx && ctx.r3.u32 == 0) {
      uint64_t k5 = 0, k6 = 0;
      if (a5 >= 0x10000 && a5 < 0xC0000000u - 8) {
        std::memcpy(&k5, base + a5, 8); k5 = __builtin_bswap64(k5);
      }
      if (a6 >= 0x10000 && a6 < 0xC0000000u - 8) {
        std::memcpy(&k6, base + a6, 8); k6 = __builtin_bswap64(k6);
      }
      static std::atomic<uint32_t> s_pn{0};
      if ((uint32_t(k5) == 0x04000009u || uint32_t(k6) == 0x04000009u) &&
          s_pn.fetch_add(1, std::memory_order_relaxed) < 200) {
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "MFPROBE lr=%08X ret=%08X a3=%08X a4=%08X a5=%08X a6=%08X "
                     "a7=%08X a8=%08X r31=%08X *a5=%016llX *a6=%016llX\n",
                  lr, ctx.r3.u32, a3, a4, a5, a6, a7, a8, mf_r31,
                  (unsigned long long)k5, (unsigned long long)k6);
          fclose(f);
        }
      }
    }
    // MFPROBE-calibrated shape (Aug 24 drive): ret bool in r3; a3/a4 = array
    // bounds; a5 = KEY POINTER (stack); a6 = per-walk small counter; r31 = the
    // walker's current record. Both hot query sites accepted — reconciler
    // (829C7A8C, gates the mode words) AND the per-frame render-path caller
    // (829C78D4): the registry is shared state, so completing it at either
    // miss fixes both, and the r31 bounds-validation refuses any call shape
    // where the record identity doesn't check out.
    if (mfx && (lr == 0x829C7A8Cu || lr == 0x829C78D4u) && ctx.r3.u32 == 0 &&
        a5 >= 0x10000 && a5 < 0xC0000000u - 8) {
      uint64_t mk; std::memcpy(&mk, base + a5, 8);
      mk = __builtin_bswap64(mk);
      if (uint32_t(mk) == 0x04000009u) {
        auto mrd32 = [&](uint32_t a) -> uint32_t {
          if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
          uint32_t v; std::memcpy(&v, base + a, 4);
          return __builtin_bswap32(v);
        };
        uint32_t rec = 0;
        for (int cand = 0; cand < 2 && !rec; cand++) {
          const uint32_t c = cand ? mrd32(mf_r31 + 4) : mf_r31;
          if (c < 0x10000u || c >= 0xC0000000u - 160u) continue;
          const uint32_t h = mrd32(c + 0x84);
          if (h && mrd32(h + 12) == a3 && mrd32(h + 16) == a4) rec = c;
        }
        if (rec) {
          static std::atomic<uint32_t> s_mf{0};
          const uint32_t mn = s_mf.fetch_add(1, std::memory_order_relaxed);
          if (mn < 4096) {
            PPCContext c2 = ctx;
            c2.r3.u32 = rec + 0x84;
            c2.r4.u64 = mk;
            __imp__sub_829EC5D0(c2, base);
            uint16_t ver = 0;
            std::memcpy(&ver, base + rec + 0x98, 2);
            ver = __builtin_bswap16(uint16_t(__builtin_bswap16(ver) + 1));
            std::memcpy(base + rec + 0x98, &ver, 2);
            if (mn < 64 || (mn & 63) == 0) {
              if (FILE* f = fopen(restuff_logpath(), "a")) {
                fprintf(f, "MISSFIX#%u t=%ldms rec=%08X k=%016llX "
                           "a=[%08X,%08X)\n",
                        mn, long(ibwatch::SinceArmMs()), rec,
                        (unsigned long long)mk, a3, a4);
                fclose(f);
              }
            }
          }
        } else {
          static std::atomic<uint32_t> s_mfu{0};
          if (s_mfu.fetch_add(1, std::memory_order_relaxed) < 16) {
            if (FILE* f = fopen(restuff_logpath(), "a")) {
              fprintf(f, "MISSFIX-noid t=%ldms r31=%08X k=%016llX "
                         "a=[%08X,%08X)\n",
                      long(ibwatch::SinceArmMs()), mf_r31,
                      (unsigned long long)mk, a3, a4);
              fclose(f);
            }
          }
        }
      }
    }
  }
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      const long t = long(ibwatch::SinceArmMs());
      if (t >= 55000) {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
        // Reconciler callsite only (lr=829C7A8C, ~736/run): these calls
        // gate the mode-word writes. r6 = &key there; deref it. The 74k/run
        // render-path calls (lr=829C78D4) are decimated context only.
        const bool recon = lr == 0x829C7A8Cu;
        if (recon && a3 >= 0x10000 && a4 > a3 && a4 - a3 < 65 * 8 && a4 < 0xC0000000u) {
          const uint64_t rec64 = (uint64_t(a3) << 32) | a4;
          for (int gi = 0; gi < 8; ++gi) {
            uint64_t cur = mergegate::g_arrs[gi].load(std::memory_order_relaxed);
            if (cur == rec64) break;
            if ((cur >> 32) == a3) { mergegate::g_arrs[gi].store(rec64, std::memory_order_relaxed); break; }
            if (cur == 0) { if (mergegate::g_arrs[gi].compare_exchange_strong(cur, rec64)) break; }
          }
        }
        // M3.204: dump the residency ARRAY the finder searches — the fix-A
        // test proved the door sector's registration INSERT never happens on
        // wedge boots; the array contents per boot are the proof and the
        // pointer to the inserter. a3:a4 = (begin,end) at the reconciler
        // callsite; dump qwords once per second, capped 48 entries.
        if (recon) {
          static std::atomic<int64_t> s_dl{0};
          int64_t dl = s_dl.load(std::memory_order_relaxed);
          if (t - dl >= 1000 && s_dl.compare_exchange_strong(dl, t) &&
              a3 >= 0x10000 && a4 > a3 && a4 < 0xC0000000u && a4 - a3 < 48 * 8 + 1) {
            if (FILE* f = fopen(restuff_logpath(), "a")) {
              fprintf(f, "RARR t=%ldms b=%08X n=%u q=", t, a3, (a4 - a3) / 8);
              for (uint32_t e = a3; e + 8 <= a4; e += 8) {
                uint64_t q; memcpy(&q, base + e, 8);
                fprintf(f, "%016llX ", (unsigned long long)__builtin_bswap64(q));
              }
              fprintf(f, "\n");
              fclose(f);
            }
          }
        }
        if (recon || n < 4000 || (n & 63) == 0) {
          uint64_t key = 0;
          if (recon && a6 >= 0x10000 && a6 < 0xC0000000u - 8) {
            memcpy(&key, base + a6, 8);
            key = __builtin_bswap64(key);
          }
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "RES#%u t=%ldms lr=%08X a=%08X,%08X,%08X,%08X,%08X,%08X k=%016llX r=%u\n",
                    n, t, lr, a3, a4, a5, a6, a7, a8,
                    (unsigned long long)key, uint8_t(ctx.r3.u32));
            fclose(f);
          }
        }
      }
    }
  }
}
// M3.249 (RESTUFF_SECTORWATCH=1): WHO keeps re-adding the stuck sector?
//
// The list-end watchpoint fired 25322 times and symbolised to sub_829D2C48,
// which is a plain container push_back:
//     *(r11+0)=*(r6+0); *(r11+4)=*(r6+4); r11+=8; *(r3+16)=r11;
// i.e. the sector list is REBUILT CONTINUOUSLY, not a persistent container with
// a missing release. So "the sector is never released" is really "the per-frame
// selection keeps re-adding it", and the interesting code is the CALLER.
// The IP alone cannot name it -- push_back has many callers across the image --
// so log the caller's LR, but only for pushes into the sector container we
// armed on, and only a few times.
static std::atomic<uint32_t> g_sector_desc{0};
REX_EXTERN(__imp__sub_829D2C48);
REX_HOOK_RAW(sub_829D2C48) {
  const uint32_t self = ctx.r3.u32, lr0 = uint32_t(ctx.lr);
  const uint32_t want = g_sector_desc.load(std::memory_order_acquire);
  if (want && self == want) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    // M3.249b: read the entry AFTER the push, not before. The first version
    // logged *(r6) at hook entry and got all zeros -- the source is not
    // populated yet at that point. The pushed entry is 8 bytes ending at the new
    // end pointer, and its object pointer is at +4 (same layout sub_829C7970
    // walks: entries are 8B in [*(desc+12), *(desc+16)) with obj at +4).
    // Logging the OBJECT's class and flags is the point: it shows exactly when a
    // class-2 sector with bit 3 set enters the list, which is the moment that
    // decides the wedge.
    __imp__sub_829D2C48(ctx, base);
    if (k < 24) {
      auto rd = [&](uint32_t a) {
        uint32_t v = 0;
        if (a >= 0x10000 && a < 0xC0000000u - 4) {
          std::memcpy(&v, base + a, 4);
          v = __builtin_bswap32(v);
        }
        return v;
      };
      const uint32_t end = rd(self + 16);
      const uint32_t obj = end >= 8 ? rd(end - 8 + 4) : 0;
      const uint32_t cls = obj ? rd(obj + 120) : 0;
      const uint32_t fl = obj ? rd(obj + 124) : 0;
      if (FILE* f = fopen(restuff_logpath(), "a"))
        fprintf(f,
                "SECTORPUSH#%u t=%ldms desc=%08X lr=%08X obj=%08X class=%u "
                "flags=%08X bit3=%u\n",
                k, long(ibwatch::SinceArmMs()), self, lr0, obj, cls, fl,
                (fl >> 3) & 1),
            fclose(f);
    }
    return;
  }
  __imp__sub_829D2C48(ctx, base);
}

REX_EXTERN(__imp__sub_829C7970);
REX_EXTERN(__imp__sub_829EC5D0);
// M3.312e: session-wide light-walk key set. The v3 A/B lost a race: the doomed
// record's ONLY hook-visible traversal (t=75115848) happened ~5s before the
// triangle key first reached a hooked walk entry (t=75120953) — but the engine
// registrar had seen the key 16s EARLIER (KREG#14 t=75099545, the stream-in
// burst). So harvest 04000009-type keys at the registrar too, not just at walk
// entry.
static std::mutex s_wkmu;
static std::set<uint64_t> s_wkeys;
static void restuff_harvest_wkey(uint64_t k) {
  if (uint32_t(k) != 0x04000009u) return;
  std::lock_guard<std::mutex> g(s_wkmu);
  if (s_wkeys.size() < 64) s_wkeys.insert(k);
}
REX_HOOK_RAW(sub_829C7970) {
  // M3.306 (RESTUFF_NO_VEROPT=1, DIAGNOSTIC): defeat the version-skip gate.
  // Recon (recon_attach_pacer.md): pacer sub_829CCB48 sets the per-view dirty
  // byte only when the scene-mutation version (view+0x98) or camera-transform
  // version (cam+0xA6) changed; the whole check is bypassed (= always dirty)
  // when OPTIM_TOGGLE_LIGHT_MASK_VERSION_OPTIM's dword_83341E9C is off. A
  // static camera with no scene mutations starves the attach/rebuild chain —
  // the measured 55->36 record plateau. Zeroing the dword per pass tests
  // whether the stalled merge resumes (plateau breaks) with the gate open.
  {
    static const bool nvo = getenv("RESTUFF_NO_VEROPT") != nullptr;
    if (nvo) {
      uint32_t cur;
      std::memcpy(&cur, base + 0x83341E9Cu, 4);
      if (cur != 0) {
        const uint32_t zero = 0;
        std::memcpy(base + 0x83341E9Cu, &zero, 4);
        static std::atomic<uint32_t> s_nv{0};
        if (s_nv.fetch_add(1, std::memory_order_relaxed) < 5) {
          if (FILE* f = fopen(restuff_logpath(), "a"))
            fprintf(f, "VEROPT t=%ldms was=%08X -> 0\n",
                    long(ibwatch::SinceArmMs()), __builtin_bswap32(cur)),
                fclose(f);
        }
      }
    }
  }
  // M3.250 (RESTUFF_STALEFIX=1, OPT-IN): fix the wedge NEAR ITS ROOT instead of
  // at the merge keys.
  //
  // What the evidence says (M3.249b): wedge boots carry extra class-2 sectors
  // that live in a DIFFERENT ALLOCATION REGION from the current set --
  //     healthy : B8895AC0..B8896E40  class1 0x1F x4, class2 0x11 x2
  //     stale   : B82AC0E0 (0x1D), B82C1920 (0x09)   ~6MB away
  // i.e. leftovers from an earlier streaming epoch that were never
  // deregistered. They matter only because BIT 3 is set: sub_829C7970 prunes a
  // light bit only when bit5==0 AND bit3==0, so a bit-3 sector keeps its light
  // forever, which keeps the mask non-zero, which makes sub_82AD3460 compose
  // forever, which re-mints merge keys with byte 23 = 0x04 and blocks
  // coalescing.
  //
  // Healthy class-2 sectors are always 0x11 (bit 3 CLEAR); only the stale ones
  // set it. Class-1 sectors legitimately set bit 3 (0x1F) and are left alone.
  // So: clear bit 3 on CLASS-2 sectors that have it, immediately before the
  // guest's own prune runs, and the game prunes them by its own rules.
  //
  // This is strictly more targeted than KEYFIX (M3.229), which clears the
  // downstream merge-key byte. OPT-IN until measured: if a class-2 sector ever
  // legitimately carries bit 3, this would prune a light that should stay.
  {
    static const bool sfix = getenv("RESTUFF_NO_STALEFIX") == nullptr;
    if (sfix) {
      auto rd32 = [&](uint32_t a) -> uint32_t {
        if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
        uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
      };
      const uint32_t desc = rd32(ctx.r3.u32 + 532);
      const uint32_t lo = rd32(desc + 12), hi = rd32(desc + 16);
      // M3.250b: the first build logged NOTHING even though SFLAG (same hook,
      // same desc, a few lines below) showed a class-2 0x1D sector present. So
      // dump what this loop actually sees -- inspection has already failed once.
      {
        // Gate on the arm time: the FIRST calls happen at menu time, where the
        // sector list says nothing about the t=99s+ state SFLAG reported.
        static std::atomic<uint32_t> dbg{0};
        static std::atomic<long> dlast{0};
        const long dnow = long(ibwatch::SinceArmMs());
        const uint32_t d = dbg.load(std::memory_order_relaxed);
        const bool want = dnow >= 95000 && d < 6 && dnow - dlast.load(std::memory_order_relaxed) >= 1000;
        if (want && (dlast.store(dnow, std::memory_order_relaxed), dbg.fetch_add(1, std::memory_order_relaxed), true)) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            fprintf(f, "STALEDBG#%u r3=%08X desc=%08X lo=%08X hi=%08X span=%u",
                    d, ctx.r3.u32, desc, lo, hi, (hi > lo) ? (hi - lo) : 0);
            if (desc && hi > lo && hi - lo < 8 * 512)
              for (uint32_t e = lo; e + 8 <= hi; e += 8) {
                const uint32_t o = rd32(e + 4);
                fprintf(f, " [%08X c=%u f=%08X]", o, rd32(o + 120), rd32(o + 124));
              }
            fprintf(f, "\n");
            fclose(f);
          }
        }
      }
      if (desc && hi > lo && hi - lo < 8 * 512) {
        static std::atomic<uint32_t> nfix{0};
        // M3.252: LIVENESS gate, not a class heuristic.
        //
        // bit3 at +124 is a BINDING flag owned by exactly one accessor,
        // sub_82982978 (the only ori|8 / rlwinm&~8 pair that stores to +0x7C in
        // the whole image): it SETS bit3 when the state applier runs with tag
        // 4BAB0AB4/B16601B3 and CLEARS it for any other tag. Nothing clears it
        // when a sector is RETIRED, so a sector left over from a previous
        // streaming epoch keeps bit3 forever; sub_829C7970 refuses to prune a
        // light whose sector still has bit3 (it requires bit5==0 && bit3==0),
        // the light survives in the 64-bit mask, and the merge keys stop
        // coalescing -> 285 draws instead of 79 -> the water wedge.
        //
        // ⛔ REFUTED, DO NOT REBUILD IT: "release any sector whose +152 counter
        // has been frozen >= 3s". +152 IS a per-pass touch counter, but the
        // inference from it is wrong -- during a streaming hitch EVERY counter
        // freezes, so live sectors read as retired. The build that shipped it
        // cleared four HEALTHY class-1 (0x1F -> 0x17) sectors at t=69s and
        // pruned lights the scene was still using: live11 came back with the
        // ground, rocks and terrain BLACK. Note the harness scored that frame
        // draws=81 wedge=0.0% -- a PASS on every metric. Only looking at the
        // image caught it. A "recent global activity" guard does not save the
        // idea either: during streaming other sectors ARE moving, so the guard
        // is open at exactly the moment the false positives happen.
        //
        // What ships instead is the invariant the data actually supports: a
        // healthy class-2 sector is 0x11 and NEVER carries this binding (only
        // class-1 legitimately does, as 0x1F/0x17). So class-2 + bit3 is a state
        // no live sector takes, and releasing it is safe.
        for (uint32_t e = lo; e + 8 <= hi; e += 8) {
          const uint32_t obj = rd32(e + 4);
          // rd32 returns 0 for out-of-range reads, but the WRITE below indexes
          // base directly -- bound it explicitly so a garbage ptr can't scribble.
          if (obj < 0x10000u || obj >= 0xC0000000u - 160u) continue;
          if (rd32(obj + 120) != 2) continue;             // class-1 keeps its binding
          const uint32_t fl = rd32(obj + 124);
          if (!((fl >> 3) & 1)) continue;                 // healthy class-2 is 0x11
          const uint32_t cleared = fl & ~8u;
          const uint32_t be = __builtin_bswap32(cleared);
          std::memcpy(base + obj + 124, &be, 4);
          const uint32_t k = nfix.fetch_add(1, std::memory_order_relaxed);
          // M3.301: 8 hid everything after level-load; late engagements are the
          // interesting ones for the deeper-in-level wedge.
          if (k < 64) {
            if (FILE* f = fopen(restuff_logpath(), "a"))
              fprintf(f, "STALEFIX#%u t=%ldms obj=%08X class=%u flags %08X->%08X (stale binding)\n",
                      k, long(ibwatch::SinceArmMs()), obj, rd32(obj + 120), fl,
                      cleared),
                  fclose(f);
          }
        }
      }
    }
  }
  // M3.312 (RESTUFF_BIT5FIX=1, OPT-IN A/B): complete the registration the
  // stream-in burst skipped — the deep-L1 triangle root fix candidate.
  //
  // Aug-23 heal-run evidence (wedge_l1/userspot_heal_mergelog.txt): the
  // triangle = two class-2 flags-0x35 sectors (bit5 SET, bit3 clear) whose
  // +0x84 pass-A registries never received the light walk key 5E44219404000009
  // in the 195047648ms burst, while every sibling sector in the SAME burst got
  // it. IDA recon (agent, Aug 23 night): the registrar has NO key filter — the
  // per-sector key set is the union of authored per-LIP-item lists (item+0xD0/
  // +0xD8), and flag bit5 comes from a DIFFERENT field of the item (tag at
  // +0xC0), so an authored bit5-tag item with a short pass-A list yields
  // "bit5 set, registry lacks the walk key" with no runtime bug — the engine
  // then queries the key forever, misses, refuses to prune the light, and the
  // merge stays fragmented until the sector releases (~19s later, task-map
  // latency). Healthy bit5 class-2 sectors always CONTAIN their light keys
  // (BACC9D70's +0x84 is in the key's KREG handle list), so absence is
  // precisely the anomaly.
  //
  // Fix: for each class-2 / bit5-set / bit3-clear sector in the walk table
  // whose +0x84 registry lacks the CURRENTLY QUERIED light-type walk key
  // (low dword 0x04000009, key in r5 per the 0x8297E488 call site), insert
  // that key via the engine's own registrar sub_829EC5D0(sector+0x84, key) —
  // identical to what registration does for healthy sectors, dedup included.
  // NOT a flag clear (bit5 semantics untouched, no light wrongly pruned) and
  // NOT a forge (the registry truly gains the key, so every later query
  // agrees). FORGE2 never tested this triangle: it was hardcoded to the hut's
  // F136BA40 key, not 5E442194.
  {
    static const bool b5f = getenv("RESTUFF_BIT5FIX") != nullptr;
    if (b5f) {
      auto rd32 = [&](uint32_t a) -> uint32_t {
        if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
        uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
      };
      const uint64_t wkey = ctx.r5.u64;
      if (uint32_t(wkey) == 0x04000009u) {
        // M3.312d: the Aug-24 second A/B (userspot_bit5fix2_mergelog.txt)
        // proved the doomed record BACEFCC0 is only ever hook-visible under
        // OTHER walk keys (its 5E442194-keyed queries reach the registry via a
        // path that doesn't traverse a ctx+532 table containing it, 588 misses
        // to session end). So collect every light-type walk key seen and
        // install ALL of them into any bit5 record that lacks one — the walks
        // that DO traverse the record then carry the missing key in.
        // Registered == the healthy-sibling state (light kept, runs coalesce).
        // Keys come from BOTH walk entries (here) and the engine registrar
        // (restuff_harvest_wkey in the sub_829EC5D0 hook — M3.312e).
        std::vector<uint64_t> wkeys;
        { std::lock_guard<std::mutex> g(s_wkmu);
          if (s_wkeys.size() < 64) s_wkeys.insert(wkey);
          wkeys.assign(s_wkeys.begin(), s_wkeys.end()); }
        const uint32_t desc = rd32(ctx.r3.u32 + 532);
        const uint32_t lo = rd32(desc + 12), hi = rd32(desc + 16);
        if (desc && hi > lo && hi - lo < 8 * 512) {
          for (uint32_t e = lo; e + 8 <= hi; e += 8) {
            const uint32_t obj = rd32(e + 4);
            if (obj < 0x10000u || obj >= 0xC0000000u - 160u) continue;
            const uint32_t cls = rd32(obj + 120);
            const uint32_t fl = rd32(obj + 124);
            const uint32_t cont = rd32(obj + 0x84);
            // M3.312c BIT5SCAN: one-shot per record — the Aug-24 A/B left one
            // persistent doomed container (A77CA9E0) that the old class-2 +
            // bit3-clear predicate never matched; recon says classes run 0..4
            // and multiple view ctxs carry their own tables, so log EVERY
            // record seen under a light-key walk and let the log name the
            // escapee's class/flags directly.
            {
              static std::mutex s_scmu;
              static std::set<uint32_t> s_seen;
              static std::atomic<uint32_t> s_scn{0};
              bool fresh = false;
              { std::lock_guard<std::mutex> g(s_scmu);
                fresh = s_seen.insert(obj).second; }
              if (fresh && s_scn.fetch_add(1, std::memory_order_relaxed) < 256) {
                if (FILE* f = fopen(restuff_logpath(), "a")) {
                  const uint32_t kb2 = rd32(cont + 12), ke2 = rd32(cont + 16);
                  fprintf(f, "BIT5SCAN r3=%08X rec=%08X c=%u fl=%08X cont=%08X "
                             "keys=[%08X,%08X)n=%u wk=%016llX\n",
                          ctx.r3.u32, obj, cls, fl, cont, kb2, ke2,
                          (ke2 > kb2) ? (ke2 - kb2) / 8 : 0,
                          (unsigned long long)wkey);
                  // M3.312g BSCAN2: dump BOTH registries' key lists — tests
                  // the "+0x84 must contain every 04-key of +0x80" invariant
                  // (REGDUMP showed the triangle key living in the sibling
                  // registry of the records whose pass-A probes miss it).
                  for (int ri = 0; ri < 2; ri++) {
                    const uint32_t c2p = rd32(obj + 0x80 + 4 * ri);
                    if (c2p < 0x10000u || c2p >= 0xC0000000u - 32u) continue;
                    const uint32_t b = rd32(c2p + 12), e2 = rd32(c2p + 16);
                    if (e2 <= b || e2 - b > 8 * 64) continue;
                    fprintf(f, "BSCAN2 rec=%08X reg%s n=%u:", obj,
                            ri ? "84" : "80", (e2 - b) / 8);
                    for (uint32_t p = b; p + 8 <= e2; p += 8) {
                      uint64_t v; std::memcpy(&v, base + p, 8);
                      fprintf(f, " %016llX",
                              (unsigned long long)__builtin_bswap64(v));
                    }
                    fprintf(f, "\n");
                  }
                  fclose(f);
                }
              }
            }
            // M3.312b (recon-corrected): the walker reads *(rec+0x84) LIVE (no
            // snapshot), so predicate = bit5 set + key missing; class and bit3
            // are logged, not filtered (the gate 'clear ⇔ contains==bit3' means
            // insertion enables the prune for bit3=1 records and matched the
            // observed healings for bit3=0 ones). After the registrar insert,
            // bump the record's u16 change counter at +0x98 exactly like the
            // engine's own insert path (sub_829990B8 does ++*(u16*)(rec+0x98)
            // per insert) so the per-frame cache compare (sub_829CCB48) marks
            // the view dirty and the passes re-run.
            if (!(fl & 0x20u)) continue;
            if (cont < 0x10000u || cont >= 0xC0000000u - 32u) continue;
            for (uint64_t ik : wkeys) {
              const uint32_t kb = rd32(cont + 12), ke = rd32(cont + 16);
              if (ke < kb || ke - kb > 8 * 4096) break;
              bool has = false;
              for (uint32_t p = kb; p + 8 <= ke; p += 8) {
                uint64_t k; std::memcpy(&k, base + p, 8);
                if (__builtin_bswap64(k) == ik) { has = true; break; }
              }
              if (has) continue;
              static std::atomic<uint32_t> s_b5{0};
              const uint32_t n = s_b5.fetch_add(1, std::memory_order_relaxed);
              if (n >= 2048) break;  // runaway backstop
              PPCContext c2 = ctx;
              c2.r3.u32 = obj + 0x84;
              c2.r4.u64 = ik;
              __imp__sub_829EC5D0(c2, base);
              uint16_t ver = 0;
              std::memcpy(&ver, base + obj + 0x98, 2);
              ver = __builtin_bswap16(uint16_t(__builtin_bswap16(ver) + 1));
              std::memcpy(base + obj + 0x98, &ver, 2);
              if (FILE* f = fopen(restuff_logpath(), "a")) {
                fprintf(f, "BIT5FIX#%u t=%ldms rec=%08X c=%u fl=%08X k=%016llX "
                           "reg=[%08X,%08X) ver->%u%s\n",
                        n, long(ibwatch::SinceArmMs()), obj, cls, fl,
                        (unsigned long long)ik, kb, ke,
                        unsigned(__builtin_bswap16(ver)),
                        ik == wkey ? "" : " x");
                fclose(f);
              }
            }
          }
        }
      }
    }
  }
  // M3.197 (RESTUFF_MERGELOG=1): per-sector FLAG dump. mrg10 proved both
  // partition representations coexist all run in both families — draws=79
  // is a render-time SELECTION. The one undumped per-sector state is the
  // +124 flag word sub_829C7970 itself reconciles (bit3 vs live residency).
  // r3 = culling-manager ctx; sector array desc at +532: entries are 8B in
  // [*(+12), *(+16)), per-entry object ptr at +4; log +120 class and +124
  // flags for every sector, rate-limited to ~1Hz.
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      static std::atomic<int64_t> s_last{0};
      const int64_t now = ibwatch::SinceArmMs();
      int64_t last = s_last.load(std::memory_order_relaxed);
      if (now - last >= 1000 && s_last.compare_exchange_strong(last, now)) {
        auto rd32 = [&](uint32_t a) -> uint32_t {
          if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
          uint32_t v; memcpy(&v, base + a, 4);
          return __builtin_bswap32(v);
        };
        const uint32_t desc = rd32(ctx.r3.u32 + 532);
        const uint32_t lo = rd32(desc + 12), hi = rd32(desc + 16);
        if (desc && hi > lo && hi - lo < 8 * 128) {
          if (FILE* f = fopen(restuff_logpath(), "a")) {
            // M3.307: task-registry counters (recon F-series): singleton
            // 0x83340DE0, pending count +0x10, active count +0x1C. A static
            // NONZERO pending at the 36-record plateau = streaming-completion
            // stall confirmed.
            {
              uint32_t pend, act;
              memcpy(&pend, base + 0x83340DF0u, 4);
              memcpy(&act, base + 0x83340DFCu, 4);
              fprintf(f, "TASKQ t=%ldms pending=%u active=%u\n", long(now),
                      __builtin_bswap32(pend), __builtin_bswap32(act));
            }
            fprintf(f, "SFLAG t=%ldms n=%u", long(now), (hi - lo) / 8);
            for (uint32_t e = lo; e + 8 <= hi; e += 8) {
              const uint32_t obj = rd32(e + 4);
              // M3.301: include the sector object address — allocation-region
              // distance is a stale-vs-live discriminator (stale epoch leftovers
              // sit ~6MB from the current set) and class:flags alone can't
              // separate a stale class-1 (bit3 legitimate) from a live one.
              fprintf(f, " %08X@%u:%08X", obj, rd32(obj + 120), rd32(obj + 124));
            }
            fprintf(f, "\n");
            fclose(f);
            // M3.245 (RESTUFF_SECTORWATCH=1): NAME THE WRITER OF THE STUCK
            // SECTOR'S FLAGS.
            //
            // Root cause (Aug 11): wedge boots carry ONE EXTRA sector, class 2
            // with flags 0x09 -- bit3 set, bit4 CLEAR, a value no healthy sector
            // takes (normal class-2 is 0x11). It appears in EVERY boot at ~66s
            // and is normally released; on wedge boots it is still there at the
            // end (present in 188/192 samples vs 30/192 and 29/192 on the two
            // clean boots). Because bit3 is set, sub_829C7970 never prunes its
            // light bit, so light 2 survives in the mask, so the composer never
            // stops, so merge keys keep being re-minted with byte 23 = 0x04 and
            // the runs stop coalescing.
            //
            // Static search for the writer FAILED and should not be retried on
            // "+124": no store to +124/+127/+126 exists in this module, so the
            // owning code reaches the field through a different base pointer.
            // A hardware watchpoint sidesteps that entirely -- and unlike the
            // stack owners MASKLOG hit, a sector object is a stable guest
            // address, so keywatch works. Arm once, on the first sighting.
            {
              // M3.245b: arm only AFTER t=99s. Lifetime analysis of the archived
              // mk1 and door1 batches shows the sector is created at ~66.9s and
              // RELEASED CORRECTLY at ~95.3-96.3s in EVERY boot, wedge and clean
              // alike. It is then RE-CREATED at ~100.1-100.4s, and it is that
              // SECOND instance that sticks:
              //     WEDGE : re-created ~100.4s, never released (present to 260s)
              //     CLEAN : either never re-created, or re-created ~100.1s and
              //             released again immediately (mk14)
              // Arming on first sighting would therefore watch the instance that
              // is released correctly and miss the interesting one entirely.
              static const bool sw = getenv("RESTUFF_SECTORWATCH") != nullptr;
              static std::atomic<bool> armed{false};
              if (sw && now >= 99000 && !armed.load(std::memory_order_acquire)) {
                for (uint32_t e = lo; e + 8 <= hi; e += 8) {
                  const uint32_t obj = rd32(e + 4);
                  // M3.245d: match "class 2 with BIT 3 SET", not the literal
                  // 0x09. The rec1 batch showed the extra sector takes DIFFERENT
                  // flag values across boots -- 0x09 on one wedge boot, 0x1D on
                  // two others -- while healthy class-2 sectors are always 0x11.
                  // Both anomalous values have bit 3 set, which is exactly what
                  // makes sub_829C7970 skip pruning them (it prunes only when
                  // bit5==0 AND bit3==0). Matching the literal 0x09 was overfit
                  // to the first three batches and would never arm on a
                  // rec14/rec15-type boot.
                  if (rd32(obj + 120) != 2) continue;
                  const uint32_t fl = rd32(obj + 124);
                  if (!((fl >> 3) & 1)) continue;   // bit3 clear => prunable => healthy
                  bool ex = false;
                  if (armed.compare_exchange_strong(ex, true)) {
                    // M3.245c: watch the LIST END POINTER too, not just the
                    // flags. The first armed run got ZERO hits on obj+124 over
                    // ~160s, which cannot distinguish "the release never runs"
                    // from "the release unlinks the sector without touching its
                    // flags" -- and the second is entirely plausible. desc+16 is
                    // the container's end pointer (sub_829C7970 reads lo =
                    // *(desc+12), hi = *(desc+16), 8-byte entries with the
                    // object at +4), so every push/pop writes it and a clean
                    // boot's removal becomes visible even if +124 is untouched.
                    g_sector_desc.store(desc, std::memory_order_release);
                    keywatch::Arm(base, {obj + 124, desc + 16});
                    if (FILE* g = fopen(restuff_logpath(), "a"))
                      fprintf(g, "SECTORWATCH t=%ldms armed on obj=%08X +124 and "
                                 "listend=%08X (class 2, flags=%02X bit3 set -- stuck sector)\n",
                              long(now), obj, desc + 16, fl),
                          fclose(g);
                  }
                  break;
                }
              }
            }
          }
        }
      }
    }
  }
  __imp__sub_829C7970(ctx, base);
}
REX_EXTERN(__imp__sub_82AEBEF0);
REX_HOOK_RAW(sub_82AEBEF0) {
  // M3.194 (RESTUFF_MERGELOG=1): per-RECORD defer test. The merger's inline
  // intercept calls this directly with r4 = the 64B merge record (the M3.193
  // sub_8279BEA8 hook only saw the per-OBJECT path — its payloads were list
  // nodes, not records). Log record bounds + verdict in the sweep window:
  // the door-area record's per-family pass/fail is the membership verdict.
  const uint32_t rec = ctx.r4.u32;
  __imp__sub_82AEBEF0(ctx, base);
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    // M3.304b: KEYFIX2 must work WITHOUT MERGELOG — the first user A/B ran with
    // only RESTUFF_KEYFIX2=1 and the clear never executed because it was nested
    // inside this logging gate (0 KEYFIX2 lines, 0 DOOR lines). The record
    // parse below runs for either flag; pure logging still requires MERGELOG.
    static const bool kf2_on = getenv("RESTUFF_KEYFIX2") != nullptr;
    if (on || kf2_on) {
      const long t = long(ibwatch::SinceArmMs());
      // M3.196: NO cap, DOOR-BOUNDS filter, full run. Every "identical
      // inputs" proof so far sampled only the stream HEAD (900/6000-call
      // caps) — and mrg5's boot2 proved late-stream variation exists. This
      // logs the complete stream history of just the records whose bounds
      // intersect the wedge area, plus their chunk-data pointer (+32) and
      // index count (+44) so re-grouping across passes is visible.
      if (t >= 30000 && rec >= 0x10000 && rec < 0xC0000000u - 64) {
        auto bef = [&](int off) {
          uint32_t u; memcpy(&u, base + rec + off, 4);
          u = __builtin_bswap32(u);
          float fv; memcpy(&fv, &u, 4);
          return fv;
        };
        const float mnx = bef(0), mnz = bef(8), mxx = bef(16), mxz = bef(24);
        // M3.302: KREC — geolocate 04-signature merge keys. record+36 holds the
        // key pointer (same field KEYFIX walks); pairing the key with this
        // record's bounds ties a doomed (never-registered) key to world-space,
        // which the deep-level-1 triangle hunt needs: an always-missing key was
        // shown NOT to imply a visible hole (E30330CF missed 175/175 at the hut
        // with a clean frame), so key->bb is the discriminator. Log a few per
        // distinct key: enough to read the box, cheap enough for full runs.
        {
          uint32_t keyp;
          memcpy(&keyp, base + rec + 36, 4);
          keyp = __builtin_bswap32(keyp);
          if (keyp >= 0x10000 && keyp < 0xC0000000u - 32) {
            // M3.302d: self-check — v1 of KREC silently logged nothing because
            // the match was wrong. Print the first few blobs UNCONDITIONALLY so
            // "0 KREC lines" is provably "no 0x04 present", not a broken hook.
            {
              static std::atomic<uint32_t> s_selfn{0};
              if (s_selfn.fetch_add(1, std::memory_order_relaxed) < 3) {
                uint64_t q0, q1, q2;
                memcpy(&q0, base + keyp, 8);
                memcpy(&q1, base + keyp + 8, 8);
                memcpy(&q2, base + keyp + 16, 8);
                if (FILE* f = fopen(restuff_logpath(), "a")) {
                  fprintf(f, "KRECSELF keyp=%08X b23=%02X blob=%016llX %016llX %016llX\n",
                          keyp, *(base + keyp + 23),
                          (unsigned long long)__builtin_bswap64(q0),
                          (unsigned long long)__builtin_bswap64(q1),
                          (unsigned long long)__builtin_bswap64(q2));
                  fclose(f);
                }
              }
            }
            // M3.302c: first attempt matched an 8-byte key at blob+0 and logged
            // NOTHING in a full drive — the blob is bigger (KEYFIX's exclusion
            // byte sits at +23). Match the KEYFIX condition and dump the whole
            // 32-byte blob; the QKEY u64's position falls out of the dump.
            // M3.304 (RESTUFF_KEYFIX2=1, DIAGNOSTIC): clear byte23==0x04 on THIS
            // record stream. The user's at-spot run (Aug 22) found 4 records with
            // the exclusion at bb x=107-126, z=-78..-42 (the deep-L1 triangle) —
            // records KEYFIX's obj+112 walk never reaches (different population,
            // recon Q3). One visual A/B decides whether byte-23 is causal here:
            // triangle merges under KEYFIX2 => same mechanism, coverage hole;
            // stays => byte-23 refuted for this site. Scoped to the triangle's
            // box so the rest of the level is untouched.
            static const bool kf2 = getenv("RESTUFF_KEYFIX2") != nullptr;
            if (kf2 && *(base + keyp + 23) == 0x04 &&
                mnx >= 100.f && mxx <= 132.f && mnz >= -84.f && mxz <= -36.f) {
              *(base + keyp + 23) = 0;
              static std::atomic<uint32_t> s_kf2{0};
              const uint32_t n2 = s_kf2.fetch_add(1, std::memory_order_relaxed);
              if (n2 < 20 || n2 % 500 == 0) {
                if (FILE* f = fopen(restuff_logpath(), "a"))
                  fprintf(f, "KEYFIX2#%u t=%ldms rec=%08X bb=%.1f,%.1f,%.1f,%.1f\n",
                          n2, t, rec, mnx, mnz, mxx, mxz), fclose(f);
              }
            }
            if (*(base + keyp + 23) == 0x04) {
              uint64_t q[4];
              memcpy(q, base + keyp, 32);
              static std::mutex s_kmu;
              static std::map<uint64_t, uint32_t> s_kseen;
              bool want = false;
              {
                std::lock_guard<std::mutex> lk(s_kmu);
                want = s_kseen[__builtin_bswap64(q[0])]++ < 6;
              }
              if (want) {
                if (FILE* f = fopen(restuff_logpath(), "a")) {
                  fprintf(f, "KREC t=%ldms blob=%016llX %016llX %016llX %016llX rec=%08X bb=%.1f,%.1f,%.1f,%.1f\n",
                          t, (unsigned long long)__builtin_bswap64(q[0]),
                          (unsigned long long)__builtin_bswap64(q[1]),
                          (unsigned long long)__builtin_bswap64(q[2]),
                          (unsigned long long)__builtin_bswap64(q[3]),
                          rec, mnx, mnz, mxx, mxz);
                  fclose(f);
                }
              }
            }
          }
        }
        // M3.311b: DOOR2 — same record stream, filtered to the DEEP-L1
        // triangle's box (KREC bbs x107-126/z-78..-42) so at-spot runs get the
        // full record-population history there (the hut DOOR box below never
        // matches the deep site).
        if (mnx <= 132.f && mxx >= 100.f && mnz <= -36.f && mxz >= -84.f &&
            mxx - mnx < 200.f && mxx >= mnx) {
          uint32_t dptr2, cnt2;
          memcpy(&dptr2, base + rec + 32, 4);
          memcpy(&cnt2, base + rec + 44, 4);
          static std::atomic<uint32_t> s_n2{0};
          const uint32_t n2 = s_n2.fetch_add(1, std::memory_order_relaxed);
          if (n2 < 60000) {
            if (FILE* f = fopen(restuff_logpath(), "a"))
              fprintf(f, "DOOR2#%u t=%ldms rec=%08X dptr=%08X cnt=%u bb=%.1f,%.1f,%.1f,%.1f\n",
                      n2, t, rec, __builtin_bswap32(dptr2), __builtin_bswap32(cnt2),
                      mnx, mnz, mxx, mxz),
                  fclose(f);
          }
        }
        if (mnx <= 90.f && mxx >= 75.f && mnz <= -24.f && mxz >= -35.f &&
            mxx - mnx < 200.f && mxx >= mnx) {
          uint32_t dptr, cnt;
          memcpy(&dptr, base + rec + 32, 4);
          memcpy(&cnt, base + rec + 44, 4);
          static std::atomic<uint32_t> s_n{0};
          const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
          if (n < 60000) {
            if (FILE* f = fopen(restuff_logpath(), "a")) {
              fprintf(f, "DOOR#%u t=%ldms rec=%08X r=%u dptr=%08X cnt=%u "
                      "bb=%.1f,%.1f,%.1f,%.1f\n",
                      n, t, rec, uint8_t(ctx.r3.u32), __builtin_bswap32(dptr),
                      __builtin_bswap32(cnt), mnx, mnz, mxx, mxz);
              fclose(f);
            }
          }
        }
      }
    }
  }
}
// M3.302b (RESTUFF_MERGELOG=1, observation only): registry-loss forensics for
// the deep-level-1 triangle. IDA recon (Aug 22): each sector carries two sorted
// u64-key registries (+0x80/+0x84); the ONLY inserter is sub_829EC5D0(holder,
// key); the merge path sub_82992D38 ERASES a registry wholesale via
// sub_829A3F90 and re-inserts only the two merging appliers' keys — a third
// applier's registration is silently dropped (loss mechanism L1). A walk key
// missing from every registry gets its light EXCLUDED (prune gate: bit5=1,
// bit3=0 → contains() decides), which bakes byte23=0x04 into merge keys and
// blocks coalescing. These two hooks log erases (with the keys destroyed) and
// inserts, so an erased-but-never-reinserted key is directly visible in the
// mergelog. No behavior change.
// M3.308 (RESTUFF_TASKARM=<N>, DIAGNOSTIC): arm parked registry tasks. Recon
// P-series: the pump (render thread, per frame: vt[35]→sub_829815C8→
// sub_8297E660→sub_82B25130→...→sub_82B26A80) walks BOTH pending rings every
// frame and runs a task's callback only when `+112 != +111 || (+113 & 2)`.
// Live probe: 374 tasks pending, active=0, frozen — they sit UN-ARMED because
// arming (sub_829E5120 → task+112=2) comes from sector-activation events that
// never fire while the player stands still. Minimal experiment: write the arm
// byte (+112=2) on up to N un-armed pending tasks per frame at pump entry and
// let the engine's own walk run them. Registry 0x83340DE0: ring heads +0/+8
// (link at task+128 ⇒ task = node-128), pending count +16.
// M3.308b (RESTUFF_MERGELOG=1): pump forensics. TASKARM armed 214 tasks and
// NOTHING ran (last-state never advanced, pending frozen) — so measure the
// pump directly: sub_82B25130's args (r5==0 disables both promoters per recon
// P1) and whether the task callback sub_829896C8 EVER fires.
REX_EXTERN(__imp__sub_82B25130);
REX_HOOK_RAW(sub_82B25130) {
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
      if (n < 20 || n % 1000 == 0) {
        if (FILE* f = fopen(restuff_logpath(), "a"))
          fprintf(f, "PUMP#%u t=%ldms r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X\n",
                  n, long(ibwatch::SinceArmMs()), ctx.r3.u32, ctx.r4.u32,
                  ctx.r5.u32, ctx.r6.u32, ctx.r7.u32),
              fclose(f);
      }
    }
  }
  __imp__sub_82B25130(ctx, base);
}
REX_EXTERN(__imp__sub_829896C8);
REX_HOOK_RAW(sub_829896C8) {
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
      if (n < 20 || n % 500 == 0) {
        if (FILE* f = fopen(restuff_logpath(), "a"))
          fprintf(f, "TASKCB#%u t=%ldms r3=%08X r4=%08X\n", n,
                  long(ibwatch::SinceArmMs()), ctx.r3.u32, ctx.r4.u32),
              fclose(f);
      }
    }
  }
  __imp__sub_829896C8(ctx, base);
}
REX_EXTERN(__imp__sub_8297E660);
REX_EXTERN(__imp__sub_829E5120);
REX_EXTERN(__imp__sub_82B24690);
REX_HOOK_RAW(sub_8297E660) {
  // M3.309 (RESTUFF_TASKARM2=<N>, DIAGNOSTIC): arm pending-ring tasks via the
  // ENGINE'S OWN arm function sub_829E5120(task, 0, 1) — the raw +112 byte
  // poke (M3.308) armed 214 tasks and none ran, so the walk's fire condition
  // involves state only the real arm sets. Recon FF: this is the safe minimal
  // intervention; the same frame's sub_82B28F00 walk then runs armed tasks.
  // Runs BEFORE the original with a COPIED context so the pump sees clean regs.
  {
    static const int arm2_n = [] {
      const char* e = getenv("RESTUFF_TASKARM2");
      return e ? atoi(e) : 0;
    }();
    if (arm2_n > 0) {
      auto rd32 = [&](uint32_t a) -> uint32_t {
        if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
        uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
      };
      static std::atomic<uint32_t> s_armed2{0};
      for (uint32_t ring = 0x83340DE0u; ring <= 0x83340DE8u; ring += 8) {
        int armed_this = 0;
        uint32_t node = rd32(ring);
        uint32_t guard = 0;
        while (node && node != ring && node >= 0x10000 &&
               node < 0xC0000000u - 8 && guard++ < 1024 && armed_this < arm2_n) {
          const uint32_t next = rd32(node);
          const uint32_t task = node - 128;
          if (task >= 0x10000 && task < 0xC0000000u - 160) {
            const uint8_t st = *(base + task + 112);
            const uint8_t last = *(base + task + 111);
            if (st == last) {
              // M3.310: ARM2 revealed +112==+111==2 — these tasks RAN once and
              // their completion never landed (owner bit 0x20 never set, never
              // unlinked). Re-arming is a no-op; the remaining lever is the
              // engine's own dequeue+rearm+run-sync sub_82B24690(task, 1),
              // which also decrements pending — measurable at TASKQ.
              PPCContext c2 = ctx;
              c2.r3.u32 = task;
              c2.r4.u32 = 1;
              __imp__sub_82B24690(c2, base);
              ++armed_this;
              const uint32_t k = s_armed2.fetch_add(1, std::memory_order_relaxed);
              if (k < 20 || k % 200 == 0) {
                if (FILE* f = fopen(restuff_logpath(), "a"))
                  fprintf(f, "ARM3#%u t=%ldms task=%08X st=%u->%u\n", k,
                          long(ibwatch::SinceArmMs()), task, st,
                          *(base + task + 112)),
                      fclose(f);
              }
            }
          }
          node = next;
        }
      }
    }
  }
  {
    static const int arm_n = [] {
      const char* e = getenv("RESTUFF_TASKARM");
      return e ? atoi(e) : 0;
    }();
    if (arm_n > 0) {
      auto rd32 = [&](uint32_t a) -> uint32_t {
        if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
        uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
      };
      static std::atomic<uint32_t> s_armed{0};
      for (uint32_t ring = 0x83340DE0u; ring <= 0x83340DE8u; ring += 8) {
        int armed_this = 0;
        uint32_t node = rd32(ring);
        uint32_t guard = 0;
        while (node && node != ring && node >= 0x10000 &&
               node < 0xC0000000u - 8 && guard++ < 1024 && armed_this < arm_n) {
          const uint32_t next = rd32(node);
          const uint32_t task = node - 128;
          if (task >= 0x10000 && task < 0xC0000000u - 160) {
            const uint8_t st = *(base + task + 112);
            const uint8_t last = *(base + task + 111);
            const uint8_t dirty = *(base + task + 113);
            if (st == last && !(dirty & 2)) {
              *(base + task + 112) = 2;
              ++armed_this;
              const uint32_t k = s_armed.fetch_add(1, std::memory_order_relaxed);
              if (k < 20 || k % 200 == 0) {
                if (FILE* f = fopen(restuff_logpath(), "a"))
                  fprintf(f, "TASKARM#%u t=%ldms task=%08X st=%u last=%u\n", k,
                          long(ibwatch::SinceArmMs()), task, st, last),
                      fclose(f);
              }
            }
          }
          node = next;
        }
      }
    }
  }
  __imp__sub_8297E660(ctx, base);
}
// M3.311 (RESTUFF_MERGELOG=1): the ENABLE choke point. Recon G1: bit 0x20 of
// owner+0x49 is the gameplay ENABLED flag; sub_82992398(this,bool) is the
// single canonical setter (listener notify + on-changed virtual + broadcast +
// via wrapper sub_82993D80 the render-item version bump). The deep-L1 triangle
// heals on camera movement — logging every enable transition WITH the caller
// LR names the gameplay trigger the recomp under-fires. Log CHANGES only.
REX_EXTERN(__imp__sub_82992398);
REX_HOOK_RAW(sub_82992398) {
  uint32_t self = ctx.r3.u32, en = ctx.r4.u32, lr = uint32_t(ctx.lr);
  uint8_t before = 0;
  const bool valid = self >= 0x10000 && self < 0xC0000000u - 80;
  if (valid) before = *(base + self + 0x49);
  __imp__sub_82992398(ctx, base);
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on && valid) {
      const uint8_t after = *(base + self + 0x49);
      if (((before ^ after) & 0x20) != 0) {
        static std::atomic<uint32_t> s_n{0};
        const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
        if (n < 400 || (n & 63) == 0) {
          if (FILE* f = fopen(restuff_logpath(), "a"))
            fprintf(f, "ENABLE#%u t=%ldms obj=%08X %02X->%02X en=%u lr=%08X\n", n,
                    long(ibwatch::SinceArmMs()), self, before, after, en, lr),
                fclose(f);
        }
      }
    }
  }
}
// M3.307 (RESTUFF_MERGELOG=1): readiness predicate sub_8298E8D8 —
// `applier+184 == 0 && (*(owner+120)+0x49 & 0x20)`, recursive over ancestors
// (recon F-series). The 36-record merge plateau's prime suspect is this
// predicate failing forever for parked appliers (streaming-completion signal
// never arrives under recomp). Log sampled verdicts with the two inputs.
REX_EXTERN(__imp__sub_8298E8D8);
REX_HOOK_RAW(sub_8298E8D8) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_8298E8D8(ctx, base);
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on && self >= 0x10000 && self < 0xC0000000u - 200) {
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
      if (n < 200 || (n & 1023) == 0) {
        auto rd32 = [&](uint32_t a) -> uint32_t {
          if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
          uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
        };
        const uint32_t f184 = rd32(self + 184);
        const uint32_t owner = rd32(self + 120);
        const uint32_t oflag = owner ? (rd32(owner + 0x48) >> 16) & 0xFF : 0;  // byte at +0x49
        if (FILE* f = fopen(restuff_logpath(), "a"))
          fprintf(f, "READY#%u t=%ldms self=%08X verdict=%u f184=%08X oflag49=%02X\n",
                  n, long(ibwatch::SinceArmMs()), self, ctx.r3.u32 & 1, f184, oflag),
              fclose(f);
      }
    }
  }
}
REX_EXTERN(__imp__sub_829A3F90);
REX_HOOK_RAW(sub_829A3F90) {
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      const uint32_t h = ctx.r3.u32;
      auto rd32 = [&](uint32_t a) -> uint32_t {
        if (a < 0x10000 || a >= 0xC0000000u - 4) return 0;
        uint32_t v; memcpy(&v, base + a, 4); return __builtin_bswap32(v);
      };
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
      if (n < 4000 && h >= 0x10000 && h < 0xC0000000u - 28) {
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "KERASE#%u t=%ldms h=%08X w=[%08X %08X %08X %08X %08X %08X %08X]",
                  n, long(ibwatch::SinceArmMs()), h, rd32(h), rd32(h + 4), rd32(h + 8),
                  rd32(h + 12), rd32(h + 16), rd32(h + 20), rd32(h + 24));
          // Container layout is unproven — dump the first word pair that looks
          // like [begin,end) bounds of a small u64 array and print its keys.
          for (int o = 0; o <= 20; o += 4) {
            const uint32_t lo = rd32(h + o), hi = rd32(h + o + 4);
            if (lo >= 0x10000 && hi > lo && hi - lo <= 32 * 8 && ((hi - lo) & 7) == 0 &&
                hi < 0xC0000000u - 8) {
              fprintf(f, " keys@+%d:", o);
              for (uint32_t e = lo; e + 8 <= hi; e += 8) {
                uint64_t k; memcpy(&k, base + e, 8);
                fprintf(f, " %016llX", (unsigned long long)__builtin_bswap64(k));
              }
              break;
            }
          }
          fprintf(f, "\n");
          fclose(f);
        }
      }
    }
  }
  __imp__sub_829A3F90(ctx, base);
}
REX_EXTERN(__imp__sub_829EC5D0);
REX_HOOK_RAW(sub_829EC5D0) {
  restuff_harvest_wkey(ctx.r4.u64);  // M3.312e: feed the BIT5FIX key set
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      static std::atomic<uint32_t> s_n{0};
      const uint32_t n = s_n.fetch_add(1, std::memory_order_relaxed);
      if (n < 8000) {
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "KREG#%u t=%ldms h=%08X k=%016llX\n", n,
                  long(ibwatch::SinceArmMs()), ctx.r3.u32,
                  (unsigned long long)ctx.r4.u64);
          fclose(f);
        }
      }
    }
  }
  __imp__sub_829EC5D0(ctx, base);
}
REX_EXTERN(__imp__sub_82AC0768);
REX_HOOK_RAW(sub_82AC0768) {
  __imp__sub_82AC0768(ctx, base);
  mergegate::g_0768_calls.fetch_add(1, std::memory_order_relaxed);
  if (uint8_t(ctx.r3.u32)) mergegate::g_0768_true.fetch_add(1, std::memory_order_relaxed);
}
REX_EXTERN(__imp__sub_8279BEA8);
REX_HOOK_RAW(sub_8279BEA8) {
  mergegate::g_sig.fetch_add(1, std::memory_order_relaxed);
  // M3.193 (RESTUFF_MERGELOG=1): log each deferred item's IDENTITY. Defer
  // counts and signals are equal across outcomes (mrg6) and late drains do
  // work (the merged-boot door touch-up), so the fork must be the deferred
  // SET's membership — same count, different members. r4 = the item, whose
  // leading bounds floats are the cross-boot-stable identity; the door-area
  // record's presence/absence per family is the verdict.
  {
    static const bool on = getenv("RESTUFF_MERGELOG") != nullptr;
    if (on) {
      static std::atomic<uint32_t> s_d{0};
      const uint32_t d = s_d.fetch_add(1, std::memory_order_relaxed);
      if (d < 3000) {
        const uint32_t item = ctx.r4.u32;
        if (FILE* f = fopen(restuff_logpath(), "a")) {
          fprintf(f, "DEFER#%u t=%ldms item=%08X q=%08X", d,
                  long(ibwatch::SinceArmMs()), item, ctx.r3.u32);
          if (item >= 0x10000 && item < 0xC0000000u - 32) {
            fprintf(f, " b=");
            for (int i = 0; i < 32; ++i) fprintf(f, "%02X", base[item + i]);
          }
          fprintf(f, "\n");
          fclose(f);
        }
      }
    }
  }
  __imp__sub_8279BEA8(ctx, base);
}

// ---------------------------------------------------------------------------
// M3.254 (RESTUFF_SIGNSTATE=1): name WHO drives the sign-in prompt.
//
// The popup the user hears is the title's own
// hazingGameStates::GSLevelSelection::StateSigninPrompt (RTTI TypeDescriptor
// 832A4E98, COL 8237C934, vtable 82226408, ctor sub_827F0508, built with the
// rest of the level-selection states by sub_827D9B60). It provably never calls
// XamShowSigninUI, and reporting "signed in" from XamUserGetSigninState did not
// stop it -- so the decision is the title's own.
//
// The state cannot be isolated by hooking a unique method: its vtable has only
// FOUR slots and all four are shared base-class implementations (each appears
// 9-12x image-wide). So identify it by VPTR -- log the calls whose `this`
// carries the StateSigninPrompt vtable, together with the caller's LR, which
// names the exact state-machine site to decompile next.
//
// Lua is not an option here (checked, per the standing fix-method priority):
// none of the 468 dumped game scripts mention hState*, LevelSelection or the
// profile manager, and the HazingPlayerProfileManager_* binding names are DEAD
// strings -- zero code materializations and zero data references image-wide.
// ---------------------------------------------------------------------------
static void restuff_signstate_probe(PPCContext& ctx, uint8_t* base, const char* which) {
  static const bool on = getenv("RESTUFF_SIGNSTATE") != nullptr;
  if (!on) return;
  const uint32_t self = ctx.r3.u32;
  if (self < 0x10000u || self >= 0xC0000000u - 8u) return;
  uint32_t vptr;
  std::memcpy(&vptr, base + self, 4);
  if (__builtin_bswap32(vptr) != 0x82226408u) return;   // not StateSigninPrompt
  static std::atomic<uint32_t> nlog{0};
  const uint32_t k = nlog.fetch_add(1, std::memory_order_relaxed);
  if (k >= 64) return;                                   // it ticks every frame
  if (FILE* f = fopen(restuff_logpath(), "a")) {
    fprintf(f, "SIGNSTATE#%u t=%ldms %s this=%08X lr=%08X\n",
            k, long(ibwatch::SinceArmMs()), which, self, uint32_t(ctx.lr));
    fclose(f);
  }
}

REX_EXTERN(__imp__sub_827EE9C0);
REX_HOOK_RAW(sub_827EE9C0) {
  restuff_signstate_probe(ctx, base, "slot0");
  __imp__sub_827EE9C0(ctx, base);
}

REX_EXTERN(__imp__sub_82781EA8);
REX_HOOK_RAW(sub_82781EA8) {
  restuff_signstate_probe(ctx, base, "slot1");
  __imp__sub_82781EA8(ctx, base);
}

REX_EXTERN(__imp__sub_82781EE8);
REX_HOOK_RAW(sub_82781EE8) {
  restuff_signstate_probe(ctx, base, "slot2");
  __imp__sub_82781EE8(ctx, base);
}

REX_EXTERN(__imp__sub_82781FA0);
REX_HOOK_RAW(sub_82781FA0) {
  restuff_signstate_probe(ctx, base, "slot3");
  __imp__sub_82781FA0(ctx, base);
}

// ---------------------------------------------------------------------------
// M3.256 (RESTUFF_ONLINESM=1): timeline of the ONLINE sign-in state machine.
//
// Correction to M3.254: the dialog the user sees ("Would you like to sign in
// with a gamer profile?" / Sign In / Continue Without Sign In) is on the TITLE
// screen, but the class M3.254 traced is scoped GSLevelSelection -- a different
// prompt. M3.254 ran against a boot that DID show the dialog (captured in
// env_sign11/W01.png) and logged ZERO hits, which is the proof.
//
// Correction to M3.255: it hooked sub_82B2D5A0 and read the vptr on ENTRY, but
// that function is a CONSTRUCTOR (it writes vptr 822800D8), so the read was of
// uninitialised memory and never matched. Zero hits there meant nothing.
//
// sub_82B2EF48 decompiles to the SignInStateMachine constructor and gives the
// exact layout:
//     [0] vtable 82280398
//     [1] CURRENT STATE pointer            <- what we actually want
//     [2] {822800F0, id 2} ShowingUi
//     [3] {822800E8, id 1} UiNotShown
//     [4] {822800F8, id 3} UiShown
//     [1] = [3]  -- starts in UiNotShown
// Each state is just {vptr, id}. So hook the machine's OWN vtable methods,
// where r3 IS the machine, and report *(this+4) -> *(state+4) = the state id.
// Log transitions only, with LR, so a dialog boot can be diffed against a clean
// one (clean boots show no sign-in activity at all).
namespace onlinesm {
static void Probe(PPCContext& ctx, uint8_t* base, const char* site) {
  static const bool on = getenv("RESTUFF_ONLINESM") != nullptr;
  if (!on) return;
  const uint32_t self = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  if (self < 0x10000u || self >= 0xC0000000u - 32u) return;
  auto rd32 = [&](uint32_t a) -> uint32_t {
    if (a < 0x10000u || a >= 0xC0000000u - 4u) return 0;
    uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
  };
  // PROVE THE INSTRUMENT IS ALIVE. Two probes in a row (M3.254, M3.255) logged
  // nothing, and "no activity" was indistinguishable from "broken" -- one was
  // genuinely broken. So report the first few calls at each site unconditionally
  // BEFORE any filtering. Silence after this is silence, not a dead hook.
  {
    static std::atomic<uint32_t> nalive{0};
    const uint32_t k = nalive.fetch_add(1, std::memory_order_relaxed);
    if (k < 6)
      xamlog::Line("ONLINESM-ALIVE#%u t=%ldms via %s r3=%08X vptr=%08X\n",
                   k, long(ibwatch::SinceArmMs()), site, self, rd32(self));
  }
  if (rd32(self) != 0x82280398u) return;      // not the state machine
  const uint32_t st = rd32(self + 4);
  if (!st) return;
  const uint32_t id = rd32(st + 4);
  static std::atomic<uint32_t> s_last{0};
  uint32_t prev = s_last.load(std::memory_order_relaxed);
  if (id == prev || !s_last.compare_exchange_strong(prev, id)) return;
  const char* name = id == 1 ? "UiNotShown" : id == 2 ? "ShowingUi"
                   : id == 3 ? "UiShown" : "?";
  xamlog::Line("ONLINESM t=%ldms %s(id=%u, was %u) via %s machine=%08X lr=%08X\n",
               long(ibwatch::SinceArmMs()), name, id, prev, site, self, lr);
}
}  // namespace onlinesm

REX_EXTERN(__imp__sub_82B3A638);
REX_HOOK_RAW(sub_82B3A638) { onlinesm::Probe(ctx, base, "m0"); __imp__sub_82B3A638(ctx, base); }

REX_EXTERN(__imp__sub_82B3A7B0);
REX_HOOK_RAW(sub_82B3A7B0) { onlinesm::Probe(ctx, base, "m1"); __imp__sub_82B3A7B0(ctx, base); }

REX_EXTERN(__imp__sub_82B339B8);
REX_HOOK_RAW(sub_82B339B8) { onlinesm::Probe(ctx, base, "m2"); __imp__sub_82B339B8(ctx, base); }

// M3.256b: the machine's constructor has exactly ONE direct caller
// (82B32740 in sub_82B32700). Log construction so "no ONLINESM lines" can be
// read correctly: machine built + no transitions = genuinely no sign-in
// activity; machine never built = the subsystem is not even instantiated.
REX_EXTERN(__imp__sub_82B2EF48);
REX_HOOK_RAW(sub_82B2EF48) {
  const uint32_t self = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  __imp__sub_82B2EF48(ctx, base);
  if (getenv("RESTUFF_ONLINESM")) {
    auto rd32 = [&](uint32_t a) -> uint32_t {
      if (a < 0x10000u || a >= 0xC0000000u - 4u) return 0;
      uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
    };
    xamlog::Line("ONLINESM-CTOR t=%ldms machine=%08X vt=%08X cur=%08X lr=%08X\n",
                 long(ibwatch::SinceArmMs()), self, rd32(self), rd32(self + 4), lr);
  }
}

// M3.257 (cont): capture the profile manager and, with RESTUFF_SIGNCACHE=1,
// dump the four cached slot states the title will actually read. Logged before
// anything is written so the +136 offset is verified from real data, not
// assumed -- the write only happens under RESTUFF_SIGNCACHE_FIX=1.
REX_EXTERN(__imp__sub_82B2F708);
REX_HOOK_RAW(sub_82B2F708) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_82B2F708(ctx, base);
  auto rd32 = [&](uint32_t a) -> uint32_t {
    if (a < 0x10000u || a >= 0xC0000000u - 4u) return 0;
    uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
  };
  if (self < 0x10000u || self >= 0xC0000000u - 80u) return;
  const uint32_t mgr = rd32(self + 72);
  if (mgr < 0x10000u || mgr >= 0xC0000000u - 160u) return;
  g_profile_mgr.store(mgr, std::memory_order_relaxed);
  if (restuff_signin_evidence()) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 8)
      xamlog::Line("SIGNCACHE#%u t=%ldms mgr=%08X slots=[%u %u %u %u] padmask=%u\n",
                   k, long(ibwatch::SinceArmMs()), mgr, rd32(mgr + 136),
                   rd32(mgr + 140), rd32(mgr + 144), rd32(mgr + 148),
                   g_pad_connected.load(std::memory_order_relaxed));
  }
  restuff_signcache_sync(base);
}

// ---------------------------------------------------------------------------
// M3.268: hook the GUEST THUNKS, not the XAM import names.
//
// ⛔ RETRACTION. M3.259/M3.266 hooked XAM export names (XamShowMessageBoxUI,
// XamUserGetXUID, ...) and every one logged ZERO, which I read as "the game
// never calls it". That reading was WRONG: import-name hooks do not intercept
// the guest at all. Proved it by hooking __imp__XamInputGetState, which the
// guest thunk sub_830B3EF0 calls on EVERY pad read -- the import hook logged 0
// while PADLOG on the thunk logged 12 in the same boot. So "MSGBOX never fires
// on a healthy boot" was an instrument artifact, not a fact.
//
// (XAMSHOW=0 for XamShowSigninUI SURVIVES this: that one hooks the guest thunk
// sub_830B2F68, not an import name.)
//
// Correct targets, found by branching from each import address (the control,
// XamUserGetSigninState at 83233754, resolves to sub_830B3198 -- exactly the
// thunk that already works):
//     XamShowMessageBoxUI        83233814 -> sub_830B2FA0
//     XamUserGetXUID             83233694 -> sub_830B33F8
//     XamUserGetName             83233834 -> sub_830B3190
//     XamUserCheckPrivilege      83233724 -> sub_830B31A8  (already hooked)
//     XamUserReadProfileSettings 832336E4 -> sub_82ED6B40
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__sub_830B2FA0);
REX_HOOK_RAW(sub_830B2FA0) {  // XamShowMessageBoxUI -- CAN pin XN_SYS_UI true
  const uint32_t user = ctx.r3.u32, lr = uint32_t(ctx.lr);
  if (restuff_signin_evidence())
    xamlog::Line("MSGBOX t=%ldms XamShowMessageBoxUI(user=%u) lr=%08X -- ENTER"
                 " (blocks here if the dialog is never answered)\n",
                 long(ibwatch::SinceArmMs()), user, lr);
  __imp__sub_830B2FA0(ctx, base);
  if (restuff_signin_evidence())
    xamlog::Line("MSGBOX t=%ldms XamShowMessageBoxUI RETURNED r3=%08X\n",
                 long(ibwatch::SinceArmMs()), ctx.r3.u32);
}

REX_EXTERN(__imp__sub_830B33F8);
REX_HOOK_RAW(sub_830B33F8) {  // XamUserGetXUID
  const uint32_t user = ctx.r3.u32;
  __imp__sub_830B33F8(ctx, base);
  if (getenv("RESTUFF_PROFILE_FAULT") &&
      std::strcmp(getenv("RESTUFF_PROFILE_FAULT"), "xuid") == 0)
    ctx.r3.u64 = 0x48F;
  if (getenv("RESTUFF_PROFILELOG")) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 12)
      xamlog::Line("PROFILE#%u XamUserGetXUID(user=%u) -> %08X\n", k, user, ctx.r3.u32);
  }
}

REX_EXTERN(__imp__sub_830B3190);
REX_HOOK_RAW(sub_830B3190) {  // XamUserGetName
  const uint32_t user = ctx.r3.u32;
  __imp__sub_830B3190(ctx, base);
  if (getenv("RESTUFF_PROFILE_FAULT") &&
      std::strcmp(getenv("RESTUFF_PROFILE_FAULT"), "name") == 0)
    ctx.r3.u64 = 0x48F;
  if (getenv("RESTUFF_PROFILELOG")) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 12)
      xamlog::Line("PROFILE#%u XamUserGetName(user=%u) -> %08X\n", k, user, ctx.r3.u32);
  }
}

// ---------------------------------------------------------------------------
// M3.269: is the t=44s SYSTEM-UI DISPATCH what the stuck boots never finish?
//
// SMLOG on a healthy boot shows the online sign-in state machine (A5124D74)
// making exactly TWO transitions, 200ms apart, at t~44s:
//     SM#0 t=43937ms state A50EA740 -> A50EA748     (UiNotShown -> shown)
//     SM#1 t=44137ms state A50EA748 -> A50EA740     (back to UiNotShown)
// That is precisely the SDK dialog-dispatch signature: pre() broadcasts
// XN_SYS_UI=true, post() sleeps 100ms and broadcasts false (xam_ui.cpp:161).
// It coincides with the t=43.9-44.3s cluster of signin queries.
//
// The game imports XamShowDeviceSelectorUI, which the SDK services through
// xeXamDispatchHeadless -- so every boot raises a system UI here and CLOSES it.
// If a boot ever fails to close it, XN_SYS_UI stays true, the guest is left
// believing a system UI is up, and input dies while the game's own prompt sits
// on screen -- exactly the reported symptom.
//
// RESTUFF_DEVSEL_FAULT=1 returns X_ERROR_IO_PENDING WITHOUT calling the
// original, so the overlapped completion never runs and the UI never closes.
// If the prompt appears, that is the mechanism. Guest thunk for import
// 83233804 is sub_830B2F98 (found the same way the working hooks were).
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__sub_830B2F98);
REX_HOOK_RAW(sub_830B2F98) {  // XamShowDeviceSelectorUI
  static const bool fault = getenv("RESTUFF_DEVSEL_FAULT") != nullptr;
  const uint32_t lr = uint32_t(ctx.lr);
  if (restuff_signin_evidence())
    xamlog::Line("DEVSEL t=%ldms XamShowDeviceSelectorUI(user=%u) lr=%08X%s\n",
                 long(ibwatch::SinceArmMs()), ctx.r3.u32, lr,
                 fault ? "  [FAULT: returning IO_PENDING, never completing]" : "");
  if (fault) {
    ctx.r3.u64 = 0x3E5;  // X_ERROR_IO_PENDING -- and nothing will ever complete it
    return;
  }
  __imp__sub_830B2F98(ctx, base);
  if (restuff_signin_evidence())
    xamlog::Line("DEVSEL t=%ldms returned r3=%08X\n",
                 long(ibwatch::SinceArmMs()), ctx.r3.u32);
}

// ---------------------------------------------------------------------------
// M3.270 (RESTUFF_PROFREAD_FAULT=1): the sign-in analogue of the storage result.
//
// M3.269 proved the SHAPE: XamShowDeviceSelectorUI is an OVERLAPPED call that
// returns IO_PENDING and is finished by CompleteOverlappedDeferred. Stop that
// completion and the game strands on "No storage device is currently selected"
// -- 2/2 boots, a blocking prompt in the same style as the sign-in one, with the
// game retrying the call forever (22 retries logged).
//
// So an XAM async op that never completes DOES produce exactly this class of
// stuck dialog. The sign-in prompt is presumably the same failure on a
// different call. The profile read is the obvious candidate: the signin queries
// cluster at t=43.9-44.3s alongside the device selector, and
// XamUserReadProfileSettings (import 832336E4 -> guest thunk sub_82ED6B40) is
// what sub_82840A00 uses to load the player's profile.
//
// ⚠️ NOTE FOR THE READER: the storage dialog and the sign-in dialog are visually
// almost identical -- same box, same two-option layout. classify_boot2 scored
// the storage one as SIGNIN (dist 1.1) until a STORAGE reference was added.
// Always confirm which dialog a "hit" actually is BY EYE.
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__sub_82ED6B40);
REX_HOOK_RAW(sub_82ED6B40) {  // XamUserReadProfileSettings
  static const bool fault = getenv("RESTUFF_PROFREAD_FAULT") != nullptr;
  if (getenv("RESTUFF_PROFILELOG")) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 8)
      xamlog::Line("PROFREAD#%u t=%ldms ReadProfileSettings(r3=%08X r4=%08X)%s\n",
                   k, long(ibwatch::SinceArmMs()), ctx.r3.u32, ctx.r4.u32,
                   fault ? "  [FAULT: IO_PENDING, never completing]" : "");
  }
  if (fault) {
    ctx.r3.u64 = 0x3E5;  // X_ERROR_IO_PENDING, never completed
    return;
  }
  __imp__sub_82ED6B40(ctx, base);
}

// ---------------------------------------------------------------------------
// M3.272 (RESTUFF_CONTENT_FAULT=create|enum|devstate|flush): the next
// enumerable class, chosen because the device-selector result proved the shape.
//
// M3.269: stalling XamShowDeviceSelectorUI (an overlapped call finished by
// CompleteOverlappedDeferred) strands the game on the STORAGE prompt, 2/2
// boots. The sign-in prompt is a sibling in the same save-setup flow, and every
// signin ANSWER site has now been faulted without producing it -- so the
// trigger is more likely another async save-path call that never finishes than
// a wrong signin answer.
//
// Guest thunks (import -> thunk, found the way the working hooks were):
//     XamContentCreateEx         832338E4 -> sub_830B4D78
//     XamContentFlush            83233914 -> sub_830B4E10
//     XamContentCreateEnumerator 83233924 -> sub_830B4E18
//     XamContentGetDeviceState   83233934 -> sub_830B4E20
//
// ⚠️ Confirm BY EYE which dialog any hit is: the storage and sign-in prompts are
// near-identical and the classifier scored one as the other until a STORAGE
// reference was added.
// ---------------------------------------------------------------------------
static bool restuff_content_fault_is(const char* which) {
  const char* v = getenv("RESTUFF_CONTENT_FAULT");
  return v && std::strcmp(v, which) == 0;
}
#define RESTUFF_CONTENT_PROBE(thunk, key, label)                                  \
  REX_EXTERN(__imp__##thunk);                                                     \
  REX_HOOK_RAW(thunk) {                                                           \
    const bool f = restuff_content_fault_is(key);                                 \
    if (restuff_signin_evidence()) {                                              \
      static std::atomic<uint32_t> n{0};                                          \
      const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);               \
      if (k < 6)                                                                  \
        xamlog::Line("CONTENT#%u t=%ldms " label "(r3=%08X)%s\n", k,              \
                     long(ibwatch::SinceArmMs()), ctx.r3.u32,                     \
                     f ? "  [FAULT: IO_PENDING, never completing]" : "");         \
    }                                                                             \
    if (f) { ctx.r3.u64 = 0x3E5; return; }                                        \
    __imp__##thunk(ctx, base);                                                    \
  }
RESTUFF_CONTENT_PROBE(sub_830B4D78, "create",   "XamContentCreateEx")
RESTUFF_CONTENT_PROBE(sub_830B4E10, "flush",    "XamContentFlush")
RESTUFF_CONTENT_PROBE(sub_830B4E18, "enum",     "XamContentCreateEnumerator")
RESTUFF_CONTENT_PROBE(sub_830B4E20, "devstate", "XamContentGetDeviceState")
#undef RESTUFF_CONTENT_PROBE

// ---------------------------------------------------------------------------
// M3.273: the ACTUAL title-screen prompt class -- hazingGameStates::
// NotSignedInPrompt. Default on with the rest of the sign-in evidence.
//
// I spent this whole investigation on GSLevelSelection::StateSigninPrompt and
// PROVED it was not the dialog (hooked by vptr against a boot that showed the
// prompt: zero hits). The RTTI dump has a second, TOP-LEVEL class I had never
// looked at:
//     .?AVNotSignedInPrompt@hazingGameStates@@   TD 832A331C, vtable 82225C0C
// which is a much better fit for a title-screen "you are not signed in" prompt
// than a level-selection sub-state.
//
// Two of its four vtable slots are UNIQUE image-wide -- sub_827D6318 and
// sub_827CA2C0 appear exactly once each -- so hooking them needs no vptr test
// at all: if either fires, THIS state is running. On a healthy boot they must
// never fire (that is the control); on a prompt boot they will, and the LR
// names the code that entered the state.
//
// Constructed by sub_827DB5C0, itself called from sub_827EA990.
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__sub_827D6318);
REX_HOOK_RAW(sub_827D6318) {
  if (restuff_signin_evidence()) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 8)
      xamlog::Line("NOTSIGNEDIN#%u t=%ldms slot2 this=%08X lr=%08X"
                   "  <<< NotSignedInPrompt IS ACTIVE\n",
                   k, long(ibwatch::SinceArmMs()), ctx.r3.u32, uint32_t(ctx.lr));
  }
  __imp__sub_827D6318(ctx, base);
}

REX_EXTERN(__imp__sub_827CA2C0);
REX_HOOK_RAW(sub_827CA2C0) {
  if (restuff_signin_evidence()) {
    static std::atomic<uint32_t> n{0};
    const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
    if (k < 8)
      xamlog::Line("NOTSIGNEDIN#%u t=%ldms slot3 this=%08X lr=%08X"
                   "  <<< NotSignedInPrompt IS ACTIVE\n",
                   k, long(ibwatch::SinceArmMs()), ctx.r3.u32, uint32_t(ctx.lr));
  }
  __imp__sub_827CA2C0(ctx, base);
}

// ---------------------------------------------------------------------------
// M3.274: cover the SIBLING prompt states too, so one caught boot answers
// everything.
//
// NotSignedInPrompt (M3.273) is the best candidate and is the only prompt class
// with UNIQUE vtable slots. Its siblings share theirs, so they need a vptr
// filter -- but sub_827F64A0 is slot 0 of BOTH GSUserPrompt (82226964) and
// GSSyncUserPrompt (822269E0), so a single hook plus a vptr test covers the
// family. The prompt is ~2%/boot and cannot be forced (12 fault arms proved
// that), so a catch is expensive: it should identify WHICHEVER prompt class is
// running, not just confirm one guess.
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__sub_827F64A0);
REX_HOOK_RAW(sub_827F64A0) {
  if (restuff_signin_evidence()) {
    const uint32_t self = ctx.r3.u32;
    if (self >= 0x10000u && self < 0xC0000000u - 8u) {
      uint32_t vptr;
      std::memcpy(&vptr, base + self, 4);
      vptr = __builtin_bswap32(vptr);
      const char* who = vptr == 0x82226964u   ? "GSUserPrompt"
                        : vptr == 0x822269E0u ? "GSSyncUserPrompt"
                        : vptr == 0x82225C0Cu ? "NotSignedInPrompt"
                        : vptr == 0x82225C54u ? "DLCRemovedPromptState"
                                              : nullptr;
      if (who) {
        static std::atomic<uint32_t> n{0};
        const uint32_t k = n.fetch_add(1, std::memory_order_relaxed);
        if (k < 8)
          xamlog::Line("PROMPTSTATE#%u t=%ldms %s this=%08X vptr=%08X lr=%08X\n",
                       k, long(ibwatch::SinceArmMs()), who, self, vptr,
                       uint32_t(ctx.lr));
      }
    }
  }
  __imp__sub_827F64A0(ctx, base);
}

// ---------------------------------------------------------------------------
// M3.275: name the prompt's class GENERICALLY -- the guesses have all missed.
//
// BREAKTHROUGH: reverting to pre-M3.253 flags (RESTUFF_NO_STALEFIX=1
// RESTUFF_KEYFIX=1) reproduces the sign-in prompt at ~44% (4 of 9 boots,
// CONFIRMED BY EYE on env_oldflags4) versus 1/123 on shipped defaults. So the
// prompt finally has a reproduction knob.
//
// And the prompt boots have a signature nothing else does:
//     healthy: sm=2 devsel=2      prompt boot: sm=0 devsel=0
// i.e. they never reach the t~44s device-selector/system-UI stage at all. That
// is why every fault injected at those later calls could never produce it --
// the prompt happens EARLIER.
//
// M3.273/274 probes fired ZERO times on those real prompt boots, so
// NotSignedInPrompt (and the GSUserPrompt siblings) are NOT the class either.
// Stop guessing classes: sub_827ECDB8 is slot 0 of ELEVEN front-end state
// vtables, so hooking it and logging each DISTINCT vptr enumerates whichever
// states actually run. On a prompt boot the culprit's vptr will be in the list,
// and its LR names the dispatcher.
// ---------------------------------------------------------------------------
// M3.277 CORRECTION: sub_827ECDB8 is slot 0 = the SCALAR-DELETING DESTRUCTOR,
// so it only runs at teardown -- which is why M3.275 logged zero. Verified: the
// probe IS in the binary and the arm that ran it produced 0 lines. Hook a slot
// that actually ticks instead: sub_827C9CC0 appears 31x image-wide as a
// non-slot-0 entry across the front-end state vtables, i.e. a shared per-frame
// method. Log each DISTINCT vptr it sees; on a prompt boot the culprit state's
// vptr lands in the list with the dispatcher's LR.
//
// (Fourth probe this session to log nothing. The rule earned: before believing
// a probe's silence, prove the hooked function is REACHED -- and check whether
// the vtable slot you picked is a destructor.)
REX_EXTERN(__imp__sub_827C9CC0);
REX_HOOK_RAW(sub_827C9CC0) {
  // M3.285: OPT-IN (was evidence-gated default-on). TICKVPTR proved genuinely
  // non-discriminating -- byte-identical across prompt and healthy boots, three
  // separate times -- so its 13 lines per boot are pure noise in the shipped
  // flight recorder. Kept compilable for future vtable spelunking.
  if (getenv("RESTUFF_TICKVPTR")) {
    const uint32_t self = ctx.r3.u32;
    if (self >= 0x10000u && self < 0xC0000000u - 8u) {
      uint32_t vptr;
      std::memcpy(&vptr, base + self, 4);
      vptr = __builtin_bswap32(vptr);
      static std::atomic<uint32_t> seen2[32];
      static std::atomic<uint32_t> nseen2{0};
      bool known = false;
      const uint32_t n = nseen2.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < n && i < 32; ++i)
        if (seen2[i].load(std::memory_order_relaxed) == vptr) { known = true; break; }
      if (!known && n < 32) {
        seen2[n].store(vptr, std::memory_order_relaxed);
        nseen2.store(n + 1, std::memory_order_relaxed);
        xamlog::Line("TICKVPTR#%u t=%ldms vptr=%08X this=%08X lr=%08X\n", n,
                     long(ibwatch::SinceArmMs()), vptr, self, uint32_t(ctx.lr));
      }
    }
  }
  __imp__sub_827C9CC0(ctx, base);
}

REX_EXTERN(__imp__sub_827ECDB8);
REX_HOOK_RAW(sub_827ECDB8) {
  if (restuff_signin_evidence()) {
    const uint32_t self = ctx.r3.u32;
    if (self >= 0x10000u && self < 0xC0000000u - 8u) {
      uint32_t vptr;
      std::memcpy(&vptr, base + self, 4);
      vptr = __builtin_bswap32(vptr);
      // log each distinct vptr once (small set: 11 vtables share this slot)
      static std::atomic<uint32_t> seen[24];
      static std::atomic<uint32_t> nseen{0};
      bool known = false;
      const uint32_t n = nseen.load(std::memory_order_relaxed);
      for (uint32_t i = 0; i < n && i < 24; ++i)
        if (seen[i].load(std::memory_order_relaxed) == vptr) { known = true; break; }
      if (!known && n < 24) {
        seen[n].store(vptr, std::memory_order_relaxed);
        nseen.store(n + 1, std::memory_order_relaxed);
        xamlog::Line("STATEVPTR#%u t=%ldms vptr=%08X this=%08X lr=%08X\n", n,
                     long(ibwatch::SinceArmMs()), vptr, self, uint32_t(ctx.lr));
      }
    }
  }
  __imp__sub_827ECDB8(ctx, base);
}

// M3.279 (RESTUFF_FEDUMP=1): the container scan, moved to a hook that TICKS.
//
// M3.278 attached it to sub_82B2F708, which runs ONCE at t~188ms -- long before
// the title sequence and the prompt. It logged nothing for that reason alone
// (the probe WAS in the binary; verified). Sixth silent probe of the session,
// same root cause every time: hooking something that does not run at the moment
// the thing I want to observe happens.
//
// The pad read ticks every frame for the whole boot, so scan from there at
// ~1 Hz and report ONLY when the active start-menu sub-state CHANGES -- that
// gives a compact per-boot state timeline instead of a flood, and a stalled
// boot should show NotSignedInPrompt where a healthy one shows MenuState.
static void restuff_fedump_tick(uint8_t* base) {
  static const bool on = getenv("RESTUFF_FEDUMP") != nullptr;
  if (!on) return;
  static std::atomic<int64_t> last{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  int64_t prev = last.load(std::memory_order_relaxed);
  if (now - prev < 1000 || !last.compare_exchange_strong(prev, now)) return;
  auto rd32 = [&](uint32_t a) -> uint32_t {
    if (a < 0x10000u || a >= 0xC0000000u - 4u) return 0;
    uint32_t v; std::memcpy(&v, base + a, 4); return __builtin_bswap32(v);
  };
  const uint32_t c = rd32(0x83331BB4u);
  // POSITIVE CONTROL FIRST. Seven probes have now logged nothing, and every
  // time the cause was an unverified assumption rather than a missing event.
  // This chain has FOUR links -- global -> container -> GSStartMenu at +3974dw
  // -> sub-state pointer -> vptr range -- and filtering on all of them at once
  // hides which one is broken. So dump the raw values once per boot: if
  // GSStartMenu's vptr reads 82225EA4/82225EAC the offsets are right and the
  // problem is further down; if it does not, the container assumption is wrong.
  {
    static std::atomic<uint32_t> nctl{0};
    const uint32_t k = nctl.fetch_add(1, std::memory_order_relaxed);
    if (k < 3)
      xamlog::Line("FECTL#%u t=%ldms global[83331BB4]=%08X sm=%08X smvptr=%08X"
                   " [0]=%08X [1]=%08X [2]=%08X\n",
                   k, long(ibwatch::SinceArmMs()), c, c + 3974u * 4u,
                   rd32(c + 3974u * 4u), rd32(c), rd32(c + 4), rd32(c + 8));
  }
  if (c < 0x10000u || c >= 0xC0000000u - 0x8000u) return;
  // ASSUMPTION-MINIMAL SEARCH. The pointer is NOT inside GSStartMenu (scanning
  // its full 1096-dword extent found nothing, and FECTL confirms the container
  // and offset are right). So stop assuming WHERE the field lives: sweep the
  // whole container for any value that points at an object whose vptr is one of
  // the ten known start-menu sub-state vtables. Wherever the game keeps it,
  // this finds it.
  {
    static const uint32_t kSub[] = {0x82225B6Cu,0x82225B80u,0x82225B94u,0x82225BA8u,
                                    0x82225BBCu,0x82225BD0u,0x82225BE4u,0x82225BF8u,
                                    0x82225C0Cu,0x82225C20u};
    static std::atomic<uint32_t> lastfound{0};
    for (uint32_t q = 0; q < 8192; ++q) {
      const uint32_t v = rd32(c + q * 4);
      if (v < 0x10000u || v >= 0xC0000000u - 4u) continue;
      const uint32_t vp = rd32(v);
      for (uint32_t j = 0; j < 10; ++j) {
        if (vp != kSub[j]) continue;
        uint32_t pv = lastfound.load(std::memory_order_relaxed);
        if (vp == pv || !lastfound.compare_exchange_strong(pv, vp)) { j = 10; break; }
        const char* nm = j==0?"A2MLogoState":j==1?"A2MLegalScreen":j==2?"LoadingState"
                        :j==3?"MenuState":j==4?"LobbyState":j==5?"PlayRequestState"
                        :j==6?"OptionState":j==7?"ManualState"
                        :j==8?"NotSignedInPrompt <<<<<<":"LeaderboardState";
        xamlog::Line("FESTATE t=%ldms container[+%u]=%08X vptr=%08X %s\n",
                     long(ibwatch::SinceArmMs()), q * 4, v, vp, nm);
        j = 10;
      }
    }
  }
  const uint32_t sm = c + 3974u * 4u;            // GSStartMenu
  // GSStartMenu spans container+3974dw .. +5070dw (the next state), i.e. 1096
  // dwords -- scanning only the first 256 was why this found nothing even
  // though the container/offset are CONFIRMED correct (FECTL shows
  // smvptr=82225EAC = GSStartMenu). Scan the whole object.
  static std::atomic<uint32_t> lastv{0};
  for (uint32_t q = 0; q < 1096; ++q) {
    const uint32_t sv = rd32(sm + q * 4);
    if (sv <= sm || sv >= sm + 0x20000u) continue;
    const uint32_t vptr = rd32(sv);
    if (vptr < 0x82225000u || vptr > 0x82226000u) continue;   // start-menu sub-state vtable
    uint32_t pv = lastv.load(std::memory_order_relaxed);
    if (vptr == pv || !lastv.compare_exchange_strong(pv, vptr)) return;
    const char* nm = vptr == 0x82225B6Cu ? "A2MLogoState"
                   : vptr == 0x82225B80u ? "A2MLegalScreen"
                   : vptr == 0x82225B94u ? "LoadingState"
                   : vptr == 0x82225BA8u ? "MenuState"
                   : vptr == 0x82225BBCu ? "LobbyState"
                   : vptr == 0x82225BD0u ? "PlayRequestState"
                   : vptr == 0x82225BE4u ? "OptionState"
                   : vptr == 0x82225BF8u ? "ManualState"
                   : vptr == 0x82225C0Cu ? "NotSignedInPrompt <<<<<<"
                   : vptr == 0x82225C20u ? "LeaderboardState" : "?";
    xamlog::Line("FESTATE t=%ldms startmenu[+%u] -> %08X vptr=%08X %s\n",
                 long(ibwatch::SinceArmMs()), q * 4, sv, vptr, nm);
    return;
  }
}

// ---------------------------------------------------------------------------
// M3.280 (RESTUFF_STATELOG=1): give the NotSignedInPrompt hooks a POSITIVE
// CONTROL, and hook the one unique slot M3.273 missed.
//
// M3.273 above already hooks slots 2 and 3, on the correct reasoning that they
// are unique image-wide. What it does NOT have is any way to tell its own
// silence apart from a broken instrument. It calls "on a healthy boot they must
// never fire" the control -- but that is a NEGATIVE control: it is satisfied
// just as well by a hook that can never fire at all. Every probe in this
// investigation has produced silence, and silence has already fooled me twice
// (the XAM import-name hooks intercepted nothing while I read their zeros as
// "the game never calls it"). So silence is only evidence when something else
// in the same boot proves the instrument was working.
//
// A full RTTI walk over .?AV*@hazingGameStates@@ (35 classes) gives both pieces:
//   - THREE of NotSignedInPrompt's four slots are unique, not two. Slot 1
//     (827E4160) is unhooked; slot 0 (827ECDB8) is the shared base scalar-
//     deleting destructor that TWO earlier probes wasted themselves on, which
//     is why those only ever fired at teardown.
//   - The other nine start-menu sub-states each have their own unique slot 1,
//     so the boot path itself can be instrumented: logo -> legal -> loading ->
//     menu MUST all fire on any boot that reaches the menu.
//
// Read the result this way:
//   controls fire, SIGNINSTATE silent -> real evidence the state never ran.
//   controls silent                   -> instrument dead; DISCARD the run.
//                                        Do not report it as a clean boot.
// ---------------------------------------------------------------------------
namespace festate {
static void Hit(const char* who, uint32_t lr, unsigned cap) {
  static const bool on = getenv("RESTUFF_STATELOG") != nullptr;
  if (!on) return;
  // one counter per call site; these are per-frame Update()s, not one-shots
  static std::atomic<uint32_t> seen[8]{};
  const size_t slot = size_t(who[0] + who[1] * 31) & 7u;
  const uint32_t k = seen[slot].fetch_add(1, std::memory_order_relaxed);
  if (k >= cap) return;
  xamlog::Line("FEHOOK t=%ldms %s #%u lr=%08X\n",
               long(ibwatch::SinceArmMs()), who, k, lr);
}
}  // namespace festate

// --- the target: unique to NotSignedInPrompt --------------------------------
REX_EXTERN(__imp__sub_827E4160);
REX_HOOK_RAW(sub_827E4160) {
  festate::Hit("SIGNINSTATE.s1", uint32_t(ctx.lr), 40);
  __imp__sub_827E4160(ctx, base);
}
// (slots 2 and 3 are ALREADY hooked by M3.273 above as NOTSIGNEDIN#n -- do not
// hook them again here; a second REX_HOOK_RAW on the same guest function does
// not link. Their gate restuff_signin_evidence() is default-on, so those hooks
// were genuinely live on every boot and their silence is real, not self-
// inflicted -- which is what makes the controls below the only thing missing.)

// --- positive controls: the normal boot path, one unique slot-1 each --------
REX_EXTERN(__imp__sub_827D5248);
REX_HOOK_RAW(sub_827D5248) {          // A2MLogoState
  festate::Hit("CTRL.logo", uint32_t(ctx.lr), 2);
  __imp__sub_827D5248(ctx, base);
}
REX_EXTERN(__imp__sub_827E6EB0);
REX_HOOK_RAW(sub_827E6EB0) {          // A2MLegalScreen
  festate::Hit("CTRL.legal", uint32_t(ctx.lr), 2);
  __imp__sub_827E6EB0(ctx, base);
}
REX_EXTERN(__imp__sub_827CA208);
REX_HOOK_RAW(sub_827CA208) {          // LoadingState
  festate::Hit("CTRL.loading", uint32_t(ctx.lr), 2);
  __imp__sub_827CA208(ctx, base);
}
REX_EXTERN(__imp__sub_827D54C8);
REX_HOOK_RAW(sub_827D54C8) {          // MenuState
  festate::Hit("CTRL.menu", uint32_t(ctx.lr), 2);
  __imp__sub_827D54C8(ctx, base);
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cwctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/graphics/graphics_system.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/function.h>
#include <rex/runtime.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/keybinds.h>

#include "video_player.h"

REXCVAR_DEFINE_INT32(fps_cap, 60, "Performance", "Software frame rate cap. Values: 20, 30, or 60.")
    .range(20, 60)
    .validator([](std::string_view v) {
        return v == "20" || v == "30" || v == "60";
    });

REXCVAR_DEFINE_BOOL(wireframe, false, "Cheats", "Force wireframe rendering on all geometry.");

// --- Health regeneration --------------------------------------------------
// Continuously regenerates the player's health to balance the later levels
// (the sequel has regen). Rate is a fraction of MAX HP per second, so the heal
// amount scales with the costume's Life stat (Life drives max HP).
REXCVAR_DEFINE_BOOL(health_regen, true, "Gameplay",
    "Enable continuous player health regeneration.");
REXCVAR_DEFINE_DOUBLE(health_regen_rate, 0.0025, "Gameplay",
    "Regen per second as a fraction of max HP (0.0025 = slow trickle).");
REXCVAR_DEFINE_BOOL(health_regen_debug, false, "Gameplay",
    "Log player hp/max ~once per second (for tuning).");
REXCVAR_DEFINE_BOOL(health_regen_hud, true, "Gameplay",
    "Heal through the game's damage pipeline so the on-screen health bar "
    "updates. Disable for a silent field write if it misbehaves.");

// --- Difficulty -------------------------------------------------------------
// 0=Nice 1=Normal 2=Naughty 3=Nutter. Scales damage amounts in the component
// apply hook (on_apply_damage). Chosen from the F8 panel (difficulty_overlay.h),
// which auto-opens when a new save profile is created (watch_new_profile);
// persisted to the restuff.toml next to the exe so it survives restarts.
REXCVAR_DEFINE_INT32(difficulty, 1, "Gameplay",
    "Difficulty: 0=Nice 1=Normal 2=Naughty 3=Nutter.")
    .range(0, 3);
REXCVAR_DEFINE_BOOL(difficulty_enabled, true, "Gameplay",
    "Enable the difficulty system (F8 panel, auto-show on new profile, damage "
    "and timer scaling). Disable for a fully vanilla experience.");
REXCVAR_DEFINE_BOOL(difficulty_debug, false, "Gameplay",
    "Log the first difficulty damage rescales (for verifying the hook).");

// --- Native score row in the objective panel ---------------------------------
// Shows the live level score and next medal target as a row of the game's own
// felt objective panel (the objective list top-left in a level), the same way
// the difficulty picker rides the game's native UserPrompt. Injection happens
// through the panel's GFx invoke wrappers; see update_score_objective below.
REXCVAR_DEFINE_BOOL(score_objective, true, "Gameplay",
    "Show the live score and next medal target as a native row in the in-level "
    "objective list. Toggle with a double-tap of F10 (single tap = ImGui "
    "trophy counter) or from the F4 settings menu.");
REXCVAR_DEFINE_BOOL(score_objective_debug, false, "Gameplay",
    "Log objective-box bookkeeping for the native score row.");

// Dev diagnostic: hunting for the hazingCharacter::CharacterModelAttributes
// sub-object on a captured damageable's owning component, so bears can be
// forced into eFEARSTATE_INSANE via ICharacterModelAttributes::GetFearStatePtr
// (vtable+0x58, confirmed via disasm: r3=sret buf, r4=this). Static analysis
// dead-ended (the Lua bindings for this interface are never called by any
// shipped script, and the getter returns a refcounted wrapper, not a raw
// field pointer) -- this dumps live memory around each newly-seen character
// (player + bears, via the existing on_read_hp capture) so a human can spot
// a second vtable-like pointer (multi-inheritance sub-object) by eye.
REXCVAR_DEFINE_BOOL(fear_debug, false, "Gameplay",
    "Dev diagnostic: log a memory dump around each newly-seen character "
    "(player/bears) hunting for the ICharacterModelAttributes vtable.");

// Dump every loaded Lua chunk's bytecode to lua_dump/<path> (basis: lua_mods
// branch). Use to extract the per-level medal score targets.
REXCVAR_DEFINE_BOOL(lua_dump_originals, false, "Modding",
    "Dump every loaded Lua chunk's original bytecode to lua_dump/<path>.");
REXCVAR_DEFINE_BOOL(unlock_all, false, "Cheats",
    "Unlock all costumes/content (calls UnlockAllUnlockables continuously).");

// --- Free camera -------------------------------------------------------------
// Detaches hg::Camera from the follow system and flies it directly with WASD
// + mouse look. See update_freecam (bearcam section area) for the mechanism.
REXCVAR_DEFINE_BOOL(freecam, false, "Gameplay",
    "Free camera: detach the camera from the follow system and fly it with "
    "WASD (+ Space/Ctrl up-down, Shift = fast) and mouse look. Toggle: F6.");
REXCVAR_DEFINE_DOUBLE(freecam_speed, 8.0, "Gameplay",
    "Free camera fly speed in world units/sec (Shift multiplies by 3).");
REXCVAR_DEFINE_DOUBLE(freecam_sensitivity, 0.0025, "Gameplay",
    "Free camera mouse-look sensitivity (radians/pixel).");

// --- Title/attract sky recolor ----------------------------------------------
// The felt menus set a per-screen background color via a "SetBackgroundColor"
// GAS tag (Execute = sub_82CD17B0). The title/attract screen uses a yellow
// background; the main menu uses blue (which is why the sky "turns blue on
// start"). on_set_bg_color rewrites the yellow tag to a tunable blue.
REXCVAR_DEFINE_BOOL(sky_recolor, true, "Modding",
    "Recolor the yellow title/attract background to blue (SetBackgroundColor tag).");
REXCVAR_DEFINE_BOOL(sky_recolor_debug, true, "Modding",
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

// --- Randomizer --------------------------------------------------------------
// Seeded randomizer. Layer 1 (stat/chaos, here) randomizes each character's
// damage-taken -- i.e. its effective toughness -- by a per-entity multiplier
// derived from the seed, so every bear plays differently each run. The shared
// core (rando_seed + rando_hash, defined near on_apply_damage) is what the
// planned spawn and weapon layers will draw their randomness from.
REXCVAR_DEFINE_BOOL(randomizer, false, "Randomizer",
    "Master switch for the seeded randomizer.");
REXCVAR_DEFINE_INT32(randomizer_seed, 0, "Randomizer",
    "Randomizer seed. 0 = pick a fresh random seed at launch; any other value "
    "is a fixed, shareable seed.").range(0, 2147483647);
REXCVAR_DEFINE_BOOL(randomizer_damage, true, "Randomizer",
    "Randomize each character's damage-taken (i.e. effective health), per entity.");
REXCVAR_DEFINE_DOUBLE(randomizer_damage_range, 4.0, "Randomizer",
    "Damage-taken spread: multiplier is log-uniform in [1/x, x]. 1 = off, "
    "4 = tough bears take 1/4, fragile ones take 4x.").range(1.0, 16.0);
REXCVAR_DEFINE_BOOL(randomizer_spawns, true, "Randomizer",
    "Master switch for spawn shuffling (per-category toggles below still apply).");
REXCVAR_DEFINE_BOOL(randomizer_enemies, true, "Randomizer",
    "Shuffle which enemy type spawns at each spot (bears -> other bears).");
REXCVAR_DEFINE_BOOL(randomizer_weapons, false, "Randomizer",
    "Shuffle which weapon spawns at each spot. Off by default: it also affects "
    "fixed weapon-source props, which can look odd.");
REXCVAR_DEFINE_BOOL(randomizer_pickups, true, "Randomizer",
    "Shuffle which pickup spawns at each spot (health <-> power pill, etc.).");
REXCVAR_DEFINE_BOOL(randomizer_props, false, "Randomizer",
    "Shuffle smashable furniture/appliance props among each other (a TV where a "
    "toilet was, etc.). Off by default; only swaps props a level actually loads.");
REXCVAR_DEFINE_INT32(randomizer_enemy_probe, 0, "Randomizer",
    "Asset-residency test: 0 = off; 1 = force every enemy spawn to armybear. Use "
    "to check whether a non-native bear type can spawn in a level.").range(0, 1);
REXCVAR_DEFINE_BOOL(randomizer_forceload, false, "Randomizer",
    "Force-load the full bear roster into every level that has bears, so enemy "
    "shuffle can pull ANY bear type (zombies, aliens, etc.) anywhere -- not just "
    "the skins a level ships. Streams extra assets; leave off if a level hitches.");
REXCVAR_DEFINE_BOOL(randomizer_debug, false, "Randomizer",
    "Log randomizer seed and rolls.");

// Defined below on_swap; called once per presented frame.
static void update_health_regen(double dt);
static void update_level_score();
static void update_score_objective();
static void update_bearcam();
static void maybe_unlock_all();
static void watch_new_profile();
static void update_native_difficulty_prompt();
static void update_freecam();
static void update_randomizer();
static void update_attract(double dt);
// Defined in the difficulty section; called from on_lua_load.
static void patch_payphone_duration(rex::memory::Memory* mem, uint32_t data);

// Set Windows timer resolution to 1ms for the lifetime of the process.
// Default is 15.6ms which causes sleep_until to overshoot badly.
static const bool s_timer_res_set = []() { timeBeginPeriod(1); return true; }();

static double s_fps_display = 0.0;
static int    s_frame_count = 0;
static auto   s_last_time   = std::chrono::high_resolution_clock::now();

// Hooked at 0x82F33FF4 (bl VdSwap) — fires once per presented frame.
// Runs the software frame limiter then updates the FPS counter.
void on_swap() {
    using clock    = std::chrono::high_resolution_clock;
    using duration = std::chrono::duration<double>;

    // --- Software frame limiter ---
    static auto last_swap = clock::now();
    const double target_interval = 1.0 / static_cast<double>(REXCVAR_GET(fps_cap));
    const auto deadline = last_swap + duration(target_interval);

    // Coarse sleep until ~2ms before deadline, then spin for accuracy.
    auto now = clock::now();
    const auto sleep_until = deadline - std::chrono::milliseconds(2);
    if (now < sleep_until)
        std::this_thread::sleep_until(sleep_until);
    while (clock::now() < deadline) {}

    last_swap = clock::now();

    // --- Wireframe override (register file) ---
    // Belt-and-suspenders: also patch the register file so any draw that
    // reads the cached register outside of the ring-buffer path is correct.
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
    }

    // Cache the live level score for the trophy overlay.
    update_level_score();

    // Keep the native score row in the objective panel current.
    update_score_objective();

    // Bear-action cutaway: cancel a live PIP event after leaving native mode.
    update_bearcam();

    // One-shot "unlock everything" when requested via the unlock_all cvar.
    maybe_unlock_all();

    // Auto-open the difficulty panel when a new save profile is created.
    watch_new_profile();

    // Open/track the native (game-dialog) difficulty prompt when requested.
    update_native_difficulty_prompt();

    // Free camera: fly the render camera with WASD + mouse when active.
    update_freecam();

    // Randomizer: resolve/announce the active seed once in-level (debug only).
    update_randomizer();

    // Attract mode: after enough idle time on the front-end, play the cinematic.
    {
        static auto attract_last = clock::now();
        const auto  attract_now  = clock::now();
        double dt = duration(attract_now - attract_last).count();
        attract_last = attract_now;
        if (dt > 0.25) dt = 0.25;  // clamp across pauses / level loads
        update_attract(dt);
    }
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

    MEMORY_BASIC_INFORMATION mbi;
    bool ok = false;
    if (VirtualQuery(reinterpret_cast<void*>(h), &mbi, sizeof(mbi)) != 0) {
        ok = (mbi.State == MEM_COMMIT) && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    }
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
// damageable can't be found by scanning the player object; instead on_apply_damage
// captures it when the player is hit (see below).
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

// Primary vtable of hazingDamage::CharacterDamageableComponent, captured from
// the player's component in on_read_hp. Only characters (player + bears) use
// that class -- props/destructibles are plain damage::DamageableComponent -- so
// it lets on_apply_damage scale bears without touching props.
std::atomic<uint32_t> g_char_vtbl{0};

// Set when a new save profile is created this session (watch_new_profile):
// cues the difficulty panel to auto-open.
std::atomic<bool> g_difficulty_autoshow{false};



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
inline void wr_u32(uint8_t* base, uint32_t addr, uint32_t v) {
    const uint32_t off = (addr >= 0xE0000000u) ? 0x1000u : 0u;
    *reinterpret_cast<volatile uint32_t*>(base + addr + off) = __builtin_bswap32(v);
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

// Hooked at DamageableComponent::GetRemainingHitPoints entry (0x82A22348). r3 is
// the IDamageable interface (component + 32). This is read constantly, so it lets
// us capture the player's damageable -- identified by the component holding a
// pointer to the local player's game object. Once captured this is ~free (early
// out); the actual regen runs in update_health_regen.
// fear_debug diagnostic: logs a memory dump around a newly-seen character's
// DamageableComponent (comp = idmg - 32) at most once per distinct idmg,
// capped at a handful of characters total (player + a few bears). Words that
// fall inside the code segment are flagged '*' as vtable-pointer candidates --
// a hazingCharacter::CharacterModelAttributes sub-object (multi-inheritance)
// would show up this way if it's laid out inline on the same allocation as
// the damageable component.
static void dump_fear_debug(uint8_t* base, uint32_t idmg, uint32_t comp) {
    static std::mutex m;
    static std::unordered_set<uint32_t> seen;
    static int count = 0;
    std::lock_guard<std::mutex> lock(m);
    if (count >= 8) return;
    if (!seen.insert(idmg).second) return;
    ++count;

    bool ok = false;
    const uint32_t vt0 = rd32_safe(base, comp, ok);
    REXLOG_INFO("[fear] #{} idmg={:08X} comp={:08X} vtbl0={:08X}", count, idmg, comp,
                ok ? vt0 : 0);

    // Real vtables live BELOW the code segment (data/rodata), not inside it --
    // confirmed by the two known-good vtable pointers (0x8222CD0C, 0x8222CD54)
    // both sitting under 0x82430000, while two earlier "candidates" that
    // landed INSIDE 0x82430000-0x832346C4 turned out to be addresses in the
    // middle of unrelated functions (false positives). 'V' below means "this
    // word plausibly IS a vtable pointer", not "this is code".
    constexpr uint32_t kDumpSize = 0x300;
    auto is_vtbl_range = [](uint32_t w) { return w >= 0x82000000u && w < 0x82430000u; };

    for (uint32_t row = 0; row < kDumpSize; row += 32) {
        char line[200];
        int o = std::snprintf(line, sizeof(line), "[fear]   +%03X:", row);
        for (uint32_t off = row; off < row + 32 && off < kDumpSize; off += 4) {
            const uint32_t w = rd32_safe(base, comp + off, ok);
            const char tag = (ok && is_vtbl_range(w)) ? 'V' : ' ';
            o += std::snprintf(line + o, sizeof(line) - static_cast<size_t>(o), " %08X%c",
                                ok ? w : 0, tag);
        }
        REXLOG_INFO("{}", line);
    }

    // Two-hop scan: for every field that looks like a live, mapped heap
    // pointer, follow it once and check whether ITS first word lands in the
    // vtable range. Catches ICharacterModelAttributes if it's a separately
    // heap-allocated object reached via a plain pointer field rather than
    // inline multi-inheritance on this same component.
    for (uint32_t off = 0; off < kDumpSize; off += 4) {
        const uint32_t w = rd32_safe(base, comp + off, ok);
        if (!ok || !ptr_ok(w) || !host_readable(base, w)) continue;
        bool ok2 = false;
        const uint32_t w2 = rd32_safe(base, w, ok2);
        if (ok2 && is_vtbl_range(w2)) {
            REXLOG_INFO("[fear]   2-hop: comp+{:03X}={:08X} -> [ptr]={:08X} (vtbl-range!)",
                        off, w, w2);
        }
    }
}

void on_read_hp(PPCRegister& r3) {
    const bool have_player = g_player_idmg.load(std::memory_order_relaxed) != 0;
    const bool want_debug = REXCVAR_GET(fear_debug);
    if (have_player && !want_debug) return;  // nothing left to do

    // Fires very often; only do the owner search occasionally until captured
    // (the fear_debug dump is separately deduped per idmg, so it's unaffected).
    static std::atomic<uint32_t> ctr{0};
    if (!have_player && (ctr.fetch_add(1, std::memory_order_relaxed) & 0x1Fu) != 0) return;

    const uint32_t idmg = r3.u32;
    if (!ptr_ok(idmg)) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    bool ok = false;
    if (rd32_safe(base, idmg, ok) != kDamageableVtable || !ok) return;  // not a damageable iface

    const uint32_t comp = idmg - 32;

    if (want_debug) dump_fear_debug(base, idmg, comp);
    if (have_player) return;

    const uint32_t player_obj = get_player_object(base);
    if (!player_obj) return;

    // Only the player's damageable references the local player's game object.
    for (uint32_t off = 0; off < 0x400; off += 4) {
        if (rd32_safe(base, comp + off, ok) == player_obj && ok) {
            g_player_idmg.store(idmg, std::memory_order_relaxed);
            // The player's component is a hazingDamage::CharacterDamageableComponent;
            // its primary vtable identifies every character for difficulty scaling.
            const uint32_t vt = rd32_safe(base, comp, ok);
            if (ok && ptr_ok(vt)) g_char_vtbl.store(vt, std::memory_order_relaxed);
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

    // Difficulty: rescale the payphone call-for-help duration in the chunk's
    // constant pool before Lua parses it (see patch_payphone_duration).
    if (key.size() >= 17 &&
        key.compare(key.size() - 17, 17, "payphone_call.lua") == 0)
        patch_payphone_duration(mem, data);

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

// ---------------------------------------------------------------------------
// Difficulty
// ---------------------------------------------------------------------------
// on_apply_damage hooks the damage apply method (primary-vtable+0x64 =
// sub_8284CE68) shared by every damage::DamageableComponent. r3 = component,
// r4 = DamageData; the amount is a float at DamageData+16, which the function
// itself later rescales in place for costume modifiers -- so rewriting it at
// entry is safe and flows through the whole pipeline (HUD bar, death, msgs).
// Positive amounts are damage, negative are heals (incl. our health regen);
// heals are never scaled.

namespace {

// {player damage taken, character/bear damage taken, escape/call-for-help
// timer duration} multipliers per level. escape_time scales the window the
// player has to interrupt a fleeing/phoning bear (longer = easier).
struct DiffTuning { float player_taken; float enemy_taken; float escape_time; };
constexpr DiffTuning kDiffTuning[4] = {
    {0.5f,  1.5f,  1.5f},   // 0 Nice    -- take half damage, bears go down faster
    {1.0f,  1.0f,  1.0f},   // 1 Normal  -- vanilla
    {1.5f,  0.75f, 0.8f},   // 2 Naughty
    {2.5f,  0.5f,  0.6f},   // 3 Nutter
};
constexpr const char* kDiffNames[4] = {"Nice", "Normal", "Naughty", "Nutter"};

}  // namespace

int get_difficulty() {
    // Disabled = behave exactly like Normal everywhere (damage, timers, and
    // the payphone patch restoring the vanilla 22s on cached chunks).
    if (!REXCVAR_GET(difficulty_enabled)) return 1;
    const int d = REXCVAR_GET(difficulty);
    return (d < 0 || d > 3) ? 1 : d;
}

bool get_difficulty_feature_enabled() { return REXCVAR_GET(difficulty_enabled); }

const char* get_difficulty_name(int level) {
    return (level >= 0 && level <= 3) ? kDiffNames[level] : "?";
}

// Rewrite (or append) the `difficulty = N` line in the restuff.toml staged
// next to the exe; the runtime reads it back into the cvar at boot.
static void persist_difficulty(int level) {
    const std::filesystem::path path =
        rex::filesystem::GetExecutableFolder() / "restuff.toml";
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string l;
        while (std::getline(in, l)) lines.push_back(l);
    }
    const std::string entry = "difficulty = " + std::to_string(level);
    bool replaced = false;
    for (auto& l : lines) {
        const size_t p = l.find_first_not_of(" \t");
        if (p == std::string::npos || l.compare(p, 10, "difficulty") != 0) continue;
        const size_t q = l.find_first_not_of(" \t", p + 10);
        if (q != std::string::npos && l[q] == '=') {  // not difficulty_debug etc.
            l = entry;
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        lines.push_back("");
        lines.push_back("# Difficulty: 0=Nice 1=Normal 2=Naughty 3=Nutter (F8 panel).");
        lines.push_back(entry);
    }
    std::ofstream out(path, std::ios::trunc);
    for (const auto& l : lines) out << l << '\n';
}

void set_difficulty(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    REXCVAR_SET(difficulty, level);
    persist_difficulty(level);
    REXLOG_INFO("[difficulty] set to {} ({})", level, kDiffNames[level]);
}

bool consume_difficulty_autoshow() {
    return g_difficulty_autoshow.exchange(false, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Native difficulty prompt — the game's own stitched UserPrompt dialog.
// ---------------------------------------------------------------------------
// Drives hazingGameStates' UserPrompt exactly like globalmenu.lua's
// ShowUserPrompt does, but from C++ via guest calls (no Lua execution).
// Guest natives (Default.xex; resolved from the SWIG wrappers):
//   GameStateUtil::GetGSUserPrompt()                       = 0x827CB440 -> GSUserPrompt*
//   GSUserPrompt::PushUserPrompt(gs)                       = 0x827EABE0 -> UserPromptInfo*
//   UserPromptInfo::SetUserPrompt(info,msg,cb,acc,can,b)   = 0x827D2AB8
//   UserPromptInfo::AddUserPromptChoice(info,text,idx1..n) = 0x827E32B8
//   UserPromptInfo::SetUserPromptInputVisibility(info,b,b) = 0x827CB5F8
//   GameStateUtil::PushUserPrompt()  [static: opens state] = 0x827CDEE0
//   GSUserPrompt::EnableUserPromptInput(gs,bool)           = 0x827CCD10
//   GSUserPrompt::GetCurrentUserPrompt(gs)                 = 0x827CF3E8 -> info|0
//   UserPromptInfo selected/default choice                 = u32 at info+112
//     (SetDefaultChoice is inlined in its wrapper as *(info+112)=idx)
// Reply routing: the engine dispatches to the Lua global named in
// SetUserPrompt arg2 ("OnGlobalUserPromptReply", stock), which on accept calls
// the global named by arg3 — we pass "HideUserPrompt" (stock: pops the prompt)
// and "" for cancel (B does nothing). We poll info+112 while the prompt is up
// and apply the last selection when it closes.

REXCVAR_DEFINE_BOOL(difficulty_native_prompt, true, "Gameplay",
    "Show the difficulty picker as the game's own stitched dialog instead of "
    "the ImGui panel.");

bool get_difficulty_native_prompt() { return REXCVAR_GET(difficulty_native_prompt); }

static std::atomic<bool> g_native_prompt_request{false};

void request_native_difficulty_prompt() {
    g_native_prompt_request.store(true, std::memory_order_relaxed);
}

// Guest-resident NUL-terminated copy of s (cached; strings live forever).
static uint32_t guest_cstring(const char* s) {
    static std::mutex m;
    static std::unordered_map<std::string, uint32_t> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return 0;
    const size_t len = std::strlen(s) + 1;
    const uint32_t addr = mem->SystemHeapAlloc(static_cast<uint32_t>(len), 0x20);
    if (addr) std::memcpy(mem->TranslateVirtual<char*>(addr), s, len);
    cache.emplace(s, addr);
    return addr;
}

static void update_native_difficulty_prompt() {
    static uint32_t open_info = 0;  // our UserPromptInfo while the prompt is up
    static int last_sel = 0;        // 1-based choice index last seen selected
    static int defer_frames = 0;    // frames spent waiting for the prompt slot

    const bool requested = g_native_prompt_request.exchange(false);
    if (!requested && !open_info) return;

    auto* fd = rex::Runtime::instance()->function_dispatcher();
    auto* mem = rex::system::kernel_memory();
    if (!fd || !mem) return;
    uint8_t* base = mem->virtual_membase();
    auto* fn_get_gs = fd->GetFunction(0x827CB440u);
    auto* fn_cur = fd->GetFunction(0x827CF3E8u);
    if (!base || !fn_get_gs || !fn_cur) return;

    const uint32_t gs = rex::ppc::GuestToHostFunction<uint32_t>(*fn_get_gs);
    if (!ptr_ok(gs)) return;
    const uint32_t cur = rex::ppc::GuestToHostFunction<uint32_t>(*fn_cur, gs);

    if (requested && !open_info) {
        if (cur != 0) {
            // Another prompt is up (on new-profile creation the save flow's own
            // prompts are still open when the autoshow fires). DEFER instead of
            // dropping: re-arm the request and try again next frame, so the
            // difficulty dialog appears the moment the game's prompts close.
            // Cap the wait so a stale request can't linger forever.
            if (++defer_frames < 1800) {  // ~30s at 60fps
                g_native_prompt_request.store(true, std::memory_order_relaxed);
            } else {
                defer_frames = 0;
                if (REXCVAR_GET(difficulty_debug))
                    REXLOG_INFO("[difficulty] native prompt request expired (prompt busy)");
            }
            return;
        }
        defer_frames = 0;
        auto* fn_push   = fd->GetFunction(0x827EABE0u);
        auto* fn_set    = fd->GetFunction(0x827D2AB8u);
        auto* fn_choice = fd->GetFunction(0x827E32B8u);
        auto* fn_vis    = fd->GetFunction(0x827CB5F8u);
        auto* fn_state  = fd->GetFunction(0x827CDEE0u);
        auto* fn_input  = fd->GetFunction(0x827CCD10u);
        if (!fn_push || !fn_set || !fn_choice || !fn_vis || !fn_state || !fn_input)
            return;
        const uint32_t msg = guest_cstring("Select difficulty");
        const uint32_t cb  = guest_cstring("OnGlobalUserPromptReply");
        const uint32_t acc = guest_cstring("HideUserPrompt");
        const uint32_t can = guest_cstring("");
        if (!msg || !cb || !acc || !can) return;

        const uint32_t info = rex::ppc::GuestToHostFunction<uint32_t>(*fn_push, gs);
        if (!ptr_ok(info)) return;
        rex::ppc::GuestToHostFunction<void>(*fn_set, info, msg, cb, acc, can, 1);
        for (int i = 0; i < 4; ++i)
            rex::ppc::GuestToHostFunction<void>(*fn_choice, info,
                                                guest_cstring(kDiffNames[i]),
                                                static_cast<uint32_t>(i + 1));
        last_sel = get_difficulty() + 1;
        *reinterpret_cast<uint32_t*>(base + info + 112) =
            __builtin_bswap32(static_cast<uint32_t>(last_sel));
        // Mirrors ShowUserPrompt: (cancel_name == "", accept_name == "").
        rex::ppc::GuestToHostFunction<void>(*fn_vis, info, 1, 0);
        rex::ppc::GuestToHostFunction<void>(*fn_state);
        rex::ppc::GuestToHostFunction<void>(*fn_input, gs, 1);
        open_info = info;
        if (REXCVAR_GET(difficulty_debug))
            REXLOG_INFO("[difficulty] native prompt opened: info={:08X}", info);
        return;
    }

    if (open_info) {
        if (cur == open_info) {
            const uint32_t sel = rd32(base, open_info + 112);
            if (sel >= 1 && sel <= 4 && static_cast<int>(sel) != last_sel) {
                last_sel = static_cast<int>(sel);
                if (REXCVAR_GET(difficulty_debug))
                    REXLOG_INFO("[difficulty] native prompt selection -> {}", sel);
            }
        } else {
            // Closed (accept ran the stock HideUserPrompt) — apply the choice.
            open_info = 0;
            if (REXCVAR_GET(difficulty_debug))
                REXLOG_INFO("[difficulty] native prompt closed, sel={}", last_sel);
            set_difficulty(last_sel - 1);
        }
    }
}

// --- New-profile detection --------------------------------------------------
// The frontend's "create new saved game" flow (startmenu.lua:
// OnSaveGameCreatePromptReplyAccept -> OnNewSavedGameCreatedSuccessfully)
// materializes on the host as a new save container directory:
//   <user_data_root>/<profile XUID>/464F07D8/00000001/NAUGHTYBEARSAVEGAME
// Rather than hooking the save path natively, watch_new_profile (on_swap,
// ~1/sec) snapshots the containers present at boot and cues the difficulty
// panel when a NEW one appears mid-session -- i.e. exactly when the player
// just created a fresh profile.

// Save root captured from RestuffApp::OnConfigurePaths (user_data_root).
static std::filesystem::path g_user_data_root;

void set_user_data_root(const std::filesystem::path& p) { g_user_data_root = p; }

static void watch_new_profile() {
    if (!REXCVAR_GET(difficulty_enabled)) return;

    // New profiles can only be created at the frontend, never mid-level, so
    // skip the filesystem-hitting scan entirely while in gameplay. Without
    // this it was doing directory I/O ~4x/sec for the whole session,
    // including deep into a level where a new profile could never appear.
    if (auto* mem = rex::system::kernel_memory()) {
        if (uint8_t* base = mem->virtual_membase()) {
            if (resolve_player_damageable(base)) return;
        }
    }

    static int tick = 0;
    if (tick++ % 15 != 0) return;  // ~4x/sec, so the panel opens promptly

    static bool primed = false;
    static std::unordered_set<std::wstring> known;

    std::filesystem::path root = g_user_data_root;
    std::error_code ec;
    if (root.empty() || !std::filesystem::exists(root, ec))
        root = rex::filesystem::GetUserFolder() / "restuff";

    bool created = false;
    for (std::filesystem::directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::filesystem::path save =
            it->path() / "464F07D8" / "00000001" / "NAUGHTYBEARSAVEGAME";
        if (!std::filesystem::exists(save, ec)) continue;
        // native() (wide), not string(): the latter converts via the active code
        // page and throws if the user's profile path isn't representable.
        if (known.insert(save.native()).second && primed) created = true;
    }

    if (!primed) {  // first scan = boot snapshot, never triggers
        primed = true;
        return;
    }
    if (created) {
        REXLOG_INFO("[difficulty] new save profile created -> opening panel");
        g_difficulty_autoshow.store(true, std::memory_order_relaxed);
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

// Attract mode: declared early so the pad hook below can dismiss a cinematic.
void note_input_activity();

// True when the 12-byte X_INPUT_GAMEPAD shows nothing pressed: buttons (u16) and
// both triggers zero, sticks inside a wide deadzone (big-endian s16s, so test
// the high byte: 0x00..0x1F / 0xE0..0xFF). The deadzone is deliberately wide so
// a drifting stick doesn't read as input.
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

REX_EXTERN(__imp__sub_830B3EF0);
REX_HOOK_RAW(sub_830B3EF0) {
    const uint32_t state = ctx.r4.u32;
    __imp__sub_830B3EF0(ctx, base);
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

    // ANY pad activity dismisses a cinematic. Done here, on the raw state before
    // it gets blanked, because XamInputGetKeystroke only reports a subset of the
    // buttons -- this way every button/trigger/stick works, not just menu keys.
    if (g_video_active.load(std::memory_order_relaxed) && !pad_idle(pad)) {
        note_input_activity();
    }

    if (mode == kInputUntilRelease && pad_idle(pad)) {
        g_input_suppress.store(kInputPass, std::memory_order_relaxed);
        return;
    }
    std::memset(pad, 0, 12);
}

// Attract mode: any real keystroke resets the idle timer (and cancels playback).
void note_input_activity();
// Attract mode: load-unit streaming = still booting/changing screens.
void note_loadunit_activity(const char* name);

REX_EXTERN(__imp__sub_830B3FC8);
REX_HOOK_RAW(sub_830B3FC8) {
    if (g_input_suppress.load(std::memory_order_relaxed) != kInputPass) {
        // While a cinematic is up input is suppressed, but we still need to SEE
        // the dismissing press. Read the real keystroke for our own use, then
        // hand the game X_ERROR_EMPTY so it can't also act on it (otherwise the
        // button that stops the video toggles the menu underneath).
        if (g_video_active.load(std::memory_order_relaxed)) {
            __imp__sub_830B3FC8(ctx, base);
            if (ctx.r3.u32 == 0) note_input_activity();
        }
        ctx.r3.u64 = 0x10D2;  // X_ERROR_EMPTY
        return;
    }
    __imp__sub_830B3FC8(ctx, base);
    // Result 0 == a keystroke was actually dequeued (X_ERROR_EMPTY otherwise).
    // This is the one path both the pad and the keyboard funnel through, so it
    // doubles as the "player is still here" signal for the attract timer.
    if (ctx.r3.u32 == 0) note_input_activity();
}

// --- Randomizer core ---------------------------------------------------------
// Shared seeded-RNG foundation for every randomizer layer. rando_hash(key,salt)
// is a deterministic 32-bit mix of (seed, key); pass a distinct salt per
// subsystem so independent streams (stats/spawns/weapons) don't correlate.
namespace {
std::atomic<uint32_t> g_rando_seed{0};
std::atomic<int32_t>  g_rando_seed_src{-1};  // the cvar value g_rando_seed came from

uint32_t rando_seed() {
    const int32_t want = REXCVAR_GET(randomizer_seed);
    if (g_rando_seed_src.load(std::memory_order_relaxed) != want) {
        uint32_t s = static_cast<uint32_t>(want);
        if (want == 0) {  // fresh, non-reproducible seed
            s = static_cast<uint32_t>(std::chrono::high_resolution_clock::now()
                                          .time_since_epoch()
                                          .count());
            s ^= 0x9E3779B9u;
            if (!s) s = 1;
        }
        g_rando_seed.store(s, std::memory_order_relaxed);
        g_rando_seed_src.store(want, std::memory_order_relaxed);
        if (REXCVAR_GET(randomizer_debug))
            REXLOG_INFO("[rando] seed = {} (from cvar {})", s, want);
    }
    return g_rando_seed.load(std::memory_order_relaxed);
}

uint32_t rando_hash(uint32_t key, uint32_t salt) {
    uint32_t x = key + salt + rando_seed() + 0x9E3779B9u;  // fixed-point Weyl step
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
// Uniform float in [0, 1) from (key, salt).
float rando_unit(uint32_t key, uint32_t salt) {
    return static_cast<float>(rando_hash(key, salt) >> 8) * (1.0f / 16777216.0f);
}
// Per-entity damage-taken multiplier, log-uniform in [1/r, r] so "half as tough"
// and "twice as tough" are equally likely. r comes from randomizer_damage_range.
float rando_damage_mult(uint32_t comp) {
    const float r = static_cast<float>(REXCVAR_GET(randomizer_damage_range));
    if (r <= 1.0f) return 1.0f;
    return (1.0f / r) * std::pow(r * r, rando_unit(comp, 0xD1CEu));
}
}  // namespace

// Per-frame (on_swap). When the randomizer is on, resolve the seed so
// randomizer_debug logs "[rando] seed = N" the moment you're in-game -- an
// immediate "it's armed" signal without needing to land a hit first. The stat
// effect itself still only applies on actual damage (in on_apply_damage).
static void update_randomizer() {
    if (REXCVAR_GET(randomizer)) (void)rando_seed();
}

// --- Randomizer spawn layer (Layer 2: enemy/weapon/pickup) -------------------
// A level-placed spawner names the object it spawns by CRC32(lowercase(name)),
// where name == the .lu prefab file's basename (e.g. "machete" -> 0xAC32DC3B).
// That hash lives at outer+48 (the high word of the spawner's 64-bit spawn UID)
// and is passed as the GOD arg to the real spawn call. So we shuffle *what* each
// spawner spawns by rewriting outer+48 to another GOD of the same category just
// before the spawn, then restoring it (transient -- respawns re-trigger the hook).
//
// Replacement targets are drawn ONLY from GODs confirmed resident in ordinary
// village levels: the game's GetGameObjectDefinitionByHashedName is a registry
// lookup (no on-demand disk load), so remapping to a non-resident GOD would fail.
// Enemy variety across skins is the open question -- randomizer_enemy_probe forces
// enemy -> armybear so a playthrough tells us whether a non-native bear can spawn.
namespace {
enum RandCat : uint8_t { CAT_NONE = 0, CAT_ENEMY, CAT_WEAPON, CAT_PICKUP, CAT_PROP };
struct GodEntry { uint32_t hash; RandCat cat; const char* name; };
constexpr GodEntry kGodTable[] = {
    {0x29DE930E, CAT_ENEMY,  "normalbear"},
    {0x5FE25CC1, CAT_ENEMY,  "armybear"},
    {0x3A9CA8F3, CAT_ENEMY,  "copbear"},
    {0xBC1641FA, CAT_ENEMY,  "swatbear"},
    {0x7666CCC4, CAT_ENEMY,  "ninjabear"},
    {0xFA08C0F2, CAT_ENEMY,  "piratebear"},
    {0x579E16FB, CAT_ENEMY,  "alienbear"},
    {0xB52947F0, CAT_ENEMY,  "robobear"},
    {0xB50B01EE, CAT_ENEMY,  "zombiebear"},
    {0x56819005, CAT_ENEMY,  "vampirebear"},
    {0x3D1BC88A, CAT_ENEMY,  "dangerbear"},
    {0x983E2751, CAT_ENEMY,  "unibear"},
    {0x9E5103DD, CAT_WEAPON, "baseballbat"},
    {0xF113D492, CAT_WEAPON, "beartrap"},
    {0xCA7AAFBE, CAT_WEAPON, "hatchet"},
    {0xAC32DC3B, CAT_WEAPON, "machete"},
    {0x13CDF72D, CAT_WEAPON, "revolver"},
    {0xC594115C, CAT_WEAPON, "cricketbat"},
    {0x05EC08CD, CAT_WEAPON, "crowbar"},
    {0xDEF6D4F5, CAT_WEAPON, "fellingaxe"},
    {0x321CE065, CAT_WEAPON, "golfclub"},
    {0x28196F7D, CAT_WEAPON, "sledgehammer"},
    {0x4CC53B29, CAT_WEAPON, "uzi"},
    {0x10E2D66A, CAT_WEAPON, "silentpistol"},
    {0xF097A0CB, CAT_WEAPON, "raygun"},
    {0x3546BF77, CAT_WEAPON, "bone"},
    {0xBB861B1F, CAT_WEAPON, "branch"},
    {0x9065FEAB, CAT_PICKUP, "healthpickup"},
    {0x4CF0FF64, CAT_PICKUP, "powerpill"},
    // NOTE: honeycolacooler is a fixed dispenser PROP, not a grab pickup -- kept
    // out of the pickup category on purpose so it's never a source or a target.
    // Decorative / smashable furniture props (shuffle among each other).
    {0xB3EC7A04, CAT_PROP,   "tv"},
    {0x9B3B9D73, CAT_PROP,   "stove"},
    {0x7212AEE2, CAT_PROP,   "toilet"},
    {0x12C52C32, CAT_PROP,   "teapot"},
    {0xBBE50FD9, CAT_PROP,   "teacup"},
    {0x3ED6DE38, CAT_PROP,   "sink"},
    {0x4AF33561, CAT_PROP,   "payphone"},
    {0xF2E94D89, CAT_PROP,   "fridge"},
    {0x5CA5C559, CAT_PROP,   "mixingmachine"},
    {0x1E067DC0, CAT_PROP,   "locker"},
    {0xFDBCAE17, CAT_PROP,   "present"},
    {0x218D64AC, CAT_PROP,   "toychest"},
    {0x7B85DB61, CAT_PROP,   "speaker"},
    {0xD6F4CEF0, CAT_PROP,   "turntable"},
    {0x6378D70E, CAT_PROP,   "jellyplate"},
    {0x6D6372E0, CAT_PROP,   "frozenlambleg"},
    {0x59CF486E, CAT_PROP,   "frog"},
    {0xC4DB8ED3, CAT_PROP,   "bbq"},
    {0xD5776FF1, CAT_PROP,   "bookshelves"},
    {0xF29EBBC8, CAT_PROP,   "arcademachine"},
    {0xE0461B0F, CAT_PROP,   "radio"},
    {0x2C80050E, CAT_PROP,   "wardrobe"},
};
RandCat god_category(uint32_t hash) {
    for (const auto& e : kGodTable) if (e.hash == hash) return e.cat;
    return CAT_NONE;
}
// Full candidate rosters per category. We don't assume which are loaded -- at
// spawn time we filter to the GODs actually resident in the current level (via
// the refcount-neutral registry probe god_resident), because remapping to a
// non-resident GOD makes that spawn silently no-op. The original is always
// resident, so a filtered pool is never empty. This auto-expands variety in
// levels that load more skins/weapons and stays a no-op where they don't.
constexpr uint32_t kWeaponRoster[] = {
    0x9E5103DD, 0xF113D492, 0xCA7AAFBE, 0xAC32DC3B, 0x13CDF72D,  // bat/trap/hatchet/machete/revolver
    0xC594115C, 0x05EC08CD, 0xDEF6D4F5, 0x321CE065, 0x28196F7D,  // cricketbat/crowbar/fellingaxe/golfclub/sledgehammer
    0x4CC53B29, 0x10E2D66A, 0xF097A0CB, 0x3546BF77, 0xBB861B1F,  // uzi/silentpistol/raygun/bone/branch
};
constexpr uint32_t kPickupRoster[] = {
    0x9065FEAB, 0x4CF0FF64,  // healthpickup / powerpill  (cooler prop deliberately excluded)
};
constexpr uint32_t kPropRoster[] = {
    0xB3EC7A04, 0x9B3B9D73, 0x7212AEE2, 0x12C52C32, 0xBBE50FD9,  // tv/stove/toilet/teapot/teacup
    0x3ED6DE38, 0x4AF33561, 0xF2E94D89, 0x5CA5C559, 0x1E067DC0,  // sink/payphone/fridge/mixingmachine/locker
    0xFDBCAE17, 0x218D64AC, 0x7B85DB61, 0xD6F4CEF0, 0x6378D70E,  // present/toychest/speaker/turntable/jellyplate
    0x6D6372E0, 0x59CF486E, 0xC4DB8ED3, 0xD5776FF1, 0xF29EBBC8,  // frozenlambleg/frog/bbq/bookshelves/arcademachine
    0xE0461B0F, 0x2C80050E,                                      // radio/wardrobe
};
constexpr uint32_t kEnemyRoster[] = {
    0x29DE930E, 0x5FE25CC1, 0x3A9CA8F3, 0xBC1641FA,  // normalbear/armybear/copbear/swatbear
    0x7666CCC4, 0xFA08C0F2, 0x579E16FB, 0xB52947F0,  // ninjabear/piratebear/alienbear/robobear
    0xB50B01EE, 0x56819005, 0x3D1BC88A, 0x983E2751,  // zombiebear/vampirebear/dangerbear/unibear
};
// Same order as kEnemyRoster -- the .lu load-unit names, for force-load (below).
constexpr const char* kBearNames[] = {
    "normalbear", "armybear",   "copbear",    "swatbear",
    "ninjabear",  "piratebear", "alienbear",  "robobear",
    "zombiebear", "vampirebear","dangerbear", "unibear",
};
constexpr int kNumBears = sizeof(kBearNames) / sizeof(kBearNames[0]);

// True iff the GOD is currently registered/loaded. GetGameObjectDefinitionByHashedName
// (sub_82986240) is a refcount-neutral registry lookup: a non-zero result == resident,
// a miss returns 0 without touching any refcount.
bool god_resident(uint32_t hash) {
    auto* fd = rex::Runtime::instance()->function_dispatcher();
    if (!fd) return false;
    auto* fn = fd->GetFunction(0x82986240u);
    return fn && rex::ppc::GuestToHostFunction<uint32_t>(*fn, hash) != 0;
}

// Guest-resident copy of a bear name (AddLoadUnit needs a guest char*). Lazily
// allocated from the guest heap and cached; a handful of bytes, never freed.
uint32_t bear_name_guest(uint8_t* base, int i) {
    static std::atomic<uint32_t> cache[kNumBears] = {};
    if (uint32_t a = cache[i].load(std::memory_order_relaxed)) return a;
    auto* fd = rex::Runtime::instance()->function_dispatcher();
    if (!fd) return 0;
    auto* alloc = fd->GetFunction(0x82BA60E0u);  // guest allocator(size)
    if (!alloc) return 0;
    const char* nm = kBearNames[i];
    const uint32_t len = static_cast<uint32_t>(std::strlen(nm));
    const uint32_t g = rex::ppc::GuestToHostFunction<uint32_t>(*alloc, len + 1);
    if (!g || !host_readable(base, g)) return 0;
    const uint32_t off = (g >= 0xE0000000u) ? 0x1000u : 0u;
    for (uint32_t k = 0; k <= len; ++k) base[g + off + k] = static_cast<uint8_t>(nm[k]);
    cache[i].store(g, std::memory_order_relaxed);
    return g;
}

// Deterministic within-category remap of one GOD hash. Keyed on spot_key -- the
// spawner instance address -- so EACH placed spawn point rolls independently and
// the category spreads evenly (no "all machetes -> one type"). The same spawner
// fires that same object every respawn, and it's reproducible per seed given a
// fixed level load order.
uint32_t rando_remap_god(uint32_t hash, uint32_t spot_key) {
    const uint32_t* roster = nullptr; size_t n = 0; uint32_t salt = 0;
    uint32_t roll_key = spot_key;  // weapons/pickups: per fixed spot (reproducible placement)
    const RandCat cat = god_category(hash);
    switch (cat) {
        case CAT_WEAPON:
            if (!REXCVAR_GET(randomizer_weapons)) return hash;
            roster = kWeaponRoster; n = sizeof(kWeaponRoster)/sizeof(uint32_t); salt = 0x5EA5u; break;
        case CAT_PICKUP:
            if (!REXCVAR_GET(randomizer_pickups)) return hash;
            roster = kPickupRoster; n = sizeof(kPickupRoster)/sizeof(uint32_t); salt = 0x91CBu; break;
        case CAT_PROP:
            if (!REXCVAR_GET(randomizer_props)) return hash;
            roster = kPropRoster; n = sizeof(kPropRoster)/sizeof(uint32_t); salt = 0x9D0Fu; break;
        case CAT_ENEMY: {
            if (!REXCVAR_GET(randomizer_enemies)) return hash;
            if (REXCVAR_GET(randomizer_enemy_probe) == 1) return 0x5FE25CC1u;  // test override: force armybear
            // Roll EACH bear independently (not per spawn point) so a wave from one
            // spawner is a mix, not clones -- "truly random". Seed-driven sequence.
            static std::atomic<uint32_t> ecount{1};
            roll_key = ecount.fetch_add(1, std::memory_order_relaxed) * 2654435761u;
            roster = kEnemyRoster; n = sizeof(kEnemyRoster)/sizeof(uint32_t); salt = 0xB3A2u; break;
        }
        default: return hash;  // unknown GOD -> leave alone
    }
    // Keep only resident candidates (crash-safe: the original is always resident).
    uint32_t res[24]; size_t rc = 0;
    for (size_t i = 0; i < n && rc < 24; ++i)
        if (god_resident(roster[i])) res[rc++] = roster[i];
    if (REXCVAR_GET(randomizer_debug) && cat == CAT_ENEMY) {
        static std::atomic<uint32_t> once{0};
        if (once.fetch_add(1, std::memory_order_relaxed) < 8)
            REXLOG_INFO("[rando-enemy] resident bear types = {}/{}", rc, n);
    }
    if (rc < 2) return hash;  // nothing else loaded in this level -> unchanged
    return res[rando_hash(roll_key, salt) % rc];
}
}  // namespace

REX_EXTERN(__imp__sub_82935878);
REX_HOOK_RAW(sub_82935878) {
    const bool active = REXCVAR_GET(randomizer) && REXCVAR_GET(randomizer_spawns);
    const bool dbg    = REXCVAR_GET(randomizer_debug);
    if ((active || dbg) && base && ptr_ok(ctx.r3.u32)) {
        const uint32_t outer = ctx.r3.u32 - 32;
        bool ok = false;
        const uint32_t g48   = rd32_safe(base, outer + 48, ok);
        const uint32_t stype = rd32_safe(base, outer + 56, ok);
        // The player spawn and the two "None"-fallback spawners take a code path
        // that ignores the GOD-hash arg, so only touch normal configured spawns.
        const bool magic = (stype == 1758671386u) ||
                           (stype == static_cast<uint32_t>(-237345888));
        if (active && ok && !magic) {
            const uint32_t rep = rando_remap_god(g48, outer);
            if (rep != g48 && host_readable(base, outer + 48)) {
                wr_u32(base, outer + 48, rep);       // rewrite GOD-hash arg
                if (dbg) REXLOG_INFO("[rando-remap] {:08X} -> {:08X}", g48, rep);
                __imp__sub_82935878(ctx, base);
                wr_u32(base, outer + 48, g48);        // restore (transient)
                return;
            }
        }
        if (dbg) {
            static std::atomic<uint32_t> n{0};
            if (n.fetch_add(1, std::memory_order_relaxed) < 400)
                REXLOG_INFO("[rando-spawn] type={:08X} g48={:08X} cat={}",
                            stype, g48, static_cast<int>(god_category(g48)));
        }
    }
    __imp__sub_82935878(ctx, base);
}

// --- Randomizer force-load: pull the full bear roster into any bear level ------
// A .lu is a streamable "load unit"; gameSpawner requests its GOD units via
// LoadUnitRequester::AddLoadUnit (sub_8293A710). We hook that: whenever the game
// streams in one bear unit, we request the REST of the roster on the SAME
// requester, so every bear type becomes resident and the enemy shuffle can pick
// any of them -- even skins the level never shipped. Streaming is async, so the
// extra bears drift in over a second or two and the residency filter picks them
// up as they land. No effect where no bear unit is loaded (menus/cutscenes), and
// gated behind randomizer_forceload so normal play is untouched.
REX_EXTERN(__imp__sub_8293A710);
REX_HOOK_RAW(sub_8293A710) {
    const uint32_t requester = ctx.r3.u32;
    const uint32_t name_addr = ctx.r4.u32;
    __imp__sub_8293A710(ctx, base);  // perform the game's own add first

    static thread_local bool s_injecting = false;
    if (s_injecting || !base || !ptr_ok(requester)) return;
    // The name is a char* and may be unaligned, so don't ptr_ok it (ptr_ok
    // requires 4-byte alignment). Bounds-check + confirm the page is readable.
    if (name_addr < 0x10000u || name_addr >= 0xC0000000u || !host_readable(base, name_addr)) return;

    // Read the load-unit name, lowercased (the game names units CamelCase, e.g.
    // "NormalBear", but hashes/loads them case-insensitively -- our roster and the
    // .lu files on disk are lowercase, so normalise here to match either way).
    char nm[32]; int L = 0;
    const uint32_t off = (name_addr >= 0xE0000000u) ? 0x1000u : 0u;
    for (; L < 31; ++L) {
        char c = static_cast<char>(base[name_addr + off + L]);
        if (!c) break;
        if (c >= 'A' && c <= 'Z') c += 32;
        nm[L] = c;
    }
    nm[L] = 0;

    // Attract mode: streaming activity means we're still booting / changing
    // screens, so this doubles as the "not settled yet" signal (and tells us
    // when the title screen itself has loaded). Runs regardless of randomizer.
    note_loadunit_activity(nm);

    // Diagnostic: log the real load-unit names so we can see how bears are named.
    if (REXCVAR_GET(randomizer_debug)) {
        static std::atomic<uint32_t> nlog{0};
        if (nlog.fetch_add(1, std::memory_order_relaxed) < 150)
            REXLOG_INFO("[rando-lu] AddLoadUnit '{}'", nm);
    }

    if (!REXCVAR_GET(randomizer) || !REXCVAR_GET(randomizer_forceload)) return;
    int idx = -1;
    for (int i = 0; i < kNumBears; ++i) if (std::strcmp(nm, kBearNames[i]) == 0) { idx = i; break; }
    if (idx < 0) return;  // not a bear unit

    auto* fd = rex::Runtime::instance()->function_dispatcher();
    if (!fd) return;
    auto* addfn = fd->GetFunction(0x8293A710u);
    if (!addfn) return;
    s_injecting = true;  // our AddLoadUnit calls re-enter this hook; guard stops recursion
    int n = 0;
    for (int i = 0; i < kNumBears; ++i) {
        if (i == idx) continue;
        const uint32_t g = bear_name_guest(base, i);
        if (!g) continue;
        rex::ppc::GuestToHostFunction<void>(*addfn, requester, g, 0u);
        ++n;
    }
    s_injecting = false;
    if (REXCVAR_GET(randomizer_debug)) {
        static std::atomic<uint32_t> once{0};
        if (once.fetch_add(1, std::memory_order_relaxed) < 6)
            REXLOG_INFO("[rando-forceload] '{}' loaded -> requested {} more bear units", nm, n);
    }
}

// NOTE (enemy-weapon layer): the melee weapon lives on gameWeapon::CloseCombat-
// Component as an enum at CCC+0x44 and a name std::string at CCC+0x48. The
// obvious lever -- the virtual GetWeaponType getter (vtable slot 6 of the +32
// IWeapon subobject) -- turned out to be a DEAD END twice over: its concrete
// address is shared by many classes via identical-code folding, AND a filtered
// probe showed the CloseCombatComponent vtable never reaches it, i.e. the game
// reads the weapon field directly and only Lua uses the getter. Randomizing
// enemy weapons therefore needs the equip function that writes CCC+0x44/+0x48
// at spawn -- not yet located. See the randomizer memory note before retrying.

// Midasm hook at the damage apply method entry (0x8284CE68). Scales the
// DamageData amount. Two independent effects stack here: (1) difficulty tuning
// (player_taken / enemy_taken), and (2) the randomizer's per-entity toughness.
// Target class is resolved once: the player's captured component, or any
// component sharing the player's concrete character vtable (bears, not props).
void on_apply_damage(PPCRegister& r3, PPCRegister& r4) {
    const int diff = get_difficulty();
    const bool rando = REXCVAR_GET(randomizer) && REXCVAR_GET(randomizer_damage);
    if (diff == 1 && !rando) return;  // Normal difficulty + no randomizer = vanilla

    const uint32_t comp = r3.u32;
    const uint32_t dd   = r4.u32;
    if (!ptr_ok(comp) || !ptr_ok(dd)) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;

    bool ok = false;
    const uint32_t raw = rd32_safe(base, dd + 16, ok);
    if (!ok) return;
    float amount;
    std::memcpy(&amount, &raw, 4);
    // Only scale real damage: negative amounts are heals (incl. health regen).
    if (!(amount > 0.0f) || amount > 1.0e6f) return;

    const uint32_t player = g_player_idmg.load(std::memory_order_relaxed);
    if (!player) return;  // player not captured yet -> can't classify, stay vanilla

    const bool is_player = (comp + 32 == player);
    bool is_char = is_player;
    if (!is_player) {
        const uint32_t vt = rd32_safe(base, comp, ok);
        is_char = ok && vt != 0 && vt == g_char_vtbl.load(std::memory_order_relaxed);
    }
    if (!is_char) return;  // prop/other -> leave alone

    float mul = 1.0f;
    if (diff != 1)
        mul *= is_player ? kDiffTuning[diff].player_taken : kDiffTuning[diff].enemy_taken;
    if (rando)
        mul *= rando_damage_mult(comp);
    if (mul == 1.0f) return;

    wr_f32(base, dd + 16, amount * mul);

    if (REXCVAR_GET(difficulty_debug) || REXCVAR_GET(randomizer_debug)) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 64)
            REXLOG_INFO("[dmg] {} {:.1f} -> {:.1f} (x{:.2f}{})",
                        is_player ? "player" : "bear", amount, amount * mul, mul,
                        rando ? ", rando" : "");
    }
}

// Midasm hook at hazingHud escape-timer start (sub_8270AE88), the native
// behind the script-exposed HudComponent::StartEscapeTimer(duration, type).
// f1 = duration in seconds, r4 = eEscapeType (car/boat/call-police).
// LOG-ONLY: verified in-game that this HUD countdown is display-only -- the
// gameplay outcome runs on the script's own CreateTimer clock. Scaling happens
// at the source instead (patch_payphone_duration rewrites the constant both
// the real timer and this HUD call read), so the two always agree. Scaling
// f1 here too would double-apply.
void on_escape_timer(PPCRegister& f1, PPCRegister& r4) {
    if (REXCVAR_GET(difficulty_debug))
        REXLOG_INFO("[difficulty] escape timer start: type={} {:.1f}s",
                    r4.u32, f1.f64);
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

// Rewrite the payphone call-for-help duration inside payphone_call.lua as it
// loads. The script stores the 22-second duration as ONE integer constant
// (this Lua uses the LNUM patch: constant tag 0xFE + little-endian int64)
// that feeds BOTH the authoritative script timer (CreateTimer(22, __this__,
// "PayPhone_Call_Timer1_End")) and the HUD (StartEscapeTimer(22, ...)), so
// patching it keeps gameplay and display in sync. The pattern occurs exactly
// once in the chunk; a value other than 22 (already patched / changed file)
// simply doesn't match, so reloads can't compound the scaling.
// Car/boat escapes are left vanilla: their pacing is a fixed fumble/cutscene
// animation chain, not a single timer.
static void patch_payphone_duration(rex::memory::Memory* mem, uint32_t data) {
    const int diff = get_difficulty();

    // The game caches the decompressed chunk and reuses the SAME buffer on
    // later loads (observed: double load per level, second already patched).
    // So the constant we're looking for may hold the original 22 OR any value
    // a previous difficulty wrote -- accept the whole candidate set and always
    // run (Normal must restore 22 over a cached scaled value).
    constexpr float kBase = 22.0f;
    auto scaled = [](int d) {
        int64_t v = static_cast<int64_t>(kBase * kDiffTuning[d].escape_time + 0.5f);
        return v < 5 ? int64_t{5} : v;
    };
    const int64_t want = scaled(diff);

    const uint32_t buf  = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(data));
    const uint32_t size = __builtin_bswap32(*mem->TranslateVirtual<const uint32_t*>(data + 4));
    if (!buf || !size || size > (16u << 20)) return;
    uint8_t* p = mem->TranslateVirtual<uint8_t*>(buf);

    for (uint32_t i = 0; i + 9 <= size; ++i) {
        if (p[i] != 0xFE) continue;  // Lua int-constant tag
        int64_t cur = 0;
        for (int b = 7; b >= 0; --b) cur = (cur << 8) | p[i + 1 + b];  // LE int64
        const bool known = (cur == static_cast<int64_t>(kBase)) ||
                           cur == scaled(0) || cur == scaled(1) ||
                           cur == scaled(2) || cur == scaled(3);
        if (!known) continue;
        if (cur != want) {
            for (int b = 0; b < 8; ++b)
                p[i + 1 + b] = static_cast<uint8_t>((want >> (8 * b)) & 0xFF);
            REXLOG_INFO("[difficulty] payphone call duration {}s -> {}s ({})",
                        cur, want, kDiffNames[diff]);
        }
        return;
    }
    REXLOG_WARN("[difficulty] payphone_call.lua: duration constant not found");
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
// Score bonus cheat
// ---------------------------------------------------------------------------
// inject_score_bonus hooks sub_8280F428 (HazingScoreMgr::GetTotalScore) just
// before it returns, after the accumulation loop has finished.  r31 holds the
// HazingScoreMgr* (PPC guest address); offset 0x88 (136) is the cached float
// total that both return paths read.  We add g_score_bonus here so that every
// score query the game makes reflects the cheat amount.

static std::atomic<float> g_score_bonus{0.0f};

// Midasm hook at HazingScoreMgr::GetTotalScore (sub_8280F428) just before it
// returns, after the accumulation loop. r31 = HazingScoreMgr*; also captures
// it into g_score_mgr (see update_level_score) since this fires constantly.
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
            ClipCursor(nullptr);
            ShowCursor(TRUE);
            g_freecam.cursor_captured = false;
        }
        g_freecam.active = want;
        REXLOG_INFO("[freecam] transition: active={}", want);
    }
    if (!want) return;

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
}

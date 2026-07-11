#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
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

// Dump every loaded Lua chunk's bytecode to lua_dump/<path> (basis: lua_mods
// branch). Use to extract the per-level medal score targets.
REXCVAR_DEFINE_BOOL(lua_dump_originals, false, "Modding",
    "Dump every loaded Lua chunk's original bytecode to lua_dump/<path>.");
REXCVAR_DEFINE_BOOL(unlock_all, false, "Cheats",
    "Unlock all costumes/content (calls UnlockAllUnlockables continuously).");

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

// Defined below on_swap; called once per presented frame.
static void update_health_regen(double dt);
static void update_level_score();
static void maybe_unlock_all();
static void watch_new_profile();
static void update_native_difficulty_prompt();
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

    // One-shot "unlock everything" when requested via the unlock_all cvar.
    maybe_unlock_all();

    // Auto-open the difficulty panel when a new save profile is created.
    watch_new_profile();

    // Open/track the native (game-dialog) difficulty prompt when requested.
    update_native_difficulty_prompt();
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
    static int tick = 0;
    if (tick++ % 15 != 0) return;  // ~4x/sec, so the panel opens promptly

    static bool primed = false;
    static std::unordered_set<std::string> known;

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
        if (known.insert(save.string()).second && primed) created = true;
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
        ctx.r3.u64 = 0x10D2;  // X_ERROR_EMPTY
        return;
    }
    __imp__sub_830B3FC8(ctx, base);
}

// Midasm hook at the damage apply method entry (0x8284CE68). Scales the
// DamageData amount by the difficulty tuning: the player's component gets
// player_taken; other components with the player's concrete class vtable
// (i.e. bears, not props) get enemy_taken.
void on_apply_damage(PPCRegister& r3, PPCRegister& r4) {
    const int diff = get_difficulty();
    if (diff == 1) return;  // Normal = vanilla, touch nothing

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

    float mul = 1.0f;
    const char* who = nullptr;
    if (comp + 32 == player) {
        mul = kDiffTuning[diff].player_taken;
        who = "player";
    } else {
        const uint32_t vt = rd32_safe(base, comp, ok);
        if (ok && vt != 0 && vt == g_char_vtbl.load(std::memory_order_relaxed)) {
            mul = kDiffTuning[diff].enemy_taken;
            who = "bear";
        }
    }
    if (!who || mul == 1.0f) return;

    wr_f32(base, dd + 16, amount * mul);

    if (REXCVAR_GET(difficulty_debug)) {
        static std::atomic<uint32_t> n{0};
        if (n.fetch_add(1, std::memory_order_relaxed) < 64)
            REXLOG_INFO("[difficulty] {} dmg {:.1f} -> {:.1f} (x{:.2f}, {})",
                        who, amount, amount * mul, mul, kDiffNames[diff]);
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

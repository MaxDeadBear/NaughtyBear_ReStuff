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

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
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

REXCVAR_DEFINE_BOOL(wireframe, false, "Debug", "Force wireframe rendering on all geometry.");

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

// --- Trophy / medal target tracker ----------------------------------------
// The per-level medal score targets live in the game's Lua data and aren't
// readable from native code, so they're entered here (persisted in restuff.toml)
// and the overlay shows points-to-next against the live level score. 0 = unset.
REXCVAR_DEFINE_INT32(trophy_bronze, 0, "Gameplay", "Bronze medal score target (0 = unset).");
REXCVAR_DEFINE_INT32(trophy_silver, 0, "Gameplay", "Silver medal score target (0 = unset).");
REXCVAR_DEFINE_INT32(trophy_gold,   0, "Gameplay", "Gold medal score target (0 = unset).");
REXCVAR_DEFINE_INT32(trophy_plat,   0, "Gameplay", "Platinum medal score target (0 = unset).");

// Dump every loaded Lua chunk's bytecode to lua_dump/<path> (basis: lua_mods
// branch). Use to extract the per-level medal score targets.
REXCVAR_DEFINE_BOOL(lua_dump_originals, false, "Modding",
    "Dump every loaded Lua chunk's original bytecode to lua_dump/<path>.");

// Defined below on_swap; called once per presented frame.
static void update_health_regen(double dt);
static void update_level_score();

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
    const int autov = g_trophy_auto[tier].load(std::memory_order_relaxed);
    if (autov > 0) return autov;  // captured from the game; falls back to cvar override
    switch (tier) {
        case 0: return REXCVAR_GET(trophy_bronze);
        case 1: return REXCVAR_GET(trophy_silver);
        case 2: return REXCVAR_GET(trophy_gold);
        case 3: return REXCVAR_GET(trophy_plat);
        default: return 0;
    }
}

void set_trophy_target(int tier, int value) {
    if (value < 0) value = 0;
    switch (tier) {
        case 0: REXCVAR_SET(trophy_bronze, value); break;
        case 1: REXCVAR_SET(trophy_silver, value); break;
        case 2: REXCVAR_SET(trophy_gold, value); break;
        case 3: REXCVAR_SET(trophy_plat, value); break;
        default: break;
    }
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
// Lua chunk dumper (basis: lua_mods branch)
// ---------------------------------------------------------------------------
// Hooked at lua_load (0x82BB5010): r5 = reader data {buf,size}, r6 = chunk name.
// When lua_dump_originals is set, write each chunk's bytecode (Xbox-360 Lua 5.1)
// to lua_dump/<path> so we can read the per-level medal score targets.

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

static void dump_lua_original(const std::string& key, uint8_t* base, uint32_t data) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    std::lock_guard<std::mutex> lock(m);
    if (seen[key]++) return;  // once per chunk
    const uint32_t buf  = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + data));
    const uint32_t size = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + data + 4));
    if (!buf || !size || size > (16u << 20)) return;
    const std::filesystem::path out_path = std::filesystem::path("lua_dump") / key;
    std::error_code ec;
    std::filesystem::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path, std::ios::binary);
    if (out) out.write(reinterpret_cast<const char*>(base + buf), size);
}

// Midasm hook at lua_load entry: r5 = reader data {buf,size}, r6 = chunk name.
void on_lua_load(PPCRegister& r5, PPCRegister& r6) {
    if (!REXCVAR_GET(lua_dump_originals)) return;
    const uint32_t data = r5.u32;
    const uint32_t name_addr = r6.u32;
    if (!data || !name_addr) return;
    auto* mem = rex::system::kernel_memory();
    if (!mem) return;
    uint8_t* base = mem->virtual_membase();
    if (!base) return;
    const char* path = reinterpret_cast<const char*>(base + name_addr);
    size_t len = 0;
    while (len < 256 && path[len]) ++len;
    dump_lua_original(lua_mod_key(path, len), base, data);
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

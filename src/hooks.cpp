#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/ppc/types.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_INT32(fps_cap, 60, "Performance", "Software frame rate cap. Values: 20, 30, or 60.")
    .range(20, 60)
    .validator([](std::string_view v) {
        return v == "20" || v == "30" || v == "60";
    });

REXCVAR_DEFINE_BOOL(wireframe, false, "Debug", "Force wireframe rendering on all geometry.");

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
}

double get_fps() { return s_fps_display; }

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

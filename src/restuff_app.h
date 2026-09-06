// restuff - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>

#ifdef __linux__
#include <execinfo.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/time.h>
#include <ucontext.h>
#include <xmmintrin.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#endif

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>

#include "fps_overlay.h"
#include "cheats_overlay.h"
// The difficulty selector was the modding fork's feature and is fully removed
// from this tree (M3.287 disabled it; the code was excised outright later).
// difficulty_overlay.h, the scaling hooks, the payphone timer patch and the
// save-root new-profile watcher are all gone. The fork keeps them; main does not.
#include "trophy_overlay.h"
#include "video_overlay.h"
#include "native_vk.h"

#ifdef __linux__
// M3.40: FP-exception guard. The SDK's fpscr.enableFlushMode() (emitted by
// recompiled code whenever the guest sets flush-to-zero) writes its CACHED
// csr member to MXCSR; that cache only carries the exception-MASK bits if
// InitHost() ran on the context first. The APC-delivery path
// (XThread::DeliverAPCs -> FunctionDispatcher::Execute -> guest fn) runs
// recompiled code on contexts that skip InitHost(), so enableFlushMode there
// writes MXCSR=0x8040 (FTZ+DAZ only) and UNMASKS all six FP exceptions. The
// next SSE op on inf/NaN/overflow then raises SIGFPE (observed: repeated
// crashes in simde_mm_add_ps <- sub_82A39838 via DeliverAPCs). Real Xbox 360
// code, like ~all game code, runs with FP exceptions masked.
//
// Fix the symptom robustly host-side: on a FLOAT SIGFPE, re-set the six
// exception-mask bits in the fault's SAVED MXCSR (must edit the ucontext, not
// live MXCSR -- the kernel restores fpregs on sigreturn) and return; the
// faulting instruction re-executes with exceptions masked and yields the
// default IEEE result. INTEGER div/overflow SIGFPE is a genuine fault
// elsewhere -- chain to default so it can't mask-loop. Opt out:
// RESTUFF_NO_FPE_GUARD=1.
namespace restuff_fpe_guard {
inline std::atomic<uint64_t> g_caught{0};
inline void Handler(int sig, siginfo_t* info, void* ucv) {
  if (info && (info->si_code == FPE_INTDIV || info->si_code == FPE_INTOVF)) {
    signal(SIGFPE, SIG_DFL);  // re-raise as a real crash
    return;
  }
  auto* uc = static_cast<ucontext_t*>(ucv);
  if (uc && uc->uc_mcontext.fpregs) {
    uc->uc_mcontext.fpregs->mxcsr |= 0x1F80u;  // mask IM/DM/ZM/OM/UM/PM
  }
  if (g_caught.fetch_add(1, std::memory_order_relaxed) == 0) {
    const char msg[] = "[FPE-GUARD] caught+masked a float SIGFPE; continuing\n";
    ssize_t n = write(2, msg, sizeof(msg) - 1);
    (void)n;
  }
}
inline void Init() {
  // M3.42: OPT-IN (was default-on in M3.40). The handler catches a float
  // SIGFPE, re-masks, and RETRIES the instruction -- but if the SDK's
  // MXCSR-unmask (enableFlushMode on APC contexts) happens FREQUENTLY (per
  // skinned/animated draw), the per-exception signal-delivery cost tanks fps
  // and starves the audio thread (user: laggy, no music/narrator, few sfx,
  // 3x cutscene load with the guard on). A rare crash is the lesser evil, and
  // the real fix is to stop the unmask at the source, not catch every trap.
  // RESTUFF_FPE_GUARD=1 re-enables the catch-and-continue behaviour.
  if (getenv("RESTUFF_FPE_GUARD")) {
    struct sigaction sa = {};
    sa.sa_sigaction = Handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGFPE, &sa, nullptr);
  }
  // RESTUFF_FPE_SELFTEST=1: deterministically reproduce the guest's failure
  // mode (unmask FP exceptions exactly as the buggy enableFlushMode path does,
  // then do an SSE op that raises FE_INVALID) and confirm the guard converts
  // the would-be SIGFPE crash into a survivable, masked continuation.
  if (getenv("RESTUFF_FPE_SELFTEST")) {
    fprintf(stderr, "[FPE-SELFTEST] unmasking FP exceptions and forcing 0.0/0.0...\n");
    fflush(stderr);
    _mm_setcsr(_mm_getcsr() & ~0x1F80u);  // clear all mask bits (as the bug does)
    volatile float z = 0.0f;
    volatile float r = z / z;  // 0/0 => FE_INVALID => SIGFPE with masks cleared
    (void)r;
    fprintf(stderr, "[FPE-SELFTEST] SURVIVED (guard caught=%llu); result=%f\n",
            (unsigned long long)g_caught.load(), (double)r);
    fflush(stderr);
  }
}
}  // namespace restuff_fpe_guard

// RESTUFF_SELFPROF=<path>: in-process CPU sampler (yama blocks external
// ptrace profilers even with PR_SET_PTRACER_ANY for frame reads). SIGPROF via
// ITIMER_PROF lands on whichever thread is consuming CPU; the handler records
// raw leaf PCs, a flusher aggregates and appends "off=<hex> n=<count>" lines
// (PC minus the main module base) for offline addr2line against ./restuff.
namespace restuff_selfprof {
inline std::atomic<uint32_t> g_pos{0};
inline void* g_pc[65536];
inline uintptr_t g_base = 0;

inline void Handler(int) {
  void* frames[4];
  const int n = backtrace(frames, 4);
  if (n > 2) g_pc[g_pos.fetch_add(1, std::memory_order_relaxed) % 65536] = frames[2];
}

inline void Init() {
  const char* path = getenv("RESTUFF_SELFPROF");
  if (!path) return;
  // main module base from /proc/self/maps (first executable mapping of us)
  if (FILE* f = fopen("/proc/self/maps", "r")) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      if (strstr(line, "/restuff") && strstr(line, " r-xp ")) {
        g_base = strtoull(line, nullptr, 16);
        break;
      }
    }
    fclose(f);
  }
  struct sigaction sa = {};
  sa.sa_handler = Handler;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGPROF, &sa, nullptr);
  itimerval tv{{0, 4000}, {0, 4000}};  // 250Hz across busy threads
  setitimer(ITIMER_PROF, &tv, nullptr);
  std::thread([path] {
    std::string out = path;
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(15));
      std::map<uintptr_t, uint32_t> agg;
      const uint32_t end = std::min<uint32_t>(g_pos.load(std::memory_order_relaxed), 65536);
      for (uint32_t i = 0; i < end; ++i)
        if (g_pc[i]) ++agg[reinterpret_cast<uintptr_t>(g_pc[i])];
      std::multimap<uint32_t, uintptr_t, std::greater<>> top;
      for (auto& [pc, n] : agg) top.emplace(n, pc);
      if (FILE* f = fopen(out.c_str(), "a")) {
        fprintf(f, "=== selfprof window samples=%u base=%zx\n", end, size_t(g_base));
        int k = 0;
        for (auto& [n, pc] : top) {
          fprintf(f, "off=%zx n=%u\n", size_t(pc - g_base), n);
          if (++k >= 40) break;
        }
        fclose(f);
      }
      g_pos.store(0, std::memory_order_relaxed);
      memset(g_pc, 0, sizeof g_pc);
    }
  }).detach();
}
}  // namespace restuff_selfprof
#endif

class RestuffApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<RestuffApp>(new RestuffApp(ctx, "restuff",
        PPCImageConfig));
  }

  // Rendering backend selection. With use_native_renderer (default true), inject
  // our own native Vulkan graphics system (NativeVulkanGraphicsSystem) and clear
  // gpu_plugin -- config.graphics wins because the xenos plugin is only loaded
  // when config.graphics is empty. Set use_native_renderer=false in restuff.toml
  // to fall back to the stock xenos GPU emulation plugin (staged next to the exe
  // via GPU_PLUGINS xenos in CMakeLists.txt).
  void OnPreSetup(rex::RuntimeConfig& config) override {
#ifdef __linux__
    // Allow any same-user process to ptrace us (yama scope-1 otherwise limits
    // attach to ancestors) -- lets the drive harness sample stacks with
    // eu-stack/gdb for guest-hot-function profiling. Debug tooling only.
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    restuff_fpe_guard::Init();
    restuff_selfprof::Init();
#endif
    if (REXCVAR_GET(use_native_renderer)) {
      config.gpu_plugin.clear();
      config.graphics = std::make_unique<restuff::NativeVulkanGraphicsSystem>();
    } else if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // M3.146 (RESTUFF_NO_OVERLAYS=1): adding ANY dialog registers the ImGui
    // drawer as a UI drawer (imgui_drawer.cpp:65-72), and the presenter then
    // chooses kUIThreadOnRequest over kGuestOutputThreadImmediately purely
    // because ui_drawers_ is non-empty (presenter.cpp:1327). That puts
    // vkQueuePresentKHR on the UI THREAD, which holds the SDK's single queue
    // mutex across a blocking X write -- while our present thread starves in
    // AcquireQueue and the swapchain keeps showing the bare clear. That is the
    // captured mechanism behind the blue/black launch failures. This gate
    // exists to A/B it: with no dialogs the drawer never registers and the
    // guest-output thread presents directly.
    if (getenv("RESTUFF_NO_OVERLAYS")) {
      return;
    }
    // NOTE: do NOT call drawer->AddDialog() here. ImGuiDialog's constructor
    // already self-adds (imgui_dialog.cpp:22), so an explicit AddDialog is
    // redundant -- and worse, it UNDOES the ctor-time RemoveDialog each overlay
    // uses to stay unregistered until it is actually shown (M3.147). Adding it
    // back re-pins the presenter to the UI thread and the blue/black boot
    // failures return; that is exactly what a first attempt at this got wrong.
    new FpsOverlayDialog(drawer);
    new CheatsDialog(drawer);
    new TrophyOverlayDialog(drawer);
    // Attract-mode video needs the immediate drawer to blit frames.
    // (No AddDialog here -- see the note above; the ctor self-adds.)
    new VideoOverlayDialog(drawer, immediate_drawer());
  }
  // (DifficultyDialog and its Salsbury font loader were removed with the fork
  // feature -- see the note at the former include site.)

  // Default the game data root to the project's assets folder when
  // --game_data_root isn't passed on the command line. The new SDK leaves
  // paths.game_data_root empty if no CLI arg / cvar supplied it, so without
  // this the exe wouldn't find Default.xex unless launched from a folder that
  // already has an assets/ next to it.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;  // honor an explicit --game_data_root
    }

    // Search for an "assets" folder without hardcoding any machine path.
    // Start from the working directory and the executable's folder, then walk
    // up the parent directories of each so the exe finds assets whether it's
    // launched from the project root, out/build/<preset>, or Visual Studio.
    std::error_code ec;
    for (std::filesystem::path base :
         {std::filesystem::current_path(ec), rex::filesystem::GetExecutableFolder()}) {
      for (; !base.empty(); base = base.parent_path()) {
        std::filesystem::path candidate = base / "assets";
        if (std::filesystem::exists(candidate / "Default.xex")) {
          paths.game_data_root = candidate;
          return;
        }
        if (base == base.parent_path()) {
          break;  // reached the filesystem root
        }
      }
    }
  }
};

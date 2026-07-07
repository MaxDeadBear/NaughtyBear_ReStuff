// restuff - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>

#include "fps_overlay.h"
#include "cheats_overlay.h"
#include "difficulty_overlay.h"
#include "trophy_overlay.h"

class RestuffApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<RestuffApp>(new RestuffApp(ctx, "restuff",
        PPCImageConfig));
  }

  // The new SDK extracted Xenos GPU emulation into a runtime-loaded plugin
  // (rexgpu-xenos). This project renders through that emulated GPU rather than
  // its own renderer, so the plugin must be loaded. config.gpu_plugin is
  // pre-populated from the `gpu_plugin` cvar (CLI / restuff.toml), so only
  // default it here when nothing set it -- the recomp then renders even with no
  // config file present. CMake stages rexgpu-xenos next to the exe (see
  // GPU_PLUGINS xenos in CMakeLists.txt).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    drawer->AddDialog(new FpsOverlayDialog(drawer));
    drawer->AddDialog(new CheatsDialog(drawer));
    drawer->AddDialog(new DifficultyDialog(drawer));
    drawer->AddDialog(new TrophyOverlayDialog(drawer));
  }

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

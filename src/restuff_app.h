// restuff - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include <rex/filesystem.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include "fps_overlay.h"
#include "cheats_overlay.h"
#include "difficulty_overlay.h"
#include "trophy_overlay.h"
#include "ui_image.h"

// hooks.cpp: save root for the new-profile watcher (difficulty auto-show).
extern void set_user_data_root(const std::filesystem::path& p);

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
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    // Difficulty panel background: the game's blue stitched-felt plate.
    // Upload once; the dialog gets the ImTextureID plus UVs cropped to the
    // plate's alpha bounds (the PNG has transparent margins). Any failure
    // (file missing, decode error, no immediate drawer) leaves the id 0 and
    // the panel draws its old procedural background instead.
    ImTextureID panel_id{};
    ImVec2 panel_uv0(0.0f, 0.0f), panel_uv1(1.0f, 1.0f);
    float panel_aspect = 0.0f;
    if (auto* imm = immediate_drawer()) {
      const std::filesystem::path png = FindAssetFile(
          std::filesystem::path("menustuff") / "blueborder.png");
      std::vector<uint8_t> rgba;
      uint32_t w = 0, h = 0;
      if (!png.empty() && restuff::LoadImageRGBA(png, rgba, w, h)) {
        panel_bg_tex_ = imm->CreateTexture(
            w, h, rex::ui::ImmediateTextureFilter::kLinear, false, rgba.data());
        if (panel_bg_tex_) {
          restuff::AlphaBoundsUV(rgba, w, h, panel_uv0, panel_uv1);
          panel_id = reinterpret_cast<ImTextureID>(panel_bg_tex_.get());
          panel_aspect = ((panel_uv1.x - panel_uv0.x) * w) /
                         ((panel_uv1.y - panel_uv0.y) * h);
        }
      }
    }

    drawer->AddDialog(new FpsOverlayDialog(drawer));
    drawer->AddDialog(new CheatsDialog(drawer));
    drawer->AddDialog(new DifficultyDialog(drawer, panel_id, panel_uv0,
                                           panel_uv1, panel_aspect));
    drawer->AddDialog(new TrophyOverlayDialog(drawer));
  }

  // Load Salsbury (the game's cloth-lettering font) for the difficulty panel.
  // Runs before the atlas is built; searched with the same walk-up used for
  // the game data root so it works from any launch directory.
  void OnConfigureFonts(ImFontAtlas* atlas) override {
    const std::filesystem::path ttf = FindAssetFile(
        std::filesystem::path("fonts") / "Salsbury Regular" / "Salsbury Regular.ttf");
    if (!ttf.empty()) {
      g_salsbury_font = atlas->AddFontFromFileTTF(ttf.string().c_str(), 48.0f);
    }
  }

  // Find assets/<rel> with the same walk-up used for the game data root, so
  // it works from any launch directory. Empty path if not found.
  static std::filesystem::path FindAssetFile(const std::filesystem::path& rel) {
    std::error_code ec;
    for (std::filesystem::path base :
         {std::filesystem::current_path(ec), rex::filesystem::GetExecutableFolder()}) {
      for (; !base.empty(); base = base.parent_path()) {
        std::filesystem::path candidate = base / "assets" / rel;
        if (std::filesystem::exists(candidate, ec)) {
          return candidate;
        }
        if (base == base.parent_path()) {
          break;
        }
      }
    }
    return {};
  }

  // Default the game data root to the project's assets folder when
  // --game_data_root isn't passed on the command line. The new SDK leaves
  // paths.game_data_root empty if no CLI arg / cvar supplied it, so without
  // this the exe wouldn't find Default.xex unless launched from a folder that
  // already has an assets/ next to it.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    // The new-profile watcher (difficulty auto-show) scans the save root.
    set_user_data_root(paths.user_data_root);

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

 private:
  // Keeps the difficulty panel's background upload alive for the app's life
  // (the dialog only holds the raw ImTextureID).
  std::unique_ptr<rex::ui::ImmediateTexture> panel_bg_tex_;
};

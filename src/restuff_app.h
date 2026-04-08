
// restuff - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include "fps_overlay.h"
#include "cheats_overlay.h"

class RestuffApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<RestuffApp>(new RestuffApp(ctx, "restuff",
        PPCImageConfig));
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    drawer->AddDialog(new FpsOverlayDialog(drawer));
    drawer->AddDialog(new CheatsDialog(drawer));
  }
};

#pragma once
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include "imgui.h"

extern void add_score_cheat(float amount);
extern void reset_score_cheat();
extern float get_score_bonus();

// Wireframe is controlled directly via REXCVAR_GET/SET(wireframe) but we
// expose helpers so the overlay doesn't need to pull in rex/cvar.h directly.
void set_wireframe(bool val);
bool get_wireframe();

class CheatsDialog : public rex::ui::ImGuiDialog {
public:
    explicit CheatsDialog(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer) {
        rex::ui::RegisterBind("bind_cheats", "F11", "Toggle cheats overlay", [this] {
            visible_ = !visible_;
        });
    }

    ~CheatsDialog() {
        rex::ui::UnregisterBind("bind_cheats");
    }

    void OnDraw(ImGuiIO& /*io*/) override {
        if (!visible_) return;

        ImGui::SetNextWindowSize(ImVec2(220.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Cheats", &visible_)) {
            ImGui::Text("Score bonus: %.0f", get_score_bonus());
            ImGui::Separator();
            if (ImGui::Button("+100"))   add_score_cheat(100.0f);
            ImGui::SameLine();
            if (ImGui::Button("+1000"))  add_score_cheat(1000.0f);
            ImGui::SameLine();
            if (ImGui::Button("+1000000")) add_score_cheat(1000000.0f);
            if (ImGui::Button("Reset bonus")) reset_score_cheat();

            ImGui::Separator();
            bool wf = get_wireframe();
            if (ImGui::Checkbox("Wireframe", &wf))
                set_wireframe(wf);
        }
        ImGui::End();
    }

private:
    bool visible_ = false;
};

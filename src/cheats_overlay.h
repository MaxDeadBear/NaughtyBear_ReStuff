#pragma once
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include "imgui.h"
#include "trophy_overlay.h"

extern void add_score_cheat(float amount);
extern void reset_score_cheat();
extern float get_score_bonus();

// Wireframe is controlled directly via REXCVAR_GET/SET(wireframe) but we
// expose helpers so the overlay doesn't need to pull in rex/cvar.h directly.
void set_wireframe(bool val);
bool get_wireframe();

// Unlock-all is the unlock_all cvar; while on, all costumes/content are forced
// unlocked (re-applied continuously). See maybe_unlock_all() in hooks.cpp.
void set_unlock_all(bool val);
bool get_unlock_all();

// Texture dumping cvars
bool get_tex_dump();
void set_tex_dump(bool val);
int  get_tex_dump_format_index();  // 0 = TGA, 1 = PNG
void set_tex_dump_format_index(int idx);

// Attract video controls
bool   get_attract_enabled();
void   set_attract_enabled(bool val);
double get_attract_delay();
void   set_attract_delay(double sec);
double get_attract_idle_time();
void   play_attract_video();

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

        ImGui::SetNextWindowSize(ImVec2(280.0f, 0.0f), ImGuiCond_FirstUseEver);
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

            bool ua = get_unlock_all();
            if (ImGui::Checkbox("Unlock all costumes/content", &ua))
                set_unlock_all(ua);

            ImGui::Separator();
            ImGui::Text("Score Displays:");

            bool trophy = get_trophy_overlay_visible();
            if (ImGui::Checkbox("Score HUD Overlay (F10)", &trophy))
                set_trophy_overlay_visible(trophy);

            int mode = get_trophy_overlay_mode();
            const char* modes[] = { "Countdown Remaining", "Absolute Targets" };
            if (ImGui::Combo("Score HUD Mode (F9)", &mode, modes, 2))
                set_trophy_overlay_mode(mode);

            if (get_level_score() < 0) {
                ImGui::TextDisabled("(HUD appears once in a level)");
            }

            bool obj_row = get_score_objective();
            if (ImGui::Checkbox("Objective Menu Score Row", &obj_row))
                set_score_objective(obj_row);

            ImGui::Separator();
            ImGui::Text("Texture Dumping:");

            bool td = get_tex_dump();
            if (ImGui::Checkbox("Dump Textures (tex_dump)", &td))
                set_tex_dump(td);

            int fmt = get_tex_dump_format_index();
            const char* formats[] = { "TGA (.tga)", "PNG (.png)" };
            if (ImGui::Combo("Dump Format", &fmt, formats, 2))
                set_tex_dump_format_index(fmt);

            ImGui::Separator();
            ImGui::Text("Attract Trailers:");

            bool att = get_attract_enabled();
            if (ImGui::Checkbox("Attract Mode Enabled", &att))
                set_attract_enabled(att);

            float delay_sec = static_cast<float>(get_attract_delay());
            if (ImGui::SliderFloat("Delay (sec)", &delay_sec, 5.0f, 120.0f, "%.0fs"))
                set_attract_delay(static_cast<double>(delay_sec));

            ImGui::Text("Idle: %.1fs / %.1fs", get_attract_idle_time(), get_attract_delay());
            ImGui::SameLine();
            if (ImGui::Button("Play Now (F7)"))
                play_attract_video();
        }
        ImGui::End();
    }

private:
    bool visible_ = false;
};


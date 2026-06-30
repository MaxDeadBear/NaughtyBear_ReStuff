#pragma once
#include <cstring>
#include <cstdio>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include "imgui.h"

// Live current-level score, or -1 when not in a level.
extern int get_level_score();
// Per-tier medal score target (tier 0=Bronze..3=Platinum), auto-resolved from
// the game's GradeScore; 0 = unknown.
extern int get_trophy_target(int tier);

// A compact, draggable on-screen counter (FPS-style shadowed text) showing how
// many points remain to each trophy tier. Drag it anywhere with the mouse.
class TrophyOverlayDialog : public rex::ui::ImGuiDialog {
public:
    explicit TrophyOverlayDialog(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer) {
        rex::ui::RegisterBind("bind_trophy_overlay", "F10", "Toggle trophy counter",
                              [this] { visible_ = !visible_; });
        rex::ui::RegisterBind("bind_trophy_mode", "F9", "Cycle trophy counter mode",
                              [this] { mode_ = (mode_ + 1) % 2; });
    }

    ~TrophyOverlayDialog() {
        rex::ui::UnregisterBind("bind_trophy_overlay");
        rex::ui::UnregisterBind("bind_trophy_mode");
    }

    void OnDraw(ImGuiIO& /*io*/) override {
        if (!visible_) return;

        static const char* kNames[4] = {"Bronze", "Silver", "Gold", "Plat"};

        ImGui::SetNextWindowPos(ImVec2(10.0f, 120.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.35f);
        // No title bar + auto-resize, but movable (no NoMove/NoInputs) so it can
        // be dragged by its body with the mouse.
        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoFocusOnAppearing;

        if (ImGui::Begin("##trophy_counter", nullptr, flags)) {
            const int score = get_level_score();
            if (score < 0) {
                Shadowed("Trophy  -- not in a level --", ImVec4(0.75f, 0.75f, 0.75f, 1));
            } else {
                const bool remaining = (mode_ == 0);

                char hdr[48];
                char snum[24];
                Commafmt(score, snum);
                std::snprintf(hdr, sizeof(hdr), "Score: %s", snum);
                Shadowed(hdr, ImVec4(1, 1, 1, 1));

                int next = -1;
                for (int i = 0; i < 4; ++i) {
                    const int t = get_trophy_target(i);
                    if (t > 0 && score < t) { next = i; break; }
                }
                for (int i = 0; i < 4; ++i) {
                    const int t = get_trophy_target(i);
                    char buf[64];
                    ImVec4 col;
                    if (t <= 0) {
                        std::snprintf(buf, sizeof(buf), "%-7s  --", kNames[i]);
                        col = ImVec4(0.55f, 0.55f, 0.55f, 1);
                    } else if (score >= t) {
                        char num[24];
                        Commafmt(t, num);
                        std::snprintf(buf, sizeof(buf), "%-7s  %s", kNames[i],
                                      remaining ? "reached" : num);
                        col = ImVec4(0.4f, 1.0f, 0.4f, 1);
                    } else if (remaining) {
                        char num[24];
                        Commafmt(t - score, num);  // counts down live as score rises
                        std::snprintf(buf, sizeof(buf), "%-7s  %s remain", kNames[i], num);
                        col = (i == next) ? ImVec4(1.0f, 0.9f, 0.3f, 1)
                                          : ImVec4(0.85f, 0.85f, 0.85f, 1);
                    } else {
                        char num[24];
                        Commafmt(t, num);
                        std::snprintf(buf, sizeof(buf), "%-7s  %s", kNames[i], num);
                        col = (i == next) ? ImVec4(1.0f, 0.9f, 0.3f, 1)
                                          : ImVec4(0.85f, 0.85f, 0.85f, 1);
                    }
                    Shadowed(buf, col);
                }
            }
        }
        ImGui::End();
    }

private:
    bool visible_ = false;
    int mode_ = 0;  // 0 = points remaining (countdown), 1 = absolute targets

    static void Commafmt(int v, char* out) {
        char tmp[24];
        std::snprintf(tmp, sizeof(tmp), "%d", v);
        const int len = static_cast<int>(std::strlen(tmp));
        int o = 0;
        for (int i = 0; i < len; ++i) {
            if (i > 0 && (len - i) % 3 == 0) out[o++] = ',';
            out[o++] = tmp[i];
        }
        out[o] = '\0';
    }

    // FPS-overlay-style text with a 1px black drop shadow for readability.
    static void Shadowed(const char* text, ImVec4 color) {
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 sh = IM_COL32(0, 0, 0, 200);
        dl->AddText(ImVec2(pos.x - 1, pos.y - 1), sh, text);
        dl->AddText(ImVec2(pos.x + 1, pos.y - 1), sh, text);
        dl->AddText(ImVec2(pos.x - 1, pos.y + 1), sh, text);
        dl->AddText(ImVec2(pos.x + 1, pos.y + 1), sh, text);
        ImGui::TextColored(color, "%s", text);
    }
};

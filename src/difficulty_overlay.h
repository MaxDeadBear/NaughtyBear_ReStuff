#pragma once
#include <cfloat>
#include <cmath>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include "imgui.h"

// Difficulty state lives in hooks.cpp: cvar `difficulty` (0=Nice 1=Normal
// 2=Naughty 3=Nutter), persisted to the restuff.toml staged next to the exe.
extern int  get_difficulty();
extern void set_difficulty(int level);
extern const char* get_difficulty_name(int level);
// True exactly once, when the frontend menu (startmenu.lua) first loads.
extern bool consume_difficulty_autoshow();

// Salsbury (the game's cloth-lettering font), loaded in
// RestuffApp::OnConfigureFonts from assets/fonts/Salsbury Regular/. Baked at
// 48px; the panel downscales it per element. nullptr = font file missing,
// panel falls back to the default ImGui font.
inline ImFont* g_salsbury_font = nullptr;

// "Select difficulty" panel styled after the game's stitched-cloth dialogs:
// pale-blue rounded panel with a dashed stitch border over a dimmed screen.
// Auto-opens the first time the main menu comes up; F8 reopens it anytime.
// Mouse click or Up/Down + Enter picks, Esc dismisses. The stitched underline
// marks the difficulty currently in effect.
class DifficultyDialog : public rex::ui::ImGuiDialog {
public:
    explicit DifficultyDialog(rex::ui::ImGuiDrawer* drawer)
        : rex::ui::ImGuiDialog(drawer) {
        rex::ui::RegisterBind("bind_difficulty", "F8", "Select difficulty", [this] {
            visible_ = !visible_;
            if (visible_) sel_ = get_difficulty();
        });
    }

    ~DifficultyDialog() { rex::ui::UnregisterBind("bind_difficulty"); }

    void OnDraw(ImGuiIO& /*io*/) override {
        if (consume_difficulty_autoshow() && !visible_) {
            visible_ = true;
            sel_ = get_difficulty();
        }
        if (!visible_) return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 vpos = vp->Pos, vsz = vp->Size;

        // Dim the game behind the panel (like the mock's darkened menu).
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            vpos, ImVec2(vpos.x + vsz.x, vpos.y + vsz.y), IM_COL32(0, 0, 0, 120));

        const float pw = Clamp(vsz.x * 0.62f, 460.0f, 920.0f);
        const float ph = Clamp(vsz.y * 0.62f, 320.0f, 560.0f);
        ImGui::SetNextWindowPos(ImVec2(vpos.x + vsz.x * 0.5f, vpos.y + vsz.y * 0.5f),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoNav;
        if (!ImGui::Begin("##difficulty_panel", nullptr, flags)) {
            ImGui::End();
            return;
        }

        // Keyboard first so the highlight matches this frame's draw.
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))   sel_ = (sel_ + 3) % 4;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) sel_ = (sel_ + 1) % 4;
        const bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                             ImGui::IsKeyPressed(ImGuiKey_Space);
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) visible_ = false;

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetWindowPos();
        const ImVec2 mx = ImVec2(mn.x + pw, mn.y + ph);
        const float rounding = 30.0f;

        // Cloth panel + stitch border.
        dl->AddRectFilled(mn, mx, kPanelBg, rounding);
        dl->AddRect(mn, mx, kPanelEdge, rounding, 0, 3.0f);
        StitchRoundedRect(dl, ImVec2(mn.x + 9, mn.y + 9), ImVec2(mx.x - 9, mx.y - 9),
                          rounding - 8.0f, kStitch, 3.0f, 11.0f, 8.0f);

        ImFont* font = g_salsbury_font ? g_salsbury_font : ImGui::GetFont();
        const float title_px = Clamp(ph * 0.115f, 24.0f, 44.0f);
        const float opt_px   = Clamp(ph * 0.09f, 20.0f, 36.0f);

        // Title.
        {
            const char* t = "Select difficulty";
            const ImVec2 ts = font->CalcTextSizeA(title_px, FLT_MAX, 0.0f, t);
            const ImVec2 p(mn.x + (pw - ts.x) * 0.5f, mn.y + ph * 0.115f);
            dl->AddText(font, title_px, ImVec2(p.x + 2, p.y + 2), kTextShadow, t);
            dl->AddText(font, title_px, p, kText, t);
        }

        // Options.
        const int   current  = get_difficulty();
        const float area_top = mn.y + ph * 0.30f;
        const float row_step = (ph * 0.62f) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            const char* t = get_difficulty_name(i);
            const ImVec2 ts = font->CalcTextSizeA(opt_px, FLT_MAX, 0.0f, t);
            const float cy = area_top + row_step * (i + 0.5f);
            const ImVec2 p(mn.x + (pw - ts.x) * 0.5f, cy - ts.y * 0.5f);

            // Hover/click hitbox spanning most of the row.
            ImGui::SetCursorScreenPos(ImVec2(mn.x + pw * 0.25f, cy - row_step * 0.45f));
            ImGui::PushID(i);
            const bool clicked =
                ImGui::InvisibleButton("opt", ImVec2(pw * 0.5f, row_step * 0.9f));
            if (ImGui::IsItemHovered()) sel_ = i;
            ImGui::PopID();

            const bool hot = (sel_ == i);
            if (hot)
                dl->AddRectFilled(ImVec2(p.x - 24, p.y - 6),
                                  ImVec2(p.x + ts.x + 24, p.y + ts.y + 6),
                                  kHotBg, 12.0f);
            dl->AddText(font, opt_px, ImVec2(p.x + 2, p.y + 2), kTextShadow, t);
            dl->AddText(font, opt_px, p, hot ? kTextHot : kText, t);

            // Stitched underline marks the difficulty currently in effect.
            if (i == current)
                StitchLine(dl, ImVec2(p.x - 6, p.y + ts.y + 5),
                           ImVec2(p.x + ts.x + 6, p.y + ts.y + 5), kStitch, 2.5f,
                           8.0f, 6.0f);

            if (clicked || (hot && confirm)) {
                set_difficulty(i);
                visible_ = false;
            }
        }

        // Footer hint (Salsbury too; slightly larger than the old default-font
        // hint so the display face stays legible at footer size).
        {
            const char* t = "Enter / click to select   -   Esc to close   -   F8 reopens";
            const float px = Clamp(ph * 0.048f, 15.0f, 20.0f);
            const ImVec2 ts = font->CalcTextSizeA(px, FLT_MAX, 0.0f, t);
            dl->AddText(font, px,
                        ImVec2(mn.x + (pw - ts.x) * 0.5f, mx.y - ph * 0.075f),
                        kTextFaint, t);
        }

        ImGui::End();
    }

private:
    bool visible_ = false;
    int  sel_ = 1;

    // Palette sampled from the game's stitched dialog look.
    static constexpr ImU32 kPanelBg    = IM_COL32(213, 237, 244, 252);
    static constexpr ImU32 kPanelEdge  = IM_COL32(74, 163, 199, 255);
    static constexpr ImU32 kStitch     = IM_COL32(58, 145, 185, 220);
    static constexpr ImU32 kText       = IM_COL32(23, 111, 156, 255);
    static constexpr ImU32 kTextHot    = IM_COL32(6, 76, 116, 255);
    static constexpr ImU32 kTextFaint  = IM_COL32(23, 111, 156, 140);
    static constexpr ImU32 kTextShadow = IM_COL32(255, 255, 255, 110);
    static constexpr ImU32 kHotBg      = IM_COL32(255, 255, 255, 90);

    static float Clamp(float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // Dashes along a straight line.
    static void StitchLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col,
                           float th, float dash, float gap) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1.0f) return;
        const float ux = dx / len, uy = dy / len;
        float t = 0.0f;
        while (t < len) {
            const float e = (t + dash < len) ? t + dash : len;
            dl->AddLine(ImVec2(a.x + ux * t, a.y + uy * t),
                        ImVec2(a.x + ux * e, a.y + uy * e), col, th);
            t = e + gap;
        }
    }

    // Dashed "stitching" along a rounded-rect outline: sample the outline into
    // a polyline (4 quarter arcs; the straight edges are the spans between
    // consecutive arc endpoints), then walk it emitting dashes by arc length.
    static void StitchRoundedRect(ImDrawList* dl, ImVec2 mn, ImVec2 mx, float r,
                                  ImU32 col, float th, float dash, float gap) {
        constexpr float kPi = 3.14159265f;
        ImVec2 pts[64];
        int n = 0;
        auto arc = [&](float cx, float cy, float a0, float a1) {
            for (int i = 0; i <= 8; ++i) {
                const float a = a0 + (a1 - a0) * (i / 8.0f);
                pts[n++] = ImVec2(cx + std::cos(a) * r, cy + std::sin(a) * r);
            }
        };
        arc(mn.x + r, mn.y + r, kPi, kPi * 1.5f);         // top-left
        arc(mx.x - r, mn.y + r, kPi * 1.5f, kPi * 2.0f);  // top-right
        arc(mx.x - r, mx.y - r, 0.0f, kPi * 0.5f);        // bottom-right
        arc(mn.x + r, mx.y - r, kPi * 0.5f, kPi);         // bottom-left
        pts[n++] = pts[0];                                // close the loop

        bool  down = true;   // pen down = currently drawing a dash
        float pen  = 0.0f;   // length already spent in the current dash/gap
        for (int i = 1; i < n; ++i) {
            const ImVec2 a = pts[i - 1], b = pts[i];
            const float sdx = b.x - a.x, sdy = b.y - a.y;
            const float seg = std::sqrt(sdx * sdx + sdy * sdy);
            if (seg < 0.001f) continue;
            float t0 = 0.0f;
            while (t0 < seg) {
                const float want = (down ? dash : gap) - pen;
                const float t1 = t0 + want;
                const float te = (t1 < seg) ? t1 : seg;
                if (down)
                    dl->AddLine(ImVec2(a.x + sdx * (t0 / seg), a.y + sdy * (t0 / seg)),
                                ImVec2(a.x + sdx * (te / seg), a.y + sdy * (te / seg)),
                                col, th);
                if (t1 < seg) { down = !down; pen = 0.0f; }
                else          { pen += seg - t0; }
                t0 = te;
            }
        }
    }
};

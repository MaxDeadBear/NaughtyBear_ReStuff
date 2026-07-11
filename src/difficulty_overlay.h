#pragma once
#include <cfloat>
#include <cmath>
#include <rex/input/input.h>
#include <rex/input/input_system.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include "imgui.h"

// Difficulty state lives in hooks.cpp: cvar `difficulty` (0=Nice 1=Normal
// 2=Naughty 3=Nutter), persisted to the restuff.toml staged next to the exe.
extern int  get_difficulty();
extern void set_difficulty(int level);
extern const char* get_difficulty_name(int level);
// True exactly once, when a new save profile is created this session.
extern bool consume_difficulty_autoshow();
// difficulty_enabled cvar: false = vanilla experience, panel never opens.
extern bool get_difficulty_feature_enabled();
// difficulty_native_prompt cvar: true = show the game's own stitched
// UserPrompt dialog (driven from hooks.cpp) instead of this ImGui panel.
extern bool get_difficulty_native_prompt();
extern void request_native_difficulty_prompt();
// While true, the guest's XamInput reads are blanked so the game menu behind
// the panel doesn't react to the same presses (hooks.cpp).
extern void set_guest_input_suppressed(bool on);

// Salsbury (the game's cloth-lettering font), loaded in
// RestuffApp::OnConfigureFonts from assets/fonts/Salsbury Regular/. Baked at
// 48px; the panel downscales it per element. nullptr = font file missing,
// panel falls back to the default ImGui font.
inline ImFont* g_salsbury_font = nullptr;

// "Select difficulty" panel styled after the game's stitched-cloth dialogs:
// the game's own blue stitched-felt plate (assets/menustuff/blueborder.png,
// uploaded by RestuffApp) drawn over a dimmed screen; when the texture is
// missing it falls back to the old procedural pale-blue panel + dash border.
// Slides in from the top of the screen. Auto-opens when a new save profile is
// created; F8 reopens it anytime. Controller (D-pad/stick + A, B closes),
// mouse, or Up/Down + Enter picks; Esc dismisses. The stitched underline
// marks the difficulty currently in effect.
class DifficultyDialog : public rex::ui::ImGuiDialog {
public:
    // panel_tex: ImmediateTexture* cast to ImTextureID (0 = procedural
    // fallback); panel_uv0/uv1 crop the texture's transparent margins so the
    // cloth plate fills the panel rect edge to edge. panel_aspect = cropped
    // plate width/height — the panel keeps that shape and limits how far the
    // plate is stretched past its native pixels so it stays sharp.
    explicit DifficultyDialog(rex::ui::ImGuiDrawer* drawer,
                              ImTextureID panel_tex = ImTextureID{},
                              ImVec2 panel_uv0 = ImVec2(0.0f, 0.0f),
                              ImVec2 panel_uv1 = ImVec2(1.0f, 1.0f),
                              float panel_aspect = 0.0f)
        : rex::ui::ImGuiDialog(drawer),
          panel_tex_(panel_tex),
          panel_uv0_(panel_uv0),
          panel_uv1_(panel_uv1),
          panel_aspect_(panel_aspect) {
        rex::ui::RegisterBind("bind_difficulty", "F8", "Select difficulty", [this] {
            if (visible_)                                   ClosePanel();
            else if (!get_difficulty_feature_enabled())     {}
            else if (get_difficulty_native_prompt())        request_native_difficulty_prompt();
            else                                            OpenPanel();
        });
    }

    ~DifficultyDialog() {
        rex::ui::UnregisterBind("bind_difficulty");
        if (visible_) set_guest_input_suppressed(false);
    }

    void OnDraw(ImGuiIO& io) override {
        if (consume_difficulty_autoshow() && !visible_) {
            if (get_difficulty_native_prompt()) request_native_difficulty_prompt();
            else                                OpenPanel();
        }
        if (!visible_) return;

        // Slide-in: ease-out cubic from above the screen to center.
        anim_ = Clamp(anim_ + io.DeltaTime / kSlideSeconds, 0.0f, 1.0f);
        const float ease = 1.0f - (1.0f - anim_) * (1.0f - anim_) * (1.0f - anim_);

        ImGuiViewport* vp = ImGui::GetMainViewport();
        const ImVec2 vpos = vp->Pos, vsz = vp->Size;

        // Dim the game behind the panel (fades in with the slide).
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            vpos, ImVec2(vpos.x + vsz.x, vpos.y + vsz.y),
            IM_COL32(0, 0, 0, static_cast<int>(120 * ease)));

        // With the plate texture, keep its native shape and don't blow it up
        // much past its source pixels (512-class art goes soft when doubled).
        float pw, ph;
        if (panel_tex_ && panel_aspect_ > 0.0f) {
            ph = Clamp(vsz.y * 0.62f, 300.0f, 500.0f);
            pw = ph * panel_aspect_;
            const float max_w = vsz.x * 0.55f;
            if (pw > max_w) {
                pw = max_w;
                ph = pw / panel_aspect_;
            }
        } else {
            pw = Clamp(vsz.x * 0.62f, 460.0f, 920.0f);
            ph = Clamp(vsz.y * 0.62f, 320.0f, 560.0f);
        }
        const float cy_end   = vpos.y + vsz.y * 0.5f;          // settled: centered
        const float cy_start = vpos.y - ph * 0.5f - 12.0f;     // fully off-screen top
        const float cy = cy_start + (cy_end - cy_start) * ease;
        ImGui::SetNextWindowPos(ImVec2(vpos.x + vsz.x * 0.5f, cy),
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

        // Keyboard + controller first so the highlight matches this frame's draw.
        using namespace rex::input;
        const uint16_t pad = PollPadButtons();
        const uint16_t pressed = static_cast<uint16_t>(pad & ~pad_prev_);
        pad_prev_ = pad;

        bool nav_up   = ImGui::IsKeyPressed(ImGuiKey_UpArrow) ||
                        (pressed & X_INPUT_GAMEPAD_DPAD_UP);
        bool nav_down = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                        (pressed & X_INPUT_GAMEPAD_DPAD_DOWN);
        // Hold-to-repeat, like the game's own menus.
        if (pad & (X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_DOWN)) {
            hold_ += io.DeltaTime;
            if (hold_ > 0.45f) {
                repeat_ += io.DeltaTime;
                if (repeat_ > 0.13f) {
                    repeat_ = 0.0f;
                    if (pad & X_INPUT_GAMEPAD_DPAD_UP)   nav_up = true;
                    if (pad & X_INPUT_GAMEPAD_DPAD_DOWN) nav_down = true;
                }
            }
        } else {
            hold_ = repeat_ = 0.0f;
        }
        if (nav_up)   sel_ = (sel_ + 3) % 4;
        if (nav_down) sel_ = (sel_ + 1) % 4;

        const bool confirm = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                             ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) ||
                             ImGui::IsKeyPressed(ImGuiKey_Space) ||
                             (pressed & (X_INPUT_GAMEPAD_A | X_INPUT_GAMEPAD_START));
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (pressed & X_INPUT_GAMEPAD_B)) {
            ClosePanel();
            ImGui::End();
            return;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 mn = ImGui::GetWindowPos();
        const ImVec2 mx = ImVec2(mn.x + pw, mn.y + ph);
        const float rounding = 30.0f;

        // Cloth panel: the game's stitched felt plate when available, else the
        // procedural rounded rect + dash border it used to draw.
        if (panel_tex_) {
            dl->AddImage(panel_tex_, mn, mx, panel_uv0_, panel_uv1_);
        } else {
            dl->AddRectFilled(mn, mx, kPanelBg, rounding);
            dl->AddRect(mn, mx, kPanelEdge, rounding, 0, 3.0f);
            StitchRoundedRect(dl, ImVec2(mn.x + 9, mn.y + 9),
                              ImVec2(mx.x - 9, mx.y - 9), rounding - 8.0f,
                              kStitch, 3.0f, 11.0f, 8.0f);
        }

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
            const float cyy = area_top + row_step * (i + 0.5f);
            const ImVec2 p(mn.x + (pw - ts.x) * 0.5f, cyy - ts.y * 0.5f);

            // Hover/click hitbox spanning most of the row.
            ImGui::SetCursorScreenPos(ImVec2(mn.x + pw * 0.25f, cyy - row_step * 0.45f));
            ImGui::PushID(i);
            const bool clicked =
                ImGui::InvisibleButton("opt", ImVec2(pw * 0.5f, row_step * 0.9f));
            if (ImGui::IsItemHovered()) sel_ = i;
            ImGui::PopID();

            // Selection = white text (dark shadow keeps it readable on the
            // pale cloth); everything else stays the stitched blue.
            const bool hot = (sel_ == i);
            dl->AddText(font, opt_px, ImVec2(p.x + 2, p.y + 2),
                        hot ? kTextHotShadow : kTextShadow, t);
            dl->AddText(font, opt_px, p, hot ? kTextHot : kText, t);

            // Stitched underline marks the difficulty currently in effect.
            if (i == current)
                StitchLine(dl, ImVec2(p.x - 6, p.y + ts.y + 5),
                           ImVec2(p.x + ts.x + 6, p.y + ts.y + 5), kStitch, 2.5f,
                           8.0f, 6.0f);

            if (clicked || (hot && confirm)) {
                set_difficulty(i);
                ClosePanel();
            }
        }

        ImGui::End();
    }

private:
    static constexpr float kSlideSeconds = 0.65f;

    bool     visible_ = false;
    int      sel_ = 1;
    float    anim_ = 0.0f;      // slide-in progress 0..1
    uint16_t pad_prev_ = 0;     // last frame's pad buttons (edge detection)
    float    hold_ = 0.0f;      // dpad hold time (auto-repeat)
    float    repeat_ = 0.0f;
    ImTextureID panel_tex_{};   // blueborder.png upload (0 = draw procedural)
    ImVec2   panel_uv0_{0.0f, 0.0f};
    ImVec2   panel_uv1_{1.0f, 1.0f};
    float    panel_aspect_ = 0.0f;  // cropped plate w/h (0 = no texture)

    void OpenPanel() {
        visible_ = true;
        sel_ = get_difficulty();
        anim_ = 0.0f;
        hold_ = repeat_ = 0.0f;
        // Seed edge detection with the buttons currently held so the press
        // that opened the menu (e.g. A on "create saved game") can't also
        // instantly select an option.
        pad_prev_ = PollPadButtons();
        set_guest_input_suppressed(true);
    }

    void ClosePanel() {
        visible_ = false;
        set_guest_input_suppressed(false);
    }

    // Pad 0 buttons with the left stick folded into the D-pad bits, read from
    // the SDK input system (host side, unaffected by the guest suppression).
    static uint16_t PollPadButtons() {
        auto* rt = rex::Runtime::instance();
        if (!rt) return 0;
        auto* is = static_cast<rex::input::InputSystem*>(rt->input_system());
        if (!is) return 0;
        rex::input::X_INPUT_STATE st{};
        if (is->GetState(0, &st) != 0) return 0;
        uint16_t b = st.gamepad.buttons;
        const int16_t ly = st.gamepad.thumb_ly;
        if (ly >  16000) b |= rex::input::X_INPUT_GAMEPAD_DPAD_UP;
        if (ly < -16000) b |= rex::input::X_INPUT_GAMEPAD_DPAD_DOWN;
        return b;
    }

    // Palette sampled from the game's stitched dialog look.
    static constexpr ImU32 kPanelBg    = IM_COL32(213, 237, 244, 252);
    static constexpr ImU32 kPanelEdge  = IM_COL32(74, 163, 199, 255);
    static constexpr ImU32 kStitch     = IM_COL32(58, 145, 185, 220);
    static constexpr ImU32 kText          = IM_COL32(23, 111, 156, 255);
    static constexpr ImU32 kTextHot       = IM_COL32(255, 255, 255, 255);
    static constexpr ImU32 kTextShadow    = IM_COL32(255, 255, 255, 110);
    static constexpr ImU32 kTextHotShadow = IM_COL32(6, 76, 116, 170);

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

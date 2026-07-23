// Fullscreen video overlay. Polls the restuff::PlayVideo() request slot, decodes
// frames with VideoPlayer, and draws them letterboxed over everything else.
//
// Playback is driven from the render thread (OnDraw), so decoding is capped per
// frame inside VideoPlayer::Update to avoid stalling presentation.

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "imgui.h"
#include "video_player.h"

// The SDK's audio output mute (defined in its SDL audio driver). The video has
// its own XAudio2 output, so we silence the guest mixer while one is playing --
// otherwise menu music plays over the cinematic.
REXCVAR_DECLARE(bool, audio_mute);

class VideoOverlayDialog : public rex::ui::ImGuiDialog {
 public:
    VideoOverlayDialog(rex::ui::ImGuiDrawer* drawer, rex::ui::ImmediateDrawer* imm)
        : rex::ui::ImGuiDialog(drawer), imm_(imm) {
        if (!imm_) REXLOG_WARN("[video] no immediate drawer -- frames can't be uploaded");
    }

    void OnDraw(ImGuiIO& io) override {
        PumpRequest();
        if (!player_.IsOpen()) return;

        const double elapsed = player_.elapsed();

        // Input aborts playback -- but only after a short grace period, or the
        // very keypress that started the video would cancel it on frame one.
        // (Gamepad/guest input never reaches ImGui; hooks call StopVideo().)
        constexpr double kInputGrace = 0.75;
        if (elapsed > kInputGrace && AnyInput()) {
            REXLOG_INFO("[video] cancelled by input at {:.1f}s", elapsed);
            Stop();
            return;
        }

        std::vector<uint8_t> rgba;
        bool finished = false;
        if (player_.Update(rgba, finished) && !rgba.empty() && imm_) {
            // No texture-update path on ImmediateDrawer, so replace per frame.
            auto tex = imm_->CreateTexture(player_.width(), player_.height(),
                                           rex::ui::ImmediateTextureFilter::kLinear,
                                           false, rgba.data());
            if (tex) {
                tex_ = std::move(tex);
                if (!logged_frame_) {
                    REXLOG_INFO("[video] first frame decoded+uploaded ({}x{})",
                                player_.width(), player_.height());
                    logged_frame_ = true;
                }
            } else if (!logged_texfail_) {
                REXLOG_WARN("[video] CreateTexture failed ({}x{})", player_.width(),
                            player_.height());
                logged_texfail_ = true;
            }
        }
        if (finished) {
            const std::filesystem::path again = loop_ ? path_ : std::filesystem::path();
            REXLOG_INFO("[video] finished at {:.1f}s", elapsed);
            Stop();
            // Re-post through the normal request path so looping re-applies the
            // mute and logging exactly like a fresh start.
            if (!again.empty()) restuff::PlayVideo(again, true);
            return;
        }
        if (!tex_) return;

        // Black backdrop + aspect-correct letterboxed frame, on top of everything.
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const ImVec2 screen = io.DisplaySize;
        dl->AddRectFilled(ImVec2(0, 0), screen, IM_COL32(0, 0, 0, 255));

        const float vw = float(player_.width()), vh = float(player_.height());
        if (vw > 0.0f && vh > 0.0f && screen.x > 0.0f && screen.y > 0.0f) {
            const float scale = (screen.x / screen.y > vw / vh) ? (screen.y / vh)
                                                                : (screen.x / vw);
            const ImVec2 size(vw * scale, vh * scale);
            const ImVec2 mn((screen.x - size.x) * 0.5f, (screen.y - size.y) * 0.5f);
            dl->AddImage(reinterpret_cast<ImTextureID>(tex_.get()), mn,
                         ImVec2(mn.x + size.x, mn.y + size.y));
        }
    }

 private:
    // Picks up PlayVideo()/StopVideo() posted from other threads.
    void PumpRequest() {
        std::filesystem::path want;
        bool want_loop = false, want_stop = false;
        {
            auto& r = restuff::video_request();
            std::lock_guard<std::mutex> lk(r.mtx);
            if (r.stop) {
                want_stop = true;
                r.stop = false;
            }
            if (!r.path.empty()) {
                want = r.path;
                want_loop = r.loop;
                r.path.clear();
            }
        }
        if (want_stop) Stop();
        if (want.empty()) return;
        Stop();
        if (player_.Open(want)) {
            path_ = want;
            loop_ = want_loop;
            logged_frame_ = false;
            logged_texfail_ = false;
            // Silence the guest mixer so menu music doesn't play over the video.
            if (!muted_) {
                prev_mute_ = REXCVAR_GET(audio_mute);
                REXCVAR_SET(audio_mute, true);
                muted_ = true;
            }
            Publish(true);
            REXLOG_INFO("[video] opened {}x{} audio={}", player_.width(),
                        player_.height(), player_.has_audio() ? "yes" : "none");
        } else {
            REXLOG_WARN("[video] Media Foundation could not open '{}'",
                        restuff::PathToUtf8(want));
        }
    }

    void Stop() {
        player_.Close();
        tex_.reset();
        path_.clear();
        loop_ = false;
        if (muted_) {  // restore whatever the user had before
            REXCVAR_SET(audio_mute, prev_mute_);
            muted_ = false;
        }
        Publish(false);
    }

    void Publish(bool playing) {
        auto& r = restuff::video_request();
        std::lock_guard<std::mutex> lk(r.mtx);
        r.playing = playing;
    }

    static bool AnyInput() {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            return true;
        }
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(k), false)) return true;
        }
        return false;
    }

    rex::ui::ImmediateDrawer* imm_ = nullptr;
    restuff::VideoPlayer player_;
    std::unique_ptr<rex::ui::ImmediateTexture> tex_;
    std::filesystem::path path_;
    bool loop_ = false;
    bool logged_frame_ = false;
    bool logged_texfail_ = false;
    bool muted_ = false;       // we forced audio_mute on
    bool prev_mute_ = false;   // what it was before we did
};

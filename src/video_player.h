// Minimal MP4/H.264 playback via Windows Media Foundation, with the audio track
// played through XAudio2. Both ship with Windows, so no thirdparty dependency.
//
// The guest's own mixer is untouched -- this is a separate host audio output, so
// the caller should mute game audio while a video plays (the overlay does that
// via the SDK's audio_mute cvar).
//
// Links mfplat + mfreadwrite + mfuuid + xaudio2 (see CMakeLists.txt).

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <xaudio2.h>

namespace restuff {

// Paths are natively wide on Windows. std::filesystem::path::string() converts
// via the active code page and THROWS std::system_error on anything it can't
// represent (accents, dashes, etc. in a downloaded filename), so never use it --
// carry paths as `path` and use this non-throwing UTF-8 form for logging only.
inline std::string PathToUtf8(const std::filesystem::path& p) {
    const std::wstring& w = p.native();
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

// One-time Media Foundation startup, shared by every player instance.
inline bool EnsureMediaFoundation() {
    static const bool s_ok = [] {
        // MF and XAudio2 both need COM. MTA matches ui_image.h's WIC usage; a
        // RPC_E_CHANGED_MODE just means the thread already picked a mode.
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }();
    return s_ok;
}

// Streams PCM to the default output. Buffers stay owned here until XAudio2 has
// consumed them (XAUDIO2_BUFFER only borrows the pointer).
class AudioOut {
 public:
    ~AudioOut() { Stop(); }

    bool Start(const WAVEFORMATEX* wfx) {
        Stop();
        if (!wfx) return false;
        if (FAILED(XAudio2Create(xa_.GetAddressOf(), 0, XAUDIO2_DEFAULT_PROCESSOR))) {
            return false;
        }
        if (FAILED(xa_->CreateMasteringVoice(&master_)) ||
            FAILED(xa_->CreateSourceVoice(&voice_, wfx))) {
            Stop();
            return false;
        }
        sample_rate_ = wfx->nSamplesPerSec;
        voice_->Start(0);
        return true;
    }

    void Submit(std::vector<uint8_t>&& pcm) {
        if (!voice_ || pcm.empty()) return;
        auto held = std::make_unique<std::vector<uint8_t>>(std::move(pcm));
        XAUDIO2_BUFFER b{};
        b.AudioBytes = static_cast<UINT32>(held->size());
        b.pAudioData = held->data();
        if (SUCCEEDED(voice_->SubmitSourceBuffer(&b))) {
            queued_.push_back(std::move(held));
        }
        // Release buffers XAudio2 has finished with.
        XAUDIO2_VOICE_STATE st{};
        voice_->GetState(&st);
        while (queued_.size() > st.BuffersQueued) queued_.pop_front();
    }

    // Seconds of audio actually played -- the master clock for A/V sync.
    double PlayedSeconds() const {
        if (!voice_ || sample_rate_ == 0) return 0.0;
        XAUDIO2_VOICE_STATE st{};
        voice_->GetState(&st);
        return double(st.SamplesPlayed) / double(sample_rate_);
    }

    size_t Queued() const {
        if (!voice_) return 0;
        XAUDIO2_VOICE_STATE st{};
        voice_->GetState(&st);
        return st.BuffersQueued;
    }

    bool ok() const { return voice_ != nullptr; }

    void Stop() {
        if (voice_) {
            voice_->Stop(0);
            voice_->FlushSourceBuffers();
            voice_->DestroyVoice();
            voice_ = nullptr;
        }
        if (master_) {
            master_->DestroyVoice();
            master_ = nullptr;
        }
        xa_.Reset();
        queued_.clear();
        sample_rate_ = 0;
    }

 private:
    Microsoft::WRL::ComPtr<IXAudio2> xa_;
    IXAudio2MasteringVoice* master_ = nullptr;
    IXAudio2SourceVoice* voice_ = nullptr;
    std::deque<std::unique_ptr<std::vector<uint8_t>>> queued_;
    uint32_t sample_rate_ = 0;
};

// Decodes a video file to RGBA8 frames and plays its audio track.
// Single-threaded: drive it from whichever thread draws.
class VideoPlayer {
 public:
    VideoPlayer() = default;
    ~VideoPlayer() { Close(); }
    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    bool Open(const std::filesystem::path& path) {
        Close();
        if (!EnsureMediaFoundation()) return false;
        using Microsoft::WRL::ComPtr;

        // Advanced video processing lets the reader hand us plain RGB32 no
        // matter what the file's native format is (NV12, etc.).
        ComPtr<IMFAttributes> attrs;
        if (FAILED(MFCreateAttributes(&attrs, 1))) return false;
        attrs->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

        if (FAILED(MFCreateSourceReaderFromURL(path.wstring().c_str(), attrs.Get(),
                                               reader_.GetAddressOf()))) {
            reader_.Reset();
            return false;
        }
        reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

        ComPtr<IMFMediaType> want;
        if (FAILED(MFCreateMediaType(&want)) ||
            FAILED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
            FAILED(want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32)) ||
            FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                nullptr, want.Get()))) {
            Close();
            return false;
        }

        // The resolved type carries the real frame size and row order.
        ComPtr<IMFMediaType> actual;
        if (FAILED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                                                actual.GetAddressOf())) ||
            FAILED(MFGetAttributeSize(actual.Get(), MF_MT_FRAME_SIZE, &width_, &height_)) ||
            width_ == 0 || height_ == 0) {
            Close();
            return false;
        }
        UINT32 stride = 0;
        if (SUCCEEDED(actual->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) &&
            static_cast<INT32>(stride) < 0) {
            bottom_up_ = true;  // RGB32 often arrives bottom-up
        }

        OpenAudio();  // optional -- a silent file just plays without sound

        eof_ = false;
        have_next_ = false;
        next_pts_ = 0.0;
        audio_eof_ = false;
        start_ = std::chrono::steady_clock::now();
        return true;
    }

    void Close() {
        audio_.Stop();
        reader_.Reset();
        next_rgba_.clear();
        width_ = height_ = 0;
        eof_ = true;
        have_next_ = false;
        bottom_up_ = false;
        next_pts_ = 0.0;
        audio_eof_ = true;
        has_audio_ = false;
    }

    bool IsOpen() const { return reader_ != nullptr; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }
    bool has_audio() const { return has_audio_; }

    // Playback position. Prefers the audio clock so video tracks sound; falls
    // back to wall time for silent files.
    double elapsed() const {
        if (has_audio_ && audio_.ok()) {
            const double t = audio_.PlayedSeconds();
            if (t > 0.0) return t;
        }
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start_)
            .count();
    }

    // Advances to the frame that should be on screen now, and keeps the audio
    // queue fed. Fills out_rgba and returns true only when a NEW frame was
    // produced. finished becomes true once everything has run out.
    bool Update(std::vector<uint8_t>& out_rgba, bool& finished) {
        finished = false;
        if (!reader_) {
            finished = true;
            return false;
        }
        PumpAudio();
        const double now = elapsed();

        bool updated = false;
        // Cap the work per call so a burst of decoding can't stall the frame.
        for (int guard = 0; guard < 8; ++guard) {
            if (!have_next_ && !ReadNext()) break;  // eof / no sample this round
            if (!have_next_ || next_pts_ > now) break;  // next frame still in the future
            out_rgba = std::move(next_rgba_);
            next_rgba_.clear();
            have_next_ = false;
            updated = true;
        }
        // Let queued audio drain before declaring the end.
        finished = eof_ && !have_next_ && (!has_audio_ || audio_.Queued() == 0);
        return updated;
    }

 private:
    // Configures the audio stream to PCM and opens the output. Optional.
    void OpenAudio() {
        using Microsoft::WRL::ComPtr;
        if (FAILED(reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE))) {
            return;
        }
        ComPtr<IMFMediaType> want;
        if (FAILED(MFCreateMediaType(&want)) ||
            FAILED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) ||
            FAILED(want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)) ||
            FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                nullptr, want.Get()))) {
            reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
            return;
        }
        ComPtr<IMFMediaType> actual;
        if (FAILED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                actual.GetAddressOf()))) {
            return;
        }
        WAVEFORMATEX* wfx = nullptr;
        UINT32 wfx_size = 0;
        if (FAILED(MFCreateWaveFormatExFromMFMediaType(actual.Get(), &wfx, &wfx_size)) ||
            !wfx) {
            return;
        }
        has_audio_ = audio_.Start(wfx);
        CoTaskMemFree(wfx);
        audio_eof_ = !has_audio_;
    }

    // Keeps roughly a second of audio queued ahead of playback.
    void PumpAudio() {
        using Microsoft::WRL::ComPtr;
        if (!has_audio_ || audio_eof_ || !reader_) return;
        for (int guard = 0; guard < 8 && audio_.Queued() < 8; ++guard) {
            ComPtr<IMFSample> sample;
            DWORD flags = 0;
            LONGLONG ts = 0;
            if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                                           &flags, &ts, sample.GetAddressOf()))) {
                audio_eof_ = true;
                return;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                audio_eof_ = true;
                return;
            }
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) continue;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                if (data && len) {
                    audio_.Submit(std::vector<uint8_t>(data, data + len));
                }
                buf->Unlock();
            }
        }
    }

    // Decodes one video sample into next_rgba_/next_pts_. Returns true only if a
    // frame is now pending; sets eof_ at end of stream.
    bool ReadNext() {
        using Microsoft::WRL::ComPtr;
        if (!reader_ || eof_) return false;

        ComPtr<IMFSample> sample;
        DWORD flags = 0;
        LONGLONG ts = 0;
        if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr,
                                       &flags, &ts, sample.GetAddressOf()))) {
            eof_ = true;
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            eof_ = true;
            return false;
        }
        if (!sample) return false;  // stream tick with no frame; try again next call

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(buf.GetAddressOf()))) return false;

        BYTE* data = nullptr;
        DWORD len = 0;
        if (FAILED(buf->Lock(&data, nullptr, &len))) return false;

        const uint32_t w = width_, h = height_;
        const size_t row = size_t(w) * 4;
        if (data && len >= row * h) {
            next_rgba_.resize(row * h);
            for (uint32_t y = 0; y < h; ++y) {
                const uint32_t sy = bottom_up_ ? (h - 1 - y) : y;
                const BYTE* src = data + size_t(sy) * row;
                uint8_t* dst = next_rgba_.data() + size_t(y) * row;
                for (uint32_t x = 0; x < w; ++x) {  // BGRA -> RGBA, force opaque
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 0xFF;
                }
            }
            next_pts_ = double(ts) / 10000000.0;  // 100ns units -> seconds
            have_next_ = true;
        }
        buf->Unlock();
        return have_next_;
    }

    Microsoft::WRL::ComPtr<IMFSourceReader> reader_;
    AudioOut audio_;
    std::vector<uint8_t> next_rgba_;
    std::chrono::steady_clock::time_point start_{};
    double   next_pts_ = 0.0;
    uint32_t width_ = 0, height_ = 0;
    bool     eof_ = true;
    bool     have_next_ = false;
    bool     bottom_up_ = false;
    bool     has_audio_ = false;
    bool     audio_eof_ = true;
};

// --- Cross-thread control -----------------------------------------------------
// Game-thread code (hooks.cpp) posts a request; the overlay picks it up on the
// render thread. This is the "call a function to play it" entry point.
struct VideoRequest {
    std::mutex  mtx;
    // Kept as a path (natively wide) rather than a narrow string: converting a
    // non-ASCII filename through path::string() throws std::system_error.
    std::filesystem::path path;  // non-empty = start playing this file
    bool        stop = false;    // asked to stop
    bool        loop = false;
    bool        playing = false; // published by the overlay
};

inline VideoRequest& video_request() {
    static VideoRequest r;
    return r;
}

// Start playing `path` (absolute, or resolved by the caller). Safe from any thread.
inline void PlayVideo(const std::filesystem::path& path, bool loop = false) {
    auto& r = video_request();
    std::lock_guard<std::mutex> lk(r.mtx);
    r.path = path;
    r.loop = loop;
    r.stop = false;
}

// Stop whatever is playing. Safe from any thread.
inline void StopVideo() {
    auto& r = video_request();
    std::lock_guard<std::mutex> lk(r.mtx);
    r.path.clear();
    r.stop = true;
}

// True while a video is actually on screen.
inline bool IsVideoPlaying() {
    auto& r = video_request();
    std::lock_guard<std::mutex> lk(r.mtx);
    return r.playing;
}

}  // namespace restuff

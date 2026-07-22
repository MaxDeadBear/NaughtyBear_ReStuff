// PNG -> straight RGBA8 via Windows Imaging Component (no extra thirdparty).
// Used for overlay panel art (assets/menustuff/*). Links windowscodecs+ole32.

#pragma once

#include <cstdint>
#include <filesystem>
#include <vector>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "imgui.h"

namespace restuff {

// Decodes any WIC-supported image file to straight (non-premultiplied) RGBA8.
// Returns false and leaves out_rgba empty on any failure.
inline bool LoadImageRGBA(const std::filesystem::path& path,
                          std::vector<uint8_t>& out_rgba, uint32_t& out_w,
                          uint32_t& out_h) {
    using Microsoft::WRL::ComPtr;
    out_rgba.clear();
    out_w = out_h = 0;

    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_OK/S_FALSE need a balancing CoUninitialize; RPC_E_CHANGED_MODE must
    // leave the thread's existing COM mode alone.
    const bool balance = SUCCEEDED(co);

    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateDecoderFromFilename(
                path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) &&
            SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(
                frame.Get(), GUID_WICPixelFormat32bppRGBA,
                WICBitmapDitherTypeNone, nullptr, 0.0,
                WICBitmapPaletteTypeCustom))) {
            UINT w = 0, h = 0;
            if (SUCCEEDED(converter->GetSize(&w, &h)) && w && h) {
                out_rgba.resize(size_t(w) * h * 4);
                if (SUCCEEDED(converter->CopyPixels(
                        nullptr, w * 4, static_cast<UINT>(out_rgba.size()),
                        out_rgba.data()))) {
                    out_w = w;
                    out_h = h;
                    ok = true;
                } else {
                    out_rgba.clear();
                }
            }
        }
    }
    if (balance) CoUninitialize();
    return ok;
}

// Tight bounding box of pixels with alpha > threshold, as normalized UVs.
// Falls back to the full image if nothing is opaque.
inline void AlphaBoundsUV(const std::vector<uint8_t>& rgba, uint32_t w,
                          uint32_t h, ImVec2& uv0, ImVec2& uv1,
                          uint8_t threshold = 8) {
    uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            if (rgba[(size_t(y) * w + x) * 4 + 3] > threshold) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x0 > x1 || y0 > y1) {
        uv0 = ImVec2(0.0f, 0.0f);
        uv1 = ImVec2(1.0f, 1.0f);
        return;
    }
    uv0 = ImVec2(float(x0) / w, float(y0) / h);
    uv1 = ImVec2(float(x1 + 1) / w, float(y1 + 1) / h);
}

}  // namespace restuff

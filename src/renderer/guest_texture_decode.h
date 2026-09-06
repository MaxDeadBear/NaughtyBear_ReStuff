#pragma once

// CPU decode of guest Xenos textures to RGBA8, for the native Vulkan renderer.
// Formats: k_8 (2), k_8_8_8_8 (6), DXT1/2_3/4_5 (18/19/20). Tiled surfaces are
// untiled with the friend's verified tiling replica. BC blocks are decoded to
// RGBA8 on the CPU (avoids depending on the device's textureCompressionBC
// feature, which VulkanProvider may not have enabled).

#include <cstdint>
#include <vector>

#include "renderer/up_draws.h"

namespace restuff::renderer {

// FNV-1a over the texture's full byte extent (format-aware, capped at 1MB).
// UI textures are streamed in after the first draws reference their address,
// and heap addresses are REUSED across scenes, so the cache must detect any
// content change; a sampled hash collided on transparent-padding atlases.
uint64_t GuestTextureContentHash(const GuestTextureDesc& desc);

// Decodes the texture into tightly-packed RGBA8. Returns false when the
// format is unsupported or the source is unmapped; w/h are clamped to 2048.
bool DecodeGuestTexture(const GuestTextureDesc& desc, std::vector<uint8_t>& rgba_out,
                        uint32_t& width_out, uint32_t& height_out);

// Source-pointer overload for offline tools (no live runtime required).
bool DecodeGuestTexture(const GuestTextureDesc& desc, const uint8_t* gsrc,
                        std::vector<uint8_t>& rgba_out, uint32_t& width_out,
                        uint32_t& height_out);

// Byte offset of block (bx,by) in the guest's tiled layout (M3.89: exported
// for the resolve->guest-RAM writeback, which must SCATTER in tiled order).
uint32_t TiledBlockByteOffset(uint32_t bx, uint32_t by, uint32_t pitch_blocks,
                              uint32_t bytes_per_block);

// M4.3: native-BC upload path. For DXT formats (18/19/20), byteswaps + untiles
// the raw guest blocks into tightly-packed standard little-endian BC1/2/3
// block rows (ready for vkCmdCopyBufferToImage on a BC image) WITHOUT decoding
// to RGBA8 -- skips the palette/alpha decode and shrinks the upload 4-8x.
// Only usable when the device enables textureCompressionBC (which guarantees
// the whole BC family for sampled images); DecodeGuestTexture remains the
// fallback. Returns false for non-DXT formats or an unmapped source.
bool CopyGuestBCBlocks(const GuestTextureDesc& desc, std::vector<uint8_t>& blocks_out,
                       uint32_t& width_out, uint32_t& height_out);

}  // namespace restuff::renderer

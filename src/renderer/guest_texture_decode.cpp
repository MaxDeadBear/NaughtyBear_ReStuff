#include "renderer/guest_texture_decode.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <rex/runtime.h>

// Tiling replica + per-format decode ported from the friend's verified
// native_backend.cpp:526-685 ("NB:"), with BC1/2/3 additionally decoded to
// RGBA8 on the CPU (the friend uploaded native BC blocks to D3D12).

namespace restuff::renderer {
namespace {

// --- Xenos 2D tiling (NB:526-550, verbatim) -----------------------------------

uint32_t TiledOffset2DRow(uint32_t y, uint32_t width_blocks, uint32_t log2_bpp) {
  const uint32_t macro = ((y / 32) * (width_blocks / 32)) << (log2_bpp + 7);
  const uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bpp)) +
         ((y & 1) << 4);
}

uint32_t TiledOffset2DColumn(uint32_t x, uint32_t y, uint32_t log2_bpp, uint32_t base_offset) {
  const uint32_t macro = (x / 32) << (log2_bpp + 7);
  const uint32_t micro = (x & 7) << log2_bpp;
  const uint32_t offset = base_offset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) +
         ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

}  // namespace

// Byte offset of tiled block (bx, by) for the given block size. (M3.89:
// namespace-scope, exported for the resolve writeback.)
uint32_t TiledBlockByteOffset(uint32_t bx, uint32_t by, uint32_t pitch_blocks,
                              uint32_t bytes_per_block) {
  pitch_blocks = (pitch_blocks + 31) & ~31u;  // tile rows are 32 blocks wide
  const uint32_t log2_bpp =
      (bytes_per_block / 4) + ((bytes_per_block / 2) >> (bytes_per_block / 4));
  const uint32_t row = TiledOffset2DRow(by, pitch_blocks, log2_bpp);
  const uint32_t element = TiledOffset2DColumn(bx, by, log2_bpp, row) >> log2_bpp;
  return element * bytes_per_block;
}

namespace {

// --- BC block decode (blocks are standard little-endian AFTER the guest's
// per-16-bit byteswap, NB:662-667) ----------------------------------------------

void DecodeColorEndpoints(uint16_t c0, uint16_t c1, uint8_t pal[4][4], bool bc1_mode) {
  auto expand = [](uint16_t c, uint8_t* out) {
    out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
    out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
    out[2] = uint8_t((c & 31) * 255 / 31);
    out[3] = 255;
  };
  expand(c0, pal[0]);
  expand(c1, pal[1]);
  if (!bc1_mode || c0 > c1) {  // 4-color mode (BC2/BC3 color is always 4-color)
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = uint8_t((2 * pal[0][i] + pal[1][i]) / 3);
      pal[3][i] = uint8_t((pal[0][i] + 2 * pal[1][i]) / 3);
    }
    pal[2][3] = pal[3][3] = 255;
  } else {  // 3-color + transparent
    for (int i = 0; i < 3; ++i) {
      pal[2][i] = uint8_t((pal[0][i] + pal[1][i]) / 2);
      pal[3][i] = 0;
    }
    pal[2][3] = 255;
    pal[3][3] = 0;
  }
}

// blk: 8 bytes (BC1 color block). out: 4x4 RGBA8 texels.
void DecodeBC1Block(const uint8_t* blk, uint8_t out[16][4], bool bc1_mode) {
  uint16_t c0, c1;
  std::memcpy(&c0, blk + 0, 2);
  std::memcpy(&c1, blk + 2, 2);
  uint8_t pal[4][4];
  DecodeColorEndpoints(c0, c1, pal, bc1_mode);
  for (int row = 0; row < 4; ++row) {
    const uint8_t bits = blk[4 + row];
    for (int col = 0; col < 4; ++col) {
      const uint8_t idx = (bits >> (col * 2)) & 3;
      std::memcpy(out[row * 4 + col], pal[idx], 4);
    }
  }
}

// blk: 16 bytes (BC3). alpha block first, then BC1-style color.
void DecodeBC3Block(const uint8_t* blk, uint8_t out[16][4]) {
  DecodeBC1Block(blk + 8, out, /*bc1_mode=*/false);
  const uint8_t a0 = blk[0], a1 = blk[1];
  uint8_t apal[8];
  apal[0] = a0;
  apal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i < 7; ++i) apal[1 + i] = uint8_t(((7 - i) * a0 + i * a1) / 7);
  } else {
    for (int i = 1; i < 5; ++i) apal[1 + i] = uint8_t(((5 - i) * a0 + i * a1) / 5);
    apal[6] = 0;
    apal[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(blk[2 + i]) << (8 * i);
  for (int t = 0; t < 16; ++t) {
    out[t][3] = apal[(bits >> (t * 3)) & 7];
  }
}

// blk: 16 bytes (BC2). explicit 4-bit alpha, then BC1-style color.
void DecodeBC2Block(const uint8_t* blk, uint8_t out[16][4]) {
  DecodeBC1Block(blk + 8, out, /*bc1_mode=*/false);
  for (int t = 0; t < 16; ++t) {
    const uint8_t byte = blk[t / 2];
    const uint8_t nib = (t & 1) ? (byte >> 4) : (byte & 0xF);
    out[t][3] = uint8_t(nib * 17);
  }
}

}  // namespace

uint64_t GuestTextureContentHash(const GuestTextureDesc& desc) {
  // Hash the texture's FULL byte extent. A sampled hash (first 128B + 128B at
  // +4KB) collided on UI atlases whose sampled windows are transparent-black
  // padding (all-zero DXT blocks): when the engine streams a new asset into a
  // reused heap address, the stale cached decode survived — the title-screen
  // hills strip rendered as a leftover intro starburst.
  uint64_t hash = 1469598103934665603ull;  // FNV offset
  auto* runtime = rex::Runtime::instance();
  const uint8_t* src = runtime && runtime->memory()
                           ? runtime->memory()->TranslatePhysical<const uint8_t*>(desc.phys_addr)
                           : nullptr;
  if (!src) return hash;
  const uint32_t w = std::min(std::max(desc.width, desc.pitch_texels), 2048u);
  const uint32_t h = std::min(desc.height, 2048u);
  uint32_t bytes;
  if (desc.format == 18) bytes = (w / 4) * (h / 4) * 8;  // DXT1
  else if (desc.format == 19 || desc.format == 20) bytes = (w / 4) * (h / 4) * 16;  // DXT3/5
  else bytes = w * h * 4;
  bytes = std::min(bytes, 1u << 20);
  // Four independent FNV lanes: the single-lane loop is a serial multiply
  // dependency chain (~1GB/s) and was the bulk of draw prep. Lanes cover the
  // same full byte extent -- coverage identical, just ILP. (Do NOT go back to
  // sampled hashing to save time; see the atlas-collision note above.)
  const uint64_t* s64 = reinterpret_cast<const uint64_t*>(src);
  const uint32_t n = bytes / 8;
  constexpr uint64_t kP = 1099511628211ull;
  uint64_t h0 = hash, h1 = hash ^ 0x9E3779B97F4A7C15ull, h2 = hash ^ 0xC2B2AE3D27D4EB4Full,
           h3 = hash ^ 0x165667B19E3779F9ull;
  uint32_t i = 0;
  for (; i + 4 <= n; i += 4) {
    h0 = (h0 ^ s64[i]) * kP;
    h1 = (h1 ^ s64[i + 1]) * kP;
    h2 = (h2 ^ s64[i + 2]) * kP;
    h3 = (h3 ^ s64[i + 3]) * kP;
  }
  for (; i < n; ++i) h0 = (h0 ^ s64[i]) * kP;
  hash = (((h0 * kP ^ h1) * kP ^ h2) * kP ^ h3);
  hash = (hash ^ desc.width) * 1099511628211ull;
  hash = (hash ^ desc.height) * 1099511628211ull;
  hash = (hash ^ desc.format) * 1099511628211ull;
  return hash;
}

bool DecodeGuestTexture(const GuestTextureDesc& desc, std::vector<uint8_t>& rgba_out,
                        uint32_t& width_out, uint32_t& height_out) {
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  return DecodeGuestTexture(desc,
                            runtime->memory()->TranslatePhysical<const uint8_t*>(desc.phys_addr),
                            rgba_out, width_out, height_out);
}

// Source-pointer overload so offline tools (nb_ps_exec) can decode trace
// captures without a live runtime.
bool DecodeGuestTexture(const GuestTextureDesc& desc, const uint8_t* gsrc,
                        std::vector<uint8_t>& rgba_out, uint32_t& width_out,
                        uint32_t& height_out) {
  const uint32_t w = std::min(desc.width, 2048u);
  const uint32_t h = std::min(desc.height, 2048u);
  if (w == 0 || h == 0) return false;
  if (!gsrc) return false;

  rgba_out.assign(size_t(w) * h * 4, 0);
  width_out = w;
  height_out = h;

  if (desc.format == 2) {  // k_8: glyph/intensity atlas -> white with alpha
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        uint32_t src_off;
        if (desc.tiled) {
          src_off = TiledBlockByteOffset(x, y, desc.pitch_texels, 1);
        } else {
          src_off = y * desc.pitch_texels + x;
        }
        uint8_t* d = rgba_out.data() + (size_t(y) * w + x) * 4;
        d[0] = d[1] = d[2] = 255;
        d[3] = gsrc[src_off];
      }
    }
    return true;
  }

  if (desc.format == 6) {  // k_8_8_8_8: raw LE read is GPU-ready 0xAARRGGBB
    for (uint32_t y = 0; y < h; ++y) {
      for (uint32_t x = 0; x < w; ++x) {
        uint32_t src_off;
        if (desc.tiled) {
          src_off = TiledBlockByteOffset(x, y, desc.pitch_texels, 4);
        } else {
          src_off = (y * desc.pitch_texels + x) * 4;
        }
        uint32_t c;
        std::memcpy(&c, gsrc + src_off, 4);
        uint8_t* d = rgba_out.data() + (size_t(y) * w + x) * 4;
        d[0] = uint8_t((c >> 16) & 0xFF);  // R
        d[1] = uint8_t((c >> 8) & 0xFF);   // G
        d[2] = uint8_t(c & 0xFF);          // B
        d[3] = uint8_t((c >> 24) & 0xFF);  // A
      }
    }
    return true;
  }

  if (desc.format == 18 || desc.format == 19 || desc.format == 20) {  // DXT1/2_3/4_5
    const uint32_t block_bytes = desc.format == 18 ? 8 : 16;
    const uint32_t blocks_w = (w + 3) / 4;
    const uint32_t blocks_h = (h + 3) / 4;
    const uint32_t pitch_blocks = std::max(1u, (desc.pitch_texels + 3) / 4);
    uint8_t blk[16];
    uint8_t texels[16][4];
    for (uint32_t by = 0; by < blocks_h; ++by) {
      for (uint32_t bx = 0; bx < blocks_w; ++bx) {
        uint32_t src_off;
        if (desc.tiled) {
          src_off = TiledBlockByteOffset(bx, by, pitch_blocks, block_bytes);
        } else {
          src_off = (by * pitch_blocks + bx) * block_bytes;
        }
        // Guest blocks store 16-bit words byteswapped; swapping restores the
        // standard little-endian BC layout (NB:662-667).
        for (uint32_t k = 0; k < block_bytes; k += 2) {
          blk[k] = gsrc[src_off + k + 1];
          blk[k + 1] = gsrc[src_off + k];
        }
        if (desc.format == 18) {
          DecodeBC1Block(blk, texels, /*bc1_mode=*/true);
        } else if (desc.format == 19) {
          DecodeBC2Block(blk, texels);
        } else {
          DecodeBC3Block(blk, texels);
        }
        for (uint32_t ty = 0; ty < 4; ++ty) {
          const uint32_t y = by * 4 + ty;
          if (y >= h) break;
          for (uint32_t tx = 0; tx < 4; ++tx) {
            const uint32_t x = bx * 4 + tx;
            if (x >= w) break;
            std::memcpy(rgba_out.data() + (size_t(y) * w + x) * 4, texels[ty * 4 + tx], 4);
          }
        }
      }
    }
    return true;
  }

  return false;  // unsupported format -> caller renders flat (white)
}

// M4.3: see header. The per-16-bit byteswap and the tiled-block gather are the
// SAME steps the RGBA8 decode above starts with (NB:662-667); this just stops
// there and emits the linear BC block stream instead of decoding it.
bool CopyGuestBCBlocks(const GuestTextureDesc& desc, std::vector<uint8_t>& blocks_out,
                       uint32_t& width_out, uint32_t& height_out) {
  if (desc.format != 18 && desc.format != 19 && desc.format != 20) return false;
  auto* runtime = rex::Runtime::instance();
  if (!runtime || !runtime->memory()) return false;
  const uint8_t* gsrc = runtime->memory()->TranslatePhysical<const uint8_t*>(desc.phys_addr);
  if (!gsrc) return false;
  const uint32_t w = std::min(desc.width, 2048u);
  const uint32_t h = std::min(desc.height, 2048u);
  if (w == 0 || h == 0) return false;
  const uint32_t block_bytes = desc.format == 18 ? 8 : 16;
  const uint32_t blocks_w = (w + 3) / 4;
  const uint32_t blocks_h = (h + 3) / 4;
  const uint32_t pitch_blocks = std::max(1u, (desc.pitch_texels + 3) / 4);
  blocks_out.resize(size_t(blocks_w) * blocks_h * block_bytes);
  width_out = w;
  height_out = h;
  for (uint32_t by = 0; by < blocks_h; ++by) {
    for (uint32_t bx = 0; bx < blocks_w; ++bx) {
      uint32_t src_off;
      if (desc.tiled) {
        src_off = TiledBlockByteOffset(bx, by, pitch_blocks, block_bytes);
      } else {
        src_off = (by * pitch_blocks + bx) * block_bytes;
      }
      uint8_t* dst = blocks_out.data() + (size_t(by) * blocks_w + bx) * block_bytes;
      for (uint32_t k = 0; k < block_bytes; k += 2) {
        dst[k] = gsrc[src_off + k + 1];
        dst[k + 1] = gsrc[src_off + k];
      }
    }
  }
  return true;
}

}  // namespace restuff::renderer

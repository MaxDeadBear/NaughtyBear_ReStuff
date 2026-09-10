#pragma once

// Texture dumping + replacement for the native renderer.
//
// IDENTITY IS THE CONTENT HASH, NOT THE ADDRESS. Guest heap addresses are
// reused across scenes (the texture cache exists precisely to notice that), so
// phys_addr names a slot, not a texture: keying on it dumps only the first
// texture to land at each address and would apply a replacement to whatever
// happens to occupy that slot later. GuestTextureContentHash is already
// computed for every texture on the upload path, is content-derived, and is
// stable across runs -- so it is the file name.
//
// Both are cvars, so they live in restuff.toml next to the exe (env vars of
// the same name still work as a one-off override):
//
//   tex_dump = true   write every decoded texture to texture_dump/<hash>.tga
//   tex_mods = true   load replacements from texture_mods/<hash>.*
//
// tex_mods is live; tex_dump is latched at the first texture upload because
// it also vetoes the native-BC path, and flipping that mid-session
// would dump raw BC blocks as though they were RGBA.
//
// A replacement may be ANY size: guest UVs are normalised, so a 4x upscale
// needs no other change. It is uploaded as plain RGBA8, which is why the
// caller must disable the native-BC path for replaced textures.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace restuff::renderer {

namespace texmod {

inline std::filesystem::path ModDir() { return "texture_mods"; }
inline std::filesystem::path DumpDir() { return "texture_dump"; }

inline std::string HashName(uint64_t hash) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(hash));
  return buf;
}

// --- dump ------------------------------------------------------------------
// Writes texture_dump/<hash>.<ext>, where the extension follows the
// tex_dump_format cvar: "tga" (default; hand-rolled, works everywhere) or
// "png" (smaller, via WIC -- Windows only, falls back to TGA elsewhere).
bool WriteDump(uint64_t hash, const uint8_t* rgba, uint32_t w, uint32_t h);

// Already dumped in a previous session? Dumping is a capture pass you leave
// running for a while, so re-writing hundreds of identical files on every
// launch is pure cost.
bool DumpExists(uint64_t hash);

// --- mip chain --------------------------------------------------------------
// Builds a complete RGBA8 mip pyramid by successive 2x2 box filtering and
// returns the level count; `out` receives level 0 followed by every smaller
// level, contiguously, ready for one staging buffer.
//
// Done on the CPU deliberately: vkCmdBlitImage is NOT in the SDK's device
// function table (only vkCmdCopyBufferToImage is), so the usual GPU
// blit-down cannot be used without editing SDK headers. Replacements are
// decoded once and cached, so the filtering cost is amortised, and uploading
// N levels needs only the copy entry point that already exists.
//
// This matters for upscaled art: a 4K replacement minified to a few screen
// pixels with no mip chain wrecks GPU texture-cache locality and shimmers.
uint32_t BuildMipChain(const uint8_t* rgba, uint32_t w, uint32_t h, std::vector<uint8_t>& out);

// Uncompressed 32-bit TGA. One file carrying alpha, unlike the older
// PPM+PGM pair, and every image editor opens it. Bottom-up origin is the TGA
// default, so rows are written last-to-first.
inline bool WriteTGA(const std::filesystem::path& path, const uint8_t* rgba, uint32_t w,
                     uint32_t h) {
  if (!rgba || !w || !h) return false;
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
#ifdef _WIN32
  FILE* f = _wfopen(path.wstring().c_str(), L"wb");
#else
  FILE* f = std::fopen(path.string().c_str(), "wb");
#endif
  if (!f) return false;
  uint8_t hdr[18] = {};
  hdr[2] = 2;                                    // uncompressed true-colour
  hdr[12] = uint8_t(w & 0xFF); hdr[13] = uint8_t(w >> 8);
  hdr[14] = uint8_t(h & 0xFF); hdr[15] = uint8_t(h >> 8);
  hdr[16] = 32;                                  // bits per pixel
  hdr[17] = 8;                                   // 8 alpha bits
  std::fwrite(hdr, 1, sizeof(hdr), f);
  std::vector<uint8_t> row(size_t(w) * 4);
  for (uint32_t y = 0; y < h; ++y) {
    const uint8_t* src = rgba + size_t(h - 1 - y) * w * 4;
    for (uint32_t x = 0; x < w; ++x) {          // RGBA -> BGRA
      row[x * 4 + 0] = src[x * 4 + 2];
      row[x * 4 + 1] = src[x * 4 + 1];
      row[x * 4 + 2] = src[x * 4 + 0];
      row[x * 4 + 3] = src[x * 4 + 3];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

// --- replacement lookup ----------------------------------------------------
// Loaded lazily and remembered BOTH ways: a hit keeps the decoded pixels, a
// miss is recorded too. Without the negative cache every content-changed
// re-decode of an unmodded texture would stat the filesystem again, and the
// glyph atlas alone re-decodes ~20-28 times a second.
struct Replacement {
  bool present = false;
  // Level 0 followed by every smaller level, contiguously -- the finished
  // pyramid, built ONCE on the loader thread. It used to be rebuilt on the
  // render thread at every re-resolve, which with ~900 evict/re-decode
  // events per session meant millions of pixels of box filtering plus a
  // multi-megabyte allocation on the frame path.
  std::vector<uint8_t> rgba;
  uint32_t w = 0, h = 0;
  uint32_t mip_levels = 1;
};

// ASYNCHRONOUS. Returns null when the replacement is not decoded YET -- the
// caller should just use the guest texture; the load is queued and the
// texture re-resolves automatically once it lands (the loader bumps that
// hash's generation). The render thread never decodes.
//
// shared_ptr, not a raw pointer: this is called from several render threads
// while PollModDir erases entries on another, so callers must own what they
// read for as long as they read it.
// Optional generation is captured under the same lock as the pixels. Stamp
// uploaded textures with this value so a concurrent reload is not lost.
std::shared_ptr<const Replacement> FindReplacement(uint64_t hash,
                                                 uint32_t* generation = nullptr);

// LIVE RELOAD. A file changing in texture_mods/ must invalidate the texture
// that uses it -- and ONLY that texture. The first cut bumped a single
// global generation stamped on every TexEntry, which invalidated the whole
// cache: with ~700 live textures that is a full re-hash and CPU re-decode of
// the corpus, i.e. a minute of stalling on every save, not a hitch.
//
// So generations are per-hash. Mod file names ARE the content hash, so a
// directory diff names exactly which textures went stale.
//
// Cost at the call site is one atomic load in the common case: an entry
// carrying the current GLOBAL generation cannot be stale, so the per-hash
// table is consulted only in the window after a change, once per texture,
// after which the entry is re-stamped.
uint32_t ModGeneration();                  // bumps on ANY change (cheap gate)
uint32_t ModGenerationFor(uint64_t hash);  // bumps only for that texture

// Publishes the enabled state and wakes the loader on transitions. Directory
// scanning runs on that worker at 4 Hz; this never touches the filesystem.
void PollModDir();

// Live cvar / environment override, independent of directory existence.
bool ModsEnabled();

}  // namespace texmod

}  // namespace restuff::renderer

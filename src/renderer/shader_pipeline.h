// M2.3 Stage 2: runtime GLSL -> SPIR-V compilation and translated-shader cache.
//
// The ucode translator (ucode_translator.h) turns guest microcode into GLSL;
// this module compiles that GLSL to SPIR-V at runtime via libshaderc (which
// statically bundles glslang, so no external toolchain is needed on end-user
// machines -- ship libshaderc_shared alongside the exe). It also caches
// translated+compiled shaders by guest ucode hash.
#pragma once

#include <cstdint>
#include <vector>

#include "renderer/ucode_translator.h"

namespace restuff::renderer::spc {

// Compile a GLSL 4.50 source string to SPIR-V. `is_vertex` selects the stage.
// Returns empty on failure (error is logged).
std::vector<uint32_t> CompileGlsl(const char* glsl, bool is_vertex, bool is_compute = false);

// A translated + compiled guest shader, cached by ucode hash. `spirv` is empty
// if translation or compilation failed (then `valid` is false).
struct CachedShader {
  ucode::TranslatedShader t;
  std::vector<uint32_t> spirv;
  // M3.60: compiled SPIR-V of the compute winding-probe (t.probe_glsl). Empty
  // for pixel shaders and skinned VSes. The renderer dispatches it once per
  // shader to classify winding and flip inverted (backface-culled) draws.
  std::vector<uint32_t> probe_spirv;
  bool valid = false;
};

// Get-or-create the translation+SPIR-V for a guest shader, keyed by `hash`.
// `be_words`/`num_dwords` are the big-endian guest microcode. Translated and
// compiled once per hash; subsequent calls return the cache entry. The returned
// reference is stable for the process lifetime.
const CachedShader& GetShader(uint64_t hash, bool is_vertex, const uint32_t* be_words,
                              uint32_t num_dwords);

// Lookup-only: returns the cached entry for `hash`, or nullptr if never
// translated. The draw path uses this (it has the hash but not the ucode).
const CachedShader* GetCachedShader(uint64_t hash);

// One-time smoke test: compiles a trivial VS+FS in-process to prove libshaderc
// is linked and functional. Logs the outcome. Safe to call at startup.
bool RuntimeCompileSelfTest();

// M4.40: persistent SPIR-V cache ("offline shader compilation").
//
// Measured on a level route: 248 shaders cost 19 ms to TRANSLATE and 930 ms to
// COMPILE -- shaderc is 98% of the bill, and it is paid in bursts as new
// shaders appear, which is the choppy-open stutter. Caching the compiled SPIR-V
// on disk removes essentially all of it, while the translator still runs (19 ms
// total) so every consumer of TranslatedShader metadata -- attrs, textures,
// vfetch_count -- keeps working untouched. That is why this caches SPIR-V only:
// 98% of the win for none of the metadata-serialisation risk.
//
// The key is a hash of the GENERATED GLSL plus the flags that change codegen,
// NOT the guest ucode hash. That makes staleness structurally impossible: edit
// the translator and the GLSL changes, so the key changes and the entry is
// recompiled. No version stamp to forget to bump.
//
// Ship a populated file next to the exe and a user's first run pays nothing.
void LoadShaderSpvCache();  // safe to call once at startup; also lazily self-loads
void SaveShaderSpvCache();  // no-op unless new entries were compiled this run
// M4.45: identity of the shader-emitting code in this build (translator +
// pipeline TU compile stamps). Both on-disk caches are stamped with it and
// discarded on mismatch, so an updated build never carries dead entries.
uint64_t ShaderBuildId();

}  // namespace restuff::renderer::spc

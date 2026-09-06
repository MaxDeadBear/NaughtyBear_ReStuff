// M2.3: Xenos microcode -> GLSL shader translator.
//
// Replaces the hand-written kPm4Decls[] vertex-declaration heuristics and the
// three fixed fragment shaders with a general translator that reads captured
// guest vertex/pixel shader microcode and emits an equivalent GLSL 4.50 shader
// plus the metadata the draw layer needs to bind it (vertex attribute layout,
// texture slots, interpolator usage). The GLSL is compiled to SPIR-V at runtime.
//
// Front-end (CF walk + vertex/texture fetch decode) is ported from the proven
// tools/nb_ucode_dump.cpp disassembler. Back-end (register-file ALU emission)
// is the new part.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace restuff::renderer::ucode {

// One vertex attribute, derived from a VS vertex-fetch instruction. Each fetch
// pulls one register (r#) from a vertex stream at a byte offset; the draw layer
// turns this into a VkVertexInputAttributeDescription and the GPU does the
// format unpack, so the emitted GLSL just reads an `in` of matching component
// count and applies the fetch destination swizzle.
struct VertexAttr {
  uint32_t fetch_slot = 0;    // fetch constant index (vertex stream binding)
  uint32_t dest_reg = 0;      // GPR the fetch writes -> maps to a GLSL `in`
  uint32_t byte_offset = 0;   // offset within the vertex, in bytes
  uint32_t stride_bytes = 0;  // vertex stride in bytes (per stream)
  uint32_t format = 0;        // xenos::VertexFormat value
  uint32_t dst_swizzle = 0;   // 12-bit fetch destination swizzle (4x3 bits)
  bool is_signed = false;
  bool is_normalized = false;
  uint32_t location = 0;      // assigned GLSL input location
};

// One texture sample, derived from a PS texture-fetch instruction.
struct TextureBinding {
  uint32_t fetch_slot = 0;   // fetch constant index (texture slot)
  uint32_t dest_reg = 0;     // GPR the sample result is written to
  uint32_t src_reg = 0;      // GPR holding the coordinates
  uint32_t src_swizzle = 0;  // coordinate source swizzle
  uint32_t dst_swizzle = 0;  // result destination swizzle
  uint32_t dimension = 1;    // 0=1D 1=2D 2=3D 3=cube
};

struct TranslatedShader {
  bool ok = false;
  std::string glsl;
  // M3.60: a COMPUTE variant of the VS that runs the same fetch+transform on 3
  // supplied vertices (attributes pre-decoded into an SSBO) and writes their
  // clip positions -- the renderer reads them back to detect inverted winding
  // and flip the front-face for backface-culled draws. Empty for pixel shaders
  // and for skinned (rel-fetch) vertex shaders (which are excluded from the
  // probe; their winding is already handled by M3.41).
  std::string probe_glsl;
  std::string error;

  // VS outputs.
  std::vector<VertexAttr> attrs;
  bool exports_position = false;
  bool exports_point_size = false;
  // M3.138: how many vertex-fetch instructions the shader actually contains,
  // counted regardless of whether any became an attribute. This distinguishes
  // the two very different meanings of "attrs is empty":
  //   vfetch_count > 0  -> the shader DOES fetch vertices and we failed to
  //                        resolve them: a detection failure, drop the draw.
  //   vfetch_count == 0 -> the shader is PROCEDURAL, building every vertex from
  //                        constants and the vertex index. Legitimate and must
  //                        be drawn -- this title's particle billboards (quad
  //                        lists, 12-196 indices) are exactly this.
  uint32_t vfetch_count = 0;
  // Position is computed FROM SHADER CONSTANTS (matrix transform) rather than
  // passed through from the vertex stream. Matrix VSes emit clip-space however
  // large their raw inputs are, so the window-space (maxabs>2) capture
  // heuristic must not re-map them through the viewport.
  bool pos_reads_consts = false;
  // M3.11/M3.14: fetch slots read by REGISTER-RELATIVE vertex fetches (vfetch
  // whose index comes from a GPR: bone-data lookups, morph-target neighbor
  // reads) -- served to the VS as storage buffers, not vertex attributes.
  // Up to two distinct slots (bind points 2 and 3). ~0u = none.
  uint32_t rel_fetch_slot = ~0u;
  uint32_t rel_fetch_slot2 = ~0u;

  // PS outputs.
  std::vector<TextureBinding> textures;
  uint32_t color_export_mask = 0;  // bit i set => o[i] color target written
  bool uses_latch = false;         // RESTUFF_PS_LATCH captured a mid-shader reg
  // PS writes gl_FragDepth (export 61) -- the game's depth-restore passes.
  bool exports_depth = false;

  // Shared: which interpolators (o0..o15) the shader reads (PS) / writes (VS).
  uint32_t interpolator_mask = 0;
};

// Translate a big-endian guest VS/PS microcode blob to GLSL + metadata.
TranslatedShader TranslateVertexShader(const uint32_t* be_words, uint32_t num_dwords);
TranslatedShader TranslatePixelShader(const uint32_t* be_words, uint32_t num_dwords);

// M4.45: compile stamp of the translator TU. Changes whenever the translator
// (or a header it includes) is rebuilt -- the signal that cached SPIR-V may
// no longer match what this build would emit.
const char* TranslatorBuildStamp();

}  // namespace restuff::renderer::ucode

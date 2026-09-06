#pragma once

// Shared data path between the guest-D3D UP-draw capture (guest_d3d_hooks.cpp,
// runs on the guest render thread) and the native Vulkan renderer
// (native_vk.cpp present thread). Capture decodes each user-primitive draw to
// host-renderable 2D triangles; the renderer consumes whole frames.

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace restuff::renderer {

// Matches the Vulkan pipeline's vertex input: pos = D3D-style NDC (y-up),
// uv normalized, color R8G8B8A8 (little-endian word = R | G<<8 | B<<16 | A<<24).
struct Draw2DVertex {
  float x, y;
  float u, v;
  uint32_t rgba;   // color1: base/mult colour
  uint32_t rgba2;  // color2: .r = texture lerp factor, .a = alpha gate (quad family)
};
static_assert(sizeof(Draw2DVertex) == 24);

// The guest texture bound to a draw, as parsed from the device's staged fetch
// record. The renderer decodes + caches it from guest physical memory.
struct GuestTextureDesc {
  bool valid = false;
  uint32_t phys_addr = 0;      // base_address << 12
  uint32_t width = 0, height = 0;
  uint32_t pitch_texels = 0;
  uint32_t format = 0;         // xenos TextureFormat numeric (2/6/18/19/20 supported)
  uint32_t endian = 0;
  bool tiled = false;
  // Fetch-constant clamp modes (dword0 bits 10-12/13-15): 0 = repeat,
  // 1 = mirrored repeat, 2+ = clamp variants. Backdrop grid cells rely on
  // repeat (neighbor cells address UV 1..2 of a shared texture).
  uint32_t clamp_x = 2, clamp_y = 2;
  bool wants_wrap() const { return clamp_x <= 1 || clamp_y <= 1; }
  // Xenos per-channel "sign" field (dword0 bits 2..9, 2 bits each): 0=unsigned,
  // 1=signed, 2=bias, 3=GAMMA (sRGB). RGB=gamma => the texture is sRGB-encoded
  // and must be linearized on sample; alpha stays linear (matches Vulkan _SRGB).
  bool gamma = false;
  // Xenos fetch-constant dword_3 EXP_ADJUST (signed 6-bit at bit 13): every
  // fetched value is scaled by 2^exp_adjust. Ignoring it made the sun-shaft's
  // depth fetch read ~32x too large, so its world reconstruction landed inside
  // the sun's falloff at EVERY pixel -> a uniform screen-wide darkening
  // (occ 0.36 instead of ~0) = the global world dim. The SDK applies this in
  // both its SPIR-V translator and its CPU interpreter (ldexp(1, exp_adjust)).
  int32_t exp_adjust = 0;
  float exp_adjust_scale() const { return std::ldexp(1.0f, exp_adjust); }
};

// Blend mode classified from the shadowed RB_BLENDCONTROL0 at draw time.
enum class BlendMode : uint32_t {
  kStandard,  // srcAlpha / invSrcAlpha
  kAdditive,  // one|srcAlpha / one
  kPremul,    // one / invSrcAlpha
};

// Shader family, from the disassembled ucode corpus (tools/nb_ucode_dump).
enum class DrawFamily : uint32_t {
  kQuad,  // VS EE34A8BA/34647... quad: c = mix(color1, tex, color2.r); a *= color2.a
  kText,  // PS 5A6850E0 text: rgb = color1.rgb; a = color1.a * tex.a
};

// One captured draw, already triangulated (triangle list).
struct DecodedDraw {
  std::vector<Draw2DVertex> verts;
  GuestTextureDesc tex;
  DrawFamily family = DrawFamily::kQuad;
  BlendMode blend = BlendMode::kStandard;
};

// M3.0: one vertex stream of a draw (per vertex-fetch constant slot). 2D UI
// draws use a single stream; 3D skinned meshes fetch from two (mesh data in
// one, blend weights/indices in a second 16-byte stream).
// M3.4: the payload is SHARED between draws -- 3D scenes issue hundreds of
// draws over the same multi-MB static vertex buffer, and per-draw copies
// exploded to GBs/frame (walker crawl + 2GB RSS + frozen presentation). The
// capture dedups by (vb_phys, stride, vs_hash) per guest frame; the uploader
// dedups by payload pointer per host frame.
struct VtxStream {
  uint32_t fetch_slot = 0;  // vertex fetch constant index
  uint32_t stride = 0;      // bytes per vertex
  std::shared_ptr<const std::vector<uint8_t>> data;  // LE-normalized guest VB region
  const std::vector<uint8_t>& bytes() const { return *data; }
};

// M4.2: shared ALU-constant snapshot. Consecutive draws usually see ZERO
// constant writes between them (the SHAFTCONST probe's cw_since=0
// observation), so the capture shares ONE immutable 4KB block across draws
// whose (const base, write generation) match, instead of malloc + 8KB
// zero-fill + 8KB memcpy per draw (~12-16MB/frame of churn, freed on the
// OTHER thread). Sharing is keyed on a write-counting generation stamp in
// WriteShadowRegister -- exact by construction, never content-sampled (the
// M4.1 stale-bone-pose lesson). The read API mirrors the std::vector subset
// the existing ~60 consumers use; the one mutation site (RESTUFF_SHAFT_OFF)
// must go through set(), which detaches a private copy first.
class ConstBank {
 public:
  static constexpr size_t kFloats = 256 * 4;
  using Block = std::array<float, kFloats>;
  size_t size() const { return sp_ ? kFloats : 0; }
  bool empty() const { return !sp_; }
  const float* data() const { return sp_ ? sp_->data() : nullptr; }
  float operator[](size_t i) const { return (*sp_)[i]; }
  void reset(std::shared_ptr<const Block> b) { sp_ = std::move(b); }
  // Identity of the shared block: equal pointers <=> captured under the same
  // (base, generation) key <=> byte-identical constants AND no bool/loop
  // writes in between (the generation covers 0x4900..0x4927 too). The UBO
  // ring dedup keys on exactly this.
  const void* ident() const { return sp_.get(); }
  void set(size_t i, float v) {  // copy-on-write; capture thread only
    if (!sp_) return;
    auto own = std::make_shared<Block>(*sp_);
    (*own)[i] = v;
    sp_ = std::move(own);
  }

 private:
  std::shared_ptr<const Block> sp_;
};

// M2.3: a captured draw in RAW form for the ucode-translator render path. The
// guest vertex buffer is uploaded verbatim and transformed on-GPU by the
// translated vertex shader; guest constants and textures are bound as the
// shader expects. (Contrast DecodedDraw, which is CPU-transformed for the
// fixed heuristic shaders.) Gated behind the use_translated_shaders cvar.
constexpr uint32_t kMaxTexSlots = 8;  // PS texture fetch slots seen in the corpus: 0..7
struct RawGuestDraw {
  uint64_t vs_hash = 0, ps_hash = 0;
  uint32_t prim = 0;               // xenos primitive type
  std::vector<VtxStream> streams;  // one per fetch slot the VS reads, in
                                   // first-appearance order of the VS attrs
  // M3.11: whole payload of the VS's register-relative fetch stream (skinning
  // bone data), served as a storage buffer -- NOT a vertex binding. ~0u=none.
  uint32_t rel_stream_slot = ~0u;
  std::shared_ptr<const std::vector<uint8_t>> rel_stream_data;
  // M3.101: second rel-fetch slot (skinned shadow volumes use vfd AND vfd2).
  uint32_t rel_stream_slot2 = ~0u;
  std::shared_ptr<const std::vector<uint8_t>> rel_stream_data2;
  // M3.45: index list, SHARED cross-draw (was a deep std::vector copy per draw
  // at ~43% of the per-draw capture cost -- static world strips repeat the same
  // widened index array every frame, already cached in a shared_ptr). Read via
  // idx(); it returns an empty vector when unset (resolve draws carry none).
  std::shared_ptr<const std::vector<uint32_t>> indices_sp;
  const std::vector<uint32_t>& idx() const {
    static const std::vector<uint32_t> kEmpty;
    return indices_sp ? *indices_sp : kEmpty;
  }
  ConstBank vs_consts;  // c0.. (vec4), already offset by vs_base (M4.2 shared)
  ConstBank ps_consts;  // c0.. (vec4), already offset by ps_base (M4.2 shared)
  // SHADER_CONSTANT_BOOL_* (0x4900..0x4907): 256 uniform bool flags the shader
  // microcode branches on via kCondJmp. The translated PS lowers those jumps to
  // structured `if (bcond(n))` guards and reads these bits as a push constant.
  uint32_t bool_consts[8] = {};
  // SHADER_CONSTANT_LOOP_* (0x4908..0x4927): 32 loop constants (count/aL
  // start/step) read by kLoopStart/kLoopEnd bone loops (M3.14).
  uint32_t loop_consts[32] = {};
  // SQ_PROGRAM_CNTL (0x2180) / SQ_CONTEXT_MISC (0x2181): param_gen (bit 18 of
  // the former) injects the screen-space pixel position into PS GPR
  // param_gen_pos (bits 8..15 of the latter) instead of an interpolator --
  // fullscreen post passes (e.g. the DOF) compute their UVs from it.
  uint32_t sq_program_cntl = 0, sq_context_misc = 0;
  GuestTextureDesc tex[kMaxTexSlots];  // indexed by PS texture fetch slot
  BlendMode blend = BlendMode::kStandard;
  // M3.0 raw render state (register shadow at draw time).
  uint32_t blend_control = 0;   // RB_BLENDCONTROL0 (0x2201)
  uint32_t depth_control = 0;   // RB_DEPTHCONTROL  (0x2200)
  // RB_STENCILREFMASK (0x210D) / _BF (0x210C): ref bits0-7, compare mask
  // bits8-15, write mask bits16-23. NOTE 0x210F is PA_CL_VPORT_XSCALE and
  // 0x210E is RB_ALPHA_REF -- these sit just below them.
  uint32_t stencil_ref_mask = 0;
  uint32_t stencil_ref_mask_bf = 0;
  uint32_t su_mode = 0;         // PA_SU_SC_MODE_CNTL (0x2205): cull/winding
  // M3.126: guest primitive-reset state. Reset is active ONLY when
  // PA_SU_SC_MODE_CNTL.multi_prim_ib_ena (bit 21) is set, and the cut value is
  // VGT_MULTI_PRIM_IB_RESET_INDX (0x2103) -- not an implicit 0xFFFF.
  bool prim_reset_enabled = false;
  uint32_t color_control = 0;   // RB_COLORCONTROL (0x2202): alpha test
  float alpha_ref = 0.0f;       // RB_ALPHA_REF (0x210E)
  float vport_zscale = 1.0f, vport_zoffset = 0.0f;  // PA_CL_VPORT_Z* (0x2113/0x2114)
  uint32_t color_mask = 0xF;
  // Per-draw clip transform (push constant): gl_Position.xy = pos.xy*ndc.xy +
  // ndc.zw*pos.w. {1,-1,0,0} for matrix-transformed (clip) shaders; a
  // screen->NDC map for pretransformed window-space shaders.
  float ndc[4] = {1.0f, -1.0f, 0.0f, 0.0f};
  // M3.293: true when the position stream carries pretransformed WINDOW
  // coordinates (UI/HUD quads). Set by the recorder's window-space heuristic;
  // consumed to place the scene-tone boundary (tone the scene RT after the
  // last 3D content, before UI lands on it).
  bool window_space = false;
  // M2.4: EDRAM-copy draw (RB_MODECONTROL edram_mode == kCopy). The scene
  // rendered so far gets copied ("resolved") to the copy_dest guest texture;
  // no geometry is rasterized to the screen for these.
  bool is_resolve = false;
  uint32_t copy_dest = 0;              // RB_COPY_DEST_BASE (guest physical)
  uint32_t copy_w = 0, copy_h = 0;     // RB_COPY_DEST_PITCH fields
  // Resolve rect (window coords, from the resolve draw's vertices): only this
  // region is copied. 0-sized = copy the full surface.
  uint32_t copy_rx = 0, copy_ry = 0, copy_rw = 0, copy_rh = 0;
  // Debug: raw register context at capture time (viewport 210F-2112, surface
  // info 2000, window offset 2080, scissor TL/BR 2081/2082, copy control 2318).
  uint32_t dbg_vport[4] = {0, 0, 0, 0};
  uint32_t dbg_surf = 0, dbg_winoff = 0, dbg_sciss_tl = 0, dbg_sciss_br = 0;
  // RB_COLOR_INFO (0x2001) / RB_DEPTH_INFO (0x2002): EDRAM base tiles. On
  // hardware distinct bases = distinct surfaces; our single scene_img/depth
  // model needs these to know when passes must NOT share (post passes with
  // z-write would stomp the main depth otherwise).
  uint32_t dbg_color_info = 0, dbg_depth_info = 0;
  uint32_t dbg_copyctl = 0;
  // RB_COLOR_CLEAR / RB_DEPTH_CLEAR (231E/231D) at capture: resolve-path fast
  // clears use these; a clear-only resolve has no copy dest.
  uint32_t dbg_color_clear = 0, dbg_depth_clear = 0;
  // Guest VB base: lets the dump re-read live memory at present time to catch
  // UP-draw payloads the game commits after the draw packet (stale captures).
  uint32_t dbg_vb_phys = 0;
  // M4.10: guest frame epoch (walker s_frame_serial) at capture. Chunk merge
  // on the present thread can assemble one published frame from draws of
  // MULTIPLE guest frames; this stamp is the only way to see it happen.
  uint64_t cap_frame = 0;
  // M3.59: winding-flip decision plumbed to the async pipeline worker (which
  // rebuilds a fd without vs_consts, so it can't recompute the determinant).
  // -1 = compute from vs_consts + the per-frame majority; 0/1 = use directly.
  int wind_flip_hint = -1;
};

void SubmitRawDraw(RawGuestDraw&& draw);
// frame_fb (optional out): the VdSwap front-buffer pointer published WITH this
// frame -- the display source that matches these draws (M3.13).
bool ConsumeRawFrame(std::vector<RawGuestDraw>& out, uint32_t* frame_fb = nullptr);
// M3.52: true if a freshly-published guest frame is waiting (non-destructive).
bool HasRawFrame();
// #37: total guest frames published since boot -- lets pacing logs separate
// "the game only produced N fps" from "our present loop only showed N fps".
uint64_t GuestFrameCount();

// Debug: translate a guest physical address for dump-time live re-reads
// (nullptr if memory unavailable).
const uint8_t* DebugGuestPhysPtr(uint32_t phys);
uint8_t* GuestPhysPtrMut(uint32_t phys);  // M3.89 writeback

// M2.4: front buffer physical address most recently passed to VdSwap -- the
// resolve destination whose contents the frame presents.
void SetFrontBufferPhys(uint32_t phys);
uint32_t GetFrontBufferPhys();

// --- capture side (called from guest hooks) ---------------------------------
void RecordPendingUpDraw(uint32_t device, bool indexed, uint32_t prim, uint32_t vertex_count,
                         uint32_t index_count, uint32_t stride);
// Finalizes the in-flight UP draw: the game fills the buffers Begin* returned
// AFTER Begin returns; the commit is an inlined single store, so completion
// runs at the next draw and at the frame's swap.
void CompletePendingUpDraw();
// D3D_WriteLoadAluConstantPacket: engine-loaded ALU blocks bypass the device
// constant staging; record the source so rows c0..c5 read live values.
void StageEngineAluBlock(uint32_t type, uint32_t offset_vec4, uint32_t physaddr,
                         uint32_t count_vec4);
// Frame boundary (called from on_swap): publish the building draw list.
void EndGuestFrame(uint32_t frame_fb);
// Append a fully-decoded draw to the building frame (used by the PM4 walker
// capture path in native_backend_vk.cpp; UP capture appends internally).
void SubmitDecodedDraw(DecodedDraw&& draw);
// Periodic capture statistics (budgeted logging).
void DumpDrawStatsIfDue();

// M4.39 (RESTUFF_D3DCENSUS=1): counts guest-D3D9 setter/draw entry calls and
// prints them per-frame against the PM4 walker's draw count. This is the gate
// for Part B of the API-interception plan: if the setters fire at draw rate
// the API sees the same traffic we do, and if they barely fire the engine
// bypasses the API and the migration is dead. Inert unless the env is set.
enum D3dFn {
  kFnSetVertexShader,
  kFnSetPixelShader,
  kFnStreamDecl,
  kFnResolve,
  kFnDrawB,
  kFnDrawC,
  kFnDrawF,
  kFnDrawG,
  // M4.39b: the UP (user-pointer) draw entries. These were MISSING from the
  // first census, which made the "share of draws that pass through D3D" figure
  // an undercount -- they are as much a D3D9 draw path as the indexed entries.
  kFnBeginVertices,
  kFnBeginIndexedVertices,
  // M4.39c: found statically, not from the address map. These are the only
  // other functions that call sub_82F47D40 -- the command-buffer write helper
  // the known draw/resolve entries use 8x (draws) / 6x (resolve) each. If the
  // draws the API census cannot account for are emitted by guest code at all,
  // it is one of these three.
  kFnEmitA,  // 0x82F398F0
  kFnEmitB,  // 0x82F3E088
  kFnEmitC,  // 0x82F46D30
  // M4.39d: the PM4 side of the same question. Every guest function that
  // writes the command buffer is now hooked above and they still only account
  // for ~68% of captured draws, so the remainder cannot be an unfound API
  // entry point -- it has to be one API call producing several PM4 draws.
  // These count the three draw opcodes separately. DRAW_INDX_2_BIN is the
  // 360's PREDICATED TILING path: the command buffer is replayed once per
  // EDRAM tile, so one logical draw legitimately appears N times on the wire.
  kOpDrawIndx,
  kOpDrawIndx2,
  kOpDrawIndx2Bin,
  kD3dFnCount,
};
void D3dCensusHit(int fn);

// --- renderer side -----------------------------------------------------------
// Moves the latest published frame into `out` and returns true, or returns
// false when no new frame was published since the last call (keep drawing the
// previous copy).
bool ConsumeReadyFrame(std::vector<DecodedDraw>& out);

}  // namespace restuff::renderer

#pragma once

// M1.2(a) — guest GPU life-support for the native Vulkan renderer.
//
// Linux/Vulkan analog of the friend's Windows native_backend.cpp "PM4 mini
// command processor": with the Xenos GPU emulation plugin never loaded, this
// executes the CPU-visible side effects of the real CP — fence writes
// (MEM_WRITE / EVENT_WRITE_SHD / REG_TO_MEM / COND_WRITE), interrupt requests
// (INTERRUPT cpu masks), register writes into a shadow file (with the scratch
// write-back the game polls as its GPU-alive flags), WAIT_REG_MEM park/resume,
// and indirect-buffer descent — so the game's render thread never deadlocks.
// Draw/shader packets are skipped (no GPU work happens here; actual pixels
// come from the guest-D3D UP-draw capture rendered by native_vk.cpp).
//
// Unlike the friend's tree (config.graphics = nullptr, guest-side midasm
// hooks), our NativeVulkanGraphicsSystem IS the IGraphicsSystem, so the
// kernel's Vd* exports route straight into it and forward here. Ring kicks
// (CP_RB_WPTR stores to 0x7FC8xxxx) arrive via the MMIO range claimed in
// Initialize() — the recompiled code routes that aperture through MMIOHandler
// unconditionally.

#include <cstdint>
#include <vector>

namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::system {
class KernelState;
}

namespace restuff::native {

// Claim the GPU register MMIO range and seed shadow defaults. Call from
// SetupGuestGpu (runs during Runtime::Setup, before the guest boots — MMIO
// reads are uninitialized garbage until the range is registered).
void Initialize(rex::runtime::FunctionDispatcher* dispatcher,
                rex::system::KernelState* kernel_state);
// Stop the pump thread. Call before tearing down Vulkan/kernel state.
void Shutdown();

// IGraphicsSystem forwarding targets (kernel Vd* exports).
void SetGuestInterruptCallback(uint32_t callback, uint32_t user_data);
void SetRingBuffer(uint32_t physical_base, uint32_t size_log2);
void SetRingWritebackSlot(uint32_t physical_address, uint32_t block_size_log2);

// Raw shadow-register read for the draw-capture layer (fetch-constant
// endianness etc.). No side effects.
uint32_t GetShadowRegRaw(uint32_t reg);

// M3.100: the guest's DISPLAY GAMMA RAMP (DC_LUT_* MMIO writes). The 360's
// display controller applies this 256-entry curve at scanout; the reference
// bakes it into the front buffer during resolve. Version is 0 until the guest
// programs the ramp (the power-on default is identity). CopyGammaRamp fills
// 256 dwords packed R10|G10<<10|B10<<20 (matches A2B10G10R10_UNORM_PACK32).
uint32_t GetGammaRampVersion();
void CopyGammaRamp(uint32_t* packed256);

// Latest XE_SWAP frontbuffer info (guest physical, dimensions). Zero until
// the game presents its first frame.
void GetFrontbuffer(uint32_t& phys, uint32_t& width, uint32_t& height);

// --- M2.0: guest shader registry --------------------------------------------
// Every shader the GPU would run passes through PM4 IM_LOAD (phys addr + size)
// or IM_LOAD_IMMEDIATE (inline in the stream). The mini-CP registers each one
// here: FNV-1a hash over the raw big-endian ucode words, dumped once to
// shader_dump/<type>_<hash>.ucode.bin next to the exe for offline translation.
struct GuestShaderInfo {
  uint32_t type = 0;        // 0 = vertex, 1 = pixel
  uint32_t phys_addr = 0;   // 0 for IM_LOAD_IMMEDIATE
  uint32_t size_dwords = 0;
  uint64_t hash = 0;
};
// Look up a registered shader by the guest physical address of its ucode.
// Returns false if no shader has been loaded from that address.
bool FindShaderByPhys(uint32_t phys_addr, GuestShaderInfo& out);
// Scan a host-mapped guest region (raw big-endian words) for any registered
// shader (4-word prefix match, then full-hash verify). Returns 0 on miss.
// Needed because most UI shaders arrive via IM_LOAD_IMMEDIATE (no phys addr):
// the draw-time shader OBJECT points at its own ucode copy, which this matches
// against the registered corpus.
uint64_t FindShaderInRegion(const uint32_t* be_region, uint32_t region_words);

// M4.0: copy one registered shader's raw big-endian ucode words (retained for
// the process lifetime in the blob store). Feeds the pipeline pre-warm
// manifest writer in native_vk.cpp. Returns false if the hash was never
// registered this run.
bool CopyShaderUcode(uint64_t hash, GuestShaderInfo& info_out,
                     std::vector<uint32_t>& words_out);

}  // namespace restuff::native

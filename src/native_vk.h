#pragma once

// M1.1 -- native Vulkan graphics system for restuff.
//
// Implements rex::system::IGraphicsSystem DIRECTLY. The SDK's
// rex::graphics::vulkan::VulkanGraphicsSystem and the CommandProcessor base are
// compiled with hidden visibility into the xenos GPU plugin (librexgpu-xenos)
// and export no symbols, so they cannot be linked from the exe -- which is why
// the friend's native_renderer.cpp (which subclassed them) never built. The
// core runtime instead exports all of rex::ui::vulkan::*, so we stand up a
// VulkanProvider + VulkanPresenter ourselves and present our own frame via
// Presenter::RefreshGuestOutput, bypassing the xenos plugin entirely.
//
// For M1.1 this just clears the guest-output image to a solid colour to prove
// the native Vulkan path renders on Linux. Guest GPU emulation (ring buffer,
// interrupts, real draws) is intentionally NOT wired yet -- that's M1.2. A
// stalled guest is fine here: the present thread is independent, so the clear
// colour displays regardless.

#include <atomic>
#include <memory>
#include <thread>

// Use the SDK's canonical Vulkan setup (defines VK_NO_PROTOTYPES +
// VK_ENABLE_BETA_EXTENSIONS + the platform surface macros and includes
// vulkan.h in the right order) rather than <vulkan/vulkan.h> directly -- a raw
// include here parses vulkan_core.h without beta extensions, and the include
// guard then blocks the rex chain's beta re-include, breaking vulkan_enums.hpp.
#include <rex/ui/vulkan/api.h>

#include <rex/cvar.h>
#include <rex/system/interfaces/graphics.h>

// Toggle: native Vulkan renderer vs. the stock xenos GPU plugin. Default on for
// M1.1 bring-up; set to false in restuff.toml to fall back to xenos.
REXCVAR_DECLARE(bool, use_native_renderer);

namespace rex::ui::vulkan {
class VulkanProvider;
}  // namespace rex::ui::vulkan

namespace restuff {

class NativeVulkanGraphicsSystem final : public rex::system::IGraphicsSystem {
 public:
  NativeVulkanGraphicsSystem();
  ~NativeVulkanGraphicsSystem() override;

  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
  rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                              rex::system::KernelState* kernel_state) override;
  bool has_presentation() const override;
  rex::ui::GraphicsProvider* provider() const override;
  rex::ui::Presenter* presenter() const override;
  void Shutdown() override;

  // Guest GPU services reached from the xboxkrnl Vd* exports — forwarded to
  // the life-support mini-CP in renderer/native_backend_vk.cpp so the guest
  // (fences, interrupts, ring) runs without any GPU emulation.
  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;

 private:
  // Present thread: repeatedly refresh the guest-output image with our clear
  // colour so the presenter has something to composite each frame.
  void PresentThreadMain();
  // One RefreshGuestOutput call that records+submits a clear of the guest image.
  bool PresentClearFrame();

  std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider_;
  std::unique_ptr<rex::ui::Presenter> presenter_;

  VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
  // M4.5: two frame slots (cmd buffer + fence + submitted flag). Serialized
  // mode (default) only ever uses slot 0 and waits right after submit --
  // byte-identical to the historical single-buffered flow. RESTUFF_PIPELINED=1
  // alternates slots and moves the wait to the top of the next frame, so the
  // CPU prepares frame N+1 while the GPU executes frame N.
  VkCommandBuffer cmd_bufs_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  VkFence fences_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  bool slot_submitted_[2] = {false, false};
  uint32_t frame_slot_ = 0;

  std::thread present_thread_;
  std::atomic<bool> running_{false};
};

}  // namespace restuff

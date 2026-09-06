# rexglue SDK: RenderDoc capture layer self-attaches on every Linux run when RenderDoc is installed system-wide

**Reported against:** rexglue SDK source tree (`src/ui/renderdoc_api.cpp`, `src/ui/vulkan/vulkan_instance.cpp`).
**Reporter context:** downstream static-recompilation project (Naughty Bear) on Linux x86-64 with the distro `renderdoc` package installed.

## Summary

`rex::ui::RenderDocAPI::CreateIfConnected()` decides "RenderDoc is attached" by whether
`librenderdoc` can be loaded:

```cpp
// src/ui/renderdoc_api.cpp:23-28
// Try to load the RenderDoc library. If RenderDoc is attached, the library
// should already be loaded into the process and this will increment the
// reference count. If not attached, the load will fail and we return nullptr.
if (!renderdoc_api->library_.Load(platform::lib_names::kRenderDoc)) {
  return nullptr;
}
```

That inference is correct on Windows (`renderdoc.dll` is not on the DLL search path unless
injected), but **wrong on Linux**: distros install `librenderdoc.so` into the default
linker path (`/usr/lib/librenderdoc.so` on Arch), so the `dlopen` succeeds on every run.
Loading `librenderdoc.so` is not a passive query — it is an attach: the library's
initialization arms RenderDoc's capture machinery for the process, and the subsequently
created Vulkan instance gets the capture layer with its in-viewport overlay
("Capturing Vulkan. … F12, PrtScrn to capture.") and per-call state-tracking overhead.

`VulkanInstance::Create()` calls this unconditionally before instance creation
(`src/ui/vulkan/vulkan_instance.cpp:36`), with no cvar or config gate.

## Observed effect (downstream, Linux + renderdoc installed)

- Every native run of the recompiled title shows the RenderDoc overlay and carries the
  capture layer — including normal gameplay sessions where no capture tooling is in use.
- A/B on the same build, same scene: launching with a bogus `librenderdoc.so` shadowed
  onto `LD_LIBRARY_PATH` (so the SDK's load fails) removes the overlay and the layer
  entirely; nothing else changes.

## Suggested fixes (any of)

1. Gate `CreateIfConnected()` behind an opt-in cvar (e.g. `--renderdoc`), defaulting off.
2. On POSIX, treat "attached" as "already loaded in-process": use
   `dlopen(name, RTLD_NOW | RTLD_NOLOAD)` for the probe, so a system-installed library
   that nothing injected does not count as attached. (An injected/preloaded
   `librenderdoc.so` — qrenderdoc launch, `ENABLE_VULKAN_RENDERDOC_CAPTURE=1` — is
   already loaded and still probes true.)
3. At minimum, honor an explicit disable env before loading.

Option 2 preserves the current "just works under qrenderdoc" behavior with a one-flag
change to the probe and no new configuration surface.

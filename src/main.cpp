// restuff - ReXGlue Recompiled Project

#include "generated/default/restuff_init.h"

#include "restuff_app.h"

#ifdef __linux__
#include <dlfcn.h>
#include <cstdlib>
#include <cstring>

// M3.122: the SDK's RenderDocAPI::CreateIfConnected() infers "RenderDoc is
// attached" from whether librenderdoc loads. Correct on Windows (the DLL is
// only findable when injected), wrong on Linux with the distro renderdoc
// package installed: /usr/lib/librenderdoc.so dlopens on EVERY run, and
// loading it IS attaching -- every native run got the capture layer (viewport
// overlay + per-call state tracking), proven by the layer overlay in all
// headless drives and its disappearance when the load is made to fail.
// The SDK call site is prebuilt, so interpose dlopen in the executable (our
// definition wins symbol resolution for the statically linked SDK code and
// for shared libs' PLT lookups alike) and refuse librenderdoc unless capture
// was explicitly requested (ENABLE_VULKAN_RENDERDOC_CAPTURE=1, the same env
// the M3.120 in-app trigger path documents, or RESTUFF_RENDERDOC=1).
// Upstream report: docs/upstream-reports/renderdoc-always-attached-on-linux.md
extern "C" void* dlopen(const char* filename, int flags) {
  using DlopenFn = void* (*)(const char*, int);
  static DlopenFn real = reinterpret_cast<DlopenFn>(dlsym(RTLD_NEXT, "dlopen"));
  if (filename && strstr(filename, "librenderdoc")) {
    const char* en = getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
    if (!(en && *en == '1') && !getenv("RESTUFF_RENDERDOC")) return nullptr;
  }
  return real ? real(filename, flags) : nullptr;
}
#endif

REX_DEFINE_APP(restuff, RestuffApp::Create)

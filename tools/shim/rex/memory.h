// Include-path shim for offline shader-interpreter harnesses. Shadows the
// SDK's <rex/memory.h> umbrella so tools can compile the SDK's
// ShaderInterpreter (src/graphics/pipeline/shader/interpreter.cpp) without the
// full guest-memory subsystem: the interpreter's only memory dependency is
// physical_membase() (one flat pointer indexed by guest physical address).
// Everything else the umbrella provided to other headers (byte swaps,
// Reinterpret, arenas...) still comes from the real sub-headers below.
//
// Use: put tools/shim BEFORE the SDK include dir on the include path, compile
// interpreter.cpp from SDK source in the same build (its symbols then shadow
// any archive copy), and back physical_membase_shim with a 512MB mapping
// (guest physical space) holding raw big-endian guest bytes.
#pragma once

#include <cstdint>

#include <rex/memory/arena.h>
#include <rex/memory/mapped_memory.h>
#include <rex/memory/ring_buffer.h>
#include <rex/memory/utils.h>

namespace rex::memory {

class Memory {
 public:
  uint8_t* physical_membase_shim = nullptr;
  uint8_t* physical_membase() const { return physical_membase_shim; }
};

}  // namespace rex::memory

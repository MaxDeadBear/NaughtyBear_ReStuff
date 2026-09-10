#pragma once
#include <cstddef>
#include <cstdint>

namespace restuff::renderer {
// Apply once, immediately before the last main-surface colour resolve into
// the buffer this frame will present. UI/window-space flags are not reliable
// in this title's translated draw stream. Returning size() means no match.
template <class Records>
size_t FindCelResolve(const Records& records, uint32_t front_buffer) {
  if (front_buffer) {
    for (size_t i = records.size(); i > 0; --i) {
      const auto& r = records[i - 1];
      if (r.is_resolve && !r.is_depth_resolve && r.surf == 0 &&
          r.copy_dest == front_buffer)
        return i - 1;
    }
  }
  return records.size();
}
}  // namespace restuff::renderer

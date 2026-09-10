#include "renderer/scene_effect_boundary.h"
#include <cassert>
#include <vector>

struct Record {
  bool is_resolve = false, is_depth_resolve = false;
  uint32_t surf = 0, copy_dest = 0;
};
int main() {
  using restuff::renderer::FindCelResolve;
  const uint32_t fb = 0x03FA0000;
  std::vector<Record> r;
  assert(FindCelResolve(r, fb) == r.size());
  // Reproduce a scene with no window-space UI marker at all.
  r.resize(100);
  r.push_back({true, false, 0, fb});
  assert(FindCelResolve(r, fb) == 100);
  // Later offscreen / depth resolves must not capture the effect.
  r.push_back({true, false, 0, fb + 4096});
  r.push_back({true, true, 0, fb});
  r.push_back({true, false, 1, fb});
  assert(FindCelResolve(r, fb) == 100);
  r.push_back({true, false, 0, fb});
  assert(FindCelResolve(r, fb) == 104); // once, at the final colour resolve
  assert(FindCelResolve(r, 0) == r.size());
  assert(FindCelResolve(r, 0x12345678) == r.size());
  // Small/menu frames must not silently disable a user-enabled effect.
  r = {{}, {true, false, 0, fb}};
  assert(FindCelResolve(r, fb) == 1);
}

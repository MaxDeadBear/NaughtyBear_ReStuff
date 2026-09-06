#version 450
// Captured guest UP draws: positions arrive in D3D-style NDC (y-up); the push
// constants flip to Vulkan's y-down clip space (scale = (1,-1), offset = 0).
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color1;
layout(location = 3) in vec4 in_color2;
layout(push_constant) uniform PC {
  vec2 scale;
  vec2 offset;
} pc;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color1;
layout(location = 2) out vec4 v_color2;
void main() {
  gl_Position = vec4(in_pos * pc.scale + pc.offset, 0.0, 1.0);
  v_uv = in_uv;
  v_color1 = in_color1;
  v_color2 = in_color2;
}

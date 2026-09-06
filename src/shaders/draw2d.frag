#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;
void main() {
  // Untextured draws bind a 1x1 white texture, so this is pure vertex colour.
  o_color = v_color * texture(u_tex, v_uv);
}

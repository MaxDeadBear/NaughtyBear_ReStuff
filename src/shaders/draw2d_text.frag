#version 450
// Text family — exact port of guest PS 5A6850E039FC3F1B: rgb = text colour,
// alpha = colour.a * glyph atlas alpha (k_8 texture decoded as white+alpha).
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color1;
layout(location = 2) in vec4 v_color2;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;
void main() {
  float glyph = texture(u_tex, v_uv).a;
  o_color = vec4(v_color1.rgb, v_color1.a * glyph);
}

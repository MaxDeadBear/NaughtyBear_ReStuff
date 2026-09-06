#version 450
// Scaleform quad family — exact port of guest PS 98E0B1214352B492
// (tools/nb_ucode_dump): c = mix(color1, tex, color2.r); alpha gated by
// color2.a. PS cxform constants c2/c3 treated as identity.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color1;
layout(location = 2) in vec4 v_color2;
layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(location = 0) out vec4 o_color;
void main() {
  vec4 t = texture(u_tex, v_uv);
  vec4 c = mix(v_color1, t, v_color2.r);
  o_color = vec4(c.rgb, c.a * v_color2.a);
}

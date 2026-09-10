#version 450
layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(set = 1, binding = 0) uniform sampler2D lutTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(push_constant) uniform Settings {
    layout(offset = 16) vec4 adjust; // brightness, gamma, use guest LUT, unused
} pc;

void main() {
    vec3 c = texture(srcTex, v_uv).rgb;
    if (pc.adjust.z > 0.5) {
        vec3 u = c * (255.0 / 256.0) + (0.5 / 256.0);
        c = vec3(texture(lutTex, vec2(u.r, 0.5)).r,
                 texture(lutTex, vec2(u.g, 0.5)).g,
                 texture(lutTex, vec2(u.b, 0.5)).b);
    }
    // Neutral controls execute exactly the original LUT output path.
    // Apply user controls after the game's own display curve, once per frame.
    if (pc.adjust.x != 1.0 || pc.adjust.y != 1.0) {
        c = pow(clamp(c * pc.adjust.x, 0.0, 1.0), vec3(1.0 / pc.adjust.y));
    }
    o = vec4(c, 1.0);
}

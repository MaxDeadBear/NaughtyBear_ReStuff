#version 450
layout(set = 0, binding = 0) uniform sampler2D srcTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o;
layout(push_constant) uniform Settings {
    vec4 tone_cel; // tone gain, tone power, band count, band strength
    vec4 edges;    // edge strength, sample offset x/y, unused
} pc;

float luminance(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float neighborLuma(vec2 offset) {
    vec2 halfTexel = 0.5 / vec2(textureSize(srcTex, 0));
    return luminance(texture(srcTex, clamp(v_uv + offset, halfTexel, 1.0 - halfTexel)).rgb);
}

void main() {
    vec4 source = texture(srcTex, v_uv);
    vec3 c = pow(clamp(source.rgb * pc.tone_cel.x, 0.0, 1.0), vec3(pc.tone_cel.y));
    // Quantize brightness together to preserve texture colours. This is a
    // scene stylization pass, not a reconstruction of per-material lighting.
    float luma = luminance(c);
    float steps = max(pc.tone_cel.z - 1.0, 1.0);
    float scaled = luma * steps;
    // A narrow derivative-sized transition reduces crawling at band edges.
    float width = clamp(fwidth(scaled), 0.001, 0.15);
    float band = (floor(scaled) + smoothstep(0.5 - width, 0.5 + width, fract(scaled))) / steps;
    vec3 cel = c * (band / max(luma, 0.0001));
    c = mix(c, cel, pc.tone_cel.w);

    // Optional image edges; these also pick up strong texture detail, rather
    // than being geometry silhouettes. Offset scales with render resolution.
    if (pc.edges.x > 0.0) {
        float dx = neighborLuma(vec2(pc.edges.y, 0.0)) - neighborLuma(vec2(-pc.edges.y, 0.0));
        float dy = neighborLuma(vec2(0.0, pc.edges.z)) - neighborLuma(vec2(0.0, -pc.edges.z));
        float edge = smoothstep(0.08, 0.25, length(vec2(dx, dy)));
        c *= 1.0 - pc.edges.x * edge;
    }
    o = vec4(clamp(c, 0.0, 1.0), source.a);
}

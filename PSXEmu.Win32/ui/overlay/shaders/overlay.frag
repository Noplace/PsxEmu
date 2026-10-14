#version 450
// The overlay's fragment shader for Vulkan - compiled to spirv_overlay_frag.h by
// build_spirv.bat. Keep in step with kOverlayGlslFragment in overlay_shaders.h: a u of 2 or more
// is the game's frame, blurred, for the glass theme; anything else is the atlas.
layout(binding = 0) uniform sampler2D u_atlas;
layout(binding = 1) uniform sampler2D u_frame;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
const float kRadius = 22.0;
vec4 Frosted(vec2 uv, vec2 pixel) {
    vec3 sum = textureLod(u_frame, uv, 0.0).rgb;
    float weight = 1.0;
    for (int i = 0; i < 24; ++i) {
        float t = (float(i) + 0.5) / 24.0;
        float angle = float(i) * 2.3999632;
        vec2 offset = vec2(cos(angle), sin(angle)) * sqrt(t) * kRadius * pixel;
        float k = 1.0 - 0.6 * t;
        sum += textureLod(u_frame, uv + offset, 0.0).rgb * k;
        weight += k;
    }
    vec3 c = sum / weight;
    float grey = dot(c, vec3(0.299, 0.587, 0.114));
    c = clamp(mix(vec3(grey), c, 1.35), 0.0, 1.0);
    float inside = (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) ? 1.0 : 0.0;
    return vec4(c * inside, 1.0);
}
void main() {
    vec2 pixel = vec2(abs(dFdx(v_uv.x)), abs(dFdy(v_uv.y)));
    vec4 texel = textureLod(u_atlas, v_uv, 0.0);
    if (v_uv.x >= 2.0)
        texel = Frosted(v_uv - vec2(2.0, 0.0), pixel);
    o_color = texel * v_color;
}

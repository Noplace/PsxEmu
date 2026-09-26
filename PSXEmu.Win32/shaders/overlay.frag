#version 450
// The overlay's fragment shader for Vulkan - compiled to spirv_overlay.h by build_spirv.bat.
// Keep in step with kOverlayGlslFragment in overlay_shaders.h.
layout(binding = 0) uniform sampler2D u_atlas;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 0) out vec4 o_color;
void main() {
    o_color = texture(u_atlas, v_uv) * v_color;
}

#version 450
// The overlay's vertex shader for Vulkan - compiled to spirv_overlay.h by build_spirv.bat.
// Keep in step with kOverlayGlslVertex in overlay_shaders.h.
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}

/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

// The overlay's one shader pair, for every engine: sample the atlas, multiply by the vertex's
// colour. Positions arrive already in clip space - each engine converts them as it copies the
// vertices up (WriteOverlayVertices, ui/overlay/overlay_draw.h) - so there are no constants.
//
// A texture coordinate with u at 2 or more is not the atlas: it is the game's frame, at
// (u - 2, v), and what is drawn is that frame blurred - the glass theme's frosted panels
// (Overlay::GlassRoundRect). Thirteen taps in two rings, a little more saturated, and black
// outside the picture, where the letterbox bars are. The frame is the second texture.
//
// Direct3D compiles the HLSL at start-up, OpenGL the GLSL 3.30. Vulkan takes SPIR-V made from
// shaders/overlay.vert and overlay.frag by build_spirv.bat (spirv_overlay.h); keep the three in
// step.

namespace psxemu {

    inline constexpr char kOverlayHlsl[] = R"HLSL(
Texture2D atlas : register(t0);
Texture2D frame : register(t1);
SamplerState overlay_sampler : register(s0);
struct VsIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
VsOut VsMain(VsIn input) {
    VsOut output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
// `pixel` is one screen pixel in the frame's coordinates, so the blur is the same size on
// screen whatever the resolution: 24 taps on a golden-angle spiral out to kRadius pixels.
static const float kRadius = 22.0;
float4 Frosted(float2 uv, float2 pixel) {
    float3 sum = frame.SampleLevel(overlay_sampler, uv, 0).rgb;
    float weight = 1.0;
    [unroll] for (int i = 0; i < 24; ++i) {
        float t = (i + 0.5) / 24.0;
        float angle = i * 2.3999632;
        float2 offset = float2(cos(angle), sin(angle)) * sqrt(t) * kRadius * pixel;
        float k = 1.0 - 0.6 * t;
        sum += frame.SampleLevel(overlay_sampler, uv + offset, 0).rgb * k;
        weight += k;
    }
    float3 c = sum / weight;
    float grey = dot(c, float3(0.299, 0.587, 0.114));
    c = saturate(lerp(float3(grey, grey, grey), c, 1.35));
    float inside = (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0) ? 1.0 : 0.0;
    return float4(c * inside, 1.0);
}
float4 PsMain(VsOut input) : SV_TARGET {
    float2 pixel = float2(abs(ddx(input.uv.x)), abs(ddy(input.uv.y)));
    float4 texel = atlas.SampleLevel(overlay_sampler, input.uv, 0);
    if (input.uv.x >= 2.0)
        texel = Frosted(input.uv - float2(2.0, 0.0), pixel);
    return texel * input.color;
}
)HLSL";

    inline constexpr char kOverlayGlslVertex[] = R"GLSL(#version 330 core
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
out vec2 v_uv;
out vec4 v_color;
void main() {
    v_uv = a_uv;
    v_color = a_color;
    gl_Position = vec4(a_pos, 0.0, 1.0);
}
)GLSL";

    inline constexpr char kOverlayGlslFragment[] = R"GLSL(#version 330 core
uniform sampler2D u_atlas;
uniform sampler2D u_frame;
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
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
)GLSL";

}   // namespace psxemu

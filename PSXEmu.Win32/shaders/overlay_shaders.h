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
// Direct3D compiles the HLSL at start-up, OpenGL the GLSL 3.30. Vulkan takes SPIR-V made from
// shaders/overlay.vert and overlay.frag by build_spirv.bat (spirv_overlay.h); keep the three in
// step.

namespace psxemu {

    inline constexpr char kOverlayHlsl[] = R"HLSL(
Texture2D atlas : register(t0);
SamplerState atlas_sampler : register(s0);
struct VsIn { float2 pos : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
VsOut VsMain(VsIn input) {
    VsOut output;
    output.pos = float4(input.pos, 0.0, 1.0);
    output.uv = input.uv;
    output.color = input.color;
    return output;
}
float4 PsMain(VsOut input) : SV_TARGET {
    return atlas.Sample(atlas_sampler, input.uv) * input.color;
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
in vec2 v_uv;
in vec4 v_color;
out vec4 o_color;
void main() {
    o_color = texture(u_atlas, v_uv) * v_color;
}
)GLSL";

}   // namespace psxemu

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

#include <cstddef>
#include <cstdint>
#include <iterator>

// Every video filter, in GLSL for the OpenGL engine: a line-for-line port of the HLSL ones
// (legacy_shaders.h, Resource/ps_scanline_filter.hlsl, ps_xbrz_filter.hlsl and superxbr/), under
// the same keys, so the Video > Filter menu means the same thing whichever renderer draws it.
//
// The ports change the language, not the arithmetic. Each HLSL sampler maps to the GL sampler set
// up to behave the same (see kGlslFilterHeader), GetDimensions is textureSize, SampleLevel is
// textureLod, frac/lerp/float2 are fract/mix/vec2, and SV_Position - which D3D counts from the top
// of the target - is FragPosition(), which does the same from GL's bottom-up gl_FragCoord.
// Anything that is not a mechanical translation says so where it is.

namespace psxemu {

    // Put in front of every filter body by OpenGLGraphicsEngine.
    inline constexpr const char kGlslFilterHeader[] = R"GLSL(#version 330 core
in vec2 v_uv;
out vec4 o_color;
// One input, four ways of sampling it - D3D12GraphicsEngine's static samplers s0-s3 - and the
// untouched emulator frame (HLSL t1), point-sampled and clamped as the chain's s2 reads it.
uniform sampler2D u_point;          // s0: point, wrap
uniform sampler2D u_linear;         // s1: linear, wrap
uniform sampler2D u_point_clamp;    // s2: point, clamp to edge
uniform sampler2D u_linear_clamp;   // s3: linear, clamp to edge
uniform sampler2D u_original;       // t1 through s2
// The HLSL constant buffer b0: this draw's target size, then its input's. The chain passes call
// the same four OutputSize and TextureSize.
uniform vec4 u_params;
uniform vec2 u_frag_y;
vec4 FragPosition() {
    return vec4(gl_FragCoord.x, u_frag_y.x + u_frag_y.y * gl_FragCoord.y, gl_FragCoord.zw);
}
)GLSL";

    // The same header for Vulkan, which compiles the same bodies to SPIR-V (make_spirv.cpp, into
    // spirv_filters.h). Vulkan GLSL wants explicit bindings and a push-constant block in place of
    // loose uniforms, and needs no FragPosition arithmetic: its gl_FragCoord already counts down
    // from the top of the target, as SV_Position does. Bindings 0-4 are the same five samplers.
    inline constexpr const char kVulkanFilterHeader[] = R"GLSL(#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(set = 0, binding = 0) uniform sampler2D u_point;          // s0: point, wrap
layout(set = 0, binding = 1) uniform sampler2D u_linear;         // s1: linear, wrap
layout(set = 0, binding = 2) uniform sampler2D u_point_clamp;    // s2: point, clamp to edge
layout(set = 0, binding = 3) uniform sampler2D u_linear_clamp;   // s3: linear, clamp to edge
layout(set = 0, binding = 4) uniform sampler2D u_original;       // t1 through s2
layout(push_constant) uniform Params { vec4 u_params; };         // outW, outH, inW, inH
vec4 FragPosition() { return gl_FragCoord; }
)GLSL";

    // Vulkan's vertex shader: the same big triangle, with uv (0,0) at the top left. Vulkan's clip
    // space already has y pointing down, and its render targets and textures both start at the
    // top row, so nothing is flipped anywhere; the viewport and scissor place it, as in D3D12.
    inline constexpr const char kVulkanVertexShader[] = R"GLSL(#version 450
layout(location = 0) out vec2 v_uv;
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

    // The built-in pass-through, and the blit that ends a chain.
    inline constexpr const char kGlslDefault[] = R"GLSL(
void main() { o_color = texture(u_point, v_uv); }
)GLSL";

    inline constexpr const char kGlslBlit[] = R"GLSL(
void main() { o_color = texture(u_linear_clamp, v_uv); }
)GLSL";

    // legacy_shaders.h[1]: Sharp Bilinear.
    inline constexpr const char kGlslSharpBilinear[] = R"GLSL(
void main() {
    vec2 texSize = u_params.zw;
    vec2 scale = u_params.xy / texSize;

    // Position in texel coordinates
    vec2 texel = v_uv * texSize;
    vec2 texel_floored = floor(texel);
    vec2 s = fract(texel);

    // Sharp bilinear: nearest neighbor in the center of each texel,
    // smooth bilinear interpolation only at the 1-output-pixel-wide edges
    vec2 region_range = 0.5 - 0.5 / scale;
    vec2 center_dist = s - 0.5;
    // HLSL's clamp(x, lo, hi) is min(max(x, lo), hi) whatever the bounds. GLSL's is undefined
    // when lo > hi - which it is here whenever the picture is smaller than the frame (a scale
    // below 1 makes region_range negative) - and a driver gave a different answer. Spelt out.
    vec2 f = (center_dist - min(max(center_dist, -region_range), region_range)) * scale + 0.5;

    vec2 newUV = (texel_floored + f) / texSize;
    o_color = texture(u_linear, newUV);
}
)GLSL";

    // legacy_shaders.h[2]: CRT-Lottes (public domain, Timothy Lottes).
    inline constexpr const char kGlslCrtLottes[] = R"GLSL(
// --- Tunable CRT parameters ---
const float hardScan      = -8.0;   // Scanline hardness (-20=soft, 0=hard)
const float hardPix       = -3.0;   // Pixel sharpness (-20=soft, 0=sharp)
const vec2 crtWarp        = vec2(0.031, 0.041); // Barrel distortion
const float maskDark      = 0.5;    // Shadow mask dark level
const float maskLight     = 1.5;    // Shadow mask bright level
const float brightBoost   = 1.15;   // Compensate scanline darkening
const float hardBloomScan = -2.0;   // Bloom scanline softness
const float hardBloomPix  = -1.5;   // Bloom pixel softness
const float bloomAmount   = 0.12;   // Bloom intensity
const float crtShape      = 2.0;    // Gaussian shape exponent

// --- sRGB gamma ---
float ToLinear1(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
vec3 ToLinear(vec3 c) { return vec3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b)); }
float ToSrgb1(float c) { return (c < 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }
vec3 ToSrgb(vec3 c) { return vec3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }

// Fetch a texel at exact integer offset and convert to linear
vec3 Fetch(vec2 pos, vec2 off) {
    vec2 res = u_params.zw;
    pos = (floor(pos * res + off) + 0.5) / res;
    if (max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5) return vec3(0.0);
    vec3 c = textureLod(u_point, pos, 0.0).rgb;
    return ToLinear(c) * brightBoost;
}

// Gaussian weight function
float Gaus(float p, float scale) { return exp2(scale * pow(abs(p), crtShape)); }

// 3-tap horizontal filter (sharp)
vec3 Horz3(vec2 pos, float off) {
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2( 0.0, off));
    vec3 d = Fetch(pos, vec2( 1.0, off));
    float dst = fract(pos.x * u_params.z) - 0.5;
    float wb = Gaus(dst - 1.0, hardPix);
    float wc = Gaus(dst,       hardPix);
    float wd = Gaus(dst + 1.0, hardPix);
    return (b * wb + c * wc + d * wd) / (wb + wc + wd);
}

// 5-tap horizontal filter (bloom / soft glow)
vec3 Horz5(vec2 pos, float off) {
    vec3 a = Fetch(pos, vec2(-2.0, off));
    vec3 b = Fetch(pos, vec2(-1.0, off));
    vec3 c = Fetch(pos, vec2( 0.0, off));
    vec3 d = Fetch(pos, vec2( 1.0, off));
    vec3 e = Fetch(pos, vec2( 2.0, off));
    float dst = fract(pos.x * u_params.z) - 0.5;
    float wa = Gaus(dst - 2.0, hardBloomPix);
    float wb = Gaus(dst - 1.0, hardBloomPix);
    float wc = Gaus(dst,       hardBloomPix);
    float wd = Gaus(dst + 1.0, hardBloomPix);
    float we = Gaus(dst + 2.0, hardBloomPix);
    return (a*wa + b*wb + c*wc + d*wd + e*we) / (wa+wb+wc+wd+we);
}

// Scanline weight (vertical Gaussian)
float Scan(vec2 pos, float off) {
    return Gaus(fract(pos.y * u_params.w) - 0.5 + off, hardScan);
}
float BloomScan(vec2 pos, float off) {
    return Gaus(fract(pos.y * u_params.w) - 0.5 + off, hardBloomScan);
}

// Barrel distortion (screen curvature)
vec2 Warp(vec2 pos) {
    pos = pos * 2.0 - 1.0;
    pos *= vec2(1.0 + pos.y * pos.y * crtWarp.x, 1.0 + pos.x * pos.x * crtWarp.y);
    return pos * 0.5 + 0.5;
}

// Shadow mask (stretched VGA-style phosphor pattern)
vec3 Mask(vec2 pos) {
    vec3 m = vec3(maskDark, maskDark, maskDark);
    pos.x += pos.y * 3.0;
    pos.x = fract(pos.x / 6.0);
    if      (pos.x < 0.333) m.r = maskLight;
    else if (pos.x < 0.666) m.g = maskLight;
    else                    m.b = maskLight;
    return m;
}

void main() {
    vec2 p = Warp(v_uv);
    // Combine two adjacent scanlines with Gaussian weights
    vec3 color = Horz3(p, -1.0) * Scan(p, -1.0)
               + Horz3(p,  0.0) * Scan(p,  0.0);
    // Bloom: wider Gaussian pass for soft glow
    vec3 bloom = Horz5(p, -1.0) * BloomScan(p, -1.0)
               + Horz5(p,  0.0) * BloomScan(p,  0.0);
    color += bloom * bloomAmount;
    // Apply phosphor shadow mask
    color *= Mask(FragPosition().xy);
    o_color = vec4(ToSrgb(color), 1.0);
}
)GLSL";

    // legacy_shaders.h[3]: SuperEagle.
    inline constexpr const char kGlslSuperEagle[] = R"GLSL(
bool c_match(vec4 c1, vec4 c2) {
    return all(lessThan(abs(c1 - c2), vec4(0.01)));
}

void main() {
    vec2 uv = v_uv;
    ivec2 size = textureSize(u_point, 0);
    float width = float(size.x);
    float height = float(size.y);
    vec2 texel = vec2(1.0 / width, 1.0 / height);

    vec4 C0 = texture(u_point, uv + vec2(-texel.x, -texel.y));
    vec4 C1 = texture(u_point, uv + vec2(     0.0, -texel.y));
    vec4 C2 = texture(u_point, uv + vec2( texel.x, -texel.y));
    vec4 C3 = texture(u_point, uv + vec2(-texel.x,      0.0));
    vec4 C4 = texture(u_point, uv + vec2(     0.0,      0.0)); // Center
    vec4 C5 = texture(u_point, uv + vec2( texel.x,      0.0));
    vec4 C6 = texture(u_point, uv + vec2(-texel.x,  texel.y));
    vec4 C7 = texture(u_point, uv + vec2(     0.0,  texel.y));
    vec4 C8 = texture(u_point, uv + vec2( texel.x,  texel.y));

    vec2 f = fract(uv * vec2(width, height));
    vec4 color = C4;

    if (f.x < 0.5 && f.y < 0.5) {
        if (c_match(C3, C1) && !c_match(C0, C4)) color = C3;
        else if (c_match(C0, C4) && !c_match(C3, C1)) color = C0;
        else color = 0.5 * (C3 + C1);
    }
    else if (f.x >= 0.5 && f.y < 0.5) {
        if (c_match(C1, C5) && !c_match(C2, C4)) color = C1;
        else if (c_match(C2, C4) && !c_match(C1, C5)) color = C2;
        else color = 0.5 * (C1 + C5);
    }
    else if (f.x < 0.5 && f.y >= 0.5) {
        if (c_match(C6, C4) && !c_match(C3, C7)) color = C6;
        else if (c_match(C3, C7) && !c_match(C6, C4)) color = C3;
        else color = 0.5 * (C3 + C7);
    }
    else {
        if (c_match(C4, C8) && !c_match(C5, C7)) color = C4;
        else if (c_match(C5, C7) && !c_match(C4, C8)) color = C5;
        else color = 0.5 * (C5 + C7);
    }
    o_color = color;
}
)GLSL";

    // legacy_shaders.h[5]: the placeholder "xBRZ", a directional-edge blend.
    inline constexpr const char kGlslXbrzLegacy[] = R"GLSL(
// Helper function to calculate color distance (YUV or RGB distance)
float dist(vec4 c1, vec4 c2) {
    float r = c1.r - c2.r;
    float g = c1.g - c2.g;
    float b = c1.b - c2.b;
    // Basic weighted RGB distance for better edge detection
    return sqrt(r*r * 0.299 + g*g * 0.587 + b*b * 0.114);
}

void main() {
    // Obtain texture dimensions
    vec2 texSize = vec2(textureSize(u_point, 0));

    // Core pixel (E) and its immediate 3x3 neighbors
    vec2 f = fract(v_uv * texSize);

    // Add 0.5 to sample from the exact center of the texel and avoid floating-point boundary snapping
    vec2 ip = (floor(v_uv * texSize) + 0.5) / texSize;
    vec2 tp = 1.0 / texSize;

    // Sample the 3x3 matrix around the current pixel
    vec4 A = texture(u_point, ip + vec2(-tp.x, -tp.y));
    vec4 B = texture(u_point, ip + vec2( 0.0,  -tp.y));
    vec4 C = texture(u_point, ip + vec2( tp.x, -tp.y));
    vec4 D = texture(u_point, ip + vec2(-tp.x,  0.0));
    vec4 E = texture(u_point, ip + vec2( 0.0,   0.0)); // Center
    vec4 F = texture(u_point, ip + vec2( tp.x,  0.0));
    vec4 G = texture(u_point, ip + vec2(-tp.x,  tp.y));
    vec4 H = texture(u_point, ip + vec2( 0.0,   tp.y));
    vec4 I = texture(u_point, ip + vec2( tp.x,  tp.y));

    // Simple xBRZ blend weights calculation for a 5x upscale
    vec4 fp = vec4(f, 1.0 - f);

    // Scale logic: check pixel distances to blend colors smoothly
    float d_ED = dist(E, D);
    float d_EC = dist(E, C);
    float d_EB = dist(E, B);
    float d_EF = dist(E, F);

    vec4 color = E;

    // Blend logic based on directional edge analysis
    if (d_ED + d_EB < d_EC + d_EF) {
        if (fp.x + fp.y < 0.5)  color = mix(E, D, 0.5);
        if (fp.x + fp.y < 0.25) color = B;
    }
    if (d_EB + d_EF < d_ED + d_EC) {
        if (fp.z + fp.y < 0.5)  color = mix(E, F, 0.5);
        if (fp.z + fp.y < 0.25) color = B;
    }

    o_color = color;
}
)GLSL";

    // Resource/ps_scanline_filter.hlsl.
    inline constexpr const char kGlslScanline[] = R"GLSL(
void main() {
    vec4 c = texture(u_linear, v_uv);
    float scanline = sin(v_uv.y * u_params.y * 3.14159265 * 0.5) * 0.15 + 0.85;
    vec2 dc = abs(v_uv - 0.5) * 2.0;
    float vignette = 1.0 - pow(max(dc.x, dc.y), 4.0) * 0.3;
    c.rgb *= scanline * vignette;
    o_color = c;
}
)GLSL";

    // Resource/ps_xbrz_filter.hlsl.
    inline constexpr const char kGlslXbrz[] = R"GLSL(
// Helper function: Calculates perceptual brightness (Luma) difference to detect pixel-art edges
float ColorDiff(vec4 c1, vec4 c2) {
    // Standard YUV luma weights for accurate edge detection
    vec3 weights = vec3(0.299, 0.587, 0.114);
    return dot(abs(c1.rgb - c2.rgb), weights);
}

void main() {
    // 1. Automatically grab texture dimensions
    vec2 texSize = vec2(textureSize(u_point, 0));
    vec2 texel = 1.0 / texSize;

    // 2. Calculate integer pixel coordinates and fractional offsets
    vec2 pixel_coord = v_uv * texSize;
    vec2 center_pixel = floor(pixel_coord) + 0.5;
    vec2 f = fract(pixel_coord); // Range [0.0 to 1.0] inside the current pixel

    vec2 uv_center = center_pixel * texel;

    // 3. Sample the 3x3 neighborhood (Point sampling is optimal here)
    vec4 c11 = texture(u_point, uv_center); // Center Pixel

    vec4 c10 = texture(u_point, uv_center + vec2(0.0, -texel.y)); // Top
    vec4 c12 = texture(u_point, uv_center + vec2(0.0, texel.y)); // Bottom
    vec4 c01 = texture(u_point, uv_center + vec2(-texel.x, 0.0)); // Left
    vec4 c21 = texture(u_point, uv_center + vec2(texel.x, 0.0)); // Right

    vec4 c00 = texture(u_point, uv_center + vec2(-texel.x, -texel.y)); // Top-Left
    vec4 c20 = texture(u_point, uv_center + vec2(texel.x, -texel.y)); // Top-Right
    vec4 c02 = texture(u_point, uv_center + vec2(-texel.x, texel.y)); // Bottom-Left
    vec4 c22 = texture(u_point, uv_center + vec2(texel.x, texel.y)); // Bottom-Right

    // 4. Determine which quadrant of the current pixel we are rendering in
    vec2 quad = sign(f - 0.5); // Will be -1.0 or 1.0

    // Fetch the two orthogonal neighbors and the diagonal corner for the current quadrant
    vec4 n1 = (quad.y < 0.0) ? c10 : c12; // Vertical neighbor
    vec4 n2 = (quad.x < 0.0) ? c01 : c21; // Horizontal neighbor
    vec4 corner = (quad.x < 0.0 && quad.y < 0.0) ? c00 :
                  (quad.x > 0.0 && quad.y < 0.0) ? c20 :
                  (quad.x < 0.0 && quad.y > 0.0) ? c02 : c22;

    vec4 result = c11;

    // 5. Edge detection logic (xBR style)
    float edge_diff = ColorDiff(n1, n2);

    if (edge_diff < 0.15 && ColorDiff(c11, n1) > 0.1 && ColorDiff(c11, n2) > 0.1 && ColorDiff(c11, corner) > 0.1)
    {
        // 6. Smooth the corner
        vec2 dist_to_center = abs(f - 0.5);
        float diag_dist = dist_to_center.x + dist_to_center.y;

        // If we cross the diagonal threshold of the pixel corner, we blend it
        if (diag_dist > 0.5)
        {
            float blend = smoothstep(0.5, 1.0, diag_dist);
            result = mix(c11, n1, blend);
        }
    }

    o_color = result;
}
)GLSL";

    // A Super-xBR pass is three pieces joined when it is loaded (kGlslFilters): this, the pass's
    // own sampling and weights, and kGlslSuperXbrBlend. This one is
    // Resource/superxbr/superxbr_common.hlsli.
    inline constexpr const char kGlslSuperXbrCommon[] = R"GLSL(
// Settings
#define XBR_EDGE_STR 0.6
#define XBR_WEIGHT 1.0
#define XBR_ANTI_RINGING 1.0
#define MODE 0.0
#define XBR_EDGE_SHP 0.4
#define XBR_TEXTURE_SHP 1.0

const vec3 Y_VEC = vec3(0.2126, 0.7152, 0.0722);

float RGBtoYUV(vec3 color) { return dot(color, Y_VEC); }

float df(float A, float B) { return abs(A - B); }

float d_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6,
           float b0, float b1, float c0, float c1, float c2, float d0,
           float d1, float d2, float d3, float e1, float e2, float e3, float f2, float f3)
{
    return (wp1 * (df(c1, c2) + df(c1, c0) + df(e2, e1) + df(e2, e3)) +
            wp2 * (df(d2, d3) + df(d0, d1)) +
            wp3 * (df(d1, d3) + df(d0, d2)) +
            wp4 * df(d1, d2) +
            wp5 * (df(c0, c2) + df(e1, e3)) +
            wp6 * (df(b0, b1) + df(f2, f3)));
}

float hv_wd(float wp1, float wp2, float wp3, float wp4, float wp5, float wp6,
            float i1, float i2, float i3, float i4, float e1, float e2, float e3, float e4)
{
    return (wp4 * (df(i1, i2) + df(i3, i4)) +
            wp1 * (df(i1, e1) + df(i2, e2) + df(i3, e3) + df(i4, e4)) +
            wp3 * (df(i1, e2) + df(i3, e4) + df(e1, i2) + df(e3, i4)));
}

vec3 min4(vec3 a, vec3 b, vec3 c, vec3 d) { return min(a, min(b, min(c, d))); }
vec3 max4(vec3 a, vec3 b, vec3 c, vec3 d) { return max(a, max(b, max(c, d))); }
float max4float(float a, float b, float c, float d) { return max(a, max(b, max(c, d))); }
)GLSL";

    // The part every pass shares after its sixteen samples: edge weights, the blend and the
    // anti-ringing clamp, and the end of main(). In HLSL it is written out in each pass; only the
    // weights differ, and those stay in the passes.
    inline constexpr const char kGlslSuperXbrBlend[] = R"GLSL(
    float b = RGBtoYUV(B);
    float c = RGBtoYUV(C);
    float d = RGBtoYUV(D);
    float e = RGBtoYUV(E);
    float f = RGBtoYUV(F);
    float g = RGBtoYUV(G);
    float h = RGBtoYUV(H);
    float i = RGBtoYUV(I);

    float i4 = RGBtoYUV(I4);
    float p0 = RGBtoYUV(P0);
    float i5 = RGBtoYUV(I5);
    float p1 = RGBtoYUV(P1);
    float h5 = RGBtoYUV(H5);
    float p2 = RGBtoYUV(P2);
    float f4 = RGBtoYUV(F4);
    float p3 = RGBtoYUV(P3);

    float d_edge = d_wd(wp1, wp2, wp3, wp4, wp5, wp6, d, b, g, e, c, p2, h, f, p1, h5, i, f4, i5, i4) -
                   d_wd(wp1, wp2, wp3, wp4, wp5, wp6, c, f4, b, f, i4, p0, e, i, p3, d, h, i5, g, h5);
    float hv_edge = hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, f, i, e, h, c, i5, b, h5) -
                    hv_wd(wp1, wp2, wp3, wp4, wp5, wp6, e, f, h, i, d, f4, g, i4);

    float limits = XBR_EDGE_STR + 0.000001;
    float edge_strength = smoothstep(0.0, limits, abs(d_edge));

    vec4 w1 = vec4(-weight1, weight1 + 0.5, weight1 + 0.5, -weight1);
    vec4 w2 = vec4(-weight2, weight2 + 0.25, weight2 + 0.25, -weight2);

    vec3 c3 = w2.x * (D + G) + w2.y * (E + H) + w2.z * (F + I) + w2.w * (F4 + I4);
    vec3 c4 = w2.x * (C + B) + w2.y * (F + E) + w2.z * (I + H) + w2.w * (I5 + H5);

    vec3 c1 = w1.x * P2 + w1.y * H + w1.z * F + w1.w * P1;
    vec3 c2 = w1.x * P0 + w1.y * E + w1.z * I + w1.w * P3;

    vec3 color = mix(mix(c1, c2, step(0.0, d_edge)), mix(c3, c4, step(0.0, hv_edge)), 1.0 - edge_strength);

    vec3 min_sample = min4(E, F, H, I) + (1.0 - XBR_ANTI_RINGING) * mix((P2 - H) * (F - P1), (P0 - E) * (I - P3), step(0.0, d_edge));
    vec3 max_sample = max4(E, F, H, I) - (1.0 - XBR_ANTI_RINGING) * mix((P2 - H) * (F - P1), (P0 - E) * (I - P3), step(0.0, d_edge));
    color = clamp(color, min_sample, max_sample);

    o_color = vec4(color, 1.0);
}
)GLSL";

    // superxbr_pass0.hlsl: reads the frame (s2 - clamped, as the HLSL comment there explains).
    inline constexpr const char kGlslSuperXbrPass0[] = R"GLSL(
void main() {
    vec2 uv = v_uv;
    vec2 TextureSize = u_params.zw;
    float dx = 1.0 / TextureSize.x;
    float dy = 1.0 / TextureSize.y;

    vec3 P0 = texture(u_point_clamp, uv + vec2(-dx, -dy)).xyz;
    vec3 P1 = texture(u_point_clamp, uv + vec2(2.0 * dx, -dy)).xyz;
    vec3 P2 = texture(u_point_clamp, uv + vec2(-dx, 2.0 * dy)).xyz;
    vec3 P3 = texture(u_point_clamp, uv + vec2(2.0 * dx, 2.0 * dy)).xyz;

    vec3 B = texture(u_point_clamp, uv + vec2(0, -dy)).xyz;
    vec3 C = texture(u_point_clamp, uv + vec2(dx, -dy)).xyz;
    vec3 H5 = texture(u_point_clamp, uv + vec2(0, 2.0 * dy)).xyz;
    vec3 I5 = texture(u_point_clamp, uv + vec2(dx, 2.0 * dy)).xyz;

    vec3 D = texture(u_point_clamp, uv + vec2(-dx, 0)).xyz;
    vec3 F4 = texture(u_point_clamp, uv + vec2(2.0 * dx, 0)).xyz;
    vec3 G = texture(u_point_clamp, uv + vec2(-dx, dy)).xyz;
    vec3 I4 = texture(u_point_clamp, uv + vec2(2.0 * dx, dy)).xyz;

    vec3 E = texture(u_point_clamp, uv).xyz;
    vec3 F = texture(u_point_clamp, uv + vec2(dx, 0)).xyz;
    vec3 H = texture(u_point_clamp, uv + vec2(0, dy)).xyz;
    vec3 I = texture(u_point_clamp, uv + vec2(dx, dy)).xyz;

    float wp1 = 2.0, wp2 = 1.0, wp3 = -1.0, wp4 = 4.0, wp5 = -1.0, wp6 = 1.0;
    float weight1 = (XBR_WEIGHT * 1.29633 / 10.0);
    float weight2 = (XBR_WEIGHT * 1.75068 / 10.0 / 2.0);
)GLSL";

    // superxbr_pass1.hlsl: reads pass 0's output and the original frame.
    inline constexpr const char kGlslSuperXbrPass1[] = R"GLSL(
void main() {
    vec2 uv = v_uv;
    vec2 TextureSize = u_params.zw;
    vec2 fp = fract(uv * TextureSize);
    vec2 dir = fp - vec2(0.5, 0.5);

    if ((dir.x * dir.y) > 0.0)
    {
        o_color = (fp.x > 0.5) ? texture(u_point_clamp, uv) : texture(u_original, uv);
        return;
    }

    vec2 g1 = (fp.x > 0.5) ? vec2(0.5 / TextureSize.x, 0.0) : vec2(0.0, 0.5 / TextureSize.y);
    vec2 g2 = (fp.x > 0.5) ? vec2(0.0, 0.5 / TextureSize.y) : vec2(0.5 / TextureSize.x, 0.0);

    vec3 P0 = texture(u_original, uv - 3.0 * g1).xyz;
    vec3 P1 = texture(u_point_clamp, uv - 3.0 * g2).xyz;
    vec3 P2 = texture(u_point_clamp, uv + 3.0 * g2).xyz;
    vec3 P3 = texture(u_original, uv + 3.0 * g1).xyz;

    vec3 B = texture(u_point_clamp, uv - 2.0 * g1 - g2).xyz;
    vec3 C = texture(u_original, uv - g1 - 2.0 * g2).xyz;
    vec3 D = texture(u_point_clamp, uv - 2.0 * g1 + g2).xyz;
    vec3 E = texture(u_original, uv - g1).xyz;
    vec3 F = texture(u_point_clamp, uv - g2).xyz;
    vec3 G = texture(u_original, uv - g1 + 2.0 * g2).xyz;
    vec3 H = texture(u_point_clamp, uv + g2).xyz;
    vec3 I = texture(u_original, uv + g1).xyz;

    vec3 F4 = texture(u_original, uv + g1 - 2.0 * g2).xyz;
    vec3 I4 = texture(u_point_clamp, uv + 2.0 * g1 - g2).xyz;
    vec3 H5 = texture(u_original, uv + g1 + 2.0 * g2).xyz;
    vec3 I5 = texture(u_point_clamp, uv + 2.0 * g1 + g2).xyz;

    float wp1 = 8.0, wp2 = 0.0, wp3 = 0.0, wp4 = 0.0, wp5 = 0.0, wp6 = 0.0;
    float weight1 = (XBR_WEIGHT * 1.75068 / 10.0);
    float weight2 = (XBR_WEIGHT * 1.29633 / 10.0 / 2.0);
)GLSL";

    // superxbr_pass2.hlsl: reads pass 1's output.
    inline constexpr const char kGlslSuperXbrPass2[] = R"GLSL(
void main() {
    vec2 uv = v_uv;
    vec2 TextureSize = u_params.zw;
    float dx = 1.0 / TextureSize.x;
    float dy = 1.0 / TextureSize.y;

    vec3 P0 = texture(u_point_clamp, uv + vec2(-2.0 * dx, -2.0 * dy)).xyz;
    vec3 P1 = texture(u_point_clamp, uv + vec2(dx, -2.0 * dy)).xyz;
    vec3 P2 = texture(u_point_clamp, uv + vec2(-2.0 * dx, dy)).xyz;
    vec3 P3 = texture(u_point_clamp, uv + vec2(dx, dy)).xyz;

    vec3 B = texture(u_point_clamp, uv + vec2(-dx, -2.0 * dy)).xyz;
    vec3 C = texture(u_point_clamp, uv + vec2(0, -2.0 * dy)).xyz;
    vec3 H5 = texture(u_point_clamp, uv + vec2(-dx, dy)).xyz;
    vec3 I5 = texture(u_point_clamp, uv + vec2(0, dy)).xyz;

    vec3 D = texture(u_point_clamp, uv + vec2(-2.0 * dx, -dy)).xyz;
    vec3 F4 = texture(u_point_clamp, uv + vec2(dx, -dy)).xyz;
    vec3 G = texture(u_point_clamp, uv + vec2(-2.0 * dx, 0)).xyz;
    vec3 I4 = texture(u_point_clamp, uv + vec2(dx, 0)).xyz;

    vec3 E = texture(u_point_clamp, uv + vec2(-dx, -dy)).xyz;
    vec3 F = texture(u_point_clamp, uv + vec2(0, -dy)).xyz;
    vec3 H = texture(u_point_clamp, uv + vec2(-dx, 0)).xyz;
    vec3 I = texture(u_point_clamp, uv).xyz;

    float wp1 = 1.0, wp2 = 0.0, wp3 = 2.0, wp4 = 3.0, wp5 = -2.0, wp6 = 1.0;
    float weight1 = (XBR_WEIGHT * 1.29633 / 10.0);
    float weight2 = (XBR_WEIGHT * 1.75068 / 10.0 / 2.0);
)GLSL";

    // Every single-shader filter, by the key the Video > Filter menu and LoadAllFilters use.
    // HQ2X is a placeholder in HLSL too - the same pass-through as Nearest Neighbor.
    struct GlslFilter {
        const char* key;
        const char* parts[3];   // joined in order; unused ones null
    };

    // clang-format off
    inline constexpr GlslFilter kGlslFilters[] = {
        { "nearest",        { kGlslDefault } },
        { "bilinear",       { kGlslSharpBilinear } },
        { "crt",            { kGlslCrtLottes } },
        { "eagle",          { kGlslSuperEagle } },
        { "hq2x",           { kGlslDefault } },
        { "xbrz_legacy",    { kGlslXbrzLegacy } },
        { "scanline",       { kGlslScanline } },
        { "xbrz",           { kGlslXbrz } },
        { "superxbr_pass0", { kGlslSuperXbrCommon, kGlslSuperXbrPass0, kGlslSuperXbrBlend } },
        { "superxbr_pass1", { kGlslSuperXbrCommon, kGlslSuperXbrPass1, kGlslSuperXbrBlend } },
        { "superxbr_pass2", { kGlslSuperXbrCommon, kGlslSuperXbrPass2, kGlslSuperXbrBlend } },
    };
    // clang-format on

    // Every shader the Vulkan engine runs, as the pieces make_spirv.cpp joins and compiles: the
    // vertex shader, the pass-through, the blit that ends a chain, then kGlslFilters in order, each
    // behind kVulkanFilterHeader. spirv_filters.h holds their SPIR-V in the same order.
    struct VulkanShaderSource {
        const char* key;
        const char* parts[4];   // joined in order; unused ones null
    };

    inline constexpr size_t kVulkanShaderCount = 3 + std::size(kGlslFilters);

    constexpr VulkanShaderSource VulkanShaderSourceAt(size_t index) {
        if (index == 0)
            return { "vertex", { kVulkanVertexShader, nullptr, nullptr, nullptr } };
        if (index == 1)
            return { "default", { kVulkanFilterHeader, kGlslDefault, nullptr, nullptr } };
        if (index == 2)
            return { "blit", { kVulkanFilterHeader, kGlslBlit, nullptr, nullptr } };
        const GlslFilter& filter = kGlslFilters[index - 3];
        return { filter.key,
                 { kVulkanFilterHeader, filter.parts[0], filter.parts[1], filter.parts[2] } };
    }

    // FNV-1a, 64-bit, of one piece of shader source (0 for none). spirv_filters.h records the
    // hash of every piece it was compiled from and checks them at compile time, so a filter
    // changed here without its SPIR-V being rebuilt stops the build instead of drawing the old one.
    constexpr uint64_t ShaderSourceHash(const char* text) {
        if (text == nullptr)
            return 0;
        uint64_t hash = 14695981039346656037ull;
        for (; *text != 0; ++text) {
            hash ^= static_cast<unsigned char>(*text);
            hash *= 1099511628211ull;
        }
        return hash;
    }

}   // namespace psxemu

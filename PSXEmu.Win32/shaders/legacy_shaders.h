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

// Ported from GBAEmu's legacy_shaders.h - six pixel shaders of varying
// completeness, kept together because that is how the source project keeps
// them. Two edits from the original, applied to every one of the six:
//
// GBAEmu's framebuffer texture is R8G8B8A8, but its own source data is
// packed the other way round, so every shader ends with an explicit
// `float4(c.b, c.g, c.r, c.a)` to fix that up. This project's framebuffer
// texture is B8G8R8A8 - a byte-for-byte match for what Gpu::ResolveFramebuffer
// actually packs - so that swap would put the channels back in the wrong
// order here. Removed from all six.
//
// Index 1, Sharp Bilinear, re-derived its own letterbox aspect from the
// input texture's own pixel ratio (`aspect = inW/inH`, clamped into
// `viewSize`) - the same mistake bug 47 fixed on the C++ side, reintroduced
// inside a shader. `outW`/`outH` are now always the already-letterboxed
// viewport size by the time they reach here (see d3d12_graphics_engine.cpp),
// so the shader just needs the straight ratio between that and the input
// texture size - no re-derivation, no clamping.
inline const char* kLegacyShaders[6] = {
    // 0: Nearest Neighbor
    R"HLSL(
    Texture2D g_Tex : register(t0); SamplerState g_Point : register(s0);
    float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD0) : SV_TARGET {
        return g_Tex.Sample(g_Point, uv);
    })HLSL",
    // 1: Sharp Bilinear
    R"HLSL(
    Texture2D g_Tex : register(t0);
    SamplerState g_Linear : register(s1);
    cbuffer Params : register(b0) { float outW, outH, inW, inH; };

    float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD0) : SV_TARGET {
        float2 texSize = float2(inW, inH);
        float2 scale = float2(outW, outH) / texSize;

        // Position in texel coordinates
        float2 texel = uv * texSize;
        float2 texel_floored = floor(texel);
        float2 s = frac(texel);

        // Sharp bilinear: nearest neighbor in the center of each texel,
        // smooth bilinear interpolation only at the 1-output-pixel-wide edges
        float2 region_range = 0.5 - 0.5 / scale;
        float2 center_dist = s - 0.5;
        float2 f = (center_dist - clamp(center_dist, -region_range, region_range)) * scale + 0.5;

        float2 newUV = (texel_floored + f) / texSize;
        return g_Tex.Sample(g_Linear, newUV);
    })HLSL",
    // 2: CRT-Lottes (Public Domain by Timothy Lottes)
    R"HLSL(
    Texture2D g_Tex : register(t0);
    SamplerState g_Point : register(s0);
    cbuffer Params : register(b0) { float outW, outH, inW, inH; };

    // --- Tunable CRT parameters ---
    static const float hardScan      = -8.0;   // Scanline hardness (-20=soft, 0=hard)
    static const float hardPix       = -3.0;   // Pixel sharpness (-20=soft, 0=sharp)
    static const float2 crtWarp      = float2(0.031, 0.041); // Barrel distortion
    static const float maskDark      = 0.5;    // Shadow mask dark level
    static const float maskLight     = 1.5;    // Shadow mask bright level
    static const float brightBoost   = 1.15;   // Compensate scanline darkening
    static const float hardBloomScan = -2.0;   // Bloom scanline softness
    static const float hardBloomPix  = -1.5;   // Bloom pixel softness
    static const float bloomAmount   = 0.12;   // Bloom intensity
    static const float crtShape      = 2.0;    // Gaussian shape exponent

    // --- sRGB gamma ---
    float ToLinear1(float c) { return (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
    float3 ToLinear(float3 c) { return float3(ToLinear1(c.r), ToLinear1(c.g), ToLinear1(c.b)); }
    float ToSrgb1(float c) { return (c < 0.0031308) ? c * 12.92 : 1.055 * pow(c, 1.0/2.4) - 0.055; }
    float3 ToSrgb(float3 c) { return float3(ToSrgb1(c.r), ToSrgb1(c.g), ToSrgb1(c.b)); }

    // Fetch a texel at exact integer offset and convert to linear
    float3 Fetch(float2 pos, float2 off) {
        float2 res = float2(inW, inH);
        pos = (floor(pos * res + off) + 0.5) / res;
        if (max(abs(pos.x - 0.5), abs(pos.y - 0.5)) > 0.5) return float3(0,0,0);
        float3 c = g_Tex.SampleLevel(g_Point, pos, 0).rgb;
        return ToLinear(c) * brightBoost;
    }

    // Gaussian weight function
    float Gaus(float p, float scale) { return exp2(scale * pow(abs(p), crtShape)); }

    // 3-tap horizontal filter (sharp)
    float3 Horz3(float2 pos, float off) {
        float3 b = Fetch(pos, float2(-1.0, off));
        float3 c = Fetch(pos, float2( 0.0, off));
        float3 d = Fetch(pos, float2( 1.0, off));
        float dst = frac(pos.x * inW) - 0.5;
        float wb = Gaus(dst - 1.0, hardPix);
        float wc = Gaus(dst,       hardPix);
        float wd = Gaus(dst + 1.0, hardPix);
        return (b * wb + c * wc + d * wd) / (wb + wc + wd);
    }

    // 5-tap horizontal filter (bloom / soft glow)
    float3 Horz5(float2 pos, float off) {
        float3 a = Fetch(pos, float2(-2.0, off));
        float3 b = Fetch(pos, float2(-1.0, off));
        float3 c = Fetch(pos, float2( 0.0, off));
        float3 d = Fetch(pos, float2( 1.0, off));
        float3 e = Fetch(pos, float2( 2.0, off));
        float dst = frac(pos.x * inW) - 0.5;
        float wa = Gaus(dst - 2.0, hardBloomPix);
        float wb = Gaus(dst - 1.0, hardBloomPix);
        float wc = Gaus(dst,       hardBloomPix);
        float wd = Gaus(dst + 1.0, hardBloomPix);
        float we = Gaus(dst + 2.0, hardBloomPix);
        return (a*wa + b*wb + c*wc + d*wd + e*we) / (wa+wb+wc+wd+we);
    }

    // Scanline weight (vertical Gaussian)
    float Scan(float2 pos, float off) {
        return Gaus(frac(pos.y * inH) - 0.5 + off, hardScan);
    }
    float BloomScan(float2 pos, float off) {
        return Gaus(frac(pos.y * inH) - 0.5 + off, hardBloomScan);
    }

    // Barrel distortion (screen curvature)
    float2 Warp(float2 pos) {
        pos = pos * 2.0 - 1.0;
        pos *= float2(1.0 + pos.y * pos.y * crtWarp.x, 1.0 + pos.x * pos.x * crtWarp.y);
        return pos * 0.5 + 0.5;
    }

    // Shadow mask (stretched VGA-style phosphor pattern)
    float3 Mask(float2 pos) {
        float3 m = float3(maskDark, maskDark, maskDark);
        pos.x += pos.y * 3.0;
        pos.x = frac(pos.x / 6.0);
        if      (pos.x < 0.333) m.r = maskLight;
        else if (pos.x < 0.666) m.g = maskLight;
        else                    m.b = maskLight;
        return m;
    }

    float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
        float2 p = Warp(uv);
        // Combine two adjacent scanlines with Gaussian weights
        float3 color = Horz3(p, -1.0) * Scan(p, -1.0)
                     + Horz3(p,  0.0) * Scan(p,  0.0);
        // Bloom: wider Gaussian pass for soft glow
        float3 bloom = Horz5(p, -1.0) * BloomScan(p, -1.0)
                     + Horz5(p,  0.0) * BloomScan(p,  0.0);
        color += bloom * bloomAmount;
        // Apply phosphor shadow mask
        color *= Mask(pos.xy);
        return float4(ToSrgb(color), 1.0);
    })HLSL",
    // 3: SuperEagle
    R"HLSL(
    Texture2D g_Tex : register(t0);
    SamplerState g_Point : register(s0);

    struct PS_INPUT {
        float4 pos : SV_POSITION;
        float2 uv  : TEXCOORD0;
    };

    bool c_match(float4 c1, float4 c2) {
        return all(abs(c1 - c2) < 0.01);
    }

    float4 main(PS_INPUT input) : SV_TARGET
    {
        float2 uv = input.uv;
        uint width, height;
        g_Tex.GetDimensions(width, height);
        float2 texel = float2(1.0 / width, 1.0 / height);

        float4 C0 = g_Tex.Sample(g_Point, uv + float2(-texel.x, -texel.y));
        float4 C1 = g_Tex.Sample(g_Point, uv + float2(     0.0, -texel.y));
        float4 C2 = g_Tex.Sample(g_Point, uv + float2( texel.x, -texel.y));
        float4 C3 = g_Tex.Sample(g_Point, uv + float2(-texel.x,      0.0));
        float4 C4 = g_Tex.Sample(g_Point, uv + float2(     0.0,      0.0)); // Center
        float4 C5 = g_Tex.Sample(g_Point, uv + float2( texel.x,      0.0));
        float4 C6 = g_Tex.Sample(g_Point, uv + float2(-texel.x,  texel.y));
        float4 C7 = g_Tex.Sample(g_Point, uv + float2(     0.0,  texel.y));
        float4 C8 = g_Tex.Sample(g_Point, uv + float2( texel.x,  texel.y));

        float2 f = frac(uv * float2(width, height));
        float4 color = C4;

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
        return color;
    })HLSL",
    // 4: HQ2X (Placeholder) - not a real HQ2X implementation, kept for
    // parity with GBAEmu's own set. Identical to Nearest Neighbor above.
    R"HLSL(
    Texture2D g_Tex : register(t0); SamplerState g_Point : register(s0);
    float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD0) : SV_TARGET {
        return g_Tex.Sample(g_Point, uv);
    })HLSL",
    // 5: xBRZ (Placeholder) - a simple directional-edge blend, distinct from
    // the real xBRZ filter in ps_xbrz_filter.h, kept under its own "xbrz_legacy"
    // key so the two are never confused with each other.
    R"HLSL(
    Texture2D g_Tex : register(t0);
    SamplerState g_Point : register(s0);

    // Helper function to calculate color distance (YUV or RGB distance)
    float dist(float4 c1, float4 c2) {
        float r = c1.r - c2.r;
        float g = c1.g - c2.g;
        float b = c1.b - c2.b;
        // Basic weighted RGB distance for better edge detection
        return sqrt(r*r * 0.299 + g*g * 0.587 + b*b * 0.114);
    }

    struct VS_OUTPUT {
        float4 pos : SV_POSITION;
        float2 uv  : TEXCOORD0;
    };

    float4 main(VS_OUTPUT input) : SV_TARGET {
        // Obtain texture dimensions
        uint width, height;
        g_Tex.GetDimensions(width, height);
        float2 texSize = float2(width, height);

        // Core pixel (E) and its immediate 3x3 neighbors
        float2 f = frac(input.uv * texSize);

        // Add 0.5 to sample from the exact center of the texel and avoid floating-point boundary snapping
        float2 ip = (floor(input.uv * texSize) + 0.5) / texSize;
        float2 tp = 1.0 / texSize;

        // Sample the 3x3 matrix around the current pixel
        float4 A = g_Tex.Sample(g_Point, ip + float2(-tp.x, -tp.y));
        float4 B = g_Tex.Sample(g_Point, ip + float2( 0.0,  -tp.y));
        float4 C = g_Tex.Sample(g_Point, ip + float2( tp.x, -tp.y));
        float4 D = g_Tex.Sample(g_Point, ip + float2(-tp.x,  0.0));
        float4 E = g_Tex.Sample(g_Point, ip + float2( 0.0,   0.0)); // Center
        float4 F = g_Tex.Sample(g_Point, ip + float2( tp.x,  0.0));
        float4 G = g_Tex.Sample(g_Point, ip + float2(-tp.x,  tp.y));
        float4 H = g_Tex.Sample(g_Point, ip + float2( 0.0,   tp.y));
        float4 I = g_Tex.Sample(g_Point, ip + float2( tp.x,  tp.y));

        // Simple xBRZ blend weights calculation for a 5x upscale
        float4 fp = float4(f, 1.0 - f);

        // Scale logic: check pixel distances to blend colors smoothly
        float d_ED = dist(E, D);
        float d_EC = dist(E, C);
        float d_EB = dist(E, B);
        float d_EF = dist(E, F);

        float4 color = E;

        // Blend logic based on directional edge analysis
        if (d_ED + d_EB < d_EC + d_EF) {
            if (fp.x + fp.y < 0.5)  color = lerp(E, D, 0.5);
            if (fp.x + fp.y < 0.25) color = B;
        }
        if (d_EB + d_EF < d_ED + d_EC) {
            if (fp.z + fp.y < 0.5)  color = lerp(E, F, 0.5);
            if (fp.z + fp.y < 0.25) color = B;
        }

        return color;
    })HLSL"
};

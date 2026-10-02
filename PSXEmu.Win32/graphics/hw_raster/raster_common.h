/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

// What the two hardware rasterisers - Direct3D 11's (d3d11_raster) and Direct3D 12's
// (d3d12_raster) - draw with in common: the shaders, the words each vertex carries for them, and
// the arithmetic around both. One copy, so the two cannot come to draw differently.

#include "psx/raster.h"
#include "psx/shared_picture.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace psxemu {
namespace raster {

    // What each vertex carries for the pixel shader, the same at every corner of a
    // primitive - `p` in the vertex, P0-P2 in the shader:
    //
    //   p[8]  attributes: texture page x (bits 0-9) and y (10-18), depth (19-20), textured
    //         (21), raw (22), semi-transparent (23), blend mode (24-25), dithered (26), kind
    //         (27-28), Gouraud (29)
    //   p[9]  CLUT x (0-9) and y (10-18)
    //
    // and by kind:
    //
    //   0, a triangle, its vertices a, b, c in the winding the software rasteriser turns
    //   every triangle to (positive area):
    //     p[0-2]  each one's x and y, 16 bits apiece
    //     p[3-5]  each one's colour, and u in the top byte
    //     p[6]    their v's, and which of the edges ab, bc, ca the fill rule leaves out
    //             (bits 24-26)
    //     p[7]    twice the area
    //   1, a rectangle:
    //     p[0]    its top-left corner, which the texture is counted from
    //     p[3]    its colour, and u at that corner in the top byte
    //     p[6]    v at that corner, and whether it is flipped across (bit 8) and down (9)
    //   2, a pixel of a line, or a fill:
    //     p[3]    its colour
    //   a copy (PsCopy):
    //     p[0]    where this piece of the destination starts
    //     p[1]    where its source starts
    //
    // and for any triangle or rectangle, its motion, which only the plane beside VRAM takes
    // (Docs/DLSS-Plan.md, phase 2) - where each corner was in the last picture minus where it
    // is, in 64ths of a console pixel, x and y 16 bits apiece:
    //   p[16-18]  corners a, b, c's (a rectangle's whole motion in p[16])
    //   p[19]     which of them is known, bits 0-2
    enum Kind : uint32_t { kKindTriangle = 0, kKindRectangle = 1, kKindFlat = 2, kKindPrecise = 3 };

    // Words a vertex carries for the pixel shader: 16 for the primitive, and 4 for its motion,
    // which only the plane takes (Docs/DLSS-Plan.md, phase 2).
    inline constexpr int kPayload = 20;

    // A corner of what is drawn, and - the same at every corner - what the pixel shader needs to
    // work out each pixel of the primitive.
    struct Vertex {
        float x, y;
        uint32_t p[kPayload];
    };

    // What the pixel shaders are told, per batch. The layout is the HLSL cbuffer's - and twelve
    // words, which Direct3D 12 passes as root constants.
    struct Constants {
        int32_t skip_field;        // leave the rows of the displayed field alone: 1, or 2 all
                                   // but each pixel's console sample (FillsSkippedFields)
        int32_t active_line_lsb;   // ...which are those with this low bit
        int32_t force_mask;        // GP0(E6h) bit 0
        int32_t check_mask;        // GP0(E6h) bit 1
        int32_t tw_mask_x, tw_mask_y, tw_offset_x, tw_offset_y;   // the texture window
        float jitter_x, jitter_y;  // a triangle's sample point within its sub-pixel (Begin)
        int32_t pad[2];
    };
    static_assert(sizeof(Constants) == 12 * 4, "twelve words");

    // The shaders, by what they do.
    enum Shader {
        kShaderDraw, kShaderCopy, kShaderDownsample, kShaderExpand, kShaderDisplay,
        kShaderDisplayDepth, kShaderDisplayMotion,
        kShaderCount
    };
    inline constexpr const char* kShaderEntries[kShaderCount] = {
        "PsDraw", "PsCopy", "PsDownsample", "PsExpand", "PsDisplay", "PsDisplayDepth",
        "PsDisplayMotion",
    };

    // Everything the rasteriser draws with, compiled when it is made - for its scale, which
    // the shaders take as the constant SCALE, so at 1x they are the native code and nothing
    // more. The arithmetic is the software rasteriser's (psx/software_raster.cpp) line for
    // line, in integers.
    //
    // The viewport is VRAM at SCALE times its size, and every primitive is drawn as
    // rectangles whose edges fall on the console's pixel boundaries: a pixel of the target
    // is a sub-pixel of the console's, SCALE by SCALE of them to one, and exactly the ones
    // inside a rectangle run the shader. (At 1x a line's pixels are points, half a pixel in.)
    //
    // A console pixel's top-left sub-pixel is its own sample point: there the edge
    // functions are the software rasteriser's times SCALE squared, exactly, so that one
    // sub-pixel is decided and coloured with the software rasteriser's own integers, reads
    // its textures and the pixel underneath from the same sub-pixel of theirs, and so comes
    // out as the console's pixel. Native VRAM is downloaded from those sub-pixels alone,
    // which keeps what the machine sees the same at every scale. The other sub-pixels
    // interpolate between the same vertices where they are, which is the detail.
    //
    // TRUE_COLOR (above 1x only): no dithering, and eight bits a channel kept rather than
    // five, the pixel underneath included. It gives up the sub-pixel exactness above.
    //
    // PLANES: PsDraw and PsCopy write the plane beside VRAM too (psx/shared_picture.h), as a
    // second target - which is what DLSS will need to know of each sub-pixel. Compiled both
    // ways, so with the plane not kept the shaders are exactly what they were.
    inline constexpr char kShaderSource[] =
        "static const int scale = SCALE;\n"
        "static const bool fine = SCALE > 1 && TRUE_COLOR != 0;\n"
        "static const float unknown_motion = UNKNOWN_MOTION;\n"
        "static const float depth_scale = DEPTH_SCALE;\n"
        "\n"
        "cbuffer Constants : register(b0) {\n"
        "  int skip_field;\n"
        "  int active_line_lsb;\n"
        "  int force_mask;\n"
        "  int check_mask;\n"
        "  int tw_mask_x;\n"
        "  int tw_mask_y;\n"
        "  int tw_offset_x;\n"
        "  int tw_offset_y;\n"
        "  // Where a triangle is sampled within each sub-pixel, for DLSS (Docs/DLSS-Plan.md,\n"
        "  // phase 3): 0 but while jittering.\n"
        "  float2 jitter;\n"
        "};\n"
        "// The read copy, for drawing and copies; the target or native VRAM's copy on the\n"
        "// card for a download or an upload; the plane, to show it.\n"
        "Texture2D<float4> source : register(t0);\n"
        "// The plane's read copy, for copies to carry it along.\n"
        "Texture2D<float4> plane_source : register(t1);\n"
        "\n"
        "#if PLANES\n"
        "struct Out {\n"
        "  float4 colour : SV_TARGET0;\n"
        "  float4 plane : SV_TARGET1;\n"
        "};\n"
        "#endif\n"
        "\n"
        "struct VsIn {\n"
        "  float2 position : POSITION;\n"
        "  uint4 p0 : P0;\n"
        "  uint4 p1 : P1;\n"
        "  uint4 p2 : P2;\n"
        "  uint4 p3 : P3;\n"
        "  uint4 p4 : P4;\n"
        "};\n"
        "struct VsOut {\n"
        "  float4 position : SV_POSITION;\n"
        "  nointerpolation uint4 p0 : P0;\n"
        "  nointerpolation uint4 p1 : P1;\n"
        "  nointerpolation uint4 p2 : P2;\n"
        "  nointerpolation uint4 p3 : P3;\n"
        "  nointerpolation uint4 p4 : P4;\n"
        "};\n"
        "\n"
        "// Positions are in the console's pixels, whatever the viewport's size.\n"
        "VsOut VsMain(VsIn input) {\n"
        "  VsOut output;\n"
        "  output.position = float4(input.position.x / 512.0 - 1.0,\n"
        "                           1.0 - input.position.y / 256.0, 0.0, 1.0);\n"
        "  output.p0 = input.p0;\n"
        "  output.p1 = input.p1;\n"
        "  output.p2 = input.p2;\n"
        "  output.p3 = input.p3;\n"
        "  output.p4 = input.p4;\n"
        "  return output;\n"
        "}\n"
        "\n"
        "static const int kDither[16] = { -4, 0, -3, 1,  2, -2, 3, -1,\n"
        "                                 -3, 1, -4, 0,  3, -1, 2, -2 };\n"
        "\n"
        "int2 XY(uint w) { return int2(asint(w << 16) >> 16, asint(w) >> 16); }\n"
        "// A corner's motion (p[16-18]), in console pixels; and three of them weighted.\n"
        "float2 Motion(uint w) { return float2(XY(w)) / 64.0; }\n"
        "float2 Weigh(float3 l, uint4 m) {\n"
        "  return l.x * Motion(m.x) + l.y * Motion(m.y) + l.z * Motion(m.z);\n"
        "}\n"
        "uint3 RGB(uint w) { return uint3(w & 255u, (w >> 8) & 255u, (w >> 16) & 255u); }\n"
        "int Widen8(int c5) { return (c5 << 3) | (c5 >> 2); }\n"
        "int3 Widen(int pixel) {\n"
        "  return int3(Widen8(pixel & 31), Widen8((pixel >> 5) & 31), Widen8((pixel >> 10) & 31));\n"
        "}\n"
        "float Cut(int c8) { return (float)Widen8(c8 >> 3) / 255.0; }\n"
        "\n"
        "// Sub-pixel (sx, sy) of VRAM pixel (x, y) in the read copy: eight bits a channel, and\n"
        "// alpha 255 where the mask bit is set.\n"
        "int4 Texel(int x, int y, int sx, int sy) {\n"
        "  return int4(source.Load(int3((x & 1023) * scale + sx, (y & 511) * scale + sy, 0)) *\n"
        "              255.0 + 0.5);\n"
        "}\n"
        "// ...as the 16 bits the console keeps.\n"
        "int Pack(int4 c) {\n"
        "  return (c.r >> 3) | ((c.g >> 3) << 5) | ((c.b >> 3) << 10) | (c.a >= 128 ? 0x8000 : 0);\n"
        "}\n"
        "int Fetch(int x, int y) { return Pack(Texel(x, y, 0, 0)); }\n"
        "\n"
        "// A precise triangle's edge function from P to Q at p. Worked out from the two ends in\n"
        "// one fixed order and negated for the other, so the triangle on the far side of a shared\n"
        "// edge gets exactly the same value with the opposite sign: in floating point the two\n"
        "// orders round differently, and a sample on the edge could fail both tests, leaving a\n"
        "// pixel in neither - a crack along every long thin shared edge.\n"
        "float Edge(float2 P, float2 Q, float2 p) {\n"
        "  bool swap = P.x > Q.x || (P.x == Q.x && P.y > Q.y);\n"
        "  float2 a = swap ? Q : P, b = swap ? P : Q;\n"
        "  float e = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);\n"
        "  return swap ? -e : e;\n"
        "}\n"
        "\n"
        "#if PLANES\n"
        "Out PsDraw(VsOut input) {\n"
        "#else\n"
        "float4 PsDraw(VsOut input) : SV_TARGET {\n"
        "#endif\n"
        "  int2 at = int2(input.position.xy);\n"
        "  int2 pixel = at / scale;\n"
        "  int2 sub = at - pixel * scale;\n"
        "  bool exact = sub.x == 0 && sub.y == 0;\n"
        "  float inverse_depth = 0.0;   // 1 / the depth here, where PGXP gave every vertex one\n"
        "  bool direct = false;         // a 15-bit texel, read from here in the target:\n"
        "  int2 direct_at = int2(0, 0); // what may be a picture the game drew, and its plane\n"
        "  // Where this was in the last picture minus where it is, in console pixels, when\n"
        "  // every corner's is known (p[16-19]).\n"
        "  bool motion_known = (input.p4.w & 7u) == 7u;\n"
        "  float2 motion = float2(0.0, 0.0);\n"
        "  // The displayed field's rows, 480 lines interlaced (skip_field 1): left alone, as the\n"
        "  // console leaves them. Filled (2): all but the console's own sample, which native\n"
        "  // VRAM is taken from - so native VRAM is the console's, and the rest of each pixel\n"
        "  // is this frame, making the sharper picture one whole frame.\n"
        "  if (skip_field != 0 && (pixel.y & 1) == active_line_lsb && (skip_field == 1 || exact))\n"
        "    discard;\n"
        "  uint a0 = input.p2.x;\n"
        "  uint kind = (a0 >> 27) & 3u;\n"
        "  bool textured = (a0 & 0x200000u) != 0;\n"
        "  int3 c;\n"
        "  int u = 0, v = 0;\n"
        "  int2 fine_texel = int2(0, 0);\n"
        "  if (kind == 0u) {\n"
        "    int2 A = XY(input.p0.x) * scale, B = XY(input.p0.y) * scale;\n"
        "    int2 C = XY(input.p0.z) * scale;\n"
        "    uint rule = input.p1.z >> 24;\n"
        "    int w0 = (B.x - A.x) * (at.y - A.y) - (B.y - A.y) * (at.x - A.x);\n"
        "    int w1 = (C.x - B.x) * (at.y - B.y) - (C.y - B.y) * (at.x - B.x);\n"
        "    int w2 = (A.x - C.x) * (at.y - C.y) - (A.y - C.y) * (at.x - C.x);\n"
        "    if (w0 - (int)(rule & 1u) < 0 || w1 - (int)((rule >> 1) & 1u) < 0 ||\n"
        "        w2 - (int)((rule >> 2) & 1u) < 0)\n"
        "      discard;\n"
        "    // Barycentric weights: w1 belongs to a, w2 to b, w0 to c.\n"
        "    if (motion_known)\n"
        "      motion = Weigh(float3(w1, w2, w0) / ((float)input.p1.w * (float)(scale * scale)),\n"
        "                     input.p4);\n"
        "    uint3 ca = RGB(input.p0.w), cb = RGB(input.p1.x), cc = RGB(input.p1.y);\n"
        "    uint3 us = uint3(input.p0.w >> 24, input.p1.x >> 24, input.p1.y >> 24);\n"
        "    uint vw = input.p1.z;\n"
        "    uint3 vs = uint3(vw & 255u, (vw >> 8) & 255u, (vw >> 16) & 255u);\n"
        "    bool gouraud = (a0 & 0x20000000u) != 0;\n"
        "    if (exact) {\n"
        "      // The console's own pixel: its weights are these divided by scale squared.\n"
        "      uint square = (uint)(scale * scale), area = input.p1.w;\n"
        "      uint wa = (uint)w1 / square, wb = (uint)w2 / square, wc = (uint)w0 / square;\n"
        "      c = gouraud ? int3(min((wa * ca + wb * cb + wc * cc) / area, 255u)) : int3(ca);\n"
        "      if (textured) {\n"
        "        u = (int)((wa * us.x + wb * us.y + wc * us.z) / area);\n"
        "        v = (int)((wa * vs.x + wb * vs.y + wc * vs.z) / area);\n"
        "      }\n"
        "    } else {\n"
        "      float3 w = float3(w1, w2, w0);\n"
        "      float area = (float)input.p1.w * (float)(scale * scale);\n"
        "      c = gouraud ? int3(min(floor((w.x * float3(ca) + w.y * float3(cb) +\n"
        "                                    w.z * float3(cc)) / area + 0.002), 255.0))\n"
        "                  : int3(ca);\n"
        "      if (textured) {\n"
        "        float2 t = float2(dot(w, float3(us)), dot(w, float3(vs))) / area;\n"
        "        float2 whole = floor(t + 0.002);\n"
        "        u = (int)whole.x;\n"
        "        v = (int)whole.y;\n"
        "        fine_texel = clamp(int2((t - whole) * scale), 0, scale - 1);\n"
        "      }\n"
        "    }\n"
        "  } else if (kind == 3u) {\n"
        "    // Vertices PGXP kept unrounded: the same fill rule, in floating point, at each\n"
        "    // sub-pixel's own position - and with a depth at every vertex, texture\n"
        "    // coordinates interpolated in perspective.\n"
        "    float2 A = asfloat(uint2(input.p0.x, input.p0.y));\n"
        "    float2 B = asfloat(uint2(input.p0.z, input.p1.w));\n"
        "    float2 C = asfloat(uint2(input.p2.z, input.p2.w));\n"
        "    float3 depth = asfloat(input.p3.xyz);\n"
        "    uint rule = input.p3.w;\n"
        "    float2 p = (float2(at) + jitter) / scale;\n"
        "    float e0 = Edge(A, B, p), e1 = Edge(B, C, p), e2 = Edge(C, A, p);\n"
        "    if (e0 < 0.0 || e1 < 0.0 || e2 < 0.0 || (e0 == 0.0 && (rule & 1u) != 0) ||\n"
        "        (e1 == 0.0 && (rule & 2u) != 0) || (e2 == 0.0 && (rule & 4u) != 0))\n"
        "      discard;\n"
        "    float area = e0 + e1 + e2;\n"
        "    if (area <= 0.0)\n"
        "      discard;\n"
        "    float3 l = float3(e1, e2, e0) / area;   // a's weight, b's, c's\n"
        "    // One over the depth runs straight across the screen, so it is interpolated\n"
        "    // with the screen's weights.\n"
        "    if (depth.x > 0.0 && depth.y > 0.0 && depth.z > 0.0)\n"
        "      inverse_depth = dot(l, 1.0 / depth);\n"
        "    // Motion goes across a polygon as a texture does: in perspective, with a depth.\n"
        "    if (motion_known) {\n"
        "      float3 q = inverse_depth > 0.0 ? l / depth : l;\n"
        "      motion = Weigh(q / (q.x + q.y + q.z), input.p4);\n"
        "    }\n"
        "    uint3 ca = RGB(input.p0.w), cb = RGB(input.p1.x), cc = RGB(input.p1.y);\n"
        "    c = (a0 & 0x20000000u) != 0\n"
        "        ? int3(min(floor(l.x * float3(ca) + l.y * float3(cb) + l.z * float3(cc) + 0.002), 255.0))\n"
        "        : int3(ca);\n"
        "    if (textured) {\n"
        "      uint vw = input.p1.z;\n"
        "      float3 us = float3(input.p0.w >> 24, input.p1.x >> 24, input.p1.y >> 24);\n"
        "      float3 vs = float3(vw & 255u, (vw >> 8) & 255u, (vw >> 16) & 255u);\n"
        "      float2 t;\n"
        "      if (depth.x > 0.0 && depth.y > 0.0 && depth.z > 0.0) {\n"
        "        float3 q = l / depth;\n"
        "        t = float2(dot(q, us), dot(q, vs)) / (q.x + q.y + q.z);\n"
        "      } else {\n"
        "        t = float2(dot(l, us), dot(l, vs));\n"
        "      }\n"
        "      float2 whole = floor(t + 0.002);\n"
        "      u = (int)whole.x;\n"
        "      v = (int)whole.y;\n"
        "      fine_texel = clamp(int2((t - whole) * scale), 0, scale - 1);\n"
        "    }\n"
        "  } else if (kind == 1u) {\n"
        "    int2 offset = pixel - XY(input.p0.x);\n"
        "    int base_u = (int)(input.p0.w >> 24), base_v = (int)(input.p1.z & 255u);\n"
        "    bool flip_x = (input.p1.z & 0x100u) != 0, flip_y = (input.p1.z & 0x200u) != 0;\n"
        "    u = flip_x ? base_u - offset.x : base_u + offset.x;\n"
        "    v = flip_y ? base_v - offset.y : base_v + offset.y;\n"
        "    fine_texel = int2(flip_x ? scale - 1 - sub.x : sub.x, flip_y ? scale - 1 - sub.y : sub.y);\n"
        "    c = int3(RGB(input.p0.w));\n"
        "    if (motion_known)\n"
        "      motion = Motion(input.p4.x);\n"
        "  } else {\n"
        "    c = int3(RGB(input.p0.w));\n"
        "    motion_known = false;   // a line's pixel, or a fill\n"
        "  }\n"
        "  if (exact)\n"
        "    fine_texel = int2(0, 0);\n"
        "  if (!fine && (a0 & 0x4000000u) != 0)\n"
        "    c = clamp(c + kDither[(pixel.y & 3) * 4 + (pixel.x & 3)], 0, 255);\n"
        "\n"
        "  bool texel_mask = false;\n"
        "  if (textured) {\n"
        "    u = ((u & ~(tw_mask_x * 8)) | ((tw_offset_x & tw_mask_x) * 8)) & 255;\n"
        "    v = ((v & ~(tw_mask_y * 8)) | ((tw_offset_y & tw_mask_y) * 8)) & 255;\n"
        "    int page_x = (int)(a0 & 1023u);\n"
        "    int page_y = (int)((a0 >> 10) & 511u);\n"
        "    int depth = (int)((a0 >> 19) & 3u);\n"
        "    int clut_x = (int)(input.p2.y & 1023u);\n"
        "    int clut_y = (int)((input.p2.y >> 10) & 511u);\n"
        "    // A palette index cannot be a finer sample of anything, so a paletted texture\n"
        "    // and its CLUT are read at their pixels' own sub-pixels; a 15-bit one - which may\n"
        "    // itself have been drawn at this scale - at the sub-texel under this sub-pixel.\n"
        "    int4 t;\n"
        "    if (depth == 0) {\n"
        "      int block = Fetch(page_x + u / 4, page_y + v);\n"
        "      t = Texel(clut_x + ((block >> ((u & 3) * 4)) & 15), clut_y, 0, 0);\n"
        "    } else if (depth == 1) {\n"
        "      int block = Fetch(page_x + u / 2, page_y + v);\n"
        "      t = Texel(clut_x + ((block >> ((u & 1) * 8)) & 255), clut_y, 0, 0);\n"
        "    } else {\n"
        "      t = Texel(page_x + u, page_y + v, fine_texel.x, fine_texel.y);\n"
        "      direct = true;\n"
        "      direct_at = int2(((page_x + u) & 1023) * scale + fine_texel.x,\n"
        "                       ((page_y + v) & 511) * scale + fine_texel.y);\n"
        "    }\n"
        "    int texel = Pack(t);\n"
        "    if (texel == 0)\n"
        "      discard;\n"
        "    texel_mask = (texel & 0x8000) != 0;\n"
        "    int3 tc = fine ? t.rgb : Widen(texel);\n"
        "    if ((a0 & 0x400000u) == 0)\n"
        "      tc = min((tc * c) >> 7, 255);\n"
        "    c = tc;\n"
        "  }\n"
        "\n"
        "  bool blend = (a0 & 0x800000u) != 0 && (!textured || texel_mask);\n"
        "  if (check_mask != 0 || blend) {\n"
        "    int4 back = Texel(pixel.x, pixel.y, sub.x, sub.y);\n"
        "    if (check_mask != 0 && back.a >= 128)\n"
        "      discard;\n"
        "    if (blend) {\n"
        "      int3 b = fine ? back.rgb : Widen(Pack(back));\n"
        "      uint mode = (a0 >> 24) & 3u;\n"
        "      if (mode == 0u)\n"
        "        c = (b + c) >> 1;\n"
        "      else if (mode == 1u)\n"
        "        c = b + c;\n"
        "      else if (mode == 2u)\n"
        "        c = b - c;\n"
        "      else\n"
        "        c = b + (c >> 2);\n"
        "      c = clamp(c, 0, 255);\n"
        "    }\n"
        "  }\n"
        "  float3 colour = fine ? float3(c) / 255.0 : float3(Cut(c.r), Cut(c.g), Cut(c.b));\n"
        "  float4 result = float4(colour, (force_mask != 0 || texel_mask) ? 1.0 : 0.0);\n"
        "#if PLANES\n"
        "  // A fill is a background standing still; motion otherwise as worked out above, in\n"
        "  // sub-pixels. A 15-bit texel brings its own plane along, as a copy does - a game\n"
        "  // drawing a picture it drew itself back onto the screen, a blur or a wipe - with the\n"
        "  // primitive's own motion and depth first where it has them. Translucent, alpha 0\n"
        "  // keeps what is under it (the target's blend) and says so.\n"
        "  bool fill = kind == 2u && input.p3.w != 0u;\n"
        "  float4 plane = float4(unknown_motion, unknown_motion, depth_scale * inverse_depth, 1.0);\n"
        "  if (fill)\n"
        "    plane.rg = float2(0.0, 0.0);\n"
        "  else if (motion_known)\n"
        "    plane.rg = motion * scale;\n"
        "  if (direct) {\n"
        "    float4 carried = plane_source.Load(int3(direct_at, 0));\n"
        "    if (!motion_known)\n"
        "      plane.rg = carried.rg;\n"
        "    if (inverse_depth == 0.0)\n"
        "      plane.b = carried.b;\n"
        "  }\n"
        "  plane.a = blend ? 0.0 : 1.0;\n"
        "  Out output;\n"
        "  output.colour = result;\n"
        "  output.plane = plane;\n"
        "  return output;\n"
        "#else\n"
        "  return result;\n"
        "#endif\n"
        "}\n"
        "\n"
        "// A VRAM-to-VRAM copy, sub-pixel for sub-pixel - and the plane with it.\n"
        "#if PLANES\n"
        "Out PsCopy(VsOut input) {\n"
        "#else\n"
        "float4 PsCopy(VsOut input) : SV_TARGET {\n"
        "#endif\n"
        "  int2 at = int2(input.position.xy);\n"
        "  int2 pixel = at / scale;\n"
        "  int2 sub = at - pixel * scale;\n"
        "  if (check_mask != 0 && source.Load(int3(at, 0)).a > 0.5)\n"
        "    discard;\n"
        "  int2 from = ((pixel - XY(input.p0.x) + XY(input.p0.y)) & int2(1023, 511)) * scale + sub;\n"
        "  float4 texel = source.Load(int3(from, 0));\n"
        "  if (force_mask != 0)\n"
        "    texel.a = 1.0;\n"
        "#if PLANES\n"
        "  Out output;\n"
        "  output.colour = texel;\n"
        "  output.plane = plane_source.Load(int3(from, 0));\n"
        "  return output;\n"
        "#else\n"
        "  return texel;\n"
        "#endif\n"
        "}\n"
        "\n"
        "// Downloads, above 1x: each console pixel's own sub-pixel, into a native-sized target.\n"
        "float4 PsDownsample(VsOut input) : SV_TARGET {\n"
        "  return source.Load(int3(int2(input.position.xy) * scale, 0));\n"
        "}\n"
        "\n"
        "// Uploads, above 1x: native VRAM's copy on the card, each pixel repeated over its\n"
        "// sub-pixels.\n"
        "float4 PsExpand(VsOut input) : SV_TARGET {\n"
        "  return source.Load(int3(int2(input.position.xy) / scale, 0));\n"
        "}\n"
        "\n"
        "// Where in the target a pixel of the display area is: p0.x the area's corner. p0.y,\n"
        "// when a field-at-a-time picture was filled (skip_field 2), is 1 + the parity of the\n"
        "// rows whose console samples - each pixel's top-left sub-pixel - still hold the last\n"
        "// field; Filled says whether `at` is one of them. Every sub-pixel around one is this\n"
        "// frame's: left and right were filled, above and below drawn or filled.\n"
        "int2 At(VsOut input) { return int2(input.position.xy) + XY(input.p0.x); }\n"
        "bool Filled(VsOut input, int2 at) {\n"
        "  if (input.p0.y == 0u || scale < 2)\n"
        "    return false;\n"
        "  int2 pixel = at / scale;\n"
        "  int2 sub = at - pixel * scale;\n"
        "  return (uint)(pixel.y & 1) == input.p0.y - 1u && sub.x == 0 && sub.y == 0;\n"
        "}\n"
        "// The picture there: a console sample of the last field is taken from this frame's\n"
        "// sub-pixels around it, along an edge rather than across one - the one to its right\n"
        "// when left and right are more alike than above and below, else the one below; both\n"
        "// inside the same pixel, and no colour made up. Edges inside a pixel stay where they\n"
        "// are, and a corner, alike both ways, goes with below. Left and above, on the display\n"
        "// area's edge, are outside it, and are taken as right and below.\n"
        "float4 Picture(VsOut input) {\n"
        "  int2 at = At(input);\n"
        "  if (!Filled(input, at))\n"
        "    return source.Load(int3(at, 0));\n"
        "  int2 corner = XY(input.p0.x);\n"
        "  float4 r = source.Load(int3(at + int2(1, 0), 0));\n"
        "  float4 d = source.Load(int3(at + int2(0, 1), 0));\n"
        "  float4 l = at.x > corner.x ? source.Load(int3(at - int2(1, 0), 0)) : r;\n"
        "  float4 u = at.y > corner.y ? source.Load(int3(at - int2(0, 1), 0)) : d;\n"
        "  float across = dot(abs(l.rgb - r.rgb), 1.0), down = dot(abs(u.rgb - d.rgb), 1.0);\n"
        "  return across < down ? r : d;\n"
        "}\n"
        "// The plane there: one of this frame's sub-pixels, not a mean - motion and depth are not\n"
        "// averaged across an edge - the one below, inside the same pixel.\n"
        "float4 Plane(VsOut input) {\n"
        "  int2 at = At(input);\n"
        "  return source.Load(int3(Filled(input, at) ? at + int2(0, 1) : at, 0));\n"
        "}\n"
        "\n"
        "// Showing, above 1x: the display area out of the target, alpha opaque.\n"
        "float4 PsDisplay(VsOut input) : SV_TARGET {\n"
        "  return float4(Picture(input).rgb, 1.0);\n"
        "}\n"
        "\n"
        "// View > Depth: the plane's depth in place of the picture, nearer brighter, on a scale of\n"
        "// powers of two from about 100 to 100,000 GTE units. Dark blue where there is no depth;\n"
        "// reddened where the last thing drawn was translucent.\n"
        "float4 PsDisplayDepth(VsOut input) : SV_TARGET {\n"
        "  float4 p = Plane(input);\n"
        "  if (!(p.b > 0.0))\n"
        "    return float4(0.05, 0.05, 0.3, 1.0);\n"
        "  float g = saturate((log2(p.b / depth_scale) + 16.5) / 10.0);\n"
        "  float3 c = p.a < 0.5 ? float3(saturate(g + 0.35), g * 0.6, g * 0.6) : float3(g, g, g);\n"
        "  return float4(c, 1.0);\n"
        "}\n"
        "\n"
        "// View > Motion: which way each sub-pixel moved, as the hue, and how far - up to eight\n"
        "// console pixels - as the brightness. Black is still; dim purple, not known.\n"
        "float4 PsDisplayMotion(VsOut input) : SV_TARGET {\n"
        "  float4 p = Plane(input);\n"
        "  if (p.r >= unknown_motion * 0.5 || p.g >= unknown_motion * 0.5)\n"
        "    return float4(0.25, 0.0, 0.3, 1.0);\n"
        "  float2 m = p.rg / scale;\n"
        "  float length_ = length(m);\n"
        "  float hue = (atan2(m.y, m.x) / 6.2831853 + 1.0) * 6.0;\n"
        "  float3 rgb = saturate(abs(fmod(hue + float3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0);\n"
        "  return float4(rgb * saturate(length_ / 8.0), 1.0);\n"
        "}\n";

    // One of the shaders above, for `target` ("ps_4_0", "vs_5_0", ...) at a scale, true colour
    // or not, and writing the plane or not. `name` is what the compiler calls the source.
    inline bool CompileShader(const char* name, const char* entry, const char* target, int scale,
                              bool true_color, bool planes,
                              Microsoft::WRL::ComPtr<ID3DBlob>* blob, std::string* error) {
        const std::string scale_text = std::to_string(scale);
        const std::string unknown_text = std::to_string(emulation::psx::kUnknownMotion);
        const std::string depth_text = std::to_string(emulation::psx::kPlaneDepthScale);
        const D3D_SHADER_MACRO macros[] = {
            { "SCALE", scale_text.c_str() },
            { "TRUE_COLOR", true_color ? "1" : "0" },
            { "PLANES", planes ? "1" : "0" },
            { "UNKNOWN_MOTION", unknown_text.c_str() },
            { "DEPTH_SCALE", depth_text.c_str() },
            { nullptr, nullptr },
        };
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        const HRESULT result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, name, macros,
                                          nullptr, entry, target, 0, 0,
                                          blob->ReleaseAndGetAddressOf(), &errors);
        if (SUCCEEDED(result))
            return true;
        *error = std::string("the hardware rasteriser's shader ") + entry + " did not compile";
        if (errors)
            *error += std::string(": ") + static_cast<const char*>(errors->GetBufferPointer());
        return false;
    }

    inline uint32_t Widen(uint32_t c5) {
        return (c5 << 3) | (c5 >> 2);
    }

    // A pixel as the card keeps it, and back: the 16-bit VRAM value <-> RGBA8, with alpha the
    // mask bit. Exact both ways for every one of the 65,536 values.
    inline uint32_t ToCard(uint16_t pixel) {
        return Widen(pixel & 0x1F) | (Widen((pixel >> 5) & 0x1F) << 8) |
               (Widen((pixel >> 10) & 0x1F) << 16) | ((pixel & 0x8000) ? 0xFF000000u : 0u);
    }

    inline uint16_t FromCard(uint32_t rgba) {
        return static_cast<uint16_t>(((rgba & 0xFF) >> 3) | (((rgba >> 11) & 0x1F) << 5) |
                                     (((rgba >> 19) & 0x1F) << 10) |
                                     ((rgba & 0x80000000u) ? 0x8000 : 0));
    }

    inline uint32_t PackColor(uint8_t r, uint8_t g, uint8_t b) {
        return r | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16);
    }

    inline uint32_t PackXy(int32_t x, int32_t y) {
        return (static_cast<uint32_t>(x) & 0xFFFF) | (static_cast<uint32_t>(y) << 16);
    }

    inline uint8_t Clamp8(int32_t v) {
        return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }

    // A motion for p[16-18]: 64ths of a console pixel, x and y 16 bits apiece.
    inline uint32_t PackMotion(float mx, float my) {
        auto part = [](float value) {
            const float steps = std::min(std::max(std::round(value * 64.0f), -32768.0f), 32767.0f);
            return static_cast<uint32_t>(static_cast<int32_t>(steps)) & 0xFFFFu;
        };
        return part(mx) | (part(my) << 16);
    }

    // Three corners' motion into p[16-19].
    inline void PutMotion(uint32_t* payload, const emulation::psx::RasterVertex& a,
                          const emulation::psx::RasterVertex& b,
                          const emulation::psx::RasterVertex& c) {
        payload[16] = PackMotion(a.mx, a.my);
        payload[17] = PackMotion(b.mx, b.my);
        payload[18] = PackMotion(c.mx, c.my);
        payload[19] = (a.moved ? 1u : 0u) | (b.moved ? 2u : 0u) | (c.moved ? 4u : 0u);
    }

    // Whether the fill rule leaves an edge out - the software rasteriser's EdgeBias: in the
    // winding every triangle is turned to, an edge that is neither top nor left.
    inline bool EdgeLeftOut(int32_t dx, int32_t dy) {
        return !((dy < 0) || (dy == 0 && dx > 0));
    }

    // The attributes word, p[8].
    inline uint32_t Attributes(const emulation::psx::RasterState& state, uint32_t kind,
                               bool textured, bool dither) {
        return (state.texpage_x & 1023) | ((state.texpage_y & 511) << 10) |
               ((state.texpage_colors & 3) << 19) | (textured ? 1u << 21 : 0) |
               (state.raw_texture ? 1u << 22 : 0) | (state.semi_transparent ? 1u << 23 : 0) |
               ((state.semi_mode & 3) << 24) | (dither ? 1u << 26 : 0) | (kind << 27) |
               (state.gouraud ? 1u << 29 : 0);
    }

    // Calls `piece(x, y, w, h, col, row)` for each part of a rectangle that wraps round
    // VRAM's edges, each part lying inside VRAM - up to four - with the part's offset into
    // the rectangle.
    template <typename Piece>
    void ForEachPiece(int32_t x, int32_t y, int32_t w, int32_t h, Piece piece) {
        x &= 1023;
        y &= 511;
        w = std::min(w, 1024);
        h = std::min(h, 512);
        if (w <= 0 || h <= 0)
            return;
        const int32_t first_w = std::min(w, 1024 - x);
        const int32_t first_h = std::min(h, 512 - y);
        piece(x, y, first_w, first_h, 0, 0);
        if (first_w < w)
            piece(0, y, w - first_w, first_h, first_w, 0);
        if (first_h < h) {
            piece(x, 0, first_w, h - first_h, 0, first_h);
            if (first_w < w)
                piece(0, 0, w - first_w, h - first_h, first_w, first_h);
        }
    }

    // Every texture any rasteriser shares gets its own number, so a presenter's cache of what it
    // opened cannot mistake a new one for an old one made at the same handle.
    inline std::atomic<uint64_t> next_texture_id{ 1 };

    // The Halton sequence's `index`th number in `base`: the radical inverse, in [0, 1). Its
    // first few in bases 2 and 3 spread over the square more evenly than any random ones -
    // what DLSS asks of a jitter sequence (Docs/DLSS-Plan.md, phase 3).
    inline float Halton(uint32_t index, uint32_t base) {
        float value = 0.0f, fraction = 1.0f;
        while (index > 0) {
            fraction /= static_cast<float>(base);
            value += fraction * static_cast<float>(index % base);
            index /= base;
        }
        return value;
    }

    // A 16-bit float, as the plane keeps its values, widened.
    inline float HalfToFloat(uint16_t half) {
        const uint32_t sign = static_cast<uint32_t>(half & 0x8000u) << 16;
        const uint32_t exponent = (half >> 10) & 0x1Fu;
        uint32_t mantissa = half & 0x3FFu;
        uint32_t bits;
        if (exponent == 31) {
            bits = sign | 0x7F800000u | (mantissa << 13);
        } else if (exponent != 0) {
            bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
        } else if (mantissa == 0) {
            bits = sign;
        } else {   // subnormal: shifted up until it is a normal number's
            uint32_t shifts = 0;
            while ((mantissa & 0x400u) == 0) {
                mantissa <<= 1;
                ++shifts;
            }
            bits = sign | ((113 - shifts) << 23) | ((mantissa & 0x3FFu) << 13);
        }
        float value;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }

    // The warp check's sums (RasterBackend::set_motion_check): `now` - a new picture, RGBA rows
    // `width` wide - against `last`, the last new picture, moved by the plane's motion (`plane`,
    // RGBA16F rows `plane_pitch` bytes apart) and left still, into `counters`. Nothing when
    // there is no last picture, or `reset` says this one has nothing to do with it.
    //
    // A filled picture's console samples on its `filled_rows` (SharedPicture::filled_rows) still
    // hold the last field, and are shown from beside rather than as they are: they are left out.
    inline void WarpSums(const std::vector<uint8_t>& now, const std::vector<uint8_t>& last,
                         const uint8_t* plane, size_t plane_pitch, uint32_t width,
                         uint32_t height, bool reset, emulation::psx::RasterCounters* counters,
                         int filled_rows = 0, int scale = 1) {
        if (last.empty() || reset)
            return;
        auto left_out = [filled_rows, scale](uint32_t col, uint32_t row) {
            return filled_rows != 0 && scale > 1 && col % scale == 0 && row % scale == 0 &&
                   static_cast<int>((row / scale) & 1) == filled_rows - 1;
        };
        // The last new picture at (fx, fy) - the nearest pixel, held to its edges. Not blended
        // between pixels: that blurs sharp edges, and on motion under a pixel, as a waving flag's,
        // the blur costs more than the motion saves - the check would count right motion wrong.
        auto sample = [&](float fx, float fy, float* rgb) {
            const float x = std::min(std::max(std::floor(fx + 0.5f), 0.0f), static_cast<float>(width - 1));
            const float y = std::min(std::max(std::floor(fy + 0.5f), 0.0f), static_cast<float>(height - 1));
            const uint8_t* p = &last[(static_cast<size_t>(y) * width + static_cast<size_t>(x)) * 4];
            for (int c = 0; c < 3; ++c)
                rgb[c] = p[c];
        };
        // Jittered, the two pictures were sampled up to a sub-pixel apart. That is not taken out by
        // looking a jitter's difference further on: at the nearest pixel, a difference of half a
        // sub-pixel or more moves every pixel a whole one - 2D that was never jittered included -
        // and Valkyrie Profile, all sprites, went from 0.80 to 5.29 moved and 2.67 to 6.36 still.
        // So moved and still are both left with the jitter's own difference along triangles'
        // edges, the same in each.
        double moved_error = 0.0, still_error = 0.0;
        uint64_t moved_pixels = 0;
        const float unknown = emulation::psx::kUnknownMotion * 0.5f;
        for (uint32_t row = 0; row < height; ++row) {
            const uint16_t* motion = reinterpret_cast<const uint16_t*>(plane + row * plane_pitch);
            for (uint32_t col = 0; col < width; ++col) {
                if (left_out(col, row))
                    continue;
                const uint8_t* here = &now[(static_cast<size_t>(row) * width + col) * 4];
                float there[3];
                sample(static_cast<float>(col), static_cast<float>(row), there);
                float still = 0.0f;
                for (int c = 0; c < 3; ++c)
                    still += std::abs(static_cast<float>(here[c]) - there[c]);
                const float mx = HalfToFloat(motion[col * 4]);
                const float my = HalfToFloat(motion[col * 4 + 1]);
                float moved = still;
                if (mx < unknown && my < unknown) {
                    ++moved_pixels;
                    float rgb[3];
                    sample(static_cast<float>(col) + mx, static_cast<float>(row) + my, rgb);
                    moved = 0.0f;
                    for (int c = 0; c < 3; ++c)
                        moved += std::abs(static_cast<float>(here[c]) - rgb[c]);
                }
                moved_error += moved;
                still_error += still;
            }
        }
        ++counters->warp_pictures;
        counters->warp_pixels += static_cast<uint64_t>(width) * height;
        counters->warp_moved_pixels += moved_pixels;
        counters->warp_error_moved += moved_error;
        counters->warp_error_still += still_error;
    }

}   // namespace raster
}   // namespace psxemu

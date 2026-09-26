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

// What the on-screen overlay hands a graphics engine to draw over the picture: triangles, in
// window pixels, textured from one atlas, alpha-blended. Every engine draws it the same way -
// see IGraphicsEngine::SetOverlay.
//
// The vertex is laid out exactly as Dear ImGui's ImDrawVert (position, texture coordinate,
// colour packed R,G,B,A from the low byte), so ImGui's core could one day draw through the
// same four passes without a backend of its own.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace psxemu {

    struct OverlayVertex {
        float x, y;       // window pixels, from the top left
        float u, v;       // into the atlas, 0..1
        uint32_t color;   // R | G << 8 | B << 16 | A << 24; multiplies the atlas texel
    };
    static_assert(sizeof(OverlayVertex) == 20, "the engines' vertex layouts assume 20 bytes");

    // Packs a colour the way OverlayVertex wants it.
    constexpr uint32_t OverlayColor(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
        return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
               (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
    }

    // `color` with its alpha scaled by `opacity` (0..1) - how things fade.
    inline uint32_t OverlayFade(uint32_t color, float opacity) {
        if (opacity >= 1.0f)
            return color;
        if (opacity <= 0.0f)
            return color & 0x00FFFFFFu;
        const uint32_t alpha = static_cast<uint32_t>(static_cast<float>(color >> 24) * opacity + 0.5f);
        return (color & 0x00FFFFFFu) | (alpha << 24);
    }

    // One frame's overlay. The engine uploads the atlas again only when `atlas_version`
    // changes, and draws nothing when there are no indices.
    struct OverlayDrawData {
        const OverlayVertex* vertices = nullptr;
        size_t vertex_count = 0;
        const uint32_t* indices = nullptr;
        size_t index_count = 0;

        const uint8_t* atlas = nullptr;   // RGBA8, atlas_width * atlas_height * 4 bytes
        int atlas_width = 0;
        int atlas_height = 0;
        uint64_t atlas_version = 0;       // never 0 once there is an atlas

        bool empty() const { return index_count == 0 || atlas == nullptr; }
    };

    // Copies the vertices into an engine's buffer with their positions in clip space for a
    // `width` x `height` target - which is all the overlay's vertex shader then has to pass on.
    // `y_down` is Vulkan's clip space, where +1 is the bottom; every other API has +1 at the top.
    inline void WriteOverlayVertices(const OverlayDrawData& data, int width, int height,
                                     bool y_down, OverlayVertex* out) {
        const float sx = 2.0f / static_cast<float>(width > 0 ? width : 1);
        const float sy = 2.0f / static_cast<float>(height > 0 ? height : 1);
        for (size_t i = 0; i < data.vertex_count; ++i) {
            OverlayVertex v = data.vertices[i];
            v.x = v.x * sx - 1.0f;
            v.y = y_down ? v.y * sy - 1.0f : 1.0f - v.y * sy;
            out[i] = v;
        }
    }

}   // namespace psxemu

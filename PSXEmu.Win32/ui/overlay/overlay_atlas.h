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

// The overlay's one texture: its fonts, its icons, and a white pixel to draw solid shapes with.
//
// Built on the CPU when the overlay first draws and again whenever its scale changes - the
// fonts are rasterised by GDI at the size they are drawn, rather than drawn at one size and
// stretched, so text stays sharp at any window size. The icons are flat white shapes drawn
// with GDI+ and tinted by the vertex colour.

#include <cstdint>
#include <string>
#include <vector>

namespace psxemu {

    enum class OverlayIcon {
        kPadDigital,    // the original controller: a d-pad and four buttons
        kPadAnalog,     // the same with two sticks - Dual Analog and DualShock
        kMouse,
        kGunCon,
        kMultitap,
        kKeyboard,
        kNone,          // an empty port
        kDisc,
        kCard,
        kSave,
        kLoad,
        kInfo,
        kWarning,
        kSpeed,
        kScreen,
        kCount
    };

    enum class OverlayFont { kSmall, kRegular, kBold, kLarge, kCount };

    class OverlayAtlas {
     public:
        struct Glyph {
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
            float width = 0, height = 0;   // of the cell, pixels
            float offset_x = 0;             // where the cell starts, from the pen
            float advance = 0;
        };

        struct Font {
            float line_height = 0;
            float ascent = 0;
            std::vector<Glyph> glyphs;       // indexed by code point, for kFirst..kLast
            Glyph fallback;                  // anything else: '?'
            const Glyph& Get(wchar_t c) const;
        };

        struct Icon {
            float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        };

        // Rebuilds everything at `scale` (1 is sized for a 720-line window). Returns false if
        // GDI could not; the overlay then draws nothing.
        bool Build(float scale);

        float scale() const { return scale_; }
        uint64_t version() const { return version_; }
        const std::vector<uint8_t>& pixels() const { return pixels_; }
        int width() const { return width_; }
        int height() const { return height_; }

        const Font& font(OverlayFont which) const { return fonts_[static_cast<int>(which)]; }
        const Icon& icon(OverlayIcon which) const { return icons_[static_cast<int>(which)]; }
        // The centre of the white pixel, for solid shapes.
        float white_u() const { return white_u_; }
        float white_v() const { return white_v_; }

        // Width `text` takes in `which`.
        float Measure(OverlayFont which, const std::wstring& text) const;

        static const wchar_t kFirst = 32;
        static const wchar_t kLast = 255;

     private:
        float scale_ = 0.0f;
        uint64_t version_ = 0;
        std::vector<uint8_t> pixels_;
        int width_ = 0;
        int height_ = 0;
        Font fonts_[static_cast<int>(OverlayFont::kCount)];
        Icon icons_[static_cast<int>(OverlayIcon::kCount)];
        float white_u_ = 0.0f;
        float white_v_ = 0.0f;
    };

}   // namespace psxemu

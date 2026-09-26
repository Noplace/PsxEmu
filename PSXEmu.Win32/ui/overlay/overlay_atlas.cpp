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
#include "ui/overlay/overlay_atlas.h"

#include "app/framework.h"

#include <cmath>
#include <cstring>

// GDI+ for the icons: anti-aliased shapes, which plain GDI cannot draw. The same arrangement as
// the bindings window: its headers use min and max unqualified, which NOMINMAX takes away, and
// need objidl.h, which WIN32_LEAN_AND_MEAN leaves out.
#include <algorithm>
#include <objidl.h>
namespace Gdiplus {
    using std::max;
    using std::min;
}   // namespace Gdiplus
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace psxemu {

    namespace {

        const int kAtlasWidth = 1024;
        const int kPadding = 2;   // between entries, so linear sampling never reaches a neighbour

        // A shelf packer: rows left to right, a new row below when one fills.
        struct Packer {
            int x = kPadding, y = kPadding, row_height = 0;
            bool Place(int width, int height, int* out_x, int* out_y) {
                if (width + 2 * kPadding > kAtlasWidth)
                    return false;
                if (x + width + kPadding > kAtlasWidth) {
                    x = kPadding;
                    y += row_height + kPadding;
                    row_height = 0;
                }
                *out_x = x;
                *out_y = y;
                x += width + kPadding;
                row_height = (std::max)(row_height, height);
                return true;
            }
            int used_height() const { return y + row_height + kPadding; }
        };

        // A coverage bitmap waiting for its place in the atlas.
        struct Pending {
            int width = 0, height = 0;
            std::vector<uint8_t> alpha;   // width * height
            int x = 0, y = 0;             // where the packer put it
        };

        // ---- icons: flat white shapes, holes cut with a transparent brush ---------------------

        using Gdiplus::Graphics;
        using Gdiplus::GraphicsPath;
        using Gdiplus::RectF;
        using Gdiplus::PointF;

        struct Canvas {
            Graphics& g;
            float s;   // the icon's size; shapes are given in 0..1
            Gdiplus::SolidBrush white{ Gdiplus::Color(255, 255, 255, 255) };
            Gdiplus::SolidBrush clear{ Gdiplus::Color(0, 0, 0, 0) };

            RectF R(float x0, float y0, float x1, float y1) const {
                return RectF(x0 * s, y0 * s, (x1 - x0) * s, (y1 - y0) * s);
            }
            static void AddRound(GraphicsPath& path, const RectF& r, float radius) {
                const float d = (std::min)(radius * 2.0f, (std::min)(r.Width, r.Height));
                path.AddArc(r.X, r.Y, d, d, 180, 90);
                path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
                path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
                path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
                path.CloseFigure();
            }
            void Round(float x0, float y0, float x1, float y1, float radius, bool cut = false) {
                GraphicsPath path;
                AddRound(path, R(x0, y0, x1, y1), radius * s);
                Fill(path, cut);
            }
            void Ellipse(float x0, float y0, float x1, float y1, bool cut = false) {
                SetMode(cut);
                g.FillEllipse(cut ? &clear : &white, R(x0, y0, x1, y1));
            }
            void Circle(float cx, float cy, float r, bool cut = false) {
                Ellipse(cx - r, cy - r, cx + r, cy + r, cut);
            }
            void Rect(float x0, float y0, float x1, float y1, bool cut = false) {
                SetMode(cut);
                g.FillRectangle(cut ? &clear : &white, R(x0, y0, x1, y1));
            }
            void Polygon(std::initializer_list<PointF> points, bool cut = false) {
                std::vector<PointF> scaled;
                for (const PointF& p : points)
                    scaled.push_back(PointF(p.X * s, p.Y * s));
                SetMode(cut);
                g.FillPolygon(cut ? &clear : &white, scaled.data(), static_cast<INT>(scaled.size()));
            }
            void Fill(GraphicsPath& path, bool cut) {
                SetMode(cut);
                g.FillPath(cut ? &clear : &white, &path);
            }
            void SetMode(bool cut) {
                g.SetCompositingMode(cut ? Gdiplus::CompositingModeSourceCopy
                                         : Gdiplus::CompositingModeSourceOver);
            }
        };

        // The pad's outline: a body and two grips, the shape of the original controller.
        void PadBody(Canvas& c, bool outline) {
            GraphicsPath path;
            Canvas::AddRound(path, c.R(0.10f, 0.26f, 0.90f, 0.60f), 0.15f * c.s);
            path.AddEllipse(c.R(0.10f, 0.38f, 0.40f, 0.86f));
            path.AddEllipse(c.R(0.60f, 0.38f, 0.90f, 0.86f));
            if (!outline) {
                c.Fill(path, false);
                return;
            }
            // An empty port: the same shape as a ring.
            c.Fill(path, false);
            GraphicsPath inner;
            Canvas::AddRound(inner, c.R(0.16f, 0.32f, 0.84f, 0.54f), 0.10f * c.s);
            inner.AddEllipse(c.R(0.16f, 0.44f, 0.34f, 0.80f));
            inner.AddEllipse(c.R(0.66f, 0.44f, 0.84f, 0.80f));
            c.Fill(inner, true);
        }

        void PadControls(Canvas& c, bool sticks) {
            // The d-pad, a cross cut out on the left.
            c.Rect(0.215f, 0.385f, 0.365f, 0.435f, true);
            c.Rect(0.265f, 0.335f, 0.315f, 0.485f, true);
            // Four buttons in a diamond on the right.
            const float bx = 0.71f, by = 0.41f, d = 0.07f, r = 0.032f;
            c.Circle(bx, by - d, r, true);
            c.Circle(bx, by + d, r, true);
            c.Circle(bx - d, by, r, true);
            c.Circle(bx + d, by, r, true);
            if (sticks) {
                c.Circle(0.39f, 0.58f, 0.075f, true);
                c.Circle(0.61f, 0.58f, 0.075f, true);
                c.Circle(0.39f, 0.58f, 0.04f);
                c.Circle(0.61f, 0.58f, 0.04f);
            }
        }

        void DrawIcon(Canvas& c, OverlayIcon icon) {
            switch (icon) {
                case OverlayIcon::kPadDigital:
                    PadBody(c, false);
                    PadControls(c, false);
                    break;
                case OverlayIcon::kPadAnalog:
                    PadBody(c, false);
                    PadControls(c, true);
                    break;
                case OverlayIcon::kNone:
                    PadBody(c, true);
                    break;
                case OverlayIcon::kMouse:
                    c.Round(0.28f, 0.10f, 0.72f, 0.90f, 0.22f);
                    c.Rect(0.485f, 0.10f, 0.515f, 0.40f, true);
                    c.Rect(0.28f, 0.385f, 0.72f, 0.415f, true);
                    c.Round(0.465f, 0.19f, 0.535f, 0.33f, 0.035f);
                    break;
                case OverlayIcon::kGunCon:
                    c.Round(0.08f, 0.28f, 0.90f, 0.46f, 0.05f);
                    c.Polygon({ PointF(0.50f, 0.40f), PointF(0.72f, 0.40f), PointF(0.66f, 0.88f),
                                PointF(0.44f, 0.88f) });
                    c.Round(0.36f, 0.44f, 0.52f, 0.62f, 0.06f);
                    c.Round(0.40f, 0.46f, 0.48f, 0.58f, 0.03f, true);
                    c.Rect(0.82f, 0.22f, 0.88f, 0.30f);
                    break;
                case OverlayIcon::kMultitap:
                    c.Rect(0.47f, 0.08f, 0.53f, 0.32f);
                    c.Round(0.10f, 0.30f, 0.90f, 0.76f, 0.08f);
                    for (int i = 0; i < 4; ++i) {
                        const float x = 0.17f + i * 0.175f;
                        c.Round(x, 0.44f, x + 0.12f, 0.64f, 0.03f, true);
                    }
                    break;
                case OverlayIcon::kKeyboard:
                    c.Round(0.06f, 0.28f, 0.94f, 0.74f, 0.07f);
                    for (int row = 0; row < 3; ++row) {
                        for (int col = 0; col < 8; ++col) {
                            const float x = 0.12f + col * 0.1f;
                            const float y = 0.34f + row * 0.1f;
                            c.Rect(x, y, x + 0.065f, y + 0.065f, true);
                        }
                    }
                    c.Rect(0.26f, 0.63f, 0.74f, 0.68f, true);
                    break;
                case OverlayIcon::kDisc:
                    c.Circle(0.5f, 0.5f, 0.42f);
                    c.Circle(0.5f, 0.5f, 0.09f, true);
                    {
                        Gdiplus::Pen ring(Gdiplus::Color(0, 0, 0, 0), 0.03f * c.s);
                        c.SetMode(true);
                        c.g.DrawEllipse(&ring, c.R(0.28f, 0.28f, 0.72f, 0.72f));
                    }
                    break;
                case OverlayIcon::kCard:
                    c.Polygon({ PointF(0.24f, 0.10f), PointF(0.64f, 0.10f), PointF(0.76f, 0.22f),
                                PointF(0.76f, 0.90f), PointF(0.24f, 0.90f) });
                    for (int i = 0; i < 4; ++i) {
                        const float x = 0.30f + i * 0.11f;
                        c.Rect(x, 0.70f, x + 0.06f, 0.84f, true);
                    }
                    c.Round(0.32f, 0.22f, 0.62f, 0.52f, 0.04f, true);
                    break;
                case OverlayIcon::kSave:
                case OverlayIcon::kLoad: {
                    // A tray, and an arrow into it or out of it.
                    c.Rect(0.14f, 0.60f, 0.22f, 0.88f);
                    c.Rect(0.78f, 0.60f, 0.86f, 0.88f);
                    c.Rect(0.14f, 0.80f, 0.86f, 0.88f);
                    const bool down = icon == OverlayIcon::kSave;
                    c.Rect(0.44f, down ? 0.10f : 0.34f, 0.56f, down ? 0.46f : 0.70f);
                    if (down)
                        c.Polygon({ PointF(0.28f, 0.42f), PointF(0.72f, 0.42f), PointF(0.50f, 0.68f) });
                    else
                        c.Polygon({ PointF(0.28f, 0.38f), PointF(0.72f, 0.38f), PointF(0.50f, 0.12f) });
                    break;
                }
                case OverlayIcon::kInfo:
                    c.Circle(0.5f, 0.5f, 0.42f);
                    c.Circle(0.5f, 0.30f, 0.055f, true);
                    c.Round(0.455f, 0.41f, 0.545f, 0.74f, 0.03f, true);
                    break;
                case OverlayIcon::kWarning:
                    c.Polygon({ PointF(0.50f, 0.08f), PointF(0.94f, 0.88f), PointF(0.06f, 0.88f) });
                    c.Round(0.455f, 0.34f, 0.545f, 0.64f, 0.03f, true);
                    c.Circle(0.5f, 0.75f, 0.05f, true);
                    break;
                case OverlayIcon::kSpeed: {
                    Gdiplus::Pen arc(Gdiplus::Color(255, 255, 255, 255), 0.10f * c.s);
                    c.SetMode(false);
                    c.g.DrawArc(&arc, c.R(0.14f, 0.20f, 0.86f, 0.92f), 180, 180);
                    c.Polygon({ PointF(0.46f, 0.60f), PointF(0.54f, 0.60f), PointF(0.78f, 0.30f) });
                    c.Circle(0.5f, 0.58f, 0.07f);
                    break;
                }
                case OverlayIcon::kScreen:
                    c.Round(0.08f, 0.16f, 0.92f, 0.72f, 0.05f);
                    c.Rect(0.14f, 0.22f, 0.86f, 0.66f, true);
                    c.Rect(0.44f, 0.72f, 0.56f, 0.82f);
                    c.Round(0.30f, 0.82f, 0.70f, 0.88f, 0.02f);
                    break;
                case OverlayIcon::kCount:
                    break;
            }
        }

        bool RenderIcon(OverlayIcon icon, int size, Pending* out) {
            Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
            {
                Graphics g(&bitmap);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
                g.Clear(Gdiplus::Color(0, 0, 0, 0));
                Canvas canvas{ g, static_cast<float>(size) };
                DrawIcon(canvas, icon);
            }
            Gdiplus::BitmapData data;
            Gdiplus::Rect whole(0, 0, size, size);
            if (bitmap.LockBits(&whole, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) !=
                Gdiplus::Ok)
                return false;
            out->width = size;
            out->height = size;
            out->alpha.resize(static_cast<size_t>(size) * size);
            for (int y = 0; y < size; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(data.Scan0) + y * data.Stride;
                for (int x = 0; x < size; ++x)
                    out->alpha[static_cast<size_t>(y) * size + x] = row[x * 4 + 3];
            }
            bitmap.UnlockBits(&data);
            return true;
        }

        // ---- fonts: GDI, grey anti-aliasing, white on black and the coverage kept -------------

        struct FontSpec {
            float pixels;   // em height at scale 1
            int weight;
        };
        const FontSpec kFontSpecs[] = {
            { 13.0f, FW_NORMAL },     // kSmall
            { 15.0f, FW_NORMAL },     // kRegular
            { 15.0f, FW_SEMIBOLD },   // kBold
            { 28.0f, FW_BOLD },       // kLarge
        };

    }   // namespace

    const OverlayAtlas::Glyph& OverlayAtlas::Font::Get(wchar_t c) const {
        if (c >= kFirst && c <= kLast) {
            const Glyph& glyph = glyphs[static_cast<size_t>(c - kFirst)];
            if (glyph.advance > 0.0f)
                return glyph;
        }
        return fallback;
    }

    float OverlayAtlas::Measure(OverlayFont which, const std::wstring& text) const {
        const Font& f = font(which);
        float width = 0.0f;
        for (wchar_t c : text)
            width += f.Get(c).advance;
        return width;
    }

    bool OverlayAtlas::Build(float scale) {
        ULONG_PTR gdiplus_token = 0;
        Gdiplus::GdiplusStartupInput gdiplus_input;
        if (Gdiplus::GdiplusStartup(&gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok)
            return false;

        HDC dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) {
            Gdiplus::GdiplusShutdown(gdiplus_token);
            return false;
        }

        Packer packer;
        std::vector<Pending> pending;
        // The white block every solid shape samples - 4x4, so a linear sample of its centre is
        // white whatever the neighbours.
        {
            Pending white;
            white.width = white.height = 4;
            white.alpha.assign(16, 255);
            pending.push_back(white);
        }

        // Per font: every glyph, rendered into a cell and kept whole. Which cell each glyph got,
        // by index into `pending`, or -1 for none.
        std::vector<int> cell_of[static_cast<int>(OverlayFont::kCount)];
        for (int f = 0; f < static_cast<int>(OverlayFont::kCount); ++f) {
            const int pixels = static_cast<int>(std::lround(kFontSpecs[f].pixels * scale));
            HFONT gdi_font = CreateFontW(-pixels, 0, 0, 0, kFontSpecs[f].weight, FALSE, FALSE, FALSE,
                                         DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                         ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HGDIOBJ old_font = SelectObject(dc, gdi_font);
            TEXTMETRICW metrics = {};
            GetTextMetricsW(dc, &metrics);
            Font& font = fonts_[f];
            font.line_height = static_cast<float>(metrics.tmHeight);
            font.ascent = static_cast<float>(metrics.tmAscent);
            font.glyphs.assign(kLast - kFirst + 1, Glyph());

            cell_of[f].assign(kLast - kFirst + 1, -1);
            for (wchar_t c = kFirst; c <= kLast; ++c) {
                // 7Fh-9Fh are control codes in Latin-1, not characters.
                if (c >= 0x7F && c <= 0x9F)
                    continue;
                ABCFLOAT abc = {};
                if (!GetCharABCWidthsFloatW(dc, c, c, &abc))
                    continue;
                Glyph& glyph = font.glyphs[static_cast<size_t>(c - kFirst)];
                glyph.advance = abc.abcfA + abc.abcfB + abc.abcfC;
                // The cell spans the ink with a pixel to spare each side; the pen starts at
                // `offset_x` from it.
                const int left = static_cast<int>(std::floor(abc.abcfA)) - 1;
                const int width = static_cast<int>(std::ceil(abc.abcfA + abc.abcfB)) - left + 1;
                const int height = metrics.tmHeight;
                glyph.offset_x = static_cast<float>(left);
                glyph.width = static_cast<float>(width);
                glyph.height = static_cast<float>(height);
                Pending cell;
                cell.width = (std::max)(width, 1);
                cell.height = (std::max)(height, 1);
                if (c != L' ') {
                    BITMAPINFO info = {};
                    info.bmiHeader.biSize = sizeof(info.bmiHeader);
                    info.bmiHeader.biWidth = cell.width;
                    info.bmiHeader.biHeight = -cell.height;   // top-down
                    info.bmiHeader.biPlanes = 1;
                    info.bmiHeader.biBitCount = 32;
                    info.bmiHeader.biCompression = BI_RGB;
                    void* bits = nullptr;
                    HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
                    if (dib == nullptr || bits == nullptr) {
                        if (dib != nullptr)
                            DeleteObject(dib);
                        glyph = Glyph();   // left out, and drawn as the fallback
                        continue;
                    }
                    HGDIOBJ old_bitmap = SelectObject(dc, dib);
                    memset(bits, 0, static_cast<size_t>(cell.width) * cell.height * 4);
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, RGB(255, 255, 255));
                    TextOutW(dc, -left, 0, &c, 1);
                    GdiFlush();
                    cell.alpha.resize(static_cast<size_t>(cell.width) * cell.height);
                    const uint8_t* source = static_cast<const uint8_t*>(bits);
                    for (size_t i = 0; i < cell.alpha.size(); ++i)
                        cell.alpha[i] = source[i * 4 + 1];   // green: grey coverage
                    SelectObject(dc, old_bitmap);
                    DeleteObject(dib);
                } else {
                    cell.alpha.assign(static_cast<size_t>(cell.width) * cell.height, 0);
                }
                cell_of[f][static_cast<size_t>(c - kFirst)] = static_cast<int>(pending.size());
                pending.push_back(std::move(cell));
            }
            SelectObject(dc, old_font);
            DeleteObject(gdi_font);
        }
        DeleteDC(dc);

        // Icons, all one size.
        const int icon_size = static_cast<int>(std::lround(64.0f * scale));
        const size_t first_icon = pending.size();
        bool icons_ok = true;
        for (int i = 0; i < static_cast<int>(OverlayIcon::kCount); ++i) {
            Pending icon;
            if (!RenderIcon(static_cast<OverlayIcon>(i), icon_size, &icon)) {
                icons_ok = false;
                icon.width = icon.height = 1;
                icon.alpha.assign(1, 0);
            }
            pending.push_back(std::move(icon));
        }
        Gdiplus::GdiplusShutdown(gdiplus_token);
        if (!icons_ok)
            return false;

        for (Pending& entry : pending) {
            if (!packer.Place(entry.width, entry.height, &entry.x, &entry.y))
                return false;
        }
        int height = 64;
        while (height < packer.used_height())
            height *= 2;

        width_ = kAtlasWidth;
        height_ = height;
        pixels_.assign(static_cast<size_t>(width_) * height_ * 4, 0);
        for (const Pending& entry : pending) {
            for (int y = 0; y < entry.height; ++y) {
                uint8_t* row = &pixels_[(static_cast<size_t>(entry.y + y) * width_ + entry.x) * 4];
                for (int x = 0; x < entry.width; ++x) {
                    row[x * 4 + 0] = 255;
                    row[x * 4 + 1] = 255;
                    row[x * 4 + 2] = 255;
                    row[x * 4 + 3] = entry.alpha[static_cast<size_t>(y) * entry.width + x];
                }
            }
        }

        const float inv_w = 1.0f / static_cast<float>(width_);
        const float inv_h = 1.0f / static_cast<float>(height_);
        white_u_ = (static_cast<float>(pending[0].x) + 2.0f) * inv_w;
        white_v_ = (static_cast<float>(pending[0].y) + 2.0f) * inv_h;

        for (int f = 0; f < static_cast<int>(OverlayFont::kCount); ++f) {
            Font& font = fonts_[f];
            for (wchar_t c = kFirst; c <= kLast; ++c) {
                Glyph& glyph = font.glyphs[static_cast<size_t>(c - kFirst)];
                const int index = cell_of[f][static_cast<size_t>(c - kFirst)];
                if (index < 0) {
                    glyph = Glyph();
                    continue;
                }
                const Pending& cell = pending[static_cast<size_t>(index)];
                glyph.u0 = static_cast<float>(cell.x) * inv_w;
                glyph.v0 = static_cast<float>(cell.y) * inv_h;
                glyph.u1 = static_cast<float>(cell.x + cell.width) * inv_w;
                glyph.v1 = static_cast<float>(cell.y + cell.height) * inv_h;
            }
            font.fallback = font.Get(L'?');
        }
        for (int i = 0; i < static_cast<int>(OverlayIcon::kCount); ++i) {
            const Pending& icon = pending[first_icon + static_cast<size_t>(i)];
            icons_[i].u0 = static_cast<float>(icon.x) * inv_w;
            icons_[i].v0 = static_cast<float>(icon.y) * inv_h;
            icons_[i].u1 = static_cast<float>(icon.x + icon.width) * inv_w;
            icons_[i].v1 = static_cast<float>(icon.y + icon.height) * inv_h;
        }

        scale_ = scale;
        ++version_;
        return true;
    }

}   // namespace psxemu

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
#include "app/screenshot.h"

// GDI+ for the PNG encoder and the stretch. Its headers use min and max unqualified, which
// NOMINMAX (psx.h) takes away, and need objidl.h, which WIN32_LEAN_AND_MEAN leaves out.
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

        bool PngEncoder(CLSID* clsid) {
            UINT count = 0;
            UINT size = 0;
            if (Gdiplus::GetImageEncodersSize(&count, &size) != Gdiplus::Ok || size == 0)
                return false;
            std::vector<uint8_t> buffer(size);
            auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
            if (Gdiplus::GetImageEncoders(count, size, encoders) != Gdiplus::Ok)
                return false;
            for (UINT i = 0; i < count; ++i) {
                if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
                    *clsid = encoders[i].Clsid;
                    return true;
                }
            }
            return false;
        }

    }   // namespace

    int ScreenshotWidth(int width, int height) {
        const int wide = (height * 4 + 1) / 3;
        return wide > 0 ? wide : width;
    }

    bool SaveScreenshotPng(const std::wstring& path, const std::vector<uint32_t>& pixels,
                           int width, int height, std::wstring* error) {
        if (width <= 0 || height <= 0 ||
            pixels.size() < static_cast<size_t>(width) * static_cast<size_t>(height)) {
            *error = L"There is no picture to save yet.";
            return false;
        }
        // Its own start and stop: nothing here can rely on a window having started GDI+ first.
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            *error = L"GDI+ could not be started.";
            return false;
        }
        bool saved = false;
        {
            CLSID png;
            // 32bppRGB rather than ARGB: the frame's alpha byte means nothing, and a PNG that
            // kept it could come out transparent.
            Gdiplus::Bitmap frame(width, height, width * 4, PixelFormat32bppRGB,
                                  reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pixels.data())));
            const int out_width = ScreenshotWidth(width, height);
            Gdiplus::Bitmap shaped(out_width, height, PixelFormat24bppRGB);
            if (!PngEncoder(&png)) {
                *error = L"Windows has no PNG encoder.";
            } else if (frame.GetLastStatus() != Gdiplus::Ok ||
                       shaped.GetLastStatus() != Gdiplus::Ok) {
                *error = L"Not enough memory for the picture.";
            } else {
                {
                    Gdiplus::Graphics graphics(&shaped);
                    // Only ever stretched sideways, and by less than 1.5x, where bicubic keeps
                    // the edges without the uneven columns nearest-neighbour would give.
                    graphics.SetInterpolationMode(out_width == width
                                                      ? Gdiplus::InterpolationModeNearestNeighbor
                                                      : Gdiplus::InterpolationModeHighQualityBicubic);
                    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
                    Gdiplus::ImageAttributes clamp;
                    clamp.SetWrapMode(Gdiplus::WrapModeTileFlipXY);   // no dark fringe at the edges
                    graphics.DrawImage(&frame, Gdiplus::Rect(0, 0, out_width, height), 0, 0,
                                       width, height, Gdiplus::UnitPixel, &clamp);
                }
                saved = shaped.Save(path.c_str(), &png, nullptr) == Gdiplus::Ok;
                if (!saved)
                    *error = L"The file could not be written.";
            }
        }
        Gdiplus::GdiplusShutdown(token);
        return saved;
    }

}   // namespace psxemu

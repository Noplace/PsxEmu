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

// A frame as a PNG, for F12.
//
// The picture is saved as the machine made it - no filter, no overlay - but at the shape it is
// shown: a PlayStation's display is 4:3 whatever its width in pixels, so a 256- or 368-wide
// frame is stretched to 4:3 at its own height rather than saved squashed. 320x240 stays
// 320x240, and 640x480 stays 640x480.

#include "app/framework.h"

#include <cstdint>
#include <string>
#include <vector>

namespace psxemu {

    // `pixels` are 0xAARRGGBB, `width` x `height`, as Gpu::framebuffer makes them; alpha is
    // ignored. False, with `error` saying why, if the file could not be written.
    bool SaveScreenshotPng(const std::wstring& path, const std::vector<uint32_t>& pixels,
                           int width, int height, std::wstring* error);

    // The width a `width` x `height` frame is saved at: 4:3 at its own height.
    int ScreenshotWidth(int width, int height);

}   // namespace psxemu

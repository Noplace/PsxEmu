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

// A picture as a PNG file, with the core's own zlib: the texture tracker's dumps
// (render/texture_tracker.h). The core knows no operating system, so this is the whole format
// written by hand - signature, IHDR, one IDAT, IEND - rather than a call into Windows.

#include <cstdint>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

  // `rgba` is `width` x `height` pixels, four bytes each, rows top to bottom. False if the file
  // cannot be written.
  bool WritePngFile(const std::string& path, int width, int height, const uint8_t* rgba);

  // The same as bytes, for the tests.
  std::vector<uint8_t> EncodePng(int width, int height, const uint8_t* rgba);

}  // namespace psx
}  // namespace emulation

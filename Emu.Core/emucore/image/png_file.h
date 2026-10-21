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

// A picture as a PNG file, with Emu.Core's own zlib: in PSXEmu, the texture tracker's dumps
// (render/texture_tracker.h) and the replacements read back (render/texture_pack.h). The core
// knows no operating system, so this is the format by hand rather than a call into Windows:
// written as signature, IHDR, one IDAT, IEND; read in any of its colour types and depths,
// palettes and tRNS, interlaced or not.

#include <cstdint>
#include <string>
#include <vector>

namespace emulation {
namespace image {

  // `rgba` is `width` x `height` pixels, four bytes each, rows top to bottom. False if the file
  // cannot be written. `level` is zlib's, 1 (fastest, and run-length only) to 9; -1 its default.
  bool WritePngFile(const std::string& path, int width, int height, const uint8_t* rgba,
                    int level = -1);

  // The same as bytes, for the tests.
  std::vector<uint8_t> EncodePng(int width, int height, const uint8_t* rgba, int level = -1);

  // A PNG file's pixels as RGBA8, rows top to bottom - 16-bit samples cut to their high byte,
  // a colour key (tRNS) as alpha 0. False if it cannot be read or is not a PNG this reads.
  bool ReadPngFile(const std::string& path, int* width, int* height, std::vector<uint8_t>* rgba);
  bool DecodePng(const uint8_t* data, size_t size, int* width, int* height,
                 std::vector<uint8_t>* rgba);

}  // namespace image
}  // namespace emulation

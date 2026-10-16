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
#include "render/png_file.h"

#include "zlib.h"

#include <cstdio>

namespace emulation {
namespace psx {

namespace {

void Put32(std::vector<uint8_t>* out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out->push_back(static_cast<uint8_t>(value >> shift));
}

// A chunk: its length, its type, its data, and the CRC of the type and data.
void Chunk(std::vector<uint8_t>* out, const char* type, const std::vector<uint8_t>& data) {
  Put32(out, static_cast<uint32_t>(data.size()));
  const size_t start = out->size();
  out->insert(out->end(), type, type + 4);
  out->insert(out->end(), data.begin(), data.end());
  Put32(out, static_cast<uint32_t>(crc32(0, out->data() + start, static_cast<uInt>(out->size() - start))));
}

}  // namespace

std::vector<uint8_t> EncodePng(int width, int height, const uint8_t* rgba) {
  std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  std::vector<uint8_t> header;
  Put32(&header, static_cast<uint32_t>(width));
  Put32(&header, static_cast<uint32_t>(height));
  header.insert(header.end(), { 8, 6, 0, 0, 0 });   // 8 bits, RGBA, deflate, no filter, no interlace
  Chunk(&png, "IHDR", header);

  // Each row behind a filter byte of 0 (none), then deflated in one go.
  const size_t row = static_cast<size_t>(width) * 4;
  std::vector<uint8_t> raw;
  raw.reserve((row + 1) * static_cast<size_t>(height));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    raw.insert(raw.end(), rgba + y * row, rgba + (y + 1) * row);
  }
  z_stream z = {};
  deflateInit(&z, Z_DEFAULT_COMPRESSION);
  std::vector<uint8_t> packed(deflateBound(&z, static_cast<uLong>(raw.size())));
  z.next_in = raw.data();
  z.avail_in = static_cast<uInt>(raw.size());
  z.next_out = packed.data();
  z.avail_out = static_cast<uInt>(packed.size());
  deflate(&z, Z_FINISH);
  packed.resize(z.total_out);
  deflateEnd(&z);
  Chunk(&png, "IDAT", packed);
  Chunk(&png, "IEND", {});
  return png;
}

bool WritePngFile(const std::string& path, int width, int height, const uint8_t* rgba) {
  const std::vector<uint8_t> png = EncodePng(width, height, rgba);
  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr)
    return false;
  const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
  return fclose(f) == 0 && ok;
}

}  // namespace psx
}  // namespace emulation

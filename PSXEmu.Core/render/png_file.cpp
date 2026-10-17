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
#include <cstring>

namespace emulation {
namespace psx {

namespace {

void Put32(std::vector<uint8_t>* out, uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    out->push_back(static_cast<uint8_t>(value >> shift));
}

uint32_t Get32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

int Paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
  return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}

// One pass of an image - the whole of it, or one of Adam7's seven: its first pixel and the steps
// between its pixels.
struct Pass {
  uint32_t x0, y0, dx, dy;
};

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

bool DecodePng(const uint8_t* data, size_t size, int* width, int* height,
               std::vector<uint8_t>* rgba) {
  static const uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  if (size < 8 || memcmp(data, kSignature, 8) != 0)
    return false;
  uint32_t w = 0, h = 0;
  int depth = 0, colour = -1, interlace = 0;
  std::vector<uint8_t> packed, palette, transparent;
  for (size_t at = 8; at + 12 <= size;) {
    const uint32_t length = Get32(data + at);
    if (length > size - at - 12)
      return false;
    const uint8_t* type = data + at + 4;
    const uint8_t* body = data + at + 8;
    if (memcmp(type, "IHDR", 4) == 0) {
      if (length < 13 || body[10] != 0 || body[11] != 0)
        return false;   // deflate and the one filter method are all there is
      w = Get32(body);
      h = Get32(body + 4);
      depth = body[8];
      colour = body[9];
      interlace = body[12];
    } else if (memcmp(type, "PLTE", 4) == 0) {
      palette.assign(body, body + length);
    } else if (memcmp(type, "tRNS", 4) == 0) {
      transparent.assign(body, body + length);
    } else if (memcmp(type, "IDAT", 4) == 0) {
      packed.insert(packed.end(), body, body + length);
    } else if (memcmp(type, "IEND", 4) == 0) {
      break;
    }
    at += 12 + static_cast<size_t>(length);
  }
  // Up to 32768 a side and 64 million pixels: all of VRAM at 4 bits, replaced at 8x, is 32768x4096.
  if (w == 0 || h == 0 || w > 32768 || h > 32768 || static_cast<uint64_t>(w) * h > (1u << 26) ||
      interlace > 1)
    return false;
  int channels = 0;
  bool depth_ok = false;
  switch (colour) {
  case 0:
    channels = 1;
    depth_ok = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    break;
  case 2:
    channels = 3;
    depth_ok = depth == 8 || depth == 16;
    break;
  case 3:
    channels = 1;
    depth_ok = (depth == 1 || depth == 2 || depth == 4 || depth == 8) && palette.size() >= 3;
    break;
  case 4:
    channels = 2;
    depth_ok = depth == 8 || depth == 16;
    break;
  case 6:
    channels = 4;
    depth_ok = depth == 8 || depth == 16;
    break;
  default:
    break;
  }
  if (!depth_ok)
    return false;
  const uint32_t bits = static_cast<uint32_t>(channels * depth);   // a pixel's
  const uint32_t step = bits >= 8 ? bits / 8 : 1;                  // the filters' byte distance
  static const Pass kWhole[1] = { { 0, 0, 1, 1 } };
  static const Pass kAdam7[7] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
                                  { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
  const Pass* passes = interlace ? kAdam7 : kWhole;
  const int pass_count = interlace ? 7 : 1;
  auto pass_size = [&](const Pass& p, uint32_t* pw, uint32_t* ph) {
    *pw = w > p.x0 ? (w - p.x0 + p.dx - 1) / p.dx : 0;
    *ph = h > p.y0 ? (h - p.y0 + p.dy - 1) / p.dy : 0;
  };
  size_t raw_size = 0;
  for (int i = 0; i < pass_count; ++i) {
    uint32_t pw, ph;
    pass_size(passes[i], &pw, &ph);
    if (pw != 0 && ph != 0)
      raw_size += static_cast<size_t>(ph) * (1 + (static_cast<size_t>(pw) * bits + 7) / 8);
  }
  std::vector<uint8_t> raw(raw_size);
  z_stream z = {};
  if (inflateInit(&z) != Z_OK)
    return false;
  z.next_in = packed.data();
  z.avail_in = static_cast<uInt>(packed.size());
  z.next_out = raw.data();
  z.avail_out = static_cast<uInt>(raw.size());
  const int inflated = inflate(&z, Z_FINISH);
  const bool whole = (inflated == Z_STREAM_END || inflated == Z_BUF_ERROR) && z.total_out == raw.size();
  inflateEnd(&z);
  if (!whole)
    return false;

  rgba->assign(static_cast<size_t>(w) * h * 4, 0);
  const uint32_t most = (1u << depth) - 1;
  // Sample `index` of a row at 8 bits; `stored` as the file has it, for tRNS's comparison.
  auto sample = [&](const uint8_t* row, uint32_t index, uint32_t* stored) -> uint8_t {
    if (depth == 16) {
      *stored = (static_cast<uint32_t>(row[index * 2]) << 8) | row[index * 2 + 1];
      return row[index * 2];
    }
    if (depth == 8) {
      *stored = row[index];
      return row[index];
    }
    const uint32_t bit = index * static_cast<uint32_t>(depth);
    *stored = (row[bit / 8] >> (8 - depth - static_cast<int>(bit % 8))) & most;
    return static_cast<uint8_t>(*stored * 255 / most);
  };
  auto key = [&](int i) -> uint32_t {   // tRNS's colour key, a 16-bit value per channel
    return (static_cast<uint32_t>(transparent[i * 2]) << 8) | transparent[i * 2 + 1];
  };
  size_t offset = 0;
  for (int i = 0; i < pass_count; ++i) {
    const Pass& p = passes[i];
    uint32_t pw, ph;
    pass_size(p, &pw, &ph);
    if (pw == 0 || ph == 0)
      continue;
    const size_t stride = (static_cast<size_t>(pw) * bits + 7) / 8;
    const uint8_t* above = nullptr;
    for (uint32_t y = 0; y < ph; ++y) {
      uint8_t* line = raw.data() + offset;
      const uint8_t filter = line[0];
      uint8_t* row = line + 1;
      for (size_t x = 0; x < stride; ++x) {
        const int a = x >= step ? row[x - step] : 0;
        const int b = above != nullptr ? above[x] : 0;
        const int c = (above != nullptr && x >= step) ? above[x - step] : 0;
        switch (filter) {
        case 0: break;
        case 1: row[x] = static_cast<uint8_t>(row[x] + a); break;
        case 2: row[x] = static_cast<uint8_t>(row[x] + b); break;
        case 3: row[x] = static_cast<uint8_t>(row[x] + ((a + b) >> 1)); break;
        case 4: row[x] = static_cast<uint8_t>(row[x] + Paeth(a, b, c)); break;
        default: return false;
        }
      }
      for (uint32_t x = 0; x < pw; ++x) {
        uint8_t* out = &(*rgba)[((static_cast<size_t>(p.y0) + y * p.dy) * w + p.x0 + x * p.dx) * 4];
        uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
        switch (colour) {
        case 0: {
          const uint8_t g = sample(row, x, &s0);
          out[0] = out[1] = out[2] = g;
          out[3] = (transparent.size() >= 2 && s0 == key(0)) ? 0 : 255;
          break;
        }
        case 2:
          out[0] = sample(row, x * 3, &s0);
          out[1] = sample(row, x * 3 + 1, &s1);
          out[2] = sample(row, x * 3 + 2, &s2);
          out[3] = (transparent.size() >= 6 && s0 == key(0) && s1 == key(1) && s2 == key(2)) ? 0 : 255;
          break;
        case 3:
          sample(row, x, &s0);
          if (s0 * 3 + 2 < palette.size()) {
            out[0] = palette[s0 * 3];
            out[1] = palette[s0 * 3 + 1];
            out[2] = palette[s0 * 3 + 2];
          }
          out[3] = s0 < transparent.size() ? transparent[s0] : 255;
          break;
        case 4: {
          const uint8_t g = sample(row, x * 2, &s0);
          out[0] = out[1] = out[2] = g;
          out[3] = sample(row, x * 2 + 1, &s1);
          break;
        }
        default:
          out[0] = sample(row, x * 4, &s0);
          out[1] = sample(row, x * 4 + 1, &s1);
          out[2] = sample(row, x * 4 + 2, &s2);
          out[3] = sample(row, x * 4 + 3, &s3);
          break;
        }
      }
      above = row;
      offset += 1 + stride;
    }
  }
  *width = static_cast<int>(w);
  *height = static_cast<int>(h);
  return true;
}

bool ReadPngFile(const std::string& path, int* width, int* height, std::vector<uint8_t>* rgba) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr)
    return false;
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> chunk(65536);
  size_t got;
  while ((got = fread(chunk.data(), 1, chunk.size(), f)) > 0)
    bytes.insert(bytes.end(), chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(got));
  fclose(f);
  return DecodePng(bytes.data(), bytes.size(), width, height, rgba);
}

}  // namespace psx
}  // namespace emulation

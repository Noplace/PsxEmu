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

// What the AI texture upscaler is handed (Docs/Texture-Upscaling-Plan.md, phase 3), as the core
// sees it: the upscaler itself - a model on the graphics card - is the front end's.
//
// - **A source's texels** (SourceTexels): what the texture tracker names, as it has read it so
//   far - a canvas of the source's records, decoded as a dump decodes it, and the 8x8-texel cells
//   draws have read through its palette. Or a dump read back, for Upscale All.
// - **The queue** (TextureUpscaleQueue): the tracker tells it of a source each time draws read
//   more of it, on the rasteriser's thread, and goes on at once; the upscaler works on its own
//   thread and hands what it makes to the texture pack (TexturePack::Provide), which draws it
//   from the next job on.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

  class TexturePack;

  // A source to upscale. Immutable once handed over, so any thread may read it.
  class SourceTexels {
   public:
    virtual ~SourceTexels() {}
    std::string name;            // as the tracker names it, without ".png"
    uint64_t key = 0;            // TextureTracker::Key
    uint32_t width = 0, height = 0;   // in texels
    // Its 8x8-texel cells, row by row, cells_x to a row: 1 for each read so far.
    uint32_t cells_x = 0, cells_y = 0;
    std::vector<uint8_t> cells;
    // The texels of [x, x + w) x [y, y + h) - inside the source - as RGBA8 rows, `w` wide, alpha
    // 0 where a texel is 0000h, 128 where bit 15 is set, 255 elsewhere: every texel, read or not.
    virtual void Decode(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* rgba) const = 0;
  };

  // A source whose texels are a picture already: a dump read back.
  class PictureTexels : public SourceTexels {
   public:
    std::vector<uint8_t> rgba;   // width x height
    void Decode(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t* out) const override {
      for (uint32_t row = 0; row < h; ++row)
        for (uint32_t col = 0; col < w * 4; ++col)
          out[static_cast<size_t>(row) * w * 4 + col] =
              rgba[(static_cast<size_t>(y + row) * width + x) * 4 + col];
    }
  };

  // The upscaler, as the tracker and the machine see it.
  class TextureUpscaleQueue {
   public:
    virtual ~TextureUpscaleQueue() {}
    // On the rasteriser's thread: draws have read more of `source` - every cell read so far, the
    // ones already handed over too. Quick; the work is the upscaler's own thread's.
    virtual void Read(std::shared_ptr<const SourceTexels> source) = 0;
    // The pack its images go to, or null for none - the machine's thread, with nothing being
    // drawn. Returns once nothing is being handed to the old one.
    virtual void set_pack(TexturePack* pack) = 0;
  };

}  // namespace psx
}  // namespace emulation

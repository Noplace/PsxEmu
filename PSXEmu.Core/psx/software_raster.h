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

// The software rasteriser: every `DrawJob` drawn straight into the native 1024x512 VRAM, one
// pixel at a time, to the console's rules - the fill rule, dithering, semi-transparency, the
// mask bit, the texture window, the displayed field. This is the reference every other
// rasteriser is checked against (Docs/Hardware-Renderer-Plan.md), and it is the code `Gpu`
// always had, moved behind `RasterBackend` without a change.
//
// It draws into VRAM that `Gpu` owns, so `PrepareRead`, `Written` and `Reloaded` have nothing
// to do: there is only one copy.

#include "psx/gpu_core.h"
#include "psx/raster.h"

namespace emulation {
namespace psx {

  class SoftwareRaster : public RasterBackend {
   public:
    // `vram` is Gpu's, kVramWidth x kVramHeight, and outlives this.
    explicit SoftwareRaster(uint16_t* vram);

    void Apply(const DrawJob& job) override;
    void PrepareRead(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void Written(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void Reloaded() override {}
    RasterCounters& counters() override { return counters_; }
    void set_watch(const RasterWatch& watch) override { watch_ = watch; }
    void NoteWatchWrite(uint32_t x, uint32_t y) override;

   private:
    static const int kVramWidth = GpuCore::kVramWidth;
    static const int kVramHeight = GpuCore::kVramHeight;

    void RasterTriangle(const RasterVertex& v0, const RasterVertex& v1, const RasterVertex& v2,
                        const RasterState& state);
    void DrawLineSegment(const RasterVertex& v0, const RasterVertex& v1,
                         const RasterState& state);
    // A triangle once its corners are worked out: the drawing bounds, the three edge functions and
    // each interpolated quantity as its value at (left, top) and its step per pixel and per row.
    struct Stepped { int32_t at, dx, dy; };
    struct TriSetup {
      int32_t left, right, top, bottom;
      int32_t double_area;
      int32_t bias[3];
      Stepped edge[3];
      Stepped red, green, blue, tex_u, tex_v;
      uint8_t flat_r, flat_g, flat_b;
    };
    template <bool kGouraud, bool kTextured, bool kDither>
    void ShadeRows(const TriSetup& setup, const RasterState& state);
    template <bool kTextured>
    void ShadeRectangle(const DrawJob& job);
    void RasterRectangle(const DrawJob& job);
    void RasterFill(const DrawJob& job);
    void RasterVramCopy(const DrawJob& job);
    void PlotPixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b,
                   const RasterState& state, bool from_texture, bool texture_mask);
    // PlotPixel without its two checks - the drawing area and the displayed field - for a
    // caller that has settled both for a whole row.
    void WritePixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b,
                    const RasterState& state, bool from_texture, bool texture_mask);
    uint16_t SampleTexture(uint32_t u, uint32_t v, const RasterState& state);
    void BlendSemiTransparent(uint16_t* dst, uint8_t r, uint8_t g, uint8_t b,
                              uint32_t mode) const;

    // Whether a draw leaves this VRAM row alone. In 480i with drawing to the display area
    // prohibited, hardware puts down only the field that is not being shown (bug 89).
    // Primitives and fills skip; a CPU-to-VRAM transfer and a VRAM-to-VRAM copy do not.
    // Answered from the environment the job carries, so a rasteriser running behind the
    // machine skips the field that was being displayed when the command was issued.
    bool SkipsVramRow(int32_t y) const {
      return env_.skip_field && (static_cast<uint32_t>(y) & 1u) == env_.active_line_lsb;
    }

    inline uint16_t& VramAt(uint32_t x, uint32_t y) {
      return vram_[((y & (kVramHeight - 1)) * kVramWidth) + (x & (kVramWidth - 1))];
    }

    uint16_t* vram_;
    // The environment of the job being drawn.
    RasterEnv env_ = {};
    RasterCounters counters_ = {};
    RasterWatch watch_;
    // The command of the job being drawn, or last drawn, for NoteWatchWrite.
    uint8_t command_ = 0;
  };

}  // namespace psx
}  // namespace emulation

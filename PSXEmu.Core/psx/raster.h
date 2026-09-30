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

// The line between the GPU and whatever puts its pixels down (Docs/Hardware-Renderer-Plan.md).
//
// `Gpu` is the console's GPU: GP0 and GP1, GPUSTAT, the command queue, DMA's handshake, the
// display timing and what every draw costs. It parses each drawing command into a `DrawJob` -
// a triangle, line, rectangle, fill or VRAM-to-VRAM copy, with a snapshot of the state it was
// issued under - and hands it to a `RasterBackend`, on the rasteriser's thread or its own.
//
// Two backends are meant to exist: `SoftwareRaster`, which draws into the native 1024x512 VRAM
// exactly as this emulator always has, and a hardware one that draws at a higher resolution on
// the host's GPU. Whichever is in use, `Gpu` keeps the native VRAM that the rest of the machine
// reads - CPU transfers, the display, save states - and tells the backend when it is about to
// read it or has just written it, which is how a backend that draws somewhere else keeps the
// two in step. For the software backend those calls are nothing, since it draws into that same
// VRAM.
//
// Nothing here may change what a game observes. Every cost is charged by `Gpu` when the command
// is parsed; a backend only makes pixels.

#include "psx/shared_picture.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

  struct RasterVertex {
    int32_t x, y;      // already offset, in VRAM space
    uint8_t r, g, b;
    uint8_t u, v;
    // PGXP: where the GTE put this vertex before rounding it into x and y, offset the same
    // way, and the depth it was projected from (0 when unknown). Only when `precise`, and only
    // the hardware rasteriser draws with them; the machine and the software rasteriser use
    // x and y, always.
    bool precise = false;
    float fx = 0.0f, fy = 0.0f, w = 0.0f;
  };

  // How a primitive is drawn: its texture, if any, and how it blends.
  struct RasterState {
    bool textured;
    bool raw_texture;    // sample the texture without modulating by the colour
    bool semi_transparent;
    bool gouraud;
    uint32_t clut_x, clut_y;
    uint32_t texpage_x, texpage_y;
    uint32_t texpage_colors;
    uint32_t semi_mode;
    bool dither;
    bool flip_x, flip_y;   // textured rectangles only
  };

  // Everything a draw needs that is not in its own words: the drawing area it is clipped to,
  // the texture window, the mask rules and which field is being displayed. Snapshotted when the
  // command is parsed rather than read as the pixels go down, so a backend running behind the
  // machine draws on the state the command was issued under (phase 7 of
  // Docs/Threading-Plan.md).
  struct RasterEnv {
    int32_t area_left, area_top, area_right, area_bottom;
    uint32_t tw_mask_x, tw_mask_y, tw_offset_x, tw_offset_y;
    bool force_set_mask, check_mask;
    bool skip_field;
    uint32_t active_line_lsb;
  };

  // One piece of rasterising, parsed and costed but not yet drawn. Fixed size on purpose: a
  // polyline becomes one job per segment and a quad two triangles, so nothing here needs a
  // side buffer.
  struct DrawJob {
    enum Kind { kTriangle, kLine, kRectangle, kFill, kVramCopy };
    Kind kind;
    RasterEnv env;
    RasterState state;
    RasterVertex v[3];            // triangle: three, line: the first two
    int32_t x, y, w, h;           // rectangle, fill, copy destination
    int32_t src_x, src_y;         // copy source
    uint8_t r, g, b;              // rectangle colour
    uint8_t base_u, base_v;       // textured rectangle
    uint16_t fill_colour;
    uint8_t command;              // the GP0 command byte, for attributing writes
  };

  // What the backend counts as it draws. Kept apart from `Gpu::Stats` so that no member of it is
  // ever written by two threads: `Gpu` adds these into its stats at a barrier, the only moment
  // the machine thread may read them, and clears them.
  struct RasterCounters {
    uint64_t pixels, clipped, field_skipped, mask_rejected, transparent_texels;
    uint64_t texels_by_depth[4];
    uint64_t watch_writes;
    uint32_t watch_writers[256];
  };

  // What is shown of the plane beside VRAM (psx/shared_picture.h), in place of the picture:
  // nothing, its depth, or its motion. Video > View Depth and View Motion.
  enum class PlaneView { kPicture, kDepth, kMotion };

  // A VRAM rectangle whose writes are counted against the command making them - see
  // `Gpu::WatchVram`. Empty when `w` is zero.
  struct RasterWatch {
    uint32_t x = 0, y = 0, w = 0, h = 0;
  };

  class RasterBackend {
   public:
    virtual ~RasterBackend() {}

    // Draws one job. On whichever thread is rasterising - the rasteriser's own, or the
    // machine's when there is no such thread - and only ever one at a time.
    virtual void Apply(const DrawJob& job) = 0;

    // The rest are called on the machine thread, with nothing being drawn: `Gpu` has waited for
    // the backend to finish everything it was handed first.

    // The CPU is about to read this rectangle of native VRAM - a VRAM-to-CPU transfer, the
    // display, a save state, the debugger. A backend drawing somewhere else brings it up to
    // date. Coordinates wrap as VRAM does.
    virtual void PrepareRead(uint32_t x, uint32_t y, uint32_t w, uint32_t h) = 0;
    // The CPU has written this rectangle of native VRAM - a CPU-to-VRAM transfer.
    virtual void Written(uint32_t x, uint32_t y, uint32_t w, uint32_t h) = 0;
    // All of native VRAM has just been replaced - a save state loaded, or the GPU reset.
    virtual void Reloaded() = 0;

    // Why the backend can no longer draw - its graphics device has gone, say - or null while it
    // can. `Gpu` looks once a frame and carries on with the software rasteriser if it cannot.
    virtual const char* lost() const { return nullptr; }

    // What it draws on - a graphics card's name - for saying so; empty for the software one.
    virtual std::string device() const { return std::string(); }

    // The display area as the backend draws it when that is sharper than native VRAM,
    // `*scale` times native size each way: left on the graphics card in `shared` if the backend
    // was made to (RasterOptions::shared_picture) and has a texture free, and otherwise rows of
    // 0xFFRRGGBB in `picture`. False when there is nothing better than native VRAM's own - the
    // software rasteriser, or a display area that wraps round VRAM's edge - and `Gpu` shows
    // native VRAM's.
    virtual bool ResolveDisplay(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                std::vector<uint32_t>* picture, SharedPicture* shared,
                                int* scale) {
      (void)x; (void)y; (void)w; (void)h; (void)picture; (void)shared; (void)scale;
      return false;
    }

    // Keeps the plane beside VRAM or not (psx/shared_picture.h, Docs/DLSS-Plan.md), and shows it
    // in place of the picture or not - which keeps it too. Nothing to the software rasteriser.
    // What the machine sees is the same either way: the plane is never read back.
    virtual void SetPlanes(bool keep, PlaneView view) { (void)keep; (void)view; }

    // The counters since the last call, which `Gpu` merges into its stats and clears.
    virtual RasterCounters& counters() = 0;
    // Which rectangle's writes to attribute. Set by `Gpu` when it changes.
    virtual void set_watch(const RasterWatch& watch) = 0;
    // Records one write into the watched rectangle, for writes `Gpu` makes itself - a CPU-to-VRAM
    // transfer's. It is attributed to the command the backend last drew, which is what it always
    // was: the rasteriser's command, not the transfer's.
    virtual void NoteWatchWrite(uint32_t x, uint32_t y) = 0;
  };

  // How a hardware rasteriser is asked to draw: EmuConfig's resolution_scale and true_color,
  // and - from the front end, which knows what shows the picture - where.
  struct RasterOptions {
    int scale = 1;             // internal resolution, 1 (native) to 8 times
    bool true_color = false;   // above 1x: no dithering, and eight bits a channel kept
    // The graphics adapter to draw on, by its LUID - the presenter's, so the picture can be
    // shared with it - or 0 for Windows' default.
    uint64_t adapter = 0;
    // Whether what shows the picture can take it on the card (SharedPicture) rather than as
    // pixels.
    bool shared_picture = false;
  };

  // Makes a hardware rasteriser for `vram` - Gpu's native VRAM, which it must keep in step
  // with - or returns null with `error` saying why. The core cannot make one itself, since it
  // knows no graphics API; a front end hands one over (System::set_hardware_raster).
  typedef std::function<std::unique_ptr<RasterBackend>(uint16_t* vram, const RasterOptions& options,
                                                       std::string* error)>
      RasterFactory;

}  // namespace psx
}  // namespace emulation

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
#include "psx/psx.h"

#include <algorithm>

namespace emulation {
namespace psx {

namespace {

// The GPU runs at 53.222400 MHz against the CPU's 33.868800 MHz, so a GPU
// dot clock is 11/7 of a CPU cycle. Tick() is handed CPU cycles and scales
// them. The ratio and the scanline width live in the header now - the
// display timing accessors the root counters use are inline and need them.
using emulation::psx::Gpu;
const uint32_t kGpuClockNumerator   = Gpu::kGpuClockNumerator;
const uint32_t kGpuClockDenominator = Gpu::kGpuClockDenominator;
const uint32_t kDotsPerScanline      = Gpu::kDotsPerScanline;
const uint32_t kScanlinesNtsc     = 263;
const uint32_t kScanlinesPal      = 314;

// Dither matrix, applied to the 8-bit components before they are truncated to
// the 5 bits VRAM stores. Without it, Gouraud shading bands visibly.
const int8_t kDitherTable[4][4] = {
  { -4,  0, -3,  1 },
  {  2, -2,  3, -1 },
  { -3,  1, -4,  0 },
  {  3, -1,  2, -2 },
};

inline int32_t SignExtend11(uint32_t value) {
  return static_cast<int32_t>(value << 21) >> 21;
}

inline uint8_t Clamp8(int32_t v) {
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline uint16_t To15Bit(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
}

// 5-bit VRAM components are widened by replicating the top bits, so 0x1F maps
// to 0xFF rather than 0xF8 and white stays white.
inline uint8_t From5Bit(uint32_t c) {
  return static_cast<uint8_t>((c << 3) | (c >> 2));
}

}  // namespace

Gpu::Gpu() : vram_(nullptr), framebuffer_(nullptr) {
}

Gpu::~Gpu() {
}

int Gpu::Initialize() {
  vram_ = new uint16_t[kVramWidth * kVramHeight];
  framebuffer_ = new uint32_t[kVramWidth * kVramHeight];
  memset(vram_, 0, sizeof(uint16_t) * kVramWidth * kVramHeight);
  memset(framebuffer_, 0, sizeof(uint32_t) * kVramWidth * kVramHeight);

  status_.raw = 0x14802000;
  fifo_count_ = 0;
  fifo_needed_ = 0;
  transfer_mode_ = kTransferNone;
  memset(&transfer_, 0, sizeof(transfer_));
  read_latch_ = 0;
  current_command_ = 0;
  watch_x_ = watch_y_ = watch_w_ = watch_h_ = 0;

  draw_area_left_ = 0;
  draw_area_top_ = 0;
  draw_area_right_ = 0;
  draw_area_bottom_ = 0;
  draw_offset_x_ = 0;
  draw_offset_y_ = 0;
  texture_window_mask_x_ = 0;
  texture_window_mask_y_ = 0;
  texture_window_offset_x_ = 0;
  texture_window_offset_y_ = 0;
  force_set_mask_ = false;
  check_mask_ = false;
  rect_flip_x_ = false;
  rect_flip_y_ = false;

  display_vram_x_ = 0;
  display_vram_y_ = 0;
  horizontal_display_start_ = 0x200;
  horizontal_display_end_ = 0xC00;
  vertical_display_start_ = 0x10;
  vertical_display_end_ = 0x100;

  dot_accumulator_ = 0;
  dot_clock_remainder_ = 0;
  dot_clock_accum_ = 0;
  pending_dot_clocks_ = 0;
  pending_hblanks_ = 0;
  scanline_ = 0;
  was_in_vblank_ = false;
  frame_count_ = 0;
  memset(&stats_, 0, sizeof(stats_));

  UpdateDisplaySize();
  return S_OK;
}

int Gpu::Deinitialize() {
  delete[] vram_;
  delete[] framebuffer_;
  vram_ = nullptr;
  framebuffer_ = nullptr;
  return S_OK;
}

// ---------------------------------------------------------------------------
// Register interface
// ---------------------------------------------------------------------------

uint32_t Gpu::ReadStatus() {
  GpuStatus s = status_;
  // The core is not cycle-accurate enough to model the FIFO filling up, so all
  // three ready bits report permanently ready. Reporting busy would deadlock
  // software that spins on them.
  //
  // Bit 27 has to be included in that. Reporting it only while a VRAM-to-CPU
  // transfer is in flight looks more honest, but it is not what the bit means:
  // it says the GPU is ready to hand VRAM over, not that a transfer is already
  // running. Software that checks readiness *before* issuing the read command
  // waits for a bit that this GPU would only set afterwards, and spins for
  // ever - which is exactly where the BIOS shell was stopping.
  s.ready_cmd = 1;
  s.ready_dma = 1;
  s.ready_vram_send = 1;

  switch (s.dma_direction) {
    case 0:  s.dma_request = 0; break;              // off
    case 1:  s.dma_request = 1; break;              // FIFO status
    case 2:  s.dma_request = s.ready_dma; break;    // CPU -> GP0
    default: s.dma_request = s.ready_vram_send; break;  // GPUREAD -> CPU
  }
  return s.raw;
}

uint32_t Gpu::ReadData() {
  if (transfer_mode_ != kTransferFromVram)
    return read_latch_;

  // Two 16-bit pixels per 32-bit read, left to right, top to bottom.
  uint32_t result = 0;
  for (int half = 0; half < 2; ++half) {
    uint16_t pixel = VramAt(transfer_.x + transfer_.px, transfer_.y + transfer_.py);
    result |= static_cast<uint32_t>(pixel) << (half * 16);
    if (++transfer_.px >= transfer_.w) {
      transfer_.px = 0;
      if (++transfer_.py >= transfer_.h) {
        transfer_mode_ = kTransferNone;
        break;
      }
    }
  }
  read_latch_ = result;
  return result;
}

void Gpu::WriteData(uint32_t data) {
  ++stats_.gp0_words;
  if (transfer_mode_ == kTransferToVram) {
    StepTransfer(data);
    return;
  }

  if (fifo_count_ == 0) {
    fifo_needed_ = CommandLength(data >> 24);
    // A polyline runs until its terminator rather than for a fixed length.
    if (fifo_needed_ < 0) {
      fifo_[fifo_count_++] = data;
      return;
    }
  } else if (fifo_needed_ < 0) {
    // Collecting a polyline. 0x55555555 (with the low bits masked) ends it.
    if ((data & 0xF000F000) == 0x50005000) {
      fifo_needed_ = fifo_count_;
    } else {
      if (fifo_count_ < static_cast<int>(sizeof(fifo_) / sizeof(fifo_[0])))
        fifo_[fifo_count_++] = data;
      return;
    }
  }

  if (fifo_count_ < static_cast<int>(sizeof(fifo_) / sizeof(fifo_[0])))
    fifo_[fifo_count_++] = data;

  if (fifo_count_ >= fifo_needed_) {
    ExecuteCommand();
    fifo_count_ = 0;
    fifo_needed_ = 0;
  }
}

void Gpu::WriteStatus(uint32_t data) {
  ++stats_.gp1_words;
  ExecuteGp1(data);
}

// Number of 32-bit words each GP0 command consumes, including the command word
// itself. A negative result means "variable, terminated by 0x55555555".
int Gpu::CommandLength(uint32_t command) {
  if (command == 0x02) return 3;                 // fill rectangle
  if (command < 0x20)  return 1;                 // nop / clear cache / irq

  if (command < 0x40) {                          // polygons
    const bool gouraud  = (command & 0x10) != 0;
    const bool quad     = (command & 0x08) != 0;
    const bool textured = (command & 0x04) != 0;
    const int verts = quad ? 4 : 3;
    return 1 + verts * (1 + (textured ? 1 : 0) + (gouraud ? 1 : 0)) -
           (gouraud ? 1 : 0);
  }

  if (command < 0x60) {                          // lines
    const bool gouraud  = (command & 0x10) != 0;
    const bool polyline = (command & 0x08) != 0;
    if (polyline) return -1;
    return gouraud ? 4 : 3;
  }

  if (command < 0x80) {                          // rectangles / sprites
    const uint32_t size = (command >> 3) & 3;
    const bool textured = (command & 0x04) != 0;
    return 2 + (textured ? 1 : 0) + (size == 0 ? 1 : 0);
  }

  if (command < 0xA0) return 4;                  // VRAM -> VRAM
  if (command < 0xC0) return 3;                  // CPU  -> VRAM
  if (command < 0xE0) return 3;                  // VRAM -> CPU
  return 1;                                      // E1..E6 rendering attributes
}

void Gpu::ExecuteCommand() {
  const uint32_t command = fifo_[0] >> 24;
  current_command_ = command;
  ++stats_.gp0_commands[command & 0xFF];
  if (command >= 0x20 && command < 0x80)
    ++stats_.primitives;

  if (command == 0x02) { CmdFillRectangle(); return; }
  if (command < 0x20)  { return; }               // nop / clear cache / irq
  if (command < 0x40)  { CmdPolygon(); return; }
  if (command < 0x60)  { CmdLine(); return; }
  if (command < 0x80)  { CmdRectangle(); return; }
  if (command < 0xA0)  { CmdVramToVramCopy(); return; }
  if (command < 0xC0)  { CmdCpuToVram(); return; }
  if (command < 0xE0)  { CmdVramToCpu(); return; }

  const uint32_t data = fifo_[0];
  switch (command) {
    case 0xE1:  // draw mode setting
      // GPUSTAT holds bits 0-10 as written and the texture-disable bit at 15.
      status_.raw = (status_.raw & ~0x87FF) | (data & 0x7FF) |
                    ((data & 0x800) << 4);
      // Bits 12-13 have nowhere to live in GPUSTAT, so they are kept here.
      rect_flip_x_ = (data & 0x1000) != 0;
      rect_flip_y_ = (data & 0x2000) != 0;
      break;
    case 0xE2:  // texture window
      texture_window_mask_x_   = (data >> 0)  & 0x1F;
      texture_window_mask_y_   = (data >> 5)  & 0x1F;
      texture_window_offset_x_ = (data >> 10) & 0x1F;
      texture_window_offset_y_ = (data >> 15) & 0x1F;
      break;
    case 0xE3:  // drawing area top-left
      draw_area_left_ = data & 0x3FF;
      draw_area_top_  = (data >> 10) & 0x1FF;
      break;
    case 0xE4:  // drawing area bottom-right
      draw_area_right_  = data & 0x3FF;
      draw_area_bottom_ = (data >> 10) & 0x1FF;
      break;
    case 0xE5:  // drawing offset
      draw_offset_x_ = SignExtend11(data & 0x7FF);
      draw_offset_y_ = SignExtend11((data >> 11) & 0x7FF);
      break;
    case 0xE6:  // mask bit setting
      force_set_mask_ = (data & 1) != 0;
      check_mask_     = (data & 2) != 0;
      status_.set_mask   = force_set_mask_ ? 1 : 0;
      status_.check_mask = check_mask_ ? 1 : 0;
      break;
    default:
      break;
  }
}

void Gpu::ExecuteGp1(uint32_t data) {
  const uint32_t command = (data >> 24) & 0x3F;
  ++stats_.gp1_commands[command];
  const uint32_t params = data & 0xFFFFFF;

  switch (command) {
    case 0x00:  // reset GPU
      status_.raw = 0x14802000;
      fifo_count_ = 0;
      fifo_needed_ = 0;
      transfer_mode_ = kTransferNone;
      draw_area_left_ = draw_area_top_ = 0;
      draw_area_right_ = draw_area_bottom_ = 0;
      draw_offset_x_ = draw_offset_y_ = 0;
      texture_window_mask_x_ = texture_window_mask_y_ = 0;
      texture_window_offset_x_ = texture_window_offset_y_ = 0;
      display_vram_x_ = display_vram_y_ = 0;
      horizontal_display_start_ = 0x200;
      horizontal_display_end_ = 0xC00;
      vertical_display_start_ = 0x10;
      vertical_display_end_ = 0x100;
      UpdateDisplaySize();
      break;

    case 0x01:  // reset command buffer
      fifo_count_ = 0;
      fifo_needed_ = 0;
      transfer_mode_ = kTransferNone;
      break;

    case 0x02:  // acknowledge interrupt
      status_.irq = 0;
      break;

    case 0x03:  // display enable
      status_.display_disable = params & 1;
      break;

    case 0x04:  // DMA direction
      status_.dma_direction = params & 3;
      break;

    case 0x05:  // start of display area in VRAM
      display_vram_x_ = params & 0x3FE;
      display_vram_y_ = (params >> 10) & 0x1FF;
      break;

    case 0x06:  // horizontal display range
      horizontal_display_start_ = params & 0xFFF;
      horizontal_display_end_ = (params >> 12) & 0xFFF;
      UpdateDisplaySize();
      break;

    case 0x07:  // vertical display range
      vertical_display_start_ = params & 0x3FF;
      vertical_display_end_ = (params >> 10) & 0x3FF;
      UpdateDisplaySize();
      break;

    case 0x08:  // display mode
      status_.hres1 = params & 3;
      status_.vres = (params >> 2) & 1;
      status_.video_mode = (params >> 3) & 1;
      status_.display_depth = (params >> 4) & 1;
      status_.vertical_interlace = (params >> 5) & 1;
      status_.hres2 = (params >> 6) & 1;
      status_.reverse = (params >> 7) & 1;
      UpdateDisplaySize();
      break;

    case 0x10:  // get GPU info
      switch (params & 0xF) {
        case 2: read_latch_ = (texture_window_mask_y_ << 15) |
                              (texture_window_mask_x_ << 10) |
                              (texture_window_offset_y_ << 5) |
                              texture_window_offset_x_; break;
        case 3: read_latch_ = (draw_area_top_ << 10) | draw_area_left_; break;
        case 4: read_latch_ = (draw_area_bottom_ << 10) | draw_area_right_; break;
        case 5: read_latch_ = ((draw_offset_y_ & 0x7FF) << 11) |
                              (draw_offset_x_ & 0x7FF); break;
        case 7: read_latch_ = 2; break;  // GPU version
        default: break;
      }
      break;

    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// CPU <-> VRAM transfers
// ---------------------------------------------------------------------------

void Gpu::StepTransfer(uint32_t data) {
  for (int half = 0; half < 2; ++half) {
    const uint16_t pixel = static_cast<uint16_t>(data >> (half * 16));
    VramAt(transfer_.x + transfer_.px, transfer_.y + transfer_.py) = pixel;
    NoteWatchWrite(transfer_.x + transfer_.px, transfer_.y + transfer_.py);
    if (stats_.transfer_log_count > 0 &&
        stats_.transfer_log_count <= Stats::kTransferCapacity)
      ++stats_.transfers[stats_.transfer_log_count - 1].written;
    if (++transfer_.px >= transfer_.w) {
      transfer_.px = 0;
      if (++transfer_.py >= transfer_.h) {
        transfer_mode_ = kTransferNone;
        return;
      }
    }
  }
}

void Gpu::CmdCpuToVram() {
  // Close off the previous entry before starting a new one, so a transfer that
  // never finished is visible as a short pixel count.

  transfer_.x = fifo_[1] & 0x3FF;
  transfer_.y = (fifo_[1] >> 16) & 0x1FF;
  // A width or height field of zero means the maximum, not nothing.
  transfer_.w = ((fifo_[2] & 0xFFFF) - 1 & 0x3FF) + 1;
  transfer_.h = (((fifo_[2] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;
  transfer_.px = 0;
  transfer_.py = 0;
  transfer_mode_ = kTransferToVram;

  if (stats_.transfer_log_count < Stats::kTransferCapacity) {
    Stats::Transfer& entry = stats_.transfers[stats_.transfer_log_count++];
    entry.x = static_cast<uint16_t>(transfer_.x);
    entry.y = static_cast<uint16_t>(transfer_.y);
    entry.w = static_cast<uint16_t>(transfer_.w);
    entry.h = static_cast<uint16_t>(transfer_.h);
    entry.written = 0;
  }
}

void Gpu::CmdVramToCpu() {
  transfer_.x = fifo_[1] & 0x3FF;
  transfer_.y = (fifo_[1] >> 16) & 0x1FF;
  transfer_.w = ((fifo_[2] & 0xFFFF) - 1 & 0x3FF) + 1;
  transfer_.h = (((fifo_[2] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;
  transfer_.px = 0;
  transfer_.py = 0;
  transfer_mode_ = kTransferFromVram;
}

void Gpu::CmdVramToVramCopy() {
  const uint32_t sx = fifo_[1] & 0x3FF;
  const uint32_t sy = (fifo_[1] >> 16) & 0x1FF;
  const uint32_t dx = fifo_[2] & 0x3FF;
  const uint32_t dy = (fifo_[2] >> 16) & 0x1FF;
  const uint32_t w = ((fifo_[3] & 0xFFFF) - 1 & 0x3FF) + 1;
  const uint32_t h = (((fifo_[3] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;

  for (uint32_t row = 0; row < h; ++row) {
    for (uint32_t col = 0; col < w; ++col) {
      const uint16_t pixel = VramAt(sx + col, sy + row);
      if (check_mask_ && (VramAt(dx + col, dy + row) & 0x8000))
        continue;
      VramAt(dx + col, dy + row) =
          force_set_mask_ ? (pixel | 0x8000) : pixel;
      NoteWatchWrite(dx + col, dy + row);
    }
  }
}

void Gpu::CmdFillRectangle() {
  const uint8_t r = static_cast<uint8_t>(fifo_[0]);
  const uint8_t g = static_cast<uint8_t>(fifo_[0] >> 8);
  const uint8_t b = static_cast<uint8_t>(fifo_[0] >> 16);
  const uint16_t colour = To15Bit(r, g, b);

  // Fill is aligned to 16-pixel columns and ignores the drawing area and the
  // mask bits entirely, which is what makes it the fast way to clear VRAM.
  const uint32_t x = fifo_[1] & 0x3F0;
  const uint32_t y = (fifo_[1] >> 16) & 0x1FF;
  const uint32_t w = ((fifo_[2] & 0x3FF) + 0x0F) & ~0x0F;
  const uint32_t h = (fifo_[2] >> 16) & 0x1FF;

  for (uint32_t row = 0; row < h; ++row) {
    for (uint32_t col = 0; col < w; ++col) {
      VramAt(x + col, y + row) = colour;
      NoteWatchWrite(x + col, y + row);
    }
  }
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

void Gpu::CmdPolygon() {
  const uint32_t command = fifo_[0] >> 24;
  const bool gouraud  = (command & 0x10) != 0;
  const bool quad     = (command & 0x08) != 0;
  const bool textured = (command & 0x04) != 0;
  const bool semi     = (command & 0x02) != 0;
  const bool raw      = (command & 0x01) != 0;
  const int verts = quad ? 4 : 3;

  DrawState state;
  state.textured = textured;
  state.raw_texture = raw;
  state.semi_transparent = semi;
  state.gouraud = gouraud;
  state.clut_x = state.clut_y = 0;
  state.texpage_x = status_.texpage_x * 64;
  state.texpage_y = status_.texpage_y * 256;
  state.texpage_colors = status_.texpage_colors;
  state.semi_mode = status_.semi_mode;
  state.dither = status_.dither != 0;
  state.flip_x = false;
  state.flip_y = false;

  Vertex v[4];
  uint32_t raw_page = 0;
  uint32_t raw_clut = 0;
  int word = 0;
  uint32_t colour = fifo_[word++] & 0xFFFFFF;

  for (int i = 0; i < verts; ++i) {
    if (gouraud && i > 0)
      colour = fifo_[word++] & 0xFFFFFF;

    const uint32_t position = fifo_[word++];
    v[i].x = SignExtend11(position & 0x7FF) + draw_offset_x_;
    v[i].y = SignExtend11((position >> 16) & 0x7FF) + draw_offset_y_;
    v[i].r = static_cast<uint8_t>(colour);
    v[i].g = static_cast<uint8_t>(colour >> 8);
    v[i].b = static_cast<uint8_t>(colour >> 16);
    v[i].u = 0;
    v[i].v = 0;

    if (textured) {
      const uint32_t coord = fifo_[word++];
      v[i].u = static_cast<uint8_t>(coord);
      v[i].v = static_cast<uint8_t>(coord >> 8);
      // The CLUT rides on the first vertex, the texpage on the second.
      if (i == 0) {
        const uint32_t clut = (coord >> 16) & 0xFFFF;
        raw_clut = clut;
        state.clut_x = (clut & 0x3F) * 16;
        state.clut_y = (clut >> 6) & 0x1FF;
      } else if (i == 1) {
        const uint32_t page = (coord >> 16) & 0xFFFF;
        raw_page = page;
        state.texpage_x = (page & 0x0F) * 64;
        state.texpage_y = ((page >> 4) & 1) * 256;
        state.semi_mode = (page >> 5) & 3;
        state.texpage_colors = (page >> 7) & 3;

        // Bit 11 disables texturing for this primitive: it is drawn with its
        // own colour and the texture page is not read at all. Ignoring it
        // meant sampling whatever happened to be at the texpage and painting
        // it on screen.
        if (page & 0x0800)
          state.textured = false;

        // A polygon's texpage also updates the persistent draw mode. Only
        // bits 0-8 map straight across; the texture-disable bit lands at
        // GPUSTAT bit 15, not bit 11 - bit 11 is the mask-set flag, and
        // writing texture-disable into it corrupted the mask setting that
        // software reads back.
        status_.raw = (status_.raw & ~0x81FF) | (page & 0x01FF) |
                      ((page & 0x0800) << 4);
      }
    }
  }

  RecordSetup(command, state, raw_page, raw_clut);

  RasterTriangle(v[0], v[1], v[2], state);
  if (quad)
    RasterTriangle(v[1], v[2], v[3], state);
}

void Gpu::CmdLine() {
  const uint32_t command = fifo_[0] >> 24;
  const bool gouraud  = (command & 0x10) != 0;
  const bool semi     = (command & 0x02) != 0;

  DrawState state;
  state.textured = false;
  state.raw_texture = false;
  state.semi_transparent = semi;
  state.gouraud = gouraud;
  state.clut_x = state.clut_y = 0;
  state.texpage_x = state.texpage_y = 0;
  state.texpage_colors = 0;
  state.semi_mode = status_.semi_mode;
  state.dither = status_.dither != 0;
  state.flip_x = false;
  state.flip_y = false;

  int word = 0;
  uint32_t colour = fifo_[word++] & 0xFFFFFF;

  Vertex previous;
  bool have_previous = false;

  while (word < fifo_count_) {
    if (gouraud && have_previous)
      colour = fifo_[word++] & 0xFFFFFF;
    if (word >= fifo_count_)
      break;

    const uint32_t position = fifo_[word++];
    Vertex current;
    current.x = SignExtend11(position & 0x7FF) + draw_offset_x_;
    current.y = SignExtend11((position >> 16) & 0x7FF) + draw_offset_y_;
    current.r = static_cast<uint8_t>(colour);
    current.g = static_cast<uint8_t>(colour >> 8);
    current.b = static_cast<uint8_t>(colour >> 16);
    current.u = current.v = 0;

    if (have_previous)
      DrawLineSegment(previous, current, state);
    previous = current;
    have_previous = true;
  }
}

void Gpu::CmdRectangle() {
  const uint32_t command = fifo_[0] >> 24;
  const uint32_t size     = (command >> 3) & 3;
  const bool textured     = (command & 0x04) != 0;
  const bool semi         = (command & 0x02) != 0;
  const bool raw          = (command & 0x01) != 0;

  DrawState state;
  state.textured = textured;
  state.raw_texture = raw;
  state.semi_transparent = semi;
  state.gouraud = false;
  state.clut_x = state.clut_y = 0;
  state.texpage_x = status_.texpage_x * 64;
  state.texpage_y = status_.texpage_y * 256;
  state.texpage_colors = status_.texpage_colors;
  state.semi_mode = status_.semi_mode;
  // Rectangles are never dithered on hardware.
  state.dither = false;
  state.flip_x = rect_flip_x_;
  state.flip_y = rect_flip_y_;
  // A rectangle has no texpage word of its own, so texture disable comes from
  // the persistent draw mode.
  if (status_.texture_disable)
    state.textured = false;

  int word = 0;
  const uint32_t colour = fifo_[word++] & 0xFFFFFF;
  const uint32_t position = fifo_[word++];
  const int32_t x = SignExtend11(position & 0x7FF) + draw_offset_x_;
  const int32_t y = SignExtend11((position >> 16) & 0x7FF) + draw_offset_y_;

  uint8_t base_u = 0, base_v = 0;
  if (textured) {
    const uint32_t coord = fifo_[word++];
    base_u = static_cast<uint8_t>(coord);
    base_v = static_cast<uint8_t>(coord >> 8);
    const uint32_t clut = (coord >> 16) & 0xFFFF;
    state.clut_x = (clut & 0x3F) * 16;
    state.clut_y = (clut >> 6) & 0x1FF;
  }

  int32_t w = 0, h = 0;
  switch (size) {
    case 0: {
      const uint32_t extent = fifo_[word++];
      w = extent & 0x3FF;
      h = (extent >> 16) & 0x1FF;
      break;
    }
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
  }

  const uint8_t r = static_cast<uint8_t>(colour);
  const uint8_t g = static_cast<uint8_t>(colour >> 8);
  const uint8_t b = static_cast<uint8_t>(colour >> 16);

  RecordSetup(command, state, 0, 0);

  for (int32_t row = 0; row < h; ++row) {
    for (int32_t col = 0; col < w; ++col) {
      if (!textured) {
        PlotPixel(x + col, y + row, r, g, b, state, false, false);
        continue;
      }
      // A flipped rectangle walks its texture backwards from the base.
      const int32_t tu = state.flip_x ? (base_u - col) : (base_u + col);
      const int32_t tv = state.flip_y ? (base_v - row) : (base_v + row);
      const uint16_t texel = SampleTexture(
          static_cast<uint8_t>(tu), static_cast<uint8_t>(tv), state);
      if (texel == 0) {  // fully transparent texel
        ++stats_.transparent_texels;
        continue;
      }
      uint8_t tr = From5Bit(texel & 0x1F);
      uint8_t tg = From5Bit((texel >> 5) & 0x1F);
      uint8_t tb = From5Bit((texel >> 10) & 0x1F);
      if (!raw) {
        tr = Clamp8((tr * r) >> 7);
        tg = Clamp8((tg * g) >> 7);
        tb = Clamp8((tb * b) >> 7);
      }
      PlotPixel(x + col, y + row, tr, tg, tb, state, true,
                (texel & 0x8000) != 0);
    }
  }
}

// ---------------------------------------------------------------------------
// Rasterisation
// ---------------------------------------------------------------------------

// Captures the first few textured primitive setups for the harnesses.
void Gpu::RecordSetup(uint32_t command, const DrawState& state,
                      uint32_t raw_page, uint32_t raw_clut) {
  // Only 15-bit direct-colour draws for now: those are the ones under
  // suspicion, and the 4-bit ones flood the log before they appear.
  if (!state.textured || state.texpage_colors != 2 ||
      stats_.setup_count >= Stats::kSetupCapacity)
    return;
  Stats::TexturedSetup& setup = stats_.setups[stats_.setup_count++];
  setup.command = static_cast<uint8_t>(command);
  setup.colors = static_cast<uint8_t>(state.texpage_colors);
  setup.semi_mode = static_cast<uint8_t>(state.semi_mode);
  setup.flags = static_cast<uint8_t>((state.raw_texture ? 1 : 0) |
                                     (state.semi_transparent ? 2 : 0));
  setup.texpage_x = static_cast<uint16_t>(state.texpage_x);
  setup.texpage_y = static_cast<uint16_t>(state.texpage_y);
  setup.clut_x = static_cast<uint16_t>(state.clut_x);
  setup.clut_y = static_cast<uint16_t>(state.clut_y);
  setup.raw_page = static_cast<uint16_t>(raw_page);
  setup.raw_clut = static_cast<uint16_t>(raw_clut);
}

uint16_t Gpu::SampleTexture(uint32_t u, uint32_t v, const DrawState& state) {
  ++stats_.texels_by_depth[state.texpage_colors & 3];

  // The texture window folds the coordinates before they index the page.
  u = (u & ~(texture_window_mask_x_ * 8)) |
      ((texture_window_offset_x_ & texture_window_mask_x_) * 8);
  v = (v & ~(texture_window_mask_y_ * 8)) |
      ((texture_window_offset_y_ & texture_window_mask_y_) * 8);
  u &= 0xFF;
  v &= 0xFF;

  switch (state.texpage_colors) {
    case 0: {  // 4 bits per texel, via CLUT
      const uint16_t block = VramAt(state.texpage_x + (u / 4), state.texpage_y + v);
      const uint32_t index = (block >> ((u & 3) * 4)) & 0x0F;
      return VramAt(state.clut_x + index, state.clut_y);
    }
    case 1: {  // 8 bits per texel, via CLUT
      const uint16_t block = VramAt(state.texpage_x + (u / 2), state.texpage_y + v);
      const uint32_t index = (block >> ((u & 1) * 8)) & 0xFF;
      return VramAt(state.clut_x + index, state.clut_y);
    }
    default:   // 15 bits per texel, direct
      return VramAt(state.texpage_x + u, state.texpage_y + v);
  }
}

void Gpu::BlendSemiTransparent(uint16_t* dst, uint8_t r, uint8_t g, uint8_t b,
                               uint32_t mode) const {
  const uint16_t back = *dst;
  const int32_t br = From5Bit(back & 0x1F);
  const int32_t bg = From5Bit((back >> 5) & 0x1F);
  const int32_t bb = From5Bit((back >> 10) & 0x1F);

  int32_t nr, ng, nb;
  switch (mode) {
    case 0:  // B/2 + F/2
      nr = (br + r) / 2; ng = (bg + g) / 2; nb = (bb + b) / 2;
      break;
    case 1:  // B + F
      nr = br + r; ng = bg + g; nb = bb + b;
      break;
    case 2:  // B - F
      nr = br - r; ng = bg - g; nb = bb - b;
      break;
    default: // B + F/4
      nr = br + r / 4; ng = bg + g / 4; nb = bb + b / 4;
      break;
  }
  *dst = To15Bit(Clamp8(nr), Clamp8(ng), Clamp8(nb)) | (back & 0x8000);
}

void Gpu::PlotPixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b,
                    const DrawState& state, bool from_texture,
                    bool texture_mask) {
  if (x < draw_area_left_ || x > draw_area_right_ ||
      y < draw_area_top_ || y > draw_area_bottom_) {
    ++stats_.clipped;
    return;
  }

  uint16_t& target = VramAt(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
  if (check_mask_ && (target & 0x8000)) {
    ++stats_.mask_rejected;
    return;
  }

  // A textured pixel is only blended when its own mask bit says so; an
  // untextured one follows the primitive's semi-transparency flag.
  const bool blend = state.semi_transparent &&
                     (!from_texture || texture_mask);

  if (blend) {
    BlendSemiTransparent(&target, r, g, b, state.semi_mode);
  } else {
    target = To15Bit(r, g, b);
  }

  if (force_set_mask_)
    target |= 0x8000;

  ++stats_.pixels;

  NoteWatchWrite(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
}

void Gpu::RasterTriangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                         const DrawState& state) {
  // Hardware rejects any primitive spanning more than 1023x511.
  const int32_t min_x = std::min(v0.x, std::min(v1.x, v2.x));
  const int32_t max_x = std::max(v0.x, std::max(v1.x, v2.x));
  const int32_t min_y = std::min(v0.y, std::min(v1.y, v2.y));
  const int32_t max_y = std::max(v0.y, std::max(v1.y, v2.y));
  if (max_x - min_x >= 1024 || max_y - min_y >= 512)
    return;

  const int32_t left   = std::max(min_x, draw_area_left_);
  const int32_t right  = std::min(max_x, draw_area_right_);
  const int32_t top    = std::max(min_y, draw_area_top_);
  const int32_t bottom = std::min(max_y, draw_area_bottom_);
  if (left > right || top > bottom)
    return;

  const int32_t area = (v1.x - v0.x) * (v2.y - v0.y) -
                       (v2.x - v0.x) * (v1.y - v0.y);
  if (area == 0)
    return;

  // Work in a consistent winding so the edge functions share a sign test.
  const Vertex& a = v0;
  const Vertex& b = (area > 0) ? v1 : v2;
  const Vertex& c = (area > 0) ? v2 : v1;
  const int32_t double_area = (area > 0) ? area : -area;

  for (int32_t y = top; y <= bottom; ++y) {
    for (int32_t x = left; x <= right; ++x) {
      const int32_t w0 = (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
      const int32_t w1 = (c.x - b.x) * (y - b.y) - (c.y - b.y) * (x - b.x);
      const int32_t w2 = (a.x - c.x) * (y - c.y) - (a.y - c.y) * (x - c.x);
      if (w0 < 0 || w1 < 0 || w2 < 0)
        continue;

      // Barycentric weights: w1 belongs to a, w2 to b, w0 to c.
      uint8_t r, g, bl;
      if (state.gouraud) {
        r  = Clamp8((w1 * a.r + w2 * b.r + w0 * c.r) / double_area);
        g  = Clamp8((w1 * a.g + w2 * b.g + w0 * c.g) / double_area);
        bl = Clamp8((w1 * a.b + w2 * b.b + w0 * c.b) / double_area);
      } else {
        r = a.r; g = a.g; bl = a.b;
      }

      if (state.dither) {
        const int8_t offset = kDitherTable[y & 3][x & 3];
        r  = Clamp8(r + offset);
        g  = Clamp8(g + offset);
        bl = Clamp8(bl + offset);
      }

      if (!state.textured) {
        PlotPixel(x, y, r, g, bl, state, false, false);
        continue;
      }

      const int32_t u = (w1 * a.u + w2 * b.u + w0 * c.u) / double_area;
      const int32_t v = (w1 * a.v + w2 * b.v + w0 * c.v) / double_area;
      const uint16_t texel = SampleTexture(static_cast<uint32_t>(u),
                                           static_cast<uint32_t>(v), state);
      if (texel == 0) {  // fully transparent texel
        ++stats_.transparent_texels;
        continue;
      }

      uint8_t tr = From5Bit(texel & 0x1F);
      uint8_t tg = From5Bit((texel >> 5) & 0x1F);
      uint8_t tb = From5Bit((texel >> 10) & 0x1F);
      if (!state.raw_texture) {
        tr = Clamp8((tr * r) >> 7);
        tg = Clamp8((tg * g) >> 7);
        tb = Clamp8((tb * bl) >> 7);
      }
      PlotPixel(x, y, tr, tg, tb, state, true, (texel & 0x8000) != 0);
    }
  }
}

void Gpu::DrawLineSegment(const Vertex& v0, const Vertex& v1,
                          const DrawState& state) {
  int32_t x = v0.x;
  int32_t y = v0.y;
  const int32_t dx = std::abs(v1.x - v0.x);
  const int32_t dy = -std::abs(v1.y - v0.y);
  const int32_t step_x = (v0.x < v1.x) ? 1 : -1;
  const int32_t step_y = (v0.y < v1.y) ? 1 : -1;
  int32_t error = dx + dy;
  const int32_t steps = std::max(dx, -dy);

  for (int32_t i = 0; ; ++i) {
    uint8_t r = v0.r, g = v0.g, b = v0.b;
    if (state.gouraud && steps > 0) {
      r = Clamp8(v0.r + ((v1.r - v0.r) * i) / steps);
      g = Clamp8(v0.g + ((v1.g - v0.g) * i) / steps);
      b = Clamp8(v0.b + ((v1.b - v0.b) * i) / steps);
    }
    if (state.dither) {
      const int8_t offset = kDitherTable[y & 3][x & 3];
      r = Clamp8(r + offset);
      g = Clamp8(g + offset);
      b = Clamp8(b + offset);
    }
    PlotPixel(x, y, r, g, b, state, false, false);

    if (x == v1.x && y == v1.y)
      break;
    const int32_t error2 = 2 * error;
    if (error2 >= dy) { error += dy; x += step_x; }
    if (error2 <= dx) { error += dx; y += step_y; }
  }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void Gpu::UpdateDisplaySize() {
  // Horizontal resolution comes from two separate fields: hres2 overrides
  // hres1 when set.
  if (status_.hres2) {
    display_width_ = 368;
  } else {
    static const int kWidths[4] = { 256, 320, 512, 640 };
    display_width_ = kWidths[status_.hres1 & 3];
  }

  int lines = static_cast<int>(vertical_display_end_) -
              static_cast<int>(vertical_display_start_);
  if (lines <= 0)
    lines = 240;
  if (status_.vres && status_.vertical_interlace)
    lines *= 2;
  if (lines > kVramHeight)
    lines = kVramHeight;
  display_height_ = lines;
}

void Gpu::ResolveFramebuffer() {
  if (status_.display_disable) {
    memset(framebuffer_, 0,
           sizeof(uint32_t) * display_width_ * display_height_);
    return;
  }

  for (int y = 0; y < display_height_; ++y) {
    uint32_t* row = framebuffer_ + y * display_width_;
    const uint32_t vram_y = display_vram_y_ + y;

    if (!status_.display_depth) {
      // 15 bit: one VRAM halfword per pixel.
      for (int x = 0; x < display_width_; ++x) {
        const uint16_t pixel = VramAt(display_vram_x_ + x, vram_y);
        row[x] = 0xFF000000u |
                 (From5Bit(pixel & 0x1F) << 16) |
                 (From5Bit((pixel >> 5) & 0x1F) << 8) |
                 From5Bit((pixel >> 10) & 0x1F);
      }
    } else {
      // 24 bit: three bytes per pixel, so two pixels span three halfwords.
      for (int x = 0; x < display_width_; ++x) {
        const uint32_t byte_offset = x * 3;
        const uint16_t w0 = VramAt(display_vram_x_ + (byte_offset / 2), vram_y);
        const uint16_t w1 = VramAt(display_vram_x_ + (byte_offset / 2) + 1, vram_y);
        uint8_t r, g, b;
        if ((byte_offset & 1) == 0) {
          r = static_cast<uint8_t>(w0);
          g = static_cast<uint8_t>(w0 >> 8);
          b = static_cast<uint8_t>(w1);
        } else {
          r = static_cast<uint8_t>(w0 >> 8);
          g = static_cast<uint8_t>(w1);
          b = static_cast<uint8_t>(w1 >> 8);
        }
        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
    }
  }
}

bool Gpu::Tick(uint32_t cycles) {
  dot_accumulator_ += cycles * kGpuClockNumerator;
  const uint32_t dots = dot_accumulator_ / kGpuClockDenominator;
  dot_accumulator_ -= dots * kGpuClockDenominator;

  // Counter 0 counts dot clocks, which are GPU clocks divided down by the
  // horizontal resolution - narrower modes spend more GPU clocks per pixel.
  // The remainder is carried, so a resolution change mid-line loses nothing.
  //
  // `dots` cannot be used for this. What the loop below leaves in
  // dot_accumulator_ is the beam position within the scanline, not a
  // fractional remainder, so `dots` is the position plus this call's elapsed
  // clocks - counting it as a delta counts most of the line again on every
  // single call. Hence a remainder of its own.
  dot_clock_remainder_ += cycles * kGpuClockNumerator;
  const uint32_t gpu_clocks = dot_clock_remainder_ / kGpuClockDenominator;
  dot_clock_remainder_ %= kGpuClockDenominator;

  dot_clock_accum_ += gpu_clocks;
  const uint32_t divider = dot_clock_divider();
  pending_dot_clocks_ += dot_clock_accum_ / divider;
  dot_clock_accum_ %= divider;

  const uint32_t total_lines =
      status_.video_mode ? kScanlinesPal : kScanlinesNtsc;

  bool frame_completed = false;
  uint32_t remaining = dots;
  while (remaining >= kDotsPerScanline) {
    remaining -= kDotsPerScanline;
    ++scanline_;
    // Exactly one hblank per scanline. Counting them off completed lines
    // rather than off the gate below means the count is exact even when a
    // batch spans several lines; only the instant within the line they are
    // attributed to is approximate, and no counter can observe that.
    ++pending_hblanks_;

    if (scanline_ >= total_lines) {
      scanline_ = 0;
      frame_completed = true;
      // Bit 31 means two different things. With vertical interlace on it is
      // the field being drawn, and flips once per frame; with it off it is
      // simply the parity of the current scanline. Flipping it per scanline
      // in interlace mode left software that waits for a particular field
      // spinning on GPUSTAT forever.
      if (status_.vertical_interlace)
        status_.odd_line ^= 1;
    }

    if (!status_.vertical_interlace)
      status_.odd_line = scanline_ & 1;

    const bool now_in_vblank = scanline_ >= vertical_display_end_;
    if (now_in_vblank && !was_in_vblank_) {
      system().io().SetInterrupt(kInterruptVSYNC);
      ResolveFramebuffer();
      ++frame_count_;
    }
    was_in_vblank_ = now_in_vblank;
  }
  dot_accumulator_ += remaining * kGpuClockDenominator;
  return frame_completed;
}

}
}

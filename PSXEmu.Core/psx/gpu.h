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

namespace emulation {
namespace psx {

/*
  Software GPU.

  Owns the 1 MB of VRAM, executes the GP0 (drawing) and GP1 (display control)
  command streams, and resolves the visible part of VRAM into a 32-bit
  framebuffer a front end can present. It has no graphics API dependency, which
  is what lets the headless harnesses render and checksum frames.
*/
class Gpu : public GpuCore {
 public:
  Gpu();
  ~Gpu();

  int Initialize();
  int Deinitialize();

  uint32_t ReadData();
  uint32_t ReadStatus();
  void WriteData(uint32_t data);
  void WriteStatus(uint32_t data);

  bool Tick(uint32_t cycles);

  const uint16_t* vram() const { return vram_; }
  const uint32_t* framebuffer(int& width, int& height) const {
    width = display_width_;
    height = display_height_;
    return framebuffer_;
  }

  // Watches a VRAM rectangle and records which GP0 command wrote each pixel
  // into it. "What is this region and who made it" is otherwise a question
  // only answerable by staring at a dump.
  void WatchVram(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    watch_x_ = x; watch_y_ = y; watch_w_ = w; watch_h_ = h;
  }

  // Incremented once per completed frame; a cheap way for a harness to wait
  // for a specific frame without knowing anything about timing.
  uint64_t frame_count() const { return frame_count_; }

  // Command and drawing tallies. A boot that draws nothing looks identical to
  // a boot that draws the wrong thing from the framebuffer alone; these say
  // which of the two it is.
  struct Stats {
    uint64_t gp0_words;
    uint64_t gp1_words;
    uint64_t primitives;
    uint64_t pixels;
    // How many times each GP0 and GP1 command byte was executed. A primitive
    // that is never issued and one that is issued and drawn wrongly look the
    // same on screen; this separates them.
    uint32_t gp0_commands[256];
    uint32_t gp1_commands[64];
    // Which GP0 command wrote pixels into the watched rectangle, and how many.
    uint32_t watch_writers[256];
    uint64_t watch_writes;

    // The first few CPU-to-VRAM transfers: where they landed, how big they
    // were, and how many pixels actually arrived. A transfer that is set up
    // correctly but runs short leaves holes that look like a drawing bug.
    struct Transfer {
      uint16_t x, y, w, h;
      uint32_t written;
    };
    static const int kTransferCapacity = 24;
    Transfer transfers[kTransferCapacity];
    uint32_t transfer_log_count;
    // Pixels rejected by each of the reasons PlotPixel can reject one.
    uint64_t clipped;
    uint64_t mask_rejected;
    uint64_t transparent_texels;
    // Texels sampled at each colour depth: 4-bit CLUT, 8-bit CLUT, 15-bit
    // direct. A texture sampled at the wrong depth is the difference between
    // a picture and coloured noise.
    uint64_t texels_by_depth[4];

    // The setup of the first few textured primitives. A primitive that draws
    // noise and one that draws a picture differ only in these fields, and they
    // are not visible from anywhere else.
    struct TexturedSetup {
      uint8_t command;
      uint8_t colors;        // 0 = 4-bit CLUT, 1 = 8-bit CLUT, 2 = 15-bit
      uint8_t semi_mode;
      uint8_t flags;         // bit 0 raw, bit 1 semi-transparent, bit 2 disabled
      uint16_t texpage_x, texpage_y;
      uint16_t clut_x, clut_y;
      uint16_t raw_page;    // the texpage attribute word, undecoded
      uint16_t raw_clut;    // the clut attribute word, undecoded
    };
    static const int kSetupCapacity = 32;
    TexturedSetup setups[kSetupCapacity];
    uint32_t setup_count;
  };
  const Stats& stats() const { return stats_; }

  // Total scanlines and dot clocks per line for the current video mode. The
  // root counters need these to stay in step with the display.
  uint32_t scanline() const { return scanline_; }
  bool in_vblank() const { return scanline_ >= vertical_display_end_; }
  // Where in VRAM the display window sits, and whether it is switched on at
  // all. A game that draws into VRAM and shows black is usually one of these.
  uint32_t display_vram_x() const { return display_vram_x_; }
  uint32_t display_vram_y() const { return display_vram_y_; }
  bool display_disabled() const { return status_.display_disable != 0; }

 private:
  // ---- state -------------------------------------------------------------
  uint16_t* vram_;
  uint32_t* framebuffer_;

  union GpuStatus {
    struct {
      uint32_t texpage_x     : 4;   // 0-3   in units of 64 pixels
      uint32_t texpage_y     : 1;   // 4     in units of 256 lines
      uint32_t semi_mode     : 2;   // 5-6
      uint32_t texpage_colors: 2;   // 7-8   0=4bit 1=8bit 2=15bit
      uint32_t dither        : 1;   // 9
      uint32_t draw_to_display:1;   // 10
      uint32_t set_mask       :1;   // 11
      uint32_t check_mask     :1;   // 12
      uint32_t interlace_field:1;   // 13
      uint32_t reverse        :1;   // 14
      uint32_t texture_disable:1;   // 15
      uint32_t hres2          :1;   // 16
      uint32_t hres1          :2;   // 17-18
      uint32_t vres           :1;   // 19
      uint32_t video_mode     :1;   // 20    0=NTSC 1=PAL
      uint32_t display_depth  :1;   // 21    0=15bit 1=24bit
      uint32_t vertical_interlace:1;// 22
      uint32_t display_disable:1;   // 23
      uint32_t irq            :1;   // 24
      uint32_t dma_request    :1;   // 25
      uint32_t ready_cmd      :1;   // 26
      uint32_t ready_vram_send:1;   // 27
      uint32_t ready_dma      :1;   // 28
      uint32_t dma_direction  :2;   // 29-30
      uint32_t odd_line       :1;   // 31
    };
    uint32_t raw;
  } status_;

  // GP0 command assembly. A command is buffered until every word it needs has
  // arrived, then executed in one go.
  uint32_t fifo_[16];
  int fifo_count_;
  int fifo_needed_;

  // CPU <-> VRAM transfer state. A transfer runs for as many words as the
  // rectangle needs, with GP0 writes feeding it rather than starting commands.
  enum TransferMode { kTransferNone, kTransferToVram, kTransferFromVram };
  TransferMode transfer_mode_;
  struct {
    uint32_t x, y, w, h;   // in pixels, already masked to VRAM
    uint32_t px, py;       // cursor within the rectangle
  } transfer_;
  uint32_t read_latch_;

  // The command currently executing, so a pixel write can be attributed.
  uint32_t current_command_;
  uint32_t watch_x_, watch_y_, watch_w_, watch_h_;

  // Drawing state.
  int32_t draw_area_left_, draw_area_top_, draw_area_right_, draw_area_bottom_;
  int32_t draw_offset_x_, draw_offset_y_;
  uint32_t texture_window_mask_x_, texture_window_mask_y_;
  uint32_t texture_window_offset_x_, texture_window_offset_y_;
  bool force_set_mask_, check_mask_;
  // GP0(E1) bits 12-13: a textured rectangle can be mirrored in either axis.
  bool rect_flip_x_, rect_flip_y_;

  // Display state.
  uint32_t display_vram_x_, display_vram_y_;
  uint32_t horizontal_display_start_, horizontal_display_end_;
  uint32_t vertical_display_start_, vertical_display_end_;
  int display_width_, display_height_;

  // Timing.
  uint32_t dot_accumulator_;
  uint32_t scanline_;
  bool was_in_vblank_;
  uint64_t frame_count_;
  Stats stats_;

  // ---- command handling --------------------------------------------------
  static int CommandLength(uint32_t command);
  void ExecuteCommand();
  void ExecuteGp1(uint32_t data);
  void StepTransfer(uint32_t data);

  void CmdFillRectangle();
  void CmdPolygon();
  void CmdLine();
  void CmdRectangle();
  void CmdVramToVramCopy();
  void CmdCpuToVram();
  void CmdVramToCpu();

  // ---- rasterisation -----------------------------------------------------
  struct Vertex {
    int32_t x, y;      // already offset, in VRAM space
    uint8_t r, g, b;
    uint8_t u, v;
  };

  struct DrawState {
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

  void RecordSetup(uint32_t command, const DrawState& state,
                   uint32_t raw_page, uint32_t raw_clut);

  void RasterTriangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
                      const DrawState& state);
  void DrawLineSegment(const Vertex& v0, const Vertex& v1,
                       const DrawState& state);
  void PlotPixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b,
                 const DrawState& state, bool from_texture, bool texture_mask);
  uint16_t SampleTexture(uint32_t u, uint32_t v, const DrawState& state);
  void BlendSemiTransparent(uint16_t* dst, uint8_t r, uint8_t g, uint8_t b,
                            uint32_t mode) const;

  void ResolveFramebuffer();
  void UpdateDisplaySize();

  // Records a write into the watched rectangle against the command doing it.
  inline void NoteWatchWrite(uint32_t x, uint32_t y) {
    if (watch_w_ == 0)
      return;
    if ((x - watch_x_) < watch_w_ && (y - watch_y_) < watch_h_) {
      ++stats_.watch_writers[current_command_ & 0xFF];
      ++stats_.watch_writes;
    }
  }

  inline uint16_t& VramAt(uint32_t x, uint32_t y) {
    return vram_[((y & (kVramHeight - 1)) * kVramWidth) + (x & (kVramWidth - 1))];
  }
  inline uint16_t VramAt(uint32_t x, uint32_t y) const {
    return vram_[((y & (kVramHeight - 1)) * kVramWidth) + (x & (kVramWidth - 1))];
  }
};

}
}

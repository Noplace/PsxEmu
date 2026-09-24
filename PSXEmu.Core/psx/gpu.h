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

#include <condition_variable>
#include <mutex>
#include <thread>

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

  // Both of these wait for the rasteriser: what it has been handed is part of
  // the picture, and answering before it lands would show a half-drawn frame.
  const uint16_t* vram() const { SyncRaster(); return vram_; }
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
    // GPU clocks charged for drawing, and how many of them the CPU actually
    // had to wait through - the second is zero unless something asked. See
    // Gpu::AddDrawTicks.
    uint64_t draw_ticks;
    uint64_t draw_ticks_waited;
    // The deepest the GP0 queue has been, and how many words were dropped
    // because it could not go deeper. The second should stay zero: it means
    // software wrote GP0 far past what the port said it could take.
    // Barriers that actually had to wait for the rasteriser, and how many jobs
    // it was handed. A threaded run whose waits approach its jobs is being
    // serialised by something reading VRAM back, and is not going to be faster
    // for it - see bug 91.
    uint64_t raster_jobs;
    uint64_t raster_waits;
    uint32_t queue_peak;
    uint64_t queue_overflows;
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
    // Pixels a draw left alone because their row was the field being shown
    // (bug 89). Zero unless a game is in 480i interlace.
    uint64_t field_skipped;
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
  const Stats& stats() const { SyncRaster(); return stats_; }

  // Total scanlines and dot clocks per line for the current video mode. The
  // root counters need these to stay in step with the display.
  uint32_t scanline() const { return scanline_; }
  bool in_vblank() const { return scanline_ >= vertical_display_end_; }
  // Where in VRAM the display window sits, and whether it is switched on at
  // all. A game that draws into VRAM and shows black is usually one of these.
  uint32_t display_vram_x() const { return display_vram_x_; }
  uint32_t display_vram_y() const { return display_vram_y_; }
  bool display_disabled() const { return status_.display_disable != 0; }
  // The CRTC's horizontal display window, in GPU clocks. GP1(08) says how
  // fast pixels leave the GPU; this says how many of them the beam actually
  // paints, which is what decides the visible width.
  uint32_t horizontal_display_start() const { return horizontal_display_start_; }
  uint32_t horizontal_display_end() const { return horizontal_display_end_; }
  uint32_t status_raw() const { return status_.raw; }

  // The GPU runs at 11/7 of the CPU clock, and a scanline is this many GPU
  // clocks wide. Declared here rather than in the .cpp because the display
  // timing accessors below are inline and need the ratio to place the beam
  // within a line.
  static const uint32_t kGpuClockNumerator   = 11;
  static const uint32_t kGpuClockDenominator = 7;
  static const uint32_t kDotsPerScanline     = 3413;

  // How many frames a second the emulated display is actually producing:
  // 33868800 * 11/7 GPU clocks, divided by a frame's worth of them. 59.29 Hz
  // in NTSC, 49.76 in PAL - neither of which is 60, and neither of which is
  // any host monitor's refresh rate.
  //
  // A front end that wants to run at the speed of the machine rather than at
  // the speed of the screen it is drawn on needs this. Vertical blanks are
  // what software paces itself on, so this is the machine's clock as far as
  // anything watching it is concerned.
  double refresh_hz() const {
    const double gpu_clock = 33868800.0 * kGpuClockNumerator /
                             kGpuClockDenominator;
    const double lines = status_.video_mode ? 314.0 : 263.0;
    return gpu_clock / (kDotsPerScanline * lines);
  }

  // ---- display timing, for the root counters -----------------------------
  //
  // Counter 0 counts dot clocks and is gated by hblank; counter 1 counts
  // hblanks and is gated by vblank. All of that is display timing, so it is
  // measured here rather than guessed at from CPU cycles by whoever asks.

  // Where the beam is horizontally, in GPU clocks into the current scanline.
  uint32_t dot_in_scanline() const {
    return dot_accumulator_ / kGpuClockDenominator;
  }
  // Whether the GP0 queue has room for more, which is what GPUSTAT bits 26 and
  // 28 report and what DMA channel 2 waits on before handing over its next
  // node. False means the rasteriser is behind and the port is full.
  bool ready_for_dma() const { return queue_size_ < kFifoDepth; }

  // Lets the rasteriser use time a transfer in progress has already spent.
  //
  // A DMA moving words into GP0 takes real cycles, and the GPU is drawing
  // through them - but nothing here advances the GPU until the machine next
  // ticks, which is once per 32-cycle batch. Without this a full port is only
  // emptied at those boundaries, and channel 2 crawls at 16 words a batch
  // however idle the GPU actually is: Silent Hill ran at a third speed on 2%
  // of the GPU's time. The cycles are remembered so Tick does not count them
  // twice (bug 87).
  void AdvanceDrawing(uint32_t cpu_cycles);

  // Outside the horizontal display window is hblank. Both ends come from
  // GP1(06), so a game that narrows its display widens its own hblank.
  bool in_hblank() const {
    const uint32_t dot = dot_in_scanline();
    return dot < horizontal_display_start_ || dot >= horizontal_display_end_;
  }
  // How many GPU clocks make one dot clock at the current resolution. The
  // visible pixel count times this is roughly one scanline either way, which
  // is the check that these are the right numbers.
  uint32_t dot_clock_divider() const {
    if (status_.hres2)
      return 7;                                  // 368 wide
    static const uint32_t kDividers[4] = { 10, 8, 5, 4 };   // 256/320/512/640
    return kDividers[status_.hres1 & 3];
  }
  // Dot clocks and hblanks that have gone by since the last call, and are
  // handed over rather than reported - each one must be counted exactly once
  // by the counter that consumes it.
  uint32_t TakeDotClocks() {
    const uint32_t taken = pending_dot_clocks_;
    pending_dot_clocks_ = 0;
    return taken;
  }
  uint32_t TakeHblanks() {
    const uint32_t taken = pending_hblanks_;
    pending_hblanks_ = 0;
    return taken;
  }

  // vram_ (Bytes) plus every register/FIFO/timing field below stats_.
  // framebuffer_ is deliberately not here - ResolveFramebuffer() rebuilds it
  // from vram_ after a load, so a state taken mid-frame never shows a torn
  // picture that then corrects itself (Docs/Save-States-Plan.md's own trap).
  void Serialise(StateIO& io);

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
  // Display timing handed to the root counters. Accumulated during Tick and
  // taken away by whoever consumes them, so nothing is counted twice.
  uint32_t dot_clock_remainder_;
  uint32_t dot_clock_accum_;
  uint32_t pending_dot_clocks_;
  uint32_t pending_hblanks_;
  uint32_t scanline_;
  bool was_in_vblank_;
  uint64_t frame_count_;
  // Mutable because the rasteriser's counters are merged into it at a barrier,
  // and a barrier can happen inside a const read of the stats.
  mutable Stats stats_;

  // How much drawing the GPU still owes, in its own 53.2 MHz clocks. Charged
  // per primitive (AddDrawTicks) and burnt down by Tick. A real GPU takes time
  // to rasterise, and software can see that: it is why GPUSTAT's ready bits
  // exist and why the DMA request line drops. See Docs/Gaps.md.
  int32_t pending_draw_ticks_;

  // ---- the GP0 queue -----------------------------------------------------
  // Words that have arrived but not been acted on yet. Hardware holds 16 of
  // them and stops accepting more until the rasteriser has caught up, which is
  // what `kFifoDepth` reports - but the store here is far deeper, because
  // nothing in this emulator can make a CPU write wait. A game that ignores
  // the ready bits and writes anyway would lose words if this were exactly 16;
  // real hardware would have stalled its CPU instead. So the depth is what
  // software is *told*, and the capacity is what is actually kept.
  static const int kFifoDepth = 16;
  static const int kQueueCapacity = 1024;
  uint32_t queue_[kQueueCapacity];
  int queue_head_, queue_size_;

  // GPU clocks already paid to the rasteriser out of time a DMA transfer was
  // spending anyway - see AdvanceDrawing - so that Tick does not pay for the
  // same cycles a second time.
  uint32_t prepaid_ticks_;

  void PushQueue(uint32_t word);
  uint32_t PopQueue();
  // Hands queued words to the command assembler for as long as the GPU is free
  // to take them. Called after every write and from Tick.
  void DrainQueue();
  // One word into the assembler below, executing the command once its last
  // word has arrived. This is what WriteData used to be.
  void FeedCommand(uint32_t word);

  // Whether the GPU is still rasterising. Everything that reports "busy"
  // derives from this rather than testing the counter directly.
  bool drawing() const { return pending_draw_ticks_ > 0; }
  void AddDrawTicks(int32_t ticks);

  // What one primitive costs, in GPU clocks. The shapes and the constants are
  // DuckStation's, which are community measurements rather than anything
  // Sony published - see Docs/Bugs-Found.md's entry for this work. Declared
  // below Vertex and DrawState, which they take.
  static int32_t PolygonSetupTicks(bool quad, bool shaded, bool textured);

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

  // Everything a draw needs that is not in its own words: the drawing area it
  // is clipped to, the texture window, the mask rules and which field is being
  // displayed. Snapshotted when the command is parsed rather than read as the
  // pixels go down, so the rasteriser can run behind the machine on the state
  // the command was issued under and not on state that has since moved on
  // (phase 7 of Docs/Threading-Plan.md).
  struct DrawEnv {
    int32_t area_left, area_top, area_right, area_bottom;
    uint32_t tw_mask_x, tw_mask_y, tw_offset_x, tw_offset_y;
    bool force_set_mask, check_mask;
    bool skip_field;
    uint32_t active_line_lsb;
  };

  // One piece of rasterising, parsed and costed but not yet drawn. The machine
  // thread produces these; the rasteriser consumes them. Fixed size on purpose:
  // a polyline becomes one job per segment and a quad two triangles, so nothing
  // here needs a side buffer.
  struct DrawJob {
    enum Kind { kTriangle, kLine, kRectangle, kFill, kVramCopy };
    Kind kind;
    DrawEnv env;
    DrawState state;
    Vertex v[3];                  // triangle: three, line: the first two
    int32_t x, y, w, h;           // rectangle, fill, copy destination
    int32_t src_x, src_y;         // copy source
    uint8_t r, g, b;              // rectangle colour
    uint8_t base_u, base_v;       // textured rectangle
    uint16_t fill_colour;
    uint8_t command;
  };


  // The environment the job being drawn was issued under. Owned by whoever is
  // rasterising; the members it shadows stay the machine thread's, for costing
  // and for GPUSTAT readback.
  DrawEnv raster_env_;

  // The counters the rasteriser owns. Kept apart from Stats so that no member of
  // it is ever written by two threads: these are added into stats_ at a barrier,
  // which is the only moment the machine thread can read them. Without this the
  // pixel counts drifted between an inline run and a threaded one - the picture
  // was right either way, but a counter that disagrees with itself is a counter
  // nobody can use to check anything.
  struct RasterCounters {
    uint64_t pixels, clipped, field_skipped, mask_rejected, transparent_texels;
    uint64_t texels_by_depth[4];
    uint64_t watch_writes;
    uint32_t watch_writers[256];
  };
  mutable RasterCounters raster_counters_;
  // Adds them into stats_ and clears them. The caller holds jobs_mutex_.
  void MergeRasterCounters() const;

  // Snapshots the state a draw will need. Machine thread.
  DrawEnv CaptureDrawEnv() const;
  // Hands one piece of rasterising over. Machine thread.
  void SubmitJob(const DrawJob& job);
  // Draws one job. Whichever thread is rasterising.
  void ApplyJob(const DrawJob& job);
  void RasterRectangle(const DrawJob& job);
  void RasterFill(const DrawJob& job);
  void RasterVramCopy(const DrawJob& job);

  // ---- the rasteriser's thread (phase 7) ---------------------------------
  // Jobs go into this ring and a thread of its own applies them, so the
  // machine can be running the next frame's CPU work while the last frame's
  // pixels are still going down. Nothing the machine can observe depends on
  // how far behind it is: every read of VRAM, of the stats it keeps, or of a
  // state to save waits for it first, which is what keeps a threaded run
  // byte-identical to an unthreaded one.
  //
  // The GPU's *timing* does not move. Draw ticks, the GP0 queue and GPUSTAT's
  // ready bits are all charged and answered on the machine thread, where they
  // were, because a game can see them and they have to stay deterministic.
  static const int kJobCapacity = 1024;
  DrawJob jobs_[kJobCapacity];
  int jobs_head_ = 0;
  int jobs_count_ = 0;
  bool raster_busy_ = false;
  bool raster_stop_ = false;
  bool threaded_ = false;
  uint8_t raster_command_ = 0;
  mutable std::mutex jobs_mutex_;
  mutable std::condition_variable jobs_added_;
  mutable std::condition_variable jobs_drained_;
  std::thread raster_thread_;

  void StartRasterThread();
  void StopRasterThread();
  void RasterLoop();
  // Waits until everything handed over has been drawn. Cheap when the
  // rasteriser is keeping up, and a no-op when it is not threaded at all.
  void SyncRaster() const;
  void SyncThreadWithConfig();

  // The per-pixel halves of the drawing cost - see PolygonSetupTicks above.
  // Whether hardware is putting down only the active field, which halves what
  // a primitive costs: 480 lines, vertical interlace on, and drawing to the
  // display area prohibited. DuckStation's SkipDrawingToActiveField, and the
  // same three bits.
  //
  // Our rasteriser does not skip those lines - it draws every one - so this
  // charges half for twice the work. That is deliberate: what a game can
  // observe is how long hardware would have taken, not how long we took.
  bool DrawsOneFieldOnly() const {
    return status_.vres && status_.vertical_interlace &&
           !status_.draw_to_display;
  }

  // Which VRAM row parity the beam is currently showing, which is the one a
  // draw skips. DuckStation's crtc_state.active_line_lsb: the display area's
  // row in VRAM plus the field being shown, and zero outside 480i.
  uint32_t ActiveLineLsb() const {
    if (!status_.vres || !status_.vertical_interlace)
      return 0;
    return (display_vram_y_ + status_.odd_line) & 1u;
  }

  // Whether a draw leaves this VRAM row alone. In 480i with drawing to the
  // display area prohibited, hardware puts down only the field that is not
  // being shown - which is what bug 88 already charges half for, and bug 89
  // makes true of the pixels as well.
  //
  // Primitives and fills skip; a CPU-to-VRAM transfer and a VRAM-to-VRAM copy
  // do not, which is also where DuckStation draws the line - neither of its
  // WriteVRAM or CopyVRAM paths is even told the field.
  // Answered from the environment the job carries, so a rasteriser running
  // behind the machine skips the field that was being displayed when the
  // command was issued, not whatever is on screen by the time it draws.
  bool SkipsVramRow(int32_t y) const {
    return raster_env_.skip_field &&
           (static_cast<uint32_t>(y) & 1u) == raster_env_.active_line_lsb;
  }

  // One coordinate held inside the drawing area, for the cost estimates below.
  int32_t ClampToDrawArea(int32_t v, bool horizontal) const {
    const int32_t lo = horizontal ? draw_area_left_ : draw_area_top_;
    const int32_t hi = horizontal ? draw_area_right_ : draw_area_bottom_;
    return (v < lo) ? lo : ((v > hi) ? hi : v);
  }
  int32_t TriangleDrawTicks(const Vertex& a, const Vertex& b, const Vertex& c,
                            const DrawState& state) const;
  int32_t RectangleDrawTicks(int32_t x, int32_t y, int32_t width, int32_t height,
                             const DrawState& state) const;

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
  //
  // Wrapped to VRAM first, exactly as VramAt does, because that is the cell
  // actually written: a fill, transfer or copy that runs off the right or
  // bottom edge comes back round, and passing the unwrapped coordinate here
  // reported it against a rectangle that does not exist. A write landing
  // somewhere unexpected is precisely what this is for, so the one class of
  // write most worth catching was the one it could not see.
  inline void NoteWatchWrite(uint32_t x, uint32_t y) {
    if (watch_w_ == 0)
      return;
    x &= (kVramWidth - 1);
    y &= (kVramHeight - 1);
    if ((x - watch_x_) < watch_w_ && (y - watch_y_) < watch_h_) {
      ++raster_counters_.watch_writers[raster_command_ & 0xFF];
      ++raster_counters_.watch_writes;
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

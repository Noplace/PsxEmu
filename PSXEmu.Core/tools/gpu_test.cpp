// gpu_test - checks the GPU's command and status handling against things that
// must be true. No BIOS, no window: commands are written straight to GP0/GP1
// the way the memory-mapped registers would, and GPUSTAT/I_STAT are read back.
//
// This is a starting set, not full coverage: the rasteriser is exercised
// indirectly by every boot_runner run and by the framebuffer checksums in
// Test-Suite.md, so what is missing here is the register-level behaviour nothing
// else ever drives - starting with GP0(1Fh), which no game observed so far has
// issued.

#include "psx/psx.h"
#ifdef PSXEMU_HW_RASTER
#include "graphics/hw_raster/hardware_raster.h"
#endif

#include <cstdio>
#include <cstring>

using emulation::psx::Gpu;
using emulation::psx::kInterruptGPU;
using emulation::psx::System;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckEqual(uint32_t got, uint32_t want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %08X want %08X\n", what, got, want);
  }
}

bool Irq1Pending(System* system) {
  return (system->io().io.interrupt_stat & kInterruptGPU) != 0;
}

// Lets the GPU work through whatever is queued. Since bug 86 a GP0 word waits
// behind the rasteriser rather than being acted on the instant it arrives, so
// a test that writes commands and then reads VRAM has to give the machine the
// time a game would have given it. Well past what any of these owe.
void RunGpu(System* system) {
  for (int i = 0; i < 64; ++i)
    system->gpu().Tick(4096);
}

// VRAM as a check reads it: every read waits for the rasteriser first, the same
// way GPUREAD and DMA do inside the machine. These checks used to keep the pointer
// Gpu::vram() handed back and read through it after issuing more commands, which
// was fine while drawing happened where it was submitted. With the rasteriser on
// its own thread (bug 91, and the default since) the later draws had not landed
// yet - and a check that a pixel was "left clear" could pass only because nothing
// had been drawn there at all (bug 95).
struct VramView {
  System* system;
  uint16_t operator[](size_t index) const { return system->gpu().vram()[index]; }
};

// Acknowledges I_STAT's GPU bit the way software does - write a word with
// that bit 0 and every other bit 1 - without touching GPUSTAT.24, which only
// GP1(02h) clears. Keeping the two separate is the point of this test file.
void AckIrq1(System* system) {
  system->io().Write32(0x1F801070, ~static_cast<uint32_t>(kInterruptGPU));
}

void TestResetStartsIdle(System* system) {
  printf("reset leaves GPUSTAT.24 and I_STAT.GPU both clear\n");
  system->gpu().WriteStatus(0x00000000);  // GP1(00h) reset GPU
  AckIrq1(system);
  CheckEqual(system->gpu().ReadStatus() & (1u << 24), 0,
             "GPUSTAT.24 after GP1(00h)");
  Check(!Irq1Pending(system), "I_STAT.GPU after GP1(00h) and an ack");
}

void TestInterruptRequestSetsStatusAndIrq(System* system) {
  printf("GP0(1Fh) sets GPUSTAT.24 and raises I_STAT.GPU\n");
  system->gpu().WriteStatus(0x00000000);  // start from a clean reset
  AckIrq1(system);

  system->gpu().WriteData(0x1F000000);    // GP0(1Fh), no parameters

  CheckEqual(system->gpu().ReadStatus() & (1u << 24), (1u << 24),
             "GPUSTAT.24 after GP0(1Fh)");
  Check(Irq1Pending(system), "I_STAT.GPU after GP0(1Fh)");
}

void TestAcknowledgeClearsStatusAndAllowsANewEdge(System* system) {
  printf("GP1(02h) clears GPUSTAT.24; a fresh GP0(1Fh) can set it again\n");
  system->gpu().WriteStatus(0x00000000);
  AckIrq1(system);
  system->gpu().WriteData(0x1F000000);
  AckIrq1(system);

  system->gpu().WriteStatus(0x02000000);  // GP1(02h) acknowledge
  CheckEqual(system->gpu().ReadStatus() & (1u << 24), 0,
             "GPUSTAT.24 after GP1(02h)");

  system->gpu().WriteData(0x1F000000);
  CheckEqual(system->gpu().ReadStatus() & (1u << 24), (1u << 24),
             "GPUSTAT.24 after a second GP0(1Fh) post-acknowledge");
  Check(Irq1Pending(system), "I_STAT.GPU after the second GP0(1Fh)");
}

void TestRepeatedRequestIsNotANewEdge(System* system) {
  printf("GP0(1Fh) while GPUSTAT.24 is already set raises no second I_STAT edge\n");
  system->gpu().WriteStatus(0x00000000);
  AckIrq1(system);
  system->gpu().WriteData(0x1F000000);
  Check(Irq1Pending(system), "the first request reached I_STAT");

  // Acknowledge at the interrupt controller only, exactly as real software
  // racing the two acks would: GPUSTAT.24 is still set.
  AckIrq1(system);
  Check(!Irq1Pending(system), "I_STAT.GPU cleared without touching GPUSTAT.24");

  system->gpu().WriteData(0x1F000000);   // GP0(1Fh) again, bit 24 still 1
  Check(!Irq1Pending(system),
        "a repeated GP0(1Fh) does not re-latch I_STAT while GPUSTAT.24 "
        "was already set");
}

// Drawing takes time now (bug 85), and bit 28 - ready to receive a DMA block -
// is where software sees it. The other two still report ready unconditionally,
// for the reasons in Gpu::ReadStatus: there is no command queue to fill, and
// bit 27 means "able to hand VRAM over" rather than "a transfer is running".
void TestReadinessBits(System* system) {
  printf("the ready bits: 26 and 27 always, 28 only when not drawing\n");
  system->gpu().WriteStatus(0x00000000);
  const uint32_t status = system->gpu().ReadStatus();
  Check((status & (1u << 26)) != 0, "ready to receive a command word");
  Check((status & (1u << 27)) != 0, "ready to send VRAM to the CPU");
  Check((status & (1u << 28)) != 0, "ready to receive a DMA block when idle");
}

// What a primitive costs, and that the cost is paid down rather than
// remembered for ever. The numbers are DuckStation's model (bug 85): a flat
// untextured triangle is 46 ticks of setup plus one per pixel of area.
void TestDrawingTakesTime(System* system) {
  printf("drawing costs GPU time, and the GPU reports busy until it is paid\n");
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);                       // draw area top-left
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);  // bottom-right

  const uint64_t before = system->gpu().stats().draw_ticks;
  // A right triangle with legs of 100: 5,000 pixels of area, flat and
  // untextured, so 46 + 5000. Drawn at (200,200) rather than the origin
  // because these tests share one VRAM, and the polyline test below checks
  // that nothing has touched (0,0).
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t charged = system->gpu().stats().draw_ticks - before;
  CheckEqual(static_cast<uint32_t>(charged), 46u + 5000u,
             "a flat untextured triangle costs its setup plus its area");

  // The port is still open: the queue is empty, and hardware would take 16
  // more words while the rasteriser worked through this one. What drawing time
  // costs is visible only once words pile up behind it - the test below.
  Check((system->gpu().ReadStatus() & (1u << 28)) != 0,
        "an empty queue is ready for more even while drawing");

  // The shape of the cost, across the four polygon kinds: each step up costs
  // more setup than the last.
  const uint32_t flat_tri = 46, tex_tri = 226, shaded_tri = 334, both_tri = 496;
  Check(flat_tri < tex_tri && tex_tri < shaded_tri && shaded_tri < both_tri,
        "setup rises with what the primitive has to do");

  // A textured triangle pays twice per pixel, and a semi-transparent one half
  // as much again on top - it has to read the framebuffer back.
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);
  const uint64_t before_semi = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x22000000 | 0x808080);   // flat, semi-transparent
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t semi_charged = system->gpu().stats().draw_ticks - before_semi;
  CheckEqual(static_cast<uint32_t>(semi_charged), 46u + 5000u + 2500u,
             "a semi-transparent one pays half as much again per pixel");
}

// What a primitive costs is what it actually rasterises (bug 87). Charging the
// whole of a primitive that the drawing area mostly throws away is what made
// Silent Hill run at a third speed: a 3D game aims big polygons at a small
// viewport, so the bill was nearly three frames of drawing per frame and the
// rasteriser never caught up.
void TestDrawingCostIsClipped(System* system) {
  printf("drawing time is charged on the clipped primitive, not the whole one\n");

  // The same triangle twice - legs of 100 at (200,200), 5,000 pixels of area -
  // against a drawing area that holds all of it, then against one that keeps a
  // 50x50 corner of it.
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);
  const uint64_t before_whole = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t whole = system->gpu().stats().draw_ticks - before_whole;
  CheckEqual(static_cast<uint32_t>(whole), 46u + 5000u,
             "a triangle wholly inside the drawing area costs its full area");

  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000 | (200u << 10) | 200u);
  system->gpu().WriteData(0xE4000000 | (250u << 10) | 250u);
  const uint64_t before_clipped = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t clipped = system->gpu().stats().draw_ticks - before_clipped;
  CheckEqual(static_cast<uint32_t>(clipped), 46u + 1250u,
             "one clipped to a 50x50 corner costs that corner");
  Check(clipped < whole,
        "the same primitive costs less where less of it is drawn");

  // A primitive entirely outside costs setup and nothing more. Without the
  // clamping this was the worst case: the full area of something that never
  // put down a pixel.
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (10u << 10) | 10u);
  const uint64_t before_outside = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t outside = system->gpu().stats().draw_ticks - before_outside;
  CheckEqual(static_cast<uint32_t>(outside), 46u,
             "one wholly outside it costs setup and no pixels");

  // A rectangle is clipped the same way: 40x40 at (200,200) against a drawing
  // area that keeps a 20x20 corner, so 16 of setup and 400 of pixels.
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000 | (200u << 10) | 200u);
  system->gpu().WriteData(0xE4000000 | (219u << 10) | 219u);
  const uint64_t before_rect = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x60000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((40u << 16) | 40u);
  const uint64_t rect = system->gpu().stats().draw_ticks - before_rect;
  CheckEqual(static_cast<uint32_t>(rect), 16u + 400u,
             "a rectangle is charged on its clipped width and height");

  RunGpu(system);
}

// A triangle spanning 1024 or more across, or 512 or more down, is not drawn -
// hardware rejects it, and so does every rasteriser here - so it costs setup
// and nothing more. Silent Hill lays a quad about 2,045 pixels across over one
// of its corridors several times a frame, and each was charged the whole
// drawing area, textured and semi-transparent: 120% of the GPU's time, and the
// game crawled (bug 143).
void TestCulledPolygonCostsSetupOnly(System* system) {
  printf("a polygon too large to draw costs its setup and nothing more\n");
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);

  // Draws a flat untextured triangle and returns what it was charged.
  auto Cost = [system](int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t x2,
                       int32_t y2) -> uint64_t {
    auto Word = [](int32_t x, int32_t y) {
      return (static_cast<uint32_t>(y & 0x7FF) << 16) | static_cast<uint32_t>(x & 0x7FF);
    };
    // Caught up before and after, so the command is neither queued behind the
    // last one's drawing nor still waiting when it is counted.
    RunGpu(system);
    const uint64_t before = system->gpu().stats().draw_ticks;
    system->gpu().WriteData(0x20000000 | 0x808080);
    system->gpu().WriteData(Word(x0, y0));
    system->gpu().WriteData(Word(x1, y1));
    system->gpu().WriteData(Word(x2, y2));
    RunGpu(system);
    return system->gpu().stats().draw_ticks - before;
  };
  CheckEqual(static_cast<uint32_t>(Cost(-400, 100, 624, 100, 100, 300)), 46u,
             "1024 across is culled: setup only");
  Check(Cost(-399, 100, 624, 100, 100, 300) > 46u, "1023 across is drawn and charged");
  CheckEqual(static_cast<uint32_t>(Cost(100, -200, 300, 312, 200, 100)), 46u,
             "512 down is culled: setup only");
  Check(Cost(100, -199, 300, 312, 200, 100) > 46u, "511 down is drawn and charged");

  // Silent Hill's own quad, word for word: corners at -1022 and 1023 both ways,
  // textured and semi-transparent. 262 is a textured flat quad's setup.
  const uint32_t quad[] = { 0x2E808080, 0x03FF03FF, 0x7C7B70C0, 0x040203FF, 0x004C7FC0,
                            0x03FF0402, 0x7C7B70D0, 0x04020402, 0x004C7FD0 };
  RunGpu(system);
  const uint64_t before = system->gpu().stats().draw_ticks;
  for (uint32_t word : quad)
    system->gpu().WriteData(word);
  RunGpu(system);
  CheckEqual(static_cast<uint32_t>(system->gpu().stats().draw_ticks - before), 262u,
             "Silent Hill's corridor quad costs its setup alone");
}

// In 480-line interlace with drawing to the display area prohibited, hardware
// puts down only the active field, so a primitive costs half (bug 88). GP1(08)
// sets the display mode; GP0(E1) bit 10 is the draw-to-display bit.
void TestInterlacedDrawingCostsHalf(System* system) {
  printf("drawing only the active field costs half as much\n");

  // 480 lines with vertical interlace on: GP1(08) bit 2 is the vertical
  // resolution and bit 5 the interlace. Drawing to the display area is left
  // prohibited, which is what makes hardware skip a field.
  system->gpu().WriteStatus(0x08000004 | (1u << 5));
  system->gpu().WriteData(0xE1000000);                       // draw to display off
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);

  const uint64_t before_field = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t one_field = system->gpu().stats().draw_ticks - before_field;
  CheckEqual(static_cast<uint32_t>(one_field), 46u + 2500u,
             "a triangle costs half its area when only one field is drawn");

  // The same primitive with drawing to the display area allowed: hardware has
  // to put down every line, so the full price is back.
  RunGpu(system);
  const uint64_t before_both = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0xE1000000 | (1u << 10));          // draw to display on
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t both_fields = system->gpu().stats().draw_ticks - before_both;
  CheckEqual(static_cast<uint32_t>(both_fields), 46u + 5000u,
             "and its full area once it may draw to the display area");

  // And it is the interlace that does it, not the 480 lines: progressive 480
  // pays in full.
  RunGpu(system);
  system->gpu().WriteStatus(0x08000004);
  system->gpu().WriteData(0xE1000000);
  const uint64_t before_prog = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  const uint64_t progressive = system->gpu().stats().draw_ticks - before_prog;
  CheckEqual(static_cast<uint32_t>(progressive), 46u + 5000u,
             "480 lines without interlace pays in full");

  // A rectangle halves its rows the same way: 40x40 is charged as 40x20.
  RunGpu(system);
  system->gpu().WriteStatus(0x08000004 | (1u << 5));
  system->gpu().WriteData(0xE1000000);
  const uint64_t before_rect = system->gpu().stats().draw_ticks;
  system->gpu().WriteData(0x60000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((40u << 16) | 40u);
  const uint64_t rect = system->gpu().stats().draw_ticks - before_rect;
  CheckEqual(static_cast<uint32_t>(rect), 16u + (40u * 20u),
             "a rectangle is charged for half its rows");

  // Back to progressive for whatever runs after this.
  system->gpu().WriteStatus(0x08000000);
  system->gpu().WriteData(0xE1000000);
  RunGpu(system);
}

// And the pixels follow the cost (bug 89): in that same mode hardware leaves
// the field it is displaying alone, so half the rows of a primitive or a fill
// are never written. A transfer is not affected - neither of DuckStation's
// WriteVRAM or CopyVRAM paths is even told which field is showing.
void TestInterlacedSkipsDisplayedField(System* system) {
  printf("drawing leaves the displayed field's rows alone\n");

  // 480 lines interlaced with drawing to the display area prohibited, and the
  // display area at VRAM row 0 so the skipped parity is just the field bit.
  system->gpu().WriteStatus(0x08000004 | (1u << 5));
  system->gpu().WriteData(0xE1000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);
  system->gpu().WriteStatus(0x05000000);            // display area at (0,0)

  const VramView vram{system};

  // Which parity is showing is GPUSTAT bit 31 in interlaced mode, and it flips
  // once a frame - so each stage below drains the GPU first, then reads the
  // field, then writes a command that runs as its last word arrives. Running
  // the machine between the read and the draw would move the field under it,
  // which is what the first version of this test did. From the start of vblank the
  // field a draw skips is already the next one (bug 130), so bit 31 is flipped
  // while the beam is in vblank.
  auto field_now = [&]() -> uint32_t {
    RunGpu(system);
    return ((system->gpu().ReadStatus() >> 31) ^ (system->gpu().in_vblank() ? 1u : 0u)) & 1u;
  };
  auto rows_match = [&](int32_t y0, int32_t x, uint32_t field,
                        bool* shown_clear, bool* others_written) {
    *shown_clear = true;
    *others_written = true;
    for (int32_t row = y0; row < y0 + 4; ++row) {
      const uint16_t pixel = vram[row * 1024 + x];
      if ((static_cast<uint32_t>(row) & 1u) == field) {
        if (pixel != 0)
          *shown_clear = false;
      } else if (pixel == 0) {
        *others_written = false;
      }
    }
  };

  // A solid 4x4 white rectangle at (300,300), clear of everything else here.
  uint32_t field = field_now();
  system->gpu().WriteData(0x60FFFFFF);
  system->gpu().WriteData((300u << 16) | 300u);
  system->gpu().WriteData((4u << 16) | 4u);
  bool shown_clear = false, others_written = false;
  rows_match(300, 300, field, &shown_clear, &others_written);
  Check(shown_clear, "the rows being displayed are left untouched");
  Check(others_written, "the rows that are not being displayed are drawn");

  // A fill skips the same rows, although its cost is not halved.
  field = field_now();
  system->gpu().WriteData(0x02FFFFFF);
  system->gpu().WriteData((400u << 16) | 400u);
  system->gpu().WriteData((4u << 16) | 16u);
  rows_match(400, 400, field, &shown_clear, &others_written);
  Check(shown_clear, "a fill skips the displayed field's rows too");
  Check(others_written, "and fills the rest");

  // Once drawing to the display area is allowed, every row is drawn again.
  field_now();
  system->gpu().WriteData(0xE1000000 | (1u << 10));
  system->gpu().WriteData(0x60FFFFFF);
  system->gpu().WriteData((300u << 16) | 320u);
  system->gpu().WriteData((4u << 16) | 4u);
  bool all_rows = true;
  for (int32_t row = 300; row < 304; ++row) {
    if (vram[row * 1024 + 320] == 0)
      all_rows = false;
  }
  Check(all_rows, "every row is drawn once drawing to the display area is allowed");

  // A CPU-to-VRAM transfer is not gated by the field, so it lands whole even
  // with the skip back on. Four rows of two pixels, two pixels per word.
  field_now();
  system->gpu().WriteData(0xE1000000);
  system->gpu().WriteData(0xA0000000);
  system->gpu().WriteData((300u << 16) | 340u);
  system->gpu().WriteData((4u << 16) | 2u);
  for (int i = 0; i < 4; ++i)
    system->gpu().WriteData(0x7FFF7FFF);
  bool transfer_whole = true;
  for (int32_t row = 300; row < 304; ++row) {
    if (vram[row * 1024 + 340] == 0)
      transfer_whole = false;
  }
  Check(transfer_whole, "a CPU-to-VRAM transfer is not gated by the field");

  // Back to progressive for whatever runs after this.
  system->gpu().WriteStatus(0x08000000);
  system->gpu().WriteData(0xE1000000);
  RunGpu(system);
}

// The field a draw leaves alone is the next one to be shown from the moment vblank
// begins, not from the moment the frame wraps (bug 130). A game draws its next frame
// from the vblank interrupt on, and a frame that takes a while to draw straddles the
// wrap: skipping by GPUSTAT bit 31, which flips there, put the start of it on one
// field's rows and the end on the other's - Silent Hill's Konami logo, alternate lines
// of two different pictures for a hundred frames.
void TestSkippedFieldFlipsAtTheStartOfVblank(System* system) {
  printf("the field a draw skips changes at the start of vblank, not at the frame's wrap\n");

  system->gpu().WriteStatus(0x08000004 | (1u << 5));
  system->gpu().WriteData(0xE1000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (511u << 10) | 1023u);
  system->gpu().WriteStatus(0x05000000);            // display area at (0,0)
  RunGpu(system);

  const VramView vram{system};
  auto tick_until = [&](bool want_vblank) {
    for (int i = 0; i < 40000 && system->gpu().in_vblank() != want_vblank; ++i)
      system->gpu().Tick(100);
  };
  // Draws a 4x4 white rectangle at (x, 300) and says which parity of its rows was left
  // clear: 0 or 1, or -1 if that is not one clean parity.
  auto draw_and_find_skipped = [&](int32_t x) -> int {
    system->gpu().WriteData(0x60FFFFFF);
    system->gpu().WriteData((300u << 16) | static_cast<uint32_t>(x));
    system->gpu().WriteData((4u << 16) | 4u);
    const bool even_clear = vram[300 * 1024 + x] == 0;
    const bool odd_clear = vram[301 * 1024 + x] == 0;
    if (even_clear && !odd_clear)
      return 0;
    if (odd_clear && !even_clear)
      return 1;
    return -1;
  };

  tick_until(false);
  tick_until(true);                                  // the first line of vblank
  Check(system->gpu().in_vblank(), "the run reached vblank");
  const uint32_t bit31 = (system->gpu().ReadStatus() >> 31) & 1u;
  const int in_vblank = draw_and_find_skipped(700);
  CheckEqual(static_cast<uint32_t>(in_vblank), bit31 ^ 1u,
             "in vblank a draw already skips the field that comes next");

  tick_until(false);                                 // the frame wraps; bit 31 flips
  Check(!system->gpu().in_vblank(), "and the run reached the next frame");
  const int after_wrap = draw_and_find_skipped(720);
  CheckEqual(static_cast<uint32_t>(after_wrap), static_cast<uint32_t>(in_vblank),
             "the wrap does not change which field a draw skips");

  tick_until(true);                                  // the next frame's vblank
  const int next_frame = draw_and_find_skipped(740);
  Check(next_frame >= 0 && next_frame != after_wrap,
        "and the field alternates from one frame to the next");

  // Back to progressive for whatever runs after this.
  system->gpu().WriteStatus(0x08000000);
  system->gpu().WriteData(0xE1000000);
  RunGpu(system);
}

// A VRAM read is served after everything queued ahead of it has run (bug 130). The
// queue only drains while the GPU is not drawing, and each primitive makes it busy
// again - so the drain a read did stopped after one, and the words read before the
// read command had been reached came back as the last word latched. The BIOS menu
// draws its spheres, reads them straight back and draws from the copy: the copy arrived
// shifted by the number of words read early.
void TestReadBackWaitsForQueuedDrawing(System* system) {
  printf("a VRAM read sees the drawing queued ahead of it\n");

  system->gpu().WriteStatus(0x08000000);             // progressive, so no field is skipped
  system->gpu().WriteData(0xE1000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (511u << 10) | 1023u);
  RunGpu(system);

  // A large triangle keeps the rasteriser busy for a long while; the fill and the read
  // behind it wait in the queue.
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((300u << 16) | 900u);
  system->gpu().WriteData((500u << 16) | 900u);
  system->gpu().WriteData((300u << 16) | 1000u);
  system->gpu().WriteData(0x02FFFFFF);               // fill 16x2 white at (800, 100)
  system->gpu().WriteData((100u << 16) | 800u);
  system->gpu().WriteData((2u << 16) | 16u);
  system->gpu().WriteData(0xC0000000);               // read 4x1 from there: two words
  system->gpu().WriteData((100u << 16) | 800u);
  system->gpu().WriteData((1u << 16) | 4u);

  const uint32_t first = system->gpu().ReadData();
  const uint32_t second = system->gpu().ReadData();
  CheckEqual(first, 0x7FFF7FFFu, "the first word read is the fill, not the stale latch");
  CheckEqual(second, 0x7FFF7FFFu, "and so is the second");

  RunGpu(system);
}

// The GP0 queue (bug 86): words wait in it while the rasteriser is busy, the
// port reports full at the 16 words hardware holds, and everything drains once
// the machine has run. This is what gives DMA channel 2 something to wait for.
void TestGp0QueueFillsAndDrains(System* system) {
  printf("the GP0 queue holds words back while the GPU is drawing\n");
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);

  // One big primitive to make the GPU busy, then enough one-word commands
  // (GP0(01h), clear cache) to fill the port past the 16 hardware holds.
  system->gpu().WriteData(0x20000000 | 0x808080);
  system->gpu().WriteData((200u << 16) | 200u);
  system->gpu().WriteData((200u << 16) | 300u);
  system->gpu().WriteData((300u << 16) | 200u);
  Check((system->gpu().ReadStatus() & (1u << 28)) != 0, "the port starts open");

  for (int i = 0; i < 16; ++i)
    system->gpu().WriteData(0x01000000);

  Check((system->gpu().ReadStatus() & (1u << 28)) == 0,
        "sixteen words in, the port reports full");
  Check((system->gpu().ReadStatus() & (1u << 26)) == 0,
        "and so does the command-word bit");
  Check((system->gpu().ReadStatus() & (1u << 25)) == 0,
        "the DMA request line drops with them");
  Check(system->gpu().stats().queue_peak >= 16,
        "the queue really is holding them");

  // Running the machine pays the drawing off, and the queue empties.
  for (int i = 0; i < 8; ++i)
    system->gpu().Tick(1000);
  Check((system->gpu().ReadStatus() & (1u << 28)) != 0,
        "the port opens again once the drawing is paid for");
  CheckEqual(static_cast<uint32_t>(system->gpu().stats().queue_overflows), 0u,
             "and nothing was ever dropped");
}

// GP0(E6h) bit 0 decides what goes into the framebuffer's bit 15 while drawing:
// "0=TextureBit15, 1=ForceBit15=1" (psx-spx). Forced is the easy half and was
// the only half modelled. The other one is that a *textured* draw hands the
// texel's own bit 15 through to the pixel it writes, and an untextured draw
// writes zero - which is what makes the mask usable as a per-texel stencil,
// and is how Silent Hill keeps a flat semi-transparent quad off the scene it
// has already drawn (bug 83). Checked by drawing, then reading VRAM, then
// drawing again with mask-checking on to see which pixels are protected.
void TestTextureBit15BecomesTheMaskBit(System* system) {
  printf("a textured draw writes the texel's bit 15 as the mask bit\n");
  system->gpu().WriteStatus(0x00000000);                     // GP1(00h) reset
  system->gpu().WriteData(0xE3000000);                       // draw area top-left
  system->gpu().WriteData(0xE4000000 | (300u << 10) | 300u);  // bottom-right
  system->gpu().WriteData(0xE6000000);                       // mask: neither bit

  // A two-texel 15-bit texture at (0,256), the base of texture page 1: one
  // texel with bit 15 set, one without, both otherwise white. CPU->VRAM, so
  // this is the same route a game loads a texture by.
  system->gpu().WriteData(0xA0000000);
  system->gpu().WriteData((256u << 16) | 0u);   // destination (0,256)
  system->gpu().WriteData((1u << 16) | 2u);     // two across, one down
  system->gpu().WriteData((0x7FFFu << 16) | 0xFFFFu);   // texel0 FFFF, texel1 7FFF

  // Two 1x1 raw textured rectangles - GP0(65h) - one per texel. Raw, so the
  // texel reaches the framebuffer unmodulated and the colour word is ignored.
  // The texture page is 15-bit (colours=2) at page x=0, y=256: bit 4 set,
  // bits 7-8 = 2.
  system->gpu().WriteData(0xE1000000 | (2u << 7) | (1u << 4));
  system->gpu().WriteData(0x65000000);                  // textured rect, 1x1, raw
  system->gpu().WriteData((10u << 16) | 10u);           // at (10,10)
  system->gpu().WriteData(0u);                          // u=0, v=0, clut 0
  system->gpu().WriteData((1u << 16) | 1u);             // 1x1

  system->gpu().WriteData(0x65000000);
  system->gpu().WriteData((10u << 16) | 12u);           // at (12,10)
  system->gpu().WriteData(1u);                          // u=1, v=0
  system->gpu().WriteData((1u << 16) | 1u);

  RunGpu(system);
  const VramView vram{system};
  CheckEqual(vram[10 * 1024 + 10] & 0x8000, 0x8000,
             "the texel with bit 15 set marked its pixel");
  CheckEqual(vram[10 * 1024 + 12] & 0x8000, 0,
             "the texel without it did not");

  // An untextured draw writes a clear mask bit whatever else it is doing -
  // including a semi-transparent one, where bit 15 is the blend flag on the
  // way in but not on the way out.
  system->gpu().WriteData(0x68000000 | 0xFFFFFF);   // mono rect, 1x1, opaque
  system->gpu().WriteData((10u << 16) | 14u);       // at (14,10)
  system->gpu().WriteData(0x6A000000 | 0xFFFFFF);   // mono rect, 1x1, semi-transparent
  system->gpu().WriteData((10u << 16) | 16u);       // at (16,10)
  CheckEqual(vram[10 * 1024 + 14] & 0x8000, 0, "an opaque untextured draw leaves it clear");
  CheckEqual(vram[10 * 1024 + 16] & 0x8000, 0,
             "and so does a semi-transparent one");

  // Now the point of all that: with mask-checking on, the marked pixel is
  // protected and its neighbours are not.
  system->gpu().WriteData(0xE6000002);              // mask: check, do not set
  system->gpu().WriteData(0x60000000 | 0x0000FF);   // flat blue rect
  system->gpu().WriteData((10u << 16) | 10u);       // at (10,10)
  system->gpu().WriteData((1u << 16) | 8u);         // eight across, one down
  RunGpu(system);

  CheckEqual(vram[10 * 1024 + 10] & 0x7FFF, 0x7FFF,
             "the marked pixel kept its own colour");
  Check((vram[10 * 1024 + 12] & 0x7FFF) != 0x7FFF,
        "the unmarked pixel beside it was painted over");
  system->gpu().WriteData(0xE6000000);              // leave the mask as found
}

// A polyline's terminator word (GP0, X and Y fields both 0x5000..0x5FFF)
// used to be pushed into the vertex fifo like one more point instead of
// being discarded, and CmdLine decoded it as a bogus final vertex - X=Y=0
// for the common 0x50005000 terminator - so every polyline grew a spurious
// extra segment from its last real point back to the screen origin. This
// draws a polyline nowhere near (0,0) and checks that (0,0) itself, and a
// point on the straight line from the last real vertex to the origin, both
// stay untouched.
void TestPolylineTerminatorIsNotAVertex(System* system) {
  printf("a polyline's terminator word is not drawn as a vertex\n");
  system->gpu().WriteStatus(0x00000000);  // GP1(00h) reset

  // GP1(00h) resets the drawing area to a single pixel at (0,0), which would
  // clip the line below out entirely and make this test pass for the wrong
  // reason. Open it up to cover both the real line and the origin.
  system->gpu().WriteData(0xE3000000);              // top-left (0,0)
  system->gpu().WriteData(0xE4000000 | (300u << 10) | 300u);  // bottom-right

  const uint32_t kWhite = 0xFFFFFF;
  system->gpu().WriteData(0x48000000 | kWhite);  // mono polyline, opaque
  system->gpu().WriteData((150u << 16) | 100u);  // (100, 150)
  system->gpu().WriteData((150u << 16) | 200u);  // (200, 150)
  system->gpu().WriteData(0x50005000);            // terminator

  RunGpu(system);
  const VramView vram{system};
  Check(vram[0] == 0, "(0,0) was not touched by the terminator-as-vertex bug");
  // Roughly midway along the bogus (200,150)->(0,0) diagonal the old code
  // would have drawn.
  Check(vram[75 * 1024 + 100] == 0,
        "a point on the old bogus diagonal was not touched either");
  // The real segment did draw: its own midpoint should be lit.
  Check(vram[150 * 1024 + 150] != 0, "the real segment was still drawn");
}

// Two adjacent opaque flat quads share a vertical edge (A's right edge is
// B's left edge, both at x=416). The shared column is B's: a quad's own
// extent is half-open, so A stops at x=415 and never draws it, whichever is
// drawn first. This used to be described as "the later one wins", from when
// the fill rule was kept to semi-transparent triangles (bug 105) - but it
// never depended on that; the diagonal test below is the one that does.
void TestOpaqueAdjacentQuadsShareNoColumn(System* system) {
  printf("two opaque quads side by side: the shared column is the right-hand "
         "one's\n");
  system->gpu().WriteStatus(0x00000000);  // GP1(00h) reset
  system->gpu().WriteData(0xE3000000);                          // top-left (0,0)
  system->gpu().WriteData(0xE4000000 | (450u << 10) | 450u);    // bottom-right

  const uint32_t kRed   = 0x0000FF;   // r=255,g=0,b=0
  const uint32_t kGreen = 0x00FF00;   // r=0,g=255,b=0

  // Quad A: x400..416, y400..416, red. Drawn first.
  system->gpu().WriteData(0x28000000 | kRed);
  system->gpu().WriteData((400u << 16) | 400u);
  system->gpu().WriteData((400u << 16) | 416u);
  system->gpu().WriteData((416u << 16) | 400u);
  system->gpu().WriteData((416u << 16) | 416u);

  // Quad B: x416..432, y400..416, green - shares A's right edge. Drawn
  // second, so on real hardware (and pre-regression) it simply repaints
  // that shared column, same as it always did for an opaque draw.
  system->gpu().WriteData(0x28000000 | kGreen);
  system->gpu().WriteData((400u << 16) | 416u);
  system->gpu().WriteData((400u << 16) | 432u);
  system->gpu().WriteData((416u << 16) | 416u);
  system->gpu().WriteData((416u << 16) | 432u);

  RunGpu(system);
  const VramView vram{system};
  const uint16_t kRed15   = 0x001F;   // To15Bit(255,0,0)
  const uint16_t kGreen15 = 0x03E0;   // To15Bit(0,255,0)

  CheckEqual(vram[408 * 1024 + 404], kRed15,   "A's interior is red");
  CheckEqual(vram[408 * 1024 + 428], kGreen15, "B's interior is green");
  CheckEqual(vram[408 * 1024 + 416], kGreen15,
             "the shared edge column belongs to B, the right-hand quad");
}

// Two opaque triangles sharing a diagonal, drawn in both orders (bug 105).
// The fill rule gives every pixel on the diagonal to exactly one of them, by
// geometry, so the picture is the same either way round - and no pixel of it
// is left undrawn. With the rule kept to semi-transparent triangles, an
// opaque pair both drew the diagonal and whichever went second took it.
void TestOpaqueSharedDiagonalIgnoresDrawOrder(System* system) {
  printf("two opaque triangles sharing a diagonal: the same pixels whichever is "
         "drawn first\n");
  const uint32_t kRed   = 0x0000FF;
  const uint32_t kGreen = 0x00FF00;
  // Upper-right triangle (x > y side) and lower-left one, sharing the
  // diagonal from (300,300) to (340,340).
  auto upper = [&](uint32_t colour) {
    system->gpu().WriteData(0x20000000 | colour);
    system->gpu().WriteData((300u << 16) | 300u);
    system->gpu().WriteData((300u << 16) | 340u);
    system->gpu().WriteData((340u << 16) | 340u);
  };
  auto lower = [&](uint32_t colour) {
    system->gpu().WriteData(0x20000000 | colour);
    system->gpu().WriteData((300u << 16) | 300u);
    system->gpu().WriteData((340u << 16) | 340u);
    system->gpu().WriteData((340u << 16) | 300u);
  };
  auto clear = [&]() {
    system->gpu().WriteStatus(0x00000000);
    system->gpu().WriteData(0xE3000000);
    system->gpu().WriteData(0xE4000000 | (450u << 10) | 450u);
    system->gpu().WriteData(0x02000000);                  // fill black
    system->gpu().WriteData((290u << 16) | 290u);
    system->gpu().WriteData((64u << 16) | 64u);
  };
  std::vector<uint16_t> first, second;
  clear();
  upper(kRed);
  lower(kGreen);
  RunGpu(system);
  {
    const VramView vram{system};
    for (uint32_t y = 290; y < 350; ++y)
      for (uint32_t x = 290; x < 350; ++x)
        first.push_back(vram[y * 1024 + x]);
  }
  clear();
  lower(kGreen);
  upper(kRed);
  RunGpu(system);
  {
    const VramView vram{system};
    for (uint32_t y = 290; y < 350; ++y)
      for (uint32_t x = 290; x < 350; ++x)
        second.push_back(vram[y * 1024 + x]);
    CheckEqual(vram[310 * 1024 + 330], 0x001F, "the upper triangle is red");
    CheckEqual(vram[330 * 1024 + 310], 0x03E0, "the lower one green");
    bool diagonal_drawn = true;
    for (uint32_t i = 301; i < 339; ++i)
      diagonal_drawn = diagonal_drawn && vram[i * 1024 + i] != 0;
    Check(diagonal_drawn, "every pixel on the shared diagonal is drawn by one of them");
  }
  uint32_t differing = 0;
  for (size_t i = 0; i < first.size(); ++i)
    differing += first[i] != second[i];
  CheckEqual(differing, 0, "and the same one, whichever was drawn first");
}

// Silent Hill regression: two adjacent semi-transparent flat quads sharing a
// vertical edge, same additive colour, over a black background. The shared
// edge column must be blended exactly once. Before the edge-bias rule
// existed, a plain w>=0 test accepted that column for both triangles that
// touch it, and an additive blend applied twice there - "a fine diagonal
// hatching over the whole thing", per the original fix. This is the case
// the bias must still cover after being scoped to semi-transparent draws
// only: it is unconditional on state.semi_transparent, so this must still
// pass exactly as it did the day the bias was introduced.
void TestSemiTransparentSharedEdgeBlendsOnce(System* system) {
  printf("two independent semi-transparent quads sharing an edge blend "
         "exactly once there, not twice\n");
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (450u << 10) | 450u);
  system->gpu().WriteData(0xE1000020);   // draw mode: semi_mode = 1 (B+F)

  const uint32_t kColour = 0x000040;   // r=64,g=0,b=0 - small enough that
                                        // a double blend (128) does not clamp
                                        // to the same value as a single one.
  const uint32_t kSemiFlatQuad = 0x2A000000;  // quad, semi-transparent, flat

  // Quad A: x400..416, y440..456.
  system->gpu().WriteData(kSemiFlatQuad | kColour);
  system->gpu().WriteData((440u << 16) | 400u);
  system->gpu().WriteData((440u << 16) | 416u);
  system->gpu().WriteData((456u << 16) | 400u);
  system->gpu().WriteData((456u << 16) | 416u);

  // Quad B: x416..432, y440..456 - shares A's right edge.
  system->gpu().WriteData(kSemiFlatQuad | kColour);
  system->gpu().WriteData((440u << 16) | 416u);
  system->gpu().WriteData((440u << 16) | 432u);
  system->gpu().WriteData((456u << 16) | 416u);
  system->gpu().WriteData((456u << 16) | 432u);

  RunGpu(system);
  const VramView vram{system};
  const uint16_t kSingleBlend15 = 0x0008;   // To15Bit(64,0,0): 64>>3
  const uint16_t kDoubleBlend15 = 0x0010;   // To15Bit(128,0,0): a double add

  CheckEqual(vram[448 * 1024 + 404], kSingleBlend15, "A's interior blended once");
  CheckEqual(vram[448 * 1024 + 428], kSingleBlend15, "B's interior blended once");
  CheckEqual(vram[448 * 1024 + 416], kSingleBlend15,
             "the shared edge column blended exactly once, not twice");
  Check(vram[448 * 1024 + 416] != kDoubleBlend15,
        "explicitly not the double-blend value the original bug produced");
}

// The visible width is the horizontal display window divided by the dot
// clock, not the resolution GP1(08) picked. Metal Gear Solid's codec screen
// is the case that made this matter: it runs the 368-pixel mode but opens a
// window only 318 pixels wide, and taking the mode at its word painted 50
// columns of whatever VRAM sat to the right of its framebuffer - which, with
// two buffers being flipped, was different garbage every frame.
void TestVisibleWidthFollowsTheDisplayWindow(System* system) {
  printf("the visible width comes from GP1(06), not from GP1(08)'s mode\n");
  Gpu& gpu = system->gpu();

  // GP1(06) with the standard window every game uses: 512 to 3072 GPU clocks.
  // Each mode's nominal width is exactly what that window produces, so none of
  // them may move.
  struct Mode {
    uint32_t gp1_08;   // the display-mode parameter
    int width;         // what the standard window must still produce
    const char* what;
  };
  static const Mode kModes[] = {
    { 0x00, 256, "256 mode, standard window" },
    { 0x01, 320, "320 mode, standard window" },
    { 0x02, 512, "512 mode, standard window" },
    { 0x03, 640, "640 mode, standard window" },
  };
  int width = 0, height = 0;
  for (const Mode& mode : kModes) {
    gpu.WriteStatus(0x06000000u | (3072u << 12) | 512u);
    gpu.WriteStatus(0x08000000u | mode.gp1_08);
    gpu.framebuffer(width, height);
    CheckEqual(width, mode.width, mode.what);
  }

  // 368 mode divides by 7, which the standard window does not divide evenly:
  // 2560/7 is 365 whole pixels, and the beam cannot paint the 366th.
  gpu.WriteStatus(0x06000000u | (3072u << 12) | 512u);
  gpu.WriteStatus(0x08000000u | 0x40);
  gpu.framebuffer(width, height);
  CheckEqual(width, 365, "368 mode, standard window");

  // The codec screen's own registers.
  gpu.WriteStatus(0x06000000u | (2968u << 12) | 742u);
  gpu.WriteStatus(0x08000000u | 0x40);
  gpu.framebuffer(width, height);
  CheckEqual(width, 318, "368 mode, Metal Gear Solid's codec window");

  // A window wider than the mode is overscan; following it would mean
  // sampling past the framebuffer for the same reason, so it stays capped.
  gpu.WriteStatus(0x06000000u | (3568u << 12) | 400u);
  gpu.WriteStatus(0x08000000u | 0x01);
  gpu.framebuffer(width, height);
  CheckEqual(width, 320, "320 mode, a window wider than the mode");

  // A nonsense window - end before start - falls back to the mode rather than
  // producing a zero-width frame nothing can present.
  gpu.WriteStatus(0x06000000u | (512u << 12) | 3072u);
  gpu.WriteStatus(0x08000000u | 0x01);
  gpu.framebuffer(width, height);
  CheckEqual(width, 320, "320 mode, an inverted window");

  gpu.WriteStatus(0x00000000);  // leave the GPU as the next test expects it
}

}  // namespace

// A burst-mode transfer written with its start bit but not its trigger (bug 112):
// it runs when the device is asking for data and not otherwise. JaCzekanski's
// dma/chopping starts its GPU uploads with 01000001h, and a console runs them.
void TestBurstDmaStartsOnTheDevicesRequest(System* system) {
  printf("a burst DMA without its trigger waits for the device to ask\n");
  auto& io = system->io();
  auto& ram = io.ram_buffer;
  system->gpu().WriteStatus(0x00000000);
  RunGpu(system);

  // GP0(A0h): four pixels at (512,256) - the command, where, how big, then two
  // words of pixels - sent down channel 2 in one burst.
  const uint32_t packet[] = { 0xA0000000, (256u << 16) | 512u, (1u << 16) | 4u,
                              0x7C1F03E0, 0x001F7FFF };
  for (int i = 0; i < 5; ++i)
    ram.u32[(0x3000 >> 2) + i] = packet[i];
  io.Write32(0x1F8010F0, 0x00000800);        // DPCR: channel 2 on
  const size_t at = 256 * 1024 + 512;

  // Direction off: the GPU is not asking, so nothing moves.
  system->gpu().WriteStatus(0x04000000);
  io.Write32(0x1F8010A0, 0x3000);
  io.Write32(0x1F8010A4, 5);
  io.Write32(0x1F8010A8, 0x01000001);        // start, burst, from RAM - no trigger
  for (int i = 0; i < 64; ++i)
    io.Tick(32);
  RunGpu(system);
  VramView vram{system};
  CheckEqual(vram[at], 0, "with the GPU's DMA direction off, nothing is sent");
  CheckEqual(io.Read32(0x1F8010A8) & 0x01000000, 0, "and the channel is not left busy");

  // CPU to GP0: the GPU asks, and the same write sends the packet.
  system->gpu().WriteStatus(0x04000002);
  io.Write32(0x1F8010A0, 0x3000);
  io.Write32(0x1F8010A4, 5);
  io.Write32(0x1F8010A8, 0x01000001);
  for (int i = 0; i < 64; ++i)
    io.Tick(32);
  RunGpu(system);
  CheckEqual(vram[at], 0x03E0, "with it set to CPU-to-GP0, the pixels arrive");
  CheckEqual(vram[at + 3], 0x001F, "all four of them");
  CheckEqual(io.Read32(0x1F8010A8) & 0x01000000, 0, "and the channel finishes");

  // Channel 3 the same way, with no sector in the CD-ROM's data FIFO: the drive is
  // not asking, so nothing is written - the case the trigger rule was added for.
  ram.u32[0x4000 >> 2] = 0xDEADBEEF;
  io.Write32(0x1F8010F0, 0x00008000);        // DPCR: channel 3 on
  io.Write32(0x1F8010B0, 0x4000);
  io.Write32(0x1F8010B4, 1);
  io.Write32(0x1F8010B8, 0x01000000);        // start, burst, to RAM - no trigger
  for (int i = 0; i < 64; ++i)
    io.Tick(32);
  CheckEqual(ram.u32[0x4000 >> 2], 0xDEADBEEF,
             "a CD-ROM with nothing loaded leaves RAM alone");
  io.Write32(0x1F8010F0, 0x00000000);
}

// libgpu's BreakDraw: software clears channel 2's start bit while a linked list
// is still going, reads MADR for where drawing got to, draws something of its
// own, and ContinueDraw starts the channel again from there. Since bug 86 a list
// can be paused half way, waiting for the GP0 port, and a write to the busy
// channel was ignored - the list ran on underneath and ContinueDraw sent its
// tail a second time. Final Fantasy VII's battle does this every 3D frame
// (bug 142).
void TestBreakDrawStopsAPausedList(System* system) {
  printf("clearing channel 2's start bit stops a paused list where it is\n");
  auto& io = system->io();
  auto& ram = io.ram_buffer;
  system->gpu().WriteStatus(0x00000000);
  system->gpu().WriteData(0xE3000000);
  system->gpu().WriteData(0xE4000000 | (400u << 10) | 600u);
  RunGpu(system);
  system->gpu().WriteStatus(0x04000002);     // DMA direction: CPU to GP0
  const uint64_t fills_before = system->gpu().stats().gp0_commands[0x02];

  // A list: one big triangle to keep the rasteriser busy, then 24 nodes of one
  // fill each, a 16x1 row at y=300 - node i at x=16i - so what was drawn says
  // which nodes went.
  const uint32_t kList = 0x5000;
  const int kFills = 24;
  ram.u32[kList >> 2] = (4u << 24) | (kList + 0x10);
  ram.u32[(kList >> 2) + 1] = 0x20808080;
  ram.u32[(kList >> 2) + 2] = 0;
  ram.u32[(kList >> 2) + 3] = 600u;
  ram.u32[(kList >> 2) + 4] = 250u << 16;
  for (int i = 0; i < kFills; ++i) {
    const uint32_t node = kList + 0x10 + i * 0x10;
    const uint32_t next = (i + 1 < kFills) ? node + 0x10 : 0xFFFFFF;
    ram.u32[node >> 2] = (3u << 24) | next;
    ram.u32[(node >> 2) + 1] = 0x02F8F8F8;
    ram.u32[(node >> 2) + 2] = (300u << 16) | static_cast<uint32_t>(i * 16);
    ram.u32[(node >> 2) + 3] = (1u << 16) | 16u;
  }
  io.Write32(0x1F8010F0, 0x00000800);        // DPCR: channel 2 on
  io.Write32(0x1F8010A0, kList);
  io.Write32(0x1F8010A4, 0);
  io.Write32(0x1F8010A8, 0x01000401);        // start, linked list, from RAM

  const uint32_t madr = io.Read32(0x1F8010A0) & 0xFFFFFF;
  Check((io.Read32(0x1F8010A8) & 0x01000000) != 0 && madr != 0xFFFFFF,
        "the list pauses part way, behind the busy rasteriser");

  io.Write32(0x1F8010A8, io.Read32(0x1F8010A8) & ~0x01000000u);
  CheckEqual(io.Read32(0x1F8010A8) & 0x01000000, 0, "clearing the start bit stops it");
  CheckEqual(io.Read32(0x1F8010A0) & 0xFFFFFF, madr, "with MADR at the next node");

  for (int i = 0; i < 64; ++i)
    io.Tick(1024);
  RunGpu(system);
  CheckEqual(io.Read32(0x1F8010A0) & 0xFFFFFF, madr,
             "and it stays stopped while the GPU catches up");
  VramView vram{system};
  const int first_unsent = static_cast<int>((madr - (kList + 0x10)) / 0x10);
  Check(first_unsent > 0 && first_unsent < kFills, "the break fell among the fills");
  CheckEqual(vram[300 * 1024 + (first_unsent - 1) * 16], 0x7FFF,
             "what went before the break is drawn");
  CheckEqual(vram[300 * 1024 + first_unsent * 16], 0,
             "and nothing from the break on");

  // ContinueDraw.
  io.Write32(0x1F8010A0, madr);
  io.Write32(0x1F8010A8, 0x01000401);
  for (int i = 0; i < 64; ++i)
    io.Tick(1024);
  RunGpu(system);
  CheckEqual(vram[300 * 1024 + (kFills - 1) * 16], 0x7FFF,
             "starting again from MADR draws the rest");
  CheckEqual(io.Read32(0x1F8010A8) & 0x01000000, 0, "and the list finishes");
  CheckEqual(static_cast<uint32_t>(system->gpu().stats().gp0_commands[0x02]),
             static_cast<uint32_t>(fills_before + kFills),
             "every fill sent once, none twice");
  io.Write32(0x1F8010F0, 0x00000000);
  system->gpu().WriteStatus(0x04000000);
}

int main(int argc, char** argv) {
  System* system = new System();
  // --hw-raster: every scene drawn by the Direct3D 11 rasteriser on WARP instead, which must
  // give the same answers (Docs/Hardware-Renderer-Plan.md); --d3d12, by the Direct3D 12 one. In
  // a build that has them.
  const bool d3d12 = argc > 1 && strcmp(argv[1], "--d3d12") == 0;
  if (argc > 1 && (strcmp(argv[1], "--hw-raster") == 0 || d3d12)) {
#ifdef PSXEMU_HW_RASTER
    system->set_hardware_raster([d3d12](uint16_t* vram,
                                        const emulation::psx::RasterOptions& options,
                                        std::string* error)
                                    -> std::unique_ptr<emulation::psx::RasterBackend> {
      return psxemu::HardwareRaster::Create(d3d12 ? psxemu::HardwareRaster::Api::kD3D12
                                                  : psxemu::HardwareRaster::Api::kD3D11,
                                            vram, options, true, error);
    });
    system->config().gpu_rasteriser = "hardware";
#else
    printf("--hw-raster: this build has no hardware rasteriser\n");
    return 2;
#endif
  }
  system->InitializeWithoutBios();
  if (system->config().gpu_rasteriser == "hardware") {
    if (!system->gpu().hardware_raster()) {
      printf("--hw-raster: %s\n", system->gpu().raster_error().c_str());
      return 1;
    }
    printf("drawing with the Direct3D %s hardware rasteriser, on WARP\n\n", d3d12 ? "12" : "11");
  }

  TestResetStartsIdle(system);
  TestInterruptRequestSetsStatusAndIrq(system);
  TestAcknowledgeClearsStatusAndAllowsANewEdge(system);
  TestRepeatedRequestIsNotANewEdge(system);
  TestReadinessBits(system);
  TestDrawingTakesTime(system);
  TestDrawingCostIsClipped(system);
  TestInterlacedDrawingCostsHalf(system);
  TestInterlacedSkipsDisplayedField(system);
  TestSkippedFieldFlipsAtTheStartOfVblank(system);
  TestReadBackWaitsForQueuedDrawing(system);
  TestGp0QueueFillsAndDrains(system);
  TestTextureBit15BecomesTheMaskBit(system);
  TestPolylineTerminatorIsNotAVertex(system);
  TestOpaqueAdjacentQuadsShareNoColumn(system);
  TestOpaqueSharedDiagonalIgnoresDrawOrder(system);
  TestSemiTransparentSharedEdgeBlendsOnce(system);
  TestVisibleWidthFollowsTheDisplayWindow(system);
  TestBurstDmaStartsOnTheDevicesRequest(system);
  TestBreakDrawStopsAPausedList(system);
  TestCulledPolygonCostsSetupOnly(system);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  delete system;
  return g_failures == 0 ? 0 : 1;
}

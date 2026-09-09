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

#include <cstdio>

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

// The core does not model the GP0 FIFO filling up or a draw taking real GPU
// time (see Docs/Gaps.md), so all three readiness bits report ready
// unconditionally. This pins that choice down as a fact about the current
// code rather than an assumption a future change discovers the hard way.
void TestReadinessBitsAreAlwaysSet(System* system) {
  printf("the ready bits report ready unconditionally (no FIFO/timing model)\n");
  system->gpu().WriteStatus(0x00000000);
  const uint32_t status = system->gpu().ReadStatus();
  Check((status & (1u << 26)) != 0, "ready to receive a command word");
  Check((status & (1u << 27)) != 0, "ready to send VRAM to the CPU");
  Check((status & (1u << 28)) != 0, "ready to receive a DMA block");
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

  const uint16_t* vram = system->gpu().vram();
  Check(vram[0] == 0, "(0,0) was not touched by the terminator-as-vertex bug");
  // Roughly midway along the bogus (200,150)->(0,0) diagonal the old code
  // would have drawn.
  Check(vram[75 * 1024 + 100] == 0,
        "a point on the old bogus diagonal was not touched either");
  // The real segment did draw: its own midpoint should be lit.
  Check(vram[150 * 1024 + 150] != 0, "the real segment was still drawn");
}

// Two adjacent opaque flat quads share a vertical edge (A's right edge is
// B's left edge, both at x=416). Regression for the fix that scoped the
// top-left edge-bias rule to semi-transparent primitives only: applying it
// unconditionally (the fully-regressed state) still passed the coverage
// test (every pixel drawn exactly once, no gaps) but changed which of the
// two *independent, differently-coloured* primitives owns the shared edge
// column from "whichever was drawn last" to "whichever the edge-direction
// rule geometrically favours" - and for a ground built from many small
// differently-textured tiles (Wild Arms' overworld) that shows up as fine
// seams through the whole field, tile edges sampling the wrong neighbour.
// For a single quad's own two triangles this is invisible (same texture,
// continuous data either way), which is why it was not caught by the fix
// that introduced the bias.
void TestOpaqueSharedEdgeUsesLastDrawnPrimitive(System* system) {
  printf("two independent opaque quads sharing an edge: the later one wins "
         "the shared column, same as before edge-biasing existed\n");
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

  const uint16_t* vram = system->gpu().vram();
  const uint16_t kRed15   = 0x001F;   // To15Bit(255,0,0)
  const uint16_t kGreen15 = 0x03E0;   // To15Bit(0,255,0)

  CheckEqual(vram[408 * 1024 + 404], kRed15,   "A's interior is red");
  CheckEqual(vram[408 * 1024 + 428], kGreen15, "B's interior is green");
  CheckEqual(vram[408 * 1024 + 416], kGreen15,
             "the shared edge column belongs to B, the later draw - not "
             "geometrically reassigned to A");
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

  const uint16_t* vram = system->gpu().vram();
  const uint16_t kSingleBlend15 = 0x0008;   // To15Bit(64,0,0): 64>>3
  const uint16_t kDoubleBlend15 = 0x0010;   // To15Bit(128,0,0): a double add

  CheckEqual(vram[448 * 1024 + 404], kSingleBlend15, "A's interior blended once");
  CheckEqual(vram[448 * 1024 + 428], kSingleBlend15, "B's interior blended once");
  CheckEqual(vram[448 * 1024 + 416], kSingleBlend15,
             "the shared edge column blended exactly once, not twice");
  Check(vram[448 * 1024 + 416] != kDoubleBlend15,
        "explicitly not the double-blend value the original bug produced");
}

}  // namespace

int main() {
  System* system = new System();
  system->InitializeWithoutBios();

  TestResetStartsIdle(system);
  TestInterruptRequestSetsStatusAndIrq(system);
  TestAcknowledgeClearsStatusAndAllowsANewEdge(system);
  TestRepeatedRequestIsNotANewEdge(system);
  TestReadinessBitsAreAlwaysSet(system);
  TestPolylineTerminatorIsNotAVertex(system);
  TestOpaqueSharedEdgeUsesLastDrawnPrimitive(system);
  TestSemiTransparentSharedEdgeBlendsOnce(system);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  delete system;
  return g_failures == 0 ? 0 : 1;
}

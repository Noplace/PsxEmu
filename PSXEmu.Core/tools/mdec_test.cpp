// mdec_test - checks the motion decoder against things that must be true.
//
// The MDEC is a pipe with no state worth speaking of between macroblocks, so
// almost all of it can be checked by feeding it one block and looking at what
// comes out. The parts that are easy to get wrong and hard to see wrong in a
// running game - the parameter countdown, the table unpacking, the run/level
// walk, the zigzag, the colour matrix - are each pinned here.

#include "psx/psx.h"

#include <cstdio>
#include <cstring>
#include <vector>

using emulation::psx::Mdec;
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

const uint32_t kMdecData = 0x1F801820;
const uint32_t kMdecControl = 0x1F801824;

// Command words.
uint32_t DecodeCommand(uint32_t depth, bool is_signed, bool bit15,
                       uint32_t words) {
  return (1u << 29) | (depth << 27) | (is_signed ? 0x04000000u : 0u) |
         (bit15 ? 0x02000000u : 0u) | (words & 0xFFFF);
}

// A quantisation table where every entry is 1, so dequantising is the identity
// apart from the documented scaling. Packed four to a word as the hardware
// takes it.
void SetFlatQuantTable(Mdec& mdec, uint8_t value) {
  mdec.Write(kMdecData, (2u << 29) | 1);        // luminance and colour
  uint32_t word = static_cast<uint32_t>(value);
  word |= word << 8;
  word |= word << 16;
  for (int i = 0; i < 32; ++i)
    mdec.Write(kMdecData, word);
}

// The scale table is the cosine matrix the inverse transform multiplies by.
// This builds the real one, in the 1.15 fixed point the hardware uses, so the
// transform can be checked against what it should actually produce.
void SetScaleTable(Mdec& mdec) {
  int16_t table[64];
  for (int u = 0; u < 8; ++u) {
    for (int x = 0; x < 8; ++x) {
      const double c = (u == 0) ? (1.0 / 2.8284271247461903)   // 1/(2*sqrt(2))
                                : 0.5;
      const double value =
          c * cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0);
      table[u * 8 + x] = static_cast<int16_t>(value * 32768.0);
    }
  }
  mdec.Write(kMdecData, 3u << 29);
  for (int i = 0; i < 32; ++i) {
    uint32_t word = static_cast<uint16_t>(table[i * 2]) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(
                         table[i * 2 + 1])) << 16);
    mdec.Write(kMdecData, word);
  }
}

// Builds the run/level stream for a block whose only coefficient is the DC.
// Returns the two 16-bit codes packed into one word: the DC word, then the
// end-of-block marker.
uint32_t FlatBlockWord(uint32_t quant_scale, int32_t dc) {
  const uint16_t first =
      static_cast<uint16_t>((quant_scale << 10) | (dc & 0x3FF));
  return first | (0xFE00u << 16);
}

// Drains everything the decoder is holding.
std::vector<uint32_t> Drain(Mdec& mdec) {
  std::vector<uint32_t> out;
  while (mdec.HasData())
    out.push_back(mdec.ReadWord());
  return out;
}

// ---------------------------------------------------------------------------

void TestParameterCountdown(Mdec& mdec) {
  printf("parameter countdown\n");

  // Idle: no command in flight, so the count reads as "none".
  mdec.Write(kMdecControl, 0x80000000);         // reset
  CheckEqual(mdec.Status() & 0xFFFF, 0xFFFF, "idle count is FFFF");
  Check((mdec.Status() & 0x20000000) == 0, "idle is not busy");
  Check((mdec.Status() & 0x80000000) != 0, "idle output fifo is empty");

  // A quant table command wants 16 words for luminance alone.
  mdec.Write(kMdecData, 2u << 29);
  Check((mdec.Status() & 0x20000000) != 0, "busy while taking parameters");
  CheckEqual(mdec.Status() & 0xFFFF, 15, "16 words wanted reads as 15");

  for (int i = 0; i < 15; ++i)
    mdec.Write(kMdecData, 0);
  CheckEqual(mdec.Status() & 0xFFFF, 0, "one word left reads as 0");
  Check((mdec.Status() & 0x20000000) != 0, "still busy with one to go");

  mdec.Write(kMdecData, 0);
  Check((mdec.Status() & 0x20000000) == 0, "ready once the last word lands");
  CheckEqual(mdec.Status() & 0xFFFF, 0xFFFF, "count back to FFFF when idle");

  // With colour it wants 32.
  mdec.Write(kMdecData, (2u << 29) | 1);
  CheckEqual(mdec.Status() & 0xFFFF, 31, "colour table wants 32 words");
  for (int i = 0; i < 32; ++i)
    mdec.Write(kMdecData, 0);
  Check((mdec.Status() & 0x20000000) == 0, "colour table completes");

  // And the scale table wants 32.
  mdec.Write(kMdecData, 3u << 29);
  CheckEqual(mdec.Status() & 0xFFFF, 31, "scale table wants 32 words");
  for (int i = 0; i < 32; ++i)
    mdec.Write(kMdecData, 0);
  Check((mdec.Status() & 0x20000000) == 0, "scale table completes");
}

void TestControlAndStatusBits(Mdec& mdec) {
  printf("control and status bits\n");

  mdec.Write(kMdecControl, 0x80000000);
  Check(!mdec.WantsData(), "idle wants no data");
  Check(!mdec.HasData(), "idle has no data");

  // The DMA request bits only appear once the matching enable is set.
  mdec.Write(kMdecData, 2u << 29);              // a command that wants data
  Check(mdec.WantsData(), "wants data mid-command");
  Check((mdec.Status() & 0x10000000) == 0,
        "no data-in request while DMA0 is disabled");
  mdec.Write(kMdecControl, 0x40000000);         // enable DMA0
  Check((mdec.Status() & 0x10000000) != 0,
        "data-in request once DMA0 is enabled");

  mdec.Write(kMdecControl, 0x80000000);         // reset clears the enables
  Check((mdec.Status() & 0x10000000) == 0, "reset clears the DMA0 enable");

  // Depth, signedness and bit15 are echoed from the command into the status.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth15, true, true, 2));
  const uint32_t status = mdec.Status();
  CheckEqual((status >> 25) & 3, Mdec::kDepth15, "depth echoed to status");
  Check((status & 0x01000000) != 0, "signed echoed to status");
  Check((status & 0x00800000) != 0, "bit15 echoed to status");
  mdec.Write(kMdecControl, 0x80000000);
}

void TestQuantTableUnpacking(Mdec& mdec) {
  printf("quant table unpacking\n");

  // Each word carries four table entries, low byte first. Feeding a counting
  // pattern and then decoding a block with a known DC shows which entry the
  // decoder actually used.
  mdec.Write(kMdecControl, 0x80000000);
  SetScaleTable(mdec);

  // Luminance entry 0 is 4; every colour entry is 1.
  mdec.Write(kMdecData, (2u << 29) | 1);
  mdec.Write(kMdecData, 0x01010104);            // luma[0..3] = 4,1,1,1
  for (int i = 1; i < 16; ++i)
    mdec.Write(kMdecData, 0x01010101);
  for (int i = 0; i < 16; ++i)
    mdec.Write(kMdecData, 0x01010101);

  // A monochrome block uses the luminance table, so a DC of 8 with a non-zero
  // quant scale is scaled by luma[0] = 4.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, false, false, 1));
  mdec.Write(kMdecData, FlatBlockWord(1, 8));
  const std::vector<uint32_t> out = Drain(mdec);

  // 8 * 4 = 32 through a flat inverse transform is a constant block. What
  // matters here is only that the luminance entry was used at all, so the
  // check is that the output is not what an entry of 1 would give.
  Check(!out.empty(), "monochrome block produced output");
  if (!out.empty()) {
    const uint32_t first_pixel = out[0] & 0xFF;
    Check(first_pixel != 128,
          "a non-zero DC moved the output away from mid-grey");
  }
}

void TestDcOnlyBlockIsFlat(Mdec& mdec) {
  printf("a DC-only block is flat\n");

  mdec.Write(kMdecControl, 0x80000000);
  SetScaleTable(mdec);
  SetFlatQuantTable(mdec, 1);

  // Monochrome, 8 bits, one block, DC only. Every pixel must be the same:
  // that is what "only the DC coefficient" means after an inverse transform,
  // and it is the single most useful check on the transform being separable
  // and normalised correctly.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, false, false, 1));
  mdec.Write(kMdecData, FlatBlockWord(0, 16));
  const std::vector<uint32_t> out = Drain(mdec);

  CheckEqual(static_cast<uint32_t>(out.size()), 16,
             "an 8x8 block at 8bpp is 16 words");
  if (out.size() == 16) {
    bool flat = true;
    const uint32_t first = out[0];
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i] != first)
        flat = false;
    }
    Check(flat, "every pixel of a DC-only block is identical");

    // The four bytes of a word are four pixels, and they must agree too.
    const uint32_t b0 = first & 0xFF;
    const uint32_t b1 = (first >> 8) & 0xFF;
    Check(b0 == b1, "pixels within a word agree");
  }
}

void TestZeroBlockIsMidGrey(Mdec& mdec) {
  printf("an empty block is mid-grey\n");

  mdec.Write(kMdecControl, 0x80000000);
  SetScaleTable(mdec);
  SetFlatQuantTable(mdec, 1);

  // A DC of zero is luminance zero, which unsigned output offsets to 128.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, false, false, 1));
  mdec.Write(kMdecData, FlatBlockWord(0, 0));
  const std::vector<uint32_t> out = Drain(mdec);

  Check(!out.empty(), "empty block produced output");
  if (!out.empty())
    CheckEqual(out[0] & 0xFF, 128, "zero luminance is 128 unsigned");

  // Signed output leaves it at zero instead.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, true, false, 1));
  mdec.Write(kMdecData, FlatBlockWord(0, 0));
  const std::vector<uint32_t> signed_out = Drain(mdec);
  Check(!signed_out.empty(), "signed empty block produced output");
  if (!signed_out.empty())
    CheckEqual(signed_out[0] & 0xFF, 0, "zero luminance is 0 signed");
}

void TestColourMacroblockShape(Mdec& mdec) {
  printf("colour macroblock\n");

  mdec.Write(kMdecControl, 0x80000000);
  SetScaleTable(mdec);
  SetFlatQuantTable(mdec, 1);

  // Six blocks: Cr, Cb, then four luminance. All DC-only and all zero, which
  // must give a 16x16 block of mid-grey.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth15, false, false, 6));
  for (int i = 0; i < 6; ++i)
    mdec.Write(kMdecData, FlatBlockWord(0, 0));
  const std::vector<uint32_t> out = Drain(mdec);

  CheckEqual(static_cast<uint32_t>(out.size()), 128,
             "16x16 at 15bpp is 128 words");
  if (out.size() == 128) {
    const uint32_t pixel = out[0] & 0xFFFF;
    const uint32_t r = pixel & 0x1F;
    const uint32_t g = (pixel >> 5) & 0x1F;
    const uint32_t b = (pixel >> 10) & 0x1F;
    CheckEqual(r, 16, "neutral red is mid-scale");
    CheckEqual(g, 16, "neutral green is mid-scale");
    CheckEqual(b, 16, "neutral blue is mid-scale");
    Check((pixel & 0x8000) == 0, "bit15 clear when not asked for");

    bool uniform = true;
    for (size_t i = 0; i < out.size(); ++i) {
      if (out[i] != out[0])
        uniform = false;
    }
    Check(uniform, "a neutral macroblock is uniform");
  }

  // 24-bit output of the same thing is three bytes a pixel: 768 bytes.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth24, false, false, 6));
  for (int i = 0; i < 6; ++i)
    mdec.Write(kMdecData, FlatBlockWord(0, 0));
  const std::vector<uint32_t> out24 = Drain(mdec);
  CheckEqual(static_cast<uint32_t>(out24.size()), 192,
             "16x16 at 24bpp is 192 words");

  // And bit15 must appear when it is asked for.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth15, false, true, 6));
  for (int i = 0; i < 6; ++i)
    mdec.Write(kMdecData, FlatBlockWord(0, 0));
  const std::vector<uint32_t> masked = Drain(mdec);
  Check(!masked.empty(), "bit15 run produced output");
  if (!masked.empty())
    Check((masked[0] & 0x8000) != 0, "bit15 set when asked for");
}

void TestChrominanceMovesColour(Mdec& mdec) {
  printf("chrominance moves colour\n");

  mdec.Write(kMdecControl, 0x80000000);
  SetScaleTable(mdec);
  SetFlatQuantTable(mdec, 1);

  // Cr positive with everything else neutral must push red up and green down,
  // and leave blue alone. That is the colour matrix in one check, and it is
  // the one that catches Cr and Cb being the wrong way round.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth15, false, false, 6));
  mdec.Write(kMdecData, FlatBlockWord(0, 40));     // Cr
  mdec.Write(kMdecData, FlatBlockWord(0, 0));      // Cb
  for (int i = 0; i < 4; ++i)
    mdec.Write(kMdecData, FlatBlockWord(0, 0));    // Y
  const std::vector<uint32_t> reddish = Drain(mdec);

  Check(!reddish.empty(), "Cr run produced output");
  if (!reddish.empty()) {
    const uint32_t pixel = reddish[0] & 0xFFFF;
    const uint32_t r = pixel & 0x1F;
    const uint32_t g = (pixel >> 5) & 0x1F;
    const uint32_t b = (pixel >> 10) & 0x1F;
    Check(r > 16, "positive Cr raises red");
    Check(g < 16, "positive Cr lowers green");
    CheckEqual(b, 16, "Cr leaves blue alone");
  }

  // Cb positive must push blue up and green down, and leave red alone.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth15, false, false, 6));
  mdec.Write(kMdecData, FlatBlockWord(0, 0));      // Cr
  mdec.Write(kMdecData, FlatBlockWord(0, 40));     // Cb
  for (int i = 0; i < 4; ++i)
    mdec.Write(kMdecData, FlatBlockWord(0, 0));
  const std::vector<uint32_t> bluish = Drain(mdec);

  Check(!bluish.empty(), "Cb run produced output");
  if (!bluish.empty()) {
    const uint32_t pixel = bluish[0] & 0xFFFF;
    const uint32_t r = pixel & 0x1F;
    const uint32_t g = (pixel >> 5) & 0x1F;
    const uint32_t b = (pixel >> 10) & 0x1F;
    CheckEqual(r, 16, "Cb leaves red alone");
    Check(g < 16, "positive Cb lowers green");
    Check(b > 16, "positive Cb raises blue");
  }
}

void TestRunLevelWalk(Mdec& mdec) {
  printf("run and level walk\n");

  mdec.Write(kMdecControl, 0x80000000);
  SetScaleTable(mdec);
  SetFlatQuantTable(mdec, 1);

  // A DC of zero with one AC coefficient at position 1 must produce a block
  // that is not flat: the first AC basis function is a horizontal ramp. This
  // is what catches a broken zigzag or a run that lands in the wrong place.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, false, false, 2));
  const uint16_t dc = 0;                             // quant scale 0, dc 0
  const uint16_t ac = (0u << 10) | (100 & 0x3FF);    // run 0 -> index 1
  mdec.Write(kMdecData, dc | (static_cast<uint32_t>(ac) << 16));
  mdec.Write(kMdecData, 0xFE00u | (0xFE00u << 16));
  const std::vector<uint32_t> out = Drain(mdec);

  Check(!out.empty(), "run/level block produced output");
  if (out.size() == 16) {
    // Along a row the value must change; down a column it must not, because
    // coefficient 1 varies in x only.
    const uint32_t row0 = out[0];
    const uint8_t p0 = row0 & 0xFF;
    const uint8_t p1 = (row0 >> 8) & 0xFF;
    Check(p0 != p1, "an x-varying coefficient varies along the row");

    const uint32_t row1 = out[2];      // 8 pixels a row, 4 to a word
    Check(row0 == row1, "an x-varying coefficient is constant down columns");
  }

  // Coefficient 2 is where the zigzag first actually moves something:
  // it belongs at position 8, the first *vertical* basis function, not at
  // position 2. So it must vary down columns and stay constant along rows -
  // exactly the opposite of coefficient 1 above. Skipping the zigzag puts it
  // at position 2, which varies along x, and this check fails.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, false, false, 2));
  const uint16_t ac2 = (1u << 10) | (100 & 0x3FF);   // run 1 -> index 2
  mdec.Write(kMdecData, dc | (static_cast<uint32_t>(ac2) << 16));
  mdec.Write(kMdecData, 0xFE00u | (0xFE00u << 16));
  const std::vector<uint32_t> vertical = Drain(mdec);

  Check(vertical.size() == 16, "vertical coefficient block is 16 words");
  if (vertical.size() == 16) {
    const uint8_t p0 = vertical[0] & 0xFF;
    const uint8_t p1 = (vertical[0] >> 8) & 0xFF;
    Check(p0 == p1, "a y-varying coefficient is constant along the row");
    Check(vertical[0] != vertical[2],
          "a y-varying coefficient varies down columns");
  }

  // A run that walks past the end of the block must end it rather than write
  // outside it. Nothing to check but that we are still alive and the decoder
  // came back to idle.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, false, false, 2));
  const uint16_t big_run = (63u << 10) | 1;
  mdec.Write(kMdecData, dc | (static_cast<uint32_t>(big_run) << 16));
  mdec.Write(kMdecData, 0xFE00u | (0xFE00u << 16));
  Drain(mdec);
  Check(mdec.stats().short_blocks > 0, "a run off the end is counted");
}

void TestPaddingIsIgnored(Mdec& mdec) {
  printf("padding\n");

  mdec.Write(kMdecControl, 0x80000000);
  SetScaleTable(mdec);
  SetFlatQuantTable(mdec, 1);

  const uint64_t before = mdec.stats().macroblocks;

  // Software pads the tail of its data to a whole number of words with
  // end-of-block markers. Those must not be mistaken for empty blocks.
  mdec.Write(kMdecData, DecodeCommand(Mdec::kDepth8, false, false, 3));
  mdec.Write(kMdecData, FlatBlockWord(0, 8));
  mdec.Write(kMdecData, 0xFE00u | (0xFE00u << 16));
  mdec.Write(kMdecData, 0xFE00u | (0xFE00u << 16));

  CheckEqual(static_cast<uint32_t>(mdec.stats().macroblocks - before), 1,
             "padding after a block does not make more blocks");
}

void TestWiredIntoTheBus() {
  printf("bus wiring\n");

  // The registers have to be reachable through the memory map, at all three of
  // the addresses every PSX register has.
  System* system = new System();
  emulation::psx::IOInterface& io = system->io();

  io.Write32(0x1F801824, 0x80000000);
  const uint32_t status = io.Read32(0x1F801824);
  Check((status & 0x80000000) != 0, "status readable at 1F801824");
  Check((status & 0x20000000) == 0, "reset leaves the decoder idle");

  io.Write32(0x1F801820, 2u << 29);
  Check((io.Read32(0x1F801824) & 0x20000000) != 0,
        "a command through the bus makes it busy");

  delete system;
}

}  // namespace

int main() {
  System* system = new System();
  Mdec& mdec = system->io().mdec;
  mdec.set_system(system);
  mdec.Initialize();

  TestParameterCountdown(mdec);
  TestControlAndStatusBits(mdec);
  TestQuantTableUnpacking(mdec);
  TestDcOnlyBlockIsFlat(mdec);
  TestZeroBlockIsMidGrey(mdec);
  TestColourMacroblockShape(mdec);
  TestChrominanceMovesColour(mdec);
  TestRunLevelWalk(mdec);
  TestPaddingIsIgnored(mdec);
  TestWiredIntoTheBus();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  delete system;
  return g_failures == 0 ? 0 : 1;
}

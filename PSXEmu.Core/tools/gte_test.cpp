// gte_test - unit tests for the geometry transformation engine.
//
//   gte_test [group]
//
// Takes no BIOS, no window and no disc. Registers are loaded, a command word
// is executed, and the results are checked - the same path a game takes,
// through MFC2/MTC2/CFC2/CTC2 semantics.
//
// This exists because the GTE is the component where being wrong is quietest.
// A CPU bug hangs the boot; a GTE bug moves a vertex slightly, or sets a flag
// software reads to decide whether a polygon is facing away, and the only
// symptom is that one game looks subtly wrong. There are about thirty commands
// and no game will tell you which of them is the problem.
//
// Expected values are derived from the hardware description rather than from
// this implementation, so a test failing means the implementation is wrong -
// not that it changed.

#include "psx/psx.h"
#include "psx/recompiler_bridge.h"
#include "rec/gte_compiler.h"

#include <cstdio>
#include <cstring>
#include <string>

using emulation::psx::Gte;

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_test;

void BeginTest(const std::string& name) { g_test = name; }

std::string Hex(uint32_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "0x%08X", value);
  return buffer;
}

void CheckEqual(uint32_t actual, uint32_t expected, const char* what) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    printf("  FAIL  %s / %s\n        got %s, expected %s\n", g_test.c_str(),
           what, Hex(actual).c_str(), Hex(expected).c_str());
  }
}

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s / %s\n", g_test.c_str(), what);
  }
}

// Data register numbers, named as the hardware description names them.
enum DataRegister {
  VXY0 = 0, VZ0, VXY1, VZ1, VXY2, VZ2, RGBC, OTZ,
  IR0, IR1, IR2, IR3, SXY0, SXY1, SXY2, SXYP,
  SZ0, SZ1, SZ2, SZ3, RGB0, RGB1, RGB2, RES1,
  MAC0, MAC1, MAC2, MAC3, IRGB, ORGB, LZCS, LZCR
};

// Control register numbers.
enum ControlRegister {
  RT11RT12 = 0, RT13RT21, RT22RT23, RT31RT32, RT33, TRX, TRY, TRZ,
  L11L12, L13L21, L22L23, L31L32, L33, RBK, GBK, BBK,
  LR1LR2, LR3LG1, LG2LG3, LB1LB2, LB3, RFC, GFC, BFC,
  OFX, OFY, H, DQA, DQB, ZSF3, ZSF4, FLAG
};

// Command word fields.
uint32_t Command(uint32_t opcode, bool sf = false, bool lm = false,
                 uint32_t mx = 0, uint32_t v = 0, uint32_t tx = 0) {
  return (0x25u << 25) | opcode | (sf ? (1u << 19) : 0) |
         (lm ? (1u << 10) : 0) | (mx << 17) | (v << 15) | (tx << 13);
}

uint32_t Pack16(int16_t low, int16_t high) {
  return static_cast<uint16_t>(low) |
         (static_cast<uint32_t>(static_cast<uint16_t>(high)) << 16);
}

class Machine {
 public:
  Machine() : system_(new emulation::psx::System()) {
    system_->InitializeWithoutBios();
  }
  ~Machine() {
    system_->Deinitialize();
    delete system_;
  }

  Gte& gte() { return system_->gte(); }

  // Clears the whole register file, so no test can depend on another.
  void Reset() {
    for (uint32_t i = 0; i < 32; ++i) {
      gte().WriteData(i, 0);
      if (i != 31)
        gte().WriteControl(i, 0);
    }
    gte().WriteControl(FLAG, 0);
  }

  void SetData(uint32_t index, uint32_t value) { gte().WriteData(index, value); }
  void SetControl(uint32_t index, uint32_t value) {
    gte().WriteControl(index, value);
  }
  uint32_t Data(uint32_t index) { return gte().ReadData(index); }
  uint32_t Control(uint32_t index) { return gte().ReadControl(index); }
  void Run(uint32_t command) { gte().Execute(command); }

  // The identity rotation, in the 1.3.12 fixed-point the GTE uses.
  void SetIdentityRotation() {
    SetControl(RT11RT12, Pack16(0x1000, 0));
    SetControl(RT13RT21, Pack16(0, 0));
    SetControl(RT22RT23, Pack16(0x1000, 0));
    SetControl(RT31RT32, Pack16(0, 0));
    SetControl(RT33, 0x1000);
  }

 private:
  emulation::psx::System* system_;
};

// ---------------------------------------------------------------------------
// The register file
// ---------------------------------------------------------------------------

void TestRegisters(Machine& m) {
  BeginTest("the vector registers pack two 16-bit halves");
  m.Reset();
  m.SetData(VXY0, 0xBBBBAAAA);
  CheckEqual(m.Data(VXY0), 0xBBBBAAAA, "VXY0 round trip");

  BeginTest("the Z components sign-extend on read");
  m.Reset();
  m.SetData(VZ0, 0x0000FFFF);
  CheckEqual(m.Data(VZ0), 0xFFFFFFFF, "VZ0 reads sign-extended");
  m.SetData(VZ0, 0x00007FFF);
  CheckEqual(m.Data(VZ0), 0x00007FFF, "a positive Z is unchanged");

  BeginTest("IR1-3 sign-extend on read");
  m.Reset();
  m.SetData(IR1, 0x00008000);
  CheckEqual(m.Data(IR1), 0xFFFF8000, "IR1 reads sign-extended");

  BeginTest("OTZ and SZ are unsigned");
  m.Reset();
  m.SetData(OTZ, 0x0000FFFF);
  CheckEqual(m.Data(OTZ), 0x0000FFFF, "OTZ does not sign-extend");
  m.SetData(SZ3, 0x0000FFFF);
  CheckEqual(m.Data(SZ3), 0x0000FFFF, "SZ3 does not sign-extend");

  BeginTest("writing SXYP pushes the screen FIFO rather than a register");
  // This is the one register that is a side effect. Writing it must move
  // SXY1 to SXY0 and SXY2 to SXY1, not store anything of its own.
  m.Reset();
  m.SetData(SXY0, 0x00010001);
  m.SetData(SXY1, 0x00020002);
  m.SetData(SXY2, 0x00030003);
  m.SetData(SXYP, 0x00040004);
  CheckEqual(m.Data(SXY0), 0x00020002, "SXY1 moved down to SXY0");
  CheckEqual(m.Data(SXY1), 0x00030003, "SXY2 moved down to SXY1");
  CheckEqual(m.Data(SXY2), 0x00040004, "the new value is at the top");
  CheckEqual(m.Data(SXYP), 0x00040004, "SXYP mirrors the top of the FIFO");

  BeginTest("IRGB packs IR1-3 into five bits each");
  m.Reset();
  m.SetData(IR1, 0x0F80);      // 0x0F80 / 128 = 31
  m.SetData(IR2, 0x0000);
  m.SetData(IR3, 0x0100);      // 0x0100 / 128 = 2
  CheckEqual(m.Data(IRGB), 31u | (0u << 5) | (2u << 10), "IRGB read");
  CheckEqual(m.Data(ORGB), 31u | (0u << 5) | (2u << 10), "ORGB reads the same");

  BeginTest("IRGB clamps rather than wrapping");
  m.Reset();
  m.SetData(IR1, 0x7FFF);      // well past 31 after the divide
  m.SetData(IR2, 0x8000);      // negative
  CheckEqual(m.Data(IRGB) & 0x1F, 0x1F, "a large IR1 clamps to 31");
  CheckEqual((m.Data(IRGB) >> 5) & 0x1F, 0u, "a negative IR2 clamps to 0");

  BeginTest("writing IRGB expands back into IR1-3");
  m.Reset();
  m.SetData(IRGB, 31u | (1u << 5) | (2u << 10));
  CheckEqual(m.Data(IR1), 31u * 128, "IR1");
  CheckEqual(m.Data(IR2), 1u * 128, "IR2");
  CheckEqual(m.Data(IR3), 2u * 128, "IR3");

  BeginTest("LZCR counts leading zeroes of a positive LZCS");
  m.Reset();
  m.SetData(LZCS, 0x00000000);
  CheckEqual(m.Data(LZCR), 32, "zero has 32 leading zeroes");
  m.SetData(LZCS, 0x00FFFFFF);
  CheckEqual(m.Data(LZCR), 8, "0x00FFFFFF has 8");
  m.SetData(LZCS, 0x80000000);
  CheckEqual(m.Data(LZCR), 1, "a negative value counts leading ones");
  m.SetData(LZCS, 0xFFFFFFFF);
  CheckEqual(m.Data(LZCR), 32, "all ones has 32");

  BeginTest("LZCR is read-only");
  m.Reset();
  m.SetData(LZCS, 0x00FFFFFF);
  m.SetData(LZCR, 5);
  CheckEqual(m.Data(LZCR), 8, "the write was ignored");

  BeginTest("ORGB is read-only");
  m.Reset();
  m.SetData(IR1, 0x0F80);
  m.SetData(ORGB, 0);
  CheckEqual(m.Data(IR1), 0x0F80, "IR1 was not disturbed");

  BeginTest("the control matrices pack across register boundaries");
  // RT13 and RT21 share one register, which is easy to get wrong by one.
  m.Reset();
  m.SetControl(RT13RT21, Pack16(0x1111, 0x2222));
  CheckEqual(m.Control(RT13RT21), Pack16(0x1111, 0x2222), "round trip");

  BeginTest("the single-component matrix entries sign-extend");
  m.Reset();
  m.SetControl(RT33, 0x0000FFFF);
  CheckEqual(m.Control(RT33), 0xFFFFFFFF, "RT33 reads sign-extended");
  m.SetControl(L33, 0x00008000);
  CheckEqual(m.Control(L33), 0xFFFF8000, "L33 reads sign-extended");

  BeginTest("H reads back sign-extended even though it is used unsigned");
  // A genuine hardware quirk, kept deliberately: H is a 16-bit unsigned
  // projection distance, but CFC2 sign-extends it.
  m.Reset();
  m.SetControl(H, 0x0000FFFF);
  CheckEqual(m.Control(H), 0xFFFFFFFF, "H reads sign-extended");

  BeginTest("FLAG bit 31 is derived, not written");
  m.Reset();
  m.SetControl(FLAG, 0x7FFFF000);
  Check((m.Control(FLAG) & 0x80000000u) != 0, "an error bit sets bit 31");
  m.SetControl(FLAG, 0x00001000);           // IR0 saturated, not an error bit
  CheckEqual(m.Control(FLAG) & 0x80000000u, 0u, "a non-error bit does not");
}

// ---------------------------------------------------------------------------
// Saturation and the FLAG register
// ---------------------------------------------------------------------------

void TestFlags(Machine& m) {
  BeginTest("FLAG is cleared at the start of every command");
  m.Reset();
  m.SetControl(FLAG, 0x7FFFF000);
  m.SetIdentityRotation();
  m.Run(Command(0x06));                     // NCLIP, on zeroed vertices
  CheckEqual(m.Control(FLAG), 0u, "a clean command leaves FLAG empty");

  BeginTest("IR saturation sets its own flag bit");
  m.Reset();
  m.SetIdentityRotation();
  // A vertex far enough out that the rotation result cannot fit in IR1.
  m.SetData(VXY0, Pack16(0x7FFF, 0));
  m.SetControl(RT11RT12, Pack16(0x7FFF, 0));
  m.Run(Command(0x01));                     // RTPS
  Check((m.Control(FLAG) & (1u << 24)) != 0, "IR1 saturation flagged");
  CheckEqual(m.Data(IR1), 0x7FFF, "IR1 clamped to its maximum");

  BeginTest("lm clamps IR to zero rather than to -8000");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(-0x7FFF, 0));
  m.SetControl(RT11RT12, Pack16(0x7FFF, 0));
  m.Run(Command(0x01, false, false));       // lm off
  CheckEqual(m.Data(IR1), 0xFFFF8000, "without lm, IR1 clamps to -8000");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(-0x7FFF, 0));
  m.SetControl(RT11RT12, Pack16(0x7FFF, 0));
  m.Run(Command(0x01, false, true));        // lm on
  CheckEqual(m.Data(IR1), 0u, "with lm, IR1 clamps to 0");

  BeginTest("the error bit is the OR of the error flags");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(0x7FFF, 0));
  m.SetControl(RT11RT12, Pack16(0x7FFF, 0));
  m.Run(Command(0x01));
  Check((m.Control(FLAG) & 0x80000000u) != 0, "bit 31 set alongside bit 24");

  BeginTest("MAC0 overflow is flagged");
  m.Reset();
  // NCLIP multiplies screen coordinates; the largest ones cannot overflow it,
  // so drive MAC0 through AVSZ4 with a big ZSF4 instead.
  m.SetData(SZ0, 0xFFFF);
  m.SetData(SZ1, 0xFFFF);
  m.SetData(SZ2, 0xFFFF);
  m.SetData(SZ3, 0xFFFF);
  m.SetControl(ZSF4, 0x7FFF);
  m.Run(Command(0x2E));                     // AVSZ4
  Check((m.Control(FLAG) & (1u << 16)) != 0, "MAC0 positive overflow flagged");
  Check((m.Control(FLAG) & (1u << 18)) != 0, "OTZ saturation flagged");
  CheckEqual(m.Data(OTZ), 0xFFFFu, "OTZ clamped to its maximum");
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void TestRtps(Machine& m) {
  BeginTest("RTPS with the identity rotation and no translation");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(100, 200));
  m.SetData(VZ0, 300);
  m.SetControl(H, 1000);
  m.SetControl(OFX, 0);
  m.SetControl(OFY, 0);
  m.Run(Command(0x01, true));                // sf = 1

  // With the identity matrix and sf=1 the accumulator holds v * 0x1000 >> 12,
  // which is the vertex itself.
  CheckEqual(m.Data(MAC1), 100, "MAC1 is the x component");
  CheckEqual(m.Data(MAC2), 200, "MAC2 is the y component");
  CheckEqual(m.Data(MAC3), 300, "MAC3 is the z component");
  CheckEqual(m.Data(IR1), 100, "IR1 follows MAC1");
  CheckEqual(m.Data(SZ3), 300, "SZ3 is the depth");

  BeginTest("RTPS applies the translation vector");
  m.Reset();
  m.SetIdentityRotation();
  m.SetControl(TRX, 10);
  m.SetControl(TRY, 20);
  m.SetControl(TRZ, 30);
  m.SetData(VXY0, Pack16(1, 2));
  m.SetData(VZ0, 3);
  m.SetControl(H, 1000);
  m.Run(Command(0x01, true));
  CheckEqual(m.Data(MAC1), 11, "translation added to x");
  CheckEqual(m.Data(MAC2), 22, "translation added to y");
  CheckEqual(m.Data(MAC3), 33, "translation added to z");

  BeginTest("RTPS pushes the depth FIFO");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(SZ1, 1);
  m.SetData(SZ2, 2);
  m.SetData(SZ3, 3);
  m.SetData(VZ0, 400);
  m.SetControl(H, 1000);
  m.Run(Command(0x01, true));
  CheckEqual(m.Data(SZ0), 1, "SZ1 moved down");
  CheckEqual(m.Data(SZ1), 2, "SZ2 moved down");
  CheckEqual(m.Data(SZ2), 3, "SZ3 moved down");
  CheckEqual(m.Data(SZ3), 400, "the new depth is on top");

  BeginTest("RTPS applies the screen offset");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(0, 0));
  m.SetData(VZ0, 1000);
  m.SetControl(H, 1000);
  m.SetControl(OFX, 320 << 16);              // OFX is 1.15.16 fixed point
  m.SetControl(OFY, 240 << 16);
  m.Run(Command(0x01, true));
  CheckEqual(m.Data(SXY2), Pack16(320, 240),
             "a vertex on the axis lands at the offset");

  BeginTest("RTPS divides by the depth");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(100, 0));
  m.SetData(VZ0, 1000);
  m.SetControl(H, 1000);                     // H == Z, so the scale is 1
  m.SetControl(OFX, 0);
  m.SetControl(OFY, 0);
  m.Run(Command(0x01, true));
  CheckEqual(m.Data(SXY2) & 0xFFFF, 100u,
             "with H equal to Z the x coordinate passes through");

  BeginTest("the divide overflows when H is at least twice the depth");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(1, 0));
  m.SetData(VZ0, 100);
  m.SetControl(H, 1000);                     // far more than 2 * 100
  m.Run(Command(0x01, true));
  Check((m.Control(FLAG) & (1u << 17)) != 0, "divide overflow flagged");

  BeginTest("RTPS computes IR0 from DQA and DQB, RTPT does not");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VZ0, 1000);
  m.SetControl(H, 1000);
  m.SetControl(DQA, 0);
  m.SetControl(DQB, 0x1000);
  m.Run(Command(0x01, true));
  CheckEqual(m.Data(IR0), 1u, "IR0 is MAC0 >> 12");

  BeginTest("RTPT transforms all three vertices");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(1, 1));
  m.SetData(VZ0, 100);
  m.SetData(VXY1, Pack16(2, 2));
  m.SetData(VZ1, 200);
  m.SetData(VXY2, Pack16(3, 3));
  m.SetData(VZ2, 300);
  m.SetControl(H, 100);
  m.Run(Command(0x30, true));                // RTPT
  CheckEqual(m.Data(SZ1), 100, "the first vertex's depth");
  CheckEqual(m.Data(SZ2), 200, "the second");
  CheckEqual(m.Data(SZ3), 300, "the third");
  CheckEqual(m.Data(MAC3), 300, "MAC3 holds the last vertex");
}

void TestNclip(Machine& m) {
  BeginTest("NCLIP computes the signed area of the screen triangle");
  m.Reset();
  // A triangle with a known cross product: (0,0), (10,0), (0,10).
  m.SetData(SXY0, Pack16(0, 0));
  m.SetData(SXY1, Pack16(10, 0));
  m.SetData(SXY2, Pack16(0, 10));
  m.Run(Command(0x06));
  // sx0*sy1 + sx1*sy2 + sx2*sy0 - sx0*sy2 - sx1*sy0 - sx2*sy1
  //   = 0 + 100 + 0 - 0 - 0 - 0 = 100
  CheckEqual(m.Data(MAC0), 100, "the area");

  BeginTest("NCLIP changes sign with the winding");
  m.Reset();
  m.SetData(SXY0, Pack16(0, 0));
  m.SetData(SXY1, Pack16(0, 10));
  m.SetData(SXY2, Pack16(10, 0));
  m.Run(Command(0x06));
  CheckEqual(m.Data(MAC0), static_cast<uint32_t>(-100),
             "the opposite winding is negative");

  BeginTest("NCLIP is zero for a degenerate triangle");
  m.Reset();
  m.SetData(SXY0, Pack16(5, 5));
  m.SetData(SXY1, Pack16(5, 5));
  m.SetData(SXY2, Pack16(5, 5));
  m.Run(Command(0x06));
  CheckEqual(m.Data(MAC0), 0u, "no area");
}

void TestAverageZ(Machine& m) {
  BeginTest("AVSZ3 averages the newest three depths");
  m.Reset();
  m.SetData(SZ0, 1000);            // not included
  m.SetData(SZ1, 100);
  m.SetData(SZ2, 200);
  m.SetData(SZ3, 300);
  m.SetControl(ZSF3, 0x1000 / 3);  // a third, in 1.3.12
  m.Run(Command(0x2D));
  const uint32_t expected = (0x1000 / 3) * 600;
  CheckEqual(m.Data(MAC0), expected, "MAC0 is ZSF3 * (SZ1+SZ2+SZ3)");
  CheckEqual(m.Data(OTZ), expected >> 12, "OTZ is MAC0 >> 12");

  BeginTest("AVSZ4 averages all four");
  m.Reset();
  m.SetData(SZ0, 100);
  m.SetData(SZ1, 200);
  m.SetData(SZ2, 300);
  m.SetData(SZ3, 400);
  m.SetControl(ZSF4, 0x1000 / 4);
  m.Run(Command(0x2E));
  const uint32_t expected4 = (0x1000 / 4) * 1000;
  CheckEqual(m.Data(MAC0), expected4, "MAC0 is ZSF4 * (SZ0+SZ1+SZ2+SZ3)");
  CheckEqual(m.Data(OTZ), expected4 >> 12, "OTZ");

  BeginTest("a negative average saturates OTZ to zero");
  m.Reset();
  m.SetData(SZ1, 100);
  m.SetData(SZ2, 100);
  m.SetData(SZ3, 100);
  m.SetControl(ZSF3, static_cast<uint32_t>(-0x1000));
  m.Run(Command(0x2D));
  CheckEqual(m.Data(OTZ), 0u, "OTZ clamped to 0");
  Check((m.Control(FLAG) & (1u << 18)) != 0, "and flagged");
}

void TestArithmetic(Machine& m) {
  BeginTest("SQR squares IR1-3");
  m.Reset();
  m.SetData(IR1, 4);
  m.SetData(IR2, 5);
  m.SetData(IR3, 6);
  m.Run(Command(0x28));                      // sf = 0
  CheckEqual(m.Data(MAC1), 16, "IR1 squared");
  CheckEqual(m.Data(MAC2), 25, "IR2 squared");
  CheckEqual(m.Data(MAC3), 36, "IR3 squared");
  CheckEqual(m.Data(IR1), 16, "IR1 follows MAC1");

  BeginTest("SQR of a negative is positive");
  m.Reset();
  m.SetData(IR1, 0xFFFFFFFC);                // -4
  m.Run(Command(0x28));
  CheckEqual(m.Data(MAC1), 16, "(-4) squared");

  BeginTest("OP is the cross product of IR and the rotation diagonal");
  m.Reset();
  // D1 = RT11, D2 = RT22, D3 = RT33.
  m.SetControl(RT11RT12, Pack16(2, 0));
  m.SetControl(RT22RT23, Pack16(3, 0));
  m.SetControl(RT33, 4);
  m.SetData(IR1, 5);
  m.SetData(IR2, 6);
  m.SetData(IR3, 7);
  m.Run(Command(0x0C));                      // sf = 0
  CheckEqual(m.Data(MAC1), static_cast<uint32_t>(3 * 7 - 4 * 6), "D2*IR3 - D3*IR2");
  CheckEqual(m.Data(MAC2), static_cast<uint32_t>(4 * 5 - 2 * 7), "D3*IR1 - D1*IR3");
  CheckEqual(m.Data(MAC3), static_cast<uint32_t>(2 * 6 - 3 * 5), "D1*IR2 - D2*IR1");

  BeginTest("GPF scales IR1-3 by IR0");
  m.Reset();
  m.SetData(IR0, 3);
  m.SetData(IR1, 10);
  m.SetData(IR2, 20);
  m.SetData(IR3, 30);
  m.Run(Command(0x3D));                      // sf = 0
  CheckEqual(m.Data(MAC1), 30, "IR0 * IR1");
  CheckEqual(m.Data(MAC2), 60, "IR0 * IR2");
  CheckEqual(m.Data(MAC3), 90, "IR0 * IR3");

  BeginTest("GPF pushes the colour FIFO");
  m.Reset();
  m.SetData(RGBC, 0x21000000);               // CODE = 0x21
  m.SetData(IR0, 0x10);
  m.SetData(IR1, 0x10);
  m.SetData(IR2, 0x20);
  m.SetData(IR3, 0x30);
  m.Run(Command(0x3D));
  // MAC / 16 is the colour, and CODE passes through untouched.
  CheckEqual(m.Data(RGB2) >> 24, 0x21u, "CODE carried into the FIFO");

  BeginTest("GPL adds to the existing accumulator");
  m.Reset();
  m.SetData(MAC1, 100);
  m.SetData(MAC2, 200);
  m.SetData(MAC3, 300);
  m.SetData(IR0, 2);
  m.SetData(IR1, 1);
  m.SetData(IR2, 2);
  m.SetData(IR3, 3);
  m.Run(Command(0x3E));                      // sf = 0, so no pre-shift
  CheckEqual(m.Data(MAC1), 102, "MAC1 + IR0*IR1");
  CheckEqual(m.Data(MAC2), 204, "MAC2 + IR0*IR2");
  CheckEqual(m.Data(MAC3), 306, "MAC3 + IR0*IR3");
}

void TestMvmva(Machine& m) {
  BeginTest("MVMVA multiplies the rotation matrix by V0");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY0, Pack16(7, 8));
  m.SetData(VZ0, 9);
  m.Run(Command(0x12, true, false, 0, 0, 3));   // rotation, V0, no translation
  CheckEqual(m.Data(MAC1), 7, "x passes through the identity");
  CheckEqual(m.Data(MAC2), 8, "y");
  CheckEqual(m.Data(MAC3), 9, "z");

  BeginTest("MVMVA can select V1 and V2");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY1, Pack16(11, 12));
  m.SetData(VZ1, 13);
  m.Run(Command(0x12, true, false, 0, 1, 3));
  CheckEqual(m.Data(MAC1), 11, "V1 selected");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VXY2, Pack16(21, 22));
  m.SetData(VZ2, 23);
  m.Run(Command(0x12, true, false, 0, 2, 3));
  CheckEqual(m.Data(MAC1), 21, "V2 selected");

  BeginTest("MVMVA can use IR as its vector");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(IR1, 31);
  m.SetData(IR2, 32);
  m.SetData(IR3, 33);
  m.Run(Command(0x12, true, false, 0, 3, 3));
  CheckEqual(m.Data(MAC1), 31, "IR selected");

  BeginTest("MVMVA adds the chosen translation vector");
  m.Reset();
  m.SetIdentityRotation();
  m.SetControl(TRX, 5);
  m.SetData(VXY0, Pack16(1, 0));
  m.Run(Command(0x12, true, false, 0, 0, 0));   // translation = TR
  CheckEqual(m.Data(MAC1), 6, "TR added");

  m.Reset();
  m.SetIdentityRotation();
  m.SetControl(RBK, 5);
  m.SetData(VXY0, Pack16(1, 0));
  m.Run(Command(0x12, true, false, 0, 0, 1));   // translation = BK
  CheckEqual(m.Data(MAC1), 6, "BK added");

  BeginTest("MVMVA with the far colour is the documented broken case");
  // The translation and the first product are computed only to set flags and
  // are then discarded, so the result is the last two products alone.
  m.Reset();
  m.SetIdentityRotation();
  m.SetControl(RFC, 0x7FFFFF);
  m.SetData(VXY0, Pack16(1, 1));
  m.SetData(VZ0, 1);
  m.Run(Command(0x12, true, false, 0, 0, 2));   // translation = FC
  CheckEqual(m.Data(MAC1), 0u,
             "the first row keeps only M12*Vy + M13*Vz, which is zero here");
}

void TestColour(Machine& m) {
  BeginTest("NCS runs the light and colour matrices");
  m.Reset();
  // Light matrix identity, colour matrix identity, no background.
  m.SetControl(L11L12, Pack16(0x1000, 0));
  m.SetControl(L13L21, Pack16(0, 0));
  m.SetControl(L22L23, Pack16(0x1000, 0));
  m.SetControl(L31L32, Pack16(0, 0));
  m.SetControl(L33, 0x1000);
  m.SetControl(LR1LR2, Pack16(0x1000, 0));
  m.SetControl(LR3LG1, Pack16(0, 0));
  m.SetControl(LG2LG3, Pack16(0x1000, 0));
  m.SetControl(LB1LB2, Pack16(0, 0));
  m.SetControl(LB3, 0x1000);
  m.SetData(VXY0, Pack16(0x100, 0x200));
  m.SetData(VZ0, 0x300);
  m.Run(Command(0x1E, true));                // NCS
  CheckEqual(m.Data(MAC1), 0x100, "the light vector passes through");
  CheckEqual(m.Data(IR1), 0x100, "IR1 follows");

  BeginTest("the colour FIFO carries CODE through unchanged");
  m.Reset();
  m.SetData(RGBC, 0x5A000000);
  m.SetData(IR0, 0);
  m.Run(Command(0x3D));                      // GPF, which pushes a colour
  CheckEqual(m.Data(RGB2) >> 24, 0x5Au, "CODE preserved");

  BeginTest("the colour FIFO shifts on every push");
  m.Reset();
  m.SetData(RGB0, 0x00111111);
  m.SetData(RGB1, 0x00222222);
  m.SetData(RGB2, 0x00333333);
  m.SetData(IR0, 0);
  m.Run(Command(0x3D));
  CheckEqual(m.Data(RGB0), 0x00222222u, "RGB1 moved down");
  CheckEqual(m.Data(RGB1), 0x00333333u, "RGB2 moved down");

  BeginTest("colour components saturate to 0..255 and flag");
  m.Reset();
  m.SetData(IR0, 0x7FFF);
  m.SetData(IR1, 0x7FFF);
  m.Run(Command(0x3D));                      // GPF with a huge product
  Check((m.Control(FLAG) & (1u << 21)) != 0, "the red channel flagged");
  CheckEqual(m.Data(RGB2) & 0xFF, 0xFFu, "red clamped to 255");
}

// What amidog's psxtest_gte OPCODE group caught (bug 79): the translated
// products check the 44-bit accumulator after every partial sum, and RTPS's
// IR0 saturates from the full depth-cue sum rather than from wrapped MAC0.
void TestOpcode(Machine& m) {
  // TRX*1000h = 2^43 - 4096. Adding RT11*VX = 7FFFh*7FFFh = 3FFF0001h passes
  // 2^43 - 1, so the first partial sum overflows positive and wraps to about
  // -2^43. Adding RT12*VY = -8000h*7FFFh = -3FFF8000h then passes -2^43 the
  // other way. The final sum, 2^43 - 36863, is back in range - so a check made
  // only at the end sees nothing, while the hardware has set both bits.
  const uint32_t kBothWays = (1u << 30) | (1u << 27);

  BeginTest("MVMVA checks the accumulator after each partial sum");
  m.Reset();
  m.SetControl(RT11RT12, Pack16(0x7FFF, -0x8000));
  m.SetControl(TRX, 0x7FFFFFFF);
  m.SetData(VXY0, Pack16(0x7FFF, 0x7FFF));
  m.Run(Command(0x12, true, false, 0, 0, 0));   // RT * V0 + TR, sf=1
  CheckEqual(m.Control(FLAG), 0x80000000u | kBothWays | (1u << 24),
             "MAC1 overflowed both ways on the way, and IR1 saturated");
  CheckEqual(m.Data(MAC1), 0x7FFFFFF7u,
             "the value is the true sum, (2^43 - 36863) >> 12");

  BeginTest("RTPS checks the accumulator after each partial sum");
  m.Reset();
  m.SetControl(RT11RT12, Pack16(0x7FFF, -0x8000));
  m.SetControl(TRX, 0x7FFFFFFF);
  m.SetData(VXY0, Pack16(0x7FFF, 0x7FFF));
  m.SetControl(H, 1000);
  m.Run(Command(0x01));                          // sf=0, the OPCODE group's form
  CheckEqual(m.Control(FLAG) & kBothWays, kBothWays,
             "MAC1 overflowed both ways on the way");

  BeginTest("NCS's background-colour step checks each partial sum");
  m.Reset();
  // The light matrix passes V0 through (sf=0), leaving IR1 = IR2 = 7FFFh for
  // the colour matrix, whose first row and RBK repeat the numbers above.
  m.SetControl(L11L12, Pack16(1, 0));
  m.SetControl(L22L23, Pack16(1, 0));
  m.SetControl(LR1LR2, Pack16(0x7FFF, -0x8000));
  m.SetControl(RBK, 0x7FFFFFFF);
  m.SetData(VXY0, Pack16(0x7FFF, 0x7FFF));
  m.Run(Command(0x1E));                          // NCS, sf=0
  CheckEqual(m.Control(FLAG) & kBothWays, kBothWays,
             "MAC1 overflowed both ways on the way");

  BeginTest("RTPS takes IR0 from the full depth-cue sum, not wrapped MAC0");
  m.Reset();
  m.SetIdentityRotation();
  m.SetData(VZ0, 1000);
  m.SetControl(H, 1000);                         // the divide gives about 10000h
  m.SetControl(DQA, 0x7FFF);
  m.SetControl(DQB, 0x7FFFFFFF);
  m.Run(Command(0x01, true));
  // 10000h*7FFFh + 7FFFFFFFh is about 2^32: MAC0 overflows and wraps to a
  // small negative number, but IR0 is that sum >> 12, far above 1000h.
  Check((m.Control(FLAG) & (1u << 16)) != 0, "MAC0 overflow flagged");
  CheckEqual(m.Data(IR0), 0x1000u, "IR0 saturates high, not low");
  Check((m.Control(FLAG) & (1u << 12)) != 0, "and IR0 saturation flagged");
}

void TestUnknown(Machine& m) {
  BeginTest("an unrecognised command is counted rather than ignored");
  m.Reset();
  const uint64_t before = m.gte().stats().unknown_commands;
  m.Run(Command(0x02));                      // not a real opcode
  Check(m.gte().stats().unknown_commands == before + 1,
        "the unknown command was counted");
}

// ---------------------------------------------------------------------------
// The register file as compiled code reaches it (Gte::RegisterHome)
// ---------------------------------------------------------------------------

// Every register's state, as ReadData and ReadControl give it.
struct Snapshot {
  uint32_t data[32];
  uint32_t control[32];
  bool operator==(const Snapshot& other) const {
    return memcmp(this, &other, sizeof(*this)) == 0;
  }
};

Snapshot Take(Machine& m) {
  Snapshot s;
  for (uint32_t i = 0; i < 32; ++i) {
    s.data[i] = m.Data(i);
    s.control[i] = m.Control(i);
  }
  return s;
}

// The same arbitrary state each time for a given seed, every register written.
void Scramble(Machine& m, uint32_t seed) {
  uint32_t x = seed * 2654435761u + 1;
  auto next = [&x]() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; };
  for (uint32_t i = 0; i < 32; ++i) {
    m.SetControl(i, next());
    m.SetData(i, next());
  }
}

// Compiled code moves a register by loading or storing it at RegisterHome's offset into the Gte, of
// the width and sign it says, where ReadData and the rest would have gone through a switch. So each
// register said to be plain has to read, by its home, what ReadData reads - and written by its home,
// leave every register as WriteData would have left it.
void TestCompiledHomes(Machine& m) {
  BeginTest("each register read by its home reads what ReadData and ReadControl read");
  uint8_t* const base = reinterpret_cast<uint8_t*>(&m.gte());
  int plain_reads = 0, plain_writes = 0, misread = 0, miswritten = 0;
  for (uint32_t seed = 0; seed < 4; ++seed) {
    for (uint32_t i = 0; i < 64; ++i) {
      const bool control = i >= 32;
      const uint32_t index = i & 31;
      const Gte::Home home = m.gte().RegisterHome(control, index);
      if (home.read == Gte::Home::kComputed)
        continue;
      Scramble(m, seed);
      uint32_t raw = 0;
      if (home.read == Gte::Home::kWord) {
        memcpy(&raw, base + home.offset, 4);
      } else {
        uint16_t half = 0;
        memcpy(&half, base + home.offset, 2);
        raw = home.read == Gte::Home::kSigned16
                  ? static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(half)))
                  : half;
      }
      const uint32_t expected = control ? m.Control(index) : m.Data(index);
      if (raw != expected) {
        printf("  %s %u by its home: %08X, read %08X\n", control ? "control" : "data", index, raw, expected);
        ++misread;
      }
      if (seed == 0)
        ++plain_reads;
    }
  }
  CheckEqual(static_cast<uint32_t>(misread), 0u, "every plain register, in four states, reads the same by its home");

  BeginTest("each register written by its home leaves what WriteData and WriteControl leave");
  const uint32_t values[] = { 0x8001F234u, 0x00017FFFu, 0xFFFFFFFFu, 0x12345678u, 0 };
  for (uint32_t i = 0; i < 64; ++i) {
    const bool control = i >= 32;
    const uint32_t index = i & 31;
    const Gte::Home home = m.gte().RegisterHome(control, index);
    if (home.write == Gte::Home::kComputed)
      continue;
    ++plain_writes;
    for (uint32_t value : values) {
      Scramble(m, i);
      if (home.write == Gte::Home::kWord)
        memcpy(base + home.offset, &value, 4);
      else if (home.write == Gte::Home::kHalf)
        memcpy(base + home.offset, &value, 2);
      if (home.cleared >= 0)
        memset(base + home.cleared, 0, 4);
      const Snapshot by_home = Take(m);
      Scramble(m, i);
      if (control)
        m.SetControl(index, value);
      else
        m.SetData(index, value);
      const Snapshot by_write = Take(m);
      if (!(by_home == by_write)) {
        printf("  %s %u <- %08X by its home differs from the write\n", control ? "control" : "data", index, value);
        ++miswritten;
      }
    }
  }
  CheckEqual(static_cast<uint32_t>(miswritten), 0u, "every plain register, with five values, writes the same by its home");
  // Which registers are plain is a fact about the hardware, not about this test: these are the
  // counts the layout comments in gte.cpp describe (data 28 and 29 are worked out when read; SXYP,
  // the SXY entries, IRGB, LZCS and FLAG when written).
  CheckEqual(static_cast<uint32_t>(plain_reads), 62u, "sixty-two registers read as their storage");
  CheckEqual(static_cast<uint32_t>(plain_writes), 57u, "fifty-seven written as it");

  BeginTest("a command run by its own function is what Execute runs, at Execute's cost");
  const uint32_t opcodes[] = { 0x01, 0x06, 0x0C, 0x10, 0x11, 0x12, 0x13, 0x14, 0x16, 0x1B, 0x1C,
                               0x1E, 0x20, 0x28, 0x29, 0x2A, 0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F, 0x02 };
  for (uint32_t opcode : opcodes) {
    const uint32_t command = Command(opcode, true, false, 1, 2, 0);
    Scramble(m, opcode);
    const uint32_t cycles = m.gte().Execute(command);
    const Snapshot executed = Take(m);
    Scramble(m, opcode);
    Gte::CommandFunction(opcode)(&m.gte(), command);
    const Snapshot direct = Take(m);
    Check(executed == direct, "the same registers either way");
    CheckEqual(Gte::CommandCycles(opcode), cycles, "and the same cost");
  }
  CheckEqual(Gte::CommandCycles(0x30), 23u, "RTPT is 23 cycles");
  CheckEqual(Gte::CommandCycles(0x16), 44u, "NCDT is 44");
  CheckEqual(Gte::CommandCycles(0x02), 1u, "and a word that is not a command costs one");
}

// ---------------------------------------------------------------------------
// The commands emitted as x64 (rec/gte_compiler.h)
// ---------------------------------------------------------------------------

// A value for a register, biased toward where the arithmetic has edges: each half often one of
// the 16-bit limits, the word sometimes one of the 32-bit ones, and small numbers, which are most
// of what games use.
//
// A third of the states are instead all moderate - halves within +-2048, words within +-2^20, what
// a game's coordinates and colours look like - because at the edges nearly everything saturates,
// and a result that always clamps proves nothing about the arithmetic before the clamp (AVSZ's
// shift went uncaught by edge values alone).
struct Values {
  uint32_t x;
  bool moderate;
  explicit Values(uint32_t seed) : x(seed * 2654435761u + 0x9E3779B9u), moderate(seed % 3 == 0) {}
  uint32_t Next() { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return x; }
  uint16_t Half() {
    if (moderate)
      return static_cast<uint16_t>(static_cast<int16_t>(Next() % 4096) - 2048);
    static const uint16_t kEdges[] = { 0, 1, 0xFFFF, 0x7FFF, 0x8000, 0x1000, 0xF000, 0x00FF, 0x0100, 0x8001 };
    const uint32_t pick = Next() % 4;
    if (pick == 0)
      return kEdges[Next() % (sizeof(kEdges) / sizeof(kEdges[0]))];
    if (pick == 1)
      return static_cast<uint16_t>(static_cast<int16_t>(Next() % 512) - 256);
    return static_cast<uint16_t>(Next());
  }
  uint32_t Word() {
    if (moderate) {
      if (Next() % 2)
        return Half() | (static_cast<uint32_t>(Half()) << 16);
      return static_cast<uint32_t>(static_cast<int32_t>(Next() % 0x200000) - 0x100000);
    }
    static const uint32_t kEdges[] = { 0x7FFFFFFFu, 0x80000000u, 0, 0xFFFFFFFFu, 0x00010000u, 0x7FFF0000u };
    switch (Next() % 4) {
      case 0: return kEdges[Next() % (sizeof(kEdges) / sizeof(kEdges[0]))];
      case 1: return static_cast<uint32_t>(static_cast<int32_t>(Next() % 0x20000) - 0x10000);
      case 2: return Half() | (static_cast<uint32_t>(Half()) << 16);
      default: return Next();
    }
  }
};

// Every register given a value from `seed`, the same each time - IR1-3 last, since writing IRGB
// would otherwise leave them only its 5-bit steps.
void Fill(Machine& m, uint32_t seed) {
  Values values(seed);
  for (uint32_t i = 0; i < 31; ++i)
    m.SetControl(i, values.Word());
  for (uint32_t i = 0; i < 32; ++i) {
    if (i == 15 || i == 28 || i == 29 || i == 31 || (i >= 8 && i <= 11))
      continue;
    m.SetData(i, values.Word());
  }
  for (uint32_t i = 8; i <= 11; ++i)
    m.SetData(i, values.Word());
}

struct Counts {
  uint64_t commands, unknown;
  uint32_t executed[64];
};
Counts TakeCounts(Machine& m) {
  Counts c;
  c.commands = m.gte().stats().commands;
  c.unknown = m.gte().stats().unknown_commands;
  memcpy(c.executed, m.gte().stats().executed, sizeof(c.executed));
  return c;
}

// The bytes the assembler gives for one instruction, as hex.
template <typename Emit>
std::string Encoded(Emit emit) {
  emulation::rec::Emitter emitter;
  emulation::rec::CodeBlock* block = emitter.create_block(4096);
  emitter.set_block(block);
  emulation::rec::x64::Asm a(&emitter);
  emit(a);
  std::string hex;
  char byte[4];
  for (size_t i = 0; i < block->cursor; ++i) {
    snprintf(byte, sizeof(byte), "%02X", block->ptr8bit[i]);
    if (!hex.empty())
      hex += ' ';
    hex += byte;
  }
  emitter.destroy_block(block);
  return hex;
}

void CheckBytes(const std::string& got, const char* expected, const char* what) {
  ++g_checks;
  if (got != expected) {
    ++g_failures;
    printf("  FAIL  %s / %s\n        got %s, expected %s\n", g_test.c_str(), what, got.c_str(), expected);
  }
}

void TestX64(Machine& m) {
  using namespace emulation::rec::x64;
  BeginTest("the assembler encodes the forms the commands use");
  CheckBytes(Encoded([](Asm& a) { a.mov(32, r8, At(r10, 0x1234)); }), "45 8B 82 34 12 00 00", "mov r8d, [r10+1234h]");
  CheckBytes(Encoded([](Asm& a) { a.movsx16(64, r9, At(r10, 8)); }), "4D 0F BF 4A 08", "movsx r9, word [r10+8]");
  CheckBytes(Encoded([](Asm& a) { a.movzx8(rax, At(rcx, rax, 1)); }), "0F B6 04 01", "movzx eax, byte [rcx+rax]");
  CheckBytes(Encoded([](Asm& a) { a.movzx16(r9, At(r10, 0)); }), "45 0F B7 0A", "movzx r9d, word [r10]");
  CheckBytes(Encoded([](Asm& a) { a.imul(64, rax, rdx); }), "48 0F AF C2", "imul rax, rdx");
  CheckBytes(Encoded([](Asm& a) { a.imul(32, r9, rax); }), "44 0F AF C8", "imul r9d, eax");
  CheckBytes(Encoded([](Asm& a) { a.mov(16, At(r10, 4), rax); }), "66 41 89 42 04", "mov word [r10+4], ax");
  CheckBytes(Encoded([](Asm& a) { a.mov(8, At(r10, 3), rax); }), "41 88 42 03", "mov byte [r10+3], al");
  CheckBytes(Encoded([](Asm& a) { a.alu(Asm::kAdd, 64, At(r10, 16), 1); }), "49 83 42 10 01", "add qword [r10+16], 1");
  CheckBytes(Encoded([](Asm& a) { a.sar(64, r8, 12); }), "49 C1 F8 0C", "sar r8, 12");
  CheckBytes(Encoded([](Asm& a) { a.cmov(kA, 64, rax, rcx); }), "48 0F 47 C1", "cmova rax, rcx");
  CheckBytes(Encoded([](Asm& a) { a.bsr(32, rax, r9); }), "41 0F BD C1", "bsr eax, r9d");
  CheckBytes(Encoded([](Asm& a) { a.shift_cl(Asm::kShl, 32, rdx); }), "D3 E2", "shl edx, cl");
  CheckBytes(Encoded([](Asm& a) { a.movsxd(rax, r8); }), "49 63 C0", "movsxd rax, r8d");
  CheckBytes(Encoded([](Asm& a) { a.movsxd(rdx, At(r10, 0x40)); }), "49 63 52 40", "movsxd rdx, [r10+40h]");
  CheckBytes(Encoded([](Asm& a) { a.alu(Asm::kCmp, 32, rax, -0x8000); }), "81 F8 00 80 FF FF", "cmp eax, -8000h");
  CheckBytes(Encoded([](Asm& a) { a.alu(Asm::kOr, 32, r11, 0x40000000); }), "41 81 CB 00 00 00 40", "or r11d, 40000000h");
  CheckBytes(Encoded([](Asm& a) { a.mov_imm(8, At(r10, 5), 1); }), "41 C6 42 05 01", "mov byte [r10+5], 1");
  CheckBytes(Encoded([](Asm& a) { a.mov_imm(32, rcx, 15); }), "B9 0F 00 00 00", "mov ecx, 15");
  CheckBytes(Encoded([](Asm& a) { a.neg(64, rdx); }), "48 F7 DA", "neg rdx");
  CheckBytes(Encoded([](Asm& a) { a.jmp(rax); }), "FF E0", "jmp rax");
  CheckBytes(Encoded([](Asm& a) { a.mov(32, At(r13, 0), rax); }), "41 89 45 00", "mov [r13], eax (r13 needs a disp8)");
  CheckBytes(Encoded([](Asm& a) { a.mov(32, rax, At(r12, 8)); }), "41 8B 44 24 08", "mov eax, [r12+8] (r12 needs a SIB)");
  CheckBytes(Encoded([](Asm& a) { Label l; a.jcc(kE, l); a.bind(l); }), "0F 84 00 00 00 00", "je to the next instruction");

  BeginTest("every command, emitted as x64, leaves every register as the C++ does");
  emulation::rec::GteLayout layout;
  emulation::psx::RecompilerBridge::DescribeGte(m.gte(), &layout);
  emulation::rec::GteCompiler compiler(&layout);
  Check(compiler.available(), "the layout describes enough to emit from");

  const uint32_t opcodes[] = { 0x01, 0x06, 0x0C, 0x10, 0x11, 0x13, 0x14, 0x16, 0x1B, 0x1C, 0x1E,
                               0x20, 0x28, 0x29, 0x2A, 0x2D, 0x2E, 0x30, 0x3D, 0x3E, 0x3F,
                               0x00, 0x02, 0x3C };   // and three that are not commands
  struct Form { uint32_t word; int states; };
  std::vector<Form> forms;
  for (uint32_t opcode : opcodes)
    for (uint32_t sf = 0; sf < 2; ++sf)
      for (uint32_t lm = 0; lm < 2; ++lm)
        forms.push_back({ Command(opcode, sf != 0, lm != 0), 3000 });
  for (uint32_t mx = 0; mx < 4; ++mx)
    for (uint32_t v = 0; v < 4; ++v)
      for (uint32_t tx = 0; tx < 4; ++tx)
        for (uint32_t sf = 0; sf < 2; ++sf)
          for (uint32_t lm = 0; lm < 2; ++lm)
            forms.push_back({ Command(0x12, sf != 0, lm != 0, mx, v, tx), 400 });

  int runs = 0, differing = 0, missing = 0, flags_seen = 0;
  uint32_t flag_union = 0;
  for (const Form& form : forms) {
    const emulation::rec::GteCommandFn emitted = compiler.Function(form.word);
    if (emitted == nullptr) {
      ++missing;
      continue;
    }
    for (int s = 0; s < form.states; ++s) {
      const uint32_t seed = form.word * 7919u + static_cast<uint32_t>(s);
      // The bits the command ignores (20-24) vary too.
      const uint32_t word = form.word ^ ((seed & 0x1F) << 20);
      Fill(m, seed);
      const Counts before_cpp = TakeCounts(m);
      m.gte().Execute(word);
      const Snapshot cpp = Take(m);
      const Counts after_cpp = TakeCounts(m);
      Fill(m, seed);
      const Counts before_x64 = TakeCounts(m);
      emitted(&m.gte(), word);
      const Snapshot x64 = Take(m);
      const Counts after_x64 = TakeCounts(m);
      ++runs;
      bool counted_alike = after_cpp.commands - before_cpp.commands == after_x64.commands - before_x64.commands &&
                           after_cpp.unknown - before_cpp.unknown == after_x64.unknown - before_x64.unknown;
      for (int i = 0; i < 64 && counted_alike; ++i)
        counted_alike = after_cpp.executed[i] - before_cpp.executed[i] == after_x64.executed[i] - before_x64.executed[i];
      if (!(cpp == x64) || !counted_alike) {
        if (++differing <= 6) {
          printf("  word %08X state %d differs:", word, s);
          for (int i = 0; i < 32; ++i) {
            if (cpp.data[i] != x64.data[i])
              printf(" data%d %08X/%08X", i, cpp.data[i], x64.data[i]);
            if (cpp.control[i] != x64.control[i])
              printf(" ctrl%d %08X/%08X", i, cpp.control[i], x64.control[i]);
          }
          printf(counted_alike ? " (C++/x64)\n" : " and the counts (C++/x64)\n");
        }
      }
      flag_union |= cpp.control[31];
      flags_seen += cpp.control[31] != 0;
    }
  }
  CheckEqual(static_cast<uint32_t>(missing), 0u, "a function for every form");
  CheckEqual(static_cast<uint32_t>(differing), 0u, "no state of any form differs in any register or count");
  CheckEqual(static_cast<uint32_t>(runs), 24u * 4u * 3000u + 256u * 400u, "and that is every form's every state");
  // The states reach every flag the commands can set - a sweep that never overflowed would prove
  // nothing about overflow.
  CheckEqual(flag_union, 0xFFFFF000u, "every FLAG bit was set by some run");
  printf("  %d runs, %d with flags, %zu command words, %zu bytes of x64\n", runs, flags_seen,
         compiler.functions(), compiler.bytes());

  BeginTest("with PGXP on, RTPS, RTPT and NCLIP are the C++'s, unrounded positions and all");
  m.gte().set_pgxp(true, true);
  int pgxp_differing = 0;
  for (uint32_t opcode : { 0x01u, 0x30u, 0x06u }) {
    const uint32_t word = Command(opcode, true, false);
    const emulation::rec::GteCommandFn emitted = compiler.Function(word);
    for (int s = 0; s < 200; ++s) {
      Fill(m, 0xC0FFEEu + static_cast<uint32_t>(s));
      m.gte().Execute(Command(0x30, true, false));   // projected vertices, so there are positions to keep
      m.gte().Execute(word);
      const Snapshot cpp = Take(m);
      const emulation::psx::PreciseVertex p_cpp = m.gte().Precise(14);
      Fill(m, 0xC0FFEEu + static_cast<uint32_t>(s));
      m.gte().Execute(Command(0x30, true, false));
      emitted(&m.gte(), word);
      const Snapshot x64 = Take(m);
      const emulation::psx::PreciseVertex p_x64 = m.gte().Precise(14);
      if (!(cpp == x64) || p_cpp.valid != p_x64.valid || p_cpp.x != p_x64.x || p_cpp.y != p_x64.y)
        ++pgxp_differing;
    }
  }
  m.gte().set_pgxp(false, false);
  CheckEqual(static_cast<uint32_t>(pgxp_differing), 0u, "the same registers and the same positions");
}

struct Group {
  const char* name;
  void (*run)(Machine&);
};

// ---------------------------------------------------------------------------
// Motion for DLSS (psx/vertex_motion.h, Docs/DLSS-Plan.md, phase 2)
// ---------------------------------------------------------------------------

// Projects the vertex at model (x, y, z), 1000 from the eye with H 1000, moved by TRX `tx`, and
// returns PGXP's record of it.
emulation::psx::PreciseVertex Project(Machine& m, int16_t x, int16_t y, int16_t z, int32_t tx) {
  m.SetControl(TRX, static_cast<uint32_t>(tx));
  m.SetData(VXY0, Pack16(x, y));
  m.SetData(VZ0, static_cast<uint16_t>(z));
  m.Run(Command(0x01, true));
  return m.gte().Precise(SXY2);
}

bool Close(float value, float expected) { return value > expected - 0.01f && value < expected + 0.01f; }

void TestMotion(Machine& m) {
  m.Reset();
  m.SetIdentityRotation();
  m.SetControl(H, 1000);
  Gte& gte = m.gte();
  gte.set_pgxp(true, false);
  gte.set_motion(true);
  gte.set_motion_key(Gte::MotionKey::kModel);

  BeginTest("motion: the first picture knows nothing");
  Check(!Project(m, 100, 50, 1000, 0).moved, "a vertex not seen before has no motion");

  BeginTest("motion: the same vertex in the next picture, moved");
  gte.NewPicture();
  const emulation::psx::PreciseVertex moved = Project(m, 100, 50, 1000, 10);
  Check(moved.moved, "found in the last picture by its model's coordinates");
  Check(Close(moved.mx, -10.0f) && Close(moved.my, 0.0f),
        "where it was minus where it is: (-10, 0)");
  Check(!Project(m, 7, 8, 1000, 10).moved, "another vertex is not found under its key");

  BeginTest("motion: two of the same vertex, the nearer taken");
  gte.NewPicture();
  Project(m, 20, 20, 1000, 0);
  Project(m, 20, 20, 1000, 100);
  gte.NewPicture();
  const emulation::psx::PreciseVertex nearer = Project(m, 20, 20, 1000, 95);
  Check(nearer.moved && Close(nearer.mx, 5.0f), "the instance 5 away, not the one 95 away");

  BeginTest("motion: too far to be the same");
  gte.NewPicture();
  Project(m, 30, 30, 1000, 0);
  gte.NewPicture();
  Check(!Project(m, 30, 30, 1000, 300).moved, "300 pixels is not the same vertex moved");

  BeginTest("motion: a model the CPU moves itself, found by its place in a steady list");
  gte.ForgetMotion();
  for (int picture = 0; picture < 3; ++picture) {
    Project(m, 40, static_cast<int16_t>(40 + picture), 1000, 0);
    Project(m, 60, static_cast<int16_t>(40 + picture), 1000, 0);
    gte.NewPicture();
  }
  const emulation::psx::PreciseVertex waved = Project(m, 40, 43, 1000, 0);
  Check(waved.moved && Close(waved.mx, 0.0f) && Close(waved.my, -1.0f),
        "its coordinates changed, its place did not: found, (0, -1)");

  BeginTest("motion: forgotten");
  gte.ForgetMotion();
  Check(!Project(m, 100, 50, 1000, 10).moved, "nothing projected before is found after");

  gte.set_motion(false);
  gte.set_pgxp(false, false);
}

const Group kGroups[] = {
  { "registers",  TestRegisters },
  { "flags",      TestFlags },
  { "rtps",       TestRtps },
  { "nclip",      TestNclip },
  { "averagez",   TestAverageZ },
  { "arithmetic", TestArithmetic },
  { "mvmva",      TestMvmva },
  { "colour",     TestColour },
  { "opcode",     TestOpcode },
  { "unknown",    TestUnknown },
  { "motion",     TestMotion },
  { "homes",      TestCompiledHomes },
  { "x64",        TestX64 },
};

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* only = (argc > 1) ? argv[1] : nullptr;

  printf("gte_test - geometry transformation engine\n\n");

  Machine machine;
  for (size_t i = 0; i < sizeof(kGroups) / sizeof(kGroups[0]); ++i) {
    if (only != nullptr && strcmp(only, kGroups[i].name) != 0)
      continue;
    const int before = g_failures;
    printf("%s\n", kGroups[i].name);
    kGroups[i].run(machine);
    if (g_failures == before)
      printf("  ok\n");
  }

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

// cpu_test - unit tests for the R3000A core, the memory map, exceptions and
// the interrupt path.
//
//   cpu_test [group]
//
// Takes no BIOS, no window and no disc. Each test assembles a handful of MIPS
// instructions into RAM, runs them through the real CPU, and checks what came
// out. That is deliberately the same path a game takes - nothing here reaches
// around the emulation to poke at internals except to set up and inspect.
//
// This exists because every bug found in this core so far was found by
// archaeology through a BIOS trace: hours of disassembly to discover that BLEZ
// was assigning to its own operand. All of those are one assertion each here.

#include "psx/psx.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------------
// Assembler
// ---------------------------------------------------------------------------

enum Register {
  zero = 0, at, v0, v1, a0, a1, a2, a3,
  t0, t1, t2, t3, t4, t5, t6, t7,
  s0, s1, s2, s3, s4, s5, s6, s7,
  t8, t9, k0, k1, gp, sp, fp, ra
};

uint32_t RType(uint32_t op, uint32_t rs, uint32_t rt, uint32_t rd,
               uint32_t shamt, uint32_t funct) {
  return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (shamt << 6) |
         funct;
}

uint32_t IType(uint32_t op, uint32_t rs, uint32_t rt, uint32_t immediate) {
  return (op << 26) | (rs << 21) | (rt << 16) | (immediate & 0xFFFF);
}

uint32_t JType(uint32_t op, uint32_t target) {
  return (op << 26) | ((target >> 2) & 0x03FFFFFF);
}

// Only the forms the tests use, named the way the manual names them.
uint32_t NOP()                                   { return 0; }
uint32_t SLL(int d, int t, int sa)               { return RType(0, 0, t, d, sa, 0x00); }
uint32_t SRL(int d, int t, int sa)               { return RType(0, 0, t, d, sa, 0x02); }
uint32_t SRA(int d, int t, int sa)               { return RType(0, 0, t, d, sa, 0x03); }
uint32_t SLLV(int d, int t, int s)               { return RType(0, s, t, d, 0, 0x04); }
uint32_t SRLV(int d, int t, int s)               { return RType(0, s, t, d, 0, 0x06); }
uint32_t SRAV(int d, int t, int s)               { return RType(0, s, t, d, 0, 0x07); }
uint32_t JR(int s)                               { return RType(0, s, 0, 0, 0, 0x08); }
uint32_t JALR(int d, int s)                      { return RType(0, s, 0, d, 0, 0x09); }
uint32_t SYSCALL()                               { return RType(0, 0, 0, 0, 0, 0x0C); }
uint32_t BREAK()                                 { return RType(0, 0, 0, 0, 0, 0x0D); }
uint32_t MFHI(int d)                             { return RType(0, 0, 0, d, 0, 0x10); }
uint32_t MTHI(int s)                             { return RType(0, s, 0, 0, 0, 0x11); }
uint32_t MFLO(int d)                             { return RType(0, 0, 0, d, 0, 0x12); }
uint32_t MTLO(int s)                             { return RType(0, s, 0, 0, 0, 0x13); }
uint32_t MULT(int s, int t)                      { return RType(0, s, t, 0, 0, 0x18); }
uint32_t MULTU(int s, int t)                     { return RType(0, s, t, 0, 0, 0x19); }
uint32_t DIV(int s, int t)                       { return RType(0, s, t, 0, 0, 0x1A); }
uint32_t DIVU(int s, int t)                      { return RType(0, s, t, 0, 0, 0x1B); }
uint32_t ADD(int d, int s, int t)                { return RType(0, s, t, d, 0, 0x20); }
uint32_t ADDU(int d, int s, int t)               { return RType(0, s, t, d, 0, 0x21); }
uint32_t SUB(int d, int s, int t)                { return RType(0, s, t, d, 0, 0x22); }
uint32_t SUBU(int d, int s, int t)               { return RType(0, s, t, d, 0, 0x23); }
uint32_t AND(int d, int s, int t)                { return RType(0, s, t, d, 0, 0x24); }
uint32_t OR(int d, int s, int t)                 { return RType(0, s, t, d, 0, 0x25); }
uint32_t XOR(int d, int s, int t)                { return RType(0, s, t, d, 0, 0x26); }
uint32_t NOR(int d, int s, int t)                { return RType(0, s, t, d, 0, 0x27); }
uint32_t SLT(int d, int s, int t)                { return RType(0, s, t, d, 0, 0x2A); }
uint32_t SLTU(int d, int s, int t)               { return RType(0, s, t, d, 0, 0x2B); }

uint32_t BLTZ(int s, int off)                    { return IType(0x01, s, 0x00, off >> 2); }
uint32_t BGEZ(int s, int off)                    { return IType(0x01, s, 0x01, off >> 2); }
uint32_t BLTZAL(int s, int off)                  { return IType(0x01, s, 0x10, off >> 2); }
uint32_t BGEZAL(int s, int off)                  { return IType(0x01, s, 0x11, off >> 2); }
uint32_t J(uint32_t target)                      { return JType(0x02, target); }
uint32_t JAL(uint32_t target)                    { return JType(0x03, target); }
uint32_t BEQ(int s, int t, int off)              { return IType(0x04, s, t, off >> 2); }
uint32_t BNE(int s, int t, int off)              { return IType(0x05, s, t, off >> 2); }
uint32_t BLEZ(int s, int off)                    { return IType(0x06, s, 0, off >> 2); }
uint32_t BGTZ(int s, int off)                    { return IType(0x07, s, 0, off >> 2); }
uint32_t ADDI(int t, int s, int imm)             { return IType(0x08, s, t, imm); }
uint32_t ADDIU(int t, int s, int imm)            { return IType(0x09, s, t, imm); }
uint32_t SLTI(int t, int s, int imm)             { return IType(0x0A, s, t, imm); }
uint32_t SLTIU(int t, int s, int imm)            { return IType(0x0B, s, t, imm); }
uint32_t ANDI(int t, int s, int imm)             { return IType(0x0C, s, t, imm); }
uint32_t ORI(int t, int s, int imm)              { return IType(0x0D, s, t, imm); }
uint32_t XORI(int t, int s, int imm)             { return IType(0x0E, s, t, imm); }
uint32_t LUI(int t, int imm)                     { return IType(0x0F, 0, t, imm); }
uint32_t MFC0(int t, int d)                      { return RType(0x10, 0x00, t, d, 0, 0); }
uint32_t MTC0(int t, int d)                      { return RType(0x10, 0x04, t, d, 0, 0); }
uint32_t RFE()                                   { return 0x42000010; }
uint32_t MFC2(int t, int d)                      { return RType(0x12, 0x00, t, d, 0, 0); }
uint32_t LB(int t, int off, int s)               { return IType(0x20, s, t, off); }
uint32_t LH(int t, int off, int s)               { return IType(0x21, s, t, off); }
uint32_t LWL(int t, int off, int s)              { return IType(0x22, s, t, off); }
uint32_t LW(int t, int off, int s)               { return IType(0x23, s, t, off); }
uint32_t LBU(int t, int off, int s)              { return IType(0x24, s, t, off); }
uint32_t LHU(int t, int off, int s)              { return IType(0x25, s, t, off); }
uint32_t LWR(int t, int off, int s)              { return IType(0x26, s, t, off); }
uint32_t SB(int t, int off, int s)               { return IType(0x28, s, t, off); }
uint32_t SH(int t, int off, int s)               { return IType(0x29, s, t, off); }
uint32_t SWL(int t, int off, int s)              { return IType(0x2A, s, t, off); }
uint32_t SW(int t, int off, int s)               { return IType(0x2B, s, t, off); }
uint32_t SWR(int t, int off, int s)              { return IType(0x2E, s, t, off); }

// Cop0 register numbers.
const int kCop0Status = 12;
const int kCop0Cause = 13;
const int kCop0Epc = 14;

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";
std::string g_test;

void BeginTest(const std::string& name) { g_test = name; }

void Fail(const char* what, const std::string& detail) {
  ++g_failures;
  printf("  FAIL  %s / %s\n        %s\n", g_test.c_str(), what, detail.c_str());
}

std::string Hex(uint32_t value) {
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "0x%08X", value);
  return buffer;
}

void CheckEqual(uint32_t actual, uint32_t expected, const char* what) {
  ++g_checks;
  if (actual != expected)
    Fail(what, "got " + Hex(actual) + ", expected " + Hex(expected));
}

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition)
    Fail(what, "condition was false");
}

// ---------------------------------------------------------------------------
// The machine under test
// ---------------------------------------------------------------------------

// Programs are assembled into KSEG0 RAM. Using KSEG0 rather than KUSEG means
// the address translation is exercised on every instruction fetch as well.
const uint32_t kProgramBase = 0x80001000;
const uint32_t kDataBase    = 0x80002000;
const uint32_t kExceptionVector = 0x80000080;

class Machine {
 public:
  Machine() : system_(new emulation::psx::System()) {
    system_->InitializeWithoutBios();
  }
  ~Machine() {
    system_->Deinitialize();
    delete system_;
  }

  // Clears the register file and points the pc at the program area. Cop0
  // status starts with the coprocessor usable and the exception vectors in
  // RAM, which is the state the BIOS leaves behind.
  void Reset() {
    emulation::psx::CpuContext* context = system_->cpu().context();
    memset(&context->gp, 0, sizeof(context->gp));
    memset(&context->ctrl.reg, 0, sizeof(context->ctrl.reg));
    context->low = 0;
    context->high = 0;
    context->ctrl.SR.raw = 0x10000000;   // CU0 usable, BEV clear
    context->pc = kProgramBase;
    context->prev_pc = kProgramBase;
    memset(system_->ram(), 0, 0x200000);
  }

  void Load(const std::vector<uint32_t>& program) {
    for (size_t i = 0; i < program.size(); ++i)
      WriteWord(kProgramBase + static_cast<uint32_t>(i) * 4, program[i]);
  }

  void Run(int instructions) {
    for (int i = 0; i < instructions; ++i)
      system_->StepInstruction();
  }

  // Runs the two instructions it takes for a load still in flight to reach
  // its register.
  //
  // A load on this CPU does not land until the second instruction after it,
  // so a test that ends on a load and then reads the register is asking for a
  // value the hardware would not have produced yet. Real code ends with the
  // pipeline drained because there is always something after it; a test
  // program that stops dead has to say so. The memory after every test
  // program is zero, which decodes as NOP, so this runs two of those.
  void Settle() {
    Run(2);
  }

  // Assembles, runs, and leaves the result in the register file. The count is
  // the number of *steps*; a taken branch runs its delay slot inside one step.
  void Execute(const std::vector<uint32_t>& program, int steps) {
    Reset();
    Load(program);
    Run(steps);
  }

  uint32_t reg(int index) const {
    return system_->cpu().context()->gp.reg[index];
  }
  void set_reg(int index, uint32_t value) {
    system_->cpu().context()->gp.reg[index] = value;
  }

  uint32_t pc() const { return system_->cpu().context()->pc; }
  uint32_t hi() const { return system_->cpu().context()->high; }
  uint32_t lo() const { return system_->cpu().context()->low; }
  uint32_t cop0(int index) const {
    return system_->cpu().context()->ctrl.reg[index];
  }
  void set_cop0(int index, uint32_t value) {
    system_->cpu().context()->ctrl.reg[index] = value;
  }

  void WriteWord(uint32_t address, uint32_t value) {
    memcpy(system_->ram() + (address & 0x1FFFFC), &value, sizeof(value));
  }
  uint32_t ReadWord(uint32_t address) const {
    uint32_t value = 0;
    memcpy(&value, system_->ram() + (address & 0x1FFFFC), sizeof(value));
    return value;
  }

  emulation::psx::System* system() { return system_; }

 private:
  emulation::psx::System* system_;
};

// ---------------------------------------------------------------------------
// Arithmetic and logic
// ---------------------------------------------------------------------------

void TestArithmetic(Machine& m) {
  BeginTest("addu wraps rather than trapping");
  m.Execute({ LUI(t0, 0xFFFF), ORI(t0, t0, 0xFFFF),      // t0 = -1
              ADDIU(t1, zero, 1),
              ADDU(t2, t0, t1) }, 4);
  CheckEqual(m.reg(t2), 0, "0xFFFFFFFF + 1");

  BeginTest("addiu sign-extends its immediate");
  m.Execute({ ADDIU(t0, zero, 100), ADDIU(t1, t0, -50) }, 2);
  CheckEqual(m.reg(t1), 50, "100 + (-50)");

  BeginTest("subu");
  m.Execute({ ADDIU(t0, zero, 10), ADDIU(t1, zero, 30),
              SUBU(t2, t0, t1) }, 3);
  CheckEqual(m.reg(t2), 0xFFFFFFECu, "10 - 30");

  BeginTest("andi zero-extends, addiu sign-extends");
  // The same 0xFFFF means -1 to addiu and 65535 to andi. Getting these the
  // same way round is a classic decode slip.
  m.Execute({ ADDIU(t0, zero, -1),
              ANDI(t1, t0, 0xFFFF),
              ADDIU(t2, zero, 0xFFFF) }, 3);
  CheckEqual(m.reg(t1), 0x0000FFFFu, "andi 0xFFFF zero-extends");
  CheckEqual(m.reg(t2), 0xFFFFFFFFu, "addiu 0xFFFF sign-extends");

  BeginTest("logical ops");
  m.Execute({ LUI(t0, 0xF0F0), ORI(t0, t0, 0xF0F0),
              LUI(t1, 0x0FF0), ORI(t1, t1, 0x0FF0),
              AND(t2, t0, t1), OR(t3, t0, t1),
              XOR(t4, t0, t1), NOR(t5, t0, t1) }, 8);
  CheckEqual(m.reg(t2), 0x00F000F0u, "and");
  CheckEqual(m.reg(t3), 0xFFF0FFF0u, "or");
  CheckEqual(m.reg(t4), 0xFF00FF00u, "xor");
  CheckEqual(m.reg(t5), 0x000F000Fu, "nor");

  BeginTest("lui");
  m.Execute({ LUI(t0, 0x1234) }, 1);
  CheckEqual(m.reg(t0), 0x12340000u, "lui 0x1234");

  BeginTest("slt is signed, sltu is not");
  m.Execute({ ADDIU(t0, zero, -1),         // -1, or 0xFFFFFFFF
              ADDIU(t1, zero, 1),
              SLT(t2, t0, t1),             // -1 < 1  -> 1
              SLTU(t3, t0, t1) }, 4);      // 0xFFFFFFFF < 1 -> 0
  CheckEqual(m.reg(t2), 1, "slt -1 < 1");
  CheckEqual(m.reg(t3), 0, "sltu 0xFFFFFFFF < 1");

  BeginTest("slti / sltiu compare against a sign-extended immediate");
  // sltiu is the awkward one: the immediate is sign-extended and *then*
  // compared without sign.
  m.Execute({ ADDIU(t0, zero, 1),
              SLTI(t1, t0, -1),
              SLTIU(t2, t0, -1) }, 3);
  CheckEqual(m.reg(t1), 0, "slti 1 < -1");
  CheckEqual(m.reg(t2), 1, "sltiu 1 < 0xFFFFFFFF");
}

void TestShifts(Machine& m) {
  BeginTest("sra sign-extends, srl does not");
  m.Execute({ LUI(t0, 0x8000),
              SRA(t1, t0, 4),
              SRL(t2, t0, 4) }, 3);
  CheckEqual(m.reg(t1), 0xF8000000u, "sra");
  CheckEqual(m.reg(t2), 0x08000000u, "srl");

  BeginTest("sll");
  m.Execute({ ADDIU(t0, zero, 1), SLL(t1, t0, 31) }, 2);
  CheckEqual(m.reg(t1), 0x80000000u, "1 << 31");

  BeginTest("variable shifts use only the low five bits of the amount");
  // Shifting by 32 means shifting by 0, not by 32. Masking is the whole point.
  m.Execute({ ADDIU(t0, zero, 1),
              ADDIU(t1, zero, 32),
              SLLV(t2, t0, t1),
              ADDIU(t3, zero, 33),
              SLLV(t4, t0, t3) }, 5);
  CheckEqual(m.reg(t2), 1, "sllv by 32 is a shift by 0");
  CheckEqual(m.reg(t4), 2, "sllv by 33 is a shift by 1");

  BeginTest("srav");
  m.Execute({ LUI(t0, 0x8000), ADDIU(t1, zero, 4), SRAV(t2, t0, t1) }, 3);
  CheckEqual(m.reg(t2), 0xF8000000u, "srav");

  BeginTest("srlv");
  m.Execute({ LUI(t0, 0x8000), ADDIU(t1, zero, 4), SRLV(t2, t0, t1) }, 3);
  CheckEqual(m.reg(t2), 0x08000000u, "srlv");
}

void TestMultiplyDivide(Machine& m) {
  BeginTest("mult is signed");
  m.Execute({ ADDIU(t0, zero, -3), ADDIU(t1, zero, 5),
              MULT(t0, t1), MFLO(t2), MFHI(t3) }, 5);
  CheckEqual(m.reg(t2), 0xFFFFFFF1u, "-3 * 5 low");
  CheckEqual(m.reg(t3), 0xFFFFFFFFu, "-3 * 5 high");

  BeginTest("multu is unsigned");
  m.Execute({ ADDIU(t0, zero, -1), ADDIU(t1, zero, 2),
              MULTU(t0, t1), MFLO(t2), MFHI(t3) }, 5);
  CheckEqual(m.reg(t2), 0xFFFFFFFEu, "0xFFFFFFFF * 2 low");
  CheckEqual(m.reg(t3), 0x00000001u, "0xFFFFFFFF * 2 high");

  BeginTest("div is signed and truncates toward zero");
  m.Execute({ ADDIU(t0, zero, -7), ADDIU(t1, zero, 2),
              DIV(t0, t1), MFLO(t2), MFHI(t3) }, 5);
  CheckEqual(m.reg(t2), 0xFFFFFFFDu, "-7 / 2 quotient is -3");
  CheckEqual(m.reg(t3), 0xFFFFFFFFu, "-7 % 2 remainder is -1");

  BeginTest("divu");
  m.Execute({ ADDIU(t0, zero, 17), ADDIU(t1, zero, 5),
              DIVU(t0, t1), MFLO(t2), MFHI(t3) }, 5);
  CheckEqual(m.reg(t2), 3, "17 / 5");
  CheckEqual(m.reg(t3), 2, "17 % 5");

  // Division by zero does not trap on MIPS; it produces defined nonsense, and
  // software relies on the specific nonsense.
  BeginTest("div by zero, positive dividend");
  m.Execute({ ADDIU(t0, zero, 5), DIV(t0, zero), MFLO(t2), MFHI(t3) }, 4);
  CheckEqual(m.reg(t2), 0xFFFFFFFFu, "quotient is -1");
  CheckEqual(m.reg(t3), 5, "remainder is the dividend");

  BeginTest("div by zero, negative dividend");
  m.Execute({ ADDIU(t0, zero, -5), DIV(t0, zero), MFLO(t2), MFHI(t3) }, 4);
  CheckEqual(m.reg(t2), 1, "quotient is 1");
  CheckEqual(m.reg(t3), 0xFFFFFFFBu, "remainder is the dividend");

  BeginTest("divu by zero");
  m.Execute({ ADDIU(t0, zero, 5), DIVU(t0, zero), MFLO(t2), MFHI(t3) }, 4);
  CheckEqual(m.reg(t2), 0xFFFFFFFFu, "quotient is 0xFFFFFFFF");
  CheckEqual(m.reg(t3), 5, "remainder is the dividend");

  BeginTest("div overflow: the most negative value by -1");
  m.Execute({ LUI(t0, 0x8000), ADDIU(t1, zero, -1),
              DIV(t0, t1), MFLO(t2), MFHI(t3) }, 5);
  CheckEqual(m.reg(t2), 0x80000000u, "quotient stays 0x80000000");
  CheckEqual(m.reg(t3), 0, "remainder is 0");

  BeginTest("mthi / mtlo");
  m.Execute({ ADDIU(t0, zero, 0x123), MTHI(t0), ADDIU(t1, zero, 0x456),
              MTLO(t1), MFHI(t2), MFLO(t3) }, 6);
  CheckEqual(m.reg(t2), 0x123, "mthi then mfhi");
  CheckEqual(m.reg(t3), 0x456, "mtlo then mflo");
}

// ---------------------------------------------------------------------------
// Branches - where BLEZ was found assigning to its own operand
// ---------------------------------------------------------------------------

void TestBranches(Machine& m) {
  // Every branch test has the same shape: set a marker in the delay slot, a
  // different marker at the fall-through, and a third at the target. Which
  // markers are set says both whether the branch was taken and whether the
  // delay slot ran.
  //
  //   0: <set up>
  //   n: branch +8   (target is n+4+8)
  //   n+1: delay slot   -> t8 = 1
  //   n+2: not taken    -> t9 = 1
  //   n+3: taken        -> s0 = 1

  struct BranchCase {
    const char* name;
    uint32_t setup;         // writes t0
    uint32_t branch;
    bool expect_taken;
  };

  const BranchCase kCases[] = {
    { "beq equal",        ADDIU(t0, zero, 0),  BEQ(t0, zero, 8),  true },
    { "beq not equal",    ADDIU(t0, zero, 1),  BEQ(t0, zero, 8),  false },
    { "bne not equal",    ADDIU(t0, zero, 1),  BNE(t0, zero, 8),  true },
    { "bne equal",        ADDIU(t0, zero, 0),  BNE(t0, zero, 8),  false },

    // blez: taken for negative and for zero, not for positive.
    { "blez negative",    ADDIU(t0, zero, -1), BLEZ(t0, 8),       true },
    { "blez zero",        ADDIU(t0, zero, 0),  BLEZ(t0, 8),       true },
    { "blez positive",    ADDIU(t0, zero, 1),  BLEZ(t0, 8),       false },

    // bgtz: the mirror image.
    { "bgtz negative",    ADDIU(t0, zero, -1), BGTZ(t0, 8),       false },
    { "bgtz zero",        ADDIU(t0, zero, 0),  BGTZ(t0, 8),       false },
    { "bgtz positive",    ADDIU(t0, zero, 1),  BGTZ(t0, 8),       true },

    { "bltz negative",    ADDIU(t0, zero, -1), BLTZ(t0, 8),       true },
    { "bltz zero",        ADDIU(t0, zero, 0),  BLTZ(t0, 8),       false },
    { "bltz positive",    ADDIU(t0, zero, 1),  BLTZ(t0, 8),       false },

    { "bgez negative",    ADDIU(t0, zero, -1), BGEZ(t0, 8),       false },
    { "bgez zero",        ADDIU(t0, zero, 0),  BGEZ(t0, 8),       true },
    { "bgez positive",    ADDIU(t0, zero, 1),  BGEZ(t0, 8),       true },

    // The extremes, where a sign test written as a mask comparison goes wrong.
    { "blez most negative", LUI(t0, 0x8000),   BLEZ(t0, 8),       true },
    { "bgtz most negative", LUI(t0, 0x8000),   BGTZ(t0, 8),       false },
  };

  for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
    const BranchCase& test = kCases[i];
    BeginTest(std::string("branch: ") + test.name);

    // The branch offset is relative to the delay slot, so +8 from there lands
    // on index 4. Taken needs three steps (setup, branch-with-delay-slot,
    // target); not taken needs four (setup, branch, delay slot, fall-through)
    // and must stop before it reaches the target.
    m.Execute({ test.setup,
                test.branch,
                ADDIU(t8, zero, 1),      // delay slot
                ADDIU(t9, zero, 1),      // fall-through
                ADDIU(s0, zero, 1) },    // branch target
              test.expect_taken ? 3 : 4);

    CheckEqual(m.reg(t8), 1, "the delay slot ran");
    if (test.expect_taken) {
      CheckEqual(m.reg(s0), 1, "the branch was taken");
      CheckEqual(m.reg(t9), 0, "the fall-through did not run");
    } else {
      CheckEqual(m.reg(t9), 1, "execution fell through");
      CheckEqual(m.reg(s0), 0, "the branch target did not run");
    }
  }

  // A branch reads its operand. It must not write it. This is the assertion
  // that would have caught `(reg = 0)` written where `reg == 0` was meant, in
  // one line instead of an afternoon of BIOS tracing.
  const struct { const char* name; uint32_t branch; } kReadOnly[] = {
    { "beq",  BEQ(t0, zero, 8) }, { "bne",  BNE(t0, zero, 8) },
    { "blez", BLEZ(t0, 8) },      { "bgtz", BGTZ(t0, 8) },
    { "bltz", BLTZ(t0, 8) },      { "bgez", BGEZ(t0, 8) },
  };
  for (size_t i = 0; i < sizeof(kReadOnly) / sizeof(kReadOnly[0]); ++i) {
    BeginTest(std::string("branch does not modify its operand: ") +
              kReadOnly[i].name);
    for (int sign = 0; sign < 3; ++sign) {
      const int values[3] = { -1, 0, 1 };
      m.Execute({ ADDIU(t0, zero, values[sign]),
                  kReadOnly[i].branch,
                  NOP(), NOP(), NOP(), NOP() }, 3);
      CheckEqual(m.reg(t0), static_cast<uint32_t>(values[sign]),
                 "the operand survived the branch");
    }
  }

  BeginTest("branch offsets are relative to the delay slot");
  // A backward branch that skips over itself: the loop runs exactly twice.
  m.Execute({ ADDIU(t0, zero, 2),          // counter
              ADDIU(t1, zero, 0),          // accumulator
              ADDIU(t1, t1, 1),            // loop body
              ADDIU(t0, t0, -1),
              BNE(t0, zero, -12),          // back to the loop body
              NOP() }, 12);
  CheckEqual(m.reg(t1), 2, "the loop ran twice");
}

void TestJumps(Machine& m) {
  BeginTest("jal stores the address after the delay slot");
  m.Execute({ JAL(kProgramBase + 16),
              ADDIU(t8, zero, 1),          // delay slot
              ADDIU(t9, zero, 1),          // skipped
              NOP(),
              ADDIU(s0, zero, 1) }, 2);    // target
  CheckEqual(m.reg(t8), 1, "the delay slot ran");
  CheckEqual(m.reg(s0), 1, "the target ran");
  CheckEqual(m.reg(t9), 0, "the skipped instruction did not run");
  CheckEqual(m.reg(ra), kProgramBase + 8, "ra points after the delay slot");

  BeginTest("j keeps the top four bits of the pc");
  m.Execute({ J(kProgramBase + 16),
              ADDIU(t8, zero, 1),
              ADDIU(t9, zero, 1),
              NOP(),
              ADDIU(s0, zero, 1) }, 2);
  CheckEqual(m.reg(s0), 1, "the target ran");
  CheckEqual(m.reg(ra), 0, "j does not touch ra");

  BeginTest("jr");
  m.Execute({ LUI(t0, (kProgramBase + 20) >> 16),
              ORI(t0, t0, (kProgramBase + 20) & 0xFFFF),
              JR(t0),
              ADDIU(t8, zero, 1),          // delay slot
              ADDIU(t9, zero, 1),          // skipped
              ADDIU(s0, zero, 1) }, 4);    // target
  CheckEqual(m.reg(t8), 1, "the delay slot ran");
  CheckEqual(m.reg(s0), 1, "the target ran");
  CheckEqual(m.reg(t9), 0, "the skipped instruction did not run");

  BeginTest("jalr");
  m.Execute({ LUI(t0, (kProgramBase + 24) >> 16),
              ORI(t0, t0, (kProgramBase + 24) & 0xFFFF),
              JALR(ra, t0),
              ADDIU(t8, zero, 1),
              ADDIU(t9, zero, 1),
              NOP(),
              ADDIU(s0, zero, 1) }, 4);
  CheckEqual(m.reg(s0), 1, "the target ran");
  CheckEqual(m.reg(ra), kProgramBase + 16, "ra points after the delay slot");

  BeginTest("bltzal and bgezal always write ra");
  // Even when the branch is not taken, the link register is written. Software
  // uses bltzal $zero as an unconditional "get my address".
  m.Execute({ ADDIU(t0, zero, 1),
              BLTZAL(t0, 8),               // not taken
              NOP(), NOP(), NOP(), NOP() }, 3);
  CheckEqual(m.reg(ra), kProgramBase + 12, "ra written even when not taken");
}

// ---------------------------------------------------------------------------
// Loads and stores
// ---------------------------------------------------------------------------

void TestLoadStore(Machine& m) {
  BeginTest("lb sign-extends, lbu does not");
  m.Reset();
  m.WriteWord(kDataBase, 0x000000FF);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LB(t1, 0, t0), LBU(t2, 0, t0) });
  m.Run(4);
    m.Settle();
  CheckEqual(m.reg(t1), 0xFFFFFFFFu, "lb 0xFF");
  CheckEqual(m.reg(t2), 0x000000FFu, "lbu 0xFF");

  BeginTest("lh sign-extends, lhu does not");
  m.Reset();
  m.WriteWord(kDataBase, 0x0000FFFF);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LH(t1, 0, t0), LHU(t2, 0, t0) });
  m.Run(4);
    m.Settle();
  CheckEqual(m.reg(t1), 0xFFFFFFFFu, "lh 0xFFFF");
  CheckEqual(m.reg(t2), 0x0000FFFFu, "lhu 0xFFFF");

  BeginTest("byte and halfword stores leave their neighbours alone");
  m.Reset();
  m.WriteWord(kDataBase, 0xAABBCCDD);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           ADDIU(t1, zero, 0x11),
           SB(t1, 1, t0) });
  m.Run(4);
  CheckEqual(m.ReadWord(kDataBase), 0xAABB11DDu, "sb wrote one byte");

  m.Reset();
  m.WriteWord(kDataBase, 0xAABBCCDD);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LUI(t1, 0x1122), ORI(t1, t1, 0x3344),
           SH(t1, 0, t0) });
  m.Run(5);
  CheckEqual(m.ReadWord(kDataBase), 0xAABB3344u, "sh wrote two bytes");

  BeginTest("sw and lw round trip");
  m.Reset();
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LUI(t1, 0xDEAD), ORI(t1, t1, 0xBEEF),
           SW(t1, 0, t0), LW(t2, 0, t0) });
  m.Run(6);
    m.Settle();
  CheckEqual(m.reg(t2), 0xDEADBEEFu, "round trip");
}

// The unaligned group. SWL and SWR were merging the register into an
// uninitialised local instead of reading the word first, which corrupted the
// three bytes they were supposed to preserve.
void TestUnalignedLoadStore(Machine& m) {
  const uint32_t kMemory = 0x11223344;
  const uint32_t kRegister = 0xAABBCCDD;

  // Little-endian LWL/LWR, by byte offset within the word.
  const uint32_t kLwlExpected[4] = {
    0x44BBCCDD, 0x3344CCDD, 0x223344DD, 0x11223344
  };
  const uint32_t kLwrExpected[4] = {
    0x11223344, 0xAA112233, 0xAABB1122, 0xAABBCC11
  };
  const uint32_t kSwlExpected[4] = {
    0x112233AA, 0x1122AABB, 0x11AABBCC, 0xAABBCCDD
  };
  const uint32_t kSwrExpected[4] = {
    0xAABBCCDD, 0xBBCCDD44, 0xCCDD3344, 0xDD223344
  };

  for (int offset = 0; offset < 4; ++offset) {
    char name[64];

    snprintf(name, sizeof(name), "lwl at offset %d", offset);
    BeginTest(name);
    m.Reset();
    m.WriteWord(kDataBase, kMemory);
    m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
             LUI(t1, kRegister >> 16), ORI(t1, t1, kRegister & 0xFFFF),
             LWL(t1, offset, t0) });
    m.Run(5);
    m.Settle();
    CheckEqual(m.reg(t1), kLwlExpected[offset], "lwl result");

    snprintf(name, sizeof(name), "lwr at offset %d", offset);
    BeginTest(name);
    m.Reset();
    m.WriteWord(kDataBase, kMemory);
    m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
             LUI(t1, kRegister >> 16), ORI(t1, t1, kRegister & 0xFFFF),
             LWR(t1, offset, t0) });
    m.Run(5);
    m.Settle();
    CheckEqual(m.reg(t1), kLwrExpected[offset], "lwr result");

    // The store cases are the important ones: whatever the instruction does
    // not write has to survive untouched.
    snprintf(name, sizeof(name), "swl at offset %d preserves the rest", offset);
    BeginTest(name);
    m.Reset();
    m.WriteWord(kDataBase, kMemory);
    m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
             LUI(t1, kRegister >> 16), ORI(t1, t1, kRegister & 0xFFFF),
             SWL(t1, offset, t0) });
    m.Run(5);
    CheckEqual(m.ReadWord(kDataBase), kSwlExpected[offset], "swl result");

    snprintf(name, sizeof(name), "swr at offset %d preserves the rest", offset);
    BeginTest(name);
    m.Reset();
    m.WriteWord(kDataBase, kMemory);
    m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
             LUI(t1, kRegister >> 16), ORI(t1, t1, kRegister & 0xFFFF),
             SWR(t1, offset, t0) });
    m.Run(5);
    CheckEqual(m.ReadWord(kDataBase), kSwrExpected[offset], "swr result");
  }

  // The pair, used together, is how a MIPS memcpy moves an unaligned word.
  BeginTest("lwl+lwr load a whole unaligned word");
  m.Reset();
  m.WriteWord(kDataBase, 0x44332211);
  m.WriteWord(kDataBase + 4, 0x88776655);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LWL(t1, 5, t0),           // most significant bytes
           LWR(t1, 2, t0) });        // least significant bytes
  m.Run(4);
    m.Settle();
  CheckEqual(m.reg(t1), 0x66554433u, "the unaligned word at offset 2");

  BeginTest("swl+swr store a whole unaligned word");
  m.Reset();
  m.WriteWord(kDataBase, 0x00000000);
  m.WriteWord(kDataBase + 4, 0x00000000);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LUI(t1, 0xAABB), ORI(t1, t1, 0xCCDD),
           SWL(t1, 5, t0),
           SWR(t1, 2, t0) });
  m.Run(6);
  CheckEqual(m.ReadWord(kDataBase), 0xCCDD0000u, "low half of the store");
  CheckEqual(m.ReadWord(kDataBase + 4), 0x0000AABBu, "high half of the store");
}

// ---------------------------------------------------------------------------
// The memory map
// ---------------------------------------------------------------------------


// The load delay slot. A load on this CPU does not reach its register in time
// for the instruction right after it - the value arrives one instruction
// later - and software written for the machine both relies on that and works
// around it. An emulator that writes the register straight away runs such
// code differently, and does so silently.
void TestLoadDelaySlot(Machine& m) {
  const uint32_t kValue = 0xCAFEF00D;

  BeginTest("the instruction after a load sees the old value");
  m.Reset();
  m.WriteWord(kDataBase, kValue);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           ORI(t1, zero, 0x1234),      // t1 has something in it beforehand
           LW(t1, 0, t0),              // load into the same register
           ADDU(t2, t1, zero) });      // the delay slot reads t1
  m.Run(5);
  CheckEqual(m.reg(t2), 0x1234u,
             "the delay slot got the value from before the load");
  m.Settle();
  CheckEqual(m.reg(t1), kValue, "and the load itself did land");

  BeginTest("the instruction after that sees the new value");
  m.Reset();
  m.WriteWord(kDataBase, kValue);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           ORI(t1, zero, 0x1234),
           LW(t1, 0, t0),
           NOP(),                      // the delay slot
           ADDU(t2, t1, zero) });      // one later - this one sees it
  m.Run(6);
  CheckEqual(m.reg(t2), kValue, "one instruction later the value is there");

  // The hardware writes the load back before the next instruction's own
  // result, so the instruction wins and the load is simply lost. Getting this
  // backwards is the easy mistake: the load would arrive an instruction late
  // and quietly overwrite whatever was computed in the slot.
  BeginTest("a write in the delay slot beats the load");
  m.Reset();
  m.WriteWord(kDataBase, kValue);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LW(t1, 0, t0),
           ORI(t1, zero, 0x5678),      // same register, in the delay slot
           NOP(), NOP() });
  m.Run(6);
  CheckEqual(m.reg(t1), 0x5678u, "the instruction result survived");

  BeginTest("a second load to the same register discards the first");
  m.Reset();
  m.WriteWord(kDataBase, kValue);
  m.WriteWord(kDataBase + 4, 0x11112222);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LW(t1, 0, t0),
           LW(t1, 4, t0),
           NOP(), NOP() });
  m.Run(6);
  CheckEqual(m.reg(t1), 0x11112222u, "the second load is what lands");

  // A load two instructions before a branch has to land in the branch's delay
  // slot. The delay slot runs nested inside the branch, so anything that
  // advances the load pipeline at the end of an instruction rather than the
  // start gets this out of order and delivers the value one instruction late.
  BeginTest("a load lands correctly across a branch delay slot");
  m.Reset();
  m.WriteWord(kDataBase, kValue);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LW(t1, 0, t0),              // load
           BEQ(zero, zero, 8),         // branch - t1 not available here
           ADDU(t2, t1, zero),         // its delay slot - t1 IS available
           NOP(), NOP() });
  m.Run(6);
  CheckEqual(m.reg(t2), kValue,
             "the branch delay slot saw the loaded value");

  // Loading into r0 must still discard the value - r0 reads as zero however
  // it is written.
  BeginTest("a load into r0 changes nothing");
  m.Reset();
  m.WriteWord(kDataBase, kValue);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LW(zero, 0, t0),
           NOP(), NOP() });
  m.Run(5);
  CheckEqual(m.reg(zero), 0u, "r0 is still zero");

  // lwl and lwr are meant to be used back to back with no gap, which only
  // works because the hardware forwards the first one to the second. Without
  // that forwarding the pair silently loses half its result.
  BeginTest("lwl and lwr forward to each other with no gap");
  m.Reset();
  m.WriteWord(kDataBase, 0x11223344);
  m.WriteWord(kDataBase + 4, 0xAABBCCDD);
  m.Load({ LUI(t0, kDataBase >> 16), ORI(t0, t0, kDataBase & 0xFFFF),
           LWL(t1, 5, t0),             // unaligned word at offset 2
           LWR(t1, 2, t0),
           NOP(), NOP() });
  m.Run(6);
  CheckEqual(m.reg(t1), 0xCCDD1122u, "the pair assembled the whole word");
}

void TestGteDelay(Machine& m) {
  // psx-spx: "Using CFC2/MFC2 has a delay of 1 instruction until the GPR is
  // loaded with its new value." Tekken 2's geometry depends on getting this
  // right. MFC2 used to write the register the instant it ran; it now goes
  // through the same pending-load pipeline LW/LB/LH already use, so these
  // are the same three shapes TestLoadDelaySlot checks for an ordinary load,
  // aimed at MFC2 instead.
  const uint32_t kValue = 0x00001234;  // small and positive: no int16 games

  BeginTest("the instruction after MFC2 sees the old value");
  m.Reset();
  m.system()->gte().WriteData(1, kValue);  // VZ0
  m.Load({ ORI(t1, zero, 0x5678),   // t1 has something in it beforehand
           MFC2(t1, 1),             // load into the same register
           ADDU(t2, t1, zero) });   // the delay slot reads t1
  m.Run(3);
  CheckEqual(m.reg(t2), 0x5678u, "the delay slot got the value from before");
  m.Settle();
  CheckEqual(m.reg(t1), kValue, "and MFC2 itself did land");

  BeginTest("the instruction after that sees the new value");
  m.Reset();
  m.system()->gte().WriteData(1, kValue);
  m.Load({ ORI(t1, zero, 0x5678),
           MFC2(t1, 1),
           NOP(),                  // the delay slot
           ADDU(t2, t1, zero) });  // one later - this one sees it
  m.Run(4);
  CheckEqual(m.reg(t2), kValue, "one instruction later the value is there");

  BeginTest("a write in the delay slot beats MFC2");
  m.Reset();
  m.system()->gte().WriteData(1, kValue);
  m.Load({ MFC2(t1, 1),
           ORI(t1, zero, 0x5678),  // same register, in the delay slot
           NOP(), NOP() });
  m.Run(4);
  CheckEqual(m.reg(t1), 0x5678u, "the instruction result survived");

  // psx-spx: a GTE command or register access issued before the previous
  // command has finished stalls the CPU until it has. SQR is documented at
  // 5 cycles; issuing it and reading a result register back immediately
  // (through CFC2/MFC2, the same pattern amidog's psxtest_gte times) should
  // cost noticeably more than the 2 cycles two ordinary instructions would.
  BeginTest("a GTE register read right after a command waits for it");
  m.Reset();
  const uint64_t before = m.system()->cpu().context()->cycles;
  m.Load({ RType(0x12, 0, 0, 0, 0, 0x28) | (1u << 25),  // SQR
           MFC2(t1, 8) });                              // IR0, right after
  m.Run(2);
  const uint64_t elapsed = m.system()->cpu().context()->cycles - before;
  Check(elapsed >= 5, "SQR's 5-cycle busy window was not skipped");
}

void TestMemoryMap(Machine& m) {
  // KUSEG, KSEG0 and KSEG1 are three views of the same 2 MB of RAM. A write
  // through one must be visible through the others.
  BeginTest("RAM is visible through KUSEG, KSEG0 and KSEG1");
  m.Reset();
  m.Load({ LUI(t0, 0x8000), ORI(t0, t0, 0x2000),      // KSEG0
           LUI(t1, 0x0000), ORI(t1, t1, 0x2000),      // KUSEG
           LUI(t2, 0xA000), ORI(t2, t2, 0x2000),      // KSEG1
           LUI(t3, 0xCAFE), ORI(t3, t3, 0xF00D),
           SW(t3, 0, t0),
           LW(t4, 0, t1),
           LW(t5, 0, t2) });
  m.Run(11);
    m.Settle();
  CheckEqual(m.reg(t4), 0xCAFEF00Du, "read back through KUSEG");
  CheckEqual(m.reg(t5), 0xCAFEF00Du, "read back through KSEG1");

  BeginTest("RAM mirrors every 2 MB");
  m.Reset();
  m.Load({ LUI(t0, 0x8000), ORI(t0, t0, 0x2000),
           LUI(t1, 0x8020), ORI(t1, t1, 0x2000),      // +2 MB
           LUI(t3, 0x1234), ORI(t3, t3, 0x5678),
           SW(t3, 0, t0),
           LW(t4, 0, t1) });
  m.Run(9);
    m.Settle();
  CheckEqual(m.reg(t4), 0x12345678u, "the mirror sees the same word");

  BeginTest("the scratchpad is its own memory");
  m.Reset();
  m.Load({ LUI(t0, 0x1F80), ORI(t0, t0, 0x0000),
           LUI(t1, 0xFEED), ORI(t1, t1, 0xFACE),
           SW(t1, 0, t0),
           LW(t2, 0, t0) });
  m.Run(6);
    m.Settle();
  CheckEqual(m.reg(t2), 0xFEEDFACEu, "scratchpad round trip");

  // Hardware registers appear at three virtual addresses and software uses all
  // three. Decoding the virtual address instead of the physical one made a
  // KSEG1 access fall off the end of the map and silently return zero.
  BeginTest("hardware registers answer through KUSEG, KSEG0 and KSEG1");
  const uint32_t kViews[3] = { 0x1F801074, 0x9F801074, 0xBF801074 };
  for (int i = 0; i < 3; ++i) {
    m.Reset();
    m.Load({ LUI(t0, kViews[i] >> 16), ORI(t0, t0, kViews[i] & 0xFFFF),
             LUI(t1, 0x0000), ORI(t1, t1, 0x0501),
             SW(t1, 0, t0),
             LW(t2, 0, t0) });
    m.Run(6);
    m.Settle();
    char name[64];
    snprintf(name, sizeof(name), "I_MASK through %08X", kViews[i]);
    CheckEqual(m.reg(t2), 0x0501, name);
  }

  BeginTest("the BIOS region is read-only");
  // Compare against what was there rather than against zero: whether the BIOS
  // buffer happens to be empty is not what this is testing.
  m.Reset();
  m.Load({ LUI(t0, 0xBFC0), ORI(t0, t0, 0x0000),
           LW(t3, 0, t0),                    // before
           ADDIU(t1, zero, 0x55),
           SW(t1, 0, t0),
           LW(t2, 0, t0) });                 // after
  m.Run(6);
    m.Settle();
  CheckEqual(m.reg(t2), m.reg(t3), "the BIOS did not take the write");

  BeginTest("register zero stays zero");
  m.Execute({ ADDIU(zero, zero, 5),
              LUI(zero, 0xFFFF),
              ADDU(t0, zero, zero) }, 3);
  CheckEqual(m.reg(zero), 0, "$zero was not written");
  CheckEqual(m.reg(t0), 0, "$zero reads as zero");
}

// ---------------------------------------------------------------------------
// Exceptions and the Cop0 status stack
// ---------------------------------------------------------------------------

void TestExceptions(Machine& m) {
  BeginTest("syscall vectors and records the faulting address");
  m.Reset();
  m.Load({ NOP(), SYSCALL(), NOP() });
  m.Run(2);
  CheckEqual(m.pc(), kExceptionVector, "vectored to 0x80000080");
  CheckEqual(m.cop0(kCop0Epc), kProgramBase + 4, "EPC is the syscall itself");
  CheckEqual((m.cop0(kCop0Cause) >> 2) & 0x1F, 8, "cause code is Syscall");

  BeginTest("break");
  m.Reset();
  m.Load({ BREAK() });
  m.Run(1);
  CheckEqual(m.pc(), kExceptionVector, "vectored");
  CheckEqual((m.cop0(kCop0Cause) >> 2) & 0x1F, 9, "cause code is Break");

  BeginTest("an exception pushes the status stack");
  m.Reset();
  // IEc and KUc set; after the push they should appear in the "previous" slot
  // and the current slot should be clear.
  m.set_cop0(kCop0Status, 0x10000003);
  m.Load({ SYSCALL() });
  m.Run(1);
  const uint32_t pushed = m.cop0(kCop0Status);
  CheckEqual(pushed & 0x3F, 0x0C, "IEc/KUc cleared, moved to IEp/KUp");

  BeginTest("rfe pops the status stack");
  m.Reset();
  m.set_cop0(kCop0Status, 0x1000000C);   // IEp and KUp set, current clear
  m.Load({ RFE() });
  m.Run(1);
  CheckEqual(m.cop0(kCop0Status) & 0x3F, 0x03, "IEp/KUp moved down to IEc/KUc");

  BeginTest("push then pop is the identity");
  m.Reset();
  m.set_cop0(kCop0Status, 0x10000003);
  m.Load({ SYSCALL() });
  m.Run(1);
  m.Load({ RFE() });
  m.system()->cpu().context()->pc = kProgramBase;
  m.Run(1);
  CheckEqual(m.cop0(kCop0Status) & 0x3F, 0x03, "back to where it started");

  BeginTest("mfc0 and mtc0 reach the right registers");
  m.Reset();
  m.Load({ LUI(t0, 0x1234), ORI(t0, t0, 0x5678),
           MTC0(t0, kCop0Epc),
           MFC0(t1, kCop0Epc) });
  m.Run(4);
  CheckEqual(m.reg(t1), 0x12345678u, "EPC round trip");
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------

void TestInterrupts(Machine& m) {
  emulation::psx::IOInterface& io = m.system()->io();

  BeginTest("writing I_STAT acknowledges rather than assigns");
  // A zero bit clears the flag; a one bit leaves it alone. Assigning the
  // written value instead sets every other bit on every acknowledge.
  m.Reset();
  io.io.interrupt_stat = 0x0000000F;
  io.io.interrupt_mask = 0x0000FFFF;
  m.Load({ LUI(t0, 0x1F80), ORI(t0, t0, 0x1070),
           LUI(t1, 0xFFFF), ORI(t1, t1, 0xFFFE),   // clear bit 0 only
           SW(t1, 0, t0) });
  m.Run(5);
  CheckEqual(io.io.interrupt_stat, 0x0000000Eu, "only bit 0 was cleared");

  BeginTest("acknowledging one flag does not raise the others");
  m.Reset();
  io.io.interrupt_stat = 0x00000001;
  io.io.interrupt_mask = 0x0000FFFF;
  m.Load({ LUI(t0, 0x1F80), ORI(t0, t0, 0x1070),
           LUI(t1, 0xFFFF), ORI(t1, t1, 0xFFFE),
           SW(t1, 0, t0) });
  m.Run(5);
  CheckEqual(io.io.interrupt_stat, 0, "I_STAT is empty, not full");

  BeginTest("an interrupt is not taken while the mask is clear");
  m.Reset();
  m.set_cop0(kCop0Status, 0x10000401);      // IEc and IM2 set
  io.io.interrupt_stat = 0x00000001;
  io.io.interrupt_mask = 0x00000000;        // but nothing is enabled
  m.Load({ NOP(), NOP(), NOP() });
  m.Run(3);
  Check(m.pc() != kExceptionVector, "no exception was taken");

  BeginTest("an interrupt is not taken while IEc is clear");
  m.Reset();
  m.set_cop0(kCop0Status, 0x10000400);      // IM2 set, IEc clear
  io.io.interrupt_stat = 0x00000001;
  io.io.interrupt_mask = 0x00000001;
  m.Load({ NOP(), NOP(), NOP() });
  m.Run(3);
  Check(m.pc() != kExceptionVector, "no exception was taken");

  BeginTest("an interrupt is not taken while the Cop0 mask bit is clear");
  m.Reset();
  m.set_cop0(kCop0Status, 0x10000001);      // IEc set, IM2 clear
  io.io.interrupt_stat = 0x00000001;
  io.io.interrupt_mask = 0x00000001;
  m.Load({ NOP(), NOP(), NOP() });
  m.Run(3);
  Check(m.pc() != kExceptionVector, "no exception was taken");

  // The one that matters. EPC has to be the instruction that has *not* run.
  // Pointing it at the instruction that just finished makes that instruction
  // execute twice on return - and when it is an RFE, the status stack is
  // popped twice and interrupts never come back.
  BeginTest("an interrupt sets EPC to the instruction that has not run");
  m.Reset();
  m.set_cop0(kCop0Status, 0x10000401);
  io.io.interrupt_stat = 0x00000001;
  io.io.interrupt_mask = 0x00000001;
  m.Load({ ADDIU(t0, zero, 1),
           ADDIU(t1, zero, 1),
           ADDIU(t2, zero, 1) });
  // The interrupt is pending before the first instruction, so it is taken
  // straight away: EPC is that instruction, and it has not run.
  m.Run(1);
  CheckEqual(m.cop0(kCop0Epc), kProgramBase,
             "EPC is the instruction that has not run yet");
  CheckEqual(m.reg(t0), 0, "that instruction did not run before the exception");
  Check(m.pc() >= kExceptionVector && m.pc() < kExceptionVector + 0x100,
        "control is in the exception handler");

  BeginTest("an interrupt reports itself as cause code zero");
  CheckEqual((m.cop0(kCop0Cause) >> 2) & 0x1F, 0, "cause code is Int");

  BeginTest("the interrupted instruction runs exactly once on return");
  // Stand in for the handler: acknowledge, pop the status stack, jump to EPC.
  io.io.interrupt_stat = 0;
  m.set_cop0(kCop0Status, 0x10000401);
  m.system()->cpu().context()->pc = m.cop0(kCop0Epc);
  m.Run(3);
  CheckEqual(m.reg(t0), 1, "the interrupted instruction ran");
  CheckEqual(m.reg(t1), 1, "and the one after it");
  CheckEqual(m.reg(t2), 1, "and the one after that");
}

struct Group {
  const char* name;
  void (*run)(Machine&);
};

const Group kGroups[] = {
  { "arithmetic", TestArithmetic },
  { "shifts",     TestShifts },
  { "muldiv",     TestMultiplyDivide },
  { "branches",   TestBranches },
  { "jumps",      TestJumps },
  { "loadstore",  TestLoadStore },
  { "unaligned",  TestUnalignedLoadStore },
  { "loaddelay",  TestLoadDelaySlot },
  { "gtedelay",   TestGteDelay },
  { "memory",     TestMemoryMap },
  { "exceptions", TestExceptions },
  { "interrupts", TestInterrupts },
};

}  // namespace

int main(int argc, char** argv) {
  const char* only = (argc > 1) ? argv[1] : nullptr;

  printf("cpu_test - R3000A, memory map, exceptions, interrupts\n\n");

  Machine machine;
  for (size_t i = 0; i < sizeof(kGroups) / sizeof(kGroups[0]); ++i) {
    if (only != nullptr && strcmp(only, kGroups[i].name) != 0)
      continue;
    const int before = g_failures;
    g_group = kGroups[i].name;
    printf("%s\n", kGroups[i].name);
    kGroups[i].run(machine);
    if (g_failures == before)
      printf("  ok\n");
  }

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

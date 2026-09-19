// debug_test - the debugger's core (psx/debugger.h): breakpoints and stepping, checked the way
// cpu_test checks the CPU - small programs assembled into RAM, run through System, no BIOS.
//
// Two properties matter more than any single feature, and most checks here are about one or the
// other. A halt is *before* the instruction and runs nothing: no register moves, no cycle passes.
// And stepping lands where a person expects - after a call returns, in the caller after a return
// - including when the function in between makes calls of its own, which is where a debugger that
// only looks at $ra goes wrong.

#include "psx/psx.h"
#include "psx/disasm.h"

#include <cstdio>
#include <cstring>
#include <vector>

using emulation::psx::Debugger;
using emulation::psx::System;

namespace {

int g_checks = 0;
int g_failures = 0;

void Group(const char* name) { printf("%s\n", name); }

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

enum Register { zero = 0, t0 = 8, t1, t2, t3, s0 = 16, ra = 31 };

uint32_t RType(uint32_t op, uint32_t rs, uint32_t rt, uint32_t rd, uint32_t funct) {
  return (op << 26) | (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
uint32_t IType(uint32_t op, uint32_t rs, uint32_t rt, int immediate) {
  return (op << 26) | (rs << 21) | (rt << 16) | (static_cast<uint32_t>(immediate) & 0xFFFF);
}
uint32_t NOP() { return 0; }
uint32_t ADDIU(int t, int s, int imm) { return IType(0x09, s, t, imm); }
uint32_t ADDU(int d, int s, int t) { return RType(0, s, t, d, 0x21); }
uint32_t BNE(int s, int t, int off) { return IType(0x05, s, t, off >> 2); }
uint32_t BEQ(int s, int t, int off) { return IType(0x04, s, t, off >> 2); }
uint32_t JAL(uint32_t target) { return (0x03u << 26) | ((target >> 2) & 0x03FFFFFF); }
uint32_t JR(int s) { return RType(0, s, 0, 0, 0x08); }
uint32_t SYSCALL() { return 0x0000000C; }
uint32_t LW(int t, int s, int imm) { return IType(0x23, s, t, imm); }
uint32_t LB(int t, int s, int imm) { return IType(0x20, s, t, imm); }
uint32_t SW(int t, int s, int imm) { return IType(0x2B, s, t, imm); }
uint32_t SB(int t, int s, int imm) { return IType(0x28, s, t, imm); }
uint32_t SWL(int t, int s, int imm) { return IType(0x2A, s, t, imm); }
uint32_t LUI(int t, int imm) { return IType(0x0F, 0, t, imm); }
uint32_t ORI(int t, int s, int imm) { return IType(0x0D, s, t, imm); }

const uint32_t kBase = 0x80001000;
const uint32_t kVector = 0x80000080;

class Machine {
 public:
  Machine() : system_(new System()) { system_->InitializeWithoutBios(); }
  ~Machine() {
    system_->Deinitialize();
    delete system_;
  }

  System& system() { return *system_; }
  Debugger& debugger() { return system_->debugger(); }

  void Reset() {
    auto* context = system_->cpu().context();
    memset(&context->gp, 0, sizeof(context->gp));
    memset(&context->ctrl.reg, 0, sizeof(context->ctrl.reg));
    context->ctrl.SR.raw = 0x10000000;   // CU0, vectors in RAM
    context->pc = kBase;
    context->prev_pc = kBase;
    memset(system_->ram(), 0, 0x200000);
    debugger().ClearBreakpoints();
    debugger().ClearWatchpoints();
    debugger().ClearBiosBreaks();
    debugger().SetCallTracking(false);
    debugger().Reset();
    system_->EnableRecompiler(false);
  }

  void Load(uint32_t address, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i) {
      const uint32_t at = (address + static_cast<uint32_t>(i) * 4) & 0x1FFFFC;
      memcpy(system_->ram() + at, &words[i], 4);
    }
  }

  // Steps until the debugger halts or `limit` steps have run. Returns the steps that ran - a
  // halted step is not one, which is the same rule host::Machine and boot_runner keep.
  int RunUntilHalt(int limit) {
    int ran = 0;
    while (ran < limit) {
      system_->StepInstruction();
      if (debugger().halted())
        break;
      ++ran;
    }
    return ran;
  }

  uint32_t reg(int r) { return system_->cpu().context()->gp.reg[r]; }
  void set_reg(int r, uint32_t v) { system_->cpu().context()->gp.reg[r] = v; }
  uint32_t pc() { return system_->cpu().context()->pc; }

 private:
  System* system_;
};

void TestBreakpoints(Machine& m) {
  Group("breakpoints");
  m.Reset();
  m.Load(kBase, { ADDIU(t0, zero, 1), ADDIU(t1, zero, 2), ADDIU(t2, zero, 3), NOP(), NOP() });
  m.debugger().AddBreakpoint(kBase + 4);
  Check(m.debugger().armed(), "a breakpoint arms the debugger");
  const uint64_t cycles_before = m.system().cpu().context()->cycles;
  const int ran = m.RunUntilHalt(10);
  Check(m.debugger().halted(), "halts");
  CheckEqual(m.pc(), kBase + 4, "at the breakpoint");
  CheckEqual(ran, 1, "after one instruction");
  CheckEqual(m.reg(t0), 1, "the one before it ran");
  CheckEqual(m.reg(t1), 0, "the one it is on did not");
  Check(m.debugger().halt_reason() == Debugger::HaltReason::kBreakpoint, "reason: breakpoint");
  CheckEqual(static_cast<uint32_t>(m.debugger().breakpoints()[0].hits), 1, "one hit");

  const uint64_t cycles_at_halt = m.system().cpu().context()->cycles;
  m.system().StepInstruction();   // halted: must do nothing, not even tick
  Check(m.system().cpu().context()->cycles == cycles_at_halt && m.reg(t1) == 0,
        "a step while halted runs nothing and takes no time");
  CheckEqual(static_cast<uint32_t>(m.debugger().breakpoints()[0].hits), 1,
             "and does not count the breakpoint again");
  Check(cycles_at_halt > cycles_before, "(the instruction before it did take time)");

  m.debugger().Resume();
  m.RunUntilHalt(3);
  Check(!m.debugger().halted(), "resuming does not halt on the same instruction again");
  CheckEqual(m.reg(t1), 2, "which now runs");
  CheckEqual(m.reg(t2), 3, "and so does the rest");

  m.Reset();
  m.Load(kBase, { ADDIU(t0, zero, 1), ADDIU(t1, zero, 2), NOP() });
  m.debugger().AddBreakpoint(kBase + 4);
  m.debugger().SetBreakpointEnabled(kBase + 4, false);
  m.RunUntilHalt(3);
  Check(!m.debugger().halted() && m.reg(t1) == 2, "a disabled breakpoint does not fire");

  m.Reset();
  m.Load(kBase, { ADDIU(t0, zero, 1), ADDIU(t1, zero, 2), NOP() });
  m.debugger().AddBreakpoint((kBase + 4) & 0x1FFFFFFF);   // the KUSEG address of a KSEG0 pc
  m.RunUntilHalt(3);
  Check(m.debugger().halted() && m.pc() == kBase + 4,
        "a breakpoint matches its physical address, whichever window reaches it");

  // A loop: the breakpoint on its first instruction fires once each time round.
  m.Reset();
  m.set_reg(t1, 3);
  m.Load(kBase, { ADDIU(t0, t0, 1), BNE(t0, t1, -8), NOP(), NOP(), NOP() });
  m.debugger().AddBreakpoint(kBase);
  int halts = 0;
  for (int round = 0; round < 10; ++round) {
    m.RunUntilHalt(10);
    if (!m.debugger().halted())
      break;
    ++halts;
    m.debugger().Resume();
  }
  CheckEqual(halts, 3, "a loop run three times halts three times");
  CheckEqual(m.reg(t0), 3, "and still counts to three");
}

void TestStepInto(Machine& m) {
  Group("step into");
  m.Reset();
  m.Load(kBase, { ADDIU(t0, zero, 1), ADDIU(t1, zero, 2), ADDIU(t2, zero, 3), NOP() });
  m.debugger().AddBreakpoint(kBase);
  m.RunUntilHalt(5);
  m.debugger().StepInto();
  const int ran = m.RunUntilHalt(5);
  Check(m.debugger().halted() && m.pc() == kBase + 4, "one instruction on");
  CheckEqual(ran, 1, "having run exactly one");
  CheckEqual(m.reg(t0), 1, "which was the one halted on");
  Check(m.debugger().halt_reason() == Debugger::HaltReason::kStep, "reason: step");
  m.debugger().StepInto();
  m.RunUntilHalt(5);
  Check(m.pc() == kBase + 8 && m.reg(t1) == 2, "and again");

  // A taken branch and its delay slot are one step on this CPU (Cpu::Jump runs the slot).
  m.Reset();
  m.Load(kBase, { BEQ(zero, zero, 12), ADDIU(t0, zero, 7), NOP(), NOP(), ADDIU(t1, zero, 9) });
  m.debugger().AddBreakpoint(kBase);
  m.RunUntilHalt(5);
  m.debugger().StepInto();
  m.RunUntilHalt(5);
  CheckEqual(m.pc(), kBase + 16, "stepping a taken branch lands on its target");
  CheckEqual(m.reg(t0), 7, "with the delay slot run on the way");
}

// main:  jal func / nop / addiu t3,7 / nop...      func (at +40h): addiu t2,5 / jr ra / nop
void LoadCallProgram(Machine& m) {
  m.Load(kBase, { JAL(kBase + 0x40), NOP(), ADDIU(t3, zero, 7), NOP(), NOP() });
  m.Load(kBase + 0x40, { ADDIU(t2, zero, 5), JR(ra), NOP() });
}

void TestStepOver(Machine& m) {
  Group("step over");
  m.Reset();
  LoadCallProgram(m);
  m.debugger().AddBreakpoint(kBase);
  m.RunUntilHalt(5);
  m.debugger().StepOver();
  m.RunUntilHalt(20);
  Check(m.debugger().halted(), "halts");
  CheckEqual(m.pc(), kBase + 8, "after the call and its delay slot");
  CheckEqual(m.reg(t2), 5, "with the function run");
  CheckEqual(m.reg(t3), 0, "and nothing after it");

  m.Reset();
  m.Load(kBase, { ADDIU(t0, zero, 1), ADDIU(t1, zero, 2), NOP() });
  m.debugger().AddBreakpoint(kBase);
  m.RunUntilHalt(5);
  m.debugger().StepOver();
  m.RunUntilHalt(5);
  Check(m.pc() == kBase + 4 && m.reg(t0) == 1, "over anything but a call is one step");
}

void TestStepOut(Machine& m) {
  Group("step out");
  m.Reset();
  LoadCallProgram(m);
  m.debugger().AddBreakpoint(kBase + 0x40);   // inside func
  m.RunUntilHalt(20);
  Check(m.pc() == kBase + 0x40, "(halted inside the function)");
  m.debugger().StepOut();
  m.RunUntilHalt(20);
  Check(m.debugger().halted(), "halts");
  CheckEqual(m.pc(), kBase + 8, "back in the caller, past the call");
  CheckEqual(m.reg(t2), 5, "with the function finished");

  // A function that calls another before returning: the inner `jr ra` must not count as the
  // outer one returning. A saves ra in s0 around its call, as real code saves it on the stack.
  //   main:  jal A / nop / addiu t3,7
  //   A:     addu s0,ra,zero / jal B / nop / addu ra,s0,zero / jr ra / nop
  //   B:     addiu t2,5 / jr ra / nop
  m.Reset();
  m.Load(kBase, { JAL(kBase + 0x40), NOP(), ADDIU(t3, zero, 7), NOP() });
  m.Load(kBase + 0x40, { ADDU(s0, ra, zero), JAL(kBase + 0x80), NOP(), ADDU(ra, s0, zero), JR(ra),
                         NOP() });
  m.Load(kBase + 0x80, { ADDIU(t2, zero, 5), JR(ra), NOP() });
  m.debugger().AddBreakpoint(kBase + 0x40);
  m.RunUntilHalt(20);
  m.debugger().StepOut();
  m.RunUntilHalt(40);
  CheckEqual(m.pc(), kBase + 8, "out of a function that made its own call: in its caller");
  CheckEqual(m.reg(t2), 5, "with the inner call made");
}

void TestRunToAndBreak(Machine& m) {
  Group("run to, break, and the exception vector");
  m.Reset();
  m.Load(kBase, { ADDIU(t0, zero, 1), ADDIU(t1, zero, 2), ADDIU(t2, zero, 3), NOP() });
  m.debugger().RunTo(kBase + 8);
  m.RunUntilHalt(10);
  Check(m.debugger().halted() && m.pc() == kBase + 8, "run to an address halts there");
  Check(m.debugger().halt_reason() == Debugger::HaltReason::kRunTo, "reason: run to");
  CheckEqual(m.reg(t2), 0, "before it runs");

  m.Reset();
  m.Load(kBase, { ADDIU(t0, zero, 1), ADDIU(t1, zero, 2), NOP() });
  m.RunUntilHalt(1);
  m.debugger().RequestBreak();
  m.RunUntilHalt(10);
  Check(m.debugger().halted() && m.pc() == kBase + 4, "a requested break halts at once");
  Check(m.debugger().halt_reason() == Debugger::HaltReason::kRequested, "reason: requested");

  m.Reset();
  m.Load(kBase, { SYSCALL(), NOP() });
  m.debugger().AddBreakpoint(kVector);
  m.RunUntilHalt(5);
  Check(m.debugger().halted() && m.pc() == kVector, "a breakpoint on the exception vector fires");
  CheckEqual(m.system().cpu().context()->ctrl.EPC, kBase, "with EPC at the syscall");
}

void TestRecompiler(Machine& m) {
  Group("the recompiler");
  // Eight instructions in a row: a compiled chain would run them all in one step.
  m.Reset();
  std::vector<uint32_t> program;
  for (int i = 0; i < 8; ++i)
    program.push_back(ADDIU(t0 + (i % 4), zero, i + 1));
  program.push_back(NOP());
  m.Load(kBase, program);
  m.system().EnableRecompiler(true);
  m.debugger().AddBreakpoint(kBase + 12);
  m.RunUntilHalt(20);
  Check(m.debugger().halted() && m.pc() == kBase + 12,
        "with the recompiler on, a breakpoint still stops on its exact instruction");
  CheckEqual(m.reg(t2), 3, "the instruction before it ran");
  CheckEqual(m.reg(t3), 0, "the one it is on did not");

  m.debugger().ClearBreakpoints();
  m.debugger().Resume();
  Check(m.debugger().armed(), "resuming arms one skip");
  m.system().StepInstruction();
  Check(!m.debugger().armed(), "then, with nothing set, disarms - compiled code comes back");
  m.system().EnableRecompiler(false);
}

void TestSnapshot(Machine& m) {
  Group("the snapshot the window is shown");
  const uint32_t kData = 0x80002000;
  m.Reset();
  m.Load(kBase, { LW(t1, t0, 0), ADDIU(t2, zero, 5), BEQ(zero, zero, 16), NOP(), NOP(), NOP(),
                  NOP(), NOP(), NOP() });
  m.Load(kData, { 0xCAFEF00D });
  m.set_reg(t0, kData);
  m.debugger().AddBreakpoint(kBase + 4);
  m.RunUntilHalt(10);

  Debugger::Snapshot snap;
  m.debugger().Capture(&snap, m.pc(), 8);
  Check(snap.halted && snap.reason == Debugger::HaltReason::kBreakpoint, "halted, at a breakpoint");
  CheckEqual(snap.pc, kBase + 4, "the pc");
  CheckEqual(snap.gpr[t0], kData, "a register");
  Check(snap.breakpoints.size() == 1 && snap.breakpoints[0].hits == 1, "the breakpoints and hits");

  // Straight after the load: its value is one stage from its register - the next instruction
  // still reads the old one.
  Check(snap.in_flight && snap.in_flight_reg == t1 && snap.in_flight_value == 0xCAFEF00D,
        "after lw: the load is in flight, with its register and value");
  Check(!snap.landing && snap.gpr[t1] == 0, "and t1 still holds the old value");
  m.debugger().StepInto();
  m.RunUntilHalt(10);
  m.debugger().Capture(&snap, m.pc(), 8);
  Check(snap.landing && snap.landing_reg == t1 && snap.landing_value == 0xCAFEF00D &&
            !snap.in_flight,
        "one instruction on: landing - written before the next instruction reads it");
  m.debugger().StepInto();
  m.RunUntilHalt(10);
  m.debugger().Capture(&snap, m.pc(), 8);
  Check(!snap.landing && !snap.in_flight && snap.gpr[t1] == 0xCAFEF00D, "and then in t1");

  // The disassembly: eight lines centred on the pc, now on the branch at +8.
  CheckEqual(snap.pc, kBase + 12 + 16, "(the branch and its slot ran as one step)");
  m.debugger().Capture(&snap, kBase + 8, 8);
  CheckEqual(snap.lines.size(), 8, "as many lines as asked for");
  CheckEqual(snap.lines[0].address, kBase + 8 - 16, "starting half a window before the centre");
  const Debugger::Line& lw = snap.lines[2];
  Check(lw.readable && lw.address == kBase && lw.text == "lw      t1, 0(t0)", "the lw, as text");
  const Debugger::Line& branch = snap.lines[4];
  Check(branch.has_target && branch.target == kBase + 28 && branch.text.rfind("b ", 0) == 0,
        "beq zero, zero is shown as b, with its target");
  Check(snap.lines[5].delay_slot && !snap.lines[4].delay_slot && !snap.lines[6].delay_slot,
        "the delay slot after it is marked, and only that one");

  m.debugger().Capture(&snap, 0x00000004, 8);
  CheckEqual(snap.lines[0].address, 0, "near address 0 the window starts at 0, not FFFFFFxx");
  m.debugger().Capture(&snap, 0x1F801810, 4);
  Check(!snap.lines[0].readable && !snap.lines[2].readable,
        "a hardware register is not read at all");
  m.debugger().Capture(&snap, 0xBFC00000, 4);
  CheckEqual(snap.lines[0].address, 0xBFC00000 - 8, "a KSEG1 centre lists KSEG1 addresses");

  // Loading a state ends a halt: the machine is somewhere else now. The breakpoints stay.
  m.debugger().AddBreakpoint(kBase + 28);
  Check(m.debugger().halted(), "(still halted, at +28)");
  const char path[] = "debug_test.state";
  const std::string saved = m.system().SaveState(path);
  const std::string loaded = m.system().LoadState(path);
  std::remove(path);
  Check(saved.empty() && loaded.empty(), "(the state saved and loaded)");
  Check(!m.debugger().halted() && m.debugger().breakpoints().size() == 2,
        "loading a state ends the halt and keeps the breakpoints");
}

void TestMemory(Machine& m) {
  Group("memory: reading without side effects, and writing");
  m.Reset();
  auto& io = m.system().io();
  const uint32_t kData = 0x80002000;
  m.Load(kData, { 0x44332211, 0x88776655 });

  uint8_t bytes[8] = {}, ok[8] = {};
  m.debugger().ReadMemory(kData + 2, 4, bytes, ok);
  Check(ok[0] && ok[3] && bytes[0] == 0x33 && bytes[1] == 0x44 && bytes[2] == 0x55 &&
            bytes[3] == 0x66,
        "RAM bytes across a word boundary, little-endian");
  m.debugger().ReadMemory(0x00002000, 1, bytes, ok);
  Check(ok[0] && bytes[0] == 0x11, "the same RAM through KUSEG");

  uint32_t word = 0;
  io.io.interrupt_stat = 0x5;
  Check(m.debugger().PeekData(0x1F801070, &word) && word == 0x5 && io.io.interrupt_stat == 0x5,
        "I_STAT, read as state");
  io.rootcounter_[1].mode.raw = 0;
  io.rootcounter_[1].mode.reached_target = 1;
  Check(m.debugger().PeekData(0xBF801114, &word) && (word & (1u << 11)) != 0 &&
            io.rootcounter_[1].mode.reached_target == 1,
        "a timer's mode, without clearing the reached flag a real read clears");
  Check(m.debugger().PeekData(0x1F801814, &word), "GPUSTAT");
  Check(m.debugger().PeekData(0x1F801C00, &word), "an SPU voice register");
  Check(m.debugger().PeekData(0xFFFE0130, &word) && word == io.io.cache_control,
        "the cache control register");
  Check(!m.debugger().PeekData(0x1F801800, &word) && !m.debugger().PeekData(0x1F801810, &word) &&
            !m.debugger().PeekData(0x1F801040, &word) && !m.debugger().PeekData(0x1F801820, &word),
        "the CD-ROM, GPUREAD, SIO and MDEC are not read: their reads consume");
  Check(!m.debugger().PeekData(0x1F801200, &word) && !m.debugger().PeekData(0x1F900000, &word),
        "nor is an address nothing answers at");

  // Writes.
  std::string error;
  const uint8_t patch[2] = { 0xAA, 0xBB };
  Check(m.debugger().WriteMemory(kData + 1, patch, 2, &error), "a RAM write");
  CheckEqual(*reinterpret_cast<uint32_t*>(m.system().ram() + 0x2000), 0x44BBAA11,
             "lands on the bytes asked for, and only those");
  Check(m.debugger().WriteMemory(0x1F800010, patch, 2, &error) &&
            io.scratchpad.u8[0x10] == 0xAA,
        "a scratchpad write");
  const uint8_t bios_before = io.bios_buffer.u8[0];
  error.clear();
  Check(!m.debugger().WriteMemory(0xBFC00000, patch, 1, &error) &&
            io.bios_buffer.u8[0] == bios_before && error.find("read-only") != std::string::npos,
        "the BIOS is refused, and says why");
  error.clear();
  const uint32_t mask_before = io.io.interrupt_mask;
  Check(!m.debugger().WriteMemory(0x1F801074, patch, 1, &error) &&
            io.io.interrupt_mask == mask_before && error.find("hardware register") != std::string::npos,
        "a hardware register is refused");
  Check(m.debugger().WriteMemory(0x801FFFFF, patch, 2, &error) &&
            m.system().ram()[0x1FFFFF] == 0xAA && m.system().ram()[0] == 0xBB,
        "a write off the end of RAM's 2 MB wraps into its mirror, as the CPU's would");

  // The snapshot carries the memory view the window asked for.
  m.debugger().SetMemoryView(kData, 8);
  Debugger::Snapshot snap;
  m.debugger().Capture(&snap, kBase, 4);
  Check(snap.memory_address == kData && snap.memory.size() == 8 && snap.memory[1] == 0xAA &&
            snap.memory_readable[7] == 1,
        "the snapshot carries the memory view");
  m.debugger().SetMemoryView(0x80000000, 0);
}

void TestPatchingCode(Machine& m, bool recompiler) {
  char name[96];
  snprintf(name, sizeof(name), "patched code runs as patched (%s)",
           recompiler ? "recompiler" : "interpreter");
  Group(name);
  m.Reset();
  // A loop: t0 += 1, forever. Run it long enough to be compiled.
  m.Load(kBase, { ADDIU(t0, t0, 1), BEQ(zero, zero, -8), NOP() });
  m.system().EnableRecompiler(recompiler);
  for (int i = 0; i < 200; ++i)
    m.system().StepInstruction();
  const uint32_t before = m.reg(t0);
  Check(before > 20, "(the loop ran)");

  // Halt at its top, patch the add to add 100, and let it go round.
  m.debugger().AddBreakpoint(kBase);
  m.RunUntilHalt(10);
  const uint32_t add100 = ADDIU(t0, t0, 100);
  std::string error;
  Check(m.debugger().WriteMemory(kBase, reinterpret_cast<const uint8_t*>(&add100), 4, &error),
        "(patched)");
  m.debugger().ClearBreakpoints();
  m.debugger().Resume();
  const uint32_t at_patch = m.reg(t0);
  for (int i = 0; i < 20; ++i)
    m.system().StepInstruction();
  Check(m.reg(t0) - at_patch >= 500, "the new instruction runs, not a stale copy of the old one");

  m.system().EnableRecompiler(false);
}

void TestRegisters(Machine& m) {
  Group("editing registers");
  const uint32_t kData = 0x80002000;
  m.Reset();
  m.Load(kBase, { LW(t1, t0, 0), NOP(), NOP(), ADDIU(t3, zero, 1), NOP(), ADDIU(t2, zero, 7),
                  NOP(), NOP() });
  m.Load(kData, { 0xCAFEF00D });
  m.set_reg(t0, kData);
  m.debugger().AddBreakpoint(kBase + 4);
  m.RunUntilHalt(10);   // right after the lw: its value is in flight to t1

  Check(m.debugger().SetRegister(t1, 0x12345678), "set t1");
  m.debugger().ClearBreakpoints();
  m.debugger().StepInto();
  m.RunUntilHalt(10);
  m.debugger().StepInto();
  m.RunUntilHalt(10);
  CheckEqual(m.reg(t1), 0x12345678, "the load that was in flight does not overwrite the edit");

  Check(!m.debugger().SetRegister(0, 5) && m.reg(zero) == 0, "zero stays zero");
  Check(m.debugger().SetRegister(Debugger::kRegisterHi, 0x11) &&
            m.debugger().SetRegister(Debugger::kRegisterLo, 0x22) &&
            m.system().cpu().context()->high == 0x11 && m.system().cpu().context()->low == 0x22,
        "hi and lo");

  // A new pc while halted: the halt moves there, and the machine resumes from it.
  Check(!m.debugger().SetRegister(Debugger::kRegisterPc, kBase + 2), "a misaligned pc is refused");
  CheckEqual(m.pc(), kBase + 12, "(halted on the addiu t3, 1)");
  Check(m.debugger().SetRegister(Debugger::kRegisterPc, kBase + 20) &&
            m.debugger().halt_pc() == kBase + 20,
        "the pc, past it, and the halt with it");
  m.debugger().StepInto();
  m.RunUntilHalt(10);
  Check(m.reg(t2) == 7 && m.reg(t3) == 0 && m.pc() == kBase + 24,
        "resuming runs from the new pc - the skipped instruction never ran");
}

void TestWatchpoints(Machine& m, bool recompiler) {
  Group(recompiler ? "watchpoints (recompiler on)" : "watchpoints");
  const uint32_t kData = 0x80002000;

  // A write: halts after the store has happened, at the next instruction, and says who.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.Load(kBase, { ADDIU(t1, zero, 0x55), SW(t1, t0, 0), ADDIU(t2, zero, 1), NOP(), NOP() });
  m.set_reg(t0, kData);
  m.debugger().AddWatchpoint(kData, 4, false, true);
  Check(m.debugger().armed(), "a watchpoint arms the debugger");
  const int ran = m.RunUntilHalt(10);
  const Debugger::WatchHit& hit = m.debugger().watch_hit();
  Check(m.debugger().halted() && m.debugger().halt_reason() == Debugger::HaltReason::kWatchpoint,
        "a store to it halts, as a watchpoint");
  CheckEqual(ran, 2, "after the store and the instruction before it");
  CheckEqual(m.pc(), kBase + 8, "on the instruction after the store");
  CheckEqual(m.reg(t2), 0, "which has not run");
  CheckEqual(*reinterpret_cast<uint32_t*>(m.system().ram() + 0x2000), 0x55,
             "the store itself happened");
  Check(hit.valid && hit.dma_channel == -1 && hit.pc == kBase + 4 && hit.write &&
            hit.address == kData && hit.size == 4 && hit.value == 0x55,
        "the hit: the CPU, the store's pc, address, size and value");
  Check(m.debugger().watchpoints()[0].hits == 1, "one hit counted");
  m.debugger().Resume();
  m.RunUntilHalt(3);
  Check(!m.debugger().halted() && m.reg(t2) == 1, "resuming runs on");

  // Reads, and the kinds kept apart.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.Load(kData, { 0x12345678 });
  m.Load(kBase, { LW(t1, t0, 0), NOP(), SW(t1, t0, 4), NOP(), LB(t2, t0, 3), NOP(), NOP() });
  m.set_reg(t0, kData);
  m.debugger().AddWatchpoint(kData, 4, true, false);
  m.RunUntilHalt(10);
  Check(m.debugger().halted() && m.debugger().watch_hit().pc == kBase &&
            !m.debugger().watch_hit().write && m.debugger().watch_hit().value == 0x12345678 &&
            m.debugger().watch_hit().value_known,
        "a read watchpoint catches a lw, with the value it reads");
  m.debugger().Resume();
  m.RunUntilHalt(10);
  Check(m.debugger().halted() && m.debugger().watch_hit().pc == kBase + 16 &&
            m.debugger().watch_hit().size == 1 && m.debugger().watch_hit().value == 0x12,
        "and an lb of its last byte - not the sw beside it, which is a write and outside it");

  // Range edges and RAM's mirrors.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.set_reg(t0, kData);
  m.set_reg(t3, 0xA0002000);   // KSEG1
  m.Load(kBase, { SB(t1, t0, 4), SB(t1, t0, -1), SB(t1, t3, 3), NOP(), NOP() });
  m.debugger().AddWatchpoint(0x00202000, 4, false, true);   // a KUSEG mirror of kData
  m.RunUntilHalt(10);
  Check(m.debugger().halted() && m.debugger().watch_hit().pc == kBase + 8,
        "the bytes either side of the range do not trip it; its last byte, through KSEG1 and "
        "watched through a mirror, does");

  // SWL reads the word it merges into - an emulator detail, not a read by the program.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.set_reg(t0, kData);
  m.Load(kBase, { SWL(t1, t0, 1), NOP(), NOP() });
  m.debugger().AddWatchpoint(kData, 4, true, false);
  m.RunUntilHalt(5);
  Check(!m.debugger().halted(), "swl does not trip a read watchpoint");
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.set_reg(t0, kData);
  m.Load(kBase, { SWL(t1, t0, 1), NOP(), NOP() });
  m.debugger().AddWatchpoint(kData, 4, false, true);
  m.RunUntilHalt(5);
  Check(m.debugger().halted() && m.debugger().watch_hit().write, "but trips a write one");

  // In a delay slot: the slot's own pc, and the halt at the branch target.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.set_reg(t0, kData);
  m.Load(kBase, { BEQ(zero, zero, 12), SW(t1, t0, 0), NOP(), NOP(), NOP(), NOP() });
  m.debugger().AddWatchpoint(kData, 4, false, true);
  m.RunUntilHalt(5);
  Check(m.debugger().halted() && m.debugger().watch_hit().pc == kBase + 4 &&
            m.pc() == kBase + 16,
        "a store in a delay slot: reported at the slot, halted at the branch target");

  // A store with the cache isolated writes nothing, and trips nothing.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.set_reg(t0, kData);
  m.system().cpu().context()->ctrl.SR.raw |= 0x10000;   // IsC
  m.Load(kBase, { SW(t1, t0, 0), NOP(), NOP() });
  m.debugger().AddWatchpoint(kData, 4, false, true);
  m.RunUntilHalt(5);
  Check(!m.debugger().halted(), "an isolated-cache store does not trip a write watchpoint");
  m.system().cpu().context()->ctrl.SR.raw &= ~0x10000u;

  // A DMA write: channel 6 clearing an ordering table over the watched word, started by a CPU
  // store to its control register.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  auto& dma = m.system().io().dma;
  dma.Write(0x1F8010F0, 0x08000000);                  // DPCR: channel 6 on
  dma.Write(0x1F8010E0, (kData & 0x1FFFFF) + 0x3C);   // MADR: the table's last entry
  dma.Write(0x1F8010E4, 16);                          // 16 entries, down to kData
  m.Load(kBase, { LUI(t2, 0x1F80), ORI(t2, t2, 0x10E8), LUI(t1, 0x1100), ORI(t1, t1, 2),
                  SW(t1, t2, 0), NOP(), NOP() });
  m.debugger().AddWatchpoint(kData + 0x10, 4, false, true);
  m.RunUntilHalt(10);
  const Debugger::WatchHit& dhit = m.debugger().watch_hit();
  Check(m.debugger().halted() && dhit.dma_channel == 6 && dhit.write &&
            (dhit.address & 0x1FFFFF) == (kData & 0x1FFFFF) + 0x10,
        "a DMA write trips it, and names channel 6");
  CheckEqual(m.pc(), kBase + 20, "halted after the store that started the transfer");
  CheckEqual(dhit.value, (kData & 0x1FFFFF) + 0x0C, "with the link the channel wrote");

  // Determinism: a loop storing to a watched word, halting each time round and let go, ends
  // exactly where it would have with nothing watched.
  m.Reset();
  m.system().EnableRecompiler(recompiler);
  m.set_reg(t0, kData);
  m.set_reg(t3, 50);
  m.Load(kBase, { ADDIU(t1, t1, 1), SW(t1, t0, 0), BNE(t1, t3, -12), NOP(), ADDIU(t2, zero, 9),
                  NOP(), NOP() });
  m.debugger().AddWatchpoint(kData, 4, false, true);
  int halts = 0;
  for (int i = 0; i < 1000 && m.reg(t2) != 9; ++i) {
    m.RunUntilHalt(1000);
    if (m.debugger().halted()) {
      ++halts;
      m.debugger().Resume();
    }
  }
  CheckEqual(halts, 50, "fifty stores, fifty halts");
  Check(m.reg(t1) == 50 && *reinterpret_cast<uint32_t*>(m.system().ram() + 0x2000) == 50,
        "and the loop still counts to fifty");
  m.debugger().SetWatchpointEnabled(kData, false);
  Check(!m.debugger().armed(), "a disabled watchpoint disarms");
  m.debugger().ClearWatchpoints();
  m.system().EnableRecompiler(false);
}

uint32_t JALR(int d, int s) { return RType(0, s, 0, d, 0x09); }

void TestBiosCalls(Machine& m) {
  Group("the BIOS call log, and breaking on a call");
  // A call the way games make one: the function number in t1, a jump to B0h. At B0h a stand-in
  // for the BIOS that just returns.
  m.Reset();
  m.Load(0x800000B0, { JR(ra), NOP() });
  m.Load(kBase, { ADDIU(t1, zero, 0x3D), ADDIU(4, zero, 0x41), ADDIU(t2, zero, 0xB0),
                  JALR(ra, t2), NOP(), ADDIU(t1, zero, 0x3F), JALR(ra, t2), NOP(), NOP(), NOP() });
  const uint64_t before = m.debugger().bios_calls();
  m.RunUntilHalt(8);
  Check(m.debugger().bios_calls() == before + 2, "both calls are logged, armed or not");
  const std::vector<Debugger::BiosCallRecord> log = m.debugger().BiosLog();
  Check(!log.empty(), "(the log has them)");
  if (log.size() >= 2) {
    const Debugger::BiosCallRecord& call = log[log.size() - 2];
    Check(call.vector == 0xB0 && call.function == 0x3D && call.args[0] == 0x41 &&
              call.ra == kBase + 20,
          "the first: B0h:3Dh, its argument, and where it returns to");
    CheckEqual(log.back().function, 0x3F, "the second, after it");
  }
  Check(emulation::psx::BiosCallName(0xB0, 0x3D) == "putchar(char ch)",
        "B0h:3Dh is putchar, without the CSV's quotes");
  Check(emulation::psx::BiosCallName(0xC0, 0x1C) == "PatchA0Table()", "C0h:1Ch by name");
  Check(emulation::psx::BiosCallName(0xB0, 0xFE) == "B0(FEh)", "a number with no name");

  // Break on B0h:3Fh only.
  m.Reset();
  m.Load(0x800000B0, { JR(ra), NOP() });
  m.Load(kBase, { ADDIU(t1, zero, 0x3D), ADDIU(t2, zero, 0xB0), JALR(ra, t2), NOP(),
                  ADDIU(t1, zero, 0x3F), JALR(ra, t2), NOP(), NOP(), NOP() });
  m.debugger().AddBiosBreak(0xB0, 0x3F);
  Check(m.debugger().armed(), "a BIOS-call break arms the debugger");
  const uint64_t at_start = m.debugger().bios_calls();
  m.RunUntilHalt(20);
  Check(m.debugger().halted() && m.debugger().halt_reason() == Debugger::HaltReason::kBiosCall &&
            m.pc() == 0xB0,
        "halts at the vector for B0h:3Fh - B0h:3Dh went past");
  CheckEqual(static_cast<uint32_t>(m.debugger().bios_calls() - at_start), 1,
             "before the call it halted on is logged");
  m.debugger().Resume();
  m.RunUntilHalt(3);
  CheckEqual(static_cast<uint32_t>(m.debugger().bios_calls() - at_start), 2,
             "and logged once, when it goes on");
  m.debugger().RemoveBiosBreak(0xB0, 0x3F);
  Check(m.debugger().bios_breaks().empty(), "removed");
}

void TestCallStack(Machine& m) {
  Group("the call stack");
  // main calls A, A calls B; A keeps ra in s0.
  m.Reset();
  m.debugger().SetCallTracking(true);
  Check(m.debugger().armed(), "tracking arms the debugger");
  m.Load(kBase, { JAL(kBase + 0x40), NOP(), ADDIU(t3, zero, 7), NOP(), NOP(), NOP() });
  m.Load(kBase + 0x40, { ADDU(s0, ra, zero), JAL(kBase + 0x80), NOP(), ADDU(ra, s0, zero),
                         JR(ra), NOP() });
  m.Load(kBase + 0x80, { ADDIU(t2, zero, 5), NOP(), JR(ra), NOP() });
  m.debugger().AddBreakpoint(kBase + 0x84);
  m.RunUntilHalt(20);
  const std::vector<Debugger::CallFrame>& stack = m.debugger().call_stack();
  Check(m.debugger().halted() && stack.size() == 2, "two deep inside B");
  if (stack.size() == 2) {
    Check(stack[0].call_pc == kBase && stack[0].target == kBase + 0x40 &&
              stack[0].return_to == kBase + 8,
          "main's call of A");
    Check(stack[1].call_pc == kBase + 0x44 && stack[1].target == kBase + 0x80,
          "A's call of B");
  }
  m.debugger().ClearBreakpoints();
  m.debugger().AddBreakpoint(kBase + 8);
  m.debugger().Resume();
  m.RunUntilHalt(20);
  Check(m.debugger().halted() && m.pc() == kBase + 8 && m.debugger().call_stack().empty(),
        "back in main, both returned");
  m.debugger().SetCallTracking(false);
  m.debugger().ClearBreakpoints();
  Check(!m.debugger().armed(), "tracking off, and nothing else set, disarms");
}

void TestLabels(Machine& m) {
  Group("labels");
  m.Reset();
  m.Load(kBase, { JAL(kBase + 0x40), NOP() });
  m.debugger().SetLabel(kBase + 0x40, "UpdatePlayer");
  Debugger::Snapshot snap;
  m.debugger().Capture(&snap, kBase + 0x20, 32);
  bool on_line = false, on_target = false;
  for (const Debugger::Line& line : snap.lines) {
    if (line.address == kBase + 0x40 && line.label == "UpdatePlayer")
      on_line = true;
    if (line.address == kBase && line.target_label == "UpdatePlayer")
      on_target = true;
  }
  Check(on_line, "a label shows on its line");
  Check(on_target, "and against a jal to it");
  Check(m.debugger().Label(0xA0001040) != nullptr, "found through KSEG1 too");
  m.debugger().SetLabel(kBase + 0x40, "");
  Check(m.debugger().Label(kBase + 0x40) == nullptr, "an empty name removes it");
  m.debugger().ClearLabels();
}

void TestDevices(Machine& m) {
  Group("the device panes");
  m.Reset();
  auto& io = m.system().io();
  io.dma.Write(0x1F8010E0, 0x1234);
  io.rootcounter_[2].mode.raw = 0;
  io.rootcounter_[2].mode.reached_target = 1;
  std::vector<Debugger::DeviceRow> rows;
  m.debugger().DescribeDevices(&rows);
  int dma = 0, timers = 0, voices = 0, gpu = 0, cd = 0, irq = 0;
  bool otc = false;
  for (const Debugger::DeviceRow& row : rows) {
    if (row.section == "DMA") ++dma;
    if (row.section == "Timers") ++timers;
    if (row.section == "SPU" && row.name.rfind("Voice", 0) == 0) ++voices;
    if (row.section == "GPU") ++gpu;
    if (row.section == "CD-ROM") ++cd;
    if (row.section == "Interrupts") ++irq;
    if (row.name == "6 OTC" && row.value.find("MADR 00001234") != std::string::npos)
      otc = true;
  }
  Check(dma == 8 && timers == 3 && voices == 24 && gpu == 4 && cd == 3 && irq >= 1,
        "every section: DPCR and seven channels, three timers, 24 voices, the GPU and CD-ROM");
  Check(otc, "a register just written shows in its row");
  Check(io.rootcounter_[2].mode.reached_target == 1,
        "describing the timers does not clear the flag a mode read would");
}

void TestDisassembler() {
  Group("the disassembler");
  using emulation::psx::Disassemble;
  char text[96];
  Disassemble(kBase, JAL(0x80001234), text, sizeof(text));
  Check(strcmp(text, "jal     0x80001234") == 0, "jal and its target");
  Disassemble(kBase, IType(0x01, t0, 0x03, 4), text, sizeof(text));
  Check(strstr(text, "bgez") == text && strstr(text, "alias") != nullptr,
        "a REGIMM alias (rt=3) is named by what it does, and marked");
  Disassemble(kBase, IType(0x01, t0, 0x12, 4), text, sizeof(text));
  Check(strstr(text, "bltz ") == text && strstr(text, "alias") != nullptr,
        "rt=12h does not link - only bits 4-1 of 1000b do");
  Disassemble(kBase, IType(0x01, t0, 0x11, 4), text, sizeof(text));
  Check(strstr(text, "bgezal ") == text && strstr(text, "alias") == nullptr, "rt=11h is bgezal");
  Disassemble(kBase, 0x4A280030, text, sizeof(text));
  Check(strstr(text, "rtpt") == text, "a GTE command by name");
  Disassemble(kBase, RType(0x10, 0, t0, 12, 0), text, sizeof(text));
  Check(strcmp(text, "mfc0    t0, sr") == 0, "a Cop0 register by name");
  Disassemble(kBase, RType(0x12, 0x06, t0, 31, 0), text, sizeof(text));
  Check(strcmp(text, "ctc2    t0, cop2r63") == 0, "a GTE control register numbered as the GTE does");
}

}  // namespace

int main() {
  printf("debug_test - breakpoints and stepping (psx/debugger.h)\n\n");
  Machine m;
  TestBreakpoints(m);
  TestStepInto(m);
  TestStepOver(m);
  TestStepOut(m);
  TestRunToAndBreak(m);
  TestRecompiler(m);
  TestSnapshot(m);
  TestMemory(m);
  TestPatchingCode(m, false);
  TestPatchingCode(m, true);
  TestRegisters(m);
  TestWatchpoints(m, false);
  TestWatchpoints(m, true);
  TestBiosCalls(m);
  TestCallStack(m);
  TestLabels(m);
  TestDevices(m);
  TestDisassembler();
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

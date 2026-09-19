/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

// Breakpoints and stepping - phase 0 of Docs/Debugger-Plan.md.
//
// System::StepInstruction asks this, before each instruction runs, whether to stop there instead.
// If so, the step does nothing at all - no instruction executes, no time passes - and the machine
// is "halted" until Resume. Whoever runs the machine (host::Machine, boot_runner, a harness)
// checks halted() after each step, stops its loop, and does not count that step as an
// instruction. That is what keeps a run that halts a thousand times computing exactly what a run
// that never halted computes: the determinism the plan makes the gate for everything after.
//
// Owned by the machine's thread, like the rest of System.
//
// Addresses are matched physically - the low 29 bits - so a breakpoint on 80010000h fires at
// 00010000h and A0010000h too: the same instruction, reached through a different window.
//
// A limit that comes from the CPU, not from here: a branch and its delay slot run as one step
// (Cpu::Jump executes the slot inside the branch). So stepping into a branch lands after the slot,
// and a breakpoint *on* a delay slot fires only if that instruction is reached some other way.

#include <cstdint>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

class System;

class Debugger {
 public:
  struct Breakpoint {
    uint32_t address = 0;   // as given; matched on its physical address
    bool enabled = true;
    uint64_t hits = 0;
  };

  enum class HaltReason { kNone, kBreakpoint, kStep, kRunTo, kRequested };

  explicit Debugger(System* system) : system_(system) {}

  // ---- Breakpoints -----------------------------------------------------------------------------
  // Adding one that exists already (by physical address) just enables it.
  void AddBreakpoint(uint32_t address);
  void RemoveBreakpoint(uint32_t address);
  void SetBreakpointEnabled(uint32_t address, bool enabled);
  void ClearBreakpoints();
  const std::vector<Breakpoint>& breakpoints() const { return breakpoints_; }

  // ---- Stepping - all of these run the machine on and halt again ------------------------------
  // One step: one instruction, or a branch and its delay slot.
  void StepInto();
  // As StepInto, except that a call (jal, jalr, bltzal, bgezal, syscall) runs to where it returns.
  void StepOver();
  // Runs until the current function returns - the `jr ra` that brings the call depth below where
  // it is now - and halts in the caller.
  void StepOut();
  // Runs until the pc reaches `address` (physically), wherever that is.
  void RunTo(uint32_t address);
  // Halts before the next instruction - the front end's Break.
  void RequestBreak();

  // Lets a halted machine run again, as far as whatever step or breakpoint is set lets it.
  void Resume();

  // A cold boot or reset: whatever step was in progress and any halt are over, the breakpoints
  // stay - they are the person's, set on addresses that mean the same thing in the next boot.
  void Reset();

  bool halted() const { return halted_; }
  HaltReason halt_reason() const { return halt_reason_; }
  uint32_t halt_pc() const { return halt_pc_; }

  // Whether anything could halt the machine. While it is, the machine runs interpreted: the
  // recompiler runs a chain of blocks per step and could not stop between them.
  bool armed() const { return armed_; }

  // ---- From System::StepInstruction, before the instruction at `pc` runs ----------------------
  // True: halt here, and do not run it.
  bool ShouldHalt(uint32_t pc);

  // ---- What the window shows ------------------------------------------------------------------
  // Taken on the machine's thread and handed to the UI's (Docs/Debugger-Plan.md, "Snapshots and
  // requests"): a copy, so the window never reads the machine.

  struct Line {
    uint32_t address = 0;
    uint32_t word = 0;
    bool readable = false;      // RAM, BIOS or scratchpad; anything else is not read at all
    bool delay_slot = false;    // the word before it is a branch or jump
    bool has_target = false;    // a branch or jump whose destination is in the word
    uint32_t target = 0;
    std::string text;
  };

  struct Snapshot {
    bool halted = false;
    HaltReason reason = HaltReason::kNone;
    uint32_t pc = 0;
    uint32_t gpr[32] = {};
    uint32_t hi = 0, lo = 0;
    uint32_t sr = 0, cause = 0, epc = 0, badvaddr = 0;
    uint64_t cycles = 0;
    // The CPU's two-stage load delay, at this instruction boundary. `landing` is written before
    // the next instruction runs, so it reads the new value; `in_flight` is written one
    // instruction later, so the next instruction still reads the old one.
    bool landing = false;
    uint32_t landing_reg = 0, landing_value = 0;
    bool in_flight = false;
    uint32_t in_flight_reg = 0, in_flight_value = 0;
    std::vector<Breakpoint> breakpoints;
    std::vector<Line> lines;    // `count` instructions, starting at `first`
    // The memory view: memory_length bytes from memory_address, and which could be read.
    uint32_t memory_address = 0;
    std::vector<uint8_t> memory;
    std::vector<uint8_t> memory_readable;
  };

  // Fills `out` with the machine's state, `count` instructions of disassembly around `center` -
  // which keeps its segment (a KSEG1 address lists KSEG1 addresses) - and the memory view set by
  // SetMemoryView. Changes nothing.
  void Capture(Snapshot* out, uint32_t center, int count) const;

  // One instruction word, without side effects: RAM, BIOS and scratchpad only. False elsewhere -
  // a hardware register's read can pop a FIFO or acknowledge an interrupt.
  bool Peek(uint32_t address, uint32_t* word) const;

  // ---- Memory and registers - phase 2 ---------------------------------------------------------

  // One aligned word of anything that can be read without changing the machine: memory as Peek,
  // plus the expansion region and the hardware registers whose state can be copied rather than
  // read - memory control, the interrupt registers, DMA, the timers, GPUSTAT, the SPU and the
  // cache control register. False for the rest (the CD-ROM, SIO and MDEC FIFOs, GPUREAD), whose
  // reads consume something, and for addresses nothing answers at.
  //
  // Two values are a little stale by design: a timer's count and a DMA channel's busy bit are
  // brought up to date by running the pending batch of cycles, which a peek must not do.
  bool PeekData(uint32_t address, uint32_t* word) const;

  // `length` bytes from `address`, through PeekData. `readable[i]` is 0 for a byte that could not
  // be read, and its value is 0.
  void ReadMemory(uint32_t address, uint32_t length, uint8_t* bytes, uint8_t* readable) const;

  // Writes into RAM, the scratchpad or the expansion region - memory whose write has no side
  // effect but the write. Anything else, including the BIOS (read-only) and every hardware
  // register, is refused whole, with a reason in `error`. Compiled code built from the bytes is
  // dropped and the instruction cache lines invalidated, as for a DMA, so patched code runs as
  // patched.
  bool WriteMemory(uint32_t address, const uint8_t* bytes, uint32_t length, std::string* error);

  // The registers the window can edit, past the 32 general ones.
  static const int kRegisterHi = 32;
  static const int kRegisterLo = 33;
  static const int kRegisterPc = 34;
  // Sets a register: 1 to 31 (zero stays zero), kRegisterHi, kRegisterLo or kRegisterPc. A load still on its way
  // to that register is dropped, or it would land on top of the edit a step later. A new pc is
  // where the machine resumes; if halted, the halt moves there with it.
  bool SetRegister(int index, uint32_t value);

  // The memory page the window is looking at, for the snapshot a halt sends unasked. Set by the
  // window's requests; it is only a place to look, and changes nothing.
  void SetMemoryView(uint32_t address, uint32_t length) {
    memory_view_ = address;
    memory_view_length_ = length;
  }

 private:
  enum class Kind { kOther, kCall, kReturn };
  enum class Mode { kRun, kInto, kOver, kOut, kRunTo };

  Kind Classify(uint32_t pc) const;
  uint32_t PeekWord(uint32_t address) const;
  void Halt(uint32_t pc, HaltReason reason);
  void UpdateArmed();

  System* system_;
  std::vector<Breakpoint> breakpoints_;
  int enabled_count_ = 0;

  bool halted_ = false;
  HaltReason halt_reason_ = HaltReason::kNone;
  uint32_t halt_pc_ = 0;
  bool armed_ = false;
  bool break_requested_ = false;

  Mode mode_ = Mode::kRun;
  bool skip_first_ = false;       // the instruction a resume starts on has already been decided
  int depth_ = 0;                 // calls entered minus returns taken, since the step began
  uint32_t target_ = 0;           // StepOver and RunTo: the physical address to stop at
  Kind previous_ = Kind::kOther;  // what the last instruction that ran was

  uint32_t memory_view_ = 0x80000000;
  uint32_t memory_view_length_ = 0;
};

}  // namespace psx
}  // namespace emulation

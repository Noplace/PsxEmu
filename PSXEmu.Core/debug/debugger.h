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
#include <map>
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

  enum class HaltReason { kNone, kBreakpoint, kStep, kRunTo, kRequested, kWatchpoint, kBiosCall };

  // ---- Phase 4 --------------------------------------------------------------------------------

  // One call into the BIOS through A0h, B0h or C0h: which function, its first four arguments,
  // where it was called from, and when.
  struct BiosCallRecord {
    uint32_t vector = 0;       // A0h, B0h or C0h
    uint32_t function = 0;     // t1
    uint32_t args[4] = {};     // a0-a3
    uint32_t ra = 0;           // where it returns to - just past the call in the caller
    uint64_t cycle = 0;
  };
  static const int kBiosLogSize = 256;

  // One frame of the approximate call stack: a call taken and not yet returned from.
  struct CallFrame {
    uint32_t call_pc = 0;      // the jal, jalr or linking branch
    uint32_t target = 0;       // where it went
    uint32_t return_to = 0;    // call_pc + 8
    uint32_t sp = 0;           // at entry
  };
  static const int kMaxCallDepth = 256;

  // One line of the device panes: a register or a piece of state, already in words.
  struct DeviceRow {
    std::string section;       // "Interrupts", "DMA", "Timers", "GPU", "CD-ROM", "SPU"
    std::string name;
    std::string value;
    std::string note;
  };

  // A range of memory to stop on when it is read, written, or either - by the CPU, or (writes
  // only) by a DMA channel. Matched physically, and RAM's four 2 MB mirrors are one RAM.
  struct Watchpoint {
    uint32_t address = 0;
    uint32_t length = 4;
    bool read = false;
    bool write = true;
    bool enabled = true;
    uint64_t hits = 0;
  };

  // The access that tripped a watchpoint. The machine halts at the next instruction boundary,
  // after the access - the instruction that made it has finished, so `pc` is where it was and
  // the halt is on whatever comes next.
  struct WatchHit {
    bool valid = false;
    int dma_channel = -1;         // -1: the CPU, at `pc`; 0-6: that DMA channel
    uint32_t pc = 0;
    uint32_t address = 0;         // as accessed
    uint32_t size = 0;            // bytes
    bool write = false;
    uint32_t value = 0;           // what was written, or what was there to be read
    bool value_known = false;     // a read of a register the debugger cannot peek
    uint32_t watchpoint = 0;      // the watchpoint's address
  };

  explicit Debugger(System* system) : system_(system) {}

  // ---- Breakpoints -----------------------------------------------------------------------------
  // Adding one that exists already (by physical address) just enables it.
  void AddBreakpoint(uint32_t address);
  void RemoveBreakpoint(uint32_t address);
  void SetBreakpointEnabled(uint32_t address, bool enabled);
  void ClearBreakpoints();
  const std::vector<Breakpoint>& breakpoints() const { return breakpoints_; }

  // ---- Watchpoints - phase 3 ------------------------------------------------------------------
  // One per start address: adding one where one starts already replaces its length and kinds
  // and enables it.
  void AddWatchpoint(uint32_t address, uint32_t length, bool read, bool write);
  void RemoveWatchpoint(uint32_t address);
  void SetWatchpointEnabled(uint32_t address, bool enabled);
  void ClearWatchpoints();
  const std::vector<Watchpoint>& watchpoints() const { return watchpoints_; }
  // The access behind the last watchpoint halt.
  const WatchHit& watch_hit() const { return watch_hit_; }

  // From Cpu::Load and Cpu::Store, while any watchpoint is enabled: a data access by the
  // instruction at `pc`. `value` is what a write stores; a read's is peeked here.
  void OnCpuAccess(uint32_t pc, uint32_t address, uint32_t size, bool write, uint32_t value);
  // From the DMA channels, through Cpu::NoteExternalWrite: one word written to RAM.
  void OnDmaWrite(int channel, uint32_t ram_offset, uint32_t value);

  // ---- The BIOS call log, and breaking on a call - phase 4 -----------------------------------
  // Every call is logged, armed or not: it is noticed at the same point the kernel's console
  // capture is, and costs a few stores per call. The last kBiosLogSize are kept.
  void OnBiosCall();   // from System::StepInstruction, at A0h/B0h/C0h
  std::vector<BiosCallRecord> BiosLog() const;   // oldest first
  uint64_t bios_calls() const { return bios_calls_; }
  // Halt at the vector - before the function runs - when this function is called.
  void AddBiosBreak(uint32_t vector, uint32_t function);
  void RemoveBiosBreak(uint32_t vector, uint32_t function);
  void ClearBiosBreaks();
  // Each as (vector << 8) | function.
  const std::vector<uint32_t>& bios_breaks() const { return bios_breaks_; }

  // ---- The call stack, approximate - phase 4 --------------------------------------------------
  // MIPS keeps no frame chain, so this is the calls taken and not yet returned from, recorded
  // while tracking is on - which arms the debugger, so the machine runs interpreted. A return
  // pops back to the frame it returns into; one that matches no frame (a longjmp, a return
  // through an exception) leaves the stack alone. Only right for calls made since tracking began.
  void SetCallTracking(bool on);
  bool call_tracking() const { return call_tracking_; }
  const std::vector<CallFrame>& call_stack() const { return call_stack_; }

  // ---- Labels - phase 4 -----------------------------------------------------------------------
  // Names for addresses, shown in the listing and against branch targets. Matched like
  // watchpoints: physically, RAM's mirrors as one. An empty name removes the label.
  void SetLabel(uint32_t address, const std::string& name);
  void ClearLabels();
  const std::string* Label(uint32_t address) const;
  // Every label, keyed by folded address.
  const std::map<uint32_t, std::string>& labels() const { return labels_; }

  // ---- Device panes - phase 4 -----------------------------------------------------------------
  // The interrupt controller, DMA, the timers, the GPU, the CD-ROM and the SPU, described in
  // rows. Read through PeekData and the devices' const accessors, so it changes nothing.
  void DescribeDevices(std::vector<DeviceRow>* rows) const;

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
    std::string label;          // this address's label, if it has one
    std::string target_label;   // the branch target's, if it has one
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
    std::vector<Watchpoint> watchpoints;
    WatchHit watch_hit;         // meaningful when reason is kWatchpoint
    std::vector<Line> lines;    // `count` instructions, starting at `first`
    // The memory view: memory_length bytes from memory_address, and which could be read.
    uint32_t memory_address = 0;
    std::vector<uint8_t> memory;
    std::vector<uint8_t> memory_readable;
    // Phase 4.
    std::vector<BiosCallRecord> bios_log;
    uint64_t bios_calls = 0;
    std::vector<uint32_t> bios_breaks;
    bool call_tracking = false;
    std::vector<CallFrame> call_stack;
    size_t label_count = 0;
    std::vector<DeviceRow> devices;
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

  void Watch(int dma_channel, uint32_t pc, uint32_t address, uint32_t size, bool write,
             uint32_t value, bool value_known);

  std::vector<Watchpoint> watchpoints_;
  int enabled_watchpoints_ = 0;
  bool watch_pending_ = false;    // an access tripped one; halt at the next boundary
  WatchHit watch_hit_;

  // Phase 4.
  void TrackCall(uint32_t call_pc, uint32_t target);
  void TrackReturn(uint32_t to);

  // On the heap: inline, its 12 KB moved the members System keeps after the debugger - the
  // config read every step among them - and the BIOS boot measured 5% slower for it.
  std::vector<BiosCallRecord> bios_log_ = std::vector<BiosCallRecord>(kBiosLogSize);
  uint64_t bios_calls_ = 0;
  std::vector<uint32_t> bios_breaks_;
  bool call_tracking_ = false;
  std::vector<CallFrame> call_stack_;
  uint32_t previous_pc_ = 0;      // the pc previous_ was classified at
  std::map<uint32_t, std::string> labels_;
};

}  // namespace psx
}  // namespace emulation

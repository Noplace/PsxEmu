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

// The one file that knows about both the interpreter and the recompiler.
//
// Everything in PSXEmu.Core/rec was built without a single include from psx/,
// on the rule that the recompiler should be switchable at the end rather than
// grown into the core. This is that switch. It fills in the HostInterface the
// engine asks for - memory access, an instruction fetch, an interpreter - out
// of Cpu, and it is the only place the two meet.
//
// **Off by default.** With no bridge attached, System::StepInstruction is the
// code it always was: same path, same calls, no branch on a flag that matters.
// Attaching one changes when interrupts land and how cycles are charged, which
// is the risk Docs/Recompiler-Plan.md has called the real one from the start,
// so it is opt-in and verified against the regression baselines rather than
// assumed to be equivalent.
//
// Four things it has to get right, none of which are the recompiler's own
// problem:
//
//   1. **Ticking.** The interpreter ticks the rest of the machine once per
//      instruction, from inside ExecuteInstruction. Compiled code does not tick
//      at all, so whatever it ran has to be charged afterwards - Cpu::TickCycles
//      exists for exactly this shape of thing (it is what a DMA uses for the
//      cycles it holds the bus for). A chain of N instructions therefore
//      advances the machine in one burst of N rather than N interleaved ones,
//      which is a real difference in timing granularity and the reason the
//      budget is kept small.
//   2. **Interrupts.** System::StepInstruction checks for a pending interrupt
//      before every instruction. Compiled code runs many instructions per
//      entry, so an interrupt raised during one is not noticed until the chain
//      ends. The budget bounds that latency; it is set in instructions, and
//      one guest instruction is one cycle in this core.
//   3. **Exceptions.** A compiled load or store can raise one. The thunks below
//      set prev_pc first so the exception points at the right instruction, then
//      ask whether the count moved; if it did, the block is told to stop and
//      the machine's own pc - already at the vector - is where execution goes.
//   4. **Entering compiled code at all.** Not while the interpreter has a load
//      in flight, and not when an interrupt is pending: both are states a block
//      has no way to be told about.

#include "psx/cpu.h"
#include "psx/system.h"
#include "rec/recompiler.h"

#include <cstdint>
#include <memory>

namespace emulation {
namespace psx {

class RecompilerBridge {
 public:
  explicit RecompilerBridge(System* system) : system_(system) {
    emulation::rec::HostInterface host;
    host.context = this;
    host.fetch = [this](uint32_t pc, uint32_t* word) { return Fetch(pc, word); };
    host.load32 = &Load32;
    host.load16 = &Load16;
    host.load8 = &Load8;
    host.store32 = &Store32;
    host.store16 = &Store16;
    host.store8 = &Store8;
    host.interpret = [this](uint32_t pc) { return Interpret(pc); };
    host.load_in_flight = [this]() { return cpu()->LoadInFlight(); };

    recompiler_.reset(new emulation::rec::Recompiler(
        host, system_->cpu().context()->gp.reg));
    recompiler_->set_budget(kBudget);

    cpu()->set_store_observer(&StoreObserver, this);
  }

  ~RecompilerBridge() { cpu()->set_store_observer(nullptr, nullptr); }

  RecompilerBridge(const RecompilerBridge&) = delete;
  RecompilerBridge& operator=(const RecompilerBridge&) = delete;

  // How many guest instructions compiled code may run before handing control
  // back. Small, because an interrupt raised inside a chain is not seen until
  // the chain ends - and because the machine is only ticked once the chain is
  // over, so this is also how coarse the CPU's timing is allowed to get.
  static const int32_t kBudget = 64;

  // Runs one step of the machine at the current pc, and returns how many guest
  // instructions ran *as compiled code*.
  //
  // Only those: an interpreted step ticks the machine from inside
  // ExecuteInstruction the way it always has, and would be charged twice if it
  // were counted here. Compiled code ticks nothing, so its instructions are the
  // caller's to charge.
  uint32_t Step() {
    Cpu* const processor = cpu();
    CpuContext* const context = processor->context();

    // The pc in this core points at the *next* instruction while one is
    // executing, and sits on the instruction to run when one is not - which is
    // the state this is called in.
    const uint32_t pc = context->pc;

    const uint32_t next = recompiler_->Step(pc);

    if (next != emulation::rec::Recompiler::kFaulted)
      context->pc = next;
    // On a fault the exception has already moved the pc, and on an interpreted
    // step Interpret() has already left it where the interpreter put it.

    // Cycles, not instructions: a load ticks this machine twice and everything
    // else once, so charging one per instruction would run the machine fast by
    // however many loads the code contains.
    return recompiler_->last_cycles();
  }

  // Thrown away when software says it has replaced code: the cache-control
  // register at 0xFFFE0130.
  void Reset() { recompiler_->Reset(); }

  const emulation::rec::Recompiler::Stats& stats() const {
    return recompiler_->stats();
  }

 private:
  Cpu* cpu() { return &system_->cpu(); }

  // Instruction words for decoding, read straight out of RAM or the BIOS. Not
  // through Cpu::Load: that ticks the machine and can raise an exception, and
  // deciding what to compile should do neither. Anywhere else - code running
  // from the scratchpad or a mirror - simply ends the block and is left to the
  // interpreter.
  bool Fetch(uint32_t pc, uint32_t* word) {
    if ((pc & 3) != 0)
      return false;
    const uint32_t physical = pc & 0x1FFFFFFF;
    // The BIOS call vectors stay the interpreter's, so that the pc arriving at
    // one comes back to System::StepInstruction, which records the call. Today
    // that happens anyway - software reaches them by `jr`, and an indirect
    // jump ends a chain - so this is the guard for what would not: a block
    // running into a vector from the word before it, or indirect jumps being
    // linked one day. cpu_test's biosconsole group is what would notice.
    if (physical == 0xA0 || physical == 0xB0 || physical == 0xC0)
      return false;
    IOInterface& io = system_->io();
    if (physical <= 0x001FFFFF) {
      *word = io.ram_buffer.u32[physical >> 2];
      return true;
    }
    if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF) {
      *word = io.bios_buffer.u32[(physical & 0x0007FFFF) >> 2];
      return true;
    }
    return false;
  }

  // One interpreted instruction, plus its delay slot when it has one - which
  // is what ExecuteInstruction already does, since it runs a branch's slot as
  // a nested execute.
  uint32_t Interpret(uint32_t pc) {
    CpuContext* const context = cpu()->context();
    context->pc = pc;
    cpu()->ExecuteInstruction();
    return context->pc;
  }

  static RecompilerBridge* Of(void* context) {
    return static_cast<RecompilerBridge*>(context);
  }

  // Every compiled access goes through one of these. They do three things
  // beyond the access itself: point prev_pc at the instruction making it, so
  // an exception lands on the right one; notice that an exception was raised;
  // and tell the block to stop when it was.
  uint32_t Access(uint32_t pc, MemorySize size, uint32_t address, bool store,
                  uint32_t value) {
    Cpu* const processor = cpu();
    processor->context()->prev_pc = pc;

    // The step the interpreter takes before every access, and the reason
    // compiled memory did not work until it was here.
    //
    // Cpu::Load and Cpu::Store both test IsBusError() on the way in, and that
    // flag is whatever the *last* address translation left behind. Every one of
    // the interpreter's memory instructions translates the address it is about
    // to use first - `Cpu::LW` and `Cpu::SW` both open with
    // AddressTranslation(virtual_address) - so the flag Load reads is about
    // that access. Compiled code that skips it leaves the flag saying something
    // about a completely different address, and a load eventually reads a stale
    // false and raises a bus error that never happened.
    processor->AddressTranslation(address);

    const uint64_t before = processor->exceptions_raised();

    uint32_t result = 0;
    if (store)
      processor->Store(size, value, address);
    else
      result = processor->Load(size, address);

    if (processor->exceptions_raised() != before)
      recompiler_->SetFault();
    return result;
  }

  static uint32_t Load32(void* c, uint32_t a, uint32_t pc) {
    return Of(c)->Access(pc, kM32, a, false, 0);
  }
  static uint32_t Load16(void* c, uint32_t a, uint32_t pc) {
    return Of(c)->Access(pc, kM16, a, false, 0) & 0xFFFF;
  }
  static uint32_t Load8(void* c, uint32_t a, uint32_t pc) {
    return Of(c)->Access(pc, kM8, a, false, 0) & 0xFF;
  }
  static void Store32(void* c, uint32_t a, uint32_t v, uint32_t pc) {
    Of(c)->Access(pc, kM32, a, true, v);
  }
  static void Store16(void* c, uint32_t a, uint32_t v, uint32_t pc) {
    Of(c)->Access(pc, kM16, a, true, v);
  }
  static void Store8(void* c, uint32_t a, uint32_t v, uint32_t pc) {
    Of(c)->Access(pc, kM8, a, true, v);
  }

  // Everything that writes guest memory, on its way to the block cache: the
  // interpreter's stores one at a time, and a DMA's whole range at once.
  static void StoreObserver(void* context, uint32_t address, uint32_t bytes) {
    Of(context)->recompiler_->NoteStoreRange(address, bytes);
  }

  System* system_;
  std::unique_ptr<emulation::rec::Recompiler> recompiler_;
};

}  // namespace psx
}  // namespace emulation

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
// Five things it has to get right, none of which are the recompiler's own
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
//   5. **Loads that read RAM themselves.** A compiled load from main RAM reads
//      it directly instead of calling Cpu::Load, and the chain is charged
//      Cpu::kRamLoadStall for it - the stall Load would have ticked out inside
//      the call - so the machine advances exactly as far either way. Only while
//      that is all Load would do: Step gives the engine no RAM, and every load
//      calls out, while PGXP wants the word's shadow, the debugger watches
//      loads, the write queue is modelled or the cache is isolated.

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
    host.move = &Move;
    host.interpret = [this](uint32_t pc) { return Interpret(pc); };
    host.load_in_flight_fn = &LoadInFlight;
    host.arm_load = [this](uint32_t reg, uint32_t value) { cpu()->ArmCompiledLoad(reg, value); };
    host.overflow = &Overflow;
    host.hilo = &HiLo;
    host.gte = &GteOp;
    host.sync = &Sync;
    host.ram_bytes = kRamBytes;
    host.ram_window_bytes = kRamWindowBytes;
    host.ram_read_cycles = Cpu::kRamLoadStall;

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

  // Main RAM, IOInterface::ram_buffer: 2 MB from physical address zero.
  static const uint32_t kRamBytes = 0x200000;

  // And its mirrors: Cpu::Load and Cpu::Store decode every physical address up to 8 MB as RAM,
  // reduced to its first 2 MB. Wild Arms makes half its RAM loads through them.
  static const uint32_t kRamWindowBytes = 0x800000;

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

    // PGXP's shadows travel with register copies in compiled code only while it is on - the
    // interpreter's moves carry them the same way (psx/pgxp.h).
    const bool pgxp = system_->pgxp().enabled();
    recompiler_->set_track_moves(pgxp);

    // Compiled loads read RAM themselves only while a load of it is a read and a stall and
    // nothing more (item 5 above). PGXP's shadow comes through Access, below; the rest is
    // Cpu::Load's own. Deciding here decides for the whole chain, which is right: the cache is
    // isolated only by MTC0, which is never compiled; PGXP is settled at the top of every step;
    // a watchpoint arms the debugger, which keeps the machine interpreted. The write queue's
    // setting is latched once a batch, so switching it reaches compiled loads at the next step.
    recompiler_->set_ram(!pgxp && processor->RamLoadIsPlain()
                             ? system_->io().ram_buffer.u8 : nullptr);
    recompiler_->set_ram_store(!pgxp && processor->RamStoreIsPlain()
                                   ? system_->io().ram_buffer.u8 : nullptr);

    // A chain runs for as long as the devices have no use for the machine: the cycles left
    // in the current batch, which is the next event any of them has scheduled. Never less
    // than kBudget, the length of a block and what this always was.
    uint32_t budget = system_->io().CyclesToBatch();
    if (budget < static_cast<uint32_t>(kBudget))
      budget = kBudget;
    recompiler_->set_budget(static_cast<int32_t>(budget));

    const uint32_t next = recompiler_->Step(pc);

    if (next != emulation::rec::Recompiler::kFaulted)
      context->pc = next;
    // On a fault the exception has already moved the pc, and on an interpreted
    // step Interpret() has already left it where the interpreter put it.

    // Cycles, not instructions: a load ticks this machine twice and everything
    // else once, so charging one per instruction would run the machine fast by
    // however many loads the code contains. A load that read RAM directly ticked
    // nothing, and last_cycles carries its stall instead.
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
    if (physical < kRamBytes) {
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

  // A compiled add or addi overflowed: the exception the interpreter's ADD and ADDI raise, with prev_pc
  // on the instruction as they have it, and the block told to stop - as for a memory access that
  // faulted. In a branch's delay slot the exception is the branch's, and RaiseException reads that
  // from the branch flag, which the interpreter's Jump holds up around the slot it runs: EPC is the
  // branch's address and Cause's BD bit is set. Compiled code never sets it, so it is held here for
  // the one call.
  static void Overflow(void* context, uint32_t pc, bool in_delay_slot) {
    RecompilerBridge* self = Of(context);
    Cpu* const processor = self->cpu();
    processor->context()->prev_pc = pc;
    processor->context()->branch_flag = in_delay_slot;
    processor->RaiseException(pc, kOtherException, kExceptionCodeOv);
    processor->context()->branch_flag = false;
    self->recompiler_->SetFault();
  }

  // The multiply and divide unit (Cpu::CompiledHiLo).
  static uint32_t HiLo(void* context, uint32_t funct, uint32_t a, uint32_t b, uint32_t elapsed,
                       uint32_t* extra_cycles) {
    return Of(context)->cpu()->CompiledHiLo(funct, a, b, elapsed, extra_cycles);
  }

  // The machine's clock brought up to where the chain is, before compiled code touches hardware.
  static void Sync(void* context, uint32_t cycles) {
    Of(context)->cpu()->TickCycles(cycles);
  }

  // The GTE (Cpu::CompiledGte).
  static uint32_t GteOp(void* context, uint32_t operation, uint32_t a, uint32_t b, uint32_t elapsed,
                      uint32_t* extra_cycles) {
    return Of(context)->cpu()->CompiledGte(operation, a, b, elapsed, extra_cycles);
  }

  // Asked before every step: a plain function, not a std::function's thunk.
  static bool LoadInFlight(void* context) {
    return Of(context)->cpu()->LoadInFlight();
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
    const uint32_t physical = processor->AddressTranslation(address);

    const uint64_t before = processor->exceptions_raised();

    // PGXP (psx/pgxp.h): a word loaded or stored carries its shadow, as the interpreter's LW and
    // SW do. Compiled code passes no register numbers, so they are read from the instruction
    // itself - the only words compiled to these calls are LW's and SW's.
    Pgxp& pgxp = system_->pgxp();
    uint32_t rt = kNoRegister;
    if (size == kM32 && pgxp.enabled())
      rt = WordRegister(pc, store);

    uint32_t result = 0;
    if (store && rt != kNoRegister) {
      const PreciseVertex& shadow = pgxp.reg(rt);
      if (PreciseVertex* word = pgxp.word(physical))
        *word = shadow;
      pgxp.set_store(&shadow);
      processor->Store(size, value, address);
      pgxp.set_store(nullptr);
    } else if (store) {
      processor->Store(size, value, address);
    } else {
      result = processor->Load(size, address);
    }

    if (processor->exceptions_raised() != before) {
      recompiler_->SetFault();
    } else if (!store && rt != kNoRegister && rt != 0) {
      const PreciseVertex* shadow = pgxp.word(physical);
      pgxp.reg(rt) = shadow != nullptr ? *shadow : PreciseVertex();
      pgxp.set_source(rt, result, physical);
    }
    return result;
  }

  // The register an LW or SW at `pc` loads or stores - kNoRegister if the word there is not one.
  static const uint32_t kNoRegister = 32;
  uint32_t WordRegister(uint32_t pc, bool store) {
    uint32_t word = 0;
    if (!Fetch(pc, &word) || (word >> 26) != (store ? 0x2Bu : 0x23u))
      return kNoRegister;
    return (word >> 16) & 31;
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

  static void Move(void* c, uint32_t to, uint32_t from) {
    Of(c)->system_->pgxp().Move(to, from);
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

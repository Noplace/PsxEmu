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
#include "psx/psx.h"
#include "psx/disasm.h"

#include <algorithm>

namespace emulation {
namespace psx {

namespace {

uint32_t Physical(uint32_t address) { return address & 0x1FFFFFFF; }

const uint32_t kSyscall = 0x0000000C;   // SPECIAL, funct 0Ch

}  // namespace

// ---- Breakpoints ------------------------------------------------------------------------------

void Debugger::AddBreakpoint(uint32_t address) {
  for (Breakpoint& bp : breakpoints_) {
    if (Physical(bp.address) == Physical(address)) {
      bp.enabled = true;
      UpdateArmed();
      return;
    }
  }
  Breakpoint bp;
  bp.address = address;
  breakpoints_.push_back(bp);
  UpdateArmed();
}

void Debugger::RemoveBreakpoint(uint32_t address) {
  for (size_t i = 0; i < breakpoints_.size(); ++i) {
    if (Physical(breakpoints_[i].address) == Physical(address)) {
      breakpoints_.erase(breakpoints_.begin() + i);
      break;
    }
  }
  UpdateArmed();
}

void Debugger::SetBreakpointEnabled(uint32_t address, bool enabled) {
  for (Breakpoint& bp : breakpoints_) {
    if (Physical(bp.address) == Physical(address))
      bp.enabled = enabled;
  }
  UpdateArmed();
}

void Debugger::ClearBreakpoints() {
  breakpoints_.clear();
  UpdateArmed();
}

// ---- Stepping ---------------------------------------------------------------------------------

void Debugger::StepInto() {
  mode_ = Mode::kInto;
  Resume();
}

void Debugger::StepOver() {
  const uint32_t pc = system_->cpu().context()->pc;
  if (PeekWord(pc) == kSyscall) {
    // The BIOS returns from a syscall to the instruction after it, through the exception path
    // rather than a `jr ra`, so this one is a plain run-to at the same depth.
    mode_ = Mode::kOver;
    target_ = Physical(pc + 4);
  } else if (Classify(pc) == Kind::kCall) {
    // Back from the call means past its delay slot, at this depth - not the first time the pc
    // passes that address, which a recursive call would do from further down.
    mode_ = Mode::kOver;
    target_ = Physical(pc + 8);
  } else {
    mode_ = Mode::kInto;
  }
  depth_ = 0;
  Resume();
}

void Debugger::StepOut() {
  mode_ = Mode::kOut;
  depth_ = 0;
  Resume();
}

void Debugger::RunTo(uint32_t address) {
  mode_ = Mode::kRunTo;
  target_ = Physical(address);
  Resume();
}

void Debugger::RequestBreak() {
  break_requested_ = true;
  UpdateArmed();
}

void Debugger::Resume() {
  if (halted_) {
    // The instruction the machine halted on was stopped before it ran. The check that comes
    // first after a resume lets it run, or the machine would halt on it again forever.
    skip_first_ = true;
    halted_ = false;
  }
  previous_ = Kind::kOther;
  UpdateArmed();
}

void Debugger::Reset() {
  halted_ = false;
  halt_reason_ = HaltReason::kNone;
  mode_ = Mode::kRun;
  skip_first_ = false;
  break_requested_ = false;
  depth_ = 0;
  previous_ = Kind::kOther;
  UpdateArmed();
}

// ---- The check --------------------------------------------------------------------------------

bool Debugger::ShouldHalt(uint32_t pc) {
  // Still halted: stays so, without counting the same breakpoint again or disturbing a step.
  if (halted_)
    return true;

  const uint32_t physical = Physical(pc);

  // What the instruction that has just run did to the call depth. A `jr ra` at depth zero is
  // the current function returning to its caller.
  bool returned = false;
  if (previous_ == Kind::kCall) {
    ++depth_;
  } else if (previous_ == Kind::kReturn) {
    if (depth_ == 0)
      returned = true;
    else
      --depth_;
  }
  previous_ = Kind::kOther;

  if (skip_first_) {
    skip_first_ = false;
    previous_ = Classify(pc);
    // A plain continue with nothing else set is disarmed from here on, so compiled code comes
    // straight back - rather than the machine staying interpreted because of one skipped check.
    UpdateArmed();
    return false;
  }

  if (break_requested_) {
    break_requested_ = false;
    Halt(pc, HaltReason::kRequested);
    return true;
  }

  switch (mode_) {
    case Mode::kInto:
      Halt(pc, HaltReason::kStep);
      return true;
    case Mode::kOver:
      if (physical == target_ && depth_ == 0) {
        Halt(pc, HaltReason::kStep);
        return true;
      }
      break;
    case Mode::kOut:
      if (returned) {
        Halt(pc, HaltReason::kStep);
        return true;
      }
      break;
    case Mode::kRunTo:
      if (physical == target_) {
        Halt(pc, HaltReason::kRunTo);
        return true;
      }
      break;
    case Mode::kRun:
      break;
  }

  if (enabled_count_ > 0) {
    for (Breakpoint& bp : breakpoints_) {
      if (bp.enabled && Physical(bp.address) == physical) {
        ++bp.hits;
        Halt(pc, HaltReason::kBreakpoint);
        return true;
      }
    }
  }

  previous_ = Classify(pc);
  return false;
}

void Debugger::Halt(uint32_t pc, HaltReason reason) {
  halted_ = true;
  halt_pc_ = pc;
  halt_reason_ = reason;
  // Every step is one-shot: whatever brought the machine here is done.
  mode_ = Mode::kRun;
  depth_ = 0;
  UpdateArmed();
}

void Debugger::UpdateArmed() {
  enabled_count_ = 0;
  for (const Breakpoint& bp : breakpoints_)
    enabled_count_ += bp.enabled ? 1 : 0;
  armed_ = enabled_count_ > 0 || mode_ != Mode::kRun || break_requested_ || skip_first_;
}

// ---- Reading the program without disturbing it ------------------------------------------------

// Instruction words only, from RAM, the BIOS or the scratchpad - never a hardware register, whose
// read can have side effects. KSEG2 (FFFE0000h, the cache control) is not memory either.
bool Debugger::Peek(uint32_t address, uint32_t* word) const {
  if (address >= 0xC0000000)
    return false;
  const uint32_t physical = Physical(address) & ~3u;
  IOInterface& io = system_->io();
  if (physical < 0x00800000) {
    *word = io.ram_buffer.u32[(physical & 0x1FFFFF) >> 2];
    return true;
  }
  if (physical >= 0x1FC00000 && physical < 0x1FC80000) {
    *word = io.bios_buffer.u32[(physical & 0x7FFFF) >> 2];
    return true;
  }
  if (physical >= 0x1F800000 && physical < 0x1F800400) {
    *word = io.scratchpad.u32[(physical & 0x3FF) >> 2];
    return true;
  }
  return false;
}

bool Debugger::PeekData(uint32_t address, uint32_t* word) const {
  address &= ~3u;
  IOInterface& io = system_->io();
  if (address == 0xFFFE0130) {
    *word = io.io.cache_control;
    return true;
  }
  if (Peek(address, word))
    return true;
  if (address >= 0xC0000000)
    return false;

  const uint32_t physical = Physical(address);
  if (physical >= 0x1F000000 && physical < 0x1F010000) {
    *word = io.parallel_port_buffer.u32[(physical & 0xFFFF) >> 2];
    return true;
  }
  if (physical < 0x1F801000 || physical > 0x1F802FFF)
    return false;

  // Hardware registers: a copy of the state behind them, never the read itself. Each device's
  // own read was checked for side effects before it went in this list; the ones left out have
  // them - a timer's mode read clears its reached flags, and the CD-ROM, SIO, MDEC and GPUREAD
  // registers pop FIFOs.
  switch (physical) {
    case 0x1F801000: *word = io.io.exp1_base_addr; return true;
    case 0x1F801004: *word = io.io.exp2_base_addr; return true;
    case 0x1F801008: *word = io.io.exp1_delay; return true;
    case 0x1F80100C: *word = io.io.exp3_delay; return true;
    case 0x1F801010: *word = io.io.bios_rom; return true;
    case 0x1F801014: *word = io.io.spu_delay; return true;
    case 0x1F801018: *word = io.io.cdrom_delay; return true;
    case 0x1F80101C: *word = io.io.exp2_delay; return true;
    case 0x1F801020: *word = io.io.com_delay; return true;
    case 0x1F801060: *word = io.io.ram_size; return true;
    case 0x1F801070: *word = io.io.interrupt_stat; return true;
    case 0x1F801074: *word = io.io.interrupt_mask; return true;
    case 0x1F801814: *word = system_->gpu_core()->ReadStatus(); return true;
    default: break;
  }
  if (physical >= 0x1F801080 && physical <= 0x1F8010F4 &&
      ((physical & 0xF) != 0xC || physical >= 0x1F8010F0)) {
    *word = io.dma.Read(physical);
    return true;
  }
  if (physical >= 0x1F801100 && physical < 0x1F801130) {
    const RootCounter& counter = io.rootcounter_[(physical >> 4) & 3];
    switch (physical & 0xF) {
      case 0x0: *word = counter.counter; return true;
      case 0x4: *word = counter.mode.raw; return true;   // the field, not ReadMode()
      case 0x8: *word = counter.target; return true;
      default:  return false;
    }
  }
  if (physical >= 0x1F801C00 && physical < 0x1F802000) {
    Spu& spu = system_->spu();
    *word = static_cast<uint32_t>(spu.Read(physical)) |
            (static_cast<uint32_t>(spu.Read(physical + 2)) << 16);
    return true;
  }
  return false;
}

void Debugger::ReadMemory(uint32_t address, uint32_t length, uint8_t* bytes,
                          uint8_t* readable) const {
  uint32_t word = 0;
  uint32_t word_address = 1;   // never aligned: nothing peeked yet
  bool word_ok = false;
  for (uint32_t i = 0; i < length; ++i) {
    const uint32_t at = address + i;
    if ((at & ~3u) != word_address) {
      word_address = at & ~3u;
      word_ok = PeekData(word_address, &word);
    }
    readable[i] = word_ok ? 1 : 0;
    bytes[i] = word_ok ? static_cast<uint8_t>(word >> ((at & 3) * 8)) : 0;
  }
}

bool Debugger::WriteMemory(uint32_t address, const uint8_t* bytes, uint32_t length,
                           std::string* error) {
  IOInterface& io = system_->io();
  // All or nothing: check the whole range first.
  for (uint32_t i = 0; i < length; ++i) {
    const uint32_t at = address + i;
    const uint32_t physical = Physical(at);
    const bool ok = at < 0xC0000000 &&
                    (physical < 0x00800000 ||
                     (physical >= 0x1F800000 && physical < 0x1F800400) ||
                     (physical >= 0x1F000000 && physical < 0x1F010000));
    if (!ok) {
      if (error != nullptr) {
        char text[160];
        if (physical >= 0x1FC00000 && physical < 0x1FC80000)
          snprintf(text, sizeof(text), "%08X is the BIOS, which is read-only.", at);
        else if (physical >= 0x1F801000 && physical <= 0x1F802FFF)
          snprintf(text, sizeof(text),
                   "%08X is a hardware register; writing one does more than store a value, so "
                   "the debugger does not.", at);
        else
          snprintf(text, sizeof(text), "Nothing writable at %08X.", at);
        *error = text;
      }
      return false;
    }
  }

  for (uint32_t i = 0; i < length; ++i) {
    const uint32_t physical = Physical(address + i);
    if (physical < 0x00800000)
      io.ram_buffer.u8[physical & 0x1FFFFF] = bytes[i];
    else if (physical >= 0x1F800000 && physical < 0x1F800400)
      io.scratchpad.u8[physical & 0x3FF] = bytes[i];
    else
      io.parallel_port_buffer.u8[physical & 0xFFFF] = bytes[i];
  }

  // RAM behind the CPU's back, as a DMA writes it: drop what was compiled from it. A range that
  // wraps round RAM's 2 MB mirror is reported in two pieces. (The interpreter fetches straight
  // from RAM - the instruction cache model keeps tags but its data is never read - so there is no
  // cached copy to drop as well.)
  const uint32_t physical = Physical(address);
  if (physical < 0x00800000) {
    const uint32_t offset = physical & 0x1FFFFF;
    const uint32_t first = std::min<uint32_t>(length, 0x200000 - offset);
    system_->cpu().NoteBulkWrite(offset, first);
    if (first < length)
      system_->cpu().NoteBulkWrite(0, length - first);
  }
  return true;
}

bool Debugger::SetRegister(int index, uint32_t value) {
  Cpu& cpu = system_->cpu();
  CpuContext* context = cpu.context();
  if (index >= 1 && index < 32) {
    cpu.CancelLoadsTo(static_cast<uint32_t>(index));
    context->gp.reg[index] = value;
    return true;
  }
  switch (index) {
    case kRegisterHi: context->high = value; return true;
    case kRegisterLo: context->low = value; return true;
    case kRegisterPc:
      if ((value & 3) != 0)
        return false;
      context->pc = value;
      if (halted_)
        halt_pc_ = value;
      return true;
    default:
      return false;
  }
}

uint32_t Debugger::PeekWord(uint32_t address) const {
  uint32_t word = 0;
  return Peek(address, &word) ? word : 0;
}

// ---- Snapshots --------------------------------------------------------------------------------

void Debugger::Capture(Snapshot* out, uint32_t center, int count) const {
  Cpu& cpu = system_->cpu();
  const CpuContext* context = cpu.context();

  out->halted = halted_;
  out->reason = halt_reason_;
  out->pc = halted_ ? halt_pc_ : context->pc;
  for (int r = 0; r < 32; ++r)
    out->gpr[r] = context->gp.reg[r];
  out->gpr[0] = 0;
  out->hi = context->high;
  out->lo = context->low;
  out->sr = context->ctrl.SR.raw;
  out->cause = context->ctrl.Cause;
  out->epc = context->ctrl.EPC;
  out->badvaddr = context->ctrl.BadVaddr;
  out->cycles = context->cycles;
  out->landing = cpu.GetPendingLoad(&out->landing_reg, &out->landing_value);
  out->in_flight = cpu.GetArmedLoad(&out->in_flight_reg, &out->in_flight_value);
  out->breakpoints = breakpoints_;
  out->memory_address = memory_view_;
  out->memory.assign(memory_view_length_, 0);
  out->memory_readable.assign(memory_view_length_, 0);
  if (memory_view_length_ > 0)
    ReadMemory(memory_view_, memory_view_length_, out->memory.data(), out->memory_readable.data());

  // The window starts half a window before the centre, but not below the start of the centre's
  // segment - so KUSEG 0 does not wrap round to FFFFxxxx.
  if (count < 1)
    count = 1;
  center &= ~3u;
  const uint32_t segment = center >= 0xA0000000 ? 0xA0000000u
                           : center >= 0x80000000 ? 0x80000000u
                                                  : 0u;
  const uint32_t half = static_cast<uint32_t>(count / 2) * 4;
  const uint32_t first = (center - segment >= half) ? center - half : segment;

  out->lines.clear();
  out->lines.resize(static_cast<size_t>(count));
  uint32_t before = 0;
  bool previous_readable = first != segment && Peek(first - 4, &before);
  uint32_t previous = before;
  for (int i = 0; i < count; ++i) {
    Line& line = out->lines[static_cast<size_t>(i)];
    line.address = first + static_cast<uint32_t>(i) * 4;
    line.readable = Peek(line.address, &line.word);
    line.delay_slot = previous_readable && HasDelaySlot(previous);
    if (line.readable) {
      char text[96];
      Disassemble(line.address, line.word, text, sizeof(text));
      line.text = text;
      line.has_target = StaticTarget(line.address, line.word, &line.target);
    } else {
      line.word = 0;
    }
    previous_readable = line.readable;
    previous = line.word;
  }
}

// What an instruction does to the call depth. A linking REGIMM branch only enters a function if it
// is taken - it links either way - so its condition is evaluated now, before it runs, on the
// register it will read.
Debugger::Kind Debugger::Classify(uint32_t pc) const {
  const uint32_t code = PeekWord(pc);
  const uint32_t op = code >> 26;
  const uint32_t rs = (code >> 21) & 0x1F;
  const uint32_t rt = (code >> 16) & 0x1F;
  const uint32_t funct = code & 0x3F;

  if (op == 0x03)
    return Kind::kCall;                                    // jal
  if (op == 0x00 && funct == 0x09)
    return Kind::kCall;                                    // jalr
  if (op == 0x00 && funct == 0x08 && rs == 31)
    return Kind::kReturn;                                  // jr ra
  if (op == 0x01 && (rt & 0x1E) == 0x10) {                 // bltzal, bgezal and their aliases
    const int32_t value = static_cast<int32_t>(system_->cpu().context()->gp.reg[rs]);
    const bool taken = (value < 0) != ((rt & 1) != 0);
    return taken ? Kind::kCall : Kind::kOther;
  }
  return Kind::kOther;
}

}  // namespace psx
}  // namespace emulation

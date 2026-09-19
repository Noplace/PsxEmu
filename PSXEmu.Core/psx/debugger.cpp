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
#include <cstdarg>
#include <cstdio>

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

// ---- Watchpoints ------------------------------------------------------------------------------

namespace {

// A watchpoint's idea of an address: physical, and RAM folded onto its first 2 MB, since the
// same byte is reached through all four mirrors.
uint32_t Folded(uint32_t address) {
  const uint32_t physical = address & 0x1FFFFFFF;
  return physical < 0x00800000 ? (physical & 0x1FFFFF) : physical;
}

}  // namespace

void Debugger::AddWatchpoint(uint32_t address, uint32_t length, bool read, bool write) {
  if (length == 0)
    length = 1;
  for (Watchpoint& wp : watchpoints_) {
    if (Folded(wp.address) == Folded(address)) {
      wp.length = length;
      wp.read = read;
      wp.write = write;
      wp.enabled = true;
      UpdateArmed();
      return;
    }
  }
  Watchpoint wp;
  wp.address = address;
  wp.length = length;
  wp.read = read;
  wp.write = write;
  watchpoints_.push_back(wp);
  UpdateArmed();
}

void Debugger::RemoveWatchpoint(uint32_t address) {
  for (size_t i = 0; i < watchpoints_.size(); ++i) {
    if (Folded(watchpoints_[i].address) == Folded(address)) {
      watchpoints_.erase(watchpoints_.begin() + i);
      break;
    }
  }
  UpdateArmed();
}

void Debugger::SetWatchpointEnabled(uint32_t address, bool enabled) {
  for (Watchpoint& wp : watchpoints_) {
    if (Folded(wp.address) == Folded(address))
      wp.enabled = enabled;
  }
  UpdateArmed();
}

void Debugger::ClearWatchpoints() {
  watchpoints_.clear();
  UpdateArmed();
}

void Debugger::OnCpuAccess(uint32_t pc, uint32_t address, uint32_t size, bool write,
                           uint32_t value) {
  bool known = true;
  if (!write) {
    // What the read is about to fetch. A peek, so a register whose read has a side effect is
    // reported without its value rather than read twice.
    uint32_t word = 0;
    known = PeekData(address & ~3u, &word);
    value = word >> ((address & 3) * 8);
  }
  if (size < 4)
    value &= (1u << (size * 8)) - 1;
  Watch(-1, pc, address, size, write, value, known);
}

void Debugger::OnDmaWrite(int channel, uint32_t ram_offset, uint32_t value) {
  Watch(channel, 0, ram_offset & 0x1FFFFC, 4, true, value, true);
}

void Debugger::Watch(int dma_channel, uint32_t pc, uint32_t address, uint32_t size, bool write,
                     uint32_t value, bool value_known) {
  const uint32_t start = Folded(address);
  const uint32_t end = start + size;
  for (Watchpoint& wp : watchpoints_) {
    if (!wp.enabled || !(write ? wp.write : wp.read))
      continue;
    const uint32_t wp_start = Folded(wp.address);
    if (end <= wp_start || start >= wp_start + wp.length)
      continue;
    ++wp.hits;
    // The first access this instruction (or this transfer) made is the one reported; a DMA
    // writing a thousand watched words is one halt, not a thousand.
    if (!watch_pending_) {
      watch_pending_ = true;
      watch_hit_.valid = true;
      watch_hit_.dma_channel = dma_channel;
      watch_hit_.pc = pc;
      watch_hit_.address = address;
      watch_hit_.size = size;
      watch_hit_.write = write;
      watch_hit_.value = value;
      watch_hit_.value_known = value_known;
      watch_hit_.watchpoint = wp.address;
    }
    return;
  }
}

// ---- The BIOS call log ------------------------------------------------------------------------

void Debugger::OnBiosCall() {
  const CpuContext* context = system_->cpu().context();
  BiosCallRecord& record = bios_log_[bios_calls_ % kBiosLogSize];
  record.vector = context->pc & 0xFF;
  record.function = context->gp.t1 & 0xFF;
  record.args[0] = context->gp.a0;
  record.args[1] = context->gp.a1;
  record.args[2] = context->gp.a2;
  record.args[3] = context->gp.a3;
  record.ra = context->gp.ra;
  record.cycle = context->cycles;
  ++bios_calls_;
}

std::vector<Debugger::BiosCallRecord> Debugger::BiosLog() const {
  const uint64_t kept = std::min<uint64_t>(bios_calls_, kBiosLogSize);
  std::vector<BiosCallRecord> log;
  log.reserve(static_cast<size_t>(kept));
  for (uint64_t i = bios_calls_ - kept; i < bios_calls_; ++i)
    log.push_back(bios_log_[i % kBiosLogSize]);
  return log;
}

void Debugger::AddBiosBreak(uint32_t vector, uint32_t function) {
  const uint32_t key = ((vector & 0xFF) << 8) | (function & 0xFF);
  if (std::find(bios_breaks_.begin(), bios_breaks_.end(), key) == bios_breaks_.end())
    bios_breaks_.push_back(key);
  UpdateArmed();
}

void Debugger::RemoveBiosBreak(uint32_t vector, uint32_t function) {
  const uint32_t key = ((vector & 0xFF) << 8) | (function & 0xFF);
  bios_breaks_.erase(std::remove(bios_breaks_.begin(), bios_breaks_.end(), key),
                     bios_breaks_.end());
  UpdateArmed();
}

void Debugger::ClearBiosBreaks() {
  bios_breaks_.clear();
  UpdateArmed();
}

// ---- The call stack ---------------------------------------------------------------------------

void Debugger::SetCallTracking(bool on) {
  if (on && !call_tracking_)
    call_stack_.clear();   // what came before tracking began is unknown, not empty
  call_tracking_ = on;
  UpdateArmed();
}

void Debugger::TrackCall(uint32_t call_pc, uint32_t target) {
  if (call_stack_.size() >= static_cast<size_t>(kMaxCallDepth))
    call_stack_.erase(call_stack_.begin());   // the oldest goes: a runaway recursion, or a leak
  CallFrame frame;
  frame.call_pc = call_pc;
  frame.target = target;
  frame.return_to = call_pc + 8;
  frame.sp = system_->cpu().context()->gp.sp;
  call_stack_.push_back(frame);
}

void Debugger::TrackReturn(uint32_t to) {
  // Back to the frame this returns into - which drops any frames above it that never returned
  // properly. A return into no frame at all changes nothing.
  for (size_t i = call_stack_.size(); i-- > 0;) {
    if (Folded(call_stack_[i].return_to) == Folded(to)) {
      call_stack_.resize(i);
      return;
    }
  }
}

// ---- Labels -----------------------------------------------------------------------------------

void Debugger::SetLabel(uint32_t address, const std::string& name) {
  if (name.empty())
    labels_.erase(Folded(address));
  else
    labels_[Folded(address)] = name;
}

void Debugger::ClearLabels() { labels_.clear(); }

const std::string* Debugger::Label(uint32_t address) const {
  const auto found = labels_.find(Folded(address));
  return found == labels_.end() ? nullptr : &found->second;
}

// ---- Device panes -----------------------------------------------------------------------------

namespace {

std::string Format(const char* format, ...) {
  char text[256];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  return text;
}

const char* CdCommandName(uint32_t command) {
  switch (command) {
    case 0x01: return "Getstat";   case 0x02: return "Setloc";   case 0x03: return "Play";
    case 0x04: return "Forward";   case 0x05: return "Backward"; case 0x06: return "ReadN";
    case 0x07: return "MotorOn";   case 0x08: return "Stop";     case 0x09: return "Pause";
    case 0x0A: return "Init";      case 0x0B: return "Mute";     case 0x0C: return "Demute";
    case 0x0D: return "Setfilter"; case 0x0E: return "Setmode";  case 0x0F: return "Getparam";
    case 0x10: return "GetlocL";   case 0x11: return "GetlocP";  case 0x12: return "SetSession";
    case 0x13: return "GetTN";     case 0x14: return "GetTD";    case 0x15: return "SeekL";
    case 0x16: return "SeekP";     case 0x19: return "Test";     case 0x1A: return "GetID";
    case 0x1B: return "ReadS";     case 0x1C: return "Reset";    case 0x1D: return "GetQ";
    case 0x1E: return "ReadTOC";
    default:   return "?";
  }
}

}  // namespace

void Debugger::DescribeDevices(std::vector<DeviceRow>* rows) const {
  auto add = [rows](const char* section, std::string name, std::string value, std::string note) {
    DeviceRow row;
    row.section = section;
    row.name = std::move(name);
    row.value = std::move(value);
    row.note = std::move(note);
    rows->push_back(std::move(row));
  };
  auto peek = [this](uint32_t address) {
    uint32_t word = 0;
    PeekData(address, &word);
    return word;
  };
  IOInterface& io = system_->io();

  // Interrupts.
  static const char* const kSources[11] = {
    "VBLANK", "GPU", "CD-ROM", "DMA", "Timer 0", "Timer 1", "Timer 2", "Pad/card", "SIO", "SPU",
    "Lightpen",
  };
  const uint32_t stat = io.io.interrupt_stat;
  const uint32_t mask = io.io.interrupt_mask;
  add("Interrupts", "I_STAT / I_MASK", Format("%04X / %04X", stat & 0xFFFF, mask & 0xFFFF),
      (stat & mask) != 0 ? "one is pending and enabled" : "");
  for (int i = 0; i < 11; ++i) {
    const bool pending = (stat >> i) & 1;
    const bool enabled = (mask >> i) & 1;
    if (pending || enabled)
      add("Interrupts", kSources[i], pending ? "pending" : "-", enabled ? "enabled" : "masked");
  }

  // DMA.
  static const char* const kChannels[7] = {
    "0 MDEC in", "1 MDEC out", "2 GPU", "3 CD-ROM", "4 SPU", "5 PIO", "6 OTC",
  };
  static const char* const kSync[4] = { "burst", "slice", "linked list", "sync 3" };
  const uint32_t dpcr = peek(0x1F8010F0);
  const uint32_t dicr = peek(0x1F8010F4);
  add("DMA", "DPCR / DICR", Format("%08X / %08X", dpcr, dicr),
      (dicr & 0x80000000u) ? "master interrupt flag set" : "");
  for (int c = 0; c < 7; ++c) {
    const uint32_t base = 0x1F801080 + static_cast<uint32_t>(c) * 0x10;
    const uint32_t madr = peek(base), bcr = peek(base + 4), chcr = peek(base + 8);
    const bool on = (dpcr >> (c * 4 + 3)) & 1;
    add("DMA", kChannels[c], Format("MADR %08X  BCR %08X  CHCR %08X", madr, bcr, chcr),
        Format("%s, %s RAM, %s%s", on ? "on" : "off", (chcr & 1) ? "from" : "to",
               kSync[(chcr >> 9) & 3], (chcr & 0x01000000) ? ", busy" : ""));
  }

  // Timers.
  static const char* const kTimers[3] = { "Timer 0 (dot)", "Timer 1 (hblank)", "Timer 2 (1/8)" };
  for (int t = 0; t < 3; ++t) {
    const RootCounter& rc = io.rootcounter_[t];
    const uint32_t mode = rc.mode.raw;
    std::string note = Format("clock %u", (mode >> 8) & 3);
    if (mode & 1) note += Format(", sync mode %u", (mode >> 1) & 3);
    if (mode & 8) note += ", wraps at target";
    if (mode & 0x10) note += ", IRQ at target";
    if (mode & 0x20) note += ", IRQ at FFFFh";
    if (mode & 0x40) note += " (repeat)";
    if ((mode & 0x400) == 0) note += ", IRQ requested";
    if (mode & 0x800) note += ", reached target";
    if (mode & 0x1000) note += ", reached FFFFh";
    add("Timers", kTimers[t],
        Format("count %04X  target %04X  mode %04X", rc.counter & 0xFFFF, rc.target & 0xFFFF,
               mode & 0xFFFF),
        note);
  }

  // GPU.
  Gpu& gpu = system_->gpu();
  const uint32_t gpustat = gpu.status_raw();
  static const int kWidths[4] = { 256, 320, 512, 640 };
  const int width = (gpustat & (1u << 16)) ? 368 : kWidths[(gpustat >> 17) & 3];
  const bool interlaced = (gpustat >> 22) & 1;
  const int height = ((gpustat >> 19) & 1) && interlaced ? 480 : 240;
  add("GPU", "GPUSTAT", Format("%08X", gpustat),
      Format("%dx%d, %s, %s%s%s", width, height, (gpustat & (1u << 20)) ? "PAL" : "NTSC",
             (gpustat & (1u << 21)) ? "24-bit" : "15-bit", interlaced ? ", interlaced" : "",
             (gpustat & (1u << 23)) ? ", display off" : ""));
  add("GPU", "Display", Format("VRAM %u,%u", gpu.display_vram_x(), gpu.display_vram_y()),
      Format("dots %u-%u, scanline %u, %.2f Hz", gpu.horizontal_display_start(),
             gpu.horizontal_display_end(), gpu.scanline(), gpu.refresh_hz()));
  add("GPU", "Texture page", Format("%u,%u", (gpustat & 0xF) * 64, ((gpustat >> 4) & 1) * 256),
      Format("%s-bit, semi-transparency %u%s",
             ((gpustat >> 7) & 3) == 0 ? "4" : ((gpustat >> 7) & 3) == 1 ? "8" : "15",
             (gpustat >> 5) & 3, (gpustat & (1u << 9)) ? ", dithered" : ""));
  add("GPU", "Frames", Format("%llu", static_cast<unsigned long long>(gpu.frame_count())),
      Format("%llu primitives, %llu GP0 words",
             static_cast<unsigned long long>(gpu.stats().primitives),
             static_cast<unsigned long long>(gpu.stats().gp0_words)));

  // CD-ROM.
  const Cdrom& cd = io.cdrom;
  const Cdrom::Stats& cs = cd.stats();
  add("CD-ROM", "Disc", cd.disc_loaded() ? "in" : "none",
      cd.disc_loaded() ? Format("last sector delivered %u", cd.delivered_lba()) : "");
  add("CD-ROM", "Last command", Format("%02X %s", cs.last_command, CdCommandName(cs.last_command)),
      Format("%llu commands, %llu unknown", static_cast<unsigned long long>(cs.commands),
             static_cast<unsigned long long>(cs.unknown_commands)));
  add("CD-ROM", "Sectors", Format("%llu read", static_cast<unsigned long long>(cs.sectors_read)),
      Format("%llu XA (%llu filtered out), %llu CD-DA",
             static_cast<unsigned long long>(cs.xa_sectors),
             static_cast<unsigned long long>(cs.xa_filtered),
             static_cast<unsigned long long>(cs.cdda_sectors)));

  // SPU.
  const uint32_t control = peek(0x1F801DA8) >> 16;         // 1F801DAAh
  const uint32_t status = peek(0x1F801DAC) >> 16;          // 1F801DAEh
  const uint32_t endx = peek(0x1F801D9C);
  add("SPU", "Control / status", Format("%04X / %04X", control, status),
      Format("%s%s%s", (control & 0x8000) ? "on" : "off", (control & 0x4000) ? ", unmuted" : ", muted",
             (control & 0x0080) ? ", reverb" : ""));
  for (int v = 0; v < 24; ++v) {
    const uint32_t base = 0x1F801C00 + static_cast<uint32_t>(v) * 16;
    const uint32_t volumes = peek(base), pitch_start = peek(base + 4), adsr = peek(base + 8),
                   level_repeat = peek(base + 12);
    const uint32_t pitch = pitch_start & 0xFFFF;
    const uint32_t start = (pitch_start >> 16) * 8;
    const uint32_t level = level_repeat & 0xFFFF;
    const uint32_t repeat = (level_repeat >> 16) * 8;
    add("SPU", Format("Voice %d", v),
        Format("pitch %04X  start %05X  repeat %05X  level %04X", pitch, start, repeat, level),
        Format("%u Hz, vol %04X/%04X, ADSR %08X%s%s", pitch * 44100 / 4096, volumes & 0xFFFF,
               volumes >> 16, adsr, level != 0 ? ", sounding" : "",
               ((endx >> v) & 1) ? ", ended" : ""));
  }
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
  watch_pending_ = false;
  watch_hit_ = WatchHit();
  // A new boot's calls are nothing to do with the old one's. The BIOS breaks, the tracking
  // setting and the labels stay, like the breakpoints.
  call_stack_.clear();
  bios_calls_ = 0;
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
    if (call_tracking_)
      TrackCall(previous_pc_, pc);
  } else if (previous_ == Kind::kReturn) {
    if (depth_ == 0)
      returned = true;
    else
      --depth_;
    if (call_tracking_)
      TrackReturn(pc);
  }
  previous_ = Kind::kOther;

  if (skip_first_) {
    skip_first_ = false;
    previous_ = Classify(pc);
    previous_pc_ = pc;
    // A plain continue with nothing else set is disarmed from here on, so compiled code comes
    // straight back - rather than the machine staying interpreted because of one skipped check.
    UpdateArmed();
    return false;
  }

  // An access the last instruction made (or a DMA during it) tripped a watchpoint. First, ahead
  // of any step or breakpoint here: it is about what already happened.
  if (watch_pending_) {
    watch_pending_ = false;
    Halt(pc, HaltReason::kWatchpoint);
    return true;
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

  if (!bios_breaks_.empty() && (physical == 0xA0 || physical == 0xB0 || physical == 0xC0)) {
    const uint32_t key = (physical << 8) | (system_->cpu().context()->gp.t1 & 0xFF);
    if (std::find(bios_breaks_.begin(), bios_breaks_.end(), key) != bios_breaks_.end()) {
      Halt(pc, HaltReason::kBiosCall);
      return true;
    }
  }

  previous_ = Classify(pc);
  previous_pc_ = pc;
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
  enabled_watchpoints_ = 0;
  for (const Watchpoint& wp : watchpoints_)
    enabled_watchpoints_ += wp.enabled ? 1 : 0;
  armed_ = enabled_count_ > 0 || enabled_watchpoints_ > 0 || mode_ != Mode::kRun ||
           break_requested_ || skip_first_ || watch_pending_ || !bios_breaks_.empty() ||
           call_tracking_;
  // The CPU and the DMA channels report accesses only while there is something to match them
  // against: one flag, tested per load and store.
  system_->cpu().set_debug_watch(enabled_watchpoints_ > 0);
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
  out->watchpoints = watchpoints_;
  out->watch_hit = watch_hit_;
  out->bios_log = BiosLog();
  out->bios_calls = bios_calls_;
  out->bios_breaks = bios_breaks_;
  out->call_tracking = call_tracking_;
  out->call_stack = call_stack_;
  out->label_count = labels_.size();
  out->devices.clear();
  DescribeDevices(&out->devices);
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
    if (!labels_.empty()) {
      if (const std::string* name = Label(line.address))
        line.label = *name;
    }
    if (line.readable) {
      char text[96];
      Disassemble(line.address, line.word, text, sizeof(text));
      line.text = text;
      line.has_target = StaticTarget(line.address, line.word, &line.target);
      if (line.has_target && !labels_.empty()) {
        if (const std::string* name = Label(line.target))
          line.target_label = *name;
      }
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

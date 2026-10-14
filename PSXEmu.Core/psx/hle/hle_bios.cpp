// The HLE BIOS's core: the ROM, the boot, the kernel's layout in RAM, the exception chains, events
// and threads, and the A0h/B0h/C0h tables. See psx/hle_bios.h for the design, and
// Docs/HLE-BIOS.md for the plan.
//
// Sources, in the order they were trusted: psx-spx's "Kernel (BIOS)" chapters (in the repository
// as psx-spx.txt); the SCPH-1001 kernel's own RAM as boot_runner shows it once the shell is up
// (for the layout games patch against); and, where psx-spx is silent or vague, PCSX-Redux's
// OpenBIOS (MIT licence, Copyright (c) 2019-2024 PCSX-Redux authors), a rewrite of the SCPH-7001
// kernel that keeps its behaviour, bugs included - the default handlers' auto-acknowledge rules
// and the pad driver's order of work are taken from it.

#include "psx/psx.h"
#include "psx/hle/hle_bios.h"
#include "psx/mips_assembler.h"

#include <cstddef>

namespace emulation {
namespace psx {
namespace hle {

namespace {

const uint32_t kIStat = 0x1F801070;
const uint32_t kIMask = 0x1F801074;

// The slots: 400h function numbers of 10h bytes each, from kTrapBase. B(5Bh), the pad and card
// driver, is the exception: games reach into it by offset (psx-spx "BIOS Patches" - B(5Bh)+7A0h,
// +884h, +894h, +9C8h, +1988h...), so it gets 8 KB of its own, where a patch's stores land on ROM
// and are discarded and the three entry points games call are traps at their own offsets.
const uint32_t kSlotBytes = 0x10;
const uint32_t kSlotRegion = 0x4000;
const uint32_t kB5BOffset = 0x10000;
const uint32_t kSetPadOutputOffset = kB5BOffset + 0x7A0;
const uint32_t kSetPadEnableOffset = kB5BOffset + 0x884;
const uint32_t kClearPadEnableOffset = kB5BOffset + 0x894;

const uint32_t kKernelDate = 0x19951204;
const char kKernelVersion[] = "CEX-3000/1001/1002 by K.S.";
const char kRomVersion[] = "System ROM Version 2.2 12/04/95 A";

// Event classes and specs (psx-spx "BIOS Event Summary").
const uint32_t kEventClassCdrom = 0xF0000003;
const uint32_t kEventClassException = 0xF0000010;
const uint32_t kEventClassCard = 0xF0000011;
const uint32_t kEventClassCardHigh = 0xF4000001;
const uint32_t kEventClassRootCounter = 0xF2000000;
const uint32_t kStatusFree = 0;
const uint32_t kStatusDisabled = 0x1000;
const uint32_t kStatusEnabled = 0x2000;
const uint32_t kStatusReady = 0x4000;
const uint32_t kModeCallback = 0x1000;
const uint32_t kModeReady = 0x2000;

// The default interrupt handler's IRQs, in the order it tests them, and the event each one
// delivers - timer 2 delivering timer 1's class is the kernel's own copy-paste slip, kept.
const int kDefIntOrder[11] = { 2, 9, 1, 10, 8, 0, 4, 5, 6, 7, 3 };
const uint32_t kDefIntClass[11] = { 0xF0000003, 0xF0000009, 0xF0000002, 0xF000000A,
                                    0xF000000B, 0xF0000001, 0xF0000005, 0xF0000006,
                                    0xF0000006, 0xF0000008, 0xF0000004 };

const uint32_t kExeHeaderBytes = 0x3C;   // what LoadExeHeader copies: file bytes 10h-4Bh

uint32_t Kseg0(uint32_t address) { return 0x80000000u | (address & 0x1FFFFFFF); }

// The character sets' places in the ROM, and charset 2's codes: the runs of assigned Shift-JIS
// cells in the order the SCPH-1001's Krom2Offset numbers them - measured by calling it for every
// code from 8140h to 84BEh (Docs/HLE-BIOS.md). Charset 3, the level-1 kanji, is arithmetic.
const uint32_t kCharset2Rom = 0x66000;
const uint32_t kCharset3Rom = 0x69D68;
const uint32_t kCharset4Rom = 0x7F8DE;
const uint16_t kCharset2Runs[][2] = {
  {0x8140, 0x817E}, {0x8180, 0x81AC}, {0x81B8, 0x81BF}, {0x81C8, 0x81CE}, {0x81DA, 0x81E8},
  {0x81F0, 0x81F7}, {0x81FC, 0x81FC}, {0x824F, 0x8258}, {0x8260, 0x8279}, {0x8281, 0x829A},
  {0x829F, 0x82F1}, {0x8340, 0x837E}, {0x8380, 0x8396}, {0x839F, 0x83B6}, {0x83BF, 0x83D6},
  {0x8440, 0x8460}, {0x8470, 0x847E}, {0x8480, 0x8491}, {0x849F, 0x84BE},
};

}  // namespace

extern const uint8_t kHleCharset2[15720];
extern const uint8_t kHleCharset3[88950];
extern const uint8_t kHleCharset4[1425];

// Which glyph of its character set `code` is, and that set's base address; FFFFFFFFh for a code
// with none.
uint32_t HleBios::KromOffset(uint32_t code, uint32_t* base) {
  if (code >= 0x8140 && code <= 0x84BE) {
    uint32_t offset = 0;
    for (const auto& run : kCharset2Runs) {
      if (code >= run[0] && code <= run[1]) {
        if (base != nullptr)
          *base = 0xBFC00000 | kCharset2Rom;
        return offset + (code - run[0]);
      }
      offset += run[1] - run[0] + 1;
    }
    return 0xFFFFFFFF;
  }
  if (code >= 0x889F && code <= 0x9872) {
    const uint32_t low = code & 0xFF;
    if (low < 0x40 || low == 0x7F || low > 0xFC)
      return 0xFFFFFFFF;
    if (base != nullptr)
      *base = 0xBFC00000 | kCharset3Rom;
    return ((code >> 8) - 0x88) * 188 + (low - 0x40) - (low >= 0x80 ? 1 : 0) - 94;
  }
  return 0xFFFFFFFF;
}

// ---- The ROM -----------------------------------------------------------------------------------

uint32_t HleBios::TrapAddress(uint32_t function) {
  const uint32_t base = 0xA0000000u | kTrapBase;
  switch (function) {
    case kTableB + 0x5B: return base + kB5BOffset;
    case kSetPadOutput: return base + kSetPadOutputOffset;
    case kSetPadEnableFlag: return base + kSetPadEnableOffset;
    case kClearPadEnableFlag: return base + kClearPadEnableOffset;
    default: return base + function * kSlotBytes;
  }
}

void HleBios::BuildRom(uint8_t* rom) {
  memset(rom, 0, System::kBiosSize);
  auto put32 = [rom](uint32_t offset, uint32_t value) { memcpy(rom + offset, &value, 4); };

  // The reset vector is a trap like any other: kReset's slot. BFC00000 jumps to it, so a pc
  // anywhere else in the ROM is still a word nothing executes.
  {
    MipsAssembler a(0xBFC00000);
    a.La(kK0, TrapAddress(kReset));
    a.Jr(kK0);
    a.Nop();
    for (size_t i = 0; i < a.words().size(); ++i)
      put32(static_cast<uint32_t>(i * 4), a.words()[i]);
  }

  // The kernel's header, which GetSystemInfo reads and games compare: the date as BCD, the
  // console type, and the maker's string. Then the GUI's version, which the SCPH-1001 has too.
  put32(0x100, kKernelDate);
  put32(0x104, 3);
  memcpy(rom + 0x108, kKernelVersion, sizeof(kKernelVersion));
  memcpy(rom + 0x7FF32, kRomVersion, sizeof(kRomVersion));

  // The character sets, where the SCPH-1001 keeps them (psx-spx "BIOS Character Sets"): charset
  // 2's 16x15 symbols and kana, charset 3's kanji, charset 4's 8x15 ASCII. Shinonome's glyphs, in
  // the public domain - hle_font_data.cpp, made by tools/make_hle_font.cpp.
  memcpy(rom + kCharset2Rom, kHleCharset2, sizeof(kHleCharset2));
  memcpy(rom + kCharset3Rom, kHleCharset3, sizeof(kHleCharset3));
  memcpy(rom + kCharset4Rom, kHleCharset4, sizeof(kHleCharset4));

  // Every slot reads as "jr ra; nop" with its function number after - nothing executes it, but a
  // disassembly of a table entry then says what it is.
  const uint32_t trap_offset = kTrapBase - 0x1FC00000;
  for (uint32_t function = 0; function < kInternalEnd; ++function) {
    const uint32_t at = trap_offset + function * kSlotBytes;
    put32(at + 0, 0x03E00008);
    put32(at + 4, 0x00000000);
    put32(at + 8, 0x48454C00 | (function >> 8));   // "\0LEH" and the table
    put32(at + 12, function & 0xFF);
  }

  // A(9Dh) GetConf: a game that wants to change the configuration without SetConf's
  // reallocation decodes where it lives from GetConf's first two instructions - a lui and a
  // load whose immediates add up to it plus 8 (psx-spx "set_conf_without_realloc", Spec Ops
  // Airborne Commando). So those two are written to say where KernelVars keeps it.
  {
    const uint32_t conf = kVars + static_cast<uint32_t>(offsetof(KernelVars, conf_tcb)) + 8;
    MipsAssembler a(TrapAddress(kTableA + 0x9D));
    a.Lui(kT0, static_cast<uint16_t>((conf + 0x8000) >> 16));
    a.Lw(kT1, static_cast<int16_t>(conf & 0xFFFF), kT0);
    const uint32_t at = trap_offset + (kTableA + 0x9D) * kSlotBytes;
    put32(at + 0, a.words()[0]);
    put32(at + 4, a.words()[1]);
  }
}

// ---- Guest memory ------------------------------------------------------------------------------

CpuContext& HleBios::cpu() { return *system_->cpu().context(); }

uint8_t* HleBios::RamPointer(uint32_t address) {
  const uint32_t physical = address & 0x1FFFFFFF;
  if (physical < 0x00800000)
    return system_->ram() + (physical & 0x1FFFFF);
  if (physical >= 0x1F800000 && physical < 0x1F800400)
    return system_->io().scratchpad.u8 + (physical & 0x3FF);
  return nullptr;
}

HleBios::KernelVars& HleBios::vars() {
  static_assert(sizeof(KernelVars) <= kChains - kVars, "KernelVars runs into the chains");
  return *reinterpret_cast<KernelVars*>(system_->ram() + kVars);
}

uint8_t HleBios::Read8(uint32_t address) {
  if (const uint8_t* p = RamPointer(address))
    return *p;
  const uint32_t physical = address & 0x1FFFFFFF;
  if (physical >= 0x1FC00000 && physical < 0x1FC80000)
    return system_->bios()[physical & 0x7FFFF];
  if (physical >= 0x1F801000 && physical < 0x1F803000)
    return system_->io().Read08(physical);
  return 0xFF;
}

uint16_t HleBios::Read16(uint32_t address) {
  return static_cast<uint16_t>(Read8(address) | (Read8(address + 1) << 8));
}

uint32_t HleBios::Read32(uint32_t address) {
  if (const uint8_t* p = RamPointer(address & ~3u)) {
    uint32_t value;
    memcpy(&value, p, 4);
    return value;
  }
  const uint32_t physical = address & 0x1FFFFFFC;
  if (physical >= 0x1FC00000 && physical < 0x1FC80000) {
    uint32_t value;
    memcpy(&value, system_->bios() + (physical & 0x7FFFF), 4);
    return value;
  }
  if (physical >= 0x1F801000 && physical < 0x1F803000)
    return system_->io().Read32(physical);
  return 0xFFFFFFFF;
}

void HleBios::Write8(uint32_t address, uint8_t value) { WriteBlock(address, &value, 1); }

void HleBios::Write16(uint32_t address, uint16_t value) { WriteBlock(address, &value, 2); }

void HleBios::Write32(uint32_t address, uint32_t value) { WriteBlock(address, &value, 4); }

void HleBios::ReadBlock(uint32_t address, void* out, uint32_t bytes) {
  uint8_t* o = static_cast<uint8_t*>(out);
  for (uint32_t i = 0; i < bytes; ++i)
    o[i] = Read8(address + i);
}

// A write to RAM tells the recompiler, as any store does - the kernel loads executables and
// copies code like anything else that writes memory.
void HleBios::WriteBlock(uint32_t address, const void* data, uint32_t bytes) {
  const uint8_t* d = static_cast<const uint8_t*>(data);
  uint32_t i = 0;
  while (i < bytes) {
    const uint32_t at = address + i;
    uint8_t* p = RamPointer(at);
    if (p == nullptr) {
      i += 1;   // ROM, I/O or nothing: the kernel writes none of these this way
      continue;
    }
    // A run inside one region, so it can be one copy and one note.
    const uint32_t physical = at & 0x1FFFFFFF;
    const bool ram = physical < 0x00800000;
    const uint32_t region_left = ram ? 0x200000 - (physical & 0x1FFFFF) : 0x400 - (physical & 0x3FF);
    const uint32_t run = (bytes - i) < region_left ? (bytes - i) : region_left;
    memcpy(p, d + i, run);
    if (ram)
      system_->cpu().NoteBulkWrite(physical & 0x1FFFFF, run);
    i += run;
  }
}

void HleBios::FillBlock(uint32_t address, uint8_t value, uint32_t bytes) {
  std::vector<uint8_t> fill(bytes, value);
  if (bytes != 0)
    WriteBlock(address, &fill[0], bytes);
}

std::string HleBios::ReadString(uint32_t address, uint32_t limit) {
  std::string text;
  for (uint32_t i = 0; i < limit; ++i) {
    const char c = static_cast<char>(Read8(address + i));
    if (c == '\0')
      break;
    text.push_back(c);
  }
  return text;
}

void HleBios::WriteString(uint32_t address, const std::string& text) {
  WriteBlock(address, text.c_str(), static_cast<uint32_t>(text.size() + 1));
}

uint32_t HleBios::Arg(int index) {
  CpuContext& c = cpu();
  if (index < 4)
    return c.gp.reg[kA0 + index];
  return Read32(c.gp.sp + 0x10 + (index - 4) * 4);
}

uint32_t HleBios::ReadIo32(uint32_t address) { return system_->io().Read32(address & 0x1FFFFFFF); }
uint16_t HleBios::ReadIo16(uint32_t address) { return system_->io().Read16(address & 0x1FFFFFFF); }
void HleBios::WriteIo32(uint32_t address, uint32_t value) {
  if (address >= 0xFFFE0000)
    system_->io().Write32(address, value);   // cache control: not translated
  else
    system_->io().Write32(address & 0x1FFFFFFF, value);
}
void HleBios::WriteIo16(uint32_t address, uint16_t value) {
  system_->io().Write16(address & 0x1FFFFFFF, value);
}
void HleBios::WriteIo8(uint32_t address, uint8_t value) {
  system_->io().Write08(address & 0x1FFFFFFF, value);
}

// ---- Returning -----------------------------------------------------------------------------------

void HleBios::Return(uint32_t v0) {
  cpu().gp.v0 = v0;
  cpu().pc = cpu().gp.ra;
}

void HleBios::ReturnVoid() { cpu().pc = cpu().gp.ra; }

void HleBios::Message(const std::string& text) {
  for (const std::string& seen : messages_)
    if (seen == text)
      return;
  if (messages_.size() < 256) {
    messages_.push_back(text);
    if (new_messages_.size() < 32)
      new_messages_.push_back(text);
  }
}

void HleBios::Console(const std::string& text) {
  for (char c : text)
    system_->kernel().WriteConsoleChar(c);
}

void HleBios::SystemError(const char* what) {
  char text[160];
  snprintf(text, sizeof(text), "system error: %s (pc=%08X ra=%08X)", what, cpu().pc, cpu().gp.ra);
  Message(text);
  // The kernel's system errors jump to themselves forever. The pc stays on the trap, so every
  // step comes back here; interrupts, if they are on, are still taken.
}

// ---- Patches -------------------------------------------------------------------------------------

// The offsets into B(5Bh)'s driver games are known to patch (psx-spx "BIOS Patches"), what each
// patch was for, and whether this kernel has to do anything to honour it.
void HleBios::RomWrite(uint32_t physical, uint32_t value) {
  const uint32_t offset = (physical & 0x1FFFFFFF) - kTrapBase;
  if (offset >= kSlotRegion && offset < kTrapSize && offset >= kB5BOffset) {
    const uint32_t at = offset - kB5BOffset;
    if (at >= 0x62C && at < 0x650) {
      // patch_no_pad_card_auto_ack (Resident Evil 2, Sporting Clays): zeroes over the code that
      // acknowledges vblank after the pads are read, so the game's own vblank handler gets it.
      if (value == 0)
        vars().pad_ack_removed = 1;
      return;
    }
    // patch_pad_error_handling (594h-5BFh: nothing here goes wrong in a way it would handle),
    // patch_optional_pad_output (3D8h-4FFh: SetPadOutput's bytes are never clipped here),
    // patch_card_specific_delay (9C4h-9E3h) and patch_card_info_step4 (1988h): timing and a
    // stray byte of the kernel's own card driver, which this one does not have.
    if ((at >= 0x594 && at < 0x5C0) || (at >= 0x3D8 && at < 0x500) || (at >= 0x9C4 && at < 0x9E4) ||
        at == 0x1988)
      return;
    char text[96];
    snprintf(text, sizeof(text), "an unknown patch to the pad and card driver at B(5Bh)+%Xh", at);
    Message(text);
    return;
  }
  if (offset < kTrapSize) {
    char text[96];
    snprintf(text, sizeof(text), "a write into the kernel's functions at %08X, ignored",
             physical | 0xA0000000);
    Message(text);
  }
}

// ---- The trap ------------------------------------------------------------------------------------

uint32_t HleBios::Trap(uint32_t pc) {
  file_event_fd_ = 0xFFFFFFFF;
  const uint32_t offset = (pc & 0x1FFFFFFF) - kTrapBase;
  uint32_t function;
  if (offset < kSlotRegion) {
    if ((offset & (kSlotBytes - 1)) != 0) {
      char text[96];
      snprintf(text, sizeof(text), "a jump into the middle of a kernel function: %08X", pc);
      Message(text);
      return kTrapCycles;   // and stay there
    }
    function = offset / kSlotBytes;
  } else if (offset == kB5BOffset) {
    function = kTableB + 0x5B;
  } else if (offset == kSetPadOutputOffset) {
    function = kSetPadOutput;
  } else if (offset == kSetPadEnableOffset) {
    function = kSetPadEnableFlag;
  } else if (offset == kClearPadEnableOffset) {
    function = kClearPadEnableFlag;
  } else {
    char text[96];
    snprintf(text, sizeof(text), "a jump into the pad and card driver at %08X", pc);
    Message(text);
    return kTrapCycles;
  }

  cost_ = function < kInternal ? CostOf(function) : kTrapCycles;
  Dispatch(function);

  // A call that has finished and costs more than a step should take is held at kDelay until its
  // time has passed. Not one still waiting on its own trap (WaitEvent), whose cost is per look,
  // and not one inside another's delay - an interrupt handler calling the kernel while a file
  // loads - which is charged as it stands.
  KernelVars& v = vars();
  if (cost_ > kDelayThreshold && cpu().pc != pc && function != kDelay && v.delay_active == 0) {
    const uint64_t until = cpu().cycles + cost_;
    v.delay_active = 1;
    v.delay_until_low = static_cast<uint32_t>(until);
    v.delay_until_high = static_cast<uint32_t>(until >> 32);
    v.delay_return = cpu().pc;
    cpu().pc = TrapAddress(kDelay);
    return kTrapCycles;
  }
  return cost_;
}

void HleBios::Dispatch(uint32_t function) {
  if (trace_ != nullptr && function != kDelay && function != kShell) [[unlikely]] {
    static const char* const kInternalNames[] = {
      "Reset", "Resume", "Unknown", "ExeReturned", "DoExecuteReturned", "SyscallVerifier",
      "CdIoVerifier", "TimerVerifier0", "TimerVerifier1", "TimerVerifier2", "TimerVerifier3",
      "TimerHandler0", "TimerHandler1", "TimerHandler2", "TimerHandler3", "PadCardVerifier",
      "PadCardHandler", "DefIntVerifier", "SetPadOutput", "SetPadEnableFlag",
      "ClearPadEnableFlag", "Delay", "Shell" };
    const CpuContext& c = cpu();
    std::string name;
    if (function < kInternal) {
      const uint32_t vector = function < kTableB ? 0xA0 : (function < kTableC ? 0xB0 : 0xC0);
      name = BiosCallName(vector, function & 0xFF);
    } else if (function - kInternal < sizeof(kInternalNames) / sizeof(kInternalNames[0])) {
      name = kInternalNames[function - kInternal];
    }
    fprintf(trace_, "%10llu  %-40.40s a0=%08X a1=%08X a2=%08X a3=%08X ra=%08X\n",
            static_cast<unsigned long long>(c.cycles), name.c_str(), c.gp.a0, c.gp.a1, c.gp.a2,
            c.gp.a3, c.gp.ra);
  }
  if (function < kTableB) {
    FunctionA(function - kTableA);
    return;
  }
  if (function < kTableC) {
    FunctionB(function - kTableB);
    return;
  }
  if (function < kInternal) {
    FunctionC(function - kTableC);
    return;
  }

  CpuContext& c = cpu();
  switch (function) {
    case kReset:
      Reset();
      return;
    case kResume:
      Resume();
      return;
    case kUnknown:
      SystemError("a table entry with no function");
      return;
    case kExeReturned:
      // The boot executable returned: SystemErrorBootOrDiskFailure('B', 38Ch).
      SystemError("the boot executable returned");
      return;
    case kDoExecuteReturned: {
      // DoExecute's executable returned: put back what DoExecute saved and return 1 to its caller.
      const uint32_t header = vars().exec_header;
      c.gp.sp = Read32(header + 0x28);
      c.gp.fp = Read32(header + 0x2C);
      c.gp.gp = Read32(header + 0x30);
      c.gp.ra = Read32(header + 0x34);
      c.gp.s0 = Read32(header + 0x38);
      Return(1);
      return;
    }
    case kSyscallVerifier: {
      Tcb* tcb = At<Tcb>(CurrentTcb());
      const uint32_t code = tcb->cause & 0x3C;
      if (code == 0x00) {   // an interrupt: someone else's
        Return(0);
        return;
      }
      if (code == 0x20) {   // syscall
        tcb->epc += 4;
        switch (tcb->reg[kA0]) {
          case 0:
            break;
          case 1:   // EnterCriticalSection
            tcb->reg[kV0] = (tcb->sr & 0x404) == 0x404 ? 1 : 0;
            tcb->sr &= ~0x404u;
            break;
          case 2:   // ExitCriticalSection
            tcb->sr |= 0x404;
            break;
          case 3:   // ChangeThreadSubFunction: the old thread gets 1, the new one runs
            Write32(Read32(kTableOfTables + 0x08), tcb->reg[kA1]);
            tcb->reg[kV0] = 1;
            break;
          default: {
            Then then;
            then.kind = kThenReturnFromException;
            DeliverEvent(kEventClassException, 0x4000, then);
            return;
          }
        }
        ReturnFromException();
        return;
      }
      // Neither: an exception nothing handles.
      Then then;
      then.kind = kThenSyscallUnresolved;
      DeliverEvent(kEventClassException, 0x1000, then);
      return;
    }
    case kCdIoVerifier:
      CdIoVerifier();
      return;
    case kTimerVerifier0:
    case kTimerVerifier1:
    case kTimerVerifier2:
    case kTimerVerifier3: {
      const uint32_t timer = function - kTimerVerifier0;
      const uint32_t bit = timer == 3 ? 1u : (0x10u << timer);
      if ((ReadIo32(kIMask) & bit) == 0 || (ReadIo32(kIStat) & bit) == 0) {
        Return(0);
        return;
      }
      Then then;
      then.a = c.gp.ra;
      then.b = 1;
      DeliverEvent(kEventClassRootCounter | timer, 2, then);
      return;
    }
    case kTimerHandler0:
    case kTimerHandler1:
    case kTimerHandler2:
    case kTimerHandler3: {
      const uint32_t timer = function - kTimerHandler0;
      const uint32_t bit = timer == 3 ? 1u : (0x10u << timer);
      if (vars().timer_autoack[timer] == 0) {
        ReturnVoid();
        return;
      }
      WriteIo32(kIStat, ~bit);
      ReturnFromException();
      return;
    }
    case kPadCardVerifier:
      Return((ReadIo32(kIMask) & ReadIo32(kIStat) & 1) != 0 ? 1 : 0);
      return;
    case kPadCardHandler:
      PadVblank();
      if (vars().pad_autoack != 0 && vars().pad_ack_removed == 0)
        WriteIo32(kIStat, ~1u);
      if (vars().card_started != 0) {
        CardVblank(c.gp.ra);
        return;
      }
      ReturnVoid();
      return;
    case kDefIntVerifier:
      DefIntFrom(0, c.gp.ra);
      return;
    case kSetPadOutput:
      // SetPadOutput(src1, blah1, src2, blah2): what to send the pads in place of zeroes.
      vars().pad_output[0] = Arg(0);
      vars().pad_output[1] = Arg(2);
      ReturnVoid();
      return;
    case kSetPadEnableFlag:
      vars().pad_enable = 1;
      ReturnVoid();
      return;
    case kClearPadEnableFlag:
      vars().pad_enable = 0;
      ReturnVoid();
      return;
    case kShell:
      ShellFrame();
      return;
    case kDelay: {
      KernelVars& v = vars();
      const uint64_t until =
          (static_cast<uint64_t>(v.delay_until_high) << 32) | v.delay_until_low;
      if (v.delay_active == 0 || c.cycles >= until) {
        v.delay_active = 0;
        c.pc = v.delay_return;
        cost_ = kTrapCycles;
        return;
      }
      // Still going: a slice at a time, so interrupts land within a slice of when they would.
      const uint64_t left = until - c.cycles;
      cost_ = left < kDelaySlice ? static_cast<uint32_t>(left) : kDelaySlice;
      return;
    }
    default:
      SystemError("an unknown kernel entry point");
      return;
  }
}

// ---- Boot ----------------------------------------------------------------------------------------

namespace {

// SYSTEM.CNF: BOOT, TCB, EVENT and STACK, KEY = VALUE, the numbers in hex.
struct BootConfig {
  std::string boot;
  std::string argument;
  uint32_t tcbs = 4;
  uint32_t events = 0x10;
  uint32_t stack = 0x801FFF00;
};

void ParseConfig(const std::vector<uint8_t>& contents, BootConfig* config) {
  const std::string text(contents.begin(), contents.end());
  size_t position = 0;
  while (position < text.size()) {
    size_t end = text.find_first_of("\r\n", position);
    if (end == std::string::npos)
      end = text.size();
    const std::string line = text.substr(position, end - position);
    position = end + 1;
    const size_t equals = line.find('=');
    if (equals == std::string::npos)
      continue;
    std::string key = line.substr(0, equals);
    std::string value = line.substr(equals + 1);
    auto trim = [](std::string* s) {
      const size_t b = s->find_first_not_of(" \t");
      const size_t e = s->find_last_not_of(" \t");
      *s = b == std::string::npos ? std::string() : s->substr(b, e - b + 1);
    };
    trim(&key);
    trim(&value);
    for (char& ch : key)
      ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    if (key == "BOOT") {
      const size_t space = value.find_first_of(" \t");
      config->boot = value.substr(0, space);
      if (space != std::string::npos) {
        config->argument = value.substr(space + 1);
        trim(&config->argument);
      }
    } else if (key == "TCB") {
      config->tcbs = strtoul(value.c_str(), nullptr, 16);
    } else if (key == "EVENT") {
      config->events = strtoul(value.c_str(), nullptr, 16);
    } else if (key == "STACK") {
      config->stack = strtoul(value.c_str(), nullptr, 16);
    }
  }
}

}  // namespace

void HleBios::Reset() {
  messages_.clear();
  CpuContext& c = cpu();

  // What the disc says, before anything is built from it.
  BootConfig config;
  bool pal = false;
  std::vector<uint8_t> image;
  bool have_image = false;
  const bool exe = system_->auto_boot_exe_;
  if (exe) {
    system_->auto_boot_exe_ = false;
    FILE* fp = fopen(system_->auto_boot_exe_path_.c_str(), "rb");
    if (fp != nullptr) {
      fseek(fp, 0, SEEK_END);
      const long size = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      if (size > 0) {
        image.resize(static_cast<size_t>(size));
        have_image = fread(&image[0], 1, image.size(), fp) == image.size();
      }
      fclose(fp);
    }
    if (!have_image)
      Message("the executable to side-load could not be read: " + system_->auto_boot_exe_path_);
  } else if (OpenDisc()) {
    Iso9660& iso = system_->iso();
    // The licence text in sector 4 says whose console the disc was pressed for.
    uint8_t licence[2048];
    if (iso.ReadLogicalSector(4, licence)) {
      const std::string text(reinterpret_cast<const char*>(licence), sizeof(licence));
      pal = text.find("Europe") != std::string::npos;
    }
    Iso9660::File file;
    if (iso.Find("SYSTEM.CNF", &file)) {
      std::vector<uint8_t> contents;
      if (iso.Read(file, &contents))
        ParseConfig(contents, &config);
    } else {
      config.boot = "cdrom:\\PSX.EXE;1";
    }
    if (!config.boot.empty())
      have_image = LoadFileImage(config.boot, &image);
    if (!have_image)
      Message("the boot executable could not be read: " + config.boot);
  }

  // The boot is instant, by design: no intro, and no time loading the executable either - that
  // is what someone choosing the HLE BIOS gets for not having a dump. Everything after it is timed.
  cost_ = kTrapCycles;

  InitHardware(pal);
  BuildKernel(config.tcbs, config.events, config.stack);
  CdInit();

  // The kernel is up: the POST code the real one writes here, which is also what
  // System::set_auto_boot_exe watches for (a no-op now, the side-load is done above).
  WriteIo8(0x1F802041, 7);

  if (!have_image) {
    Console(exe ? "HLE BIOS: no executable\n" : "HLE BIOS: no disc to boot\n");
    // Nothing to run: the shell (hle_shell.cpp), until a disc that can be is put in.
    ShellStart();
    return;
  }

  // BOOT's argument, where the kernel keeps it.
  WriteString(kBootArgument, config.argument.substr(0, 0x7F));
  strncpy(vars().boot_path, config.boot.c_str(), sizeof(vars().boot_path) - 1);

  const uint32_t header = kExecHeaderBuffer;
  if (!LoadExecutable(image, header)) {
    Message("the boot executable is not a PS-X EXE");
    ShellStart();
    return;
  }

  // The stack: SYSTEM.CNF's for a disc, the header's own (or the default) for a side-loaded
  // executable, as System::LoadPsExeFromMemory has always done.
  if (exe) {
    const uint32_t s_addr = Read32(header + 0x20);
    const uint32_t s_size = Read32(header + 0x24);
    Write32(header + 0x20, s_addr != 0 ? s_addr + s_size : 0x801FFF00);
  } else {
    Write32(header + 0x20, config.stack);
  }
  Write32(header + 0x24, 0);
  vars().boot_stack = Read32(header + 0x20);

  // DoExecute(headerbuf, 1, 0), as the kernel starts every boot executable - COP2 on, interrupts
  // off until the program says otherwise (psx-spx "BIOS Boot State").
  c.ctrl.SR.raw = 0x40000000;
  c.ctrl.Cause = 0;
  StartExecutable(header, 1, 0, TrapAddress(kExeReturned));
}

void HleBios::InitHardware(bool pal) {
  // Memory control, as the kernel leaves it (psx-spx "BIOS Boot State").
  WriteIo32(0x1F801000, 0x1F000000);
  WriteIo32(0x1F801004, 0x1F802000);
  WriteIo32(0x1F801008, 0x0013243F);
  WriteIo32(0x1F80100C, 0x00003022);
  WriteIo32(0x1F801010, 0x0013243F);
  WriteIo32(0x1F801014, 0x200931E1);
  WriteIo32(0x1F801018, 0x00020943);
  WriteIo32(0x1F80101C, 0x00070777);
  WriteIo32(0x1F801020, 0x0000132C);
  WriteIo32(0x1F801060, 0x00000B88);
  WriteIo32(0xFFFE0130, 0x0001E988);

  // No interrupts until something asks for them.
  WriteIo32(kIMask, 0);
  WriteIo32(kIStat, 0);

  // A display the game can see before it sets its own up - some never do (psx-spx: Saga
  // Frontier needs GP1(03h)=0) - 320x240, or 320x256 on a European disc, at the usual position.
  WriteIo32(0x1F801814, 0x00000000);                        // reset
  WriteIo32(0x1F801814, 0x05000000);                        // display area at 0,0
  WriteIo32(0x1F801814, 0x06C60260);                        // horizontal range 260h-C60h
  WriteIo32(0x1F801814, pal ? 0x07049025 : 0x07040010);     // vertical range
  WriteIo32(0x1F801814, pal ? 0x08000009 : 0x08000001);     // 320 wide, NTSC or PAL
  WriteIo32(0x1F801814, 0x03000000);                        // display on

  // The SPU on and audible, which is how the intro leaves it.
  WriteIo16(0x1F801DAA, 0xC000);
  WriteIo16(0x1F801D80, 0x3FFF);
  WriteIo16(0x1F801D82, 0x3FFF);
}

void HleBios::BuildKernel(uint32_t tcbs, uint32_t evcbs, uint32_t stacktop) {
  // The kernel's 64 KB, empty, then filled.
  FillBlock(0x00000000, 0, 0x10000);

  Write32(0x60, 2);       // RAM size in megabytes
  Write32(0x64, 0);
  Write32(0x68, 0xFF);

  BuildVectorsAndHandler();

  // The tables. Every entry a trap; A(00h-09h) and A(3Bh-3Eh) are the B functions they share
  // (C(1Ch) AdjustA0Table), and C(06h) is the exception handler itself, not a trap.
  for (uint32_t n = 0; n < 0xC0; ++n) {
    uint32_t function = kTableA + n;
    if (n <= 0x09)
      function = kTableB + 0x32 + n;
    else if (n >= 0x3B && n <= 0x3E)
      function = kTableB + 0x3C + (n - 0x3B);
    Write32(kATable + n * 4, TrapAddress(function));
  }
  for (uint32_t n = 0; n < 0x100; ++n)
    Write32(kBTable + n * 4, TrapAddress(kTableB + n));
  for (uint32_t n = 0; n < 0x80; ++n)
    Write32(kCTable + n * 4, n == 0x06 ? kExceptionHandler : TrapAddress(kTableC + n));

  // What the kernel remembers.
  KernelVars& v = vars();
  v.conf_tcb = tcbs;
  v.conf_evcb = evcbs;
  v.conf_stacktop = stacktop;
  v.timer_autoack[0] = v.timer_autoack[1] = v.timer_autoack[2] = v.timer_autoack[3] = 1;
  v.pad_autoack = 1;
  v.pad_enable = 1;
  v.card_status[0] = v.card_status[1] = 1;
  strncpy(v.cd_directory, "\\", sizeof(v.cd_directory) - 1);

  // The exception stack, and the exit structure the handler leaves through: ReturnFromException,
  // with the stack top less 4 - "for whatever reason" - and zeroes (B(18h)).
  Write32(kExceptionStackVar, kExceptionStackTop);
  Write32(kDefaultExit + 0x00, TrapAddress(kTableB + 0x17));
  Write32(kDefaultExit + 0x04, kExceptionStackTop - 4);
  Write32(kCurrentExit, kDefaultExit);

  AllocateControlBlocks(tcbs, evcbs);
  InstallDefaultHandlers();
  InstallDevices();
}

// The pieces of the kernel that are code in RAM: the four-word vectors, the dispatchers behind
// A0h/B0h/C0h, and the exception handler - laid out instruction for instruction where the
// SCPH-1001 kernel has them, because that is what games verify and patch.
void HleBios::BuildVectorsAndHandler() {
  auto emit = [this](MipsAssembler& a) {
    a.Finish();
    WriteBlock(a.origin(), &a.words()[0], static_cast<uint32_t>(a.words().size() * 4));
  };

  // C(07h) InstallExceptionHandlers: the vector, and the same four words at 0 - which is also
  // the "garbage" at address 0 that games reading through null pointers depend on.
  for (uint32_t at : { 0x00u, kExceptionVector }) {
    MipsAssembler a(at);
    a.Lui(kK0, 0);
    a.Addiu(kK0, kK0, static_cast<int16_t>(kExceptionHandler));
    a.Jr(kK0);
    a.Nop();
    emit(a);
  }

  // A(45h) init_a0_b0_c0_vectors.
  const uint32_t dispatch[3] = { kADispatch, kBDispatch, kCDispatch };
  for (int i = 0; i < 3; ++i) {
    MipsAssembler a(0xA0 + i * 0x10);
    a.Lui(kT0, 0);
    a.Addiu(kT0, kT0, static_cast<int16_t>(dispatch[i]));
    a.Jr(kT0);
    a.Nop();
    emit(a);
  }

  // The dispatchers: t1 is the function number, and nothing checks it.
  {
    MipsAssembler a(kADispatch);
    a.Addiu(kT0, kR0, static_cast<int16_t>(kATable));
    a.Sll(kT1, kT1, 2);
    a.Add(kT0, kT0, kT1);
    a.Lw(kT0, 0, kT0);
    a.Nop();
    a.Jr(kT0);
    a.Nop();
    emit(a);
  }
  for (int i = 0; i < 2; ++i) {
    MipsAssembler a(i == 0 ? kBDispatch : kCDispatch);
    a.Lui(kT0, 0);
    a.Addiu(kT0, kT0, static_cast<int16_t>(i == 0 ? kBTable : kCTable));
    a.Sll(kT1, kT1, 2);
    a.Add(kT0, kT0, kT1);
    a.Lw(kT0, 0, kT0);
    a.Nop();
    a.Jr(kT0);
    a.Nop();
    emit(a);
  }
  {
    // Two helpers the kernel keeps beside them.
    MipsAssembler a(0x620);
    a.Mfc0(kV0, 13);
    a.Nop();
    a.Jr(kRa);
    a.Nop();
    a.Mfc0(kV0, 12);
    a.Nop();
    a.Jr(kRa);
    a.Nop();
    emit(a);
  }

  // The exception handler, C(06h). Offsets that matter, relative to its start (psx-spx "BIOS
  // Patches"; C(06h)+n below):
  //   00-0F  four nops, user hook 0
  //   10-37  k0 = the current TCB's registers; at, v0, v1 and ra saved - a game's
  //          cop0r13 patch rewrites 00-37 with its own copy of this, ending "mfc0 v0, cause"
  //   38     jal to the helper that reads cause and epc - which is what makes that patch
  //          harmless here, as on any kernel from 1995 on
  //   3C-6F  step epc over a GTE command an interrupt arrived on; save epc
  //   70-AF  sixteen nops: the early card IRQ hook (70h, read by Metal Gear Solid's patch: its
  //          immediates must be zero while nothing is installed), the lightgun hook (80h, which
  //          games fill with lui/addiu/jalr/nop), and user hooks 1 and 2
  //   B0-    everything else saved, then the four priority chains, then out through the exit
  //          structure [75D0h]
  {
    MipsAssembler a(kExceptionHandler);
    a.Nops(4);                                    // 00
    a.Addiu(kK0, kR0, kTableOfTables);            // 10
    a.Lw(kK0, 8, kK0);
    a.Nop();
    a.Lw(kK0, 0, kK0);
    a.Nop();
    a.Addi(kK0, kK0, 8);
    a.Sw(kAt, 4, kK0);                            // 28
    a.Sw(kV0, 8, kK0);
    a.Sw(kV1, 12, kK0);
    a.Sw(kRa, 124, kK0);
    a.Jal(Kseg0(kExceptionCauseEpc));             // 38
    a.Nop();
    const int not_interrupt = a.NewLabel();
    a.Andi(kV0, kV0, 0x3C);                       // 40
    a.Bne(kV0, kR0, not_interrupt);
    a.Nop();
    a.Lw(kV0, 0, kV1);                            // the word at epc: a GTE command?
    a.Nop();
    a.Srl(kV0, kV0, 24);
    a.Andi(kV0, kV0, 0xFE);
    a.Addiu(kAt, kR0, 0x4A);
    a.Bne(kV0, kAt, not_interrupt);
    a.Nop();
    a.Addi(kV1, kV1, 4);                          // 68: then it already ran - step over it
    a.Bind(not_interrupt);
    a.Sw(kV1, 128, kK0);                          // 6C: epc
    a.Nops(16);                                   // 70-AF: the hooks
    a.Sw(kA0, 16, kK0);                           // B0
    a.Sw(kA1, 20, kK0);
    a.Sw(kA2, 24, kK0);
    a.Sw(kA3, 28, kK0);
    a.Mfc0(kA0, 12);
    a.Nop();
    a.Sw(kA0, 140, kK0);                          // sr
    a.Mfc0(kA1, 13);
    a.Nop();
    a.Sw(kA1, 144, kK0);                          // cause
    a.Sw(kK1, 108, kK0);
    for (uint32_t r = kS0; r <= kS7; ++r)
      a.Sw(r, static_cast<int16_t>(r * 4), kK0);
    for (uint32_t r = kT0; r <= kT7; ++r)
      a.Sw(r, static_cast<int16_t>(r * 4), kK0);
    a.Sw(kT8, 96, kK0);
    a.Sw(kT9, 100, kK0);
    a.Sw(kGp, 112, kK0);
    a.Sw(kSp, 116, kK0);
    a.Sw(kFp, 120, kK0);
    a.Mfhi(kA0);
    a.Nop();
    a.Sw(kA0, 132, kK0);
    a.Mflo(kA0);
    a.Nop();
    a.Sw(kA0, 136, kK0);

    // The chains: every element of priority 0, then 1, 2 and 3. An element's first function
    // decides; if it returns non-zero its second gets that value.
    a.Lui(kSp, 0);
    a.Addiu(kS3, kR0, kTableOfTables);
    a.Lw(kSp, static_cast<int16_t>(kExceptionStackVar), kSp);
    a.Lw(kS3, 0, kS3);
    a.Move(kFp, kSp);
    a.Addi(kS4, kS3, 0x20);
    const int priority = a.NewLabel();
    const int element = a.NewLabel();
    const int next_element = a.NewLabel();
    const int next_priority = a.NewLabel();
    a.Bind(priority);
    a.Lw(kS6, 0, kS3);
    a.Nop();
    a.Beq(kS6, kR0, next_priority);
    a.Nop();
    a.Bind(element);
    a.Lw(kS1, 8, kS6);
    a.Lw(kS0, 4, kS6);
    a.Beq(kS1, kR0, next_element);
    a.Nop();
    a.Jalr(kS1);
    a.Nop();
    a.Beq(kV0, kR0, next_element);
    a.Nop();
    a.Beq(kS0, kR0, next_element);
    a.Move(kA0, kV0);
    a.Jalr(kS0);
    a.Nop();
    a.Bind(next_element);
    a.Lw(kS6, 0, kS6);
    a.Nop();
    a.Bne(kS6, kR0, element);
    a.Nop();
    a.Bind(next_priority);
    a.Addi(kS3, kS3, 8);
    a.Bne(kS4, kS3, priority);
    a.Nop();

    // Out through the exit structure: its saved registers, and v0 = 1 to whatever it names -
    // ReturnFromException, unless something set its own.
    a.Addiu(kK0, kR0, kTableOfTables);
    a.Lw(kK0, 8, kK0);
    a.Lui(kA0, 0);
    a.Lw(kK0, 0, kK0);
    a.Addiu(kA0, kA0, static_cast<int16_t>(kCurrentExit));
    a.Lw(kA0, 0, kA0);
    a.Addiu(kA1, kR0, 1);
    a.Addi(kK0, kK0, 8);
    a.Lw(kRa, 0, kA0);
    a.Lw(kGp, 44, kA0);
    a.Lw(kSp, 4, kA0);
    a.Lw(kFp, 8, kA0);
    for (uint32_t r = kS0; r <= kS7; ++r)
      a.Lw(r, static_cast<int16_t>(12 + (r - kS0) * 4), kA0);
    a.Move(kV0, kA1);
    a.Jr(kRa);
    a.Nop();
    emit(a);
  }
  {
    MipsAssembler a(kExceptionCauseEpc);
    a.Mfc0(kV0, 13);
    a.Mfc0(kV1, 14);
    a.Jr(kRa);
    a.Nop();
    emit(a);
  }
  {
    // ChangeThread's last step is a real syscall, so the exception saves the old thread and
    // the syscall handler swaps the TCB in before ReturnFromException restores the new one.
    MipsAssembler a(kChangeThreadStub);
    a.Word(0x0000000C);   // syscall
    a.Jr(kRa);
    a.Nop();
    emit(a);
  }
}

uint32_t HleBios::KernelAlloc(uint32_t size) {
  return HeapAlloc(vars().kernel_heap_start, vars().kernel_heap_end, size);
}

void HleBios::AllocateControlBlocks(uint32_t tcbs, uint32_t evcbs) {
  // C(08h) SysInitMemory(A000E000h, 2000h), and the blocks in the order and sizes the kernel
  // allocates them - which puts each at the address it has on a console: ExCBs at E004h, EvCBs
  // at E028h, the PCB, then the TCBs.
  KernelVars& v = vars();
  v.kernel_heap_start = 0xA0000000 | kKernelHeap;
  v.kernel_heap_end = v.kernel_heap_start + kKernelHeapSize;
  HeapInit(v.kernel_heap_start, kKernelHeapSize);

  const uint32_t excb = KernelAlloc(0x20);
  const uint32_t evcb = KernelAlloc(evcbs * sizeof(EvCb));
  const uint32_t pcb = KernelAlloc(4);
  const uint32_t tcb = KernelAlloc(tcbs * sizeof(Tcb));
  if (excb == 0 || evcb == 0 || pcb == 0 || tcb == 0) {
    Message("the kernel's control blocks do not fit its 8 KB");
    return;
  }
  FillBlock(excb, 0, 0x20);
  FillBlock(evcb, 0, evcbs * sizeof(EvCb));
  for (uint32_t i = 0; i < tcbs; ++i)
    Write32(tcb + i * sizeof(Tcb), 0x1000);
  Write32(tcb, 0x4000);
  Write32(pcb, tcb);

  Write32(kTableOfTables + 0x00, excb);
  Write32(kTableOfTables + 0x04, 0x20);
  Write32(kTableOfTables + 0x08, pcb);
  Write32(kTableOfTables + 0x0C, 4);
  Write32(kTableOfTables + 0x10, tcb);
  Write32(kTableOfTables + 0x14, tcbs * sizeof(Tcb));
  Write32(kTableOfTables + 0x20, evcb);
  Write32(kTableOfTables + 0x24, evcbs * sizeof(EvCb));
}

// The handlers every kernel starts with (psx-spx "Priority Chains"): the syscall handler in
// priority 0, the three timers and vblank in 1, the default interrupt handler in 3. The CD-ROM's
// join priority 0 in CdInit.
void HleBios::InstallDefaultHandlers() {
  EnqueueHandler(0, ChainElementFor(TrapAddress(kSyscallVerifier), 0));
  // C(00h) EnqueueTimerAndVblankIrqs: timers off and zeroed, their IRQs masked.
  WriteIo32(kIMask, ReadIo32(kIMask) & ~0x71u);
  for (uint32_t t = 0; t < 4; ++t) {
    vars().timer_autoack[t] = 1;
    EnqueueHandler(1, ChainElementFor(TrapAddress(kTimerVerifier0 + t),
                                      TrapAddress(kTimerHandler0 + t)));
  }
  for (uint32_t t = 0; t < 3; ++t) {
    WriteIo16(0x1F801104 + t * 0x10, 0);
    WriteIo16(0x1F801108 + t * 0x10, 0);
    WriteIo16(0x1F801100 + t * 0x10, 0);
  }
  // C(0Ch) InitDefInt.
  for (uint32_t& ack : vars().irq_autoack)
    ack = 0;
  EnqueueHandler(3, ChainElementFor(TrapAddress(kDefIntVerifier), 0));
}

// The default handlers' chain elements live at fixed places in kernel RAM, one per first
// function, so enqueueing one twice moves it rather than making a loop.
uint32_t HleBios::ChainElementFor(uint32_t first, uint32_t second) {
  static const uint32_t kFirsts[] = { kSyscallVerifier, kCdIoVerifier,  kTimerVerifier0,
                                      kTimerVerifier1,  kTimerVerifier2, kTimerVerifier3,
                                      kPadCardVerifier, kDefIntVerifier };
  uint32_t index = 0;
  for (uint32_t i = 0; i < sizeof(kFirsts) / sizeof(kFirsts[0]); ++i)
    if (TrapAddress(kFirsts[i]) == first)
      index = i;
  const uint32_t element = kChains + index * sizeof(ChainElement);
  ChainElement* e = At<ChainElement>(element);
  e->first = first;
  e->second = second;
  e->unused = 0;
  return element;
}

// ---- Exceptions ----------------------------------------------------------------------------------

uint32_t HleBios::CurrentTcb() { return Read32(Read32(kTableOfTables + 0x08)); }

// B(17h): every register but k0 back from the current TCB, then the "rfe" that pops the status
// register's stack, and on at epc - with k0 holding it, as the kernel's "jr k0" leaves it.
void HleBios::ReturnFromException() {
  CpuContext& c = cpu();
  const Tcb* tcb = At<Tcb>(CurrentTcb());
  if (tcb == nullptr) {
    SystemError("ReturnFromException with no current thread");
    return;
  }
  for (uint32_t r = 1; r < 32; ++r)
    if (r != kK0)
      c.gp.reg[r] = tcb->reg[r];
  c.high = tcb->hi;
  c.low = tcb->lo;
  const uint32_t sr = tcb->sr;
  c.ctrl.SR.raw = (sr & ~0xFu) | ((sr >> 2) & 0xF);
  c.gp.k0 = tcb->epc;
  c.pc = tcb->epc;
}

void HleBios::EnqueueHandler(uint32_t priority, uint32_t element) {
  const uint32_t excb = Read32(kTableOfTables) + (priority & 3) * 8;
  // Out of the chain first if it is already there: a default handler enqueued twice moves.
  DequeueHandler(priority, element);
  Write32(element + 0, Read32(excb));
  Write32(excb, element);
}

uint32_t HleBios::DequeueHandler(uint32_t priority, uint32_t element) {
  const uint32_t head = Read32(kTableOfTables) + (priority & 3) * 8;
  uint32_t previous = 0;
  uint32_t at = Read32(head);
  for (int guard = 0; at != 0 && guard < 256; ++guard) {
    if (at == element) {
      const uint32_t next = Read32(at);
      if (previous == 0)
        Write32(head, next);
      else
        Write32(previous, next);
      return element;
    }
    previous = at;
    at = Read32(at);
  }
  return 0;
}

void HleBios::DefIntFrom(uint32_t index, uint32_t return_to) {
  const uint32_t pending = ReadIo32(kIMask) & ReadIo32(kIStat);
  for (uint32_t k = index; k < 11; ++k) {
    if ((pending & (1u << kDefIntOrder[k])) == 0)
      continue;
    Then then;
    then.kind = kThenDefInt;
    then.a = k;
    then.b = return_to;
    DeliverEvent(kDefIntClass[k], 0x1000, then);
    return;
  }
  cpu().gp.v0 = 0;
  cpu().pc = return_to;
}

// ---- Events --------------------------------------------------------------------------------------

uint32_t HleBios::EventCount() { return Read32(kTableOfTables + 0x24) / sizeof(EvCb); }

HleBios::EvCb* HleBios::Event(uint32_t handle) {
  const uint32_t index = handle & 0xFFFF;
  return At<EvCb>(Read32(kTableOfTables + 0x20) + index * sizeof(EvCb));
}

uint32_t HleBios::OpenEvent(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
  const uint32_t count = EventCount();
  for (uint32_t i = 0; i < count; ++i) {
    EvCb* e = Event(i);
    if (e->status != kStatusFree)
      continue;
    e->cls = cls;
    e->spec = spec;
    e->mode = mode;
    e->status = kStatusDisabled;
    e->func = func;
    return 0xF1000000 | i;
  }
  return 0xFFFFFFFF;
}

void HleBios::DeliverEventFrom(uint32_t cls, uint32_t spec, uint32_t first, const Then& then) {
  const uint32_t count = EventCount();
  for (uint32_t i = first; i < count; ++i) {
    EvCb* e = Event(i);
    if (e->status != kStatusEnabled || e->cls != cls || e->spec != spec)
      continue;
    if (e->mode == kModeReady) {
      e->status = kStatusReady;
    } else if (e->mode == kModeCallback && e->func != 0) {
      Frame frame = {};
      frame.kind = kFrameDeliver;
      frame.w[0] = cls;
      frame.w[1] = spec;
      frame.w[2] = i + 1;
      frame.w[3] = then.kind;
      frame.w[4] = then.a;
      frame.w[5] = then.b;
      frame.w[6] = then.c;
      CallGuest(e->func, frame);
      return;
    }
  }
  Continue(then);
}

void HleBios::UndeliverEvent(uint32_t cls, uint32_t spec) {
  const uint32_t count = EventCount();
  for (uint32_t i = 0; i < count; ++i) {
    EvCb* e = Event(i);
    if (e->status == kStatusReady && e->cls == cls && e->spec == spec && e->mode == kModeReady)
      e->status = kStatusEnabled;
  }
}

// A call into guest code: what is still to do goes on the continuation stack, and the function
// returns to kResume. The stack is a ring - a callback that never returns (one that leaves
// through ReturnFromException, say) strands its frame, and the ring lets a stranded frame be
// overwritten rather than fill the stack.
void HleBios::CallGuest(uint32_t function, const Frame& frame) {
  KernelVars& v = vars();
  const uint32_t slot = v.frames % kFrameCount;
  WriteBlock(kFrames + slot * sizeof(Frame), &frame, sizeof(Frame));
  ++v.frames;
  cpu().gp.ra = TrapAddress(kResume);
  cpu().pc = function;
}

void HleBios::Resume() {
  KernelVars& v = vars();
  if (v.frames == 0) {
    SystemError("a return to the kernel with nothing to resume");
    return;
  }
  --v.frames;
  Frame frame;
  ReadBlock(kFrames + (v.frames % kFrameCount) * sizeof(Frame), &frame, sizeof(Frame));
  switch (frame.kind) {
    case kFrameDeliver: {
      Then then;
      then.kind = frame.w[3];
      then.a = frame.w[4];
      then.b = frame.w[5];
      then.c = frame.w[6];
      DeliverEventFrom(frame.w[0], frame.w[1], frame.w[2], then);
      return;
    }
    case kFrameQsort:
    case kFrameLsearch:
    case kFrameBsearch:
      SearchStep(frame, false, static_cast<int32_t>(cpu().gp.v0));
      return;
    default:
      SystemError("a corrupt continuation frame");
      return;
  }
}

void HleBios::Continue(const Then& then) {
  CpuContext& c = cpu();
  switch (then.kind) {
    case kThenReturn:
      c.gp.v0 = then.b;
      c.pc = then.a;
      return;
    case kThenReturnFromException:
      ReturnFromException();
      return;
    case kThenDefInt: {
      const uint32_t irq = static_cast<uint32_t>(kDefIntOrder[then.a]);
      if (vars().irq_autoack[irq] != 0)
        WriteIo32(kIStat, ~(1u << irq));
      DefIntFrom(then.a + 1, then.b);
      return;
    }
    case kThenSyscallUnresolved: {
      const Tcb* tcb = At<Tcb>(CurrentTcb());
      char text[96];
      snprintf(text, sizeof(text), "unresolved exception, cause %08X at %08X",
               tcb ? tcb->cause : 0, tcb ? tcb->epc : 0);
      SystemError(text);
      c.pc = TrapAddress(kTableA + 0x40);
      return;
    }
    case kThenCardEvents: {
      // A low-level card operation finished: its event, then a = where to return to.
      Then next;
      next.a = then.a;
      DeliverEvent(kEventClassCard, then.b, next);
      return;
    }
    case kThenFileEvents: {
      // A memory card file operation: the file's own event first (the caller delivered it),
      // then the device's, then back with the result.
      Then next;
      next.a = then.a;
      next.b = then.b;
      DeliverEvent(kEventClassCardHigh, then.c, next);
      return;
    }
    default:
      SystemError("an unknown continuation");
      return;
  }
}

// ---- The tables ----------------------------------------------------------------------------------

void HleBios::FunctionA(uint32_t n) {
  CpuContext& c = cpu();
  // A(00h-09h) and A(3Bh-3Eh) are the B functions: the tables point at those, but a game that
  // works out an A slot's address itself lands here.
  if (n <= 0x09) {
    FunctionB(0x32 + n);
    return;
  }
  if (n >= 0x3B && n <= 0x3E) {
    FunctionB(0x3C + (n - 0x3B));
    return;
  }
  if (LibcFunction(n))
    return;

  switch (n) {
    case 0x3A: SystemError("SystemErrorExit"); return;
    case 0x40: SystemError("SystemErrorUnresolvedException"); return;
    case 0x41: {   // LoadExeHeader(filename, headerbuf)
      std::vector<uint8_t> image;
      if (!LoadFileImage(ReadString(Arg(0)), &image) || image.size() < 0x800) {
        Return(0);
        return;
      }
      WriteBlock(Arg(1), &image[0x10], kExeHeaderBytes);
      Return(1);
      return;
    }
    case 0x42: {   // LoadExeFile(filename, headerbuf)
      std::vector<uint8_t> image;
      if (!LoadFileImage(ReadString(Arg(0)), &image) || !LoadExecutable(image, Arg(1))) {
        Return(0);
        return;
      }
      system_->ResetCompiledCode();
      Return(1);
      return;
    }
    case 0x43: {   // DoExecute(headerbuf, param1, param2)
      const uint32_t header = Arg(0);
      Write32(header + 0x28, c.gp.sp);
      Write32(header + 0x2C, c.gp.fp);
      Write32(header + 0x30, c.gp.gp);
      Write32(header + 0x34, c.gp.ra);
      Write32(header + 0x38, c.gp.s0);
      vars().exec_header = header;
      StartExecutable(header, Arg(1), Arg(2), TrapAddress(kDoExecuteReturned));
      return;
    }
    case 0x44:     // FlushCache
      system_->ResetCompiledCode();
      Return(0);
      return;
    case 0x45:     // init_a0_b0_c0_vectors
      BuildVectorsAndHandler();
      ReturnVoid();
      return;
    case 0x46: {   // GPU_dw(x, y, w, h, src)
      const uint32_t x = Arg(0), y = Arg(1), w = Arg(2), h = Arg(3), src = Arg(4);
      WriteIo32(0x1F801810, 0xA0000000);
      WriteIo32(0x1F801810, ((y & 0xFFFF) << 16) | (x & 0xFFFF));
      WriteIo32(0x1F801810, ((h & 0xFFFF) << 16) | (w & 0xFFFF));
      const uint32_t words = (w * h) / 2;
      for (uint32_t i = 0; i < words; ++i)
        WriteIo32(0x1F801810, Read32(src + i * 4));
      Return(src + words * 4);
      return;
    }
    case 0x47: {   // gpu_send_dma(x, y, w, h, src)
      const uint32_t x = Arg(0), y = Arg(1), w = Arg(2), h = Arg(3), src = Arg(4);
      WriteIo32(0x1F801810, 0xA0000000);
      WriteIo32(0x1F801810, ((y & 0xFFFF) << 16) | (x & 0xFFFF));
      WriteIo32(0x1F801810, ((h & 0xFFFF) << 16) | (w & 0xFFFF));
      WriteIo32(0x1F801814, 0x04000002);
      WriteIo32(0x1F8010F0, ReadIo32(0x1F8010F0) | 0x800);
      WriteIo32(0x1F8010A0, src);
      WriteIo32(0x1F8010A4, (((w * h) / 32) << 16) | 0x10);
      WriteIo32(0x1F8010A8, 0x01000201);
      Return(0x1F801810);
      return;
    }
    case 0x48:     // SendGP1Command(gp1cmd)
      WriteIo32(0x1F801814, Arg(0));
      ReturnVoid();
      return;
    case 0x49:     // GPU_cw(gp0cmd)
      WriteIo32(0x1F801810, Arg(0));
      Return(0);
      return;
    case 0x4A: {   // GPU_cwp(src, num)
      const uint32_t src = Arg(0), num = Arg(1);
      for (uint32_t i = 0; i < num; ++i)
        WriteIo32(0x1F801810, Read32(src + i * 4));
      Return(0);
      return;
    }
    case 0x4B:     // send_gpu_linked_list(src)
      WriteIo32(0x1F801814, 0x04000002);
      WriteIo32(0x1F8010F4, 0);
      WriteIo32(0x1F8010F0, ReadIo32(0x1F8010F0) | 0x800);
      WriteIo32(0x1F8010A0, Arg(0));
      WriteIo32(0x1F8010A4, 0);
      WriteIo32(0x1F8010A8, 0x01000401);
      ReturnVoid();
      return;
    case 0x4C:     // gpu_abort_dma
      WriteIo32(0x1F8010A8, 0x401);
      WriteIo32(0x1F801814, 0x04000000);
      WriteIo32(0x1F801814, 0x02000000);
      WriteIo32(0x1F801814, 0x01000000);
      Return(0x1F801814);
      return;
    case 0x4D:     // GetGPUStatus
      Return(ReadIo32(0x1F801814));
      return;
    case 0x4E:     // gpu_sync: the GPU here is ready as soon as it is written to
      Return(0);
      return;
    case 0x4F: case 0x50: case 0x52: case 0x53: case 0x9A: case 0x9B:
      SystemError("an A function that is a system error");
      return;
    case 0x51: {   // LoadAndExecute(filename, stackbase, stackoffset)
      std::vector<uint8_t> image;
      const uint32_t header = kExecHeaderBuffer;
      c.ctrl.SR.raw |= 0x401;   // ExitCriticalSection
      if (!LoadFileImage(ReadString(Arg(0)), &image) || !LoadExecutable(image, header)) {
        SystemError("LoadAndExecute could not load its file");
        return;
      }
      system_->ResetCompiledCode();
      Write32(header + 0x20, Arg(1));
      Write32(header + 0x24, Arg(2));
      StartExecutable(header, 1, 0, TrapAddress(kExeReturned));
      return;
    }
    case 0x54: case 0x71:   // CdInit
      CdInit();
      Return(1);
      return;
    case 0x55: case 0x70: {   // _bu_init
      // Sector 0 of each slot, read and waited for in turn, change flag ignored - the kernel's
      // directory buffers being loaded. Waited for by staying on this trap, so the vblank that
      // completes each read can run; the reads' events reach whatever the game has opened.
      KernelVars& v = vars();
      if (v.card_started == 0) {
        v.card_auto_format = 0;
        Return(1);
        return;
      }
      switch (v.bu_init_stage) {
        case 0:
        case 2: {
          const uint32_t port = v.bu_init_stage == 0 ? 0x00 : 0x10;
          if ((v.card_status[port >> 4] & 1) == 0)
            return;   // something else of the slot's still going: wait for it
          v.card_allow_new = 1;
          v.card_op[port >> 4] = 1;
          v.card_sector[port >> 4] = 0;
          v.card_buffer[port >> 4] = kCardDirectoryBuffer;
          v.card_status[port >> 4] = 2;
          ++v.bu_init_stage;
          return;
        }
        case 1:
        case 3:
          if ((v.card_status[v.bu_init_stage == 1 ? 0 : 1] & 1) == 0)
            return;
          if (++v.bu_init_stage == 4) {
            v.bu_init_stage = 0;
            v.card_auto_format = 0;
            Return(1);
          }
          return;
        default:
          v.bu_init_stage = 0;
          return;
      }
    }
    case 0x56: case 0x72:   // CdRemove
      CdRemove();
      Return(1);
      return;
    case 0x96: case 0x97: case 0x98: case 0x99:   // Add...Device: there already
      Return(1);
      return;
    case 0x9C: {   // SetConf(num_EvCB, num_TCB, stacktop)
      const uint32_t evcbs = Arg(0), tcbs = Arg(1), stacktop = Arg(2);
      vars().conf_evcb = evcbs;
      vars().conf_tcb = tcbs;
      vars().conf_stacktop = stacktop;
      AllocateControlBlocks(tcbs, evcbs);
      InstallDefaultHandlers();
      ReturnVoid();
      return;
    }
    case 0x9D:     // GetConf(num_EvCB_dst, num_TCB_dst, stacktop_dst)
      Write32(Arg(0), vars().conf_evcb);
      Write32(Arg(1), vars().conf_tcb);
      Write32(Arg(2), vars().conf_stacktop);
      ReturnVoid();
      return;
    case 0x9F: {   // SetMemSize(megabytes)
      const uint32_t mb = Arg(0);
      if (mb != 2 && mb != 8) {
        Return(0);
        return;
      }
      Write32(0x60, mb);
      WriteIo32(0x1F801060, (ReadIo32(0x1F801060) & ~0x700u) | (mb == 8 ? 0x300u : 0u));
      Return(0);
      return;
    }
    case 0xA0:     // WarmBoot
      Reset();
      return;
    case 0xA1: {   // SystemErrorBootOrDiskFailure(type, errorcode)
      char text[64];
      snprintf(text, sizeof(text), "boot or disc failure '%c' %X", static_cast<char>(Arg(0)), Arg(1));
      SystemError(text);
      return;
    }
    case 0xB4: {   // GetSystemInfo(index)
      static const uint32_t kInfo[] = { 0, 3, 0, 0, 0, 0, 0, 0x400, 0, 0x200, 0, 0, 1, 1, 1 };
      const uint32_t index = Arg(0);
      if (index == 0)
        Return(kKernelDate);
      else if (index == 2)
        Return(0xBFC00108);
      else if (index == 5)
        Return(Read32(0x60) << 10);
      else if (index < 0x0F)
        Return(kInfo[index]);
      else
        Return(0);
      return;
    }
    case 0xB2:     // ioabort_raw: the kernel's own recovery buffer, which nothing here sets up
      SystemError("ioabort");
      return;
    default:
      break;
  }

  // The device, CD-ROM and memory card functions: hle_bios_io.cpp's, as 100h + n.
  if (IoFunction(0x100 + n))
    return;

  char text[48];
  snprintf(text, sizeof(text), "A(%02Xh) is not implemented", n);
  Message(text);
  Return(0);
}

void HleBios::FunctionB(uint32_t n) {
  CpuContext& c = cpu();
  KernelVars& v = vars();
  switch (n) {
    case 0x00:     // alloc_kernel_memory(size)
      Return(KernelAlloc(Arg(0)));
      return;
    case 0x01:     // free_kernel_memory(buf)
      if (Arg(0) != 0)
        Write32(Arg(0) - 4, Read32(Arg(0) - 4) | 1);
      ReturnVoid();
      return;
    case 0x02: {   // init_timer(t, reload, flags)
      const uint32_t t = Arg(0) & 0xFFFF, reload = Arg(1), flags = Arg(2);
      if (t > 2) {
        Return(0);
        return;
      }
      WriteIo16(0x1F801104 + t * 0x10, 0);
      WriteIo16(0x1F801108 + t * 0x10, static_cast<uint16_t>(reload));
      uint16_t mode = (flags & 0x10) ? 0x49 : 0x48;
      if ((flags & 0x01) == 0)
        mode |= 0x100;
      if (flags & 0x1000)
        mode |= 0x10;
      WriteIo16(0x1F801104 + t * 0x10, mode);
      Return(1);
      return;
    }
    case 0x03: {   // get_timer(t)
      const uint32_t t = Arg(0) & 0xFFFF;
      Return(t > 2 ? 0 : ReadIo16(0x1F801100 + t * 0x10));
      return;
    }
    case 0x04: {   // enable_timer_irq(t)
      const uint32_t t = Arg(0) & 0xFFFF;
      static const uint32_t kMasks[4] = { 0x10, 0x20, 0x40, 0x01 };
      WriteIo32(kIMask, ReadIo32(kIMask) | kMasks[t & 3]);
      Return(t <= 2 ? 1 : 0);
      return;
    }
    case 0x05: {   // disable_timer_irq(t)
      const uint32_t t = Arg(0) & 0xFFFF;
      static const uint32_t kMasks[4] = { 0x10, 0x20, 0x40, 0x01 };
      WriteIo32(kIMask, ReadIo32(kIMask) & ~kMasks[t & 3]);
      Return(1);
      return;
    }
    case 0x06: {   // restart_timer(t)
      const uint32_t t = Arg(0) & 0xFFFF;
      if (t > 2) {
        Return(0);
        return;
      }
      WriteIo16(0x1F801100 + t * 0x10, 0);
      Return(1);
      return;
    }
    case 0x07: {   // DeliverEvent(class, spec)
      Then then;
      then.a = c.gp.ra;
      then.b = 0;
      DeliverEvent(Arg(0), Arg(1), then);
      return;
    }
    case 0x08:     // OpenEvent(class, spec, mode, func)
      Return(OpenEvent(Arg(0), Arg(1), Arg(2), Arg(3)));
      return;
    case 0x09:     // CloseEvent(event)
      if (EvCb* e = Event(Arg(0)))
        e->status = kStatusFree;
      Return(1);
      return;
    case 0x0A: {   // WaitEvent(event)
      EvCb* e = Event(Arg(0));
      if (e == nullptr) {
        Return(0);
        return;
      }
      if (e->status == kStatusReady) {
        e->status = kStatusEnabled;
        Return(1);
      } else if (e->status == kStatusEnabled) {
        // Busy: stay on the trap and look again next step, with time passing and interrupts
        // taken in between - which is what the kernel's loop is.
      } else {
        Return(0);
      }
      return;
    }
    case 0x0B: {   // TestEvent(event)
      EvCb* e = Event(Arg(0));
      if (e != nullptr && e->status == kStatusReady) {
        e->status = kStatusEnabled;
        Return(1);
      } else {
        Return(0);
      }
      return;
    }
    case 0x0C:     // EnableEvent(event)
      if (EvCb* e = Event(Arg(0)))
        if (e->status != kStatusFree)
          e->status = kStatusEnabled;
      Return(1);
      return;
    case 0x0D:     // DisableEvent(event)
      if (EvCb* e = Event(Arg(0)))
        if (e->status != kStatusFree)
          e->status = kStatusDisabled;
      Return(1);
      return;
    case 0x0E: {   // OpenThread(pc, sp_fp, gp)
      const uint32_t base = Read32(kTableOfTables + 0x10);
      const uint32_t count = Read32(kTableOfTables + 0x14) / sizeof(Tcb);
      for (uint32_t i = 0; i < count; ++i) {
        Tcb* t = At<Tcb>(base + i * sizeof(Tcb));
        if (t->status != 0x1000)
          continue;
        t->status = 0x4000;
        t->unused = 0x1000;
        t->reg[kSp] = Arg(1);
        t->reg[kFp] = Arg(1);
        t->epc = Arg(0);
        t->reg[kGp] = Arg(2);
        Return(0xFF000000 | i);
        return;
      }
      Return(0xFFFFFFFF);
      return;
    }
    case 0x0F:     // CloseThread(handle)
      Write32(Read32(kTableOfTables + 0x10) + (Arg(0) & 0xFFFF) * sizeof(Tcb), 0x1000);
      Return(1);
      return;
    case 0x10:     // ChangeThread(handle): syscall(3, tcb)
      c.gp.a1 = Read32(kTableOfTables + 0x10) + (Arg(0) & 0xFFFF) * sizeof(Tcb);
      c.gp.a0 = 3;
      c.pc = Kseg0(kChangeThreadStub);
      return;
    case 0x12: {   // InitPad(buf1, siz1, buf2, siz2)
      v.pad_buf[0] = Arg(0);
      v.pad_size[0] = Arg(1);
      v.pad_buf[1] = Arg(2);
      v.pad_size[1] = Arg(3);
      v.pad_output[0] = v.pad_output[1] = 0;
      v.outdated_pad_dest = 0;
      FillBlock(v.pad_buf[0], 0, v.pad_size[0]);
      FillBlock(v.pad_buf[1], 0, v.pad_size[1]);
      v.pad_started = 1;
      v.pad_enable = 1;   // InitPad "does set the pad_enable_flag" (psx-spx)
      Return(1);
      return;
    }
    case 0x13:     // StartPad
      // Into priority 2, vblank unmasked and acknowledged, and the vblank timer's handler told
      // not to acknowledge it - this one does, after reading the pads.
      EnqueueHandler(2, ChainElementFor(TrapAddress(kPadCardVerifier),
                                        TrapAddress(kPadCardHandler)));
      WriteIo32(kIStat, ~1u);
      WriteIo32(kIMask, ReadIo32(kIMask) | 1);
      v.pad_autoack = 1;
      v.timer_autoack[3] = 0;
      c.ctrl.SR.raw |= 0x401;   // it leaves through ExitCriticalSection
      Return(1);
      return;
    case 0x14:     // StopPad
      v.timer_autoack[3] = 1;
      DequeueHandler(2, ChainElementFor(TrapAddress(kPadCardVerifier),
                                        TrapAddress(kPadCardHandler)));
      ReturnVoid();
      return;
    case 0x15: {   // OutdatedPadInitAndStart(type, button_dest, unused, unused)
      const uint32_t type = Arg(0);
      if (type != 0x20000000 && type != 0x20000001) {
        Return(0);
        return;
      }
      const uint32_t dest = Arg(1);
      Write32(c.gp.sp + 8, Arg(2));
      Write32(c.gp.sp + 12, Arg(3));
      v.pad_buf[0] = kOutdatedPadBuffers;
      v.pad_buf[1] = kOutdatedPadBuffers + 0x24;
      v.pad_size[0] = v.pad_size[1] = 0x22;
      FillBlock(kOutdatedPadBuffers, 0, 0x48);
      v.pad_started = 1;
      v.pad_enable = 1;   // InitPad "does set the pad_enable_flag" (psx-spx)
      v.outdated_pad_type = type;
      EnqueueHandler(2, ChainElementFor(TrapAddress(kPadCardVerifier),
                                        TrapAddress(kPadCardHandler)));
      WriteIo32(kIStat, ~1u);
      WriteIo32(kIMask, ReadIo32(kIMask) | 1);
      v.pad_autoack = 1;
      v.timer_autoack[3] = 0;
      v.outdated_pad_dest = dest;
      c.ctrl.SR.raw |= 0x401;
      Return(2);
      return;
    }
    case 0x16:     // OutdatedPadGetButtons
      Return(OutdatedPadButtons());
      return;
    case 0x17:     // ReturnFromException
      ReturnFromException();
      return;
    case 0x18:     // SetDefaultExitFromException
      Write32(kCurrentExit, kDefaultExit);
      Return(kDefaultExit);
      return;
    case 0x19:     // SetCustomExitFromException(addr)
      Write32(kCurrentExit, Arg(0));
      ReturnVoid();
      return;
    case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F:
    case 0x21: case 0x22: case 0x23: case 0x2A: case 0x2B: case 0x52: case 0x5A:
      SystemError("a B function that is a system error");
      return;
    case 0x20:     // UnDeliverEvent(class, spec)
      UndeliverEvent(Arg(0), Arg(1));
      ReturnVoid();
      return;
    case 0x3C:     // std_in_getchar: nothing is ever typed
      Return(0);
      return;
    case 0x3D:     // std_out_putchar(char): the console already has it (Kernel::Call)
      Return(Arg(0) & 0xFF);
      return;
    case 0x3E:     // std_in_gets(dst)
      Write8(Arg(0), 0);
      Return(Arg(0));
      return;
    case 0x3F:     // std_out_puts(src): the console already has it
      Return(0);
      return;
    case 0x47:     // AddDevice(device_info)
    case 0x48:     // RemoveDevice(name)
      Return(1);
      return;
    case 0x49:     // PrintInstalledDevices
      Console("cdrom bu tty\n");
      ReturnVoid();
      return;
    case 0x4A:     // InitCard(pad_enable)
      v.pad_enable = Arg(0);
      v.card_status[0] = v.card_status[1] = 1;
      v.card_op[0] = v.card_op[1] = 0;
      Return(0);
      return;
    case 0x4B:     // StartCard
      EnqueueHandler(2, ChainElementFor(TrapAddress(kPadCardVerifier),
                                        TrapAddress(kPadCardHandler)));
      WriteIo32(kIMask, ReadIo32(kIMask) | 1);
      v.pad_autoack = 1;
      v.timer_autoack[3] = 0;
      v.card_started = 1;
      c.ctrl.SR.raw |= 0x401;
      Return(1);
      return;
    case 0x4C:     // StopCard
      v.timer_autoack[3] = 1;
      DequeueHandler(2, ChainElementFor(TrapAddress(kPadCardVerifier),
                                        TrapAddress(kPadCardHandler)));
      v.card_started = 0;
      Return(1);
      return;
    case 0x50:     // allow_new_card
      v.card_allow_new = 1;
      ReturnVoid();
      return;
    case 0x51: {   // Krom2RawAdd(shiftjis): where its 16x15 glyph is in the ROM, or -1
      uint32_t base = 0;
      const uint32_t offset = KromOffset(Arg(0) & 0xFFFF, &base);
      Return(offset == 0xFFFFFFFF ? 0xFFFFFFFF : base + offset * 30);
      return;
    }
    case 0x53:     // Krom2Offset(shiftjis): which glyph of its character set
      Return(KromOffset(Arg(0) & 0xFFFF, nullptr));
      return;
    case 0x54:     // GetLastError
      Return(v.last_error);
      return;
    case 0x55: {   // GetLastFileError(fd)
      const Fcb* f = FileBlock(Arg(0));
      Return(f != nullptr && f->status != 0 ? f->error : 0xFFFFFFFF);
      return;
    }
    case 0x56:     // GetC0Table
      Return(kCTable);
      return;
    case 0x57:     // GetB0Table
      Return(kBTable);
      return;
    case 0x58:     // get_bu_callback_port
      Return(v.card_last_port);
      return;
    case 0x59:     // testdevice(name)
      Return(0);
      return;
    case 0x5B:     // ChangeClearPad(int)
      v.pad_autoack = Arg(0);
      ReturnVoid();
      return;
    case 0x5C:     // get_card_status(slot)
    case 0x5D:     // wait_card_status(slot): the vblank handler finishes one, so wait for it
      {
        const uint32_t slot = Arg(0) & 1;
        const uint32_t status = v.card_status[slot];
        if (n == 0x5D && (status & 1) == 0 && status != 0)
          return;   // busy: stay on the trap
        Return(status);
      }
      return;
    default:
      break;
  }

  // Files, devices, CD-ROM and memory cards: hle_bios_io.cpp's.
  if (n >= 0x32 && n <= 0x3B) {
    switch (n) {
      case 0x32: Return(FileOpen(Arg(0), Arg(1))); return;
      case 0x33: Return(FileSeek(Arg(0), static_cast<int32_t>(Arg(1)), Arg(2))); return;
      case 0x34: FinishFile(FileRead(Arg(0), Arg(1), Arg(2))); return;
      case 0x35: FinishFile(FileWrite(Arg(0), Arg(1), Arg(2))); return;
      case 0x36: Return(FileClose(Arg(0))); return;
      case 0x37: Return(0); return;   // FileIoctl: TTY only
      case 0x38: SystemError("exit"); return;
      case 0x39: {   // FileGetDeviceFlag(fd)
        const Fcb* f = FileBlock(Arg(0));
        Return(f != nullptr ? (f->device_flags >> 1) & 1 : 0);
        return;
      }
      case 0x3A: {   // FileGetc(fd)
        const uint32_t scratch = c.gp.sp - 0x10;
        const uint32_t got = FileRead(Arg(0), scratch, 1);
        FinishFile(got == 1 ? static_cast<uint32_t>(static_cast<int8_t>(Read8(scratch)))
                            : 0xFFFFFFFF);
        return;
      }
      case 0x3B: {   // FilePutc(char, fd)
        const uint32_t scratch = c.gp.sp - 0x10;
        Write8(scratch, static_cast<uint8_t>(Arg(0)));
        FinishFile(FileWrite(Arg(1), scratch, 1));
        return;
      }
    }
  }
  switch (n) {
    case 0x40: Return(ChangeDirectory(Arg(0))); return;
    case 0x41: Return(FormatDevice(Arg(0))); return;
    case 0x42: Return(FirstFile(Arg(0), Arg(1))); return;
    case 0x43: Return(NextFile(Arg(0))); return;
    case 0x44: Return(FileRename(Arg(0), Arg(1))); return;
    case 0x45: Return(FileDelete(Arg(0), false)); return;
    case 0x46: Return(FileDelete(Arg(0), true)); return;
    default: break;
  }
  if (IoFunction(n))
    return;

  char text[48];
  snprintf(text, sizeof(text), "B(%02Xh) is not implemented", n);
  Message(text);
  Return(0);
}

void HleBios::FunctionC(uint32_t n) {
  KernelVars& v = vars();
  switch (n) {
    case 0x00:     // EnqueueTimerAndVblankIrqs(priority)
      for (uint32_t t = 0; t < 4; ++t) {
        v.timer_autoack[t] = 1;
        EnqueueHandler(Arg(0), ChainElementFor(TrapAddress(kTimerVerifier0 + t),
                                               TrapAddress(kTimerHandler0 + t)));
      }
      Return(0);
      return;
    case 0x01:     // EnqueueSyscallHandler(priority)
      EnqueueHandler(Arg(0), ChainElementFor(TrapAddress(kSyscallVerifier), 0));
      Return(0);
      return;
    case 0x02:     // SysEnqIntRP(priority, struc)
      EnqueueHandler(Arg(0), Arg(1));
      Return(0);
      return;
    case 0x03:     // SysDeqIntRP(priority, struc)
      Return(DequeueHandler(Arg(0), Arg(1)));
      return;
    case 0x04: {   // get_free_EvCB_slot
      const uint32_t count = EventCount();
      for (uint32_t i = 0; i < count; ++i)
        if (Event(i)->status == kStatusFree) {
          Return(i);
          return;
        }
      Return(0xFFFFFFFF);
      return;
    }
    case 0x05: {   // get_free_TCB_slot
      const uint32_t base = Read32(kTableOfTables + 0x10);
      const uint32_t count = Read32(kTableOfTables + 0x14) / sizeof(Tcb);
      for (uint32_t i = 0; i < count; ++i)
        if (Read32(base + i * sizeof(Tcb)) == 0x1000) {
          Return(i);
          return;
        }
      Return(0xFFFFFFFF);
      return;
    }
    case 0x07:     // InstallExceptionHandlers
    case 0x1C:     // AdjustA0Table: the tables are built adjusted
      if (n == 0x07)
        BuildVectorsAndHandler();
      ReturnVoid();
      return;
    case 0x08:     // SysInitMemory(addr, size)
      v.kernel_heap_start = Arg(0);
      v.kernel_heap_end = Arg(0) + Arg(1);
      HeapInit(Arg(0), Arg(1));
      Return(0);
      return;
    case 0x09:     // SysInitKernelVariables: broken by the kernel's own boot on a console
      ReturnVoid();
      return;
    case 0x0A: {   // ChangeClearRCnt(t, flag)
      const uint32_t t = Arg(0) & 3;
      const uint32_t old = v.timer_autoack[t];
      v.timer_autoack[t] = Arg(1);
      Return(old);
      return;
    }
    case 0x0B:
      SystemError("C(0Bh)");
      return;
    case 0x0C:     // InitDefInt(priority)
      for (uint32_t& ack : v.irq_autoack)
        ack = 0;
      EnqueueHandler(Arg(0), ChainElementFor(TrapAddress(kDefIntVerifier), 0));
      Return(0);
      return;
    case 0x0D: {   // SetIrqAutoAck(irq, flag)
      const uint32_t irq = Arg(0);
      if (irq < 11)
        v.irq_autoack[irq] = Arg(1);
      ReturnVoid();
      return;
    }
    case 0x0E: case 0x0F: case 0x10: case 0x11: case 0x14:
      Return(0);
      return;
    case 0x12:     // InstallDevices(ttyflag)
      InstallDevices();
      ReturnVoid();
      return;
    case 0x13:     // FlushStdInOutPut
      ReturnVoid();
      return;
    case 0x1A:     // set_card_find_mode(mode)
      v.card_find_mode = Arg(0);
      ReturnVoid();
      return;
    case 0x1B:     // KernelRedirect(ttyflag)
      ReturnVoid();
      return;
    case 0x1D:     // get_card_find_mode
      Return(v.card_find_mode);
      return;
    default: {
      char text[48];
      snprintf(text, sizeof(text), "C(%02Xh) is not implemented", n);
      Message(text);
      Return(0);
      return;
    }
  }
}

// ---- Executables -----------------------------------------------------------------------------

// The header's 3Ch bytes into `header_buffer`, and the text where it says - the kernel's
// LoadExeFile.
bool HleBios::LoadExecutable(const std::vector<uint8_t>& image, uint32_t header_buffer) {
  if (image.size() < 0x800 || memcmp(&image[0], "PS-X EXE", 8) != 0)
    return false;
  WriteBlock(header_buffer, &image[0x10], kExeHeaderBytes);
  const uint32_t t_addr = Read32(header_buffer + 0x08);
  uint32_t t_size = Read32(header_buffer + 0x0C);
  if (image.size() < 0x800 + static_cast<size_t>(t_size))
    t_size = static_cast<uint32_t>(image.size() - 0x800);
  if (t_size != 0)
    WriteBlock(t_addr, &image[0x800], t_size);
  return true;
}

// DoExecute's work once the registers are saved: clear the BSS, set the stack and gp, and call
// the entry point with `a0`, `a1`, returning to `ra`.
void HleBios::StartExecutable(uint32_t header, uint32_t a0, uint32_t a1, uint32_t ra) {
  CpuContext& c = cpu();
  const uint32_t b_addr = Read32(header + 0x18);
  const uint32_t b_size = Read32(header + 0x1C);
  if (b_addr != 0 && b_size != 0 && b_size < 0x00800000)
    FillBlock(b_addr, 0, b_size);
  const uint32_t s_addr = Read32(header + 0x20);
  const uint32_t s_size = Read32(header + 0x24);
  if (s_addr != 0) {
    c.gp.sp = s_addr + s_size;
    c.gp.fp = s_addr + s_size;
  }
  c.gp.gp = Read32(header + 0x04);
  c.gp.a0 = a0;
  c.gp.a1 = a1;
  c.gp.ra = ra;
  c.pc = Read32(header + 0x00);
}

}  // namespace hle
}  // namespace psx
}  // namespace emulation

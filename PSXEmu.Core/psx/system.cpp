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
#include "psx/psx.h"
#include "psx/recompiler_bridge.h"
//#include <stdio.h>
//#include <stdlib.h>
//#pragma warning( disable : 4996 )

namespace emulation {
namespace psx {

uint64_t TrapCounter::count = 0;
uint64_t TrapCounter::rfe_count = 0;
TrapCounter::Site TrapCounter::sites[TrapCounter::kSiteCapacity] = {};
uint32_t TrapCounter::site_count = 0;
ExceptionLog::Entry ExceptionLog::entries[ExceptionLog::kCapacity] = {};
uint32_t ExceptionLog::written = 0;

System::System() {
  memset(&cpu_context_,0,sizeof(cpu_context_));
  base_freq_hz_  = 33868800.0;
  interrupts_taken_ = 0;
  interrupts_blocked_ = 0;
  memset(interrupts_taken_by_source_, 0, sizeof(interrupts_taken_by_source_));
  interrupts_blocked_im_ = 0;
  instructions_with_ie_ = 0;
}

System::~System() {
    
    
}

int System::Initialize(const char* bios_path) {
  const int result = InitializeWithoutBios();
  if (result != 0)
    return result;
  // A BIOS dump is required, not optional, so a missing one is a hard failure
  // rather than something to run past with a buffer full of zeroes.
  if (!LoadBiosFromFile(bios_path))
    return -1;
  return 0;
}

int System::InitializeWithoutBios() {
  #if defined(_DEBUG)
    csvlog.system_ = this;
    csvlog.Open("log.csv");
  #endif

  io_.set_system(this);
  cpu_.set_system(this);
  gpu_.set_system(this);
  spu_.set_system(this);
  mc_[0].set_system(this);
  mc_[1].set_system(this);
  for (auto& port : multitap_mc_)
    for (MC& card : port)
      card.set_system(this);
  kernel_.set_system(this);
  gte_.set_system(this);

  io_.Initialize();
  cpu_.set_context(&cpu_context_);
  cpu_.Initialize();
  cpu_.Reset();
  gpu_.Initialize();
  spu_.Initialize();
  mc_[0].Initialize();
  mc_[1].Initialize();
  for (auto& port : multitap_mc_)
    for (MC& card : port)
      card.Initialize();
  kernel_.Initialize();
  debugger_.Reset();
  gte_.Initialize();
  //mc_[0].LoadFile("D:\\Personal\\Projects\\PsxEmu\\test\\ff7.mcr");
  
  //lets skip this and do proper emulation first
  /*while (cpu_.context()->pc!=0x80030000) {
	  //cpu_.ExecuteInstruction();
    Step();
  }*/
  //extern bool output_inst;
  //output_inst = true;
  return 0;
}

int System::Deinitialize() {
  gte_.Deinitialize();
  //kernel_.De
  mc_[0].Deinitialize();
  mc_[1].Deinitialize();
  for (auto& port : multitap_mc_)
    for (MC& card : port)
      card.Deinitialize();
  spu_.Deinitialize();
  gpu_.Deinitialize();
  cpu_.Deinitialize();
  io_.Deinitialize();
  return 0;
}

// One instruction, with any pending interrupt taken before the next one. No
// wall clock is consulted, so a headless run is reproducible.
//
// Two copies: kDebugger false is StepInstructionUnarmed, with the debugger's
// check compiled out rather than tested and skipped.
template <bool kDebugger>
bool System::StepImpl() {
  // The one safe point to change CPU: here, between instructions, on the
  // thread that runs the machine. Switching the recompiler off frees the
  // compiled code, and the menu that asks for it runs on the message thread -
  // doing it there could free the block the machine is executing.
  if (config_.recompiler != (recompiler_ != nullptr))
    EnableRecompiler(config_.recompiler);

  cpu_.context()->current_cycles = 0;

  if (cpu_.context()->ctrl.SR.IEc && (cpu_.context()->ctrl.SR.raw & 0x400))
    ++instructions_with_ie_;

  // Take a pending interrupt *before* the next instruction, with EPC pointing
  // at that instruction - not after the last one, with EPC pointing back at it.
  //
  // Getting this backwards makes the interrupted instruction run a second time
  // when the handler returns. Usually harmless; when the instruction happens to
  // be the RFE at the end of another handler, it pops the Cop0 status stack
  // twice, and interrupts are then off for good. That is exactly what happened
  // here: the very first vertical blank landed on an RFE and no interrupt was
  // ever delivered again.
  //
  // Nothing needs to check for a branch delay slot: Jump() runs the delay slot
  // inside the same ExecuteInstruction call, so control never arrives here
  // partway through a branch.
  //
  // A GTE command is the one instruction an interrupt may not be delivered in
  // front of. The hardware issues it to the coprocessor before it recognises
  // the interrupt, so by the time the handler runs the command has already
  // happened - and the BIOS handler knows it, which is why it fetches the
  // instruction at EPC, tests it for COP2-with-bit-25 (the 0x4A compare at
  // 0x00000CD8) and steps EPC over it before returning. Deliver the interrupt
  // in front of the instruction and it never executes, the BIOS skips it
  // anyway, and the command is simply lost.
  //
  // Losing one is not subtle. Silent Hill builds each display-list packet with
  // lwc2/DPCS/swc2 triplets, so a dropped DPCS leaves the colour FIFO holding
  // the previous packet's colour - and that word carries the primitive's
  // command byte. A 0x38 flat quad wearing a 0x3C textured-quad byte makes the
  // GPU read a 12-word packet out of an 8-word one, and every command after it
  // is read at the wrong offset. The game's colour words happen to look like
  // valid commands, so the stream stays plausibly misaligned instead of
  // failing outright, until one is executed as GP0(02h) Fill Rectangle and
  // paints a block of ground colour across the texture pages.
  //
  // So let it run first and raise the interrupt behind it, with EPC still
  // pointing at the instruction - which is what the BIOS is expecting to skip.
  bool gte_command_first = false;
  uint32_t gte_command_pc = 0;

  // What the R3000A actually decides on: Cause's pending field against SR's
  // mask, both bits 8-15, with IEc as the global enable. For the interrupt
  // controller's own line that is the same test as `interrupt_stat &
  // interrupt_mask` and SR bit 10, which is what this used to read directly;
  // going through Cause adds the two software-interrupt bits, which software
  // sets with MTC0 and which are as real as the hardware one (bug 84).
  const uint32_t cause_pending = (cpu_.CauseRegister() >> 8) & 0xFF;
  const uint32_t cause_masked = cause_pending & ((cpu_.context()->ctrl.SR.raw >> 8) & 0xFF);

  if (cause_pending != 0) {
    if (cause_masked != 0 && cpu_.context()->ctrl.SR.IEc) {
      ++interrupts_taken_;
      const uint32_t pending = io_.io.interrupt_stat & io_.io.interrupt_mask;
      for (int bit = 0; bit < 11; ++bit)
        if (pending & (1u << bit))
          ++interrupts_taken_by_source_[bit];
      if (cpu_.NextIsGteCommand()) {
        gte_command_first = true;
        gte_command_pc = cpu_.context()->pc;
        ++interrupts_after_gte_command_;
      } else {
        cpu_.RaiseException(cpu_.context()->pc, kOtherException,
                            kExceptionCodeInt);
      }
    } else if (cause_masked == 0) {
      // Pending, but every pending line is masked off in SR.
      ++interrupts_blocked_im_;
    } else {
      // Unmasked and pending, but IEc says not now.
      ++interrupts_blocked_;
    }
  }

  // The debugger (psx/debugger.h) may stop the machine here, before the instruction at the pc
  // runs - after any interrupt has moved the pc, so a breakpoint on an exception vector fires
  // however the vector was reached, and before the BIOS-call hook, so a halted step records
  // nothing. A halted step does nothing at all: no instruction, no time.
  if constexpr (kDebugger) {
    if (debugger_.armed() && debugger_.ShouldHalt(cpu_.context()->pc))
      return false;
  }

  // A BIOS call is a jump to A0h, B0h or C0h with the function number in t1.
  // It is noticed here, before the instruction at the vector runs and after
  // any interrupt has moved the pc, so that both CPUs go through it: the
  // recompiler bridge never compiles a block at those three addresses, so
  // compiled code always comes back to this point before one runs. It used to
  // be checked at the end of Cpu::ExecuteInstruction, which compiled code
  // never reaches - with the recompiler on, the BIOS console recorded nothing.
  {
    const uint32_t pc = cpu_.context()->pc;
    if (pc == 0xA0 || pc == 0xB0 || pc == 0xC0) {
      kernel_.Call();
      debugger_.OnBiosCall();   // the call log (psx/debugger.h) - every call, armed or not
    }
  }

  // With the recompiler on, one step is a chain of compiled blocks rather than
  // one instruction - but only when nothing about the machine's state makes
  // that unsafe. A GTE command has to be the interpreter's, because the
  // interrupt behind it is delivered by the code below; and an interrupt that
  // was just raised has to reach the vector before any block runs.
  //
  // What compiled code ran is charged to the rest of the machine afterwards:
  // it does not tick as it goes, the way ExecuteInstruction does.
  // While the debugger could halt the machine, it runs interpreted: a compiled chain is many
  // instructions per step, and a breakpoint in the middle of one would never be seen.
  if (recompiler_ != nullptr && !gte_command_first && !(kDebugger && debugger_.armed())) {
    const uint32_t cycles = recompiler_->Step();
    if (cycles > 0)
      cpu_.TickCycles(cycles);
  } else {
    cpu_.ExecuteInstruction();
  }

  if (gte_command_first) {
    cpu_.RaiseException(gte_command_pc, kOtherException, kExceptionCodeInt);
  }

  if (auto_boot_ && cpu_.context()->pc == 0x80030000) {
    auto_boot_ = false;
    if (!auto_boot_path_.empty()) {
      LoadDisc(auto_boot_path_.c_str());
    }
    BootDisc(nullptr);
  }

  // POST 7: the kernel is initialised and the shell not yet copied into RAM.
  // See set_auto_boot_exe.
  if (auto_boot_exe_ && (io_.io.post & 0x0F) == 7) {
    auto_boot_exe_ = false;
    LoadPsExe(auto_boot_exe_path_.c_str());
  }
  return true;
}

template bool System::StepImpl<true>();
template bool System::StepImpl<false>();

void System::LoadBiosFromMemory(const void* buffer) {
  memcpy(io_.bios_buffer.u8, buffer, kBiosSize);
}

// A BIOS dump is required, so a missing or wrong-sized file is a hard failure
// rather than something to carry on past with a buffer full of zeroes.
bool System::LoadBiosFromFile(const char* filename) {
  FILE* fp = fopen(filename, "rb");
  if (fp == nullptr)
    return false;

  fseek(fp, 0, SEEK_END);
  const long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (size != kBiosSize) {
    fclose(fp);
    return false;
  }

  uint8_t* buffer = new uint8_t[kBiosSize];
  const size_t read = fread(buffer, 1, kBiosSize, fp);
  fclose(fp);

  bool ok = false;
  if (read == kBiosSize) {
    LoadBiosFromMemory(buffer);
    ok = true;
  }
  delete[] buffer;
  return ok;
}

// A PS-EXE, wherever it came from. The disc path and the side-load path go
// through the same code so that booting a game and booting a test executable
// cannot drift apart.
bool System::LoadPsExeFromMemory(const void* data, size_t size) {
  struct PSXEXE {
    char     id[8];
    uint32_t text;
    uint32_t data;
    uint32_t pc0;
    uint32_t gp0;
    uint32_t t_addr;
    uint32_t t_size;
    uint32_t d_addr;
    uint32_t d_size;
    uint32_t b_addr;
    uint32_t b_size;
    uint32_t s_addr;
    uint32_t s_size;
    uint32_t saved_sp;
    uint32_t saved_fp;
    uint32_t saved_gp;
    uint32_t saved_ra;
    uint32_t saved_s0;
  };

  // The header occupies the start of the first 2048 bytes; the image begins
  // where that region ends. The header is *inside* those 2048 bytes, not after
  // them, so the minimum size is the header itself - requiring 0x800 plus a
  // header rejects every executable with a small text section.
  const size_t kHeaderRegion = 0x800;
  if (data == nullptr || size < sizeof(PSXEXE))
    return false;

  PSXEXE header;
  memcpy(&header, data, sizeof(header));
  if (memcmp(header.id, "PS-X EXE", 8) != 0)
    return false;

  const uint32_t offset = header.t_addr & 0x1FFFFF;
  if (header.t_size == 0 || offset + header.t_size > kRamSize)
    return false;
  if (size < kHeaderRegion + header.t_size)
    return false;

  memcpy(&io_.ram_buffer.u8[offset],
         static_cast<const uint8_t*>(data) + kHeaderRegion, header.t_size);

  // A side-loaded executable is new code arriving in RAM without a single
  // store, so anything compiled from what used to be there has to go.
  cpu_.NoteBulkWrite(offset, header.t_size);

  cpu_context_.pc = header.pc0;
  cpu_context_.prev_pc = header.pc0;
  cpu_context_.gp.reg[28] = header.gp0;                              // gp
  cpu_context_.gp.reg[29] = (header.s_addr == 0) ? 0x801FFF00        // sp
                                                 : header.s_addr;
  cpu_context_.gp.reg[30] = cpu_context_.gp.reg[29];                 // fp
  return true;
}

bool System::LoadPsExe(const char* filename) {
  FILE* fp = fopen(filename, "rb");
  if (fp == nullptr)
    return false;

  fseek(fp, 0, SEEK_END);
  const long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (size <= 0) {
    fclose(fp);
    return false;
  }

  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  const size_t read = fread(&buffer[0], 1, buffer.size(), fp);
  fclose(fp);
  if (read != buffer.size())
    return false;

  return LoadPsExeFromMemory(&buffer[0], buffer.size());
}

// Reads the BOOT line out of SYSTEM.CNF. The file is a handful of KEY = VALUE
// lines; the one that matters names the executable, usually as
// "cdrom:\SLUS_007.55;1". Line endings and spacing vary by publisher, so this
// is deliberately forgiving about both.
bool System::ParseSystemCnf(const std::vector<uint8_t>& contents,
                            std::string* boot_path) {
  if (contents.empty())
    return false;
  const std::string text(reinterpret_cast<const char*>(&contents[0]),
                         contents.size());
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

    // Trim both halves, and upper-case the key so "boot" matches too.
    const char* kSpace = " \t";
    const size_t key_begin = key.find_first_not_of(kSpace);
    const size_t key_end = key.find_last_not_of(kSpace);
    if (key_begin == std::string::npos)
      continue;
    key = key.substr(key_begin, key_end - key_begin + 1);
    for (size_t i = 0; i < key.size(); ++i)
      key[i] = static_cast<char>(toupper(static_cast<unsigned char>(key[i])));

    if (key != "BOOT")
      continue;

    const size_t value_begin = value.find_first_not_of(kSpace);
    const size_t value_end = value.find_last_not_of(kSpace);
    if (value_begin == std::string::npos)
      continue;
    *boot_path = value.substr(value_begin, value_end - value_begin + 1);
    return true;
  }
  return false;
}

// Boots whatever disc is in the drive: find SYSTEM.CNF, read the executable it
// names, and start it.
//
// This is the shortcut that skips the BIOS shell rather than the way real
// hardware does it, and it is deliberate for now - the CD-ROM controller can
// serve sectors but the BIOS's own boot path needs more of the drive than is
// implemented. `info` records what was found either way, so a failure says
// which step failed rather than just "did not boot".
bool System::BootDisc(DiscBootInfo* info) {
  DiscBootInfo local;
  if (info == nullptr)
    info = &local;
  *info = DiscBootInfo();

  if (!io_.cdrom.disc_loaded()) {
    info->error = "no disc is mounted";
    return false;
  }

  if (!iso_.Open(&io_.cdrom.disc())) {
    info->error = "the disc has no ISO9660 filesystem";
    return false;
  }
  info->volume_id = iso_.volume_id();

  Iso9660::File config;
  if (!iso_.Find("SYSTEM.CNF", &config)) {
    // A few discs omit it and are expected to run PSX.EXE instead.
    Iso9660::File fallback;
    if (iso_.Find("PSX.EXE", &fallback)) {
      info->boot_path = "PSX.EXE";
      info->executable = fallback.name;
      std::vector<uint8_t> image;
      if (!iso_.Read(fallback, &image)) {
        info->error = "PSX.EXE could not be read";
        return false;
      }
      info->executable_size = static_cast<uint32_t>(image.size());
      if (!LoadPsExeFromMemory(&image[0], image.size())) {
        info->error = "PSX.EXE is not a valid executable";
        return false;
      }
      return true;
    }
    info->error = "no SYSTEM.CNF and no PSX.EXE on the disc";
    return false;
  }

  std::vector<uint8_t> contents;
  if (!iso_.Read(config, &contents)) {
    info->error = "SYSTEM.CNF could not be read";
    return false;
  }

  if (!ParseSystemCnf(contents, &info->boot_path)) {
    info->error = "SYSTEM.CNF has no BOOT line";
    return false;
  }

  Iso9660::File executable;
  if (!iso_.Find(info->boot_path.c_str(), &executable)) {
    info->error = "the executable named by BOOT is not on the disc";
    return false;
  }
  info->executable = executable.name;

  std::vector<uint8_t> image;
  if (!iso_.Read(executable, &image)) {
    info->error = "the executable could not be read";
    return false;
  }
  info->executable_size = static_cast<uint32_t>(image.size());

  if (!LoadPsExeFromMemory(&image[0], image.size())) {
    info->error = "the executable is not a valid PS-EXE";
    return false;
  }
  return true;
}

// See Docs/Save-States-Plan.md. Fixed serialisation order, both directions:
// cpu_context_, cpu_, gte_, io_ (which cascades into its own sub-components,
// see IOInterface::Serialise), gpu_, spu_.
std::string System::SaveState(const std::string& path) {
  StateIO state(/*saving=*/true);
  cpu_context_.Serialise(state);
  cpu_.Serialise(state);
  gte_.Serialise(state);
  io_.Serialise(state);
  gpu_.Serialise(state);
  spu_.Serialise(state);

  FILE* fp = fopen(path.c_str(), "wb");
  if (fp == nullptr)
    return "could not create " + path;

  fwrite(kStateMagic, 1, sizeof(kStateMagic), fp);
  const uint32_t version = kStateVersion;
  fwrite(&version, sizeof(version), 1, fp);
  const uint64_t bios_hash = Fnv1a64(io_.bios_buffer.u8, kBiosSize);
  fwrite(&bios_hash, sizeof(bios_hash), 1, fp);
  const std::string& disc_path = io_.cdrom.disc().path();
  const uint32_t disc_path_length = static_cast<uint32_t>(disc_path.size());
  fwrite(&disc_path_length, sizeof(disc_path_length), 1, fp);
  if (disc_path_length > 0)
    fwrite(disc_path.data(), 1, disc_path_length, fp);

  const std::vector<uint8_t>& payload = state.bytes();
  if (!payload.empty())
    fwrite(payload.data(), 1, payload.size(), fp);
  fclose(fp);
  return "";
}

std::string System::LoadState(const std::string& path) {
  FILE* fp = fopen(path.c_str(), "rb");
  if (fp == nullptr)
    return "could not open " + path;

  char magic[sizeof(kStateMagic)];
  if (fread(magic, 1, sizeof(magic), fp) != sizeof(magic) ||
      memcmp(magic, kStateMagic, sizeof(magic)) != 0) {
    fclose(fp);
    return path + " is not a save state";
  }
  uint32_t version = 0;
  fread(&version, sizeof(version), 1, fp);
  if (version != kStateVersion) {
    fclose(fp);
    char detail[128];
    snprintf(detail, sizeof(detail),
             "save state is version %u, this build expects version %u",
             version, kStateVersion);
    return detail;
  }
  uint64_t bios_hash = 0;
  fread(&bios_hash, sizeof(bios_hash), 1, fp);
  if (bios_hash != Fnv1a64(io_.bios_buffer.u8, kBiosSize)) {
    fclose(fp);
    return "save state was made with a different BIOS";
  }
  // A courtesy copy for a future save browser - System::LoadState restores
  // the real disc path from the payload below (Cdrom::Serialise), which is
  // the single source of truth, so this is only skipped over here.
  uint32_t disc_path_length = 0;
  fread(&disc_path_length, sizeof(disc_path_length), 1, fp);
  if (disc_path_length > 0)
    fseek(fp, disc_path_length, SEEK_CUR);

  std::vector<uint8_t> payload;
  uint8_t chunk[65536];
  size_t read;
  while ((read = fread(chunk, 1, sizeof(chunk), fp)) > 0)
    payload.insert(payload.end(), chunk, chunk + read);
  fclose(fp);

  StateIO state(/*saving=*/false);
  state.BeginLoad(payload.data(), payload.size());
  cpu_context_.Serialise(state);
  cpu_.Serialise(state);
  gte_.Serialise(state);
  io_.Serialise(state);
  gpu_.Serialise(state);
  spu_.Serialise(state);

  if (state.truncated())
    return path + " is truncated or corrupt";
  if (!state.error().empty())
    return state.error();

  // Every byte of RAM has just been replaced, so nothing compiled from the
  // old contents means anything. Save states do not carry compiled code and
  // never will - it is derived, like the GPU's framebuffer.
  cpu_.NoteBulkWrite(0, kRamSize);

  // Iso9660 is derived from the disc, not saved (the same reasoning as the
  // GPU's framebuffer) - reopen it the way BootDisc does, if the disc
  // Cdrom::Serialise just reopened actually has a filesystem. Some discs
  // (pure CD-DA) legitimately don't; that is not a load failure.
  if (io_.cdrom.disc_loaded())
    iso_.Open(&io_.cdrom.disc());
  else
    iso_.Close();

  // The machine is somewhere else now: a halt, or a step half taken, was about the old one. The
  // breakpoints stay, as they do across a reset.
  debugger_.Reset();

  return "";
}

// The recompiler is created on demand and destroyed when it is turned off, so
// with it off the machine carries no trace of it: nothing worth naming in
// StepInstruction, no store observer on the Cpu, and no compiled code held.
void System::ResetCompiledCode() {
  if (recompiler_ != nullptr)
    recompiler_->Reset();
}

void System::EnableRecompiler(bool on) {
  // The setting is the thing StepInstruction reconciles against, so a direct
  // call has to move it too - otherwise the next instruction would notice the
  // disagreement and undo this. That is not hypothetical: it made
  // `boot_runner --recompiler` a silent no-op.
  config_.recompiler = on;
  if (on == (recompiler_ != nullptr))
    return;
  if (on)
    recompiler_.reset(new RecompilerBridge(this));
  else
    recompiler_.reset();
}


}
}

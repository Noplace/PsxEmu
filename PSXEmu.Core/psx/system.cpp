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
//#include <stdio.h>
//#include <stdlib.h>
//#pragma warning( disable : 4996 )

namespace emulation {
namespace psx {

uint64_t TrapCounter::count = 0;
uint64_t TrapCounter::rfe_count = 0;
ExceptionLog::Entry ExceptionLog::entries[ExceptionLog::kCapacity] = {};
uint32_t ExceptionLog::written = 0;

System::System() {
  memset(&cpu_context_,0,sizeof(cpu_context_));
  base_freq_hz_  = 33868800.0;
  interrupts_taken_ = 0;
  interrupts_blocked_ = 0;
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
  kernel_.Initialize();
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
  spu_.Deinitialize();
  gpu_.Deinitialize();
  cpu_.Deinitialize();
  io_.Deinitialize();
  return 0;
}

// One instruction, with any pending interrupt taken before the next one. No
// wall clock is consulted, so a headless run is reproducible.
void System::StepInstruction() {
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
  if (io_.io.interrupt_stat & io_.io.interrupt_mask) {
    // Cop0 SR: bit 10 is the hardware interrupt mask line the PSX wires all
    // of its interrupts to, and IEc is the global enable.
    if ((cpu_.context()->ctrl.SR.raw & 0x400) && (cpu_.context()->ctrl.SR.IEc)) {
      ++interrupts_taken_;
      cpu_.RaiseException(cpu_.context()->pc, kOtherException,
                          kExceptionCodeInt);
    } else if (!(cpu_.context()->ctrl.SR.raw & 0x400)) {
      ++interrupts_blocked_im_;
    } else {
      ++interrupts_blocked_;
    }
  }

  cpu_.ExecuteInstruction();

  if (auto_boot_ && cpu_.context()->pc == 0x80030000) {
    auto_boot_ = false;
    BootDisc(nullptr);
  }
}

// Paced against the wall clock, for a front end that wants the machine to run
// at something close to real speed.
void System::Step() {
  const double dt = 1000.0 / base_freq_hz_;
  timing_.current_cycles = timer.GetCurrentCycles();
  timing_.time_span =
      (timing_.current_cycles - timing_.prev_cycles) * timer.resolution();
  if (timing_.time_span > 500.0)  // clamping time
    timing_.time_span = 500.0;

  timing_.span_accumulator += timing_.time_span;

  while (timing_.span_accumulator >= dt) {
    StepInstruction();
    const uint64_t spent = cpu_.context()->current_cycles;
    timing_.span_accumulator -= dt * (spent > 0 ? spent : 1);
  }

  timing_.total_cycles += timing_.current_cycles - timing_.prev_cycles;
  timing_.prev_cycles = timing_.current_cycles;
  timing_.fps_time_span += timing_.time_span;
}

void System::Run() {
  if (thread!=nullptr && state == 1) return;
  state = 1;
  cycles_per_second_ = 0;
  thread = new std::thread(System::thread_func,this);
}

void System::Stop() {
  if (thread==nullptr && state == 0) return;
  state = 0;
  thread->join();
  OutputDebugStringA("killed thread\n");
  SafeDelete(&thread);
}

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

void System::thread_func(System* sys) {
  memset(&sys->timing_,0,sizeof(sys->timing_));
  sys->timer.Calibrate();
  sys->timing_.prev_cycles = sys->timer.GetCurrentCycles();
  //gfx init  

  while (sys->state != 0) {
      sys->Step();
  }
 
  //opengl.Deinitialize();
  OutputDebugStringA("end of thread\n");
}


}
}

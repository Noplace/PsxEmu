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
  mc_.set_system(this);
  kernel_.set_system(this);
  gte_.set_system(this);

  io_.Initialize();
  cpu_.set_context(&cpu_context_);
  cpu_.Initialize();
  cpu_.Reset();
  gpu_.Initialize();
  spu_.Initialize();
  mc_.Initialize();
  kernel_.Initialize();
  gte_.Initialize();
  //mc_.LoadFile("D:\\Personal\\Projects\\PsxEmu\\test\\ff7.mcr");
  
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
  mc_.Deinitialize();
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

bool System::LoadPsExe(const char* filename) {
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

  FILE* fp = fopen(filename, "rb");
  if (fp == nullptr)
    return false;

  PSXEXE header;
  if (fread(&header, sizeof(PSXEXE), 1, fp) != 1) {
    fclose(fp);
    return false;
  }
  if (memcmp(header.id, "PS-X EXE", 8) != 0) {
    fclose(fp);
    return false;
  }

  const uint32_t offset = header.t_addr & 0x1FFFFF;
  if (offset + header.t_size > kRamSize) {
    fclose(fp);
    return false;
  }

  fseek(fp, 0x800, SEEK_SET);
  const size_t read = fread(&io_.ram_buffer.u8[offset], 1, header.t_size, fp);
  fclose(fp);
  if (read != header.t_size)
    return false;

  cpu_context_.pc = header.pc0;
  cpu_context_.prev_pc = header.pc0;
  cpu_context_.gp.reg[28] = header.gp0;                              // gp
  cpu_context_.gp.reg[29] = (header.s_addr == 0) ? 0x801FFF00        // sp
                                                 : header.s_addr;
  cpu_context_.gp.reg[30] = cpu_context_.gp.reg[29];                 // fp
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

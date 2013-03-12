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
#ifndef EMULATION_PSX_SYSTEM_H
#define EMULATION_PSX_SYSTEM_H

//forwards
namespace emulation {
namespace psx {
class Dma;
}
}

#include <WinCore/types.h>
#include <memory.h>
#include "types.h"
#ifdef _DEBUG
#define BREAKPOINT DebugBreak();
#define PC_BREAKPOINT(x) if (context_->pc==x) { DebugBreak(); }
#include <Windows.h>
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include "debug_assist.h"
#else
#define BREAKPOINT 
#endif
#include "component.h"
#include "cpu/cpu_context.h"
#include "cpu/cpu.h"
#include "cpu/gte.h"
#include "gpu/gpu.h"
#include "spu/spu.h"
#include "io/root_counter.h"
#include "io/dma.h"
#include "io/io_interface.h"
#include "bios/kernel.h"
#include "mc/mc.h"


namespace emulation {
namespace psx {

class System {
 friend DebugAssist;
 public:
  System();
  ~System();
  int Initialize();
  int Deinitialize();
  void Step();
  void LoadBiosFromMemory(void* buffer);
  void LoadBiosFromFile(char* filename);
  void LoadPsExe(char* filename);
  Cpu& cpu() { return cpu_; };
  Gpu& gpu() { return gpu_; };
  Spu& spu() { return spu_; };
  IOInterface& io() { return io_; };
  MC& mc() { return mc_; };
  Kernel& kernel() { return kernel_; };
  uint8_t* ram() { return io_.ram_buffer.u8; }
  uint8_t* bios() { return io_.bios_buffer.u8; }
  uint32_t master_clock_frequency() { return mcf_; }
  void set_master_clock_frequency(uint32_t mcf) { mcf_ = mcf; }
  #ifdef _DEBUG
  DebugAssist csvlog;
  #endif
 private:
  CpuContext cpu_context_;
  Cpu cpu_;
  Gpu gpu_;
  Spu spu_;
  IOInterface io_;
  MC mc_;
  Kernel kernel_;
  uint32_t mcf_;
};

}
}

#endif
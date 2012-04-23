/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : system.cpp
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
#ifndef EMULATION_PSX_SYSTEM_H
#define EMULATION_PSX_SYSTEM_H

#include <WinCore/types.h>
#include <memory.h>
#include "types.h"
#ifndef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include "debug_assist.h"
#endif
#include "component.h"
#include "cpu/cpu_context.h"
#include "cpu/cpu.h"
#include "cpu/gte.h"
#include "gpu/gpu.h"
#include "spu/spu.h"
#include "io/io_interface.h"
#include "bios/kernel.h"


namespace emulation {
namespace psx {

class System {
 public:
  System();
  ~System();
  int Initialize();
  int Deinitialize();
  void Step();
  void LoadBiosFromMemory(void* buffer);
  void LoadBiosFromFile(char* filename);
  Cpu& cpu() { return cpu_; };
  //Gpu& gpu() { return gpu_; };
  Spu& spu() { return spu_; };
  IOInterface& io() { return io_; };
  uint8_t* ram() { return io_.ram_buffer.u8; }
  uint8_t* bios() { return io_.bios_buffer.u8; }
  DebugAssist log;
 private:
   CpuContext cpu_context_;
   Cpu cpu_;
   //Gpu gpu_;
   Spu spu_;
   IOInterface io_;
   Kernel kernel_;
};

}
}

#endif
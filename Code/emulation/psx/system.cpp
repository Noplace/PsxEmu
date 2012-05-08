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
#include "system.h"
#include <stdio.h>
#include <stdlib.h>
#pragma warning( disable : 4996 )

namespace emulation {
namespace psx {

System::System() {
  memset(&cpu_context_,0,sizeof(cpu_context_));
  mcf_ = 33868800;
}

System::~System() {
    
    
}

int System::Initialize() {
  #if defined(_DEBUG)
    csvlog.system_ = this;
    csvlog.Open("log.csv");
  #endif
 
  io_.set_system(this);
  cpu_.set_system(this);
  gpu_.set_system(this);
  spu_.set_system(this);
  kernel_.set_system(this);

  auto set_comp_systems = [&](Component& comp) {
    //comp.set_system(this);
    //comp.cpu_ = &this->cpu();
    //comp.io_ = &this->io();
  };


  io_.Initialize();
  LoadBiosFromFile("D:\\Personal\\Projects\\PsxEmu\\Bios\\SCPH1001.BIN");
  cpu_.set_context(&cpu_context_);
  cpu_.Initialize();
  cpu_.Reset();
  gpu_.Initialize();
  spu_.Initialize();
  kernel_.Initialize();

  while (cpu_.context()->pc!=0x80030000) {
	   Step();
  }

  return 0;
}

int System::Deinitialize() {
  spu_.Deinitialize();
  gpu_.Deinitialize();
  cpu_.Deinitialize();
  io_.Deinitialize();
  return 0;
}

void System::Step() {
  cpu_.context()->current_cycles = 0;
  cpu_.ExecuteInstruction();
  if (cpu_.context()->pc == 0xa0 || 
      cpu_.context()->pc == 0xb0 || 
      cpu_.context()->pc == 0xc0) {
        //bios call
        kernel_.Call();
        int a= 1;
  }
  
  io_.Tick(cpu_.context()->current_cycles);

  if (io_.interrupt_reg & io_.interrupt_mask)	{
    if ((cpu_.context()->ctrl.SR.raw & 0x400)&&(cpu_.context()->ctrl.SR.IEc))	{
        cpu_.RaiseException(cpu_.context()->prev_pc,kOtherException,kExceptionCodeInt);
        cpu_.context()->ctrl.Cause |= 0x400;
	   	  //Exception(0x400,branch_slot);
		}
	}

}

void System::LoadBiosFromMemory(void* buffer) {
  memcpy(io_.bios_buffer.u8,(uint8_t*)buffer,0x80000);
}

void System::LoadBiosFromFile(char* filename) {
  FILE* fp = fopen(filename,"rb");
  fseek(fp,0,SEEK_END);
  int size = ftell(fp);
  fseek(fp,0,SEEK_SET);
  if (size != 0x80000) 
    return;
  uint8_t* buffer = new uint8_t[0x80000];
  fread(buffer,sizeof(uint8_t),0x80000,fp);
  fclose(fp);
  LoadBiosFromMemory(buffer);
  delete [] buffer;
  buffer = NULL;
}

}
}
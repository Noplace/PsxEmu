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
#include "global.h"
//#include <stdio.h>
//#include <stdlib.h>
//#pragma warning( disable : 4996 )

namespace emulation {
namespace psx {

System::System() {
  memset(&cpu_context_,0,sizeof(cpu_context_));
  base_freq_hz_  = 33868800.0;
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
  gpu_core_->set_system(this);
  spu_.set_system(this);
  mc_.set_system(this);
  kernel_.set_system(this);
  gte_.set_system(this);

  auto set_comp_systems = [&](Component& comp) {
    //comp.set_system(this);
    //comp.cpu_ = &this->cpu();
    //comp.io_ = &this->io();
  };


  io_.Initialize();
  LoadBiosFromFile("bios\\SCPH1001.BIN");
  cpu_.set_context(&cpu_context_);
  cpu_.Initialize();
  cpu_.Reset();
  gpu_core_->Initialize();
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
  gpu_core_->Deinitialize();
  cpu_.Deinitialize();
  io_.Deinitialize();
  return 0;
}

void System::Step() {
  

  const double dt =  1000.0 / base_freq_hz_;//options.cpu_freq(); 0.00058f;//16.667f;
  timing_.current_cycles = timer.GetCurrentCycles();
  timing_.time_span =  (timing_.current_cycles - timing_.prev_cycles) * timer.resolution();
  if (timing_.time_span > 500.0) //clamping time
    timing_.time_span = 500.0;

  timing_.span_accumulator += timing_.time_span;
  
  
  //while (timing_.span_accumulator >= dt) {
    cpu_.context()->current_cycles = 0;
    cpu_.ExecuteInstruction();
    //io_.Tick(cycles);
    if (io_.io.interrupt_stat & io_.io.interrupt_mask)	{
      if ((cpu_.context()->ctrl.SR.raw & 0x400)&&(cpu_.context()->ctrl.SR.IEc))	{
          cpu_.RaiseException(cpu_.context()->prev_pc,kOtherException,kExceptionCodeInt);
		  }
	  }
    timing_.span_accumulator -= dt*cpu_.context()->current_cycles;
  //}


  timing_.total_cycles += timing_.current_cycles-timing_.prev_cycles;
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
  OutputDebugString("killed thread\n");
  SafeDelete(&thread);
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

void System::LoadPsExe(char* filename) {
  struct PSXEXE {
          unsigned char id[8];
          unsigned long text;
          unsigned long data;
          unsigned long pc0;
          unsigned long gp0;
          unsigned long t_addr;
          unsigned long t_size;
          unsigned long d_addr;
          unsigned long d_size;
          unsigned long b_addr;
          unsigned long b_size;
          unsigned long S_addr;
          unsigned long s_size;
          unsigned long SavedSP;
          unsigned long SavedFP;
          unsigned long SavedGP;
          unsigned long SavedRA;
          unsigned long SavedS0;
  };

  FILE *fp;

  fopen_s(&fp,filename,"rb");

  if (fp) {
    PSXEXE header;
    fread(&header,sizeof(PSXEXE),1,fp);
    fseek(fp,0x800,SEEK_SET);
    //if (header.t_addr > 0x80000000) {
      fread(&io_.ram_buffer.u8[header.t_addr&0x1FFFFF],header.t_size,1,fp);
      fclose(fp);
      cpu_context_.pc = header.pc0;
      cpu_context_.gp.reg[28] = header.gp0;
      cpu_context_.gp.reg[29] = (header.S_addr==0)?0x801fff00:header.S_addr;
    //}
  }
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
  OutputDebugString("end of thread\n");
}


}
}
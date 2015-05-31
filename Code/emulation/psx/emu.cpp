/*****************************************************************************************************************
* Copyright (c) 2013 Khalid Ali Al-Kooheji                                                                       *
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


namespace emulation {
namespace psx {

void Emu::Initialize(double base_freq_hz) {
  base_freq_hz_ = base_freq_hz;

  memory.Initialize(this);
  iointerface.Initialize(this);
  dma.Initialize(this);
  cpu.set_context(&cpu_context_);
  cpu.Initialize(this);
  cpu.Reset();
  gpu.Initialize(this);
  spu.Initialize(this);
  
  {
    uint8_t* data = nullptr;
    size_t length;
    util::ReadWholeFileBinary("bios\\SCPH1001.BIN",&data,length);
    memcpy(memory.rom.u8,data,length);
    util::DestroyFileBuffer(&data);
  }
}

void Emu::Deinitialize() {
  Stop();
  spu.Deinitialize();
  gpu.Deinitialize();
  cpu.Deinitialize();
  dma.Deinitialize();
  iointerface.Deinitialize();
  memory.Deinitialize();
}

double Emu::Step() {


  uint32_t cycles = 0;
  
  cpu.ExecuteInstruction();
  cycles += cpu_context_.current_cycles;

  if (iointerface.int_stat & iointerface.int_mask)	{
    if ((cpu_context_.ctrl.SR.raw & 0x400)&&(cpu_context_.ctrl.SR.IEc))	{
        cpu.RaiseException(cpu_context_.prev_pc,kOtherException,kExceptionCodeInt);
	  }
  }
/*
  const double dt =  1000.0 / base_freq_hz_;//options.cpu_freq(); 0.00058f;//16.667f;
  timing.current_cycles = utimer.GetCurrentCycles();
  timing.time_span =  (timing.current_cycles - timing.prev_cycles) * utimer.resolution();
  if (timing.time_span > 500.0) //clamping time
    timing.time_span = 500.0;

  timing.span_accumulator += timing.time_span;
  while (timing.span_accumulator >= dt) {
    {
      uint32_t cycles = 0;
  
      cpu.ExecuteInstruction();
      cycles += cpu_context_.current_cycles;

      iointerface.Tick(cycles);

      if (iointerface.int_stat & iointerface.int_mask)	{
        if ((cpu.context()->ctrl.SR.raw & 0x400)&&(cpu.context()->ctrl.SR.IEc))	{
            cpu.RaiseException(cpu.context()->prev_pc,kOtherException,kExceptionCodeInt);
		    }
	    }
      cpu_cycles_per_step_ = cycles;
    }
    
    timing.span_accumulator -= dt*cpu_cycles_per_step_;
    cpu_cycles_ += cpu_cycles_per_step_;
  }

  timing.misc_time_span += timing.time_span;
  if (timing.misc_time_span > 1000.0) {
    cycles_per_second_ = cpu_cycles_ - last_cpu_cycles_;
    frequency_mhz_ = double( cycles_per_second_ ) * 0.000001f;
    last_cpu_cycles_ = cpu_cycles_;
    timing.misc_time_span = 0;
  }

  timing.total_cycles += timing.current_cycles-timing.prev_cycles;
  timing.prev_cycles = timing.current_cycles;
  timing.fps_time_span += timing.time_span;
*/
  return timing.span_accumulator;
}

void Emu::Run() {
  if (thread!=nullptr && state == 1) return;
  state = 1;
  frequency_mhz_ = 0;
  cycles_per_second_ = 0;
  cpu_cycles_ = 0;
  last_cpu_cycles_ = 0;
  thread = new std::thread(Emu::thread_func,this);
}

void Emu::Stop() {
  if (thread==nullptr && state == 0) return;
  state = 0;
  thread->join();
  OutputDebugString("killed thread\n");
  SafeDelete(&thread);
}

void Emu::Pause() {
  Stop();
}

void Emu::Reset() {
  Stop();
}

void Emu::Render() {
  ++timing.fps_counter;
  on_render();
  if (timing.fps_time_span >= 1000.0) {
    timing.fps = timing.fps_counter * (1000.0/timing.fps_time_span);
    timing.fps_counter = 0;
    timing.fps_time_span = 0;
  }
}

void Emu::thread_func(Emu* emu) {
  memset(&emu->timing,0,sizeof(emu->timing));
  emu->utimer.Calibrate();
  emu->timing.prev_cycles = emu->utimer.GetCurrentCycles();
  gfx::OpenGL opengl;
  opengl.Initialize(emu->window_handle_,256,240);
  emu->gpu.set_graphics(&opengl);
  

  while (emu->state != 0) {
      emu->Step();
  }
  opengl.Deinitialize();
  OutputDebugString("end of thread\n");
}



}
}
   
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

//#define IODEBUG

namespace emulation {
namespace psx {

double video_clk;

int IOInterface::Initialize() { 
  cpu_ = &system().cpu();
  bios_buffer.Alloc(0x80000);
  ram_buffer.Alloc(0x200000);
 // io_buffer.Alloc(0x2000);
  parallel_port_buffer.Alloc(64*1024);
  scratchpad.Alloc(0x400);
  memset(ram_buffer.u8,0,0x200000);
  memset(scratchpad.u8,0,0x400);

  memset(parallel_port_buffer.u8,0,0xFFFF);

  
  io.exp1_base_addr = 0;
  io.exp2_base_addr = 0;
  io.exp1_delay = 0;
  io.exp3_delay = 0;
  io.bios_rom = 0;
  io.spu_delay = 0;
  io.cdrom_delay = 0;
  io.exp2_delay = 0;
  io.com_delay = 0;
  io.ram_size = 0;
  io.post = 0;
  io.interrupt_stat = 0;
  io.interrupt_mask = 0;
  io.cache_control = 0;

  dma.set_system(system_);
  dma.Initialize();

  memset(rootcounter_,0,sizeof(rootcounter_));
  rootcounter_[0].mode.en = rootcounter_[1].mode.en = rootcounter_[2].mode.en = 1;
  rootcounter_[3].target = ((uint32_t)system_->base_freq_hz() / 60) * 64;
  rootcounter_[3].WriteMode(0x58);

  //vertical retrace timer expermintal code
  video_clk = 0;

  return 0;
} 

int IOInterface::Deinitialize() {
  scratchpad.Dealloc();
  parallel_port_buffer.Dealloc();
 // io_buffer.Dealloc();
  ram_buffer.Dealloc();
  bios_buffer.Dealloc();
  return 0;
}

void IOInterface::SetInterrupt(InterruptCodes interrupt) {
  io.interrupt_stat |= interrupt;
}

void IOInterface::Tick(uint32_t cycles) {
  if (rootcounter_[0].Tick(cycles)==true) {
    if ((rootcounter_[0].mode.reached_0xffff && rootcounter_[0].mode.irq_0xffff)||
        (rootcounter_[0].mode.reached_target && rootcounter_[0].mode.irq_target)) {
      SetInterrupt(kInterruptCNT0);
    }
  }
  rootcounter_[1].Tick(cycles);
  rootcounter_[2].Tick(cycles);
  

  video_clk += cycles * 11.0 / 7.0;
  if (video_clk >= 887040.0) {
     //SetInterrupt(kInterruptVSYNC);
    system_->gpu_core()->Render();
    video_clk = 0;
  }
  
  if (rootcounter_[3].Tick(cycles)==true) {
    #ifdef _DEBUG
    //time_t  timev;
    //time(&timev);
    //struct tm today;
    //localtime_s(&today,&timev);
    fprintf(system_->csvlog.fp,",0x%08x,0x%08x,vsync\n",cpu_->index,cpu_->context()->prev_pc);
    #endif
    SetInterrupt(kInterruptVSYNC);
    int a =1;
  }
  dma.Tick();
}

/******************************************************************************
* Name        : Read08
* Description : read a byte at address
* Parameters  : address
*
* Notes :
* 
* 
*******************************************************************************/
uint8_t IOInterface::Read08(uint32_t address) {
  #if defined(_DEBUG) && defined(IODEBUG)
    if (system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,IO Read 8,0x%08X\n",system().cpu().index,system().cpu().context()->prev_pc,address);
  #endif
  BREAKPOINT
  /*if (IsValid(address,1) == false) {
    cpu_->context()->ctrl.BadVaddr = address;
    cpu_->RaiseException(cpu_->context()->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }*/

  /*if (address >= 0xBFC00000 && address <= 0xBFC80000 ) {
    //return *((uint32_t*)&system_->bios()[address & 0x0007FFFF]);
    return bios_buffer.u8[(address & 0x0007FFFF)];
  }

  
  if (address >= 0x1F000000 && address <= 0x1F00FFFF ) {
    return parallel_port_buffer.u8[address&0xFFFF];
  }
  
  
  if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    return ram_buffer.u8[(address & 0x001FFFFF)];
  }*/

  
  return 0;
}

/******************************************************************************
* Name        : Read16
* Description : read 2 bytes at address
* Parameters  : address
*
* Notes :
* 
* 
*******************************************************************************/
uint16_t IOInterface::Read16(uint32_t address) {
  #if defined(_DEBUG) && defined(IODEBUG)
    if (system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,IO Read 16,0x%08X\n",system().cpu().index,system().cpu().context()->prev_pc,address);
  #endif

  /*if (IsValid(address,2) == false) {
    cpu_->context()->ctrl.BadVaddr = address;
    cpu_->RaiseException(cpu_->context()->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }*/

  switch (address) {
    case 0x1F801070: return io.interrupt_stat&0xFFFF;
    case 0x1F801074: return io.interrupt_mask&0xFFFF;
    case 0x1F801100: return rootcounter_[0].ReadCounter();
    case 0x1F801104: return rootcounter_[0].ReadMode(); 
    case 0x1F801108: return rootcounter_[0].ReadTarget(); 
    case 0x1F801110: return rootcounter_[1].ReadCounter();        
    case 0x1F801114: return rootcounter_[1].ReadMode(); 
    case 0x1F801118: return rootcounter_[1].ReadTarget(); 
    case 0x1F801120: return rootcounter_[2].ReadCounter();
    case 0x1F801124: return rootcounter_[2].ReadMode(); 
    case 0x1F801128: return rootcounter_[2].ReadTarget(); 
    /*case 0x1F801130: return;
    case 0x1F801134: rootcounter_[3].WriteMode(data); return;
    case 0x1F801138: rootcounter_[3].WriteTarget(data); return;*/
  }

  //SPU range
  if (address >= 0x1F801C00 && address <= 0x1F801DFF) {
    return system_->spu().Read(address);
  }


  
  BREAKPOINT
  return 0;
}

/******************************************************************************
* Name        : Read32
* Description : read 4 bytes at address
* Parameters  : address
*
* Notes :
* 
* 
*******************************************************************************/
uint32_t IOInterface::Read32(uint32_t address) {
  #if defined(_DEBUG) && defined(IODEBUG)
    if (system_->csvlog.fp && cpu_->current_stage != 1)
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,IO Read 32,0x%08X\n",system().cpu().index,system().cpu().context()->prev_pc,address);
  #endif

  if (address == 0xFFFE0130) {
    BREAKPOINT
    return io.cache_control;
  }

  if (address >= 0x1F801080 && address <= 0x1F8010F4) {
    return dma.Read(address);
  }

  switch (address) {
    case 0x1F801070: return io.interrupt_stat;
    case 0x1F801074: return io.interrupt_mask;
    case 0x1F801100: return rootcounter_[0].ReadCounter();
    case 0x1F801104: return rootcounter_[0].ReadMode(); 
    case 0x1F801108: return rootcounter_[0].ReadTarget(); 
    case 0x1F801110: return rootcounter_[1].ReadCounter();        
    case 0x1F801114: return rootcounter_[1].ReadMode(); 
    case 0x1F801118: return rootcounter_[1].ReadTarget(); 
    case 0x1F801120: return rootcounter_[2].ReadCounter();
    case 0x1F801124: return rootcounter_[2].ReadMode(); 
    case 0x1F801128: return rootcounter_[2].ReadTarget(); 
    case 0x1F801810: return system_->gpu_core()->ReadData();
    case 0x1F801814: return system_->gpu_core()->ReadStatus();
    
  }
  BREAKPOINT
  //todo: hardware io
  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    BREAKPOINT
    return 0;//io_buffer.u32[((address-0x1000)&0x1FFF)>>2];
  }
  
  /*if (address >= 0xBFC00000 && address <= 0xBFC80000 ) {
    return bios_buffer.u32[(address & 0x0007FFFF)>>2];
    //return *((uint32_t*)&system_->bios()[address & 0x0007FFFF]);
    //return ((uint32_t*)system_->bios())[(address & 0x0007FFFF)>>2];
  }*/

  

  BREAKPOINT
  return 0;
}

/******************************************************************************
* Name        : Write08
* Description : write a byte at address
* Parameters  : address data
*
* Notes :
* 
* 
*******************************************************************************/
void IOInterface::Write08(uint32_t address,uint8_t data) {
  #if defined(_DEBUG) && defined(IODEBUG)
    if (system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,IO Write 8,0x%08X,Data,0x%02X\n",system().cpu().index,system().cpu().context()->prev_pc,address,data);
  #endif
  /*if (IsValid(address,1) == false) {
    cpu_->context()->ctrl.BadVaddr = address;
    cpu_->RaiseException(cpu_->context()->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }*/

  /*if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    ram_buffer.u8[(address & 0x001FFFFF)] = data;
    return;
  }*/

  switch (address) {
    case 0x1F802041:
      io.post = data;
      return;
  };
    
  if (address >= 0x1F802020 && address <= 0x1F8002F ) {
    //io_buffer.u8[((address-0x1000)&0x1FFF)] = data;
    BREAKPOINT
    

    return;
  }


  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    //io_buffer.u8[((address-0x1000)&0x1FFF)] = data;
    BREAKPOINT
    return;
  }

  BREAKPOINT
}

/******************************************************************************
* Name        : Write16
* Description : write 2 bytes at address
* Parameters  : address data
*
* Notes :
* 
* 
*******************************************************************************/
void IOInterface::Write16(uint32_t address,uint16_t data) {
  #if defined(_DEBUG) && defined(IODEBUG)
    if (system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,IO Write 16,0x%08X,Data,0x%04X\n",system().cpu().index,system().cpu().context()->prev_pc,address,data);
  #endif
    
  switch (address) {
    case 0x1F801070: io.interrupt_stat = (io.interrupt_stat&0xFFFF0000)|(data & io.interrupt_mask & 0xFFFF);  return;
    case 0x1F801074: io.interrupt_mask = (io.interrupt_mask&0xFFFF0000)|(data & 0xFFFF);  return;
    case 0x1F801100: rootcounter_[0].WriteCounter(data); return;
    case 0x1F801104: rootcounter_[0].WriteMode(data); return;
    case 0x1F801108: rootcounter_[0].WriteTarget(data); return;
    case 0x1F801110: rootcounter_[1].WriteCounter(data); return;
    case 0x1F801114: rootcounter_[1].WriteMode(data); return;
    case 0x1F801118: rootcounter_[1].WriteTarget(data); return;
    case 0x1F801120: rootcounter_[2].WriteCounter(data); return;
    case 0x1F801124: rootcounter_[2].WriteMode(data); return;
    case 0x1F801128: rootcounter_[2].WriteTarget(data); return;
    /*case 0x1F801130: return;
    case 0x1F801134: rootcounter_[3].WriteMode(data); return;
    case 0x1F801138: rootcounter_[3].WriteTarget(data); return;*/
  }

  if (address >= 0x1F801C00 && address <= 0x1F801DFF) {
    system_->spu().Write(address,data);
    return;
  }
  BREAKPOINT
}

/******************************************************************************
* Name        : Write32
* Description : write 4 bytes at address
* Parameters  : address data
*
* Notes :
* 
* 
*******************************************************************************/
void IOInterface::Write32(uint32_t address,uint32_t data) {
  #if defined(_DEBUG) && defined(IODEBUG)
    if (system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,IO Write 32,0x%08X,Data,0x%08X\n",system().cpu().index,system().cpu().context()->prev_pc,address,data);
  #endif


  if (address >= 0x1F801080 && address <= 0x1F8010F4) {
    dma.Write(address,data);
    return;
  }

  switch (address) {
    case 0x1F801000: io.exp1_base_addr = data; return;
    case 0x1F801004: io.exp2_base_addr = data; return;
    case 0x1F801008: io.exp1_delay = data; return;
    case 0x1F80100C: io.exp3_delay = data; return;
    case 0x1F801010: io.bios_rom = data; return;
    case 0x1F801014: io.spu_delay = data; return;
    case 0x1F801018: io.cdrom_delay = data; return;
    case 0x1F80101C: io.exp2_delay = data; return;
    case 0x1F801020: io.com_delay = data; return;
    case 0x1F801060: io.ram_size = data; return;
    case 0x1F801070: io.interrupt_stat = data & io.interrupt_mask;  return;
    case 0x1F801074: io.interrupt_mask = data;  return;
    case 0x1F801100: rootcounter_[0].WriteCounter(data); return;
    case 0x1F801104: rootcounter_[0].WriteMode(data); return;
    case 0x1F801108: rootcounter_[0].WriteTarget(data); return;
    case 0x1F801110: rootcounter_[1].WriteCounter(data); return;
    case 0x1F801114: rootcounter_[1].WriteMode(data); return;
    case 0x1F801118: rootcounter_[1].WriteTarget(data); return;
    case 0x1F801120: rootcounter_[2].WriteCounter(data); return;
    case 0x1F801124: rootcounter_[2].WriteMode(data); return;
    case 0x1F801128: rootcounter_[2].WriteTarget(data); return;
    case 0x1F801810: system_->gpu_core()->WriteData(data); return;
    case 0x1F801814: system_->gpu_core()->WriteStatus(data); return;
    case 0xFFFE0130: 
      io.cache_control = data; 
      if ((data&0x800)==0x800) 
        system_->cpu().icache.Invalidate(); 
      return;
  }
  BREAKPOINT
  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    //io_buffer.u32[((address-0x1000)&0x1FFF)>>2] = data;
    return;
  }

  BREAKPOINT
}


}
}
/******************************************************************************
* Filename    : io_interface.cpp
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
/*
** kuseg 0x0000 0000 – 7FFF FFFF
** kseg0 0x8000 0000 – 9FFF FFFF
** kseg1 0xA000 0000 – BFFF FFFF
** kseg2 0xC000 0000 – FFFF FFFF
**
**
**
*/
#include "../system.h"
#include <assert.h>

//#define IODEBUG

namespace emulation {
namespace psx {


/******************************************************************************
* Name        : Initialize
* Description : Initialize
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
int IOInterface::Initialize() { 
  cpu_ = &system().cpu();
  bios_buffer.Alloc(0x80000);
  ram_buffer.Alloc(0x200000);
  io_buffer.Alloc(0x2000);
  parallel_port_buffer.Alloc(64*1024);
  memset(ram_buffer.u8,0,0x200000);
	//memset(SRam->u8,0,0x1000);
	//memset(HRam->u8,0,0x10000);
  memset(io_buffer.u8,0,0x2000);
  memset(parallel_port_buffer.u8,0,0xFFFF);
  interrupt_reg() = 0x80;
  interrupt_mask() = 0;
  memset(rootcounter_,0,sizeof(rootcounter_));
  return 0;
} 

int IOInterface::Deinitialize() {
  parallel_port_buffer.Dealloc();
  io_buffer.Dealloc();
  ram_buffer.Dealloc();
  bios_buffer.Dealloc();
  return 0;
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
      fprintf(system_->csvlog.fp,"-,-,IO Read 08,0x%08X\n",address);
  #endif
  
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
      fprintf(system_->csvlog.fp,"-,-,IO Read 16,0x%08X\n",address);
  #endif

  /*if (IsValid(address,2) == false) {
    cpu_->context()->ctrl.BadVaddr = address;
    cpu_->RaiseException(cpu_->context()->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }*/

  switch (address) {
    case 0x1f801070: return  interrupt_reg()&0xFFFF;
    case 0x1f801074: return  interrupt_mask()&0xFFFF;
  }

  switch (address) {
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


  
  assert(1==0);
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
      fprintf(system_->csvlog.fp,"-,-,IO Read 32,0x%08X\n",address);
  #endif
  /*if (IsValid(address,4) == false) {
    cpu_->context()->ctrl.BadVaddr = address;
    cpu_->RaiseException(cpu_->context()->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }*/


  /*if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    return ram_buffer.u32[(address & 0x001FFFFF)>>2];
  }
  
  //todo: parallel port
  if (address >= 0x1F000000 && address <= 0x1F00FFFF ) {
    return parallel_port_buffer.u32[(address&0xFFFF)>>2];
  }
  
  //todo: scratch pad
  if (address >= 0x1F800000 && address <= 0x1F8003FF ) {
    assert(1==0);
  }
  */
  switch (address) {
    case 0x1F801070: return interrupt_reg();
    case 0x1F801074: return interrupt_mask();
    case 0x1F8010F0: return dma_enable.raw;
    case 0x1F801810: 
    case 0x1F801814: return system_->gpu().Read(address);
    
  }

  //todo: hardware io
  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    return io_buffer.u32[((address-0x1000)&0x1FFF)>>2];
  }
  
  /*if (address >= 0xBFC00000 && address <= 0xBFC80000 ) {
    return bios_buffer.u32[(address & 0x0007FFFF)>>2];
    //return *((uint32_t*)&system_->bios()[address & 0x0007FFFF]);
    //return ((uint32_t*)system_->bios())[(address & 0x0007FFFF)>>2];
  }*/

  

  assert(1==0);
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
      fprintf(system_->csvlog.fp,"-,-,IO Write 08,Data=0x%02X @ 0x%08X\n",data,address);
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

  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    io_buffer.u8[((address-0x1000)&0x1FFF)] = data;
    return;
  }

  assert(1==0);
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
      fprintf(system_->csvlog.fp,"-,-,IO Write 16,Data=0x%04X @ 0x%08X\n",data,address);
  #endif
  /*if (IsValid(address,2) == false) {
    cpu_->context()->ctrl.BadVaddr = address;
    cpu_->RaiseException(cpu_->context()->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }*/

  /*if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    ram_buffer.u16[(address & 0x001FFFFF)>>1] = data;
    return;
  }
  */
  switch (address) {
    case 0x1f801070:   io_buffer.u16[0x0070>>1] = data;  return;
    case 0x1f801074:   io_buffer.u16[0x0074>>1] = data;  return;
  }

  switch (address) {
    case 0x1F801100: return;
    case 0x1F801104: rootcounter_[0].WriteMode(data); return;
    case 0x1F801108: rootcounter_[0].WriteTarget(data); return;
    case 0x1F801110: return;
    case 0x1F801114: rootcounter_[1].WriteMode(data); return;
    case 0x1F801118: rootcounter_[1].WriteTarget(data); return;
    case 0x1F801120: return;
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

  assert(1==0);
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
      fprintf(system_->csvlog.fp,"-,-,IO Write 32,Data=0x%08X @ 0x%08X\n",data,address);
  #endif

  /*if (IsValid(address,4) == false) {
    cpu_->context()->ctrl.BadVaddr = address;
    cpu_->RaiseException(cpu_->context()->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }*/

  /*if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    ram_buffer.u32[(address & 0x001FFFFF)>>2] = data;
    return;
  }*/

  switch (address) {
    case 0x1F801070: interrupt_reg() = data;/*interrupt_reg &= (interrupt_mask&data);*/  return;
    case 0x1F801074: interrupt_mask() = data;  return;
    case 0x1F8010F0: dma_enable.raw = data; return;
    case 0x1F801810: 
    case 0x1F801814: system_->gpu().Write(address,data); return;
  }

  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    io_buffer.u32[((address-0x1000)&0x1FFF)>>2] = data;
    return;
  }

  assert(1==0);
}


}
}
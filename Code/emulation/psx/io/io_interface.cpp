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
  bios_buffer.Alloc(0x80000);
  ram_buffer.Alloc(0x200000);
  io_buffer.Alloc(0x2000);
  parallel_port_buffer.Alloc(64*1024);
  memset(io_buffer.u8,0,0x2000); 
  interrupt_reg() = 0x80;
  interrupt_mask() = 0;
  #if defined(_DEBUG) && defined(IODEBUG)
    debug.Open("io_log.txt");
  #endif
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
    if (debug.fp)
      fprintf(debug.fp,"%08X - IO Read 08 @ %08X\n",system().cpu().context()->prev_pc,address);
  #endif

  if (IsValid(address,1) == false) {
    system().cpu().context()->ctrl.BadVaddr = address;
    system().cpu().RaiseException(system().cpu().context()->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }

  if (address >= 0xBFC00000 && address <= 0xBFC80000 ) {
    //return *((uint32_t*)&system().bios()[address & 0x0007FFFF]);
    return system().bios()[(address & 0x0007FFFF)];
  }

  
  if (address >= 0x1F000000 && address <= 0x1F00FFFF ) {
    return parallel_port_buffer.u8[address&0xFFFF];
  }
  
  
  if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    return ram_buffer.u8[(address & 0x001FFFFF)];
  }

  assert(1==0);
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
    if (debug.fp)
      fprintf(debug.fp,"%08X - IO Read 16 @ %08X\n",system().cpu().context()->prev_pc,address);
  #endif

  if (IsValid(address,2) == false) {
    system().cpu().context()->ctrl.BadVaddr = address;
    system().cpu().RaiseException(system().cpu().context()->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }

  switch (address) {
    case 0x1f801070: return  interrupt_reg()&0xFFFF;
    case 0x1f801074: return  interrupt_mask()&0xFFFF;
  }

  //SPU range
  if (address >= 0x1F801C00 && address <= 0x1F801DFF) {
    return system().spu().Read(address);
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
    if (debug.fp)
      fprintf(debug.fp,"%08X - IO Read 32 @ %08X\n",system().cpu().context()->prev_pc,address);
  #endif
  if (IsValid(address,4) == false) {
    system().cpu().context()->ctrl.BadVaddr = address;
    system().cpu().RaiseException(system().cpu().context()->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }

  uint32_t paddress; //physical address
  if (address >= 0x00000000 && address <= 0x7FFFFFFF ) {
    paddress = address + 0x40000000;
  }

  if (address >= 0x80000000 && address <= 0x9FFFFFFF ) {
    paddress = address & 0x7FFFFFFF;
  }

  if (address >= 0xA0000000 && address <= 0xBFFFFFFF ) {
    paddress = address & 0x1FFFFFFF;
  }

  if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    return ram_buffer.u32[(address & 0x001FFFFF)>>2];
  }
  
  //todo: parallel port
  if (address >= 0x1F000000 && address <= 0x1F00FFFF ) {
    //return ParallelPort[];
  }
  
  //todo: scratch pad
  if (address >= 0x1F800000 && address <= 0x1F8003FF ) {
    
  }

  switch (address) {
    case 0x1f801070: return  interrupt_reg();
    case 0x1f801074: return  interrupt_mask();
  }

  //todo: hardware io
  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    return io_buffer.u32[((address-0x1000)&0x1FFF)>>2];
  }
  
  if (address >= 0xBFC00000 && address <= 0xBFC80000 ) {
    return bios_buffer.u32[(address & 0x0007FFFF)>>2];
    //return *((uint32_t*)&system().bios()[address & 0x0007FFFF]);
    //return ((uint32_t*)system().bios())[(address & 0x0007FFFF)>>2];
  }

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
    if (debug.fp)
      fprintf(debug.fp,"%08X - IO Write 08 %02X @ %08X\n",data,address);
  #endif
  if (IsValid(address,1) == false) {
    system().cpu().context()->ctrl.BadVaddr = address;
    system().cpu().RaiseException(system().cpu().context()->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }

  if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    ram_buffer.u8[(address & 0x001FFFFF)] = data;
    return;
  }

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
    if (debug.fp)
      fprintf(debug.fp,"%08X - IO Write 16 %04X @ %08X\n",data,address);
  #endif
  if (IsValid(address,2) == false) {
    system().cpu().context()->ctrl.BadVaddr = address;
    system().cpu().RaiseException(system().cpu().context()->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }

  if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    ram_buffer.u16[(address & 0x001FFFFF)>>1] = data;
    return;
  }
  
  switch (address) {
    case 0x1f801070:   io_buffer.u16[0x0070>>1] = data;  return;
    case 0x1f801074:   io_buffer.u16[0x0074>>1] = data;  return;
  }

  if (address >= 0x1F801C00 && address <= 0x1F801DFF) {
    system().spu().Write(address,data);
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
    if (debug.fp)
      fprintf(debug.fp,"%08X - IO Write 32 %08X @ %08X\n",data,address);
  #endif
  //todo: research about this value, ignore for now
  if (address == 0xFFFE0130)
    return;

  if (IsValid(address,4) == false) {
    system().cpu().context()->ctrl.BadVaddr = address;
    system().cpu().RaiseException(system().cpu().context()->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }

  if ((address >= 0x00000000 && address <= 0x001FFFFF ) ||
      (address >= 0x80000000 && address <= 0x801FFFFF ) ||
      (address >= 0xA0000000 && address <= 0xA01FFFFF )) {
    ram_buffer.u32[(address & 0x001FFFFF)>>2] = data;
    return;
  }

  switch (address) {
    case 0x1f801070:   interrupt_reg() = data;/*interrupt_reg &= (interrupt_mask&data);*/  return;
    case 0x1f801074:   interrupt_mask() = data;  return;
  }

  if (address >= 0x1F801000 && address <= 0x1F802FFF ) {
    io_buffer.u32[((address-0x1000)&0x1FFF)>>2] = data;
    return;
  }

  assert(1==0);
}

/******************************************************************************
* Name        : IsValid
* Description : validates the given address against some conditions
* Parameters  : address alignment
*
* Notes :
* 
* 
*******************************************************************************/
bool IOInterface::IsValid(uint32_t address,int alignment) {
  
  //not in kernel mode and accessing non kuseg
  if (address > 0x7FFFFFFF)
    if ((system().cpu().context()->ctrl.SR & 0x2) != 0) 
     return false;

  //misaligned read/write
  if ((address & (alignment-1)) != 0)
    return false;
 
  return true;
}

}
}
/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : io_interface.h
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
/*
  Memory Emulator
  Memory Chip
*/
#ifndef EMULATION_PSX_IO_INTERFACE_H
#define EMULATION_PSX_IO_INTERFACE_H

namespace emulation {
namespace psx {

class IOInterface : public Component {
 public:
  Buffer io_buffer;//[0x2000];
  Buffer parallel_port_buffer;//[64*1024];
  Buffer ram_buffer;//[0x200000];
  Buffer bios_buffer;//[0x80000];

  int Initialize();
  int Deinitialize();
  uint8_t Read08(uint32_t address);
  uint16_t Read16(uint32_t address);
  uint32_t Read32(uint32_t address);
  void Write08(uint32_t address,uint8_t data);
  void Write16(uint32_t address,uint16_t data);
  void Write32(uint32_t address,uint32_t data);
  uint32_t& interrupt_reg() { return io_buffer.u32[0x0070>>2]; }
  uint32_t& interrupt_mask() { return io_buffer.u32[0x0074>>2]; }
 private:
#ifndef NDEBUG
   DebugAssist debug;
#endif
  bool IsValid(uint32_t address,int alignment);
};

}
}

#endif
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
  //Buffer io_buffer;//[0x2000];
  Buffer parallel_port_buffer;//[64*1024];
  Buffer ram_buffer;//[0x200000];
  Buffer bios_buffer;//[0x80000];
  Buffer scratchpad;

  int Initialize();
  int Deinitialize();
  void SetInterrupt(InterruptCodes interrupt);
  void Tick(uint32_t cycles);
  uint8_t Read08(uint32_t address);
  uint16_t Read16(uint32_t address);
  uint32_t Read32(uint32_t address);
  void Write08(uint32_t address,uint8_t data);
  void Write16(uint32_t address,uint16_t data);
  void Write32(uint32_t address,uint32_t data);
  uint32_t interrupt_reg;
  uint32_t interrupt_mask;
  uint32_t hw_1000,hw_1004,hw_1008,hw_100C,hw_1010,hw_101C,ram_size,com_delay,spu_delay,dv5_delay;
  uint32_t cache_control;
  uint8_t io_post;
  RootCounter rootcounter_[4];
  Dma dma;
};

}
}

#endif
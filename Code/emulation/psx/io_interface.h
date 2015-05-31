/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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
#pragma once

namespace emulation {
namespace psx {

class IOInterface : public Component {
 public:
  struct {
    uint32_t exp1_base_addr;
    uint32_t exp2_base_addr;
    uint32_t exp1_delay;
    uint32_t exp3_delay;
    uint32_t bios_rom;
    uint32_t spu_delay;
    uint32_t cdrom_delay;
    uint32_t exp2_delay;
    uint32_t com_delay;
    uint32_t ram_size;
    uint8_t  post;
    uint32_t interrupt_stat;
    uint32_t interrupt_mask;
    uint32_t cache_control;
  } io;
  Buffer parallel_port_buffer;//[64*1024];
  Buffer ram_buffer;//[0x200000];
  Buffer bios_buffer;//[0x80000];
  Buffer scratchpad;
  RootCounter rootcounter_[4];
  Dma dma;

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

};

}
}


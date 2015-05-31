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

struct DmaChannel{
 uint32_t madr;
 uint32_t bcr;
 uint32_t chcr;
 bool enable;
};

class Dma : public Component {
 public:
  Dma();
  ~Dma();
  int Initialize();
  void SetInterrupt(int channel);
  void Tick();
  uint32_t Read(uint32_t address);
  void Write(uint32_t address,uint32_t data);
  DmaChannel& channel(int i) { return channels[i]; }
 private:
  DmaChannel channels[7];
  union {
    struct {
      uint32_t unused;
    };
    uint32_t raw;
  } dma_enable;
  union {
    struct {
      uint32_t fast_dma0:1;
      uint32_t fast_dma1:1;
      uint32_t fast_dma2:1;
      uint32_t fast_dma3:1;
      uint32_t fast_dma4:1;
      uint32_t fast_dma5:1;
      uint32_t fast_dma6:1;
      uint32_t unused1:9;
      uint32_t enable_dma0_interrupt:1;
      uint32_t enable_dma1_interrupt:1;
      uint32_t enable_dma2_interrupt:1;
      uint32_t enable_dma3_interrupt:1;
      uint32_t enable_dma4_interrupt:1;
      uint32_t enable_dma5_interrupt:1;
      uint32_t enable_dma6_interrupt:1;
      uint32_t unused2:1;
      uint32_t acknowledge_dma0_interrupt:1;
      uint32_t acknowledge_dma1_interrupt:1;
      uint32_t acknowledge_dma2_interrupt:1;
      uint32_t acknowledge_dma3_interrupt:1;
      uint32_t acknowledge_dma4_interrupt:1;
      uint32_t acknowledge_dma5_interrupt:1;
      uint32_t acknowledge_dma6_interrupt:1;
      uint32_t unused3:1;
    };
    uint32_t raw;
  } interrupt_control;
  void Dma2();
  void Dma6();
};

}
}

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
#ifndef EMULATION_PSX_ROOT_COUNTER_H
#define EMULATION_PSX_ROOT_COUNTER_H

namespace emulation {
namespace psx {

class RootCounter {
 public:
  uint32_t counter;
  uint32_t target;
  union {
    struct {
      unsigned en:1;
      unsigned _unused1:2;
      unsigned tar:1;
      unsigned irq1:1;
      unsigned _unused2:1;
      unsigned irq2:1;
      unsigned _unused3:1;
      unsigned clc:1;
      unsigned div:1;
      unsigned _unused4:21;
    };
    uint32_t raw;
  }mode;

  uint32_t ReadCounter() {
    return counter;
  }

  uint32_t ReadMode() {
    return this->mode.raw;
  }

  uint32_t ReadTarget() {
    return target;
  }

  void WriteMode(uint32_t mode) {
    this->mode.raw = mode;
  }

  void WriteTarget(uint32_t target) {
    this->target = target & 0xFFFF;
  }

  bool Tick(uint32_t cycles) {
    if (mode.en == 0) {
      counter += cycles;
      auto limit = mode.tar == 0? 0xffff:target;
      if (counter >= limit) {
        counter = 0;
        return true;
      }
    }
    return false;
  }

};

}
}

#endif
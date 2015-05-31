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

class RootCounter {
 public:
  uint32_t counter;
  uint32_t target;
  union {
    struct {
      uint32_t en:1;
      uint32_t syncmode:2;
      uint32_t resetmode:1;
      uint32_t irq_target:1;
      uint32_t irq_0xffff:1;
      uint32_t irqrepeat:1;
      uint32_t irqpulse:1;
      uint32_t clcsrc:2;
      uint32_t intreq:1;
      uint32_t reached_target:1;
      uint32_t reached_0xffff:1;
      uint32_t _unknown:3;
      uint32_t _garbage:16;
    };
    uint32_t raw;
  }mode;

  uint32_t ReadCounter() {
    return counter;
  }

  uint32_t ReadMode() {
    auto result = this->mode.raw;
    mode.reached_0xffff = 0;
    mode.reached_target = 0;
    return result; 
  }

  uint32_t ReadTarget() {
    return target;
  }

  void WriteCounter(uint32_t counter) {
    this->counter = counter & 0xFFFF;
  }

  void WriteMode(uint32_t mode) {
    this->mode.raw = mode;
    this->counter = 0;
  }

  void WriteTarget(uint32_t target) {
    this->target = target & 0xFFFF;
  }

  bool Tick(uint32_t cycles) {
    if (mode.en == 0) {
      counter += cycles;
      auto limit = mode.resetmode == 0? 0xffff:target;
      if (!mode.reached_0xffff) mode.reached_0xffff = counter == 0xffff;
      if (!mode.reached_target) mode.reached_target = counter == target;
      if (counter >= limit) {
        counter = 0;
        return true;
      }
    } else {
      //int a = 1;
    }
    return false;
  }

};

}
}


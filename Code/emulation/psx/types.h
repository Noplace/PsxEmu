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
#ifndef EMULATION_PSX_TYPES_H
#define EMULATION_PSX_TYPES_H

namespace emulation {
namespace psx {

class Component;
class Cpu;
class Gpu;
class Spu;
class Gte;
class Pio;
class IOInterface;
class Kernel;
class System;
class DebugAssist;

enum Exceptions {  kTLBMissException , kOtherException ,  kResetException };

enum ExceptionCodes { 
  kExceptionCodeInt      = 0  ,
  kExceptionCodeMod      = 1  ,
  kExceptionCodeTLBL     = 2  ,
  kExceptionCodeTLBS     = 3  ,
  kExceptionCodeAdEL     = 4  ,
  kExceptionCodeAdES     = 5  ,
  kExceptionCodeIBE      = 6  ,
  kExceptionCodeDBE      = 7  ,
  kExceptionCodeSyscall  = 8  ,
  kExceptionCodeBp       = 9  ,
  kExceptionCodeRI       = 10 ,
  kExceptionCodeCpU      = 11 ,
  kExceptionCodeOv       = 12 
};

enum InterruptCodes { 
  kInterruptVSYNC =  0x0001,
  kInterruptGPU   =  0x0002,
  kInterruptCDROM =  0x0004,
  kInterruptDMA   =  0x0008,
  kInterruptCNT0  =  0x0010,
  kInterruptCNT1  =  0x0020,
  kInterruptCNT2  =  0x0040,
  kInterruptSIO0  =  0x0080,
  kInterruptSIO1  =  0x0100,
  kInterruptSPU   =  0x0200,
  kInterruptPIO   =  0x0400
};

struct Buffer {
  uint8_t* u8;
  uint16_t* u16;
  uint32_t* u32;
  void Alloc(size_t size_bytes) {
    u8 = new uint8_t[size_bytes];
    u32 = (uint32_t*)u8;
    u16 = (uint16_t*)u8;
  }
  void Dealloc() {
    delete [] u8;
    u8 = nullptr;
    u16 = nullptr;
    u32 = nullptr;
  }
};

}
}
#endif
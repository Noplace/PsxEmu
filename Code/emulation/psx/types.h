/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : types.h
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
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
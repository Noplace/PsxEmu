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

struct CpuContext {
  
  union {
    uint32_t reg[32];
    struct {
      uint32_t zero;
      uint32_t at;
      uint32_t v0;
      uint32_t v1;
      uint32_t a0;
      uint32_t a1;
      uint32_t a2;
      uint32_t a3;
      uint32_t t0;
      uint32_t t1;
      uint32_t t2;
      uint32_t t3;
      uint32_t t4;
      uint32_t t5;
      uint32_t t6;
      uint32_t t7;
      uint32_t s0;
      uint32_t s1;
      uint32_t s2;
      uint32_t s3;
      uint32_t s4;
      uint32_t s5;
      uint32_t s6;
      uint32_t s7;
      uint32_t t8;
      uint32_t t9;
      uint32_t k0;
      uint32_t k1;
      uint32_t gp;
      uint32_t sp;
      union {
        uint32_t fp;
        uint32_t s8;
      };
      uint32_t ra;
    };
  } gp; //General Purpose

  union {
    uint32_t reg[32];
    struct {
      uint32_t _unused0;
      uint32_t _unused1;
      uint32_t BusCtrl;
      uint32_t Config;
      uint32_t _unused4;
      uint32_t _unused5;
      uint32_t _unused6;
      uint32_t _unused7;
      uint32_t BadVaddr;
      uint32_t Count;
      uint32_t PortSize;
      uint32_t Compare;
      union {
        struct {
          uint32_t IEc:1;
          uint32_t KUc:1;
          uint32_t IEp:1;
          uint32_t KUp:1;
          uint32_t IEo:1;
          uint32_t KUo:1;
          uint32_t unused1:2;
          uint32_t IM:8;
          uint32_t IsC:1;
          uint32_t SwC:1;
          uint32_t PZ:1;
          uint32_t CM:1;
          uint32_t PE:1;
          uint32_t TS:1;
          uint32_t BEV:1;
          uint32_t unused2:2;
          uint32_t RE:1;
          uint32_t unused3:2;
          uint32_t CU0:1;
          uint32_t CU1:1;
          uint32_t CU2:1;
          uint32_t CU3:1;
        };
        uint32_t raw;
      } SR;
      uint32_t Cause;
      uint32_t EPC;
      uint32_t PRId;
      uint32_t _unused16;
      uint32_t _unused17;
      uint32_t _unused18;
      uint32_t _unused19;
      uint32_t _unused20;
      uint32_t _unused21;
      uint32_t _unused22;
      uint32_t _unused23;
      uint32_t _unused24;
      uint32_t _unused25;
      uint32_t _unused26;
      uint32_t _unused27;
      uint32_t _unused28;
      uint32_t _unused29;
      uint32_t _unused30;
      uint32_t _unused31;
    };
  } ctrl; //Control Processor 0 Standard

  uint32_t cpr2[32];
  uint64_t cycles;
  uint32_t prev_pc;
  uint32_t pc;
  uint32_t code;
  uint32_t low,high;
  uint8_t  current_cycles;
  bool branch_flag;

  CpuContext() : pc(0),code(0),low(0),high(0),branch_flag(false) {
    memset(&gp,0,sizeof(gp));
    memset(&ctrl.reg,0,sizeof(ctrl.reg));
    memset(&cpr2,0,sizeof(cpr2));
  }

  int32_t immediate_32bit_sign_extended() { return (int32_t)((int16_t)immediate()); }
  uint16_t immediate() { return (uint16_t)(code & 0xFFFF); }
  
  uint8_t opcode(){ return (code >> 26); }

  uint8_t rd() { return ((code >> 11) & 0x1F); }
  uint8_t rt() { return ((code >> 16) & 0x1F); }
  uint8_t rs() { return ((code >> 21) & 0x1F); }
  uint8_t fu() { return code&0x3f; }
  uint8_t sa() { return ((code>>6) & 0x1F); }
  uint32_t target() { return (code & 0x3FFFFFF); }

  //uint32_t& gpr_rd() { return gp.reg[rd()]; }
  //uint32_t& gpr_rt() { return gp.reg[rt()]; }
  //uint32_t& gpr_rs() { return gp.reg[rs()]; }

  
  
};

}
}


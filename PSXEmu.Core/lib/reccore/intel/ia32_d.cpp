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
#include "../reccore.h"


namespace reccore {
namespace intel {

void IA32::DAA() {
  e->emit8(0x27);
}

void IA32::DAS() {
  e->emit8(0x2F);
}

void IA32::DEC(Reg32 reg) {
  //e->emit8(
  //emitREX(e);
  e->emit8(0xFF);
  e->emit8(_ModRM(3,1,reg));
}

void IA32::DEC(Reg64 reg) {
  e->emit8(PREFIX::REX(1,0,0,reg>>3));
  e->emit8(0xFF);
  e->emit8(_ModRM(3,1,reg));
}

void IA32::DEC(EA mem) {
  mem.reg = 1;
  if (mem.mode == 1)
  emitREX(e,mem);
  e->emit8(0xFF);
  emitModRMSIB(e,mem);
}

void IA32::DIV(Reg8 reg) {
  e->emit8(0xF6);
  e->emit8(_ModRM(3,6,reg));
}

void IA32::DIV(Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0xF7);
  e->emit8(_ModRM(3,6,reg));
}

void IA32::DIV(Reg32 reg) {
  e->emit8(0xF7);
  e->emit8(_ModRM(3,6,reg));
}

void IA32::DIV(Reg64 reg) {
  EA rm(reg);
  emitREX(e,rm);
  e->emit8(0xF7);
  rm.reg = 6;
  emitModRMSIB(e,rm);
}



void IA32::DIV(EA rm, IntSize size) {
  switch (size) {
    case Int8:
      e->emit8(0xF6);
      rm.reg = 6;
      emitModRMSIB(e,rm);
    break;
    case Int16:
      e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
      e->emit8(0xF7);
      rm.reg = 6;
      emitModRMSIB(e,rm);
    break;
    case Int32:
      e->emit8(0xF7);
      rm.reg = 6;
      emitModRMSIB(e,rm);
    break;
    case Int64:
      emitREX(e,rm);
      e->emit8(0xF7);
      rm.reg = 6;
      emitModRMSIB(e,rm);
    break;
  }

}



void IA32::DIVPD(Reg_xmm a, EA b) {
  e->emit8(0x66);
  e->emit8(0x05);
  e->emit8(0x5E);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VDIVPD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,PREFIX::kPP66));
  e->emit8(0x5E);
  emitModRMSIB(e,c);
}

void IA32::VDIVPD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,PREFIX::kPP66));
  e->emit8(0x5E);
  emitModRMSIB(e,c);
}


void IA32::DIVPS(Reg_xmm a, EA b) {
  e->emit8(0x0F);
  e->emit8(0x5E);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VDIVPS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,0));
  e->emit8(0x5E);
  emitModRMSIB(e,c);
}

void IA32::VDIVPS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,0));
  e->emit8(0x5E);
  emitModRMSIB(e,c);
}

void IA32::DIVSD(Reg_xmm a, EA b) {
  e->emit8(0xF2);
  e->emit8(0x05);
  e->emit8(0x5E);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VDIVSD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,3));
  e->emit8(0x5E);
  emitModRMSIB(e,c);
}

void IA32::DIVSS(Reg_xmm a, EA b) {
  e->emit8(0xF3);
  e->emit8(0x05);
  e->emit8(0x5E);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VDIVSS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,2));
  e->emit8(0x5E);
  emitModRMSIB(e,c);
}


void IA32::DPPD(Reg_xmm a, EA b, uint8_t imm8) {
  e->emit8(0x66);
  e->emit8(0x0F);
  e->emit8(0x3A);
  e->emit8(0x41);
  b.reg = a;
  emitModRMSIB(e,b);
  e->emit8(imm8);
}

void IA32::VDPPD(Reg_xmm a, Reg_xmm b, EA c, uint8_t imm8) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,0,PREFIX::k0F3A));
  e->emit8(PREFIX::VEX3_2(0,b,0,PREFIX::kPP66));
  e->emit8(0x41);
  emitModRMSIB(e,c);
  e->emit8(imm8);
}


}
}
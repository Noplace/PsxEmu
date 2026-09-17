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


void IA32::LAHF() {
  e->emit8(0x9F);
}

void IA32::LAR(Reg16 reg, EA rm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0x0F);
  e->emit8(0x02);
  rm.reg = reg;
  emitModRMSIB(e,rm);
}

void IA32::LAR(Reg32 reg, EA rm) {
  e->emit8(0x0F);
  e->emit8(0x02);
  rm.reg = reg;
  emitModRMSIB(e,rm);
}

void IA32::LAR(Reg64 reg, EA rm) {
  emitREX(e,rm);
  e->emit8(0x0F);
  e->emit8(0x02);
  rm.reg = reg;
  emitModRMSIB(e,rm);
}

void IA32::LDDQU(Reg_xmm reg, EA mem) {
  mem.reg = reg;
  e->emit8(0xF2);
  e->emit8(0x0F);
  e->emit8(0xF0);

  emitModRMSIB(e,mem);
}

void IA32::VLDDQU(Reg_xmm reg, EA mem) {
  mem.reg = reg;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(reg>>3,0,0,3));
  e->emit8(0xF0);
  emitModRMSIB(e,mem);
}

void IA32::VLDDQU(Reg_ymm reg, EA mem) {
  mem.reg = reg;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(reg>>3,0,1,3));
  e->emit8(0xF0);
  emitModRMSIB(e,mem);
}

void IA32::LEA(Reg16 r16, EA mem) {
  mem.reg = r16;
  e->emit8(0x8D);
  emitModRMSIB(e,mem);
}

void IA32::LEA(Reg32 r32, EA mem) {
  mem.reg = r32;
  e->emit8(0x8D);
  emitModRMSIB(e,mem);
}


void IA32::LEA(Reg64 r64, EA mem) {
  mem.reg = r64;  
  emitREX(e,mem);
  e->emit8(0x8D);
  emitModRMSIB(e,mem);
}

void IA32::LEA(Reg32 r32, void* ptr) {
  LEA(r32,EA("[disp32]",int32_t(ptr)));
}

void IA32::LEAVE() {
  e->emit8(0xC9);
}

void IA32::LOOP(int8_t rel8) {
  e->emit8(0xE2);
  e->emit8(rel8);
}

void IA32::LOOPE(int8_t rel8) {
  e->emit8(0xE1);
  e->emit8(rel8);
}

void IA32::LOOPNE(int8_t rel8) {
  e->emit8(0xE0);
  e->emit8(rel8);
}


}
}
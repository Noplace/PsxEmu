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


void IA32::OR(Reg8 dest, uint8_t imm) {
  if (dest == AL) {
    e->emit8(0x0C);
    e->emit32(imm);
  } else {
    e->emit8(0x80);
    e->emit8(_ModRM(3,1,dest));
    e->emit32(imm);
  }
}

void IA32::OR(Reg16 dest, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  if (dest == AX) {
    e->emit8(0x0D);
    e->emit16(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,1,dest));
    e->emit16(imm);
  }
}

void IA32::OR(Reg32 dest, uint32_t imm) {
  if (dest == EAX) {
    e->emit8(0x0D);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,1,dest));
    e->emit32(imm);
  }
}

void IA32::OR(Reg64 dest, uint32_t imm) {
  e->emit8(PREFIX::REX(1,dest>>3,0,0));
  if (dest == RAX) {
    e->emit8(0x0D);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,1,dest));
    e->emit32(imm);
  }
}

void IA32::OR(EA rm, uint8_t imm) {
  e->emit8(0x80);
  rm.reg = 1;
  emitModRMSIB(e,rm);
  e->emit8(imm);
}

void IA32::OR(EA rm, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0x81);
  rm.reg = 1;
  emitModRMSIB(e,rm);
  e->emit16(imm);
}

void IA32::OR(EA rm, uint32_t imm) {
  e->emit8(0x81);
  rm.reg = 1;
  emitModRMSIB(e,rm);
  e->emit32(imm);
}

void IA32::OR(EA rm, Reg8 reg) {
  rm.reg = reg;
  e->emit8(0x08);
  emitModRMSIB(e,rm);
}

void IA32::OR(EA rm, Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x09);
  emitModRMSIB(e,rm);
}

void IA32::OR(EA rm, Reg32 reg) {
  rm.reg = reg;
  e->emit8(0x09);
  emitModRMSIB(e,rm);
}

void IA32::OR(EA rm, Reg64 reg) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x09);
  emitModRMSIB(e,rm);
}

void IA32::OR(Reg8 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x0A);
  emitModRMSIB(e,rm);
}

void IA32::OR(Reg16 reg, EA rm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x0B);
  emitModRMSIB(e,rm);
}

void IA32::OR(Reg32 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x0B);
  emitModRMSIB(e,rm);
}

void IA32::OR(Reg64 reg, EA rm) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x0B);
  emitModRMSIB(e,rm);
}


}
}
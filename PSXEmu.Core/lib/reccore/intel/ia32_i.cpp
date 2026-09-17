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


void IA32::IDIV(Reg8 reg) {
  e->emit8(0xF6);
  e->emit8(_ModRM(3,7,reg));
}

void IA32::IDIV(Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0xF7);
  e->emit8(_ModRM(3,7,reg));
}

void IA32::IDIV(Reg32 reg) {
  e->emit8(0xF7);
  e->emit8(_ModRM(3,7,reg));
}

void IA32::IDIV(Reg64 reg) {
  EA rm(reg);
  emitREX(e,rm);
  e->emit8(0xF7);
  rm.reg = 7;
  emitModRMSIB(e,rm);
}



void IA32::IDIV(EA rm, IntSize size) {
  switch (size) {
    case Int8:
      e->emit8(0xF6);
      rm.reg = 7;
      emitModRMSIB(e,rm);
    break;
    case Int16:
      e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
      e->emit8(0xF7);
      rm.reg = 7;
      emitModRMSIB(e,rm);
    break;
    case Int32:
      e->emit8(0xF7);
      rm.reg = 7;
      emitModRMSIB(e,rm);
    break;
    case Int64:
      emitREX(e,rm);
      e->emit8(0xF7);
      rm.reg = 7;
      emitModRMSIB(e,rm);
    break;
  }
}

void IA32::INC(Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0xFF);
  e->emit8(_ModRM(3,0,reg));
}

void IA32::INC(Reg32 reg) {
  e->emit8(0xFF);
  e->emit8(_ModRM(3,0,reg));
}

void IA32::INC(Reg64 reg) {
  e->emit8(PREFIX::REX(1,0,0,reg>>3));
  e->emit8(0xFF);
  e->emit8(_ModRM(3,0,reg));
}

void IA32::INC(EA mem) {
  mem.reg = 0;
  if (mem.mode == 1)
    emitREX(e,mem);
  e->emit8(0xFF);
  emitModRMSIB(e,mem);
}



}
}
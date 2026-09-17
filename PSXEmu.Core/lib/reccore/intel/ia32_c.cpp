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



void IA32::CALL(uint16_t rel16) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0xE8);
  e->emit16(rel16);
}

void IA32::CALL(uint32_t rel32) {
  
  e->emit8(0xE8);
  e->emit32(rel32);
}

void IA32::CALL(void* ptr) {

  auto rel32 = intptr_t(ptr) - (intptr_t(e->block()->address)+e->block()->cursor+5);
  CALL(uint32_t(rel32));
  /*MOV(EAX,uint32_t(ptr));
  e->emit8(0xFF);
  e->emit8(ModRM(3,2,EAX));*/
}


void IA32::CMP(Reg8 reg, uint8_t imm8) {
  if (reg == AL) {
    e->emit8(0x3C);
    e->emit8(imm8);
  } else {
    e->emit8(0x80);
    e->emit8(_ModRM(0x3,7,reg));
    e->emit8(imm8);
  }
}

void IA32::CMP(Reg32 reg, uint32_t imm32) {
  if (reg == EAX) {
    e->emit8(0x3D);
    e->emit32(imm32);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(0x3,7,reg));
    e->emit32(imm32);
  }
}


}
}
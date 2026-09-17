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

void IA32::EMMS() {
  e->emit8(0x0F);
  e->emit8(0x77);
}

void IA32::ENTER(uint16_t imm16, uint8_t imm8) {
  e->emit8(0xC8);
  e->emit16(imm16);
  e->emit8(imm8);
}

void IA32::EXTRACTPS(EA rm32, Reg_xmm xmm, uint8_t imm8) {
  rm32.reg = xmm;
  e->emit8(0x66);
  e->emit16(0x0F);
  e->emit16(0x3A);
  e->emit16(0x17);
  emitModRMSIB(e,rm32);
  e->emit8(imm8);
}

void IA32::VEXTRACTPS(EA rm32, Reg_xmm xmm, uint8_t imm8) {
  rm32.reg = xmm;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(xmm>>3,0,0,PREFIX::k0F3A));
  e->emit8(PREFIX::VEX3_2(0,0,0,PREFIX::kPP66));
  e->emit16(0x17);
  emitModRMSIB(e,rm32);
  e->emit8(imm8);
}

}
}
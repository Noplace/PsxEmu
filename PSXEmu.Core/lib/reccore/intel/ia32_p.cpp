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

void IA32::PAUSE() {
  e->emit8(0xF3);
  e->emit8(0x90);
}


void IA32::PCLMULQDQ(Reg_xmm a, EA b, uint8_t imm8) {
  e->emit8(0x66);
  e->emit8(0x0F);
  e->emit8(0x3A);
  e->emit8(0x44);
  b.reg = a;
  emitModRMSIB(e,b);
  e->emit8(imm8);
}

void IA32::VPCLMULQDQ(Reg_xmm a, Reg_xmm b, EA c, uint8_t imm8) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F3A));
  e->emit8(PREFIX::VEX3_2(0,b,0,PREFIX::kPP66));
  e->emit8(0x44);
  emitModRMSIB(e,c);
  e->emit8(imm8);
}

void IA32::POP(Reg32 reg) {
  e->emit8(0x58|(reg&0x7));
}

void IA32::PUSH(EA mem) {
  e->emit8(0xFF);
  mem.reg = 6;
  emitModRMSIB(e,mem);
}

void IA32::POPA() {
	e->emit8(0x61);
}

void IA32::POPAD() {
	e->emit8(0x61);
}

void IA32::PUSH(Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
	e->emit8(0x50+reg);
}

void IA32::PUSH(Reg32 reg) {
	e->emit8(0x50+reg);
}

void IA32::PUSH(Reg64 reg) {
	e->emit8(0x50+reg);
}

void IA32::PUSH(uint8_t imm) {
  e->emit8(0x6A);
  e->emit8(imm);
}

void IA32::PUSH(uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0x68);
  e->emit16(imm);
}
  
void IA32::PUSH(uint32_t imm) {
  e->emit8(0x68);
  e->emit32(imm);
}

void IA32::PUSHA() {
  e->emit8(0x60);
}

void IA32::PUSHAD() {
  e->emit8(0x60);
}

void IA32::PUSHF() {
  e->emit8(0x9C);
}

void IA32::PUSHFD() {
  e->emit8(0x9C);
}

void IA32::PUSHFQ() {
  e->emit8(0x9C);
}

}
}
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


void IA32::AAA()
{
	e->emit8(0x37);
}

void IA32::AAD()
{
	e->emit8(0xD5);
	e->emit8(0x0A);
}

void IA32::AAM()
{
	e->emit8(0xD4);
	e->emit8(0x0A);
}

void IA32::AAS()
{
	e->emit8(0x3F);
}

void IA32::ADC(Reg8 dest, uint8_t imm) {
  if (dest == AL) {
    e->emit8(0x14);
    e->emit32(imm);
  } else {
    e->emit8(0x80);
    e->emit8(_ModRM(3,2,dest));
    e->emit32(imm);
  }
}

void IA32::ADC(Reg16 dest, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  if (dest == AX) {
    e->emit8(0x15);
    e->emit16(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,2,dest));
    e->emit16(imm);
  }
}

void IA32::ADC(Reg32 dest, uint32_t imm) {
  if (dest == EAX) {
    e->emit8(0x15);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,2,dest));
    e->emit32(imm);
  }
}

void IA32::ADC(Reg64 dest, uint32_t imm) {
  e->emit8(PREFIX::REX(1,dest>>3,0,0));
  if (dest == RAX) {
    e->emit8(0x15);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,2,dest));
    e->emit32(imm);
  }
}

void IA32::ADC(EA rm, uint8_t imm) {
  e->emit8(0x80);
  rm.reg = 2;
  emitModRMSIB(e,rm);
  e->emit8(imm);
}

void IA32::ADC(EA rm, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0x81);
  rm.reg = 2;
  emitModRMSIB(e,rm);
  e->emit16(imm);
}

void IA32::ADC(EA rm, uint32_t imm) {
  e->emit8(0x81);
  rm.reg = 2;
  emitModRMSIB(e,rm);
  e->emit32(imm);
}

void IA32::ADC(EA rm, Reg8 reg) {
  rm.reg = reg;
  e->emit8(0x10);
  emitModRMSIB(e,rm);
}

void IA32::ADC(EA rm, Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x11);
  emitModRMSIB(e,rm);
}

void IA32::ADC(EA rm, Reg32 reg) {
  rm.reg = reg;
  e->emit8(0x11);
  emitModRMSIB(e,rm);
}

void IA32::ADC(EA rm, Reg64 reg) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x11);
  emitModRMSIB(e,rm);
}

void IA32::ADC(Reg8 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x12);
  emitModRMSIB(e,rm);
}

void IA32::ADC(Reg16 reg, EA rm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x03);
  emitModRMSIB(e,rm);
}

void IA32::ADC(Reg32 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x13);
  emitModRMSIB(e,rm);
}

void IA32::ADC(Reg64 reg, EA rm) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x13);
  emitModRMSIB(e,rm);
}

void IA32::ADD(Reg8 dest, uint8_t imm) {
  if (dest == AL) {
    e->emit8(0x05);
    e->emit32(imm);
  } else {
    e->emit8(0x80);
    e->emit8(_ModRM(3,0,dest));
    e->emit32(imm);
  }
}

void IA32::ADD(Reg16 dest, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  if (dest == AX) {
    e->emit8(0x05);
    e->emit16(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,0,dest));
    e->emit16(imm);
  }
}

void IA32::ADD(Reg32 dest, uint32_t imm) {
  if (dest == EAX) {
    e->emit8(0x05);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,0,dest));
    e->emit32(imm);
  }
}

void IA32::ADD(Reg64 dest, uint32_t imm) {
  e->emit8(PREFIX::REX(1,dest>>3,0,0));
  if (dest == RAX) {
    e->emit8(0x05);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,0,dest));
    e->emit32(imm);
  }
}

void IA32::ADD(EA rm, uint8_t imm) {
  e->emit8(0x80);
  emitModRMSIB(e,rm);
  e->emit8(imm);
}

void IA32::ADD(EA rm, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0x81);
  emitModRMSIB(e,rm);
  e->emit16(imm);
}

void IA32::ADD(EA rm, uint32_t imm) {
  e->emit8(0x81);
  emitModRMSIB(e,rm);
  e->emit32(imm);
}

void IA32::ADD(EA rm, Reg8 reg) {
  rm.reg = reg;
  e->emit8(0x00);
  emitModRMSIB(e,rm);
}

void IA32::ADD(EA rm, Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x01);
  emitModRMSIB(e,rm);
}

void IA32::ADD(EA rm, Reg32 reg) {
  rm.reg = reg;
  e->emit8(0x01);
  emitModRMSIB(e,rm);
}

void IA32::ADD(EA rm, Reg64 reg) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x01);
  emitModRMSIB(e,rm);
}

void IA32::ADD(Reg8 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x02);
  emitModRMSIB(e,rm);
}

void IA32::ADD(Reg16 reg, EA rm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x03);
  emitModRMSIB(e,rm);
}

void IA32::ADD(Reg32 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x03);
  emitModRMSIB(e,rm);
}

void IA32::ADD(Reg64 reg, EA rm) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x03);
  emitModRMSIB(e,rm);
}

void IA32::ADDPD(Reg_xmm a, EA b) {
  e->emit8(0x66);
  e->emit8(0x05);
  e->emit8(0x58);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VADDPD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,1));
  e->emit8(0x58);
  emitModRMSIB(e,c);
}

void IA32::VADDPD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,1));
  e->emit8(0x58);
  emitModRMSIB(e,c);
}

void IA32::ADDPS(Reg_xmm a, EA b) {
  e->emit8(0x05);
  e->emit8(0x58);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VADDPS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,0));
  e->emit8(0x58);
  emitModRMSIB(e,c);
}

void IA32::VADDPS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,0));
  e->emit8(0x58);
  emitModRMSIB(e,c);
}

void IA32::ADDSD(Reg_xmm a, EA b) {
  e->emit8(0xF2);
  e->emit8(0x05);
  e->emit8(0x58);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VADDSD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,3));
  e->emit8(0x58);
  emitModRMSIB(e,c);
}

void IA32::ADDSS(Reg_xmm a, EA b) {
  e->emit8(0xF3);
  e->emit8(0x05);
  e->emit8(0x58);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VADDSS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,2));
  e->emit8(0x58);
  emitModRMSIB(e,c);
}

void IA32::ADDSUBPD(Reg_xmm a, EA b) {
  e->emit8(0x66);
  e->emit8(0x0F);
  e->emit8(0xD0);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VADDSUBPD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,1));
  e->emit8(0xD0);
  emitModRMSIB(e,c);
}

void IA32::VADDSUBPD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,1));
  e->emit8(0xD0);
  emitModRMSIB(e,c);
}


void IA32::AND(Reg8 dest, uint8_t imm) {
  if (dest == AL) {
    e->emit8(0x24);
    e->emit8(imm);
  } else {
    e->emit8(0x80);
    e->emit8(_ModRM(3,4,dest));
    e->emit8(imm);
  }
}

void IA32::AND(Reg16 dest, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  if (dest == AX) {
    e->emit8(0x25);
    e->emit16(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,4,dest));
    e->emit16(imm);
  }
}

void IA32::AND(Reg32 dest, uint32_t imm) {
  if (dest == EAX) {
    e->emit8(0x25);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,4,dest));
    e->emit32(imm);
  }
}

void IA32::AND(Reg64 dest, uint32_t imm) {
  e->emit8(PREFIX::REX(1,dest>>3,0,0));
  if (dest == RAX) {
    e->emit8(0x25);
    e->emit32(imm);
  } else {
    e->emit8(0x81);
    e->emit8(_ModRM(3,4,dest));
    e->emit32(imm);
  }
}

void IA32::AND(EA rm, uint8_t imm) {
  e->emit8(0x80);
  rm.reg = 4;
  emitModRMSIB(e,rm);
  e->emit8(imm);
}

void IA32::AND(EA rm, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0x81);
  rm.reg = 4;
  emitModRMSIB(e,rm);
  e->emit16(imm);
}

void IA32::AND(EA rm, uint32_t imm) {
  e->emit8(0x81);
  rm.reg = 4;
  emitModRMSIB(e,rm);
  e->emit32(imm);
}

void IA32::AND(EA rm, Reg8 reg) {
  rm.reg = reg;
  e->emit8(0x20);
  emitModRMSIB(e,rm);
}

void IA32::AND(EA rm, Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x21);
  emitModRMSIB(e,rm);
}

void IA32::AND(EA rm, Reg32 reg) {
  rm.reg = reg;
  e->emit8(0x21);
  emitModRMSIB(e,rm);
}

void IA32::AND(EA rm, Reg64 reg) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x21);
  emitModRMSIB(e,rm);
}

void IA32::AND(Reg8 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x22);
  emitModRMSIB(e,rm);
}

void IA32::AND(Reg16 reg, EA rm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm.reg = reg;
  e->emit8(0x23);
  emitModRMSIB(e,rm);
}

void IA32::AND(Reg32 reg, EA rm) {
  rm.reg = reg;
  e->emit8(0x23);
  emitModRMSIB(e,rm);
}

void IA32::AND(Reg64 reg, EA rm) {
  e->emit8(PREFIX::REX(1,reg>>3,0,0));
  rm.reg = reg;
  e->emit8(0x23);
  emitModRMSIB(e,rm);
}



}
}
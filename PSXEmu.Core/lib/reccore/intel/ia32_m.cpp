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


void IA32::MAXPD(Reg_xmm a, EA b) {
  e->emit8(0x66);
  e->emit8(0x05);
  e->emit8(0x5F);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMAXPD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,1));
  e->emit8(0x5F);
  emitModRMSIB(e,c);
}

void IA32::VMAXPD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,1));
  e->emit8(0x5F);
  emitModRMSIB(e,c);
}

void IA32::MAXPS(Reg_xmm a, EA b) {
  e->emit8(0x05);
  e->emit8(0x5F);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMAXPS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,0));
  e->emit8(0x5F);
  emitModRMSIB(e,c);
}

void IA32::VMAXPS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,0));
  e->emit8(0x5F);
  emitModRMSIB(e,c);
}

void IA32::MAXSD(Reg_xmm a, EA b) {
  e->emit8(0xF2);
  e->emit8(0x05);
  e->emit8(0x5F);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMAXSD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,3));
  e->emit8(0x5F);
  emitModRMSIB(e,c);
}

void IA32::MAXSS(Reg_xmm a, EA b) {
  e->emit8(0xF3);
  e->emit8(0x05);
  e->emit8(0x5F);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMAXSS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,2));
  e->emit8(0x5F);
  emitModRMSIB(e,c);
}

void IA32::MFENCE() {
  e->emit8(0x0F);
  e->emit8(0xAE);
  e->emit8(_ModRM(3,6,0));
}


void IA32::MINPD(Reg_xmm a, EA b) {
  e->emit8(0x66);
  e->emit8(0x05);
  e->emit8(0x5D);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMINPD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,1));
  e->emit8(0x5D);
  emitModRMSIB(e,c);
}

void IA32::VMINPD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,1));
  e->emit8(0x5D);
  emitModRMSIB(e,c);
}

void IA32::MINPS(Reg_xmm a, EA b) {
  e->emit8(0x05);
  e->emit8(0x5D);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMINPS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,0));
  e->emit8(0x5D);
  emitModRMSIB(e,c);
}

void IA32::VMINPS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,0));
  e->emit8(0x5D);
  emitModRMSIB(e,c);
}

void IA32::MINSD(Reg_xmm a, EA b) {
  e->emit8(0xF2);
  e->emit8(0x05);
  e->emit8(0x5D);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMINSD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,3));
  e->emit8(0x5D);
  emitModRMSIB(e,c);
}

void IA32::MINSS(Reg_xmm a, EA b) {
  e->emit8(0xF3);
  e->emit8(0x05);
  e->emit8(0x5D);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMINSS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,2));
  e->emit8(0x5D);
  emitModRMSIB(e,c);
}

void IA32::MONITOR() {
  e->emit8(0x0F);
  e->emit8(0x01);
  e->emit8(0xC8);
}

void IA32::MOV(EA m8, Reg8 src) {
	e->emit8(0x88);
  m8.reg = src;
  emitModRMSIB(e,m8);
}

void IA32::MOV(EA m16, Reg16 src) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
	e->emit8(0x89);
  m16.reg = src;
  emitModRMSIB(e,m16);
}

void IA32::MOV(EA m32, Reg32 src) {
	e->emit8(0x89);
  m32.reg = src;
  emitModRMSIB(e,m32);
}

void IA32::MOV(EA m64, Reg64 src) {
  m64.reg = src;
  emitREX(e,m64);
	e->emit8(0x89);
  emitModRMSIB(e,m64);
}

void IA32::MOV(Reg8 dest, EA m8) {
	e->emit8(0x8A);
  m8.reg = dest;
  emitModRMSIB(e,m8);
}

void IA32::MOV(Reg16 dest, EA m16) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
	e->emit8(0x8B);
  m16.reg = dest;
  emitModRMSIB(e,m16);
}

void IA32::MOV(Reg32 dest, EA m32) {
	e->emit8(0x8B);
  m32.reg = dest;
  emitModRMSIB(e,m32);
}

void IA32::MOV(Reg64 dest, EA m64) {
  m64.reg = dest;	
  emitREX(e,m64);
  e->emit8(0x8B);
  emitModRMSIB(e,m64);
}


void IA32::MOV(Reg8 dest, Reg8 src) {
	e->emit8(0x8A);
	e->emit8(_ModRM(0x3,dest,src));
}

void IA32::MOV(Reg16 dest, Reg16 src) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
	e->emit8(0x8B);
	e->emit8(_ModRM(0x3,dest,src));
}

void IA32::MOV(Reg32 dest, Reg32 src) {
	e->emit8(0x8B);
	e->emit8(_ModRM(0x3,dest,src));
}

void IA32::MOV(Reg8 dest, uint8_t imm) {
	e->emit8(0xB0|(dest&0x7));
	e->emit8(imm);
}

void IA32::MOV(Reg16 dest, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
	e->emit8(0xB8|(dest&0x7));
	e->emit16(imm);
}

void IA32::MOV(Reg32 dest, uint32_t imm) {
	e->emit8(0xB8|(dest&0x7));
	e->emit32(imm);
}

void IA32::MOV(Reg64 dest, uint64_t imm) {
  e->emit8(PREFIX::REX(1,0,0,dest>>3));
	e->emit8(0xB8+(dest&0x7));
	e->emit64(imm);
}

void IA32::MOV(void* dest, Reg32 src) {
  MOV(EA("[disp32]",int32_t(dest)),src);
}

void IA32::MOV(Reg32 dest, void* src) {
//should we change this based on platform?
  MOV(dest,EA("[disp32]",int32_t(src)));
}

void IA32::MOV(Reg64 dest, void* src) {
  MOV(RSI,(uint64_t)src);
  MOV(dest,EA("[RSI]"));
}

void IA32::MOV(Reg64 dest, Reg64 src) {
  e->emit8(PREFIX::REX(1,dest>>3,0,src>>3));
	e->emit8(0x8B);
	e->emit8(_ModRM(0x3,dest,src));
}

void IA32::MOV(EA rm16, uint16_t imm) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  rm16.reg = 0;
	e->emit8(0xC7);
	emitModRMSIB(e,rm16);
  e->emit16(imm);
}

void IA32::MOV(EA rm, uint32_t imm) {
  rm.reg = 0;
  if (rm.mode==1)
    emitREX(e,rm);
	e->emit8(0xC7);
	emitModRMSIB(e,rm);
  e->emit32(imm);

}

void IA32::MOV(Reg32 r32, Reg_CR cr) {
  e->emit8(0x0F);
  e->emit8(0x20);
  e->emit8(_ModRM(3,cr,r32));
}

void IA32::MOV(Reg64 r64, Reg_CR cr) {
  if (cr!=CR8) {
    e->emit8(0x0F);
    e->emit8(0x20);
    e->emit8(_ModRM(3,cr,r64));
  } else {
    e->emit8(PREFIX::REX(1,r64>>3,0,1));
    e->emit8(0x0F);
    e->emit8(0x20);
    e->emit8(_ModRM(3,0,r64));
  }
}

void IA32::MOV(Reg_CR cr, Reg32 r32) {
  e->emit8(0x0F);
  e->emit8(0x22);
  e->emit8(_ModRM(3,cr,r32));
}

void IA32::MOV(Reg_CR cr, Reg64 r64) {
  if (cr != CR8) {
    e->emit8(0x0F);
    e->emit8(0x22);
    e->emit8(_ModRM(3,cr,r64));
  } else {
    e->emit8(PREFIX::REX(1,r64>>3,0,1));
    e->emit8(0x0F);
    e->emit8(0x22);
    e->emit8(_ModRM(3,0,r64));
  }
}

void IA32::MOV(Reg32 r32, Reg_DR dr) {
  e->emit8(0x0F);
  e->emit8(0x21);
  e->emit8(_ModRM(3,dr,r32));
}

void IA32::MOV(Reg64 r64, Reg_DR dr) {
  e->emit8(0x0F);
  e->emit8(0x21);
  e->emit8(_ModRM(3,dr,r64));
}

void IA32::MOV(Reg_DR dr, Reg32 r32) {
  e->emit8(0x0F);
  e->emit8(0x23);
  e->emit8(_ModRM(3,dr,r32));
}

void IA32::MOV(Reg_DR dr, Reg64 r64) {
  e->emit8(0x0F);
  e->emit8(0x23);
  e->emit8(_ModRM(3,dr,r64));
}

void IA32::MOVAPD(Reg_xmm a, EA b) {
  e->emit8(0x66);
  e->emit8(0x0F);
  e->emit8(0x28);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::MOVAPD(EA a, Reg_xmm b) {
  e->emit8(0x66);
  e->emit8(0x0F);
  e->emit8(0x29);
  a.reg = b;
  emitModRMSIB(e,a);
}

void IA32::VMOVAPD(Reg_xmm a, EA b) {
  b.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,0,0,1));
  e->emit8(0x28);
  emitModRMSIB(e,b);
}

void IA32::VMOVAPD(EA a, Reg_xmm b) {
  a.reg = b;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(b>>3,0,0,1));
  e->emit8(0x29);
  emitModRMSIB(e,a);
}



void IA32::VMOVAPD(Reg_ymm a, EA b) {
  b.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,0,1,1));
  e->emit8(0x28);
  emitModRMSIB(e,b);
}

void IA32::VMOVAPD(EA a, Reg_ymm b) {
  a.reg = b;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(b>>3,0,1,1));
  e->emit8(0x29);
  emitModRMSIB(e,a);
}


void IA32::MOVAPS(Reg_xmm a, EA b) {
  e->emit8(0x0F);
  e->emit8(0x28);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::MOVAPS(EA a, Reg_xmm b) {
  e->emit8(0x0F);
  e->emit8(0x29);
  a.reg = b;
  emitModRMSIB(e,a);
}

void IA32::VMOVAPS(Reg_xmm a, EA b) {
  b.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,0,0,0));
  e->emit8(0x28);
  emitModRMSIB(e,b);
}

void IA32::VMOVAPS(EA a, Reg_xmm b) {
  a.reg = b;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(b>>3,0,0,0));
  e->emit8(0x29);
  emitModRMSIB(e,a);
}

void IA32::VMOVAPS(Reg_ymm a, EA b) {
  b.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,0,1,0));
  e->emit8(0x28);
  emitModRMSIB(e,b);
}

void IA32::VMOVAPS(EA a, Reg_ymm b) {
  a.reg = b;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(b>>3,0,1,0));
  e->emit8(0x29);
  emitModRMSIB(e,a);
}


void IA32::MUL(Reg8 reg) {
  e->emit8(0xF6);
  e->emit8(_ModRM(3,6,reg));
}

void IA32::MUL(Reg16 reg) {
  e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
  e->emit8(0xF7);
  e->emit8(_ModRM(3,6,reg));
}

void IA32::MUL(Reg32 reg) {
  e->emit8(0xF7);
  e->emit8(_ModRM(3,6,reg));
}

void IA32::MUL(Reg64 reg) {
  EA rm(reg);
  emitREX(e,rm);
  e->emit8(0xF7);
  rm.reg = 6;
  emitModRMSIB(e,rm);
}



void IA32::MUL(EA rm, IntSize size) {
  switch (size) {
    case Int8:
      e->emit8(0xF6);
      rm.reg = 4;
      emitModRMSIB(e,rm);
    break;
    case Int16:
      e->emit8(PREFIX::Group3::OPERAND_SIZE_OVERRIDE);
      e->emit8(0xF7);
      rm.reg = 4;
      emitModRMSIB(e,rm);
    break;
    case Int32:
      e->emit8(0xF7);
      rm.reg = 4;
      emitModRMSIB(e,rm);
    break;
    case Int64:
      emitREX(e,rm);
      e->emit8(0xF7);
      rm.reg = 4;
      emitModRMSIB(e,rm);
    break;
  }

}


void IA32::MULPD(Reg_xmm a, EA b) {
  e->emit8(0x66);
  e->emit8(0x05);
  e->emit8(0x59);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMULPD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,1));
  e->emit8(0x59);
  emitModRMSIB(e,c);
}

void IA32::VMULPD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,1));
  e->emit8(0x59);
  emitModRMSIB(e,c);
}

void IA32::MULPS(Reg_xmm a, EA b) {
  e->emit8(0x05);
  e->emit8(0x59);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMULPS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,0));
  e->emit8(0x59);
  emitModRMSIB(e,c);
}

void IA32::VMULPS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,1,0));
  e->emit8(0x59);
  emitModRMSIB(e,c);
}

void IA32::MULSD(Reg_xmm a, EA b) {
  e->emit8(0xF2);
  e->emit8(0x05);
  e->emit8(0x59);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMULSD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,3));
  e->emit8(0x59);
  emitModRMSIB(e,c);
}

void IA32::MULSS(Reg_xmm a, EA b) {
  e->emit8(0xF3);
  e->emit8(0x05);
  e->emit8(0x59);
  b.reg = a;
  emitModRMSIB(e,b);
}

void IA32::VMULSS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX2_0());
  e->emit8(PREFIX::VEX2_1(a>>3,b,0,2));
  e->emit8(0x59);
  emitModRMSIB(e,c);
}

void IA32::MWAIT() {
  e->emit8(0x0F);
  e->emit8(0x01);
  e->emit8(0xC9);
}

}
}
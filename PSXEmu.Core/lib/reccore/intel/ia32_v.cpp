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

void IA32::VFMADD132PD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(1,b,0,PREFIX::kPP66));
  e->emit8(0x98);
  emitModRMSIB(e,c);
}

void IA32::VFMADD213PD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(1,b,0,PREFIX::kPP66));
  e->emit8(0xA8);
  emitModRMSIB(e,c);
}

void IA32::VFMADD231PD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(1,b,0,PREFIX::kPP66));
  e->emit8(0xB8);
  emitModRMSIB(e,c);
}

void IA32::VFMADD132PD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(1,b,1,PREFIX::kPP66));
  e->emit8(0x98);
  emitModRMSIB(e,c);
}

void IA32::VFMADD213PD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(1,b,1,PREFIX::kPP66));
  e->emit8(0xA8);
  emitModRMSIB(e,c);
}

void IA32::VFMADD231PD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(1,b,1,PREFIX::kPP66));
  e->emit8(0xB8);
  emitModRMSIB(e,c);
}


void IA32::VFMADD132PS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,0,PREFIX::kPP66));
  e->emit8(0x98);
  emitModRMSIB(e,c);
}

void IA32::VFMADD213PS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,0,PREFIX::kPP66));
  e->emit8(0xA8);
  emitModRMSIB(e,c);
}

void IA32::VFMADD231PS(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,0,PREFIX::kPP66));
  e->emit8(0xB8);
  emitModRMSIB(e,c);
}

void IA32::VFMADD132PS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,1,PREFIX::kPP66));
  e->emit8(0x98);
  emitModRMSIB(e,c);
}

void IA32::VFMADD213PS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,1,PREFIX::kPP66));
  e->emit8(0xA8);
  emitModRMSIB(e,c);
}

void IA32::VFMADD231PS(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,1,PREFIX::kPP66));
  e->emit8(0xB8);
  emitModRMSIB(e,c);
}

void IA32::VPSRAVD(Reg_xmm a, Reg_xmm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,0,PREFIX::kPP66));
  e->emit8(0x46);
  emitModRMSIB(e,c);
}

void IA32::VPSRAVD(Reg_ymm a, Reg_ymm b, EA c) {
  c.reg = a;
  e->emit8(PREFIX::VEX3_0());
  e->emit8(PREFIX::VEX3_1(a>>3,0,c.rm>>3,PREFIX::k0F38));
  e->emit8(PREFIX::VEX3_2(0,b,1,PREFIX::kPP66));
  e->emit8(0x46);
  emitModRMSIB(e,c);
}

}
}
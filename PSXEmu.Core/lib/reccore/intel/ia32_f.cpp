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

void IA32::F2XM1() {
  e->emit8(0xD9);
  e->emit8(0xF0);
}

void IA32::FABS() {
  e->emit8(0xD9);
  e->emit8(0xE1);
}

void IA32::FADD(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD8);
      mfp.reg = 0;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDC);
      mfp.reg = 0;
      emitModRMSIB(e,mfp);
      break;
  }
}

void IA32::FADD(Reg_sti dest, Reg_sti src) {
  if (dest == ST0) {
    e->emit8(0xD8);
    e->emit8(0xC0+src);
  } else {
    e->emit8(0xDC);
    e->emit8(0xC0+dest);
  }
}

void IA32::FADDP(Reg_sti dest, Reg_sti src) {
  if (dest == ST1 && src == ST0) {
    e->emit8(0xDE);
    e->emit8(0xC1);
  } else if (src == ST0) {
    e->emit8(0xDC);
    e->emit8(0xC0+dest);
  }
}

void IA32::FIADD(EA mint, IntSize size) {
  switch (size) {
    case Int16:
      e->emit8(0xDE);
      mint.reg = 0;
      emitModRMSIB(e,mint);
      break;
    case Int32:
      e->emit8(0xDA);
      mint.reg = 0;
      emitModRMSIB(e,mint);
      break;
  }
}

void IA32::FBLD(EA m80dec) {
  e->emit8(0xDF);
  m80dec.reg = 4;
  emitModRMSIB(e,m80dec);
}

void IA32::FBSTP(EA m80bcd) {
  e->emit8(0xDF);
  m80bcd.reg = 6;
  emitModRMSIB(e,m80bcd);
}

void IA32::FCHS() {
  e->emit8(0xD9);
  e->emit8(0xE0);
}

void IA32::FCLEX() {
  e->emit8(0x9B);
  e->emit8(0xDB);
  e->emit8(0xE2);
}

void IA32::FNCLEX() {
  e->emit8(0xDB);
  e->emit8(0xE2);
}

void IA32::FCOMI(Reg_sti STi) {
  e->emit8(0xDB);
  e->emit8(0xF0+STi);
}

void IA32::FCOMIP(Reg_sti STi) {
  e->emit8(0xDF);
  e->emit8(0xF0+STi);
}

void IA32::FUCOMI(Reg_sti STi) {
  e->emit8(0xDB);
  e->emit8(0xE8+STi);
}

void IA32::FUCOMIP(Reg_sti STi) {
  e->emit8(0xDF);
  e->emit8(0xE8+STi);
}


void IA32::FCOS() {
  e->emit8(0xD9);
  e->emit8(0xFF);
}


void IA32::FDIV(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD8);
      mfp.reg = 6;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDC);
      mfp.reg = 6;
      emitModRMSIB(e,mfp);
      break;
  }
}

void IA32::FDIV(Reg_sti dest, Reg_sti src) {
  if (dest == ST0) {
    e->emit8(0xD8);
    e->emit8(0xF0+src);
  } else {
    e->emit8(0xDC);
    e->emit8(0xF8+dest);
  }
}

void IA32::FDIVP(Reg_sti dest, Reg_sti src) {
  if (dest == ST1 && src == ST0) {
    e->emit8(0xDE);
    e->emit8(0xF9);
  } else if (src == ST0) {
    e->emit8(0xDE);
    e->emit8(0xF8+dest);
  }
}

void IA32::FIDIV(EA mint, IntSize size) {
  switch (size) {
    case Int16:
      e->emit8(0xDE);
      mint.reg = 6;
      emitModRMSIB(e,mint);
      break;
    case Int32:
      e->emit8(0xDA);
      mint.reg = 6;
      emitModRMSIB(e,mint);
      break;
  }
}


void IA32::FDIVR(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD8);
      mfp.reg = 7;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDC);
      mfp.reg = 7;
      emitModRMSIB(e,mfp);
      break;
  }
}

void IA32::FDIVR(Reg_sti dest, Reg_sti src) {
  if (dest == ST0) {
    e->emit8(0xD8);
    e->emit8(0xF8+src);
  } else {
    e->emit8(0xDC);
    e->emit8(0xF0+dest);
  }
}

void IA32::FDIVPR(Reg_sti dest, Reg_sti src) {
  if (dest == ST1 && src == ST0) {
    e->emit8(0xDE);
    e->emit8(0xF1);
  } else if (src == ST0) {
    e->emit8(0xDE);
    e->emit8(0xF0+dest);
  }
}

void IA32::FIDIVR(EA mint, IntSize size) {
  switch (size) {
    case Int16:
      e->emit8(0xDE);
      mint.reg = 7;
      emitModRMSIB(e,mint);
      break;
    case Int32:
      e->emit8(0xDA);
      mint.reg = 7;
      emitModRMSIB(e,mint);
      break;
  }
}

void IA32::FILD(EA mint, IntSize size) {
 switch (size) {
    case Int16:
      e->emit8(0xDF);
      mint.reg = 0;
      emitModRMSIB(e,mint);
      break;
    case Int32:
      e->emit8(0xDB);
      mint.reg = 0;
      emitModRMSIB(e,mint);
      break;
    case Int64:
      e->emit8(0xDF);
      mint.reg = 5;
      emitModRMSIB(e,mint);
      break;
  }
}

void IA32::FINCSTP() {
  e->emit8(0xD9);
  e->emit8(0xF7);
}

void IA32::FINIT() {
  e->emit8(0x9B);
  e->emit8(0xDB);
  e->emit8(0xE3);
}

void IA32::FNINIT() {
  e->emit8(0xDB);
  e->emit8(0xE3);
}

void IA32::FLD(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD9);
      mfp.reg = 0;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDD);
      mfp.reg = 0;
      emitModRMSIB(e,mfp);
      break;
    case FP80:
      e->emit8(0xDB);
      mfp.reg = 5;
      emitModRMSIB(e,mfp);
      break;
  }

}

void IA32::FLD(Reg_sti STi) {
  e->emit8(0xD9);
  e->emit8(0xC0+(STi&0x7));
}

void IA32::FLD1() {
  e->emit8(0xD9);
  e->emit8(0xE8);
}

void IA32::FLDL2T() {
  e->emit8(0xD9);
  e->emit8(0xE9);
}

void IA32::FLDL2E() {
  e->emit8(0xD9);
  e->emit8(0xEA);
}

void IA32::FLDPI() {
  e->emit8(0xD9);
  e->emit8(0xEB);
}

void IA32::FLDLG2() {
  e->emit8(0xD9);
  e->emit8(0xEC);
}

void IA32::FLDLN2() {
  e->emit8(0xD9);
  e->emit8(0xED);
}

void IA32::FLDZ() {
  e->emit8(0xD9);
  e->emit8(0xEE);
}

void IA32::FLDCW(EA m2byte) {
  e->emit8(0xD9);
  m2byte.reg = 5;
  emitModRMSIB(e,m2byte);
}

void IA32::FLDENV(EA mem) {
  e->emit8(0xD9);
  mem.reg = 4;
  emitModRMSIB(e,mem);
}

void IA32::FPTAN() {
  e->emit8(0xD9);
  e->emit8(0xF2);
}

void IA32::FSIN() {
  e->emit8(0xD9);
  e->emit8(0xFE);
}

void IA32::FSINCOS() {
  e->emit8(0xD9);
  e->emit8(0xFB);
}

void IA32::FSQRT() {
  e->emit8(0xD9);
  e->emit8(0xFA);
}

void IA32::FST(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD9);
      mfp.reg = 2;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDD);
      mfp.reg = 2;
      emitModRMSIB(e,mfp);
      break;
  }

}

void IA32::FST(Reg_sti STi) {
  e->emit8(0xDD);
  e->emit8(0xD0+STi);
}

void IA32::FSTP(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD9);
      mfp.reg = 3;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDD);
      mfp.reg = 3;
      emitModRMSIB(e,mfp);
      break;
    case FP80:
      e->emit8(0xDB);
      mfp.reg = 7;
      emitModRMSIB(e,mfp);
      break;
  }

}

void IA32::FSTP(Reg_sti STi) {
  e->emit8(0xDD);
  e->emit8(0xD8+STi);
}



void IA32::FSUB(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD8);
      mfp.reg = 4;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDC);
      mfp.reg = 4;
      emitModRMSIB(e,mfp);
      break;
  }
}

void IA32::FSUB(Reg_sti dest, Reg_sti src) {
  if (dest == ST0) {
    e->emit8(0xD8);
    e->emit8(0xE0+src);
  } else {
    e->emit8(0xDC);
    e->emit8(0xE8+dest);
  }
}

void IA32::FSUBP(Reg_sti dest, Reg_sti src) {
  if (dest == ST1 && src == ST0) {
    e->emit8(0xDE);
    e->emit8(0xE9);
  } else if (src == ST0) {
    e->emit8(0xDC);
    e->emit8(0xE8+dest);
  }
}

void IA32::FISUB(EA mint, IntSize size) {
  switch (size) {
    case Int16:
      e->emit8(0xDE);
      mint.reg = 4;
      emitModRMSIB(e,mint);
      break;
    case Int32:
      e->emit8(0xDA);
      mint.reg = 4;
      emitModRMSIB(e,mint);
      break;
  }
}

void IA32::FSUBR(EA mfp, FPSize size) {
  switch (size) {
    case FP32:
      e->emit8(0xD8);
      mfp.reg = 5;
      emitModRMSIB(e,mfp);
      break;
    case FP64:
      e->emit8(0xDC);
      mfp.reg = 5;
      emitModRMSIB(e,mfp);
      break;
  }
}

void IA32::FSUBR(Reg_sti dest, Reg_sti src) {
  if (dest == ST0) {
    e->emit8(0xD8);
    e->emit8(0xE8+src);
  } else {
    e->emit8(0xDC);
    e->emit8(0xE0+dest);
  }
}

void IA32::FSUBRP(Reg_sti dest, Reg_sti src) {
  if (dest == ST1 && src == ST0) {
    e->emit8(0xDE);
    e->emit8(0xE1);
  } else if (src == ST0) {
    e->emit8(0xDC);
    e->emit8(0xE0+dest);
  }
}

void IA32::FISUBR(EA mint, IntSize size) {
  switch (size) {
    case Int16:
      e->emit8(0xDE);
      mint.reg = 5;
      emitModRMSIB(e,mint);
      break;
    case Int32:
      e->emit8(0xDA);
      mint.reg = 5;
      emitModRMSIB(e,mint);
      break;
  }
}

void IA32::FTST() {
  e->emit8(0xD9);
  e->emit8(0xE4);
}

void IA32::FUCOM(Reg_sti STi) {
  e->emit8(0xDD);
  e->emit8(0xE0+STi);
}

void IA32::FUCOMP(Reg_sti STi) {
  e->emit8(0xDD);
  e->emit8(0xE8+STi);
}

void IA32::FUCOMPP() {
  e->emit8(0xDA);
  e->emit8(0xE9);
}

void IA32::FXAM() {
  e->emit8(0xD9);
  e->emit8(0xE5);
}
void IA32::FXCH(Reg_sti STi) {
  e->emit8(0xD9);
  e->emit8(0xC8+STi);
}

void IA32::FXRSTOR(EA m512byte) {
  e->emit8(0x0F);
  e->emit8(0xAE);
  m512byte.reg = 1;
  emitModRMSIB(e,m512byte);
}

void IA32::FXSAVE(EA m512byte) {
  e->emit8(0x0F);
  e->emit8(0xAE);
  m512byte.reg = 0;
  emitModRMSIB(e,m512byte);
}

void IA32::FXTRACT() {
  e->emit8(0xD9);
  e->emit8(0xF4);
}

void IA32::FYL2X() {
  e->emit8(0xD9);
  e->emit8(0xF1);
}

void IA32::FYL2XP1() {
  e->emit8(0xD9);
  e->emit8(0xF9);
}

}
}
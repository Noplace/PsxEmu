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
#pragma once

namespace reccore {
namespace intel {


inline uint8_t _ModRM(uint8_t mod,uint8_t reg,uint8_t rm)  {
	return (((mod&0x3)<<6)|((reg&0x7)<<3)|((rm&0x7)));
}

inline uint8_t _SIB(uint8_t scale,uint8_t index,uint8_t base)  {
	return (((scale&0x3)<<6)|((index&0x7)<<3)|((base&0x7)));
}

inline void emitREX(Emitter* e, EA mem) {
 if (mem.mod != 3) {
    if (mem.rm == 4) {
      e->emit8(PREFIX::REX(1,mem.reg>>3,mem.index>>3,mem.base>>3));
    } else {
      e->emit8(PREFIX::REX(1,mem.reg>>3,0,mem.rm>>3));
    }
  } else {
    e->emit8(PREFIX::REX(1,mem.reg>>3,0,mem.rm>>3)); //same as above i know
  }
}

inline void emitModRMSIB(Emitter* e, EA mem) {


	e->emit8(_ModRM(mem.mod,mem.reg,mem.rm));
  if (mem.mod != 3 && mem.rm == 4) {
    e->emit8(_SIB(mem.scale,mem.index,mem.base));
  }
  if (mem.mod == 0 && ((mem.rm == 5) || (mem.rm == 4 && mem.base == EBP)))
    e->emit32(mem.displacement);
  else if (mem.mod == 1)
    e->emit8(mem.displacement&0xFF);
  else if (mem.mod == 2)
    e->emit32(mem.displacement);
  
}


class IA32 : public InstructionSet {
 public:
  inline int8_t offset8(SIZE_T bytes) {
    return int8_t(bytes-(2+e->block()->cursor));
  }

	IA32(Emitter* pEmitter) : InstructionSet(pEmitter) {

  }
  virtual ~IA32() {

  }

  

  void AAA();
  void AAD();
  void AAM();
  void AAS();

  void ADC(Reg8 dest, uint8_t imm);
  void ADC(Reg16 dest, uint16_t imm);
  void ADC(Reg32 dest, uint32_t imm);
  void ADC(Reg64 dest, uint32_t imm);
  void ADC(EA rm, uint8_t imm);
  void ADC(EA rm, uint16_t imm);
  void ADC(EA rm, uint32_t imm);
  void ADC(EA rm, Reg8 reg);
  void ADC(EA rm, Reg16 reg);
  void ADC(EA rm, Reg32 reg);
  void ADC(EA rm, Reg64 reg);
  void ADC(Reg8 reg, EA rm);
  void ADC(Reg16 reg, EA rm);
  void ADC(Reg32 reg, EA rm);
  void ADC(Reg64 reg, EA rm);

  void ADD(Reg8 dest, uint8_t imm);
  void ADD(Reg16 dest, uint16_t imm);
  void ADD(Reg32 dest, uint32_t imm);
  void ADD(Reg64 dest, uint32_t imm);
  void ADD(EA rm, uint8_t imm);
  void ADD(EA rm, uint16_t imm);
  void ADD(EA rm, uint32_t imm);
  void ADD(EA rm, Reg8 reg);
  void ADD(EA rm, Reg16 reg);
  void ADD(EA rm, Reg32 reg);
  void ADD(EA rm, Reg64 reg);
  void ADD(Reg8 reg, EA rm);
  void ADD(Reg16 reg, EA rm);
  void ADD(Reg32 reg, EA rm);
  void ADD(Reg64 reg, EA rm);

  void ADDPD(Reg_xmm a, EA b);
  void VADDPD(Reg_xmm a, Reg_xmm b, EA c);
  void VADDPD(Reg_ymm a, Reg_ymm b, EA c);
  void ADDPS(Reg_xmm a, EA b);
  void VADDPS(Reg_xmm a, Reg_xmm b, EA c);
  void VADDPS(Reg_ymm a, Reg_ymm b, EA c);
  void ADDSD(Reg_xmm a, EA b);
  void VADDSD(Reg_xmm a, Reg_xmm b, EA c);
  void ADDSS(Reg_xmm a, EA b);
  void VADDSS(Reg_xmm a, Reg_xmm b, EA c);
  void ADDSUBPD(Reg_xmm a, EA b);
  void VADDSUBPD(Reg_xmm a, Reg_xmm b, EA c);
  void VADDSUBPD(Reg_ymm a, Reg_ymm b, EA c);

  void AND(Reg8 dest, uint8_t imm);
  void AND(Reg16 dest, uint16_t imm);
  void AND(Reg32 dest, uint32_t imm);
  void AND(Reg64 dest, uint32_t imm);
  void AND(EA rm, uint8_t imm);
  void AND(EA rm, uint16_t imm);
  void AND(EA rm, uint32_t imm);
  void AND(EA rm, Reg8 reg);
  void AND(EA rm, Reg16 reg);
  void AND(EA rm, Reg32 reg);
  void AND(EA rm, Reg64 reg);
  void AND(Reg8 reg, EA rm);
  void AND(Reg16 reg, EA rm);
  void AND(Reg32 reg, EA rm);
  void AND(Reg64 reg, EA rm);

  void CALL(uint16_t rel16);
  void CALL(uint32_t rel32);
  void CALL(void* ptr);

  void CMP(Reg8 reg, uint8_t imm8);
  void CMP(Reg32 reg, uint32_t imm32);

  void DAA();
  void DAS();

  void DEC(Reg32 reg);
  void DEC(Reg64 reg);
  void DEC(EA mem);

  void DIV(Reg8 reg);
  void DIV(Reg16 reg);
  void DIV(Reg32 reg);
  void DIV(Reg64 reg);
  void DIV(EA rm, IntSize size=Int32);

  void DIVPD(Reg_xmm a, EA b);
  void VDIVPD(Reg_xmm a, Reg_xmm b, EA c);
  void VDIVPD(Reg_ymm a, Reg_ymm b, EA c);
  void DIVPS(Reg_xmm a, EA b);
  void VDIVPS(Reg_xmm a, Reg_xmm b, EA c);
  void VDIVPS(Reg_ymm a, Reg_ymm b, EA c);
  void DIVSD(Reg_xmm a, EA b);
  void VDIVSD(Reg_xmm a, Reg_xmm b, EA c);
  void DIVSS(Reg_xmm a, EA b);
  void VDIVSS(Reg_xmm a, Reg_xmm b, EA c);

  void DPPD(Reg_xmm a, EA b, uint8_t imm8);
  void VDPPD(Reg_xmm a, Reg_xmm b, EA c, uint8_t imm8);

  void EMMS();
  void ENTER(uint16_t imm16, uint8_t imm8);
  void EXTRACTPS(EA rm32, Reg_xmm xmm, uint8_t imm8);
  void VEXTRACTPS(EA rm32, Reg_xmm xmm, uint8_t imm8);

  void F2XM1();
  void FABS();
  void FADD(EA mfp, FPSize size);
  void FADD(Reg_sti dest, Reg_sti src);
  void FADDP(Reg_sti dest, Reg_sti src);
  void FIADD(EA mint, IntSize size);
  void FBLD(EA m80dec);
  void FBSTP(EA m80bcd);
  void FCHS();
  void FCLEX();
  void FNCLEX();
  void FCOMI(Reg_sti STi);
  void FCOMIP(Reg_sti STi);
  void FUCOMI(Reg_sti STi);
  void FUCOMIP(Reg_sti STi);
  void FCOS();
  void FDIV(EA mfp, FPSize size);
  void FDIV(Reg_sti dest, Reg_sti src);
  void FDIVP(Reg_sti dest, Reg_sti src);
  void FIDIV(EA mint, IntSize size);
  void FDIVR(EA mfp, FPSize size);
  void FDIVR(Reg_sti dest, Reg_sti src);
  void FDIVPR(Reg_sti dest, Reg_sti src);
  void FIDIVR(EA mint, IntSize size);
  void FILD(EA mint, IntSize size);  
  void FINCSTP();
  void FINIT();
  void FNINIT();
  void FLD(EA mfp, FPSize size);  
  void FLD(Reg_sti STi);
  void FLD1();
  void FLDL2T();
  void FLDL2E();
  void FLDPI();
  void FLDLG2();
  void FLDLN2();
  void FLDZ();
  void FLDCW(EA m2byte);
  void FLDENV(EA mem);
  void FPTAN();
  void FSIN();
  void FSINCOS();
  void FSQRT();
  void FST(EA mfp, FPSize size);
  void FST(Reg_sti STi);
  void FSTP(EA mfp, FPSize size);
  void FSTP(Reg_sti STi);
  void FSUB(EA mfp, FPSize size);
  void FSUB(Reg_sti dest, Reg_sti src);
  void FSUBP(Reg_sti dest, Reg_sti src);
  void FISUB(EA mint, IntSize size);
  void FSUBR(EA mfp, FPSize size);
  void FSUBR(Reg_sti dest, Reg_sti src);
  void FSUBRP(Reg_sti dest, Reg_sti src);
  void FISUBR(EA mint, IntSize size);
  void FTST();
  void FUCOM(Reg_sti STi);
  void FUCOMP(Reg_sti STi);
  void FUCOMPP();
  void FXAM();
  void FXCH(Reg_sti STi);
  void FXRSTOR(EA m512byte);
  void FXSAVE(EA m512byte);
  void FXTRACT();
  void FYL2X();
  void FYL2XP1();

  void GETSEC();

  void HADDPD(Reg_xmm a, EA b);
  void VHADDPD(Reg_xmm a, Reg_xmm b, EA c);
  void VHADDPD(Reg_ymm a, Reg_ymm b, EA c);


  void HADDPS(Reg_xmm a, EA b);
  void VHADDPS(Reg_xmm a, Reg_xmm b, EA c);
  void VHADDPS(Reg_ymm a, Reg_ymm b, EA c);

  void IDIV(Reg8 reg);
  void IDIV(Reg16 reg);
  void IDIV(Reg32 reg);
  void IDIV(Reg64 reg);
  void IDIV(EA rm, IntSize size=Int32);

  void INC(Reg16 reg);
  void INC(Reg32 reg);
  void INC(Reg64 reg);
  void INC(EA mem);

  void Jcc(ConditionCode cc, int8_t rel8);
  void Jcc(ConditionCode cc, int16_t rel16);
  void Jcc(ConditionCode cc, int32_t rel32);

  void JA(LabelType label) {
    Jcc(kCCA,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JAE(LabelType label) {
    Jcc(kCCAE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JB(LabelType label) {
    Jcc(kCCB,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JBE(LabelType label) {
    Jcc(kCCBE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JC(LabelType label) {
    Jcc(kCCC,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JE(LabelType label) {
    Jcc(kCCE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JG(LabelType label) {
    Jcc(kCCG,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JGE(LabelType label) {
    Jcc(kCCGE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JL(LabelType label) {
    Jcc(kCCL,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JLE(LabelType label) {
    Jcc(kCCLE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNA(LabelType label) {
    Jcc(kCCNA,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNAE(LabelType label) {
    Jcc(kCCNAE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNB(LabelType label) {
    Jcc(kCCNB,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNBE(LabelType label) {
    Jcc(kCCNBE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNC(LabelType label) {
    Jcc(kCCNC,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNE(LabelType label) {
    Jcc(kCCNE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNG(LabelType label) {
    Jcc(kCCNG,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNGE(LabelType label) {
    Jcc(kCCNGE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNL(LabelType label) {
    Jcc(kCCNL,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNLE(LabelType label) {
    Jcc(kCCNLE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNO(LabelType label) {
    Jcc(kCCNO,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNP(LabelType label) {
    Jcc(kCCNP,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNS(LabelType label) {
    Jcc(kCCNS,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JNZ(LabelType label) {
    Jcc(kCCNZ,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JO(LabelType label) {
    Jcc(kCCO,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JP(LabelType label) {
    Jcc(kCCP,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JPE(LabelType label) {
    Jcc(kCCPE,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JPO(LabelType label) {
    Jcc(kCCPO,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JS(LabelType label) {
    Jcc(kCCS,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void JZ(LabelType label) {
    Jcc(kCCZ,int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }

  void JMP(int8_t rel8);
  void JMP(int16_t rel16);
  void JMP(int32_t rel32);

  void JMP(LabelType label) {
    JMP(int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }

  void LAHF();

  void LAR(Reg16 reg, EA rm);
  void LAR(Reg32 reg, EA rm);
  void LAR(Reg64 reg, EA rm);

  void LDDQU(Reg_xmm reg, EA mem);
  void VLDDQU(Reg_xmm reg, EA mem);
  void VLDDQU(Reg_ymm reg, EA mem);

  void LEA(Reg16 r16, EA mem);
  void LEA(Reg32 r32, EA mem);
  void LEA(Reg64 r64, EA mem);
  void LEA(Reg32 r32, void* ptr);
  
  void LEAVE();

  void LOOP(int8_t rel8);
  void LOOPE(int8_t rel8);
  void LOOPNE(int8_t rel8);
  void LOOP(LabelType label){
    LOOP(int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void LOOPE(LabelType label){
    LOOPE(int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }
  void LOOPNE(LabelType label) {
    LOOPNE(int8_t(0xFF));
    jumps[jump_counter++] = {0,label,e->GetCursor()-1};
  }

  void MAXPD(Reg_xmm a, EA b);
  void VMAXPD(Reg_xmm a, Reg_xmm b, EA c);
  void VMAXPD(Reg_ymm a, Reg_ymm b, EA c);
  void MAXPS(Reg_xmm a, EA b);
  void VMAXPS(Reg_xmm a, Reg_xmm b, EA c);
  void VMAXPS(Reg_ymm a, Reg_ymm b, EA c);
  void MAXSD(Reg_xmm a, EA b);
  void VMAXSD(Reg_xmm a, Reg_xmm b, EA c);
  void MAXSS(Reg_xmm a, EA b);
  void VMAXSS(Reg_xmm a, Reg_xmm b, EA c);

  void MFENCE();

  void MINPD(Reg_xmm a, EA b);
  void VMINPD(Reg_xmm a, Reg_xmm b, EA c);
  void VMINPD(Reg_ymm a, Reg_ymm b, EA c);
  void MINPS(Reg_xmm a, EA b);
  void VMINPS(Reg_xmm a, Reg_xmm b, EA c);
  void VMINPS(Reg_ymm a, Reg_ymm b, EA c);
  void MINSD(Reg_xmm a, EA b);
  void VMINSD(Reg_xmm a, Reg_xmm b, EA c);
  void MINSS(Reg_xmm a, EA b);
  void VMINSS(Reg_xmm a, Reg_xmm b, EA c);

  void MONITOR();

  void MOV(EA m8, Reg8 src);
  void MOV(EA m16, Reg16 src);
  void MOV(EA m32, Reg32 src);
  void MOV(EA m64, Reg64 src);
  void MOV(Reg8 dest, EA m8);
  void MOV(Reg16 dest, EA m16);
  void MOV(Reg32 dest, EA m32);
  void MOV(Reg64 dest, EA m64);
  void MOV(Reg8 dest, Reg8 src);
  void MOV(Reg16 dest, Reg16 src);
  void MOV(Reg32 dest, Reg32 src);
  void MOV(Reg8 dest, uint8_t imm);
  void MOV(Reg16 dest, uint16_t imm);
  void MOV(Reg32 dest, uint32_t imm);
  void MOV(Reg64 dest, uint64_t imm);
  void MOV(void* dest, Reg32 src);
  void MOV(Reg32 dest, void* src);
  void MOV(Reg64 dest, void* src);
  void MOV(Reg64 dest, Reg64 src);
  void MOV(EA rm16, uint16_t imm);
  void MOV(EA rm, uint32_t imm);

  void MOV(Reg32 r32, Reg_CR cr);
  void MOV(Reg64 r64, Reg_CR cr);
  void MOV(Reg_CR cr, Reg32 r32);
  void MOV(Reg_CR cr, Reg64 r64);
  void MOV(Reg32 r32, Reg_DR dr);
  void MOV(Reg64 r64, Reg_DR dr);
  void MOV(Reg_DR dr, Reg32 r32);
  void MOV(Reg_DR dr, Reg64 r64);

  void MOVAPD(Reg_xmm a, EA b);
  void MOVAPD(EA a, Reg_xmm b);
  void VMOVAPD(Reg_xmm a, EA b);
  void VMOVAPD(EA a, Reg_xmm b);
  void VMOVAPD(Reg_ymm a, EA b);
  void VMOVAPD(EA a, Reg_ymm b);

  void MOVAPS(Reg_xmm a, EA b);
  void MOVAPS(EA a, Reg_xmm b);
  void VMOVAPS(Reg_xmm a, EA b);
  void VMOVAPS(EA a, Reg_xmm b);
  void VMOVAPS(Reg_ymm a, EA b);
  void VMOVAPS(EA a, Reg_ymm b);

  void MUL(Reg8 reg);
  void MUL(Reg16 reg);
  void MUL(Reg32 reg);
  void MUL(Reg64 reg);
  void MUL(EA rm, IntSize size=Int32);

  void MULPD(Reg_xmm a, EA b);
  void VMULPD(Reg_xmm a, Reg_xmm b, EA c);
  void VMULPD(Reg_ymm a, Reg_ymm b, EA c);
  void MULPS(Reg_xmm a, EA b);
  void VMULPS(Reg_xmm a, Reg_xmm b, EA c);
  void VMULPS(Reg_ymm a, Reg_ymm b, EA c);
  void MULSD(Reg_xmm a, EA b);
  void VMULSD(Reg_xmm a, Reg_xmm b, EA c);
  void MULSS(Reg_xmm a, EA b);
  void VMULSS(Reg_xmm a, Reg_xmm b, EA c);

  void MWAIT();

  void NOP();

  void OR(Reg8 dest, uint8_t imm);
  void OR(Reg16 dest, uint16_t imm);
  void OR(Reg32 dest, uint32_t imm);
  void OR(Reg64 dest, uint32_t imm);
  void OR(EA rm, uint8_t imm);
  void OR(EA rm, uint16_t imm);
  void OR(EA rm, uint32_t imm);
  void OR(EA rm, Reg8 reg);
  void OR(EA rm, Reg16 reg);
  void OR(EA rm, Reg32 reg);
  void OR(EA rm, Reg64 reg);
  void OR(Reg8 reg, EA rm);
  void OR(Reg16 reg, EA rm);
  void OR(Reg32 reg, EA rm);
  void OR(Reg64 reg, EA rm);

  void PAUSE();

  void PCLMULQDQ(Reg_xmm a, EA b, uint8_t imm8);
  void VPCLMULQDQ(Reg_xmm a, Reg_xmm b, EA c, uint8_t imm8);

  void POP(Reg32 reg);
  void POPA();
  void POPAD();


  void PUSH(EA mem); 
  void PUSH(Reg16 reg);
  void PUSH(Reg32 reg);
  void PUSH(Reg64 reg);
  void PUSH(uint8_t imm);
  void PUSH(uint16_t imm);
  void PUSH(uint32_t imm);
  void PUSHA();
  void PUSHAD();
  void PUSHF();
  void PUSHFD();
  void PUSHFQ();

  void RDRAND(Reg16 reg);
  void RDRAND(Reg32 reg);
  void RDRAND(Reg64 reg);
  void RDTSC();
  void RDTSCP();
  void RET(bool farreturn=false);

  void SETcc(ConditionCode cc, EA rm);

  void SHL(EA rm, uint8_t imm8);

  void VFMADD132PD(Reg_xmm a, Reg_xmm b, EA c);
  void VFMADD213PD(Reg_xmm a, Reg_xmm b, EA c);
  void VFMADD231PD(Reg_xmm a, Reg_xmm b, EA c);
  void VFMADD132PD(Reg_ymm a, Reg_ymm b, EA c);
  void VFMADD213PD(Reg_ymm a, Reg_ymm b, EA c);
  void VFMADD231PD(Reg_ymm a, Reg_ymm b, EA c);
  void VFMADD132PS(Reg_xmm a, Reg_xmm b, EA c);
  void VFMADD213PS(Reg_xmm a, Reg_xmm b, EA c);
  void VFMADD231PS(Reg_xmm a, Reg_xmm b, EA c);
  void VFMADD132PS(Reg_ymm a, Reg_ymm b, EA c);
  void VFMADD213PS(Reg_ymm a, Reg_ymm b, EA c);
  void VFMADD231PS(Reg_ymm a, Reg_ymm b, EA c);

  void VPSRAVD(Reg_xmm a, Reg_xmm b, EA c);
  void VPSRAVD(Reg_ymm a, Reg_ymm b, EA c);

  void XTEST();

  //call if using labels, preferebly only call once after finishing writing to the  block
  //todo , 16bit and 32bit offsets
  void ResolveJumps() {
    for (auto jump : jumps) {
      auto item = jump.second;
      switch (item.size) {
        case 0 :  {
            uint8_t offset = int8_t(labels[item.label]-(1+item.cursor));
            e->block()->ptr8bit[item.cursor] = offset; 
           break;
        }
        //case 1 : e->block()->ptr8bit[item.cursor] = offset16(labels[item.label]); break;
        //case 2 : e->block()->ptr8bit[item.cursor] = offset32(labels[item.label]); break;
        
      }
    }
    jumps.clear();
    labels.clear();
    jump_counter = 0;
    
  }
 private:

  /*__inline uint8_t WriteBitAt(uint8_t bit,uint8_t index)  {
	  bit&=0x1;
	  return (bit<<index);
  }

  __inline uint8_t WriteBitsAt(uint8_t bits,uint8_t count,uint8_t index)  {
	  bits&=((1<<count)-1);
	  return (bits<<index);
  }

  __inline uint8_t ModRM(uint8_t mod,uint8_t reg,uint8_t rm)  {
	  return (((mod&0x3)<<6)|((reg&0x7)<<3)|((rm&0x7)));
  }

  __inline uint8_t SIB(uint8_t scale,uint8_t index,uint8_t base)  {
	  return (((scale&0x3)<<6)|((index&0x7)<<3)|((base&0x7)));
  }*/
};

}
}
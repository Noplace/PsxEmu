/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : cpu.h
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
/*
  Cpu Emulator
  R3000 Chip
*/
#ifndef EMULATION_PSX_CPU_H
#define EMULATION_PSX_CPU_H

namespace emulation {
namespace psx {

class Cpu : public Component {
 public:
  typedef void (Cpu::*Instruction)();
  DebugAssist bioscode;
  int index;
  bool inside_bios_call;
  bool bios_logged[3][256];
  Cpu();
  ~Cpu();
  void Initialize();
  void ExecuteInstruction();
  void RaiseException(uint32_t address, Exceptions exception, ExceptionCodes code);
  void Reset() { RaiseException(context_->pc,kResetException,kExceptionCodeInt); }
  CpuContext* context() { return context_; }
  void set_context(CpuContext* context) { context_ = context; }
 private:
#ifndef NDEBUG
  DebugAssist csvout;
#endif
  bool __inside_instruction;
  bool __inside_delay_slot;
  CpuContext* context_;
  static Instruction machine_instruction_main_[64];
  static Instruction machine_instruction_special_[64];
  static Instruction machine_instruction_regimm_[32];
  static Instruction machine_instruction_cop0_[64];
  static Instruction machine_instruction_cop2_[64];

  void Branch(uint32_t address);

  void UNKNOWN();
  void SPECIAL();
  void REGIMM();
  void J();
  void JAL();
  void BEQ();
  void BNE();
  void BLEZ();
  void BGTZ();
  void ADDI();
  void ADDIU();
  void SLTI();
  void SLTIU();
  void ANDI();
  void ORI();
  void XORI();
  void LUI();
  void COP0();
  void COP2();

  void LB();
  void LH();
  void LWL();
  void LW();
  void LBU();
  void LHU();
  void LWR();
  void SB();
  void SH();
  void SWL();
  void SW();
  void SWR();
  

  void SLL();
  void SRL();
  void SRA();
  void SLLV();
  void SRLV();
  void SRAV();
  void JR();
  void JALR();
  void SYSCALL();
  void BREAK();
  void MFHI();
  void MTHI();
  void MFLO();
  void MTLO(); 
  void MULT(); 
  void MULTU(); 
  void DIV(); 
  void DIVU(); 
  void ADD();
  void ADDU();
  void SUB();
  void SUBU();
  void AND();
  void OR();
  void XOR();
  void NOR();
  void SLT();
  void SLTU();


  void BLTZ();
  void BGEZ();
  void BLTZAL();
  void BGEZAL();
};

}
}

#endif
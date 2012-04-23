/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : cpu.cpp
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
#include "../system.h"

#define CSVOUT
#define CPU_DEBUG
//#define BIOSCALL

namespace emulation {
namespace psx {

Cpu::Instruction Cpu::machine_instruction_main_[64] = {
  &Cpu::SPECIAL, &Cpu::REGIMM , &Cpu::J      , &Cpu::JAL    , &Cpu::BEQ    , &Cpu::BNE    , &Cpu::BLEZ   , &Cpu::BGTZ   ,
  &Cpu::ADDI   , &Cpu::ADDIU  , &Cpu::SLTI   , &Cpu::SLTIU  , &Cpu::ANDI   , &Cpu::ORI    , &Cpu::XORI   , &Cpu::LUI    ,
  &Cpu::COP0   , &Cpu::UNKNOWN, &Cpu::COP2   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::LB     , &Cpu::LH     , &Cpu::LWL    , &Cpu::LW     , &Cpu::LBU    , &Cpu::LHU    , &Cpu::LWR    , &Cpu::UNKNOWN,
  &Cpu::SB     , &Cpu::SH     , &Cpu::SWL    , &Cpu::SW     , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SWR    , &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
};

Cpu::Instruction Cpu::machine_instruction_special_[64] = {
  &Cpu::SLL    , &Cpu::UNKNOWN, &Cpu::SRL    , &Cpu::SRA    , &Cpu::SLLV   , &Cpu::UNKNOWN, &Cpu::SRLV   , &Cpu::SRAV   ,
  &Cpu::JR     , &Cpu::JALR   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SYSCALL, &Cpu::BREAK  , &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::MFHI   , &Cpu::MTHI   , &Cpu::MFLO   , &Cpu::MTLO   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::MULT   , &Cpu::MULTU  , &Cpu::DIV    , &Cpu::DIVU   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::ADD    , &Cpu::ADDU   , &Cpu::SUB    , &Cpu::SUBU   , &Cpu::AND    , &Cpu::OR     , &Cpu::XOR    , &Cpu::NOR    ,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SLT    , &Cpu::SLTU   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN
};

Cpu::Instruction Cpu::machine_instruction_regimm_[32] = {
  &Cpu::BLTZ   , &Cpu::BGEZ   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::BLTZAL , &Cpu::BGEZAL , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN
};

/******************************************************************************
* Name        : Cpu
* Description : Cpu Constructor
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
Cpu::Cpu() : __inside_instruction(false),__inside_delay_slot(false), context_(NULL) {
  memset(bios_logged,0,sizeof(bios_logged));
}

/******************************************************************************
* Name        : ~Cpu
* Description : Cpu Destructor
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
Cpu::~Cpu() {
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(BIOSCALL)
    bioscode.Close();
  #endif
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    csvout.Close();
  #endif
}

/******************************************************************************
* Name        : Initialize
* Description : 
* Parameters  : (none)
* 
* Notes :
* 
* 
*******************************************************************************/
void Cpu::Initialize() {
  index = 0;
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT) && defined(BIOSCALL)
    bioscode.Open("bioscode.txt");
  #endif
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    csvout.Open("cpu_log.csv");
    if (csvout.fp)
    fprintf(csvout.fp,"\"Counter\",\"PC\",\"Opcode\",\"Code\",\"RS Index\",\"RS Value\",\"RT Index\",\"RT Value\",\"RD Index\",\"RD Value\",\"imm\",\"jump address\",\"branch address\",\"l/s address\"\n");
    assert(context_ != NULL);
  #endif
  context_->gp.zero = 0;
  context_->ctrl.PRId = 3 << 8; //R3000A
  context_->ctrl.SR = 0x10900000;//1111
}

/******************************************************************************
* Name        : ExecuteInstruction
* Description : 
* Parameters  : (none)
* 
* Notes :
* 
* 
*******************************************************************************/
void Cpu::ExecuteInstruction() {
  context_->gp.zero = 0; //make sure r0 is always 0.
  context_->code = system_->io().Read32(context_->pc);
  context_->prev_pc = context_->pc;
  context_->pc  += 4;
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(BIOSCALL)
    if (inside_bios_call == true) {
      char* inst_str = csvout.machine_instruction_main_[context_->opcode()];
      char* sp_str = csvout.machine_instruction_special_[context_->code&0x3f];
      char* rm_str = csvout.machine_instruction_regimm_[context_->rt()];
      if (context_->opcode() == 0)
        inst_str = sp_str;
      if (context_->opcode() == 1)
        inst_str = rm_str;
      if (context_->code == 0)
        inst_str = "NOP";

      uint32_t address0 = (context_->pc & 0xF0000000) | (context_->target() << 2);
      uint32_t address1 = context_->pc + (context_->immediate_32bit_sign_extended() << 2);
      uint32_t address2 = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
      if (bioscode.fp != NULL)
        fprintf(bioscode.fp,"\"%08X\",\"%08X\",\"%s\",%08X,%d,%08X,%d,%08X,%d,%08X,\"u%08X s%08X\",%08X,%08X,%08X\n",
          index,context_->prev_pc,inst_str,context_->code,context_->rs(),
          context_->gpr_rs(),context_->rt(),context_->gpr_rt(),
          context_->rd(),context_->gpr_rd(),context_->immediate(),
          context_->immediate_32bit_sign_extended(),address0,address1,address2);
    }
  #endif
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    {
      char* inst_str = csvout.machine_instruction_main_[context_->opcode()];
      char* sp_str = csvout.machine_instruction_special_[context_->code&0x3f];
      char* rm_str = csvout.machine_instruction_regimm_[context_->rt()];
      if (context_->opcode() == 0)
        inst_str = sp_str;
      if (context_->opcode() == 1)
        inst_str = rm_str;

      uint32_t address0 = (context_->pc & 0xF0000000) | (context_->target() << 2);
      uint32_t address1 = context_->pc + (context_->immediate_32bit_sign_extended() << 2);
      uint32_t address2 = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
      if (csvout.fp != NULL)
        fprintf(csvout.fp,"\"0x%08X\",\"0x%08X\",\"%s\",0x%08X,%d,0x%08X,%d,0x%08X,%d,0x%08X,\"u0x%08X s0x%08X\",0x%08X,0x%08X,0x%08X\n",
          index,context_->prev_pc,inst_str,context_->code,context_->rs(),
          context_->gpr_rs(),context_->rt(),context_->gpr_rt(),
          context_->rd(),context_->gpr_rd(),context_->immediate(),
          context_->immediate_32bit_sign_extended(),address0,address1,address2);
    }
  #endif
  index++;
  __inside_instruction = true;
  (this->*(machine_instruction_main_[context_->opcode()]))();
  __inside_instruction = false;
  ++context_->cycles;
  ++context_->cycle_counter;
}

/******************************************************************************
* Name        : RaiseException
* Description : raise the specified exception and change pc
* Parameters  : address , exception , code
* 
* Notes :
* 
*   So on an exception, the CPU:
*   1) sets up EPC to point to the restart location.
*   2) the pre-existing user-mode and interrupt-enable flags in SR are
*       saved by pushing the 3-entry stack inside SR, and changing to
*       kernel mode with interrupts disabled.
*   3) Cause is setup so that software can see the reason for the
*       exception. On address exceptions BadVaddr is also set. Memory
*       management system exceptions set up some of the MMU
*       registers too; see the chapter on memory management for more
*       detail.
*   4) transfers control to the exception entry point.  
* 
*******************************************************************************/
void Cpu::RaiseException(uint32_t address, Exceptions exception, ExceptionCodes code) {
  #if defined(_DEBUG) && defined(CPU_DEBUG)
    if(system_->log.fp)
      fprintf(system_->log.fp,"exception @ %08X address=%08X\texception=%08X\tcode=%08X\n",context_->prev_pc,address,exception,code);
  #endif
  //save to epc
  context_->ctrl.EPC = context_->branch_flag == true ? address-4 : address;

  //push the bit stack for kernel,interrupt flags
  uint32_t& sr = context_->ctrl.SR;
  sr = (sr & ~0x3F) | ((sr & 0xF) << 2) | 0x2;

  //set cause
  //todo : set ip flags correctly
  uint32_t& cause = context_->ctrl.Cause;
  cause = 0;
  cause |= context_->branch_flag == true ? 0x80000000 : 0;
  cause |= (code&0x1F)<<2;


  //specific exception handling
  if (exception == kTLBMissException) {
    if ((context_->ctrl.SR & 0x00400000) == 0) //BEV = 0
      context_->pc = 0x80000000;
    else
      context_->pc = 0xBFC00100;
  }

  if (exception == kOtherException) {
    if ((context_->ctrl.SR & 0x00400000) == 0) //BEV = 0
      context_->pc = 0x80000080;
    else
      context_->pc = 0xBFC00180;
  }

  if (exception == kResetException) {
    //default state : 0101 0000 0110 0001 0000 0000 0000 0000
    context_->ctrl.SR = 0x50610000;
    context_->pc = 0xBFC00000;
  }

}

/******************************************************************************
* Name        : Branch
* Description : branch to given address, executing the delay slot instruction
* Parameters  : address
* 
* Notes :
* 
* 
*******************************************************************************/
void Cpu::Branch(uint32_t address) {
 #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    if (csvout.fp != NULL)
      fprintf(csvout.fp,"following is delay slot\n");
  #endif
  __inside_delay_slot = true;
  ExecuteInstruction();
  __inside_delay_slot = false;
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    if (csvout.fp != NULL)
      fprintf(csvout.fp,"end of delay slot\n");
  #endif
  context_->pc = address;
}

void Cpu::UNKNOWN() {
  throw;
}

void Cpu::SPECIAL() {
  (this->*(machine_instruction_special_[context_->fu()]))();
}

void Cpu::REGIMM() {
  (this->*(machine_instruction_regimm_[context_->rt()]))();
}

void Cpu::J() {
  uint32_t address = (context_->pc & 0xF0000000) | (context_->target() << 2);
  Branch(address);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;
}

void Cpu::JAL() {
  auto temp = context_->target();
  context_->gp.ra = context_->pc + 4;
  uint32_t address = (context_->pc & 0xF0000000) | (temp << 2);
  Branch(address);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;
}

void Cpu::BEQ() {
  if (context_->gpr_rs() == context_->gpr_rt()) {
    Branch(context_->pc + (context_->immediate_32bit_sign_extended() << 2));
  }
}

void Cpu::BNE() {
  if (context_->gpr_rs() != context_->gpr_rt()) {
    Branch(context_->pc + (context_->immediate_32bit_sign_extended() << 2));
  }
}

void Cpu::BLEZ() {
  bool cond = ((context_->gpr_rs() & 0x80000000)==1) || (context_->gpr_rs() = 0 );
  if (cond==true) {//context_->gpr_rs() <= 0) {
    Branch(context_->pc + (context_->immediate_32bit_sign_extended() << 2));
  }
}

void Cpu::BGTZ() {
  bool cond = ((context_->gpr_rs() & 0x80000000)==0) && (context_->gpr_rs() != 0 );
  if (cond==true) {//context_->gpr_rs() > 0) {
    Branch(context_->pc + (context_->immediate_32bit_sign_extended() << 2));
  }
}

void Cpu::ADDI() {
  context_->gpr_rt() = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
}

void Cpu::ADDIU() {
  context_->gpr_rt() = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
}

void Cpu::SLTI() {
  context_->gpr_rt() = (int32_t)context_->gpr_rs() < context_->immediate_32bit_sign_extended();
}

void Cpu::SLTIU() {
  context_->gpr_rt() = context_->gpr_rs() < context_->immediate();
}


void Cpu::ANDI() {
  context_->gpr_rt() = context_->gpr_rs() & context_->immediate();
}

void Cpu::ORI() {
  context_->gpr_rt() = context_->gpr_rs() | context_->immediate();
}

void Cpu::XORI() {
  context_->gpr_rt() = context_->gpr_rs() ^ context_->immediate();
}

void Cpu::LUI() {
  context_->gpr_rt() = context_->immediate() << 16;
}

void Cpu::COP0() {
  switch (context_->rs()) {
    //MFC
    case 0x00: {
      context_->gpr_rt() = context_->ctrl.reg[context_->rd()];
      break;
    }
    //MTC
    case 0x04: {
      context_->ctrl.reg[context_->rd()] = context_->gpr_rt();
      break;
    }
    //RFE
    case 0x10: {
      context_->ctrl.SR = (context_->ctrl.SR & ~0xF) | ((context_->ctrl.SR >> 2) & 0xF);
      break;
    }
    default:
      throw;
  }
}

void Cpu::COP2() {
  throw;
}

void Cpu::LB() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  context_->gpr_rt() = (int32_t)system_->io().Read08(address);
}

void Cpu::LH() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  context_->gpr_rt() = (int32_t)system_->io().Read16(address);
}

void Cpu::LWL() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  uint32_t data = system_->io().Read32(address & ~0x03);
  switch (address & 0x3) {
    case 0:
      context_->gpr_rt() = data;
      break;
    case 1:
      context_->gpr_rt() = (context_->gpr_rt() & 0x000000FF) | (data<<8);
      break;
    case 2:
      context_->gpr_rt() = (context_->gpr_rt() & 0x0000FFFF) | (data<<16);
      break;
    case 3:
      context_->gpr_rt() = (context_->gpr_rt() & 0x00FFFFFF) | (data<<24);
      break;
  }
}

void Cpu::LW() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  context_->gpr_rt() = system_->io().Read32(address);
}

void Cpu::LBU() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  context_->gpr_rt() = (uint32_t)system_->io().Read08(address);
}

void Cpu::LHU() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  context_->gpr_rt() = (uint32_t)system_->io().Read16(address);
}

void Cpu::LWR() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  uint32_t data = system_->io().Read32(address & ~0x03);
  switch (address & 0x3) {
    case 0:
      context_->gpr_rt() = data;
      break;
    case 1:
      context_->gpr_rt() = (context_->gpr_rt() & 0xFF000000) | (data>>8);
      break;
    case 2:
      context_->gpr_rt() = (context_->gpr_rt() & 0xFFFF0000) | (data>>16);
      break;
    case 3:
      context_->gpr_rt() = (context_->gpr_rt() & 0xFFFFFF00) | (data>>24);
      break;
  }
}

void Cpu::SB() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  system_->io().Write08(address,(uint8_t)context_->gpr_rt());
}

void Cpu::SH() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  system_->io().Write16(address,(uint16_t)context_->gpr_rt());
}

void Cpu::SWL() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  uint32_t data = system_->io().Read32(address & ~0x03);
  switch (address & 0x3) {
    case 0:
      data = (data & 0xFFFFFF00) | (context_->gpr_rt() >> 24);
      break;
    case 1:
      data = (data & 0xFFFF0000) | (context_->gpr_rt() >> 16);
      break;
    case 2:
      data = (data & 0xFF000000) | (context_->gpr_rt() >> 8);
      break;
    case 3:
      data = context_->gpr_rt();
      break;
  }
  system_->io().Write32(address & ~0x03,data);
}

void Cpu::SW() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  system_->io().Write32(address,context_->gpr_rt());
}

void Cpu::SWR() {
  uint32_t address = context_->gpr_rs() + context_->immediate_32bit_sign_extended();
  uint32_t data = system_->io().Read32(address & ~0x03);
  switch (address & 0x3) {
    case 0:
      data = context_->gpr_rt();
      break;
    case 1:
      data = (data & 0x000000FF) | (context_->gpr_rt() << 8);
      break;
    case 2:
      data = (data & 0x0000FFFF) | (context_->gpr_rt() << 16);
      break;
    case 3:
      data = (data & 0x00FFFFFF) | (context_->gpr_rt() << 24);
      break;
  }
  system_->io().Write32(address & ~0x03,data);
}

void Cpu::SLL() {
  context_->gpr_rd() = context_->gpr_rt() << context_->sa();
}

void Cpu::SRL() {
  context_->gpr_rd() = context_->gpr_rt() >> context_->sa();
}

void Cpu::SRA() {
  context_->gpr_rd() = (int32_t)context_->gpr_rt() >> context_->sa();
}

void Cpu::SLLV() {
  context_->gpr_rd() = context_->gpr_rt() << context_->gpr_rs() & 0x1F;
}

void Cpu::SRLV() {
 context_->gpr_rd() = context_->gpr_rt() >> context_->gpr_rs() & 0x1F;
}

void Cpu::SRAV() {
  context_->gpr_rd() = (int32_t)context_->gpr_rt() >> context_->gpr_rs() & 0x1F;
}

void Cpu::JR() {
  Branch(context_->gpr_rs());
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;
}

void Cpu::JALR() {
  context_->gpr_rd() = context_->pc + 4; // rd must be 31
  Branch(context_->gpr_rs());
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;

}

void Cpu::SYSCALL() {
  context_->pc -= 4;
  RaiseException(context_->pc,kOtherException,kExceptionCodeSyscall);
  throw;
}

void Cpu::BREAK() {
  throw;
}

void Cpu::MFHI() {
  context_->gpr_rd() = context_->high;
}

void Cpu::MTHI() {
  context_->high  = context_->gpr_rd();
}

void Cpu::MFLO() {
  context_->gpr_rd() = context_->low;
}

void Cpu::MTLO() {
  context_->low  = context_->gpr_rd();
}

void Cpu::MULT() {
  signed __int64 test = (int32_t)context_->gpr_rs() * (int32_t)context_->gpr_rt();
  context_->low  = (uint32_t)(test & 0x00000000FFFFFFFFULL);
  context_->high = (uint32_t)(test & 0xFFFFFFFF00000000ULL >> 32);
}

void Cpu::MULTU() {
  unsigned __int64 test = context_->gpr_rs() * context_->gpr_rt();
  context_->low  = (uint32_t)(test & 0x00000000FFFFFFFFULL);
  context_->high = (uint32_t)(test & 0xFFFFFFFF00000000ULL >> 32);
}

void Cpu::DIV() {
  if (context_->gpr_rt()) {
    context_->low  = (int32_t)context_->gpr_rs() / (int32_t)context_->gpr_rt();
    context_->high = (int32_t)context_->gpr_rs() % (int32_t)context_->gpr_rt();
  }
  else {
    context_->low = context_->high = 0;
  }
}

void Cpu::DIVU() {
  if (context_->gpr_rt()) {
    context_->low  = context_->gpr_rs() / context_->gpr_rt();
    context_->high = context_->gpr_rs() % context_->gpr_rt();
  }
  else {
    context_->low = context_->high = 0;
  }
}

void Cpu::ADD() {
  context_->gpr_rd() = context_->gpr_rs() + context_->gpr_rt();
}

void Cpu::ADDU() {
  context_->gpr_rd() = context_->gpr_rs() + context_->gpr_rt();
}

void Cpu::SUB() {
  context_->gpr_rd() = context_->gpr_rs() - context_->gpr_rt();
}

void Cpu::SUBU() {
  context_->gpr_rd() = context_->gpr_rs() - context_->gpr_rt();
}

void Cpu::AND() {
  context_->gpr_rd() = context_->gpr_rs() & context_->gpr_rt();
}

void Cpu::OR() {
  context_->gpr_rd() = context_->gpr_rs() | context_->gpr_rt();
}

void Cpu::XOR() {
  context_->gpr_rd() = context_->gpr_rs() ^ context_->gpr_rt();
}

void Cpu::NOR() {
  context_->gpr_rd() = ~(context_->gpr_rs() | context_->gpr_rt());
}

void Cpu::SLT() {
  context_->gpr_rd() = (int32_t)context_->gpr_rs() < (int32_t)context_->gpr_rt();
}

void Cpu::SLTU() {
  context_->gpr_rd() = context_->gpr_rs() < context_->gpr_rt();
}

void Cpu::BLTZ() {
  bool cond = (context_->gpr_rs() & 0x80000000)==0x80000000;
  if (cond==true) {
    Branch(context_->pc + (context_->immediate_32bit_sign_extended() << 2));
  }
}

void Cpu::BGEZ() {
  bool cond = (context_->gpr_rs() & 0x80000000)==0;
  if (cond==true) {
    Branch(context_->pc + (context_->immediate_32bit_sign_extended() << 2));
  }
}

void Cpu::BLTZAL() {
  context_->gp.ra = context_->pc + 4;
  BLTZ();
}

void Cpu::BGEZAL() {
  context_->gp.ra = context_->pc + 4;
  BGEZ();
}

}
}
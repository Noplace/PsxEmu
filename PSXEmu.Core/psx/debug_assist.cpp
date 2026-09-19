/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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
#ifdef _DEBUG
#include "psx/psx.h"
//#define PROG_ONLY
//#pragma warning(disable : 4996)

namespace emulation {
namespace psx {

const char* DebugAssist::gpr[32] = {
  "zero",
  "at",
  "v0",
  "v1",
  "a0",
  "a1",
  "a2",
  "a3",
  "t0",
  "t1",
  "t2",
  "t3",
  "t4",
  "t5",
  "t6",
  "t7",
  "s0",
  "s1",
  "s2",
  "s3",
  "s4",
  "s5",
  "s6",
  "s7",
  "t8",
  "t9",
  "k0",
  "k1",
  "gp",
  "sp",
  "fp/s8",
  "ra"
};

int DebugAssist::opcode_type[160] = {
  0 , 0 , 7 , 7 , 6 , 6 , 0 , 0 ,
  3 , 3 , 3 , 3 , 3 , 3 , 3 , 2 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  1 , 1 , 1 , 1 , 1 , 1 , 1 , 0 ,
  1 , 1 , 1 , 1 , 0 , 0 , 1 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  5 , 0 , 5 , 5 , 4 , 0 , 4 , 4 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  4 , 4 , 4 , 4 , 4 , 4 , 4 , 4 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 ,
  0 , 0 , 0 , 0 , 0 , 0 , 0 , 0 
};

const char* DebugAssist::assembly_code[160] = {
  "special", "regimm ", "j,\"0x%08X\"", "jal,\"0x%08X\"", "beq,\"%s,%s,0x%08X\"", "bne,\"%s,%s,0x%08X\"", "blez   ", "bgtz   ",
  "addi,\"%s,%s,0x%04X\"", "addiu,\"%s,%s,0x%04X\"", "slti,\"%s,%s,0x%04X\"", "sltiu,\"%s,%s,0x%04X\"", "andi,\"%s,%s,0x%04X\"", "ori,\"%s,%s,0x%04X\"", "xori,\"%s,%s,0x%04X\"", "lui,\"%s,0x%04X\"",
  "cop0   ", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "lb,\"%s,0x%04X(%s)\"", "lh,\"%s,0x%04X(%s)\"", "lwl,\"%s,0x%04X(%s)\"", "lw,\"%s,0x%04X(%s)\"", "lbu,\"%s,0x%04X(%s)\"", "lhu,\"%s,0x%04X(%s)\"", "lwr,\"%s,0x%04X(%s)\"", "unknown,\"\"",
  "sb,\"%s,0x%04X(%s)\"", "sh,\"%s,0x%04X(%s)\"", "swl,\"%s,0x%04X(%s)\"", "sw,\"%s,0x%04X(%s)\"", "unknown,\"\"", "unknown,\"\"", "swr,\"%s,0x%04X(%s)\"", "unknown,\"\"",
  "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "sll,\"%s,%s,%d\"", "unknown,\"\"", "srl,\"%s,%s,%d\"", "sra,\"%s,%s,%d\"", "sllv,\"%s,%s,%s\"", "unknown,\"\"", "srlv,\"%s,%s,%s\"", "srav,\"%s,%s,%s\"",
  "jr     ", "jalr   ", "unknown,\"\"", "unknown,\"\"", "syscall", "break  ", "unknown,\"\"", "unknown,\"\"",
  "mfhi   ", "mthi   ", "mflo   ", "mtlo   ", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "mult   ", "multu  ", "div    ", "divu   ", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "add,\"%s,%s,%s\"", "addu,\"%s,%s,%s\"", "sub,\"%s,%s,%s\"", "subu,\"%s,%s,%s\"", "and,\"%s,%s,%s\"", "or,\"%s,%s,%s\"", "xor,\"%s,%s,%s\"", "nor,\"%s,%s,%s\"",
  "unknown,\"\"", "unknown,\"\"", "slt    ", "sltu   ", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "bltz   ", "bgez   ", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "bltzal ", "bgezal ", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"",
  "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\"", "unknown,\"\""
};

const char* DebugAssist::machine_instruction_main_[64] = {
  "SPECIAL", "REGIMM ", "J      ", "JAL    ", "BEQ    ", "BNE    ", "BLEZ   ", "BGTZ   ",
  "ADDI   ", "ADDIU  ", "SLTI   ", "SLTIU  ", "ANDI   ", "ORI    ", "XORI   ", "LUI    ",
  "COP0   ", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "LB     ", "LH     ", "LWL    ", "LW     ", "LBU    ", "LHU    ", "LWR    ", "UNKNOWN",
  "SB     ", "SH     ", "SWL    ", "SW     ", "UNKNOWN", "UNKNOWN", "SWR    ", "UNKNOWN",
  "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
};

const char* DebugAssist::machine_instruction_special_[64] = {
  "SLL    ", "UNKNOWN", "SRL    ", "SRA    ", "SLLV   ", "UNKNOWN", "SRLV   ", "SRAV   ",
  "JR     ", "JALR   ", "UNKNOWN", "UNKNOWN", "SYSCALL", "BREAK  ", "UNKNOWN", "UNKNOWN",
  "MFHI   ", "MTHI   ", "MFLO   ", "MTLO   ", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "MULT   ", "MULTU  ", "DIV    ", "DIVU   ", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "ADD    ", "ADDU   ", "SUB    ", "SUBU   ", "AND    ", "OR     ", "XOR    ", "NOR    ",
  "UNKNOWN", "UNKNOWN", "SLT    ", "SLTU   ", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN"
};

const char* DebugAssist::machine_instruction_regimm_[32] = {
  "BLTZ   ", "BGEZ   ", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "BLTZAL ", "BGEZAL ", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN",
  "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN", "UNKNOWN"
};


DebugAssist::DebugAssist(void):fp(NULL) {

}


DebugAssist::~DebugAssist(void) {
  Close();
}

void DebugAssist::Open(const char* filename) {
  
    return;//dont need it at the moment
    char fullpath[256];
  sprintf(fullpath,"PsxDebug\\%s",filename);
  fp = fopen(fullpath,"w");
  char date_str[128];
  char time_str[128];
  _strdate_s(date_str,128);
  _strtime_s(time_str,128);
  
  fprintf(fp,"start of run @ %s - %s\n",date_str,time_str);
}

void DebugAssist::Close() {
    return;//dont need it at the moment
  if (fp != NULL) {
    fclose(fp);
    fp = NULL;
  }
}

void DebugAssist::OutputCSVHeader() {
    return;//dont need it at the moment
    //fprintf(system_->csvlog.fp,"\"Counter\",\"PC\",\"Opcode\",\"Code\",\"RS Index\",\"RS Value\",\"RT Index\",\"RT Value\",\"RD Index\",\"RD Value\",\"imm\",\"jump address\",\"branch address\",\"l/s address\"\n");
    
    if (system_->csvlog.fp) {
      fprintf(system_->csvlog.fp,"\"Counter\",\"PC\",\"Opcode\",\"Params\",\"\",");
      fprintf(system_->csvlog.fp,"\"RS Index\",\"RS Value\",\"RT Index\",\"RT Value\",\"RD Index\",\"RD Value\",\"imm\",\"jump address\",\"branch address\",\"l/s address\"\n");
      /*for (int i=0;i<32;++i) {
        fprintf(system_->csvlog.fp,"\"r%d(%s)\",",i,DebugAssist::gpr[i]);
      }*/
      fprintf(system_->csvlog.fp,"\n");
    }
}

void DebugAssist::OutputInstruction() {
    return;//dont need it at the moment
  Cpu& cpu = system_->cpu_;
  CpuContext* context = system_->cpu_.context_;
  const char* inst_str = DebugAssist::machine_instruction_main_[context->opcode()];
  const char* sp_str = DebugAssist::machine_instruction_special_[context->code&0x3f];
  const char* rm_str = DebugAssist::machine_instruction_regimm_[cpu.rt_];
  if (context->opcode() == 0)
    inst_str = sp_str;
  if (context->opcode() == 1)
    inst_str = rm_str;

  uint32_t address0 = (context->pc & 0xF0000000) | (cpu.target_ << 2);
  uint32_t address1 = context->pc + (cpu.immediate_32bit_sign_extended_ << 2);
  uint32_t address2 = context->gp.reg[cpu.rs_] + cpu.immediate_32bit_sign_extended_;
  if (system_->csvlog.fp != NULL)
    fprintf(fp,"\"0x%08X\",\"0x%08X\",\"%s\",0x%08X,%d,0x%08X,%d,0x%08X,%d,0x%08X,\"u0x%08X s0x%08X\",0x%08X,0x%08X,0x%08X\n",
      cpu.index,context->prev_pc,inst_str,context->code,context->rs(),
      context->gp.reg[cpu.rs_],cpu.rt_,context->gp.reg[cpu.rt_],
      context->rd(),context->gp.reg[cpu.rd_],cpu.immediate_,
      cpu.immediate_32bit_sign_extended_,address0,address1,address2); 
}

void DebugAssist::OutputInstruction2() {
    return;//dont need it at the moment
  Cpu& cpu = system_->cpu_;
  CpuContext* context = cpu.context_;
  #ifdef PROG_ONLY
    if (context->prev_pc < 0x80000000 || context->prev_pc >= 0xBFC00000) {
      return;
    }
  #endif

  if (cpu.__inside_delay_slot == true)
    fprintf(system_->csvlog.fp,"following is delay slot\n");

  int opcode = cpu.opcode_;
  if (opcode == 0)
    opcode += 64 +  system_->cpu_.funct_;
  if (opcode == 1)
    opcode += 64+32+  system_->cpu_.rt_;
  const char* assembly = assembly_code[opcode];
  char outputline[256];
  if (cpu.context_->code == 0) {
    sprintf(outputline,"nop,");
  }
  else {
    switch (DebugAssist::opcode_type[opcode]) {
      case 0:
        sprintf(outputline,assembly);
        break;
      case 1:
        sprintf(outputline,assembly,gpr[cpu.rt_],cpu.immediate_,gpr[cpu.rs_]);
        break;
      case 2:
        sprintf(outputline,assembly,gpr[cpu.rt_],cpu.immediate_);
        break;
      case 3:
        sprintf(outputline,assembly,gpr[cpu.rt_],gpr[cpu.rs_],cpu.immediate_);
        break;
      case 4:
        sprintf(outputline,assembly,gpr[cpu.rd_],gpr[cpu.rs_],gpr[cpu.rt_]);
        break;
      case 5:
        sprintf(outputline,assembly,gpr[cpu.rd_],gpr[cpu.rt_],cpu.shamt_);
        break;
      case 6: { //branchs
        uint32_t target = context->pc + (cpu.immediate_32bit_sign_extended_ << 2);
        sprintf(outputline,assembly,gpr[cpu.rs_],gpr[cpu.rt_],target);
        break;
      }
      case 7: { //Jump
        uint32_t target = (context->pc & 0xF0000000) | (cpu.target_ << 2);
        sprintf(outputline,assembly,target);
        break;
      }
    }
  }
  if (system_->csvlog.fp != NULL) {
    fprintf(fp,"\"0x%08X\",\"0x%08X\",%s,\"\",",cpu.index,context->prev_pc,outputline);
    /*for (int i=0;i<32;++i) {
      fprintf(fp,"\"0x%08x\",",i,context->gp.reg[i]);
    }*/

  uint32_t address0 = (context->pc & 0xF0000000) | (cpu.target_ << 2);
  uint32_t address1 = context->pc + (cpu.immediate_32bit_sign_extended_ << 2);
  uint32_t address2 = context->gp.reg[cpu.rs_] + cpu.immediate_32bit_sign_extended_;
  if (system_->csvlog.fp != NULL)
    fprintf(fp,"%s,0x%08X,%s,0x%08X,%s,0x%08X,\"u0x%08X s0x%08X\",0x%08X,0x%08X,0x%08X",
      gpr[cpu.rs_],
      context->gp.reg[cpu.rs_],gpr[cpu.rt_],context->gp.reg[cpu.rt_],
      gpr[cpu.rd_],context->gp.reg[cpu.rd_],cpu.immediate_,
      cpu.immediate_32bit_sign_extended_,address0,address1,address2); 

    fprintf(fp,"\n");
  }
  /*
  char* inst_str = DebugAssist::machine_instruction_main_[context->opcode()];
  char* sp_str = DebugAssist::machine_instruction_special_[context->code&0x3f];
  char* rm_str = DebugAssist::machine_instruction_regimm_[cpu.rt_];
  if (context->opcode() == 0)
    inst_str = sp_str;
  if (context->opcode() == 1)
    inst_str = rm_str;

  uint32_t address0 = (context->pc & 0xF0000000) | (cpu.target_ << 2);
  uint32_t address1 = context->pc + (cpu.immediate_32bit_sign_extended_ << 2);
  uint32_t address2 = context->gp.reg[cpu.rs_] + cpu.immediate_32bit_sign_extended_;
  if (system_->csvlog.fp != NULL)
    fprintf(fp,"\"0x%08X\",\"0x%08X\",\"%s\",0x%08X,%d,0x%08X,%d,0x%08X,%d,0x%08X,\"u0x%08X s0x%08X\",0x%08X,0x%08X,0x%08X\n",
      cpu.index,context->prev_pc,inst_str,context->code,context->rs(),
      context->gp.reg[cpu.rs_],cpu.rt_,context->gp.reg[cpu.rt_],
      context->rd(),context->gp.reg[cpu.rd_],cpu.immediate_,
      cpu.immediate_32bit_sign_extended_,address0,address1,address2); */
  if (cpu.__inside_delay_slot == true)
   fprintf(system_->csvlog.fp,"end of delay slot\n");

}

}
}
#endif
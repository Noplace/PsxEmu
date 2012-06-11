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
#include "../system.h"

bool output_inst = false;
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
  /*DecodeIType dit;
  decoders[0] = dit;
  DecodeJType djt;
  decoders[1] = djt;
  DecodeRType drt;
  decoders[2] = drt;
  */
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
    system_->csvlog.Close();
  #endif
}


int Cpu::Initialize() {
  cpu_ = this;
  icache_.Initialize();
  dcache_.Initialize();

  index = 0;
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    system_->csvlog.OutputCSVHeader();
  #endif
  current_stage = 0;
  context_->gp.zero = 0;
  context_->ctrl.PRId = 0x00000002;
  //context_->ctrl.PRId = 3 << 8; //R3000A
  context_->ctrl.SR.raw = 0x10900000;//1111
  context_->cycles = 0;
  context_->current_cycles = 0;
  return 0;
}

int Cpu::Deinitialize() {
  dcache_.Deinitialize();
  icache_.Deinitialize();
  return 0;
}


void Cpu::ExecuteInstruction() {
  context_->prev_pc = context_->pc;
  context_->gp.zero = 0; //make sure r0 is always 0.
  //PC_BREAKPOINT(0xBFC01920);
  //PC_BREAKPOINT(0xBFC0194C);
  StageIF();
  StageRD();
  current_stage = 3;
  #if defined(_DEBUG) && defined(CPU_DEBUG) && defined(CSVOUT)
    if (output_inst == true) {
      system_->csvlog.OutputInstruction2();
    }
  #endif
  index++;
  __inside_instruction = true;
  (this->*(machine_instruction_main_[opcode_]))();
  __inside_instruction = false;

  ++context_->cycles;
  ++context_->current_cycles;

    if (context_->pc == 0xa0 || 
        context_->pc == 0xb0 || 
        context_->pc == 0xc0) {
          //bios call
          system_->kernel().Call();
          int a= 1;
    }
}

void Cpu::RaiseException(uint32_t address, Exceptions exception, ExceptionCodes code) {
  #if defined(_DEBUG) && defined(CPU_DEBUG)
    if(system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,Exception,address,0x%08X,exception,0x%08X,code=0x%08X,SR,0x%08X\n",index,context_->prev_pc,address,exception,code,context_->ctrl.SR);
  #endif
  //save to epc
  context_->ctrl.EPC = context_->branch_flag == true ? address-4 : address;

  //push the bit stack for kernel,interrupt flags
  uint32_t& sr = context_->ctrl.SR.raw;
  sr = (sr & ~0x3F) | ((sr & 0xF) << 2) | 0x2;


  //set cause
  //todo : set ip flags correctly
  uint32_t& cause = context_->ctrl.Cause;
  cause = 0;
  cause |= context_->branch_flag == true ? 0x80000000 : 0;
  cause |= (code&0x1F)<<2;
  if (code == kExceptionCodeInt)
    cause |= (sr&0xFF00);


  //specific exception handling
  if (exception == kTLBMissException) {
    if ((context_->ctrl.SR.BEV) == 0) //BEV = 0
      context_->pc = 0x80000000;
    else
      context_->pc = 0xBFC00100;
  }

  if (exception == kOtherException) {
    if ((context_->ctrl.SR.BEV) == 0) //BEV = 0
      context_->pc = 0x80000080;
    else
      context_->pc = 0xBFC00180;
  }

  if (exception == kResetException) {
    //default state : 0101 0000 0110 0001 0000 0000 0000 0000
    context_->ctrl.SR.raw = 0x10900000;//0x50610000;
    context_->ctrl.PRId = 0x00000002;
    context_->pc = 0xBFC00000;
  }

}

void Cpu::Tick() {
  ++context_->cycles;
  //system_->io().Tick(1);
}

uint32_t Cpu::LoadMemory(bool cached, int size_bytes, uint32_t physical_address, uint32_t virtual_address) {
  if (IsBusError() == true) {
    context_->ctrl.BadVaddr = context_->prev_pc;
    auto code = current_stage == 1 ? kExceptionCodeIBE : kExceptionCodeDBE;
    RaiseException(context_->prev_pc,kOtherException,code);
    return 0;
  }

  if (IsAddressError(virtual_address,size_bytes) == true) {
    context_->ctrl.BadVaddr = virtual_address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }

  if ((context_->ctrl.SR.IsC) && current_stage != 1) { //cache isolation
    uint32_t data;
    if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      dcache_.Read(physical_address,data);
      if (size_bytes != 4)
        dcache_.InvalidateLine(physical_address);
    } else {
      icache_.Read(physical_address,data);
      if (size_bytes != 4)
        icache_.InvalidateLine(physical_address);
    }
    return data;
  }

  

  Buffer* buffer = nullptr;
  uint32_t target_address=0;
  if (physical_address >= 0x1FC00000 && physical_address <= 0x1FC80000) {
    buffer = &system_->io().bios_buffer;
    target_address = physical_address & 0x0007FFFF;
  }

  if (physical_address >= 0x00000000 && physical_address <= 0x001FFFFF) {
    buffer = &system_->io().ram_buffer;
    target_address = physical_address & 0x001FFFFF;
  }

  if (physical_address >= 0x1F000000 && physical_address <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    target_address = physical_address & 0x0000FFFF;
  }

  if (physical_address >= 0x1F800000 && physical_address <= 0x1F8003FF) {
    return dcache_.lines[physical_address&0x3FF].data[0];
  }

  if (physical_address >= 0x1F801000 && physical_address <= 0x1F802FFF) {
    switch (size_bytes) {
      case 1: return system_->io().Read08(physical_address);
      case 2: return system_->io().Read16(physical_address);
      case 4: return system_->io().Read32(physical_address);
    }
  }
  
  if (buffer != nullptr) {
    if (cached == true) {
      uint32_t data;
      auto cache_hit = icache_.Read(physical_address,data);

      if (cache_hit == true) {
        uint32_t mask[] = {0x0,0xFF,0xFFFF,0xFFFFFF,0xFFFFFFFF};
        data = ( data >> ((physical_address&0x3)<<3)) & mask[size_bytes];
        return data;
      }
      else {
        icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
        //Tick();Tick();Tick();Tick();Tick();Tick();
      }

      /*//if (cache_hit == true)
      //  return data&((1<<(8<<(size_bytes>>1)))-1);
      //fprintf(system_->csvlog.fp,",,cache read,0x%08x,cache data,0x%08X,actual data,0x%08X,hit=%d\n",physical_address,data,buffer->u32[target_address>>2],cache_hit);
      //if (icache_.Read(physical_address,data)==true)//cache hit
      //  return data;
      if (size_bytes == 4 && data!=buffer->u32[target_address>>2] && cache_hit == true) {
        BREAKPOINT
      }
      if (size_bytes == 2 && data!=buffer->u16[target_address>>1] && cache_hit == true) {
        BREAKPOINT
      }
      if (size_bytes == 1 && data!=buffer->u8[target_address] && cache_hit == true) {
        BREAKPOINT
      }*/
    }

    switch (size_bytes) {
      case 1: return buffer->u8[target_address];
      case 2: return buffer->u16[target_address>>1];
      case 4: return buffer->u32[target_address>>2];
    }
  }

  BREAKPOINT
  return 0;
}

void Cpu::StoreMemory(bool cached, int size_bytes,uint32_t data, uint32_t physical_address, uint32_t virtual_address) {
  //todo: research about this value, ignore for now
  if (IsBusError() == true) { 
    context_->ctrl.BadVaddr = context_->prev_pc;
    return;
  }
  if ((IsAddressError(virtual_address,size_bytes) == true)) {
    context_->ctrl.BadVaddr = context_->prev_pc;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }

  if (context_->ctrl.SR.IsC) { //cache isolation
    uint32_t cdata[4] = { data };
    if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      dcache_.Write(physical_address,cdata);
      if (size_bytes != 4)
        dcache_.InvalidateLine(physical_address);
    } else {
      icache_.Write(physical_address,cdata);
      if (size_bytes != 4)
        icache_.InvalidateLine(physical_address);
    }
    return;
  }

  Buffer* buffer = nullptr;
  uint32_t target_address=0;
  if (physical_address >= 0x1FC00000 && physical_address <= 0x1FC80000) {
    buffer = &system_->io().bios_buffer;
    target_address = physical_address & 0x0007FFFF;
  }

  if (physical_address >= 0x00000000 && physical_address <= 0x001FFFFF) {
    buffer = &system_->io().ram_buffer;
    target_address = physical_address & 0x001FFFFF;
  }

  if (physical_address >= 0x1F000000 && physical_address <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    target_address = physical_address & 0x0000FFFF;
  }

   if (physical_address >= 0x1F800000 && physical_address <= 0x1F8003FF) {
    dcache_.lines[physical_address&0x3FF].data[0] = data;
    return;
  }

  if (physical_address >= 0x1F801000 && physical_address <= 0x1F802FFF) {
    switch (size_bytes) {
      case 1: system_->io().Write08(physical_address,data&0xFF); return;
      case 2: system_->io().Write16(physical_address,data&0xFFFF); return;
      case 4: system_->io().Write32(physical_address,data); return;
    }
  }
  
  if (buffer != nullptr) {
    switch (size_bytes) {
      case 1: buffer->u8[target_address] = data; break;
      case 2: buffer->u16[target_address>>1] = data; break;
      case 4: buffer->u32[target_address>>2] = data; break;
    }
    if (cached == true) {
      //fprintf(system_->csvlog.fp,",,cache write,0x%08x,cache data,,actual data,0x%08X\n",physical_address,data);
      icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
    }
    return;
  }

  BREAKPOINT
}

void Cpu::StageIF() {
  current_stage = 1;
  auto ppc = AddressTranslation(context_->pc);
  context_->code = LoadMemory(cache_flag_,4,ppc,context_->pc);
  context_->pc  += 4;
}

void Cpu::StageRD() {
  current_stage = 2;
  opcode_ = context_->opcode();
  immediate_ = context_->immediate();
  immediate_32bit_sign_extended_ = context_->immediate_32bit_sign_extended();
  target_ = context_->target();
  funct_ = context_->fu();
  shamt_ = context_->sa();
  rd_ = context_->rd();
  rt_ = context_->rt();
  rs_ = context_->rs();
}

void Cpu::Jump(uint32_t address) {
  __inside_delay_slot = true;
  ExecuteInstruction();
  __inside_delay_slot = false;
  context_->pc = address;
}

void Cpu::UNKNOWN() {
  #if defined(_DEBUG) && defined(CPU_DEBUG)
    if(system_->csvlog.fp)
      fprintf(system_->csvlog.fp,"unknown intstruction @ 0x%08X code=0x%08X\n",context_->prev_pc,context_->code);
  #endif
}

void Cpu::SPECIAL() {
  (this->*(machine_instruction_special_[funct_]))();
}

void Cpu::REGIMM() {
  (this->*(machine_instruction_regimm_[rt_]))();
}

void Cpu::J() {
  Tick();
  Jump((context_->pc & 0xF0000000) | (target_ << 2));
}

void Cpu::JAL() {
  context_->gp.ra = context_->pc + 4;
  Tick();
  Jump((context_->pc & 0xF0000000) | (target_ << 2));
}

void Cpu::BEQ() {
  if (context_->gp.reg[rs_] == context_->gp.reg[rt_]) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::BNE() {
  if (context_->gp.reg[rs_] != context_->gp.reg[rt_]) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::BLEZ() {
  int32_t r = (int32_t)context_->gp.reg[rs_];
  bool cond = r <= 0; //((context_->gp.reg[rs_] & 0x80000000)==1) || (context_->gp.reg[rs_] = 0 );
  if (cond==true) {//context_->gp.reg[rs_] <= 0) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::BGTZ() {
  int32_t r = (int32_t)context_->gp.reg[rs_];
  bool cond = r > 0;//((context_->gp.reg[rs_] & 0x80000000)==0) && (context_->gp.reg[rs_] != 0 );
  if (cond==true) {//context_->gp.reg[rs_] > 0) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::ADDI() {
  context_->gp.reg[rt_] = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  Tick();
}

void Cpu::ADDIU() {
  context_->gp.reg[rt_] = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  Tick();
}

void Cpu::SLTI() {
  context_->gp.reg[rt_] = (int32_t)context_->gp.reg[rs_] < immediate_32bit_sign_extended_;
  Tick();
}

void Cpu::SLTIU() {
  context_->gp.reg[rt_] = context_->gp.reg[rs_] < immediate_;
  Tick();
}


void Cpu::ANDI() {
  context_->gp.reg[rt_] = context_->gp.reg[rs_] & immediate_;
  Tick();
}

void Cpu::ORI() {
  context_->gp.reg[rt_] = context_->gp.reg[rs_] | immediate_;
  Tick();
}

void Cpu::XORI() {
  context_->gp.reg[rt_] = context_->gp.reg[rs_] ^ immediate_;
  Tick();
}

void Cpu::LUI() {
  context_->gp.reg[rt_] = immediate_ << 16;
  Tick();
  //context_->gp.reg[rt_] = context_->immediate_ << 16;
}

void Cpu::COP0() {
  switch (context_->rs()) {
    //MFC
    case 0x00: {
      context_->gp.reg[rt_] = context_->ctrl.reg[rd_];
      break;
    }
    //MTC
    case 0x04: {
      context_->ctrl.reg[rd_] = context_->gp.reg[rt_];
      break;
    }
    //RFE
    case 0x10: {
      context_->ctrl.SR.raw = (context_->ctrl.SR.raw & ~0xF) | ((context_->ctrl.SR.raw >> 2) & 0xF);
      break;
    }
    default:
      BREAKPOINT;
  }
  Tick();
}

void Cpu::COP2() {
  BREAKPOINT
}

void Cpu::LB() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint8_t mem = LoadMemory(cache_flag_,1,physical_address,virtual_address);
  Tick();
  context_->gp.reg[rt_] = (int8_t)mem;
  Tick();
}

void Cpu::LH() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint16_t mem = LoadMemory(cache_flag_,2,physical_address,virtual_address);
  Tick();
  context_->gp.reg[rt_] = (int16_t)mem;
  Tick();
}

void Cpu::LWL() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = LoadMemory(cache_flag_,4,physical_address & ~0x03,virtual_address & ~0x03);
  Tick();
  switch (virtual_address & 0x3) {
    case 0:
      context_->gp.reg[rt_] = (context_->gp.reg[rt_] & 0x00FFFFFF) | (mem<<24);
      break;
    case 1:
      context_->gp.reg[rt_] = (context_->gp.reg[rt_] & 0x0000FFFF) | (mem<<16);
      break;
    case 2:
      context_->gp.reg[rt_] = (context_->gp.reg[rt_] & 0x000000FF) | (mem<<8);
      break;
    case 3:
      context_->gp.reg[rt_] = mem;
      break;
  }
  Tick();
}

void Cpu::LW() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem;
  mem = LoadMemory(cache_flag_,4,physical_address,virtual_address);
  Tick();
  context_->gp.reg[rt_] = mem;
  Tick();
}

void Cpu::LBU() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = LoadMemory(cache_flag_,1,physical_address,virtual_address);
  Tick();
  context_->gp.reg[rt_] = (uint8_t)mem;
  Tick();
}

void Cpu::LHU() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = LoadMemory(cache_flag_,2,physical_address,virtual_address);
  Tick();
  context_->gp.reg[rt_] = (uint16_t)mem;
  Tick();
}

void Cpu::LWR() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = LoadMemory(cache_flag_,4,physical_address & ~0x03,virtual_address & ~0x03);
  Tick();
  switch (virtual_address & 0x3) {
    case 0:
      context_->gp.reg[rt_] = mem;
      break;
    case 1:
      context_->gp.reg[rt_] = (context_->gp.reg[rt_] & 0xFF000000) | (mem>>8);
      break;
    case 2:
      context_->gp.reg[rt_] = (context_->gp.reg[rt_] & 0xFFFF0000) | (mem>>16);
      break;
    case 3:
      context_->gp.reg[rt_] = (context_->gp.reg[rt_] & 0xFFFFFF00) | (mem>>24);
      break;
  }
  Tick();
}

void Cpu::SB() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  StoreMemory(cache_flag_,1,context_->gp.reg[rt_],physical_address,virtual_address);
  Tick();
}

void Cpu::SH() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  StoreMemory(cache_flag_,2,context_->gp.reg[rt_],physical_address,virtual_address);
  Tick();
}

void Cpu::SWL() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t data;
  switch (virtual_address & 0x3) {
    case 0:
      data = (data & 0xFFFFFF00) | (context_->gp.reg[rt_] >> 24);
      break;
    case 1:
      data = (data & 0xFFFF0000) | (context_->gp.reg[rt_] >> 16);
      break;
    case 2:
      data = (data & 0xFF000000) | (context_->gp.reg[rt_] >> 8);
      break;
    case 3:
      data = context_->gp.reg[rt_];
      break;
  }
  StoreMemory(cache_flag_,4,data,physical_address & ~0x03,virtual_address & ~0x03);
  Tick();
}

void Cpu::SW() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  StoreMemory(cache_flag_,4,context_->gp.reg[rt_],physical_address,virtual_address);
  Tick();
}

void Cpu::SWR() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t data;
  switch (virtual_address & 0x3) {
    case 0:
      data = context_->gp.reg[rt_];
      break;
    case 1:
      data = (data & 0x000000FF) | (context_->gp.reg[rt_] << 8);
      break;
    case 2:
      data = (data & 0x0000FFFF) | (context_->gp.reg[rt_] << 16);
      break;
    case 3:
      data = (data & 0x00FFFFFF) | (context_->gp.reg[rt_] << 24);
      break;
  }
  StoreMemory(cache_flag_,4,data,physical_address & ~0x03,virtual_address & ~0x03);
  Tick();
}

void Cpu::SLL() {
  context_->gp.reg[rd_] = context_->gp.reg[rt_] << shamt_;
  Tick();
}

void Cpu::SRL() {
  context_->gp.reg[rd_] = context_->gp.reg[rt_] >> shamt_;
  Tick();
}

void Cpu::SRA() {
  context_->gp.reg[rd_] = (int32_t)context_->gp.reg[rt_] >> shamt_;
  Tick();
}

void Cpu::SLLV() {
  context_->gp.reg[rd_] = context_->gp.reg[rt_] << (context_->gp.reg[rs_] & 0x1F);
  Tick();
}

void Cpu::SRLV() {
 context_->gp.reg[rd_] = context_->gp.reg[rt_] >> (context_->gp.reg[rs_] & 0x1F);
 Tick();
}

void Cpu::SRAV() {
  context_->gp.reg[rd_] = (int32_t)context_->gp.reg[rt_] >> (context_->gp.reg[rs_] & 0x1F);
  Tick();
}

void Cpu::JR() {
  Jump(context_->gp.reg[rs_]);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;
}

void Cpu::JALR() {
  context_->gp.reg[rd_] = context_->pc + 4; // rd must be 31
  Jump(context_->gp.reg[rs_]);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;

}

void Cpu::SYSCALL() {
  RaiseException(context_->prev_pc,kOtherException,kExceptionCodeSyscall);
}

void Cpu::BREAK() {
  BREAKPOINT
}

void Cpu::MFHI() {
  context_->gp.reg[rd_] = context_->high;
  Tick();
}

void Cpu::MTHI() {
  context_->high  = context_->gp.reg[rs_];
  Tick();
}

void Cpu::MFLO() {
  context_->gp.reg[rd_] = context_->low;
  Tick();
}

void Cpu::MTLO() {
  context_->low  = context_->gp.reg[rs_];
  Tick();
}

void Cpu::MULT() {
  uint64_t test = int64_t((int64_t)((int32_t)context_->gp.reg[rs_]) * (int64_t)((int32_t)context_->gp.reg[rt_]));
  context_->low  = (uint32_t)(test & 0xFFFFFFFF);
  context_->high = (uint32_t)((test >> 32) & 0xFFFFFFFF);
  Tick();
}

void Cpu::MULTU() {
  uint64_t test = uint64_t((uint64_t)((uint32_t)context_->gp.reg[rs_]) * (uint64_t)((uint32_t)context_->gp.reg[rt_]));
  context_->low  = (uint32_t)(test & 0xFFFFFFFF);
  context_->high = (uint32_t)((test >> 32) & 0xFFFFFFFF);
  Tick();
}

void Cpu::DIV() {
  if (context_->gp.reg[rt_]) {
    context_->low  = (int32_t)context_->gp.reg[rs_] / (int32_t)context_->gp.reg[rt_];
    context_->high = (int32_t)context_->gp.reg[rs_] % (int32_t)context_->gp.reg[rt_];
  }
  else {
    context_->low = context_->high = 0;
  }
  Tick();
}

void Cpu::DIVU() {
  if (context_->gp.reg[rt_]) {
    context_->low  = context_->gp.reg[rs_] / context_->gp.reg[rt_];
    context_->high = context_->gp.reg[rs_] % context_->gp.reg[rt_];
  }
  else {
    context_->low = context_->high = 0;
  }
  Tick();
}

void Cpu::ADD() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] + context_->gp.reg[rt_];
  Tick();
}

void Cpu::ADDU() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] + context_->gp.reg[rt_];
  Tick();
}

void Cpu::SUB() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] - context_->gp.reg[rt_];
  Tick();
}

void Cpu::SUBU() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] - context_->gp.reg[rt_];
  Tick();
}

void Cpu::AND() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] & context_->gp.reg[rt_];
  Tick();
}

void Cpu::OR() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] | context_->gp.reg[rt_];
  Tick();
}

void Cpu::XOR() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] ^ context_->gp.reg[rt_];
  Tick();
}

void Cpu::NOR() {
  context_->gp.reg[rd_] = ~(context_->gp.reg[rs_] | context_->gp.reg[rt_]);
  Tick();
}

void Cpu::SLT() {
  context_->gp.reg[rd_] = (int32_t)context_->gp.reg[rs_] < (int32_t)context_->gp.reg[rt_];
  Tick();
}

void Cpu::SLTU() {
  context_->gp.reg[rd_] = context_->gp.reg[rs_] < context_->gp.reg[rt_];
  Tick();
}

void Cpu::BLTZ() {
  int32_t r = (int32_t)context_->gp.reg[rs_] ;
  bool cond = r < 0; //(context_->gp.reg[rs_] & 0x80000000)==0x80000000;
  if (cond==true) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::BGEZ() {
  int32_t r = (int32_t)context_->gp.reg[rs_] ;
  bool cond = r >= 0;//(context_->gp.reg[rs_] & 0x80000000)==0;
  if (cond==true) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
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
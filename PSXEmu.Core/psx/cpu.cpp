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
#include "psx/psx.h"

bool output_inst = false;
uint32_t until_address =0;
#define CSVOUT
#define CPU_DEBUG
//#define BIOSCALL

namespace emulation {
namespace psx {

Buffer* ICache2::GetBufferAndOffset(uint32_t address, uint32_t& output_offset) {
  uint32_t pc_bank, pc_offset, pc_cache;
  pc_bank = address >> 24;
  pc_offset = address & 0xffffff;
  pc_cache = address & 0xfff;
  
  if ((system_->io().io.cache_control & 0x800) == 0) {
    if (pc_bank == 0xA0 || pc_bank == 0x80|| pc_bank == 0x00) {
      output_offset = address & 0x001FFFFF;
      return &system_->io().ram_buffer;
    } else if (pc_bank == 0x1F || pc_bank == 0x9F|| pc_bank == 0xBF) {
      output_offset = address & 0x0007FFFF;
      return &system_->io().bios_buffer;
    } else {
      return nullptr;
    }
  }

  if (pc_bank == 0xA0) {//non cache segments
    output_offset = address & 0x001FFFFF;
    return &system_->io().ram_buffer;
  }
  if (pc_bank == 0xBF) {//non cache segments
    output_offset = address & 0x0007FFFF;
    return &system_->io().bios_buffer;
  }
  if (addresses[pc_cache] == pc_offset) {
    output_offset = pc_cache;
    return &buffer;
  } else {
    addresses[pc_cache] = pc_offset;
    buffer.u32[pc_cache>>2] = system_->io().ram_buffer.u32[(address & 0x001FFFFF)>>2];
    output_offset = pc_cache;
    return &buffer;
  }
  
    
}

Cpu::Instruction Cpu::machine_instruction_main_[64] = {
  &Cpu::SPECIAL, &Cpu::REGIMM , &Cpu::J      , &Cpu::JAL    , &Cpu::BEQ    , &Cpu::BNE    , &Cpu::BLEZ   , &Cpu::BGTZ   ,
  &Cpu::ADDI   , &Cpu::ADDIU  , &Cpu::SLTI   , &Cpu::SLTIU  , &Cpu::ANDI   , &Cpu::ORI    , &Cpu::XORI   , &Cpu::LUI    ,
  &Cpu::COP0   , &Cpu::UNKNOWN, &Cpu::COP2   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::LB     , &Cpu::LH     , &Cpu::LWL    , &Cpu::LW     , &Cpu::LBU    , &Cpu::LHU    , &Cpu::LWR    , &Cpu::UNKNOWN,
  &Cpu::SB     , &Cpu::SH     , &Cpu::SWL    , &Cpu::SW     , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SWR    , &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::LWC2   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
  &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::SWC2   , &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN, &Cpu::UNKNOWN,
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
  icache.set_system(system_);
  icache.Initialize();
  //dcache_.Initialize();

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
  // Nothing is in flight on a cold start, and leaving a stale record here
  // would write a register during the first instruction after a reset.
  pending_load_ = PendingLoad();
  armed_load_ = PendingLoad();
  return 0;
}

int Cpu::Deinitialize() {
//  dcache_.Deinitialize();
  icache.Deinitialize();
  return 0;
}

void Cpu::NoteExternalWrite(uint32_t tag, uint32_t byte_address,
                            uint32_t value) {
  if (watch_address_ == 0)
    return;
  const uint32_t low = byte_address & 0x1FFFFF;
  const uint32_t target = watch_address_ & 0x1FFFFF;
  if (low + 4 <= target || low >= target + 4)
    return;
  Watch& w = watch_[watch_count_ % kWatchCapacity];
  w.pc = tag;
  w.address = byte_address;
  w.value = value;
  w.size = 4;
  ++watch_count_;
}

void Cpu::DumpTrace(const char* filename, ExceptionCodes code) {
  FILE* fp = fopen(filename, "w");
  if (!fp) return;

  fprintf(fp, "--- PSX Execution Trace ---\n");
  for (int i = 0; i < kTraceSize; ++i) {
    int idx = (trace_index_ + i) % kTraceSize;
    if (trace_buffer_[idx].pc == 0) continue; // Skip uninitialized entries

    uint32_t pc = trace_buffer_[idx].pc;
    uint32_t inst = trace_buffer_[idx].instruction;
    
    // Simple hex dump for now, decoding requires more setup, 
    // but the PC and Instruction hex are enough to trace loops.
    fprintf(fp, "[%05d] PC: 0x%08X  Inst: 0x%08X\n", i, pc, inst);
  }
  
  fprintf(fp, "--- Exception State ---\n");
  fprintf(fp, "Caught Code: 0x%02X\n", code);
  fprintf(fp, "EPC: 0x%08X\n", context_->ctrl.EPC);
  fprintf(fp, "Cause: 0x%08X\n", context_->ctrl.Cause);
  fprintf(fp, "BadVaddr: 0x%08X\n", context_->ctrl.BadVaddr);
  fprintf(fp, "a0: 0x%08X\n", context_->gp.reg[4]);
  fprintf(fp, "a1: 0x%08X\n", context_->gp.reg[5]);
  fprintf(fp, "a2: 0x%08X\n", context_->gp.reg[6]);
  fprintf(fp, "a3: 0x%08X\n", context_->gp.reg[7]);
  fprintf(fp, "v0: 0x%08X\n", context_->gp.reg[2]);
  fprintf(fp, "v1: 0x%08X\n", context_->gp.reg[3]);
  fprintf(fp, "ra: 0x%08X\n", context_->gp.reg[31]);

  fprintf(fp, "--- End of Trace ---\n");
  fclose(fp);
}


// Moves the load pipeline on by one instruction.
//
// Both halves of the load delay are here. The value a load promised lands in
// the register file at the start of the second instruction after it, so the
// one in between reads whatever the register held before - which is the whole
// point, and what software written for this CPU expects.
//
// Doing this at the start of an instruction rather than at the end is not a
// detail. A branch runs its delay slot as a nested ExecuteInstruction, so
// end-of-instruction bookkeeping would run the slot's before the branch's,
// out of program order, and a load two instructions before a branch would
// reach its register one instruction late.
void Cpu::AdvanceLoadDelay() {
  if (pending_load_.active) {
    if (pending_load_.reg != 0)
      context_->gp.reg[pending_load_.reg] = pending_load_.value;
    pending_load_.active = false;
  }
  if (armed_load_.active) {
    pending_load_ = armed_load_;
    armed_load_.active = false;
  }
}
void Cpu::ExecuteInstruction() {
  context_->prev_pc = context_->pc;
  AdvanceLoadDelay();
  context_->gp.zero = 0; //make sure r0 is always 0.
  
  /*if (context_->pc == 0x000000A0 && context_->gp.t1 == 0x40) {
    // Automatically dump trace when SystemError A0(0x40) is hit
    DumpTrace("trace_crash.txt");
    // To prevent infinite dumping, change t1 so it doesn't match again immediately
    context_->gp.t1 = 0; 
  }*/

  StageIF();
  
  //if trace required, record instruction in ring buffer
  //if (1 == 0)
  {
      // Record instruction in ring buffer
      trace_buffer_[trace_index_].pc = context_->prev_pc;
      trace_buffer_[trace_index_].instruction = context_->code;
      trace_index_ = (trace_index_ + 1) % kTraceSize;
  }

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
      fprintf_s(system_->csvlog.fp,"0x%08X,0x%08X,Exception,address,0x%08X,exception,0x%08X,code=0x%08X,SR,0x%08X\n",index,context_->prev_pc,address,exception,code,context_->ctrl.SR.raw);
  #endif
  //save to epc
  context_->ctrl.EPC = context_->branch_flag == true ? address-4 : address;

  if (code == kExceptionCodeDBE || code == kExceptionCodeIBE || 
      code == kExceptionCodeAdEL || code == kExceptionCodeAdES || 
      code == kExceptionCodeRI || code == kExceptionCodeBp || code == kExceptionCodeCpU) {
    static bool dumped = false;
    if (!dumped) {
      DumpTrace("trace_fatal_exception.txt", code);
      dumped = true;
    }
  }

  //push the bit stack for kernel,interrupt flags
  uint32_t& sr = context_->ctrl.SR.raw;
  const uint32_t status_before = sr;
  sr = (sr & ~0x3F) | ((sr & 0xF) << 2);


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

  ExceptionLog::Record(ExceptionLog::kException, address, context_->ctrl.EPC,
                       cause, status_before, sr);

  if (exception == kResetException) {
    //default state : 0101 0000 0110 0001 0000 0000 0000 0000
    context_->ctrl.SR.raw = 0x10900000;//0x50610000;
    context_->ctrl.PRId = 0x00000002;
    context_->pc = 0xBFC00000;
  }

}

void Cpu::Tick() {
  ++context_->cycles;
  ++context_->current_cycles;
  system_->io().Tick(1);
}

// A DMA holds the bus for the length of its transfer. The CPU does not
// execute during that time, but the GPU, the timers, the CD and the SPU all
// keep running - so those cycles have to reach them, and have to count
// towards the front end's idea of how much work a frame took.
void Cpu::TickCycles(uint32_t cycles) {
  context_->cycles += cycles;
  context_->current_cycles += cycles;
  // Handed on in the same size steps ordinary execution uses. One enormous
  // step would jump the GPU dozens of scanlines at once and leave the display
  // gates the counters watch meaningless for the whole transfer.
  while (cycles > 32) {
    system_->io().Tick(32);
    cycles -= 32;
  }
  if (cycles > 0)
    system_->io().Tick(cycles);
}
/*
uint32_t Cpu::LoadMemory(bool cached, int size_bytes, uint32_t physical_address, uint32_t virtual_address) {
  if (IsBusError() == true) {
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
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
    //dont check if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      //dcache_.Read(physical_address,data);
      //if (size_bytes != 4)
//        dcache_.InvalidateLine(physical_address);
      switch (size_bytes) {
        case kM8: data = system_->io().scratchpad.u32[(physical_address&0x3FF)];
        case kM16: data = system_->io().scratchpad.u32[(physical_address&0x3FF)>>1];
        case kM32: data = system_->io().scratchpad.u32[(physical_address&0x3FF)>>2];
      }
    //} else {
    //  icache_.Read(physical_address,data);
    //  if (size_bytes != 4)
    //    icache_.InvalidateLine(physical_address);
    //}
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
    buffer = &system_->io().scratchpad;//.u32[physical_address&0x3FF];
    target_address = physical_address & 0x3FF;
    //return dcache_.lines[physical_address&0x3FF].data[0];
  }

  if (physical_address >= 0x1F801000 && physical_address <= 0x1F802FFF) {
    switch (size_bytes) {
      case 1: return system_->io().Read08(physical_address);
      case 2: return system_->io().Read16(physical_address);
      case 4: return system_->io().Read32(physical_address);
    }
  }
  
  if (buffer != nullptr) {
    /*if (cached == true) {
      uint32_t data;
      auto cache_hit = icache_.Read(physical_address,data);

      if (cache_hit == true) {
        const uint32_t mask[] = {0x0,0xFF,0xFFFF,0xFFFFFF,0xFFFFFFFF};
        data = ( data >> ((physical_address&0x3)<<3)) & mask[size_bytes];
        return data;
      }
      else {
        icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
        //Tick();Tick();Tick();Tick();Tick();Tick();
      }

    }*/
/*
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
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeDBE);
    return;
  }
  if ((IsAddressError(virtual_address,size_bytes) == true)) {
    // BadVaddr is the address that faulted, not the instruction that did it -
    // the pc is already in EPC. Storing the pc here made every address error
    // report BadVaddr == EPC, which reads like a jump into nowhere and hides
    // the pointer that was actually bad.
    context_->ctrl.BadVaddr = virtual_address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }

  if (context_->ctrl.SR.IsC) { //cache isolation
    uint32_t cdata[4] = { data };
    //if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      //dcache_.Write(physical_address,cdata);
      //if (size_bytes != 4)
        //dcache_.InvalidateLine(physical_address);
      assert(size_bytes==4);
      system_->io().scratchpad.u32[(physical_address&0x3FF)>>2] = data;
    /*} else {
      icache_.Write(physical_address,cdata);
      if (size_bytes != 4)
        icache_.InvalidateLine(physical_address);
    }*/
  /*  return;
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
    buffer = &system_->io().scratchpad;//[physical_address&0x3FF] = data;
    target_address = physical_address & 0x3FF;

    //dcache_.lines[physical_address&0x3FF].data[0] = data;
    //return;
  }

  if ((physical_address >= 0x1F801000 && physical_address <= 0x1F802FFF)||
    (physical_address >= 0xFFFE0000 && physical_address <= 0xFFFE0134)) {
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
    /*if (cached == true) {
      //fprintf(system_->csvlog.fp,",,cache write,0x%08x,cache data,,actual data,0x%08X\n",physical_address,data);
      icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
    }*/
 /*   return;
  }

  BREAKPOINT
}
*/

uint32_t Cpu::Load(MemorySize size, uint32_t address) {
  if (IsBusError() == true) {
    // Only the first one is worth recording. This opened, appended to and
    // closed the file on every bus error, and a game that faults in a loop
    // faults millions of times - which turned a diagnostic into the slowest
    // thing in the run.
    static bool logged = false;
    if (current_stage != 1 && !logged) {
      logged = true;
      FILE* f = fopen("dbe_debug.txt", "w");
      if (f) {
        fprintf(f, "DBE! address=0x%08X valid=%d pc=0x%08X\n", address,
                valid_address_flag_, context_->pc);
        fclose(f);
      }
    }
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
    auto code = current_stage == 1 ? kExceptionCodeIBE : kExceptionCodeDBE;
    RaiseException(context_->prev_pc,kOtherException,code);
    return 0;
  }

  if (IsAddressError(address,size) == true) {
    context_->ctrl.BadVaddr = address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdEL);
    return 0;
  }

  if ((context_->ctrl.SR.IsC) && current_stage != 1) { //cache isolation
    uint32_t data;
    switch (size) {
      case kM8: data = system_->io().scratchpad.u8[(address&0x3FF)];
      case kM16: data = system_->io().scratchpad.u16[(address&0x3FF)>>1];
      case kM32: data = system_->io().scratchpad.u32[(address&0x3FF)>>2];
    }
    /*if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      dcache_.Read(physical_address,data);
      if (size_bytes != 4)
        dcache_.InvalidateLine(physical_address);
       // data = system_->io().scratchpad.u32[physical_address&0x3FF];
    } else {
      icache_.Read(physical_address,data);
      if (size_bytes != 4)
        icache_.InvalidateLine(physical_address);
    }*/
    return data;
  }
  
  

  // Decode on the physical address, not the virtual one. Every register has
  // three virtual addresses - KUSEG 0x1F80xxxx, KSEG0 0x9F80xxxx and KSEG1
  // 0xBF80xxxx - and the BIOS uses all three. Matching the virtual address
  // meant a register read through KSEG1 fell off the end of the decode and
  // returned zero without so much as a trap.
  const uint32_t physical = AddressTranslation(address);

  // Charge what the access actually costs. The R3000A issues a load in one
  // cycle but then stalls on the bus, and only main RAM and the scratchpad are
  // anywhere near fast. Without this the CPU runs roughly twice as many
  // instructions per frame as real hardware, and the BIOS notices: its VSync
  // gives up waiting for a vertical blank that has not had time to arrive and
  // prints "VSync: timeout", which is why the intro used to race past.
  //
  // Instruction fetches are excluded: those come through the instruction
  // cache, which is a separate cost and is not modelled here.
  if (current_stage != 1) {
    uint32_t stall = 0;
    if (physical <= 0x007FFFFF)                                   stall = 3;
    else if (physical >= 0x1F800000 && physical <= 0x1F8003FF)    stall = 0;
    else if (physical >= 0x1F801000 && physical <= 0x1F802FFF)    stall = 3;
    else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF)    stall = 5;
    else                                                          stall = 5;
    for (uint32_t i = 0; i < stall; ++i)
      Tick();
  }

  Buffer* buffer = nullptr;
  uint32_t offset = 0;

  if (physical <= 0x007FFFFF) {                 // RAM, and its mirrors
    // Read RAM directly. This used to go through ICache2, which is an
    // *instruction* cache: once the BIOS enabled it, every data load returned
    // whatever happened to be in a line buffer that is indexed by byte address
    // but filled one word at a time. A cache is a performance model, and
    // modelling it wrongly is worse than not modelling it at all.
    buffer = &system_->io().ram_buffer;
    offset = physical & 0x001FFFFF;
  } else if (physical >= 0x1F000000 && physical <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    offset = physical & 0x0000FFFF;
  } else if (physical >= 0x1F800000 && physical <= 0x1F8003FF) {
    buffer = &system_->io().scratchpad;
    offset = physical & 0x000003FF;
  } else if (physical >= 0x1F801000 && physical <= 0x1F802FFF) {
    switch (size) {
      case kM8:  return system_->io().Read08(physical);
      case kM16: return system_->io().Read16(physical);
      case kM32: return system_->io().Read32(physical);
    }
  } else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF) {
    buffer = &system_->io().bios_buffer;
    offset = physical & 0x0007FFFF;
  } else if (address >= 0xFFFE0000 && address <= 0xFFFE0FFF) {
    // Cache control sits in KSEG2, which is not translated at all.
    switch (size) {
      case kM8:  return system_->io().Read08(address);
      case kM16: return system_->io().Read16(address);
      case kM32: return system_->io().Read32(address);
    }
  }

  if (buffer != nullptr) {
   /* if (cached == true) {
      uint32_t data;
      auto cache_hit = icache_.Read(physical_address,data);

      if (cache_hit == true) {
        const uint32_t mask[] = {0x0,0xFF,0xFFFF,0xFFFFFF,0xFFFFFFFF};
        data = ( data >> ((physical_address&0x3)<<3)) & mask[size_bytes];
        return data;
      }
      else {
        icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
        //Tick();Tick();Tick();Tick();Tick();Tick();
      }

    }*/

    switch (size) {
      case kM8: return buffer->u8[offset];
      case kM16: return buffer->u16[offset>>1];
      case kM32: return buffer->u32[offset>>2];
    }
  }

  //BREAKPOINT
  return 0;
}

void Cpu::Store(MemorySize size, uint32_t data, uint32_t address) {
  //todo: research about this value, ignore for now
  if (IsBusError() == true) { 
    //context_->ctrl.BadVaddr = context_->prev_pc; //bus errors leave it
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeDBE);
    return;
  }
  if ((IsAddressError(address,size) == true)) {
    context_->ctrl.BadVaddr = address;
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeAdES);
    return;
  }
  // A watched RAM address records who wrote it. "This structure holds garbage"
  // is otherwise a dead end: the write that put it there happened long before
  // the read that noticed.
  if (watch_address_ != 0) {
    const uint32_t low = address & 0x1FFFFFFF;
    const uint32_t bytes = (size == kM8) ? 1u : (size == kM16) ? 2u : 4u;
    if (low + bytes > (watch_address_ & 0x1FFFFFFF) &&
        low < (watch_address_ & 0x1FFFFFFF) + 4) {
      Watch& w = watch_[watch_count_ % kWatchCapacity];
      w.pc = context_->prev_pc;
      w.address = address;
      w.value = data;
      w.size = bytes;
      ++watch_count_;
    }
  }


   if (context_->ctrl.SR.IsC) { //cache 
    switch (size) {
      case kM8: system_->io().scratchpad.u8[(address&0x3FF)] = data&0xFF;
      case kM16: system_->io().scratchpad.u16[(address&0x3FF)>>1] = data&0xFFFF;
      case kM32: system_->io().scratchpad.u32[(address&0x3FF)>>2] = data;
    }
    
    /*uint32_t cdata[4] = { data };
    if ((context_->ctrl.SR.SwC) == 0) { //check for swap!
      dcache_.Write(physical_address,cdata);
      if (size_bytes != 4)
        dcache_.InvalidateLine(physical_address);
      //system_->io().scratchpad.u32[physical_address&0x3FF] = data;
    } else {
      icache_.Write(physical_address,cdata);
      if (size_bytes != 4)
        icache_.InvalidateLine(physical_address);
    }*/
    return;
  }

  // Same physical-address decode as Load; see the comment there.
  const uint32_t physical = AddressTranslation(address);

  Buffer* buffer = nullptr;
  uint32_t offset = 0;

  if (physical <= 0x007FFFFF) {                 // RAM, and its mirrors
    icache.InvalidateLine(address);
    buffer = &system_->io().ram_buffer;
    offset = physical & 0x001FFFFF;
  } else if (physical >= 0x1F000000 && physical <= 0x1F00FFFF) {
    buffer = &system_->io().parallel_port_buffer;
    offset = physical & 0x0000FFFF;
  } else if (physical >= 0x1F800000 && physical <= 0x1F8003FF) {
    buffer = &system_->io().scratchpad;
    offset = physical & 0x000003FF;
  } else if (physical >= 0x1F801000 && physical <= 0x1F802FFF) {
    switch (size) {
      case kM8:  system_->io().Write08(physical, data & 0xFF); return;
      case kM16: system_->io().Write16(physical, data & 0xFFFF); return;
      case kM32: system_->io().Write32(physical, data); return;
    }
  } else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF) {
    // The BIOS is read-only; a write there is discarded, not an error.
    return;
  } else if (address >= 0xFFFE0000 && address <= 0xFFFE0FFF) {
    switch (size) {
      case kM8:  system_->io().Write08(address, data & 0xFF); return;
      case kM16: system_->io().Write16(address, data & 0xFFFF); return;
      case kM32: system_->io().Write32(address, data); return;
    }
  }

  if (buffer != nullptr) {
    switch (size) {
      case kM8: buffer->u8[offset] = data; break;
      case kM16: buffer->u16[offset>>1] = data; break;
      case kM32: buffer->u32[offset>>2] = data; break;
    }
    /*if (cached == true) {
      //fprintf(system_->csvlog.fp,",,cache write,0x%08x,cache data,,actual data,0x%08X\n",physical_address,data);
      icache_.Write(physical_address,&buffer->u32[(target_address&~0xF)>>2]);
    }*/
    return;
  }

  BREAKPOINT
}


void Cpu::StageIF() {
  current_stage = 1;
  auto ppc = AddressTranslation(context_->pc);
  context_->code = Load(kM32,context_->pc); //LoadMemory(cache_flag_,4,ppc,context_->pc);
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
  context_->branch_flag = true;
  uint32_t prev_cause = context_->ctrl.Cause;
  ExecuteInstruction();
  context_->branch_flag = false;
  __inside_delay_slot = false;
  // If an exception (like an interrupt) happened during the delay slot,
  // ExecuteInstruction would have called RaiseException and set PC to 0x80000080.
  // We should NOT overwrite PC with the jump target in this case.
  if (context_->ctrl.Cause == prev_cause) {
    context_->pc = address;
  }
  if (output_inst == true && until_address == context_->pc)
    output_inst = false;
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
  WriteReg(31, context_->pc + 4);
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
  const int32_t r = static_cast<int32_t>(context_->gp.reg[rs_]);
  if (r <= 0) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::BGTZ() {
  const int32_t r = static_cast<int32_t>(context_->gp.reg[rs_]);
  if (r > 0) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::ADDI() {
  WriteReg(rt_, context_->gp.reg[rs_] + immediate_32bit_sign_extended_);
  Tick();
}

void Cpu::ADDIU() {
  WriteReg(rt_, context_->gp.reg[rs_] + immediate_32bit_sign_extended_);
  Tick();
}

void Cpu::SLTI() {
  WriteReg(rt_, (int32_t)context_->gp.reg[rs_] < immediate_32bit_sign_extended_);
  Tick();
}

void Cpu::SLTIU() {
  WriteReg(rt_, context_->gp.reg[rs_] < immediate_);
  Tick();
}


void Cpu::ANDI() {
  WriteReg(rt_, context_->gp.reg[rs_] & immediate_);
  Tick();
}

void Cpu::ORI() {
  WriteReg(rt_, context_->gp.reg[rs_] | immediate_);
  Tick();
}

void Cpu::XORI() {
  WriteReg(rt_, context_->gp.reg[rs_] ^ immediate_);
  Tick();
}

void Cpu::LUI() {
  WriteReg(rt_, immediate_ << 16);
  Tick();
  //WriteReg(rt_, context_->immediate_ << 16);
}

void Cpu::COP0() {
  switch (context_->rs()) {
    //MFC
    case 0x00: {
      WriteReg(rt_, context_->ctrl.reg[rd_]);
      break;
    }
    //MTC
    case 0x04: {
      const uint32_t before = context_->ctrl.SR.raw;
      context_->ctrl.reg[rd_] = context_->gp.reg[rt_];
      if (rd_ == 12) {
        ExceptionLog::Record(ExceptionLog::kStatusWrite, context_->prev_pc,
                             context_->ctrl.EPC, context_->ctrl.Cause, before,
                             context_->ctrl.SR.raw);
      }
      break;
    }
    //RFE
    case 0x10: {
      ++TrapCounter::rfe_count;
      const uint32_t before = context_->ctrl.SR.raw;
      context_->ctrl.SR.raw = (before & ~0xF) | ((before >> 2) & 0xF);
      ExceptionLog::Record(ExceptionLog::kReturn, context_->prev_pc,
                           context_->ctrl.EPC, context_->ctrl.Cause, before,
                           context_->ctrl.SR.raw);
      break;
    }
    default:
      BREAKPOINT_DETAIL(context_->code);
  }
  Tick();
}

// Coprocessor 2 is the GTE. Bit 25 of the instruction picks between a command
// and a register move; the move form is selected by rs, exactly as for Cop0.
//
// Only the command form was handled before, and it trapped afterwards anyway,
// so every MFC2/MTC2/CFC2/CTC2 - which is how software gets its vertices in
// and its results out - did nothing at all.
void Cpu::COP2() {
  if (context_->code & (1u << 25)) {
    system_->gte().Execute(context_->code);
    Tick();
    return;
  }

  switch (context_->rs()) {
    case 0x00:  // MFC2
      WriteReg(rt_, system_->gte().ReadData(rd_));
      break;
    case 0x02:  // CFC2
      WriteReg(rt_, system_->gte().ReadControl(rd_));
      break;
    case 0x04:  // MTC2
      system_->gte().WriteData(rd_, context_->gp.reg[rt_]);
      break;
    case 0x06:  // CTC2
      system_->gte().WriteControl(rd_, context_->gp.reg[rt_]);
      break;
    default:
      BREAKPOINT_DETAIL(context_->code);
      break;
  }
  Tick();
}

// LWC2 and SWC2 move a GTE data register straight to or from memory. They were
// UNKNOWN in the opcode table, so a display list built with them silently
// transferred nothing.
void Cpu::LWC2() {
  const uint32_t address =
      context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  system_->gte().WriteData(rt_, Load(kM32, address));
  Tick();
}

void Cpu::SWC2() {
  const uint32_t address =
      context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  Store(kM32, system_->gte().ReadData(rt_), address);
  Tick();
}

void Cpu::LB() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint8_t mem = Load(kM8,virtual_address);
  Tick();
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((int8_t)mem));
  Tick();
}

void Cpu::LH() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint16_t mem = Load(kM16,virtual_address);
  Tick();
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((int16_t)mem));
  Tick();
}

void Cpu::LWL() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = Load(kM32,virtual_address & ~0x03);
  Tick();
  switch (virtual_address & 0x3) {
    case 0:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0x00FFFFFF) | (mem<<24));
      break;
    case 1:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0x0000FFFF) | (mem<<16));
      break;
    case 2:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0x000000FF) | (mem<<8));
      break;
    case 3:
      ArmLoad(rt_, mem);
      break;
  }
  Tick();
}

void Cpu::LW() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem;
  mem = Load(kM32,virtual_address);
  Tick();
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>(mem));
  Tick();
}

void Cpu::LBU() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = Load(kM8,virtual_address);
  Tick();
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((uint8_t)mem));
  Tick();
}

void Cpu::LHU() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = Load(kM16,virtual_address);
  Tick();
  // The value is promised here and delivered one instruction later, which
  // is what the hardware does - see AdvanceLoadDelay.
  ArmLoad(rt_, static_cast<uint32_t>((uint16_t)mem));
  Tick();
}

void Cpu::LWR() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  uint32_t mem = Load(kM32,virtual_address & ~0x03);
  Tick();
  switch (virtual_address & 0x3) {
    case 0:
      ArmLoad(rt_, mem);
      break;
    case 1:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0xFF000000) | (mem>>8));
      break;
    case 2:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0xFFFF0000) | (mem>>16));
      break;
    case 3:
      ArmLoad(rt_, (ReadRegForwarded(rt_) & 0xFFFFFF00) | (mem>>24));
      break;
  }
  Tick();
}

void Cpu::SB() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  //StoreMemory(cache_flag_,1,context_->gp.reg[rt_],physical_address,virtual_address);
  Store(kM8,context_->gp.reg[rt_],virtual_address);
  Tick();
}

void Cpu::SH() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  //StoreMemory(cache_flag_,2,context_->gp.reg[rt_],physical_address,virtual_address);
  Store(kM16,context_->gp.reg[rt_],virtual_address);
  Tick();
}

void Cpu::SWL() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  // The bytes this store does not cover have to survive, so the existing word
  // has to be read before the register is merged into it. Leaving `data`
  // uninitialised here corrupted three bytes out of every four, which broke
  // every unaligned copy the BIOS makes.
  uint32_t data = Load(kM32, virtual_address & ~0x03);
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
  Store(kM32,data,virtual_address & ~0x03);

  Tick();
}

void Cpu::SW() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  Store(kM32,context_->gp.reg[rt_],virtual_address);
  Tick();
}

void Cpu::SWR() {
  uint32_t virtual_address = context_->gp.reg[rs_] + immediate_32bit_sign_extended_;
  uint32_t physical_address = AddressTranslation(virtual_address);
  // Same as SWL: the uncovered bytes have to be preserved, so read first.
  uint32_t data = Load(kM32, virtual_address & ~0x03);
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
  Store(kM32,data,virtual_address & ~0x03);
  Tick();
}

void Cpu::SLL() {
  WriteReg(rd_, context_->gp.reg[rt_] << shamt_);
  Tick();
}

void Cpu::SRL() {
  WriteReg(rd_, context_->gp.reg[rt_] >> shamt_);
  Tick();
}

void Cpu::SRA() {
  WriteReg(rd_, (int32_t)context_->gp.reg[rt_] >> shamt_);
  Tick();
}

void Cpu::SLLV() {
  WriteReg(rd_, context_->gp.reg[rt_] << (context_->gp.reg[rs_] & 0x1F));
  Tick();
}

void Cpu::SRLV() {
 WriteReg(rd_, context_->gp.reg[rt_] >> (context_->gp.reg[rs_] & 0x1F));
 Tick();
}

void Cpu::SRAV() {
  WriteReg(rd_, (int32_t)context_->gp.reg[rt_] >> (context_->gp.reg[rs_] & 0x1F));
  Tick();
}

void Cpu::JR() {
  Jump(context_->gp.reg[rs_]);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;
}

void Cpu::JALR() {
  WriteReg(rd_, context_->pc + 4); // rd must be 31
  Jump(context_->gp.reg[rs_]);
  if (context_->prev_pc >= 0xBFC00000)
    inside_bios_call = false;

}

void Cpu::SYSCALL() {
  RaiseException(context_->prev_pc,kOtherException,kExceptionCodeSyscall);
}

// `break` raises an exception like any other; it is how a debugger and the
// BIOS's own assertions stop the machine. It used to expand to nothing but a
// host-side debug marker, so the instruction simply fell through.
void Cpu::BREAK() {
  RaiseException(context_->prev_pc, kOtherException, kExceptionCodeBp);
}

void Cpu::MFHI() {
  WriteReg(rd_, context_->high);
  Tick();
}

void Cpu::MTHI() {
  context_->high  = context_->gp.reg[rs_];
  Tick();
}

void Cpu::MFLO() {
  WriteReg(rd_, context_->low);
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

// Division on MIPS never traps. Both degenerate cases have defined answers,
// and both have to be handled here rather than handed to the host CPU: x86
// raises a hardware divide-error for each of them, which takes the whole
// emulator down rather than producing a wrong number.
void Cpu::DIV() {
  const int32_t dividend = static_cast<int32_t>(context_->gp.reg[rs_]);
  const int32_t divisor = static_cast<int32_t>(context_->gp.reg[rt_]);

  if (divisor == 0) {
    // Quotient is all ones or one, depending on the sign of the dividend;
    // the remainder is the dividend itself.
    context_->high = static_cast<uint32_t>(dividend);
    context_->low = (dividend >= 0) ? 0xFFFFFFFFu : 1u;
  } else if (static_cast<uint32_t>(dividend) == 0x80000000u && divisor == -1) {
    // The one quotient that does not fit in 32 bits. The result is the
    // dividend unchanged, with no remainder.
    context_->high = 0;
    context_->low = 0x80000000u;
  } else {
    context_->low = static_cast<uint32_t>(dividend / divisor);
    context_->high = static_cast<uint32_t>(dividend % divisor);
  }
  Tick();
}

void Cpu::DIVU() {
  const uint32_t dividend = context_->gp.reg[rs_];
  const uint32_t divisor = context_->gp.reg[rt_];

  if (divisor == 0) {
    context_->high = dividend;
    context_->low = 0xFFFFFFFFu;
  } else {
    context_->low = dividend / divisor;
    context_->high = dividend % divisor;
  }
  Tick();
}

void Cpu::ADD() {

  uint64_t a = context_->gp.reg[rs_];
  uint64_t b = context_->gp.reg[rt_];
  uint64_t temp = ((BIT(a,31)<<32) | a) + ((BIT(b,31)<<32) | b);
  if (BIT(temp,32) != BIT(temp,31)) {
    RaiseException(context_->prev_pc,kOtherException,kExceptionCodeOv);
  } else {
    WriteReg(rd_, temp & 0xffffffff);
  }
  //WriteReg(rd_, context_->gp.reg[rs_] + context_->gp.reg[rt_]);
  Tick();
}

void Cpu::ADDU() {
  WriteReg(rd_, context_->gp.reg[rs_] + context_->gp.reg[rt_]);
  Tick();
}

void Cpu::SUB() {
  WriteReg(rd_, context_->gp.reg[rs_] - context_->gp.reg[rt_]);
  Tick();
}

void Cpu::SUBU() {
  WriteReg(rd_, context_->gp.reg[rs_] - context_->gp.reg[rt_]);
  Tick();
}

void Cpu::AND() {
  WriteReg(rd_, context_->gp.reg[rs_] & context_->gp.reg[rt_]);
  Tick();
}

void Cpu::OR() {
  WriteReg(rd_, context_->gp.reg[rs_] | context_->gp.reg[rt_]);
  Tick();
}

void Cpu::XOR() {
  WriteReg(rd_, context_->gp.reg[rs_] ^ context_->gp.reg[rt_]);
  Tick();
}

void Cpu::NOR() {
  WriteReg(rd_, ~(context_->gp.reg[rs_] | context_->gp.reg[rt_]));
  Tick();
}

void Cpu::SLT() {
  WriteReg(rd_, (int32_t)context_->gp.reg[rs_] < (int32_t)context_->gp.reg[rt_]);
  Tick();
}

void Cpu::SLTU() {
  WriteReg(rd_, context_->gp.reg[rs_] < context_->gp.reg[rt_]);
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
  //int32_t r = (int32_t)context_->gp.reg[rs_] ;
  bool cond = (context_->gp.reg[rs_] & 0x80000000)==0;//r >= 0;//
  if (cond==true) {
    Jump(context_->pc + (immediate_32bit_sign_extended_ << 2));
  }
}

void Cpu::BLTZAL() {
  WriteReg(31, context_->pc + 4);
  BLTZ();
}

void Cpu::BGEZAL() {
  WriteReg(31, context_->pc + 4);
  BGEZ();
}

}
}

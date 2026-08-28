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
#pragma once

namespace emulation {
namespace psx {
  /*
 //I-Type = 0,J-Type = 1,R-Type = 2
class CpuDecoder {
 public:
  virtual void operator ()(CpuContext& context) = 0;
};



class DecodeIType : public CpuDecoder  {
 public:
  void operator ()(CpuContext& context) {
    context.immediate_ = context.immediate();
    context.immediate_32bit_sign_extended_ = context.immediate_32bit_sign_extended();
    context.rt_ = context.rt();
    context.rs_ = context.rs();
  }
};

class DecodeJType : public CpuDecoder {
 public:
  void operator ()(CpuContext& context) {
    context.target_ = context.target();
  }
};

class DecodeRType : public CpuDecoder {
 public:
  void operator ()(CpuContext& context) {
    context.fu_ = context.fu();
    context.sa_ = context.sa();
    context.rd_ = context.rd();
    context.rt_ = context.rt();
    context.rs_ = context.rs();
  }
};

class CpuDecoder2 {
 public:
  void operator ()(CpuContext& context) {
    context.opcode_ = context.opcode();
    context.immediate_ = context.immediate();
    context.immediate_32bit_sign_extended_ = context.immediate_32bit_sign_extended();
    context.target_ = context.target();
    context.fu_ = context.fu();
    context.sa_ = context.sa();
    context.rd_ = context.rd();
    context.rt_ = context.rt();
    context.rs_ = context.rs();
  }
};
*/

class WBuffer {
 public:

  struct Register {
    bool status; //0 = empty , 1 = non empty
    uint32_t address;
    uint32_t data;
  };
  Register regs[4];
  int counter;
  int Initialize() {
    memset(regs,0,sizeof(Register)*4);
    counter = 0;
    return 0;
  }
  int Deinitialize() {
    return 0;
  }
  bool WritePending() {
    return regs[0].status && regs[1].status && regs[2].status && regs[3].status;
  }

  Register& Read() {
    auto ptr = &regs[0];
    while (ptr != nullptr && ptr->status == true)
      ++ptr;
    ptr->status = false;
    return *ptr;
  }

  void Write(uint32_t address,uint32_t data) {
    //if (counter == 4)
    //  return *this;
    auto ptr = &regs[0];
    while (ptr != nullptr && ptr->status == true)
      ++ptr;
    ptr->data = data;
    ptr->status = true;
    /*while (regs[counter].status == true) 
      ++counter;
    regs[counter].data = data;
    regs[counter].status = true;
    */
  }
};

class ICache2 : public Component {
 public:
  Buffer buffer;
  uint32_t addresses[0x1000];
  int Initialize() {
    buffer.Alloc(0x1000*4);
    Invalidate();
    return S_OK;
  }
  int Deinitialize() {
    buffer.Dealloc();
    return S_OK;
  }

  void Invalidate() {
    memset(addresses,0xFF,sizeof(addresses));
  }
  void InvalidateLine(uint32_t address) {
    if ((address>>24) != 0xA0)
      addresses[address & 0xfff] = 0xFFFFFFFF;
  }
  Buffer* GetBufferAndOffset(uint32_t address, uint32_t& output_offset);

};

class Cache {
 public:
  virtual int Initialize() = 0;
  virtual int Deinitialize() = 0;
  virtual bool Read(uint32_t physical_address,uint32_t& data) = 0;
  virtual void Write(uint32_t physical_address,uint32_t data[]) = 0;
  virtual void InvalidateLine(uint32_t physical_address) = 0;
};

class ICache : public Cache {
 public:
  static const int line_count = 256;
  struct CacheLine {
    bool valid;
    unsigned int memory_address:20;
    uint32_t data[4];
  };
  CacheLine* lines;
  uint32_t rbuffer;
  WBuffer wbuffer;
  ICache() {
  }
  ~ICache() {
  }
  int Initialize() {
    lines = new CacheLine[line_count];
    memset(lines,0,sizeof(CacheLine)*line_count);
    wbuffer.Initialize();
    return 0;
  }
  int Deinitialize() {
    wbuffer.Deinitialize();
    delete [] lines;
    return 0;
  }
  bool Read(uint32_t physical_address,uint32_t& data) {
    uint32_t index = ((physical_address&0xFF0)>>4) & (line_count-1);
    auto& line = lines[index];
    
    if (line.memory_address == ((physical_address&0xFFFFF000)>>12) && line.valid == true) {
      rbuffer = line.data[(physical_address&0xF)>>2];
      data = rbuffer;
      line.valid = true;//refresh
      //char debug_str[240];
      //sprintf(debug_str,"cache hit %08x data=%08x\n",physical_address,data);
      //OutputDebugString(debug_str);
      return true;
    } else {
      //char debug_str[240];
      //sprintf(debug_str,"cache miss %08x\n",physical_address);
      //OutputDebugString(debug_str);
      //line.memory_address = ((physical_address&0xFFFFF000)>>12);
      line.valid = false;
      return false;
    }
  }
  void Write(uint32_t physical_address,uint32_t data[]) {
    uint32_t index = ((physical_address&0xFF0)>>4) & (line_count-1);
    auto& line = lines[index];
    line.memory_address = ((physical_address&0xFFFFF000)>>12);
    memcpy(line.data,data,sizeof(line.data));
    line.valid = true;
      //char debug_str[240];
      //sprintf(debug_str,"cache write %08x data = %08x\n",physical_address,data[0]);
      //OutputDebugString(debug_str);
  }
  void InvalidateLine(uint32_t physical_address) {
    uint32_t index = ((physical_address&0xFF0)>>4) & (line_count-1);
    auto& line = lines[index];
    if (line.memory_address == ((physical_address&0xFFFFF000)>>12) && line.valid == true) {
      line.valid = false;
    } 
  }
 private:
};

class DCache : public Cache {
 public:
  static const int line_count = 256;
  struct CacheLine {
    bool valid;
    unsigned int memory_address:24;
    uint32_t data[1];
  };
  CacheLine* lines;
  DCache() {
  }
  ~DCache() {
  }
  int Initialize() {
    lines = new CacheLine[line_count];
    memset(lines,0,sizeof(CacheLine)*line_count);
    return 0;
  }
  int Deinitialize() {
    delete [] lines;
    return 0;
  }
  bool Read(uint32_t physical_address,uint32_t& data) {
    uint32_t index = (physical_address & (line_count-1));
    auto& line = lines[index];
    
    if (line.memory_address == ((physical_address&0xFFFFFF00)>>8) && line.valid == true) {
      data = line.data[0];
      line.valid = true;//refresh
      return true;
    } else {
      line.valid = false;
      return false;
    }
  }
  void Write(uint32_t physical_address,uint32_t data[]) {
    uint32_t index = (physical_address & (line_count-1));
    auto& line = lines[index];
    line.memory_address = ((physical_address&0xFFFFFF00)>>8);
    memcpy(line.data,data,sizeof(line.data));
    line.valid = true;
  }
  void InvalidateLine(uint32_t physical_address) {
    uint32_t index = (physical_address & (line_count-1));
    auto& line = lines[index];
    if (line.memory_address == ((physical_address&0xFFFFFF00)>>8) && line.valid == true) {
      line.valid = false;
    } 
  }
 private:
};

/*
template<size_t size,int line_size_words>
class Cache {
 public:
  Buffer mem;
  struct Tag {
    bool valid;
    uint32_t high_address;
  };
  Tag* tags;
  int line_count;
  Cache() {}
  ~Cache() {}
  int Initialize() {
    line_count = size/(line_size_words<<2);
    mem.Alloc(size);
    tags = new Tag[line_count];
    memset(tags,0,sizeof(Tag)*line_count);
    return 0;
  }
  int Deinitialize() {
    mem.Dealloc();
    delete [] tags;
    return 0;
  }
  bool Read(uint32_t physical_address,uint32_t& data) {
    uint32_t index = physical_address & (line_count-1);
    Tag& tag = tags[index];
    if (tag.high_address == (physical_address&~(line_count-1)) && tag.valid == true) {
      data = mem.u32[index];
      return true;
    } else {
      tag.valid = false;
      return false;
    }
  }
  void Write(uint32_t physical_address,uint32_t data) {
    uint32_t index = physical_address & (line_count-1);
    tags[index].valid = true;
    tags[index].high_address = physical_address & ~(line_count-1); 
    mem.u32[index] = data;
  }
};*/

class Cpu : public Component {
 friend DebugAssist;
 public:
  typedef void (Cpu::*Instruction)();
  int index,current_stage;
  bool inside_bios_call;
  bool bios_logged[3][256];
  Cpu();
  ~Cpu();
  int Initialize();
  int Deinitialize();
  void ExecuteInstruction();
  void RaiseException(uint32_t address, Exceptions exception, ExceptionCodes code);
  void Reset() { RaiseException(context_->pc,kResetException,kExceptionCodeInt); }
  bool IsBusError() {
    return !valid_address_flag_;
  }
  bool IsAddressError(uint32_t address,int alignment) {
    //not in kernel mode and accessing non kuseg
    if (address > 0x7FFFFFFF)
      if ((context_->ctrl.SR.KUc) != 0) 
        return true;
    //misaligned read/write
    if ((address & (alignment-1)) != 0)
      return true;

    return false;
  }
  // Maps a virtual address to a physical one, and decides whether it exists.
  //
  // Both halves work on the *physical* address. Validity used to be a table of
  // virtual ranges, and that table listed RAM and the BIOS through all three
  // windows but the hardware registers through only one - so a perfectly
  // ordinary register access through KSEG1 was reported as a bus error, and
  // every KUSEG RAM mirror above 2 MB with it. Deciding it after translation
  // removes the whole class of miss: every window onto a region is valid
  // exactly when the region is.
  uint32_t AddressTranslation(uint32_t virtual_address) {
    // KSEG2 is not mapped at all; the cache control register is the only thing
    // in it, and it is addressed directly.
    if (virtual_address >= 0xC0000000) {
      cache_flag_ = false;
      valid_address_flag_ =
          (virtual_address >= 0xFFFE0000 && virtual_address <= 0xFFFE0FFF);
      return virtual_address;
    }

    uint32_t physical;
    if (virtual_address < 0x80000000) {          // KUSEG
      physical = virtual_address;
      cache_flag_ = true;
    } else if (virtual_address < 0xA0000000) {   // KSEG0, cached
      physical = virtual_address & 0x1FFFFFFF;
      cache_flag_ = true;
    } else {                                     // KSEG1, uncached
      physical = virtual_address & 0x1FFFFFFF;
      cache_flag_ = false;
    }

    valid_address_flag_ =
        (physical <= 0x007FFFFF) ||                             // RAM, mirrored
        (physical >= 0x1F000000 && physical <= 0x1F7FFFFF) ||   // expansion 1
        (physical >= 0x1F800000 && physical <= 0x1F8003FF) ||   // scratchpad
        (physical >= 0x1F801000 && physical <= 0x1F802FFF) ||   // hardware
        (physical >= 0x1FA00000 && physical <= 0x1FBFFFFF) ||   // expansion 3
        (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF);     // BIOS
    return physical;
  }

  /*uint32_t AddressTranslation(uint32_t virtual_address) {
    if (virtual_address >= 0x00000000 && virtual_address <= 0x7FFFFFFF ) {
      cache_flag_ = true;
      return virtual_address;
    } else if (virtual_address >= 0x80000000 && virtual_address <= 0x9FFFFFFF ) {
      cache_flag_ = true;
      return virtual_address & 0x7FFFFFFF;
    } else  if (virtual_address >= 0xA0000000 && virtual_address <= 0xBFFFFFFF ) {
      cache_flag_ = false;
      return virtual_address & 0x1FFFFFFF;
    } else if (virtual_address >= 0xC0000000 && virtual_address <= 0xFFFFFFFF ) {
      cache_flag_ = false;
      valid_address_flag_ = true;
      return virtual_address;
    }
    //error i guess
    valid_address_flag_ = false;
    return 0xFFFFFFFF;
  }*/

  void Tick();
  uint32_t LoadMemory(bool cached, int size_bytes, uint32_t physical_address, uint32_t virtual_address);
  void StoreMemory(bool cached, int size_bytes,uint32_t data, uint32_t physical_address, uint32_t virtual_address);
  uint32_t Load(MemorySize size, uint32_t address);
  void Store(MemorySize size, uint32_t data, uint32_t address);
  CpuContext* context() { return context_; }
  void set_context(CpuContext* context) { context_ = context; }  
  ICache2 icache;
 private:
  static Instruction machine_instruction_main_[64];
  static Instruction machine_instruction_special_[64];
  static Instruction machine_instruction_regimm_[32];
  static Instruction machine_instruction_cop0_[64];
  static Instruction machine_instruction_cop2_[64];
  //CpuDecoder decoders[3];

  //DCache dcache_;
  CpuContext* context_;
  uint32_t target_;
  int32_t immediate_32bit_sign_extended_;
  uint16_t immediate_;
  uint8_t opcode_;
  uint8_t rd_;
  uint8_t rt_;
  uint8_t rs_;
  uint8_t funct_;
  uint8_t shamt_;
  bool __inside_instruction;
  bool __inside_delay_slot;
  bool cache_flag_, valid_address_flag_;
  void StageIF();
  void StageRD();
  void Jump(uint32_t address);
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
  void LWC2();
  void SWC2();

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


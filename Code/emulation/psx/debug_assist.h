#pragma once

#ifdef _DEBUG
#define PC_BREAKPOINT(x) if (context_->pc==x) { DebugBreak(); }
#endif

namespace emulation {
namespace psx {

struct BiosCall
{
    int address;
    int operation;
    const char *prototype;
};

class DebugAssist {
 public:
  static char* gpr[32];
  static char* cop0[32];
  static char* cop2[32];
  static int opcode_type[160];
  static char* assembly_code[160];
  static char*  machine_instruction_main_[64];
  static char*  machine_instruction_special_[64];
  static char*  machine_instruction_regimm_[32];
  static char*  machine_instruction_cop0_[64];
  static char*  machine_instruction_cop2_[64];
  static BiosCall bios_call_[3][256];
  System* system_;
  FILE* fp;
  DebugAssist(void);
  ~DebugAssist(void);
  void Open(char* filename);
  void Close();
  void OutputInstruction();
  void OutputInstruction2();
};

}
}

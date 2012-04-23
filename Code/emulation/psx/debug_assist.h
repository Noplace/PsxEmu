#pragma once

namespace emulation {
namespace psx {

struct BiosCall
{
    int address;
    int operation;
    const char *prototype;
};

class DebugAssist {
 friend Cpu;
 friend Kernel;
 friend IOInterface;
 public:
   
  DebugAssist(void);
  ~DebugAssist(void);
  void Open(char* filename);
  void Close();
 private:
  FILE* fp;
  static char*  machine_instruction_main_[64];
  static char*  machine_instruction_special_[64];
  static char*  machine_instruction_regimm_[32];
  static char*  machine_instruction_cop0_[64];
  static char*  machine_instruction_cop2_[64];
  static BiosCall bios_call_[3][256];
};

}
}

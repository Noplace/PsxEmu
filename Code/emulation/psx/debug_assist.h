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
  void OutputCSVHeader();
  void OutputInstruction();
  void OutputInstruction2();
};

}
}

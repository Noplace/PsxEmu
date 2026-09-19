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

class Kernel : public Component {
 public:
  Kernel();
  ~Kernel();
  void Initialize();
  void Call();

  // How often each BIOS entry point was called, indexed by table and function
  // number. "Which kernel call is it stuck in" is otherwise only answerable by
  // tracing millions of instructions.
  struct Stats {
    uint32_t a0[256];
    uint32_t b0[256];
    uint32_t c0[256];
    uint64_t total;
    // Everything the BIOS wrote to its serial console. The BIOS narrates its
    // own boot failures there and nowhere else, so this is usually the fastest
    // route from "the screen is blank" to the actual reason.
    static const int kTtyCapacity = 262144;
    char tty[kTtyCapacity];
    uint32_t tty_length;
  };
  const Stats& stats() const { return stats_; }

  // The live feed of the same console text, for a front end to show as it
  // arrives: whatever was written since the last call, moved into `out`.
  // Stats::tty is the whole run for a harness to print at the end and stops
  // at its capacity; this is drained every frame and never fills in normal
  // use. It is capped anyway, so text nobody collects cannot grow without
  // bound - and what the cap turned away is counted, not silently lost.
  void TakeConsoleText(std::string* out);
  uint64_t console_text_dropped() const { return console_dropped_; }

  // Incremented by every Initialize - a cold boot or a reset. A front end
  // compares it against the last value it saw to mark where one boot's
  // console output ends and the next one's begins.
  uint32_t session() const { return session_; }

 private:
   Stats stats_;
   std::string console_pending_;
   uint64_t console_dropped_ = 0;
   uint32_t session_ = 0;
   static const size_t kConsolePendingCapacity = 1 << 20;
   void putc(char c,int fd);
   // Appends one character to the captured console output.
   void RecordTty(char c);

#ifdef _DEBUG
   //DebugAssist debug;
   DebugAssist psxout;
#endif
};

}
}


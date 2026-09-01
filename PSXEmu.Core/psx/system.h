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

class System {
 friend DebugAssist;
 public:
  // Sizes of the memories the machine actually has, not settings.
  static const uint32_t kRamSize  = 0x200000;   // 2 MB
  static const uint32_t kBiosSize = 0x80000;    // 512 KB

  System();
  ~System();

  // The BIOS path is a setting, not a constant: the user supplies the dump.
  int Initialize(const char* bios_path);

  // Brings every device up without a BIOS, for tests that drive a device
  // through its protocol and never execute an instruction.
  int InitializeWithoutBios();
  int Deinitialize();

  // One instruction, plus whatever interrupt it lets through. Deterministic
  // and free of any wall-clock throttling, so a headless harness can drive it
  // directly and get the same answer every run.
  void StepInstruction();

  // StepInstruction wrapped in the wall-clock pacing a front end wants.
  void Step();

  void Run();
  void Stop();
  void LoadBiosFromMemory(const void* buffer);
  bool LoadBiosFromFile(const char* filename);
  bool LoadPsExe(const char* filename);
  bool LoadPsExeFromMemory(const void* data, size_t size);

  // What BootDisc found, whether or not it succeeded. A boot that fails says
  // which step failed rather than just "no".
  struct DiscBootInfo {
    std::string volume_id;
    std::string boot_path;      // the BOOT line, verbatim
    std::string executable;     // the identifier as stored on the disc
    uint32_t executable_size;
    const char* error;
    DiscBootInfo() : executable_size(0), error(nullptr) {}
  };

  // Reads SYSTEM.CNF from the mounted disc and starts the executable it names.
  bool BootDisc(DiscBootInfo* info);

  // The filesystem on the mounted disc, for a harness that wants to look.
  Iso9660& iso() { return iso_; }

  static bool ParseSystemCnf(const std::vector<uint8_t>& contents,
                             std::string* boot_path);

  // Watch a VRAM rectangle, for the harnesses.
  void WatchVram(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    gpu_.WatchVram(x, y, w, h);
  }

  // Mounting a disc. Accepts a cue sheet, an image file or a drive letter;
  // an empty path ejects.
  bool LoadDisc(const char* path) { return io_.cdrom.OpenDisc(path); }
  void EjectDisc() { io_.cdrom.CloseDisc(); }
  Cdrom& cdrom() { return io_.cdrom; }
  Sio& sio() { return io_.sio; }
  Cpu& cpu() { return cpu_; };
  Spu& spu() { return spu_; };
  IOInterface& io() { return io_; };
  MC& mc(int slot) { return mc_[slot]; }
  Kernel& kernel() { return kernel_; };
  GTE& gte() { return gte_; };
  uint8_t* ram() { return io_.ram_buffer.u8; }
  uint8_t* bios() { return io_.bios_buffer.u8; }
  double base_freq_hz() { return base_freq_hz_; }
  void set_base_freq_hz(double base_freq_hz) { base_freq_hz_ = base_freq_hz; }
  // The core owns the GPU and the framebuffer; a front end only reads them.
  Gpu& gpu() { return gpu_; }
  GpuCore* gpu_core() { return &gpu_; }
  
  bool auto_boot_ = false;
  std::string auto_boot_path_;
  void set_auto_boot(bool v, const std::string& path = "") {
    auto_boot_ = v;
    auto_boot_path_ = path;
  }
  #ifdef _DEBUG
  DebugAssist csvlog;
  #endif
  inline void Tick() {
    ++cpu_context_.cycles;
    ++cpu_context_.current_cycles;
    io_.Tick(1);
  }
  TimingInfo& timing() { return timing_; }

  // How many hardware interrupts were actually taken. An interrupt that is
  // raised but never delivered looks exactly like one that was never raised.
  uint64_t interrupts_taken() const { return interrupts_taken_; }
  // Instructions that ran with an interrupt pending but blocked by Cop0 SR.
  uint64_t interrupts_blocked() const { return interrupts_blocked_; }
  uint64_t interrupts_blocked_im() const { return interrupts_blocked_im_; }
  // Instructions that ran with interrupts globally enabled.
  uint64_t instructions_with_ie() const { return instructions_with_ie_; }
 private:
  std::atomic<int> state;
  utilities::Timer timer;
  uint64_t cycles_per_second_;

  std::unique_ptr<std::thread> thread;
  double base_freq_hz_;
  TimingInfo timing_;
  uint64_t interrupts_taken_;
  uint64_t interrupts_blocked_;
  uint64_t interrupts_blocked_im_;
  uint64_t instructions_with_ie_;
  static void thread_func(System* sys);
  Gpu gpu_;
  CpuContext cpu_context_;
  Cpu cpu_;
  Spu spu_;
  IOInterface io_;
  MC mc_[2];
  Kernel kernel_;
  GTE gte_;
  Iso9660 iso_;
};

}
}


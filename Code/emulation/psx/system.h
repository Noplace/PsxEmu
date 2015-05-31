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
  System();
  ~System();
  int Initialize();
  int Deinitialize();
  void Step();
  void Run();
  void Stop();
  void LoadBiosFromMemory(void* buffer);
  void LoadBiosFromFile(char* filename);
  void LoadPsExe(char* filename);
  Cpu& cpu() { return cpu_; };
  Spu& spu() { return spu_; };
  IOInterface& io() { return io_; };
  MC& mc() { return mc_; };
  Kernel& kernel() { return kernel_; };
  GTE& gte() { return gte_; };
  uint8_t* ram() { return io_.ram_buffer.u8; }
  uint8_t* bios() { return io_.bios_buffer.u8; }
  double base_freq_hz() { return base_freq_hz_; }
  void set_base_freq_hz(double base_freq_hz) { base_freq_hz_ = base_freq_hz; }
  GpuCore* gpu_core() { return gpu_core_; }  
  void set_gpu_core(GpuCore* gpu_core) { gpu_core_ = gpu_core; }
  #ifdef _DEBUG
  DebugAssist csvlog;
  #endif
  inline void Tick() {
    ++cpu_context_.cycles;
    ++cpu_context_.current_cycles;
    io_.Tick(1);
  }
  TimingInfo& timing() { return timing_; }
 private:
  std::atomic<int> state;
  utilities::Timer timer;
  uint64_t cycles_per_second_;

  std::thread* thread;
  double base_freq_hz_;
  TimingInfo timing_;
  static void thread_func(System* sys);
  GpuCore* gpu_core_;
  CpuContext cpu_context_;
  Cpu cpu_;
  Spu spu_;
  IOInterface io_;
  MC mc_;
  Kernel kernel_;
  GTE gte_;
};

}
}


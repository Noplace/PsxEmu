/*****************************************************************************************************************
* Copyright (c) 2013 Khalid Ali Al-Kooheji                                                                       *
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

const double default_gb_hz = 4194304.0;

class Emu {
 public:
  Memory memory;
  IOInterface iointerface;
  Dma dma;
  Cpu cpu;
  Gpu gpu;
  Spu spu;
  uint64_t cpu_cycles_,last_cpu_cycles_,cpu_cycles_per_step_;
  uint8_t speed;

  Emu():cpu_cycles_(0),last_cpu_cycles_(0),cpu_cycles_per_step_(0) {}
  ~Emu() {}
  void Initialize(double base_freq_hz);
  void Deinitialize();
  double Step();
  void Run();
  void Stop();
  void Pause();
  void Reset();
  void Render();

  std::function <void()> on_render;
  void set_window_handle(HWND hwnd) { window_handle_ = hwnd; }
  void set_on_render(const  std::function <void()>& on_render) {
    if (on_render != nullptr)
      this->on_render = on_render;
  }
  std::atomic<int> state;
  const double fps() { return timing.fps; }
  const double base_freq_hz() { return base_freq_hz_; }
  void set_base_freq_hz(double base_freq_hz) { base_freq_hz_ = base_freq_hz; }
  double frequency_mhz() { return frequency_mhz_; }
  uint64_t cycles_per_second() { return cycles_per_second_; }

  inline void Tick() {
    ++cpu_context_.cycles;
    ++cpu_context_.current_cycles;
    iointerface.Tick(1);
  }
 private:
  HWND window_handle_;
  CpuContext cpu_context_;
  pe::util::Timer utimer;
  uint64_t cycles_per_second_;

  std::thread* thread;
  double base_freq_hz_,frequency_mhz_;
  struct {
    uint64_t extra_cycles;
    uint64_t current_cycles;
    uint64_t prev_cycles;
    uint64_t total_cycles;
    uint32_t fps_counter;
    double fps;
    double misc_time_span;
    double fps_time_span;
    double span_accumulator;
    double time_span;
  } timing;
  static void thread_func(Emu* emu);
};

}
}
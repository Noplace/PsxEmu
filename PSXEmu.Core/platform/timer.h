// Replaces <WinCore/timer/timer2.h>. The emulation core needs exactly three
// things from it: Calibrate(), GetCurrentCycles() and resolution(), where
// (cycles_now - cycles_then) * resolution() is a span in milliseconds.
#pragma once

#include <chrono>
#include <cstdint>

namespace utilities {

class Timer {
 public:
  Timer() : resolution_(0.0) { Calibrate(); }

  // steady_clock ticks are already a fixed, known rate, so "calibration" is
  // just recording how many milliseconds one tick is worth.
  void Calibrate() {
    typedef std::chrono::steady_clock::period period;
    resolution_ = 1000.0 * static_cast<double>(period::num) /
                  static_cast<double>(period::den);
  }

  uint64_t GetCurrentCycles() const {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
  }

  // Milliseconds per cycle returned by GetCurrentCycles().
  double resolution() const { return resolution_; }

 private:
  double resolution_;
};

}  // namespace utilities

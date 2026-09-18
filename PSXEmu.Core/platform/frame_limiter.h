// Paces a loop to a target rate against the wall clock.
//
// This exists because nothing else in the front end does it. Its loop runs one
// emulated frame and goes round again, so the rate it actually achieves is
// whatever happens to block first:
//
//   - `Present(1, 0)` with vsync on, which is the *monitor's* refresh rate.
//     165 Hz here, against an emulated display producing 59.29 - the machine
//     runs at 2.8x and the whole BIOS intro goes past in a third of the time.
//   - `QueueAudio`, which used to block when the sound device's buffer was
//     full and so, with a working device, paced the machine to about the right
//     rate as a side effect. That was luck, not design: it did nothing at all
//     when `CreateAudioEngine` returned null. It no longer blocks (bug 49), so
//     this is now the only thing pacing the loop - which is why how evenly it
//     spaces the frames matters as much as the rate it holds on average.
//
// Neither is the machine's own clock, so neither belongs in charge of it. This
// is, and it lives in Core rather than in a front end because "how fast should
// this run" is a property of the emulated machine - see Emulator-Project-
// Standards section 1.
#pragma once

#include <chrono>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Windows 10 1803 and later. Named here in case the SDK in use hides it behind
// a version check; an older Windows simply refuses the flag, and the limiter
// falls back to plain sleeping.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace utilities {

class FrameLimiter {
 public:
  typedef std::chrono::steady_clock Clock;

  // The wait needs a timer that can actually wake at a sub-millisecond
  // deadline, which the obvious one cannot - see WaitFor() below.
  FrameLimiter() {
#ifdef _WIN32
    timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                    CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                    TIMER_ALL_ACCESS);
#endif
  }

  ~FrameLimiter() {
#ifdef _WIN32
    if (timer_ != nullptr)
      CloseHandle(timer_);
#endif
  }

  FrameLimiter(const FrameLimiter&) = delete;
  FrameLimiter& operator=(const FrameLimiter&) = delete;

  // Whether the precise timer was available. False on Windows before 10 1803,
  // where the limiter still holds the average rate but not the spacing.
  bool precise() const {
#ifdef _WIN32
    return timer_ != nullptr;
#else
    return false;
#endif
  }

  // Forgets the deadline, so the next Wait starts a fresh one instead of
  // trying to make up a debt. Call after anything that legitimately stopped
  // the machine for a while - a pause, a reset, a state load, a modal dialog.
  void Reset() { have_deadline_ = false; }

  // Blocks until the current frame is due at `hz`, then moves the deadline on
  // by one frame. Returns immediately, and starts a fresh deadline, if the
  // caller is already more than `kResyncFrames` late - a host that cannot keep
  // up should run slow, not sprint through the next second to catch up.
  //
  // A non-positive `hz` disables it entirely, which is what a caller with no
  // machine to ask should pass.
  void Wait(double hz) {
    if (hz <= 0.0) {
      have_deadline_ = false;
      return;
    }
    const Clock::duration period =
        std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / hz));

    const Clock::time_point now = Clock::now();
    if (!have_deadline_ || now > deadline_ + period * kResyncFrames) {
      have_deadline_ = true;
      deadline_ = now + period;
      return;
    }

    // Sleep off the bulk and spin the last stretch: a sleep that overshoots by
    // even a millisecond or two is a frame arriving late, and spinning all of
    // it would burn a core for nothing.
    const Clock::duration spin = precise() ? std::chrono::milliseconds(1)
                                           : std::chrono::milliseconds(2);
    for (;;) {
      const Clock::duration remaining = deadline_ - Clock::now();
      if (remaining <= Clock::duration::zero())
        break;
      if (remaining > spin)
        WaitFor(remaining - spin);
      else
        std::this_thread::yield();
    }
    deadline_ += period;
  }

 private:
  // Blocks for about `duration`.
  //
  // This used to be sleep_for(1ms) in a loop, on the stated understanding that
  // "Sleep's granularity is around a millisecond". It is not, unless something
  // has raised the system timer resolution, and nothing in this project does:
  // measured, sleep_for(1ms) took 15.5 ms at the median, and timeBeginPeriod(1)
  // did not change that. One such sleep landing near the end of a frame put the
  // frame 13 ms late, the deadline arithmetic then ran the next one at once to
  // catch up, and frames came out anywhere from 0 to 30 ms apart - exactly on
  // average, so every rate check passed, while the sound device was fed in
  // bursts with gaps longer than its buffer.
  //
  // A high-resolution waitable timer wakes within about half a millisecond
  // without touching the timer resolution of the whole system, which
  // timeBeginPeriod would. Without one - Windows before 10 1803 - this is the
  // old one-millisecond sleep, which keeps the average and not the spacing.
  void WaitFor(Clock::duration duration) {
#ifdef _WIN32
    if (timer_ != nullptr) {
      const long long ticks =
          std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count() / 100;
      LARGE_INTEGER due;
      due.QuadPart = -ticks;   // negative: relative to now, in 100 ns units
      if (ticks > 0 &&
          SetWaitableTimerEx(timer_, &due, 0, nullptr, nullptr, nullptr, 0)) {
        WaitForSingleObject(timer_, INFINITE);
        return;
      }
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

#ifdef _WIN32
  HANDLE timer_ = nullptr;
#endif
  // How far behind the caller has to be before the deadline is abandoned
  // rather than caught up. Four frames is long enough that ordinary jitter -
  // a slow frame, a scheduler hiccup - is still absorbed and averaged out.
  static const int kResyncFrames = 4;

  Clock::time_point deadline_;
  bool have_deadline_ = false;
};

}  // namespace utilities

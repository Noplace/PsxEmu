// Paces a loop to a target rate against the wall clock.
//
// This exists because nothing else in the front end does it. Its loop runs one
// emulated frame and goes round again, so the rate it actually achieves is
// whatever happens to block first:
//
//   - `Present(1, 0)` with vsync on, which is the *monitor's* refresh rate.
//     165 Hz here, against an emulated display producing 59.29 - the machine
//     runs at 2.8x and the whole BIOS intro goes past in a third of the time.
//   - `QueueAudio`, which blocks when the sound device's buffer is full and,
//     when there is a working device, happens to pace it to about the right
//     rate as a side effect. That is luck, not design: it does nothing at all
//     when `CreateAudioEngine` returned null, and it is the reason the same
//     build runs at the right speed on one machine and far too fast on
//     another.
//
// Neither is the machine's own clock, so neither belongs in charge of it. This
// is, and it lives in Core rather than in a front end because "how fast should
// this run" is a property of the emulated machine - see Emulator-Project-
// Standards section 1.
#pragma once

#include <chrono>
#include <thread>

namespace utilities {

class FrameLimiter {
 public:
  typedef std::chrono::steady_clock Clock;

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

    // Sleep off the bulk and spin the last couple of milliseconds. Sleep's
    // granularity is around a millisecond and a whole frame is only
    // seventeen, so sleeping the remainder outright would overshoot by enough
    // to matter; spinning all of it would burn a core for nothing.
    for (;;) {
      const Clock::duration remaining = deadline_ - Clock::now();
      if (remaining <= Clock::duration::zero())
        break;
      if (remaining > std::chrono::milliseconds(2))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      else
        std::this_thread::yield();
    }
    deadline_ += period;
  }

 private:
  // How far behind the caller has to be before the deadline is abandoned
  // rather than caught up. Four frames is long enough that ordinary jitter -
  // a slow frame, a scheduler hiccup - is still absorbed and averaged out.
  static const int kResyncFrames = 4;

  Clock::time_point deadline_;
  bool have_deadline_ = false;
};

}  // namespace utilities
